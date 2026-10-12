/* linux/idr.h -- Kestrel's Linux KPI: small integer ids (linuxkpi/src/idr.c)
 *
 * A simple IDR: a sorted list of (id, pointer) pairs, enough for the
 * handful of ids a driver hands out (minor numbers, instance numbers).
 * The IDA is an IDR whose pointers are all the same marker. */
#ifndef _LKPI_LINUX_IDR_H
#define _LKPI_LINUX_IDR_H

#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/spinlock.h>
#include <linux/list.h>

struct idr {
    struct list_head    lkpi_entries;   /* struct lkpi_idr_entry, by id */
    spinlock_t          lkpi_lock;
    unsigned int        idr_base;
    unsigned int        idr_next;
};
#define IDR_INIT_BASE(name, base) \
    { .lkpi_entries = LIST_HEAD_INIT((name).lkpi_entries), .lkpi_lock = __SPIN_LOCK_UNLOCKED(name.lkpi_lock), \
      .idr_base = (base), .idr_next = 0 }
#define IDR_INIT(name)          IDR_INIT_BASE(name, 0)
#define DEFINE_IDR(name)        struct idr name = IDR_INIT(name)

void idr_init_base(struct idr *idr, int base);
static inline void idr_init(struct idr *idr) { idr_init_base(idr, 0); }
/* an id in [start, end) (end <= 0: no limit) for ptr: the id, -ENOMEM or -ENOSPC */
int  idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int  idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int  idr_alloc_u32(struct idr *idr, void *ptr, u32 *id, unsigned long max, gfp_t gfp);
void *idr_find(const struct idr *idr, unsigned long id);
void *idr_remove(struct idr *idr, unsigned long id);
void *idr_replace(struct idr *idr, void *ptr, unsigned long id);
void *idr_get_next(struct idr *idr, int *nextid);
void *idr_get_next_ul(struct idr *idr, unsigned long *nextid);
int  idr_for_each(const struct idr *idr, int (*fn)(int id, void *p, void *data), void *data);
void idr_destroy(struct idr *idr);
bool idr_is_empty(const struct idr *idr);
static inline void idr_preload(gfp_t gfp) { }
static inline void idr_preload_end(void) { }
#define idr_for_each_entry(idr, entry, id) \
    for (id = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; id += 1U)
#define idr_for_each_entry_ul(idr, entry, tmp, id) \
    for (tmp = 0, id = 0; ((entry) = idr_get_next_ul(idr, &(id))) != NULL; id += 1UL)

struct ida { struct idr idr; };
#define IDA_INIT(name)          { .idr = IDR_INIT((name).idr) }
#define DEFINE_IDA(name)        struct ida name = IDA_INIT(name)
static inline void ida_init(struct ida *ida) { idr_init(&ida->idr); }
/* an id in [min, max] (inclusive, as in Linux) */
int  ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp);
static inline int ida_alloc(struct ida *ida, gfp_t gfp) { return ida_alloc_range(ida, 0, ~0U >> 1, gfp); }
static inline int ida_alloc_min(struct ida *ida, unsigned int min, gfp_t gfp) { return ida_alloc_range(ida, min, ~0U >> 1, gfp); }
static inline int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp) { return ida_alloc_range(ida, 0, max, gfp); }
void ida_free(struct ida *ida, unsigned int id);
void ida_destroy(struct ida *ida);
static inline bool ida_is_empty(const struct ida *ida) { return idr_is_empty(&ida->idr); }
#define ida_simple_get(ida, start, end, gfp)    ida_alloc_range(ida, start, (end) ? (end) - 1 : ~0U >> 1, gfp)
#define ida_simple_remove(ida, id)              ida_free(ida, id)

#endif
