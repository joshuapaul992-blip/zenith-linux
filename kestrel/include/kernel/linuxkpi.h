/* include/kernel/linuxkpi.h -- the Linux driver compatibility layer (linuxkpi/) */
#ifndef KERNEL_LINUXKPI_H
#define KERNEL_LINUXKPI_H

/* Start the KPI's threads, build the Linux view of the PCI devices and run
 * the built-in Linux drivers' initcalls (they probe here). Boot thread,
 * after PCI, MSI, the scheduler and the root file system are up; returns
 * when the initcalls (and the self-test, with linuxkpi.selftest=1) are done. */
void linuxkpi_init(void);

#endif
