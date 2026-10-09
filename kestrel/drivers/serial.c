/* drivers/serial.c -- COM1 (0x3F8) 115200 8N1, polled output.
 * Used for the kernel log so you can watch boot with `-serial stdio`. */
#include <kernel/serial.h>
#include <kernel/cpu.h>

#define COM1 0x3F8
static bool present;

bool serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable UART interrupts      */
    outb(COM1 + 3, 0x80);   /* DLAB on                      */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud     */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop    */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte trig */
    outb(COM1 + 4, 0x1E);   /* loopback mode for self-test  */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { present = false; return false; }
    outb(COM1 + 4, 0x0F);   /* normal mode, OUT1/OUT2/RTS/DTR */
    present = true;
    return true;
}

bool serial_present(void) { return present; }

/* Non-blocking receive: -1 when nothing is waiting (or no UART). */
int serial_getc(void)
{
    if (!present || !(inb(COM1 + 5) & 0x01)) return -1;     /* LSR.DR */
    return inb(COM1);
}

void serial_putc(char c)
{
    if (!present) return;
    if (c == '\n') serial_putc('\r');
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        cpu_relax();
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) { while (*s) serial_putc(*s++); }
