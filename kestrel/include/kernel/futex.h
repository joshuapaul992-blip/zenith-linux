/* include/kernel/futex.h -- fast user-space mutexes (kernel/futex.c) */
#ifndef KESTREL_FUTEX_H
#define KESTREL_FUTEX_H

#include <stdint.h>
#include <stdbool.h>

int64_t sys_futex_call(uint64_t uaddr, int op, uint32_t val, uint64_t utime, uint64_t uaddr2, uint32_t val3);
/* Wake up to n waiters on the futex at uaddr (thread exit: clear_child_tid). */
int     futex_wake(uint64_t uaddr, int n, bool shared);
/* Robust futexes of an exiting thread (set_robust_list). */
void    futex_exit_robust(uint64_t head, int tid);

#endif
