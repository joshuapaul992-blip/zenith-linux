/* kernel/block.c -- block device registry, MBR/GPT partition discovery,
 * /dev nodes (see block.h) */
#include <kernel/block.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/crc32.h>
#include <kernel/posix.h>
#include <kernel/cpu.h>
#include <kernel/task.h>

#define BLK_MAJOR       259             /* "blkext", as on Linux */
#define BOUNCE_MAX      (64u * 1024)
#define MAX_LOGICAL     60              /* EBR chain guard */

static struct blkdev *devs[BLK_MAX];
static int ndev;

int blk_count(void) { return ndev; }
struct blkdev *blk_get(int i) { return i >= 0 && i < ndev ? devs[i] : NULL; }
struct blkdev *blk_disk_of(struct blkdev *b) { while (b && b->parent) b = b->parent; return b; }
uint64_t blk_size_bytes(const struct blkdev *b) { return b->blocks * b->block_size; }

struct blkdev *blk_find(const char *name)
{
    if (!strncmp(name, "/dev/", 5)) name += 5;
    for (int i = 0; i < ndev; i++) if (!strcmp(devs[i]->name, name)) return devs[i];
    return NULL;
}

const char *blk_strerror(int err)
{
    switch (-err) {
    case 0:          return "success";
    case EIO:        return "I/O error";
    case ENODEV:     return "device removed";
    case ETIMEDOUT:  return "timed out";
    case ENOMEDIUM:  return "no medium";
    case ERANGE:     return "beyond end of device";
    case EINVAL:     return "invalid argument";
    case ENOMEM:     return "out of memory";
    default:         return "error";
    }
}

void blk_guid_str(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* ======================================================================== */
/*  I/O                                                                        */
/* ======================================================================== */
/* Drivers keep per-device transport state (BOT tags, AHCI slots), so a disk
 * serves one request at a time; other threads yield until it is free. */
static void disk_lock(struct blkdev *d)
{
    for (;;) {
        uint64_t f = irq_save();
        bool ok = !d->io_busy;
        if (ok) d->io_busy = true;
        irq_restore(f);
        if (ok) return;
        if (irqs_enabled()) sched_yield(); else cpu_relax();
    }
}
static void disk_unlock(struct blkdev *d) { d->io_busy = false; }

int blk_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    if (!b || !buf) return -EINVAL;
    if (count == 0) return 0;
    if (lba >= b->blocks || count > b->blocks - lba) return -ERANGE;
    struct blkdev *disk = b;
    while (disk->parent) { lba += disk->start; disk = disk->parent; }

    if (disk->removed || b->removed) return -ENODEV;
    if (disk->ops->alive && !disk->ops->alive(disk)) {
        kprintf("block: %s is no longer attached\n", disk->name);
        blk_mark_removed(disk);
        return -ENODEV;
    }
    disk_lock(disk);
    int rc = disk->removed ? -ENODEV : disk->ops->read(disk, lba, count, buf);
    disk_unlock(disk);
    if (rc < 0) {
        disk->errors++; disk->last_error = rc;
        if (b != disk) { b->errors++; b->last_error = rc; }
        if (rc == -ENODEV) blk_mark_removed(disk);
    }
    return rc;
}

int blk_read_bytes(struct blkdev *b, uint64_t off, void *buf, size_t len)
{
    uint64_t size = blk_size_bytes(b);
    if (off > size || len > size - off) return -ERANGE;
    if (!len) return 0;
    uint32_t bs = b->block_size;
    size_t cap = BOUNCE_MAX;
    if (cap > ((len + bs - 1) / bs + 1) * bs) cap = ((len + bs - 1) / bs + 1) * bs;
    uint8_t *bounce = kmalloc(cap);             /* heap: identity mapped, DMA safe */
    if (!bounce) return -ENOMEM;

    size_t done = 0;
    int rc = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t lba = pos / bs;
        uint32_t skip = (uint32_t)(pos % bs);
        uint64_t want = (skip + (len - done) + bs - 1) / bs;
        uint32_t n = (uint32_t)(want < cap / bs ? want : cap / bs);
        if ((rc = blk_read(b, lba, n, bounce)) < 0) break;
        size_t chunk = (size_t)n * bs - skip;
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, bounce + skip, chunk);
        done += chunk;
    }
    kfree(bounce);
    return rc < 0 ? rc : 0;
}

/* ======================================================================== */
/*  /dev nodes                                                                 */
/* ======================================================================== */
static ssize_t node_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    struct blkdev *b = vn->ctx;
    uint64_t size = blk_size_bytes(b);
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    if (len > (1u << 20)) len = 1u << 20;           /* bounded per call */
    int rc = blk_read_bytes(b, off, buf, len);
    return rc < 0 ? rc : (ssize_t)len;
}
static ssize_t node_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    (void)vn; (void)buf; (void)len; (void)off;
    return -EROFS;
}
static uint64_t node_size(struct vnode *vn) { return blk_size_bytes(vn->ctx); }
static const struct vnode_ops node_ops = { .read = node_read, .write = node_write, .size = node_size };

void blk_publish(void)
{
    for (int i = 0; i < ndev; i++) {
        struct blkdev *b = devs[i];
        if (b->node) continue;
        b->node = devfs_register(b->name, 0440, (uint32_t)(BLK_MAJOR << 8 | (i & 0xFF)), &node_ops, b);
    }
}

static struct blkdev *add(struct blkdev *b)
{
    if (ndev >= BLK_MAX) { kprintf("block: registry full, %s ignored\n", b->name); kfree(b); return NULL; }
    devs[ndev++] = b;
    blk_publish();                  /* no-op until devfs exists */
    return b;
}

/* ======================================================================== */
/*  registration                                                               */
/* ======================================================================== */
struct blkdev *blk_register_disk(const char *prefix, enum blk_kind kind, const char *model,
                                 uint32_t block_size, uint64_t blocks,
                                 const struct blkdev_ops *ops, void *ctx)
{
    if (!ops || !ops->read || block_size < 512 || block_size > 4096 || (block_size & (block_size - 1)) || !blocks) {
        kprintf("block: refusing %s disk \"%s\": bad geometry %lu x %u\n", prefix, model ? model : "", blocks, block_size);
        return NULL;
    }
    int idx = 0;
    for (int i = 0; i < ndev; i++)
        if (!devs[i]->parent && !strncmp(devs[i]->name, prefix, strlen(prefix))) idx++;
    struct blkdev *b = kzalloc(sizeof *b);
    if (!b) return NULL;
    snprintf(b->name, sizeof b->name, "%s%d", prefix, idx);
    b->kind = kind;
    strlcpy(b->model, model ? model : "", sizeof b->model);
    b->block_size = block_size;
    b->blocks = blocks;
    b->ops = ops;
    b->ctx = ctx;
    uint64_t mib = blocks * block_size >> 20;
    kprintf("block: %s: \"%s\", %lu blocks of %u bytes (%lu %s)\n", b->name, b->model, blocks, block_size,
            mib >= 10240 ? mib >> 10 : mib, mib >= 10240 ? "GiB" : "MiB");
    return add(b);
}

void blk_mark_removed(struct blkdev *disk)
{
    for (int i = 0; i < ndev; i++)
        if (devs[i] == disk || blk_disk_of(devs[i]) == disk) {
            if (!devs[i]->removed) kprintf("block: %s removed\n", devs[i]->name);
            devs[i]->removed = true;
        }
}

static struct blkdev *add_partition(struct blkdev *disk, int no, uint64_t start, uint64_t count)
{
    if (start == 0 || start >= disk->blocks || count == 0 || count > disk->blocks - start) {
        kprintf("block: %s: partition %d (start %lu, %lu blocks) lies outside the disk, ignored\n",
                disk->name, no, start, count);
        return NULL;
    }
    struct blkdev *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    snprintf(p->name, sizeof p->name, "%sp%d", disk->name, no);
    p->kind = BLK_PART;
    strlcpy(p->model, disk->model, sizeof p->model);
    p->block_size = disk->block_size;
    p->blocks = count;
    p->parent = disk;
    p->start = start;
    p->part_no = no;
    return add(p) ? p : NULL;
}

/* ======================================================================== */
/*  MBR                                                                        */
/* ======================================================================== */
struct mbr_entry {
    uint8_t  status, chs_first[3], type, chs_last[3];
    uint32_t lba_first, sectors;
} __attribute__((packed));

static bool is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static int scan_mbr(struct blkdev *disk, const uint8_t *sec0, uint8_t *buf)
{
    const struct mbr_entry *e = (const struct mbr_entry *)(sec0 + 446);
    int found = 0;
    for (int i = 0; i < 4; i++) {
        if (!e[i].type || !e[i].sectors) continue;
        if (is_extended(e[i].type)) {
            /* logical partitions: EBR chain, links relative to the extended start */
            uint64_t ext = e[i].lba_first, ebr = ext;
            for (int n = 0; n < MAX_LOGICAL; n++) {
                if (ebr >= disk->blocks || blk_read(disk, ebr, 1, buf) < 0) break;
                if (buf[510] != 0x55 || buf[511] != 0xAA) {
                    kprintf("block: %s: EBR at %lu has no signature, chain ends\n", disk->name, ebr);
                    break;
                }
                const struct mbr_entry *l = (const struct mbr_entry *)(buf + 446);
                if (l[0].type && l[0].sectors) {
                    struct blkdev *p = add_partition(disk, 5 + n, ebr + l[0].lba_first, l[0].sectors);
                    if (p) { p->mbr_type = l[0].type; found++; }
                }
                if (!is_extended(l[1].type) || !l[1].lba_first) break;
                uint64_t next = ext + l[1].lba_first;
                if (next <= ebr) { kprintf("block: %s: EBR chain loops, stopped\n", disk->name); break; }
                ebr = next;
            }
            continue;
        }
        struct blkdev *p = add_partition(disk, i + 1, e[i].lba_first, e[i].sectors);
        if (p) { p->mbr_type = e[i].type; found++; }
    }
    return found;
}

/* ======================================================================== */
/*  GPT                                                                        */
/* ======================================================================== */
struct gpt_header {
    char     signature[8];          /* "EFI PART" */
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t  type_guid[16], part_guid[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[36];
} __attribute__((packed));

static bool guid_zero(const uint8_t *g) { for (int i = 0; i < 16; i++) if (g[i]) return false; return true; }

/* Validate the header at `lba` and load its entry array. Returns the
 * array (kmalloc'd) or NULL. */
static uint8_t *load_gpt(struct blkdev *disk, uint64_t lba, uint8_t *buf, struct gpt_header *out)
{
    if (lba == 0 || lba >= disk->blocks || blk_read(disk, lba, 1, buf) < 0) return NULL;
    struct gpt_header h;
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.signature, "EFI PART", 8)) return NULL;
    if (h.header_size < 92 || h.header_size > disk->block_size) {
        kprintf("block: %s: GPT header at %lu: bad size %u\n", disk->name, lba, h.header_size);
        return NULL;
    }
    uint32_t want = h.header_crc;
    ((struct gpt_header *)buf)->header_crc = 0;
    if (crc32(0, buf, h.header_size) != want) {
        kprintf("block: %s: GPT header at %lu: CRC mismatch\n", disk->name, lba);
        return NULL;
    }
    if (h.my_lba != lba || h.entry_size < 128 || h.entry_size > 1024 || (h.entry_size & 7) ||
        h.entry_count == 0 || h.entry_count > 1024 || h.entries_lba >= disk->blocks) {
        kprintf("block: %s: GPT header at %lu: implausible layout\n", disk->name, lba);
        return NULL;
    }
    size_t bytes = (size_t)h.entry_count * h.entry_size;
    uint32_t blocks = (uint32_t)((bytes + disk->block_size - 1) / disk->block_size);
    if (h.entries_lba + blocks > disk->blocks) return NULL;
    uint8_t *arr = kmalloc((size_t)blocks * disk->block_size);
    if (!arr) return NULL;
    if (blk_read(disk, h.entries_lba, blocks, arr) < 0 || crc32(0, arr, bytes) != h.entries_crc) {
        kprintf("block: %s: GPT entry array at %lu: unreadable or CRC mismatch\n", disk->name, h.entries_lba);
        kfree(arr);
        return NULL;
    }
    *out = h;
    return arr;
}

static int scan_gpt(struct blkdev *disk, uint8_t *buf)
{
    struct gpt_header h;
    uint8_t *arr = load_gpt(disk, 1, buf, &h);
    if (!arr) {
        kprintf("block: %s: primary GPT invalid, trying the backup at LBA %lu\n", disk->name, disk->blocks - 1);
        arr = load_gpt(disk, disk->blocks - 1, buf, &h);
    }
    if (!arr) { kprintf("block: %s: no valid GPT\n", disk->name); return -EIO; }

    int found = 0;
    for (uint32_t i = 0; i < h.entry_count; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(arr + (size_t)i * h.entry_size);
        if (guid_zero(e->type_guid)) continue;
        if (e->last_lba < e->first_lba) continue;
        struct blkdev *p = add_partition(disk, (int)i + 1, e->first_lba, e->last_lba - e->first_lba + 1);
        if (!p) continue;
        p->gpt = true;
        memcpy(p->type_guid, e->type_guid, 16);
        memcpy(p->part_guid, e->part_guid, 16);
        int n = 0;
        for (int c = 0; c < 36 && e->name[c]; c++)
            p->part_name[n++] = e->name[c] < 0x80 && e->name[c] >= 0x20 ? (char)e->name[c] : '?';
        p->part_name[n] = 0;
        found++;
    }
    kfree(arr);
    return found;
}

int blk_scan_partitions(struct blkdev *disk)
{
    if (!disk || disk->parent) return -EINVAL;
    if (disk->partitions_scanned) {
        int n = 0;
        for (int i = 0; i < ndev; i++) if (devs[i]->parent == disk) n++;
        return n;
    }
    uint8_t *sec0 = kmalloc(disk->block_size);
    uint8_t *buf = kmalloc(disk->block_size);
    if (!sec0 || !buf) { kfree(sec0); kfree(buf); return -ENOMEM; }

    int rc = blk_read(disk, 0, 1, sec0);
    if (rc < 0) {
        kprintf("block: %s: cannot read block 0: %s\n", disk->name, blk_strerror(rc));
    } else if (sec0[510] != 0x55 || sec0[511] != 0xAA) {
        kprintf("block: %s: no partition table (no 55AA signature)\n", disk->name);
        rc = 0;
    } else {
        const struct mbr_entry *e = (const struct mbr_entry *)(sec0 + 446);
        bool protective = false;
        for (int i = 0; i < 4; i++) if (e[i].type == 0xEE) protective = true;
        rc = protective ? scan_gpt(disk, buf) : scan_mbr(disk, sec0, buf);
        if (rc >= 0) kprintf("block: %s: %s, %d partition(s)\n", disk->name, protective ? "GPT" : "MBR", rc);
    }
    if (rc >= 0) disk->partitions_scanned = true;
    kfree(sec0);
    kfree(buf);
    return rc;
}

/* ======================================================================== */
/*  /proc/partitions                                                           */
/* ======================================================================== */
size_t blk_proc(char *buf, size_t cap)
{
    size_t n = 0;
#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))
    P("NAME         BLOCKS        BSIZE  SIZE(MiB)  TYPE          STATE    MODEL / NAME\n");
    for (int i = 0; i < ndev; i++) {
        struct blkdev *b = devs[i];
        char type[40];
        if (b->kind == BLK_PART && b->gpt) { char g[37]; blk_guid_str(b->type_guid, g); snprintf(type, sizeof type, "gpt %.8s", g); }
        else if (b->kind == BLK_PART) snprintf(type, sizeof type, "mbr 0x%02x", b->mbr_type);
        else snprintf(type, sizeof type, "%s disk", b->kind == BLK_DISK_USB ? "usb" : "sata");
        char label[48] = "";
        if (b->kind != BLK_PART) strlcpy(label, b->model, sizeof label);
        else if (b->part_name[0]) snprintf(label, sizeof label, "\"%s\"", b->part_name);
        P("%-12s %-13lu %-6u %-10lu %-13s %-8s %s\n", b->name, b->blocks, b->block_size,
          blk_size_bytes(b) >> 20, type, b->removed ? "removed" : b->errors ? "errors" : "ok", label);
    }
#undef P
    return n;
}
