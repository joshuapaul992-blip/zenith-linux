/* kernel/coreutils/neofetch.c -- system summary beside the Kestrel logo
 *
 * Layout on the 128-column terminal (1-based columns):
 *   col 1        blank margin
 *   cols 2-28    the falcon (27 columns), silver
 *   cols 29-31   gutter
 *   cols 32-127  the information (at most 96 visible characters, so a line
 *                never reaches column 128, where the terminal would wrap)
 * Every value is read at run time: uname(2), getuid(2), /etc/passwd,
 * /etc/os-release, /proc/uptime, /proc/meminfo, /sys/class/graphics/fb0
 * and the CPUID brand-string leaves. */
#include "cu.h"
#include <kernel/string.h>
#include <kernel/cpu.h>

#define ART_W     27
#define INFO_W    96
#define MAX_LINES 16

static const char *const falcon[] = {
    "\\`-._                 _.-'/",
    " \\\\  `-._         _.-'  // ",
    "  \\\\\\    `-. _ .-'    ///  ",
    "   `\\\\\\.   (o o)   .///'   ",
    "     `\\\\\\\\._\\v/_.////'     ",
    "        `-.|   |.-'        ",
    "          |  :  |          ",
    "          | ::: |          ",
    "           \\:::/           ",
    "            |:|            ",
    "           /|:|\\           ",
    "          /_/ \\_\\          ",
};
#define FALCON_ROWS ((int)(sizeof falcon / sizeof *falcon))

struct info { char text[160]; int visible; };      /* text may hold colour codes */

static void add(struct info *lines, int *n, const char *label, const char *value, bool tty)
{
    if (*n >= MAX_LINES) return;
    struct info *l = &lines[(*n)++];
    int room = INFO_W - (int)strlen(label) - 2;
    if (room < 0) room = 0;
    char val[128];
    strlcpy(val, value, sizeof val);
    if ((int)strlen(val) > room) val[room] = 0;         /* never wrap past column 127 */
    snprintf(l->text, sizeof l->text, "%s%s%s: %s", tty ? "\033[92m" : "", label, tty ? CU_RESET : "", val);
    l->visible = (int)(strlen(label) + 2 + strlen(val));
}

static void add_raw(struct info *lines, int *n, const char *text, int visible)
{
    if (*n >= MAX_LINES) return;
    strlcpy(lines[*n].text, text, sizeof lines[*n].text);
    lines[(*n)++].visible = visible;
}

/* "key:   value" from a /proc-style file; value copied up to the line end. */
static bool field(const char *text, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, key, k)) continue;
        p += k;
        while (*p == ' ' || *p == '\t' || *p == ':' || *p == '=' || *p == '"') p++;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && p[n] != '"' && n + 1 < cap) { out[n] = p[n]; n++; }
        out[n] = 0;
        return true;
    }
    return false;
}

static void cpu_model(char *out, size_t cap)
{
    uint32_t regs[12], max, b, c, d;
    cpuid(0x80000000, 0, &max, &b, &c, &d);
    if (max < 0x80000004) { strlcpy(out, "unknown x86_64 CPU", cap); return; }
    for (uint32_t leaf = 0; leaf < 3; leaf++)               /* brand string: 3 x 16 bytes */
        cpuid(0x80000002 + leaf, 0, &regs[leaf * 4], &regs[leaf * 4 + 1], &regs[leaf * 4 + 2], &regs[leaf * 4 + 3]);
    char brand[49];
    memcpy(brand, regs, 48);
    brand[48] = 0;
    char *s = brand;
    while (*s == ' ') s++;
    size_t n = 0;                                           /* collapse runs of spaces */
    for (; *s && n + 1 < cap; s++) if (*s != ' ' || (n && out[n - 1] != ' ')) out[n++] = *s;
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void format_uptime(uint64_t secs, char *out, size_t cap)
{
    uint64_t d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60;
    size_t n = 0;
    if (d) n += (size_t)snprintf(out + n, cap - n, "%lu day%s, ", d, d == 1 ? "" : "s");
    if (d || h) n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%lu hour%s, ", h, h == 1 ? "" : "s");
    snprintf(out + n, n < cap ? cap - n : 0, "%lu min%s", m, m == 1 ? "" : "s");
}

int neofetch_main(struct cu_io *io, int argc, char **argv)
{
    (void)argv;
    if (argc > 1) { cu_eprintf(io, "usage: neofetch\n"); return 2; }
    bool tty = cu_isatty(io->out);
    struct info lines[MAX_LINES];
    int n = 0;
    char buf[1024], val[128], tmp[64];

    struct utsname u;
    if (u_uname(&u) < 0) { strlcpy(u.sysname, "Kestrel", 65); strlcpy(u.nodename, "kestrel", 65); u.release[0] = u.machine[0] = 0; }

    /* root@kestrel-pc, then a dark grey rule of the same length */
    char user[32];
    cu_owner_name(u_getuid(), user, sizeof user);
    snprintf(val, sizeof val, "%s@%s", user, u.nodename);
    int len = (int)strlen(val);
    if (len > INFO_W) len = INFO_W;
    snprintf(buf, sizeof buf, "%s%.*s%s", tty ? "\033[92m" : "", len, val, tty ? CU_RESET : "");
    add_raw(lines, &n, buf, len);
    char rule[INFO_W + 1];
    memset(rule, '-', (size_t)len);
    rule[len] = 0;
    snprintf(buf, sizeof buf, "%s%s%s", tty ? CU_DIM : "", rule, tty ? CU_RESET : "");
    add_raw(lines, &n, buf, len);

    if (cu_read_small("/etc/os-release", buf, sizeof buf) > 0 && field(buf, "PRETTY_NAME", val, sizeof val))
        add(lines, &n, "OS", val, tty);
    else {
        snprintf(val, sizeof val, "%s OS %s", u.sysname, u.machine);
        add(lines, &n, "OS", val, tty);
    }
    snprintf(val, sizeof val, "%s %s", u.sysname, u.release);
    add(lines, &n, "Kernel", val, tty);

    uint64_t secs = 0;
    if (cu_read_small("/proc/uptime", tmp, sizeof tmp) > 0) {
        char *dot = strchr(tmp, '.');
        if (dot) *dot = 0;
        cu_parse_u64(tmp, 10, &secs);
    }
    format_uptime(secs, val, sizeof val);
    add(lines, &n, "Uptime", val, tty);

    add(lines, &n, "Shell", "kush (Kestrel Native)", tty);

    if (cu_read_small("/sys/class/graphics/fb0/virtual_size", tmp, sizeof tmp) > 0) {
        char *comma = strchr(tmp, ','), *nl = strchr(tmp, '\n');
        if (nl) *nl = 0;
        if (comma) *comma = 'x';
        uint16_t ws[4] = { 0 };
        if (tty && u_ioctl(io->out, TIOCGWINSZ, ws) == 0)
            snprintf(val, sizeof val, "%s (%ux%u text)", tmp, ws[1], ws[0]);
        else
            strlcpy(val, tmp, sizeof val);
        add(lines, &n, "Resolution", val, tty);
    }
    if (cu_read_small("/sys/class/graphics/fb0/name", tmp, sizeof tmp) > 0) {
        char *nl = strchr(tmp, '\n');
        if (nl) *nl = 0;
        const char *desc = !strcmp(tmp, "efi-gop") ? "UEFI GOP linear framebuffer"
                         : !strcmp(tmp, "vesa-vbe") ? "VESA VBE linear framebuffer" : tmp;
        snprintf(val, sizeof val, "%s (%s)", desc, tmp);
        add(lines, &n, "Display Driver", val, tty);
    }

    cpu_model(val, sizeof val);
    add(lines, &n, "CPU", val, tty);

    if (cu_read_small("/proc/meminfo", buf, sizeof buf) > 0) {
        uint64_t total = 0, free = 0;
        if (field(buf, "MemTotal", tmp, sizeof tmp)) { char *k = strchr(tmp, ' '); if (k) *k = 0; cu_parse_u64(tmp, 10, &total); }
        if (field(buf, "MemFree", tmp, sizeof tmp))  { char *k = strchr(tmp, ' '); if (k) *k = 0; cu_parse_u64(tmp, 10, &free); }
        snprintf(val, sizeof val, "%luMiB / %luMiB", (total - free) >> 10, total >> 10);
        add(lines, &n, "Memory", val, tty);
    }

    /* colour blocks: the 8 normal and 8 bright ANSI colours, 3 cells each */
    if (tty) {
        add_raw(lines, &n, "", 0);
        char strip[256];
        for (int row = 0; row < 2; row++) {
            size_t k = 0;
            for (int c = 0; c < 8; c++)
                k += (size_t)snprintf(strip + k, sizeof strip - k, "\033[%dm   ", (row ? 100 : 40) + c);
            snprintf(strip + k, sizeof strip - k, CU_RESET);
            add_raw(lines, &n, strip, 24);
        }
    }

    /* side by side: margin, falcon (silver), gutter, information */
    int rows = n > FALCON_ROWS ? n : FALCON_ROWS;
    cu_puts(io, "\n");
    for (int r = 0; r < rows; r++) {
        const char *art = r < FALCON_ROWS ? falcon[r] : "";
        cu_printf(io, " %s%-*s%s   %s\n", tty ? CU_SILVER : "", ART_W, art, tty ? CU_RESET : "", r < n ? lines[r].text : "");
    }
    cu_puts(io, "\n");
    return 0;
}
