/* =============================================================================
 *  nvme.h -- standalone NVMe 1.4 driver (NVM command set) for x86_64 bare metal
 *
 *  Freestanding: needs only <stdint.h>, <stddef.h>, <stdbool.h>. The
 *  controller is driven by polling the completion queues' phase tags (no
 *  interrupts), so it works before an interrupt controller or scheduler
 *  exists.
 *
 *  Typical use, after a PCI scan found class 01.08 (prog-if 02) and enabled
 *  MMIO + bus mastering on it:
 *
 *      nvme_init(bar0_phys, NULL);             // BAR0/1 = 64-bit register BAR
 *      for (int i = 0; i < nvme_namespace_count(); i++) {
 *          struct nvme_namespace *ns = nvme_namespace_get(i);
 *          nvme_read(ns, lba, count, buffer);  // NVM Read, PRP-described buffer
 *      }
 *
 *  One admin queue pair and one I/O queue pair per controller. Requests are
 *  synchronous: the caller's thread submits, rings the doorbell and polls
 *  for the completion. A per-controller lock serialises requests from
 *  different namespaces sharing the I/O queue.
 *
 *  Platform requirements (all overridable through struct nvme_platform):
 *    - Queues, PRP lists and the identify buffer come from dma_alloc():
 *      physically contiguous and 4 KiB aligned. NVMe addresses all 64 bits,
 *      so no 4 GiB limit applies.
 *    - Caller buffers may be scattered physically; every 4 KiB page is
 *      translated separately through virt_to_phys(). They must be 4-byte
 *      aligned (PRP entries have dword granularity).
 *    - The default hooks assume an identity mapping (virtual == physical)
 *      and allocate from a static, page-aligned pool in .bss.
 *    - The register BAR should be mapped uncached.
 * ============================================================================= */
#ifndef NVME_DRIVER_H
#define NVME_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NVME_MAX_CTRLS
#define NVME_MAX_CTRLS          4
#endif
#ifndef NVME_MAX_NAMESPACES
#define NVME_MAX_NAMESPACES     16
#endif
#ifndef NVME_ADMIN_QUEUE_DEPTH
#define NVME_ADMIN_QUEUE_DEPTH  32          /* entries, clamped to CAP.MQES + 1  */
#endif
#ifndef NVME_IO_QUEUE_DEPTH
#define NVME_IO_QUEUE_DEPTH     64          /* one page of SQEs                  */
#endif
#ifndef NVME_MAX_TRANSFER
#define NVME_MAX_TRANSFER       (1024u * 1024)  /* bytes per command; MDTS may lower it */
#endif
#ifndef NVME_STATIC_POOL
#define NVME_STATIC_POOL        (128 * 1024)    /* default DMA pool (no allocator hook) */
#endif

#define NVME_PAGE_SIZE          4096u       /* CC.MPS = 0: the only page size used */

/* ============================================================================
 *  Controller registers (BAR0, NVMe 1.4 section 3.1)
 * ============================================================================ */
struct nvme_regs {
    uint64_t cap;           /* 0x00 CAP     controller capabilities              */
    uint32_t vs;            /* 0x08 VS      version (major 31:16, minor 15:8)    */
    uint32_t intms;         /* 0x0C INTMS   interrupt mask set                   */
    uint32_t intmc;         /* 0x10 INTMC   interrupt mask clear                 */
    uint32_t cc;            /* 0x14 CC      controller configuration             */
    uint32_t reserved0;     /* 0x18                                              */
    uint32_t csts;          /* 0x1C CSTS    controller status                    */
    uint32_t nssr;          /* 0x20 NSSR    NVM subsystem reset                  */
    uint32_t aqa;           /* 0x24 AQA     admin queue attributes               */
    uint64_t asq;           /* 0x28 ASQ     admin submission queue base          */
    uint64_t acq;           /* 0x30 ACQ     admin completion queue base          */
    uint32_t cmbloc;        /* 0x38 CMBLOC  controller memory buffer location    */
    uint32_t cmbsz;         /* 0x3C CMBSZ                                        */
    uint32_t bpinfo;        /* 0x40 BPINFO  boot partition information           */
    uint32_t bprsel;        /* 0x44 BPRSEL                                       */
    uint64_t bpmbl;         /* 0x48 BPMBL                                        */
    uint64_t cmbmsc;        /* 0x50 CMBMSC                                       */
    uint32_t cmbsts;        /* 0x58 CMBSTS                                       */
    uint8_t  reserved1[0xE00 - 0x5C];
    uint8_t  pmr[0x1000 - 0xE00];       /* 0xE00 persistent memory region regs  */
    uint32_t doorbell[];    /* 0x1000 SQ0TDBL, CQ0HDBL, SQ1TDBL ... (stride CAP.DSTRD) */
} __attribute__((packed, aligned(4)));

_Static_assert(offsetof(struct nvme_regs, cc)       == 0x14,   "CC at 0x14");
_Static_assert(offsetof(struct nvme_regs, csts)     == 0x1C,   "CSTS at 0x1C");
_Static_assert(offsetof(struct nvme_regs, aqa)      == 0x24,   "AQA at 0x24");
_Static_assert(offsetof(struct nvme_regs, asq)      == 0x28,   "ASQ at 0x28");
_Static_assert(offsetof(struct nvme_regs, acq)      == 0x30,   "ACQ at 0x30");
_Static_assert(offsetof(struct nvme_regs, cmbsts)   == 0x58,   "CMBSTS at 0x58");
_Static_assert(offsetof(struct nvme_regs, doorbell) == 0x1000, "doorbells at 0x1000");

/* CAP (64 bits) */
#define NVME_CAP_MQES(c)        ((uint32_t)((c) & 0xFFFF))         /* max entries - 1   */
#define NVME_CAP_CQR            (1ull << 16)    /* contiguous queues required      */
#define NVME_CAP_AMS_WRR        (1ull << 17)    /* weighted round robin supported  */
#define NVME_CAP_AMS_VENDOR     (1ull << 18)
#define NVME_CAP_TO(c)          ((uint32_t)(((c) >> 24) & 0xFF))  /* 500 ms units      */
#define NVME_CAP_DSTRD(c)       ((uint32_t)(((c) >> 32) & 0xF))   /* stride 4 << DSTRD */
#define NVME_CAP_NSSRS          (1ull << 36)    /* NVM subsystem reset supported   */
#define NVME_CAP_CSS(c)         ((uint32_t)(((c) >> 37) & 0xFF))
#define NVME_CAP_CSS_NVM        0x01u           /* NVM command set                 */
#define NVME_CAP_CSS_IOCS       0x40u           /* I/O command set selection       */
#define NVME_CAP_CSS_ADMIN_ONLY 0x80u
#define NVME_CAP_BPS            (1ull << 45)    /* boot partitions                 */
#define NVME_CAP_MPSMIN(c)      ((uint32_t)(((c) >> 48) & 0xF))   /* 2^(12+n) bytes    */
#define NVME_CAP_MPSMAX(c)      ((uint32_t)(((c) >> 52) & 0xF))
#define NVME_CAP_PMRS           (1ull << 56)
#define NVME_CAP_CMBS           (1ull << 57)

/* CC */
#define NVME_CC_EN              (1u << 0)
#define NVME_CC_CSS_NVM         (0u << 4)       /* bits 6:4                        */
#define NVME_CC_CSS_MASK        (7u << 4)
#define NVME_CC_MPS(n)          ((uint32_t)(n) << 7)    /* 2^(12+n), bits 10:7     */
#define NVME_CC_MPS_MASK        (0xFu << 7)
#define NVME_CC_AMS_RR          (0u << 11)      /* bits 13:11                      */
#define NVME_CC_AMS_MASK        (7u << 11)
#define NVME_CC_SHN_NONE        (0u << 14)      /* bits 15:14                      */
#define NVME_CC_SHN_NORMAL      (1u << 14)
#define NVME_CC_SHN_ABRUPT      (2u << 14)
#define NVME_CC_SHN_MASK        (3u << 14)
#define NVME_CC_IOSQES(n)       ((uint32_t)(n) << 16)   /* 2^n bytes, bits 19:16   */
#define NVME_CC_IOCQES(n)       ((uint32_t)(n) << 20)   /* 2^n bytes, bits 23:20   */
#define NVME_CC_IOQES_MASK      (0xFFu << 16)

/* CSTS */
#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)       /* controller fatal status         */
#define NVME_CSTS_SHST_MASK     (3u << 2)
#define NVME_CSTS_SHST_NORMAL   (0u << 2)
#define NVME_CSTS_SHST_BUSY     (1u << 2)       /* shutdown processing occurring   */
#define NVME_CSTS_SHST_DONE     (2u << 2)       /* shutdown processing complete    */
#define NVME_CSTS_NSSRO         (1u << 4)
#define NVME_CSTS_PP            (1u << 5)       /* processing paused               */

/* AQA: queue sizes are 0's based, 12 bits each */
#define NVME_AQA(sq_entries, cq_entries) \
    ((uint32_t)(((sq_entries) - 1) & 0xFFF) | ((uint32_t)(((cq_entries) - 1) & 0xFFF) << 16))

/* ============================================================================
 *  Queue entries (host memory)
 * ============================================================================ */

/* Submission queue entry: 64 bytes (common command format, section 4.2) */
struct nvme_sqe {
    uint8_t  opcode;        /* CDW0  7:0                                        */
    uint8_t  flags;         /* CDW0 15:8  FUSE 9:8, PSDT 15:14 (0 = PRPs)       */
    uint16_t cid;           /* CDW0 31:16 command identifier                    */
    uint32_t nsid;          /* CDW1                                              */
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;          /* CDW4-5  metadata pointer                          */
    uint64_t prp1;          /* CDW6-7  data pointer: PRP entry 1                 */
    uint64_t prp2;          /* CDW8-9  PRP entry 2, or PRP list pointer          */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed, aligned(4)));

/* Completion queue entry: 16 bytes (section 4.6) */
struct nvme_cqe {
    uint32_t result;        /* DW0  command specific                             */
    uint32_t rsvd;          /* DW1                                               */
    uint16_t sq_head;       /* DW2 15:0  SQ head pointer                         */
    uint16_t sq_id;         /* DW2 31:16 SQ identifier                           */
    uint16_t cid;           /* DW3 15:0  command identifier                      */
    uint16_t status;        /* DW3 31:16 bit 0 = phase tag, 15:1 = status field  */
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct nvme_sqe) == 64, "SQE is 64 bytes");
_Static_assert(offsetof(struct nvme_sqe, prp1) == 24, "PRP1 in CDW6");
_Static_assert(offsetof(struct nvme_sqe, cdw10) == 40, "CDW10 at byte 40");
_Static_assert(sizeof(struct nvme_cqe) == 16, "CQE is 16 bytes");
_Static_assert(offsetof(struct nvme_cqe, status) == 14, "status in DW3 31:16");

#define NVME_SQES_LOG2          6           /* 64-byte SQE  -> CC.IOSQES */
#define NVME_CQES_LOG2          4           /* 16-byte CQE  -> CC.IOCQES */

/* CQE status field (the upper 15 bits of `status`) */
#define NVME_CQE_PHASE          0x0001u
#define NVME_STATUS_SC(s)       (((s) >> 1) & 0xFFu)    /* status code             */
#define NVME_STATUS_SCT(s)      (((s) >> 9) & 0x7u)     /* status code type        */
#define NVME_STATUS_MORE        (1u << 14)
#define NVME_STATUS_DNR         (1u << 15)              /* do not retry            */
#define NVME_SCT_GENERIC        0
#define NVME_SCT_CMD_SPECIFIC   1
#define NVME_SCT_MEDIA          2
#define NVME_SC_SUCCESS         0x00
#define NVME_SC_LBA_RANGE       0x80                    /* generic: LBA out of range */

/* Admin command opcodes */
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_ABORT        0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A

/* NVM command set opcodes */
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

/* Create I/O CQ / SQ (CDW11) */
#define NVME_QUEUE_PHYS_CONTIG  (1u << 0)
#define NVME_CQ_IRQ_ENABLED     (1u << 1)
#define NVME_SQ_PRIO_MEDIUM     (2u << 1)

/* Identify CNS values */
#define NVME_CNS_NAMESPACE      0x00
#define NVME_CNS_CONTROLLER     0x01
#define NVME_CNS_ACTIVE_NS_LIST 0x02

/* Feature identifiers */
#define NVME_FEAT_NUM_QUEUES    0x07

/* Identify Controller data structure (4096 bytes, fields used here) */
struct nvme_id_ctrl {
    uint16_t vid;           /*   0 PCI vendor ID                               */
    uint16_t ssvid;         /*   2 subsystem vendor ID                         */
    char     sn[20];        /*   4 serial number (ASCII, space padded)         */
    char     mn[40];        /*  24 model number                                */
    char     fr[8];         /*  64 firmware revision                           */
    uint8_t  rab;           /*  72 recommended arbitration burst               */
    uint8_t  ieee[3];       /*  73                                             */
    uint8_t  cmic;          /*  76                                             */
    uint8_t  mdts;          /*  77 max data transfer: 2^n * CAP.MPSMIN, 0 = none */
    uint16_t cntlid;        /*  78                                             */
    uint32_t ver;           /*  80                                             */
    uint32_t rtd3r;         /*  84 RTD3 resume latency (us)                    */
    uint32_t rtd3e;         /*  88 RTD3 entry latency (us): shutdown budget    */
    uint8_t  rsvd92[512 - 92];
    uint8_t  sqes;          /* 512 required (3:0) / max (7:4) SQE size, log2   */
    uint8_t  cqes;          /* 513 required / max CQE size, log2               */
    uint16_t maxcmd;        /* 514                                             */
    uint32_t nn;            /* 516 number of namespaces (highest NSID)         */
    uint16_t oncs;          /* 520 optional NVM commands                       */
    uint16_t fuses;         /* 522                                             */
    uint8_t  fna;           /* 524                                             */
    uint8_t  vwc;           /* 525 bit 0: volatile write cache present         */
    uint8_t  rsvd526[4096 - 526];
} __attribute__((packed));

/* LBA format descriptor */
struct nvme_lbaf {
    uint16_t ms;            /* metadata bytes per LBA                          */
    uint8_t  lbads;         /* LBA data size, log2 (9 = 512 B)                 */
    uint8_t  rp;            /* bits 1:0 relative performance                   */
} __attribute__((packed));

/* Identify Namespace data structure (4096 bytes, fields used here) */
struct nvme_id_ns {
    uint64_t nsze;          /*   0 namespace size in LBAs                      */
    uint64_t ncap;          /*   8 capacity                                    */
    uint64_t nuse;          /*  16 utilisation                                 */
    uint8_t  nsfeat;        /*  24                                             */
    uint8_t  nlbaf;         /*  25 number of LBA formats - 1                   */
    uint8_t  flbas;         /*  26 bits 3:0 (+6:5) format index, bit 4 extended LBA */
    uint8_t  mc;            /*  27                                             */
    uint8_t  dpc;           /*  28                                             */
    uint8_t  dps;           /*  29 end-to-end protection type                  */
    uint8_t  rsvd30[104 - 30];
    uint8_t  nguid[16];     /* 104 namespace globally unique identifier        */
    uint8_t  eui64[8];      /* 120                                             */
    struct nvme_lbaf lbaf[64];  /* 128                                         */
    uint8_t  rsvd384[4096 - 384];
} __attribute__((packed));

_Static_assert(sizeof(struct nvme_id_ctrl) == 4096, "Identify Controller is 4 KiB");
_Static_assert(offsetof(struct nvme_id_ctrl, mdts) == 77, "MDTS at byte 77");
_Static_assert(offsetof(struct nvme_id_ctrl, rtd3e) == 88, "RTD3E at byte 88");
_Static_assert(offsetof(struct nvme_id_ctrl, nn) == 516, "NN at byte 516");
_Static_assert(offsetof(struct nvme_id_ctrl, vwc) == 525, "VWC at byte 525");
_Static_assert(sizeof(struct nvme_id_ns) == 4096, "Identify Namespace is 4 KiB");
_Static_assert(offsetof(struct nvme_id_ns, nguid) == 104, "NGUID at byte 104");
_Static_assert(offsetof(struct nvme_id_ns, lbaf) == 128, "LBAF table at byte 128");

/* ============================================================================
 *  Driver API
 * ============================================================================ */

/* Error codes (negative return values) */
#define NVME_OK              0
#define NVME_ERR_NODEV      -1      /* no such namespace / controller failed  */
#define NVME_ERR_TIMEOUT    -2      /* controller did not respond in time     */
#define NVME_ERR_IO         -3      /* command completed with an error status */
#define NVME_ERR_FATAL      -4      /* CSTS.CFS: controller fatal status      */
#define NVME_ERR_RANGE      -5      /* LBA range beyond the end of the namespace */
#define NVME_ERR_ALIGN      -6      /* buffer not dword aligned               */
#define NVME_ERR_NOMEM      -7      /* DMA allocator exhausted                */
#define NVME_ERR_UNSUPPORTED -8     /* page size, command set or LBA format   */

struct nvme_platform {
    /* Physically contiguous, `align`-aligned, zeroed memory; physical address
     * in *phys. NULL hook: static identity-mapped pool. */
    void    *(*dma_alloc)(size_t size, size_t align, uint64_t *phys);
    /* Translate a caller buffer address to physical. NULL: identity. */
    uint64_t (*virt_to_phys)(const void *virt);
    /* Map `size` bytes of MMIO at `phys` uncached. NULL: identity. */
    void    *(*map_mmio)(uint64_t phys, size_t size);
    /* Busy-wait. NULL: port-0x80 writes (~1 us each on PC hardware). */
    void     (*delay_us)(uint32_t us);
    /* Called while waiting for the controller lock. NULL: `pause`. */
    void     (*yield)(void);
    /* Debug output, one character at a time. NULL: polled COM1 (0x3F8). */
    void     (*log_putc)(char c);
};

struct nvme_namespace {
    int      ctrl;              /* controller index                         */
    uint32_t nsid;              /* namespace ID (1-based)                   */
    uint64_t blocks;            /* NSZE: capacity in logical blocks         */
    uint32_t block_size;        /* bytes per logical block                  */
    uint32_t max_blocks;        /* per command (MDTS / NVME_MAX_TRANSFER)   */
    bool     volatile_cache;    /* controller has a volatile write cache    */
    char     model[41];         /* controller model, serial and firmware    */
    char     serial[21];
    char     firmware[9];
    uint8_t  eui64[8];
    uint8_t  nguid[16];
};

/* Reset the controller at physical BAR `bar` (BAR0/1, 64-bit), set up the
 * admin queues, enable it, create one I/O queue pair and identify every
 * active namespace. `plat` may be NULL. Returns the number of usable
 * namespaces on this controller, or a negative NVME_ERR_*. May be called
 * once per controller. */
int  nvme_init(uint64_t bar, const struct nvme_platform *plat);

int                    nvme_namespace_count(void);
struct nvme_namespace *nvme_namespace_get(int index);

/* Synchronous transfers of `count` logical blocks starting at `lba`. `buf`
 * must be 4-byte aligned and hold count * block_size bytes; it is passed to
 * the controller page by page through PRP entries (PRP1, PRP2 or a chained
 * PRP list). Requests larger than max_blocks are split. */
int nvme_read (struct nvme_namespace *ns, uint64_t lba, uint32_t count, void *buf);
int nvme_write(struct nvme_namespace *ns, uint64_t lba, uint32_t count, const void *buf);
int nvme_flush(struct nvme_namespace *ns);

/* Normal shutdown (CC.SHN = 01b) of every controller, for power-off and
 * reboot: the controller flushes its caches and records a clean shutdown. */
void nvme_shutdown_all(void);

const char *nvme_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* NVME_DRIVER_H */
