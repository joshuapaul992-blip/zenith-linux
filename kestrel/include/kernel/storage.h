/* include/kernel/storage.h -- block storage bring-up (AHCI and NVMe via PCI) */
#ifndef KESTREL_STORAGE_H
#define KESTREL_STORAGE_H

#include <stddef.h>

void   storage_init(void);              /* after pci_init() and the heap  */
void   storage_register_devices(void);  /* after vfs_init(): /dev/sdX, block devices */
size_t storage_summary(char *buf, size_t cap);
void   storage_shutdown(void);          /* before reboot/power-off: NVMe normal shutdown */

#endif
