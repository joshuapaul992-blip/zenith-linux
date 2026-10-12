/* linux/sprintf.h -- Kestrel's Linux KPI: formatted output (linuxkpi/src/vsprintf.c)
 *
 * Linux's format language, %p extensions included (%pe, %pM, %pI4, %pa,
 * %pad, %ph, %pR, %pS, %pV ...), so these are the KPI's own functions,
 * not Kestrel's printf. */
#ifndef _LKPI_LINUX_SPRINTF_H
#define _LKPI_LINUX_SPRINTF_H

#include <linux/types.h>
#include <linux/stdarg.h>

#define vsnprintf       lkpi_vsnprintf
#define snprintf        lkpi_snprintf
#define vscnprintf      lkpi_vscnprintf
#define scnprintf       lkpi_scnprintf
#define vsprintf        lkpi_vsprintf
#define sprintf         lkpi_sprintf
#define kasprintf       lkpi_kasprintf
#define kvasprintf      lkpi_kvasprintf
#define sscanf          lkpi_sscanf
#define vsscanf         lkpi_vsscanf

__printf(3, 0) int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(3, 4) int snprintf(char *buf, size_t size, const char *fmt, ...);
__printf(3, 0) int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(3, 4) int scnprintf(char *buf, size_t size, const char *fmt, ...);
__printf(2, 0) int vsprintf(char *buf, const char *fmt, va_list args);
__printf(2, 3) int sprintf(char *buf, const char *fmt, ...);
__printf(2, 3) __malloc char *kasprintf(gfp_t gfp, const char *fmt, ...);
__printf(2, 0) __malloc char *kvasprintf(gfp_t gfp, const char *fmt, va_list args);
__scanf(2, 3) int sscanf(const char *buf, const char *fmt, ...);
__scanf(2, 0) int vsscanf(const char *buf, const char *fmt, va_list args);

#endif
