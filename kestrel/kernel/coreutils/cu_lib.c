/* kernel/coreutils/cu_lib.c -- helpers shared by the core utilities */
#include "cu.h"
#include <kernel/string.h>
#include <stdarg.h>

/* ======================================================================== */
/*  output                                                                     */
/* ======================================================================== */
int cu_write(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = u_write(fd, p, len);
        if (n < 0) return (int)n;
        if (n == 0) return -EIO;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void cu_puts(struct cu_io *io, const char *s) { cu_write(io->out, s, strlen(s)); }

static void vout(int fd, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    cu_write(fd, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

void cu_printf(struct cu_io *io, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vout(io->out, fmt, ap); va_end(ap);
}

void cu_eprintf(struct cu_io *io, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vout(io->err, fmt, ap); va_end(ap);
}

bool cu_isatty(int fd)
{
    uint16_t ws[4];
    return u_ioctl(fd, TIOCGWINSZ, ws) == 0;
}

const char *cu_col(struct cu_io *io, const char *ansi) { return cu_isatty(io->out) ? ansi : ""; }

const char *cu_strerror(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case EPERM:        return "Operation not permitted";
    case ENOENT:       return "No such file or directory";
    case EIO:          return "Input/output error";
    case EBADF:        return "Bad file descriptor";
    case ENOMEM:       return "Out of memory";
    case EACCES:       return "Permission denied";
    case EFAULT:       return "Bad address";
    case EBUSY:        return "Device or resource busy";
    case EEXIST:       return "File exists";
    case EXDEV:        return "Invalid cross-device link";
    case ENODEV:       return "No such device";
    case ENOTDIR:      return "Not a directory";
    case EISDIR:       return "Is a directory";
    case EINVAL:       return "Invalid argument";
    case EMFILE:       return "Too many open files";
    case ENOTTY:       return "Inappropriate ioctl for device";
    case ENOSPC:       return "No space left on device";
    case ESPIPE:       return "Illegal seek";
    case EROFS:        return "Read-only file system";
    case ERANGE:       return "Result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS:       return "Function not implemented";
    case ENOTEMPTY:    return "Directory not empty";
    case ETIMEDOUT:    return "Connection timed out";
    case ENOMEDIUM:    return "No medium found";
    default:           return "Unknown error";
    }
}

int cu_fail(struct cu_io *io, const char *prog, const char *what, int err)
{
    if (what) cu_eprintf(io, "%s%s: %s: %s%s\n", cu_col(io, CU_RED), prog, what, cu_strerror(err), cu_col(io, CU_RESET));
    else      cu_eprintf(io, "%s%s: %s%s\n", cu_col(io, CU_RED), prog, cu_strerror(err), cu_col(io, CU_RESET));
    return 1;
}

/* ======================================================================== */
/*  paths                                                                      */
/* ======================================================================== */
int cu_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    bool slash = n && dir[n - 1] == '/';
    int w = snprintf(out, cap, "%s%s%s", dir, slash ? "" : "/", name);
    return (w < 0 || (size_t)w >= cap) ? -ENAMETOOLONG : 0;
}

void cu_basename(const char *path, char *out, size_t cap)
{
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;          /* "dir/" -> "dir" */
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = end - start;
    if (n == 0) { strlcpy(out, "/", cap); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, path + start, n);
    out[n] = 0;
}

bool cu_is_dir(const char *path)
{
    struct stat st;
    return u_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool cu_exists(const char *path)
{
    struct stat st;
    return u_stat(path, &st) == 0;
}

/* ======================================================================== */
/*  numbers                                                                    */
/* ======================================================================== */
bool cu_parse_u64(const char *s, unsigned base, uint64_t *out)
{
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return false;
        if (d >= base) return false;
        if (v > (UINT64_MAX - d) / base) return false;          /* overflow */
        v = v * base + d;
    }
    *out = v;
    return true;
}

bool cu_parse_count(const char *s, long *out)
{
    uint64_t v;
    if (!cu_parse_u64(s, 10, &v) || v > 0x7FFFFFFF) return false;
    *out = (long)v;
    return true;
}

/* ======================================================================== */
/*  /etc/passwd and /etc/group                                                 */
/* ======================================================================== */
long cu_read_small(const char *path, char *buf, size_t cap)
{
    int fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    size_t len = 0;
    ssize_t n;
    while (len + 1 < cap && (n = u_read(fd, buf + len, cap - 1 - len)) > 0) len += (size_t)n;
    u_close(fd);
    buf[len] = 0;
    return (long)len;
}

/* Lines are "name:x:id:..."; call fn for each until it returns true. */
static bool each_entry(const char *file, bool (*fn)(const char *name, long id, void *ctx), void *ctx)
{
    char buf[2048];
    if (cu_read_small(file, buf, sizeof buf) < 0) return false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[4] = { 0 };
        int nf = 0;
        char *p = line;
        f[nf++] = p;
        while (*p && nf < 4) { if (*p == ':') { *p = 0; f[nf++] = p + 1; } p++; }
        if (nf < 3) continue;
        uint64_t id;
        if (!cu_parse_u64(f[2], 10, &id)) continue;
        if (fn(f[0], (long)id, ctx)) return true;
    }
    return false;
}

struct by_name { const char *name; long id; };
struct by_id { long id; char *out; size_t cap; };

static bool match_name(const char *name, long id, void *ctx)
{
    struct by_name *q = ctx;
    if (strcmp(name, q->name)) return false;
    q->id = id;
    return true;
}

static bool match_id(const char *name, long id, void *ctx)
{
    struct by_id *q = ctx;
    if (id != q->id) return false;
    strlcpy(q->out, name, q->cap);
    return true;
}

long cu_lookup_id(const char *file, const char *name)
{
    struct by_name q = { name, -1 };
    each_entry(file, match_name, &q);
    return q.id;
}

bool cu_lookup_name(const char *file, long id, char *out, size_t cap)
{
    struct by_id q = { id, out, cap };
    return each_entry(file, match_id, &q);
}

void cu_owner_name(uint32_t uid, char *out, size_t cap)
{
    if (!cu_lookup_name("/etc/passwd", uid, out, cap)) snprintf(out, cap, "%u", uid);
}

void cu_group_name(uint32_t gid, char *out, size_t cap)
{
    if (!cu_lookup_name("/etc/group", gid, out, cap)) snprintf(out, cap, "%u", gid);
}

/* ======================================================================== */
/*  time (UTC; Kestrel has no time zone database)                              */
/* ======================================================================== */
const char *const cu_wday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *const cu_month[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* civil_from_days (H. Hinnant), valid for the whole proleptic calendar */
void cu_gmtime(int64_t t, struct cu_tm *tm)
{
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->hour = (int)(rem / 3600);
    tm->min = (int)(rem % 3600 / 60);
    tm->sec = (int)(rem % 60);
    tm->wday = (int)((days % 7 + 11) % 7);                  /* 1970-01-01 was a Thursday */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    tm->mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->mon = (int)(mp < 10 ? mp + 3 : mp - 9) - 1;
    tm->year = (int)(y + (tm->mon < 2));
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    bool leap = (tm->year % 4 == 0 && tm->year % 100 != 0) || tm->year % 400 == 0;
    tm->yday = cum[tm->mon] + tm->mday - 1 + (leap && tm->mon > 1);
}

/* strftime subset: %a %b %d %e %H %M %S %Y %m %j %y %s %Z %F %T %D %R %n %t %% */
size_t cu_strftime(char *out, size_t cap, const char *fmt, const struct cu_tm *tm, int64_t t)
{
    size_t n = 0;
    if (!cap) return 0;
#define PUT(...) do { int w_ = snprintf(out + n, cap - n, __VA_ARGS__); \
                      if (w_ > 0) n += (size_t)w_ < cap - n ? (size_t)w_ : cap - n - 1; } while (0)
    for (const char *p = fmt; *p && n + 1 < cap; p++) {
        if (*p != '%' || !p[1]) { out[n++] = *p; continue; }
        switch (*++p) {
        case 'a': PUT("%s", cu_wday[tm->wday]); break;
        case 'b': case 'h': PUT("%s", cu_month[tm->mon]); break;
        case 'd': PUT("%02d", tm->mday); break;
        case 'e': PUT("%2d", tm->mday); break;
        case 'H': PUT("%02d", tm->hour); break;
        case 'M': PUT("%02d", tm->min); break;
        case 'S': PUT("%02d", tm->sec); break;
        case 'Y': PUT("%d", tm->year); break;
        case 'y': PUT("%02d", tm->year % 100); break;
        case 'm': PUT("%02d", tm->mon + 1); break;
        case 'j': PUT("%03d", tm->yday + 1); break;
        case 's': PUT("%ld", (long)t); break;
        case 'Z': PUT("UTC"); break;
        case 'F': PUT("%d-%02d-%02d", tm->year, tm->mon + 1, tm->mday); break;
        case 'T': PUT("%02d:%02d:%02d", tm->hour, tm->min, tm->sec); break;
        case 'R': PUT("%02d:%02d", tm->hour, tm->min); break;
        case 'D': PUT("%02d/%02d/%02d", tm->mon + 1, tm->mday, tm->year % 100); break;
        case 'n': PUT("\n"); break;
        case 't': PUT("\t"); break;
        case '%': PUT("%%"); break;
        default:  PUT("%%%c", *p); break;
        }
    }
#undef PUT
    out[n] = 0;
    return n;
}
