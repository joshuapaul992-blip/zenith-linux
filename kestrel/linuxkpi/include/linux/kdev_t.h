/* linux/kdev_t.h -- Kestrel's Linux KPI: device numbers (12-bit major, 20-bit minor) */
#ifndef _LKPI_LINUX_KDEV_T_H
#define _LKPI_LINUX_KDEV_T_H
#define MINORBITS       20
#define MINORMASK       ((1U << MINORBITS) - 1)
#define MAJOR(dev)      ((unsigned int)((dev) >> MINORBITS))
#define MINOR(dev)      ((unsigned int)((dev) & MINORMASK))
#define MKDEV(ma, mi)   (((ma) << MINORBITS) | (mi))
#endif
