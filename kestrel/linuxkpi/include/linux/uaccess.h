/* linux/uaccess.h -- Kestrel's Linux KPI: user memory (linuxkpi/src/chardev.c)
 *
 * Linux semantics: copy_*_user return the number of bytes NOT copied.
 * Kestrel hands read()/write() handlers kernel buffers, so around those
 * calls the KPI marks the thread and these become plain copies; ioctl
 * arguments are real user addresses, checked and copied by Kestrel. */
#ifndef _LKPI_LINUX_UACCESS_H
#define _LKPI_LINUX_UACCESS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/minmax.h>

unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long lkpi_clear_user(void __user *to, unsigned long n);
long lkpi_strncpy_from_user(char *dst, const char __user *src, long count);
bool lkpi_access_ok(const void __user *addr, unsigned long size);

static inline unsigned long __must_check copy_from_user(void *to, const void __user *from, unsigned long n)
{ return lkpi_copy_from_user(to, from, n); }
static inline unsigned long __must_check copy_to_user(void __user *to, const void *from, unsigned long n)
{ return lkpi_copy_to_user(to, from, n); }
#define __copy_from_user(to, from, n)   copy_from_user(to, from, n)
#define __copy_to_user(to, from, n)     copy_to_user(to, from, n)
#define raw_copy_from_user(to, from, n) copy_from_user(to, from, n)
#define raw_copy_to_user(to, from, n)   copy_to_user(to, from, n)
static inline unsigned long __must_check clear_user(void __user *to, unsigned long n) { return lkpi_clear_user(to, n); }
#define __clear_user(to, n)             clear_user(to, n)
static inline long strncpy_from_user(char *dst, const char __user *src, long count)
{ return lkpi_strncpy_from_user(dst, src, count); }
#define access_ok(addr, size)           lkpi_access_ok((addr), (size))

/* get_user(x, ptr) / put_user(x, ptr): 0 or -EFAULT */
#define get_user(x, ptr) ({ \
        __unqual_scalar_typeof(*(ptr)) __gu_val; \
        int __gu_err = copy_from_user(&__gu_val, (ptr), sizeof(__gu_val)) ? -EFAULT : 0; \
        (x) = __gu_err ? (__unqual_scalar_typeof(*(ptr)))0 : __gu_val; \
        __gu_err; })
#define put_user(x, ptr) ({ \
        typeof(*(ptr)) __pu_val = (x); \
        copy_to_user((ptr), &__pu_val, sizeof(__pu_val)) ? -EFAULT : 0; })
#define __get_user(x, ptr)              get_user(x, ptr)
#define __put_user(x, ptr)              put_user(x, ptr)
static inline int check_zeroed_user(const void __user *from, size_t size) { return -EFAULT; }
static inline int copy_struct_from_user(void *dst, size_t ksize, const void __user *src, size_t usize)
{
    size_t size = min(ksize, usize);
    if (usize < ksize) memset((char *)dst + size, 0, ksize - size);
    return copy_from_user(dst, src, size) ? -EFAULT : 0;
}
#define pagefault_disable()             do { } while (0)
#define pagefault_enable()              do { } while (0)
#define user_access_begin(p, s)         access_ok(p, s)
#define user_access_end()               do { } while (0)

#endif
