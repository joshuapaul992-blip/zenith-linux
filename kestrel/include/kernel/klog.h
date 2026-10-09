/* include/kernel/klog.h -- kernel log (dmesg ring buffer, COM1 mirror, panic) */
#ifndef KESTREL_KLOG_H
#define KESTREL_KLOG_H

#include <stddef.h>
#include <stdbool.h>

#define KLOG_SIZE 16384

/* Append a formatted message to the kernel log. The log is always mirrored
 * to COM1; it is also echoed to the on-screen console once the console is
 * live and klog_set_console(true) has been called. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_set_console(bool on);
/* Character sink for standalone drivers (pci.c, ahci.c): buffers a line and
 * hands it to kprintf() at each newline. */
void klog_putc(char c);
bool klog_console_enabled(void);

/* Copy up to `len` bytes of the log starting at `off` (for /proc/kmsg). */
size_t klog_read(size_t off, char *buf, size_t len);
size_t klog_size(void);

__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

#define KASSERT(x) do { if (!(x)) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

#endif
