/* arch/x86_64/pit.c -- 8254 programmable interval timer: system tick source */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>

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

static void pit_irq(struct int_frame *f)
{
    (void)f;
    ticks++;
    for (int i = 0; i < MAX_TICK_HOOKS && tick_hooks[i]; i++)
        tick_hooks[i]();            /* xhci_poll(), terminal cursor blink, ... */
    sched_tick();               /* wakes sleepers, may preempt the current thread */
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
uint64_t uptime_ms(void) { return ticks * 1000 / tick_hz; }

void pit_sleep_ms(uint64_t ms)
{
    uint64_t end = uptime_ms() + ms;
    while (uptime_ms() < end) hlt();
}
