/* linux/mod_devicetable.h -- Kestrel's Linux KPI: device ID tables */
#ifndef _LKPI_LINUX_MOD_DEVICETABLE_H
#define _LKPI_LINUX_MOD_DEVICETABLE_H

#include <linux/types.h>

typedef unsigned long kernel_ulong_t;

#define PCI_ANY_ID (~0)

struct pci_device_id {
    __u32 vendor, device;
    __u32 subvendor, subdevice;
    __u32 class, class_mask;
    kernel_ulong_t driver_data;
    __u32 override_only;
};

#define PLATFORM_NAME_SIZE 20
struct platform_device_id {
    char name[PLATFORM_NAME_SIZE];
    kernel_ulong_t driver_data;
};

struct of_device_id {
    char name[32];
    char type[32];
    char compatible[128];
    const void *data;
};

#define ACPI_ID_LEN 16
struct acpi_device_id {
    __u8 id[ACPI_ID_LEN];
    kernel_ulong_t driver_data;
    __u32 cls;
    __u32 cls_msk;
};

#define USB_DEVICE_ID_MATCH_VENDOR 0x0001
struct usb_device_id {
    __u16 match_flags;
    __u16 idVendor, idProduct;
    __u16 bcdDevice_lo, bcdDevice_hi;
    __u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol;
    __u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    __u8 bInterfaceNumber;
    kernel_ulong_t driver_info;
};

#endif
