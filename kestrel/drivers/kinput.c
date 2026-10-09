/* =============================================================================
 *  kinput.c -- /dev/kinput: raw keyboard/mouse events for a display server
 *
 *  See include/kernel/kinput.h. The producers run in interrupt context (IRQ1
 *  for PS/2, the timer-driven xHCI poll for USB); the queue is a ring of
 *  fixed-size records protected by disabling interrupts on the reader side.
 * ============================================================================= */
#include <kernel/kinput.h>
#include <kernel/vfs.h>
#include <kernel/posix.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/klog.h>
#include <kernel/uvm.h>

#define QLEN 1024                       /* events; ~170 mouse reports of slack */

static struct kinput_event q[QLEN];
static volatile uint32_t q_head, q_tail;
static volatile bool grabbed;
static struct file *grab_file;          /* the open file holding the grab */
static uint32_t dropped;
static uint8_t  mouse_buttons;
static bool     ps2_e0;

bool kinput_grabbed(void) { return grabbed; }

static void push(uint16_t type, uint16_t code, int32_t value)
{
    uint32_t next = (q_head + 1) % QLEN;
    if (next == q_tail) { dropped++; return; }
    q[q_head] = (struct kinput_event){ time_us(), type, code, value };
    q_head = next;
}

static void commit(void) { wakeup(&q); }

/* ---- PS/2 scancode set 1 ---------------------------------------------------
 * Linux key codes 1..88 are the set-1 make codes themselves; the E0-prefixed
 * keys need a table. */
static uint16_t ps2_e0_code(uint8_t c)
{
    switch (c) {
    case 0x1C: return 96;   /* KPENTER   */   case 0x1D: return 97;   /* RIGHTCTRL */
    case 0x35: return 98;   /* KPSLASH   */   case 0x37: return 99;   /* SYSRQ     */
    case 0x38: return 100;  /* RIGHTALT  */   case 0x47: return 102;  /* HOME      */
    case 0x48: return 103;  /* UP        */   case 0x49: return 104;  /* PAGEUP    */
    case 0x4B: return 105;  /* LEFT      */   case 0x4D: return 106;  /* RIGHT     */
    case 0x4F: return 107;  /* END       */   case 0x50: return 108;  /* DOWN      */
    case 0x51: return 109;  /* PAGEDOWN  */   case 0x52: return 110;  /* INSERT    */
    case 0x53: return 111;  /* DELETE    */   case 0x5B: return 125;  /* LEFTMETA  */
    case 0x5C: return 126;  /* RIGHTMETA */   case 0x5D: return 127;  /* COMPOSE   */
    default:   return 0;    /* includes the fake shifts E0 2A / E0 36 */
    }
}

void kinput_ps2_byte(uint8_t sc)
{
    if (sc == 0xE0) { ps2_e0 = true; return; }
    if (sc == 0xE1 || sc == 0xFA || sc == 0xFE) return;     /* Pause prefix, ACK, resend */
    bool release = sc & 0x80;
    uint8_t c = sc & 0x7F;
    uint16_t code = ps2_e0 ? ps2_e0_code(c) : (c <= 88 ? c : 0);
    ps2_e0 = false;
    if (!code) return;
    /* The 8042 repeats makes while a key is held; X does its own repeat. */
    push(KINPUT_EV_KEY, code, release ? 0 : 1);
    push(KINPUT_EV_SYN, 0, 0);
    commit();
}

/* ---- USB HID keyboard usage page 7 -> Linux key codes (usbkbd.c table) ---- */
static const uint8_t hid_to_linux[0xE8] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,
};

void kinput_usb_key(uint8_t usage, bool pressed, bool repeat)
{
    if (usage >= sizeof hid_to_linux || !hid_to_linux[usage]) return;
    push(KINPUT_EV_KEY, hid_to_linux[usage], !pressed ? 0 : repeat ? 2 : 1);
    push(KINPUT_EV_SYN, 0, 0);
    commit();
}

void kinput_usb_mouse(int dx, int dy, int wheel, uint8_t buttons)
{
    static const uint16_t btn[3] = { KINPUT_BTN_LEFT, KINPUT_BTN_RIGHT, KINPUT_BTN_MIDDLE };
    if (dx) push(KINPUT_EV_REL, KINPUT_REL_X, dx);
    if (dy) push(KINPUT_EV_REL, KINPUT_REL_Y, dy);
    if (wheel) push(KINPUT_EV_REL, KINPUT_REL_WHEEL, wheel);
    uint8_t changed = (uint8_t)(buttons ^ mouse_buttons);
    for (int i = 0; i < 3; i++)
        if (changed & (1u << i)) push(KINPUT_EV_KEY, btn[i], (buttons >> i) & 1);
    mouse_buttons = buttons;
    push(KINPUT_EV_SYN, 0, 0);
    commit();
}

/* ---- the device ------------------------------------------------------------ */
static ssize_t kinput_fread(struct file *f, void *buf, size_t len)
{
    if (len < sizeof(struct kinput_event)) return -EINVAL;
    size_t max = len / sizeof(struct kinput_event), n = 0;
    struct kinput_event *out = buf;
    uint64_t fl = irq_save();
    while (q_head == q_tail) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        sleep_on(&q);
    }
    while (n < max && q_tail != q_head) {
        out[n++] = q[q_tail];
        q_tail = (q_tail + 1) % QLEN;
    }
    irq_restore(fl);
    return (ssize_t)(n * sizeof(struct kinput_event));
}

static int kinput_poll(struct file *f, int events)
{
    (void)f;
    return (q_head != q_tail) ? (events & POLLIN) : 0;
}

static void set_grab(struct file *f, bool on)
{
    uint64_t fl = irq_save();
    if (on) {
        q_head = q_tail = 0;
        mouse_buttons = 0;
        ps2_e0 = false;
        grab_file = f;
        grabbed = true;
    } else {
        grabbed = false;
        grab_file = NULL;
    }
    irq_restore(fl);
    kprintf("kinput: %s by pid %d\n", on ? "grabbed" : "released", current_task()->pid);
    if (dropped) { kprintf("kinput: %u event(s) dropped (queue full)\n", dropped); dropped = 0; }
}

static bool bad_user_ptr(void *p, size_t len)
{
    struct tcb *t = current_task();
    if ((uint64_t)p < 4096) return true;
    return t->user && !uvm_mapped(t->pml4, (uint64_t)p, len);
}

static int kinput_ioctl(struct file *f, unsigned long req, void *arg)
{
    if (req != KINPUT_GRAB) return -ENOTTY;
    if (bad_user_ptr(arg, sizeof(int))) return -EFAULT;
    int on = *(int *)arg;
    if (on) {
        if (grabbed && grab_file != f) return -EBUSY;
        set_grab(f, true);
    } else if (grabbed && grab_file == f) {
        set_grab(NULL, false);
    }
    return 0;
}

static void kinput_release(struct file *f)
{
    if (grabbed && grab_file == f) set_grab(NULL, false);
}

static const struct vnode_ops kinput_ops = {
    .fread = kinput_fread, .poll = kinput_poll, .fioctl = kinput_ioctl, .release = kinput_release,
};

void kinput_init(void)
{
    devfs_register("kinput", 0660, (13u << 8) | 64u, &kinput_ops, NULL);
}
