/* linux/kstrtox.h -- Kestrel's Linux KPI: strict string to number (linuxkpi/src/lib.c)
 *
 * The whole string must be the number, optionally followed by one newline;
 * 0, -EINVAL or -ERANGE. Base 0 accepts 0x.., 0.. (octal) and decimal. */
#ifndef _LKPI_LINUX_KSTRTOX_H
#define _LKPI_LINUX_KSTRTOX_H

#include <linux/compiler.h>
#include <linux/types.h>

int __must_check kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int __must_check kstrtoll(const char *s, unsigned int base, long long *res);
int __must_check kstrtoul(const char *s, unsigned int base, unsigned long *res);
int __must_check kstrtol(const char *s, unsigned int base, long *res);
int __must_check kstrtouint(const char *s, unsigned int base, unsigned int *res);
int __must_check kstrtoint(const char *s, unsigned int base, int *res);
int __must_check kstrtou16(const char *s, unsigned int base, u16 *res);
int __must_check kstrtos16(const char *s, unsigned int base, s16 *res);
int __must_check kstrtou8(const char *s, unsigned int base, u8 *res);
int __must_check kstrtos8(const char *s, unsigned int base, s8 *res);
int __must_check kstrtobool(const char *s, bool *res);
static inline int __must_check kstrtou64(const char *s, unsigned int base, u64 *res) { return kstrtoull(s, base, res); }
static inline int __must_check kstrtos64(const char *s, unsigned int base, s64 *res) { return kstrtoll(s, base, res); }
static inline int __must_check kstrtou32(const char *s, unsigned int base, u32 *res) { return kstrtouint(s, base, res); }
static inline int __must_check kstrtos32(const char *s, unsigned int base, s32 *res) { return kstrtoint(s, base, res); }
int __must_check kstrtouint_from_user(const char __user *s, size_t count, unsigned int base, unsigned int *res);
int __must_check kstrtoint_from_user(const char __user *s, size_t count, unsigned int base, int *res);
int __must_check kstrtobool_from_user(const char __user *s, size_t count, bool *res);

/* the old, lenient ones: stop at the first non-digit */
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base);
long long simple_strtoll(const char *cp, char **endp, unsigned int base);
static inline unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base)
{ return simple_strtoull(cp, endp, base); }
static inline long simple_strtol(const char *cp, char **endp, unsigned int base)
{ return simple_strtoll(cp, endp, base); }

#endif
