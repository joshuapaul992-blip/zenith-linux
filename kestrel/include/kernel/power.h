/* include/kernel/power.h -- power off, restart, halt (kernel/power.c) */
#ifndef KESTREL_POWER_H
#define KESTREL_POWER_H

__attribute__((noreturn)) void power_off(void);
__attribute__((noreturn)) void power_reboot(void);
__attribute__((noreturn)) void power_halt(void);

#endif
