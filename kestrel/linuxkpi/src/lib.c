/* linuxkpi/src/lib.c -- Linux's lib/: strings, number parsing, bitmaps, random
 *
 * The C library basics (memcpy, strlen, strcmp ...) are Kestrel's; this
 * file adds the kernel's own string helpers, the strict kstrto*() parsers
 * and the bitmap search functions, written for the KPI. */
#include <linux/kernel.h>
#include <asm/page.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/kstrtox.h>
#include <linux/bitmap.h>
#include <linux/find.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/sysfs.h>
#include <linux/spinlock.h>
#include <linux/limits.h>
#include <linux/err.h>
#include "internal.h"

/* ---- strings ------------------------------------------------------------- */
char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (!ca) break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) { return strncasecmp(a, b, (size_t)-1); }

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

char *strnchr(const char *s, size_t n, int c)
{
    for (; n-- && *s; s++)
        if (*s == (char)c) return (char *)s;
    return NULL;
}

char *strnstr(const char *s, const char *find, size_t len)
{
    size_t l2 = strlen(find);
    if (!l2) return (char *)s;
    while (len >= l2) {
        len--;
        if (!memcmp(s, find, l2)) return (char *)s;
        s++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        if (strchr(accept, *s)) return (char *)s;
    return NULL;
}

char *strsep(char **s, const char *delim)
{
    char *begin = *s;
    if (!begin) return NULL;
    char *end = strpbrk(begin, delim);
    if (end) *end++ = 0;
    *s = end;
    return begin;
}

char *skip_spaces(const char *s)
{
    while (isspace(*s)) s++;
    return (char *)s;
}

char *strim(char *s)
{
    size_t n = strlen(s);
    if (!n) return s;
    char *end = s + n - 1;
    while (end >= s && isspace(*end)) end--;
    end[1] = 0;
    return skip_spaces(s);
}

void *memchr_inv(const void *s, int c, size_t n)
{
    const u8 *p = s;
    for (; n; n--, p++)
        if (*p != (u8)c) return (void *)p;
    return NULL;
}

bool sysfs_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return true;
    if (!*a && *b == '\n' && !b[1]) return true;
    if (*a == '\n' && !a[1] && !*b) return true;
    return false;
}

int match_string(const char *const *array, size_t n, const char *string)
{
    for (size_t i = 0; i < n; i++) {
        if (!array[i]) break;
        if (!strcmp(array[i], string)) return (int)i;
    }
    return -EINVAL;
}

char *strreplace(char *s, char old, char new)
{
    for (char *p = s; *p; p++)
        if (*p == old) *p = new;
    return s;
}

ssize_t lkpi_strscpy(char *dst, const char *src, size_t size)
{
    if (!size) return -E2BIG;
    size_t len = strnlen(src, size);
    if (len == size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = 0;
        return -E2BIG;
    }
    memcpy(dst, src, len + 1);
    return (ssize_t)len;
}

ssize_t lkpi_strscpy_pad(char *dst, const char *src, size_t size)
{
    ssize_t r = lkpi_strscpy(dst, src, size);
    size_t used = r < 0 ? size : (size_t)r + 1;
    if (used < size) memset(dst + used, 0, size - used);
    return r;
}

/* ---- allocating copies ------------------------------------------------------- */
char *kstrdup(const char *s, gfp_t gfp)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = kmalloc(n, gfp);
    if (p) memcpy(p, s, n);
    return p;
}

char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
    if (!s) return NULL;
    size_t n = strnlen(s, max);
    char *p = kmalloc(n + 1, gfp);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

/* Linux skips the copy for strings in .rodata; every string is copied here */
const char *kstrdup_const(const char *s, gfp_t gfp) { return kstrdup(s, gfp); }
void kfree_const(const void *x) { kfree(x); }

void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
    void *p = kmalloc(len, gfp);
    if (p) memcpy(p, src, len);
    return p;
}

char *kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
    char *p = kmalloc(len + 1, gfp);
    if (p) { memcpy(p, s, len); p[len] = 0; }
    return p;
}

void *memdup_user(const void __user *src, size_t len)
{
    void *p = kmalloc(len, GFP_KERNEL);
    if (!p) return ERR_PTR(-ENOMEM);
    if (copy_from_user(p, src, len)) { kfree(p); return ERR_PTR(-EFAULT); }
    return p;
}

void *memdup_user_nul(const void __user *src, size_t len)
{
    char *p = kmalloc(len + 1, GFP_KERNEL);
    if (!p) return ERR_PTR(-ENOMEM);
    if (copy_from_user(p, src, len)) { kfree(p); return ERR_PTR(-EFAULT); }
    p[len] = 0;
    return p;
}

char *strndup_user(const char __user *s, long n)
{
    char *p = kmalloc((size_t)n + 1, GFP_KERNEL);
    if (!p) return ERR_PTR(-ENOMEM);
    long len = strncpy_from_user(p, s, n);
    if (len < 0) { kfree(p); return ERR_PTR(len); }
    if (len >= n) { kfree(p); return ERR_PTR(-EINVAL); }
    return p;
}

/* ---- numbers from strings ----------------------------------------------------- */
static const char *parse_base(const char *s, unsigned int *base)
{
    if (*base == 0) {
        if (s[0] == '0') {
            if (_tolower(s[1]) == 'x' && isxdigit(s[2])) *base = 16;
            else *base = 8;
        } else {
            *base = 10;
        }
    }
    if (*base == 16 && s[0] == '0' && _tolower(s[1]) == 'x') s += 2;
    return s;
}

/* digits from s: their count (0: none), the value, overflow noted */
static size_t parse_digits(const char *s, unsigned int base, unsigned long long *res, bool *overflow)
{
    unsigned long long v = 0;
    size_t n = 0;
    *overflow = false;
    for (;; n++) {
        int c = (unsigned char)s[n];
        unsigned int d;
        if (isdigit(c)) d = (unsigned)(c - '0');
        else if (isxdigit(c)) d = (unsigned)(_tolower((char)c) - 'a' + 10);
        else break;
        if (d >= base) break;
        if (v > (ULLONG_MAX - d) / base) *overflow = true;
        v = v * base + d;
    }
    *res = v;
    return n;
}

unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base)
{
    unsigned long long v;
    bool of;
    cp = parse_base(cp, &base);
    cp += parse_digits(cp, base, &v, &of);
    if (endp) *endp = (char *)cp;
    return v;
}

long long simple_strtoll(const char *cp, char **endp, unsigned int base)
{
    if (*cp == '-') return -(long long)simple_strtoull(cp + 1, endp, base);
    return (long long)simple_strtoull(cp, endp, base);
}

int kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
    unsigned long long v;
    bool of;
    if (*s == '+') s++;
    s = parse_base(s, &base);
    size_t n = parse_digits(s, base, &v, &of);
    if (!n) return -EINVAL;
    s += n;
    if (*s == '\n') s++;
    if (*s) return -EINVAL;
    if (of) return -ERANGE;
    *res = v;
    return 0;
}

int kstrtoll(const char *s, unsigned int base, long long *res)
{
    unsigned long long v;
    int r;
    if (*s == '-') {
        if ((r = kstrtoull(s + 1, base, &v))) return r;
        if ((long long)-v > 0) return -ERANGE;
        *res = -(long long)v;
    } else {
        if ((r = kstrtoull(s, base, &v))) return r;
        if ((long long)v < 0) return -ERANGE;
        *res = (long long)v;
    }
    return 0;
}

#define KSTRTO_UNSIGNED(name, type, max) \
    int name(const char *s, unsigned int base, type *res) \
    { \
        unsigned long long v; \
        int r = kstrtoull(s, base, &v); \
        if (r) return r; \
        if (v > (max)) return -ERANGE; \
        *res = (type)v; \
        return 0; \
    }
#define KSTRTO_SIGNED(name, type, min, max) \
    int name(const char *s, unsigned int base, type *res) \
    { \
        long long v; \
        int r = kstrtoll(s, base, &v); \
        if (r) return r; \
        if (v < (min) || v > (max)) return -ERANGE; \
        *res = (type)v; \
        return 0; \
    }
KSTRTO_UNSIGNED(kstrtoul, unsigned long, ULONG_MAX)
KSTRTO_UNSIGNED(kstrtouint, unsigned int, UINT_MAX)
KSTRTO_UNSIGNED(kstrtou16, u16, USHRT_MAX)
KSTRTO_UNSIGNED(kstrtou8, u8, UCHAR_MAX)
KSTRTO_SIGNED(kstrtol, long, LONG_MIN, LONG_MAX)
KSTRTO_SIGNED(kstrtoint, int, INT_MIN, INT_MAX)
KSTRTO_SIGNED(kstrtos16, s16, SHRT_MIN, SHRT_MAX)
KSTRTO_SIGNED(kstrtos8, s8, SCHAR_MIN, SCHAR_MAX)

/* y/Y/1/on and n/N/0/off */
int kstrtobool(const char *s, bool *res)
{
    if (!s) return -EINVAL;
    switch (s[0]) {
    case 'y': case 'Y': case '1': *res = true; return 0;
    case 'n': case 'N': case '0': *res = false; return 0;
    case 'o': case 'O':
        if (_tolower(s[1]) == 'n') { *res = true; return 0; }
        if (_tolower(s[1]) == 'f') { *res = false; return 0; }
        break;
    }
    return -EINVAL;
}

static int from_user(char *buf, size_t size, const char __user *s, size_t count)
{
    if (count >= size) return -EINVAL;
    if (copy_from_user(buf, s, count)) return -EFAULT;
    buf[count] = 0;
    return 0;
}

int kstrtouint_from_user(const char __user *s, size_t count, unsigned int base, unsigned int *res)
{
    char buf[24];
    int r = from_user(buf, sizeof buf, s, count);
    return r ? r : kstrtouint(buf, base, res);
}

int kstrtoint_from_user(const char __user *s, size_t count, unsigned int base, int *res)
{
    char buf[24];
    int r = from_user(buf, sizeof buf, s, count);
    return r ? r : kstrtoint(buf, base, res);
}

int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
    char buf[4] = { 0 };
    if (copy_from_user(buf, s, min(count, sizeof buf - 1))) return -EFAULT;
    return kstrtobool(buf, res);
}

/* ---- sysfs buffers (one page) ------------------------------------------------- */
int sysfs_emit(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vscnprintf(buf, PAGE_SIZE, fmt, ap);
    va_end(ap);
    return n;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
    if (at < 0 || at >= (int)PAGE_SIZE) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vscnprintf(buf + at, PAGE_SIZE - at, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- bitmaps --------------------------------------------------------------------- */
static unsigned long find_bit(const unsigned long *addr, unsigned long size, unsigned long offset, unsigned long invert)
{
    if (offset >= size) return size;
    unsigned long w = (addr[BIT_WORD(offset)] ^ invert) & BITMAP_FIRST_WORD_MASK(offset);
    offset &= ~(unsigned long)(BITS_PER_LONG - 1);
    while (!w) {
        offset += BITS_PER_LONG;
        if (offset >= size) return size;
        w = addr[BIT_WORD(offset)] ^ invert;
    }
    unsigned long r = offset + __builtin_ctzl(w);
    return r < size ? r : size;
}

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{ return find_bit(addr, size, offset, 0); }

unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{ return find_bit(addr, size, offset, ~0UL); }

unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
    for (unsigned long i = size; i-- > 0;)
        if (addr[BIT_WORD(i)] & BIT_MASK(i)) return i;
    return size;
}

void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
    for (unsigned int i = start; i < start + nbits; i++) map[BIT_WORD(i)] |= BIT_MASK(i);
}

void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
    for (unsigned int i = start; i < start + nbits; i++) map[BIT_WORD(i)] &= ~BIT_MASK(i);
}

bool bitmap_empty(const unsigned long *src, unsigned int nbits) { return find_first_bit(src, nbits) >= nbits; }
bool bitmap_full(const unsigned long *src, unsigned int nbits) { return find_first_zero_bit(src, nbits) >= nbits; }

unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
    unsigned int w = 0, k;
    for (k = 0; k < nbits / BITS_PER_LONG; k++) w += (unsigned)hweight_long(src[k]);
    if (nbits % BITS_PER_LONG) w += (unsigned)hweight_long(src[k] & BITMAP_LAST_WORD_MASK(nbits));
    return w;
}

bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    unsigned int k;
    for (k = 0; k < nbits / BITS_PER_LONG; k++)
        if (a[k] != b[k]) return false;
    if (nbits % BITS_PER_LONG) return !((a[k] ^ b[k]) & BITMAP_LAST_WORD_MASK(nbits));
    return true;
}

void bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = a[k] & b[k];
}

void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = a[k] | b[k];
}

void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = ~src[k];
}

unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size, unsigned long start,
                                         unsigned int nr, unsigned long align_mask)
{
    for (;;) {
        unsigned long index = (find_next_zero_bit(map, size, start) + align_mask) & ~align_mask;
        unsigned long end = index + nr;
        if (end > size) return end;
        unsigned long i = find_next_bit(map, end, index);
        if (i >= end) return index;
        start = i + 1;
    }
}

int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order)
{
    unsigned int n = 1u << order;
    for (unsigned int pos = 0; pos + n <= bits; pos += n) {
        if (find_next_bit(bitmap, pos + n, pos) >= pos + n) {
            bitmap_set(bitmap, pos, n);
            return (int)pos;
        }
    }
    return -ENOMEM;
}

void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order)
{
    bitmap_clear(bitmap, pos, 1u << order);
}

unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)
{ return kmalloc_array(BITS_TO_LONGS(nbits), sizeof(unsigned long), flags); }
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)
{ return bitmap_alloc(nbits, flags | __GFP_ZERO); }
void bitmap_free(const unsigned long *bitmap) { kfree(bitmap); }

/* ---- random numbers (xorshift64*, seeded from the clock) ------------------------------ */
static u64 rnd_state;
static DEFINE_SPINLOCK(rnd_lock);

u64 get_random_u64(void)
{
    unsigned long flags;
    spin_lock_irqsave(&rnd_lock, flags);
    if (!rnd_state) rnd_state = lkpi_time_ns() * 0x9E3779B97F4A7C15ull | 1;
    u64 x = rnd_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rnd_state = x;
    spin_unlock_irqrestore(&rnd_lock, flags);
    return x * 0x2545F4914F6CDD1Dull;
}

u32 get_random_u32(void) { return (u32)(get_random_u64() >> 32); }

void get_random_bytes(void *buf, size_t len)
{
    u8 *p = buf;
    while (len) {
        u64 v = get_random_u64();
        size_t n = len < sizeof v ? len : sizeof v;
        memcpy(p, &v, n);
        p += n;
        len -= n;
    }
}
