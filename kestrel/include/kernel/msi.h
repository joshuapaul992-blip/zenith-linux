/* include/kernel/msi.h -- message-signalled interrupts for PCI devices
 *
 * A device with an MSI or MSI-X capability interrupts by writing a message
 * to the local APIC's address window: no interrupt pins, no routing tables
 * (ACPI _PRT) and no sharing. Kestrel gives each such device its own
 * vector from VEC_MSI_BASE up and delivers it to the boot CPU; the handler
 * runs like an IRQ handler (kernel lock held, interrupts off) after the
 * EOI. MSI-X is preferred when a device has both; only its first table
 * entry is used. Needs the local APIC (apic_enabled()). */
#ifndef KESTREL_MSI_H
#define KESTREL_MSI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VEC_MSI_BASE    0x50
#define VEC_MSI_COUNT   32

struct pci_device;
typedef void (*msi_handler_t)(void *arg);

/* Route the device's interrupt to `fn(arg)`: allocate a vector, program
 * the capability, enable it (legacy INTx goes off). Returns the vector,
 * or -1 if the device has no MSI capability, there is no APIC or no free
 * vector: the driver then keeps polling. `name` is for /proc/interrupts. */
int  msi_enable(struct pci_device *dev, msi_handler_t fn, void *arg, const char *name);
/* The vector msi_enable() returned is delivered by MSI-X (not MSI). */
bool msi_is_msix(int vector);
/* isr_dispatch: a vector in [VEC_MSI_BASE, VEC_MSI_BASE + VEC_MSI_COUNT) */
void msi_dispatch(int vector);
/* /proc/interrupts lines for the MSI vectors in use */
size_t msi_proc(char *buf, size_t cap);

#endif
