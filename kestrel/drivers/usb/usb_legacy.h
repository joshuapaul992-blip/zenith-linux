/* =============================================================================
 *  usb_legacy.h -- take USB host controllers away from the firmware
 *
 *  BIOSes run USB keyboards and boot drives through SMM ("legacy support")
 *  until the OS claims each controller. If the OS starts its own driver
 *  while SMM is still active, both fight over the same registers: the
 *  classic cause of hangs or dead ports on real hardware. This module, run
 *  before any USB driver starts, performs:
 *
 *    EHCI   USB Legacy Support capability in PCI config space (EECP):
 *           set "HC OS Owned", wait (with timeout) for "HC BIOS Owned" to
 *           clear, disable all SMI sources; then halt the controller and
 *           clear CONFIGFLAG so its ports fall back to companion routing.
 *    UHCI   LEGSUP (PCI 0xC0): disable legacy keyboard emulation and SMIs.
 *    OHCI   ownership change request (HcCommandStatus.OCR) and wait for
 *           HcControl.IR to clear.
 *    Intel  PCH xHCI port switchover (Panther/Lynx/Wildcat Point): route
 *           the shared USB 2.0 ports and enable SuperSpeed on all ports of
 *           the xHCI, so devices are not stranded on an un-driven EHCI.
 *
 *  The xHCI's own USBLEGSUP handshake (extended capability ID 1) is done by
 *  the xHCI driver itself (xhci.c) right before it resets the controller.
 *  Requires pci.h (enumeration done) and platform hooks for MMIO + delays.
 * ============================================================================= */
#ifndef USB_LEGACY_H
#define USB_LEGACY_H

#include <stdint.h>
#include <stdbool.h>

struct usb_legacy_platform {
    void    *(*map_mmio)(uint64_t phys, uint32_t size);
    void     (*delay_us)(uint32_t us);
    void     (*log_putc)(char c);
};

struct usb_legacy_report {
    int ehci, ehci_bios_owned, ehci_forced;     /* controllers / BIOS released / forced */
    int uhci, ohci, ohci_forced;
    int intel_routed;                           /* xHCI port switchover performed       */
    uint32_t xusb2pr, usb3_pssen;               /* values written (last Intel xHCI)     */
};

/* Run the handoffs for every EHCI/UHCI/OHCI/Intel-xHCI function found by the
 * PCI scan. Safe to call more than once. */
void usb_legacy_handoff_all(const struct usb_legacy_platform *plat, struct usb_legacy_report *out);

#endif
