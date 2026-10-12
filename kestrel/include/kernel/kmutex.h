/* include/kernel/kmutex.h -- a sleeping, recursive mutex, and a sleeping
 * reader-writer lock (proc/task.c) */
#ifndef KESTREL_KMUTEX_H
#define KESTREL_KMUTEX_H

#include <stdbool.h>

struct tcb;

/* The owner may lock it again (a page fault inside a memory-management
 * call); others sleep until it is free. Zeroed memory is an unlocked mutex.
 * Safe with or without the big kernel lock. */
struct kmutex {
    struct tcb *owner;
    int         depth;
    int         waiters;            /* asleep in kmutex_lock: unlock wakes them */
};
void        kmutex_lock(struct kmutex *m);
void        kmutex_unlock(struct kmutex *m);
bool        kmutex_held(const struct kmutex *m);   /* by the calling task */

/* Many readers or one writer; a waiting writer holds new readers back, so
 * a stream of readers cannot starve it. Not recursive: a task that may
 * take it again keeps its own count (fs/vfs.c: the file tree's lock).
 * Waiting sleeps, which gives up the big kernel lock. Zeroed memory is an
 * unlocked lock. */
struct krwlock {
    int         readers;
    struct tcb *writer;
    int         wwait;              /* writers waiting                       */
    int         waiters;            /* asleep, readers and writers           */
};
void        krw_rlock(struct krwlock *l);
void        krw_runlock(struct krwlock *l);
void        krw_wlock(struct krwlock *l);
void        krw_wunlock(struct krwlock *l);

#endif
