/* linux/rcupdate.h -- Kestrel's Linux KPI: a minimal RCU
 *
 * Readers disable preemption; synchronize_rcu() waits until every CPU
 * has been through a point outside any reader (it runs a no-op on each
 * by scheduling, approximated here by a short sleep: phase 1 has no
 * per-CPU quiescent-state tracking). call_rcu/kfree_rcu defer through
 * the system workqueue after such a grace period. */
#ifndef _LKPI_LINUX_RCUPDATE_H
#define _LKPI_LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/preempt.h>
#include <asm/barrier.h>

#define rcu_read_lock()                 preempt_disable()
#define rcu_read_unlock()               preempt_enable()
#define rcu_read_lock_bh()              preempt_disable()
#define rcu_read_unlock_bh()            preempt_enable()
#define rcu_read_lock_sched()           preempt_disable()
#define rcu_read_unlock_sched()         preempt_enable()
#define rcu_read_lock_held()            1
#define rcu_dereference(p)              READ_ONCE(p)
#define rcu_dereference_raw(p)          READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_check(p, c)     READ_ONCE(p)
#define rcu_access_pointer(p)           READ_ONCE(p)
#define rcu_assign_pointer(p, v)        smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v)          WRITE_ONCE(p, v)
#define RCU_INITIALIZER(v)              (v)
#define rcu_replace_pointer(rcu_ptr, ptr, c) ({ typeof(ptr) __tmp = (rcu_ptr); rcu_assign_pointer(rcu_ptr, ptr); __tmp; })

void synchronize_rcu(void);
void call_rcu(struct rcu_head *head, rcu_callback_t func);
void rcu_barrier(void);
void lkpi_kfree_rcu(void *obj, struct rcu_head *head);
#define synchronize_rcu_expedited()     synchronize_rcu()
#define synchronize_net()               synchronize_rcu()
#define kfree_rcu(ptr, rhf)             lkpi_kfree_rcu((ptr), &(ptr)->rhf)
#define kfree_rcu_mightsleep(ptr)       do { synchronize_rcu(); kfree(ptr); } while (0)

#endif
