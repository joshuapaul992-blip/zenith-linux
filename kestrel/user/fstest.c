/* user/fstest.c -- the disk file system (ext2 root) through system calls:
 * files of every size class (direct, indirect, double-indirect, sparse),
 * truncation, symbolic and hard links, renames, directories, unlinked open
 * files, permissions, and free-space accounting that returns to where it
 * started.
 *
 *   fstest [DIR]            run the checks in DIR (default /var/tmp/fstest)
 *   fstest -w               write the persistence files (/var/fstest-persist)
 *   fstest -c               check them after a reboot, then remove them
 *   fstest -r               both, across a reboot: write them and restart, or
 *                           (when they are there) check them and power off
 *   -p (with any of these)  power off when done (sync first)
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/reboot.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static uint8_t pat(uint64_t off, unsigned seed) { return (uint8_t)((off * 2654435761u >> 7) ^ seed ^ (off >> 12)); }

static int write_pattern(const char *path, uint64_t off, size_t len, unsigned seed, int flags)
{
    int fd = open(path, O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0) return -1;
    static uint8_t buf[65536];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof buf ? len - done : sizeof buf;
        for (size_t i = 0; i < n; i++) buf[i] = pat(off + done + i, seed);
        if (pwrite(fd, buf, n, (off_t)(off + done)) != (ssize_t)n) { close(fd); return -1; }
        done += n;
    }
    return close(fd);
}

static int check_pattern(const char *path, uint64_t off, size_t len, unsigned seed)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static uint8_t buf[65536];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof buf ? len - done : sizeof buf;
        if (pread(fd, buf, n, (off_t)(off + done)) != (ssize_t)n) { close(fd); return 0; }
        for (size_t i = 0; i < n; i++)
            if (buf[i] != pat(off + done + i, seed)) { close(fd); return 0; }
        done += n;
    }
    close(fd);
    return 1;
}

static int all_zero(const char *path, uint64_t off, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static uint8_t buf[4096];
    while (len) {
        size_t n = len < sizeof buf ? len : sizeof buf;
        if (pread(fd, buf, n, (off_t)off) != (ssize_t)n) { close(fd); return 0; }
        for (size_t i = 0; i < n; i++) if (buf[i]) { close(fd); return 0; }
        off += n;
        len -= n;
    }
    close(fd);
    return 1;
}

static long free_blocks(const char *dir)
{
    struct statfs s;
    sync();
    return statfs(dir, &s) == 0 ? (long)s.f_bfree : -1;
}

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        char sub[1024];
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_rf(sub);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static char P[512];
static const char *in(const char *dir, const char *name) { snprintf(P, sizeof P, "%s/%s", dir, name); return P; }

static void inotify_checks(const char *dir);
static void lock_checks(const char *dir);

static void checks(const char *dir)
{
    char a[512], b[512];
    struct stat st;
    struct statfs sf;
    rm_rf(dir);
    CHECK(mkdir(dir, 0755) == 0, "mkdir the test directory");
    CHECK(statfs(dir, &sf) == 0 && sf.f_type == 0xEF53, "statfs: an ext2 file system");
    printf("  (block size %ld, %ld of %ld blocks free)\n", (long)sf.f_bsize, (long)sf.f_bfree, (long)sf.f_blocks);
    long bs = sf.f_bsize;
    long free0 = free_blocks(dir);

    printf("files:\n");
    CHECK(write_pattern(in(dir, "small"), 0, 100, 1, O_TRUNC) == 0 && check_pattern(P, 0, 100, 1), "100 bytes");
    CHECK(stat(P, &st) == 0 && st.st_size == 100 && S_ISREG(st.st_mode) && st.st_nlink == 1, "stat: size, type, links");
    size_t direct = (size_t)bs * 12;
    CHECK(write_pattern(in(dir, "direct"), 0, direct, 2, O_TRUNC) == 0 && check_pattern(P, 0, direct, 2),
          "12 blocks (direct pointers)");
    size_t ind = (size_t)bs * (12 + bs / 4 / 2);
    CHECK(write_pattern(in(dir, "indirect"), 0, ind, 3, O_TRUNC) == 0 && check_pattern(P, 0, ind, 3),
          "into the single-indirect block");
    size_t dind = (size_t)bs * (12 + bs / 4 + 300);
    CHECK(write_pattern(in(dir, "dindirect"), 0, dind, 4, O_TRUNC) == 0 && check_pattern(P, 0, dind, 4),
          "into the double-indirect blocks");
    CHECK(write_pattern(in(dir, "unaligned"), 1000, 10000, 5, O_TRUNC) == 0 && check_pattern(P, 1000, 10000, 5) &&
          all_zero(P, 0, 1000), "unaligned write after a hole");
    /* sparse: one block far out (a triple-indirect one with 1 KiB blocks, double with 4 KiB) */
    uint64_t far = (uint64_t)bs * (12 + bs / 4 + (uint64_t)(bs / 4) * (bs / 4) + 7);
    if (far > 1500000000ull) far = 1500000000ull;
    CHECK(write_pattern(in(dir, "sparse"), far, 5000, 6, O_TRUNC) == 0 && check_pattern(P, far, 5000, 6),
          "sparse file: data far past the start");
    CHECK(stat(P, &st) == 0 && st.st_size == (off_t)(far + 5000) && st.st_blocks * 512 < 64 * bs,
          "sparse file: size counts the hole, blocks do not");
    CHECK(all_zero(P, far / 2, 8192), "sparse file: the hole reads as zeros");

    printf("truncate:\n");
    strcpy(a, in(dir, "dindirect"));
    CHECK(truncate(a, 5000) == 0 && stat(a, &st) == 0 && st.st_size == 5000 && check_pattern(a, 0, 5000, 4),
          "shrink keeps the head");
    CHECK(truncate(a, 20000) == 0 && all_zero(a, 5000, 15000) && check_pattern(a, 0, 5000, 4),
          "grow again: the old tail reads as zeros");
    CHECK(stat(a, &st) == 0 && st.st_blocks * 512 <= ((5000 + bs - 1) / bs + 1) * bs, "shrinking freed the blocks");
    int fd = open(a, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 0, "O_TRUNC");
    close(fd);
    fd = open(in(dir, "small"), O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "tail", 4) == 4 && stat(P, &st) == 0 && st.st_size == 104, "O_APPEND");
    close(fd);

    printf("symbolic links:\n");
    CHECK(symlink("small", in(dir, "fast")) == 0, "symlink (short target, kept in the inode)");
    CHECK(lstat(P, &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 5, "lstat sees the link");
    CHECK(stat(P, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 104, "stat follows it");
    char lt[300];
    memset(lt, 0, sizeof lt);
    char longt[200];
    snprintf(longt, sizeof longt, "%s/%s", dir, "a-rather-long-name-that-does-not-fit-in-sixty-bytes/../small");
    mkdir(in(dir, "a-rather-long-name-that-does-not-fit-in-sixty-bytes"), 0755);
    CHECK(symlink(longt, in(dir, "slow")) == 0 && readlink(P, lt, sizeof lt) == (ssize_t)strlen(longt) &&
          !strcmp(lt, longt), "symlink (long target, in a block) and readlink");
    CHECK(check_pattern(P, 0, 100, 1), "read through a link with \"..\" in its target");
    CHECK(symlink("loop2", in(dir, "loop1")) == 0 && symlink("loop1", in(dir, "loop2")) == 0 &&
          open(in(dir, "loop1"), O_RDONLY) < 0 && errno == ELOOP, "a link cycle gives ELOOP");
    CHECK(open(in(dir, "fast"), O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP, "O_NOFOLLOW on a link");

    printf("hard links:\n");
    strcpy(a, in(dir, "small"));
    strcpy(b, in(dir, "small-link"));
    CHECK(link(a, b) == 0 && stat(a, &st) == 0 && st.st_nlink == 2, "link: two names, nlink 2");
    struct stat st2;
    CHECK(stat(b, &st2) == 0 && st2.st_ino == st.st_ino, "both names, one inode");
    fd = open(b, O_WRONLY | O_APPEND);
    if (fd >= 0) { write(fd, "!", 1); close(fd); }
    CHECK(stat(a, &st) == 0 && st.st_size == 105, "a write through one name shows through the other");
    CHECK(unlink(a) == 0 && stat(b, &st) == 0 && st.st_nlink == 1 && check_pattern(b, 0, 100, 1),
          "unlink one name: the data stays");

    printf("renames and directories:\n");
    strcpy(a, in(dir, "small-link"));
    strcpy(b, in(dir, "renamed"));
    CHECK(rename(a, b) == 0 && access(a, F_OK) < 0 && check_pattern(b, 0, 100, 1), "rename a file");
    mkdir(in(dir, "d1"), 0755);
    mkdir(in(dir, "d2"), 0755);
    mkdir(in(dir, "d1/sub"), 0755);
    write_pattern(in(dir, "d1/sub/f"), 0, 3000, 7, O_TRUNC);
    strcpy(a, in(dir, "d1/sub"));
    strcpy(b, in(dir, "d2/moved"));
    CHECK(rename(a, b) == 0 && check_pattern(in(dir, "d2/moved/f"), 0, 3000, 7), "move a directory to another");
    CHECK(stat(in(dir, "d2/moved/.."), &st) == 0 && stat(in(dir, "d2"), &st2) == 0 && st.st_ino == st2.st_ino,
          "its \"..\" follows");
    CHECK(stat(in(dir, "d1"), &st) == 0 && st.st_nlink == 2 && stat(in(dir, "d2"), &st) == 0 && st.st_nlink == 3,
          "directory link counts");
    write_pattern(in(dir, "victim"), 0, 777, 8, O_TRUNC);
    strcpy(a, in(dir, "renamed"));
    strcpy(b, in(dir, "victim"));
    CHECK(rename(a, b) == 0 && check_pattern(b, 0, 100, 1) && stat(b, &st) == 0 && st.st_size == 105,
          "rename over an existing file replaces it");
    CHECK(rmdir(in(dir, "d2")) < 0 && errno == ENOTEMPTY, "rmdir of a non-empty directory: ENOTEMPTY");
    CHECK(rmdir(in(dir, "d1")) == 0 && access(P, F_OK) < 0, "rmdir an empty one");
    int made = 0;
    for (int i = 0; i < 400; i++) {
        char n[64];
        snprintf(n, sizeof n, "d2/entry-with-a-longish-name-%03d", i);
        int f2 = open(in(dir, n), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (f2 >= 0) { made++; if (i % 50 == 0) write(f2, n, strlen(n)); close(f2); }
    }
    CHECK(made == 400 && count_entries(in(dir, "d2")) == 401, "400 files in one directory (it grows)");
    int gone = 0;
    for (int i = 0; i < 400; i += 2) {
        char n[64];
        snprintf(n, sizeof n, "d2/entry-with-a-longish-name-%03d", i);
        if (unlink(in(dir, n)) == 0) gone++;
    }
    CHECK(gone == 200 && count_entries(in(dir, "d2")) == 201, "every other one removed");
    fd = open(in(dir, "d2/entry-with-a-longish-name-007"), O_RDONLY);
    CHECK(fd >= 0 && open(in(dir, "d2/entry-with-a-longish-name-008"), O_RDONLY) < 0, "the right ones");
    close(fd);

    printf("open files and metadata:\n");
    write_pattern(in(dir, "held"), 0, (size_t)bs * 40, 9, O_TRUNC);
    fd = open(P, O_RDONLY);
    CHECK(fd >= 0 && unlink(P) == 0 && access(P, F_OK) < 0, "unlink a file that is open");
    uint8_t c = 0;
    CHECK(pread(fd, &c, 1, bs * 30 + 5) == 1 && c == pat((uint64_t)bs * 30 + 5, 9), "still readable through the descriptor");
    close(fd);
    strcpy(a, in(dir, "victim"));
    CHECK(chmod(a, 0600) == 0 && stat(a, &st) == 0 && (st.st_mode & 0777) == 0600, "chmod");
    CHECK(chown(a, 1000, 1000) == 0 && stat(a, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000, "chown");
    CHECK(open(in(dir, "nope/x"), O_RDONLY) < 0 && errno == ENOENT, "ENOENT");
    CHECK(mkdir(in(dir, "victim"), 0755) < 0 && errno == EEXIST, "EEXIST");

    printf("locks and extended attributes:\n");
    int l1 = open(a, O_RDWR), l2 = open(a, O_RDONLY);
    CHECK(flock(l1, LOCK_EX | LOCK_NB) == 0, "flock: exclusive lock");
    CHECK(flock(l2, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK, "flock: a second open file description is refused");
    int l3 = dup(l1);
    CHECK(flock(l3, LOCK_EX | LOCK_NB) == 0, "flock: a dup'd descriptor shares the lock");
    pid_t kid = fork();
    if (kid == 0) {                                     /* the child blocks until the parent's last close */
        close(l1); close(l3);                           /* its copies would keep the lock alive */
        _exit(flock(l2, LOCK_EX) == 0 ? 0 : 1);
    }
    usleep(100000);
    close(l1); close(l3);
    int ws = -1;
    CHECK(waitpid(kid, &ws, 0) == kid && WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "flock: a waiter gets the lock when the holder closes");
    CHECK(flock(l2, LOCK_UN) == 0 && flock(l2, LOCK_SH | LOCK_NB) == 0, "flock: unlock, then a shared lock");
    close(l2);
    char xb[64];
    CHECK(listxattr(a, xb, sizeof xb) == 0, "listxattr: an empty list");
    CHECK(getxattr(a, "user.x", xb, sizeof xb) < 0 && errno == ENODATA, "getxattr: ENODATA");
    CHECK(setxattr(a, "user.x", "1", 1, 0) < 0 && errno == ENOTSUP, "setxattr: ENOTSUP");

    printf("inotify:\n");
    inotify_checks(dir);

    printf("record locks, descriptor reopen, waitid:\n");
    lock_checks(dir);

    printf("space accounting:\n");
    rm_rf(dir);
    CHECK(access(dir, F_OK) < 0, "rm -r of the whole tree (deleting while reading directories)");
    long free1 = free_blocks("/");
    CHECK(free0 >= 0 && free1 == free0 + 1, "after removing everything, the free count is back (all blocks freed)");
    if (free1 != free0 + 1) printf("  (free before %ld, after %ld; the test directory's own block accounts for 1)\n", free0, free1);
}

/* inotify: read what is queued into "MASK:name" words, e.g. "100:f 2:f 8:f" */
static int inotify_drain(int fd, char *out, size_t cap)
{
    char buf[4096] __attribute__((aligned(8)));
    out[0] = 0;
    int count = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "%s%x:%s", l ? " " : "", e->mask, e->len ? e->name : "");
            count++;
            p += sizeof *e + e->len;
        }
    }
    return count;
}

static void inotify_checks(const char *dir)
{
    char ev[1024], a[256], b[256];
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0, "inotify_init1");
    char buf[64];
    CHECK(read(fd, buf, sizeof buf) < 0 && errno == EAGAIN, "inotify: nothing queued, EAGAIN");
    int wd = inotify_add_watch(fd, dir, IN_ALL_EVENTS);
    CHECK(wd > 0, "inotify_add_watch on a directory");
    snprintf(a, sizeof a, "%s/watched", dir);
    int f = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(f, "x", 1);
    write(f, "y", 1);                                   /* the same event twice: queued once */
    close(f);
    struct pollfd p = { fd, POLLIN, 0 };
    int avail = 0;
    CHECK(poll(&p, 1, 0) == 1 && ioctl(fd, FIONREAD, &avail) == 0 && avail > 0, "inotify: readable, FIONREAD counts the bytes");
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "100:watched") && strstr(ev, "2:watched") && strstr(ev, "8:watched") && !strstr(ev, "2:watched 2:watched"),
          "create, modify (once), close-write of a file in the directory");
    printf("  (%s)\n", ev);
    snprintf(b, sizeof b, "%s/renamed", dir);
    rename(a, b);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "40:watched") && strstr(ev, "80:renamed"), "rename: moved-from and moved-to");
    int fw = inotify_add_watch(fd, b, IN_ATTRIB | IN_DELETE_SELF);
    CHECK(fw > 0 && fw != wd, "a second watch, on the file");
    chmod(b, 0600);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "4:") && strstr(ev, "4:renamed"), "chmod: attrib, on the file's watch and the directory's");
    unlink(b);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "200:renamed") && strstr(ev, "400:") && strstr(ev, "8000:"), "unlink: delete, delete-self, ignored");
    snprintf(a, sizeof a, "%s/sub", dir);
    mkdir(a, 0755);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "40000100:sub") != NULL, "mkdir: create with IN_ISDIR");
    rmdir(a);
    CHECK(inotify_add_watch(fd, "/etc/passwd", IN_MODIFY | IN_ONLYDIR) < 0 && errno == ENOTDIR, "IN_ONLYDIR on a file: ENOTDIR");
    CHECK(inotify_rm_watch(fd, wd) == 0, "inotify_rm_watch");
    inotify_drain(fd, ev, sizeof ev);
    CHECK(strstr(ev, "8000:") != NULL, "...queues IN_IGNORED");
    CHECK(inotify_rm_watch(fd, wd) < 0 && errno == EINVAL, "...and the watch is gone");
    close(fd);
}

/* fcntl byte-range locks between two processes (SQLite's way of locking),
 * reopening a descriptor through /proc/self/fd (Firefox's read-only memfd
 * copies) and waitid. */
static int lock_probe(int fd, short type, off_t start, off_t len, struct flock *out)
{
    struct flock l = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    int r = fcntl(fd, F_GETLK, &l);
    if (out) *out = l;
    return r;
}

static void lock_checks(const char *dir)
{
    char a[256];
    snprintf(a, sizeof a, "%s/locked", dir);
    int fd = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 4 };
    CHECK(fcntl(fd, F_SETLK, &l) == 0, "F_SETLK: write lock on bytes 0-3");
    l = (struct flock){ .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 6, .l_len = 0 };
    CHECK(fcntl(fd, F_SETLK, &l) == 0, "F_SETLK: read lock from byte 6 to the end");
    struct flock g;
    CHECK(lock_probe(fd, F_WRLCK, 0, 0, &g) == 0 && g.l_type == F_UNLCK, "F_GETLK: our own locks never conflict");
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        struct flock c;
        int ok = 1;
        ok &= lock_probe(fd, F_WRLCK, 2, 1, &c) == 0 && c.l_type == F_WRLCK && c.l_start == 0 && c.l_len == 4 && c.l_pid == getppid();
        ok &= lock_probe(fd, F_RDLCK, 8, 1, &c) == 0 && c.l_type == F_UNLCK;        /* read vs read */
        ok &= lock_probe(fd, F_WRLCK, 4, 2, &c) == 0 && c.l_type == F_UNLCK;        /* the gap */
        c = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 3, .l_len = 1 };
        ok &= fcntl(fd, F_SETLK, &c) < 0 && (errno == EAGAIN || errno == EACCES);
        c.l_start = 4;
        ok &= fcntl(fd, F_SETLK, &c) == 0;
        write(p[1], "r", 1);
        c = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
        ok &= fcntl(fd, F_SETLKW, &c) == 0;                 /* waits for the parent's unlock */
        _exit(ok ? 0 : 1);
    }
    char ch;
    read(p[0], &ch, 1);
    CHECK(lock_probe(fd, F_WRLCK, 4, 1, &g) == 0 && g.l_type == F_WRLCK && g.l_pid == pid, "the child's lock on byte 4, seen by F_GETLK");
    usleep(100000);
    int st = -1;
    CHECK(waitpid(pid, &st, WNOHANG) == 0, "the child waits in F_SETLKW...");
    l = (struct flock){ .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    CHECK(fcntl(fd, F_SETLK, &l) == 0, "F_UNLCK of everything");
    siginfo_t si;
    memset(&si, 0xff, sizeof si);
    CHECK(waitid(P_PID, pid, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == pid && si.si_code == CLD_EXITED && si.si_status == 0,
          "...gets it, and passes every check (waitid with WNOWAIT)");
    CHECK(waitid(P_ALL, 0, &si, WEXITED) == 0 && si.si_pid == pid, "waitid reaps it after the look");
    CHECK(waitid(P_ALL, 0, &si, WEXITED | WNOHANG) < 0 && errno == ECHILD, "waitid: no children left, ECHILD");
    close(p[0]); close(p[1]);
    l = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    fcntl(fd, F_SETLK, &l);
    pid = fork();
    if (pid == 0) {
        struct flock c = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        _exit(fcntl(fd, F_SETLK, &c) == 0 ? 0 : 1);
    }
    int fd2 = open(a, O_RDONLY);
    close(fd2);                                             /* any close drops the process's locks */
    waitpid(pid, &st, 0);
    st = -1;
    pid = fork();
    if (pid == 0) {
        struct flock c = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        _exit(fcntl(fd, F_SETLK, &c) == 0 ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "closing another descriptor of the file releases the locks (POSIX)");

    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    int ro = open(path, O_RDONLY);
    char buf[4] = { 0 };
    CHECK(ro >= 0 && ro != fd && pread(ro, buf, 3, 2) == 3 && !memcmp(buf, "234", 3), "open(/proc/self/fd/N): the same file again");
    CHECK((fcntl(ro, F_GETFL) & O_ACCMODE) == O_RDONLY && write(ro, "x", 1) < 0 && errno == EBADF, "...with the new open's access mode");
    close(ro);
    close(fd);
    unlink(a);

    int m = memfd_create("fstest", MFD_CLOEXEC | 8 /* MFD_NOEXEC_SEAL */);
    CHECK(m >= 0 && fcntl(m, F_GET_SEALS) == 0x20, "memfd_create(MFD_NOEXEC_SEAL): sealable, F_SEAL_EXEC set");
    ftruncate(m, 4096);
    CHECK(fcntl(m, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0, "F_ADD_SEALS: shrink and grow");
    snprintf(path, sizeof path, "/proc/self/fd/%d", m);
    ro = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(ro >= 0 && (fcntl(ro, F_GET_SEALS) & 0x6) == 0x6, "a read-only reopen of the memfd keeps its seals");
    close(ro);
    close(m);
}

#define PERSIST "/var/fstest-persist"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = "/var/tmp/fstest";
    int mode = 0, poweroff = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) mode = 'w';
        else if (!strcmp(argv[i], "-c")) mode = 'c';
        else if (!strcmp(argv[i], "-p")) poweroff = 1;
        else if (!strcmp(argv[i], "-r")) mode = 'r';
        else dir = argv[i];
    }
    printf("fstest: pid %d\n", getpid());
    int restart = 0;
    if (mode == 'r') {
        restart = access(PERSIST, F_OK) < 0;
        mode = restart ? 'w' : 'c';
        poweroff = !restart;
    }
    if (mode == 'w') {
        rm_rf(PERSIST);
        CHECK(mkdir(PERSIST, 0755) == 0, "persistence: directory");
        CHECK(write_pattern(PERSIST "/data", 0, 300000, 11, O_TRUNC) == 0, "persistence: 300000-byte file");
        CHECK(symlink("data", PERSIST "/link") == 0, "persistence: symlink");
        CHECK(link(PERSIST "/data", PERSIST "/hard") == 0, "persistence: hard link");
    } else if (mode == 'c') {
        struct stat st;
        char t[16] = { 0 };
        CHECK(check_pattern(PERSIST "/data", 0, 300000, 11), "after reboot: the file's contents");
        CHECK(readlink(PERSIST "/link", t, sizeof t - 1) == 4 && !strcmp(t, "data"), "after reboot: the symlink");
        CHECK(stat(PERSIST "/hard", &st) == 0 && st.st_nlink == 2 && check_pattern(PERSIST "/hard", 0, 300000, 11),
              "after reboot: the hard link");
        rm_rf(PERSIST);
        CHECK(access(PERSIST, F_OK) < 0, "removed");
    } else {
        checks(dir);
    }
    if (failures) printf("fstest: %d check(s) FAILED\n", failures);
    else printf("fstest: all checks passed\n");
    if (restart && !failures) {
        printf("fstest: restarting\n");
        sync();
        reboot(RB_AUTOBOOT);
    }
    if (poweroff) {
        sync();
        reboot(RB_POWER_OFF);
    }
    return failures ? 1 : 0;
}
