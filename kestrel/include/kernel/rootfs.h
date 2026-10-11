/* include/kernel/rootfs.h -- the root file system on disk (fs/rootfs.c) */
#pragma once

/* Find the root volume (kestrel.rootfs=, default: the ext2 volume labelled
 * "kestrel-root") and make it "/". 0 on success; otherwise "/" stays the
 * in-memory tree. Call after the boot volume is mounted on /boot. */
int         rootfs_mount(void);
const char *rootfs_device(void);        /* "sata0p2", or NULL: in memory */
