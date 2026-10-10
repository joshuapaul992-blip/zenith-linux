/* arch/x86_64/idt.c -- IDT setup and the central interrupt dispatcher */
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/arch.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include <kernel/vm.h>

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
    for (int v = 0; v < 48; v++) {
        uint8_t ist = v == 8 ? IST_DOUBLE_FAULT : v == 2 ? IST_NMI : v == 18 ? IST_MACHINE_CHECK : 0;
        set_gate(v, isr_stub_table[v], ist, v == 3 || v == 4 ? 3 : 0);   /* int3/into allowed from ring 3 */
    }
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

/* ---- faults in ring 3: signals ---------------------------------------------
 * The exception becomes the signal Linux sends for it, with the si_code and
 * si_addr a handler expects. A process with a handler (SIGSEGV for a JIT or
 * a garbage collector, SIGFPE, ...) gets to run it; otherwise the default
 * action ends the process. Either way the kernel is unaffected. */
#define SI_KERNEL     0x80
#define SEGV_MAPERR   1
#define SEGV_ACCERR   2
#define SEGV_CPERR    10
#define BUS_ADRALN    1
#define ILL_ILLOPN    2
#define TRAP_BRKPT    1
#define TRAP_TRACE    2
#define FPE_INTDIV    1
#define FPE_FLTDIV    3
#define FPE_FLTOVF    4
#define FPE_FLTUND    5
#define FPE_FLTRES    6
#define FPE_FLTINV    7

static int simd_code(void)
{
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    uint32_t pending = mxcsr & ~(mxcsr >> 7) & 0x3f;    /* raised and not masked */
    if (pending & 0x01) return FPE_FLTINV;
    if (pending & 0x04) return FPE_FLTDIV;
    if (pending & 0x08) return FPE_FLTOVF;
    if (pending & 0x10) return FPE_FLTUND;
    if (pending & 0x20) return FPE_FLTRES;
    if (pending & 0x02) return FPE_FLTUND;              /* denormal operand */
    return FPE_FLTINV;
}

static void user_fault(struct int_frame *f)
{
    int sig = SIGSEGV, code = SI_KERNEL;
    uint64_t addr = 0;
    switch (f->vector) {
    case 0:  sig = SIGFPE;  code = FPE_INTDIV; addr = f->rip; break;
    case 1:  sig = SIGTRAP; code = TRAP_TRACE; addr = f->rip; break;
    case 3:  sig = SIGTRAP; code = SI_KERNEL; break;
    case 6:  sig = SIGILL;  code = ILL_ILLOPN; addr = f->rip; break;
    case 7:  sig = SIGILL;  code = SI_KERNEL; break;
    case 11: case 12: sig = SIGBUS; break;              /* segment / stack-segment not present */
    case 14:
        addr = read_cr2();
        if (vm_fault(addr, f->error, true) == 0) return;  /* demand paging, copy-on-write */
        /* Like Linux: anything outside the user half (the kernel's own
         * supervisor-only mappings included) is "not mapped" for the process. */
        code = (f->error & 1) && user_range_ok(addr, 1) ? SEGV_ACCERR : SEGV_MAPERR;
        break;
    case 16: sig = SIGFPE;  code = FPE_FLTINV; addr = f->rip; break;
    case 17: sig = SIGBUS;  code = BUS_ADRALN; break;
    case 19: sig = SIGFPE;  code = simd_code(); addr = f->rip; break;
    case 21: code = SEGV_CPERR; break;
    default: break;                                     /* #GP, #OF, #BR, #TS: SIGSEGV */
    }
    signal_force_fault(sig, code, addr, (uint32_t)f->vector, f->error);
}

/* ---- faults in the kernel: diagnostics, never silent corruption ------------ */
extern char __text_start[], __text_end[];
extern uint8_t boot_stack[], boot_stack_top[];

static bool kernel_text(uint64_t a) { return a >= (uint64_t)__text_start && a < (uint64_t)__text_end; }

/* The stack a frame pointer may point into: the current task's kernel stack,
 * the boot stack, or an IST stack. */
static bool stack_bounds(uint64_t rbp, uint64_t *lo, uint64_t *hi)
{
    struct tcb *t = current_task();
    if (t && t->kstack && rbp >= (uint64_t)t->kstack && rbp < (uint64_t)t->kstack + t->kstack_size) {
        *lo = (uint64_t)t->kstack; *hi = *lo + t->kstack_size; return true;
    }
    if (rbp >= (uint64_t)boot_stack && rbp < (uint64_t)boot_stack_top) {
        *lo = (uint64_t)boot_stack; *hi = (uint64_t)boot_stack_top; return true;
    }
    return ist_stack_range(rbp, lo, hi);
}

/* Symbol table from the second link (tools/ksyms.py). */
extern const unsigned ksym_count;
extern const uint64_t ksym_addr[];
extern const uint32_t ksym_off[];
extern const char ksym_names[];

static const char *ksym(uint64_t a, uint64_t *off)
{
    if (!ksym_count || a < ksym_addr[0]) return NULL;
    unsigned lo = 0, hi = ksym_count;                   /* last symbol <= a */
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (ksym_addr[mid] <= a) lo = mid; else hi = mid;
    }
    *off = a - ksym_addr[lo];
    return ksym_names + ksym_off[lo];
}

static void print_addr(const char *label, uint64_t a)
{
    uint64_t off;
    const char *n = kernel_text(a) ? ksym(a, &off) : NULL;
    if (n) kprintf("%s%016lx %s+0x%lx\n", label, a, n, off);
    else kprintf("%s%016lx\n", label, a);
}

static void backtrace(uint64_t rbp)
{
    uint64_t lo, hi, last = 0;
    int repeats = 0;
    kprintf(" backtrace:\n");
    for (int i = 0; i < 32; i++) {
        if ((rbp & 7) || !stack_bounds(rbp, &lo, &hi) || rbp + 16 > hi) break;
        uint64_t ret = ((const uint64_t *)rbp)[1], next = ((const uint64_t *)rbp)[0];
        if (!kernel_text(ret)) break;
        if (ret == last) repeats++;                     /* deep recursion: one line */
        else { if (repeats) kprintf("   ... %d more times\n", repeats); repeats = 0; print_addr("   ", ret); }
        last = ret;
        if (next <= rbp) break;                         /* frames only go up */
        rbp = next;
    }
    if (repeats) kprintf("   ... %d more times\n", repeats);
}

static void dump_frame(struct int_frame *f)
{
    struct tcb *t = current_task();
    kprintf(" task: pid %d (%s)%s\n", t ? t->pid : -1, t ? t->name : "?", t && t->user ? ", in a system call" : "");
    print_addr(" at ", f->rip);
    kprintf(" RIP=%016lx  CS=%04lx  RFLAGS=%016lx  ERR=%lx\n", f->rip, f->cs, f->rflags, f->error);
    kprintf(" RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf(" RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx\n", f->rsi, f->rdi, f->rbp, f->rsp);
    kprintf(" R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf(" R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf(" CR0=%016lx CR2=%016lx CR3=%016lx CR4=%016lx\n", read_cr0(), read_cr2(), read_cr3(), read_cr4());
    if (kernel_text(f->rip) && kernel_text(f->rip + 15)) {
        kprintf(" code:");
        for (int i = 0; i < 16; i++) kprintf(" %02x", ((const uint8_t *)f->rip)[i]);
        kprintf("\n");
    }
    backtrace(f->rbp);
}

static void machine_check_report(void)
{
    uint64_t cap = rdmsr(0x179), st = rdmsr(0x17A);     /* IA32_MCG_CAP, IA32_MCG_STATUS */
    kprintf(" MCG_STATUS=%lx (RIPV %lu, EIPV %lu, MCIP %lu)\n", st, st & 1, (st >> 1) & 1, (st >> 2) & 1);
    for (unsigned b = 0; b < (cap & 0xff) && b < 32; b++) {
        uint64_t s2 = rdmsr(0x401 + 4 * b);             /* IA32_MCi_STATUS */
        if (!(s2 >> 63)) continue;                      /* VAL */
        kprintf(" bank %u: STATUS=%016lx", b, s2);
        if (s2 & (1ull << 58)) kprintf(" ADDR=%016lx", rdmsr(0x402 + 4 * b));
        if (s2 & (1ull << 59)) kprintf(" MISC=%016lx", rdmsr(0x403 + 4 * b));
        kprintf("%s\n", (s2 & (1ull << 61)) ? " (uncorrected)" : "");
    }
}

__attribute__((noreturn)) static void kernel_exception(struct int_frame *f)
{
    struct tcb *t = current_task();
    kprintf("\n=== KERNEL EXCEPTION %lu: %s ===\n", f->vector, exc_names[f->vector]);
    if (f->vector == 8 && t && task_stack_guard_hit(t, read_cr2()))
        kprintf(" kernel stack overflow: pid %d (%s) ran into the guard page below its stack\n", t->pid, t->name);
    if (f->vector == 14)
        kprintf(" page fault at %016lx (%s, %s%s)\n", read_cr2(), (f->error & 1) ? "protection" : "not present",
                (f->error & 2) ? "write" : "read", (f->error & 16) ? ", instruction fetch" : "");
    if (f->vector == 18) machine_check_report();
    dump_frame(f);
    panic("%s (vector %lu) at RIP=%016lx, error=%lx", exc_names[f->vector], f->vector, f->rip, f->error);
}

static void nmi(struct int_frame *f)
{
    uint8_t reason = inb(0x61);                         /* system control port B */
    kprintf("nmi: at RIP=%016lx, port 0x61 = %02x%s%s\n", f->rip, reason,
            (reason & 0x80) ? " (memory parity / SERR)" : "", (reason & 0x40) ? " (I/O channel check)" : "");
}

static void exception(struct int_frame *f)
{
    if (f->vector == 2) { nmi(f); return; }
    if ((f->cs & 3) == 3) { user_fault(f); return; }
    if (f->vector == 14) {
        /* A fault on a user address inside a uaccess routine: let the memory
         * manager resolve it (demand paging, copy-on-write) unless the caller
         * asked for no fault handling, else resume at the fixup (-EFAULT). */
        uint64_t addr = read_cr2();
        if (user_range_ok(addr, 1) && !pagefault_disabled() && vm_fault(addr, f->error, false) == 0) return;
        uint64_t fix = extable_fixup(f->rip);
        if (fix && user_range_ok(addr, 1)) { f->rip = fix; return; }
    }
    kernel_exception(f);
}

void isr_dispatch(struct int_frame *f)
{
    if (f->vector < 32) {
        exception(f);
        if ((f->cs & 3) == 3) signal_deliver(f, 0, false);    /* a handler, or the default action */
        return;
    }

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
        if ((f->cs & 3) == 3) signal_deliver(f, 0, false);  /* back to ring 3 */
    }
}
