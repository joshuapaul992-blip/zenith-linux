/* include/kernel/spinlock.h -- interrupt-safe spinlocks
 *
 * A spinlock is taken with interrupts disabled on the local CPU (so an
 * interrupt handler that wants the same lock cannot deadlock against the
 * code it interrupted) and acquired with a locked exchange, which is also a
 * full memory barrier on x86: no load or store moves across it, whatever
 * the store buffer would otherwise allow under TSO. Release is a plain
 * store with release ordering, which TSO keeps behind every earlier access.
 * Never sleep while holding one. */
#ifndef KESTREL_SPINLOCK_H
#define KESTREL_SPINLOCK_H

#include <stdint.h>
#include <kernel/cpu.h>

struct spinlock { volatile uint32_t locked; };

static inline uint64_t spin_lock_irqsave(struct spinlock *l)
{
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) cpu_relax();   /* spin on a read, not a locked op */
    return f;
}

static inline void spin_unlock_irqrestore(struct spinlock *l, uint64_t f)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

/* Release the lock but keep interrupts off (the caller restores them). */
static inline void spin_unlock_keep_irqs(struct spinlock *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

#endif
