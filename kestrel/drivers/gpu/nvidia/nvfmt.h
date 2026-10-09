/* nvfmt.h -- the small printf the freestanding NVIDIA code shares:
 * %s %c %d %u %x with l/ll, width, '0' and '-' flags. */
#ifndef NVFMT_H
#define NVFMT_H

#include <stdarg.h>
#include <stddef.h>

/* Output goes to buf (always NUL-terminated if cap > 0) or, when putc is
 * set, to putc. Returns the length the full output would have. */
struct nv_sbuf { char *p; size_t n, cap; void (*putc)(char); };

void   nv_sb_putc(struct nv_sbuf *b, char c);
void   nv_sb_vprintf(struct nv_sbuf *b, const char *fmt, va_list ap);
void   nv_sb_printf(struct nv_sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
size_t nv_snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
size_t nv_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif
