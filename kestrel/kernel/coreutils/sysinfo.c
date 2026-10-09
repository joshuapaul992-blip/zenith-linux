/* kernel/coreutils/sysinfo.c -- date, uname */
#include "cu.h"
#include <kernel/string.h>

/* ---- date [+FORMAT] ----------------------------------------------------------
 * clock_gettime(CLOCK_REALTIME): the CMOS RTC read at boot, advanced by the
 * HPET (or calibrated TSC). Kestrel keeps UTC; there is no time zone data. */
int date_main(struct cu_io *io, int argc, char **argv)
{
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') fmt = argv[i] + 1;
        else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) continue;
        else { cu_eprintf(io, "usage: date [-u] [+FORMAT]   (%%Y %%m %%d %%H %%M %%S %%a %%b %%e %%j %%s %%F %%T %%Z)\n"); return 2; }
    }
    struct timespec ts;
    int rc = u_clock_gettime(CLOCK_REALTIME, &ts);
    if (rc < 0) return cu_fail(io, "date", NULL, rc);
    struct cu_tm tm;
    cu_gmtime(ts.tv_sec, &tm);
    char out[256];
    cu_strftime(out, sizeof out, fmt, &tm, ts.tv_sec);
    cu_printf(io, "%s\n", out);
    return 0;
}

/* ---- uname [-asnrvm] -----------------------------------------------------------
 * Without options: the labelled block. With options: the classic one-line
 * form in POSIX field order. */
int uname_main(struct cu_io *io, int argc, char **argv)
{
    enum { S = 1, N = 2, R = 4, V = 8, M = 16 };
    int want = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) goto usage;
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'a': want |= S | N | R | V | M; break;
            case 's': want |= S; break;
            case 'n': want |= N; break;
            case 'r': want |= R; break;
            case 'v': want |= V; break;
            case 'm': want |= M; break;
            default: goto usage;
            }
        }
    }
    struct utsname u;
    int rc = u_uname(&u);
    if (rc < 0) return cu_fail(io, "uname", NULL, rc);
    if (!want) {
        cu_printf(io, "Sysname: %s\nNodename: %s\nRelease: %s\nVersion: %s\nMachine: %s\n",
                  u.sysname, u.nodename, u.release, u.version, u.machine);
        return 0;
    }
    const char *fields[5] = { u.sysname, u.nodename, u.release, u.version, u.machine };
    bool first = true;
    for (int b = 0; b < 5; b++) {
        if (!(want & (1 << b))) continue;
        cu_printf(io, "%s%s", first ? "" : " ", fields[b]);
        first = false;
    }
    cu_puts(io, "\n");
    return 0;
usage:
    cu_eprintf(io, "usage: uname [-asnrvm]\n");
    return 2;
}
