/* =============================================================================
 *  pci.h -- standalone PCI / PCIe bus enumerator for x86_64 bare metal
 *
 *  Uses PCI Configuration Mechanism #1 (I/O ports 0xCF8 address, 0xCFC data).
 *  Self-contained: needs only the compiler's freestanding headers
 *  (<stdint.h>, <stddef.h>, <stdbool.h>) and a ring-0 x86_64 environment.
 *
 *  Typical use:
 *      if (pci_enumerate(PCI_SCAN_RECURSIVE, PCI_SCAN_LOG | PCI_SCAN_SIZE_BARS) > 0) {
 *          for (size_t i = 0; i < pci_match_count(PCI_MATCH_NVME); i++) {
 *              struct pci_device *nvme = pci_match_get(PCI_MATCH_NVME, i);
 *              uint64_t regs = nvme->bars[0].base;     // NVMe registers (BAR0/1)
 *              ...
 *          }
 *      }
 *
 *  Scope: Mechanism #1 reaches the legacy 256-byte configuration space of
 *  every PCI and PCIe function. The PCIe extended space (offsets 0x100-0xFFF)
 *  needs ECAM/MMCONFIG from the ACPI MCFG table and is out of scope here.
 *
 *  Compile-time options (define before building pci.c):
 *      PCI_MAX_TRACKED          functions kept in the device pool   (256)
 *      PCI_MAX_MATCHES          references kept per match category   (32)
 *      PCI_NO_MEM_FALLBACK      don't provide weak memset/memcpy
 *      PCI_PORT_IO_HOOKS        route port I/O through extern hooks (tests)
 * ============================================================================= */
#ifndef PCI_ENUM_H
#define PCI_ENUM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- configuration mechanism #1 ------------------------------------------- */
#define PCI_CONFIG_ADDRESS      0x0CF8
#define PCI_CONFIG_DATA         0x0CFC
#define PCI_CONFIG_ENABLE       0x80000000u

#define PCI_MAX_BUSES           256
#define PCI_MAX_DEVICES         32
#define PCI_MAX_FUNCTIONS       8

#ifndef PCI_MAX_TRACKED
#define PCI_MAX_TRACKED         256
#endif
#ifndef PCI_MAX_MATCHES
#define PCI_MAX_MATCHES         32
#endif

/* ---- configuration-space register offsets ---------------------------------- */
#define PCI_REG_VENDOR_ID       0x00    /* 16 */
#define PCI_REG_DEVICE_ID       0x02    /* 16 */
#define PCI_REG_COMMAND         0x04    /* 16 */
#define PCI_REG_STATUS          0x06    /* 16 */
#define PCI_REG_REVISION        0x08    /*  8 */
#define PCI_REG_PROG_IF         0x09    /*  8 */
#define PCI_REG_SUBCLASS        0x0A    /*  8 */
#define PCI_REG_CLASS           0x0B    /*  8 */
#define PCI_REG_HEADER_TYPE     0x0E    /*  8 */
#define PCI_REG_BAR0            0x10    /* 32 x 6 (type 0), x 2 (type 1) */
#define PCI_REG_PRIMARY_BUS     0x18    /*  8, type 1 */
#define PCI_REG_SECONDARY_BUS   0x19    /*  8, type 1 */
#define PCI_REG_SUBORDINATE_BUS 0x1A    /*  8, type 1 */
#define PCI_REG_CAP_PTR         0x34    /*  8 */
#define PCI_REG_INTERRUPT_LINE  0x3C    /*  8 */
#define PCI_REG_INTERRUPT_PIN   0x3D    /*  8 */

#define PCI_CMD_IO_SPACE        0x0001
#define PCI_CMD_MEM_SPACE       0x0002
#define PCI_CMD_BUS_MASTER      0x0004
#define PCI_CMD_INTX_DISABLE    0x0400

#define PCI_STATUS_CAP_LIST     0x0010

#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_HEADER_MULTIFUNC    0x80
#define PCI_HEADER_GENERAL      0x00
#define PCI_HEADER_PCI_BRIDGE   0x01
#define PCI_HEADER_CARDBUS      0x02

#define PCI_VENDOR_INVALID      0xFFFF
#define PCI_VENDOR_NVIDIA       0x10DE

/* ---- class codes used by the filters ----------------------------------- */
#define PCI_CLASS_STORAGE       0x01
#define   PCI_SUBCLASS_SATA     0x06    /* prog-if 0x01 = AHCI 1.0          */
#define   PCI_SUBCLASS_NVM      0x08    /* prog-if 0x02 = NVM Express       */
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_BRIDGE        0x06
#define   PCI_SUBCLASS_HOST_BRIDGE   0x00
#define   PCI_SUBCLASS_PCI_BRIDGE    0x04
#define   PCI_SUBCLASS_PCI_BRIDGE_ST 0x09   /* semi-transparent PCI-to-PCI  */
#define PCI_CLASS_SERIAL_BUS    0x0C
#define   PCI_SUBCLASS_USB      0x03
#define     PCI_PROGIF_XHCI     0x30

/* ============================================================================
 *  Packed configuration-space layouts (first 64 bytes of each function)
 * ============================================================================ */

/* Bytes 0x00-0x0F: identical for every header type */
struct pci_header_common {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t  revision_id;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  cache_line_size;
    uint8_t  latency_timer;
    uint8_t  header_type;           /* bit 7 = multi-function device */
    uint8_t  bist;
} __attribute__((packed));

/* Header type 0x00: endpoints (controllers, GPUs, ...) */
struct pci_header_type0 {
    struct pci_header_common common;
    uint32_t bar[6];                /* 0x10 - 0x24 */
    uint32_t cardbus_cis;           /* 0x28 */
    uint16_t subsystem_vendor_id;   /* 0x2C */
    uint16_t subsystem_id;          /* 0x2E */
    uint32_t expansion_rom;         /* 0x30 */
    uint8_t  capabilities_ptr;      /* 0x34 */
    uint8_t  reserved0[3];
    uint32_t reserved1;             /* 0x38 */
    uint8_t  interrupt_line;        /* 0x3C: legacy PIC IRQ set by firmware */
    uint8_t  interrupt_pin;         /* 0x3D: 0 = none, 1..4 = INTA#..INTD# */
    uint8_t  min_grant;
    uint8_t  max_latency;
} __attribute__((packed));

/* Header type 0x01: PCI-to-PCI bridges (and PCIe root/downstream ports) */
struct pci_header_type1 {
    struct pci_header_common common;
    uint32_t bar[2];                /* 0x10 - 0x14 */
    uint8_t  primary_bus;           /* 0x18 */
    uint8_t  secondary_bus;         /* 0x19: bus directly behind the bridge */
    uint8_t  subordinate_bus;       /* 0x1A: highest bus behind the bridge  */
    uint8_t  secondary_latency;
    uint8_t  io_base;               /* 0x1C */
    uint8_t  io_limit;
    uint16_t secondary_status;
    uint16_t memory_base;           /* 0x20 */
    uint16_t memory_limit;
    uint16_t prefetch_base;         /* 0x24 */
    uint16_t prefetch_limit;
    uint32_t prefetch_base_upper;   /* 0x28 */
    uint32_t prefetch_limit_upper;  /* 0x2C */
    uint16_t io_base_upper;         /* 0x30 */
    uint16_t io_limit_upper;
    uint8_t  capabilities_ptr;      /* 0x34 */
    uint8_t  reserved[3];
    uint32_t expansion_rom;         /* 0x38 */
    uint8_t  interrupt_line;        /* 0x3C */
    uint8_t  interrupt_pin;
    uint16_t bridge_control;
} __attribute__((packed));

/* Raw 64-byte header, viewable as dwords or as either layout */
union pci_config_header {
    uint32_t                 dword[16];
    struct pci_header_common common;
    struct pci_header_type0  type0;
    struct pci_header_type1  type1;
};

_Static_assert(sizeof(struct pci_header_common) == 16, "common header must be 16 bytes");
_Static_assert(sizeof(struct pci_header_type0)  == 64, "type 0 header must be 64 bytes");
_Static_assert(sizeof(struct pci_header_type1)  == 64, "type 1 header must be 64 bytes");
_Static_assert(sizeof(union pci_config_header)  == 64, "config header must be 64 bytes");

/* ============================================================================
 *  Decoded device records
 * ============================================================================ */

enum pci_bar_kind {
    PCI_BAR_UNUSED = 0,
    PCI_BAR_IO,                 /* I/O port range                        */
    PCI_BAR_MEM32,              /* 32-bit MMIO                           */
    PCI_BAR_MEM64,              /* 64-bit MMIO (consumes two BAR slots)  */
    PCI_BAR_MEM16,              /* legacy "below 1 MiB" MMIO             */
    PCI_BAR_MEM64_UPPER,        /* upper half of the previous 64-bit BAR */
};

struct pci_bar {
    uint64_t base;              /* port number or physical address        */
    uint64_t size;              /* bytes; 0 unless sized (PCI_SCAN_SIZE_BARS) */
    uint8_t  kind;              /* enum pci_bar_kind                      */
    bool     prefetchable;
};

/* Match categories (bit flags, a function can match several) */
enum pci_match {
    PCI_MATCH_AHCI           = 1u << 0,   /* class 01, subclass 06          */
    PCI_MATCH_NVME           = 1u << 1,   /* class 01, subclass 08          */
    PCI_MATCH_NVIDIA_DISPLAY = 1u << 2,   /* vendor 10DE, class 03 display  */
    PCI_MATCH_XHCI           = 1u << 3,   /* class 0C, subclass 03, if 30   */
};
#define PCI_MATCH_CATEGORIES 4

struct pci_device {
    uint8_t  bus, device, function;
    uint8_t  header_type;       /* without the multi-function bit */
    bool     multifunction;
    uint16_t vendor_id, device_id;
    uint16_t subsystem_vendor_id, subsystem_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  interrupt_line, interrupt_pin;
    uint8_t  secondary_bus, subordinate_bus;    /* bridges only */
    uint8_t  bar_count;                         /* 6, 2 or 0 by header type */
    struct pci_bar bars[6];
    uint32_t matches;                           /* enum pci_match bits */
    struct pci_device *parent;                  /* upstream bridge, NULL on root bus */
    union pci_config_header config;             /* raw copy at scan time */
};

struct pci_scan_stats {
    uint32_t buses_scanned;
    uint32_t functions_found;
    uint32_t bridges_found;
    uint32_t dropped;           /* functions not stored: pool full */
    uint32_t match_overflow;    /* matches not stored: category full */
    uint64_t config_reads;
};

/* ============================================================================
 *  API
 * ============================================================================ */

/* Raw configuration access (offset is byte-granular; 16/32-bit accesses
 * must be naturally aligned). Interrupts are disabled around each access. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t v);

/* True if the platform answers Configuration Mechanism #1. */
bool pci_mech1_present(void);

enum pci_scan_mode {
    PCI_SCAN_RECURSIVE,     /* walk from the host bridge(s) through bridges  */
    PCI_SCAN_BRUTE_FORCE,   /* probe all 256 x 32 x 8 bus/device/function     */
};
#define PCI_SCAN_LOG        0x01    /* log each function to the debug sink    */
#define PCI_SCAN_SIZE_BARS  0x02    /* measure BAR sizes (writes BARs briefly) */

/* Scan the hierarchy, filling the device pool and match lists. Any previous
 * results are discarded. Returns the number of functions found, or -1 if
 * Configuration Mechanism #1 is not available. */
int pci_enumerate(enum pci_scan_mode mode, unsigned flags);

/* Filtering helper: classify `dev`, store a reference in every matching
 * category list and, if `log` is set, print it to the debug sink.
 * Called by the scanner for each function; exposed for re-filtering. */
uint32_t pci_filter_and_log(struct pci_device *dev, bool log);

/* Pure classifier: which categories do these IDs fall into? */
uint32_t pci_classify(uint16_t vendor, uint8_t class_code, uint8_t subclass, uint8_t prog_if);

size_t                    pci_device_count(void);
struct pci_device        *pci_device_at(size_t index);
struct pci_device        *pci_find(uint8_t bus, uint8_t dev, uint8_t func);
size_t                    pci_match_count(enum pci_match category);
struct pci_device        *pci_match_get(enum pci_match category, size_t index);
const struct pci_scan_stats *pci_stats(void);

/* Turn on I/O decoding, MMIO decoding and/or bus mastering for a driver. */
void pci_enable(struct pci_device *dev, bool io, bool mmio, bool bus_master);

/* Human-readable names (static strings) */
const char *pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if);
const char *pci_match_name(enum pci_match category);

/* Debug output: defaults to polled COM1 (0x3F8). pci_debug_init() programs
 * the UART for 115200 8N1; skip it if your kernel already did. A custom sink
 * receives one character at a time. */
void pci_debug_init(void);
void pci_set_log_sink(void (*putc_fn)(char c));
void pci_log_device(const struct pci_device *dev);
void pci_log_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* PCI_ENUM_H */
