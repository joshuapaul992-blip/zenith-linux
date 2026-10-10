/* kernel/kernel.c -- Kestrel kernel entry and initialisation sequence
 *
 * boot.asm enters here in 64-bit long mode with the first 4 GiB identity
 * mapped, interrupts disabled and a 32 KiB boot stack.
 *
 *   Phase 1  hardware & memory  serial log, Multiboot2 parsing, CPU detection,
 *                               GDT/TSS, IDT, physical memory, heap, frame
 *                               buffer (+ back buffer), PIC, PIT, keyboard,
 *                               scheduler (boot thread = pid 0)
 *   Phase 2  boot manager       menu, countdown, tools, F8 options
 *   Phase 3  kernel services    monitors + one TTY per monitor, syscalls, VFS
 *                               (/ /dev /proc /sys), init (pid 1) with a shell
 *                               per monitor, kernel worker threads
 *   Phase 4  idle               pid 0 halts until there is work
 */
#include <kernel/exec.h>
#include <kernel/uvm.h>
#include <kernel/cpu.h>
#include <kernel/arch.h>
#include <kernel/multiboot2.h>
#include <kernel/serial.h>
#include <kernel/klog.h>
#include <kernel/fb.h>
#include <kernel/console.h>
#include <kernel/keyboard.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/vfs.h>
#include <kernel/syscall.h>
#include <kernel/usyscall.h>
#include <kernel/bootmgr.h>
#include <kernel/bootinfo.h>
#include <kernel/string.h>
#include "pci.h"
#include <kernel/storage.h>
#include <kernel/usbhost.h>
#include <kernel/tty.h>
#include <kernel/kinput.h>
#include <kernel/display.h>
#include <kernel/report.h>
#include <kernel/kush.h>
#include <kernel/vt.h>
#include <kernel/time.h>
#include <kernel/bootvol.h>
#include <kernel/block.h>

#define KERNEL_HEAP_SIZE (8u << 20)
#define WANT_WIDTH  1024
#define WANT_HEIGHT 768

struct bootinfo g_boot;

/* ========================================================================== */
/*  Phase 1 helpers                                                             */
/* ========================================================================== */

static struct mb2_tag_framebuffer *parse_multiboot(uintptr_t mbi)
{
    struct mb2_tag_framebuffer *fb = NULL;
    for (struct mb2_tag *t = mb2_first_tag(mbi); t->type != MB2_TAG_END; t = mb2_next_tag(t)) {
        switch (t->type) {
        case MB2_TAG_CMDLINE:
            strlcpy(g_boot.cmdline, ((struct mb2_tag_string *)t)->string, sizeof g_boot.cmdline);
            break;
        case MB2_TAG_BOOT_LOADER_NAME:
            strlcpy(g_boot.loader, ((struct mb2_tag_string *)t)->string, sizeof g_boot.loader);
            break;
        case MB2_TAG_FRAMEBUFFER:
            fb = (struct mb2_tag_framebuffer *)t;
            break;
        case 11:                            /* EFI 32-bit system table */
        case MB2_TAG_EFI64:
            g_boot.uefi = true;
            break;
        }
    }
    return fb;
}

static void detect_cpu(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(g_boot.cpu_vendor + 0, &b, 4);
    memcpy(g_boot.cpu_vendor + 4, &d, 4);
    memcpy(g_boot.cpu_vendor + 8, &c, 4);
    g_boot.cpu_vendor[12] = 0;

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *brand = (uint32_t *)g_boot.cpu_brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++)
            cpuid(0x80000002 + leaf, 0, &brand[leaf * 4], &brand[leaf * 4 + 1],
                  &brand[leaf * 4 + 2], &brand[leaf * 4 + 3]);
        g_boot.cpu_brand[48] = 0;
        char *s = g_boot.cpu_brand;                  /* trim leading spaces */
        while (*s == ' ') s++;
        memmove(g_boot.cpu_brand, s, strlen(s) + 1);
    }
}

static void video_init(struct mb2_tag_framebuffer *t)
{
    if (!t) panic("no frame buffer: the boot loader ignored the Multiboot2 video request");
    if (t->fb_type != MB2_FB_TYPE_RGB)
        panic("boot loader gave a %s mode, not a linear RGB frame buffer",
              t->fb_type == MB2_FB_TYPE_EGA ? "VGA text" : "palette");

    uint64_t size = (uint64_t)t->pitch * t->height;
    if (t->addr + size > PMM_MAX_PHYS && !vmm_identity_map(t->addr, size, VMM_WRITE))
        panic("cannot map frame buffer at %lx", t->addr);
    if (!fb_init(t)) panic("unsupported frame buffer: %u bpp", t->bpp);

    kprintf("video: %ux%ux%u, pitch %u, LFB at %lx via %s\n", t->width, t->height, t->bpp,
            t->pitch, t->addr, g_boot.uefi ? "UEFI GOP" : "VBE");
    if (t->width != WANT_WIDTH || t->height != WANT_HEIGHT)
        kprintf("video: WARNING requested %dx%d, got %ux%u (layout adapts)\n",
                WANT_WIDTH, WANT_HEIGHT, t->width, t->height);

    uint64_t frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t back = pmm_alloc_contig(frames);
    if (back) {
        fb_attach_backbuffer((void *)back);
        kprintf("video: %lu KiB back buffer at %lx (double buffering on)\n", size >> 10, back);
    }
    fb_clear(COL_BLACK);
    fb_flush();
}

/* The PCI enumerator logs through klog_putc(): COM1 + the kernel log. */
static void pci_init(void)
{
    pci_set_log_sink(klog_putc);
    int n = pci_enumerate(PCI_SCAN_RECURSIVE, PCI_SCAN_LOG | PCI_SCAN_SIZE_BARS);
    if (n < 0) kprintf("pci: no configuration mechanism #1, bus not scanned\n");
}

/* ========================================================================== */
/*  Phase 3: services, init (summary + shell) and kernel threads                */
/* ========================================================================== */

/* Once Phase 3 starts every monitor has its own terminal (tty.c); the generic
 * console (g_con) forwards its output to the calling task's TTY. */
#define CON_ROWS ((int)g_fb.height / 16)

/* --- init (pid 1): talks to the kernel only through system calls ---------- */

static void u_puts(const char *s) { u_write(1, s, strlen(s)); }

static void u_cat(const char *path)
{
    int fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) { u_puts("  (cannot open "); u_puts(path); u_puts(")\n"); return; }
    char buf[512]; ssize_t n;
    while ((n = u_read(fd, buf, sizeof buf)) > 0) u_write(1, buf, (size_t)n);
    u_close(fd);
}

/* List a directory with getdents64(2); recurse when depth > 0. */
static void u_tree(const char *path, int depth, int indent)
{
    int fd = u_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return;
    char *buf = kmalloc(2048);
    int n;
    while (buf && (n = u_getdents64(fd, buf, 2048)) > 0) {
        for (int off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
            char child[VFS_PATH_MAX], line[160];
            snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", d->d_name);
            struct stat st;
            u_stat(child, &st);
            const char *col = d->d_type == DT_DIR ? "\033[94m" : d->d_type == DT_CHR ? "\033[93m" : "\033[0m";
            snprintf(line, sizeof line, "%*s%s%s%s\033[0m", indent * 2, "", col, d->d_name,
                     d->d_type == DT_DIR ? "/" : "");
            u_puts(line);
            if (d->d_type == DT_REG && st.st_size) { snprintf(line, sizeof line, "  \033[90m%ld bytes\033[0m", st.st_size); u_puts(line); }
            if (d->d_type == DT_CHR) { snprintf(line, sizeof line, "  \033[90mchar %lu,%lu\033[0m", st.st_rdev >> 8, st.st_rdev & 0xFF); u_puts(line); }
            u_puts("\n");
            if (d->d_type == DT_DIR && depth > 0) u_tree(child, depth - 1, indent + 1);
        }
    }
    kfree(buf);
    u_close(fd);
}

static void u_ls_line(const char *path)
{
    int fd = u_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return;
    char buf[1024], line[512];
    int n, pos = snprintf(line, sizeof line, "  \033[94m%-6s\033[0m", path);
    while ((n = u_getdents64(fd, buf, sizeof buf)) > 0)
        for (int off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (d->d_name[0] == '.') continue;
            pos += snprintf(line + pos, sizeof line - (size_t)pos, " %s", d->d_name);
            if (pos >= (int)sizeof line - 1) break;
        }
    u_close(fd);
    u_puts(line);
    u_puts("\n");
}

static void section(const char *title)
{
    char buf[128];
    snprintf(buf, sizeof buf, "\n\033[97m%s\033[0m\n", title);
    u_puts(buf);
}

/* /bin/sh on another monitor's terminal (vt.c). */
static int getty_main(void *arg)
{
    vt_session((int)(intptr_t)arg);
}

/* kush on another monitor: bind the thread to that TTY first, so its
 * stdin/stdout (/dev/tty) and everything it starts use that monitor. */
static int shell_main(void *arg)
{
    tty_bind_current((int)(intptr_t)arg);
    u_open("/dev/tty", O_RDONLY, 0);
    u_open("/dev/tty", O_WRONLY, 0);
    u_open("/dev/tty", O_WRONLY, 0);
    kush_main();
}

static int exec_waiter(void *arg)
{
    int pid = (int)(intptr_t)arg;
    int st = exec_wait(pid);
    kprintf("kestrel.exec: process %d finished with status %d%s\n", pid, st, st >= 128 ? " (killed)" : "");
    return 0;
}

static int exec_launcher(void *arg)
{
    for (char *prog = arg; prog && *prog; ) {
        char *next = strchr(prog, '+');
        if (next) *next++ = 0;
        size_t len = strlen(prog);
        if (len >= 2 && prog[len - 1] == 's' && prog[0] >= '0' && prog[0] <= '9') {
            uint64_t sec = 0;
            for (size_t k = 0; k + 1 < len && prog[k] >= '0' && prog[k] <= '9'; k++)
                sec = sec * 10 + (uint64_t)(prog[k] - '0');
            task_sleep_ms(sec * 1000);
            prog = next;
            continue;
        }
        char *args[EXEC_MAX_ARGS];
        int n = 0;
        for (char *p = prog; p && n < EXEC_MAX_ARGS; ) {
            args[n++] = p;
            p = strchr(p, ',');
            if (p) *p++ = 0;
        }
        int pid = exec_spawn_io(args[0], n, args, "/dev/kmsg");     /* output into the log */
        if (pid < 0) kprintf("kestrel.exec: %s: cannot start (%d)\n", args[0], pid);
        else task_create("exec-wait", exec_waiter, (void *)(intptr_t)pid);
        prog = next;
    }
    return 0;
}

static int init_main(void *arg)
{
    (void)arg;
    /* stdin, stdout, stderr -> the console tty */
    u_open("/dev/tty", O_RDONLY, 0);
    u_open("/dev/tty", O_WRONLY, 0);
    u_open("/dev/tty", O_WRONLY, 0);

    struct utsname un;
    u_uname(&un);
    char buf[256];
    snprintf(buf, sizeof buf, "\ninit: pid %d (ppid %d) on %s %s %s -- talking to the kernel via int 0x80\n",
             u_getpid(), u_getppid(), un.sysname, un.release, un.machine);
    u_puts(buf);

    section("Kernel (/proc/version)");
    u_cat("/proc/version");
    section("Mounted file systems (/proc/mounts)");
    u_cat("/proc/mounts");

    if (g_boot.boot_flags & BOOTOPT_VERBOSE_VFS) {
        section("Virtual file system tree");
        u_puts("/\n");
        u_tree("/", 6, 1);
    } else {
        section("Virtual file system");
        u_ls_line("/");
        u_ls_line("/dev");
        u_ls_line("/proc");
        u_ls_line("/sys");
    }
    section("Framebuffer (/sys/class/graphics/fb0)");
    u_puts("  virtual_size   "); u_cat("/sys/class/graphics/fb0/virtual_size");
    u_puts("  bits_per_pixel "); u_cat("/sys/class/graphics/fb0/bits_per_pixel");
    u_puts("  driver         "); u_cat("/sys/class/graphics/fb0/name");
    section("Processes (/proc/tasks)");
    u_cat("/proc/tasks");
    u_puts("\n");
    u_cat("/etc/motd");

    if (!(g_boot.boot_flags & BOOTOPT_DEBUG)) klog_set_console(false);

    /* One shell per monitor: TTY 0 keeps this thread, every other TTY gets a
     * thread of its own, so a command running on one monitor never blocks
     * typing on another. The shell is /bin/sh (BusyBox ash) on a real
     * terminal; kush, the built-in one, with kestrel.shell=kush or when the
     * boot volume has no /bin/sh. */
    bool sh = vt_shell_available();
    for (int i = 1; i < tty_count; i++) {
        char name[TASK_NAME_LEN];
        snprintf(name, sizeof name, "%s/tty%d", sh ? "getty" : "kush", i + 1);
        if (!task_create(name, sh ? getty_main : shell_main, (void *)(intptr_t)i))
            kprintf("init: cannot start a shell on tty%d\n", i + 1);
    }
    if (sh && tty_count) {
        u_puts("\n");
        vt_session(0);                            /* /bin/sh on tty1; never returns */
    }
    kush_main();                                  /* interactive shell on tty1; never returns */
}

/* --- kworker/0: CPU-bound work, proves time-slice preemption ------------- */
static volatile uint64_t primes_found;

static int kworker_compute(void *arg)
{
    (void)arg;
    for (uint64_t n = 3;; n += 2) {
        bool prime = true;
        for (uint64_t d = 3; d * d <= n; d += 2) if (n % d == 0) { prime = false; break; }
        if (prime) primes_found++;
        if (n > 50000000) n = 3;
    }
    return 0;
}

/* --- kworker/1: periodic housekeeping, proves timed sleep/wakeup ---------- */
static int kworker_heartbeat(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep_ms(30000);
        kprintf("kworker/1: heartbeat, uptime %lus, %lu primes found by kworker/0\n",
                uptime_ms() / 1000, primes_found);
    }
    return 0;
}

/* ========================================================================== */
/*  kernel_main                                                                 */
/* ========================================================================== */

void kernel_main(uint32_t magic, uintptr_t mbi)
{
    /* ---------------- Phase 1: hardware & memory ---------------- */
    serial_init();
    g_boot.boot_tsc = rdtsc();
    kprintf("\n%s %s (%s) starting\n", KESTREL_NAME, KESTREL_VERSION, KESTREL_MACHINE);

    if (magic != MB2_BOOTLOADER_MAGIC) panic("bad Multiboot2 magic %x", magic);
    g_boot.mbi = mbi;
    struct mb2_tag_framebuffer *fbt = parse_multiboot(mbi);
    detect_cpu();
    kprintf("boot: loader \"%s\", firmware %s, cmdline \"%s\"\n", g_boot.loader,
            g_boot.uefi ? "UEFI" : "BIOS", g_boot.cmdline);
    kprintf("cpu: %s / %s\n", g_boot.cpu_vendor, g_boot.cpu_brand);

    gdt_init();
    idt_init();
    kprintf("arch: GDT+TSS loaded, IDT with 32 exceptions, 16 IRQs, int 0x80 gate\n");

    pmm_init(mbi);
    heap_init(KERNEL_HEAP_SIZE);
    time_init(mbi);             /* HPET / calibrated TSC: timeouts that work before sti */
    pci_init();                 /* before video: BAR sizing briefly disables decoding */
    video_init(fbt);
    storage_init();             /* AHCI: HBA bring-up + MBR verify loop (COM1 + klog) */
    usb_init();                 /* xHCI: USB keyboards/mice usable in the boot menu */

    pic_init();
    pit_init(PIT_HZ);
    keyboard_init();
    uvm_init();                 /* shared kernel PDPTs, before any address space */
    sched_init();
    sti();
    kprintf("arch: interrupts enabled, PIT at %d Hz\n", PIT_HZ);

    /* ---------------- Phase 2: boot manager ---------------- */
    struct boot_choice ch = bootmgr_run();
    g_boot.boot_entry = (uint32_t)ch.entry;
    g_boot.boot_flags = ch.flags;
    strlcpy(g_boot.boot_entry_name, ch.name, sizeof g_boot.boot_entry_name);

    if (ch.flags & BOOTOPT_NO_DBLBUF) fb_detach_backbuffer();

    /* ---------------- Phase 3: kernel services ---------------- */
    fb_clear(COL_BLACK);
    fb_flush();
    con_init(&g_con, &font_8x16, 0, 0, (int)g_fb.width / 8, CON_ROWS, COL_LIGHTGRAY, COL_BLACK);
    display_probe();                                /* boot frame buffer + secondary adapters */
    if (tty_init_all()) con_set_redirect(tty_write_current);   /* a terminal per monitor */
    else con_clear(&g_con);
    con_printf(&g_con, "\033[97mStarting %s\033[0m  (%s)\n\n", ch.name,
               ch.flags ? "advanced options" : "normal boot");

    if (ch.flags & BOOTOPT_DEBUG) {                 /* replay everything logged so far */
        char buf[256]; size_t off = 0, n;
        con_puts(&g_con, "\033[90m");
        while ((n = klog_read(off, buf, sizeof buf)) > 0) { con_write(&g_con, buf, n); off += n; }
        con_puts(&g_con, "\033[0m");
    }
    klog_set_console(true);

    if (ch.flags & BOOTOPT_SAFE_MODE) {
        sched_set_preemption(false);
        kprintf("sched: SAFE MODE - preemption disabled, no worker threads\n");
    }
    syscall_init();
    vfs_init();
    kinput_init();                  /* /dev/kinput, for the X server */
    storage_register_devices();     /* sataN + partitions in the block layer */
    ramdisk_init(mbi);              /* Multiboot2 modules (the ISO's boot volume) */
    current_task()->cwd = vfs_root();

    /* Find the Kestrel boot volume by its markers (never by device name),
     * waiting for USB storage to settle; a mandatory volume that is missing
     * or unreadable ends in the recovery console, not a crash. */
    bootvol_mount_root();

    /* No serial port on many machines: write REPORT.TXT (and the video BIOS)
     * to the stick's "KESTREL RPT" volume, if there is one. */
    if (!strstr(g_boot.cmdline, "kestrel.report=0") && report_locate())
        report_save("written at boot");

    /* kestrel.exec=/boot/bin/prog[,arg...][+/boot/bin/prog2[,arg...]...]:
     * run ring-3 programs at boot (several at once, e.g. a server and its
     * client) with their output in the kernel log, and log each exit status.
     * An entry "Ns" (e.g. "+2s+") waits N seconds before starting the rest,
     * so clients can follow their server. For unattended tests; no keyboard
     * needed. */
    const char *ex = strstr(g_boot.cmdline, "kestrel.exec=");
    if (ex) {
        static char line[1024];
        size_t i = 0;
        for (ex += 13; *ex && *ex != ' ' && i + 1 < sizeof line; ex++) line[i++] = *ex;
        line[i] = 0;
        task_create("exec-launch", exec_launcher, line);
    }

    if (!task_create("init", init_main, NULL)) panic("cannot start init");
    if (!(ch.flags & BOOTOPT_SAFE_MODE)) {
        struct tcb *kw = task_create("kworker/0", kworker_compute, NULL);   /* demo load: idle class */
        if (kw) task_set_idle_class(kw);
        task_create("kworker/1", kworker_heartbeat, NULL);
    }
    usb_start_thread();
    kprintf("kernel: services up at t=%lu ms (boot menu included)\n", uptime_ms());

    /* ---------------- Phase 4: become the idle thread ---------------- */
    sched_idle_loop();
}
