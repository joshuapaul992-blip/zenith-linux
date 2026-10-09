/* =============================================================================
 *  xhci.h -- standalone xHCI (USB 3.x host controller) driver with HID boot
 *            keyboard and mouse support, for x86_64 bare metal
 *
 *  Freestanding: needs only <stdint.h>, <stddef.h>, <stdbool.h>.
 *
 *      xhci_init(bar0_phys, &platform);  // BAR0 of a class 0C.03.30 function,
 *                                        // MMIO decoding + bus mastering enabled
 *      ...then call xhci_poll() regularly (timer tick, 1-8 ms) to receive
 *      key/mouse events through the platform callbacks, and xhci_service()
 *      from a thread to handle devices plugged in or removed after boot.
 *
 *  Model: one interrupter (0), polled event ring, one command ring, one
 *  transfer ring per endpoint. Devices must be attached to root-hub ports
 *  (external hubs are detected and reported but not enumerated).
 *  Reference: eXtensible Host Controller Interface for USB, revision 1.2.
 * ============================================================================= */
#ifndef XHCI_DRIVER_H
#define XHCI_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XHCI_MAX_SLOTS
#define XHCI_MAX_SLOTS          16      /* device slots enabled (CONFIG.MaxSlotsEn) */
#endif
#ifndef XHCI_MAX_PORTS
#define XHCI_MAX_PORTS          32
#endif
#ifndef XHCI_STATIC_POOL
#define XHCI_STATIC_POOL        (512 * 1024)    /* default DMA pool (no allocator hook) */
#endif
#define XHCI_CMD_RING_TRBS      64
#define XHCI_EVENT_RING_TRBS    256
#define XHCI_EP_RING_TRBS       64
#define XHCI_HID_BUFFERS        8               /* report TRBs kept queued per endpoint */

/* ============================================================================
 *  MMIO register layouts (all little endian, 32-bit accesses)
 * ============================================================================ */

/* Capability registers: BAR0 + 0 */
struct xhci_cap_regs {
    uint8_t  caplength;         /* 0x00 operational registers offset          */
    uint8_t  reserved;
    uint16_t hciversion;        /* 0x02 BCD interface version (0x0100 = 1.0)   */
    uint32_t hcsparams1;        /* 0x04 MaxSlots 7:0, MaxIntrs 18:8, MaxPorts 31:24 */
    uint32_t hcsparams2;        /* 0x08 IST, ERST max, scratchpad count        */
    uint32_t hcsparams3;        /* 0x0C U1/U2 exit latencies                   */
    uint32_t hccparams1;        /* 0x10 AC64, CSZ, PPC, xECP 31:16             */
    uint32_t dboff;             /* 0x14 doorbell array offset                  */
    uint32_t rtsoff;            /* 0x18 runtime registers offset               */
    uint32_t hccparams2;        /* 0x1C                                        */
} __attribute__((packed, aligned(4)));

/* Per-port register set: operational base + 0x400 + 0x10 * (port - 1) */
struct xhci_port_regs {
    uint32_t portsc;            /* status and control                          */
    uint32_t portpmsc;          /* power management status and control         */
    uint32_t portli;            /* link info                                   */
    uint32_t porthlpmc;         /* hardware LPM control                        */
} __attribute__((packed, aligned(4)));

/* Operational registers: BAR0 + CAPLENGTH */
struct xhci_op_regs {
    uint32_t usbcmd;            /* 0x00 */
    uint32_t usbsts;            /* 0x04 */
    uint32_t pagesize;          /* 0x08 bit n set = 2^(n+12) byte pages        */
    uint32_t reserved0[2];
    uint32_t dnctrl;            /* 0x14 device notification control            */
    uint32_t crcr_lo;           /* 0x18 command ring control                   */
    uint32_t crcr_hi;
    uint32_t reserved1[4];
    uint32_t dcbaap_lo;         /* 0x30 device context base address array ptr  */
    uint32_t dcbaap_hi;
    uint32_t config;            /* 0x38 MaxSlotsEn 7:0                          */
    uint32_t reserved2[241];    /* 0x3C - 0x3FF                                 */
    struct xhci_port_regs ports[256];   /* 0x400                               */
} __attribute__((packed, aligned(4)));

/* Interrupter register set: runtime base + 0x20 + 32 * n */
struct xhci_intr_regs {
    uint32_t iman;              /* IP bit 0 (RW1C), IE bit 1                   */
    uint32_t imod;              /* interval 15:0 (250 ns units), counter 31:16 */
    uint32_t erstsz;            /* event ring segment table size               */
    uint32_t reserved;
    uint32_t erstba_lo;         /* event ring segment table base               */
    uint32_t erstba_hi;
    uint32_t erdp_lo;           /* event ring dequeue pointer; EHB bit 3 RW1C  */
    uint32_t erdp_hi;
} __attribute__((packed, aligned(4)));

/* Runtime registers: BAR0 + RTSOFF */
struct xhci_runtime_regs {
    uint32_t mfindex;           /* 0x00 microframe index                       */
    uint32_t reserved[7];
    struct xhci_intr_regs ir[1024];     /* 0x20                                */
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct xhci_cap_regs) == 0x20, "capability registers");
_Static_assert(offsetof(struct xhci_op_regs, crcr_lo) == 0x18, "CRCR at 0x18");
_Static_assert(offsetof(struct xhci_op_regs, dcbaap_lo) == 0x30, "DCBAAP at 0x30");
_Static_assert(offsetof(struct xhci_op_regs, config) == 0x38, "CONFIG at 0x38");
_Static_assert(offsetof(struct xhci_op_regs, ports) == 0x400, "port sets at 0x400");
_Static_assert(sizeof(struct xhci_intr_regs) == 0x20, "interrupter set is 32 bytes");
_Static_assert(offsetof(struct xhci_runtime_regs, ir) == 0x20, "IR0 at runtime + 0x20");

/* HCSPARAMS / HCCPARAMS fields */
#define XHCI_HCS1_MAX_SLOTS(p)  ((p) & 0xFF)
#define XHCI_HCS1_MAX_INTRS(p)  (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(p)  (((p) >> 24) & 0xFF)
#define XHCI_HCS2_ERST_MAX(p)   (((p) >> 4) & 0xF)
#define XHCI_HCS2_SCRATCHPADS(p) ((((p) >> 21) & 0x1F) << 5 | (((p) >> 27) & 0x1F))
#define XHCI_HCC1_AC64          (1u << 0)
#define XHCI_HCC1_CSZ           (1u << 2)       /* 64-byte contexts             */
#define XHCI_HCC1_PPC           (1u << 3)       /* port power control           */
#define XHCI_HCC1_XECP(p)       (((p) >> 16) & 0xFFFF)

/* USBCMD / USBSTS */
#define XHCI_CMD_RS             (1u << 0)
#define XHCI_CMD_HCRST          (1u << 1)
#define XHCI_CMD_INTE           (1u << 2)
#define XHCI_CMD_HSEE           (1u << 3)
#define XHCI_STS_HCH            (1u << 0)       /* halted                       */
#define XHCI_STS_HSE            (1u << 2)       /* host system error            */
#define XHCI_STS_EINT           (1u << 3)
#define XHCI_STS_PCD            (1u << 4)       /* port change detect           */
#define XHCI_STS_CNR            (1u << 11)      /* controller not ready         */
#define XHCI_STS_HCE            (1u << 12)      /* host controller error        */

/* CRCR */
#define XHCI_CRCR_RCS           (1u << 0)
#define XHCI_CRCR_CRR           (1u << 3)

/* IMAN / ERDP */
#define XHCI_IMAN_IP            (1u << 0)
#define XHCI_IMAN_IE            (1u << 1)
#define XHCI_ERDP_EHB           (1u << 3)

/* PORTSC */
#define XHCI_PORTSC_CCS         (1u << 0)       /* current connect status       */
#define XHCI_PORTSC_PED         (1u << 1)       /* enabled (RW1C: 1 disables!)  */
#define XHCI_PORTSC_OCA         (1u << 3)
#define XHCI_PORTSC_PR          (1u << 4)       /* port reset                   */
#define XHCI_PORTSC_PLS_SHIFT   5
#define XHCI_PORTSC_PLS_MASK    (0xFu << 5)
#define XHCI_PORTSC_PP          (1u << 9)       /* port power                   */
#define XHCI_PORTSC_SPEED(p)    (((p) >> 10) & 0xF)
#define XHCI_PORTSC_LWS         (1u << 16)
#define XHCI_PORTSC_CSC         (1u << 17)      /* connect status change        */
#define XHCI_PORTSC_PEC         (1u << 18)
#define XHCI_PORTSC_WRC         (1u << 19)      /* warm reset change            */
#define XHCI_PORTSC_OCC         (1u << 20)
#define XHCI_PORTSC_PRC         (1u << 21)      /* port reset change            */
#define XHCI_PORTSC_PLC         (1u << 22)
#define XHCI_PORTSC_CEC         (1u << 23)
#define XHCI_PORTSC_CHANGES     (0x7Fu << 17)   /* all RW1C change bits         */
#define XHCI_PORTSC_WPR         (1u << 31)      /* warm port reset (USB3)       */
/* Bits that must be written back unchanged; everything else written as 0 so
 * that RW1C bits are not cleared and PED is not disabled by accident. */
#define XHCI_PORTSC_PRESERVE    ((1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30) | \
                                 (0xFu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25))

/* Port speed IDs (default PSI) */
#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4
#define XHCI_SPEED_SUPER_PLUS   5

/* Extended capability IDs */
#define XHCI_XCAP_LEGACY        1
#define XHCI_XCAP_PROTOCOL      2
#define XHCI_LEGACY_BIOS_OWNED  (1u << 16)
#define XHCI_LEGACY_OS_OWNED    (1u << 24)

/* ============================================================================
 *  In-memory structures shared with the controller
 * ============================================================================ */

/* Transfer Request Block: 16 bytes */
struct xhci_trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;           /* cycle bit 0, type 15:10, ...                 */
} __attribute__((packed, aligned(16)));
_Static_assert(sizeof(struct xhci_trb) == 16, "TRB is 16 bytes");

#define TRB_CYCLE               (1u << 0)
#define TRB_TC                  (1u << 1)       /* Link TRB: toggle cycle       */
#define TRB_ENT                 (1u << 1)       /* evaluate next TRB            */
#define TRB_ISP                 (1u << 2)       /* interrupt on short packet    */
#define TRB_CH                  (1u << 4)       /* chain                        */
#define TRB_IOC                 (1u << 5)       /* interrupt on completion      */
#define TRB_IDT                 (1u << 6)       /* immediate data               */
#define TRB_BSR                 (1u << 9)       /* Address Device: block SET_ADDRESS */
#define TRB_DIR_IN              (1u << 16)      /* Data/Status stage direction  */
#define TRB_TYPE(t)             ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)         (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)             ((uint32_t)(s) << 24)
#define TRB_GET_SLOT(c)         (((c) >> 24) & 0xFF)
#define TRB_EP(e)               ((uint32_t)(e) << 16)
#define TRB_GET_EP(c)           (((c) >> 16) & 0x1F)
#define TRB_TRT_NO_DATA         (0u << 16)      /* Setup Stage transfer type    */
#define TRB_TRT_OUT             (2u << 16)
#define TRB_TRT_IN              (3u << 16)
#define TRB_GET_CODE(s)         (((s) >> 24) & 0xFF)
#define TRB_GET_LEN(s)          ((s) & 0xFFFFFF)

enum xhci_trb_type {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13, TRB_RESET_EP = 14,
    TRB_STOP_EP = 15, TRB_SET_TR_DEQUEUE = 16, TRB_RESET_DEVICE = 17, TRB_NOOP_CMD = 23,
    TRB_EV_TRANSFER = 32, TRB_EV_CMD_COMPLETION = 33, TRB_EV_PORT_STATUS = 34,
    TRB_EV_HOST_CONTROLLER = 37,
};

enum xhci_completion {
    CC_SUCCESS = 1, CC_DATA_BUFFER = 2, CC_BABBLE = 3, CC_USB_TRANSACTION = 4,
    CC_TRB_ERROR = 5, CC_STALL = 6, CC_RESOURCE = 7, CC_BANDWIDTH = 8,
    CC_NO_SLOTS = 9, CC_SHORT_PACKET = 13, CC_CONTEXT_STATE = 19,
    CC_PARAMETER = 17, CC_EVENT_RING_FULL = 21,
};

/* Event Ring Segment Table entry */
struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;              /* TRBs in the segment (16 - 4096)             */
    uint32_t reserved;
} __attribute__((packed));

/* Contexts are 32 or 64 bytes (HCCPARAMS1.CSZ); only the first 32 are
 * defined. Index helpers live in xhci.c. */
struct xhci_slot_ctx {
    uint32_t dw0;               /* route string 19:0, speed 23:20, context entries 31:27 */
    uint32_t dw1;               /* max exit latency 15:0, root hub port 23:16  */
    uint32_t dw2;               /* TT info, interrupter target 31:22           */
    uint32_t dw3;               /* USB address 7:0, slot state 31:27           */
    uint32_t reserved[4];
} __attribute__((packed));

struct xhci_ep_ctx {
    uint32_t dw0;               /* EP state 2:0, interval 23:16                */
    uint32_t dw1;               /* CErr 2:1, EP type 5:3, max burst 15:8, MPS 31:16 */
    uint64_t deq;               /* TR dequeue pointer | DCS                     */
    uint32_t dw4;               /* average TRB length 15:0, max ESIT payload 31:16 */
    uint32_t reserved[3];
} __attribute__((packed));

struct xhci_input_ctrl_ctx {
    uint32_t drop_flags;
    uint32_t add_flags;         /* bit 0 slot, bit n endpoint DCI n            */
    uint32_t reserved[5];
    uint32_t dw7;               /* configuration value, interface, alternate   */
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_slot_ctx) == 32, "slot context");
_Static_assert(sizeof(struct xhci_ep_ctx) == 32, "endpoint context");
_Static_assert(sizeof(struct xhci_input_ctrl_ctx) == 32, "input control context");

#define EP_TYPE_CONTROL         4
#define EP_TYPE_INTERRUPT_IN    7
#define EP_TYPE_BULK_OUT        2
#define EP_TYPE_BULK_IN         6

/* A producer ring (command ring or transfer ring) */
struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t phys;
    uint32_t size;              /* TRBs including the trailing Link TRB         */
    uint32_t enq;               /* next slot to write                           */
    uint8_t  cycle;             /* producer cycle state                         */
    uint8_t  tag[XHCI_EP_RING_TRBS];    /* caller data per TRB (HID buffer #)  */
};

/* The consumer ring for events */
struct xhci_event_ring {
    struct xhci_trb *trbs;
    uint64_t phys;
    uint32_t size;
    uint32_t deq;
    uint8_t  cycle;             /* consumer cycle state                         */
    struct xhci_erst_entry *erst;
    uint64_t erst_phys;
};

/* Ring helpers (exposed for reuse by future class drivers) */
bool     xhci_ring_init(struct xhci_ring *r, uint32_t trbs);
uint64_t xhci_ring_enqueue(struct xhci_ring *r, uint64_t param, uint32_t status, uint32_t control);
bool     xhci_event_dequeue(struct xhci_event_ring *er, struct xhci_trb *out);

/* ============================================================================
 *  Driver API
 * ============================================================================ */
#define XHCI_OK              0
#define XHCI_ERR_NODEV      -1
#define XHCI_ERR_TIMEOUT    -2
#define XHCI_ERR_HC         -3      /* controller error / halted              */
#define XHCI_ERR_NOMEM      -4
#define XHCI_ERR_XFER       -5      /* transfer failed (stall, transaction)   */
#define XHCI_ERR_CMD        -6      /* command completed with an error code   */
#define XHCI_ERR_UNSUPPORTED -7
#define XHCI_ERR_STALL      -8      /* endpoint stalled (host side recovered)   */
#define XHCI_ERR_BUSY       -9      /* another thread holds the controller      */

#define XHCI_BULK_MAX       65536   /* largest single bulk transfer (one TRB)   */

struct usb_key_event {
    uint8_t usage;              /* HID usage ID (page 0x07)                     */
    uint8_t modifiers;          /* HID_MOD_* at the time of the event           */
    bool    pressed;            /* false = released                             */
    bool    repeat;             /* generated by the driver's typematic repeat  */
    char    ascii;              /* US layout translation, 0 if none             */
    uint8_t device;             /* index into xhci_device()                     */
};

struct usb_mouse_event {
    int16_t dx, dy, wheel;
    uint8_t buttons;            /* bit 0 left, 1 right, 2 middle                */
    uint8_t device;
};

struct xhci_platform {
    void    *(*dma_alloc)(size_t size, size_t align, uint64_t *phys);  /* zeroing optional */
    void    *(*map_mmio)(uint64_t phys, size_t size);
    void     (*delay_us)(uint32_t us);
    uint64_t (*now_ms)(void);           /* optional: enables key repeat           */
    void     (*log_putc)(char c);
    void     (*on_key)(const struct usb_key_event *ev);
    void     (*on_mouse)(const struct usb_mouse_event *ev);
};

enum usb_dev_kind { USB_KIND_OTHER = 0, USB_KIND_KEYBOARD, USB_KIND_MOUSE, USB_KIND_HUB, USB_KIND_STORAGE };

struct usb_device_info {
    bool     present;
    uint8_t  slot, port, speed;
    uint16_t vendor, product;
    uint8_t  dev_class;
    uint8_t  kind;              /* enum usb_dev_kind of the first HID interface */
    uint8_t  hid_count;
    char     manufacturer[32];
    char     product_name[48];
    uint32_t reports;           /* HID reports received                         */
    uint32_t generation;        /* bumped on every (re)enumeration of the slot  */
};

/* Mass-storage (Bulk-Only Transport) interface of an enumerated device */
struct xhci_msc_info {
    uint8_t  slot, port, speed, iface, subclass, protocol;
    uint8_t  in_ep, out_ep, in_burst, out_burst;
    uint16_t in_mps, out_mps;
    uint32_t generation;
    const struct usb_device_info *dev;
};

/* Bring up the controller at physical BAR0 `mmio` and enumerate all
 * devices already connected. Returns the number of HID interfaces
 * configured, or a negative XHCI_ERR_*. One controller is supported. */
int  xhci_init(uint64_t mmio, const struct xhci_platform *plat);

/* Drain the event ring: completes commands/transfers, dispatches HID
 * reports, re-arms report buffers, generates key repeat and records port
 * changes. Non-blocking; safe from a timer interrupt. */
void xhci_poll(void);

/* Thread context: enumerate newly connected ports, release removed
 * devices. Blocks while devices are addressed and configured. */
void xhci_service(void);

/* Boot-time device settling. Non-blocking: polls every root port's PORTSC,
 * debounces new connections (100 ms), enumerates them, retries failed
 * enumerations (3 attempts, growing back-off) and notices removals.
 * Returns the number of ports still debouncing/retrying (0 = settled), or
 * -1 if another thread is using the controller right now. Call it in a loop
 * until it returns 0 or your own deadline expires. */
int  xhci_settle_step(void);

/* Class-driver interface (thread context; serialised internally). */
int  xhci_msc_count(void);
int  xhci_msc_get(int index, struct xhci_msc_info *out);       /* by index      */
int  xhci_msc_lookup(uint8_t slot, struct xhci_msc_info *out); /* still there?  */
int  xhci_control_transfer(uint8_t slot, uint8_t reqtype, uint8_t request, uint16_t value,
                           uint16_t index, uint16_t length, void *data);
/* One bulk transfer of up to XHCI_BULK_MAX bytes through the device's DMA
 * bounce buffer (`buf` may be any memory). On timeout the TD is cancelled
 * (Stop Endpoint + Set TR Dequeue); on stall the host endpoint is reset and
 * XHCI_ERR_STALL returned so the caller can clear the device-side halt. */
int  xhci_bulk_transfer(uint8_t slot, bool in, void *buf, uint32_t len,
                        uint32_t *actual, uint32_t timeout_ms);
/* CLEAR_FEATURE(ENDPOINT_HALT) on the device + host endpoint reset. */
int  xhci_clear_halt(uint8_t slot, bool in);

/* Human-readable controller state: operational/runtime registers, the
 * firmware handoff result, every port's PORTSC decoded, slots and last
 * error codes. Used for boot-failure diagnostics. */
void xhci_dump_state(void (*putc_fn)(char c));
bool xhci_running(void);

int  xhci_device_count(void);
const struct usb_device_info *xhci_device(int index);
const char *xhci_speed_name(uint8_t speed);

/* US-layout translation of a keyboard usage, honouring Shift/Caps Lock. */
char usb_hid_usage_to_ascii(uint8_t usage, uint8_t modifiers, bool caps_lock);

#ifdef __cplusplus
}
#endif

#endif /* XHCI_DRIVER_H */
