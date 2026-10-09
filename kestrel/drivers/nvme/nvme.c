/* =============================================================================
 *  nvme.c -- standalone NVMe driver: controller reset and enable, admin and
 *            I/O queue pairs, Identify, synchronous PRP-based reads/writes.
 *
 *  Reference: NVM Express Base Specification 1.4c, sections 3.1 (controller
 *  registers), 4.3 (PRP entries and lists), 4.6 (completion queue entry),
 *  5 (admin commands), 6 (NVM commands), 7.6.1 (initialisation) and 7.6.2
 *  (shutdown).
 *
 *  Memory per controller (allocated once, physically contiguous, zeroed):
 *      admin SQ / CQ      depth x 64 B / depth x 16 B, 4 KiB aligned
 *      I/O SQ / CQ        depth x 64 B / depth x 16 B, 4 KiB aligned
 *      PRP list pages     enough for one NVME_MAX_TRANSFER command
 *      identify buffers   2 pages (active-namespace list + Identify data)
 *
 *  Polling: a completion is new when its phase tag matches the queue's
 *  expected phase, which starts at 1 and flips every time the head wraps.
 * ============================================================================= */
#include "nvme.h"

/* ---------------------------------------------------------------- tunables */
#define T_CMD_US          5000000u  /* admin or I/O command completion        */
#define T_SHUTDOWN_MIN_US 5000000u  /* shutdown budget when RTD3E is lower    */
#define T_SHUTDOWN_MAX_US 60000000u

#define IO_QID            1
#define PRP_PER_PAGE      (NVME_PAGE_SIZE / 8)              /* 512 entries   */
/* Worst case after PRP1: the transfer starts mid-page, so PRP1 covers a
 * partial page and every following page needs an entry. */
#define PRP_MAX_ENTRIES   ((NVME_MAX_TRANSFER + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE)
/* Each list page but the last gives up its final slot to the chain pointer. */
#define PRP_LIST_PAGES    (PRP_MAX_ENTRIES <= PRP_PER_PAGE ? 1 : \
                           1 + (PRP_MAX_ENTRIES - PRP_PER_PAGE + PRP_PER_PAGE - 2) / (PRP_PER_PAGE - 1))

_Static_assert(NVME_MAX_TRANSFER >= NVME_PAGE_SIZE && NVME_MAX_TRANSFER % NVME_PAGE_SIZE == 0,
               "NVME_MAX_TRANSFER must be a multiple of the 4 KiB page");
_Static_assert(NVME_ADMIN_QUEUE_DEPTH >= 2 && NVME_ADMIN_QUEUE_DEPTH <= 4096, "AQA holds 12-bit sizes");
_Static_assert(NVME_IO_QUEUE_DEPTH >= 2 && NVME_IO_QUEUE_DEPTH <= 65536, "queue size is 16 bits");

/* ======================================================================== */
/*  low-level helpers                                                          */
/* ======================================================================== */
static inline void mmio_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }
static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }
static inline void port_outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t port_inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* Registers are accessed as 32-bit dwords; 64-bit ones low half first, which
 * every controller accepts (section 3.1.1). */
static inline uint32_t rd32(const void *reg) { return *(const volatile uint32_t *)reg; }
static inline void wr32(void *reg, uint32_t v) { *(volatile uint32_t *)reg = v; }
static inline uint64_t rd64(const void *reg)
{
    uint64_t lo = rd32(reg), hi = rd32((const uint8_t *)reg + 4);
    return lo | hi << 32;
}
static inline void wr64(void *reg, uint64_t v)
{
    wr32(reg, (uint32_t)v);
    wr32((uint8_t *)reg + 4, (uint32_t)(v >> 32));
}

/* volatile stores so the compiler never turns these into libc calls */
static void mem_zero(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }

static void copy_string(char *dst, const char *src, size_t n)    /* space padded, not NUL terminated */
{
    size_t len = n;
    while (len && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    size_t lead = 0;
    while (lead < len && src[lead] == ' ') lead++;
    size_t i = 0;
    for (; lead + i < len; i++) dst[i] = src[lead + i];
    dst[i] = 0;
}

/* ======================================================================== */
/*  default platform hooks                                                     */
/* ======================================================================== */
static uint8_t g_pool[NVME_STATIC_POOL] __attribute__((aligned(4096)));
static size_t  g_pool_used;

static void *default_dma_alloc(size_t size, size_t align, uint64_t *phys)
{
    size_t start = (g_pool_used + align - 1) & ~(align - 1);
    if (start + size > sizeof g_pool) return NULL;
    g_pool_used = start + size;
    *phys = (uint64_t)(uintptr_t)&g_pool[start];        /* identity mapped */
    return &g_pool[start];
}
static uint64_t default_virt_to_phys(const void *v) { return (uint64_t)(uintptr_t)v; }
static void *default_map_mmio(uint64_t phys, size_t size) { (void)size; return (void *)(uintptr_t)phys; }
static void default_delay_us(uint32_t us) { while (us--) port_outb(0x80, 0); }
static void default_yield(void) { cpu_pause(); }

static void com1_putc(char c)
{
    for (int spin = 0; spin < 100000 && !(port_inb(0x3F8 + 5) & 0x20); spin++)
        cpu_pause();
    port_outb(0x3F8, (uint8_t)c);
}
static void default_log_putc(char c) { if (c == '\n') com1_putc('\r'); com1_putc(c); }

/* ======================================================================== */
/*  debug output                                                               */
/* ======================================================================== */
static void (*g_putc)(char) = default_log_putc;

static void out_c(char c) { g_putc(c); }
static void out_s(const char *s) { while (*s) out_c(*s++); }
static void out_hex(uint64_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) out_c(hex[(v >> (4 * i)) & 0xF]);
}
static void out_dec(uint64_t v)
{
    char buf[21]; int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_c(buf[--n]);
}
static void out_size(uint64_t bytes)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int i = 0; uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && i < 5) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; i++; }
    out_dec(whole);
    if (frac && whole < 100) { out_c('.'); out_dec(frac); }
    out_c(' '); out_s(u[i]);
}

/* ======================================================================== */
/*  driver state                                                               */
/* ======================================================================== */
struct queue {
    volatile struct nvme_sqe *sq;   uint64_t sq_phys;
    volatile struct nvme_cqe *cq;   uint64_t cq_phys;
    volatile uint32_t *sq_tail_db;          /* SQyTDBL */
    volatile uint32_t *cq_head_db;          /* CQyHDBL */
    uint16_t id, depth;
    uint16_t sq_tail, sq_head;              /* sq_head as last reported by the controller */
    uint16_t cq_head;
    uint16_t phase;                         /* expected phase tag of the next new CQE */
};

struct ctrl {
    struct nvme_regs *regs;
    uint64_t bar;
    uint64_t cap;
    uint32_t vs;
    uint32_t db_stride;                     /* bytes between doorbells: 4 << CAP.DSTRD */
    uint32_t ready_timeout_us;              /* CAP.TO                                   */
    uint32_t shutdown_timeout_us;
    uint32_t max_transfer;                  /* bytes per command                        */
    struct nvme_platform plat;
    struct queue admin, io;
    uint64_t *prp_list[PRP_LIST_PAGES];
    uint64_t  prp_list_phys[PRP_LIST_PAGES];
    uint8_t  *idbuf;  uint64_t idbuf_phys;  /* 2 pages                                  */
    uint16_t next_cid;
    volatile int lock;
    bool     alive;                         /* I/O queues usable                        */
    bool     vwc;
    uint32_t nn;
    char     model[41], serial[21], firmware[9];
};

static struct ctrl            g_ctrl[NVME_MAX_CTRLS];
static int                    g_nctrl;
static struct nvme_namespace  g_ns[NVME_MAX_NAMESPACES];
static int                    g_nns;

static void out_ctrl(const struct ctrl *c)
{
    out_s("nvme"); out_dec((uint64_t)(c - g_ctrl)); out_s(": ");
}

static inline void delay_us(const struct ctrl *c, uint32_t us) { c->plat.delay_us(us); }

/* Poll CSTS until (value & mask) == want. A fatal status ends the wait early
 * unless that is what the caller waits for. */
static int wait_csts(const struct ctrl *c, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
    for (uint32_t t = 0;; t += 100) {
        uint32_t csts = rd32(&c->regs->csts);
        if (csts == 0xFFFFFFFFu) return NVME_ERR_NODEV;            /* device gone from the bus */
        if ((csts & mask) == want) return NVME_OK;
        if ((csts & NVME_CSTS_CFS) && !(mask & NVME_CSTS_CFS) && want != 0) return NVME_ERR_FATAL;
        if (t >= timeout_us) return NVME_ERR_TIMEOUT;
        delay_us(c, 100);
    }
}

static void ctrl_lock(struct ctrl *c)
{
    while (__atomic_exchange_n(&c->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&c->lock, __ATOMIC_RELAXED)) c->plat.yield();
}
static void ctrl_unlock(struct ctrl *c) { __atomic_store_n(&c->lock, 0, __ATOMIC_RELEASE); }

/* ======================================================================== */
/*  queues and command submission                                              */
/* ======================================================================== */

/* Doorbells: SQyTDBL at 0x1000 + (2y) * stride, CQyHDBL at 0x1000 + (2y + 1) * stride. */
static void queue_bind(struct ctrl *c, struct queue *q, uint16_t qid)
{
    uint8_t *db = (uint8_t *)c->regs->doorbell;
    q->id         = qid;
    q->sq_tail_db = (volatile uint32_t *)(db + (2u * qid) * c->db_stride);
    q->cq_head_db = (volatile uint32_t *)(db + (2u * qid + 1) * c->db_stride);
}

/* Empty both rings: after a controller reset the controller's view of every
 * queue is gone, and stale CQEs must not match the expected phase. */
static void queue_reset(struct queue *q)
{
    mem_zero((void *)q->sq, (size_t)q->depth * sizeof(struct nvme_sqe));
    mem_zero((void *)q->cq, (size_t)q->depth * sizeof(struct nvme_cqe));
    q->sq_tail = q->sq_head = q->cq_head = 0;
    q->phase = 1;
}

static void log_status(const struct ctrl *c, const struct nvme_sqe *cmd, uint16_t status)
{
    out_ctrl(c);
    out_s("command 0x"); out_hex(cmd->opcode, 2);
    out_s(" (cid "); out_dec(cmd->cid); out_s(", nsid "); out_dec(cmd->nsid);
    out_s(") failed: SCT "); out_dec(NVME_STATUS_SCT(status));
    out_s(" SC 0x"); out_hex(NVME_STATUS_SC(status), 2);
    if (status & NVME_STATUS_DNR) out_s(" (do not retry)");
    out_c('\n');
}

/* Copy `cmd` into the next SQ slot, ring the tail doorbell and poll the
 * paired CQ until the completion with the same command identifier arrives. */
static int submit_wait(struct ctrl *c, struct queue *q, struct nvme_sqe *cmd, uint32_t *result)
{
    uint16_t next = (uint16_t)((q->sq_tail + 1) % q->depth);
    if (next == q->sq_head) {                       /* cannot happen with one command in flight */
        out_ctrl(c); out_s("submission queue "); out_dec(q->id); out_s(" full\n");
        return NVME_ERR_IO;
    }
    if (++c->next_cid == 0xFFFF) c->next_cid = 0;   /* 0xFFFF means "no command" in error logs */
    cmd->cid = c->next_cid;

    const uint32_t *src = (const uint32_t *)cmd;
    volatile uint32_t *dst = (volatile uint32_t *)&q->sq[q->sq_tail];
    for (size_t i = 0; i < sizeof *cmd / 4; i++) dst[i] = src[i];
    q->sq_tail = next;
    mmio_barrier();                                 /* SQE visible before the doorbell */
    *q->sq_tail_db = q->sq_tail;

    uint32_t waited = 0;
    for (uint32_t spin = 0;; spin++) {
        volatile struct nvme_cqe *e = &q->cq[q->cq_head];
        uint16_t status = e->status;
        if ((status & NVME_CQE_PHASE) == q->phase) {
            __asm__ volatile("" ::: "memory");      /* read the rest after the phase tag */
            uint16_t cid = e->cid, sq_head = e->sq_head;
            uint32_t res = e->result;
            if (++q->cq_head == q->depth) { q->cq_head = 0; q->phase ^= 1; }
            *q->cq_head_db = q->cq_head;            /* hand the slot back */
            q->sq_head = sq_head;
            if (cid != cmd->cid) {                  /* completion of an abandoned command */
                out_ctrl(c); out_s("discarding stale completion for cid "); out_dec(cid); out_c('\n');
                continue;
            }
            if (result) *result = res;
            if (status & ~(NVME_CQE_PHASE | NVME_STATUS_MORE)) {
                log_status(c, cmd, status);
                return NVME_ERR_IO;
            }
            return NVME_OK;
        }
        if (spin < 2000) continue;                  /* fast path: no MMIO, no delay */
        if ((spin & 63) == 0 && (rd32(&c->regs->csts) & NVME_CSTS_CFS)) {
            out_ctrl(c); out_s("controller fatal status during command 0x"); out_hex(cmd->opcode, 2); out_c('\n');
            return NVME_ERR_FATAL;
        }
        if (waited >= T_CMD_US) {
            out_ctrl(c); out_s("command 0x"); out_hex(cmd->opcode, 2); out_s(" on queue ");
            out_dec(q->id); out_s(" timed out, CSTS=0x"); out_hex(rd32(&c->regs->csts), 8); out_c('\n');
            return NVME_ERR_TIMEOUT;
        }
        delay_us(c, 10); waited += 10;
    }
}

static int admin_cmd(struct ctrl *c, struct nvme_sqe *cmd, uint32_t *result)
{
    return submit_wait(c, &c->admin, cmd, result);
}

/* ======================================================================== */
/*  controller reset / enable (section 7.6.1)                                  */
/* ======================================================================== */
static int ctrl_disable(struct ctrl *c)
{
    uint32_t cc = rd32(&c->regs->cc);
    if (cc & NVME_CC_EN) {
        /* Clearing EN while an enable is still in progress (RDY not yet 1) is
         * undefined on some controllers: let it finish first. */
        if (!(rd32(&c->regs->csts) & NVME_CSTS_RDY))
            wait_csts(c, NVME_CSTS_RDY, NVME_CSTS_RDY, c->ready_timeout_us);
        wr32(&c->regs->cc, cc & ~NVME_CC_EN);
    }
    /* RDY must fall to 0 even if CFS is set: a reset is what clears CFS. */
    return wait_csts(c, NVME_CSTS_RDY, 0, c->ready_timeout_us);
}

/* Disable, point the controller at fresh admin rings, enable. The I/O queues
 * are gone afterwards (a reset deletes them) and must be created again. */
static int ctrl_start_admin(struct ctrl *c)
{
    c->alive = false;
    int rc = ctrl_disable(c);
    if (rc != NVME_OK) {
        out_ctrl(c); out_s("did not become idle (CSTS=0x"); out_hex(rd32(&c->regs->csts), 8);
        out_s("): "); out_s(nvme_strerror(rc)); out_c('\n');
        return rc;
    }
    queue_reset(&c->admin);
    queue_reset(&c->io);

    wr32(&c->regs->aqa, NVME_AQA(c->admin.depth, c->admin.depth));
    wr64(&c->regs->asq, c->admin.sq_phys);
    wr64(&c->regs->acq, c->admin.cq_phys);

    uint32_t cc = rd32(&c->regs->cc);
    cc &= ~(NVME_CC_CSS_MASK | NVME_CC_MPS_MASK | NVME_CC_AMS_MASK | NVME_CC_SHN_MASK | NVME_CC_IOQES_MASK);
    cc |= NVME_CC_CSS_NVM | NVME_CC_MPS(0) | NVME_CC_AMS_RR |
          NVME_CC_IOSQES(NVME_SQES_LOG2) | NVME_CC_IOCQES(NVME_CQES_LOG2);
    wr32(&c->regs->cc, cc);
    mmio_barrier();
    wr32(&c->regs->cc, cc | NVME_CC_EN);

    rc = wait_csts(c, NVME_CSTS_RDY, NVME_CSTS_RDY, c->ready_timeout_us);
    if (rc != NVME_OK) {
        out_ctrl(c); out_s("enable failed (CSTS=0x"); out_hex(rd32(&c->regs->csts), 8);
        out_s("): "); out_s(nvme_strerror(rc)); out_c('\n');
        return rc;
    }
    /* Polled operation: keep pin-based/MSI interrupts masked (the host has
     * not configured MSI-X, so INTMS is valid to write). */
    wr32(&c->regs->intms, 0xFFFFFFFFu);
    return NVME_OK;
}

/* Number of Queues feature, then Create I/O Completion Queue followed by
 * Create I/O Submission Queue (a SQ names its CQ, which must exist). */
static int create_io_queues(struct ctrl *c)
{
    struct nvme_sqe cmd;
    uint32_t granted = 0;

    mem_zero(&cmd, sizeof cmd);
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10  = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11  = 0;                                 /* NCQR = NSQR = 0: one queue each (0's based) */
    int rc = admin_cmd(c, &cmd, &granted);
    if (rc != NVME_OK) return rc;

    mem_zero(&cmd, sizeof cmd);
    cmd.opcode = NVME_ADMIN_CREATE_CQ;
    cmd.prp1   = c->io.cq_phys;
    cmd.cdw10  = (uint32_t)(c->io.depth - 1) << 16 | IO_QID;
    cmd.cdw11  = NVME_QUEUE_PHYS_CONTIG;            /* IEN = 0: polled, vector 0 unused */
    rc = admin_cmd(c, &cmd, NULL);
    if (rc != NVME_OK) { out_ctrl(c); out_s("Create I/O CQ failed\n"); return rc; }

    mem_zero(&cmd, sizeof cmd);
    cmd.opcode = NVME_ADMIN_CREATE_SQ;
    cmd.prp1   = c->io.sq_phys;
    cmd.cdw10  = (uint32_t)(c->io.depth - 1) << 16 | IO_QID;
    cmd.cdw11  = (uint32_t)IO_QID << 16 | NVME_SQ_PRIO_MEDIUM | NVME_QUEUE_PHYS_CONTIG;
    rc = admin_cmd(c, &cmd, NULL);
    if (rc != NVME_OK) { out_ctrl(c); out_s("Create I/O SQ failed\n"); return rc; }

    c->alive = true;
    return NVME_OK;
}

/* After a timeout or fatal status the outstanding command may still own its
 * PRPs; only a controller reset guarantees the DMA is dead. */
static void ctrl_recover(struct ctrl *c)
{
    out_ctrl(c); out_s("resetting controller\n");
    if (ctrl_start_admin(c) == NVME_OK && create_io_queues(c) == NVME_OK) {
        out_ctrl(c); out_s("recovered\n");
        return;
    }
    out_ctrl(c); out_s("recovery failed, controller disabled\n");
    wr32(&c->regs->cc, rd32(&c->regs->cc) & ~NVME_CC_EN);
}

/* ======================================================================== */
/*  PRP construction (section 4.3)                                            */
/* ======================================================================== */

/* PRP1 points at the first byte (any dword offset). If the transfer ends in
 * the next page, PRP2 points at that page; otherwise PRP2 points at a list
 * of page addresses. A list page holds 512 entries; when more follow, its
 * last entry points at the next list page instead. */
static int build_prps(struct ctrl *c, const void *buf, uint32_t len, uint64_t *prp1, uint64_t *prp2)
{
    uintptr_t va = (uintptr_t)buf;
    if (va & 3) return NVME_ERR_ALIGN;

    *prp1 = c->plat.virt_to_phys(buf);
    *prp2 = 0;
    uint32_t first = NVME_PAGE_SIZE - (uint32_t)(va & (NVME_PAGE_SIZE - 1));
    if (len <= first) return NVME_OK;

    len -= first;
    va  += first;                                   /* page aligned from here on */
    if (len <= NVME_PAGE_SIZE) {
        *prp2 = c->plat.virt_to_phys((const void *)va);
        return NVME_OK;
    }

    uint32_t n = (len + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
    uint32_t page = 0, slot = 0;
    uint64_t *list = c->prp_list[0];
    *prp2 = c->prp_list_phys[0];
    for (uint32_t i = 0; i < n; i++, va += NVME_PAGE_SIZE) {
        if (slot == PRP_PER_PAGE - 1 && n - i > 1) {    /* chain to the next list page */
            if (++page >= PRP_LIST_PAGES) return NVME_ERR_RANGE;
            list[slot] = c->prp_list_phys[page];
            list = c->prp_list[page];
            slot = 0;
        }
        list[slot++] = c->plat.virt_to_phys((const void *)va);
    }
    mmio_barrier();                                 /* list complete before the doorbell */
    return NVME_OK;
}

/* ======================================================================== */
/*  Identify                                                                   */
/* ======================================================================== */
static int identify(struct ctrl *c, uint8_t cns, uint32_t nsid, uint64_t dst_phys)
{
    struct nvme_sqe cmd;
    mem_zero(&cmd, sizeof cmd);
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = dst_phys;                          /* page aligned 4 KiB: PRP1 alone suffices */
    cmd.cdw10  = cns;
    return admin_cmd(c, &cmd, NULL);
}

static int identify_controller(struct ctrl *c)
{
    struct nvme_id_ctrl *id = (struct nvme_id_ctrl *)c->idbuf;
    mem_zero(id, sizeof *id);
    int rc = identify(c, NVME_CNS_CONTROLLER, 0, c->idbuf_phys);
    if (rc != NVME_OK) return rc;

    copy_string(c->model, id->mn, sizeof id->mn);
    copy_string(c->serial, id->sn, sizeof id->sn);
    copy_string(c->firmware, id->fr, sizeof id->fr);
    c->nn  = id->nn;
    c->vwc = id->vwc & 1;

    if ((id->sqes & 0xF) > NVME_SQES_LOG2 || (id->cqes & 0xF) > NVME_CQES_LOG2) {
        out_ctrl(c); out_s("requires larger queue entries (SQES 0x"); out_hex(id->sqes, 2);
        out_s(", CQES 0x"); out_hex(id->cqes, 2); out_s(")\n");
        return NVME_ERR_UNSUPPORTED;
    }

    /* MDTS is a power of two in units of the minimum page size (4 KiB here). */
    c->max_transfer = NVME_MAX_TRANSFER;
    if (id->mdts && id->mdts < 20 && (NVME_PAGE_SIZE << id->mdts) < c->max_transfer)
        c->max_transfer = NVME_PAGE_SIZE << id->mdts;

    uint64_t sd = id->rtd3e;
    c->shutdown_timeout_us = sd < T_SHUTDOWN_MIN_US ? T_SHUTDOWN_MIN_US
                           : sd > T_SHUTDOWN_MAX_US ? T_SHUTDOWN_MAX_US : (uint32_t)sd;

    out_ctrl(c); out_s("\""); out_s(c->model); out_s("\" serial "); out_s(c->serial);
    out_s(" firmware "); out_s(c->firmware); out_s(", "); out_dec(c->nn);
    out_s(" namespace(s), max transfer "); out_size(c->max_transfer);
    out_s(c->vwc ? ", volatile write cache\n" : "\n");
    return NVME_OK;
}

static void add_namespace(struct ctrl *c, uint32_t nsid)
{
    struct nvme_id_ns *id = (struct nvme_id_ns *)(c->idbuf + NVME_PAGE_SIZE);
    mem_zero(id, sizeof *id);
    if (identify(c, NVME_CNS_NAMESPACE, nsid, c->idbuf_phys + NVME_PAGE_SIZE) != NVME_OK) return;
    if (!id->nsze) return;                          /* inactive */

    uint32_t fmt = id->flbas & 0xF;
    if (id->nlbaf >= 16) fmt |= (uint32_t)((id->flbas >> 5) & 3) << 4;
    const struct nvme_lbaf *lf = &id->lbaf[fmt];

    out_ctrl(c); out_s("namespace "); out_dec(nsid); out_s(": ");
    if (lf->ms) {
        /* Separate metadata would need MPTR and extended LBAs change the
         * data layout; this driver transfers plain data only. */
        out_s("format with "); out_dec(lf->ms); out_s(" metadata bytes per block, skipped\n");
        return;
    }
    if (lf->lbads < 9 || lf->lbads > 16 || (1u << lf->lbads) > c->max_transfer) {
        out_s("unsupported block size 2^"); out_dec(lf->lbads); out_s(", skipped\n");
        return;
    }
    if (g_nns >= NVME_MAX_NAMESPACES) { out_s("namespace table full, skipped\n"); return; }

    struct nvme_namespace *ns = &g_ns[g_nns++];
    ns->ctrl       = (int)(c - g_ctrl);
    ns->nsid       = nsid;
    ns->blocks     = id->nsze;
    ns->block_size = 1u << lf->lbads;
    ns->max_blocks = c->max_transfer / ns->block_size;
    if (ns->max_blocks > 65536) ns->max_blocks = 65536;    /* NLB is a 16-bit field */
    ns->volatile_cache = c->vwc;
    for (int i = 0; i < 41; i++) ns->model[i] = c->model[i];
    for (int i = 0; i < 21; i++) ns->serial[i] = c->serial[i];
    for (int i = 0; i < 9; i++)  ns->firmware[i] = c->firmware[i];
    for (int i = 0; i < 8; i++)  ns->eui64[i] = id->eui64[i];
    for (int i = 0; i < 16; i++) ns->nguid[i] = id->nguid[i];

    out_dec(ns->blocks); out_s(" blocks of "); out_dec(ns->block_size); out_s(" bytes (");
    out_size(ns->blocks * ns->block_size); out_s(")\n");
}

/* Active namespace list (CNS 02h, NVMe 1.1+); 1.0 controllers get a scan of
 * every NSID up to NN, where inactive namespaces report NSZE = 0. */
static int scan_namespaces(struct ctrl *c)
{
    int before = g_nns;
    uint32_t *list = (uint32_t *)c->idbuf;
    mem_zero(list, NVME_PAGE_SIZE);
    if (identify(c, NVME_CNS_ACTIVE_NS_LIST, 0, c->idbuf_phys) == NVME_OK) {
        for (uint32_t i = 0; i < NVME_PAGE_SIZE / 4 && list[i]; i++)
            add_namespace(c, list[i]);
    } else {
        uint32_t last = c->nn < 1024 ? c->nn : 1024;
        for (uint32_t nsid = 1; nsid <= last; nsid++) add_namespace(c, nsid);
    }
    return g_nns - before;
}

/* ======================================================================== */
/*  initialisation                                                             */
/* ======================================================================== */
static void *alloc(struct ctrl *c, size_t size, uint64_t *phys)
{
    void *v = c->plat.dma_alloc(size, NVME_PAGE_SIZE, phys);
    if (!v || (*phys & (NVME_PAGE_SIZE - 1))) return NULL;
    mem_zero(v, size);
    return v;
}

static bool alloc_queue(struct ctrl *c, struct queue *q, uint16_t depth)
{
    uint64_t sq_phys, cq_phys;
    q->depth = depth;
    q->sq = alloc(c, (size_t)depth * sizeof(struct nvme_sqe), &sq_phys);
    q->cq = alloc(c, (size_t)depth * sizeof(struct nvme_cqe), &cq_phys);
    q->sq_phys = sq_phys;
    q->cq_phys = cq_phys;
    return q->sq && q->cq;
}

int nvme_init(uint64_t bar, const struct nvme_platform *plat)
{
    if (g_nctrl >= NVME_MAX_CTRLS) return NVME_ERR_NOMEM;
    struct ctrl *c = &g_ctrl[g_nctrl];
    mem_zero(c, sizeof *c);

    if (plat) c->plat = *plat;
    if (!c->plat.dma_alloc)    c->plat.dma_alloc    = default_dma_alloc;
    if (!c->plat.virt_to_phys) c->plat.virt_to_phys = default_virt_to_phys;
    if (!c->plat.map_mmio)     c->plat.map_mmio     = default_map_mmio;
    if (!c->plat.delay_us)     c->plat.delay_us     = default_delay_us;
    if (!c->plat.yield)        c->plat.yield        = default_yield;
    if (plat && plat->log_putc) g_putc = plat->log_putc;

    /* Map the fixed registers first; the doorbell stride is only known from CAP. */
    c->bar  = bar;
    c->regs = c->plat.map_mmio(bar, 0x1000);
    if (!c->regs) return NVME_ERR_NODEV;
    c->cap = rd64(&c->regs->cap);
    if (c->cap == ~0ull) { out_ctrl(c); out_s("BAR reads all ones, no device\n"); return NVME_ERR_NODEV; }
    c->vs = rd32(&c->regs->vs);
    c->db_stride = 4u << NVME_CAP_DSTRD(c->cap);
    uint32_t to = NVME_CAP_TO(c->cap);
    c->ready_timeout_us = (to ? to : 1) * 500000u;
    if (!c->plat.map_mmio(bar, 0x1000 + 4u * c->db_stride)) return NVME_ERR_NODEV;   /* queues 0 and 1 */

    out_ctrl(c); out_s("NVMe "); out_dec(c->vs >> 16); out_c('.'); out_dec((c->vs >> 8) & 0xFF);
    if (c->vs & 0xFF) { out_c('.'); out_dec(c->vs & 0xFF); }
    out_s(" at 0x"); out_hex(bar, 12); out_s(", CAP 0x"); out_hex(c->cap, 16);
    out_s(": MQES "); out_dec(NVME_CAP_MQES(c->cap) + 1u); out_s(", stride "); out_dec(c->db_stride);
    out_s(", timeout "); out_dec(c->ready_timeout_us / 1000); out_s(" ms\n");

    if (!(NVME_CAP_CSS(c->cap) & NVME_CAP_CSS_NVM)) {
        out_ctrl(c); out_s("NVM command set not supported\n");
        return NVME_ERR_UNSUPPORTED;
    }
    if (NVME_CAP_MPSMIN(c->cap) != 0) {             /* 4 KiB host pages not allowed */
        out_ctrl(c); out_s("minimum page size "); out_size(NVME_PAGE_SIZE << NVME_CAP_MPSMIN(c->cap));
        out_s(" not supported\n");
        return NVME_ERR_UNSUPPORTED;
    }

    uint32_t mqes = NVME_CAP_MQES(c->cap) + 1u;     /* at least 2 */
    uint16_t adepth = (uint16_t)(mqes < NVME_ADMIN_QUEUE_DEPTH ? mqes : NVME_ADMIN_QUEUE_DEPTH);
    uint16_t iodepth = (uint16_t)(mqes < NVME_IO_QUEUE_DEPTH ? mqes : NVME_IO_QUEUE_DEPTH);
    bool ok = alloc_queue(c, &c->admin, adepth) && alloc_queue(c, &c->io, iodepth);
    for (unsigned i = 0; ok && i < PRP_LIST_PAGES; i++)
        ok = (c->prp_list[i] = alloc(c, NVME_PAGE_SIZE, &c->prp_list_phys[i])) != NULL;
    if (ok) ok = (c->idbuf = alloc(c, 2 * NVME_PAGE_SIZE, &c->idbuf_phys)) != NULL;
    if (!ok) { out_ctrl(c); out_s("out of DMA memory\n"); return NVME_ERR_NOMEM; }
    queue_bind(c, &c->admin, 0);
    queue_bind(c, &c->io, IO_QID);

    g_nctrl++;                                      /* the slot is owned from here on */
    int rc = ctrl_start_admin(c);
    if (rc == NVME_OK) rc = identify_controller(c);
    if (rc == NVME_OK) rc = create_io_queues(c);
    if (rc != NVME_OK) {
        out_ctrl(c); out_s("initialisation failed: "); out_s(nvme_strerror(rc)); out_c('\n');
        c->alive = false;
        return rc;
    }
    out_ctrl(c); out_s("admin queue "); out_dec(adepth); out_s(" entries, I/O queue ");
    out_dec(iodepth); out_s(" entries\n");
    return scan_namespaces(c);
}

int nvme_namespace_count(void) { return g_nns; }

struct nvme_namespace *nvme_namespace_get(int index)
{
    return index >= 0 && index < g_nns ? &g_ns[index] : NULL;
}

/* ======================================================================== */
/*  NVM commands                                                               */
/* ======================================================================== */
static int rw(struct nvme_namespace *ns, uint8_t opcode, uint64_t lba, uint32_t count, void *buf)
{
    if (!ns || ns->ctrl < 0 || ns->ctrl >= g_nctrl) return NVME_ERR_NODEV;
    if (count == 0) return NVME_OK;
    if (lba >= ns->blocks || count > ns->blocks - lba) return NVME_ERR_RANGE;
    if ((uintptr_t)buf & 3) return NVME_ERR_ALIGN;

    struct ctrl *c = &g_ctrl[ns->ctrl];
    ctrl_lock(c);
    int rc = NVME_OK;
    uint8_t *p = buf;
    while (count && rc == NVME_OK) {
        if (!c->alive) { rc = NVME_ERR_NODEV; break; }
        uint32_t n = count < ns->max_blocks ? count : ns->max_blocks;
        struct nvme_sqe cmd;
        mem_zero(&cmd, sizeof cmd);
        cmd.opcode = opcode;
        cmd.nsid   = ns->nsid;
        cmd.cdw10  = (uint32_t)lba;                 /* SLBA */
        cmd.cdw11  = (uint32_t)(lba >> 32);
        cmd.cdw12  = n - 1;                         /* NLB, 0's based */
        uint64_t prp1, prp2;
        rc = build_prps(c, p, n * ns->block_size, &prp1, &prp2);
        if (rc != NVME_OK) break;
        cmd.prp1 = prp1;
        cmd.prp2 = prp2;
        rc = submit_wait(c, &c->io, &cmd, NULL);
        if (rc == NVME_ERR_TIMEOUT || rc == NVME_ERR_FATAL) ctrl_recover(c);
        lba += n; count -= n; p += (size_t)n * ns->block_size;
    }
    ctrl_unlock(c);
    return rc;
}

int nvme_read(struct nvme_namespace *ns, uint64_t lba, uint32_t count, void *buf)
{
    return rw(ns, NVME_CMD_READ, lba, count, buf);
}

int nvme_write(struct nvme_namespace *ns, uint64_t lba, uint32_t count, const void *buf)
{
    return rw(ns, NVME_CMD_WRITE, lba, count, (void *)buf);     /* the controller only reads it */
}

int nvme_flush(struct nvme_namespace *ns)
{
    if (!ns || ns->ctrl < 0 || ns->ctrl >= g_nctrl) return NVME_ERR_NODEV;
    struct ctrl *c = &g_ctrl[ns->ctrl];
    ctrl_lock(c);
    int rc = NVME_ERR_NODEV;
    if (c->alive) {
        struct nvme_sqe cmd;
        mem_zero(&cmd, sizeof cmd);
        cmd.opcode = NVME_CMD_FLUSH;
        cmd.nsid   = ns->nsid;
        rc = submit_wait(c, &c->io, &cmd, NULL);
        if (rc == NVME_ERR_TIMEOUT || rc == NVME_ERR_FATAL) ctrl_recover(c);
    }
    ctrl_unlock(c);
    return rc;
}

/* Section 7.6.2: delete the I/O queues, then request a normal shutdown and
 * wait for CSTS.SHST to report completion. */
void nvme_shutdown_all(void)
{
    for (int i = 0; i < g_nctrl; i++) {
        struct ctrl *c = &g_ctrl[i];
        if (!(rd32(&c->regs->cc) & NVME_CC_EN)) continue;
        ctrl_lock(c);
        if (c->alive) {
            struct nvme_sqe cmd;
            mem_zero(&cmd, sizeof cmd);
            cmd.opcode = NVME_ADMIN_DELETE_SQ;
            cmd.cdw10  = IO_QID;
            admin_cmd(c, &cmd, NULL);
            mem_zero(&cmd, sizeof cmd);
            cmd.opcode = NVME_ADMIN_DELETE_CQ;
            cmd.cdw10  = IO_QID;
            admin_cmd(c, &cmd, NULL);
            c->alive = false;
        }
        uint32_t cc = rd32(&c->regs->cc) & ~NVME_CC_SHN_MASK;
        wr32(&c->regs->cc, cc | NVME_CC_SHN_NORMAL);
        int rc = wait_csts(c, NVME_CSTS_SHST_MASK, NVME_CSTS_SHST_DONE,
                           c->shutdown_timeout_us ? c->shutdown_timeout_us : T_SHUTDOWN_MIN_US);
        out_ctrl(c); out_s(rc == NVME_OK ? "shutdown complete\n" : "shutdown did not complete\n");
        ctrl_unlock(c);
    }
}

const char *nvme_strerror(int err)
{
    switch (err) {
    case NVME_OK:              return "success";
    case NVME_ERR_NODEV:       return "no such device";
    case NVME_ERR_TIMEOUT:     return "timeout";
    case NVME_ERR_IO:          return "command failed";
    case NVME_ERR_FATAL:       return "controller fatal status";
    case NVME_ERR_RANGE:       return "LBA out of range";
    case NVME_ERR_ALIGN:       return "buffer not dword aligned";
    case NVME_ERR_NOMEM:       return "out of DMA memory";
    case NVME_ERR_UNSUPPORTED: return "unsupported controller or format";
    default:                   return "unknown error";
    }
}
