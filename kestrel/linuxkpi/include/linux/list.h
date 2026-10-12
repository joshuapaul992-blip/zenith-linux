/* linux/list.h -- Kestrel's Linux KPI: circular doubly linked lists
 * (struct list_head) and hash-bucket lists (struct hlist_head/node) */
#ifndef _LKPI_LINUX_LIST_H
#define _LKPI_LINUX_LIST_H

#include <linux/types.h>
#include <linux/container_of.h>
#include <linux/compiler.h>
#include <linux/poison.h>

#define LIST_HEAD_INIT(name)    { &(name), &(name) }
#define LIST_HEAD(name)         struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *l) { WRITE_ONCE(l->next, l); l->prev = l; }

/* insert n between two adjacent entries */
static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    WRITE_ONCE(prev->next, n);
}

static inline void list_add(struct list_head *n, struct list_head *head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    WRITE_ONCE(prev->next, next);
}

static inline void __list_del_entry(struct list_head *e) { __list_del(e->prev, e->next); }

static inline void list_del(struct list_head *e)
{
    __list_del_entry(e);
    e->next = LIST_POISON1;
    e->prev = LIST_POISON2;
}

static inline void list_del_init(struct list_head *e) { __list_del_entry(e); INIT_LIST_HEAD(e); }

static inline void list_replace(struct list_head *old, struct list_head *n)
{
    n->next = old->next;
    n->next->prev = n;
    n->prev = old->prev;
    n->prev->next = n;
}

static inline void list_replace_init(struct list_head *old, struct list_head *n) { list_replace(old, n); INIT_LIST_HEAD(old); }
static inline void list_move(struct list_head *e, struct list_head *head) { __list_del_entry(e); list_add(e, head); }
static inline void list_move_tail(struct list_head *e, struct list_head *head) { __list_del_entry(e); list_add_tail(e, head); }
static inline int list_is_first(const struct list_head *e, const struct list_head *head) { return e->prev == head; }
static inline int list_is_last(const struct list_head *e, const struct list_head *head) { return e->next == head; }
static inline int list_is_head(const struct list_head *e, const struct list_head *head) { return e == head; }
static inline int list_empty(const struct list_head *head) { return READ_ONCE(head->next) == head; }
static inline int list_empty_careful(const struct list_head *head) { return head->next == head && head->prev == head; }
static inline int list_is_singular(const struct list_head *head) { return !list_empty(head) && head->next == head->prev; }

static inline void list_rotate_left(struct list_head *head)
{
    if (!list_empty(head)) list_move_tail(head->next, head);
}

static inline void __list_splice(const struct list_head *list, struct list_head *prev, struct list_head *next)
{
    struct list_head *first = list->next, *last = list->prev;
    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}

static inline void list_splice(const struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) __list_splice(list, head, head->next);
}

static inline void list_splice_tail(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) __list_splice(list, head->prev, head);
}

static inline void list_splice_init(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) { __list_splice(list, head, head->next); INIT_LIST_HEAD(list); }
}

static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) { __list_splice(list, head->prev, head); INIT_LIST_HEAD(list); }
}

/* move the entries up to and including `entry` from head to list */
static inline void list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry)
{
    if (list_empty(head) || (list_is_singular(head) && head->next != entry && head != entry)) return;
    if (entry == head) { INIT_LIST_HEAD(list); return; }
    struct list_head *first = head->next;
    list->next = first;
    first->prev = list;
    list->prev = entry;
    head->next = entry->next;
    entry->next->prev = head;
    entry->next = list;
}

static inline size_t list_count_nodes(struct list_head *head)
{
    size_t n = 0;
    for (struct list_head *p = head->next; p != head; p = p->next) n++;
    return n;
}

#define list_entry(ptr, type, member)           container_of(ptr, type, member)
#define list_first_entry(ptr, type, member)     list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member)      list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member) \
    ({ struct list_head *__h = (ptr); struct list_head *__p = READ_ONCE(__h->next); \
       __p != __h ? list_entry(__p, type, member) : NULL; })
#define list_next_entry(pos, member)            list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member)            list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_entry_is_head(pos, head, member)   (&(pos)->member == (head))

#define list_for_each(pos, head)        for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_prev(pos, head)   for (pos = (head)->prev; pos != (head); pos = pos->prev)
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_prev_safe(pos, n, head) \
    for (pos = (head)->prev, n = pos->prev; pos != (head); pos = n, n = pos->prev)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, typeof(*pos), member); !list_entry_is_head(pos, head, member); \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
    for (pos = list_last_entry(head, typeof(*pos), member); !list_entry_is_head(pos, head, member); \
         pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, typeof(*pos), member), n = list_next_entry(pos, member); \
         !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
    for (pos = list_last_entry(head, typeof(*pos), member), n = list_prev_entry(pos, member); \
         !list_entry_is_head(pos, head, member); pos = n, n = list_prev_entry(n, member))
#define list_prepare_entry(pos, head, member)   ((pos) ? : list_entry(head, typeof(*pos), member))
#define list_for_each_entry_continue(pos, head, member) \
    for (pos = list_next_entry(pos, member); !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_from(pos, head, member) \
    for (; !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe_from(pos, n, head, member) \
    for (n = list_next_entry(pos, member); !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))

/* ---- hlist: one pointer per bucket head ---------------------------------- */
#define HLIST_HEAD_INIT         { .first = NULL }
#define HLIST_HEAD(name)        struct hlist_head name = { .first = NULL }
#define INIT_HLIST_HEAD(ptr)    ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h) { h->next = NULL; h->pprev = NULL; }
static inline int hlist_unhashed(const struct hlist_node *h) { return !h->pprev; }
static inline int hlist_empty(const struct hlist_head *h) { return !READ_ONCE(h->first); }

static inline void __hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next, **pprev = n->pprev;
    WRITE_ONCE(*pprev, next);
    if (next) next->pprev = pprev;
}

static inline void hlist_del(struct hlist_node *n) { __hlist_del(n); n->next = LIST_POISON1; n->pprev = LIST_POISON2; }
static inline void hlist_del_init(struct hlist_node *n) { if (!hlist_unhashed(n)) { __hlist_del(n); INIT_HLIST_NODE(n); } }

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first) first->pprev = &n->next;
    WRITE_ONCE(h->first, n);
    n->pprev = &h->first;
}

static inline void hlist_add_before(struct hlist_node *n, struct hlist_node *next)
{
    n->pprev = next->pprev;
    n->next = next;
    next->pprev = &n->next;
    WRITE_ONCE(*(n->pprev), n);
}

static inline void hlist_add_behind(struct hlist_node *n, struct hlist_node *prev)
{
    n->next = prev->next;
    WRITE_ONCE(prev->next, n);
    n->pprev = &prev->next;
    if (n->next) n->next->pprev = &n->next;
}

#define hlist_entry(ptr, type, member)  container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) \
    ({ typeof(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define hlist_for_each(pos, head)       for (pos = (head)->first; pos; pos = pos->next)
#define hlist_for_each_safe(pos, n, head) for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)
#define hlist_for_each_entry(pos, head, member) \
    for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); pos; \
         pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
    for (pos = hlist_entry_safe((head)->first, typeof(*pos), member); \
         pos && ({ n = pos->member.next; 1; }); pos = hlist_entry_safe(n, typeof(*pos), member))

#endif
