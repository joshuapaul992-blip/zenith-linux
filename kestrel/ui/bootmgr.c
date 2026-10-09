/* ui/bootmgr.c -- boot manager screen in the style of the classic BIOS-based
 * Windows Boot Manager: black background, light-gray monospace text, a
 * silver title bar, a thick silver highlight bar that slides between
 * entries, a live countdown under the entry list, a "Tools:" block, and a
 * key legend along the bottom edge.
 *
 * Layout uses the 12x24 font as an 85 x 32 character grid on 1024x768.
 *
 *   row  0   [######### Kestrel Boot Manager #########]   title bar
 *   row  2   Choose an operating system to start, or press TAB to select a tool:
 *   row  3   (Use the arrow keys to highlight your choice, then press ENTER.)
 *   row  5   [ Kestrel 0.1 (x86_64)                    > ] highlight bar
 *   row  6     Kestrel 0.1 (x86_64) [Safe Mode]
 *   row  9   Seconds until the highlighted choice will be started automatically: 10
 *   row 11   To specify an advanced option for this choice, press F8.
 *   row 21   Tools:
 *   row 23     Memory Diagnostic ...
 *   row 31   [ENTER=Choose          TAB=Menu          ESC=Cancel]  legend bar
 */
#include <kernel/bootmgr.h>
#include <kernel/bootinfo.h>
#include <kernel/fb.h>
#include <kernel/keyboard.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/multiboot2.h>
#include "pci.h"
#include <kernel/storage.h>
#include <kernel/usbhost.h>

#define F       (&font_12x24)
#define CW      12
#define CH      24
#define ROW(r)  ((r) * CH)
#define SCR_W   ((int)g_fb.width)
#define SCR_H   ((int)g_fb.height)
#define BAR_X   (3 * CW)
#define BAR_W   (SCR_W - 2 * BAR_X)
#define TEXT_X  (4 * CW)
#define LAST_ROW ((SCR_H / CH) - 1)

#define ROW_TITLE       0
#define ROW_PROMPT      2
#define ROW_OS          5
#define ROW_TOOLS_LABEL 21
#define ROW_TOOLS       23

/* ------------------------------------------------------------------------- */
/*  menu content                                                                */
/* ------------------------------------------------------------------------- */

static const char *const os_items[] = {
    "Kestrel 0.1 (x86_64)",
    "Kestrel 0.1 (x86_64) [Safe Mode]",
    "Kestrel 0.1 (x86_64) [Debugging Mode]",
};
static const uint32_t os_flags[] = { 0, BOOTOPT_SAFE_MODE, BOOTOPT_DEBUG };
#define N_OS ((int)(sizeof os_items / sizeof *os_items))

static const char *const tool_items[] = {
    "Memory Diagnostic",
    "System Information",
    "Restart Computer",
};
#define N_TOOLS ((int)(sizeof tool_items / sizeof *tool_items))

#define ROW_COUNTDOWN (ROW_OS + N_OS + 1)
#define ROW_F8        (ROW_COUNTDOWN + 2)

struct list {
    int row0, n;
    const char *const *items;
    bool arrow;             /* draw the ">" marker at the right of the bar */
};

static const struct list os_list    = { ROW_OS,    N_OS,    os_items,   true  };
static const struct list tools_list = { ROW_TOOLS, N_TOOLS, tool_items, false };

/* ------------------------------------------------------------------------- */
/*  drawing helpers                                                             */
/* ------------------------------------------------------------------------- */

static void text(int col, int row, const char *s, uint32_t fg)
{
    draw_string(col * CW, ROW(row), s, fg, FB_TRANSPARENT, F);
}

static void clear_row(int row) { draw_rect(0, ROW(row), SCR_W, CH, COL_BLACK); }

static void flush_rows(int row, int n) { fb_flush_rect(0, ROW(row), SCR_W, n * CH); }

static void title_bar(const char *title)
{
    draw_rect(0, ROW(ROW_TITLE), SCR_W, CH, COL_SILVER);
    int x = (SCR_W - (int)strlen(title) * CW) / 2;
    draw_string(x, ROW(ROW_TITLE), title, COL_BLACK, FB_TRANSPARENT, F);
}

/* Bottom legend: up to three labels placed left / centre / right. */
static void legend_bar(const char *left, const char *mid, const char *right)
{
    int y = ROW(LAST_ROW);
    draw_rect(0, y, SCR_W, SCR_H - y, COL_SILVER);
    if (left)  draw_string(CW, y, left, COL_BLACK, FB_TRANSPARENT, F);
    if (mid)   draw_string((SCR_W - (int)strlen(mid) * CW) / 2, y, mid, COL_BLACK, FB_TRANSPARENT, F);
    if (right) draw_string(SCR_W - CW - (int)strlen(right) * CW, y, right, COL_BLACK, FB_TRANSPARENT, F);
}

/* Draw a list with the highlight bar at pixel row `bar_y` (-1 = no bar).
 * Text under the bar is re-drawn in black through a clip rectangle, so the
 * inversion is correct even while the bar is between two entries. */
static void draw_list_at(const struct list *l, int bar_y)
{
    int y0 = ROW(l->row0), h = l->n * CH;
    draw_rect(0, y0, SCR_W, h, COL_BLACK);
    for (int i = 0; i < l->n; i++)
        draw_string(TEXT_X, y0 + i * CH, l->items[i], COL_LIGHTGRAY, FB_TRANSPARENT, F);

    if (bar_y >= 0) {
        draw_rect(BAR_X, bar_y, BAR_W, CH, COL_SILVER);
        fb_set_clip(BAR_X, bar_y, BAR_W, CH);
        for (int i = 0; i < l->n; i++)
            draw_string(TEXT_X, y0 + i * CH, l->items[i], COL_BLACK, FB_TRANSPARENT, F);
        fb_reset_clip();
        if (l->arrow)
            fb_draw_char(BAR_X + BAR_W - 2 * CW, bar_y, '>', COL_BLACK, FB_TRANSPARENT, F);
    }
    fb_flush_rect(0, y0, SCR_W, h);
}

static void draw_list(const struct list *l, int sel)
{
    draw_list_at(l, sel >= 0 ? ROW(l->row0 + sel) : -1);
}

/* Slide the bar from `from` to `to` over ~60 ms. */
static void animate_list(const struct list *l, int from, int to)
{
    const int steps = 6;
    int y_from = ROW(l->row0 + from), y_to = ROW(l->row0 + to);
    for (int s = 1; s <= steps; s++) {
        /* ease-out: larger steps first */
        int t = s * (2 * steps - s);                 /* 0 .. steps^2 */
        draw_list_at(l, y_from + (y_to - y_from) * t / (steps * steps));
        if (s < steps) pit_sleep_ms(10);
    }
}

static void wait_any_key(void)
{
    struct key_event ev;
    keyboard_flush();
    while (!keyboard_poll(&ev)) hlt();
}

/* Word-wrap `s` starting at (col,row), `width` columns wide. */
static int text_wrapped(int col, int row, int width, const char *s, uint32_t fg)
{
    char line[96];
    while (*s) {
        int n = (int)strlen(s);
        if (n > width) {
            n = width;
            while (n > 0 && s[n] != ' ') n--;
            if (n == 0) n = width;
        }
        memcpy(line, s, (size_t)n);
        line[n] = 0;
        text(col, row++, line, fg);
        s += n;
        while (*s == ' ') s++;
    }
    return row;
}

/* ------------------------------------------------------------------------- */
/*  main screen                                                                 */
/* ------------------------------------------------------------------------- */

enum focus { FOCUS_OS, FOCUS_TOOLS };

static void draw_countdown(int seconds)
{
    clear_row(ROW_COUNTDOWN);
    if (seconds >= 0) {
        const char *msg = "Seconds until the highlighted choice will be started automatically: ";
        text(1, ROW_COUNTDOWN, msg, COL_LIGHTGRAY);
        char num[8];
        snprintf(num, sizeof num, "%d", seconds);
        text(1 + (int)strlen(msg), ROW_COUNTDOWN, num, COL_WHITE);
    }
    flush_rows(ROW_COUNTDOWN, 1);
}

static void draw_f8_hint(bool show)
{
    clear_row(ROW_F8);
    if (show) text(1, ROW_F8, "To specify an advanced option for this choice, press F8.", COL_LIGHTGRAY);
    flush_rows(ROW_F8, 1);
}

static void draw_main(enum focus focus, int sel_os, int sel_tool, int countdown)
{
    fb_clear(COL_BLACK);
    title_bar("Kestrel Boot Manager");
    text(1, ROW_PROMPT,     "Choose an operating system to start, or press TAB to select a tool:", COL_WHITE);
    text(1, ROW_PROMPT + 1, "(Use the arrow keys to highlight your choice, then press ENTER.)", COL_LIGHTGRAY);
    text(1, ROW_TOOLS_LABEL, "Tools:", COL_WHITE);
    legend_bar("ENTER=Choose", "TAB=Menu", "ESC=Cancel");
    fb_flush();

    draw_list(&os_list, focus == FOCUS_OS ? sel_os : -1);
    draw_list(&tools_list, focus == FOCUS_TOOLS ? sel_tool : -1);
    draw_countdown(countdown);
    draw_f8_hint(focus == FOCUS_OS);
}

/* ------------------------------------------------------------------------- */
/*  F8: Advanced Boot Options                                                   */
/* ------------------------------------------------------------------------- */

static const char *const adv_items[] = {
    "Safe Mode",
    "Debugging Mode",
    "Verbose File System Report",
    "Disable Double Buffering",
    "",
    "Start Kestrel Normally",
};
static const uint32_t adv_flags[] = {
    BOOTOPT_SAFE_MODE, BOOTOPT_DEBUG, BOOTOPT_VERBOSE_VFS, BOOTOPT_NO_DBLBUF, 0, 0,
};
static const char *const adv_desc[] = {
    "Starts Kestrel with cooperative scheduling only and without the background worker threads.",
    "Mirrors the complete kernel log, including messages logged before this menu appeared, to the screen.",
    "Prints the complete virtual file system tree (/, /dev, /proc and /sys) while starting.",
    "Draws directly into video memory instead of an off-screen buffer. Use this to diagnose display problems.",
    "",
    "Starts Kestrel with its regular settings.",
};
#define N_ADV ((int)(sizeof adv_items / sizeof *adv_items))
#define ROW_ADV       5
#define ROW_ADV_DESC  (LAST_ROW - 5)

static void draw_adv_desc(int sel)
{
    draw_rect(0, ROW(ROW_ADV_DESC), SCR_W, 4 * CH, COL_BLACK);
    text(1, ROW_ADV_DESC, "Description:", COL_WHITE);
    text_wrapped(14, ROW_ADV_DESC, 68, adv_desc[sel], COL_LIGHTGRAY);
    flush_rows(ROW_ADV_DESC, 4);
}

/* Returns BOOTOPT_* flags to boot with, or -1 if cancelled with ESC. */
static int advanced_options(int os_entry)
{
    static const struct list adv_list = { ROW_ADV, N_ADV, adv_items, false };
    int sel = 0;

    fb_clear(COL_BLACK);
    title_bar("Advanced Boot Options");
    text(1, ROW_PROMPT, "Choose Advanced Options for: ", COL_LIGHTGRAY);
    text(30, ROW_PROMPT, os_items[os_entry], COL_WHITE);
    text(1, ROW_PROMPT + 1, "(Use the arrow keys to highlight your choice.)", COL_LIGHTGRAY);
    legend_bar("ENTER=Choose", NULL, "ESC=Cancel");
    fb_flush();
    draw_list(&adv_list, sel);
    draw_adv_desc(sel);

    for (;;) {
        struct key_event ev;
        keyboard_wait(&ev);
        int old = sel;
        switch (ev.key) {
        case KEY_UP:   do sel = sel > 0 ? sel - 1 : sel; while (!*adv_items[sel] && sel > 0); break;
        case KEY_DOWN: do sel = sel < N_ADV - 1 ? sel + 1 : sel; while (!*adv_items[sel]); break;
        case KEY_HOME: sel = 0; break;
        case KEY_END:  sel = N_ADV - 1; break;
        case KEY_ENTER: return (int)(adv_flags[sel] | os_flags[os_entry]);
        case KEY_ESC:  return -1;
        default: break;
        }
        if (sel != old) { animate_list(&adv_list, old, sel); draw_adv_desc(sel); }
    }
}

/* ------------------------------------------------------------------------- */
/*  Tools                                                                       */
/* ------------------------------------------------------------------------- */

static void progress_bar(int row, int percent)
{
    int x = TEXT_X, w = SCR_W - 2 * TEXT_X, y = ROW(row);
    draw_rect(x, y, w, CH, COL_BLACK);
    draw_rect_outline(x, y, w, CH, 2, COL_SILVER);
    draw_rect(x + 4, y + 4, (w - 8) * percent / 100, CH - 8, COL_SILVER);
    flush_rows(row, 1);
}

static void tool_memory_diagnostic(void)
{
    enum { CHUNK_FRAMES = 256, MAX_CHUNKS = 64 };           /* 1 MiB chunks, <= 64 MiB */
    static uint64_t chunks[MAX_CHUNKS];

    fb_clear(COL_BLACK);
    title_bar("Kestrel Memory Diagnostic Tool");
    text(1, 2, "Testing free physical memory with three patterns per 1 MiB block:", COL_WHITE);
    text(3, 3, "0x55AA55AA..., 0xAA55AA55..., and address-in-address.", COL_LIGHTGRAY);
    legend_bar(NULL, NULL, "ESC=Abort");
    fb_flush();

    uint64_t avail = pmm_free_bytes();
    uint64_t budget = avail > (24ull << 20) ? avail - (16ull << 20) : 0;   /* keep 16 MiB spare */
    int total = (int)(budget >> 20);
    if (total > MAX_CHUNKS) total = MAX_CHUNKS;

    int done = 0, errors = 0;
    uint64_t first_bad = 0, t0 = uptime_ms();
    bool aborted = false;
    char buf[96];

    for (int c = 0; c < total; c++) {
        struct key_event ev;
        if (keyboard_poll(&ev) && ev.key == KEY_ESC) { aborted = true; break; }

        uint64_t base = pmm_alloc_contig(CHUNK_FRAMES);
        if (!base) break;
        chunks[done++] = base;

        volatile uint64_t *p = (volatile uint64_t *)base;
        size_t words = CHUNK_FRAMES * PAGE_SIZE / 8;
        static const uint64_t pats[2] = { 0x55AA55AA55AA55AAull, 0xAA55AA55AA55AA55ull };
        for (int k = 0; k < 3; k++) {
            for (size_t i = 0; i < words; i++) p[i] = k < 2 ? pats[k] : (uint64_t)&p[i];
            for (size_t i = 0; i < words; i++) {
                uint64_t want = k < 2 ? pats[k] : (uint64_t)&p[i];
                if (p[i] != want) { if (!errors++) first_bad = (uint64_t)&p[i]; }
            }
        }

        int pct = (c + 1) * 100 / total;
        progress_bar(6, pct);
        clear_row(8);
        snprintf(buf, sizeof buf, "Block %d of %d at 0x%08lx    %d%% complete", c + 1, total, base, pct);
        text(4, 8, buf, COL_LIGHTGRAY);
        clear_row(10);
        text(4, 10, errors ? "Status: Hardware problems were detected." : "Status: No problems have been detected yet.",
             errors ? COL_RED : COL_LIGHTGRAY);
        flush_rows(8, 3);
    }
    for (int i = 0; i < done; i++) pmm_free_contig(chunks[i], CHUNK_FRAMES);

    uint64_t ms = uptime_ms() - t0;
    clear_row(10);
    if (total == 0) {
        text(4, 10, "Not enough free memory to run the test.", COL_YELLOW);
    } else if (errors) {
        snprintf(buf, sizeof buf, "%d memory errors detected; first failing address 0x%lx.", errors, first_bad);
        text(4, 10, buf, COL_RED);
    } else {
        text(4, 10, aborted ? "The test was cancelled. No memory errors were detected so far."
                            : "No memory errors were detected.", COL_WHITE);
    }
    snprintf(buf, sizeof buf, "Tested %d MiB in %lu ms (%lu MiB/s, 3 patterns).", done, ms,
             ms ? (uint64_t)done * 3 * 2 * 1000 / ms : 0);
    text(4, 12, buf, COL_LIGHTGRAY);
    text(4, 15, "Press any key to return to the boot manager.", COL_LIGHTGRAY);
    legend_bar(NULL, NULL, NULL);
    fb_flush();
    wait_any_key();
}

static void info_line(int row, const char *label, const char *value)
{
    text(2, row, label, COL_LIGHTGRAY);
    text(22, row, value, COL_WHITE);
}

static void tool_system_information(void)
{
    char v[96];
    fb_clear(COL_BLACK);
    title_bar("System Information");

    int r = 2;
    info_line(r++, "Firmware", g_boot.uefi ? "UEFI (frame buffer from GOP)" : "BIOS (frame buffer from VBE)");
    info_line(r++, "Boot loader", g_boot.loader[0] ? g_boot.loader : "(unknown)");
    snprintf(v, sizeof v, "%u x %u x %u bpp, pitch %u, LFB at 0x%lx",
             g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch, g_fb.phys);
    info_line(r++, "Video mode", v);
    info_line(r++, "Processor", g_boot.cpu_brand[0] ? g_boot.cpu_brand : g_boot.cpu_vendor);
    info_line(r++, "CPU vendor", g_boot.cpu_vendor);
    snprintf(v, sizeof v, "%lu MiB usable, %lu MiB free",
             pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);
    info_line(r++, "Memory", v);
    snprintf(v, sizeof v, "%p - %p (%lu KiB)", _kernel_start, _kernel_end,
             (uint64_t)(_kernel_end - _kernel_start) >> 10);
    info_line(r++, "Kernel image", v);
    snprintf(v, sizeof v, "%lu KiB arena, %lu KiB used", heap_size() >> 10, heap_used() >> 10);
    info_line(r++, "Kernel heap", v);
    snprintf(v, sizeof v, "PIT @ %d Hz, %lu timer IRQs, %lu keyboard IRQs",
             PIT_HZ, irq_count(IRQ_TIMER), irq_count(IRQ_KEYBOARD));
    info_line(r++, "Timer", v);
    info_line(r++, "Command line", g_boot.cmdline[0] ? g_boot.cmdline : "(empty)");
    snprintf(v, sizeof v, "%zu functions: %zu AHCI, %zu NVMe, %zu xHCI, %zu NVIDIA display",
             pci_device_count(), pci_match_count(PCI_MATCH_AHCI), pci_match_count(PCI_MATCH_NVME),
             pci_match_count(PCI_MATCH_XHCI), pci_match_count(PCI_MATCH_NVIDIA_DISPLAY));
    info_line(r++, "PCI devices", v);
    storage_summary(v, sizeof v);
    info_line(r++, "Storage", v);
    usb_summary(v, sizeof v);
    info_line(r++, "USB", v);

    r++;
    text(2, r++, "Physical memory map:", COL_WHITE);
    static const char *types[] = { "?", "usable", "reserved", "ACPI reclaimable", "ACPI NVS", "bad" };
    for (struct mb2_tag *t = mb2_first_tag(g_boot.mbi); t->type != MB2_TAG_END; t = mb2_next_tag(t)) {
        if (t->type != MB2_TAG_MMAP) continue;
        struct mb2_tag_mmap *mm = (struct mb2_tag_mmap *)t;
        for (uint8_t *p = (uint8_t *)mm->entries; p < (uint8_t *)t + t->size && r < LAST_ROW - 2;
             p += mm->entry_size) {
            struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
            snprintf(v, sizeof v, "%016lx - %016lx  %8lu KiB  %s", e->addr, e->addr + e->len - 1,
                     e->len >> 10, types[e->type <= 5 ? e->type : 0]);
            text(4, r++, v, COL_LIGHTGRAY);
        }
    }
    text(2, LAST_ROW - 1, "Press any key to return to the boot manager.", COL_LIGHTGRAY);
    legend_bar(NULL, NULL, NULL);
    fb_flush();
    wait_any_key();
}

static void tool_restart(void)
{
    fb_clear(COL_BLACK);
    text(1, 2, "Restarting...", COL_WHITE);
    fb_flush();
    pit_sleep_ms(300);
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);                       /* 8042 pulse reset line */
    cpu_halt_forever();
}

static void run_tool(int t)
{
    kprintf("bootmgr: running tool '%s'\n", tool_items[t]);
    switch (t) {
    case 0: tool_memory_diagnostic(); break;
    case 1: tool_system_information(); break;
    case 2: tool_restart(); break;
    }
}

/* ------------------------------------------------------------------------- */
/*  event loop                                                                  */
/* ------------------------------------------------------------------------- */

static struct boot_choice launch(int entry, uint32_t flags)
{
    kprintf("bootmgr: launching '%s' (flags 0x%x)\n", os_items[entry], flags);
    return (struct boot_choice){ entry, flags, os_items[entry] };
}

struct boot_choice bootmgr_run(void)
{
    enum focus focus = FOCUS_OS;
    int sel_os = 0, sel_tool = 0;
    bool counting = true;
    uint64_t deadline = uptime_ms() + BOOTMGR_TIMEOUT_SEC * 1000ull;
    int shown = BOOTMGR_TIMEOUT_SEC;

    keyboard_flush();
    draw_main(focus, sel_os, sel_tool, shown);

    for (;;) {
        if (counting) {
            uint64_t now = uptime_ms();
            if (now >= deadline) { draw_countdown(0); return launch(sel_os, os_flags[sel_os]); }
            int rem = (int)((deadline - now + 999) / 1000);
            if (rem != shown) { shown = rem; draw_countdown(rem); }
        }

        struct key_event ev;
        if (!keyboard_poll(&ev)) { hlt(); continue; }

        if (counting) {                     /* any key stops the countdown */
            counting = false;
            draw_countdown(-1);
            kprintf("bootmgr: countdown cancelled by key %s\n", keyboard_key_name(ev.key));
        }

        const struct list *l = focus == FOCUS_OS ? &os_list : &tools_list;
        int *sel = focus == FOCUS_OS ? &sel_os : &sel_tool;
        int old = *sel;

        switch (ev.key) {
        case KEY_UP:   if (*sel > 0) (*sel)--; break;
        case KEY_DOWN: if (*sel < l->n - 1) (*sel)++; break;
        case KEY_HOME: *sel = 0; break;
        case KEY_END:  *sel = l->n - 1; break;

        case KEY_TAB:
            focus = focus == FOCUS_OS ? FOCUS_TOOLS : FOCUS_OS;
            draw_list(&os_list, focus == FOCUS_OS ? sel_os : -1);
            draw_list(&tools_list, focus == FOCUS_TOOLS ? sel_tool : -1);
            draw_f8_hint(focus == FOCUS_OS);
            continue;

        case KEY_ESC:
            if (focus == FOCUS_TOOLS) {
                focus = FOCUS_OS;
                draw_list(&os_list, sel_os);
                draw_list(&tools_list, -1);
                draw_f8_hint(true);
            }
            continue;

        case KEY_F8:
            if (focus == FOCUS_OS) {
                int fl = advanced_options(sel_os);
                if (fl >= 0) return launch(sel_os, (uint32_t)fl);
                draw_main(focus, sel_os, sel_tool, -1);
            }
            continue;

        case KEY_ENTER:
            if (focus == FOCUS_OS) return launch(sel_os, os_flags[sel_os]);
            run_tool(sel_tool);
            draw_main(focus, sel_os, sel_tool, -1);
            continue;

        default:
            continue;
        }
        if (*sel != old) animate_list(l, old, *sel);
    }
}
