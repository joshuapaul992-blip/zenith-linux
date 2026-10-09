/* drivers/keyboard.c -- interrupt-driven PS/2 keyboard driver
 *
 * The 8042 controller translates the keyboard's native scancode set 2 to
 * set 1 (config bit 6), so this driver decodes set 1, including the 0xE0
 * prefix used by the arrow/navigation cluster.
 *
 * Until the TTYs exist (boot manager, early recovery) key presses go to one
 * ring buffer filled by the IRQ1 handler. Afterwards every key goes through
 * deliver(): Ctrl+Alt+Fn switches the keyboard focus between monitors right
 * here in the ISR, and any other key is queued on the focused TTY only; the
 * readers below then read the queue of their own task's TTY. */
#include <kernel/keyboard.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/klog.h>
#include <kernel/tty.h>
#include <kernel/kinput.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

#define QSIZE 128
static struct key_event queue[QSIZE];
static volatile uint32_t q_head, q_tail;
static uint8_t mods;
static bool e0_prefix;
int keyboard_waitq;

static const char map_normal[0x59] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,   '\\','z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,
};
static const char map_shift[0x59] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,
};

/* ISR context (IRQ1, or the timer hook for USB). */
static void deliver(uint16_t key, char ascii, uint8_t m)
{
    if (tty_hotkey(key, m)) return;             /* Ctrl+Alt+Fn: focus switch, consumed */
    struct key_event ev = { key, ascii, m };
    if (tty_route_key(&ev)) return;             /* -> the focused TTY's queue */
    uint32_t next = (q_head + 1) % QSIZE;
    if (next == q_tail) return;                 /* full: drop */
    queue[q_head] = ev;
    q_head = next;
    wakeup(&keyboard_waitq);
}

static void enqueue(uint16_t key, char ascii) { deliver(key, ascii, mods); }

static uint16_t nav_key(uint8_t sc)
{
    switch (sc) {
    case 0x48: return KEY_UP;     case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;   case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;   case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;   case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT; case 0x53: return KEY_DELETE;
    default:   return KEY_NONE;
    }
}

static void reboot_now(void)
{
    kprintf("keyboard: Ctrl+Alt+Del -> reset\n");
    while (inb(KBD_STATUS) & 0x02) ;
    outb(KBD_CMD, 0xFE);                        /* pulse CPU reset line */
}

static void kbd_irq(struct int_frame *f)
{
    (void)f;
    if (!(inb(KBD_STATUS) & 0x01)) return;
    uint8_t sc = inb(KBD_DATA);

    /* A display server holds /dev/kinput: raw scancodes go there instead. */
    if (kinput_grabbed()) { mods = 0; e0_prefix = false; kinput_ps2_byte(sc); return; }

    if (sc == 0xE0) { e0_prefix = true; return; }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    bool ext = e0_prefix;
    e0_prefix = false;

    /* modifiers */
    switch (code) {
    case 0x2A: case 0x36: if (!ext) { mods = release ? mods & ~MOD_SHIFT : mods | MOD_SHIFT; return; } break;
    case 0x1D: mods = release ? mods & ~MOD_CTRL : mods | MOD_CTRL; return;
    case 0x38: mods = release ? mods & ~MOD_ALT  : mods | MOD_ALT;  return;
    case 0x3A: if (!release) mods ^= MOD_CAPS; return;
    }
    if (release) return;

    /* navigation cluster: E0-prefixed keys, and keypad keys with NumLock off */
    uint16_t nav = nav_key(code);
    if (nav != KEY_NONE && (ext || !(mods & MOD_SHIFT))) {
        if (nav == KEY_DELETE && (mods & MOD_CTRL) && (mods & MOD_ALT)) reboot_now();
        enqueue(nav, 0);
        return;
    }
    if (code >= 0x3B && code <= 0x44) { enqueue(KEY_F1 + (code - 0x3B), 0); return; }
    if (code == 0x57) { enqueue(KEY_F11, 0); return; }
    if (code == 0x58) { enqueue(KEY_F12, 0); return; }

    switch (code) {
    case 0x01: enqueue(KEY_ESC, 27); return;
    case 0x0E: enqueue(KEY_BACKSPACE, '\b'); return;
    case 0x0F: enqueue(KEY_TAB, '\t'); return;
    case 0x1C: enqueue(KEY_ENTER, '\n'); return;    /* also keypad Enter (E0 1C) */
    }
    if (ext && code == 0x35) { enqueue(KEY_CHAR, '/'); return; }

    if (code < sizeof map_normal) {
        bool shift = (mods & MOD_SHIFT) != 0;
        char c = shift ? map_shift[code] : map_normal[code];
        if ((mods & MOD_CAPS) && c >= 'a' && c <= 'z') c -= 32;
        else if ((mods & MOD_CAPS) && c >= 'A' && c <= 'Z') c += 32;
        if (c && (mods & MOD_CTRL) && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
            c = (char)((c | 0x20) - 'a' + 1);          /* Ctrl+letter -> 0x01..0x1A */
        if (c) enqueue(KEY_CHAR, c);
    }
}

static void wait_write(void) { for (int i = 0; i < 100000 && (inb(KBD_STATUS) & 0x02); i++) cpu_relax(); }
static bool wait_read(void)  { for (int i = 0; i < 100000; i++) { if (inb(KBD_STATUS) & 0x01) return true; cpu_relax(); } return false; }

void keyboard_init(void)
{
    /* drain anything the firmware left behind */
    for (int i = 0; i < 32 && (inb(KBD_STATUS) & 0x01); i++) (void)inb(KBD_DATA);

    /* enable IRQ1 + scancode translation in the controller config byte */
    wait_write(); outb(KBD_CMD, 0x20);
    uint8_t cfg = wait_read() ? inb(KBD_DATA) : 0x45;
    cfg |= 0x01 | 0x40;        /* port-1 IRQ, translation */
    cfg &= ~0x10;              /* port-1 clock enabled    */
    wait_write(); outb(KBD_CMD, 0x60);
    wait_write(); outb(KBD_DATA, cfg);
    wait_write(); outb(KBD_CMD, 0xAE);        /* enable first PS/2 port */

    irq_register(IRQ_KEYBOARD, kbd_irq);
    pic_unmask(IRQ_KEYBOARD);
    kprintf("keyboard: PS/2 controller config=%02x, IRQ1 enabled\n", cfg);
}

/* Once input goes to the TTYs, a reader gets the keys of its own task's TTY. */
static struct kestrel_tty *reader_tty(void) { return tty_input_active() ? tty_current() : NULL; }

bool keyboard_has_input(void)
{
    struct kestrel_tty *t = reader_tty();
    return t ? tty_has_key(t) : q_head != q_tail;
}

bool keyboard_poll(struct key_event *ev)
{
    struct kestrel_tty *t = reader_tty();
    if (t) return tty_read_key(t, ev, false);
    uint64_t f = irq_save();
    bool ok = q_head != q_tail;
    if (ok) { *ev = queue[q_tail]; q_tail = (q_tail + 1) % QSIZE; }
    irq_restore(f);
    return ok;
}

void keyboard_wait(struct key_event *ev)
{
    struct kestrel_tty *t = reader_tty();
    if (t) { tty_read_key(t, ev, true); return; }
    for (;;) {
        uint64_t f = irq_save();
        if (q_head != q_tail) {
            *ev = queue[q_tail]; q_tail = (q_tail + 1) % QSIZE;
            irq_restore(f);
            return;
        }
        sleep_on(&keyboard_waitq);      /* returns with IRQs disabled */
        irq_restore(f);
    }
}

void keyboard_inject(uint16_t key, char ascii, uint8_t m)
{
    uint64_t f = irq_save();
    deliver(key, ascii, m);
    irq_restore(f);
}

void keyboard_flush(void)
{
    struct kestrel_tty *t = reader_tty();
    if (t) { tty_flush_keys(t); return; }
    uint64_t f = irq_save(); q_tail = q_head; irq_restore(f);
}

const char *keyboard_key_name(uint16_t key)
{
    static const char *names[] = { "ENTER", "TAB", "ESC", "BACKSPACE", "UP", "DOWN", "LEFT", "RIGHT",
        "HOME", "END", "PGUP", "PGDN", "INSERT", "DELETE", "F1", "F2", "F3", "F4", "F5", "F6",
        "F7", "F8", "F9", "F10", "F11", "F12" };
    if (key >= KEY_ENTER && key <= KEY_F12) return names[key - KEY_ENTER];
    return "CHAR";
}
