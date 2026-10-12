/* linuxkpi/src/vsprintf.c -- Linux's printf and scanf formats
 *
 * The C conversions with Linux's length modifiers, and the %p extensions
 * drivers use in messages: %pe (error names), %pV (nested format),
 * %pM/%pm (MAC), %pI4/%pi4/%pI6, %pa/%pad (physical and DMA addresses),
 * %ph (hex bytes), %pR/%pr (resources), %pU (UUIDs). Symbol lookups
 * (%pS and friends) print the address. Plain %p prints the address
 * itself: Kestrel does not hash pointers. As in Linux, every letter or
 * digit after %p belongs to the extension. */
#include <linux/kernel.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>

#define F_LEFT      0x01
#define F_PLUS      0x02
#define F_SPACE     0x04
#define F_SPECIAL   0x08
#define F_ZEROPAD   0x10
#define F_SIGN      0x20
#define F_UPPER     0x40

struct out {
    char   *buf;
    size_t  size;
    size_t  pos;                    /* counts everything, even what did not fit */
};

struct spec {
    int flags;
    int width;                      /* -1: none */
    int prec;                       /* -1: none */
    int base;
};

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void puts_n(struct out *o, const char *s, size_t n)
{
    while (n--) put(o, *s++);
}

static void pad(struct out *o, int n, char c)
{
    while (n-- > 0) put(o, c);
}

static void number(struct out *o, unsigned long long num, struct spec s)
{
    char tmp[66];
    const char *digits = (s.flags & F_UPPER) ? hex_asc_upper : hex_asc;
    char sign = 0;
    const char *prefix = "";
    int len = 0;

    if (s.flags & F_SIGN) {
        if ((long long)num < 0) { sign = '-'; num = -(long long)num; }
        else if (s.flags & F_PLUS) sign = '+';
        else if (s.flags & F_SPACE) sign = ' ';
    }
    if (s.flags & F_SPECIAL) {
        if (s.base == 16) prefix = (s.flags & F_UPPER) ? "0X" : "0x";
        else if (s.base == 8) prefix = "0";
    }
    do {
        tmp[len++] = digits[num % (unsigned)s.base];
        num /= (unsigned)s.base;
    } while (num);

    int zeros = s.prec > len ? s.prec - len : 0;
    int total = len + zeros + (sign ? 1 : 0) + (int)strlen(prefix);
    int fill = s.width > total ? s.width - total : 0;

    if (s.prec >= 0 || (s.flags & F_LEFT)) s.flags &= ~F_ZEROPAD;     /* as in C */
    if (!(s.flags & (F_LEFT | F_ZEROPAD))) pad(o, fill, ' ');
    if (sign) put(o, sign);
    puts_n(o, prefix, strlen(prefix));
    if (s.flags & F_ZEROPAD) pad(o, fill, '0');
    pad(o, zeros, '0');
    while (len) put(o, tmp[--len]);
    if (s.flags & F_LEFT) pad(o, fill, ' ');
}

static void string(struct out *o, const char *s, struct spec sp)
{
    if (!s) s = "(null)";
    size_t len = strnlen(s, sp.prec >= 0 ? (size_t)sp.prec : (size_t)-1);
    int fill = sp.width > (int)len ? sp.width - (int)len : 0;
    if (!(sp.flags & F_LEFT)) pad(o, fill, ' ');
    puts_n(o, s, len);
    if (sp.flags & F_LEFT) pad(o, fill, ' ');
}

static void hex_ptr(struct out *o, unsigned long long v, struct spec s)
{
    s.base = 16;
    s.flags |= F_ZEROPAD;
    s.flags &= ~F_SIGN;
    if (s.width < 0) s.width = 2 * (int)sizeof(void *);
    number(o, v, s);
}

/* ---- error names for %pe ------------------------------------------------- */
#define E(x) { x, #x }
static const struct { int err; const char *name; } errnames[] = {
    E(EPERM), E(ENOENT), E(ESRCH), E(EINTR), E(EIO), E(ENXIO), E(E2BIG), E(ENOEXEC), E(EBADF),
    E(ECHILD), E(EAGAIN), E(ENOMEM), E(EACCES), E(EFAULT), E(ENOTBLK), E(EBUSY), E(EEXIST),
    E(EXDEV), E(ENODEV), E(ENOTDIR), E(EISDIR), E(EINVAL), E(ENFILE), E(EMFILE), E(ENOTTY),
    E(ETXTBSY), E(EFBIG), E(ENOSPC), E(ESPIPE), E(EROFS), E(EMLINK), E(EPIPE), E(EDOM), E(ERANGE),
    E(EDEADLK), E(ENAMETOOLONG), E(ENOLCK), E(ENOSYS), E(ENOTEMPTY), E(ELOOP), E(ENOMSG),
    E(EIDRM), E(ENODATA), E(ETIME), E(EPROTO), E(EBADMSG), E(EOVERFLOW), E(EILSEQ),
    E(EOPNOTSUPP), E(EADDRINUSE), E(EADDRNOTAVAIL), E(ENETDOWN), E(ENETUNREACH), E(ECONNRESET),
    E(ENOBUFS), E(ENOTCONN), E(ETIMEDOUT), E(ECONNREFUSED), E(EHOSTUNREACH), E(EALREADY),
    E(EINPROGRESS), E(EREMOTEIO), E(ENOMEDIUM), E(ECANCELED), E(ENOKEY), E(ERESTARTSYS),
    E(ERESTARTNOINTR), E(ERESTARTNOHAND), E(ENOIOCTLCMD), E(EPROBE_DEFER), E(ENOTSUPP),
};
#undef E

static const char *errname(int err)
{
    for (size_t i = 0; i < ARRAY_SIZE(errnames); i++)
        if (errnames[i].err == err) return errnames[i].name;
    return NULL;
}

/* ---- the %p extensions ------------------------------------------------------ */
static int do_format(struct out *o, const char *fmt, va_list *ap);

static void pointer(struct out *o, const char *ext, void *ptr, struct spec s)
{
    char buf[64];
    const u8 *b = ptr;

    switch (ext[0]) {
    case 'e':
        if (IS_ERR(ptr)) {
            const char *name = errname((int)-PTR_ERR(ptr));
            if (name) { put(o, '-'); string(o, name, s); }
            else { s.base = 10; s.flags |= F_SIGN; number(o, (unsigned long long)PTR_ERR(ptr), s); }
            return;
        }
        break;
    case 'V': {
        struct va_format *vaf = ptr;
        va_list copy;
        va_copy(copy, *vaf->va);
        do_format(o, vaf->fmt, &copy);
        va_end(copy);
        return;
    }
    case 'M': case 'm':
        if (!ptr) break;
        for (int i = 0; i < 6; i++) {
            if (i && ext[0] == 'M') put(o, ':');
            put(o, hex_asc[b[i] >> 4]);
            put(o, hex_asc[b[i] & 15]);
        }
        return;
    case 'I': case 'i':
        if (!ptr) break;
        if (ext[1] == '4') {
            scnprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            string(o, buf, s);
            return;
        }
        if (ext[1] == '6') {
            for (int i = 0; i < 16; i += 2) {
                if (i && ext[0] == 'I') put(o, ':');
                for (int k = 0; k < 2; k++) { put(o, hex_asc[b[i + k] >> 4]); put(o, hex_asc[b[i + k] & 15]); }
            }
            return;
        }
        break;
    case 'a':
        if (!ptr) break;
        s.flags |= F_SPECIAL;
        s.width = -1;
        if (ext[1] == 'd') { s.width = 2 + 2 * (int)sizeof(dma_addr_t); hex_ptr(o, *(dma_addr_t *)ptr, s); }
        else { s.width = 2 + 2 * (int)sizeof(phys_addr_t); hex_ptr(o, *(phys_addr_t *)ptr, s); }
        return;
    case 'h': {
        if (!ptr) break;
        char sep = ext[1] == 'C' ? ':' : ext[1] == 'D' ? '-' : ext[1] == 'N' ? 0 : ' ';
        int n = s.width < 0 ? 1 : s.width > 64 ? 64 : s.width;
        for (int i = 0; i < n; i++) {
            if (i && sep) put(o, sep);
            put(o, hex_asc[b[i] >> 4]);
            put(o, hex_asc[b[i] & 15]);
        }
        return;
    }
    case 'R': case 'r': {
        const struct resource *r = ptr;
        if (!r) break;
        const char *type = (r->flags & IORESOURCE_IO) ? "io " : (r->flags & IORESOURCE_MEM) ? "mem" :
                           (r->flags & IORESOURCE_IRQ) ? "irq" : (r->flags & IORESOURCE_BUS) ? "bus" : "???";
        int n = scnprintf(buf, sizeof buf, "[%s %#010llx-%#010llx", type,
                          (unsigned long long)r->start, (unsigned long long)r->end);
        if (ext[0] == 'R' && (r->flags & IORESOURCE_MEM_64)) n += scnprintf(buf + n, sizeof buf - n, " 64bit");
        if (ext[0] == 'R' && (r->flags & IORESOURCE_PREFETCH)) n += scnprintf(buf + n, sizeof buf - n, " pref");
        scnprintf(buf + n, sizeof buf - n, "]");
        string(o, buf, s);
        return;
    }
    case 'U': {
        if (!ptr) break;
        /* 'b'/'B': in byte order; 'l'/'L': the first three fields little endian */
        static const u8 be[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        static const u8 le[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
        const u8 *idx = (ext[1] == 'l' || ext[1] == 'L') ? le : be;
        const char *digits = (ext[1] == 'B' || ext[1] == 'L') ? hex_asc_upper : hex_asc;
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) put(o, '-');
            put(o, digits[b[idx[i]] >> 4]);
            put(o, digits[b[idx[i]] & 15]);
        }
        return;
    }
    case 'S': case 's': case 'B': case 'F': case 'f':
        s.flags |= F_SPECIAL;
        s.width = -1;
        s.base = 16;
        number(o, (unsigned long)ptr, s);
        return;
    }
    if (!ptr && ext[0] != 'x' && ext[0] != 'K') { string(o, "(null)", s); return; }
    hex_ptr(o, (unsigned long)ptr, s);
}

/* ---- the formatter ---------------------------------------------------------- */
static int do_format(struct out *o, const char *fmt, va_list *ap)
{
    size_t start = o->pos;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(o, *fmt); continue; }
        const char *conv = fmt++;
        struct spec s = { .flags = 0, .width = -1, .prec = -1, .base = 10 };

        for (;; fmt++) {
            if (*fmt == '-') s.flags |= F_LEFT;
            else if (*fmt == '+') s.flags |= F_PLUS;
            else if (*fmt == ' ') s.flags |= F_SPACE;
            else if (*fmt == '#') s.flags |= F_SPECIAL;
            else if (*fmt == '0') s.flags |= F_ZEROPAD;
            else break;
        }
        if (*fmt == '*') {
            s.width = va_arg(*ap, int);
            if (s.width < 0) { s.flags |= F_LEFT; s.width = -s.width; }
            fmt++;
        } else if (isdigit(*fmt)) {
            s.width = 0;
            while (isdigit(*fmt)) s.width = s.width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            s.prec = 0;
            if (*fmt == '*') { s.prec = va_arg(*ap, int); if (s.prec < 0) s.prec = -1; fmt++; }
            else while (isdigit(*fmt)) s.prec = s.prec * 10 + (*fmt++ - '0');
        }

        int qual = 0;                       /* 'H' hh, 'h', 'l', 'L' ll, 'z' */
        if (*fmt == 'h') { qual = 'h'; fmt++; if (*fmt == 'h') { qual = 'H'; fmt++; } }
        else if (*fmt == 'l') { qual = 'l'; fmt++; if (*fmt == 'l') { qual = 'L'; fmt++; } }
        else if (*fmt == 'L' || *fmt == 'q' || *fmt == 'j') { qual = 'L'; fmt++; }
        else if (*fmt == 'z' || *fmt == 'Z' || *fmt == 't') { qual = 'z'; fmt++; }

        unsigned long long num;
        switch (*fmt) {
        case '%':
            put(o, '%');
            continue;
        case 'c': {
            char c = (char)va_arg(*ap, int);
            if (!(s.flags & F_LEFT)) pad(o, s.width - 1, ' ');
            put(o, c);
            if (s.flags & F_LEFT) pad(o, s.width - 1, ' ');
            continue;
        }
        case 's':
            string(o, va_arg(*ap, const char *), s);
            continue;
        case 'p': {
            void *ptr = va_arg(*ap, void *);
            pointer(o, fmt + 1, ptr, s);
            while (isalnum(fmt[1])) fmt++;
            continue;
        }
        case 'n':                           /* not supported, as in Linux */
            (void)va_arg(*ap, void *);
            continue;
        case 'd': case 'i':
            s.flags |= F_SIGN;
            break;
        case 'u':
            break;
        case 'x':
            s.base = 16;
            break;
        case 'X':
            s.base = 16;
            s.flags |= F_UPPER;
            break;
        case 'o':
            s.base = 8;
            break;
        default:                            /* not a conversion: print it as written */
            puts_n(o, conv, (size_t)(fmt - conv) + (*fmt ? 1 : 0));
            if (!*fmt) fmt--;
            continue;
        }
        if (s.flags & F_LEFT) s.flags &= ~F_ZEROPAD;
        if (s.flags & F_SIGN) {
            switch (qual) {
            case 'H': num = (unsigned long long)(signed char)va_arg(*ap, int); break;
            case 'h': num = (unsigned long long)(short)va_arg(*ap, int); break;
            case 'l': num = (unsigned long long)va_arg(*ap, long); break;
            case 'L': num = (unsigned long long)va_arg(*ap, long long); break;
            case 'z': num = (unsigned long long)va_arg(*ap, long); break;
            default:  num = (unsigned long long)va_arg(*ap, int); break;
            }
        } else {
            switch (qual) {
            case 'H': num = (unsigned char)va_arg(*ap, unsigned int); break;
            case 'h': num = (unsigned short)va_arg(*ap, unsigned int); break;
            case 'l': num = va_arg(*ap, unsigned long); break;
            case 'L': num = va_arg(*ap, unsigned long long); break;
            case 'z': num = va_arg(*ap, size_t); break;
            default:  num = va_arg(*ap, unsigned int); break;
            }
        }
        number(o, num, s);
    }
    return (int)(o->pos - start);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct out o = { buf, size, 0 };
    va_list copy;
    va_copy(copy, args);
    do_format(&o, fmt, &copy);
    va_end(copy);
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return o.pos > INT_MAX ? INT_MAX : (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* what was actually written (without the NUL), not what would have been */
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    int r = vsnprintf(buf, size, fmt, args);
    if (!size) return 0;
    return (size_t)r < size ? r : (int)size - 1;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vscnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list args) { return vsnprintf(buf, INT_MAX, fmt, args); }

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, INT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *p = kmalloc((size_t)len + 1, gfp);
    if (p) vsnprintf(p, (size_t)len + 1, fmt, args);
    return p;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *p = kvasprintf(gfp, fmt, ap);
    va_end(ap);
    return p;
}

/* ---- scanf ---------------------------------------------------------------------- */
static bool scan_number(const char **sp, int width, unsigned int base, bool is_signed, unsigned long long *out)
{
    char tmp[72];
    const char *s = *sp;
    int n = 0, max = width > 0 && width < (int)sizeof tmp ? width : (int)sizeof tmp - 1;
    if (n < max && (*s == '-' || *s == '+') && is_signed) tmp[n++] = *s++;
    while (n < max && *s && (isxdigit(*s) || ((*s == 'x' || *s == 'X') && (base == 16 || base == 0))))
        tmp[n++] = *s++;
    tmp[n] = 0;
    char *end;
    if (is_signed) *out = (unsigned long long)simple_strtoll(tmp, &end, base);
    else *out = simple_strtoull(tmp, &end, base);
    if (end == tmp || (end == tmp + 1 && (tmp[0] == '-' || tmp[0] == '+'))) return false;
    *sp += end - tmp;
    return true;
}

int vsscanf(const char *buf, const char *fmt, va_list args)
{
    const char *s = buf;
    int assigned = 0;

    while (*fmt) {
        if (isspace(*fmt)) {
            while (isspace(*fmt)) fmt++;
            while (isspace(*s)) s++;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;
        bool skip = false;
        if (*fmt == '*') { skip = true; fmt++; }
        int width = -1;
        if (isdigit(*fmt)) { width = 0; while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0'); }
        int qual = 0;
        if (*fmt == 'h') { qual = 'h'; fmt++; if (*fmt == 'h') { qual = 'H'; fmt++; } }
        else if (*fmt == 'l') { qual = 'l'; fmt++; if (*fmt == 'l') { qual = 'L'; fmt++; } }
        else if (*fmt == 'L' || *fmt == 'q' || *fmt == 'j') { qual = 'L'; fmt++; }
        else if (*fmt == 'z' || *fmt == 'Z' || *fmt == 't') { qual = 'z'; fmt++; }
        char c = *fmt++;
        if (!c) break;

        if (c == 'n') {
            if (!skip) *va_arg(args, int *) = (int)(s - buf);
            continue;
        }
        if (c == 'c') {
            int n = width < 0 ? 1 : width;
            char *d = skip ? NULL : va_arg(args, char *);
            for (int i = 0; i < n; i++) { if (!*s) goto done; if (d) d[i] = *s; s++; }
            if (!skip) assigned++;
            continue;
        }
        while (isspace(*s)) s++;
        if (!*s) break;
        if (c == 's') {
            char *d = skip ? NULL : va_arg(args, char *);
            int n = 0;
            while (*s && !isspace(*s) && (width < 0 || n < width)) { if (d) d[n] = *s; n++; s++; }
            if (d) { d[n] = 0; assigned++; }
            continue;
        }
        unsigned int base;
        bool is_signed = false;
        switch (c) {
        case 'd': base = 10; is_signed = true; break;
        case 'i': base = 0; is_signed = true; break;
        case 'u': base = 10; break;
        case 'x': case 'X': base = 16; break;
        case 'o': base = 8; break;
        default: goto done;
        }
        unsigned long long v;
        if (!scan_number(&s, width, base, is_signed, &v)) break;
        if (skip) continue;
        switch (qual) {
        case 'H': *va_arg(args, unsigned char *) = (unsigned char)v; break;
        case 'h': *va_arg(args, unsigned short *) = (unsigned short)v; break;
        case 'l': *va_arg(args, unsigned long *) = (unsigned long)v; break;
        case 'L': *va_arg(args, unsigned long long *) = v; break;
        case 'z': *va_arg(args, size_t *) = (size_t)v; break;
        default:  *va_arg(args, unsigned int *) = (unsigned int)v; break;
        }
        assigned++;
    }
done:
    return assigned;
}

int sscanf(const char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(buf, fmt, ap);
    va_end(ap);
    return r;
}
