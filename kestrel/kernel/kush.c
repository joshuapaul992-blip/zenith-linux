/* kernel/kush.c -- 'kush', the Kestrel command interpreter (KUSH: Kestrel User SHell)
 *
 * One kush instance runs per monitor, each in its own thread bound to that
 * monitor's TTY. All shell state (line editor, history, $?, parse buffers)
 * lives in a struct kush_session, so instances share nothing.
 *
 *   input     keyboard IRQ (PS/2 scan code or USB HID report) -> ASCII ->
 *             the focused TTY's key queue -> wakes this TTY's shell thread
 *             (tty_read_key) -> line editor below
 *   editor    the TTY's 1024-byte input_buffer; printable keys are appended
 *             at buffer_index and echoed at the TTY's cursor; Backspace steps
 *             back and blanks the cell; Enter ends the line. Also: Up/Down
 *             history, Tab completion of command names, Ctrl+C, Ctrl+U, Ctrl+L.
 *   REPL      prompt "kestrel:/# ", read a line, split into words, run the
 *             matching built-in, repeat.
 */
#include <kernel/exec.h>
#include <kernel/kush.h>
#include <kernel/recovery.h>
#include <kernel/tty.h>
#include <kernel/klog.h>
#include <kernel/report.h>
#include <kernel/gpu.h>
#include <kernel/block.h>
#include <kernel/keyboard.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/fb.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/multiboot2.h>
#include <kernel/storage.h>
#include <kernel/usbhost.h>
#include <kernel/usyscall.h>
#include <kernel/kestrel_font.h>
#include "pci.h"
#include "coreutils/cu.h"

#define KUSH_LINE_MAX   1024
#define KUSH_MAX_ARGS   16
#define KUSH_HISTORY    16

_Static_assert(KUSH_LINE_MAX == TTY_INPUT_MAX, "the line editor works in the TTY's input_buffer");

struct kush_session {
    struct kestrel_tty *tty;                    /* the monitor this shell owns   */
    char   history[KUSH_HISTORY][KUSH_LINE_MAX];
    int    hist_count, hist_next;               /* ring of past commands         */
    int    last_status;                         /* $?                            */
    char   cmdline[KUSH_LINE_MAX];              /* line handed to the parser     */
    char   store[KUSH_LINE_MAX * 2];            /* parsed words                  */
};

/* The line being edited: this TTY's own input buffer. */
#define LINE     (sh->tty->input_buffer)
#define LINE_LEN (sh->tty->buffer_index)

/* ======================================================================== */
/*  output helpers                                                             */
/* ======================================================================== */
#define C_RESET  "\033[0m"
#define C_TITLE  "\033[97m"
#define C_LABEL  "\033[37m"
#define C_DIM    "\033[90m"
#define C_OK     "\033[92m"
#define C_ERR    "\033[91m"
#define C_PATH   "\033[94m"

static void kush_prompt(struct kush_session *sh)
{
    char cwd[256];
    if (u_getcwd(cwd, sizeof cwd) < 0) strlcpy(cwd, "?", sizeof cwd);
    tty_printf(sh->tty, C_OK "kestrel" C_RESET ":" C_PATH "%s" C_RESET "# ", cwd);
}

/* Print a file (procfs/sysfs or device) through the kernel VFS API. */
static void show_file(struct kush_session *sh, const char *path)
{
    struct file *f;
    if (vfs_open(path, O_RDONLY, 0, &f) < 0) { tty_printf(sh->tty, C_ERR "kush: cannot open %s\n" C_RESET, path); return; }
    char *buf = kmalloc(1024);
    ssize_t n;
    while (buf && (n = vfs_read(f, buf, 1024)) > 0) tty_write(sh->tty, buf, (size_t)n);
    kfree(buf);
    vfs_close(f);
}

static void fmt_size(char *out, size_t cap, uint64_t bytes)
{
    if (bytes >= (10ull << 30))      snprintf(out, cap, "%lu GiB", bytes >> 30);
    else if (bytes >= (10ull << 20)) snprintf(out, cap, "%lu MiB", bytes >> 20);
    else                             snprintf(out, cap, "%lu KiB", bytes >> 10);
}

/* ======================================================================== */
/*  built-in commands                                                          */
/* ======================================================================== */
typedef int (*kush_fn)(struct kush_session *sh, int argc, char **argv);
struct kush_cmd { const char *name, *args, *help; kush_fn fn; };
static const struct kush_cmd commands[];

static int cmd_help(struct kush_session *sh, int argc, char **argv);

static int cmd_clear(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_clear(sh->tty);                               /* black screen, cursor to 0,0 */
    return 0;
}

static void cpu_features(char *out, size_t cap)
{
    uint32_t a, b, c, d, b7 = 0, c7 = 0, d7 = 0, max;
    cpuid(0, 0, &max, &b, &c, &d);
    cpuid(1, 0, &a, &b, &c, &d);
    if (max >= 7) cpuid(7, 0, &a, &b7, &c7, &d7);
    struct { uint32_t reg; int bit; const char *name; } f[] = {
        { d, 25, "SSE" }, { d, 26, "SSE2" }, { c, 0, "SSE3" }, { c, 9, "SSSE3" },
        { c, 19, "SSE4.1" }, { c, 20, "SSE4.2" }, { c, 23, "POPCNT" }, { c, 25, "AES" },
        { c, 28, "AVX" }, { b7, 5, "AVX2" }, { b7, 16, "AVX-512F" }, { c, 12, "FMA" },
        { c, 30, "RDRAND" }, { d, 9, "APIC" }, { c, 21, "x2APIC" }, { c, 24, "TSC-deadline" },
        { c, 31, "hypervisor" },
    };
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof f / sizeof *f; i++)
        if (f[i].reg & (1u << f[i].bit))
            n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? " " : "", f[i].name);
}

static int cmd_sysinfo(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    char a[160], b[32], c[32];
    uint64_t ms = uptime_ms();

    tty_printf(sh->tty, C_TITLE "%s %s (%s)" C_RESET "  built " __DATE__ " " __TIME__ " with gcc " __VERSION__ "\n",
                KESTREL_NAME, KESTREL_VERSION, KESTREL_MACHINE);
    tty_printf(sh->tty, C_LABEL "  Uptime     " C_RESET "%lum %lu.%lus\n", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100);
    tty_printf(sh->tty, C_LABEL "  Boot       " C_RESET "%s firmware, %s, entry \"%s\"\n",
                g_boot.uefi ? "UEFI" : "BIOS", g_boot.loader, g_boot.boot_entry_name);

    /* CPU: vendor, brand string, family/model/stepping, features */
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t family = (eax >> 8) & 0xF, model = (eax >> 4) & 0xF, stepping = eax & 0xF;
    if (family == 0xF) family += (eax >> 20) & 0xFF;
    if (family >= 6) model |= ((eax >> 16) & 0xF) << 4;
    tty_printf(sh->tty, C_LABEL "  CPU        " C_RESET "%s\n", g_boot.cpu_brand[0] ? g_boot.cpu_brand : "(no brand string)");
    tty_printf(sh->tty, C_LABEL "             " C_RESET "vendor %s, family %u, model %u, stepping %u, %u logical CPU(s) per package\n",
                g_boot.cpu_vendor, family, model, stepping, (ebx >> 16) & 0xFF ? (ebx >> 16) & 0xFF : 1);
    cpu_features(a, sizeof a);
    tty_printf(sh->tty, C_LABEL "             " C_RESET "%s\n", a);

    /* Memory: from the PMM and the Multiboot2 memory map */
    int regions = 0;
    uint64_t top = 0;
    for (struct mb2_tag *t = mb2_first_tag(g_boot.mbi); t->type != MB2_TAG_END; t = mb2_next_tag(t)) {
        if (t->type != MB2_TAG_MMAP) continue;
        struct mb2_tag_mmap *mm = (struct mb2_tag_mmap *)t;
        for (uint8_t *p = (uint8_t *)mm->entries; p < (uint8_t *)t + t->size; p += mm->entry_size) {
            struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
            if (e->type == 1) { regions++; if (e->addr + e->len > top) top = e->addr + e->len; }
        }
    }
    fmt_size(b, sizeof b, pmm_total_bytes());
    fmt_size(c, sizeof c, pmm_free_bytes());
    tty_printf(sh->tty, C_LABEL "  Memory     " C_RESET "%s usable in %d region(s), %s free, top of RAM 0x%lx\n", b, regions, c, top);
    tty_printf(sh->tty, C_LABEL "             " C_RESET "kernel image %lu KiB, heap %zu KiB (%zu KiB used)\n",
                (uint64_t)(_kernel_end - _kernel_start) >> 10, heap_size() >> 10, heap_used() >> 10);

    for (int i = 0; i < tty_count; i++) {           /* one line per monitor, each with its own grid */
        const struct display_head *h = display_get(i);
        const struct kestrel_tty *t = tty_get(i);
        tty_printf(sh->tty, C_LABEL "%s" C_RESET "monitor %d: %dx%dx32 at 0x%lx, tty%d %d x %d cells, %s%s%s\n",
                   i ? "             " : "  Display    ", i, t->native_width, t->native_height, h->phys,
                   i + 1, t->max_cols, t->max_rows, h->name,
                   i == active_keyboard_tty ? ", " C_OK "keyboard" C_RESET : "",
                   t == sh->tty ? " (this shell)" : "");
    }
    storage_summary(a, sizeof a);
    tty_printf(sh->tty, C_LABEL "  Storage    " C_RESET "%s\n", a);
    usb_summary(a, sizeof a);
    tty_printf(sh->tty, C_LABEL "  USB        " C_RESET "%s\n", a);
    tty_printf(sh->tty, C_LABEL "  PCI        " C_RESET "%zu functions\n", pci_device_count());
    tty_printf(sh->tty, C_LABEL "  Scheduler  " C_RESET "%s, %lu context switches\n",
                sched_preemption() ? "preemptive round-robin" : "cooperative (safe mode)", sched_context_switches());
    return 0;
}

/* Commands that are views of a /proc file */
static const struct { const char *cmd, *path; } views[] = {
    { "ps", "/proc/tasks" }, { "mem", "/proc/meminfo" }, { "pci", "/proc/pci" }, { "usb", "/proc/usb" },
    { "bootvol", "/proc/bootvol" }, { "lsblk", "/proc/partitions" },
};

static int cmd_view(struct kush_session *sh, int argc, char **argv)
{
    (void)argc;
    for (size_t i = 0; i < sizeof views / sizeof *views; i++)
        if (strcmp(argv[0], views[i].cmd) == 0) { show_file(sh, views[i].path); return 0; }
    return 1;
}

static int cmd_log(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_puts(sh->tty, C_DIM);
    show_file(sh, "/proc/kmsg");
    tty_puts(sh->tty, C_RESET);
    return 0;
}

static int cmd_disk(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    char s[160];
    storage_summary(s, sizeof s);
    tty_printf(sh->tty, "%s\n", s);
    struct file *f;
    if (vfs_open("/dev/sda", O_RDONLY, 0, &f) < 0) return 0;
    uint8_t *mbr = kmalloc(512);
    if (mbr && vfs_read(f, mbr, 512) == 512)
        tty_printf(sh->tty, "/dev/sda LBA 0: boot signature %02x %02x (%s)\n", mbr[510], mbr[511],
                    mbr[510] == 0x55 && mbr[511] == 0xAA ? C_OK "valid MBR" C_RESET : C_ERR "none" C_RESET);
    kfree(mbr);
    vfs_close(f);
    return 0;
}

static int cmd_recovery(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    enum recovery_action a = recovery_enter("Recovery console requested from the shell", NULL);
    tty_printf(sh->tty, "left the recovery console (%s)\n", a == RECOVERY_RETRY ? "retry" : "continue");
    return 0;
}

static int cmd_font(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_puts(sh->tty, C_DIM "     0 1 2 3 4 5 6 7 8 9 A B C D E F" C_RESET "\n");
    for (int r = 0; r < 16; r++) {
        tty_printf(sh->tty, C_DIM " %X0  " C_RESET, r);
        for (int c = 0; c < 16; c++) { tty_put_glyph(sh->tty, (unsigned char)(r * 16 + c)); tty_putc(sh->tty, ' '); }
        tty_putc(sh->tty, '\n');
    }
    return 0;
}

static int cmd_report(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_printf(sh->tty, "writing the report to %s ...\n", report_where());
    int rc = report_save("requested from the shell");
    if (rc == 0) tty_printf(sh->tty, C_OK "done" C_RESET ": %s\n", report_where());
    else tty_printf(sh->tty, C_ERR "failed" C_RESET ": %s (boot from a stick made with 'make usbimg')\n", blk_strerror(rc));
    return rc ? 1 : 0;
}

static int cmd_gpu(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t cap = 64 * 1024;
    char *buf = kmalloc(cap);
    if (!buf) return 1;
    tty_write(sh->tty, buf, gpu_report(buf, cap));
    kfree(buf);
    return 0;
}

/* run PROGRAM [ARGS...]: start a ring-3 ELF program on this terminal and
 * wait for it. A bare name is looked up in /boot/bin (the boot volume). */
static int cmd_run(struct kush_session *sh, int argc, char **argv)
{
    if (argc < 2) { tty_puts(sh->tty, "usage: run PROGRAM [ARGS...]\n"); return 2; }
    char path[128];
    if (strchr(argv[1], '/')) strlcpy(path, argv[1], sizeof path);
    else snprintf(path, sizeof path, "/boot/bin/%s", argv[1]);
    int pid = exec_spawn(path, argc - 1, argv + 1);
    if (pid < 0) { tty_printf(sh->tty, "run: %s: %s\n", path, pid == -ENOENT ? "not found" : "cannot start"); return 127; }
    int st = exec_wait(pid);
    if (st >= 128) tty_printf(sh->tty, "[%s: process %d killed by signal %d]\n", argv[1], pid, st - 128);
    else if (st) tty_printf(sh->tty, "[%s: exit status %d]\n", argv[1], st);
    return st;
}

static int cmd_reboot(struct kush_session *sh, int argc, char **argv)
{
    (void)argc;
    bool off = argv[0][0] == 'p';
    tty_puts(sh->tty, off ? "Powering off...\n" : "Restarting...\n");
    u_reboot(off ? LINUX_REBOOT_CMD_POWER_OFF : LINUX_REBOOT_CMD_RESTART);
    return 1;
}

static const struct kush_cmd commands[] = {
    { "help",     "",       "list the built-in Kestrel tools",                 cmd_help },
    { "clear",    "",       "clear the screen",                                cmd_clear },
    { "sysinfo",  "",       "OS version, CPU details and detected memory",     cmd_sysinfo },
    { "ps",       "",       "tasks and their scheduler statistics",            cmd_view },
    { "mem",      "",       "physical memory and kernel heap usage",           cmd_view },
    { "pci",      "",       "PCI functions found at boot",                     cmd_view },
    { "usb",      "",       "USB devices, keyboard and mouse counters",        cmd_view },
    { "disk",     "",       "SATA disks and the MBR of /dev/sda",              cmd_disk },
    { "lsblk",    "",       "block devices and partitions (SATA + USB)",       cmd_view },
    { "bootvol",  "",       "how the boot volume was found and mounted",       cmd_view },
    { "recovery", "",       "open the boot recovery console",                  cmd_recovery },
    { "log",      "",       "recent kernel messages",                          cmd_log },
    { "report",   "",       "write REPORT.TXT + VBIOS.ROM to the boot stick",  cmd_report },
    { "gpu",      "",       "NVIDIA GPU: chip, VBIOS tables, outputs, monitors", cmd_gpu },
    { "run",      "PROG",   "run a ring-3 program (from /boot/bin)",           cmd_run },
    { "font",     "",       "show all 256 glyphs of kestrel_font",             cmd_font },
    { "reboot",   "",       "restart the machine",                             cmd_reboot },
    { "poweroff", "",       "power off (emulators) or halt",                   cmd_reboot },
    { NULL, NULL, NULL, NULL },
};

static int cmd_help(struct kush_session *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    tty_puts(sh->tty, C_TITLE "kush built-in tools" C_RESET "\n");
    for (const struct kush_cmd *c = commands; c->name; c++) {
        char name[32];
        snprintf(name, sizeof name, "%s %s", c->name, c->args);
        tty_printf(sh->tty, "  " C_OK "%-28s" C_RESET " %s\n", name, c->help);
    }
    tty_puts(sh->tty, C_TITLE "core utilities" C_RESET C_DIM " (system calls; output can be redirected with > and >>)" C_RESET "\n");
    for (const struct cu_cmd *c = cu_commands; c->name; c++) {
        char name[32];
        snprintf(name, sizeof name, "%s %s", c->name, c->usage);
        tty_printf(sh->tty, "  " C_OK "%-28s" C_RESET " %s\n", name, c->help);
    }
    tty_puts(sh->tty, C_DIM "  syntax: 'quotes' \"quotes\" \\escapes  > file  >> file  < file  $?   keys: Up/Down history, Tab, Ctrl+C/U/L" C_RESET "\n");
    return 0;
}

/* ======================================================================== */
/*  parser                                                                     */
/* ======================================================================== */
/* Split a command line into words. Supports 'single' and "double" quotes,
 * backslash escapes, $? (last exit status) and the redirections
 * > FILE, >> FILE and < FILE. Words are written into `store`. */
struct parsed {
    int   argc;
    char *argv[KUSH_MAX_ARGS + 1];
    char *out_file, *in_file;
    bool  append;
};

static int kush_parse(const char *line, char *store, size_t cap, struct parsed *p, int last_status)
{
    memset(p, 0, sizeof *p);
    size_t w = 0;
    const char *s = line;
    char **pending = NULL;                      /* redirection waiting for its file name */
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '>' || *s == '<') {
            if (pending) return -1;
            if (*s == '<') { pending = &p->in_file; s++; }
            else { p->append = s[1] == '>'; pending = &p->out_file; s += p->append ? 2 : 1; }
            continue;
        }
        char *word = store + w;
        char quote = 0;
        bool any = false;
        while (*s && (quote || (*s != ' ' && *s != '\t' && *s != '>' && *s != '<'))) {
            char c = *s++;
            if (quote) {
                if (c == quote) { quote = 0; continue; }
                if (quote == '"' && c == '\\' && (*s == '"' || *s == '\\' || *s == '$')) c = *s++;
                else if (quote == '"' && c == '$' && *s == '?') { s++; w += (size_t)snprintf(store + w, cap - w, "%d", last_status); any = true; continue; }
            } else if (c == '\'' || c == '"') { quote = c; any = true; continue; }
            else if (c == '\\' && *s) c = *s++;
            else if (c == '$' && *s == '?') { s++; w += (size_t)snprintf(store + w, cap - w, "%d", last_status); any = true; continue; }
            if (w + 1 >= cap) return -2;
            store[w++] = c;
            any = true;
        }
        if (quote) return -3;                   /* unterminated quote */
        if (w + 1 >= cap) return -2;
        store[w++] = 0;
        if (!any) continue;
        if (pending) { *pending = word; pending = NULL; continue; }
        if (p->argc >= KUSH_MAX_ARGS) return -4;
        p->argv[p->argc++] = word;
    }
    if (pending) return -1;
    p->argv[p->argc] = NULL;
    return 0;
}

static int kush_execute(struct kush_session *sh, char *cmdline)
{
    struct parsed p;
    int rc = kush_parse(cmdline, sh->store, sizeof sh->store, &p, sh->last_status);
    if (rc < 0) {
        static const char *const why[] = { "", "missing file name after redirection", "line too long",
                                           "unterminated quote", "too many arguments" };
        tty_printf(sh->tty, C_ERR "kush: syntax error: %s" C_RESET "\n", why[-rc]);
        return 2;
    }
    if (p.argc == 0) return 0;

    for (const struct kush_cmd *c = commands; c->name; c++)
        if (strcmp(c->name, p.argv[0]) == 0) {
            if (p.out_file || p.in_file) tty_puts(sh->tty, C_DIM "kush: note: shell built-ins write to the screen; redirection ignored" C_RESET "\n");
            return c->fn(sh, p.argc, p.argv);
        }

    const struct cu_cmd *u = cu_find(p.argv[0]);
    if (!u) {
        tty_printf(sh->tty, C_ERR "kush: %s: command not found" C_RESET " (try 'help')\n", p.argv[0]);
        return 127;
    }
    struct cu_io io = { 0, 1, 2 };
    if (p.in_file) {
        io.in = u_open(p.in_file, O_RDONLY, 0);
        if (io.in < 0) { tty_printf(sh->tty, C_ERR "kush: %s: %s" C_RESET "\n", p.in_file, cu_strerror(io.in)); return 1; }
    }
    if (p.out_file) {
        io.out = u_open(p.out_file, O_WRONLY | O_CREAT | (p.append ? O_APPEND : O_TRUNC), 0644);
        if (io.out < 0) {
            tty_printf(sh->tty, C_ERR "kush: %s: %s" C_RESET "\n", p.out_file, cu_strerror(io.out));
            if (io.in != 0) u_close(io.in);
            return 1;
        }
    }
    rc = u->fn(&io, p.argc, p.argv);
    if (io.in != 0) u_close(io.in);
    if (io.out != 1) u_close(io.out);
    return rc;
}

/* ======================================================================== */
/*  line editor                                                                */
/* ======================================================================== */
static void erase_line(struct kush_session *sh)
{
    while (LINE_LEN) { LINE_LEN--; tty_backspace(sh->tty); }
}

static void replace_line(struct kush_session *sh, const char *text)
{
    erase_line(sh);
    size_t n = strnlen(text, KUSH_LINE_MAX - 1);
    memcpy(LINE, text, n);
    LINE_LEN = (int)n;
    tty_write(sh->tty, LINE, (size_t)LINE_LEN);
}

static void complete(struct kush_session *sh)
{
    size_t len = (size_t)LINE_LEN;
    if (memchr(LINE, ' ', len)) return;         /* only the command word */
    const char *match = NULL;
    int count = 0;
    for (const struct kush_cmd *c = commands; c->name; c++)
        if (strncmp(c->name, LINE, len) == 0) { match = c->name; count++; }
    for (const struct cu_cmd *c = cu_commands; c->name; c++)
        if (strncmp(c->name, LINE, len) == 0) { match = c->name; count++; }
    if (count == 1) {
        for (const char *p = match + len; *p && LINE_LEN < KUSH_LINE_MAX - 1; p++) {
            LINE[LINE_LEN++] = *p;
            tty_putc(sh->tty, *p);
        }
        if (LINE_LEN < KUSH_LINE_MAX - 1) { LINE[LINE_LEN++] = ' '; tty_putc(sh->tty, ' '); }
    } else if (count > 1) {                      /* show the candidates, then the line again */
        tty_putc(sh->tty, '\n');
        for (const struct kush_cmd *c = commands; c->name; c++)
            if (strncmp(c->name, LINE, len) == 0) tty_printf(sh->tty, "%s  ", c->name);
        for (const struct cu_cmd *c = cu_commands; c->name; c++)
            if (strncmp(c->name, LINE, len) == 0) tty_printf(sh->tty, "%s  ", c->name);
        tty_putc(sh->tty, '\n');
        kush_prompt(sh);
        tty_write(sh->tty, LINE, len);
    }
}

static const char *history_at(struct kush_session *sh, int pos)
{
    return sh->history[(sh->hist_next - (sh->hist_count - pos) + KUSH_HISTORY) % KUSH_HISTORY];
}

/* Returns the line length, or -1 if the line was cancelled. */
static int kush_readline(struct kush_session *sh)
{
    LINE_LEN = 0;
    int hist_pos = sh->hist_count;              /* one past the newest entry */

    for (;;) {
        struct key_event ev;
        tty_read_key(sh->tty, &ev, true);       /* sleeps until a key arrives on this TTY */

        switch (ev.key) {
        case KEY_ENTER:
            LINE[LINE_LEN] = 0;
            tty_putc(sh->tty, '\n');            /* cursor_row++, cursor_col = 0 */
            return LINE_LEN;
        case KEY_BACKSPACE:
            if (LINE_LEN) { LINE_LEN--; tty_backspace(sh->tty); }
            continue;
        case KEY_UP:
            if (hist_pos > 0 && sh->hist_count) replace_line(sh, history_at(sh, --hist_pos));
            continue;
        case KEY_DOWN:
            if (hist_pos < sh->hist_count) {
                hist_pos++;
                if (hist_pos == sh->hist_count) erase_line(sh);
                else replace_line(sh, history_at(sh, hist_pos));
            }
            continue;
        case KEY_TAB:
            complete(sh);
            continue;
        case KEY_CHAR:
            break;
        default:
            continue;                           /* other special keys: ignored */
        }

        char c = ev.ascii;
        if (c == 3) { tty_puts(sh->tty, "^C\n"); return -1; }                /* Ctrl+C */
        if (c == 21) { erase_line(sh); continue; }                          /* Ctrl+U */
        if (c == 12) {                                                      /* Ctrl+L */
            tty_clear(sh->tty); kush_prompt(sh); tty_write(sh->tty, LINE, (size_t)LINE_LEN);
            continue;
        }
        if ((unsigned char)c < 0x20 || c == 0x7F) continue;                 /* non-printable */
        if (LINE_LEN >= KUSH_LINE_MAX - 1) continue;                        /* buffer full */
        LINE[LINE_LEN++] = c;
        tty_putc(sh->tty, c);                   /* drawn at the cursor, then cursor_col++ */
    }
}

static void remember(struct kush_session *sh, const char *cmdline)
{
    if (!*cmdline) return;
    int newest = (sh->hist_next - 1 + KUSH_HISTORY) % KUSH_HISTORY;
    if (sh->hist_count && strcmp(sh->history[newest], cmdline) == 0) return;   /* no duplicates in a row */
    strlcpy(sh->history[sh->hist_next], cmdline, KUSH_LINE_MAX);
    sh->hist_next = (sh->hist_next + 1) % KUSH_HISTORY;
    if (sh->hist_count < KUSH_HISTORY) sh->hist_count++;
}

/* ======================================================================== */
/*  REPL                                                                       */
/* ======================================================================== */
void kush_main(void)
{
    struct kush_session *sh = kzalloc(sizeof *sh);      /* ~20 KiB: not on the 16 KiB stack */
    struct kestrel_tty *tty = tty_current();
    if (!sh || !tty) panic("kush: no session memory or no terminal");
    sh->tty = tty;
    u_chdir("/root");                           /* start in root's home, like a login shell */
    tty_printf(tty, C_TITLE "kush" C_RESET " (Kestrel shell) on " C_OK "tty%d" C_RESET
               ", monitor %d, %dx%d, %d x %d cells -- type " C_OK "help" C_RESET " for a list of tools.\n"
               C_DIM "Ctrl+Alt+F1..F%d moves the keyboard to another monitor." C_RESET "\n\n",
               tty->index + 1, tty->index, tty->native_width, tty->native_height,
               tty->max_cols, tty->max_rows, tty_count);
    tty_flush_keys(tty);
    for (;;) {
        kush_prompt(sh);
        int n = kush_readline(sh);
        if (n < 0) continue;
        memcpy(sh->cmdline, LINE, (size_t)n + 1);       /* hand the line to the parser, */
        LINE_LEN = 0;                                   /* and reset the input index    */
        remember(sh, sh->cmdline);
        sh->last_status = kush_execute(sh, sh->cmdline);
    }
}
