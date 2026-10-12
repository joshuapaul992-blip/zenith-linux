/* linux/rculist.h -- Kestrel's Linux KPI: RCU list variants (plain lists here, see rcupdate.h) */
#ifndef _LKPI_LINUX_RCULIST_H
#define _LKPI_LINUX_RCULIST_H
#include <linux/list.h>
#include <linux/rcupdate.h>
#define list_add_rcu(n, h)              list_add(n, h)
#define list_add_tail_rcu(n, h)         list_add_tail(n, h)
#define list_del_rcu(e)                 __list_del_entry(e)
#define list_replace_rcu(o, n)          list_replace(o, n)
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_first_or_null_rcu(ptr, type, member) list_first_entry_or_null(ptr, type, member)
#define hlist_add_head_rcu(n, h)        hlist_add_head(n, h)
#define hlist_del_rcu(n)                __hlist_del(n)
#define hlist_del_init_rcu(n)           hlist_del_init(n)
#define hlist_for_each_entry_rcu(pos, head, member, ...) hlist_for_each_entry(pos, head, member)
#endif
