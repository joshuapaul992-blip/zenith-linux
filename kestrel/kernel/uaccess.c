/* kernel/uaccess.c -- checked, fault-tolerant user memory access (see uaccess.h) */
#include <kernel/uaccess.h>
#include <kernel/task.h>
#include <kernel/uvm.h>
#include <kernel/posix.h>
#include <kernel/string.h>

size_t  uaccess_copy(void *dst, const void *src, size_t n);
size_t  uaccess_clear(void *dst, size_t n);
int64_t uaccess_strncpy(char *dst, const char *usrc, size_t max);
int     uaccess_get32(const void *uaddr, uint32_t *out);
int     uaccess_get64(const void *uaddr, uint64_t *out);
int     uaccess_put32(void *uaddr, uint32_t v);
int     uaccess_put64(void *uaddr, uint64_t v);
int     uaccess_cmpxchg32(void *uaddr, uint32_t old, uint32_t nw, uint32_t *cur);

struct extable_entry { uint64_t insn, fixup; };
extern const struct extable_entry __ex_table_start[], __ex_table_end[];

uint64_t extable_fixup(uint64_t rip)
{
    for (const struct extable_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (e->insn == rip) return e->fixup;
    return 0;
}

bool user_range_ok(uint64_t addr, size_t len)
{
    return addr >= UVM_USER_START && addr <= UVM_USER_END && len <= UVM_USER_END - addr;
}

/* Kernel threads pass kernel buffers through int 0x80. */
static bool kernel_caller(void)
{
    struct tcb *t = current_task();
    return !t || !t->user;
}

int copy_from_user(void *dst, const void *usrc, size_t n)
{
    if (!n) return 0;
    if (kernel_caller()) { memcpy(dst, usrc, n); return 0; }
    if (!user_range_ok((uint64_t)usrc, n)) return -EFAULT;
    return uaccess_copy(dst, usrc, n) ? -EFAULT : 0;
}

int copy_to_user(void *udst, const void *src, size_t n)
{
    if (!n) return 0;
    if (kernel_caller()) { memcpy(udst, src, n); return 0; }
    if (!user_range_ok((uint64_t)udst, n)) return -EFAULT;
    return uaccess_copy(udst, src, n) ? -EFAULT : 0;
}

int clear_user(void *udst, size_t n)
{
    if (!n) return 0;
    if (kernel_caller()) { memset(udst, 0, n); return 0; }
    if (!user_range_ok((uint64_t)udst, n)) return -EFAULT;
    return uaccess_clear(udst, n) ? -EFAULT : 0;
}

long strncpy_from_user(char *dst, const char *usrc, size_t cap)
{
    if (!cap) return -ENAMETOOLONG;
    if (kernel_caller()) {
        size_t n = strnlen(usrc, cap);
        if (n == cap) return -ENAMETOOLONG;
        memcpy(dst, usrc, n + 1);
        return (long)n;
    }
    uint64_t a = (uint64_t)usrc;
    if (!user_range_ok(a, 1)) return -EFAULT;
    size_t avail = UVM_USER_END - a;                    /* never read past the user half */
    size_t lim = cap < avail ? cap : avail;
    int64_t n = uaccess_strncpy(dst, usrc, lim);        /* NUL index, or lim if none */
    if (n < 0) return -EFAULT;
    if ((size_t)n == lim) {
        dst[lim - 1] = 0;
        return lim < cap ? -EFAULT : -ENAMETOOLONG;     /* ran off the user half / too long */
    }
    return (long)n;
}

int get_user_u32(uint32_t *out, const void *uaddr)
{
    if (kernel_caller()) { memcpy(out, uaddr, 4); return 0; }
    if (!user_range_ok((uint64_t)uaddr, 4)) return -EFAULT;
    return uaccess_get32(uaddr, out) ? -EFAULT : 0;
}

int get_user_u64(uint64_t *out, const void *uaddr)
{
    if (kernel_caller()) { memcpy(out, uaddr, 8); return 0; }
    if (!user_range_ok((uint64_t)uaddr, 8)) return -EFAULT;
    return uaccess_get64(uaddr, out) ? -EFAULT : 0;
}

int put_user_u32(void *uaddr, uint32_t v)
{
    if (kernel_caller()) { memcpy(uaddr, &v, 4); return 0; }
    if (!user_range_ok((uint64_t)uaddr, 4)) return -EFAULT;
    return uaccess_put32(uaddr, v) ? -EFAULT : 0;
}

int put_user_u64(void *uaddr, uint64_t v)
{
    if (kernel_caller()) { memcpy(uaddr, &v, 8); return 0; }
    if (!user_range_ok((uint64_t)uaddr, 8)) return -EFAULT;
    return uaccess_put64(uaddr, v) ? -EFAULT : 0;
}

int user_cmpxchg_u32(void *uaddr, uint32_t old, uint32_t nw, uint32_t *cur)
{
    if ((uint64_t)uaddr & 3) return -EINVAL;            /* lock cmpxchg must not split */
    if (kernel_caller()) {
        *cur = __sync_val_compare_and_swap((uint32_t *)uaddr, old, nw);
        return 0;
    }
    if (!user_range_ok((uint64_t)uaddr, 4)) return -EFAULT;
    return uaccess_cmpxchg32(uaddr, old, nw, cur) ? -EFAULT : 0;
}

void pagefault_disable(void) { struct tcb *t = current_task(); if (t) t->pagefault_off++; }
void pagefault_enable(void)  { struct tcb *t = current_task(); if (t && t->pagefault_off) t->pagefault_off--; }
bool pagefault_disabled(void) { struct tcb *t = current_task(); return t && t->pagefault_off; }

int copy_from_user_nofault(void *dst, const void *usrc, size_t n)
{
    pagefault_disable();
    int r = copy_from_user(dst, usrc, n);
    pagefault_enable();
    return r;
}

int get_user_u32_nofault(uint32_t *out, const void *uaddr)
{
    pagefault_disable();
    int r = get_user_u32(out, uaddr);
    pagefault_enable();
    return r;
}
