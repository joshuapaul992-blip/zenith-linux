/* kernel/coreutils/text.c -- cat, head, tail, grep, echo */
#include "cu.h"
#include <kernel/string.h>
#include <kernel/mm.h>

#define LINE_MAX   4096
#define TAIL_MAX   (1u << 20)               /* stdin/pipe fallback buffer cap */

/* Open FILE, or use stdin for no name / "-". Returns fd or -errno. */
static int open_input(struct cu_io *io, const char *name)
{
    if (!name || !strcmp(name, "-")) return io->in;
    struct stat st;
    int rc = u_stat(name, &st);
    if (rc < 0) return rc;
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    return u_open(name, O_RDONLY, 0);
}

static void close_input(struct cu_io *io, int fd) { if (fd != io->in) u_close(fd); }

/* ---- line reader over a file descriptor ----------------------------------- */
struct lines {
    int    fd;
    char  *buf;                 /* CU_BUF raw bytes  */
    size_t len, pos;
    bool   eof;
    int    error;
};

/* Next line into `out` (without '\n'); lines longer than cap-1 are split.
 * Returns length, or -1 at end of input. *nl says whether '\n' ended it. */
static long next_line(struct lines *l, char *out, size_t cap, bool *nl)
{
    size_t n = 0;
    *nl = false;
    for (;;) {
        if (l->pos == l->len) {
            if (l->eof) return n ? (long)n : -1;
            ssize_t r = u_read(l->fd, l->buf, CU_BUF);
            if (r < 0) { l->error = (int)r; l->eof = true; continue; }
            if (r == 0) { l->eof = true; continue; }
            l->len = (size_t)r;
            l->pos = 0;
        }
        char c = l->buf[l->pos++];
        if (c == '\n') { *nl = true; out[n] = 0; return (long)n; }
        out[n++] = c;
        if (n == cap - 1) { out[n] = 0; return (long)n; }
    }
}

/* ======================================================================== */
/*  cat [-n] [FILE...]                                                         */
/* ======================================================================== */
int cat_main(struct cu_io *io, int argc, char **argv)
{
    bool number = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n")) { number = true; continue; }
        if (!strcmp(argv[i], "--")) { i++; break; }
        cu_eprintf(io, "usage: cat [-n] [FILE...]\n");
        return 2;
    }
    char *buf = kmalloc(CU_BUF);
    if (!buf) return cu_fail(io, "cat", NULL, -ENOMEM);
    int status = 0;
    long lineno = 1;
    bool at_bol = true;
    for (int f = i; f < argc || f == i; f++) {
        const char *name = f < argc ? argv[f] : NULL;
        int fd = open_input(io, name);
        if (fd < 0) { status |= cu_fail(io, "cat", name, fd); if (f >= argc) break; continue; }
        ssize_t n;
        while ((n = u_read(fd, buf, CU_BUF)) > 0) {
            if (!number) { cu_write(io->out, buf, (size_t)n); continue; }
            for (ssize_t k = 0; k < n; k++) {
                if (at_bol) cu_printf(io, "%6ld\t", lineno++);
                cu_write(io->out, &buf[k], 1);
                at_bol = buf[k] == '\n';
            }
        }
        if (n < 0) status |= cu_fail(io, "cat", name ? name : "-", (int)n);
        close_input(io, fd);
        if (f >= argc) break;
    }
    kfree(buf);
    return status;
}

/* ======================================================================== */
/*  head / tail option parsing: -n N, -N                                       */
/* ======================================================================== */
static int count_opts(struct cu_io *io, const char *prog, int argc, char **argv, long *count)
{
    int i = 1;
    *count = 10;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        const char *v = NULL;
        if (!strcmp(argv[i], "-n")) {
            if (i + 1 >= argc) { cu_eprintf(io, "%s: option requires an argument -- 'n'\n", prog); return -1; }
            v = argv[++i];
        } else if (!strncmp(argv[i], "-n", 2)) {
            v = argv[i] + 2;
        } else {
            v = argv[i] + 1;                            /* -5 */
        }
        if (!cu_parse_count(v, count)) {
            cu_eprintf(io, "%s: invalid number of lines: '%s'\nusage: %s [-n N] [FILE...]\n", prog, v, prog);
            return -1;
        }
    }
    return i;
}

/* ======================================================================== */
/*  head [-n N] [FILE...]                                                      */
/* ======================================================================== */
static int head_fd(struct cu_io *io, int fd, long count, char *buf)
{
    long lines = 0;
    ssize_t n;
    while (lines < count && (n = u_read(fd, buf, CU_BUF)) > 0) {
        ssize_t end = 0;
        while (end < n && lines < count) if (buf[end++] == '\n') lines++;
        cu_write(io->out, buf, (size_t)end);
    }
    return 0;
}

int head_main(struct cu_io *io, int argc, char **argv)
{
    long count;
    int i = count_opts(io, "head", argc, argv, &count);
    if (i < 0) return 2;
    char *buf = kmalloc(CU_BUF);
    if (!buf) return cu_fail(io, "head", NULL, -ENOMEM);
    int status = 0, files = argc - i;
    for (int f = i; f < argc || f == i; f++) {
        const char *name = f < argc ? argv[f] : NULL;
        int fd = open_input(io, name);
        if (fd < 0) { status |= cu_fail(io, "head", name, fd); if (f >= argc) break; continue; }
        if (files > 1) cu_printf(io, "%s==> %s <==\n", f > i ? "\n" : "", name);
        head_fd(io, fd, count, buf);
        close_input(io, fd);
        if (f >= argc) break;
    }
    kfree(buf);
    return status;
}

/* ======================================================================== */
/*  tail [-n N] [FILE...]                                                      */
/* ======================================================================== */
/* Seekable input: walk backwards from the end in 4 KiB blocks counting
 * newlines, then copy from that offset. A final newline does not start a
 * new (empty) line. */
static int tail_seek(struct cu_io *io, int fd, long count, int64_t size, char *buf)
{
    int64_t pos = size, start = 0;
    long seen = 0;
    bool found = false, first = true;
    while (pos > 0 && !found) {
        int64_t chunk = pos < CU_BUF ? pos : CU_BUF;
        pos -= chunk;
        if (u_lseek(fd, pos, SEEK_SET) < 0) return -ESPIPE;
        ssize_t n = u_read(fd, buf, (size_t)chunk);
        if (n != chunk) return n < 0 ? (int)n : -EIO;
        for (int64_t k = chunk - 1; k >= 0; k--) {
            if (buf[k] != '\n') { first = false; continue; }
            if (first) { first = false; continue; }     /* trailing newline */
            if (++seen == count) { start = pos + k + 1; found = true; break; }
        }
    }
    if (count == 0) start = size;
    if (u_lseek(fd, start, SEEK_SET) < 0) return -ESPIPE;
    ssize_t n;
    while ((n = u_read(fd, buf, CU_BUF)) > 0) cu_write(io->out, buf, (size_t)n);
    return 0;
}

/* Non-seekable input (terminal): buffer it all (up to 1 MiB), then print
 * the last lines. */
static int tail_stream(struct cu_io *io, int fd, long count, char *buf)
{
    size_t cap = 16384, len = 0;
    char *all = kmalloc(cap);
    if (!all) return -ENOMEM;
    ssize_t n;
    while ((n = u_read(fd, buf, CU_BUF)) > 0) {
        if (len + (size_t)n > cap) {
            if (cap >= TAIL_MAX) {                      /* keep the newest half */
                memmove(all, all + cap / 2, len - cap / 2);
                len -= cap / 2;
            } else {
                char *bigger = krealloc(all, cap * 2);
                if (!bigger) { kfree(all); return -ENOMEM; }
                all = bigger;
                cap *= 2;
            }
        }
        memcpy(all + len, buf, (size_t)n);
        len += (size_t)n;
    }
    size_t start = len;
    long seen = 0;
    for (size_t k = len; k > 0 && count; k--) {
        if (all[k - 1] != '\n' || k == len) continue;
        if (++seen == count) { start = k; break; }
    }
    if (seen < count && count) start = 0;
    cu_write(io->out, all + start, len - start);
    kfree(all);
    return 0;
}

int tail_main(struct cu_io *io, int argc, char **argv)
{
    long count;
    int i = count_opts(io, "tail", argc, argv, &count);
    if (i < 0) return 2;
    char *buf = kmalloc(CU_BUF);
    if (!buf) return cu_fail(io, "tail", NULL, -ENOMEM);
    int status = 0, files = argc - i;
    for (int f = i; f < argc || f == i; f++) {
        const char *name = f < argc ? argv[f] : NULL;
        int fd = open_input(io, name);
        if (fd < 0) { status |= cu_fail(io, "tail", name, fd); if (f >= argc) break; continue; }
        if (files > 1) cu_printf(io, "%s==> %s <==\n", f > i ? "\n" : "", name);
        struct stat st;
        int rc;
        if (u_fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            rc = tail_seek(io, fd, count, st.st_size, buf);
        else
            rc = tail_stream(io, fd, count, buf);
        if (rc < 0) status |= cu_fail(io, "tail", name ? name : "-", rc);
        close_input(io, fd);
        if (f >= argc) break;
    }
    kfree(buf);
    return status;
}

/* ======================================================================== */
/*  grep [-i] [-n] [-v] [-c] [-q] PATTERN [FILE...]                            */
/* ======================================================================== */
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Fixed-string search; returns the match offset or -1. */
static long find(const char *hay, size_t hlen, const char *needle, size_t nlen, bool icase)
{
    if (nlen == 0) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        while (k < nlen && (icase ? lower(hay[i + k]) == lower(needle[k]) : hay[i + k] == needle[k])) k++;
        if (k == nlen) return (long)i;
    }
    return -1;
}

struct grep_opts { bool icase, number, invert, count, quiet, prefix; const char *pattern; size_t plen; };

static void print_match(struct cu_io *io, const struct grep_opts *o, const char *name, long lineno,
                        const char *line, size_t len)
{
    const char *red = cu_col(io, "\033[91m"), *mag = cu_col(io, "\033[95m"),
               *grn = cu_col(io, "\033[92m"), *rst = cu_col(io, CU_RESET);
    if (o->prefix) cu_printf(io, "%s%s%s:", mag, name, rst);
    if (o->number) cu_printf(io, "%s%ld%s:", grn, lineno, rst);
    if (o->invert || !*red || !o->plen) { cu_write(io->out, line, len); cu_puts(io, "\n"); return; }
    size_t pos = 0;                                     /* highlight every occurrence */
    for (;;) {
        long m = find(line + pos, len - pos, o->pattern, o->plen, o->icase);
        if (m < 0) break;
        cu_write(io->out, line + pos, (size_t)m);
        cu_puts(io, red);
        cu_write(io->out, line + pos + m, o->plen);
        cu_puts(io, rst);
        pos += (size_t)m + o->plen;
    }
    cu_write(io->out, line + pos, len - pos);
    cu_puts(io, "\n");
}

int grep_main(struct cu_io *io, int argc, char **argv)
{
    struct grep_opts o = { 0 };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'i': o.icase = true; break;
            case 'n': o.number = true; break;
            case 'v': o.invert = true; break;
            case 'c': o.count = true; break;
            case 'q': o.quiet = true; break;
            default:
                cu_eprintf(io, "grep: invalid option -- '%c'\nusage: grep [-i] [-n] [-v] [-c] [-q] PATTERN [FILE...]\n", *p);
                return 2;
            }
        }
    }
    if (i >= argc) { cu_eprintf(io, "usage: grep [-i] [-n] [-v] [-c] [-q] PATTERN [FILE...]\n"); return 2; }
    o.pattern = argv[i++];
    o.plen = strlen(o.pattern);
    o.prefix = argc - i > 1;

    char *raw = kmalloc(CU_BUF), *line = kmalloc(LINE_MAX);
    if (!raw || !line) { kfree(raw); kfree(line); return cu_fail(io, "grep", NULL, -ENOMEM); }
    bool any = false, error = false;
    for (int f = i; f < argc || f == i; f++) {
        const char *name = f < argc ? argv[f] : "(standard input)";
        int fd = open_input(io, f < argc ? argv[f] : NULL);
        if (fd < 0) { cu_fail(io, "grep", name, fd); error = true; if (f >= argc) break; continue; }
        struct lines l = { .fd = fd, .buf = raw };
        long lineno = 0, matches = 0, len;
        bool nl;
        while ((len = next_line(&l, line, LINE_MAX, &nl)) >= 0) {
            lineno++;
            bool hit = find(line, (size_t)len, o.pattern, o.plen, o.icase) >= 0;
            if (hit == o.invert) continue;
            matches++;
            any = true;
            if (o.quiet) break;
            if (!o.count) print_match(io, &o, name, lineno, line, (size_t)len);
        }
        if (l.error) { cu_fail(io, "grep", name, l.error); error = true; }
        if (o.count && !o.quiet) {
            if (o.prefix) cu_printf(io, "%s:", name);
            cu_printf(io, "%ld\n", matches);
        }
        close_input(io, fd);
        if (o.quiet && any) break;
        if (f >= argc) break;
    }
    kfree(raw);
    kfree(line);
    return error && !(o.quiet && any) ? 2 : any ? 0 : 1;
}

/* ======================================================================== */
/*  echo [-n] [-e|-E] [ARG...]                                                 */
/*  Escapes are on by default (XSI): \n \t \\ \a \b \e \f \r \v \0NNN \c       */
/* ======================================================================== */
int echo_main(struct cu_io *io, int argc, char **argv)
{
    bool newline = true, escapes = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool ok = true;
        for (const char *p = argv[i] + 1; *p; p++) if (*p != 'n' && *p != 'e' && *p != 'E') ok = false;
        if (!ok) break;                                 /* "-x" is just text */
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n') newline = false;
            else if (*p == 'e') escapes = true;
            else escapes = false;
        }
    }
    char out[1024];
    size_t n = 0;
#define EMIT(ch) do { if (n == sizeof out) { cu_write(io->out, out, n); n = 0; } out[n++] = (char)(ch); } while (0)
    for (int a = i; a < argc; a++) {
        if (a > i) EMIT(' ');
        for (const char *p = argv[a]; *p; p++) {
            if (!escapes || *p != '\\' || !p[1]) { EMIT(*p); continue; }
            switch (*++p) {
            case 'n': EMIT('\n'); break;
            case 't': EMIT('\t'); break;
            case '\\': EMIT('\\'); break;
            case 'a': EMIT('\a'); break;
            case 'b': EMIT('\b'); break;
            case 'e': EMIT(0x1B); break;
            case 'f': EMIT('\f'); break;
            case 'r': EMIT('\r'); break;
            case 'v': EMIT('\v'); break;
            case 'c': cu_write(io->out, out, n); return 0;  /* stop, no newline */
            case '0': {
                int v = 0;
                for (int d = 0; d < 3 && p[1] >= '0' && p[1] <= '7'; d++) v = v * 8 + (*++p - '0');
                EMIT(v);
                break;
            }
            default: EMIT('\\'); EMIT(*p); break;
            }
        }
    }
    if (newline) EMIT('\n');
#undef EMIT
    cu_write(io->out, out, n);
    return 0;
}
