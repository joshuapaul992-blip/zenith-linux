/* kernel/coreutils/perms.c -- chmod, chown */
#include "cu.h"
#include <kernel/string.h>

/* ---- chmod MODE FILE... -----------------------------------------------------
 * MODE is octal (755, 0644, 4755) or symbolic: [ugoa]*[+-=][rwxXst]* clauses
 * separated by commas (u+x, go-w, a=r). */
static bool apply_symbolic(const char *spec, uint32_t old, uint32_t *out)
{
    uint32_t mode = old & 07777;
    bool is_dir = S_ISDIR(old);
    const char *p = spec;
    while (*p) {
        uint32_t who = 0;
        for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        if (!who) who = 07777;                          /* umask ignored: root, no umask here */
        if (*p != '+' && *p != '-' && *p != '=') return false;
        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            uint32_t bits = 0;
            for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
                switch (*p) {
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 'X': if (is_dir || (old & 0111)) bits |= 0111; break;
                case 's': bits |= 06000; break;
                case 't': bits |= 01000; break;
                default: return false;
                }
            }
            bits &= who;
            if (op == '+') mode |= bits;
            else if (op == '-') mode &= ~bits;
            else mode = (mode & ~(who & 07777)) | bits;
        }
        if (*p == ',') p++;
        else if (*p) return false;
    }
    *out = mode;
    return true;
}

int chmod_main(struct cu_io *io, int argc, char **argv)
{
    if (argc < 3) { cu_eprintf(io, "usage: chmod MODE FILE...  (MODE: octal like 755, or u+x,go-w)\n"); return 2; }
    const char *spec = argv[1];
    uint64_t octal = 0;
    bool is_octal = cu_parse_u64(spec, 8, &octal);
    if (is_octal && octal > 07777) { cu_eprintf(io, "chmod: invalid mode: '%s'\n", spec); return 2; }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        uint32_t mode = (uint32_t)octal;
        if (!is_octal) {
            struct stat st;
            int rc = u_stat(argv[i], &st);
            if (rc < 0) { status |= cu_fail(io, "chmod", argv[i], rc); continue; }
            if (!apply_symbolic(spec, st.st_mode, &mode)) { cu_eprintf(io, "chmod: invalid mode: '%s'\n", spec); return 2; }
        }
        int rc = u_chmod(argv[i], mode);
        if (rc < 0) {
            char what[CU_PATH_MAX + 40];
            snprintf(what, sizeof what, "changing permissions of '%s'", argv[i]);
            status |= cu_fail(io, "chmod", what, rc);
        }
    }
    return status;
}

/* ---- chown OWNER[:GROUP] FILE... --------------------------------------------
 * OWNER and GROUP are names from /etc/passwd and /etc/group, or numbers;
 * "OWNER:" leaves the group, ":GROUP" leaves the owner unchanged. */
static bool resolve(const char *file, const char *name, long *id)
{
    uint64_t v;
    if (cu_parse_u64(name, 10, &v) && v < 0x7FFFFFFF) { *id = (long)v; return true; }
    *id = cu_lookup_id(file, name);
    return *id >= 0;
}

int chown_main(struct cu_io *io, int argc, char **argv)
{
    if (argc < 3) { cu_eprintf(io, "usage: chown OWNER[:GROUP] FILE...\n"); return 2; }
    char spec[64];
    if (strlcpy(spec, argv[1], sizeof spec) >= sizeof spec) { cu_eprintf(io, "chown: invalid spec '%s'\n", argv[1]); return 2; }
    char *colon = strchr(spec, ':');
    if (!colon) colon = strchr(spec, '.');
    const char *owner = spec, *group = NULL;
    if (colon) { *colon = 0; group = colon + 1; }
    long uid = -1, gid = -1;
    if (*owner && !resolve("/etc/passwd", owner, &uid)) { cu_eprintf(io, "chown: invalid user: '%s'\n", owner); return 1; }
    if (group && *group && !resolve("/etc/group", group, &gid)) { cu_eprintf(io, "chown: invalid group: '%s'\n", group); return 1; }
    if (uid < 0 && gid < 0) { cu_eprintf(io, "chown: nothing to change in '%s'\n", argv[1]); return 2; }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        int rc = u_chown(argv[i], (int)uid, (int)gid);
        if (rc < 0) {
            char what[CU_PATH_MAX + 40];
            snprintf(what, sizeof what, "changing ownership of '%s'", argv[i]);
            status |= cu_fail(io, "chown", what, rc);
        }
    }
    return status;
}
