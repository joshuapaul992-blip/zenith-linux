/* lib/printf.c -- minimal, freestanding vsnprintf */
#include <kernel/string.h>
#include <stdbool.h>

struct out { char *buf; size_t size; size_t pos; };

static void emit(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void emit_padded(struct out *o, const char *s, size_t len, int width, bool left, char pad)
{
    int fill = width > (int)len ? width - (int)len : 0;
    if (!left) while (fill-- > 0) emit(o, pad);
    for (size_t i = 0; i < len; i++) emit(o, s[i]);
    if (left) while (fill-- > 0) emit(o, ' ');
}

static void emit_number(struct out *o, unsigned long long v, bool neg, int base, bool upper,
                        int width, bool left, char pad, const char *prefix)
{
    char tmp[32]; int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);

    size_t plen = prefix ? strlen(prefix) : 0;
    int total = n + (neg ? 1 : 0) + (int)plen;
    int fill = width > total ? width - total : 0;

    if (!left && pad == ' ') while (fill-- > 0) emit(o, ' ');
    if (neg) emit(o, '-');
    for (size_t i = 0; i < plen; i++) emit(o, prefix[i]);
    if (!left && pad == '0') while (fill-- > 0) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) while (fill-- > 0) emit(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;

        bool left = false; char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        int lng = 0;    /* 0=int 1=long 2=long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z') lng = 1;
            else if (*fmt == 'h') lng--;
            else break;
        }

        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng <= -2) v = (signed char)v;
            bool neg = v < 0;
            emit_number(&o, neg ? -(unsigned long long)v : (unsigned long long)v, neg, 10, false, width, left, pad, NULL);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            if (lng == -1) v = (unsigned short)v; else if (lng <= -2) v = (unsigned char)v;
            int base = *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16;
            emit_number(&o, v, false, base, *fmt == 'X', width, left, pad, NULL);
            break;
        }
        case 'p':
            emit_number(&o, (uintptr_t)va_arg(ap, void *), false, 16, false, width, left, '0', "0x");
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t len = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            emit_padded(&o, s, len, width, left, ' ');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); emit_padded(&o, &c, 1, width, left, ' '); break; }
        case '%': emit(&o, '%'); break;
        case 0: fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt); break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
