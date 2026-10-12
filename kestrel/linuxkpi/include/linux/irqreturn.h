/* linux/irqreturn.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_IRQRETURN_H
#define _LKPI_LINUX_IRQRETURN_H
enum irqreturn { IRQ_NONE = 0, IRQ_HANDLED = 1 << 0, IRQ_WAKE_THREAD = 1 << 1 };
typedef enum irqreturn irqreturn_t;
#define IRQ_RETVAL(x)   ((x) ? IRQ_HANDLED : IRQ_NONE)
#endif
