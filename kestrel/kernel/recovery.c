/* kernel/recovery.c -- boot failure diagnostics and the recovery shell */
#include <kernel/recovery.h>
#include <kernel/bootvol.h>
#include <kernel/block.h>
#include <kernel/usbhost.h>
#include <kernel/term.h>
#include <kernel/console.h>
#include <kernel/serial.h>
#include <kernel/keyboard.h>
#include <kernel/klog.h>
#include <kernel/time.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/posix.h>

#define LINE_MAX    160
#define C_TITLE     "\033[30;47m"
#define C_HEAD      "\033[97m"
#define C_ERR       "\033[91m"
#define C_WARN      "\033[93m"
#define C_DIM       "\033[90m"
#define C_OK        "\033[92m"
#define C_RESET     "\033[0m"

static const char *g_reason, *g_detail;

/* ---- output: the terminal (or the plain console) and COM1 ------------------ */
static void rputc(char c)
{
    if (term_active()) term_putc(c);
    else con_putc(&g_con, c);
    serial_putc(c);
}
static void rputs(const char *s) { while (*s) rputc(*s++); }

__attribute__((format(printf, 1, 2)))
static void rprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rputs(buf);
}

/* Large generators (/proc-style text) printed line by line */
static void rbig(size_t (*gen)(char *, size_t), const char *indent)
{
    size_t cap = 8192;
    char *buf = kmalloc(cap);
    if (!buf) return;
    size_t n = gen(buf, cap);
    if (n >= cap) n = cap - 1;
    buf[n] = 0;
    bool bol = true;
    for (char *p = buf; *p; p++) {
        if (bol) rputs(indent);
        rputc(*p);
        bol = *p == '\n';
    }
    if (!bol) rputc('\n');
    kfree(buf);
}

static void indent_putc(char c)
{
    static bool bol = true;
    if (bol) rputs("  ");
    rputc(c);
    bol = c == '\n';
}

/* ---- kernel log tail ---------------------------------------------------- */
static void log_tail(int lines)
{
    size_t size = klog_size(), want = 6144;
    size_t off = size > want ? size - want : 0;
    char *buf = kmalloc(want + 1);
    if (!buf) return;
    size_t n = klog_read(off, buf, want);
    buf[n] = 0;
    int seen = 0;
    char *start = buf + n;
    while (start > buf) {
        if (start[-1] == '\n' && start != buf + n && ++seen >= lines) break;
        start--;
    }
    rputs(C_DIM);
    bool bol = true;
    for (char *p = start; *p; p++) {
        if (bol) rputs("  ");
        if (*p == '\033') {                 /* strip colour escapes from logged text */
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        rputc(*p);
        bol = *p == '\n';
    }
    rputs(C_RESET);
    kfree(buf);
}

/* ---- the diagnostic screen ------------------------------------------------ */
static void title_bar(const char *text)
{
    char line[TERM_COLS + 1];
    int n = snprintf(line, sizeof line, "  %s", text);
    while (n < TERM_COLS - 1) line[n++] = ' ';
    line[n] = 0;
    rprintf(C_TITLE "%s" C_RESET "\n", line);
}

static void diagnostics(void)
{
    if (term_active()) term_clear();
    title_bar("KESTREL RECOVERY CONSOLE  -  boot pipeline diagnostic trace");
    rprintf("\n " C_ERR "%s" C_RESET "\n", g_reason ? g_reason : "Recovery requested");
    if (g_detail && *g_detail) rprintf("   %s\n", g_detail);
    const struct bootvol_status *s = bootvol_status();
    rprintf(C_DIM "   kernel %s %s, cmdline \"%s\"\n   policy kestrel.root=%s, waited %lu ms (budget %u ms), "
            "attempt %u\n   clock %s, uptime %lu ms" C_RESET "\n",
            KESTREL_NAME, KESTREL_VERSION, g_boot.cmdline, s->policy, s->elapsed_ms, s->rootwait_ms,
            s->attempts, time_source(), time_ms());

    rputs("\n" C_HEAD " USB host controller" C_RESET "\n");
    usb_dump_controller(indent_putc);
    rputs("\n" C_HEAD " USB mass storage" C_RESET "\n");
    rbig(usb_storage_proc, "  ");
    rputs("\n" C_HEAD " Block devices" C_RESET "\n");
    rbig(blk_proc, "  ");
    rputs("\n" C_HEAD " Kernel log (most recent)" C_RESET "\n");
    log_tail(6);
    rputs("\n " C_WARN "Type 'help' for commands. Input is accepted from the keyboard and from COM1 "
          "(115200 8N1)." C_RESET "\n");
}

/* ---- line input from keyboard + serial ------------------------------------ */
static int next_char(void)
{
    static int esc;                     /* swallow serial escape sequences (arrow keys) */
    struct key_event ev;
    if (keyboard_poll(&ev)) {
        if (ev.key == KEY_ENTER) return '\n';
        if (ev.key == KEY_BACKSPACE) return '\b';
        if (ev.key == KEY_CHAR && ev.ascii) {
            if ((ev.mods & MOD_CTRL) && (ev.ascii == 'c' || ev.ascii == 'C')) return 3;
            return (unsigned char)ev.ascii;
        }
        return -1;
    }
    int c = serial_getc();
    if (c < 0) return -1;
    if (esc == 1) { esc = c == '[' ? 2 : 0; return -1; }
    if (esc == 2) { if (c >= 0x40 && c <= 0x7E) esc = 0; return -1; }
    if (c == 0x1B) { esc = 1; return -1; }
    if (c == '\r') return '\n';
    if (c == '\n') return -1;           /* CR LF from terminal programs */
    if (c == 0x7F) return '\b';
    return c;
}

static void read_line(char *buf, size_t cap)
{
    size_t n = 0;
    term_cursor_enable(true);
    for (;;) {
        int c = next_char();
        if (c < 0) {
            if (irqs_enabled()) hlt(); else cpu_relax();
            continue;
        }
        if (c == '\n') { rputc('\n'); break; }
        if (c == 3) { rputs("^C\n"); n = 0; break; }
        if (c == '\b') {
            if (!n) continue;
            n--;
            if (term_active()) term_backspace();
            serial_putc('\b'); serial_putc(' '); serial_putc('\b');
            continue;
        }
        if (c < 0x20 || c > 0x7E || n + 1 >= cap) continue;
        buf[n++] = (char)c;
        rputc((char)c);
    }
    term_cursor_enable(false);
    buf[n] = 0;
}

/* ---- commands ------------------------------------------------------------------ */
static void hexdump(const uint8_t *p, size_t len, uint64_t base)
{
    for (size_t off = 0; off < len; off += 16) {
        rprintf("  %08lx  ", base + off);
        for (size_t i = 0; i < 16; i++) rprintf(i == 8 ? " %02x " : "%02x ", p[off + i]);
        rputs(" |");
        for (size_t i = 0; i < 16; i++) rputc(p[off + i] >= 0x20 && p[off + i] < 0x7F ? (char)p[off + i] : '.');
        rputs("|\n");
    }
}

static void cmd_read(int argc, char **argv)
{
    if (argc < 2) { rputs("usage: read <device> [block]\n"); return; }
    struct blkdev *b = blk_find(argv[1]);
    if (!b) { rprintf(C_ERR "no block device '%s'" C_RESET " (see 'devs')\n", argv[1]); return; }
    uint64_t lba = argc > 2 ? (uint64_t)strtol(argv[2], NULL, 0) : 0;
    uint8_t *buf = kmalloc(b->block_size);
    if (!buf) return;
    uint64_t t0 = time_ms();
    int rc = blk_read(b, lba, 1, buf);
    uint64_t ms = time_ms() - t0;
    if (rc < 0) rprintf(C_ERR "read of %s block %lu failed after %lu ms: %s" C_RESET "\n", b->name, lba, ms, blk_strerror(rc));
    else {
        rprintf("%s block %lu (%u bytes, %lu ms), first 512 bytes:\n", b->name, lba, b->block_size, ms);
        hexdump(buf, 512, lba * b->block_size);
    }
    kfree(buf);
}

static void cmd_scan(void)
{
    rputs("settling USB ports and probing mass storage for up to 3 s...\n");
    uint64_t end = time_ms() + 3000;
    int added = 0;
    for (;;) {
        int busy = usb_settle();
        added += usb_storage_scan(2000);
        if ((busy == 0 && !usb_storage_pending()) || time_ms() >= end) break;
        mdelay(20);
    }
    rprintf("%d new disk(s)\n", added);
    rbig(blk_proc, "  ");
}

static void reboot(void)
{
    rputs("restarting...\n");
    cli();
    outb(0xCF9, 0x02);                  /* PCI reset control: system reset */
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 1000000; i++) ;
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);                   /* 8042 reset line */
    for (volatile int i = 0; i < 1000000; i++) ;
    rputs("reset did not happen; halting. Power-cycle the machine.\n");
    cpu_halt_forever();
}

static void poweroff(void)
{
    rputs("powering off...\n");
    cli();
    outw(0x604, 0x2000);                /* QEMU q35/pc ACPI PM1a */
    outw(0xB004, 0x2000);               /* Bochs, old QEMU */
    outw(0x4004, 0x3400);               /* VirtualBox */
    rputs("no software power-off here; system halted. It is safe to turn it off.\n");
    cpu_halt_forever();
}

static void help(void)
{
    rputs(C_HEAD "Recovery commands" C_RESET "\n"
          "  help              this list\n"
          "  diag              redraw the full diagnostic trace\n"
          "  usb               xHCI registers, port status (PORTSC) and slots\n"
          "  devs              USB mass storage statistics and block devices\n"
          "  log [lines]       tail of the kernel log (default 30)\n"
          "  read <dev> [blk]  hex dump of one block, e.g. 'read usb0 0'\n"
          "  scan              settle USB ports again and probe new storage\n"
          "  boot <dev>        try one device as the boot volume (e.g. 'boot usb0p1')\n"
          "  retry             run the whole boot volume discovery again\n"
          "  continue          boot on without a boot volume\n"
          "  reboot            restart the machine\n"
          "  poweroff          power off (emulators) or halt\n");
}

enum recovery_action recovery_enter(const char *reason, const char *detail)
{
    g_reason = reason;
    g_detail = detail;
    kprintf("recovery: entered: %s: %s\n", reason, detail ? detail : "");
    bool was_console = klog_console_enabled();
    klog_set_console(false);            /* the trace is printed explicitly, no duplicates */
    keyboard_flush();
    diagnostics();

    char line[LINE_MAX];
    for (;;) {
        rputs(C_ERR "recovery" C_RESET "> ");
        read_line(line, sizeof line);
        char *argv[8], *save = NULL;
        int argc = 0;
        for (char *t = strtok_r(line, " \t", &save); t && argc < 8; t = strtok_r(NULL, " \t", &save)) argv[argc++] = t;
        if (!argc) continue;
        kprintf("recovery: command '%s'\n", argv[0]);
        klog_set_console(true);         /* driver messages during commands are useful here */
        if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) help();
        else if (!strcmp(argv[0], "diag")) { klog_set_console(false); diagnostics(); }
        else if (!strcmp(argv[0], "usb") || !strcmp(argv[0], "ports") || !strcmp(argv[0], "regs")) usb_dump_controller(indent_putc);
        else if (!strcmp(argv[0], "devs")) { rbig(usb_storage_proc, "  "); rbig(blk_proc, "  "); }
        else if (!strcmp(argv[0], "log")) { klog_set_console(false); log_tail(argc > 1 ? (int)strtol(argv[1], NULL, 10) : 30); }
        else if (!strcmp(argv[0], "read")) cmd_read(argc, argv);
        else if (!strcmp(argv[0], "scan")) cmd_scan();
        else if (!strcmp(argv[0], "boot")) {
            if (argc < 2) rputs("usage: boot <device>\n");
            else {
                int rc = bootvol_try_device(argv[1]);
                if (rc == 0) { rputs(C_OK "boot volume mounted, continuing boot" C_RESET "\n"); klog_set_console(was_console); return RECOVERY_RETRY; }
                rprintf(C_ERR "%s: %s" C_RESET "\n", argv[1], bootvol_status()->error[0] ? bootvol_status()->error : blk_strerror(rc));
            }
        }
        else if (!strcmp(argv[0], "retry")) { klog_set_console(was_console); return RECOVERY_RETRY; }
        else if (!strcmp(argv[0], "continue")) { klog_set_console(was_console); return RECOVERY_CONTINUE; }
        else if (!strcmp(argv[0], "reboot")) reboot();
        else if (!strcmp(argv[0], "poweroff")) poweroff();
        else rprintf("unknown command '%s' - type 'help'\n", argv[0]);
        klog_set_console(false);
    }
}
