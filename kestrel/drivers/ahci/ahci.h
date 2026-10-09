/* =============================================================================
 *  ahci.h -- standalone AHCI 1.3 SATA driver for x86_64 bare metal
 *
 *  Freestanding: needs only <stdint.h>, <stddef.h>, <stdbool.h>. The HBA is
 *  driven entirely by polling (no interrupts) through the command list, so
 *  it works before an interrupt controller or scheduler exists.
 *
 *  Typical use, after a PCI scan found class 01.06 and enabled MMIO + bus
 *  mastering on it:
 *
 *      ahci_init(abar_phys, NULL);            // BAR5 = AHCI Base Address (ABAR)
 *      for (int i = 0; i < ahci_disk_count(); i++) {
 *          struct ahci_disk *d = ahci_disk_get(i);
 *          ahci_verify_mbr(d, 3);            // dumps the MBR signature on COM1
 *          ahci_read(d, lba, count, buffer); // 48-bit LBA, READ DMA EXT
 *      }
 *
 *  Platform requirements (all overridable through struct ahci_platform):
 *    - DMA memory must be physically contiguous per allocation, below 4 GiB
 *      unless the HBA reports 64-bit addressing (CAP.S64A).
 *    - The default hooks assume an identity mapping (virtual == physical)
 *      and allocate from a static, page-aligned pool in .bss.
 *    - ABAR should be mapped uncached (PCD/PWT or a UC MTRR range).
 * ============================================================================= */
#ifndef AHCI_DRIVER_H
#define AHCI_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AHCI_MAX_HBAS
#define AHCI_MAX_HBAS        4
#endif
#ifndef AHCI_MAX_DISKS
#define AHCI_MAX_DISKS       32
#endif
#ifndef AHCI_PRDT_ENTRIES
#define AHCI_PRDT_ENTRIES    8          /* PRDs per command table (max 65535) */
#endif
#ifndef AHCI_STATIC_POOL
#define AHCI_STATIC_POOL     (128 * 1024)   /* default DMA pool (no allocator hook) */
#endif

/* ============================================================================
 *  HBA memory space (ABAR)
 * ============================================================================ */

/* Per-port register block: 0x80 bytes at ABAR + 0x100 + port * 0x80 */
struct ahci_port_regs {
    uint32_t clb;           /* 0x00 PxCLB   command list base (1 KiB aligned)   */
    uint32_t clbu;          /* 0x04 PxCLBU  upper 32 bits                       */
    uint32_t fb;            /* 0x08 PxFB    received-FIS base (256 B aligned)   */
    uint32_t fbu;           /* 0x0C PxFBU                                       */
    uint32_t is;            /* 0x10 PxIS    interrupt status (RW1C)             */
    uint32_t ie;            /* 0x14 PxIE    interrupt enable                    */
    uint32_t cmd;           /* 0x18 PxCMD   command and status                  */
    uint32_t reserved0;     /* 0x1C                                             */
    uint32_t tfd;           /* 0x20 PxTFD   task file data (ATA status/error)   */
    uint32_t sig;           /* 0x24 PxSIG   device signature                    */
    uint32_t ssts;          /* 0x28 PxSSTS  SStatus (SCR0)                      */
    uint32_t sctl;          /* 0x2C PxSCTL  SControl (SCR2)                     */
    uint32_t serr;          /* 0x30 PxSERR  SError (SCR1, RW1C)                 */
    uint32_t sact;          /* 0x34 PxSACT  SActive (NCQ)                       */
    uint32_t ci;            /* 0x38 PxCI    command issue                       */
    uint32_t sntf;          /* 0x3C PxSNTF  SNotification                       */
    uint32_t fbs;           /* 0x40 PxFBS   FIS-based switching control         */
    uint32_t devslp;        /* 0x44 PxDEVSLP                                    */
    uint32_t reserved1[10]; /* 0x48 - 0x6F                                      */
    uint32_t vendor[4];     /* 0x70 - 0x7F                                      */
} __attribute__((packed, aligned(4)));

/* Generic host control: ABAR + 0x00 */
struct ahci_hba_regs {
    uint32_t cap;           /* 0x00 CAP     host capabilities                   */
    uint32_t ghc;           /* 0x04 GHC     global host control                 */
    uint32_t is;            /* 0x08 IS      interrupt status (one bit per port) */
    uint32_t pi;            /* 0x0C PI      ports implemented                   */
    uint32_t vs;            /* 0x10 VS      version                             */
    uint32_t ccc_ctl;       /* 0x14 command completion coalescing control       */
    uint32_t ccc_ports;     /* 0x18                                             */
    uint32_t em_loc;        /* 0x1C enclosure management location               */
    uint32_t em_ctl;        /* 0x20                                             */
    uint32_t cap2;          /* 0x24 CAP2    extended capabilities               */
    uint32_t bohc;          /* 0x28 BOHC    BIOS/OS handoff control and status  */
    uint32_t reserved[29];  /* 0x2C - 0x9F                                      */
    uint32_t vendor[24];    /* 0xA0 - 0xFF                                      */
    struct ahci_port_regs ports[32];    /* 0x100 - 0x10FF                       */
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct ahci_port_regs) == 0x80, "port block is 0x80 bytes");
_Static_assert(offsetof(struct ahci_port_regs, tfd) == 0x20, "PxTFD at 0x20");
_Static_assert(offsetof(struct ahci_port_regs, ssts) == 0x28, "PxSSTS at 0x28");
_Static_assert(offsetof(struct ahci_port_regs, ci) == 0x38, "PxCI at 0x38");
_Static_assert(offsetof(struct ahci_hba_regs, bohc) == 0x28, "BOHC at 0x28");
_Static_assert(offsetof(struct ahci_hba_regs, ports) == 0x100, "ports at 0x100");
_Static_assert(sizeof(struct ahci_hba_regs) == 0x1100, "ABAR region is 0x1100 bytes");

/* CAP */
#define AHCI_CAP_NP_MASK        0x1Fu           /* ports - 1               */
#define AHCI_CAP_NCS_SHIFT      8               /* command slots - 1       */
#define AHCI_CAP_NCS_MASK       0x1Fu
#define AHCI_CAP_SAM            (1u << 18)      /* AHCI-only (no legacy)   */
#define AHCI_CAP_SCLO           (1u << 24)      /* command list override   */
#define AHCI_CAP_SSS            (1u << 27)      /* staggered spin-up       */
#define AHCI_CAP_SNCQ           (1u << 30)
#define AHCI_CAP_S64A           (1u << 31)      /* 64-bit DMA addressing   */
/* CAP2 / BOHC */
#define AHCI_CAP2_BOH           (1u << 0)
#define AHCI_BOHC_BOS           (1u << 0)       /* BIOS owns HBA           */
#define AHCI_BOHC_OOS           (1u << 1)       /* OS owns HBA (request)   */
#define AHCI_BOHC_BB            (1u << 4)       /* BIOS busy               */
/* GHC */
#define AHCI_GHC_HR             (1u << 0)       /* HBA reset               */
#define AHCI_GHC_IE             (1u << 1)       /* interrupt enable        */
#define AHCI_GHC_AE             (1u << 31)      /* AHCI enable             */

/* PxCMD */
#define AHCI_PxCMD_ST           (1u << 0)       /* start (DMA engine)      */
#define AHCI_PxCMD_SUD          (1u << 1)       /* spin-up device          */
#define AHCI_PxCMD_POD          (1u << 2)       /* power on device         */
#define AHCI_PxCMD_CLO          (1u << 3)       /* command list override   */
#define AHCI_PxCMD_FRE          (1u << 4)       /* FIS receive enable      */
#define AHCI_PxCMD_FR           (1u << 14)      /* FIS receive running     */
#define AHCI_PxCMD_CR           (1u << 15)      /* command list running    */
#define AHCI_PxCMD_ATAPI        (1u << 24)
#define AHCI_PxCMD_ICC_MASK     (0xFu << 28)
#define AHCI_PxCMD_ICC_ACTIVE   (1u << 28)
/* PxIS */
#define AHCI_PxIS_DHRS          (1u << 0)       /* D2H register FIS        */
#define AHCI_PxIS_PSS           (1u << 1)       /* PIO setup FIS           */
#define AHCI_PxIS_OFS           (1u << 24)      /* overflow                */
#define AHCI_PxIS_INFS          (1u << 26)      /* interface non-fatal     */
#define AHCI_PxIS_IFS           (1u << 27)      /* interface fatal         */
#define AHCI_PxIS_HBDS          (1u << 28)      /* host bus data error     */
#define AHCI_PxIS_HBFS          (1u << 29)      /* host bus fatal error    */
#define AHCI_PxIS_TFES          (1u << 30)      /* task file error         */
#define AHCI_PxIS_ERRORS        (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | \
                                 AHCI_PxIS_IFS | AHCI_PxIS_INFS | AHCI_PxIS_OFS)
/* PxTFD (ATA status in bits 7:0, error register in 15:8) */
#define AHCI_TFD_ERR            0x01
#define AHCI_TFD_DRQ            0x08
#define AHCI_TFD_DF             0x20
#define AHCI_TFD_BSY            0x80
/* PxSSTS */
#define AHCI_SSTS_DET_MASK      0x0Fu
#define AHCI_SSTS_DET_PRESENT   0x03u           /* device + PHY established */
#define AHCI_SSTS_SPD_SHIFT     4
#define AHCI_SSTS_IPM_SHIFT     8
#define AHCI_SSTS_IPM_ACTIVE    0x01u
/* PxSCTL */
#define AHCI_SCTL_DET_COMRESET  0x01u
#define AHCI_SCTL_IPM_NO_PS     (0x3u << 8)     /* no partial/slumber      */
/* PxSIG */
#define AHCI_SIG_ATA            0x00000101u
#define AHCI_SIG_ATAPI          0xEB140101u
#define AHCI_SIG_SEMB           0xC33C0101u
#define AHCI_SIG_PM             0x96690101u

/* ============================================================================
 *  FIS and command structures (in host memory, read/written by the HBA)
 * ============================================================================ */
#define FIS_TYPE_REG_H2D        0x27
#define FIS_TYPE_REG_D2H        0x34
#define FIS_TYPE_DMA_SETUP      0x41
#define FIS_TYPE_PIO_SETUP      0x5F
#define FIS_TYPE_DEV_BITS       0xA1

/* Register FIS, host to device (20 bytes) */
struct fis_reg_h2d {
    uint8_t fis_type;       /* FIS_TYPE_REG_H2D                           */
    uint8_t flags;          /* bit 7: C (1 = command), bits 3:0 PM port   */
    uint8_t command;        /* ATA command                                */
    uint8_t feature_lo;
    uint8_t lba0, lba1, lba2;
    uint8_t device;         /* bit 6: LBA mode                            */
    uint8_t lba3, lba4, lba5;
    uint8_t feature_hi;
    uint8_t count_lo, count_hi;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed));

/* Register FIS, device to host (20 bytes) */
struct fis_reg_d2h {
    uint8_t fis_type, flags, status, error;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, reserved0;
    uint8_t count_lo, count_hi, reserved1[2];
    uint8_t reserved2[4];
} __attribute__((packed));

/* Received-FIS area: 256 bytes, 256-byte aligned (PxFB) */
struct ahci_received_fis {
    uint8_t dsfis[0x1C];    /* 0x00 DMA setup FIS          */
    uint8_t pad0[4];
    uint8_t psfis[0x14];    /* 0x20 PIO setup FIS          */
    uint8_t pad1[12];
    struct fis_reg_d2h rfis;/* 0x40 D2H register FIS       */
    uint8_t pad2[4];
    uint8_t sdbfis[8];      /* 0x58 set-device-bits FIS    */
    uint8_t ufis[64];       /* 0x60 unknown FIS            */
    uint8_t reserved[0x60]; /* 0xA0 - 0xFF                 */
} __attribute__((packed));

/* Command header: 32 bytes, 32 per command list (1 KiB, PxCLB) */
struct ahci_cmd_header {
    uint16_t flags;         /* bits 4:0 CFL (FIS length in dwords), 5 A (ATAPI),
                               6 W (write), 7 P (prefetch), 8 R (reset), 9 B (BIST),
                               10 C (clear busy on R_OK), 15:12 PMP              */
    uint16_t prdtl;         /* PRDT entries in the command table             */
    volatile uint32_t prdbc;/* bytes transferred (written by the HBA)        */
    uint32_t ctba;          /* command table base (128-byte aligned)         */
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

#define AHCI_CMDH_CFL(dw)       ((uint16_t)((dw) & 0x1F))
#define AHCI_CMDH_ATAPI         (1u << 5)
#define AHCI_CMDH_WRITE         (1u << 6)
#define AHCI_CMDH_PREFETCH      (1u << 7)
#define AHCI_CMDH_CLEAR_BUSY    (1u << 10)

/* Physical region descriptor: 16 bytes */
struct ahci_prd {
    uint32_t dba;           /* data base address (bit 0 must be 0)           */
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;           /* bits 21:0 byte count - 1 (max 4 MiB, even),
                               bit 31 interrupt on completion                */
} __attribute__((packed));
#define AHCI_PRD_MAX_BYTES      (4u * 1024 * 1024)

/* Command table: 128-byte aligned; CFIS + ATAPI command + PRDT */
struct ahci_cmd_table {
    uint8_t cfis[64];       /* 0x00 command FIS                              */
    uint8_t acmd[16];       /* 0x40 ATAPI command                            */
    uint8_t reserved[48];   /* 0x50                                          */
    struct ahci_prd prdt[AHCI_PRDT_ENTRIES];   /* 0x80                       */
} __attribute__((packed));

_Static_assert(sizeof(struct fis_reg_h2d) == 20, "H2D FIS is 20 bytes");
_Static_assert(sizeof(struct ahci_received_fis) == 256, "received FIS is 256 bytes");
_Static_assert(offsetof(struct ahci_received_fis, rfis) == 0x40, "D2H FIS at 0x40");
_Static_assert(sizeof(struct ahci_cmd_header) == 32, "command header is 32 bytes");
_Static_assert(sizeof(struct ahci_prd) == 16, "PRD is 16 bytes");
_Static_assert(offsetof(struct ahci_cmd_table, prdt) == 0x80, "PRDT at 0x80");

/* ATA commands used */
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_DEVICE_LBA          0x40

/* ============================================================================
 *  Driver API
 * ============================================================================ */

/* Error codes (negative return values) */
#define AHCI_OK              0
#define AHCI_ERR_NODEV      -1      /* no such disk / port not ready          */
#define AHCI_ERR_TIMEOUT    -2      /* HBA or device did not respond in time  */
#define AHCI_ERR_IO         -3      /* device reported an error (TFD.ERR/DF)  */
#define AHCI_ERR_HBA        -4      /* host bus / interface error             */
#define AHCI_ERR_RANGE      -5      /* LBA range beyond the end of the disk   */
#define AHCI_ERR_ALIGN      -6      /* buffer not 2-byte aligned / above 4 GiB */
#define AHCI_ERR_NOMEM      -7      /* DMA allocator exhausted                */
#define AHCI_ERR_BUSY       -8      /* no free command slot                   */

struct ahci_platform {
    /* Physically contiguous, `align`-aligned memory; physical address in
     * *phys. NULL hook: static identity-mapped pool. */
    void    *(*dma_alloc)(size_t size, size_t align, uint64_t *phys);
    /* Translate a caller buffer address to physical. NULL: identity. */
    uint64_t (*virt_to_phys)(const void *virt);
    /* Map `size` bytes of MMIO at `phys` uncached. NULL: identity. */
    void    *(*map_mmio)(uint64_t phys, size_t size);
    /* Busy-wait. NULL: port-0x80 writes (~1 us each on PC hardware). */
    void     (*delay_us)(uint32_t us);
    /* Debug output, one character at a time. NULL: polled COM1 (0x3F8). */
    void     (*log_putc)(char c);
};

struct ahci_disk {
    int      hba;               /* controller index                      */
    int      port;              /* SATA port number 0-31                 */
    uint64_t sectors;           /* capacity in logical sectors           */
    uint32_t sector_size;       /* logical sector size in bytes          */
    bool     lba48;
    uint8_t  link_speed;        /* 1 = 1.5, 2 = 3.0, 3 = 6.0 Gb/s        */
    char     model[41];
    char     serial[21];
    char     firmware[9];
    uint16_t identify[256];     /* raw IDENTIFY DEVICE data              */
};

/* Take ownership of the HBA at physical ABAR `abar`, switch it to AHCI
 * mode, initialise every implemented port and IDENTIFY attached disks.
 * `plat` may be NULL. Returns the number of ATA disks found on this HBA,
 * or a negative AHCI_ERR_*. May be called once per controller. */
int  ahci_init(uint64_t abar, const struct ahci_platform *plat);

int               ahci_disk_count(void);
struct ahci_disk *ahci_disk_get(int index);

/* Synchronous transfers (48-bit LBA, DMA through the command list).
 * `buf` must be 2-byte aligned and hold count * sector_size bytes. */
int ahci_read (struct ahci_disk *d, uint64_t lba, uint32_t count, void *buf);
int ahci_write(struct ahci_disk *d, uint64_t lba, uint32_t count, const void *buf);
int ahci_flush(struct ahci_disk *d);

/* Debug verify loop: reads LBA 0 `iterations` times, checks that every
 * read returns identical data, hex-dumps the boot sector's start and the
 * partition table, and reports the 0x55AA signature and partitions on the
 * debug log. Returns AHCI_OK if the signature is valid. */
int ahci_verify_mbr(struct ahci_disk *d, int iterations);

/* Non-destructive write test on the last sector: save, write a pattern,
 * read back, compare, restore, verify the restore. */
int ahci_selftest_rw(struct ahci_disk *d);

const char *ahci_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* AHCI_DRIVER_H */
