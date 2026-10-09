/* =============================================================================
 *  usb.h -- USB 2.0 / 3.x chapter 9 and HID 1.11 definitions used by the
 *           xHCI driver (freestanding, no dependencies)
 * ============================================================================= */
#ifndef USB_DEFS_H
#define USB_DEFS_H

#include <stdint.h>
#include <stdbool.h>

/* ---- standard requests (USB 2.0 table 9-4) -------------------------------- */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE     0x0B

/* bmRequestType */
#define USB_DIR_OUT               0x00
#define USB_DIR_IN                0x80
#define USB_TYPE_STANDARD         0x00
#define USB_TYPE_CLASS            0x20
#define USB_RECIP_DEVICE          0x00
#define USB_RECIP_INTERFACE       0x01
#define USB_RECIP_ENDPOINT        0x02

/* descriptor types */
#define USB_DT_DEVICE             0x01
#define USB_DT_CONFIG             0x02
#define USB_DT_STRING             0x03
#define USB_DT_INTERFACE          0x04
#define USB_DT_ENDPOINT           0x05
#define USB_DT_HID                0x21
#define USB_DT_REPORT             0x22
#define USB_DT_SS_EP_COMPANION    0x30

/* device / interface classes */
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_HID             0x03
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09

/* HID (HID 1.11 section 7.2 and appendix B) */
#define HID_REQ_GET_REPORT        0x01
#define HID_REQ_GET_IDLE          0x02
#define HID_REQ_GET_PROTOCOL      0x03
#define HID_REQ_SET_REPORT        0x09
#define HID_REQ_SET_IDLE          0x0A
#define HID_REQ_SET_PROTOCOL      0x0B
#define HID_SUBCLASS_BOOT         0x01
#define HID_PROTOCOL_KEYBOARD     0x01
#define HID_PROTOCOL_MOUSE        0x02
#define HID_BOOT_PROTOCOL         0
#define HID_REPORT_PROTOCOL       1

/* endpoint descriptor bmAttributes */
#define USB_EP_XFER_MASK          0x03
#define USB_EP_XFER_CONTROL       0x00
#define USB_EP_XFER_ISOC          0x01
#define USB_EP_XFER_BULK          0x02
#define USB_EP_XFER_INT           0x03

/* ---- wire structures (little endian, packed) ------------------------------ */
struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;       /* 8/16/32/64; 2^n for USB 3 (9 = 512) */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;      /* bit 7: IN */
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;        /* bits 10:0 size, 12:11 extra transactions */
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_hid_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

_Static_assert(sizeof(struct usb_setup_packet) == 8, "setup packet");
_Static_assert(sizeof(struct usb_device_descriptor) == 18, "device descriptor");
_Static_assert(sizeof(struct usb_config_descriptor) == 9, "config descriptor");
_Static_assert(sizeof(struct usb_interface_descriptor) == 9, "interface descriptor");
_Static_assert(sizeof(struct usb_endpoint_descriptor) == 7, "endpoint descriptor");

/* ---- HID boot protocol reports (HID 1.11 appendix B) ---------------------- */
struct hid_boot_keyboard_report {
    uint8_t modifiers;              /* HID_MOD_* bits                          */
    uint8_t reserved;
    uint8_t keys[6];                /* usage IDs (page 0x07), 0 = none, 1 = rollover */
} __attribute__((packed));

struct hid_boot_mouse_report {
    uint8_t buttons;                /* bit 0 left, 1 right, 2 middle           */
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;                  /* present on most mice (4th byte)         */
} __attribute__((packed));

#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_LGUI    0x08
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_RALT    0x40
#define HID_MOD_RGUI    0x80
#define HID_MOD_CTRL    (HID_MOD_LCTRL | HID_MOD_RCTRL)
#define HID_MOD_SHIFT   (HID_MOD_LSHIFT | HID_MOD_RSHIFT)
#define HID_MOD_ALT     (HID_MOD_LALT | HID_MOD_RALT)

/* Keyboard/keypad usage IDs (HID Usage Tables, page 0x07) used by callers */
#define HID_KEY_A           0x04
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESCAPE      0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_CAPSLOCK    0x39
#define HID_KEY_F1          0x3A
#define HID_KEY_F12         0x45
#define HID_KEY_INSERT      0x49
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_DELETE      0x4C
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52
#define HID_KEY_KP_ENTER    0x58
#define HID_KEY_ERR_ROLLOVER 0x01

#endif /* USB_DEFS_H */
