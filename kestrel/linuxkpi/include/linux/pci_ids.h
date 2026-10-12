/* linux/pci_ids.h -- Kestrel's Linux KPI: PCI class codes and vendor/device IDs
 * (the IDs are the vendors' own numbers; extend as drivers need them) */
#ifndef _LKPI_LINUX_PCI_IDS_H
#define _LKPI_LINUX_PCI_IDS_H

#define PCI_CLASS_NOT_DEFINED           0x0000
#define PCI_BASE_CLASS_STORAGE          0x01
#define PCI_CLASS_STORAGE_SCSI          0x0100
#define PCI_CLASS_STORAGE_IDE           0x0101
#define PCI_CLASS_STORAGE_SATA          0x0106
#define PCI_CLASS_STORAGE_SATA_AHCI     0x010601
#define PCI_CLASS_STORAGE_EXPRESS       0x010802
#define PCI_BASE_CLASS_NETWORK          0x02
#define PCI_CLASS_NETWORK_ETHERNET      0x0200
#define PCI_BASE_CLASS_DISPLAY          0x03
#define PCI_CLASS_DISPLAY_VGA           0x0300
#define PCI_CLASS_DISPLAY_XGA           0x0301
#define PCI_CLASS_DISPLAY_3D            0x0302
#define PCI_CLASS_DISPLAY_OTHER         0x0380
#define PCI_BASE_CLASS_MULTIMEDIA       0x04
#define PCI_CLASS_MULTIMEDIA_AUDIO      0x0401
#define PCI_CLASS_MULTIMEDIA_HD_AUDIO   0x0403
#define PCI_BASE_CLASS_MEMORY           0x05
#define PCI_BASE_CLASS_BRIDGE           0x06
#define PCI_CLASS_BRIDGE_HOST           0x0600
#define PCI_CLASS_BRIDGE_ISA            0x0601
#define PCI_CLASS_BRIDGE_PCI            0x0604
#define PCI_CLASS_BRIDGE_OTHER          0x0680
#define PCI_BASE_CLASS_COMMUNICATION    0x07
#define PCI_BASE_CLASS_SYSTEM           0x08
#define PCI_CLASS_SYSTEM_OTHER          0x0880
#define PCI_BASE_CLASS_SERIAL           0x0c
#define PCI_CLASS_SERIAL_USB            0x0c03
#define PCI_CLASS_SERIAL_USB_XHCI       0x0c0330
#define PCI_CLASS_SERIAL_SMBUS          0x0c05
#define PCI_CLASS_OTHERS                0xff

#define PCI_VENDOR_ID_AMD               0x1022
#define PCI_VENDOR_ID_ATI               0x1002
#define PCI_VENDOR_ID_NVIDIA            0x10de
#define PCI_VENDOR_ID_NVIDIA_SGS        0x12d2
#define PCI_VENDOR_ID_INTEL             0x8086
#define PCI_VENDOR_ID_REDHAT_QUMRANET   0x1af4
#define PCI_VENDOR_ID_REDHAT            0x1b36
#define PCI_VENDOR_ID_QEMU              0x1234
#define PCI_VENDOR_ID_BOCHS             0x1234   /* the "Bochs/QEMU" VGA */
#define PCI_VENDOR_ID_VMWARE            0x15ad
#define PCI_VENDOR_ID_REALTEK           0x10ec
#define PCI_VENDOR_ID_BROADCOM          0x14e4
#define PCI_VENDOR_ID_CIRRUS            0x1013
#define PCI_SUBVENDOR_ID_REDHAT_QUMRANET 0x1af4
#define PCI_SUBDEVICE_ID_QEMU           0x1100

#define PCI_DEVICE_ID_INTEL_ESB_9       0x25ab   /* 6300ESB watchdog timer */
#define PCI_DEVICE_ID_INTEL_82801IR_1   0x2916

#endif
