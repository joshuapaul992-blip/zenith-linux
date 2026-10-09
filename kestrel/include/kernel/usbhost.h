/* include/kernel/usbhost.h -- USB host (xHCI) bring-up and input glue */
#ifndef KESTREL_USBHOST_H
#define KESTREL_USBHOST_H

#include <stddef.h>

void   usb_init(void);              /* after pci_init() + video, before sti     */
void   usb_start_thread(void);      /* after the scheduler: hot-plug thread     */
void   usb_redraw_pointer(void);    /* after a full-screen redraw               */
size_t usb_summary(char *buf, size_t cap);
size_t usb_proc(char *buf, size_t cap);   /* /proc/usb contents */

/* boot-time storage discovery (see bootvol.c) */
#include <stdbool.h>
#include <stdint.h>
bool   usb_present(void);           /* xHCI driver running                       */
int    usb_settle(void);            /* one non-blocking settle pass: ports busy, -1 busy */
int    usb_storage_scan(uint32_t ready_timeout_ms);  /* probe new MSC LUNs -> block devs */
int    usb_storage_pending(void);   /* MSC interfaces not yet probed (or retrying) */
void   usb_dump_controller(void (*putc_fn)(char c));
size_t usb_storage_proc(char *buf, size_t cap);

#endif
