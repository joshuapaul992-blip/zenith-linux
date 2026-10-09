/* arch/x86_64/idt.c -- IDT setup and the central interrupt dispatcher */
#include <kernel/task.h>
#include <kernel/arch.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/syscall.h>
#include <kernel/string.h>

struct idt_gate {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;     /* P | DPL | 0 | type (0xE = interrupt gate) */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

static struct idt_gate idt[256];
static irq_handler_t irq_handlers[16];
static uint64_t irq_counts[16];

extern uint64_t isr_stub_table[48];
extern void isr_syscall_stub(void);

static void set_gate(int vec, uint64_t handler, uint8_t ist, uint8_t dpl)
{
    idt[vec].offset_lo  = handler & 0xFFFF;
    idt[vec].selector   = GDT_KERNEL_CODE;
    idt[vec].ist        = ist;
    idt[vec].type_attr  = 0x80 | (dpl << 5) | 0x0E;
    idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vec].offset_hi  = handler >> 32;
    idt[vec].zero       = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof idt);
    for (int v = 0; v < 48; v++)
        set_gate(v, isr_stub_table[v], v == 8 ? 1 : 0, 0);   /* #DF on IST1 */
    /* POSIX syscall gate: DPL 3 so ring-3 code may execute `int $0x80` */
    set_gate(SYSCALL_VECTOR, (uint64_t)isr_syscall_stub, 0, 3);

    struct idt_ptr p = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}

void irq_register(int irq, irq_handler_t h) { if (irq >= 0 && irq < 16) irq_handlers[irq] = h; }
uint64_t irq_count(int irq) { return (irq >= 0 && irq < 16) ? irq_counts[irq] : 0; }

static const char *const exc_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security", "Reserved",
};

/* A fault in ring 3 kills the process, not the kernel. Exit status is
 * 128 + the signal Linux would deliver (SIGSEGV 11, SIGILL 4, SIGFPE 8,
 * SIGBUS 7, SIGTRAP 5). */
static void user_fault(struct int_frame *f)
{
    static const uint8_t sig[32] = { 8, 5, 0, 5, 11, 11, 4, 4, 0, 0, 0, 7, 7, 11, 11, 0, 8, 7, 0, 8 };
    struct tcb *t = current_task();
    int s = sig[f->vector] ? sig[f->vector] : 11;
    kprintf("\nprocess %d (%s): %s at RIP=%016lx", t->pid, t->name, exc_names[f->vector], f->rip);
    if (f->vector == 14)
        kprintf(", address %016lx (%s, %s)", read_cr2(), (f->error & 1) ? "protection" : "not present",
                (f->error & 2) ? "write" : "read");
    kprintf(", RSP=%016lx: killed (signal %d)\n", f->rsp, s);
    sti();
    task_exit(128 + s);
}

static void exception(struct int_frame *f)
{
    if ((f->cs & 3) == 3) { user_fault(f); return; }
    kprintf("\n=== CPU EXCEPTION %lu: %s ===\n", f->vector, exc_names[f->vector]);
    kprintf(" RIP=%016lx  CS=%04lx  RFLAGS=%016lx  ERR=%lx\n", f->rip, f->cs, f->rflags, f->error);
    kprintf(" RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf(" RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx\n", f->rsi, f->rdi, f->rbp, f->rsp);
    kprintf(" R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf(" R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf(" CR2=%016lx CR3=%016lx\n", read_cr2(), read_cr3());
    if (f->vector == 14)
        panic("Page fault at %016lx (%s, %s) RIP=%016lx", read_cr2(),
              (f->error & 1) ? "protection" : "not-present", (f->error & 2) ? "write" : "read", f->rip);
    panic("%s (vector %lu) at RIP=%016lx, error=%lx", exc_names[f->vector], f->vector, f->rip, f->error);
}

void isr_dispatch(struct int_frame *f)
{
    if (f->vector < 32) { exception(f); return; }

    if (f->vector == SYSCALL_VECTOR) { syscall_dispatch(f); return; }

    if (f->vector >= IRQ_BASE && f->vector < IRQ_BASE + 16) {
        int irq = (int)f->vector - IRQ_BASE;
        /* Spurious IRQ 7/15: the PIC's in-service bit is not set. */
        if (irq == 7 || irq == 15) {
            outb(irq == 7 ? 0x20 : 0xA0, 0x0B);
            uint8_t isr = inb(irq == 7 ? 0x20 : 0xA0);
            if (!(isr & 0x80)) { if (irq == 15) pic_eoi(2); return; }
        }
        irq_counts[irq]++;
        /* EOI first: the timer handler may switch to another thread and
         * not come back through here for a while. */
        pic_eoi(irq);
        if (irq_handlers[irq]) irq_handlers[irq](f);
    }
}
