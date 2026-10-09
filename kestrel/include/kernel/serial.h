/* include/kernel/serial.h -- 16550 UART (COM1) debug port */
#ifndef KESTREL_SERIAL_H
#define KESTREL_SERIAL_H

#include <stdbool.h>

bool serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
bool serial_present(void);
int  serial_getc(void);           /* non-blocking, -1 if no byte waiting */

#endif
