/* kernel/coreutils/fileops.c -- mkdir, rmdir, touch, rm */
#include "cu.h"
#include <kernel/string.h>
#include <kernel/mm.h>

/* ---- mkdir [-p] [-m MODE] DIR... ------------------------------------------ */
static int mkdir_parents(struct cu_io *io, const char *path, mode_t mode)
{
    char buf[CU_PATH_MAX];
    if (strlcpy(buf, path, sizeof buf) >= sizeof buf) return cu_fail(io, "mkdir", path, -ENAMETOOLONG);
    size_t len = strlen(buf);
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        char saved = buf[i];
        buf[i] = 0;
        int rc = u_mkdir(buf, mode);
        if (rc < 0 && !(rc == -EEXIST && cu_is_dir(buf))) return cu_fail(io, "mkdir", buf, rc);
        buf[i] = saved;
        while (buf[i] == '/' && buf[i + 1] == '/') i++;     /* collapse "a//b" */
    }
    return 0;
}

int mkdir_main(struct cu_io *io, int argc, char **argv)
{
    bool parents = false;
    mode_t mode = 0755;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-p")) { parents = true; continue; }
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            uint64_t m;
            if (!cu_parse_u64(argv[++i], 8, &m) || m > 07777) { cu_eprintf(io, "mkdir: invalid mode '%s'\n", argv[i]); return 2; }
            mode = (mode_t)m;
            continue;
        }
        cu_eprintf(io, "usage: mkdir [-p] [-m MODE] DIRECTORY...\n");
        return 2;
    }
    if (i == argc) { cu_eprintf(io, "mkdir: missing operand\n"); return 2; }
    int status = 0;
    for (; i < argc; i++) {
        if (parents) { status |= mkdir_parents(io, argv[i], mode); continue; }
        int rc = u_mkdir(argv[i], mode);
        if (rc < 0) status |= cu_fail(io, "mkdir", argv[i], rc);
    }
    return status;
}

/* ---- rmdir DIR... ----------------------------------------------------------
 * The kernel refuses non-empty directories (ENOTEMPTY), mount points (EBUSY)
 * and non-directories (ENOTDIR); each failure is reported and counted. */
int rmdir_main(struct cu_io *io, int argc, char **argv)
{
    if (argc < 2) { cu_eprintf(io, "rmdir: missing operand\n"); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = u_rmdir(argv[i]);
        if (rc < 0) {
            char what[CU_PATH_MAX + 32];
            snprintf(what, sizeof what, "failed to remove '%s'", argv[i]);
            status |= cu_fail(io, "rmdir", what, rc);
        }
    }
    return status;
}

/* ---- touch [-c] FILE... ----------------------------------------------------
 * Missing files are created with open(O_CREAT); existing ones get their
 * modification time set to now with utimensat(2). */
int touch_main(struct cu_io *io, int argc, char **argv)
{
    bool no_create = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-c")) { no_create = true; continue; }
        if (!strcmp(argv[i], "--")) { i++; break; }
        cu_eprintf(io, "usage: touch [-c] FILE...\n");
        return 2;
    }
    if (i == argc) { cu_eprintf(io, "touch: missing file operand\n"); return 2; }
    int status = 0;
    for (; i < argc; i++) {
        struct stat st;
        int rc = u_stat(argv[i], &st);
        if (rc == -ENOENT) {
            if (no_create) continue;
            int fd = u_open(argv[i], O_WRONLY | O_CREAT, 0644);
            if (fd < 0) { status |= cu_fail(io, "touch", argv[i], fd); continue; }
            u_close(fd);
        } else if (rc < 0) {
            status |= cu_fail(io, "touch", argv[i], rc);
        } else if ((rc = u_touch_now(argv[i])) < 0) {
            status |= cu_fail(io, "touch", argv[i], rc);
        }
    }
    return status;
}

/* ---- rm [-f] [-r] FILE... -------------------------------------------------- */
#define MAX_ENTRIES 512

/* Remove `path` and, for a directory, everything below it. Entries are
 * collected before deleting, because removing shifts getdents positions. */
int cu_remove_tree(struct cu_io *io, const char *prog, const char *path)
{
    struct stat st;
    int rc = u_stat(path, &st);
    if (rc < 0) return cu_fail(io, prog, path, rc);
    if (!S_ISDIR(st.st_mode)) {
        rc = u_unlink(path);
        return rc < 0 ? cu_fail(io, prog, path, rc) : 0;
    }

    int fd = u_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return cu_fail(io, prog, path, fd);
    char (*names)[64] = kmalloc(sizeof *names * MAX_ENTRIES);
    char *buf = kmalloc(2048);
    int n = 0, got, status = 0;
    if (!names || !buf) { kfree(names); kfree(buf); u_close(fd); return cu_fail(io, prog, path, -ENOMEM); }
    while (n < MAX_ENTRIES && (got = u_getdents64(fd, buf, 2048)) > 0)
        for (int off = 0; off < got && n < MAX_ENTRIES; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
            strlcpy(names[n++], d->d_name, 64);
        }
    u_close(fd);
    kfree(buf);
    for (int i = 0; i < n; i++) {
        char child[CU_PATH_MAX];
        if (cu_join(child, sizeof child, path, names[i]) < 0) { status |= cu_fail(io, prog, names[i], -ENAMETOOLONG); continue; }
        status |= cu_remove_tree(io, prog, child);
    }
    kfree(names);
    if ((rc = u_rmdir(path)) < 0) status |= cu_fail(io, prog, path, rc);
    return status;
}

int rm_main(struct cu_io *io, int argc, char **argv)
{
    bool force = false, recursive = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'f') force = true;
            else if (*p == 'r' || *p == 'R') recursive = true;
            else { cu_eprintf(io, "rm: invalid option -- '%c'\nusage: rm [-f] [-r] FILE...\n", *p); return 2; }
        }
    }
    if (i == argc) {
        if (force) return 0;
        cu_eprintf(io, "rm: missing operand\n");
        return 2;
    }
    int status = 0;
    for (; i < argc; i++) {
        char base[64];
        cu_basename(argv[i], base, sizeof base);
        if (!strcmp(base, ".") || !strcmp(base, "..")) {
            cu_eprintf(io, "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        if (!strcmp(base, "/")) { cu_eprintf(io, "rm: it is dangerous to operate recursively on '/'\n"); status = 1; continue; }
        struct stat st;
        int rc = u_stat(argv[i], &st);
        if (rc < 0) {
            if (!(force && rc == -ENOENT)) status |= cu_fail(io, "rm", argv[i], rc);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!recursive) { status |= cu_fail(io, "rm", argv[i], -EISDIR); continue; }
            status |= cu_remove_tree(io, "rm", argv[i]);
            continue;
        }
        if ((rc = u_unlink(argv[i])) < 0) status |= cu_fail(io, "rm", argv[i], rc);
    }
    return status;
}
