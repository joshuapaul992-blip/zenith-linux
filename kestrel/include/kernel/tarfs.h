/* include/kernel/tarfs.h -- read-only ustar file system on a block device */
#ifndef KESTREL_TARFS_H
#define KESTREL_TARFS_H

#include <stdint.h>
#include <kernel/block.h>

struct tarfs_stats {
    uint32_t entries, files, dirs, skipped, bad_headers;
    uint64_t bytes;
};

/* Mount the archive stored at byte `offset` (length `length`) of `dev` on
 * `path` (created if missing). Returns 0 or -errno. */
int tarfs_mount(struct blkdev *dev, uint64_t offset, uint64_t length, const char *path,
                struct tarfs_stats *stats);

#endif
