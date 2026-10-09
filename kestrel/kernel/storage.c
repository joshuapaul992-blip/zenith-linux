/* kernel/storage.c -- glue between the PCI enumerator, the standalone AHCI
 * driver and the rest of Kestrel.
 *
 *   storage_init()              boot: find AHCI controllers via PCI, enable
 *                               MMIO + bus mastering, map ABAR uncached, bring
 *                               the HBA up, run the MBR verify loop on every
 *                               disk (and the write self-test on request)
 *   storage_register_devices()  after the VFS exists: /dev/sda, /dev/sdb ...
 *
 * Kernel command line options:
 *   ahci.selftest=rw    non-destructive write/read-back/restore on the last
 *                       sector of each disk (off by default)
 *   ahci.verify=N       MBR read passes in the verify loop (default 3)
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
#include "pci.h"
#include "ahci.h"

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
void storage_init(void)
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
static const struct blkdev_ops sata_ops = { .read = sata_read };

void storage_register_devices(void)
{
    for (int i = 0; i < ahci_disk_count(); i++) {
        struct ahci_disk *d = ahci_disk_get(i);
        struct blkdev *b = blk_register_disk("sata", BLK_DISK_SATA, d->model, d->sector_size, d->sectors, &sata_ops, d);
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
    if (!ahci_disk_count()) return (size_t)snprintf(buf, cap, "no SATA disks");
    struct ahci_disk *d = ahci_disk_get(0);
    uint64_t mib = d->sectors * d->sector_size >> 20;
    return (size_t)snprintf(buf, cap, "%d disk(s); sda: %s, %lu %s on port %d",
                            ahci_disk_count(), d->model,
                            mib >= 10240 ? mib >> 10 : mib, mib >= 10240 ? "GiB" : "MiB", d->port);
}
