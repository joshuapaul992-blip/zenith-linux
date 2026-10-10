/* include/kernel/uaccess.h -- the only way the kernel touches user memory
 *
 * Every pointer a user process hands to a system call is hostile until
 * proven otherwise: it may point at kernel memory, be unmapped, be unmapped
 * by another thread a moment after it was checked, or straddle the end of
 * the user half. So system calls never dereference user pointers directly.
 * They copy arguments in and results out with the functions below:
 *
 *   - the range [addr, addr+len) must lie inside the user half
 *     (UVM_USER_START .. UVM_USER_END) without wrapping around;
 *   - the copy itself runs in arch/x86_64/uaccess.asm, whose faulting
 *     instructions are listed in the exception table. A page fault there is
 *     first offered to the memory manager (demand paging, copy-on-write);
 *     if it cannot be resolved, execution resumes at a fixup that makes the
 *     call fail with -EFAULT. There is no window between "checked" and
 *     "used" for another thread to exploit (no TOCTOU), and a bad pointer
 *     never becomes a kernel crash.
 *
 * Kernel threads (no user address space) make system calls through
 * int 0x80 with kernel buffers; for them these functions are plain copies.
 *
 * The _nofault variants never run the page-fault resolver: for callers that
 * hold a spinlock or must not sleep (futexes, blits under the TTY lock). A
 * page that is not present then simply fails with -EFAULT.
 */
#ifndef KESTREL_UACCESS_H
#define KESTREL_UACCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* [addr, addr+len) inside the user half, no wrap-around (len 0 is fine) */
bool user_range_ok(uint64_t addr, size_t len);

int  copy_from_user(void *dst, const void *usrc, size_t n);     /* 0 or -EFAULT */
int  copy_to_user(void *udst, const void *src, size_t n);       /* 0 or -EFAULT */
int  clear_user(void *udst, size_t n);                          /* 0 or -EFAULT */
/* A NUL-terminated string into dst[cap]: its length, -EFAULT, or
 * -ENAMETOOLONG when no NUL is found in the first cap-1 bytes. */
long strncpy_from_user(char *dst, const char *usrc, size_t cap);

int  get_user_u32(uint32_t *out, const void *uaddr);            /* 0 or -EFAULT */
int  get_user_u64(uint64_t *out, const void *uaddr);
int  put_user_u32(void *uaddr, uint32_t v);
int  put_user_u64(void *uaddr, uint64_t v);
/* Atomic compare-and-exchange of a user word; *cur = value found. */
int  user_cmpxchg_u32(void *uaddr, uint32_t old, uint32_t nw, uint32_t *cur);

/* No page-fault resolution inside (see above). Nests. */
void pagefault_disable(void);
void pagefault_enable(void);
bool pagefault_disabled(void);
int  copy_from_user_nofault(void *dst, const void *usrc, size_t n);
int  get_user_u32_nofault(uint32_t *out, const void *uaddr);

/* Exception table: the fixup address for a faulting kernel RIP, or 0. */
uint64_t extable_fixup(uint64_t rip);

#endif
