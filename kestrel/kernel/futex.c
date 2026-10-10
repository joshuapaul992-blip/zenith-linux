/* kernel/futex.c -- futexes: the kernel half of user-space locks (pthread
 * mutexes, condition variables, joins, semaphores).
 *
 * Waiters wait on a key: for a private futex (FUTEX_PRIVATE_FLAG, or any
 * futex in a private mapping) the address space and the user address; for a
 * shared mapping the memory object and the offset in it, so processes that
 * map the same pages meet on the same key. Keys hash into 256 buckets, each
 * a list of waiters (struct futex_q, on the waiter's own kernel stack)
 * guarded by a spinlock.
 *
 * No lost wake-ups: FUTEX_WAIT reads the futex word while it holds the
 * bucket lock and only then queues itself and sleeps. A waker that changed
 * the word must take the same lock to find the queue, so either the waiter
 * sees the new value (and returns EAGAIN) or the waker sees the waiter. The
 * read under the lock is a no-fault read (the page is faulted in before the
 * lock is taken); if the page vanished meanwhile, the call fails with EFAULT
 * rather than sleeping with a lock held. Read-modify-writes of user memory
 * (FUTEX_WAKE_OP, robust lists) are lock cmpxchg loops.
 *
 * Implemented: WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP, WAIT_BITSET,
 * WAKE_BITSET, with relative or absolute (MONOTONIC/REALTIME) timeouts.
 * Not implemented: the priority-inheritance ops (ENOSYS). */
#include <kernel/futex.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/vm.h>
#include <kernel/vmobj.h>
#include <kernel/uaccess.h>
#include <kernel/process.h>
#include <kernel/arch.h>
#include <kernel/time.h>
#include <kernel/posix.h>
#include <kernel/klog.h>

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_BITSET_ALL      0xffffffffu

#define NBUCKETS 256

struct futex_key { uint64_t a, b; };

struct futex_q {
    struct futex_key key;
    uint32_t         bitset;
    struct tcb      *task;
    bool             woken;
    struct futex_q  *next;
};

static struct bucket {
    struct spinlock lock;
    struct futex_q *head;
} buckets[NBUCKETS];

static struct bucket *bucket_of(const struct futex_key *k)
{
    uint64_t h = k->a * 0x9E3779B97F4A7C15ull ^ (k->b >> 2) * 0xC2B2AE3D27D4EB4Full;
    return &buckets[(h >> 32) % NBUCKETS];
}

static bool key_eq(const struct futex_key *x, const struct futex_key *y) { return x->a == y->a && x->b == y->b; }

static int get_key(uint64_t uaddr, bool shared, struct futex_key *k)
{
    struct tcb *t = current_task();
    if (uaddr & 3) return -EINVAL;
    if (!user_range_ok(uaddr, 4) || !t->mm) return -EFAULT;
    if (shared) {
        struct vm_object *obj = NULL;
        uint64_t off = 0;
        if (vm_shared_key(t->mm, uaddr, &obj, &off) == 0 && obj) {
            k->a = (uint64_t)obj;
            k->b = off;
            return 0;
        }
    }
    k->a = (uint64_t)t->mm;                             /* private: this address space */
    k->b = uaddr;
    return 0;
}

static void q_remove(struct bucket *b, struct futex_q *q)
{
    for (struct futex_q **pp = &b->head; *pp; pp = &(*pp)->next)
        if (*pp == q) { *pp = q->next; return; }
}

/* Wake q (bucket locked): dequeue first, then mark, then make it runnable. */
static void q_wake(struct bucket *b, struct futex_q *q)
{
    q_remove(b, q);
    __atomic_store_n(&q->woken, true, __ATOMIC_RELEASE);
    task_wake(q->task);
}

static int futex_wait(uint64_t uaddr, uint32_t val, uint64_t deadline_ms, uint32_t bitset, bool shared)
{
    struct futex_key key;
    int r = get_key(uaddr, shared, &key);
    if (r) return r;
    uint32_t cur;
    if (get_user_u32(&cur, (const void *)uaddr)) return -EFAULT;    /* fault the page in, no lock held */
    struct tcb *self = current_task();
    struct futex_q q = { .key = key, .bitset = bitset, .task = self };
    struct bucket *b = bucket_of(&key);
    uint64_t f = spin_lock_irqsave(&b->lock);
    if (get_user_u32_nofault(&cur, (const void *)uaddr)) { spin_unlock_irqrestore(&b->lock, f); return -EFAULT; }
    if (cur != val) { spin_unlock_irqrestore(&b->lock, f); return -EAGAIN; }
    if (deadline_ms && time_ms() >= deadline_ms) { spin_unlock_irqrestore(&b->lock, f); return -ETIMEDOUT; }
    if (signal_pending()) { spin_unlock_irqrestore(&b->lock, f); return -EINTR; }
    q.next = b->head;
    b->head = &q;
    for (;;) {
        task_block_self(&q, deadline_ms);               /* state set; we sleep at schedule() */
        spin_unlock_keep_irqs(&b->lock);
        schedule();                                     /* interrupts still off: nobody runs in between */
        self->wait_chan = NULL;
        /* Requeue may have moved us: lock the bucket our key is in now. */
        for (;;) {
            b = bucket_of(&q.key);
            spin_lock_irqsave(&b->lock);                /* interrupts are already off */
            if (b == bucket_of(&q.key)) break;
            spin_unlock_keep_irqs(&b->lock);
        }
        if (__atomic_load_n(&q.woken, __ATOMIC_ACQUIRE)) { r = 0; break; }
        if (signal_pending()) { r = -EINTR; break; }
        /* The sleep is timed in scheduler ticks, the deadline on the clock:
         * a tick that came early sends us back to sleep for the rest. */
        if (deadline_ms && time_ms() >= deadline_ms) { r = -ETIMEDOUT; break; }
    }
    if (r) q_remove(b, &q);
    spin_unlock_irqrestore(&b->lock, f);
    return r;
}

static int wake_key(const struct futex_key *key, int n, uint32_t bitset)
{
    struct bucket *b = bucket_of(key);
    int woken = 0;
    uint64_t f = spin_lock_irqsave(&b->lock);
    for (struct futex_q *q = b->head, *next; q && woken < n; q = next) {
        next = q->next;
        if (key_eq(&q->key, key) && (q->bitset & bitset)) { q_wake(b, q); woken++; }
    }
    spin_unlock_irqrestore(&b->lock, f);
    return woken;
}

int futex_wake(uint64_t uaddr, int n, bool shared)
{
    struct futex_key key;
    int r = get_key(uaddr, shared, &key);
    return r ? r : wake_key(&key, n, FUTEX_BITSET_ALL);
}

/* Two buckets locked in a fixed (address) order: no ABBA deadlock. */
static uint64_t lock2(struct bucket *a, struct bucket *b)
{
    if (a == b) return spin_lock_irqsave(&a->lock);
    if (a > b) { struct bucket *t = a; a = b; b = t; }
    uint64_t f = spin_lock_irqsave(&a->lock);
    spin_lock_irqsave(&b->lock);
    return f;
}

static void unlock2(struct bucket *a, struct bucket *b, uint64_t f)
{
    if (a != b) spin_unlock_keep_irqs(&b->lock);
    spin_unlock_irqrestore(&a->lock, f);
}

static int futex_requeue(uint64_t u1, uint64_t u2, int nwake, int nrequeue, bool cmp, uint32_t val3, bool shared)
{
    struct futex_key k1, k2;
    int r;
    if ((r = get_key(u1, shared, &k1)) || (r = get_key(u2, shared, &k2))) return r;
    if (nwake < 0 || nrequeue < 0) return -EINVAL;
    uint32_t cur;
    if (cmp && get_user_u32(&cur, (const void *)u1)) return -EFAULT;
    struct bucket *b1 = bucket_of(&k1), *b2 = bucket_of(&k2);
    uint64_t f = lock2(b1, b2);
    if (cmp) {
        if (get_user_u32_nofault(&cur, (const void *)u1)) { unlock2(b1, b2, f); return -EFAULT; }
        if (cur != val3) { unlock2(b1, b2, f); return -EAGAIN; }
    }
    int done = 0, moved = 0;
    for (struct futex_q *q = b1->head, *next; q; q = next) {
        next = q->next;
        if (!key_eq(&q->key, &k1)) continue;
        if (done < nwake) { q_wake(b1, q); done++; continue; }
        if (moved >= nrequeue) break;
        q->key = k2;                                    /* now waits on u2 */
        if (b1 != b2) { q_remove(b1, q); q->next = b2->head; b2->head = q; }
        moved++;
    }
    unlock2(b1, b2, f);
    return done + moved;
}

/* FUTEX_WAKE_OP: *u2 = op(*u2, oparg) atomically; wake n on u1, and n2 on
 * u2 if the old value of *u2 passes the comparison. */
static int futex_wake_op(uint64_t u1, uint64_t u2, int n, int n2, uint32_t enc, bool shared)
{
    struct futex_key k1, k2;
    int r;
    if ((r = get_key(u1, shared, &k1)) || (r = get_key(u2, shared, &k2))) return r;
    int op = (enc >> 28) & 7, cmpop = (enc >> 24) & 15;
    int32_t oparg = (int32_t)(enc << 8) >> 20, cmparg = (int32_t)(enc << 20) >> 20;
    if ((enc >> 28) & 8) { if (oparg < 0 || oparg > 31) return -EINVAL; oparg = 1 << oparg; }
    uint32_t old, seen;
    if (get_user_u32(&old, (const void *)u2)) return -EFAULT;
    for (int tries = 0;; tries++) {
        uint32_t nv;
        switch (op) {
        case 0: nv = (uint32_t)oparg; break;
        case 1: nv = old + (uint32_t)oparg; break;
        case 2: nv = old | (uint32_t)oparg; break;
        case 3: nv = old & ~(uint32_t)oparg; break;
        case 4: nv = old ^ (uint32_t)oparg; break;
        default: return -ENOSYS;
        }
        if (user_cmpxchg_u32((void *)u2, old, nv, &seen)) return -EFAULT;
        if (seen == old) break;
        old = seen;
        if (tries > 1000) return -EAGAIN;
    }
    int woken = wake_key(&k1, n, FUTEX_BITSET_ALL);
    bool c;
    int32_t o = (int32_t)old;
    switch (cmpop) {
    case 0: c = o == cmparg; break;
    case 1: c = o != cmparg; break;
    case 2: c = o < cmparg; break;
    case 3: c = o <= cmparg; break;
    case 4: c = o > cmparg; break;
    case 5: c = o >= cmparg; break;
    default: return -ENOSYS;
    }
    if (c) woken += wake_key(&k2, n2, FUTEX_BITSET_ALL);
    return woken;
}

/* Timeout in ms since boot (0 = none) from a user timespec. */
static int timeout_of(uint64_t utime, bool absolute, bool realtime, uint64_t *deadline)
{
    *deadline = 0;
    if (!utime) return 0;
    int64_t ts[2];
    if (copy_from_user(ts, (const void *)utime, sizeof ts)) return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -EINVAL;
    uint64_t ms = (uint64_t)ts[0] > (1ull << 40) ? (1ull << 50) : (uint64_t)ts[0] * 1000 + (uint64_t)(ts[1] + 999999) / 1000000;
    uint64_t now = time_ms();
    if (!absolute) *deadline = now + ms;
    else if (realtime) {
        int64_t s, ns;
        time_realtime(&s, &ns);
        uint64_t rt = (uint64_t)s * 1000 + (uint64_t)ns / 1000000;
        *deadline = ms > rt ? now + (ms - rt) : now;
    } else {
        *deadline = ms;                                 /* CLOCK_MONOTONIC = time since boot */
    }
    if (!*deadline) *deadline = 1;
    return 0;
}

int64_t sys_futex_call(uint64_t uaddr, int op, uint32_t val, uint64_t utime, uint64_t uaddr2, uint32_t val3)
{
    bool shared = !(op & FUTEX_PRIVATE_FLAG), rt = op & FUTEX_CLOCK_REALTIME;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    uint64_t deadline;
    int r;
    switch (cmd) {
    case FUTEX_WAIT:
        if (rt) return -ENOSYS;
        if ((r = timeout_of(utime, false, false, &deadline))) return r;
        return futex_wait(uaddr, val, deadline, FUTEX_BITSET_ALL, shared);
    case FUTEX_WAIT_BITSET:
        if (!val3) return -EINVAL;
        if ((r = timeout_of(utime, true, rt, &deadline))) return r;
        return futex_wait(uaddr, val, deadline, val3, shared);
    case FUTEX_WAKE:
        return futex_wake(uaddr, (int)val, shared);
    case FUTEX_WAKE_BITSET: {
        if (!val3) return -EINVAL;
        struct futex_key k;
        if ((r = get_key(uaddr, shared, &k))) return r;
        return wake_key(&k, (int)val, val3);
    }
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, uaddr2, (int)val, (int)(uint32_t)utime, false, 0, shared);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, uaddr2, (int)val, (int)(uint32_t)utime, true, val3, shared);
    case FUTEX_WAKE_OP:
        return futex_wake_op(uaddr, uaddr2, (int)val, (int)(uint32_t)utime, val3, shared);
    default:
        return -ENOSYS;                                 /* PI futexes */
    }
}

/* ---- robust futexes: locks held by a thread that dies ------------------------ */
#define FUTEX_WAITERS    0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK   0x3fffffffu

static void robust_one(uint64_t uaddr, int tid)
{
    uint32_t v, seen;
    if (uaddr & 3 || get_user_u32(&v, (const void *)uaddr)) return;
    for (int tries = 0; tries < 100; tries++) {
        if ((v & FUTEX_TID_MASK) != (uint32_t)tid) return;
        uint32_t nv = (v & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        if (user_cmpxchg_u32((void *)uaddr, v, nv, &seen)) return;
        if (seen == v) { if (v & FUTEX_WAITERS) futex_wake(uaddr, 1, true); return; }
        v = seen;
    }
}

void futex_exit_robust(uint64_t head, int tid)
{
    uint64_t h[3];                                      /* next, futex_offset, list_op_pending */
    if (!head || copy_from_user(h, (const void *)head, sizeof h)) return;
    int64_t off = (int64_t)h[1];
    uint64_t entry = h[0] & ~1ull;
    for (int n = 0; entry && entry != head && n < 2048; n++) {      /* a cycle cannot keep us here */
        uint64_t next;
        if (get_user_u64(&next, (const void *)entry)) break;
        if (entry != (h[2] & ~1ull)) robust_one(entry + (uint64_t)off, tid);
        entry = next & ~1ull;
    }
    if (h[2] & ~1ull) robust_one((h[2] & ~1ull) + (uint64_t)off, tid);
}
