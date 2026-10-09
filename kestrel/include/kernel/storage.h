/* include/kernel/storage.h -- block storage bring-up (AHCI via PCI) */
#ifndef KESTREL_STORAGE_H
#define KESTREL_STORAGE_H

#include <stddef.h>

void   storage_init(void);              /* after pci_init() and the heap  */
void   storage_register_devices(void);  /* after vfs_init(): /dev/sdX     */
size_t storage_summary(char *buf, size_t cap);

#endif
