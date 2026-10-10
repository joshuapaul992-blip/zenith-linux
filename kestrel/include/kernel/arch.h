/* include/kernel/arch.h -- x86_64 descriptor tables, interrupts, PIC, PIT */
#ifndef KESTREL_ARCH_H
#define KESTREL_ARCH_H

#include <stdint.h>
#include <stdbool.h>

/* ---- GDT selectors (layout chosen to satisfy SYSCALL/SYSRET) ----------- */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18    /* | 3 */
#define GDT_USER_CODE   0x20    /* | 3 */
#define GDT_TSS         0x28

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3
/* The IST stack containing addr, if any (backtraces). */
bool ist_stack_range(uint64_t addr, uint64_t *lo, uint64_t *hi);

/* ---- interrupt frame built by isr.asm / syscall_entry.asm -------------- */
struct int_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;      /* pushed by the CPU */
};

typedef void (*irq_handler_t)(struct int_frame *f);

#define IRQ_BASE        32
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_COM1        4
#define SYSCALL_VECTOR  0x80

void idt_init(void);
void irq_register(int irq, irq_handler_t h);
uint64_t irq_count(int irq);

/* ---- 8259A PIC ---------------------------------------------------------- */
void pic_init(void);                 /* remap to vectors 32-47, mask all */
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_eoi(int irq);

/* ---- 8253/8254 PIT ------------------------------------------------------ */
#define PIT_HZ 1000
void pit_init(uint32_t hz);
uint64_t pit_ticks(void);            /* monotonic, 1 tick = 1 ms */
/* Run `fn` on every timer tick in IRQ context (device polling, cursor blink).
 * Up to four hooks; returns false when the table is full. */
bool pit_add_tick_hook(void (*fn)(void));
uint64_t uptime_ms(void);
void pit_sleep_ms(uint64_t ms);      /* busy-wait with hlt (pre-scheduler) */

#endif
