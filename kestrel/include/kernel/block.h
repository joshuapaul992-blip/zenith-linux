/* include/kernel/block.h -- block device registry and partition discovery
 *
 * Every disk driver (AHCI, NVMe, USB mass storage) registers its disks here; the
 * layer gives them stable kernel names ("sata0", "usb0"), reads MBR or GPT
 * partition tables into child devices ("usb0p1"), publishes all of them as
 * /dev nodes, bounds-checks every request and notices removed devices.
 * Consumers (boot volume discovery, tarfs) never deal with drivers or
 * hardcoded names like /dev/sda.
 *
 * Errors are negative errno values: -EIO, -ENODEV (device removed),
 * -ETIMEDOUT, -ENOMEDIUM, -ERANGE, -EINVAL, -ENOMEM. */
#ifndef KESTREL_BLOCK_H
#define KESTREL_BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BLK_NAME_MAX    16
#define BLK_MAX         48

enum blk_kind { BLK_DISK_SATA, BLK_DISK_USB, BLK_DISK_NVME, BLK_DISK_RAM, BLK_PART };

struct blkdev;
struct blkdev_ops {
    /* read `count` blocks; buf is identity-mapped kernel memory (DMA safe) */
    int  (*read)(struct blkdev *b, uint64_t lba, uint32_t count, void *buf);
    bool (*alive)(struct blkdev *b);        /* optional: still attached?       */
    /* optional: write `count` blocks (buf DMA safe), and flush caches */
    int  (*write)(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf);
    int  (*flush)(struct blkdev *b);
};

struct blkdev {
    char      name[BLK_NAME_MAX];
    enum blk_kind kind;
    char      model[48];
    uint32_t  block_size;
    uint64_t  blocks;
    const struct blkdev_ops *ops;           /* disks only                      */
    void     *ctx;

    /* partitions */
    struct blkdev *parent;
    uint64_t  start;                        /* first block on the parent       */
    int       part_no;                      /* 1-based                          */
    bool      gpt;
    uint8_t   mbr_type;
    uint8_t   type_guid[16], part_guid[16]; /* GPT, on-disk byte order          */
    char      part_name[37];

    bool      removed;
    volatile bool io_busy;                  /* one request at a time per disk  */
    bool      partitions_scanned;
    uint32_t  errors;
    int       last_error;
    struct vnode *node;                     /* /dev/<name>                       */
};

struct blkdev *blk_register_disk(const char *prefix, enum blk_kind kind, const char *model,
                                 uint32_t block_size, uint64_t blocks,
                                 const struct blkdev_ops *ops, void *ctx);
int  blk_scan_partitions(struct blkdev *disk);      /* number found, or -errno */
void blk_mark_removed(struct blkdev *disk);         /* disk and its partitions */
void blk_publish(void);                             /* create missing /dev nodes */
void ramdisk_init(uintptr_t mbi);                   /* Multiboot2 modules -> ramN (kernel/ramdisk.c) */

int            blk_count(void);
struct blkdev *blk_get(int index);
struct blkdev *blk_find(const char *name);
struct blkdev *blk_disk_of(struct blkdev *b);       /* partition -> its disk */

int  blk_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf);
int  blk_read_bytes(struct blkdev *b, uint64_t offset, void *buf, size_t len);
/* Raw block writes (kernel use only; /dev nodes stay read-only). -EROFS if
 * the driver cannot write. blk_flush() asks the device to commit its cache. */
int  blk_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf);
int  blk_flush(struct blkdev *b);
uint64_t blk_size_bytes(const struct blkdev *b);

const char *blk_strerror(int err);
void blk_guid_str(const uint8_t g[16], char out[37]);   /* GPT mixed-endian text */
size_t blk_proc(char *buf, size_t cap);             /* /proc/partitions */

#endif
