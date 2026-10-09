/* kernel/storage.c -- glue between the PCI enumerator, the standalone AHCI
 * and NVMe drivers and the rest of Kestrel.
 *
 *   storage_init()              boot: find AHCI and NVMe controllers via PCI,
 *                               enable MMIO + bus mastering, map the register
 *                               BARs uncached, bring the controllers up, run
 *                               the MBR verify loop on every SATA disk (and the
 *                               self-tests on request)
 *   storage_register_devices()  after the VFS exists: /dev/sda, /dev/sdb ...,
 *                               sataN and nvmeN in the block layer
 *   storage_shutdown()          before reboot/power-off: NVMe normal shutdown
 *
 * Kernel command line options:
 *   ahci.selftest=rw    non-destructive write/read-back/restore on the last
 *                       sector of each disk (off by default)
 *   ahci.verify=N       MBR read passes in the verify loop (default 3)
 *   nvme.selftest=1     read the first 8 MiB of every namespace in one call
 *                       and again in odd-sized chunks into a misaligned
 *                       buffer, compare, log the CRC-32 (off by default)
 *   nvme.selftest=rw    the same, plus write/read-back/restore of the last
 *                       16 blocks
 */
#include <kernel/storage.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/cpu.h>
#include <kernel/block.h>
#include <kernel/posix.h>
#include <kernel/time.h>
#include <kernel/task.h>
#include <kernel/crc32.h>
#include "pci.h"
#include "ahci.h"
#include "nvme.h"

/* ---- platform hooks for the AHCI driver -------------------------------- */

/* PMM frames are 4 KiB aligned, physically contiguous, zeroed and identity
 * mapped: exactly what the HBA's command lists, FIS areas and tables need. */
static void *dma_alloc(size_t size, size_t align, uint64_t *phys)
{
    if (align > PAGE_SIZE) return NULL;
    uint64_t pa = pmm_alloc_contig((size + PAGE_SIZE - 1) / PAGE_SIZE);
    *phys = pa;
    return pa ? (void *)pa : NULL;
}

static void *map_mmio(uint64_t phys, size_t size)
{
    vmm_set_uncached(phys, size);       /* registers must never be cached */
    return (void *)phys;
}

static const struct ahci_platform kestrel_platform = {
    .dma_alloc = dma_alloc,
    .map_mmio  = map_mmio,
    .log_putc  = klog_putc,
    /* virt_to_phys: identity (default); delay_us: port 0x80 (default) */
};

/* Waiting for an NVMe controller that another namespace's request holds:
 * give the CPU away once the scheduler runs. */
static void nvme_yield(void)
{
    if (irqs_enabled()) sched_yield(); else cpu_relax();
}

static const struct nvme_platform kestrel_nvme = {
    .dma_alloc = dma_alloc,
    .map_mmio  = map_mmio,
    .delay_us  = udelay,
    .yield     = nvme_yield,
    .log_putc  = klog_putc,
    /* virt_to_phys: identity (default) */
};

/* ---- command line helpers ---------------------------------------------- */
static const char *cmdline_value(const char *key)
{
    size_t k = strlen(key);
    for (const char *s = g_boot.cmdline; *s; s++)
        if ((s == g_boot.cmdline || s[-1] == ' ') && strncmp(s, key, k) == 0 && s[k] == '=')
            return s + k + 1;
    return NULL;
}

/* ---- boot-time bring-up -------------------------------------------------- */
static void ahci_bringup(void)
{
    size_t n = pci_match_count(PCI_MATCH_AHCI);
    if (!n) { kprintf("storage: no AHCI controller found\n"); return; }

    for (size_t i = 0; i < n; i++) {
        struct pci_device *dev = pci_match_get(PCI_MATCH_AHCI, i);
        const struct pci_bar *abar = &dev->bars[5];          /* ABAR is always BAR5 */
        if ((abar->kind != PCI_BAR_MEM32 && abar->kind != PCI_BAR_MEM64) || !abar->base) {
            kprintf("storage: %02x:%02x.%x has no memory BAR5 (IDE-mode SATA?), skipped\n",
                    dev->bus, dev->device, dev->function);
            continue;
        }
        kprintf("storage: AHCI %04x:%04x at %02x:%02x.%x, ABAR %lx (%lu KiB)\n",
                dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function,
                abar->base, abar->size >> 10);
        /* MMIO decoding for the registers, bus mastering for the DMA engine;
         * legacy INTx off since the driver polls. */
        pci_enable(dev, false, true, true);
        pci_write32(dev->bus, dev->device, dev->function, PCI_REG_COMMAND,
                    pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND) | PCI_CMD_INTX_DISABLE);
        ahci_init(abar->base, &kestrel_platform);
    }

    /* Debug verify loop: read the MBR of every attached disk. */
    const char *v = cmdline_value("ahci.verify");
    int passes = v ? (int)strtol(v, NULL, 10) : 3;
    const char *st = cmdline_value("ahci.selftest");
    bool selftest = st && strncmp(st, "rw", 2) == 0;

    for (int i = 0; i < ahci_disk_count(); i++) {
        struct ahci_disk *d = ahci_disk_get(i);
        ahci_verify_mbr(d, passes);
        if (selftest) ahci_selftest_rw(d);
    }
}

/* Reads the start of the namespace twice -- once in a single call (split
 * into max-transfer commands with PRP lists), once in odd chunk sizes into a
 * buffer 4 bytes past a page boundary, so PRP1 carries an offset and every
 * list straddles pages -- and compares the copies. With `rw`, the last 16
 * blocks are saved, overwritten, read back and restored. */
static void nvme_selftest(struct nvme_namespace *ns, int index, bool rw)
{
    uint32_t bs = ns->block_size;
    uint64_t nblk64 = (8u << 20) / bs;
    if (nblk64 > ns->blocks) nblk64 = ns->blocks;
    uint32_t nblk = (uint32_t)nblk64;
    size_t bytes = (size_t)nblk * bs;
    size_t frames = bytes / PAGE_SIZE + 2;
    uint64_t a_pa = pmm_alloc_contig(frames), b_pa = pmm_alloc_contig(frames);
    if (!a_pa || !b_pa) {
        kprintf("nvme selftest: ns%d: out of memory\n", index);
        if (a_pa) pmm_free_contig(a_pa, frames);
        if (b_pa) pmm_free_contig(b_pa, frames);
        return;
    }
    uint8_t *a = (uint8_t *)a_pa, *b = (uint8_t *)b_pa + 4;
    int failures = 0;

    uint64_t t0 = time_us();
    int rc = nvme_read(ns, 0, nblk, a);
    uint64_t us = time_us() - t0;
    if (rc != NVME_OK) { kprintf("nvme selftest: ns%d: single read failed: %s\n", index, nvme_strerror(rc)); failures++; }
    else kprintf("nvme selftest: ns%d: %u blocks (%lu KiB) in one call: %lu us, crc32 %08x\n",
                 index, nblk, (uint64_t)bytes >> 10, us, crc32(0, a, bytes));

    static const uint32_t sizes[] = { 1, 7, 8, 9, 255, 256, 257, 0 /* max_blocks */, 3, 0 /* max_blocks + 1 */ };
    uint32_t lba = 0, cmds = 0;
    for (int k = 0; rc == NVME_OK && lba < nblk; k = (k + 1) % 10, cmds++) {
        uint32_t n = sizes[k] ? sizes[k] : ns->max_blocks + (k == 9);
        if (n > nblk - lba) n = nblk - lba;
        rc = nvme_read(ns, lba, n, b + (size_t)lba * bs);
        if (rc != NVME_OK) { kprintf("nvme selftest: ns%d: chunked read at %u+%u failed: %s\n", index, lba, n, nvme_strerror(rc)); failures++; }
        lba += n;
    }
    if (rc == NVME_OK) {
        if (memcmp(a, b, bytes)) { kprintf("nvme selftest: ns%d: chunked read differs\n", index); failures++; }
        else kprintf("nvme selftest: ns%d: %u chunked calls into a misaligned buffer match\n", index, cmds);
    }

    /* argument checks: a 2-byte aligned buffer and a range past the end */
    if (nvme_read(ns, 0, 1, (uint8_t *)b_pa + 2) != NVME_ERR_ALIGN) { kprintf("nvme selftest: ns%d: misaligned buffer accepted\n", index); failures++; }
    if (nvme_read(ns, ns->blocks - 1, 2, a) != NVME_ERR_RANGE) { kprintf("nvme selftest: ns%d: read past the end accepted\n", index); failures++; }

    if (rw && nblk >= 48) {
        uint32_t cnt = 16;
        uint64_t last = ns->blocks - cnt;
        uint8_t *save = a, *pat = b, *back = a + (size_t)cnt * bs;     /* reuse the buffers */
        for (size_t i = 0; i < (size_t)cnt * bs; i++) pat[i] = (uint8_t)(i * 7 + (i >> 9) + 0x5A);
        rc = nvme_read(ns, last, cnt, save);
        if (rc == NVME_OK) rc = nvme_write(ns, last, cnt, pat);
        if (rc == NVME_OK) rc = nvme_read(ns, last, cnt, back);
        bool same = rc == NVME_OK && !memcmp(pat, back, (size_t)cnt * bs);
        int rc2 = nvme_write(ns, last, cnt, save);
        if (rc2 == NVME_OK) rc2 = nvme_flush(ns);
        if (rc2 == NVME_OK) rc2 = nvme_read(ns, last, cnt, back);
        bool restored = rc2 == NVME_OK && !memcmp(save, back, (size_t)cnt * bs);
        kprintf("nvme selftest: ns%d: write/read-back of blocks %lu-%lu: %s, restore: %s\n", index,
                last, last + cnt - 1, same ? "ok" : "FAILED", restored ? "ok" : "FAILED");
        failures += !same + !restored;
    }
    kprintf("nvme selftest: ns%d: %s\n", index, failures ? "FAILED" : "passed");
    pmm_free_contig(a_pa, frames);
    pmm_free_contig(b_pa, frames);
}

static void nvme_bringup(void)
{
    size_t n = pci_match_count(PCI_MATCH_NVME);
    if (!n) { kprintf("storage: no NVMe controller found\n"); return; }

    for (size_t i = 0; i < n; i++) {
        struct pci_device *dev = pci_match_get(PCI_MATCH_NVME, i);
        const struct pci_bar *bar = &dev->bars[0];          /* MLBAR/MUBAR: BAR0 (+BAR1) */
        if (dev->prog_if != 0x02) {
            kprintf("storage: %02x:%02x.%x is class 01.08 prog-if %02x, not NVM Express, skipped\n",
                    dev->bus, dev->device, dev->function, dev->prog_if);
            continue;
        }
        if ((bar->kind != PCI_BAR_MEM32 && bar->kind != PCI_BAR_MEM64) || !bar->base) {
            kprintf("storage: NVMe at %02x:%02x.%x has no memory BAR0, skipped\n",
                    dev->bus, dev->device, dev->function);
            continue;
        }
        kprintf("storage: NVMe %04x:%04x at %02x:%02x.%x, BAR0 %lx (%lu KiB)\n",
                dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function,
                bar->base, bar->size >> 10);
        pci_enable(dev, false, true, true);
        pci_write32(dev->bus, dev->device, dev->function, PCI_REG_COMMAND,
                    pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND) | PCI_CMD_INTX_DISABLE);
        int rc = nvme_init(bar->base, &kestrel_nvme);
        if (rc < 0) kprintf("storage: NVMe at %02x:%02x.%x: %s\n", dev->bus, dev->device, dev->function, nvme_strerror(rc));
    }

    const char *st = cmdline_value("nvme.selftest");
    if (st && *st && *st != '0' && *st != ' ')
        for (int i = 0; i < nvme_namespace_count(); i++)
            nvme_selftest(nvme_namespace_get(i), i, strncmp(st, "rw", 2) == 0);
}

void storage_init(void)
{
    ahci_bringup();
    nvme_bringup();
}

void storage_shutdown(void)
{
    nvme_shutdown_all();
}

/* ---- /dev/sdX: read-only byte-addressable view of each disk --------------- */
#define SD_BOUNCE (64 * 1024)

static ssize_t sd_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    struct ahci_disk *d = vn->ctx;
    uint64_t size = d->sectors * d->sector_size;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;

    uint8_t *bounce = kmalloc(SD_BOUNCE);          /* heap = identity-mapped PMM memory */
    if (!bounce) return -ENOMEM;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t lba = pos / d->sector_size;
        uint32_t skip = (uint32_t)(pos % d->sector_size);
        uint32_t secs = (uint32_t)((skip + (len - done) + d->sector_size - 1) / d->sector_size);
        if (secs > SD_BOUNCE / d->sector_size) secs = SD_BOUNCE / d->sector_size;
        if (secs > d->sectors - lba) secs = (uint32_t)(d->sectors - lba);
        int rc = ahci_read(d, lba, secs, bounce);
        if (rc != AHCI_OK) { kfree(bounce); return done ? (ssize_t)done : -EIO; }
        size_t chunk = (size_t)secs * d->sector_size - skip;
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, bounce + skip, chunk);
        done += chunk;
    }
    kfree(bounce);
    return (ssize_t)done;
}

static ssize_t sd_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    (void)vn; (void)buf; (void)len; (void)off;
    return -EROFS;          /* raw writes deliberately disabled; see ahci_write() */
}

static uint64_t sd_size(struct vnode *vn)
{
    struct ahci_disk *d = vn->ctx;
    return d->sectors * d->sector_size;
}

static const struct vnode_ops sd_ops = { .read = sd_read, .write = sd_write, .size = sd_size };

/* ---- block layer: "sataN" disks with MBR/GPT partitions ------------------ */
static int sata_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    int rc = ahci_read(b->ctx, lba, count, buf);
    return rc == AHCI_OK ? 0 : rc == AHCI_ERR_TIMEOUT ? -ETIMEDOUT : -EIO;
}
static int sata_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
    int rc = ahci_write(b->ctx, lba, count, buf);
    return rc == AHCI_OK ? 0 : rc == AHCI_ERR_TIMEOUT ? -ETIMEDOUT : -EIO;
}
static int sata_flush(struct blkdev *b) { return ahci_flush(b->ctx) == AHCI_OK ? 0 : -EIO; }
static const struct blkdev_ops sata_ops = { .read = sata_read, .write = sata_write, .flush = sata_flush };

/* ---- block layer: "nvmeN" disks, one per namespace ------------------------ */
static int nvme_blk_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    switch (nvme_read(b->ctx, lba, count, buf)) {
    case NVME_OK:          return 0;
    case NVME_ERR_TIMEOUT: return -ETIMEDOUT;
    case NVME_ERR_NODEV:   return -ENODEV;
    case NVME_ERR_RANGE:   return -ERANGE;
    case NVME_ERR_ALIGN:   return -EINVAL;
    default:               return -EIO;
    }
}
static int nvme_blk_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
    int rc = nvme_write(b->ctx, lba, count, buf);
    return rc == NVME_OK ? 0 : rc == NVME_ERR_TIMEOUT ? -ETIMEDOUT : rc == NVME_ERR_NODEV ? -ENODEV : -EIO;
}
static int nvme_blk_flush(struct blkdev *b) { return nvme_flush(b->ctx) == NVME_OK ? 0 : -EIO; }
static const struct blkdev_ops nvme_ops = { .read = nvme_blk_read, .write = nvme_blk_write, .flush = nvme_blk_flush };

void storage_register_devices(void)
{
    for (int i = 0; i < ahci_disk_count(); i++) {
        struct ahci_disk *d = ahci_disk_get(i);
        struct blkdev *b = blk_register_disk("sata", BLK_DISK_SATA, d->model, d->sector_size, d->sectors, &sata_ops, d);
        if (b) blk_scan_partitions(b);
    }
    for (int i = 0; i < nvme_namespace_count(); i++) {
        struct nvme_namespace *ns = nvme_namespace_get(i);
        struct blkdev *b = blk_register_disk("nvme", BLK_DISK_NVME, ns->model, ns->block_size, ns->blocks, &nvme_ops, ns);
        if (b) blk_scan_partitions(b);
    }
    for (int i = 0; i < ahci_disk_count() && i < 26; i++) {
        char name[8] = "sda";
        name[2] = (char)('a' + i);
        struct vnode *vn = devfs_register(name, 0440, (8u << 8) | (uint32_t)(i * 16),
                                          &sd_ops, ahci_disk_get(i));
        if (vn) kprintf("storage: /dev/%s -> AHCI port %d (%s)\n", name,
                        ahci_disk_get(i)->port, ahci_disk_get(i)->model);
    }
}

size_t storage_summary(char *buf, size_t cap)
{
    size_t n;
    if (!ahci_disk_count()) n = (size_t)snprintf(buf, cap, "no SATA disks");
    else {
        struct ahci_disk *d = ahci_disk_get(0);
        uint64_t mib = d->sectors * d->sector_size >> 20;
        n = (size_t)snprintf(buf, cap, "%d disk(s); sda: %s, %lu %s on port %d",
                             ahci_disk_count(), d->model,
                             mib >= 10240 ? mib >> 10 : mib, mib >= 10240 ? "GiB" : "MiB", d->port);
    }
    if (nvme_namespace_count() && n < cap)
        n += (size_t)snprintf(buf + n, cap - n, "; %d NVMe namespace(s)", nvme_namespace_count());
    return n;
}
