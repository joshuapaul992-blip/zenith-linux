/* kernel/report.c -- REPORT.TXT and VBIOS.ROM on the stick's report volume
 *
 * FAT handling is deliberately minimal and read-only for the metadata: the
 * boot sector (BPB) and root directory are read, the FAT is only followed to
 * find which sectors belong to a file, and nothing but those data sectors is
 * ever written. FAT16 and FAT32 are understood (Microsoft FAT specification,
 * sections 3-6). */
#include <kernel/report.h>
#include <kernel/block.h>
#include <kernel/klog.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/arch.h>
#include <kernel/bootinfo.h>
#include <kernel/posix.h>
#include <kernel/time.h>

#define REPORT_LABEL    "KESTREL RPT"
#define MAX_RUNS        32              /* extents per file (we create 1) */
#define MAX_SECTIONS    8
#define IO_CHUNK        (64 * 1024)

struct run { uint64_t lba; uint32_t count; };       /* sectors, partition relative */

struct fat_file {
    bool       found;
    uint32_t   size;                    /* bytes, from the directory entry */
    int        nruns;
    struct run run[MAX_RUNS];
};

struct section { const char *title; size_t (*fn)(char *, size_t); };

static struct blkdev  *vol;             /* partition (or whole disk) holding the volume */
static struct fat_file f_report, f_vbios;
static struct section sections[MAX_SECTIONS];
static int            nsections;
static const void    *blob;
static size_t         blob_len;
static const char    *blob_what;
static char           where[96] = "no report volume found";

void report_add_section(const char *title, size_t (*fn)(char *, size_t))
{
    if (nsections < MAX_SECTIONS) sections[nsections++] = (struct section){ title, fn };
}

void report_set_blob(const void *data, size_t len, const char *what)
{
    blob = data; blob_len = len; blob_what = what;
}

const char *report_where(void) { return where; }

/* ======================================================================== */
/*  FAT lookup                                                                 */
/* ======================================================================== */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct fat_vol {
    struct blkdev *dev;
    int      bits;                      /* 16 or 32 */
    uint32_t spc;                       /* sectors per cluster */
    uint64_t fat_lba, root_lba, data_lba;
    uint32_t root_secs;                 /* FAT16 fixed root directory */
    uint32_t root_cluster;              /* FAT32 */
    uint32_t clusters;
    uint8_t *sec;                       /* one-sector scratch (DMA safe) */
    uint64_t cached_fat_lba;
};

static uint64_t cluster_lba(const struct fat_vol *v, uint32_t c) { return v->data_lba + (uint64_t)(c - 2) * v->spc; }

/* FAT entry of cluster c, or 0xFFFFFFFF on a read error. */
static uint32_t fat_next(struct fat_vol *v, uint32_t c)
{
    uint32_t off = c * (v->bits / 8);
    uint64_t lba = v->fat_lba + off / 512;
    if (lba != v->cached_fat_lba) {
        if (blk_read(v->dev, lba, 1, v->sec) < 0) return 0xFFFFFFFFu;
        v->cached_fat_lba = lba;
    }
    uint32_t e = v->bits == 16 ? rd16(v->sec + off % 512) : rd32(v->sec + off % 512) & 0x0FFFFFFFu;
    if (v->bits == 16 && e >= 0xFFF8) e = 0x0FFFFFFF;                 /* end of chain, FAT32 style */
    return e;
}

static bool is_eoc(uint32_t e) { return e >= 0x0FFFFFF8u; }

/* Cluster chain -> sector runs, at most `max_bytes` worth. */
static bool chain_to_runs(struct fat_vol *v, uint32_t first, uint32_t max_bytes, struct fat_file *f)
{
    uint32_t need = (max_bytes + v->spc * 512 - 1) / (v->spc * 512), c = first;
    f->nruns = 0;
    for (uint32_t i = 0; i < need; i++) {
        if (c < 2 || c >= v->clusters + 2) return false;               /* free, reserved or bad */
        uint64_t lba = cluster_lba(v, c);
        struct run *r = f->nruns ? &f->run[f->nruns - 1] : NULL;
        if (r && r->lba + r->count == lba) r->count += v->spc;
        else {
            if (f->nruns == MAX_RUNS) return false;                    /* too fragmented */
            f->run[f->nruns++] = (struct run){ lba, v->spc };
        }
        if (i + 1 < need) {
            uint32_t n = fat_next(v, c);
            if (n == 0xFFFFFFFFu || is_eoc(n)) return false;           /* chain shorter than the size */
            c = n;
        }
    }
    return true;
}

/* Look for the 8.3 names in one directory sector. */
static void scan_dir_sector(struct fat_vol *v, const uint8_t *s, bool *end)
{
    for (int i = 0; i < 512 && !*end; i += 32) {
        const uint8_t *d = s + i;
        if (d[0] == 0x00) { *end = true; return; }
        if (d[0] == 0xE5 || (d[11] & 0x0F) == 0x0F || (d[11] & 0x18)) continue;   /* deleted, LFN, dir, label */
        struct fat_file *f = !memcmp(d, "REPORT  TXT", 11) ? &f_report : !memcmp(d, "VBIOS   ROM", 11) ? &f_vbios : NULL;
        if (!f || f->found) continue;
        uint32_t first = rd16(d + 26) | (v->bits == 32 ? (uint32_t)rd16(d + 20) << 16 : 0);
        f->size = rd32(d + 28);
        f->found = f->size && chain_to_runs(v, first, f->size, f);
        if (!f->found) kprintf("report: %.11s on %s is empty or fragmented, not used\n", (const char *)d, v->dev->name);
    }
}

static bool try_volume(struct blkdev *b, uint8_t *sec)
{
    if (b->block_size != 512 || blk_read(b, 0, 1, sec) < 0) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA || (sec[0] != 0xEB && sec[0] != 0xE9)) return false;
    if (rd16(sec + 11) != 512) return false;
    uint32_t spc = sec[13], reserved = rd16(sec + 14), nfats = sec[16], root_entries = rd16(sec + 17);
    if (!spc || (spc & (spc - 1)) || !reserved || !nfats || nfats > 2) return false;
    uint32_t total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    uint32_t fatsz = rd16(sec + 22) ? rd16(sec + 22) : rd32(sec + 36);
    uint32_t root_secs = (root_entries * 32 + 511) / 512;
    uint64_t meta = (uint64_t)reserved + (uint64_t)nfats * fatsz + root_secs;
    if (!total || !fatsz || total > b->blocks || meta >= total) return false;
    uint32_t clusters = (uint32_t)((total - meta) / spc);
    if (clusters < 4085) return false;                                 /* FAT12: never ours */
    int bits = clusters < 65525 ? 16 : 32;
    const uint8_t *label = sec + (bits == 16 ? 43 : 71);
    if (memcmp(label, REPORT_LABEL, 11) != 0) return false;           /* not the Kestrel report volume */

    struct fat_vol v = {
        .dev = b, .bits = bits, .spc = spc, .clusters = clusters,
        .fat_lba = reserved, .root_lba = reserved + (uint64_t)nfats * fatsz, .root_secs = root_secs,
        .data_lba = meta, .root_cluster = bits == 32 ? rd32(sec + 44) : 0, .cached_fat_lba = ~0ull,
    };
    uint8_t *s = kmalloc(512 * 2);
    if (!s) return false;
    v.sec = s + 512;
    memset(&f_report, 0, sizeof f_report);
    memset(&f_vbios, 0, sizeof f_vbios);
    bool end = false;
    if (bits == 16) {
        for (uint32_t i = 0; i < root_secs && !end; i++)
            if (blk_read(b, v.root_lba + i, 1, s) == 0) scan_dir_sector(&v, s, &end);
    } else {
        uint32_t c = v.root_cluster;
        for (int guard = 0; !end && c >= 2 && !is_eoc(c) && guard < 64; guard++, c = fat_next(&v, c))
            for (uint32_t i = 0; i < spc && !end; i++)
                if (blk_read(b, cluster_lba(&v, c) + i, 1, s) == 0) scan_dir_sector(&v, s, &end);
    }
    kfree(s);
    if (!f_report.found && !f_vbios.found) return false;
    snprintf(where, sizeof where, "%s (FAT%d \"" REPORT_LABEL "\"): %s%s%s", b->name, bits,
             f_report.found ? "REPORT.TXT" : "", f_report.found && f_vbios.found ? ", " : "",
             f_vbios.found ? "VBIOS.ROM" : "");
    return true;
}

bool report_locate(void)
{
    uint8_t *sec = kmalloc(512);
    if (!sec) return false;
    vol = NULL;
    for (int pass = 0; pass < 2 && !vol; pass++)        /* USB sticks first, then any disk */
        for (int i = 0; i < blk_count() && !vol; i++) {
            struct blkdev *b = blk_get(i), *disk = blk_disk_of(b);
            if (b->removed || (pass == 0) != (disk->kind == BLK_DISK_USB)) continue;
            if (try_volume(b, sec)) vol = b;
        }
    kfree(sec);
    if (vol) kprintf("report: report volume on %s\n", where);
    return vol != NULL;
}

/* ======================================================================== */
/*  writing                                                                    */
/* ======================================================================== */

/* Overwrite file `f` with data[0..len), padded with `pad` to the file size
 * (data beyond the file size is cut). */
static int write_file(const struct fat_file *f, const uint8_t *data, size_t len, uint8_t pad)
{
    uint8_t *buf = kmalloc(IO_CHUNK);
    if (!buf) return -ENOMEM;
    size_t pos = 0, total = ((size_t)f->size + 511) & ~(size_t)511;
    int rc = 0;
    for (int r = 0; r < f->nruns && pos < total && rc == 0; r++) {
        uint64_t lba = f->run[r].lba;
        size_t left = (size_t)f->run[r].count * 512;
        while (left && pos < total && rc == 0) {
            size_t n = left < IO_CHUNK ? left : IO_CHUNK;
            if (n > total - pos) n = total - pos;
            for (size_t i = 0; i < n; i++) buf[i] = pos + i < len ? data[pos + i] : pad;
            rc = blk_write(vol, lba, (uint32_t)(n / 512), buf);
            lba += n / 512; left -= n; pos += n;
        }
    }
    kfree(buf);
    return rc;
}

static size_t header(char *buf, size_t cap, const char *reason)
{
    uint64_t ms = uptime_ms();
    return (size_t)snprintf(buf, cap,
        "Kestrel diagnostic report (%s)\n"
        "==========================================================================\n"
        "kernel     %s %s (%s), built " __DATE__ " " __TIME__ "\n"
        "uptime     %lu.%03lu s\n"
        "firmware   %s, loader %s\n"
        "cmdline    %s\n"
        "cpu        %s / %s\n",
        reason, KESTREL_NAME, KESTREL_VERSION, KESTREL_MACHINE, ms / 1000, ms % 1000,
        g_boot.uefi ? "UEFI" : "BIOS", g_boot.loader, g_boot.cmdline[0] ? g_boot.cmdline : "(empty)",
        g_boot.cpu_vendor, g_boot.cpu_brand);
}

int report_save(const char *reason)
{
    if (!vol && !report_locate()) return -ENOENT;
    if (!f_report.found) return -ENOENT;

    size_t cap = f_report.size;
    size_t frames = (cap + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pa = pmm_alloc_contig(frames);
    if (!pa) return -ENOMEM;
    char *buf = (char *)(uintptr_t)pa;
    size_t n = header(buf, cap, reason);
    for (int i = 0; i < nsections && n < cap; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "\n== %s ==\n", sections[i].title);
        if (n < cap) n += sections[i].fn(buf + n, cap - n);
    }
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "\n== kernel log (%lu bytes) ==\n", (uint64_t)klog_size());
    if (n < cap) n += klog_read(0, buf + n, cap - n);
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "\n== end of report ==\n");
    if (n > cap) n = cap;

    int rc = write_file(&f_report, (const uint8_t *)buf, n, ' ');
    pmm_free_contig(pa, frames);
    if (rc == 0 && f_vbios.found && blob)
        rc = write_file(&f_vbios, blob, blob_len, 0xFF);
    if (rc == 0) rc = blk_flush(vol);
    if (rc == 0)
        kprintf("report: wrote REPORT.TXT (%lu bytes)%s%s to %s\n", (uint64_t)n,
                f_vbios.found && blob ? " and VBIOS.ROM: " : "", f_vbios.found && blob ? blob_what : "", vol->name);
    else
        kprintf("report: writing to %s failed: %s\n", vol->name, blk_strerror(rc));
    return rc;
}
