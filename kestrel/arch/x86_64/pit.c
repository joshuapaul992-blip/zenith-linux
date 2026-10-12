/* arch/x86_64/pit.c -- the system tick: the 8254 PIT on IRQ 0, or (with an
 * APIC) the boot CPU's local APIC timer; every other CPU's local timer only
 * drives its own time slices. */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/time.h>

#define PIT_BASE_HZ 1193182u

static volatile uint64_t ticks;
static uint32_t tick_hz = PIT_HZ;
#define MAX_TICK_HOOKS 4
static void (*tick_hooks[MAX_TICK_HOOKS])(void);

bool pit_add_tick_hook(void (*fn)(void))
{
    for (int i = 0; i < MAX_TICK_HOOKS; i++)
        if (!tick_hooks[i] || tick_hooks[i] == fn) { tick_hooks[i] = fn; return true; }
    return false;
}

/* The boot CPU's tick without the kernel lock (idt.c): all but every
 * fourth, which runs the hooks (the xHCI event ring has its own interrupt
 * by then; this is the fallback and key repeat), and any with an interval
 * timer due. timer_tick_fast() runs the ones that can. */
#define HOOK_EVERY 4
bool timer_tick_needs_lock(void)
{
    return (ticks + 1) % HOOK_EVERY == 0 || sched_tick_needs_lock();
}

void timer_tick_fast(void)
{
    ticks++;
    sched_tick_boot_fast();
}

/* The rest of a boot CPU tick, big lock held: the hooks every HOOK_EVERY
 * ticks, and interval timers that are due. */
void timer_tick_locked(void)
{
    for (int i = 0; i < MAX_TICK_HOOKS && tick_hooks[i]; i++)
        tick_hooks[i]();            /* xhci_poll(), terminal cursor blink, ... */
    sched_alarms();
}

void timer_interrupt(bool boot_cpu)
{
    if (!boot_cpu) { sched_tick_local(); return; }
    ticks++;
    for (int i = 0; i < MAX_TICK_HOOKS && tick_hooks[i]; i++)
        tick_hooks[i]();            /* xhci_poll(), terminal cursor blink, ... */
    sched_tick();               /* wakes sleepers, may preempt the current thread */
}

static void pit_irq(struct int_frame *f)
{
    (void)f;
    timer_interrupt(true);
}

void pit_init(uint32_t hz)
{
    tick_hz = hz;
    uint32_t div = PIT_BASE_HZ / hz;
    outb(0x43, 0x36);           /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_register(IRQ_TIMER, pit_irq);
    pic_unmask(IRQ_TIMER);
}

uint64_t pit_ticks(void) { return ticks; }
/* From the clocksource when there is one: a tick that arrives late (the boot
 * CPU busy with interrupts off) is merged with the next, and counting ticks
 * then loses time, a quarter of it in a busy QEMU guest. */
uint64_t uptime_ms(void)
{
    uint64_t ns = time_ns();
    return ns ? ns / 1000000 : ticks * 1000 / tick_hz;
}

void pit_sleep_ms(uint64_t ms)
{
    uint64_t end = uptime_ms() + ms;
    while (uptime_ms() < end) hlt();
}
