/* kernel/coreutils/nav.c -- pwd, cd */
#include "cu.h"
#include <kernel/string.h>

static char oldpwd[CU_PATH_MAX];                /* for "cd -" */

/* pwd: the kernel tracks the working directory as a vnode on the process
 * (tcb->fs->cwd); getcwd(2) turns it back into an absolute path. */
int pwd_main(struct cu_io *io, int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-L") && strcmp(argv[i], "-P")) {
            cu_eprintf(io, "usage: pwd [-L|-P]\n");
            return 2;
        }
    char buf[CU_PATH_MAX];
    int rc = u_getcwd(buf, sizeof buf);
    if (rc < 0) return cu_fail(io, "pwd", NULL, rc);
    cu_printf(io, "%s\n", buf);
    return 0;
}

/* cd [dir | -]: no argument means the home directory (/root). Relative
 * paths, ".", ".." and absolute paths are resolved by the kernel's path
 * walk; the check before chdir(2) gives precise error messages. */
int cd_main(struct cu_io *io, int argc, char **argv)
{
    if (argc > 2) { cu_eprintf(io, "cd: too many arguments\n"); return 2; }
    const char *target = argc == 2 ? argv[1] : "/root";
    bool dash = !strcmp(target, "-");
    if (dash) {
        if (!oldpwd[0]) { cu_eprintf(io, "cd: OLDPWD not set\n"); return 1; }
        target = oldpwd;
    }
    if (strlen(target) >= CU_PATH_MAX) return cu_fail(io, "cd", target, -ENAMETOOLONG);

    struct stat st;
    int rc = u_stat(target, &st);
    if (rc < 0) return cu_fail(io, "cd", target, rc);
    if (!S_ISDIR(st.st_mode)) return cu_fail(io, "cd", target, -ENOTDIR);

    char before[CU_PATH_MAX], dest[CU_PATH_MAX];
    if (u_getcwd(before, sizeof before) < 0) before[0] = 0;
    strlcpy(dest, target, sizeof dest);                 /* target may alias oldpwd */
    if ((rc = u_chdir(dest)) < 0) return cu_fail(io, "cd", dest, rc);
    strlcpy(oldpwd, before, sizeof oldpwd);
    if (dash) {                                         /* like bash: show where we went */
        char now[CU_PATH_MAX];
        if (u_getcwd(now, sizeof now) >= 0) cu_printf(io, "%s\n", now);
    }
    return 0;
}
