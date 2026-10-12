/* user/wdtest.c -- the Linux watchdog interface (/dev/watchdog) on Kestrel
 *
 *   wdtest            checks: identity, timeout, keepalive pings past the
 *                     timeout, a second open is refused, the magic close
 *   wdtest starve [N] arm the watchdog with an N-second timeout (default 3),
 *                     ping it twice, then stop: the machine should reset
 * The driver behind it is a Linux driver built through the Linux KPI
 * (QEMU's -device i6300esb: drivers/watchdog/i6300esb.c).
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

/* <linux/watchdog.h>, defined here: musl-gcc has no kernel headers */
struct watchdog_info {
    unsigned int  options;
    unsigned int  firmware_version;
    unsigned char identity[32];
};
#define WDIOC_GETSUPPORT     _IOR('W', 0, struct watchdog_info)
#define WDIOC_GETSTATUS      _IOR('W', 1, int)
#define WDIOC_GETBOOTSTATUS  _IOR('W', 2, int)
#define WDIOC_SETOPTIONS     _IOR('W', 4, int)
#define WDIOC_KEEPALIVE      _IOR('W', 5, int)
#define WDIOC_SETTIMEOUT     _IOWR('W', 6, int)
#define WDIOC_GETTIMEOUT     _IOR('W', 7, int)
#define WDIOC_GETTIMELEFT    _IOR('W', 10, int)
#define WDIOF_SETTIMEOUT     0x0080
#define WDIOF_MAGICCLOSE     0x0100
#define WDIOF_KEEPALIVEPING  0x8000
#define WDIOS_DISABLECARD    0x0001
#define WDIOS_ENABLECARD     0x0002

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int starve(int timeout)
{
    int fd = open("/dev/watchdog", O_WRONLY);
    if (fd < 0) { printf("wdtest: /dev/watchdog: %s\n", strerror(errno)); return 1; }
    if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout) < 0) printf("wdtest: SETTIMEOUT: %s\n", strerror(errno));
    for (int i = 0; i < 2; i++) {
        ioctl(fd, WDIOC_KEEPALIVE, 0);
        printf("wdtest: ping %d (timeout %d s)\n", i + 1, timeout);
        sleep(1);
    }
    printf("wdtest: no more pings: the watchdog should reset the machine in about %d s\n", timeout);
    fflush(stdout);
    double t0 = now();
    for (;;) {                                  /* keep the file open: no magic close */
        sleep(1);
        printf("wdtest: still alive after %.0f s\n", now() - t0);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    struct watchdog_info info;
    int v, fd;

    if (argc > 1 && !strcmp(argv[1], "starve")) return starve(argc > 2 ? atoi(argv[2]) : 3);

    fd = open("/dev/watchdog", O_WRONLY);
    CHECK(fd >= 0, "open /dev/watchdog");
    if (fd < 0) {
        printf("wdtest: %s; %d checks failed\n", strerror(errno), failures);
        return 1;
    }

    memset(&info, 0, sizeof info);
    CHECK(ioctl(fd, WDIOC_GETSUPPORT, &info) == 0, "WDIOC_GETSUPPORT");
    printf("  identity \"%.32s\", options %#x, firmware %u\n", info.identity, info.options, info.firmware_version);
    CHECK(info.identity[0] != 0, "the driver has an identity");
    CHECK((info.options & (WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE)) ==
          (WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE), "settimeout, keepalive and magic close supported");

    v = -1;
    CHECK(ioctl(fd, WDIOC_GETTIMEOUT, &v) == 0 && v > 0, "WDIOC_GETTIMEOUT");
    printf("  default timeout %d s\n", v);
    v = 3;
    CHECK(ioctl(fd, WDIOC_SETTIMEOUT, &v) == 0 && v == 3, "WDIOC_SETTIMEOUT 3 s");
    v = -1;
    CHECK(ioctl(fd, WDIOC_GETTIMEOUT, &v) == 0 && v == 3, "the timeout reads back as 3 s");
    v = 0;
    CHECK(ioctl(fd, WDIOC_SETTIMEOUT, &v) < 0 && errno == EINVAL, "a zero timeout is refused (EINVAL)");
    v = -1;
    CHECK(ioctl(fd, WDIOC_GETSTATUS, &v) == 0, "WDIOC_GETSTATUS");
    CHECK(ioctl(fd, WDIOC_GETBOOTSTATUS, &v) == 0, "WDIOC_GETBOOTSTATUS");
    CHECK(ioctl(fd, 0x5401 /* TCGETS */, &v) < 0 && errno == ENOTTY, "an unknown ioctl is ENOTTY");

    /* a second opener is refused while the first holds it */
    int fd2 = open("/dev/watchdog", O_WRONLY);
    CHECK(fd2 < 0 && errno == EBUSY, "a second open of /dev/watchdog is EBUSY");
    if (fd2 >= 0) close(fd2);
    fd2 = open("/dev/watchdog0", O_WRONLY);
    CHECK(fd2 < 0 && errno == EBUSY, "/dev/watchdog0 is the same device: EBUSY");
    if (fd2 >= 0) close(fd2);

    /* 6 s of pings, twice the timeout: the machine must survive it */
    double t0 = now();
    int pings = 0;
    while (now() - t0 < 6.0) {
        if (pings % 2) CHECK(write(fd, "x", 1) == 1, "ping by write");
        else CHECK(ioctl(fd, WDIOC_KEEPALIVE, 0) == 0, "ping by WDIOC_KEEPALIVE");
        pings++;
        usleep(700000);
    }
    printf("  alive after %.1f s and %d pings with a 3 s timeout\n", now() - t0, pings);

    /* disable/enable by SETOPTIONS */
    v = WDIOS_DISABLECARD;
    CHECK(ioctl(fd, WDIOC_SETOPTIONS, &v) == 0, "WDIOS_DISABLECARD");
    sleep(4);                                   /* stopped: past the timeout, no reset */
    v = WDIOS_ENABLECARD;
    CHECK(ioctl(fd, WDIOC_SETOPTIONS, &v) == 0, "WDIOS_ENABLECARD");
    CHECK(ioctl(fd, WDIOC_KEEPALIVE, 0) == 0, "ping after re-enabling");

    /* the magic close: 'V' then close stops it */
    CHECK(write(fd, "V", 1) == 1, "write the magic 'V'");
    CHECK(close(fd) == 0, "close");
    sleep(4);                                   /* stopped: no reset */
    printf("  alive 4 s after the magic close\n");

    fd = open("/dev/watchdog0", O_WRONLY);
    CHECK(fd >= 0, "open /dev/watchdog0 once it is free");
    if (fd >= 0) {
        CHECK(write(fd, "V", 1) == 1, "magic close on /dev/watchdog0");
        close(fd);
    }

    if (failures) printf("wdtest: %d checks failed\n", failures);
    else printf("wdtest: all checks passed\n");
    return failures ? 1 : 0;
}
