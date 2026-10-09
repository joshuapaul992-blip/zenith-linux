/* =============================================================================
 *  xhci.c -- standalone xHCI host controller driver + HID boot keyboard/mouse
 *
 *  Layers, bottom to top:
 *    1. MMIO access, platform hooks, debug output
 *    2. Rings: producer (command/transfer) rings with Link TRB + cycle toggle,
 *       consumer event ring + ERST, interrupter 0
 *    3. Controller bring-up: BIOS handoff, halt, reset, DCBAA, scratchpads,
 *       rings, Run/Stop
 *    4. Commands (Enable Slot, Address Device, Evaluate Context, Configure
 *       Endpoint, Reset Endpoint, Set TR Dequeue, Disable Slot) and control
 *       transfers on EP0
 *    5. Enumeration: port reset, slot/device contexts, descriptors, strings,
 *       configuration parsing, SET_CONFIGURATION
 *    6. HID boot protocol: interrupt-IN endpoints, keyboard key diffing with
 *       typematic repeat, mouse movement
 *    7. Event dispatch (xhci_poll) and hot-plug handling (xhci_service)
 * ============================================================================= */
#include "xhci.h"

/* ======================================================================== */
/*  1. low-level helpers                                                       */
/* ======================================================================== */
#define REG(x) (*(volatile uint32_t *)&(x))

static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
static inline void port_outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t port_inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

typedef uint64_t irqstate_t;
static inline irqstate_t irq_off(void)
{ uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(irqstate_t f)
{ if (f & (1u << 9)) __asm__ volatile("sti" ::: "memory"); }

#ifndef XHCI_NO_MEM_FALLBACK
#if defined(__clang__)
#define NO_LOOP_TO_LIBCALL                  /* clang: build with -fno-builtin */
#else
#define NO_LOOP_TO_LIBCALL optimize("no-tree-loop-distribute-patterns")
#endif
__attribute__((weak, NO_LOOP_TO_LIBCALL))
void *memset(void *dst, int c, size_t n)
{ unsigned char *d = dst; while (n--) *d++ = (unsigned char)c; return dst; }
__attribute__((weak, NO_LOOP_TO_LIBCALL))
void *memcpy(void *dst, const void *src, size_t n)
{ unsigned char *d = dst; const unsigned char *s = src; while (n--) *d++ = *s++; return dst; }
#endif

static void mem_zero(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }
static void mem_copy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

/* ---- default platform hooks ---------------------------------------------- */
static uint8_t g_pool[XHCI_STATIC_POOL] __attribute__((aligned(4096)));
static size_t  g_pool_used;

/* Bump allocator. Objects up to 4 KiB never cross a page boundary, which
 * satisfies the xHCI rules for contexts (page) and rings/ERST (64 KiB). */
static void *default_dma_alloc(size_t size, size_t align, uint64_t *phys)
{
    size_t start = (g_pool_used + align - 1) & ~(align - 1);
    if (size <= 4096 && (start & ~(size_t)4095) != ((start + size - 1) & ~(size_t)4095))
        start = (start + 4095) & ~(size_t)4095;
    if (start + size > sizeof g_pool) return NULL;
    g_pool_used = start + size;
    *phys = (uint64_t)(uintptr_t)&g_pool[start];
    return &g_pool[start];
}
static void *default_map_mmio(uint64_t phys, size_t size) { (void)size; return (void *)(uintptr_t)phys; }
static void default_delay_us(uint32_t us) { while (us--) port_outb(0x80, 0); }
static void default_log_putc(char c)
{
    if (c == '\n') default_log_putc('\r');
    for (int spin = 0; spin < 100000 && !(port_inb(0x3F8 + 5) & 0x20); spin++) __asm__ volatile("pause");
    port_outb(0x3F8, (uint8_t)c);
}

/* ---- debug output ---------------------------------------------------------- */
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
    char b[21]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_c(b[--n]);
}

/* ======================================================================== */
/*  driver state                                                               */
/* ======================================================================== */
struct hid_if {
    uint8_t  kind;                  /* USB_KIND_KEYBOARD / USB_KIND_MOUSE      */
    uint8_t  iface;
    uint8_t  ep_addr;               /* bEndpointAddress                        */
    uint8_t  dci;                   /* device context index = ep * 2 + IN      */
    uint16_t mps;
    uint8_t  binterval;
    bool     active;                /* reports flowing                         */
    bool     needs_reset;           /* endpoint halted, service() recovers it  */
    struct xhci_ring ring;
    uint8_t *buf;  uint64_t buf_phys;     /* XHCI_HID_BUFFERS x 64 bytes      */
    uint8_t  prev[8];               /* last keyboard report                    */
    uint8_t  repeat_usage;
    uint64_t repeat_next;
};

/* Bulk-Only mass-storage interface */
struct msc_if {
    bool     present;
    uint8_t  iface, subclass, protocol;
    uint8_t  in_addr, out_addr, in_dci, out_dci;
    uint16_t in_mps, out_mps;
    uint8_t  in_burst, out_burst;               /* SuperSpeed companion bMaxBurst */
    struct xhci_ring in_ring, out_ring;
    uint8_t *bounce; uint64_t bounce_phys;      /* XHCI_BULK_MAX, 64 KiB aligned  */
    volatile bool     done;                     /* written by the event handler   */
    volatile uint8_t  code;
    volatile uint32_t residual;
    uint64_t wait_trb;
    uint8_t  wait_dci;
};

struct usb_dev {
    struct usb_device_info info;
    bool     used;
    uint8_t  slot, port, speed;
    uint16_t mps0;
    void    *out_ctx; uint64_t out_phys;      /* output (device) context       */
    void    *in_ctx;  uint64_t in_phys;       /* input context                 */
    struct xhci_ring ep0;
    uint8_t *ctrl_buf; uint64_t ctrl_phys;    /* 512-byte control data buffer  */
    struct usb_device_descriptor desc;
    struct hid_if hid[2];
    int      nhid;
    struct msc_if msc;
    /* control-transfer completion, written by the event handler */
    volatile bool    xfer_done;
    volatile uint8_t xfer_code;
    uint64_t xfer_wait_trb;
};

static struct {
    bool up;
    struct xhci_platform plat;
    uint8_t *base;
    struct xhci_cap_regs     *cap;
    struct xhci_op_regs      *op;
    struct xhci_runtime_regs *rt;
    volatile uint32_t        *db;
    uint32_t hcs1, hcs2, hcc1;
    uint32_t max_slots, max_ports, ctx_size, scratchpads;
    uint16_t version;
    bool ac64;
    uint64_t *dcbaa;  uint64_t dcbaa_phys;
    uint64_t *spad;   uint64_t spad_phys;
    struct xhci_ring cmd;
    struct xhci_event_ring ev;
    volatile bool    cmd_done;
    volatile uint8_t cmd_code, cmd_slot;
    uint64_t cmd_wait_trb;
    struct usb_dev devs[XHCI_MAX_SLOTS + 1];  /* indexed by slot ID (1-based)  */
    uint8_t  port_major[XHCI_MAX_PORTS + 1];  /* 2 or 3 from Supported Protocol */
    uint8_t  port_slot[XHCI_MAX_PORTS + 1];
    volatile uint32_t port_pending;           /* bit (port-1): status changed  */
    volatile bool     ep_pending;             /* some endpoint needs a reset   */
    bool caps_lock;
    /* boot settling: per-port state machine */
    struct { uint8_t state, attempts; uint64_t when; } track[XHCI_MAX_PORTS + 1];
    volatile bool busy;                       /* a thread is issuing commands   */
    uint64_t fake_ms;                         /* time base without a now_ms hook */
    uint32_t generation;
    /* diagnostics */
    const char *handoff;
    uint8_t  last_cmd_type, last_cmd_code, last_xfer_code;
    uint32_t enum_failures, timeouts, stalls;
} X;

enum { PT_IDLE, PT_DEBOUNCE, PT_DONE, PT_FAILED };
#define DEBOUNCE_MS        100                /* USB 2.0 TATTDB                 */
#define MAX_ENUM_ATTEMPTS  3

static void delay_us(uint32_t us) { X.plat.delay_us(us); X.fake_ms += us / 1000; }
static void delay_ms(uint32_t ms) { while (ms--) { X.plat.delay_us(1000); X.fake_ms++; } }
static uint64_t now_ms(void) { return X.plat.now_ms ? X.plat.now_ms() : X.fake_ms; }

/* Serialise command/transfer users (single CPU: a flag set with IRQs off).
 * Waiting threads poll with short delays; on a preemptive kernel the holder
 * gets the CPU back, on a cooperative one it never gives it up mid-way. */
static bool try_acquire(void)
{
    irqstate_t s = irq_off();
    bool ok = !X.busy;
    if (ok) X.busy = true;
    irq_restore(s);
    return ok;
}
static bool acquire(uint32_t timeout_ms)
{
    uint64_t end = now_ms() + timeout_ms;
    while (!try_acquire()) {
        if (now_ms() >= end) return false;
        delay_us(200);
    }
    return true;
}
static void release(void) { X.busy = false; }

static void *dma(size_t size, size_t align, uint64_t *phys)
{
    void *v = X.plat.dma_alloc(size, align, phys);
    if (!v) return NULL;
    if ((*phys & (align - 1)) || (!X.ac64 && *phys + size > 0x100000000ull)) return NULL;
    mem_zero(v, size);
    return v;
}

static bool wait_reg(volatile uint32_t *r, uint32_t mask, uint32_t want, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t <= timeout_ms * 10; t++) {
        if ((*r & mask) == want) return true;
        delay_us(100);
    }
    return false;
}

static void write64(volatile uint32_t *lo, uint64_t v)    /* low dword first */
{
    lo[0] = (uint32_t)v;
    lo[1] = (uint32_t)(v >> 32);
}

const char *xhci_speed_name(uint8_t s)
{
    switch (s) {
    case XHCI_SPEED_FULL:  return "full-speed 12 Mb/s";
    case XHCI_SPEED_LOW:   return "low-speed 1.5 Mb/s";
    case XHCI_SPEED_HIGH:  return "high-speed 480 Mb/s";
    case XHCI_SPEED_SUPER: return "SuperSpeed 5 Gb/s";
    case XHCI_SPEED_SUPER_PLUS: return "SuperSpeedPlus 10+ Gb/s";
    default: return "unknown speed";
    }
}

static const char *cc_name(uint8_t cc)
{
    switch (cc) {
    case CC_SUCCESS: return "success";
    case CC_DATA_BUFFER: return "data buffer error";
    case CC_BABBLE: return "babble";
    case CC_USB_TRANSACTION: return "USB transaction error";
    case CC_TRB_ERROR: return "TRB error";
    case CC_STALL: return "stall";
    case CC_RESOURCE: return "resource error";
    case CC_BANDWIDTH: return "bandwidth error";
    case CC_NO_SLOTS: return "no slots available";
    case CC_SHORT_PACKET: return "short packet";
    case CC_PARAMETER: return "parameter error";
    case CC_CONTEXT_STATE: return "context state error";
    default: return "error";
    }
}

/* ======================================================================== */
/*  2. rings                                                                   */
/* ======================================================================== */

/* A producer ring is a circular array whose last TRB is a Link TRB back to
 * the start with Toggle Cycle set. The producer writes TRBs with its
 * current cycle bit; the consumer (the xHC) owns every TRB whose cycle bit
 * matches its own consumer cycle state. */
bool xhci_ring_init(struct xhci_ring *r, uint32_t trbs)
{
    if (trbs > XHCI_EP_RING_TRBS) trbs = XHCI_EP_RING_TRBS;
    if (!r->trbs) {
        r->trbs = dma(trbs * sizeof(struct xhci_trb), 64, &r->phys);
        if (!r->trbs) return false;
    } else {
        mem_zero(r->trbs, trbs * sizeof(struct xhci_trb));
    }
    r->size = trbs;
    r->enq = 0;
    r->cycle = 1;
    struct xhci_trb *link = &r->trbs[trbs - 1];
    link->param = r->phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;        /* cycle 0: not yet valid */
    return true;
}

/* Write one TRB; returns its physical address (events refer to TRBs by
 * address). When the next slot is the Link TRB, hand the Link TRB to the
 * controller with the current cycle bit and flip the producer cycle. */
uint64_t xhci_ring_enqueue(struct xhci_ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    irqstate_t s = irq_off();
    struct xhci_trb *t = &r->trbs[r->enq];
    uint64_t phys = r->phys + (uint64_t)r->enq * sizeof *t;
    t->param = param;
    t->status = status;
    wmb();                                      /* body before the cycle bit */
    REG(t->control) = (control & ~TRB_CYCLE) | r->cycle;

    if (++r->enq == r->size - 1) {
        struct xhci_trb *link = &r->trbs[r->size - 1];
        wmb();
        /* the Link TRB inherits the chain bit so a TD may span the wrap */
        REG(link->control) = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    irq_restore(s);
    return phys;
}

/* Consumer side of the event ring: an event is valid when its cycle bit
 * equals our consumer cycle state; wrapping the single segment flips it. */
bool xhci_event_dequeue(struct xhci_event_ring *er, struct xhci_trb *out)
{
    struct xhci_trb *t = &er->trbs[er->deq];
    uint32_t control = REG(t->control);
    if ((control & TRB_CYCLE) != er->cycle) return false;
    mb();                                       /* cycle bit before the body */
    out->param = t->param;
    out->status = t->status;
    out->control = control;
    if (++er->deq == er->size) { er->deq = 0; er->cycle ^= 1; }
    return true;
}

static void ring_doorbell(uint8_t slot, uint8_t target)
{
    mb();                                       /* TRBs visible before the ring */
    X.db[slot] = target;
}

/* ======================================================================== */
/*  contexts                                                                   */
/* ======================================================================== */
static void *ctx_at(void *base, unsigned index) { return (uint8_t *)base + index * X.ctx_size; }
static struct xhci_input_ctrl_ctx *in_ctrl(struct usb_dev *d) { return ctx_at(d->in_ctx, 0); }
static struct xhci_slot_ctx *in_slot(struct usb_dev *d)       { return ctx_at(d->in_ctx, 1); }
static struct xhci_ep_ctx *in_ep(struct usb_dev *d, unsigned dci) { return ctx_at(d->in_ctx, dci + 1); }
static struct xhci_slot_ctx *out_slot(struct usb_dev *d)      { return ctx_at(d->out_ctx, 0); }

/* ======================================================================== */
/*  7a. event dispatch (needed by everything that waits)                       */
/* ======================================================================== */
static void hid_report(struct usb_dev *d, struct hid_if *h, const uint8_t *r, uint32_t len);

static void requeue_report(struct usb_dev *d, struct hid_if *h, unsigned buf_index)
{
    uint16_t len = h->mps > 64 ? 64 : h->mps;
    uint64_t trb = xhci_ring_enqueue(&h->ring, h->buf_phys + buf_index * 64u, len,
                                     TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    h->ring.tag[(trb - h->ring.phys) / sizeof(struct xhci_trb)] = (uint8_t)buf_index;
    ring_doorbell(d->slot, h->dci);
}

static void handle_transfer_event(const struct xhci_trb *ev)
{
    uint8_t slot = (uint8_t)TRB_GET_SLOT(ev->control), dci = (uint8_t)TRB_GET_EP(ev->control);
    uint8_t cc = (uint8_t)TRB_GET_CODE(ev->status);
    if (slot == 0 || slot > X.max_slots || !X.devs[slot].used) return;
    struct usb_dev *d = &X.devs[slot];

    if (dci == 1) {                             /* EP0: synchronous control transfer */
        if (ev->param == d->xfer_wait_trb || (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)) {
            d->xfer_code = cc;
            d->xfer_done = true;
        }
        return;
    }
    if (d->msc.present && (dci == d->msc.in_dci || dci == d->msc.out_dci)) {
        /* synchronous bulk transfer: completion = our TRB, or any error
         * (stall, transaction error, "stopped" after a cancel) on it */
        if (dci == d->msc.wait_dci && (ev->param == d->msc.wait_trb || cc != CC_SUCCESS)) {
            d->msc.code = cc;
            d->msc.residual = TRB_GET_LEN(ev->status);
            d->msc.done = true;
        }
        return;
    }
    for (int i = 0; i < d->nhid; i++) {
        struct hid_if *h = &d->hid[i];
        if (h->dci != dci || !h->active) continue;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            unsigned idx = h->ring.tag[(ev->param - h->ring.phys) / sizeof(struct xhci_trb) % XHCI_EP_RING_TRBS];
            uint32_t len = (h->mps > 64 ? 64u : h->mps) - TRB_GET_LEN(ev->status);
            hid_report(d, h, h->buf + idx * 64u, len);
            requeue_report(d, h, idx);
        } else {
            /* halted endpoint (stall, transaction error): recovery needs
             * commands, which may not run here; hand it to xhci_service() */
            h->active = false;
            h->needs_reset = true;
            X.ep_pending = true;
        }
        return;
    }
}

static void handle_event(const struct xhci_trb *ev)
{
    switch (TRB_GET_TYPE(ev->control)) {
    case TRB_EV_CMD_COMPLETION:
        if (ev->param == X.cmd_wait_trb) {
            X.cmd_code = (uint8_t)TRB_GET_CODE(ev->status);
            X.cmd_slot = (uint8_t)TRB_GET_SLOT(ev->control);
            X.cmd_done = true;
        }
        break;
    case TRB_EV_TRANSFER:
        handle_transfer_event(ev);
        break;
    case TRB_EV_PORT_STATUS: {
        uint32_t port = (uint32_t)(ev->param >> 24) & 0xFF;
        if (port >= 1 && port <= X.max_ports && port <= 32) X.port_pending |= 1u << (port - 1);
        break;
    }
    case TRB_EV_HOST_CONTROLLER:
        out_s("xhci: host controller event, code "); out_dec(TRB_GET_CODE(ev->status)); out_c('\n');
        break;
    default:
        break;
    }
}

static void key_repeat(void);

void xhci_poll(void)
{
    if (!X.up) return;
    irqstate_t s = irq_off();
    struct xhci_trb ev;
    bool any = false;
    while (xhci_event_dequeue(&X.ev, &ev)) { handle_event(&ev); any = true; }
    struct xhci_intr_regs *ir = &X.rt->ir[0];
    if (any) {
        /* tell the xHC how far we got; writing EHB (RW1C) re-arms the interrupter */
        uint64_t erdp = X.ev.phys + (uint64_t)X.ev.deq * sizeof(struct xhci_trb);
        write64(&REG(ir->erdp_lo), erdp | XHCI_ERDP_EHB);
    }
    if (REG(ir->iman) & XHCI_IMAN_IP) REG(ir->iman) = XHCI_IMAN_IP | XHCI_IMAN_IE;
    if (REG(X.op->usbsts) & XHCI_STS_EINT) REG(X.op->usbsts) = XHCI_STS_EINT;
    key_repeat();
    irq_restore(s);
}

/* ======================================================================== */
/*  4. commands and control transfers                                          */
/* ======================================================================== */
static int command(uint32_t control, uint64_t param, uint8_t *slot_out)
{
    X.cmd_done = false;
    uint64_t trb = xhci_ring_enqueue(&X.cmd, param, 0, control);
    X.cmd_wait_trb = trb;
    ring_doorbell(0, 0);                        /* doorbell 0 target 0: command ring */

    for (uint32_t t = 0; t < 20000 && !X.cmd_done; t++) {   /* 2 s */
        xhci_poll();
        if (!X.cmd_done) delay_us(100);
    }
    if (!X.cmd_done) {
        X.last_cmd_type = (uint8_t)TRB_GET_TYPE(control);
        X.last_cmd_code = 0xFF;
        X.timeouts++;
        out_s("xhci: command type "); out_dec(TRB_GET_TYPE(control)); out_s(" timed out\n");
        return XHCI_ERR_TIMEOUT;
    }
    if (slot_out) *slot_out = X.cmd_slot;
    X.last_cmd_type = (uint8_t)TRB_GET_TYPE(control);
    X.last_cmd_code = X.cmd_code;
    if (X.cmd_code != CC_SUCCESS) {
        out_s("xhci: command type "); out_dec(TRB_GET_TYPE(control)); out_s(" failed: ");
        out_s(cc_name(X.cmd_code)); out_s(" ("); out_dec(X.cmd_code); out_s(")\n");
        return XHCI_ERR_CMD;
    }
    return XHCI_OK;
}

/* Recover a halted endpoint: Reset Endpoint, then move the dequeue pointer
 * past everything that was queued (xHCI 4.6.8 / 4.6.10). */
static int endpoint_reset(struct usb_dev *d, uint8_t dci, struct xhci_ring *r)
{
    int rc = command(TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(dci), 0, NULL);
    if (rc != XHCI_OK && rc != XHCI_ERR_CMD) return rc;      /* context-state: not halted */
    uint64_t deq = (r->phys + (uint64_t)r->enq * sizeof(struct xhci_trb)) | r->cycle;
    return command(TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(d->slot) | TRB_EP(dci), deq, NULL);
}

/* Synchronous control transfer on EP0: Setup, optional Data, Status stage. */
static int control(struct usb_dev *d, uint8_t reqtype, uint8_t request, uint16_t value,
                   uint16_t index, uint16_t length, void *data)
{
    if (length > 512) return XHCI_ERR_UNSUPPORTED;
    bool in = (reqtype & USB_DIR_IN) != 0;
    if (!in && length) mem_copy(d->ctrl_buf, data, length);

    uint64_t setup = (uint64_t)reqtype | (uint64_t)request << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)length << 48;
    uint32_t trt = !length ? TRB_TRT_NO_DATA : in ? TRB_TRT_IN : TRB_TRT_OUT;

    d->xfer_done = false;
    d->xfer_wait_trb = 0;
    xhci_ring_enqueue(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (length)
        xhci_ring_enqueue(&d->ep0, d->ctrl_phys, length, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    /* status stage runs opposite to the data stage (IN when there is none) */
    uint32_t sdir = (length && in) ? 0 : TRB_DIR_IN;
    d->xfer_wait_trb = xhci_ring_enqueue(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | sdir | TRB_IOC);
    ring_doorbell(d->slot, 1);

    for (uint32_t t = 0; t < 10000 && !d->xfer_done; t++) {   /* 1 s */
        xhci_poll();
        if (!d->xfer_done) delay_us(100);
    }
    if (!d->xfer_done) {
        X.timeouts++;
        X.last_xfer_code = 0xFF;
        command(TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(d->slot) | TRB_EP(1), 0, NULL);
        endpoint_reset(d, 1, &d->ep0);
        out_s("usb: control request 0x"); out_hex(request, 2); out_s(" timed out\n");
        return XHCI_ERR_TIMEOUT;
    }
    X.last_xfer_code = d->xfer_code;
    if (d->xfer_code != CC_SUCCESS && d->xfer_code != CC_SHORT_PACKET) {
        uint8_t cc = d->xfer_code;
        endpoint_reset(d, 1, &d->ep0);
        out_s("usb: control request 0x"); out_hex(request, 2); out_s(" -> "); out_s(cc_name(cc)); out_c('\n');
        return XHCI_ERR_XFER;
    }
    if (in && length) mem_copy(data, d->ctrl_buf, length);
    return XHCI_OK;
}

static int get_descriptor(struct usb_dev *d, uint8_t type, uint8_t index, uint16_t lang,
                          void *buf, uint16_t len)
{
    return control(d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                   (uint16_t)(type << 8 | index), lang, len, buf);
}

static void get_string(struct usb_dev *d, uint8_t index, char *out, size_t cap)
{
    out[0] = 0;
    if (!index) return;
    uint8_t buf[256];
    uint16_t lang = 0x0409;
    if (get_descriptor(d, USB_DT_STRING, 0, 0, buf, 4) == XHCI_OK && buf[0] >= 4)
        lang = (uint16_t)(buf[2] | buf[3] << 8);
    mem_zero(buf, sizeof buf);
    if (get_descriptor(d, USB_DT_STRING, index, lang, buf, 255) != XHCI_OK) return;
    size_t n = 0;
    for (unsigned i = 2; i + 1 < buf[0] && n + 1 < cap; i += 2) {     /* UTF-16LE -> ASCII */
        uint16_t ch = (uint16_t)(buf[i] | buf[i + 1] << 8);
        out[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    out[n] = 0;
}

/* ======================================================================== */
/*  3. controller bring-up                                                     */
/* ======================================================================== */
static void scan_extended_caps(void)
{
    uint32_t off = XHCI_HCC1_XECP(X.hcc1) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        volatile uint32_t *cap = (volatile uint32_t *)(X.base + off);
        uint32_t v = cap[0];
        uint8_t id = v & 0xFF;

        if (id == XHCI_XCAP_LEGACY) {
            /* BIOS -> OS handoff (xHCI 4.22.1), then stop SMIs on USB events */
            if (v & XHCI_LEGACY_BIOS_OWNED) {
                /* request ownership, wait up to 1 s for the BIOS to let go */
                cap[0] = v | XHCI_LEGACY_OS_OWNED;
                uint64_t t0 = now_ms();
                bool ok = wait_reg(cap, XHCI_LEGACY_BIOS_OWNED, 0, 1000);
                out_s(ok ? "xhci: USBLEGSUP: BIOS released the controller after " : "xhci: USBLEGSUP: BIOS still owns the controller after ");
                out_dec(now_ms() - t0); out_s(ok ? " ms\n" : " ms, forcing OS ownership\n");
                if (!ok) cap[0] = (cap[0] & ~XHCI_LEGACY_BIOS_OWNED) | XHCI_LEGACY_OS_OWNED;
                X.handoff = ok ? "BIOS released ownership" : "BIOS timed out, ownership forced";
            } else {
                cap[0] = v | XHCI_LEGACY_OS_OWNED;
                X.handoff = "not BIOS-owned; OS ownership set";
                out_s("xhci: USBLEGSUP: controller was not BIOS-owned\n");
            }
            cap[1] = (cap[1] & ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))) | (0x7u << 29);
        } else if (id == XHCI_XCAP_PROTOCOL) {
            uint8_t major = (uint8_t)(v >> 24);
            uint32_t ports = cap[2];
            uint32_t first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (uint32_t p = first; p < first + count && p <= XHCI_MAX_PORTS; p++)
                X.port_major[p] = major;
            out_s("xhci: ports "); out_dec(first); out_c('-'); out_dec(first + count - 1);
            out_s(": USB "); out_dec(major); out_c('.'); out_dec((v >> 16) & 0xFF); out_c('\n');
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
}

static int setup_memory(void)
{
    /* Device Context Base Address Array: slot 0 entry = scratchpad array */
    X.dcbaa = dma((X.max_slots + 1) * sizeof(uint64_t), 64, &X.dcbaa_phys);
    if (!X.dcbaa) return XHCI_ERR_NOMEM;

    if (X.scratchpads) {
        X.spad = dma(X.scratchpads * sizeof(uint64_t), 64, &X.spad_phys);
        if (!X.spad) return XHCI_ERR_NOMEM;
        for (uint32_t i = 0; i < X.scratchpads; i++) {
            uint64_t page;
            if (!dma(4096, 4096, &page)) return XHCI_ERR_NOMEM;
            X.spad[i] = page;
        }
        X.dcbaa[0] = X.spad_phys;
    }
    write64(&REG(X.op->dcbaap_lo), X.dcbaa_phys);

    /* Command ring */
    if (!xhci_ring_init(&X.cmd, XHCI_CMD_RING_TRBS)) return XHCI_ERR_NOMEM;
    write64(&REG(X.op->crcr_lo), X.cmd.phys | XHCI_CRCR_RCS);

    /* Event ring: one segment + its segment table, owned by interrupter 0 */
    X.ev.size = XHCI_EVENT_RING_TRBS;
    X.ev.trbs = dma(X.ev.size * sizeof(struct xhci_trb), 64, &X.ev.phys);
    X.ev.erst = dma(sizeof(struct xhci_erst_entry), 64, &X.ev.erst_phys);
    if (!X.ev.trbs || !X.ev.erst) return XHCI_ERR_NOMEM;
    X.ev.deq = 0;
    X.ev.cycle = 1;
    X.ev.erst->base = X.ev.phys;
    X.ev.erst->size = X.ev.size;

    struct xhci_intr_regs *ir = &X.rt->ir[0];
    REG(ir->erstsz) = 1;
    write64(&REG(ir->erdp_lo), X.ev.phys);
    write64(&REG(ir->erstba_lo), X.ev.erst_phys);   /* written last: enables the ring */
    REG(ir->imod) = 4000;                           /* 4000 x 250 ns = 1 ms moderation */
    REG(ir->iman) = XHCI_IMAN_IP | XHCI_IMAN_IE;     /* clear pending, enable           */
    return XHCI_OK;
}

static int enumerate_port(uint32_t port);

int xhci_init(uint64_t mmio, const struct xhci_platform *plat)
{
    if (X.up) return XHCI_ERR_UNSUPPORTED;
    mem_zero(&X, sizeof X);
    if (plat) X.plat = *plat;
    if (!X.plat.dma_alloc) X.plat.dma_alloc = default_dma_alloc;
    if (!X.plat.map_mmio)  X.plat.map_mmio  = default_map_mmio;
    if (!X.plat.delay_us)  X.plat.delay_us  = default_delay_us;
    if (!X.plat.log_putc)  X.plat.log_putc  = default_log_putc;
    g_putc = X.plat.log_putc;

    if (!mmio) return XHCI_ERR_NODEV;
    X.base = X.plat.map_mmio(mmio, 0x10000);
    X.cap  = (struct xhci_cap_regs *)X.base;
    uint32_t cap0 = REG(*(uint32_t *)X.base);         /* CAPLENGTH + HCIVERSION as one dword */
    X.version = (uint16_t)(cap0 >> 16);
    X.op   = (struct xhci_op_regs *)(X.base + (cap0 & 0xFF));
    X.rt   = (struct xhci_runtime_regs *)(X.base + (REG(X.cap->rtsoff) & ~0x1Fu));
    X.db   = (volatile uint32_t *)(X.base + (REG(X.cap->dboff) & ~0x3u));
    X.hcs1 = REG(X.cap->hcsparams1);
    X.hcs2 = REG(X.cap->hcsparams2);
    X.hcc1 = REG(X.cap->hccparams1);
    X.max_slots = XHCI_HCS1_MAX_SLOTS(X.hcs1);
    if (X.max_slots > XHCI_MAX_SLOTS) X.max_slots = XHCI_MAX_SLOTS;
    X.max_ports = XHCI_HCS1_MAX_PORTS(X.hcs1);
    if (X.max_ports > XHCI_MAX_PORTS) X.max_ports = XHCI_MAX_PORTS;
    X.ctx_size = (X.hcc1 & XHCI_HCC1_CSZ) ? 64 : 32;
    X.ac64 = (X.hcc1 & XHCI_HCC1_AC64) != 0;
    X.scratchpads = XHCI_HCS2_SCRATCHPADS(X.hcs2);

    out_s("xhci: controller at 0x"); out_hex(mmio, mmio >> 32 ? 16 : 8); out_s(", version ");
    out_hex(X.version >> 8, 1); out_c('.'); out_hex(X.version & 0xFF, 2);
    out_s(", "); out_dec(XHCI_HCS1_MAX_SLOTS(X.hcs1)); out_s(" slots, ");
    out_dec(XHCI_HCS1_MAX_PORTS(X.hcs1)); out_s(" ports, ");
    out_dec(XHCI_HCS1_MAX_INTRS(X.hcs1)); out_s(" interrupters, ");
    out_dec(X.ctx_size); out_s("-byte contexts, ");
    out_dec(X.scratchpads); out_s(" scratchpad pages");
    out_s(X.ac64 ? ", 64-bit DMA\n" : ", 32-bit DMA\n");

    X.handoff = "no USB Legacy Support capability";
    scan_extended_caps();

    /* Halt, then reset (xHCI 4.2). Intel parts need ~1 ms before the
     * reset bit may be polled. */
    if (!(REG(X.op->usbsts) & XHCI_STS_HCH)) {
        REG(X.op->usbcmd) = REG(X.op->usbcmd) & ~XHCI_CMD_RS;
        if (!wait_reg(&REG(X.op->usbsts), XHCI_STS_HCH, XHCI_STS_HCH, 50)) {
            out_s("xhci: controller did not halt\n"); return XHCI_ERR_HC;
        }
    }
    REG(X.op->usbcmd) = XHCI_CMD_HCRST;
    delay_ms(1);
    if (!wait_reg(&REG(X.op->usbcmd), XHCI_CMD_HCRST, 0, 1000) ||
        !wait_reg(&REG(X.op->usbsts), XHCI_STS_CNR, 0, 1000)) {
        out_s("xhci: reset timed out\n"); return XHCI_ERR_HC;
    }
    if (!(REG(X.op->pagesize) & 1)) { out_s("xhci: 4 KiB pages not supported\n"); return XHCI_ERR_UNSUPPORTED; }

    REG(X.op->config) = (REG(X.op->config) & ~0xFFu) | X.max_slots;
    int rc = setup_memory();
    if (rc != XHCI_OK) { out_s("xhci: out of DMA memory\n"); return rc; }

    /* Run. Interrupts stay off at the controller (USBCMD.INTE = 0): the
     * event ring is drained by xhci_poll(). */
    REG(X.op->usbcmd) = XHCI_CMD_RS | XHCI_CMD_HSEE;
    if (!wait_reg(&REG(X.op->usbsts), XHCI_STS_HCH, 0, 100)) { out_s("xhci: failed to start\n"); return XHCI_ERR_HC; }
    X.up = true;

    rc = command(TRB_TYPE(TRB_NOOP_CMD), 0, NULL);
    out_s(rc == XHCI_OK ? "xhci: running; command ring and event ring OK\n"
                        : "xhci: command ring not responding\n");
    if (rc != XHCI_OK) { X.up = false; return XHCI_ERR_HC; }

    /* Power the ports (if software-controlled) and let USB3 links train. */
    if (X.hcc1 & XHCI_HCC1_PPC)
        for (uint32_t p = 1; p <= X.max_ports; p++) {
            uint32_t sc = REG(X.op->ports[p - 1].portsc);
            if (!(sc & XHCI_PORTSC_PP)) REG(X.op->ports[p - 1].portsc) = (sc & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PP;
        }
    delay_ms(20);

    /* Initial settle: devices already plugged in are debounced and
     * enumerated here (bounded to 600 ms so a dead port cannot stall boot);
     * slower devices are picked up later by xhci_settle_step()/service. */
    uint64_t end = now_ms() + 600;
    while (xhci_settle_step() > 0 && now_ms() < end) delay_ms(5);

    int ifaces = 0;
    for (uint32_t s = 1; s <= X.max_slots; s++)
        if (X.devs[s].used) ifaces += X.devs[s].nhid + (X.devs[s].msc.present ? 1 : 0);
    out_s("xhci: "); out_dec((uint64_t)ifaces); out_s(" interface(s) active after initial settle\n");
    return ifaces;
}

/* ======================================================================== */
/*  5. enumeration                                                             */
/* ======================================================================== */

/* Returns the port speed, or a negative error. USB2 ports must be reset to
 * become enabled; USB3 ports enable themselves after link training. */
static int port_reset(uint32_t port)
{
    volatile uint32_t *sc = &REG(X.op->ports[port - 1].portsc);
    uint32_t v = *sc;
    if (!(v & XHCI_PORTSC_CCS)) return XHCI_ERR_NODEV;
    bool usb3 = X.port_major[port] == 3 || (X.port_major[port] == 0 && XHCI_PORTSC_SPEED(v) >= XHCI_SPEED_SUPER);

    if (usb3) {
        if (!wait_reg(sc, XHCI_PORTSC_PED, XHCI_PORTSC_PED, 300)) {
            *sc = (*sc & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_WPR;          /* warm reset */
            wait_reg(sc, XHCI_PORTSC_PRC | XHCI_PORTSC_WRC, XHCI_PORTSC_PRC, 500);
        }
    } else {
        *sc = (v & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PR;
        if (!wait_reg(sc, XHCI_PORTSC_PR, 0, 500)) return XHCI_ERR_TIMEOUT;
        delay_ms(10);                                                       /* TRSTRCY */
    }
    v = *sc;
    *sc = (v & XHCI_PORTSC_PRESERVE) | (v & XHCI_PORTSC_CHANGES);           /* ack changes */
    if (!(v & XHCI_PORTSC_PED)) return XHCI_ERR_NODEV;
    return (int)XHCI_PORTSC_SPEED(v);
}

static uint8_t interval_for(uint8_t speed, uint8_t binterval)
{
    if (speed == XHCI_SPEED_HIGH || speed >= XHCI_SPEED_SUPER) {    /* 2^(b-1) microframes */
        uint8_t i = binterval ? (uint8_t)(binterval - 1) : 0;
        return i > 15 ? 15 : i;
    }
    uint32_t units = (binterval ? binterval : 1) * 8u;              /* frames -> 125 us */
    uint8_t i = 0;
    while ((1u << (i + 1)) <= units) i++;
    return i < 3 ? 3 : i > 10 ? 10 : i;
}

static bool alloc_device(struct usb_dev *d)
{
    if (!d->out_ctx) {
        d->out_ctx  = dma(X.ctx_size * 32u, 64, &d->out_phys);
        d->in_ctx   = dma(X.ctx_size * 33u, 64, &d->in_phys);
        d->ctrl_buf = dma(512, 64, &d->ctrl_phys);
        if (!d->out_ctx || !d->in_ctx || !d->ctrl_buf) { d->out_ctx = NULL; return false; }
    }
    mem_zero(d->out_ctx, X.ctx_size * 32u);
    mem_zero(d->in_ctx, X.ctx_size * 33u);
    return xhci_ring_init(&d->ep0, XHCI_EP_RING_TRBS);
}

/* Parse the configuration bundle for HID boot interfaces with an
 * interrupt-IN endpoint. */
static void parse_config(struct usb_dev *d, const uint8_t *buf, uint16_t total)
{
    struct hid_if *cur = NULL;
    bool in_msc = false;
    int msc_last = 0;                               /* 1 = IN, 2 = OUT: for SS companions */
    d->nhid = 0;
    for (uint16_t off = 0; off + 2 <= total && buf[off] >= 2; off = (uint16_t)(off + buf[off])) {
        uint8_t type = buf[off + 1];
        if (type == USB_DT_INTERFACE && off + sizeof(struct usb_interface_descriptor) <= total) {
            const struct usb_interface_descriptor *id = (const void *)(buf + off);
            cur = NULL;
            in_msc = false;
            msc_last = 0;
            if (id->bAlternateSetting != 0) continue;
            if (id->bInterfaceClass == USB_CLASS_MASS_STORAGE) {
                if (id->bInterfaceProtocol == 0x50 && !d->msc.present && !d->msc.in_addr) {   /* Bulk-Only */
                    in_msc = true;
                    d->msc.iface = id->bInterfaceNumber;
                    d->msc.subclass = id->bInterfaceSubClass;
                    d->msc.protocol = id->bInterfaceProtocol;
                } else {
                    out_s("usb:   interface "); out_dec(id->bInterfaceNumber);
                    out_s(": mass storage protocol 0x"); out_hex(id->bInterfaceProtocol, 2);
                    out_s(" not supported (Bulk-Only 0x50 only)\n");
                }
                continue;
            }
            if (id->bInterfaceClass != USB_CLASS_HID) continue;
            if (id->bInterfaceSubClass != HID_SUBCLASS_BOOT ||
                (id->bInterfaceProtocol != HID_PROTOCOL_KEYBOARD && id->bInterfaceProtocol != HID_PROTOCOL_MOUSE)) {
                out_s("usb:   interface "); out_dec(id->bInterfaceNumber);
                out_s(": HID without boot protocol (needs a report-descriptor parser), skipped\n");
                continue;
            }
            if (d->nhid >= 2) continue;
            cur = &d->hid[d->nhid];
            cur->kind = id->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD ? USB_KIND_KEYBOARD : USB_KIND_MOUSE;
            cur->iface = id->bInterfaceNumber;
            cur->ep_addr = 0;
        } else if (type == USB_DT_ENDPOINT && in_msc && off + sizeof(struct usb_endpoint_descriptor) <= total) {
            const struct usb_endpoint_descriptor *ep = (const void *)(buf + off);
            if ((ep->bmAttributes & USB_EP_XFER_MASK) != USB_EP_XFER_BULK) continue;
            uint8_t dci = (uint8_t)((ep->bEndpointAddress & 0x0F) * 2 + ((ep->bEndpointAddress & USB_DIR_IN) ? 1 : 0));
            if (ep->bEndpointAddress & USB_DIR_IN) {
                if (d->msc.in_addr) continue;
                d->msc.in_addr = ep->bEndpointAddress; d->msc.in_dci = dci;
                d->msc.in_mps = ep->wMaxPacketSize & 0x7FF; msc_last = 1;
            } else {
                if (d->msc.out_addr) continue;
                d->msc.out_addr = ep->bEndpointAddress; d->msc.out_dci = dci;
                d->msc.out_mps = ep->wMaxPacketSize & 0x7FF; msc_last = 2;
            }
            if (d->msc.in_addr && d->msc.out_addr) d->msc.present = true;
        } else if (type == USB_DT_SS_EP_COMPANION && in_msc && msc_last && off + 3 <= total) {
            if (msc_last == 1) d->msc.in_burst = buf[off + 2]; else d->msc.out_burst = buf[off + 2];
        } else if (type == USB_DT_ENDPOINT && cur && !cur->ep_addr &&
                   off + sizeof(struct usb_endpoint_descriptor) <= total) {
            const struct usb_endpoint_descriptor *ep = (const void *)(buf + off);
            if ((ep->bmAttributes & USB_EP_XFER_MASK) == USB_EP_XFER_INT && (ep->bEndpointAddress & USB_DIR_IN)) {
                cur->ep_addr = ep->bEndpointAddress;
                cur->dci = (uint8_t)((ep->bEndpointAddress & 0x0F) * 2 + 1);
                cur->mps = ep->wMaxPacketSize & 0x7FF;
                cur->binterval = ep->bInterval;
                d->nhid++;
                cur = NULL;
            }
        }
    }
}

/* Add the interrupt-IN (HID) and bulk (mass storage) endpoints with one
 * Configure Endpoint command. */
static int configure_endpoints(struct usb_dev *d)
{
    mem_zero(d->in_ctx, X.ctx_size * 33u);
    struct xhci_input_ctrl_ctx *ic = in_ctrl(d);
    struct xhci_slot_ctx *sl = in_slot(d);
    mem_copy(sl, out_slot(d), sizeof *sl);
    sl->dw3 = 0;
    uint32_t max_dci = 1;
    ic->add_flags = 1;                                   /* A0: slot context */

    for (int i = 0; i < d->nhid; i++) {
        struct hid_if *h = &d->hid[i];
        if (!xhci_ring_init(&h->ring, XHCI_EP_RING_TRBS)) return XHCI_ERR_NOMEM;
        if (!h->buf && !(h->buf = dma(XHCI_HID_BUFFERS * 64, 64, &h->buf_phys))) return XHCI_ERR_NOMEM;
        struct xhci_ep_ctx *ep = in_ep(d, h->dci);
        ep->dw0 = (uint32_t)interval_for(d->speed, h->binterval) << 16;
        ep->dw1 = (3u << 1) | (EP_TYPE_INTERRUPT_IN << 3) | ((uint32_t)h->mps << 16);   /* CErr = 3 */
        ep->deq = h->ring.phys | 1;                                                       /* DCS = 1 */
        ep->dw4 = (uint32_t)h->mps | ((uint32_t)h->mps << 16);  /* avg TRB length, max ESIT payload */
        ic->add_flags |= 1u << h->dci;
        if (h->dci > max_dci) max_dci = h->dci;
    }
    if (d->msc.present) {
        struct msc_if *m = &d->msc;
        if (!xhci_ring_init(&m->in_ring, XHCI_EP_RING_TRBS) || !xhci_ring_init(&m->out_ring, XHCI_EP_RING_TRBS))
            return XHCI_ERR_NOMEM;
        /* 64 KiB-aligned: one Normal TRB may not cross a 64 KiB boundary */
        if (!m->bounce && !(m->bounce = dma(XHCI_BULK_MAX, XHCI_BULK_MAX, &m->bounce_phys))) return XHCI_ERR_NOMEM;
        for (int dir = 0; dir < 2; dir++) {
            uint8_t dci = dir ? m->in_dci : m->out_dci;
            struct xhci_ep_ctx *ep = in_ep(d, dci);
            ep->dw0 = 0;
            ep->dw1 = (3u << 1) | ((uint32_t)(dir ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT) << 3) |
                      ((uint32_t)(dir ? m->in_burst : m->out_burst) << 8) |
                      ((uint32_t)(dir ? m->in_mps : m->out_mps) << 16);
            ep->deq = (dir ? m->in_ring.phys : m->out_ring.phys) | 1;
            ep->dw4 = 3072;                                     /* average TRB length (bulk) */
            ic->add_flags |= 1u << dci;
            if (dci > max_dci) max_dci = dci;
        }
    }
    sl->dw0 = (sl->dw0 & ~(0x1Fu << 27)) | (max_dci << 27);    /* context entries */
    return command(TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(d->slot), d->in_phys, NULL);
}

static void start_reports(struct usb_dev *d, struct hid_if *h)
{
    mem_zero(h->prev, sizeof h->prev);
    h->repeat_usage = 0;
    h->needs_reset = false;
    h->active = true;
    for (unsigned b = 0; b < XHCI_HID_BUFFERS; b++) requeue_report(d, h, b);
}

static int enumerate_port(uint32_t port)
{
    int speed = port_reset(port);
    if (speed < 0) return speed;

    uint8_t slot = 0;
    if (command(TRB_TYPE(TRB_ENABLE_SLOT), 0, &slot) != XHCI_OK || !slot || slot > X.max_slots) {
        out_s("usb: port "); out_dec(port); out_s(": no device slot available\n");
        return XHCI_ERR_CMD;
    }
    struct usb_dev *d = &X.devs[slot];
    struct usb_dev_mem { void *o, *i, *c; uint64_t op, ip, cp; struct xhci_ring e; struct hid_if h[2]; struct msc_if m; } keep =
        { d->out_ctx, d->in_ctx, d->ctrl_buf, d->out_phys, d->in_phys, d->ctrl_phys, d->ep0, { d->hid[0], d->hid[1] }, d->msc };
    mem_zero(d, sizeof *d);                         /* keep DMA memory from a previous user */
    d->out_ctx = keep.o; d->in_ctx = keep.i; d->ctrl_buf = keep.c;
    d->out_phys = keep.op; d->in_phys = keep.ip; d->ctrl_phys = keep.cp;
    d->ep0.trbs = keep.e.trbs; d->ep0.phys = keep.e.phys;
    for (int i = 0; i < 2; i++) {
        d->hid[i].ring.trbs = keep.h[i].ring.trbs; d->hid[i].ring.phys = keep.h[i].ring.phys;
        d->hid[i].buf = keep.h[i].buf; d->hid[i].buf_phys = keep.h[i].buf_phys;
    }
    d->msc.in_ring.trbs = keep.m.in_ring.trbs;   d->msc.in_ring.phys = keep.m.in_ring.phys;
    d->msc.out_ring.trbs = keep.m.out_ring.trbs; d->msc.out_ring.phys = keep.m.out_ring.phys;
    d->msc.bounce = keep.m.bounce; d->msc.bounce_phys = keep.m.bounce_phys;
    d->slot = slot; d->port = (uint8_t)port; d->speed = (uint8_t)speed;
    if (!alloc_device(d)) { out_s("usb: out of DMA memory\n"); goto fail_slot; }
    d->used = true;
    X.port_slot[port] = slot;
    X.dcbaa[slot] = d->out_phys;

    /* Address Device: input context with slot + EP0 (xHCI 4.3.3) */
    d->mps0 = speed == XHCI_SPEED_SUPER || speed == XHCI_SPEED_SUPER_PLUS ? 512
            : speed == XHCI_SPEED_HIGH ? 64 : 8;
    in_ctrl(d)->add_flags = (1u << 0) | (1u << 1);
    in_slot(d)->dw0 = ((uint32_t)speed << 20) | (1u << 27);      /* speed, 1 context entry */
    in_slot(d)->dw1 = port << 16;                                /* root hub port number   */
    struct xhci_ep_ctx *ep0 = in_ep(d, 1);
    ep0->dw1 = (3u << 1) | (EP_TYPE_CONTROL << 3) | ((uint32_t)d->mps0 << 16);
    ep0->deq = d->ep0.phys | 1;
    ep0->dw4 = 8;
    if (command(TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot), d->in_phys, NULL) != XHCI_OK) goto fail_slot;

    /* Device descriptor: first 8 bytes give bMaxPacketSize0 */
    if (get_descriptor(d, USB_DT_DEVICE, 0, 0, &d->desc, 8) != XHCI_OK) goto fail_dev;
    uint16_t mps = speed >= XHCI_SPEED_SUPER ? (uint16_t)(1u << d->desc.bMaxPacketSize0) : d->desc.bMaxPacketSize0;
    if (mps && mps != d->mps0) {                                 /* Evaluate Context */
        d->mps0 = mps;
        mem_zero(d->in_ctx, X.ctx_size * 33u);
        in_ctrl(d)->add_flags = 1u << 1;
        in_ep(d, 1)->dw1 = (3u << 1) | (EP_TYPE_CONTROL << 3) | ((uint32_t)mps << 16);
        if (command(TRB_TYPE(TRB_EVALUATE_CTX) | TRB_SLOT(slot), d->in_phys, NULL) != XHCI_OK) goto fail_dev;
    }
    if (get_descriptor(d, USB_DT_DEVICE, 0, 0, &d->desc, sizeof d->desc) != XHCI_OK) goto fail_dev;

    struct usb_device_info *info = &d->info;
    info->present = true;
    info->slot = slot; info->port = (uint8_t)port; info->speed = (uint8_t)speed;
    info->vendor = d->desc.idVendor; info->product = d->desc.idProduct;
    info->dev_class = d->desc.bDeviceClass;
    info->generation = ++X.generation;
    get_string(d, d->desc.iManufacturer, info->manufacturer, sizeof info->manufacturer);
    get_string(d, d->desc.iProduct, info->product_name, sizeof info->product_name);

    out_s("usb: port "); out_dec(port); out_s(" ("); out_s(xhci_speed_name((uint8_t)speed));
    out_s("): slot "); out_dec(slot); out_s(", USB "); out_hex(d->desc.bcdUSB >> 8, 1); out_c('.');
    out_hex((d->desc.bcdUSB >> 4) & 0xF, 1); out_s(", ID "); out_hex(info->vendor, 4); out_c(':');
    out_hex(info->product, 4); out_s(", \""); out_s(info->manufacturer); out_c(' ');
    out_s(info->product_name); out_s("\"\n");

    if (d->desc.bDeviceClass == USB_CLASS_HUB) {
        info->kind = USB_KIND_HUB;
        out_s("usb:   hub: downstream devices need route strings/TTs, not enumerated\n");
        return 0;
    }

    /* Configuration bundle */
    uint8_t cfg[512];
    if (get_descriptor(d, USB_DT_CONFIG, 0, 0, cfg, 9) != XHCI_OK) goto fail_dev;
    const struct usb_config_descriptor *cd = (const void *)cfg;
    uint16_t total = cd->wTotalLength > sizeof cfg ? sizeof cfg : cd->wTotalLength;
    uint8_t config_value = cd->bConfigurationValue;
    if (get_descriptor(d, USB_DT_CONFIG, 0, 0, cfg, total) != XHCI_OK) goto fail_dev;
    parse_config(d, cfg, total);

    if (control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                config_value, 0, 0, NULL) != XHCI_OK) goto fail_dev;
    if (!d->nhid && !d->msc.present) { out_s("usb:   no supported interface; device left configured but unused\n"); return 0; }

    /* HID class setup: boot protocol, no idle repeats (we repeat ourselves) */
    for (int i = 0; i < d->nhid; i++) {
        struct hid_if *h = &d->hid[i];
        if (control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_PROTOCOL,
                    HID_BOOT_PROTOCOL, h->iface, 0, NULL) != XHCI_OK)
            out_s("usb:   SET_PROTOCOL(boot) failed; assuming boot reports\n");
        control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_IDLE,
                0, h->iface, 0, NULL);                  /* optional: many mice stall it */
    }
    if (configure_endpoints(d) != XHCI_OK) goto fail_dev;

    for (int i = 0; i < d->nhid; i++) {
        struct hid_if *h = &d->hid[i];
        start_reports(d, h);
        out_s("usb:   HID boot "); out_s(h->kind == USB_KIND_KEYBOARD ? "keyboard" : "mouse");
        out_s(" on interface "); out_dec(h->iface); out_s(", EP 0x"); out_hex(h->ep_addr, 2);
        out_s(" (DCI "); out_dec(h->dci); out_s("), "); out_dec(h->mps); out_s(" B every ");
        uint32_t period_us = 125u << interval_for((uint8_t)speed, h->binterval);   /* as programmed */
        if (period_us >= 1000) { out_dec(period_us / 1000); out_s(" ms\n"); }
        else { out_dec(period_us); out_s(" us\n"); }
    }
    if (d->msc.present) {
        out_s("usb:   mass storage (Bulk-Only, subclass 0x"); out_hex(d->msc.subclass, 2);
        out_s(") on interface "); out_dec(d->msc.iface); out_s(": bulk IN 0x"); out_hex(d->msc.in_addr, 2);
        out_s(" / OUT 0x"); out_hex(d->msc.out_addr, 2); out_s(", "); out_dec(d->msc.in_mps); out_s(" B packets");
        if (d->msc.in_burst) { out_s(", burst "); out_dec(d->msc.in_burst + 1u); }
        out_c('\n');
    }
    info->kind = d->nhid ? d->hid[0].kind : USB_KIND_STORAGE;
    info->hid_count = (uint8_t)d->nhid;
    return d->nhid + (d->msc.present ? 1 : 0);

fail_dev:
    out_s("usb: port "); out_dec(port); out_s(": enumeration failed\n");
fail_slot:
    d->used = false;
    d->info.present = false;
    X.port_slot[port] = 0;
    X.dcbaa[slot] = 0;
    command(TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), 0, NULL);
    return XHCI_ERR_NODEV;
}

static void release_port(uint32_t port)
{
    uint8_t slot = X.port_slot[port];
    if (!slot) return;
    struct usb_dev *d = &X.devs[slot];
    irqstate_t s = irq_off();
    for (int i = 0; i < d->nhid; i++) d->hid[i].active = false;
    d->msc.present = false;
    d->msc.in_addr = d->msc.out_addr = 0;
    d->used = false;
    d->info.present = false;
    irq_restore(s);
    command(TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), 0, NULL);
    X.dcbaa[slot] = 0;
    X.port_slot[port] = 0;
    out_s("usb: port "); out_dec(port); out_s(": device removed (slot "); out_dec(slot); out_s(" freed)\n");
}

/* ======================================================================== */
/*  7b. settling, hot-plug and endpoint recovery (thread context)              */
/* ======================================================================== */

/* One pass over every root port. Port status is read directly from PORTSC
 * (events only make us look sooner), so a missed or coalesced Port Status
 * Change event cannot lose a device. Caller holds the controller. */
static int settle_locked(void)
{
    xhci_poll();
    X.port_pending = 0;
    uint64_t now = now_ms();
    int pending = 0;

    for (uint32_t p = 1; p <= X.max_ports; p++) {
        volatile uint32_t *sc = &REG(X.op->ports[p - 1].portsc);
        uint32_t v = *sc;
        bool csc = (v & XHCI_PORTSC_CSC) != 0;
        if (v & XHCI_PORTSC_CHANGES) *sc = (v & XHCI_PORTSC_PRESERVE) | (v & XHCI_PORTSC_CHANGES);
        uint8_t *state = &X.track[p].state;

        if (!(v & XHCI_PORTSC_CCS)) {                       /* nothing (or no longer) attached */
            if (X.port_slot[p]) release_port(p);
            if (*state == PT_DEBOUNCE) { out_s("usb: port "); out_dec(p); out_s(": connection lost while debouncing\n"); }
            *state = PT_IDLE;
            X.track[p].attempts = 0;
            continue;
        }
        if (csc && *state == PT_DONE && X.port_slot[p]) {  /* unplugged and replugged quickly */
            release_port(p);
            *state = PT_IDLE;
        }
        switch (*state) {
        case PT_IDLE:
            *state = PT_DEBOUNCE;
            X.track[p].when = now + DEBOUNCE_MS;
            X.track[p].attempts = 0;
            out_s("usb: port "); out_dec(p); out_s(": connect detected (PORTSC 0x"); out_hex(v, 8);
            out_s("), debouncing\n");
            pending++;
            break;
        case PT_DEBOUNCE:
            if (now < X.track[p].when) { pending++; break; }
            if (enumerate_port(p) >= 0) {
                *state = PT_DONE;
            } else if (++X.track[p].attempts < MAX_ENUM_ATTEMPTS) {
                uint32_t backoff = 250u * X.track[p].attempts;
                X.track[p].when = now_ms() + backoff;
                out_s("usb: port "); out_dec(p); out_s(": enumeration attempt "); out_dec(X.track[p].attempts);
                out_s(" failed, retrying in "); out_dec(backoff); out_s(" ms\n");
                pending++;
            } else {
                *state = PT_FAILED;
                X.enum_failures++;
                out_s("usb: port "); out_dec(p); out_s(": giving up after "); out_dec(MAX_ENUM_ATTEMPTS);
                out_s(" attempts (unplug/replug to retry)\n");
            }
            break;
        default:
            break;
        }
    }
    return pending;
}

int xhci_settle_step(void)
{
    if (!X.up) return 0;
    if (!try_acquire()) return -1;
    int pending = settle_locked();
    release();
    return pending;
}

static void recover_endpoints_locked(void)
{
    if (!X.ep_pending) return;
    X.ep_pending = false;
    for (uint32_t slot = 1; slot <= X.max_slots; slot++) {
        struct usb_dev *d = &X.devs[slot];
        if (!d->used) continue;
        for (int i = 0; i < d->nhid; i++) {
            struct hid_if *h = &d->hid[i];
            if (!h->needs_reset) continue;
            out_s("usb: slot "); out_dec(slot); out_s(" EP 0x"); out_hex(h->ep_addr, 2);
            out_s(" halted, resetting endpoint\n");
            if (endpoint_reset(d, h->dci, &h->ring) == XHCI_OK) start_reports(d, h);
        }
    }
}

void xhci_service(void)
{
    if (!X.up || !try_acquire()) return;        /* someone else is busy: next time */
    settle_locked();
    recover_endpoints_locked();
    release();
}

bool xhci_running(void) { return X.up && !(REG(X.op->usbsts) & XHCI_STS_HCH); }

/* ======================================================================== */
/*  8. class-driver interface (mass storage)                                   */
/* ======================================================================== */
static struct usb_dev *msc_dev(uint8_t slot)
{
    if (!X.up || slot == 0 || slot > X.max_slots) return NULL;
    struct usb_dev *d = &X.devs[slot];
    return (d->used && d->msc.present) ? d : NULL;
}

static void fill_msc_info(const struct usb_dev *d, struct xhci_msc_info *o)
{
    o->slot = d->slot; o->port = d->port; o->speed = d->speed;
    o->iface = d->msc.iface; o->subclass = d->msc.subclass; o->protocol = d->msc.protocol;
    o->in_ep = d->msc.in_addr; o->out_ep = d->msc.out_addr;
    o->in_burst = d->msc.in_burst; o->out_burst = d->msc.out_burst;
    o->in_mps = d->msc.in_mps; o->out_mps = d->msc.out_mps;
    o->generation = d->info.generation;
    o->dev = &d->info;
}

int xhci_msc_count(void)
{
    int n = 0;
    for (uint32_t s = 1; s <= X.max_slots; s++) if (msc_dev((uint8_t)s)) n++;
    return n;
}

int xhci_msc_get(int index, struct xhci_msc_info *out)
{
    for (uint32_t s = 1; s <= X.max_slots; s++) {
        struct usb_dev *d = msc_dev((uint8_t)s);
        if (d && index-- == 0) { fill_msc_info(d, out); return XHCI_OK; }
    }
    return XHCI_ERR_NODEV;
}

int xhci_msc_lookup(uint8_t slot, struct xhci_msc_info *out)
{
    struct usb_dev *d = msc_dev(slot);
    if (!d) return XHCI_ERR_NODEV;
    fill_msc_info(d, out);
    return XHCI_OK;
}

int xhci_control_transfer(uint8_t slot, uint8_t reqtype, uint8_t request, uint16_t value,
                          uint16_t index, uint16_t length, void *data)
{
    if (!X.up || slot == 0 || slot > X.max_slots || !X.devs[slot].used) return XHCI_ERR_NODEV;
    if (!acquire(5000)) return XHCI_ERR_BUSY;
    int rc = X.devs[slot].used ? control(&X.devs[slot], reqtype, request, value, index, length, data)
                               : XHCI_ERR_NODEV;
    release();
    return rc;
}

static uint8_t ep_state(struct usb_dev *d, uint8_t dci)
{
    /* contexts are 32/64-byte aligned in DMA memory: dw0 is a naturally aligned word */
    uintptr_t addr = (uintptr_t)ctx_at(d->out_ctx, dci);
    return (uint8_t)(*(const volatile uint32_t *)addr & 7);   /* 1 running, 2 halted, 3 stopped, 4 error */
}

int xhci_bulk_transfer(uint8_t slot, bool in, void *buf, uint32_t len, uint32_t *actual, uint32_t timeout_ms)
{
    if (actual) *actual = 0;
    if (len > XHCI_BULK_MAX) return XHCI_ERR_UNSUPPORTED;
    if (!msc_dev(slot)) return XHCI_ERR_NODEV;
    if (!acquire(5000)) return XHCI_ERR_BUSY;
    struct usb_dev *d = msc_dev(slot);
    if (!d) { release(); return XHCI_ERR_NODEV; }

    struct msc_if *m = &d->msc;
    struct xhci_ring *r = in ? &m->in_ring : &m->out_ring;
    uint8_t dci = in ? m->in_dci : m->out_dci;
    if (!in && len) mem_copy(m->bounce, buf, len);

    m->done = false;
    m->wait_dci = dci;
    m->wait_trb = xhci_ring_enqueue(r, m->bounce_phys, len,
                                    TRB_TYPE(TRB_NORMAL) | TRB_IOC | (in ? TRB_ISP : 0));
    ring_doorbell(d->slot, dci);

    uint64_t end = now_ms() + timeout_ms;
    for (uint32_t spin = 0; !m->done; spin++) {
        xhci_poll();
        if (m->done) break;
        if (now_ms() >= end) break;
        if (spin > 200) delay_us(50);
    }
    int rc = XHCI_OK;
    if (!m->done) {
        /* Cancel the TD so a late completion cannot land in a reused buffer. */
        X.timeouts++;
        X.last_xfer_code = 0xFF;
        out_s("usb: slot "); out_dec(slot); out_s(in ? " bulk IN" : " bulk OUT");
        out_s(" timed out after "); out_dec(timeout_ms); out_s(" ms, cancelling\n");
        command(TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(d->slot) | TRB_EP(dci), 0, NULL);
        uint64_t deq = (r->phys + (uint64_t)r->enq * sizeof(struct xhci_trb)) | r->cycle;
        command(TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(d->slot) | TRB_EP(dci), deq, NULL);
        rc = XHCI_ERR_TIMEOUT;
    } else {
        uint8_t cc = m->code;
        X.last_xfer_code = cc;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            uint32_t got = m->residual <= len ? len - m->residual : 0;
            if (actual) *actual = got;
            if (in && got) mem_copy(buf, m->bounce, got);
        } else {
            /* stall / transaction error: the host endpoint is now Halted */
            if (cc == CC_STALL) X.stalls++;
            out_s("usb: slot "); out_dec(slot); out_s(in ? " bulk IN: " : " bulk OUT: "); out_s(cc_name(cc)); out_c('\n');
            endpoint_reset(d, dci, r);
            rc = cc == CC_STALL ? XHCI_ERR_STALL : XHCI_ERR_XFER;
        }
    }
    m->wait_dci = 0;
    m->wait_trb = 0;
    release();
    return rc;
}

int xhci_clear_halt(uint8_t slot, bool in)
{
    if (!msc_dev(slot)) return XHCI_ERR_NODEV;
    if (!acquire(5000)) return XHCI_ERR_BUSY;
    struct usb_dev *d = msc_dev(slot);
    if (!d) { release(); return XHCI_ERR_NODEV; }
    uint8_t dci = in ? d->msc.in_dci : d->msc.out_dci;
    struct xhci_ring *r = in ? &d->msc.in_ring : &d->msc.out_ring;
    uint8_t st = ep_state(d, dci);
    if (st == 2 || st == 4) endpoint_reset(d, dci, r);           /* host side first */
    int rc = control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                     0 /* ENDPOINT_HALT */, in ? d->msc.in_addr : d->msc.out_addr, 0, NULL);
    release();
    return rc;
}

/* ======================================================================== */
/*  9. diagnostics                                                             */
/* ======================================================================== */
static const char *pls_name(uint32_t pls)
{
    static const char *n[16] = { "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
                                 "Recovery", "HotReset", "Compliance", "Test", "?", "?", "?", "Resume" };
    return n[pls & 15];
}

static void out_flag(bool on, const char *name) { if (on) { out_c(' '); out_s(name); } }

void xhci_dump_state(void (*putc_fn)(char c))
{
    void (*saved)(char) = g_putc;
    if (putc_fn) g_putc = putc_fn;

    if (!X.base) { out_s("xHCI: no controller initialised\n"); g_putc = saved; return; }
    uint32_t cmd = REG(X.op->usbcmd), sts = REG(X.op->usbsts);
    out_s("xHCI "); out_hex(X.version >> 8, 1); out_c('.'); out_hex(X.version & 0xFF, 2);
    out_s(", "); out_dec(X.max_slots); out_s(" slots enabled, "); out_dec(X.max_ports); out_s(" ports, driver ");
    out_s(X.up ? "running" : "NOT running"); out_c('\n');
    out_s("  USBCMD  0x"); out_hex(cmd, 8); out_s(" ["); out_flag(cmd & XHCI_CMD_RS, "RS"); out_flag(cmd & XHCI_CMD_HCRST, "HCRST");
    out_flag(cmd & XHCI_CMD_INTE, "INTE"); out_flag(cmd & XHCI_CMD_HSEE, "HSEE"); out_s(" ]\n");
    out_s("  USBSTS  0x"); out_hex(sts, 8); out_s(" ["); out_flag(sts & XHCI_STS_HCH, "HCHalted");
    out_flag(sts & XHCI_STS_HSE, "HostSystemError"); out_flag(sts & XHCI_STS_EINT, "EINT");
    out_flag(sts & XHCI_STS_PCD, "PortChange"); out_flag(sts & XHCI_STS_CNR, "NotReady");
    out_flag(sts & XHCI_STS_HCE, "HostControllerError"); out_s(" ]\n");
    out_s("  CRCR    0x"); out_hex(REG(X.op->crcr_lo), 8); out_s(" (command ring "); out_s(REG(X.op->crcr_lo) & XHCI_CRCR_CRR ? "running" : "idle");
    out_s(")  DCBAAP 0x"); out_hex((uint64_t)REG(X.op->dcbaap_hi) << 32 | REG(X.op->dcbaap_lo), 16);
    out_s("  CONFIG "); out_dec(REG(X.op->config) & 0xFF); out_c('\n');
    struct xhci_intr_regs *ir = &X.rt->ir[0];
    out_s("  IR0     IMAN 0x"); out_hex(REG(ir->iman), 8); out_s(" ERSTSZ "); out_dec(REG(ir->erstsz));
    out_s(" ERDP 0x"); out_hex((uint64_t)REG(ir->erdp_hi) << 32 | REG(ir->erdp_lo), 16);
    out_s(" (driver dequeue "); out_dec(X.ev.deq); out_s(", cycle "); out_dec(X.ev.cycle); out_s(")\n");
    out_s("  handoff "); out_s(X.handoff ? X.handoff : "?"); out_c('\n');
    out_s("  errors  last command type "); out_dec(X.last_cmd_type); out_s(" -> ");
    out_s(X.last_cmd_code == 0xFF ? "timeout" : cc_name(X.last_cmd_code));
    out_s("; last transfer -> "); out_s(X.last_xfer_code == 0xFF ? "timeout" : X.last_xfer_code ? cc_name(X.last_xfer_code) : "none");
    out_s("; "); out_dec(X.timeouts); out_s(" timeouts, "); out_dec(X.stalls); out_s(" stalls, ");
    out_dec(X.enum_failures); out_s(" failed ports\n");

    for (uint32_t p = 1; p <= X.max_ports; p++) {
        uint32_t v = REG(X.op->ports[p - 1].portsc);
        out_s("  port "); if (p < 10) out_c(' '); out_dec(p);
        out_s(X.port_major[p] == 3 ? " USB3" : X.port_major[p] == 2 ? " USB2" : " USB?");
        out_s(" PORTSC 0x"); out_hex(v, 8); out_c(' ');
        out_s(v & XHCI_PORTSC_CCS ? "connected" : "empty    ");
        out_flag(v & XHCI_PORTSC_PED, "enabled"); out_flag(v & XHCI_PORTSC_PP, "powered");
        out_flag(v & XHCI_PORTSC_PR, "IN-RESET"); out_flag(v & XHCI_PORTSC_OCA, "OVERCURRENT");
        out_s(" link "); out_s(pls_name((v & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT));
        if (v & XHCI_PORTSC_CCS) { out_s(" speed "); out_dec(XHCI_PORTSC_SPEED(v)); }
        if (v & XHCI_PORTSC_CHANGES) { out_s(" changes 0x"); out_hex((v & XHCI_PORTSC_CHANGES) >> 17, 2); }
        static const char *st[] = { "", " [debouncing]", " [enumerated]", " [FAILED]" };
        out_s(st[X.track[p].state & 3]);
        if (X.port_slot[p]) {
            const struct usb_dev *d = &X.devs[X.port_slot[p]];
            out_s(" slot "); out_dec(X.port_slot[p]); out_s(" \""); out_s(d->info.product_name); out_c('"');
            if (d->msc.present) out_s(" (mass storage)");
        }
        out_c('\n');
    }
    g_putc = saved;
}

int xhci_device_count(void)
{
    int n = 0;
    for (uint32_t s = 1; s <= X.max_slots; s++) if (X.devs[s].info.present) n++;
    return n;
}

const struct usb_device_info *xhci_device(int index)
{
    for (uint32_t s = 1; s <= X.max_slots; s++)
        if (X.devs[s].info.present && index-- == 0) return &X.devs[s].info;
    return NULL;
}

/* ======================================================================== */
/*  6. HID boot protocol                                                       */
/* ======================================================================== */
static const char kbd_lower[0x39] = {
    0, 0, 0, 0, 'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t',
    'u','v','w','x','y','z','1','2','3','4','5','6','7','8','9','0','\n', 27, '\b','\t',' ',
    '-','=','[',']','\\','#',';','\'','`',',','.','/',
};
static const char kbd_upper[0x39] = {
    0, 0, 0, 0, 'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T',
    'U','V','W','X','Y','Z','!','@','#','$','%','^','&','*','(',')','\n', 27, '\b','\t',' ',
    '_','+','{','}','|','~',':','"','~','<','>','?',
};

char usb_hid_usage_to_ascii(uint8_t usage, uint8_t mods, bool caps)
{
    if (usage >= 0x54 && usage <= 0x63) {               /* keypad, Num Lock assumed on */
        static const char kp[] = "/*-+\n1234567890.";
        return kp[usage - 0x54];
    }
    if (usage >= sizeof kbd_lower) return 0;
    bool shift = (mods & HID_MOD_SHIFT) != 0;
    bool letter = usage >= HID_KEY_A && usage <= HID_KEY_Z;
    char c = (shift ^ (letter && caps)) ? kbd_upper[usage] : kbd_lower[usage];
    if (c && letter && (mods & HID_MOD_CTRL)) c = (char)(c - (c >= 'a' ? 'a' : 'A') + 1);   /* ^A..^Z */
    return c;
}

static uint8_t device_index(const struct usb_dev *d)
{
    uint8_t n = 0;
    for (uint32_t s = 1; s < d->slot; s++) if (X.devs[s].info.present) n++;
    return n;
}

static void emit_key(const struct usb_dev *d, uint8_t usage, uint8_t mods, bool pressed, bool repeat)
{
    if (pressed && usage == HID_KEY_CAPSLOCK && !repeat) X.caps_lock = !X.caps_lock;
    if (!X.plat.on_key) return;
    struct usb_key_event ev = {
        .usage = usage, .modifiers = mods, .pressed = pressed, .repeat = repeat,
        .ascii = pressed ? usb_hid_usage_to_ascii(usage, mods, X.caps_lock) : 0,
        .device = device_index(d),
    };
    X.plat.on_key(&ev);
}

static bool in_report(const uint8_t *keys, uint8_t usage)
{
    for (int i = 0; i < 6; i++) if (keys[i] == usage) return true;
    return false;
}

static void keyboard_report(struct usb_dev *d, struct hid_if *h, const uint8_t *r, uint32_t len)
{
    uint8_t rep[8] = { 0 };
    mem_copy(rep, r, len < 8 ? len : 8);
    if (rep[2] == HID_KEY_ERR_ROLLOVER) return;         /* too many keys: keep old state */
    uint8_t mods = rep[0];

    for (int i = 2; i < 8; i++)                           /* releases */
        if (h->prev[i] > 3 && !in_report(rep + 2, h->prev[i])) {
            emit_key(d, h->prev[i], mods, false, false);
            if (h->repeat_usage == h->prev[i]) h->repeat_usage = 0;
        }
    for (int i = 2; i < 8; i++)                           /* presses */
        if (rep[i] > 3 && !in_report(h->prev + 2, rep[i])) {
            emit_key(d, rep[i], mods, true, false);
            if (rep[i] != HID_KEY_CAPSLOCK) {
                h->repeat_usage = rep[i];
                h->repeat_next = X.plat.now_ms ? X.plat.now_ms() + 500 : 0;   /* typematic delay */
            }
        }
    mem_copy(h->prev, rep, 8);
}

static void mouse_report(struct usb_dev *d, const uint8_t *r, uint32_t len)
{
    if (len < 3 || !X.plat.on_mouse) return;
    struct usb_mouse_event ev = {
        .buttons = r[0] & 0x07,
        .dx = (int8_t)r[1], .dy = (int8_t)r[2], .wheel = len >= 4 ? (int8_t)r[3] : 0,
        .device = device_index(d),
    };
    X.plat.on_mouse(&ev);
}

static void hid_report(struct usb_dev *d, struct hid_if *h, const uint8_t *r, uint32_t len)
{
    d->info.reports++;
    if (h->kind == USB_KIND_KEYBOARD) keyboard_report(d, h, r, len);
    else mouse_report(d, r, len);
}

/* Typematic repeat: 500 ms delay, then ~30 characters per second. */
static void key_repeat(void)
{
    if (!X.plat.now_ms) return;
    uint64_t now = 0;
    for (uint32_t s = 1; s <= X.max_slots; s++) {
        struct usb_dev *d = &X.devs[s];
        if (!d->used) continue;
        for (int i = 0; i < d->nhid; i++) {
            struct hid_if *h = &d->hid[i];
            if (!h->active || !h->repeat_usage) continue;
            if (!now) now = X.plat.now_ms();
            if (now < h->repeat_next) continue;
            emit_key(d, h->repeat_usage, h->prev[0], true, true);
            h->repeat_next = now + 33;
        }
    }
}
