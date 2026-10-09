/* kernel/coreutils/cu.h -- shared definitions for the Kestrel core utilities
 *
 * Each utility is `int name_main(struct cu_io *io, int argc, char **argv)`.
 * Utilities talk to the kernel only through the POSIX system-call gate
 * (usyscall.h: open, read, write, close, stat, mkdir, rmdir, unlink,
 * rename, chmod, chown, utimensat, getcwd, chdir, uname, clock_gettime...),
 * read standard input from io->in and write to io->out / io->err, so the
 * shell can redirect them. Exit status: 0 success, 1 failure, 2 usage
 * error (grep: 0 match, 1 no match, 2 error), as in POSIX tools. */
#ifndef KESTREL_COREUTILS_H
#define KESTREL_COREUTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/usyscall.h>

#define CU_BUF      4096            /* I/O chunk size */
#define CU_PATH_MAX 256             /* = VFS_PATH_MAX */

struct cu_io {
    int in, out, err;
};

typedef int (*cu_main_fn)(struct cu_io *io, int argc, char **argv);

struct cu_cmd {
    const char *name, *usage, *help;
    cu_main_fn  fn;
};

extern const struct cu_cmd cu_commands[];
const struct cu_cmd *cu_find(const char *name);

/* ---- output ---------------------------------------------------------------- */
int  cu_write(int fd, const void *buf, size_t len);         /* all bytes, or -errno */
void cu_puts(struct cu_io *io, const char *s);
void cu_printf(struct cu_io *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void cu_eprintf(struct cu_io *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool cu_isatty(int fd);
/* "prog: what: message" on stderr; returns 1 */
int  cu_fail(struct cu_io *io, const char *prog, const char *what, int err);
const char *cu_strerror(int err);                           /* err > 0 or < 0 */

/* colours, emitted only when the output is the terminal */
const char *cu_col(struct cu_io *io, const char *ansi);
#define CU_RESET   "\033[0m"
#define CU_BOLD    "\033[97m"
#define CU_DIM     "\033[90m"
#define CU_RED     "\033[91m"
#define CU_GREEN   "\033[92m"
#define CU_YELLOW  "\033[93m"
#define CU_BLUE    "\033[94m"
#define CU_SILVER  "\033[37m"

/* ---- paths ------------------------------------------------------------------- */
int  cu_join(char *out, size_t cap, const char *dir, const char *name);   /* 0 or -ENAMETOOLONG */
void cu_basename(const char *path, char *out, size_t cap);                /* "a/b/" -> "b" */
bool cu_is_dir(const char *path);
bool cu_exists(const char *path);

/* ---- numbers, users, time ---------------------------------------------------- */
bool cu_parse_u64(const char *s, unsigned base, uint64_t *out);           /* whole string */
bool cu_parse_count(const char *s, long *out);                            /* decimal >= 0 */
/* /etc/passwd, /etc/group lookups (name <-> id); -1 when unknown */
long cu_lookup_id(const char *file, const char *name);
bool cu_lookup_name(const char *file, long id, char *out, size_t cap);
void cu_owner_name(uint32_t uid, char *out, size_t cap);
void cu_group_name(uint32_t gid, char *out, size_t cap);

struct cu_tm { int year, mon, mday, hour, min, sec, wday, yday; };
void   cu_gmtime(int64_t t, struct cu_tm *tm);
size_t cu_strftime(char *out, size_t cap, const char *fmt, const struct cu_tm *tm, int64_t t);
extern const char *const cu_wday[7], *const cu_month[12];

/* Read a small text file (procfs/sysfs) into buf; returns length or -errno. */
long cu_read_small(const char *path, char *buf, size_t cap);

/* utility entry points */
int pwd_main(struct cu_io *, int, char **);
int cd_main(struct cu_io *, int, char **);
int ls_main(struct cu_io *, int, char **);
int mkdir_main(struct cu_io *, int, char **);
int rmdir_main(struct cu_io *, int, char **);
int touch_main(struct cu_io *, int, char **);
int rm_main(struct cu_io *, int, char **);
int cp_main(struct cu_io *, int, char **);
int mv_main(struct cu_io *, int, char **);
int cat_main(struct cu_io *, int, char **);
int head_main(struct cu_io *, int, char **);
int tail_main(struct cu_io *, int, char **);
int grep_main(struct cu_io *, int, char **);
int echo_main(struct cu_io *, int, char **);
int chmod_main(struct cu_io *, int, char **);
int chown_main(struct cu_io *, int, char **);
int date_main(struct cu_io *, int, char **);
int uname_main(struct cu_io *, int, char **);
int neofetch_main(struct cu_io *, int, char **);

/* shared by cp and mv */
int cu_copy_tree(struct cu_io *io, const char *prog, const char *src, const char *dst, bool recursive);
int cu_remove_tree(struct cu_io *io, const char *prog, const char *path);

#endif
