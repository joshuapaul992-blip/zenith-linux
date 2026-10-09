/* include/kernel/string.h -- freestanding string/memory routines */
#ifndef KESTREL_STRING_H
#define KESTREL_STRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

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
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char  *strtok_r(char *s, const char *delim, char **save);
long   strtol(const char *s, char **end, int base);

/* printf family (lib/printf.c). Supports %d %i %u %x %X %o %p %s %c %%
 * with flags '-', '0', width, precision for %s, and length modifiers
 * hh h l ll z. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
