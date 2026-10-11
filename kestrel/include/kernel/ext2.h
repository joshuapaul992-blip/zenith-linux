/* include/kernel/ext2.h -- the ext2 file system (fs/ext2.c) */
#pragma once
#include <kernel/block.h>
#include <kernel/vfs.h>

/* Is there an ext2/3/4 superblock on `dev`? Its volume label (up to 16
 * bytes, NUL-terminated) goes to `label` if not NULL. */
bool ext2_probe(struct blkdev *dev, char label[17]);
/* Mount the volume on `dev` (not yet attached to the tree: vfs_mount or
 * vfs_pivot_root). Read-only if asked, or if the device cannot write or
 * the volume uses features only a reader may ignore. */
int  ext2_mount(struct blkdev *dev, bool rdonly, struct mount **out);
/* Block cache lookups that found the block, and those that read it (kstat.h). */
void ext2_cache_stats(uint64_t *hits, uint64_t *misses);
