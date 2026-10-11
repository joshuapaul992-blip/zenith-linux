/* user/nettest.c -- AF_INET sockets on Kestrel: UDP, TCP, ICMP, interfaces
 *
 *   nettest                       loopback checks only
 *   nettest HOST:PORT             also: TCP and UDP echo through the network
 *                                 card to a server on HOST (QEMU user
 *                                 networking: 10.0.2.2 is the host), ping
 *                                 the gateway, wait for DHCP
 *   nettest serve PORT            accept one connection, read until EOF and
 *                                 answer with the byte count and checksum
 *                                 (driven from the host through hostfwd)
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("nettest:  %s  %s\n", ok_ ? "ok  " : "FAIL", what); \
                            if (!ok_) { failures++; printf("nettest:        (errno %d: %s)\n", errno, strerror(errno)); } } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static struct sockaddr_in sin_of(const char *ip, int port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static int port_of(int fd)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0) return -1;
    return ntohs(a.sin_port);
}

/* A listener on 127.0.0.1, any port; returns the fd, port in *port. */
static int tcp_listener(const char *ip, int *port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = sin_of(ip, *port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 16) < 0) { close(s); return -1; }
    *port = port_of(s);
    return s;
}

static int tcp_connect_to(const char *ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin_of(ip, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { int e = errno; close(s); errno = e; return -1; }
    return s;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static ssize_t read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static uint32_t fnv(const unsigned char *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void fill(unsigned char *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (unsigned char)(seed >> 16); }
}

/* ---- interfaces ----------------------------------------------------------- */
static void test_interfaces(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0, "socket(AF_INET, SOCK_DGRAM)");
    struct ifreq r;
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, "lo");
    CHECK(ioctl(s, SIOCGIFADDR, &r) == 0 && ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "lo is 127.0.0.1 (SIOCGIFADDR)");
    CHECK(ioctl(s, SIOCGIFFLAGS, &r) == 0 && (r.ifr_flags & IFF_UP) && (r.ifr_flags & IFF_LOOPBACK), "lo is up (SIOCGIFFLAGS)");
    CHECK(ioctl(s, SIOCGIFINDEX, &r) == 0 && r.ifr_ifindex == 1, "lo has index 1");
    CHECK(if_nametoindex("lo") == 1, "if_nametoindex(\"lo\")");
    char buf[512];
    struct ifconf ic = { .ifc_len = sizeof buf, .ifc_buf = buf };
    CHECK(ioctl(s, SIOCGIFCONF, &ic) == 0 && ic.ifc_len >= (int)sizeof(struct ifreq), "SIOCGIFCONF lists interfaces");
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, "nosuch0");
    CHECK(ioctl(s, SIOCGIFADDR, &r) < 0 && errno == ENODEV, "unknown interface: ENODEV");
    close(s);
    int fd = open("/proc/net/dev", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (n > 0) buf[n] = 0;
    CHECK(n > 0 && strstr(buf, "lo:"), "/proc/net/dev lists lo");
    if (fd >= 0) close(fd);
}

/* ---- UDP ------------------------------------------------------------------------ */
static void test_udp(void)
{
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in la = sin_of("127.0.0.1", 0);
    CHECK(bind(a, (struct sockaddr *)&la, sizeof la) == 0, "UDP bind 127.0.0.1:0");
    int pa = port_of(a);
    CHECK(pa >= 32768 && pa <= 60999, "UDP: ephemeral port from the Linux range");
    struct sockaddr_in to = sin_of("127.0.0.1", pa);
    CHECK(sendto(b, "hello", 5, 0, (struct sockaddr *)&to, sizeof to) == 5, "UDP sendto (autobinds)");
    struct pollfd pf = { a, POLLIN, 0 };
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "UDP poll: POLLIN");
    int avail = -1;
    CHECK(ioctl(a, FIONREAD, &avail) == 0 && avail == 5, "UDP FIONREAD: next datagram size");
    char buf[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(a, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    CHECK(n == 5 && !memcmp(buf, "hello", 5), "UDP recvfrom: the data");
    CHECK(fl == sizeof from && from.sin_port == htons((uint16_t)port_of(b)) && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "UDP recvfrom: the sender's address");
    /* truncation */
    sendto(b, "0123456789", 10, 0, (struct sockaddr *)&to, sizeof to);
    struct iovec iov = { buf, 4 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    n = recvmsg(a, &m, 0);
    CHECK(n == 4 && (m.msg_flags & MSG_TRUNC), "UDP recvmsg: short buffer sets MSG_TRUNC");
    /* boundaries kept */
    sendto(b, "one", 3, 0, (struct sockaddr *)&to, sizeof to);
    sendto(b, "three", 5, 0, (struct sockaddr *)&to, sizeof to);
    n = recv(a, buf, sizeof buf, 0);
    ssize_t n2 = recv(a, buf + 10, sizeof buf - 10, 0);
    CHECK(n == 3 && n2 == 5, "UDP keeps datagram boundaries");
    /* nonblocking */
    CHECK(recv(a, buf, sizeof buf, MSG_DONTWAIT) < 0 && errno == EAGAIN, "UDP MSG_DONTWAIT on empty: EAGAIN");
    /* a big datagram: fragmented on lo (MTU 16436) and reassembled */
    static unsigned char big[40000], big2[40000];
    fill(big, sizeof big, 7);
    CHECK(sendto(b, big, sizeof big, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof big, "UDP send 40000 bytes");
    n = recv(a, big2, sizeof big2, 0);
    CHECK(n == (ssize_t)sizeof big && !memcmp(big, big2, sizeof big), "UDP 40000-byte datagram reassembled intact");
    /* connected UDP and ICMP port unreachable */
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    int dead = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dl = sin_of("127.0.0.1", 0);
    bind(dead, (struct sockaddr *)&dl, sizeof dl);
    int dport = port_of(dead);
    close(dead);
    struct sockaddr_in dto = sin_of("127.0.0.1", dport);
    CHECK(connect(c, (struct sockaddr *)&dto, sizeof dto) == 0, "UDP connect");
    send(c, "x", 1, 0);
    struct timeval tv = { 2, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(recv(c, buf, sizeof buf, 0) < 0 && errno == ECONNREFUSED, "UDP to a closed port: ECONNREFUSED");
    /* SO_RCVTIMEO */
    tv.tv_sec = 0; tv.tv_usec = 200000;
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    double t0 = now();
    int r = (int)recv(a, buf, sizeof buf, 0);
    double dt = now() - t0;
    CHECK(r < 0 && errno == EAGAIN && dt > 0.15 && dt < 1.0, "SO_RCVTIMEO: EAGAIN after 200 ms");
    /* broadcast needs SO_BROADCAST */
    struct sockaddr_in bc = sin_of("255.255.255.255", 9);
    CHECK(sendto(b, "x", 1, 0, (struct sockaddr *)&bc, sizeof bc) < 0 && errno == EACCES, "UDP broadcast without SO_BROADCAST: EACCES");
    /* EADDRINUSE */
    int d = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in same = sin_of("127.0.0.1", pa);
    CHECK(bind(d, (struct sockaddr *)&same, sizeof same) < 0 && errno == EADDRINUSE, "UDP bind to a used port: EADDRINUSE");
    close(a); close(b); close(c); close(d);
}

/* ---- TCP --------------------------------------------------------------------------- */
#define BULK (8u << 20)

static void test_tcp_basic(void)
{
    int port = 0;
    int l = tcp_listener("127.0.0.1", &port);
    CHECK(l >= 0 && port > 0, "TCP listen on 127.0.0.1:0");
    int on = 0;
    socklen_t ol = sizeof on;
    CHECK(getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &on, &ol) == 0 && on == 1, "SO_ACCEPTCONN on a listener");
    fcntl(l, F_SETFL, O_NONBLOCK);
    CHECK(accept(l, NULL, NULL) < 0 && errno == EAGAIN, "non-blocking accept with nothing queued: EAGAIN");
    fcntl(l, F_SETFL, 0);
    int c = tcp_connect_to("127.0.0.1", port);
    CHECK(c >= 0, "TCP connect to the listener");
    struct sockaddr_in pa;
    socklen_t pl = sizeof pa;
    int s = accept(l, (struct sockaddr *)&pa, &pl);
    CHECK(s >= 0 && pa.sin_port == htons((uint16_t)port_of(c)), "accept: the peer is our client");
    struct sockaddr_in cp;
    socklen_t cl = sizeof cp;
    CHECK(getpeername(c, (struct sockaddr *)&cp, &cl) == 0 && ntohs(cp.sin_port) == port, "getpeername on the client");
    CHECK(write(c, "ping", 4) == 4, "TCP write");
    char buf[64];
    CHECK(read(s, buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4), "TCP read: the bytes");
    CHECK(write(s, "pong!", 5) == 5 && read(c, buf, sizeof buf) == 5, "TCP: the other way");
    /* half close */
    CHECK(shutdown(c, SHUT_WR) == 0, "shutdown(SHUT_WR)");
    CHECK(read(s, buf, sizeof buf) == 0, "the peer reads EOF");
    CHECK(write(s, "after", 5) == 5 && read(c, buf, sizeof buf) == 5, "the peer can still send after our FIN");
    struct pollfd pf = { s, POLLIN | POLLRDHUP, 0 };
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLRDHUP), "poll: POLLRDHUP after the peer's FIN");
    close(s);
    CHECK(read(c, buf, sizeof buf) == 0, "EOF after the peer closes");
    close(c);
    /* refused */
    int dead_port = 0;
    int d = tcp_listener("127.0.0.1", &dead_port);
    close(d);
    CHECK(tcp_connect_to("127.0.0.1", dead_port) < 0 && errno == ECONNREFUSED, "connect to a closed port: ECONNREFUSED");
    /* port in use */
    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin_of("127.0.0.1", port);
    CHECK(bind(l2, (struct sockaddr *)&a, sizeof a) < 0 && errno == EADDRINUSE, "bind to a listening port: EADDRINUSE");
    close(l2);
    /* non-blocking connect */
    int nb = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    a = sin_of("127.0.0.1", port);
    int rc = connect(nb, (struct sockaddr *)&a, sizeof a);
    CHECK(rc == 0 || errno == EINPROGRESS, "non-blocking connect: EINPROGRESS (or done)");
    pf = (struct pollfd){ nb, POLLOUT, 0 };
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLOUT), "poll: POLLOUT once connected");
    int err = -1;
    socklen_t el = sizeof err;
    CHECK(getsockopt(nb, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0, "SO_ERROR is 0");
    s = accept(l, NULL, NULL);
    /* unread data on close: RST */
    write(nb, "unread", 6);
    usleep(50000);
    close(s);
    usleep(50000);
    CHECK(read(nb, buf, sizeof buf) < 0 && errno == ECONNRESET, "close with unread data resets: ECONNRESET");
    close(nb);
    /* TCP_NODELAY round trip */
    c = tcp_connect_to("127.0.0.1", port);
    s = accept(l, NULL, NULL);
    int one = 1;
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0, "TCP_NODELAY");
    double t0 = now();
    for (int i = 0; i < 200; i++) { write(c, "q", 1); read(s, buf, 1); write(s, "r", 1); read(c, buf, 1); }
    double dt = now() - t0;
    printf("nettest:        200 one-byte round trips in %.1f ms\n", dt * 1000);
    CHECK(dt < 2.0, "small request/response is not stalled by delayed ACKs");
    /* epoll */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = s };
    epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
    write(c, "e", 1);
    struct epoll_event out;
    CHECK(epoll_wait(ep, &out, 1, 2000) == 1 && out.data.fd == s, "epoll sees TCP data");
    close(ep); close(c); close(s);
    /* /proc/net/tcp shows the listener (state 0A) */
    int fd = open("/proc/net/tcp", O_RDONLY);
    char pbuf[4096];
    ssize_t n = fd >= 0 ? read(fd, pbuf, sizeof pbuf - 1) : -1;
    if (n > 0) pbuf[n] = 0;
    char want[32];
    snprintf(want, sizeof want, ":%04X 00000000:0000 0A", port);
    CHECK(n > 0 && strstr(pbuf, want), "/proc/net/tcp lists the listener");
    if (fd >= 0) close(fd);
    close(l);
}

/* Bulk transfer through an echo: the child sends BULK bytes and reads
 * them back while the parent echoes. Proves flow control both ways. */
static double bulk_echo(const char *ip, int port, int listener)
{
    static unsigned char out[BULK], in[BULK];
    fill(out, BULK, 12345);
    pid_t pid = -1;
    if (listener >= 0) {
        pid = fork();
        if (pid == 0) {
            int s = accept(listener, NULL, NULL);
            static unsigned char eb[65536];
            ssize_t n;
            while ((n = read(s, eb, sizeof eb)) > 0) if (write_all(s, eb, (size_t)n) < 0) break;
            close(s);
            _exit(0);
        }
    }
    int c = tcp_connect_to(ip, port);
    if (c < 0) { if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); } return -1; }
    double t0 = now();
    pid_t w = fork();
    if (w == 0) { _exit(write_all(c, out, BULK) == 0 ? 0 : 1); }
    ssize_t got = read_all(c, in, BULK);
    int st = 0;
    waitpid(w, &st, 0);
    double dt = now() - t0;
    close(c);
    if (pid > 0) waitpid(pid, NULL, 0);
    if (got != (ssize_t)BULK || memcmp(in, out, BULK) || !WIFEXITED(st) || WEXITSTATUS(st)) return -1;
    return dt;
}

static void test_tcp_bulk(void)
{
    int port = 0;
    int l = tcp_listener("127.0.0.1", &port);
    double dt = bulk_echo("127.0.0.1", port, l);
    CHECK(dt > 0, "TCP loopback: 8 MiB echoed back intact");
    if (dt > 0) printf("nettest:        loopback echo: %.1f MiB/s each way\n", 8.0 / dt);
    close(l);
}

/* ---- through the network card ------------------------------------------------------ */
static int wait_for_dhcp(char *addr, size_t cap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r;
    for (int i = 0; i < 150; i++) {
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, "eth0");
        if (ioctl(s, SIOCGIFADDR, &r) == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, addr, (socklen_t)cap);
            close(s);
            return 0;
        }
        usleep(100000);
    }
    close(s);
    return -1;
}

static uint16_t icmp_csum(const void *d, size_t n)
{
    const uint16_t *w = d;
    uint32_t s = 0;
    for (; n > 1; n -= 2) s += *w++;
    if (n) s += *(const uint8_t *)w;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static void test_ping(const char *gw)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    CHECK(s >= 0, "raw ICMP socket");
    struct sockaddr_in to = sin_of(gw, 0);
    unsigned char pkt[64];
    memset(pkt, 0, sizeof pkt);
    struct icmphdr *ic = (struct icmphdr *)pkt;
    ic->type = ICMP_ECHO;
    ic->un.echo.id = htons(0x4b53);
    ic->un.echo.sequence = htons(1);
    memcpy(pkt + 8, "kestrel ping", 12);
    ic->checksum = icmp_csum(pkt, sizeof pkt);
    double t0 = now();
    CHECK(sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof pkt, "send an ICMP echo request");
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    unsigned char in[256];
    int ok = 0;
    for (int i = 0; i < 5 && !ok; i++) {
        ssize_t n = recv(s, in, sizeof in, 0);
        if (n < 28) break;
        int hl = (in[0] & 15) * 4;
        struct icmphdr *r = (struct icmphdr *)(in + hl);
        ok = r->type == ICMP_ECHOREPLY && r->un.echo.id == htons(0x4b53) && !memcmp(in + hl + 8, "kestrel ping", 12);
    }
    CHECK(ok, "echo reply from the gateway");
    if (ok) printf("nettest:        ping %s: %.2f ms\n", gw, (now() - t0) * 1000);
    close(s);
}

static void test_remote(const char *hostport)
{
    char host[64];
    snprintf(host, sizeof host, "%s", hostport);
    char *colon = strchr(host, ':');
    int port = colon ? atoi(colon + 1) : 7;
    if (colon) *colon = 0;
    char addr[32] = "";
    CHECK(wait_for_dhcp(addr, sizeof addr) == 0, "eth0 has an address (DHCP)");
    printf("nettest:        eth0 is %s\n", addr);
    char rbuf[512];
    int fd = open("/proc/net/route", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, rbuf, sizeof rbuf - 1) : -1;
    if (n > 0) rbuf[n] = 0;
    int dflt = 0;                                       /* a line "ethN<TAB>00000000<TAB>..." */
    for (char *l = n > 0 ? rbuf : NULL; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, "eth", 3) && strstr(l, "\t00000000\t") == strchr(l, '\t')) dflt = 1;
    CHECK(dflt, "/proc/net/route has a default route");
    if (fd >= 0) close(fd);
    test_ping(host);
    double dt = bulk_echo(host, port, -1);
    CHECK(dt > 0, "TCP through the network card: 8 MiB echoed by the host intact");
    if (dt > 0) printf("nettest:        network echo: %.1f MiB/s each way\n", 8.0 / dt);
    /* UDP echo on the same port number */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = sin_of(host, port);
    struct timeval tv = { 2, 0 };
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int good = 0;
    for (int i = 0; i < 20; i++) {
        char msg[32], back[64];
        int len = snprintf(msg, sizeof msg, "datagram %d", i);
        sendto(u, msg, (size_t)len, 0, (struct sockaddr *)&to, sizeof to);
        ssize_t r = recv(u, back, sizeof back, 0);
        if (r == len && !memcmp(back, msg, (size_t)len)) good++;
    }
    CHECK(good >= 18, "UDP through the network card: datagrams echoed by the host");
    printf("nettest:        %d of 20 datagrams came back\n", good);
    close(u);
    /* Name servers: /etc/resolv.conf links to /proc/net/pnp, filled by DHCP */
    fd = open("/etc/resolv.conf", O_RDONLY);
    n = fd >= 0 ? read(fd, rbuf, sizeof rbuf - 1) : -1;
    if (n > 0) rbuf[n] = 0;
    CHECK(n > 0 && strstr(rbuf, "nameserver "), "/etc/resolv.conf names the DHCP name server");
    if (fd >= 0) close(fd);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int g = getaddrinfo("example.com", "80", &hints, &res);
    if (!g) {
        char ip[32];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
        printf("nettest:        example.com is %s (DNS works)\n", ip);
        freeaddrinfo(res);
    } else printf("nettest:        example.com did not resolve: %s (the host may have no DNS)\n", gai_strerror(g));
}

static int serve(int port)
{
    int l = tcp_listener("0.0.0.0", &port);
    if (l < 0) { printf("nettest: serve: cannot listen on %d\n", port); return 1; }
    printf("nettest: serving on port %d\n", port);
    fflush(stdout);
    int s = accept(l, NULL, NULL);
    static unsigned char b[65536];
    uint64_t total = 0;
    uint32_t h = 2166136261u;
    ssize_t n;
    while ((n = read(s, b, sizeof b)) > 0) { h = fnv(b, (size_t)n, h); total += (uint64_t)n; }
    char ans[64];
    int len = snprintf(ans, sizeof ans, "%llu %08x\n", (unsigned long long)total, h);
    write_all(s, ans, (size_t)len);
    close(s);
    close(l);
    printf("nettest: served %llu bytes, fnv %08x\n", (unsigned long long)total, h);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "serve")) return serve(atoi(argv[2]));
    test_interfaces();
    test_udp();
    test_tcp_basic();
    test_tcp_bulk();
    if (argc > 1) test_remote(argv[1]);
    printf(failures ? "nettest: FAILED (%d)\n" : "nettest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
