/* =============================================================================
 *  libsupcxx-compat.c -- the glibc-only symbols the host's libsupc++ uses
 *
 *  musl-g++ links C++ programs against the build host's libsupc++.a, which
 *  was compiled against glibc. Everything it needs exists in musl except:
 *
 *    __isoc23_strtoul   glibc >= 2.38 redirects strtoul here in C23 mode
 *                       (used to parse GLIBCXX_TUNABLES)
 *    __sprintf_chk      _FORTIFY_SOURCE sprintf (used by the demangler)
 *    _dl_find_object    glibc >= 2.35 frame lookup for the unwinder
 *    __libc_single_threaded  glibc >= 2.32 hint used by the static-local
 *                       guards; Kestrel processes are single-threaded
 *
 *  _dl_find_object reports "not found", so the unwinder finds no frames:
 *  C++ exceptions cannot be caught (a throw ends in std::terminate). The
 *  programs ported so far do not use exceptions.
 * ============================================================================= */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

char __libc_single_threaded = 1;

unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

int __sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, len, fmt, ap);
    va_end(ap);
    if (r >= 0 && (size_t)r >= len)
        abort();                        /* what the fortified version does */
    return r;
}

int _dl_find_object(void *pc, void *result)
{
    (void)pc; (void)result;
    return -1;
}
