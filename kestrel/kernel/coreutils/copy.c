/* kernel/coreutils/copy.c -- cp, mv */
#include "cu.h"
#include <kernel/string.h>
#include <kernel/mm.h>

#define MAX_ENTRIES 512

static bool same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* Copy one regular file through a 4 KiB buffer. */
static int copy_file(struct cu_io *io, const char *prog, const char *src, const char *dst, const struct stat *sst)
{
    struct stat dst_st;
    if (u_stat(dst, &dst_st) == 0) {
        if (same_file(sst, &dst_st)) {
            cu_eprintf(io, "%s: '%s' and '%s' are the same file\n", prog, src, dst);
            return 1;
        }
        if (S_ISDIR(dst_st.st_mode)) return cu_fail(io, prog, dst, -EISDIR);
    }
    int in = u_open(src, O_RDONLY, 0);
    if (in < 0) return cu_fail(io, prog, src, in);
    int out = u_open(dst, O_WRONLY | O_CREAT | O_TRUNC, sst->st_mode & 0777);
    if (out < 0) { u_close(in); return cu_fail(io, prog, dst, out); }

    char *buf = kmalloc(CU_BUF);                        /* intermediate 4 KiB chunk */
    int status = 0;
    if (!buf) status = cu_fail(io, prog, src, -ENOMEM);
    while (buf) {
        ssize_t n = u_read(in, buf, CU_BUF);
        if (n == 0) break;
        if (n < 0) { status = cu_fail(io, prog, src, (int)n); break; }
        int rc = cu_write(out, buf, (size_t)n);
        if (rc < 0) { status = cu_fail(io, prog, dst, rc); break; }
    }
    kfree(buf);
    u_close(in);
    u_close(out);
    if (!status) u_chmod(dst, sst->st_mode & 07777);    /* keep the permission bits */
    return status;
}

int cu_copy_tree(struct cu_io *io, const char *prog, const char *src, const char *dst, bool recursive)
{
    struct stat st;
    int rc = u_stat(src, &st);
    if (rc < 0) return cu_fail(io, prog, src, rc);
    if (S_ISREG(st.st_mode)) return copy_file(io, prog, src, dst, &st);
    if (!S_ISDIR(st.st_mode)) {
        cu_eprintf(io, "%s: cannot copy special file '%s'\n", prog, src);
        return 1;
    }
    if (!recursive) {
        cu_eprintf(io, "%s: -r not specified; omitting directory '%s'\n", prog, src);
        return 1;
    }

    /* refuse to copy a directory into itself (dst below src) */
    char sabs[CU_PATH_MAX], cwd[CU_PATH_MAX], dabs[CU_PATH_MAX];
    if (u_getcwd(cwd, sizeof cwd) < 0) cwd[0] = 0;
    if (src[0] == '/') strlcpy(sabs, src, sizeof sabs); else cu_join(sabs, sizeof sabs, cwd, src);
    if (dst[0] == '/') strlcpy(dabs, dst, sizeof dabs); else cu_join(dabs, sizeof dabs, cwd, dst);
    size_t sl = strlen(sabs);
    if (!strncmp(dabs, sabs, sl) && (dabs[sl] == '/' || dabs[sl] == 0)) {
        cu_eprintf(io, "%s: cannot copy a directory, '%s', into itself, '%s'\n", prog, src, dst);
        return 1;
    }

    rc = u_mkdir(dst, st.st_mode & 0777);
    if (rc < 0 && !(rc == -EEXIST && cu_is_dir(dst))) return cu_fail(io, prog, dst, rc);

    int fd = u_open(src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return cu_fail(io, prog, src, fd);
    char (*names)[64] = kmalloc(sizeof *names * MAX_ENTRIES);
    char *buf = kmalloc(2048);
    int n = 0, got, status = 0;
    if (!names || !buf) { kfree(names); kfree(buf); u_close(fd); return cu_fail(io, prog, src, -ENOMEM); }
    while (n < MAX_ENTRIES && (got = u_getdents64(fd, buf, 2048)) > 0)
        for (int off = 0; off < got && n < MAX_ENTRIES; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) strlcpy(names[n++], d->d_name, 64);
        }
    u_close(fd);
    kfree(buf);
    for (int i = 0; i < n; i++) {
        char s[CU_PATH_MAX], d[CU_PATH_MAX];
        if (cu_join(s, sizeof s, src, names[i]) < 0 || cu_join(d, sizeof d, dst, names[i]) < 0) {
            status |= cu_fail(io, prog, names[i], -ENAMETOOLONG);
            continue;
        }
        status |= cu_copy_tree(io, prog, s, d, true);
    }
    kfree(names);
    return status;
}

/* Parse "[-r] SOURCE... DEST"; resolve DEST/basename(SOURCE) when DEST is a
 * directory. Calls `op` for every pair. */
static int each_pair(struct cu_io *io, const char *prog, int argc, char **argv, bool *recursive,
                     int (*op)(struct cu_io *, const char *, const char *, bool))
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (recursive && (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R"))) { *recursive = true; continue; }
        cu_eprintf(io, "usage: %s %sSOURCE... DEST\n", prog, recursive ? "[-r] " : "");
        return 2;
    }
    int nsrc = argc - i - 1;
    if (nsrc < 1) {
        cu_eprintf(io, "%s: missing %s operand\n", prog, argc - i == 1 ? "destination file" : "file");
        return 2;
    }
    const char *dest = argv[argc - 1];
    bool dest_dir = cu_is_dir(dest);
    if (nsrc > 1 && !dest_dir) return cu_fail(io, prog, dest, -ENOTDIR);

    int status = 0;
    for (int s = i; s < argc - 1; s++) {
        char target[CU_PATH_MAX];
        if (dest_dir) {
            char base[64];
            cu_basename(argv[s], base, sizeof base);
            if (cu_join(target, sizeof target, dest, base) < 0) { status |= cu_fail(io, prog, argv[s], -ENAMETOOLONG); continue; }
        } else {
            strlcpy(target, dest, sizeof target);
        }
        status |= op(io, argv[s], target, recursive ? *recursive : true);
    }
    return status;
}

static int cp_one(struct cu_io *io, const char *src, const char *dst, bool recursive)
{
    return cu_copy_tree(io, "cp", src, dst, recursive);
}

int cp_main(struct cu_io *io, int argc, char **argv)
{
    bool recursive = false;
    return each_pair(io, "cp", argc, argv, &recursive, cp_one);
}

/* mv: rename(2) within one file system; across file systems (EXDEV, e.g.
 * from the read-only /boot volume to the RAM root) copy, then delete. */
static int mv_one(struct cu_io *io, const char *src, const char *dst, bool unused)
{
    (void)unused;
    int rc = u_rename(src, dst);
    if (rc == 0) return 0;
    if (rc != -EXDEV) return cu_fail(io, "mv", src, rc);
    int status = cu_copy_tree(io, "mv", src, dst, true);
    if (status) return status;
    return cu_remove_tree(io, "mv", src);
}

int mv_main(struct cu_io *io, int argc, char **argv)
{
    return each_pair(io, "mv", argc, argv, NULL, mv_one);
}
