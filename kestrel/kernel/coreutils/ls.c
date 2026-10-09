/* kernel/coreutils/ls.c -- ls [-a] [-l] [-1] [PATH...] */
#include "cu.h"
#include <kernel/string.h>
#include <kernel/mm.h>

#define LS_MAX_ENTRIES 1024

struct ls_entry { char name[64]; struct stat st; bool have_st; };
struct ls_opts  { bool all, longfmt, one; };

static void mode_string(uint32_t m, char out[11])
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISREG(m) ? '-' : '?';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400u >> i)) ? rwx[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}

/* directories blue, devices yellow, executables green (terminal only) */
static const char *colour(struct cu_io *io, uint32_t mode)
{
    if (S_ISDIR(mode)) return cu_col(io, CU_BLUE);
    if (S_ISCHR(mode)) return cu_col(io, CU_YELLOW);
    if (S_ISREG(mode) && (mode & 0111)) return cu_col(io, CU_GREEN);
    return "";
}

static void long_line(struct cu_io *io, const struct stat *st, const char *shown, int64_t now)
{
    char mode[11], size[24], owner[24], group[24], when[24];
    mode_string(st->st_mode, mode);
    if (S_ISCHR(st->st_mode)) snprintf(size, sizeof size, "%lu, %lu", st->st_rdev >> 8, st->st_rdev & 0xFF);
    else snprintf(size, sizeof size, "%ld", st->st_size);
    cu_owner_name(st->st_uid, owner, sizeof owner);
    cu_group_name(st->st_gid, group, sizeof group);
    struct cu_tm tm;
    int64_t t = st->st_mtim.tv_sec;
    cu_gmtime(t, &tm);
    bool recent = t <= now + 3600 && now - t < 182LL * 86400;    /* ls(1): older than ~6 months shows the year */
    cu_strftime(when, sizeof when, recent ? "%b %e %H:%M" : "%b %e  %Y", &tm, t);
    cu_printf(io, "%s %2lu %-6s %-6s %10s %s %s%s%s%s\n", mode, st->st_nlink, owner, group, size, when,
              colour(io, st->st_mode), shown, cu_col(io, CU_RESET), S_ISDIR(st->st_mode) ? "/" : "");
}

static int list_dir(struct cu_io *io, const char *path, const struct ls_opts *o, int64_t now)
{
    int fd = u_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        char what[CU_PATH_MAX + 32];
        snprintf(what, sizeof what, "cannot open directory '%s'", path);
        return cu_fail(io, "ls", what, fd);
    }
    struct ls_entry *ents = kmalloc(sizeof *ents * LS_MAX_ENTRIES);
    char *buf = kmalloc(2048);
    if (!ents || !buf) { kfree(ents); kfree(buf); u_close(fd); return cu_fail(io, "ls", path, -ENOMEM); }
    int n = 0, got;
    while (n < LS_MAX_ENTRIES && (got = u_getdents64(fd, buf, 2048)) > 0)
        for (int off = 0; off < got && n < LS_MAX_ENTRIES; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (d->d_name[0] == '.' && !o->all) continue;
            strlcpy(ents[n].name, d->d_name, sizeof ents[n].name);
            char full[CU_PATH_MAX];
            ents[n].have_st = cu_join(full, sizeof full, path, d->d_name) == 0 && u_stat(full, &ents[n].st) == 0;
            if (!ents[n].have_st) {
                memset(&ents[n].st, 0, sizeof ents[n].st);
                ents[n].st.st_mode = d->d_type == DT_DIR ? S_IFDIR : d->d_type == DT_CHR ? S_IFCHR : S_IFREG;
            }
            n++;
        }
    u_close(fd);
    kfree(buf);

    for (int i = 1; i < n; i++)                         /* sort by name */
        for (int j = i; j > 0 && strcmp(ents[j].name, ents[j - 1].name) < 0; j--) {
            struct ls_entry t = ents[j]; ents[j] = ents[j - 1]; ents[j - 1] = t;
        }

    if (o->longfmt) {
        for (int i = 0; i < n; i++) {
            if (ents[i].have_st) long_line(io, &ents[i].st, ents[i].name, now);
            else cu_printf(io, "?????????? %s\n", ents[i].name);
        }
    } else if (n) {
        /* columns filled top to bottom like ls(1), sized to the terminal width;
         * one name per line when the output is not a terminal */
        uint16_t ws[4] = { 0 };
        int term_cols = (!o->one && u_ioctl(io->out, TIOCGWINSZ, ws) == 0 && ws[1]) ? ws[1] : 0;
        size_t width = 1;
        for (int i = 0; i < n; i++) {
            size_t l = strlen(ents[i].name) + S_ISDIR(ents[i].st.st_mode);
            if (l > width) width = l;
        }
        width += 2;
        int cols = term_cols ? (int)((size_t)term_cols / width) : 1;
        if (cols < 1) cols = 1;
        int rows = (n + cols - 1) / cols;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int i = c * rows + r;
                if (i >= n) break;
                bool dir = S_ISDIR(ents[i].st.st_mode);
                size_t len = strlen(ents[i].name) + dir;
                cu_printf(io, "%s%s%s%s", colour(io, ents[i].st.st_mode), ents[i].name, cu_col(io, CU_RESET), dir ? "/" : "");
                bool last = c == cols - 1 || (c + 1) * rows + r >= n;
                for (size_t s = len; !last && s < width; s++) cu_puts(io, " ");
            }
            cu_puts(io, "\n");
        }
    }
    if (n == LS_MAX_ENTRIES) cu_eprintf(io, "ls: listing of '%s' truncated at %d entries\n", path, n);
    kfree(ents);
    return 0;
}

int ls_main(struct cu_io *io, int argc, char **argv)
{
    struct ls_opts o = { false, false, false };
    const char *paths[32];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'a') o.all = true;
                else if (*p == 'l') o.longfmt = true;
                else if (*p == '1') o.one = true;
                else { cu_eprintf(io, "ls: invalid option -- '%c'\nusage: ls [-a] [-l] [-1] [PATH...]\n", *p); return 2; }
            }
        } else if (np < 32) {
            paths[np++] = argv[i];
        }
    }
    if (!np) paths[np++] = ".";
    struct timespec ts;
    int64_t now = u_clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : 0;

    int status = 0;
    for (int i = 0; i < np; i++) {                      /* files first, like ls(1) */
        struct stat st;
        int rc = u_stat(paths[i], &st);
        if (rc < 0) {
            char what[CU_PATH_MAX + 24];
            snprintf(what, sizeof what, "cannot access '%s'", paths[i]);
            status = 2;
            cu_fail(io, "ls", what, rc);
            continue;
        }
        if (S_ISDIR(st.st_mode)) continue;
        if (o.longfmt) long_line(io, &st, paths[i], now);
        else cu_printf(io, "%s%s%s\n", colour(io, st.st_mode), paths[i], cu_col(io, CU_RESET));
    }
    bool printed = false;
    for (int i = 0; i < np; i++) {
        if (!cu_is_dir(paths[i])) continue;
        if (np > 1) cu_printf(io, "%s%s:\n", printed ? "\n" : "", paths[i]);
        printed = true;
        status |= list_dir(io, paths[i], &o, now);
    }
    return status;
}
