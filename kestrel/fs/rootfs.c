/* fs/rootfs.c -- the root file system on disk
 *
 * At boot "/" is an in-memory tree (fs/vfs.c) with /dev, /proc, /sys, and
 * the boot volume on /boot. If a disk carries an ext2 volume labelled
 * "kestrel-root", it becomes "/" instead: the in-memory mounts move into it,
 * /tmp gets a fresh tmpfs, and files the system needs in /etc that the
 * volume lacks are written there once (hostname, passwd, group, profile,
 * motd, os-release). Only a volume chosen this way is ever written: other
 * Linux file systems on the machine are not touched.
 *
 * Command line:
 *   kestrel.rootfs=auto          the volume labelled kestrel-root (default)
 *   kestrel.rootfs=LABEL=<name>  the ext2 volume with that label
 *   kestrel.rootfs=<device>      that partition or disk, e.g. sata0p2
 *   kestrel.rootfs=none          stay on the in-memory root
 *   kestrel.rootro=1             mount it read-only */
#include <kernel/vfs.h>
#include <kernel/ext2.h>
#include <kernel/block.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/posix.h>
#include <kernel/rootfs.h>

#define DEFAULT_LABEL "kestrel-root"

static char root_dev[32];

const char *rootfs_device(void) { return root_dev[0] ? root_dev : NULL; }

static bool cmdline_get(const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *s = g_boot.cmdline; *s; s++)
        if ((s == g_boot.cmdline || s[-1] == ' ') && !strncmp(s, key, k) && s[k] == '=') {
            s += k + 1;
            size_t n = 0;
            while (s[n] && s[n] != ' ' && n + 1 < cap) { out[n] = s[n]; n++; }
            out[n] = 0;
            return true;
        }
    return false;
}

static struct blkdev *find_label(const char *want)
{
    for (int i = 0; i < blk_count(); i++) {             /* partition tables first */
        struct blkdev *b = blk_get(i);
        if (b && !b->parent && !b->removed && !b->partitions_scanned) blk_scan_partitions(b);
    }
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *b = blk_get(i);
        char label[17];
        if (!b || b->removed) continue;
        if (!b->parent) {                               /* a whole disk: only if it has no partitions */
            bool parts = false;
            for (int j = 0; j < blk_count(); j++) if (blk_get(j)->parent == b) { parts = true; break; }
            if (parts) continue;
        }
        if (ext2_probe(b, label) && !strcmp(label, want)) return b;
    }
    return NULL;
}

/* Copy the in-memory /etc file `name` (saved before the switch) to the new
 * root if it has none. */
static void seed(const char *name, const char *text)
{
    char path[64];
    struct vnode *vn;
    snprintf(path, sizeof path, "/etc/%s", name);
    if (!text || vfs_lookup(path, &vn) == 0) return;
    struct file *f;
    if (vfs_open(path, O_WRONLY | O_CREAT | O_EXCL, 0644, &f) < 0) return;
    vfs_write(f, text, strlen(text));
    vfs_close(f);
    kprintf("rootfs: %s written (the volume had none)\n", path);
}

static char *slurp(const char *path)
{
    struct file *f;
    if (vfs_open(path, O_RDONLY, 0, &f) < 0) return NULL;
    char *buf = kzalloc(4096);
    ssize_t n = buf ? vfs_read(f, buf, 4095) : -1;
    vfs_close(f);
    if (n < 0) { kfree(buf); return NULL; }
    return buf;
}

int rootfs_mount(void)
{
    char v[48] = "auto";
    cmdline_get("kestrel.rootfs", v, sizeof v);
    if (!strcmp(v, "none")) { kprintf("rootfs: staying on the in-memory root (kestrel.rootfs=none)\n"); return -ENOENT; }
    bool ro = cmdline_get("kestrel.rootro", root_dev, sizeof root_dev) && root_dev[0] == '1';
    root_dev[0] = 0;

    struct blkdev *dev;
    if (!strcmp(v, "auto")) dev = find_label(DEFAULT_LABEL);
    else if (!strncmp(v, "LABEL=", 6)) dev = find_label(v + 6);
    else {
        dev = blk_find(v);
        if (dev && !ext2_probe(dev, NULL)) { kprintf("rootfs: /dev/%s has no ext2 file system\n", v); return -EINVAL; }
    }
    if (!dev) {
        if (strcmp(v, "auto")) kprintf("rootfs: %s not found; staying on the in-memory root\n", v);
        return -ENOENT;
    }

    struct mount *m;
    int r = ext2_mount(dev, ro, &m);
    if (r < 0) { kprintf("rootfs: /dev/%s: cannot mount (%d); staying on the in-memory root\n", dev->name, r); return r; }

    static const char *const etc[] = { "hostname", "passwd", "group", "profile", "motd", "os-release" };
    char *saved[sizeof etc / sizeof *etc];
    for (size_t i = 0; i < sizeof etc / sizeof *etc; i++) {
        char path[64];
        snprintf(path, sizeof path, "/etc/%s", etc[i]);
        saved[i] = slurp(path);
    }

    vfs_pivot_root(m);
    strlcpy(root_dev, dev->name, sizeof root_dev);

    struct vnode *vn;
    if (!(m->flags & MNT_RDONLY)) {
        if (vfs_lookup("/etc", &vn) < 0) vfs_mkdir("/etc", 0755);
        for (size_t i = 0; i < sizeof etc / sizeof *etc; i++) seed(etc[i], saved[i]);
        if (vfs_symlink_at("/proc/net/pnp", NULL, "/etc/resolv.conf") == 0)    /* fails with EEXIST if present */
            kprintf("rootfs: /etc/resolv.conf -> /proc/net/pnp written (the volume had none)\n");
        if (vfs_lookup("/root", &vn) < 0) vfs_mkdir("/root", 0700);
    }
    for (size_t i = 0; i < sizeof etc / sizeof *etc; i++) kfree(saved[i]);

    struct mount *tmp;                                  /* scratch space in memory, empty at each boot */
    vfs_mkfs("tmpfs", 0, &tmp)->mode = S_IFDIR | 01777;
    if (vfs_lookup("/tmp", &vn) < 0 && !(m->flags & MNT_RDONLY)) vfs_mkdir("/tmp", 01777);
    if (vfs_mount(tmp, "/tmp") < 0) kprintf("rootfs: no /tmp on the root volume: /tmp unavailable\n");
    kprintf("rootfs: \033[92m/ is now /dev/%s\033[0m (ext2%s)\n", dev->name, (m->flags & MNT_RDONLY) ? ", read-only" : "");
    return 0;
}
