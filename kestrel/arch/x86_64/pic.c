/* arch/x86_64/pic.c -- legacy 8259A interrupt controllers */
#include <kernel/arch.h>
#include <kernel/cpu.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(void)
{
    /* ICW1: start init, expect ICW4 */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE);     io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    /* ICW3: slave on IRQ2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    /* mask everything except the cascade line */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_mask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_eoi(int irq)
{
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
