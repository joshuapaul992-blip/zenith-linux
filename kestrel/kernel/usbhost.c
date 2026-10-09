/* kernel/usbhost.c -- glue between the PCI enumerator, the standalone xHCI
 * driver and Kestrel's input system.
 *
 *   usb_init()          boot: first xHCI controller -> BAR0, MMIO + bus
 *                       mastering, uncached mapping, xhci_init(); installs
 *                       xhci_poll() on the 1 kHz timer tick
 *   usb_start_thread()  after the scheduler: "usbd" thread for hot-plug
 *   key events          translated to Kestrel key codes and injected into the
 *                       same queue as PS/2, so the boot manager and console
 *                       work with a USB-only keyboard
 *   mouse events        accumulate an absolute pointer drawn as an overlay
 */
#include <kernel/usbhost.h>
#include <kernel/keyboard.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <kernel/fb.h>
#include <kernel/klog.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/block.h>
#include <kernel/posix.h>
#include "pci.h"
#include "xhci.h"
#include "usb_legacy.h"
#include "usb_msc.h"

static bool usb_up;
static int  mouse_x, mouse_y, ptr_x = -1, ptr_y = -1;
static uint8_t mouse_buttons;
static uint32_t mouse_reports, key_events;

/* ---- platform hooks -------------------------------------------------------- */
/* PMM frames: zeroed, identity mapped, below 4 GiB. Alignments above a page
 * (the 64 KiB mass-storage bounce buffer, which must not cross a 64 KiB
 * boundary) are met by over-allocating and returning the unused frames. */
static void *dma_alloc(size_t size, size_t align, uint64_t *phys)
{
    size_t frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    *phys = 0;
    if (align <= PAGE_SIZE) {
        uint64_t pa = pmm_alloc_contig(frames);
        *phys = pa;
        return pa ? (void *)pa : NULL;
    }
    if (align & (align - 1)) return NULL;
    size_t extra = align / PAGE_SIZE - 1;
    uint64_t raw = pmm_alloc_contig(frames + extra);
    if (!raw) return NULL;
    uint64_t pa = (raw + align - 1) & ~((uint64_t)align - 1);
    size_t lead = (size_t)((pa - raw) / PAGE_SIZE);
    if (lead) pmm_free_contig(raw, lead);
    if (extra - lead) pmm_free_contig(pa + frames * PAGE_SIZE, extra - lead);
    *phys = pa;
    return (void *)pa;
}

static void *map_mmio(uint64_t phys, size_t size)
{
    vmm_set_uncached(phys, size);
    return (void *)phys;
}
static void *map_mmio_legacy(uint64_t phys, uint32_t size) { return map_mmio(phys, size); }

/* ---- keyboard: HID usage -> Kestrel key code ------------------------------ */
static uint16_t usage_to_key(uint8_t u)
{
    switch (u) {
    case HID_KEY_ENTER: case HID_KEY_KP_ENTER: return KEY_ENTER;
    case HID_KEY_ESCAPE:    return KEY_ESC;
    case HID_KEY_BACKSPACE: return KEY_BACKSPACE;
    case HID_KEY_TAB:       return KEY_TAB;
    case HID_KEY_UP:        return KEY_UP;
    case HID_KEY_DOWN:      return KEY_DOWN;
    case HID_KEY_LEFT:      return KEY_LEFT;
    case HID_KEY_RIGHT:     return KEY_RIGHT;
    case HID_KEY_HOME:      return KEY_HOME;
    case HID_KEY_END:       return KEY_END;
    case HID_KEY_PAGEUP:    return KEY_PGUP;
    case HID_KEY_PAGEDOWN:  return KEY_PGDN;
    case HID_KEY_INSERT:    return KEY_INSERT;
    case HID_KEY_DELETE:    return KEY_DELETE;
    }
    if (u >= HID_KEY_F1 && u <= HID_KEY_F12) return (uint16_t)(KEY_F1 + (u - HID_KEY_F1));
    return KEY_CHAR;
}

static void on_key(const struct usb_key_event *ev)
{
    if (!ev->pressed) return;                   /* Kestrel's queue carries presses */
    uint8_t mods = 0;
    if (ev->modifiers & HID_MOD_SHIFT) mods |= MOD_SHIFT;
    if (ev->modifiers & HID_MOD_CTRL)  mods |= MOD_CTRL;
    if (ev->modifiers & HID_MOD_ALT)   mods |= MOD_ALT;
    uint16_t key = usage_to_key(ev->usage);
    if (key == KEY_CHAR && !ev->ascii) return;  /* modifiers-only, unmapped keys */
    key_events++;
    keyboard_inject(key, ev->ascii, mods);
}

/* ---- mouse: absolute pointer from relative reports ------------------------ */
static const char *const arrow[12] = {
    "X.........", "XX........", "X#X.......", "X##X......", "X###X.....", "X####X....",
    "X#####X...", "X######X..", "X###XXXX..", "X#X#X.....", "XX.X#X....", "X...XX....",
};

static void draw_pointer(void)
{
    if (!g_fb.double_buffered) return;          /* nothing to restore from */
    if (ptr_x >= 0) fb_flush_rect(ptr_x, ptr_y, 10, 12);       /* erase old */
    ptr_x = mouse_x; ptr_y = mouse_y;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 10; x++) {
            char c = arrow[y][x];
            if (c == 'X') fb_front_pixel(ptr_x + x, ptr_y + y, COL_BLACK);
            else if (c == '#') fb_front_pixel(ptr_x + x, ptr_y + y, mouse_buttons ? COL_YELLOW : COL_WHITE);
        }
}

static void on_mouse(const struct usb_mouse_event *ev)
{
    mouse_reports++;
    mouse_x += ev->dx;
    mouse_y += ev->dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > (int)g_fb.width - 1)  mouse_x = (int)g_fb.width - 1;
    if (mouse_y > (int)g_fb.height - 1) mouse_y = (int)g_fb.height - 1;
    if (ev->buttons != mouse_buttons)
        kprintf("usb: mouse buttons %c%c%c at %d,%d\n", ev->buttons & 1 ? 'L' : '-',
                ev->buttons & 4 ? 'M' : '-', ev->buttons & 2 ? 'R' : '-', mouse_x, mouse_y);
    mouse_buttons = ev->buttons;
    draw_pointer();
}

/* HPET/TSC clock: valid before interrupts are enabled, unlike uptime_ms(),
 * so boot-time settling and transfer timeouts work in every phase. */
static uint64_t now_ms(void) { return time_ms(); }

static const struct xhci_platform kestrel_usb = {
    .dma_alloc = dma_alloc,
    .map_mmio  = map_mmio,
    .delay_us  = udelay,
    .now_ms    = now_ms,
    .log_putc  = klog_putc,
    .on_key    = on_key,
    .on_mouse  = on_mouse,
};

/* ---- bring-up ------------------------------------------------------------------ */
void usb_init(void)
{
    if (!pci_match_count(PCI_MATCH_XHCI)) { kprintf("usb: no xHCI controller found\n"); return; }
    struct pci_device *dev = pci_match_get(PCI_MATCH_XHCI, 0);
    const struct pci_bar *bar = &dev->bars[0];
    if ((bar->kind != PCI_BAR_MEM32 && bar->kind != PCI_BAR_MEM64) || !bar->base) {
        kprintf("usb: xHCI at %02x:%02x.%x has no memory BAR0\n", dev->bus, dev->device, dev->function);
        return;
    }
    kprintf("usb: xHCI %04x:%04x at %02x:%02x.%x, BAR0 %lx (%lu KiB)%s\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function,
            bar->base, bar->size >> 10,
            pci_match_count(PCI_MATCH_XHCI) > 1 ? ", other controllers ignored" : "");
    pci_enable(dev, false, true, true);
    pci_write32(dev->bus, dev->device, dev->function, PCI_REG_COMMAND,
                pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND) | PCI_CMD_INTX_DISABLE);

    /* Take every USB controller away from the firmware's SMM handler before
     * any driver touches one (EHCI/UHCI/OHCI legacy support, Intel port
     * routing). The xHCI's own USBLEGSUP handshake follows in xhci_init(). */
    static const struct usb_legacy_platform legacy = {
        .map_mmio = map_mmio_legacy, .delay_us = udelay, .log_putc = klog_putc,
    };
    struct usb_legacy_report rep;
    usb_legacy_handoff_all(&legacy, &rep);
    if (rep.ehci + rep.uhci + rep.ohci)
        kprintf("usb: firmware handoff: %d EHCI (%d BIOS-owned, %d forced), %d UHCI, %d OHCI (%d forced)\n",
                rep.ehci, rep.ehci_bios_owned, rep.ehci_forced, rep.uhci, rep.ohci, rep.ohci_forced);

    static const struct usb_msc_platform msc_plat = { .delay_us = udelay, .now_ms = now_ms, .log_putc = klog_putc };
    usb_msc_set_platform(&msc_plat);

    mouse_x = (int)g_fb.width / 2;
    mouse_y = (int)g_fb.height / 2;
    uint64_t t0 = time_ms();
    if (xhci_init(bar->base, &kestrel_usb) < 0) return;
    kprintf("usb: controller up and initial ports settled in %lu ms\n", time_ms() - t0);
    usb_up = true;
    pit_add_tick_hook(xhci_poll);               /* drain the event ring every 1 ms */
}

static int usbd_main(void *arg)
{
    (void)arg;
    for (;;) {
        xhci_service();                         /* hot-plug + endpoint recovery */
        usb_storage_scan(2000);                 /* new sticks -> /dev/usbN[pM] */
        task_sleep_ms(50);
    }
    return 0;
}

void usb_start_thread(void)
{
    if (usb_up) task_create("usbd", usbd_main, NULL);
}

void usb_redraw_pointer(void)
{
    if (!usb_up || ptr_x < 0) return;
    uint64_t f = irq_save();
    ptr_x = -1;                                 /* screen was redrawn: nothing to erase */
    draw_pointer();
    irq_restore(f);
}

size_t usb_summary(char *buf, size_t cap)
{
    if (!usb_up) return (size_t)snprintf(buf, cap, "no xHCI controller");
    int kbd = 0, mice = 0, n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        const struct usb_device_info *d = xhci_device(i);
        if (d->kind == USB_KIND_KEYBOARD) kbd++;
        if (d->kind == USB_KIND_MOUSE) mice++;
    }
    return (size_t)snprintf(buf, cap, "xHCI: %d device(s), %d keyboard(s), %d mouse/mice", n, kbd, mice);
}

size_t usb_proc(char *buf, size_t cap)
{
    size_t n = 0;
#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))
    if (!usb_up) { P("no xHCI controller\n"); return n; }
    P("SLOT PORT  ID         KIND      REPORTS  SPEED                  NAME\n");
    for (int i = 0; i < xhci_device_count(); i++) {
        const struct usb_device_info *d = xhci_device(i);
        static const char *kinds[] = { "other", "keyboard", "mouse", "hub", "storage" };
        P("%4u %4u  %04x:%04x  %-8s %8u  %-22s %s %s\n", d->slot, d->port, d->vendor, d->product,
          kinds[d->kind < 5 ? d->kind : 0], d->reports, xhci_speed_name(d->speed),
          d->manufacturer, d->product_name);
    }
    P("pointer %d,%d buttons %c%c%c, %u mouse reports, %u key presses\n", mouse_x, mouse_y,
      mouse_buttons & 1 ? 'L' : '-', mouse_buttons & 4 ? 'M' : '-', mouse_buttons & 2 ? 'R' : '-',
      mouse_reports, key_events);
#undef P
    return n;
}

/* ======================================================================== */
/*  USB mass storage -> block devices                                          */
/* ======================================================================== */
#define MAX_USB_DISKS   16
#define PROBE_ATTEMPTS  3

struct usb_disk {
    struct usb_msc_dev msc;
    struct blkdev     *blk;
};
static struct usb_disk *udisks[MAX_USB_DISKS];
static int nudisks;

static struct { uint8_t slot; uint32_t gen; uint8_t tries; uint64_t next; int last; } failed[MAX_USB_DISKS];

static int msc_errno(int rc)
{
    switch (rc) {
    case MSC_OK:           return 0;
    case MSC_ERR_NODEV:    return -ENODEV;
    case MSC_ERR_TIMEOUT:  return -ETIMEDOUT;
    case MSC_ERR_NOMEDIUM: return -ENOMEDIUM;
    case MSC_ERR_RANGE:    return -ERANGE;
    default:               return -EIO;
    }
}
static int ublk_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    struct usb_disk *u = b->ctx;
    return msc_errno(usb_msc_read(&u->msc, lba, count, buf));
}
static bool ublk_alive(struct blkdev *b) { return usb_msc_alive(&((struct usb_disk *)b->ctx)->msc); }
static const struct blkdev_ops ublk_ops = { .read = ublk_read, .alive = ublk_alive };

static bool known(uint8_t slot, uint32_t gen)
{
    for (int i = 0; i < nudisks; i++)
        if (udisks[i]->msc.slot == slot && udisks[i]->msc.generation == gen) return true;
    return false;
}

/* Has (slot, generation) failed to probe? Returns true if it should be
 * skipped for now; *give_up set once it is out of attempts. */
static bool probe_blocked(uint8_t slot, uint32_t gen, bool *give_up)
{
    *give_up = false;
    for (int i = 0; i < MAX_USB_DISKS; i++)
        if (failed[i].tries && failed[i].slot == slot && failed[i].gen == gen) {
            if (failed[i].tries >= PROBE_ATTEMPTS) { *give_up = true; return true; }
            return time_ms() < failed[i].next;
        }
    return false;
}

static void probe_failed(uint8_t slot, uint32_t gen, int rc)
{
    int idx = -1;
    for (int i = 0; i < MAX_USB_DISKS && idx < 0; i++)
        if (failed[i].tries && failed[i].slot == slot && failed[i].gen == gen) idx = i;
    for (int i = 0; i < MAX_USB_DISKS && idx < 0; i++) if (!failed[i].tries) idx = i;
    if (idx < 0) idx = 0;
    if (failed[idx].slot != slot || failed[idx].gen != gen) failed[idx].tries = 0;
    failed[idx].slot = slot; failed[idx].gen = gen; failed[idx].last = rc;
    failed[idx].tries++;
    failed[idx].next = time_ms() + 500u * failed[idx].tries;
    kprintf("usb: storage on slot %u: probe attempt %u/%d failed: %s%s\n", slot, failed[idx].tries,
            PROBE_ATTEMPTS, usb_msc_strerror(rc), failed[idx].tries >= PROBE_ATTEMPTS ? ", giving up" : "");
}

static int scan_locked(uint32_t ready_timeout_ms);
int usb_settle(void) { return usb_up ? xhci_settle_step() : 0; }
bool usb_present(void) { return usb_up; }

int usb_storage_pending(void)
{
    if (!usb_up) return 0;
    int pending = 0;
    struct xhci_msc_info info;
    for (int i = 0; xhci_msc_get(i, &info) == XHCI_OK; i++) {
        bool give_up;
        if (!known(info.slot, info.generation) && !(probe_blocked(info.slot, info.generation, &give_up) && give_up))
            pending++;
    }
    return pending;
}

static volatile bool scanning;

int usb_storage_scan(uint32_t ready_timeout_ms)
{
    if (!usb_up) return 0;
    uint64_t f = irq_save();                    /* usbd and the boot/recovery path */
    bool mine = !scanning;
    if (mine) scanning = true;
    irq_restore(f);
    if (!mine) return 0;                        /* another thread is probing right now */
    int added = scan_locked(ready_timeout_ms);
    scanning = false;
    return added;
}

static int scan_locked(uint32_t ready_timeout_ms)
{
    for (int i = 0; i < nudisks; i++) {                 /* removals */
        struct usb_disk *u = udisks[i];
        if (u->msc.present && !usb_msc_alive(&u->msc)) {
            kprintf("usb: storage %s (slot %u) disconnected\n", u->blk ? u->blk->name : "?", u->msc.slot);
            u->msc.present = false;
            if (u->blk) blk_mark_removed(u->blk);
        }
    }
    int added = 0;
    struct xhci_msc_info info;
    for (int i = 0; xhci_msc_get(i, &info) == XHCI_OK; i++) {
        bool give_up;
        if (known(info.slot, info.generation) || probe_blocked(info.slot, info.generation, &give_up)) continue;
        uint8_t max_lun = 0;
        for (uint8_t lun = 0; lun <= max_lun && nudisks < MAX_USB_DISKS; lun++) {
            struct usb_disk *u = kzalloc(sizeof *u);
            if (!u) return added;
            int rc = usb_msc_probe(&info, lun, &u->msc, ready_timeout_ms);
            if (lun == 0) max_lun = u->msc.max_lun;
            if (rc != MSC_OK) {
                if (lun == 0) probe_failed(info.slot, info.generation, rc);
                kfree(u);
                if (lun == 0) break;
                continue;
            }
            char model[48];
            snprintf(model, sizeof model, "%s %s", u->msc.vendor, u->msc.product);
            u->blk = blk_register_disk("usb", BLK_DISK_USB, model, u->msc.block_size, u->msc.blocks, &ublk_ops, u);
            if (!u->blk) { kfree(u); continue; }
            udisks[nudisks++] = u;
            kprintf("usb: slot %u port %u LUN %u -> /dev/%s\n", info.slot, info.port, lun, u->blk->name);
            blk_scan_partitions(u->blk);
            added++;
        }
    }
    return added;
}

void usb_dump_controller(void (*putc_fn)(char c))
{
    if (!usb_up) {
        const char *m = pci_match_count(PCI_MATCH_XHCI) ? "xHCI controller present but failed to initialise\n"
                                                        : "no xHCI controller on the PCI bus\n";
        while (*m) putc_fn(*m++);
    }
    xhci_dump_state(putc_fn);
}

size_t usb_storage_proc(char *buf, size_t cap)
{
    size_t n = 0;
#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))
    for (int i = 0; i < nudisks; i++) {
        const struct usb_msc_dev *m = &udisks[i]->msc;
        P("%-8s slot %u port %u lun %u  %04x:%04x \"%s %s\" %s  %lu x %u B  %s\n",
          udisks[i]->blk ? udisks[i]->blk->name : "-", m->slot, m->port, m->lun, m->vendor_id, m->product_id,
          m->vendor, m->product, m->revision, m->blocks, m->block_size, m->present ? "attached" : "GONE");
        P("         %u cmds, %u reads (%u blocks), %u retries, %u resets, %u timeouts, %u errors",
          m->commands, m->reads, m->blocks_read, m->retries, m->resets, m->timeouts, m->errors);
        if (m->errors || m->sense_key)
            P("; last: %s, sense %s %02x/%02x", usb_msc_strerror(m->last_error),
              scsi_sense_key_name(m->sense_key), m->asc, m->ascq);
        P("\n");
    }
    for (int i = 0; i < MAX_USB_DISKS; i++)
        if (failed[i].tries)
            P("slot %u (gen %u): probe failed %u time(s): %s\n", failed[i].slot, failed[i].gen,
              failed[i].tries, usb_msc_strerror(failed[i].last));
    if (!n) P("no USB mass storage devices\n");
#undef P
    return n;
}
