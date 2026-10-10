/* user/dyntest.c -- a dynamically linked program on Kestrel: PT_INTERP (musl's
 * /lib/ld-musl-x86_64.so.1), a shared library from /lib with data, TLS and
 * relocations, dlopen/dlsym, and segment permissions as the loader set them
 * (RELRO read-only, no writable+executable mapping). Exit 0 = all passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

extern int dyn_counter;
extern __thread int dyn_tls;
int dyn_add(int a, int b);
const char *dyn_name(void);
int *dyn_tls_addr(void);

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    printf("dyntest: %s, pid %d\n", argv[0], getpid());
    CHECK(dyn_add(2, 3) == 5 && dyn_counter == 101, "call into libdyntest.so; its data is shared with us");
    CHECK(!strcmp(dyn_name(), "libdyntest"), "relocated pointers in the library");
    CHECK(dyn_tls == 7 && dyn_tls_addr() == &dyn_tls, "thread-local storage of the library");
    void *h = dlopen("libdyntest.so", RTLD_NOW);
    int (*add)(int, int) = h ? (int (*)(int, int))dlsym(h, "dyn_add") : NULL;
    CHECK(add && add(40, 2) == 42 && dyn_counter == 102, "dlopen + dlsym find the same library");
    CHECK(!dlopen("libnothere.so", RTLD_NOW) && dlerror(), "dlopen of a missing library fails cleanly");

    char maps[8192] = { 0 };
    int fd = open("/proc/self/maps", O_RDONLY);
    ssize_t n = read(fd, maps, sizeof maps - 1);
    close(fd);
    CHECK(n > 0 && strstr(maps, "ld-musl-x86_64.so.1") && strstr(maps, "libdyntest.so"),
          "/proc/self/maps shows the loader and the library");
    int wx = 0;
    for (char *l = maps; *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char perm[5] = { 0 };
        if (sscanf(l, "%*s %4s", perm) == 1 && perm[1] == 'w' && perm[2] == 'x') wx = 1;
        if (!e) break;
        l = e + 1;
    }
    CHECK(!wx, "no mapping is writable and executable");
    printf(failures ? "dyntest: FAILED (%d)\n" : "dyntest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
