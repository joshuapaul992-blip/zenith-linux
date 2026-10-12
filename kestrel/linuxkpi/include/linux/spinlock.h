/* linux/spinlock.h -- Kestrel's Linux KPI: spinlocks
 *
 * A test-and-set lock taken with preemption (and so interrupts, see
 * preempt.h) off. The _irq/_irqsave forms are the same plus the explicit
 * interrupt state; _bh is the same as the plain form (no softirqs here:
 * timers and tasklets run in threads). rwlock_t is a plain spinlock. */
#ifndef _LKPI_LINUX_SPINLOCK_H
#define _LKPI_LINUX_SPINLOCK_H

#include <linux/spinlock_types.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/compiler.h>
#include <linux/bottom_half.h>
#include <linux/atomic.h>
#include <asm/barrier.h>
#include <asm/processor.h>

static inline void raw_spin_lock_init(raw_spinlock_t *l) { l->locked = 0; }
static inline bool __lkpi_raw_trylock(raw_spinlock_t *l) { return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE); }
static inline void __lkpi_raw_lock(raw_spinlock_t *l)
{
    while (!__lkpi_raw_trylock(l))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) cpu_relax();
}
static inline void __lkpi_raw_unlock(raw_spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }

static inline void raw_spin_lock(raw_spinlock_t *l) { preempt_disable(); __lkpi_raw_lock(l); }
static inline void raw_spin_unlock(raw_spinlock_t *l) { __lkpi_raw_unlock(l); preempt_enable(); }
static inline int raw_spin_trylock(raw_spinlock_t *l)
{
    preempt_disable();
    if (__lkpi_raw_trylock(l)) return 1;
    preempt_enable();
    return 0;
}
static inline void raw_spin_lock_irq(raw_spinlock_t *l) { local_irq_disable(); raw_spin_lock(l); }
static inline void raw_spin_unlock_irq(raw_spinlock_t *l) { raw_spin_unlock(l); local_irq_enable(); }
static inline int raw_spin_is_locked(raw_spinlock_t *l) { return READ_ONCE(l->locked) != 0; }
#define raw_spin_lock_irqsave(l, flags)         do { local_irq_save(flags); raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irqrestore(l, flags)    do { raw_spin_unlock(l); local_irq_restore(flags); } while (0)
#define raw_spin_lock_nested(l, sub)            raw_spin_lock(l)

#define spin_lock_init(l)               do { raw_spin_lock_init(&(l)->rlock); } while (0)
static inline void spin_lock(spinlock_t *l) { raw_spin_lock(&l->rlock); }
static inline void spin_unlock(spinlock_t *l) { raw_spin_unlock(&l->rlock); }
static inline int spin_trylock(spinlock_t *l) { return raw_spin_trylock(&l->rlock); }
static inline void spin_lock_irq(spinlock_t *l) { raw_spin_lock_irq(&l->rlock); }
static inline void spin_unlock_irq(spinlock_t *l) { raw_spin_unlock_irq(&l->rlock); }
static inline void spin_lock_bh(spinlock_t *l) { raw_spin_lock(&l->rlock); }
static inline void spin_unlock_bh(spinlock_t *l) { raw_spin_unlock(&l->rlock); }
static inline int spin_trylock_bh(spinlock_t *l) { return raw_spin_trylock(&l->rlock); }
static inline int spin_is_locked(spinlock_t *l) { return raw_spin_is_locked(&l->rlock); }
#define spin_lock_irqsave(l, flags)             raw_spin_lock_irqsave(&(l)->rlock, flags)
#define spin_unlock_irqrestore(l, flags)        raw_spin_unlock_irqrestore(&(l)->rlock, flags)
#define spin_trylock_irqsave(l, flags) \
    ({ local_irq_save(flags); spin_trylock(l) ? 1 : ({ local_irq_restore(flags); 0; }); })
#define spin_lock_nested(l, sub)                spin_lock(l)
#define spin_lock_irqsave_nested(l, f, sub)     spin_lock_irqsave(l, f)
#define assert_spin_locked(l)                   do { (void)(l); } while (0)
#define assert_raw_spin_locked(l)               do { (void)(l); } while (0)

#define rwlock_init(l)                  do { raw_spin_lock_init(&(l)->lock); } while (0)
#define read_lock(l)                    raw_spin_lock(&(l)->lock)
#define read_unlock(l)                  raw_spin_unlock(&(l)->lock)
#define write_lock(l)                   raw_spin_lock(&(l)->lock)
#define write_unlock(l)                 raw_spin_unlock(&(l)->lock)
#define read_lock_irq(l)                raw_spin_lock_irq(&(l)->lock)
#define read_unlock_irq(l)              raw_spin_unlock_irq(&(l)->lock)
#define write_lock_irq(l)               raw_spin_lock_irq(&(l)->lock)
#define write_unlock_irq(l)             raw_spin_unlock_irq(&(l)->lock)
#define read_lock_bh(l)                 raw_spin_lock(&(l)->lock)
#define read_unlock_bh(l)               raw_spin_unlock(&(l)->lock)
#define write_lock_bh(l)                raw_spin_lock(&(l)->lock)
#define write_unlock_bh(l)              raw_spin_unlock(&(l)->lock)
#define read_lock_irqsave(l, f)         raw_spin_lock_irqsave(&(l)->lock, f)
#define read_unlock_irqrestore(l, f)    raw_spin_unlock_irqrestore(&(l)->lock, f)
#define write_lock_irqsave(l, f)        raw_spin_lock_irqsave(&(l)->lock, f)
#define write_unlock_irqrestore(l, f)   raw_spin_unlock_irqrestore(&(l)->lock, f)

/* decrement, and take the lock if the count reached zero (true then) */
static inline bool atomic_dec_and_lock_kpi(atomic_t *a, spinlock_t *l)
{
    if (atomic_add_unless(a, -1, 1)) return false;
    spin_lock(l);
    if (atomic_dec_and_test(a)) return true;
    spin_unlock(l);
    return false;
}
#define atomic_dec_and_lock(a, l)       atomic_dec_and_lock_kpi(a, l)
#define refcount_dec_and_lock(r, l)     atomic_dec_and_lock_kpi(&(r)->refs, l)

#endif
