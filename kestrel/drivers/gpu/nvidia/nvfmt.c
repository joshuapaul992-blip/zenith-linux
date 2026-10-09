/* nvfmt.c -- see nvfmt.h */
#include "nvfmt.h"
#include <stdint.h>
#include <stdbool.h>

void nv_sb_putc(struct nv_sbuf *b, char c)
{
    if (b->putc) { b->putc(c); return; }
    if (b->n + 1 < b->cap) b->p[b->n] = c;
    b->n++;
}

void nv_sb_vprintf(struct nv_sbuf *b, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { nv_sb_putc(b, *f); continue; }
        f++;
        bool zero = false, left = false;
        if (*f == '-') { left = true; f++; }
        if (*f == '0') { zero = true; f++; }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int lng = 0;
        while (*f == 'l') { lng++; f++; }
        char tmp[24]; int n = 0;
        const char *s = tmp;
        switch (*f) {
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; while (s[n]) n++; break;
        case 'c': tmp[0] = (char)va_arg(ap, int); n = 1; break;
        case 'd': case 'u': case 'x': {
            uint64_t v; bool neg = false;
            if (*f == 'd') {
                int64_t sv = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
                neg = sv < 0; v = neg ? (uint64_t)-sv : (uint64_t)sv;
            } else {
                v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            }
            unsigned base = *f == 'x' ? 16 : 10;
            char rev[24]; int r = 0;
            do { rev[r++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            if (neg) tmp[n++] = '-';
            while (r) tmp[n++] = rev[--r];
            break;
        }
        case '%': tmp[0] = '%'; n = 1; break;
        default: continue;
        }
        if (!left) for (int i = n; i < width; i++) nv_sb_putc(b, zero ? '0' : ' ');
        for (int i = 0; i < n; i++) nv_sb_putc(b, s[i]);
        if (left) for (int i = n; i < width; i++) nv_sb_putc(b, ' ');
    }
}

void nv_sb_printf(struct nv_sbuf *b, const char *f, ...)
{
    va_list ap; va_start(ap, f); nv_sb_vprintf(b, f, ap); va_end(ap);
}

size_t nv_vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    struct nv_sbuf b = { buf, 0, cap, 0 };
    nv_sb_vprintf(&b, f, ap);
    if (cap) buf[b.n < cap ? b.n : cap - 1] = 0;
    return b.n;
}

size_t nv_snprintf(char *buf, size_t cap, const char *f, ...)
{
    va_list ap; va_start(ap, f);
    size_t n = nv_vsnprintf(buf, cap, f, ap);
    va_end(ap);
    return n;
}
