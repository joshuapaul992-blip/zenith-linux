/* include/kernel/kmutex.h -- a sleeping, recursive mutex (proc/task.c) */
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

#endif
