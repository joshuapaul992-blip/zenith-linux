/* include/kernel/power.h -- power off, restart, halt (kernel/power.c) */
#ifndef KESTREL_POWER_H
#define KESTREL_POWER_H

__attribute__((noreturn)) void power_off(void);
__attribute__((noreturn)) void power_reboot(void);
__attribute__((noreturn)) void power_halt(void);

/* Called once, before anything else, by the three above (the Linux KPI's
 * reboot notifiers and power-off handlers). */
#define POWER_HOOK_OFF      0
#define POWER_HOOK_REBOOT   1
#define POWER_HOOK_HALT     2
void power_set_hook(void (*fn)(int what));

#endif
