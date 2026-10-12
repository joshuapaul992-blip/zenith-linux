/* linux/printk.h -- Kestrel's Linux KPI: the kernel log (linuxkpi/src/printk.c)
 *
 * Messages go to Kestrel's log (dmesg, COM1). KERN_DEBUG messages are
 * dropped unless the command line says linuxkpi.debug=1; pr_debug and
 * dev_dbg compile to nothing unless the file defines DEBUG. */
#ifndef _LKPI_LINUX_PRINTK_H
#define _LKPI_LINUX_PRINTK_H

#include <linux/types.h>
#include <linux/stdarg.h>
#include <linux/compiler.h>
#include <linux/sprintf.h>
#include <linux/kern_levels.h>

/* %pV: a format and its arguments, printed in place */
struct va_format { const char *fmt; va_list *va; };

#define printk          lkpi_printk
#define vprintk         lkpi_vprintk
__printf(1, 2) __cold int printk(const char *fmt, ...);
__printf(1, 0) int vprintk(const char *fmt, va_list args);

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define no_printk(fmt, ...)     ({ if (0) printk(fmt, ##__VA_ARGS__); 0; })
#define pr_emerg(fmt, ...)      printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)      printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)       printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)        printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)       printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...)    pr_warn(fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)     printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)       printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)       printk(KERN_CONT fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define pr_debug(fmt, ...)      printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...)      no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif
#define pr_devel(fmt, ...)      pr_debug(fmt, ##__VA_ARGS__)

#define printk_once(fmt, ...)   ({ static bool __print_once; bool __ret = !__print_once; \
                                   if (!__print_once) { __print_once = true; printk(fmt, ##__VA_ARGS__); } \
                                   unlikely(__ret); })
#define pr_emerg_once(fmt, ...) printk_once(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err_once(fmt, ...)   printk_once(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn_once(fmt, ...)  printk_once(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk_once(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_once(fmt, ...)  printk_once(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug_once(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
/* no rate limiting yet: every message is printed */
#define printk_ratelimit()      1
#define printk_ratelimited(fmt, ...)    printk(fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(fmt, ...)    pr_err(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)   pr_warn(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...)   pr_info(fmt, ##__VA_ARGS__)
#define pr_notice_ratelimited(fmt, ...) pr_notice(fmt, ##__VA_ARGS__)
#define pr_debug_ratelimited(fmt, ...)  pr_debug(fmt, ##__VA_ARGS__)

enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
void print_hex_dump(const char *level, const char *prefix_str, int prefix_type, int rowsize, int groupsize,
                    const void *buf, size_t len, bool ascii);
#define print_hex_dump_bytes(prefix, type, buf, len) print_hex_dump(KERN_DEBUG, prefix, type, 16, 1, buf, len, true)
#define print_hex_dump_debug(prefix, type, rs, gs, buf, len, ascii) \
    do { if (0) print_hex_dump(KERN_DEBUG, prefix, type, rs, gs, buf, len, ascii); } while (0)
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf,
                       size_t linebuflen, bool ascii);
void dump_stack(void);

extern const char hex_asc[];
#define hex_asc_lo(x)   hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x)   hex_asc[((x) & 0xf0) >> 4]
extern const char hex_asc_upper[];
static inline char *hex_byte_pack(char *buf, u8 byte) { *buf++ = hex_asc_hi(byte); *buf++ = hex_asc_lo(byte); return buf; }
int hex_to_bin(unsigned char ch);
int __must_check hex2bin(u8 *dst, const char *src, size_t count);
char *bin2hex(char *dst, const void *src, size_t count);

#endif
