/* fs/tarfs.c -- read-only ustar file system backed by a block device range
 *
 * The directory tree is built once at mount time by walking the archive's
 * 512-byte headers (each header checksum is verified); file contents stay
 * on the device and are read on demand through the block layer, so a
 * device that disappears later produces -EIO/-ENODEV on read instead of a
 * crash. Supported entries: regular files ('0', '\0'), directories ('5'),
 * hard links to earlier files ('1': another name for the same data, e.g.
 * busybox applets), GNU long names ('L'); pax headers, symbolic links and
 * devices are skipped. */
#include <kernel/tarfs.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/posix.h>

#define TAR_BLOCK       512
#define TAR_MAX_ENTRIES 8192

struct ustar {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8];
    char typeflag, linkname[100], magic[6], version[2], uname[32], gname[32];
    char devmajor[8], devminor[8], prefix[155], pad[12];
} __attribute__((packed));
_Static_assert(sizeof(struct ustar) == TAR_BLOCK, "ustar header is 512 bytes");

struct tar_file { struct blkdev *dev; uint64_t offset; };

static uint64_t octal(const char *s, size_t n)
{
    if ((unsigned char)s[0] & 0x80) {           /* GNU base-256 for large values */
        uint64_t v = (unsigned char)s[0] & 0x7F;
        for (size_t i = 1; i < n; i++) v = v << 8 | (unsigned char)s[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == 0)) i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

static bool checksum_ok(const struct ustar *h)
{
    const uint8_t *b = (const uint8_t *)h;
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += (i >= 148 && i < 156) ? (uint8_t)' ' : b[i];
    return sum == octal(h->chksum, sizeof h->chksum);
}

static bool all_zero(const uint8_t *b) { for (int i = 0; i < TAR_BLOCK; i++) if (b[i]) return false; return true; }

/* ---- file reads ------------------------------------------------------------ */
static ssize_t tar_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    struct tar_file *tf = vn->ctx;
    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = (size_t)(vn->size - off);
    int rc = blk_read_bytes(tf->dev, tf->offset + off, buf, len);
    if (rc < 0) {
        kprintf("tarfs: read of %s at %lu failed on %s: %s\n", vn->name, off, tf->dev->name, blk_strerror(rc));
        return rc == -ENODEV ? -ENODEV : -EIO;
    }
    return (ssize_t)len;
}
static ssize_t tar_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    (void)vn; (void)buf; (void)len; (void)off;
    return -EROFS;
}
static const struct vnode_ops tar_file_ops = { .read = tar_read, .write = tar_write };

/* ---- tree construction ------------------------------------------------------ */
static struct vnode *child(struct vnode *dir, const char *name)
{
    for (struct vnode *c = dir->children; c; c = c->sibling) if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* The file at archive path `path` (already in the tree), or NULL. */
static struct vnode *lookup_rel(struct vnode *root, const char *path)
{
    char buf[256];
    strlcpy(buf, path, sizeof buf);
    struct vnode *v = root;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "/", &save); tok && v; tok = strtok_r(NULL, "/", &save))
        if (strcmp(tok, ".")) v = child(v, tok);
    return v;
}

/* Walk/create the directories of `path`; returns the parent directory and
 * leaves the last component in `leaf`. */
static struct vnode *make_parents(struct mount *m, struct vnode *root, char *path, char **leaf)
{
    struct vnode *dir = root;
    char *save = NULL, *tok = strtok_r(path, "/", &save), *next;
    while (tok) {
        next = strtok_r(NULL, "/", &save);
        if (!next) break;
        if (!strcmp(tok, ".")) { tok = next; continue; }
        if (!strcmp(tok, "..") || strlen(tok) >= VFS_NAME_MAX) return NULL;
        struct vnode *c = child(dir, tok);
        if (!c) {
            c = vfs_node_new(m, tok, VDIR, 0555, &ramfs_ops);
            if (!c) return NULL;
            vfs_node_add(dir, c);
        } else if (c->type != VDIR) {
            return NULL;
        }
        dir = c;
        tok = next;
    }
    *leaf = tok;
    return dir;
}

int tarfs_mount(struct blkdev *dev, uint64_t offset, uint64_t length, const char *path,
                struct tarfs_stats *stats)
{
    struct tarfs_stats st = { 0 };
    if (!dev || length < TAR_BLOCK) return -EINVAL;
    struct mount *m;
    struct vnode *root = vfs_mkfs("tarfs", MNT_RDONLY | MNT_NOCREATE, &m);
    if (!root) return -ENOMEM;
    root->mode = S_IFDIR | 0555;

    uint8_t *hdr = kmalloc(TAR_BLOCK);
    char *name = kmalloc(512);
    char *longname = kmalloc(512);
    if (!hdr || !name || !longname) { kfree(hdr); kfree(name); kfree(longname); return -ENOMEM; }
    longname[0] = 0;

    int rc = 0;
    uint64_t pos = 0;
    while (pos + TAR_BLOCK <= length) {
        if ((rc = blk_read_bytes(dev, offset + pos, hdr, TAR_BLOCK)) < 0) {
            kprintf("tarfs: %s: header read at +%lu failed: %s\n", dev->name, pos, blk_strerror(rc));
            break;
        }
        if (all_zero(hdr)) break;                       /* end-of-archive marker */
        const struct ustar *h = (const struct ustar *)hdr;
        if (memcmp(h->magic, "ustar", 5) || !checksum_ok(h)) {
            kprintf("tarfs: %s: bad header at +%lu (magic/checksum), archive truncated here\n", dev->name, pos);
            st.bad_headers++;
            rc = st.files ? 0 : -EINVAL;
            break;
        }
        uint64_t size = octal(h->size, sizeof h->size);
        uint64_t data = pos + TAR_BLOCK;
        uint64_t next = data + ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
        if (next > length) {
            kprintf("tarfs: %s: entry at +%lu runs past the volume end\n", dev->name, pos);
            rc = -EINVAL;
            break;
        }
        if (++st.entries > TAR_MAX_ENTRIES) { kprintf("tarfs: too many entries, stopping\n"); break; }

        if (h->typeflag == 'L') {                       /* GNU long name: data is the name */
            size_t n = size < 511 ? (size_t)size : 511;
            if ((rc = blk_read_bytes(dev, offset + data, longname, n)) < 0) break;
            longname[n] = 0;
            pos = next;
            continue;
        }
        if (longname[0]) {
            strlcpy(name, longname, 512);
            longname[0] = 0;
        } else if (h->prefix[0]) {
            snprintf(name, 512, "%.155s/%.100s", h->prefix, h->name);
        } else {
            snprintf(name, 512, "%.100s", h->name);
        }

        bool is_dir = h->typeflag == '5', is_file = h->typeflag == '0' || h->typeflag == 0;
        bool is_link = h->typeflag == '1';
        if (!is_dir && !is_file && !is_link) {
            if (h->typeflag != 'x' && h->typeflag != 'g')
                kprintf("tarfs: skipping %s (type '%c')\n", name, h->typeflag);
            st.skipped++;
            pos = next;
            continue;
        }
        char *leaf = NULL;
        struct vnode *dir = make_parents(m, root, name, &leaf);
        if (!dir || !leaf || !strcmp(leaf, ".")) { if (!is_dir) st.skipped++; pos = next; continue; }
        if (strlen(leaf) >= VFS_NAME_MAX) { kprintf("tarfs: name too long: %s\n", leaf); st.skipped++; pos = next; continue; }

        uint32_t mode = (uint32_t)octal(h->mode, sizeof h->mode) & 0777;
        struct vnode *vn = child(dir, leaf);
        if (is_link) {
            char target[101];
            snprintf(target, sizeof target, "%.100s", h->linkname);
            struct vnode *t = lookup_rel(root, target);
            if (!vn && t && t->type == VREG && t->ops == &tar_file_ops) {
                vn = vfs_node_new(m, leaf, VREG, mode ? mode : t->mode & 0777, &tar_file_ops);
                if (!vn) { rc = -ENOMEM; break; }
                vn->ctx = t->ctx;                       /* same data on the device */
                vn->size = t->size;
                vfs_node_add(dir, vn);
                st.files++;
            } else if (!t) {
                kprintf("tarfs: %s: link target %s not found\n", name, target);
                st.skipped++;
            }
        } else if (is_dir) {
            if (!vn) {
                vn = vfs_node_new(m, leaf, VDIR, mode ? mode : 0555, &ramfs_ops);
                if (vn) vfs_node_add(dir, vn);
            }
            if (vn) st.dirs++;
        } else if (!vn) {
            struct tar_file *tf = kmalloc(sizeof *tf);
            vn = tf ? vfs_node_new(m, leaf, VREG, mode ? mode : 0444, &tar_file_ops) : NULL;
            if (!vn) { kfree(tf); rc = -ENOMEM; break; }
            tf->dev = dev;
            tf->offset = offset + data;
            vn->ctx = tf;
            vn->size = size;
            vfs_node_add(dir, vn);
            st.files++;
            st.bytes += size;
        }
        if (vn) vn->mtime = octal(h->mtime, sizeof h->mtime);
        pos = next;
    }
    kfree(hdr); kfree(name); kfree(longname);
    if (rc < 0) return rc;

    struct vnode *mp;
    if (vfs_lookup(path, &mp) < 0 && (rc = vfs_mkdir(path, 0755)) < 0) return rc;
    if ((rc = vfs_mount(m, path)) < 0) return rc;
    m->dev = (uint32_t)(dev->node ? dev->node->rdev : m->dev);
    kprintf("tarfs: %s mounted on %s: %u files (%lu bytes), %u directories, %u skipped\n",
            dev->name, path, st.files, st.bytes, st.dirs, st.skipped);
    if (stats) *stats = st;
    return 0;
}
