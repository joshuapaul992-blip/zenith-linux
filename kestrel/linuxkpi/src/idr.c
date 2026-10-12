/* linuxkpi/src/idr.c -- IDR and IDA, the simple way
 *
 * An IDR is a list of (id, pointer) entries sorted by id, under a
 * spinlock. Allocation walks the list for the first gap at or after the
 * start: linear, which is fine for the handful of ids a driver uses
 * (Linux's radix tree is for thousands). An IDA is an IDR whose entries
 * all point at one marker. */
#include <linux/kernel.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <linux/err.h>
#include "internal.h"

struct idr_entry {
    struct list_head    node;
    unsigned long       id;
    void               *ptr;
};

void idr_init_base(struct idr *idr, int base)
{
    INIT_LIST_HEAD(&idr->lkpi_entries);
    spin_lock_init(&idr->lkpi_lock);
    idr->idr_base = (unsigned)base;
    idr->idr_next = 0;
}

/* lock held: insert e for the first free id in [start, end]; -ENOSPC if none */
static int insert_locked(struct idr *idr, struct idr_entry *e, unsigned long start, unsigned long end)
{
    struct idr_entry *pos;
    unsigned long id = start;
    if (id < idr->idr_base) id = idr->idr_base;
    list_for_each_entry(pos, &idr->lkpi_entries, node) {
        if (pos->id < id) continue;
        if (pos->id > id) break;
        id++;                                       /* taken: try the next */
    }
    if (id > end) return -ENOSPC;
    e->id = id;
    list_add_tail(&e->node, &pos->node);            /* before the first larger id */
    return 0;
}

static int alloc_range(struct idr *idr, void *ptr, unsigned long start, unsigned long end, gfp_t gfp,
                       unsigned long *out)
{
    struct idr_entry *e = kmalloc(sizeof *e, gfp);
    unsigned long flags;
    if (!e) return -ENOMEM;
    e->ptr = ptr;
    spin_lock_irqsave(&idr->lkpi_lock, flags);
    int r = insert_locked(idr, e, start, end);
    spin_unlock_irqrestore(&idr->lkpi_lock, flags);
    if (r) { kfree(e); return r; }
    *out = e->id;
    return 0;
}

int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
    unsigned long id;
    if (start < 0) return -EINVAL;
    int r = alloc_range(idr, ptr, (unsigned long)start, end > 0 ? (unsigned long)end - 1 : INT_MAX, gfp, &id);
    return r ? r : (int)id;
}

int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
    unsigned long id;
    unsigned long max = end > 0 ? (unsigned long)end - 1 : INT_MAX;
    unsigned long from = idr->idr_next > (unsigned)start ? idr->idr_next : (unsigned long)start;
    int r = alloc_range(idr, ptr, from, max, gfp, &id);
    if (r == -ENOSPC && from > (unsigned long)start) r = alloc_range(idr, ptr, (unsigned long)start, max, gfp, &id);
    if (r) return r;
    idr->idr_next = (unsigned int)id + 1;
    return (int)id;
}

int idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max, gfp_t gfp)
{
    unsigned long id;
    int r = alloc_range(idr, ptr, *nextid, max, gfp, &id);
    if (!r) *nextid = (u32)id;
    return r;
}

static struct idr_entry *find_locked(const struct idr *idr, unsigned long id)
{
    struct idr_entry *e;
    list_for_each_entry(e, &idr->lkpi_entries, node) {
        if (e->id == id) return e;
        if (e->id > id) break;
    }
    return NULL;
}

void *idr_find(const struct idr *idr, unsigned long id)
{
    unsigned long flags;
    spinlock_t *lock = (spinlock_t *)&idr->lkpi_lock;
    spin_lock_irqsave(lock, flags);
    struct idr_entry *e = find_locked(idr, id);
    void *p = e ? e->ptr : NULL;
    spin_unlock_irqrestore(lock, flags);
    return p;
}

void *idr_remove(struct idr *idr, unsigned long id)
{
    unsigned long flags;
    spin_lock_irqsave(&idr->lkpi_lock, flags);
    struct idr_entry *e = find_locked(idr, id);
    if (e) list_del(&e->node);
    spin_unlock_irqrestore(&idr->lkpi_lock, flags);
    if (!e) return NULL;
    void *p = e->ptr;
    kfree(e);
    return p;
}

void *idr_replace(struct idr *idr, void *ptr, unsigned long id)
{
    unsigned long flags;
    spin_lock_irqsave(&idr->lkpi_lock, flags);
    struct idr_entry *e = find_locked(idr, id);
    void *old = ERR_PTR(-ENOENT);
    if (e) { old = e->ptr; e->ptr = ptr; }
    spin_unlock_irqrestore(&idr->lkpi_lock, flags);
    return old;
}

void *idr_get_next_ul(struct idr *idr, unsigned long *nextid)
{
    unsigned long flags;
    struct idr_entry *e;
    void *p = NULL;
    spin_lock_irqsave(&idr->lkpi_lock, flags);
    list_for_each_entry(e, &idr->lkpi_entries, node) {
        if (e->id >= *nextid && e->ptr) { *nextid = e->id; p = e->ptr; break; }
    }
    spin_unlock_irqrestore(&idr->lkpi_lock, flags);
    return p;
}

void *idr_get_next(struct idr *idr, int *nextid)
{
    unsigned long id = (unsigned long)*nextid;
    void *p = idr_get_next_ul(idr, &id);
    if (p) *nextid = (int)id;
    return p;
}

int idr_for_each(const struct idr *idr, int (*fn)(int id, void *p, void *data), void *data)
{
    struct idr_entry *e;
    list_for_each_entry(e, &idr->lkpi_entries, node) {
        int r = fn((int)e->id, e->ptr, data);
        if (r) return r;
    }
    return 0;
}

void idr_destroy(struct idr *idr)
{
    unsigned long flags;
    struct idr_entry *e, *n;
    LIST_HEAD(gone);
    spin_lock_irqsave(&idr->lkpi_lock, flags);
    list_splice_init(&idr->lkpi_entries, &gone);
    spin_unlock_irqrestore(&idr->lkpi_lock, flags);
    list_for_each_entry_safe(e, n, &gone, node) kfree(e);
}

bool idr_is_empty(const struct idr *idr) { return list_empty(&idr->lkpi_entries); }

/* ---- IDA ------------------------------------------------------------------------ */
static char ida_marker;

int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp)
{
    unsigned long id;
    if ((int)min < 0) return -ENOSPC;
    if ((int)max < 0) max = INT_MAX;
    int r = alloc_range(&ida->idr, &ida_marker, min, max, gfp, &id);
    return r ? r : (int)id;
}

void ida_free(struct ida *ida, unsigned int id)
{
    if (!idr_remove(&ida->idr, id)) printk(KERN_WARNING "linuxkpi: ida_free called for id=%u which is not allocated\n", id);
}

void ida_destroy(struct ida *ida) { idr_destroy(&ida->idr); }
