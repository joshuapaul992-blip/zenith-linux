/* linux/reboot.h -- Kestrel's Linux KPI: reboot notifiers and power-off handlers
 * (linuxkpi/src/notifier.c), run when Kestrel powers off, restarts or halts */
#ifndef _LKPI_LINUX_REBOOT_H
#define _LKPI_LINUX_REBOOT_H

#include <linux/notifier.h>
#include <linux/err.h>

struct device;

#define SYS_DOWN        0x0001
#define SYS_RESTART     SYS_DOWN
#define SYS_HALT        0x0002
#define SYS_POWER_OFF   0x0003

enum reboot_mode { REBOOT_UNDEFINED = -1, REBOOT_COLD = 0, REBOOT_WARM, REBOOT_HARD, REBOOT_SOFT, REBOOT_GPIO };
extern enum reboot_mode reboot_mode;

int register_reboot_notifier(struct notifier_block *nb);
int unregister_reboot_notifier(struct notifier_block *nb);
int devm_register_reboot_notifier(struct device *dev, struct notifier_block *nb);
int register_restart_handler(struct notifier_block *nb);
int unregister_restart_handler(struct notifier_block *nb);
void emergency_restart(void);
void kernel_restart(char *cmd);
void kernel_halt(void);
void kernel_power_off(void);
#define orderly_poweroff(force)         kernel_power_off()
#define orderly_reboot()                kernel_restart(NULL)
#define hw_protection_shutdown(r, ms)   kernel_power_off()

enum sys_off_mode {
    SYS_OFF_MODE_POWER_OFF_PREPARE,
    SYS_OFF_MODE_POWER_OFF,
    SYS_OFF_MODE_RESTART_PREPARE,
    SYS_OFF_MODE_RESTART,
};
#define SYS_OFF_PRIO_LOW        -128
#define SYS_OFF_PRIO_DEFAULT    0
#define SYS_OFF_PRIO_HIGH       192
#define SYS_OFF_PRIO_FIRMWARE   224
#define RESTART_PRIO_RESERVED   0
#define RESTART_PRIO_DEFAULT    128
#define RESTART_PRIO_HIGH       192

struct sys_off_data {
    int mode;
    void *cb_data;
    const char *cmd;
    struct device *dev;
};
struct sys_off_handler;
struct sys_off_handler *register_sys_off_handler(enum sys_off_mode mode, int priority,
                                                 int (*callback)(struct sys_off_data *data), void *cb_data);
void unregister_sys_off_handler(struct sys_off_handler *handler);
int devm_register_sys_off_handler(struct device *dev, enum sys_off_mode mode, int priority,
                                  int (*callback)(struct sys_off_data *data), void *cb_data);
int devm_register_power_off_handler(struct device *dev, int (*callback)(struct sys_off_data *data), void *cb_data);
int devm_register_restart_handler(struct device *dev, int (*callback)(struct sys_off_data *data), void *cb_data);

#endif
