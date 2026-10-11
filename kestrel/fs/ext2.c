/* fs/ext2.c -- the second extended file system, read and write
 *
 * On-disk format: a superblock at byte 1024; block groups, each with a block
 * bitmap, an inode bitmap and an inode table, described by the group
 * descriptor table after the superblock; inodes map file blocks through 12
 * direct, one single-, one double- and one triple-indirect pointer;
 * directories are lists of variable-length records (inode, rec_len,
 * name_len, file_type, name). Symbolic links shorter than 60 bytes keep
 * their target in the inode itself ("fast" links).
 *
 * Features. Read and write: revision 0 and 1, 1/2/4 KiB blocks, any inode
 * size, file types in directory entries, sparse superblocks, large files,
 * flexible block groups, and ext3 volumes whose journal is empty (the
 * journal is left alone). Directories indexed by an htree (dir_index) are
 * read linearly; a directory we change loses its index flag, as e2fsck
 * expects of a kernel without htree support. Mounted read-only: features
 * whose metadata we would have to keep up (checksums, huge files, ...).
 * Refused: extents (ext4), 64-bit block numbers, journals that need
 * recovery, compression.
 *
 * In memory. Every vnode of an ext2 file points at a shared `struct e2ino`
 * (hard links share one), which holds the raw on-disk inode: fields we do
 * not interpret survive a write-back untouched. Directories are read into
 * the VFS tree when first looked at (refresh). Metadata and data go
 * through a write-back block cache; a kernel thread writes dirty blocks
 * every 5 seconds, and sync(2), poweroff and reboot write everything (the
 * superblock is marked clean only at shutdown, like Linux's unmount, so
 * e2fsck checks a volume Kestrel did not shut down cleanly). All entry
 * points take the volume's mutex: block I/O may sleep.
 *
 * Nothing on the disk is trusted: block numbers are range-checked before
 * any I/O and directory records are validated; a corrupt structure gives
 * EIO, never a kernel fault. */
#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/time.h>
#include <kernel/posix.h>
#include <kernel/cpu.h>
#include <kernel/ext2.h>

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_NDIR           12
#define EXT2_IND            12
#define EXT2_DIND           13
#define EXT2_TIND           14

#define INCOMPAT_FILETYPE   0x0002
#define INCOMPAT_RECOVER    0x0004
#define INCOMPAT_FLEX_BG    0x0200
#define INCOMPAT_SUPPORTED  (INCOMPAT_FILETYPE | INCOMPAT_FLEX_BG)
#define RO_SPARSE_SUPER     0x0001
#define RO_LARGE_FILE       0x0002
#define RO_BTREE_DIR        0x0004
#define RO_SUPPORTED        (RO_SPARSE_SUPER | RO_LARGE_FILE | RO_BTREE_DIR)
#define INDEX_FL            0x00001000

/* superblock offsets */
#define SB_INODES_COUNT     0
#define SB_BLOCKS_COUNT     4
#define SB_FREE_BLOCKS      12
#define SB_FREE_INODES      16
#define SB_FIRST_DATA_BLOCK 20
#define SB_LOG_BLOCK_SIZE   24
#define SB_BLOCKS_PER_GROUP 32
#define SB_INODES_PER_GROUP 40
#define SB_MTIME            44
#define SB_WTIME            48
#define SB_MNT_COUNT        52
#define SB_MAGIC            56
#define SB_STATE            58
#define SB_REV_LEVEL        76
#define SB_FIRST_INO        84
#define SB_INODE_SIZE       88
#define SB_FEATURE_COMPAT   92
#define SB_FEATURE_INCOMPAT 96
#define SB_FEATURE_RO       100
#define SB_VOLUME_NAME      120

/* inode offsets */
#define I_MODE      0
#define I_UID       2
#define I_SIZE      4
#define I_ATIME     8
#define I_CTIME     12
#define I_MTIME     16
#define I_DTIME     20
#define I_GID       24
#define I_LINKS     26
#define I_BLOCKS    28
#define I_FLAGS     32
#define I_BLOCK     40
#define I_FILE_ACL  104
#define I_SIZE_HIGH 108
#define I_UID_HIGH  120
#define I_GID_HIGH  122

#define FT_UNKNOWN 0
#define FT_REG     1
#define FT_DIR     2
#define FT_CHR     3
#define FT_BLK     4
#define FT_FIFO    5
#define FT_SOCK    6
#define FT_LNK     7

#define CACHE_BYTES   (4u << 20)            /* block cache size per volume */
#define FLUSH_MS      5000
#define RAW_INODE     256                   /* bytes of the on-disk inode kept */

static inline uint16_t g16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t g32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void p16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ---- the volume ------------------------------------------------------------------ */
struct buf {
    uint32_t blk;
    uint8_t *data;
    int      pins;
    bool     valid, dirty;
    uint64_t used;                          /* LRU stamp */
    struct buf *hnext;
};

struct e2ino {
    uint32_t ino;
    int      refs;                          /* vnodes */
    uint8_t  raw[RAW_INODE];
    struct e2ino *next;
};

struct e2fs {
    struct blkdev *dev;
    struct mount  *mnt;
    struct kmutex  lock;
    bool     rdonly;
    uint32_t bs, log_bs, sectors_per_block;
    uint32_t blocks, inodes, first_data_block;
    uint32_t bpg, ipg, ngroups, inode_size, first_ino;
    uint32_t incompat;
    uint8_t  sb[1024];                      /* the superblock as on disk */
    bool     sb_dirty;
    uint8_t *gdt;                           /* group descriptors, 32 bytes each */
    uint32_t gdt_blocks;
    bool     gdt_dirty;
    /* block cache */
    struct buf *bufs;
    uint32_t nbufs;
    struct buf *hash[256];
    uint64_t clock;
    /* inodes in use */
    struct e2ino *inodes_live;
};

static const struct vnode_ops e2_file_ops, e2_dir_ops, e2_link_ops, e2_special_ops;
static struct e2fs *volumes[8];

static uint8_t *gd(struct e2fs *fs, uint32_t g) { return fs->gdt + (size_t)g * 32; }
static uint32_t gd_block_bitmap(struct e2fs *fs, uint32_t g) { return g32(gd(fs, g)); }
static uint32_t gd_inode_bitmap(struct e2fs *fs, uint32_t g) { return g32(gd(fs, g) + 4); }
static uint32_t gd_inode_table(struct e2fs *fs, uint32_t g) { return g32(gd(fs, g) + 8); }

static bool blk_ok(struct e2fs *fs, uint32_t b) { return b >= fs->first_data_block && b < fs->blocks; }

/* ---- block cache ------------------------------------------------------------------ */
static int dev_io(struct e2fs *fs, uint32_t blk, uint32_t n, void *data, bool write)
{
    uint64_t lba = (uint64_t)blk * fs->sectors_per_block;
    uint32_t cnt = n * fs->sectors_per_block;
    return write ? blk_write(fs->dev, lba, cnt, data) : blk_read(fs->dev, lba, cnt, data);
}

static int write_back(struct e2fs *fs, struct buf *b)
{
    if (!b->dirty) return 0;
    int r = dev_io(fs, b->blk, 1, b->data, true);
    if (r == 0) b->dirty = false;
    else kprintf("ext2: %s: write of block %u failed (%d)\n", fs->dev->name, b->blk, r);
    return r;
}

/* The cached block `blk`, pinned (brelse() when done). With `read` false the
 * contents are not read from the disk: the caller overwrites all of it. */
static struct buf *bget(struct e2fs *fs, uint32_t blk, bool read)
{
    if (blk >= fs->blocks) return NULL;
    unsigned h = (blk * 2654435761u) >> 24;
    for (struct buf *b = fs->hash[h]; b; b = b->hnext)
        if (b->blk == blk && b->valid) {
            b->pins++;
            b->used = ++fs->clock;
            if (!read) memset(b->data, 0, fs->bs);      /* a cached copy from the block's last owner */
            return b;
        }
    /* a free or least recently used, unpinned buffer */
    struct buf *v = NULL;
    for (uint32_t i = 0; i < fs->nbufs; i++) {
        struct buf *b = &fs->bufs[i];
        if (b->pins) continue;
        if (!b->valid) { v = b; break; }
        if (!v || b->used < v->used) v = b;
    }
    if (!v) { kprintf("ext2: block cache exhausted\n"); return NULL; }
    if (v->valid) {
        if (v->dirty && write_back(fs, v) < 0) return NULL;
        unsigned oh = (v->blk * 2654435761u) >> 24;
        for (struct buf **pp = &fs->hash[oh]; *pp; pp = &(*pp)->hnext)
            if (*pp == v) { *pp = v->hnext; break; }
        v->valid = false;
    }
    if (read) {
        if (dev_io(fs, blk, 1, v->data, false) < 0) return NULL;
    } else {
        memset(v->data, 0, fs->bs);
    }
    v->blk = blk;
    v->valid = true;
    v->dirty = false;
    v->pins = 1;
    v->used = ++fs->clock;
    v->hnext = fs->hash[h];
    fs->hash[h] = v;
    return v;
}

static struct buf *bread(struct e2fs *fs, uint32_t blk) { return bget(fs, blk, true); }
static void brelse(struct buf *b) { if (b) b->pins--; }
static void bdirty(struct buf *b) { b->dirty = true; }

/* Write every dirty block, in block order, then the superblock and the
 * group descriptors. */
static int flush_all(struct e2fs *fs)
{
    int err = 0;
    for (;;) {                                          /* lowest dirty block first: fewer seeks */
        struct buf *best = NULL;
        for (uint32_t i = 0; i < fs->nbufs; i++) {
            struct buf *b = &fs->bufs[i];
            if (b->valid && b->dirty && (!best || b->blk < best->blk)) best = b;
        }
        if (!best) break;
        if (write_back(fs, best) < 0) { err = -EIO; best->dirty = false; }  /* do not loop on a bad block */
    }
    if (fs->gdt_dirty) {
        uint32_t first = fs->first_data_block + 1;
        if (dev_io(fs, first, fs->gdt_blocks, fs->gdt, true) < 0) err = -EIO;
        else fs->gdt_dirty = false;
    }
    if (fs->sb_dirty) {                                 /* bytes 1024..2047 of the volume */
        uint32_t dbs = fs->dev->block_size;
        uint8_t *tmp = kmalloc(2048 > dbs ? 2048 : dbs);
        if (tmp) {
            uint64_t first = 1024 / dbs, n = (2048 + dbs - 1) / dbs - first;
            if (blk_read(fs->dev, first, (uint32_t)n, tmp) == 0) {
                memcpy(tmp + (1024 - first * dbs), fs->sb, 1024);
                if (blk_write(fs->dev, first, (uint32_t)n, tmp) == 0) fs->sb_dirty = false;
                else err = -EIO;
            }
            kfree(tmp);
        }
    }
    blk_flush(fs->dev);
    return err;
}

/* ---- inodes ------------------------------------------------------------------------- */
static int inode_loc(struct e2fs *fs, uint32_t ino, uint32_t *blk, uint32_t *off)
{
    if (ino < 1 || ino > fs->inodes) return -EIO;
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
    uint64_t byte = (uint64_t)idx * fs->inode_size;
    *blk = gd_inode_table(fs, g) + (uint32_t)(byte / fs->bs);
    *off = (uint32_t)(byte % fs->bs);
    return blk_ok(fs, *blk) ? 0 : -EIO;
}

static int inode_write(struct e2fs *fs, struct e2ino *e)
{
    uint32_t blk, off;
    if (inode_loc(fs, e->ino, &blk, &off) < 0) return -EIO;
    struct buf *b = bread(fs, blk);
    if (!b) return -EIO;
    uint32_t n = fs->inode_size < RAW_INODE ? fs->inode_size : RAW_INODE;
    memcpy(b->data + off, e->raw, n);
    bdirty(b);
    brelse(b);
    return 0;
}

/* The in-memory inode `ino`, shared by every vnode of it. */
static struct e2ino *iget(struct e2fs *fs, uint32_t ino)
{
    for (struct e2ino *e = fs->inodes_live; e; e = e->next)
        if (e->ino == ino) { e->refs++; return e; }
    uint32_t blk, off;
    if (inode_loc(fs, ino, &blk, &off) < 0) return NULL;
    struct buf *b = bread(fs, blk);
    if (!b) return NULL;
    struct e2ino *e = kzalloc(sizeof *e);
    if (!e) { brelse(b); return NULL; }
    e->ino = ino;
    e->refs = 1;
    memcpy(e->raw, b->data + off, fs->inode_size < RAW_INODE ? fs->inode_size : RAW_INODE);
    brelse(b);
    e->next = fs->inodes_live;
    fs->inodes_live = e;
    return e;
}

static uint64_t i_size(struct e2ino *e)
{
    uint64_t s = g32(e->raw + I_SIZE);
    if ((g16(e->raw + I_MODE) & S_IFMT) == S_IFREG) s |= (uint64_t)g32(e->raw + I_SIZE_HIGH) << 32;
    return s;
}

static void set_size(struct e2fs *fs, struct e2ino *e, uint64_t s)
{
    p32(e->raw + I_SIZE, (uint32_t)s);
    if ((g16(e->raw + I_MODE) & S_IFMT) == S_IFREG) {
        p32(e->raw + I_SIZE_HIGH, (uint32_t)(s >> 32));
        if ((s >> 31) && !(g32(fs->sb + SB_FEATURE_RO) & RO_LARGE_FILE)) {
            p32(fs->sb + SB_FEATURE_RO, g32(fs->sb + SB_FEATURE_RO) | RO_LARGE_FILE);
            fs->sb_dirty = true;
        }
    }
}

static uint32_t now32(void) { return (uint32_t)time_realtime_sec(); }

/* ---- allocation --------------------------------------------------------------------- */
static void count_blocks(struct e2fs *fs, uint32_t g, int delta)
{
    p16(gd(fs, g) + 12, (uint16_t)(g16(gd(fs, g) + 12) + delta));
    p32(fs->sb + SB_FREE_BLOCKS, g32(fs->sb + SB_FREE_BLOCKS) + (uint32_t)delta);
    fs->gdt_dirty = fs->sb_dirty = true;
}

static void count_inodes(struct e2fs *fs, uint32_t g, int delta, bool dir)
{
    p16(gd(fs, g) + 14, (uint16_t)(g16(gd(fs, g) + 14) + delta));
    if (dir) p16(gd(fs, g) + 16, (uint16_t)(g16(gd(fs, g) + 16) - delta));
    p32(fs->sb + SB_FREE_INODES, g32(fs->sb + SB_FREE_INODES) + (uint32_t)delta);
    fs->gdt_dirty = fs->sb_dirty = true;
}

/* A free block, near `goal` if possible; zeroed in the cache. 0 = full. */
static uint32_t alloc_block(struct e2fs *fs, uint32_t goal)
{
    if (!g32(fs->sb + SB_FREE_BLOCKS)) return 0;
    uint32_t g0 = blk_ok(fs, goal) ? (goal - fs->first_data_block) / fs->bpg : 0;
    for (uint32_t k = 0; k < fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        if (!g16(gd(fs, g) + 12)) continue;
        struct buf *bm = bread(fs, gd_block_bitmap(fs, g));
        if (!bm) continue;
        uint32_t base = fs->first_data_block + g * fs->bpg;
        uint32_t n = fs->bpg;
        if (base + n > fs->blocks) n = fs->blocks - base;
        uint32_t start = (k == 0 && goal >= base && goal < base + n) ? goal - base : 0;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t i = (start + j) % n;
            if (bm->data[i / 8] & (1u << (i % 8))) continue;
            bm->data[i / 8] |= (uint8_t)(1u << (i % 8));
            bdirty(bm);
            brelse(bm);
            count_blocks(fs, g, -1);
            uint32_t blk = base + i;
            struct buf *nb = bget(fs, blk, false);      /* zeroed: no stale data reaches a file */
            if (nb) { bdirty(nb); brelse(nb); }
            return blk;
        }
        brelse(bm);
    }
    return 0;
}

static void free_block(struct e2fs *fs, uint32_t blk)
{
    if (!blk_ok(fs, blk)) return;
    uint32_t g = (blk - fs->first_data_block) / fs->bpg, i = (blk - fs->first_data_block) % fs->bpg;
    struct buf *bm = bread(fs, gd_block_bitmap(fs, g));
    if (!bm) return;
    if (bm->data[i / 8] & (1u << (i % 8))) {
        bm->data[i / 8] &= (uint8_t)~(1u << (i % 8));
        bdirty(bm);
        count_blocks(fs, g, 1);
    }
    brelse(bm);
    /* drop a cached copy: it must not be written over a later owner */
    unsigned h = (blk * 2654435761u) >> 24;
    for (struct buf *b = fs->hash[h]; b; b = b->hnext)
        if (b->blk == blk && b->valid) b->dirty = false;
}

/* A free inode: directories go to the group with the most free inodes,
 * files near their directory. 0 = full. */
static uint32_t alloc_inode(struct e2fs *fs, uint32_t dir_ino, bool dir)
{
    if (!g32(fs->sb + SB_FREE_INODES)) return 0;
    uint32_t g0 = (dir_ino - 1) / fs->ipg;
    if (dir) {
        uint32_t best = g0, most = 0;
        for (uint32_t g = 0; g < fs->ngroups; g++)
            if (g16(gd(fs, g) + 14) > most) { most = g16(gd(fs, g) + 14); best = g; }
        g0 = best;
    }
    for (uint32_t k = 0; k < fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        if (!g16(gd(fs, g) + 14)) continue;
        struct buf *bm = bread(fs, gd_inode_bitmap(fs, g));
        if (!bm) continue;
        for (uint32_t i = 0; i < fs->ipg; i++) {
            uint32_t ino = g * fs->ipg + i + 1;
            if (ino < fs->first_ino) continue;          /* reserved inodes */
            if (bm->data[i / 8] & (1u << (i % 8))) continue;
            bm->data[i / 8] |= (uint8_t)(1u << (i % 8));
            bdirty(bm);
            brelse(bm);
            count_inodes(fs, g, -1, dir);
            return ino;
        }
        brelse(bm);
    }
    return 0;
}

static void free_inode(struct e2fs *fs, uint32_t ino, bool dir)
{
    uint32_t g = (ino - 1) / fs->ipg, i = (ino - 1) % fs->ipg;
    if (g >= fs->ngroups) return;
    struct buf *bm = bread(fs, gd_inode_bitmap(fs, g));
    if (!bm) return;
    if (bm->data[i / 8] & (1u << (i % 8))) {
        bm->data[i / 8] &= (uint8_t)~(1u << (i % 8));
        bdirty(bm);
        count_inodes(fs, g, 1, dir);
    }
    brelse(bm);
}

/* ---- block map ---------------------------------------------------------------------- */
static void add_blocks(struct e2fs *fs, struct e2ino *e, int n)
{
    p32(e->raw + I_BLOCKS, g32(e->raw + I_BLOCKS) + (uint32_t)(n * (int)(fs->bs / 512)));
}

/* Slot `idx` of indirect block `blk`, allocating the next level if asked. */
static int ind_slot(struct e2fs *fs, struct e2ino *e, uint32_t blk, uint32_t idx, bool create, uint32_t goal,
                    uint32_t *out)
{
    if (!blk_ok(fs, blk)) return -EIO;
    struct buf *b = bread(fs, blk);
    if (!b) return -EIO;
    uint32_t v = g32(b->data + idx * 4);
    if (!v && create) {
        if (!(v = alloc_block(fs, goal))) { brelse(b); return -ENOSPC; }
        add_blocks(fs, e, 1);
        p32(b->data + idx * 4, v);
        bdirty(b);
    }
    brelse(b);
    if (v && !blk_ok(fs, v)) return -EIO;
    *out = v;
    return 0;
}

/* The disk block of file block `lb` (0: a hole), allocating it and any
 * indirect blocks on the way when `create`. */
static int bmap(struct e2fs *fs, struct e2ino *e, uint64_t lb, bool create, uint32_t *out)
{
    uint32_t per = fs->bs / 4;
    uint8_t *ib = e->raw + I_BLOCK;
    uint32_t goal = g32(ib) ? g32(ib) + (uint32_t)lb : gd_inode_table(fs, (e->ino - 1) / fs->ipg);
    int slot, depth;
    uint32_t idx[3];
    if (lb < EXT2_NDIR) { slot = (int)lb; depth = 0; }
    else if ((lb -= EXT2_NDIR) < per) { slot = EXT2_IND; depth = 1; idx[0] = (uint32_t)lb; }
    else if ((lb -= per) < (uint64_t)per * per) {
        slot = EXT2_DIND; depth = 2; idx[0] = (uint32_t)(lb / per); idx[1] = (uint32_t)(lb % per);
    } else if ((lb -= (uint64_t)per * per) < (uint64_t)per * per * per) {
        slot = EXT2_TIND; depth = 3;
        idx[0] = (uint32_t)(lb / ((uint64_t)per * per)); idx[1] = (uint32_t)(lb / per % per); idx[2] = (uint32_t)(lb % per);
    } else {
        return -EFBIG;
    }
    uint32_t v = g32(ib + slot * 4);
    if (!v && create) {
        if (!(v = alloc_block(fs, goal))) return -ENOSPC;
        add_blocks(fs, e, 1);
        p32(ib + slot * 4, v);
    }
    if (v && !blk_ok(fs, v)) return -EIO;
    for (int d = 0; d < depth && v; d++) {
        int r = ind_slot(fs, e, v, idx[d], create, goal, &v);
        if (r < 0) return r;
    }
    *out = v;
    return 0;
}

/* Free the blocks of the subtree at `blk` (an indirect block of `level`,
 * 0 = a data block) from file-block offset `from` on, relative to its
 * start. Returns true if the whole subtree went (the pointer can be cleared). */
static bool free_tree(struct e2fs *fs, struct e2ino *e, uint32_t blk, int level, uint64_t from)
{
    if (!blk_ok(fs, blk)) return true;                  /* corrupt pointer: drop it */
    if (level == 0) {
        if (from > 0) return false;
        free_block(fs, blk);
        add_blocks(fs, e, -1);
        return true;
    }
    uint32_t per = fs->bs / 4;
    uint64_t span = 1;
    for (int i = 1; i < level; i++) span *= per;
    struct buf *b = bread(fs, blk);
    if (!b) return false;
    bool empty = true;
    for (uint32_t i = 0; i < per; i++) {
        uint32_t c = g32(b->data + i * 4);
        uint64_t start = (uint64_t)i * span;
        if (!c) continue;
        if (start + span <= from) { empty = false; continue; }
        uint64_t sub = from > start ? from - start : 0;
        if (free_tree(fs, e, c, level - 1, sub)) { p32(b->data + i * 4, 0); bdirty(b); }
        else empty = false;
    }
    brelse(b);
    if (empty && from == 0) {
        free_block(fs, blk);
        add_blocks(fs, e, -1);
        return true;
    }
    return false;
}

/* Free every block past `size`; zero the tail of the last block. */
static void truncate_blocks(struct e2fs *fs, struct e2ino *e, uint64_t size)
{
    uint16_t mode = g16(e->raw + I_MODE);
    if ((mode & S_IFMT) == S_IFLNK && i_size(e) < 60 && g32(e->raw + I_BLOCKS) == 0) return;  /* fast link */
    uint64_t keep = (size + fs->bs - 1) / fs->bs;       /* file blocks that stay */
    uint32_t per = fs->bs / 4;
    uint8_t *ib = e->raw + I_BLOCK;
    for (uint32_t i = 0; i < EXT2_NDIR; i++)
        if (i >= keep && g32(ib + i * 4)) { free_tree(fs, e, g32(ib + i * 4), 0, 0); p32(ib + i * 4, 0); }
    uint64_t base = EXT2_NDIR, span = per;
    for (int lv = 1; lv <= 3; lv++) {
        uint32_t v = g32(ib + (EXT2_IND + lv - 1) * 4);
        if (v) {
            uint64_t from = keep > base ? keep - base : 0;
            if (from < span && free_tree(fs, e, v, lv, from)) p32(ib + (EXT2_IND + lv - 1) * 4, 0);
        }
        base += span;
        span *= per;
    }
    if (size % fs->bs) {                                /* the bytes past the end read as zeros later */
        uint32_t pb;
        if (bmap(fs, e, size / fs->bs, false, &pb) == 0 && pb) {
            struct buf *b = bread(fs, pb);
            if (b) { memset(b->data + size % fs->bs, 0, fs->bs - size % fs->bs); bdirty(b); brelse(b); }
        }
    }
}

/* ---- file data ---------------------------------------------------------------------- */
static struct e2fs *fs_of(struct vnode *vn) { return vn->fs->priv; }
static struct e2ino *ino_of(struct vnode *vn) { return vn->data; }

static ssize_t e2_read_locked(struct e2fs *fs, struct e2ino *e, void *buf, size_t len, uint64_t off)
{
    uint64_t size = i_size(e);
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t lb = (off + done) / fs->bs;
        uint32_t bo = (uint32_t)((off + done) % fs->bs);
        size_t n = fs->bs - bo < len - done ? fs->bs - bo : len - done;
        uint32_t pb;
        int r = bmap(fs, e, lb, false, &pb);
        if (r < 0) return done ? (ssize_t)done : r;
        if (!pb) {
            memset((uint8_t *)buf + done, 0, n);        /* a hole */
        } else {
            struct buf *b = bread(fs, pb);
            if (!b) return done ? (ssize_t)done : -EIO;
            memcpy((uint8_t *)buf + done, b->data + bo, n);
            brelse(b);
        }
        done += n;
    }
    return (ssize_t)done;
}

static ssize_t e2_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    struct e2fs *fs = fs_of(vn);
    kmutex_lock(&fs->lock);
    ssize_t r = e2_read_locked(fs, ino_of(vn), buf, len, off);
    kmutex_unlock(&fs->lock);
    return r;
}

static void touch(struct vnode *vn, struct e2ino *e, bool content)
{
    uint32_t t = now32();
    if (content) p32(e->raw + I_MTIME, t);
    p32(e->raw + I_CTIME, t);
    vn->mtime = g32(e->raw + I_MTIME);
    vn->ctime = t;
}

static ssize_t e2_write_locked(struct e2fs *fs, struct e2ino *e, const void *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    int err = 0;
    while (done < len) {
        uint64_t lb = (off + done) / fs->bs;
        uint32_t bo = (uint32_t)((off + done) % fs->bs);
        size_t n = fs->bs - bo < len - done ? fs->bs - bo : len - done;
        uint32_t pb;
        if ((err = bmap(fs, e, lb, true, &pb)) < 0) break;
        struct buf *b = bread(fs, pb);                  /* a new block comes zeroed from the cache */
        if (!b) { err = -EIO; break; }
        memcpy(b->data + bo, (const uint8_t *)buf + done, n);
        bdirty(b);
        brelse(b);
        done += n;
    }
    if (done && off + done > i_size(e)) set_size(fs, e, off + done);
    if (done || err) inode_write(fs, e);                /* block pointers may have changed */
    return done ? (ssize_t)done : err;
}

static ssize_t e2_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    struct e2fs *fs = fs_of(vn);
    if (fs->rdonly) return -EROFS;
    kmutex_lock(&fs->lock);
    struct e2ino *e = ino_of(vn);
    ssize_t r = e2_write_locked(fs, e, buf, len, off);
    if (r > 0) { touch(vn, e, true); inode_write(fs, e); vn->blocks = g32(e->raw + I_BLOCKS); }
    kmutex_unlock(&fs->lock);
    return r;
}

static uint64_t e2_size(struct vnode *vn) { return i_size(ino_of(vn)); }

static int e2_truncate(struct vnode *vn, uint64_t size)
{
    struct e2fs *fs = fs_of(vn);
    if (fs->rdonly) return -EROFS;
    kmutex_lock(&fs->lock);
    struct e2ino *e = ino_of(vn);
    if (size < i_size(e)) truncate_blocks(fs, e, size);
    set_size(fs, e, size);
    touch(vn, e, true);
    int r = inode_write(fs, e);
    vn->blocks = g32(e->raw + I_BLOCKS);
    kmutex_unlock(&fs->lock);
    return r;
}

/* ---- vnodes ------------------------------------------------------------------------- */
static void fill_vnode(struct vnode *vn, struct e2ino *e)
{
    uint16_t mode = g16(e->raw + I_MODE);
    vn->mode = mode;
    vn->uid = g16(e->raw + I_UID) | ((uint32_t)g16(e->raw + I_UID_HIGH) << 16);
    vn->gid = g16(e->raw + I_GID) | ((uint32_t)g16(e->raw + I_GID_HIGH) << 16);
    vn->size = i_size(e);
    vn->mtime = g32(e->raw + I_MTIME);
    vn->atime = g32(e->raw + I_ATIME);
    vn->ctime = g32(e->raw + I_CTIME);
    vn->nlink = g16(e->raw + I_LINKS);
    vn->blocks = g32(e->raw + I_BLOCKS);
    vn->ino = e->ino;
}

static struct vnode *make_vnode(struct e2fs *fs, struct e2ino *e, const char *name)
{
    uint16_t mode = g16(e->raw + I_MODE);
    enum vtype t;
    const struct vnode_ops *ops;
    switch (mode & S_IFMT) {
    case S_IFDIR: t = VDIR; ops = &e2_dir_ops; break;
    case S_IFREG: t = VREG; ops = &e2_file_ops; break;
    case S_IFLNK: t = VLNK; ops = &e2_link_ops; break;
    case S_IFCHR: t = VCHR; ops = &e2_special_ops; break;      /* not openable: /dev is devfs */
    default:      t = VNON; ops = &e2_special_ops; break;
    }
    struct vnode *vn = vfs_node_new(fs->mnt, name, t, 0, ops);
    if (!vn) return NULL;
    vn->data = e;
    fill_vnode(vn, e);
    if (t == VCHR) vn->rdev = g32(e->raw + I_BLOCK);
    return vn;
}

static void e2_getattr(struct vnode *vn) { fill_vnode(vn, ino_of(vn)); }

static void release_inode(struct e2fs *fs, struct e2ino *e)
{
    if (--e->refs > 0) return;
    if (g16(e->raw + I_LINKS) == 0 && !fs->rdonly) {    /* unlinked and now unused: free it */
        bool dir = (g16(e->raw + I_MODE) & S_IFMT) == S_IFDIR;
        truncate_blocks(fs, e, 0);
        set_size(fs, e, 0);
        p32(e->raw + I_DTIME, now32());
        inode_write(fs, e);
        free_inode(fs, e->ino, dir);
    }
    for (struct e2ino **pp = &fs->inodes_live; *pp; pp = &(*pp)->next)
        if (*pp == e) { *pp = e->next; break; }
    kfree(e);
}

static void e2_evict(struct vnode *vn)
{
    struct e2fs *fs = fs_of(vn);
    kmutex_lock(&fs->lock);
    release_inode(fs, ino_of(vn));
    vn->data = NULL;
    kmutex_unlock(&fs->lock);
}

static int e2_setattr(struct vnode *vn)
{
    struct e2fs *fs = fs_of(vn);
    if (fs->rdonly) return -EROFS;
    kmutex_lock(&fs->lock);
    struct e2ino *e = ino_of(vn);
    p16(e->raw + I_MODE, (uint16_t)((g16(e->raw + I_MODE) & S_IFMT) | (vn->mode & 07777)));
    p16(e->raw + I_UID, (uint16_t)vn->uid);
    p16(e->raw + I_UID_HIGH, (uint16_t)(vn->uid >> 16));
    p16(e->raw + I_GID, (uint16_t)vn->gid);
    p16(e->raw + I_GID_HIGH, (uint16_t)(vn->gid >> 16));
    p32(e->raw + I_MTIME, (uint32_t)vn->mtime);
    p32(e->raw + I_ATIME, (uint32_t)vn->mtime);
    p32(e->raw + I_CTIME, now32());
    int r = inode_write(fs, e);
    kmutex_unlock(&fs->lock);
    return r;
}

static int e2_readlink(struct vnode *vn, char *buf, size_t size)
{
    struct e2fs *fs = fs_of(vn);
    kmutex_lock(&fs->lock);
    struct e2ino *e = ino_of(vn);
    uint64_t len = i_size(e);
    uint32_t acl_blocks = g32(e->raw + I_FILE_ACL) ? fs->bs / 512 : 0;
    int r;
    if (len >= fs->bs || len >= VFS_PATH_MAX) r = -EIO;
    else if (g32(e->raw + I_BLOCKS) == acl_blocks && len < 60) {   /* fast: in the block pointers */
        r = (int)(len < size ? len : size);
        memcpy(buf, e->raw + I_BLOCK, (size_t)r);
    } else {
        char *tmp = kmalloc(fs->bs);
        r = tmp ? (int)e2_read_locked(fs, e, tmp, (size_t)len, 0) : -ENOMEM;
        if (r > 0) { if ((size_t)r > size) r = (int)size; memcpy(buf, tmp, (size_t)r); }
        kfree(tmp);
    }
    kmutex_unlock(&fs->lock);
    return r;
}

/* ---- directories --------------------------------------------------------------------- */
/* Call fn for every valid entry; stops at a corrupt record. fn returns
 * nonzero to stop (its value is returned). */
typedef int (*dirent_fn)(struct e2fs *fs, struct buf *b, uint32_t off, uint32_t prev, void *ctx);

static int dir_iterate(struct e2fs *fs, struct e2ino *dir, dirent_fn fn, void *ctx)
{
    uint64_t nblocks = i_size(dir) / fs->bs;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb;
        if (bmap(fs, dir, lb, false, &pb) < 0 || !pb) continue;
        struct buf *b = bread(fs, pb);
        if (!b) return -EIO;
        uint32_t off = 0, prev = UINT32_MAX;
        while (off + 8 <= fs->bs) {
            uint16_t rec = g16(b->data + off + 4);
            uint8_t nl = b->data[off + 6];
            if (rec < 8 || (rec & 3) || off + rec > fs->bs || (uint32_t)nl + 8 > rec) {
                kprintf("ext2: %s: corrupt directory %u (block %u)\n", fs->dev->name, dir->ino, pb);
                brelse(b);
                return -EIO;
            }
            int r = fn(fs, b, off, prev, ctx);
            if (r) { brelse(b); return r; }
            prev = off;
            off += rec;
        }
        brelse(b);
    }
    return 0;
}

struct load_ctx { struct vnode *dir; };

static int load_one(struct e2fs *fs, struct buf *b, uint32_t off, uint32_t prev, void *vctx)
{
    (void)prev;
    struct load_ctx *c = vctx;
    uint32_t ino = g32(b->data + off);
    uint8_t nl = b->data[off + 6];
    if (!ino || !nl) return 0;
    char name[VFS_NAME_MAX];
    memcpy(name, b->data + off + 8, nl);
    name[nl] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (ino > fs->inodes) return 0;
    struct e2ino *e = iget(fs, ino);
    if (!e) return 0;
    struct vnode *vn = make_vnode(fs, e, name);
    if (!vn) { release_inode(fs, e); return -ENOMEM; }
    vfs_node_add(c->dir, vn);
    return 0;
}

static void e2_refresh(struct vnode *dir)
{
    if (dir->loaded) return;
    struct e2fs *fs = fs_of(dir);
    kmutex_lock(&fs->lock);
    if (!dir->loaded) {
        dir->loaded = true;                             /* before: iget may recurse into nothing else */
        struct load_ctx c = { dir };
        dir_iterate(fs, ino_of(dir), load_one, &c);
    }
    kmutex_unlock(&fs->lock);
}

static uint8_t ftype_of(enum vtype t)
{
    return t == VDIR ? FT_DIR : t == VREG ? FT_REG : t == VLNK ? FT_LNK : t == VCHR ? FT_CHR : FT_UNKNOWN;
}

static void unindex(struct e2fs *fs, struct e2ino *dir)
{
    uint32_t fl = g32(dir->raw + I_FLAGS);
    if (fl & INDEX_FL) { p32(dir->raw + I_FLAGS, fl & ~INDEX_FL); inode_write(fs, dir); }
}

struct add_ctx { const char *name; uint32_t nl, ino; uint8_t ft; bool done; };

static int add_one(struct e2fs *fs, struct buf *b, uint32_t off, uint32_t prev, void *vctx)
{
    (void)prev;
    struct add_ctx *c = vctx;
    uint8_t *d = b->data + off;
    uint16_t rec = g16(d + 4);
    uint32_t need = (8 + c->nl + 3) & ~3u;
    uint32_t used = g32(d) ? (8 + d[6] + 3) & ~3u : 0;
    if (rec - used < need) return 0;
    uint8_t *n = d;
    if (used) {                                         /* split the record */
        p16(d + 4, (uint16_t)used);
        n = d + used;
        p16(n + 4, (uint16_t)(rec - used));
    }
    p32(n, c->ino);
    n[6] = (uint8_t)c->nl;
    n[7] = (fs->incompat & INCOMPAT_FILETYPE) ? c->ft : 0;
    memcpy(n + 8, c->name, c->nl);
    bdirty(b);
    c->done = true;
    return 1;
}

static int dir_add(struct e2fs *fs, struct e2ino *dir, const char *name, uint32_t ino, uint8_t ft)
{
    struct add_ctx c = { name, (uint32_t)strlen(name), ino, ft, false };
    if (c.nl > 255) return -ENAMETOOLONG;
    int r = dir_iterate(fs, dir, add_one, &c);
    if (r < 0) return r;
    unindex(fs, dir);
    if (c.done) return 0;
    uint64_t size = i_size(dir);                        /* no room: a new block */
    uint32_t pb;
    if ((r = bmap(fs, dir, size / fs->bs, true, &pb)) < 0) return r;
    struct buf *b = bread(fs, pb);
    if (!b) return -EIO;
    memset(b->data, 0, fs->bs);
    p32(b->data, ino);
    p16(b->data + 4, (uint16_t)fs->bs);
    b->data[6] = (uint8_t)c.nl;
    b->data[7] = (fs->incompat & INCOMPAT_FILETYPE) ? ft : 0;
    memcpy(b->data + 8, name, c.nl);
    bdirty(b);
    brelse(b);
    set_size(fs, dir, size + fs->bs);
    return inode_write(fs, dir);
}

struct del_ctx { const char *name; uint32_t nl; uint32_t new_ino; uint8_t new_ft; bool done; };

/* Remove the entry (merge it into the one before), or point it at another
 * inode when new_ino is set (rename over an existing name). */
static int del_one(struct e2fs *fs, struct buf *b, uint32_t off, uint32_t prev, void *vctx)
{
    struct del_ctx *c = vctx;
    uint8_t *d = b->data + off;
    if (!g32(d) || d[6] != c->nl || memcmp(d + 8, c->name, c->nl)) return 0;
    if (c->new_ino) {
        p32(d, c->new_ino);
        if (fs->incompat & INCOMPAT_FILETYPE) d[7] = c->new_ft;
    } else if (prev != UINT32_MAX) {
        uint8_t *p = b->data + prev;
        p16(p + 4, (uint16_t)(g16(p + 4) + g16(d + 4)));
    } else {
        p32(d, 0);                                      /* first in its block: just empty it */
    }
    bdirty(b);
    c->done = true;
    return 1;
}

static int dir_del(struct e2fs *fs, struct e2ino *dir, const char *name, uint32_t new_ino, uint8_t new_ft)
{
    struct del_ctx c = { name, (uint32_t)strlen(name), new_ino, new_ft, false };
    int r = dir_iterate(fs, dir, del_one, &c);
    if (r < 0) return r;
    unindex(fs, dir);
    return c.done ? 0 : -ENOENT;
}

/* Point the ".." of directory `dir` at `parent`. */
static int set_dotdot_one(struct e2fs *fs, struct buf *b, uint32_t off, uint32_t prev, void *vctx)
{
    (void)fs; (void)prev;
    uint8_t *d = b->data + off;
    if (d[6] == 2 && d[8] == '.' && d[9] == '.') { p32(d, *(uint32_t *)vctx); bdirty(b); return 1; }
    return 0;
}

static void links_add(struct e2fs *fs, struct e2ino *e, int delta)
{
    p16(e->raw + I_LINKS, (uint16_t)(g16(e->raw + I_LINKS) + delta));
    p32(e->raw + I_CTIME, now32());
    inode_write(fs, e);
}

static int e2_create(struct vnode *dir, const char *name, enum vtype t, uint32_t mode, const char *target,
                     struct vnode **out)
{
    struct e2fs *fs = fs_of(dir);
    if (fs->rdonly) return -EROFS;
    if (t != VREG && t != VDIR && t != VLNK) return -EPERM;
    if (strlen(name) > 255) return -ENAMETOOLONG;
    kmutex_lock(&fs->lock);
    struct e2ino *pd = ino_of(dir);
    int r = -ENOSPC;
    uint32_t ino = alloc_inode(fs, pd->ino, t == VDIR);
    if (!ino) goto out;
    struct e2ino *e = kzalloc(sizeof *e);
    if (!e) { free_inode(fs, ino, t == VDIR); r = -ENOMEM; goto out; }
    e->ino = ino;
    e->refs = 1;
    struct tcb *self = current_task();
    uint32_t uid = self ? self->uid : 0, gid = self ? self->gid : 0, tnow = now32();
    uint16_t fmt = t == VDIR ? S_IFDIR : t == VLNK ? S_IFLNK : S_IFREG;
    p16(e->raw + I_MODE, (uint16_t)(fmt | (t == VLNK ? 0777 : (mode & 07777))));
    p16(e->raw + I_UID, (uint16_t)uid); p16(e->raw + I_UID_HIGH, (uint16_t)(uid >> 16));
    p16(e->raw + I_GID, (uint16_t)gid); p16(e->raw + I_GID_HIGH, (uint16_t)(gid >> 16));
    p32(e->raw + I_ATIME, tnow); p32(e->raw + I_CTIME, tnow); p32(e->raw + I_MTIME, tnow);
    p16(e->raw + I_LINKS, t == VDIR ? 2 : 1);
    if (fs->inode_size > 128 && fs->inode_size <= RAW_INODE) p16(e->raw + 128, 32);   /* i_extra_isize */
    e->next = fs->inodes_live;
    fs->inodes_live = e;
    if (t == VDIR) {                                    /* "." and ".." */
        uint32_t pb;
        if ((r = bmap(fs, e, 0, true, &pb)) < 0) goto undo;
        struct buf *b = bread(fs, pb);
        if (!b) { r = -EIO; goto undo; }
        memset(b->data, 0, fs->bs);
        bool ft = fs->incompat & INCOMPAT_FILETYPE;
        p32(b->data, ino); p16(b->data + 4, 12); b->data[6] = 1; b->data[7] = ft ? FT_DIR : 0; b->data[8] = '.';
        p32(b->data + 12, pd->ino); p16(b->data + 16, (uint16_t)(fs->bs - 12)); b->data[18] = 2;
        b->data[19] = ft ? FT_DIR : 0; b->data[20] = '.'; b->data[21] = '.';
        bdirty(b);
        brelse(b);
        set_size(fs, e, fs->bs);
    } else if (t == VLNK) {
        size_t tl = strlen(target);
        if (tl >= fs->bs) { r = -ENAMETOOLONG; goto undo; }
        if (tl < 60) memcpy(e->raw + I_BLOCK, target, tl);
        else if ((r = (int)e2_write_locked(fs, e, target, tl, 0)) < 0) goto undo;
        set_size(fs, e, tl);
    }
    inode_write(fs, e);
    if ((r = dir_add(fs, pd, name, ino, ftype_of(t))) < 0) goto undo;
    if (t == VDIR) links_add(fs, pd, 1);
    touch(dir, pd, true);
    inode_write(fs, pd);
    struct vnode *vn = make_vnode(fs, e, name);
    if (!vn) { r = -ENOMEM; goto out; }                 /* on disk now; seen at the next mount */
    if (t == VDIR) vn->loaded = true;                   /* empty: nothing to read */
    *out = vn;
    r = 0;
    goto out;
undo:
    p16(e->raw + I_LINKS, 0);
    release_inode(fs, e);
out:
    kmutex_unlock(&fs->lock);
    return r;
}

static int e2_link(struct vnode *dir, const char *name, struct vnode *target, struct vnode **out)
{
    struct e2fs *fs = fs_of(dir);
    if (fs->rdonly) return -EROFS;
    kmutex_lock(&fs->lock);
    struct e2ino *e = ino_of(target);
    int r = -EMLINK;
    if (g16(e->raw + I_LINKS) >= 32000) goto out;
    if ((r = dir_add(fs, ino_of(dir), name, e->ino, ftype_of(target->type))) < 0) goto out;
    links_add(fs, e, 1);
    e->refs++;
    struct vnode *vn = make_vnode(fs, e, name);
    if (!vn) { r = -ENOMEM; e->refs--; goto out; }
    target->nlink = vn->nlink;
    *out = vn;
    r = 0;
out:
    kmutex_unlock(&fs->lock);
    return r;
}

static int e2_remove(struct vnode *dir, struct vnode *vn)
{
    struct e2fs *fs = fs_of(dir);
    if (fs->rdonly) return -EROFS;
    kmutex_lock(&fs->lock);
    struct e2ino *pd = ino_of(dir), *e = ino_of(vn);
    int r = dir_del(fs, pd, vn->name, 0, 0);
    if (r == 0) {
        if (vn->type == VDIR) {                         /* its "." and the parent's link from ".." */
            p16(e->raw + I_LINKS, 0);
            inode_write(fs, e);
            links_add(fs, pd, -1);
        } else {
            links_add(fs, e, -1);
        }
        touch(dir, pd, true);
        inode_write(fs, pd);
    }
    kmutex_unlock(&fs->lock);
    return r;   /* the inode is freed when its last vnode goes (e2_evict) */
}

static int e2_rename(struct vnode *odir, struct vnode *vn, struct vnode *ndir, const char *nname,
                     struct vnode *replaced)
{
    struct e2fs *fs = fs_of(odir);
    if (fs->rdonly) return -EROFS;
    if (strlen(nname) > 255) return -ENAMETOOLONG;
    kmutex_lock(&fs->lock);
    struct e2ino *op = ino_of(odir), *np = ino_of(ndir), *e = ino_of(vn);
    int r;
    if (replaced) {                                     /* the existing name now means vn */
        struct e2ino *re = ino_of(replaced);
        if ((r = dir_del(fs, np, nname, e->ino, ftype_of(vn->type))) < 0) goto out;
        if (replaced->type == VDIR) { p16(re->raw + I_LINKS, 0); inode_write(fs, re); links_add(fs, np, -1); }
        else links_add(fs, re, -1);
    } else if ((r = dir_add(fs, np, nname, e->ino, ftype_of(vn->type))) < 0) {
        goto out;
    }
    if ((r = dir_del(fs, op, vn->name, 0, 0)) < 0) goto out;
    if (vn->type == VDIR && op != np) {                 /* ".." moves with it */
        uint32_t pino = np->ino;
        dir_iterate(fs, e, set_dotdot_one, &pino);
        unindex(fs, e);
        links_add(fs, op, -1);
        links_add(fs, np, 1);
    }
    touch(odir, op, true);
    inode_write(fs, op);
    if (np != op) { touch(ndir, np, true); inode_write(fs, np); }
    p32(e->raw + I_CTIME, now32());
    inode_write(fs, e);
    r = 0;
out:
    kmutex_unlock(&fs->lock);
    return r;
}

static const struct vnode_ops e2_file_ops = {
    .read = e2_read, .write = e2_write, .size = e2_size, .truncate = e2_truncate,
    .setattr = e2_setattr, .evict = e2_evict, .getattr = e2_getattr,
};
static const struct vnode_ops e2_dir_ops = {
    .refresh = e2_refresh, .create = e2_create, .link = e2_link, .remove = e2_remove, .rename = e2_rename,
    .setattr = e2_setattr, .evict = e2_evict, .getattr = e2_getattr,
};
static const struct vnode_ops e2_link_ops = {
    .readlink = e2_readlink, .size = e2_size, .setattr = e2_setattr, .evict = e2_evict, .getattr = e2_getattr,
};
static const struct vnode_ops e2_special_ops = { .setattr = e2_setattr, .evict = e2_evict, .getattr = e2_getattr };

/* ---- mount, sync, statfs ------------------------------------------------------------ */
static int e2_sync(struct mount *m, bool final)
{
    struct e2fs *fs = m->priv;
    if (fs->rdonly) return 0;
    kmutex_lock(&fs->lock);
    if (final) {                                        /* like an unmount: clean */
        p16(fs->sb + SB_STATE, 1);
        p32(fs->sb + SB_WTIME, now32());
        fs->sb_dirty = true;
    }
    int r = flush_all(fs);
    kmutex_unlock(&fs->lock);
    return r;
}

static int e2_statfs(struct mount *m, struct kstatfs *st)
{
    struct e2fs *fs = m->priv;
    st->f_type = EXT2_MAGIC;
    st->f_bsize = st->f_frsize = fs->bs;
    st->f_blocks = fs->blocks - fs->first_data_block;
    st->f_bfree = g32(fs->sb + SB_FREE_BLOCKS);
    uint32_t reserved = g32(fs->sb + 8);
    st->f_bavail = st->f_bfree > reserved ? st->f_bfree - reserved : 0;
    st->f_files = fs->inodes;
    st->f_ffree = g32(fs->sb + SB_FREE_INODES);
    st->f_namelen = 255;
    return 0;
}

static int flusher(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep_ms(FLUSH_MS);
        for (int i = 0; i < 8; i++)
            if (volumes[i]) e2_sync(volumes[i]->mnt, false);
    }
    return 0;
}

bool ext2_probe(struct blkdev *dev, char label[17])
{
    uint8_t sb[1024];
    if (blk_read_bytes(dev, 1024, sb, sizeof sb) < 0) return false;
    if (g16(sb + SB_MAGIC) != EXT2_MAGIC) return false;
    if (label) { memcpy(label, sb + SB_VOLUME_NAME, 16); label[16] = 0; }
    return true;
}

int ext2_mount(struct blkdev *dev, bool rdonly, struct mount **out)
{
    struct e2fs *fs = kzalloc(sizeof *fs);
    if (!fs) return -ENOMEM;
    fs->dev = dev;
    int r = -EINVAL;
    if (blk_read_bytes(dev, 1024, fs->sb, sizeof fs->sb) < 0) { r = -EIO; goto fail; }
    uint8_t *sb = fs->sb;
    if (g16(sb + SB_MAGIC) != EXT2_MAGIC) goto fail;
    uint32_t log = g32(sb + SB_LOG_BLOCK_SIZE);
    if (log > 2) { kprintf("ext2: %s: block size %u not supported\n", dev->name, 1024u << log); goto fail; }
    fs->bs = 1024u << log;
    fs->log_bs = 10 + log;
    if (fs->bs % dev->block_size) goto fail;
    fs->sectors_per_block = fs->bs / dev->block_size;
    fs->blocks = g32(sb + SB_BLOCKS_COUNT);
    fs->inodes = g32(sb + SB_INODES_COUNT);
    fs->first_data_block = g32(sb + SB_FIRST_DATA_BLOCK);
    fs->bpg = g32(sb + SB_BLOCKS_PER_GROUP);
    fs->ipg = g32(sb + SB_INODES_PER_GROUP);
    bool rev1 = g32(sb + SB_REV_LEVEL) >= 1;
    fs->inode_size = rev1 ? g16(sb + SB_INODE_SIZE) : 128;
    fs->first_ino = rev1 ? g32(sb + SB_FIRST_INO) : 11;
    fs->incompat = rev1 ? g32(sb + SB_FEATURE_INCOMPAT) : 0;
    uint32_t ro = rev1 ? g32(sb + SB_FEATURE_RO) : 0;
    if (!fs->bpg || !fs->ipg || fs->inode_size < 128 || fs->inode_size > fs->bs || (fs->inode_size & (fs->inode_size - 1)) ||
        (uint64_t)fs->blocks * fs->bs > blk_size_bytes(dev) || fs->first_data_block >= fs->blocks) {
        kprintf("ext2: %s: superblock values out of range\n", dev->name);
        goto fail;
    }
    if (fs->incompat & 0x40) { kprintf("ext2: %s: ext4 extents are not supported yet\n", dev->name); goto fail; }
    if (fs->incompat & INCOMPAT_RECOVER) { kprintf("ext2: %s: journal needs recovery (run e2fsck)\n", dev->name); goto fail; }
    if (fs->incompat & ~INCOMPAT_SUPPORTED) {
        kprintf("ext2: %s: unsupported features %x\n", dev->name, fs->incompat & ~INCOMPAT_SUPPORTED);
        goto fail;
    }
    fs->rdonly = rdonly || !dev->parent || !blk_disk_of(dev)->ops->write;
    if (!fs->rdonly && (ro & ~RO_SUPPORTED)) {
        kprintf("ext2: %s: features %x need support we lack: mounted read-only\n", dev->name, ro & ~RO_SUPPORTED);
        fs->rdonly = true;
    }
    fs->ngroups = (fs->blocks - fs->first_data_block + fs->bpg - 1) / fs->bpg;
    if ((uint64_t)fs->ngroups * fs->ipg < fs->inodes) goto fail;
    fs->gdt_blocks = (fs->ngroups * 32 + fs->bs - 1) / fs->bs;
    fs->gdt = kmalloc((size_t)fs->gdt_blocks * fs->bs);
    if (!fs->gdt) { r = -ENOMEM; goto fail; }
    if (dev_io(fs, fs->first_data_block + 1, fs->gdt_blocks, fs->gdt, false) < 0) { r = -EIO; goto fail; }
    for (uint32_t g = 0; g < fs->ngroups; g++)
        if (!blk_ok(fs, gd_block_bitmap(fs, g)) || !blk_ok(fs, gd_inode_bitmap(fs, g)) || !blk_ok(fs, gd_inode_table(fs, g))) {
            kprintf("ext2: %s: group %u descriptor out of range\n", dev->name, g);
            goto fail;
        }
    fs->nbufs = CACHE_BYTES / fs->bs;
    fs->bufs = kzalloc(sizeof(struct buf) * fs->nbufs);
    uint8_t *pool = kmalloc(CACHE_BYTES);
    if (!fs->bufs || !pool) { kfree(pool); r = -ENOMEM; goto fail; }
    for (uint32_t i = 0; i < fs->nbufs; i++) fs->bufs[i].data = pool + (size_t)i * fs->bs;

    struct mount *m = kzalloc(sizeof *m);
    if (!m) { r = -ENOMEM; goto fail; }
    strlcpy(m->fstype, "ext2", sizeof m->fstype);
    m->flags = fs->rdonly ? MNT_RDONLY : 0;
    m->dev = 0x800 + (uint32_t)(dev - blk_get(0));
    m->priv = fs;
    m->sync = e2_sync;
    m->statfs = e2_statfs;
    fs->mnt = m;
    struct e2ino *re = iget(fs, EXT2_ROOT_INO);
    if (!re || (g16(re->raw + I_MODE) & S_IFMT) != S_IFDIR) { kprintf("ext2: %s: no root directory\n", dev->name); goto fail; }
    m->root = make_vnode(fs, re, "");
    if (!m->root) { r = -ENOMEM; goto fail; }

    if (!fs->rdonly) {                                  /* in use: not clean until shut down */
        p16(sb + SB_STATE, 0);
        p16(sb + SB_MNT_COUNT, (uint16_t)(g16(sb + SB_MNT_COUNT) + 1));
        p32(sb + SB_MTIME, now32());
        fs->sb_dirty = true;
        flush_all(fs);
    }
    static bool flusher_started;
    for (int i = 0; i < 8; i++) if (!volumes[i]) { volumes[i] = fs; break; }
    if (!fs->rdonly && !flusher_started) flusher_started = task_create("ext2-flush", flusher, NULL) != NULL;
    char label[17];
    memcpy(label, sb + SB_VOLUME_NAME, 16);
    label[16] = 0;
    kprintf("ext2: %s: \"%s\", %u KiB blocks, %u/%u blocks free, %u groups%s\n", dev->name, label, fs->bs / 1024,
            g32(sb + SB_FREE_BLOCKS), fs->blocks, fs->ngroups, fs->rdonly ? ", read-only" : "");
    *out = m;
    return 0;
fail:
    kfree(fs->gdt);
    kfree(fs);
    return r;
}
