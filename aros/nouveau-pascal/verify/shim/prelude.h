/* Things AROS's FreeBSD-derived libc headers provide implicitly */
#include <stdarg.h>
#include <stddef.h>
#define __pure2 __attribute__((__const__))
#define __unused __attribute__((__unused__))
#define __packed __attribute__((__packed__))
#define __aligned(x) __attribute__((__aligned__(x)))
typedef unsigned char u_char; typedef unsigned short u_short;
typedef unsigned int u_int; typedef unsigned long u_long;
/* glibc defines __BIG_ENDIAN unconditionally (as 4321); AROS libc does not,
   and nvkm/core/os.h selects big-endian MMIO accessors on #ifdef __BIG_ENDIAN */
#include <endian.h>
#undef __BIG_ENDIAN
/* declared by AROS libc's string.h, not by glibc in POSIX mode */
void bzero(void *, size_t);
int strncasecmp(const char *, const char *, size_t);
