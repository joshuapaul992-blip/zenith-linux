/* linux/string.h -- Kestrel's Linux KPI: memory and string functions
 *
 * The C library basics are Kestrel's own (lib/string.c, same semantics);
 * the kernel additions are implemented in linuxkpi/src/lib.c. */
#ifndef _LKPI_LINUX_STRING_H
#define _LKPI_LINUX_STRING_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/stdarg.h>
#include <linux/err.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

/* kernel additions (lib.c) */
#define strcpy          lkpi_strcpy
#define strncpy         lkpi_strncpy
#define strcat          lkpi_strcat
#define strncat         lkpi_strncat
#define strcasecmp      lkpi_strcasecmp
#define strncasecmp     lkpi_strncasecmp
#define strchrnul       lkpi_strchrnul
#define strnchr         lkpi_strnchr
#define strnstr         lkpi_strnstr
#define strpbrk         lkpi_strpbrk
#define strsep          lkpi_strsep
#define strspn          lkpi_strspn
#define strcspn         lkpi_strcspn
#define strim           lkpi_strim
#define skip_spaces     lkpi_skip_spaces
#define memchr_inv      lkpi_memchr_inv
#define sysfs_streq     lkpi_sysfs_streq
#define match_string    lkpi_match_string
#define strreplace      lkpi_strreplace
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strchrnul(const char *s, int c);
char  *strnchr(const char *s, size_t n, int c);
char  *strnstr(const char *s, const char *find, size_t len);
char  *strpbrk(const char *s, const char *accept);
char  *strsep(char **s, const char *delim);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strim(char *s);
char  *skip_spaces(const char *s);
void  *memchr_inv(const void *s, int c, size_t n);   /* first byte != c, or NULL */
bool   sysfs_streq(const char *a, const char *b);   /* equal, ignoring one trailing newline */
int    match_string(const char *const *array, size_t n, const char *string);
char  *strreplace(char *s, char old, char new);
/* copy with NUL termination: the length, or -E2BIG when truncated */
ssize_t lkpi_strscpy(char *dst, const char *src, size_t size);
ssize_t lkpi_strscpy_pad(char *dst, const char *src, size_t size);
#define __lkpi_strscpy2(d, s)           lkpi_strscpy(d, s, sizeof(d) + __must_be_array(d))
#define __lkpi_strscpy3(d, s, n)        lkpi_strscpy(d, s, n)
#define __lkpi_strscpy_pick(_1, _2, _3, NAME, ...) NAME
#define strscpy(...)    __lkpi_strscpy_pick(__VA_ARGS__, __lkpi_strscpy3, __lkpi_strscpy2)(__VA_ARGS__)
#define __lkpi_strscpy_pad2(d, s)       lkpi_strscpy_pad(d, s, sizeof(d) + __must_be_array(d))
#define __lkpi_strscpy_pad3(d, s, n)    lkpi_strscpy_pad(d, s, n)
#define strscpy_pad(...) __lkpi_strscpy_pick(__VA_ARGS__, __lkpi_strscpy_pad3, __lkpi_strscpy_pad2)(__VA_ARGS__)

static inline bool strstarts(const char *str, const char *prefix) { return strncmp(str, prefix, strlen(prefix)) == 0; }
static inline size_t str_has_prefix(const char *str, const char *prefix)
{ size_t len = strlen(prefix); return strncmp(str, prefix, len) == 0 ? len : 0; }
static inline void memzero_explicit(void *s, size_t count) { memset(s, 0, count); barrier_data(s); }
static inline const char *kbasename(const char *path) { const char *t = strrchr(path, '/'); return t ? t + 1 : path; }
static inline void *memset32(u32 *s, u32 v, size_t count) { for (size_t i = 0; i < count; i++) s[i] = v; return s; }
static inline void *memset64(u64 *s, u64 v, size_t count) { for (size_t i = 0; i < count; i++) s[i] = v; return s; }
static inline const char *str_yes_no(bool v) { return v ? "yes" : "no"; }
static inline const char *str_on_off(bool v) { return v ? "on" : "off"; }
static inline const char *str_enable_disable(bool v) { return v ? "enable" : "disable"; }
static inline const char *str_enabled_disabled(bool v) { return v ? "enabled" : "disabled"; }
static inline const char *str_true_false(bool v) { return v ? "true" : "false"; }
#define memcpy_and_pad(dest, dest_len, src, count, pad) do { \
        size_t __dl = (dest_len), __c = (count); \
        if (__dl > __c) { memcpy(dest, src, __c); memset((char *)(dest) + __c, pad, __dl - __c); } \
        else memcpy(dest, src, __dl); } while (0)
#define unsafe_memcpy(dst, src, bytes, justification) memcpy(dst, src, bytes)

/* allocating (slab.h's allocator) */
#define kstrdup         lkpi_kstrdup
char *kstrdup(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t max, gfp_t gfp);
const char *kstrdup_const(const char *s, gfp_t gfp);
void kfree_const(const void *x);
void *kmemdup(const void *src, size_t len, gfp_t gfp);
char *kmemdup_nul(const char *s, size_t len, gfp_t gfp);
void *memdup_user(const void __user *src, size_t len);
char *strndup_user(const char __user *s, long n);
void *memdup_user_nul(const void __user *src, size_t len);

#endif
