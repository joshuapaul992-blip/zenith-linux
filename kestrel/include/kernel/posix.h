/* include/kernel/posix.h -- POSIX ABI types and constants shared by the
 * kernel and user-space code. Values match Linux x86_64 so that a ported
 * libc (musl, newlib) can target Kestrel with minimal changes. */
#ifndef KESTREL_POSIX_H
#define KESTREL_POSIX_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t  ssize_t;
typedef int64_t  off_t;
typedef int32_t  pid_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

/* ---- errno ------------------------------------------------------------- */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define EBADF         9
#define ENOEXEC       8
#define E2BIG         7
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOTTY       25
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EXDEV        18
#define EPIPE        32
#define ENXIO         6
#define ENOTSOCK     88
#define EPROTOTYPE   91
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define ENOTCONN    107
#define ECONNREFUSED 111
#define EISCONN     106
#define ECONNRESET  104
#define ERANGE       34
#define ETIMEDOUT   110
#define ENOMEDIUM   123
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

/* ---- open(2) flags ------------------------------------------------------- */
#define O_RDONLY     00
#define O_WRONLY     01
#define O_RDWR       02
#define O_ACCMODE    03
#define O_CREAT      0100
#define O_EXCL       0200
#define O_TRUNC      01000
#define O_APPEND     02000
#define O_NONBLOCK   04000
#define O_CLOEXEC    02000000
#define O_DIRECTORY  0200000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ---- file mode bits -------------------------------------------------------- */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

/* ---- struct stat (Linux x86_64 layout) -------------------------------------- */
struct timespec { int64_t tv_sec; int64_t tv_nsec; };

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    struct timespec st_atim, st_mtim, st_ctim;
    int64_t  __unused[3];
};

/* ---- getdents64(2) record ----------------------------------------------------- */
#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

/* ---- uname(2) ----------------------------------------------------------------- */
struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

/* terminal ioctl (Linux numbering) */
#define TIOCGWINSZ 0x5413
struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#endif
