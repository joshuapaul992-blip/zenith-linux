/* user/kstat.c -- what the kernel spent its time on, over an interval
 *
 *   kstat                 /proc/kstat: totals since boot
 *   kstat N               the change over N seconds
 *   kstat CMD [ARG...]    the change while CMD runs
 *   kstat -p PID [...]    /proc/PID/kstat instead: one process and its threads
 *   kstat -d OLD NEW      the change between two saved copies of a kstat file
 *
 * Every line of the kstat files is "key name=value ...", values cumulative;
 * two snapshots are subtracted field by field (max_* and threads are shown
 * as they are). Lines that did not change are left out. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/wait.h>

#define SNAP_MAX (256 * 1024)

static char *snapshot(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "kstat: %s: %s\n", path, strerror(errno)); exit(1); }
    char *buf = malloc(SNAP_MAX + 1);
    if (!buf) exit(1);
    size_t n = 0;
    ssize_t r;
    while (n < SNAP_MAX && (r = read(fd, buf + n, SNAP_MAX - n)) > 0) n += (size_t)r;   /* one read, normally */
    close(fd);
    buf[n] = 0;
    return buf;
}

/* The line of snapshot s whose key (first word) is key, or NULL. */
static const char *find_line(const char *s, const char *key, size_t klen)
{
    for (const char *l = s; *l; ) {
        if (!strncmp(l, key, klen) && (l[klen] == ' ' || l[klen] == '\n' || !l[klen])) return l;
        const char *e = strchr(l, '\n');
        if (!e) break;
        l = e + 1;
    }
    return NULL;
}

/* The value of field name in line l (up to its end), or -1 if absent. */
static long long field(const char *l, const char *name, size_t nlen)
{
    const char *end = strchr(l, '\n');
    if (!end) end = l + strlen(l);
    for (const char *p = l; p < end; ) {
        while (p < end && *p == ' ') p++;
        size_t tlen = strcspn(p, " \n");
        if (tlen > nlen && !strncmp(p, name, nlen) && p[nlen] == '=') return strtoll(p + nlen + 1, NULL, 10);
        p += tlen;
    }
    return -1;
}

/* max_* and threads are levels, not totals: shown as they are */
static bool cumulative(const char *name, size_t nlen)
{
    return strncmp(name, "max_", 4) && !(nlen == 7 && !strncmp(name, "threads", 7));
}

static void print_diff(const char *a, const char *b)
{
    for (const char *l = b; *l; ) {
        const char *end = strchr(l, '\n');
        size_t len = end ? (size_t)(end - l) : strlen(l);
        if (*l == '#') { printf("%.*s\n", (int)len, l); goto next; }
        size_t klen = strcspn(l, " \n");
        const char *old = find_line(a, l, klen);
        char out[1024];
        int o = snprintf(out, sizeof out, "%.*s", (int)klen, l);
        bool changed = !old;
        for (const char *p = l + klen; p < l + len; ) {
            while (p < l + len && *p == ' ') p++;
            if (p >= l + len) break;
            const char *eq = memchr(p, '=', (size_t)(l + len - p));
            size_t tlen = strcspn(p, " \n");
            if (!eq || eq > p + tlen) { o += snprintf(out + o, sizeof out - (size_t)o, " %.*s", (int)tlen, p); p += tlen; continue; }
            size_t nlen = (size_t)(eq - p);
            long long v = strtoll(eq + 1, NULL, 10);
            if (old && cumulative(p, nlen)) {
                long long w = field(old, p, nlen);
                if (w >= 0) v -= w;
            }
            if (v && cumulative(p, nlen)) changed = true;
            if (o < (int)sizeof out) o += snprintf(out + o, sizeof out - (size_t)o, " %.*s=%lld", (int)nlen, p, v);
            p += tlen;
        }
        if (changed) printf("%s\n", out);
    next:
        if (!end) break;
        l = end + 1;
    }
}

int main(int argc, char **argv)
{
    char path[64] = "/proc/kstat";
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-p")) {
        snprintf(path, sizeof path, "/proc/%s/kstat", argv[i + 1]);
        i += 2;
    }
    if (i + 2 < argc && !strcmp(argv[i], "-d")) {
        print_diff(snapshot(argv[i + 1]), snapshot(argv[i + 2]));
        return 0;
    }
    if (i < argc && (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))) {
        fprintf(stderr, "usage: kstat [-p PID] [SECONDS | COMMAND [ARG...]]\n       kstat -d OLD NEW\n");
        return 2;
    }
    if (i >= argc) {                                    /* totals since boot */
        char *s = snapshot(path);
        fputs(s, stdout);
        return 0;
    }
    char *a = snapshot(path);
    int status = 0;
    char *end;
    long secs = strtol(argv[i], &end, 10);
    if (*argv[i] && !*end && secs >= 0) {
        sleep((unsigned)secs);
    } else {
        pid_t pid = fork();
        if (pid < 0) { perror("kstat: fork"); return 1; }
        if (pid == 0) {
            execvp(argv[i], argv + i);
            fprintf(stderr, "kstat: %s: %s\n", argv[i], strerror(errno));
            _exit(127);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    char *b = snapshot(path);
    print_diff(a, b);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
