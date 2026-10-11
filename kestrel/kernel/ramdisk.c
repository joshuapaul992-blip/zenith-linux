/* kernel/ramdisk.c -- Multiboot2 modules as RAM disks
 *
 * GRUB can load files next to the kernel (module2). Each module becomes a
 * block device ram0, ram1, ... over its memory (reserved by pmm_init), so
 * the ISO can carry the Kestrel boot volume as a module and the normal
 * boot-volume discovery finds it there (sector-0 marker, see
 * tools/mkusbimg.py --ramdisk). The disks can be written: an ext2 root
 * carried the same way (make iso ROOTFS_BASE=...) works like a live CD's,
 * and its changes last until the machine is turned off. */
#include <kernel/block.h>
#include <kernel/multiboot2.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/posix.h>

struct ramdisk { uint8_t *base; uint64_t size; };
static struct ramdisk disks[4];

static int ram_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    const struct ramdisk *r = b->ctx;
    uint64_t off = lba * b->block_size, len = (uint64_t)count * b->block_size;
    if (off > r->size || len > r->size - off) return -EIO;
    memcpy(buf, r->base + off, len);
    return 0;
}

static int ram_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
    struct ramdisk *r = b->ctx;
    uint64_t off = lba * b->block_size, len = (uint64_t)count * b->block_size;
    if (off > r->size || len > r->size - off) return -EIO;
    memcpy(r->base + off, buf, len);
    return 0;
}

static const struct blkdev_ops ram_ops = { .read = ram_read, .write = ram_write };

void ramdisk_init(uintptr_t mbi)
{
    int n = 0;
    for (struct mb2_tag *t = mb2_first_tag(mbi); t->type != MB2_TAG_END && n < 4; t = mb2_next_tag(t)) {
        if (t->type != MB2_TAG_MODULE) continue;
        const struct mb2_tag_module *m = (const struct mb2_tag_module *)t;
        uint64_t size = (uint64_t)m->mod_end - m->mod_start;
        if (size < 512) continue;
        disks[n].base = (uint8_t *)(uintptr_t)m->mod_start;
        disks[n].size = size;
        struct blkdev *b = blk_register_disk("ram", BLK_DISK_RAM, "Multiboot2 module", 512, size / 512,
                                             &ram_ops, &disks[n]);
        kprintf("ramdisk: module \"%s\" at %x, %lu KiB -> /dev/%s\n", m->string, m->mod_start, size >> 10,
                b ? b->name : "?");
        n++;
    }
}
