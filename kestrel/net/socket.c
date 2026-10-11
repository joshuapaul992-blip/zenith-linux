/* net/socket.c -- AF_INET sockets: the file side (see include/kernel/inet.h)
 *
 * A socket is an anonymous VSOCK vnode inside a struct isock, like the
 * AF_UNIX ones. Binding checks the address is ours and the port free
 * (SO_REUSEADDR as on Linux: a listener's port stays taken, a closing
 * connection's does not); ports below 1024 need uid 0. Unbound sockets get
 * an ephemeral port (32768-60999) when they first send or connect.
 *
 * The interface ioctls (SIOCGIFADDR and friends, SIOCADDRT) work on any
 * socket, so ifconfig and route from BusyBox can read and change the
 * configuration. */
#include "sock.h"
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/time.h>

#define UDP_RCVBUF      212992
#define EPHEMERAL_LO    32768
#define EPHEMERAL_HI    60999

static const struct vnode_ops inet_ops;

bool inet_is(struct file *f) { return f && f->vn && f->vn->ops == &inet_ops; }
int  inet_type(struct file *f) { return isk(f)->type; }

int tcp_sock_error(struct isock *s, bool clear);

/* ---- blocking ------------------------------------------------------------------ */
uint64_t sock_deadline(uint32_t timeo_ms) { return timeo_ms ? time_ms() + timeo_ms : 0; }

int sock_wait(void *chan, uint64_t deadline)
{
    /* What we sent to ourselves may be what we wait for: deliver it, and
     * let the caller look again instead of sleeping through its wakeup. */
    if (net_lo_flush()) return 0;
    if (signal_pending()) return -ERESTARTSYS;
    if (deadline && time_ms() >= deadline) return -EAGAIN;
    sleep_on_until(chan, deadline);
    if (signal_pending()) return -ERESTARTSYS;
    return 0;
}

/* ---- ports ------------------------------------------------------------------------ */
bool port_in_use(int proto, uint32_t addr, uint16_t port, struct isock *except, bool reuse)
{
    if (proto == IPPROTO_TCP) return tcp_port_used(addr, port, except, reuse);
    return dgram_port_used(proto, addr, port, except, reuse);
}

uint16_t port_ephemeral(int proto)
{
    static uint32_t next;
    if (!next) next = (uint32_t)(rdtsc() % (EPHEMERAL_HI - EPHEMERAL_LO + 1));
    for (int i = 0; i <= EPHEMERAL_HI - EPHEMERAL_LO; i++) {
        uint16_t p = (uint16_t)(EPHEMERAL_LO + (next++ % (EPHEMERAL_HI - EPHEMERAL_LO + 1)));
        if (!port_in_use(proto, 0, be16(p), NULL, false)) return be16(p);
    }
    return 0;
}

/* ---- creation --------------------------------------------------------------------- */
struct isock *isock_new(int type, int proto)
{
    struct isock *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->type = type;
    s->proto = proto;
    s->ttl = 64;
    s->rcvbuf = UDP_RCVBUF;
    s->sndbuf = UDP_RCVBUF;
    s->linger = -1;
    strlcpy(s->vn.name, "socket", sizeof s->vn.name);
    s->vn.type = VSOCK;
    s->vn.mode = S_IFSOCK | 0777;
    s->vn.ops = &inet_ops;
    s->vn.data = s;
    return s;
}

int inet_create(int type, int protocol, struct file **out)
{
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    int proto;
    switch (type) {
    case SOCK_STREAM:
        if (protocol && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
        proto = IPPROTO_TCP;
        break;
    case SOCK_DGRAM:
        if (protocol && protocol != IPPROTO_UDP && protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        proto = protocol ? protocol : IPPROTO_UDP;
        break;
    case SOCK_RAW:
        if (protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        if (current_task()->uid != 0) return -EPERM;
        proto = IPPROTO_ICMP;
        break;
    default:
        return -ESOCKTNOSUPPORT;
    }
    struct isock *s = isock_new(type, proto);
    if (!s) return -ENOMEM;
    uint64_t f = irq_save();
    if (proto == IPPROTO_TCP) {
        int rc = tcp_attach(s);
        if (rc) { irq_restore(f); kfree(s); return rc; }
    } else {
        dgram_register(s);
    }
    irq_restore(f);
    struct file *file = vfs_file_new(&s->vn, O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
    if (!file) { uint64_t g = irq_save(); if (s->tcb) tcp_close(s); else dgram_unregister(s); irq_restore(g); kfree(s); return -ENOMEM; }
    *out = file;
    return 0;
}

/* ---- addresses -------------------------------------------------------------------- */
static int get_sin(const void *addr, uint32_t len, struct sockaddr_in *sin)
{
    if (len < sizeof(struct sockaddr_in)) return -EINVAL;
    memcpy(sin, addr, sizeof *sin);
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;
    return 0;
}

static int autobind(struct isock *s)
{
    if (s->lport) return 0;
    if (s->proto == IPPROTO_ICMP) {                     /* ping sockets: an identifier, any unused one */
        s->lport = port_ephemeral(IPPROTO_ICMP);
        return s->lport ? 0 : -EAGAIN;
    }
    s->lport = port_ephemeral(s->proto);
    if (!s->lport) return -EAGAIN;
    if (s->tcb) tcp_bind_check(s, s->laddr, s->lport);
    return 0;
}

int inet_bind(struct file *f, const void *addr, uint32_t len)
{
    struct isock *s = isk(f);
    struct sockaddr_in sin;
    if (len < sizeof sin) return -EINVAL;
    memcpy(&sin, addr, sizeof sin);
    if (sin.sin_family != AF_INET && !(sin.sin_family == 0 && sin.sin_addr == 0)) return -EAFNOSUPPORT;
    uint64_t fl = irq_save();
    int rc = 0;
    if (s->lport && s->type != SOCK_RAW) { rc = -EINVAL; goto out; }
    if (sin.sin_addr && !ip_is_local(sin.sin_addr) && sin.sin_addr != 0xffffffffu && (sin.sin_addr & 0xf0) != 0xe0) {
        rc = -EADDRNOTAVAIL; goto out;
    }
    if (s->type == SOCK_RAW) { s->laddr = sin.sin_addr; goto out; }
    uint16_t port = sin.sin_port;
    if (port && be16(port) < 1024 && current_task()->uid != 0) { rc = -EACCES; goto out; }
    if (!port) {
        port = port_ephemeral(s->proto);
        if (!port) { rc = -EADDRINUSE; goto out; }
    } else if (port_in_use(s->proto, sin.sin_addr, port, s, s->reuseaddr || s->reuseport)) {
        rc = -EADDRINUSE; goto out;
    }
    if (s->tcb && (rc = tcp_bind_check(s, sin.sin_addr, port))) goto out;
    s->laddr = sin.sin_addr;
    s->lport = port;
out:
    irq_restore(fl);
    return rc;
}

int inet_listen(struct file *f, int backlog)
{
    struct isock *s = isk(f);
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    uint64_t fl = irq_save();
    int rc = tcp_listen(s, backlog);
    irq_restore(fl);
    return rc;
}

static int inet_connect_(struct file *f, const void *addr, uint32_t len)
{
    struct isock *s = isk(f);
    struct sockaddr_in sin;
    if (len >= 2) {
        uint16_t fam;
        memcpy(&fam, addr, 2);
        if (fam == 0 && s->type != SOCK_STREAM) {       /* AF_UNSPEC: dissolve the association */
            s->connected = false;
            s->raddr = 0; s->rport = 0;
            return 0;
        }
    }
    int rc = get_sin(addr, len, &sin);
    if (rc) return rc;
    if (s->type == SOCK_STREAM) return tcp_connect(s, f, sin.sin_addr, sin.sin_port);
    uint64_t fl = irq_save();
    if (sin.sin_addr == 0xffffffffu && !s->broadcast) { irq_restore(fl); return -EACCES; }
    if ((rc = autobind(s))) { irq_restore(fl); return rc; }
    uint32_t dst = sin.sin_addr ? sin.sin_addr : IP4(127, 0, 0, 1);
    if (!s->laddr && !ip_source_for(dst) && dst != 0xffffffffu) { irq_restore(fl); return -ENETUNREACH; }
    s->raddr = dst;
    s->rport = sin.sin_port;
    s->connected = true;
    dgram_flush(s);                                     /* Linux keeps them; nobody depends on it */
    irq_restore(fl);
    return 0;
}

static int inet_accept_(struct file *f, int flags, struct file **out, struct sockaddr_in *peer)
{
    struct isock *s = isk(f);
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    struct isock *ns;
    int rc = tcp_accept(s, f, &ns);
    if (rc) return rc;
    struct file *nf = vfs_file_new(&ns->vn, O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
    if (!nf) { uint64_t g = irq_save(); tcp_close(ns); irq_restore(g); kfree(ns); return -ENOMEM; }
    if (peer) {
        memset(peer, 0, sizeof *peer);
        peer->sin_family = AF_INET;
        peer->sin_addr = ns->raddr;
        peer->sin_port = ns->rport;
    }
    *out = nf;
    return 0;
}

static long inet_sendto_(struct file *f, const void *buf, size_t len, int flags, const void *to, uint32_t tolen)
{
    struct isock *s = isk(f);
    if (s->type == SOCK_STREAM) {
        return tcp_send(s, f, buf, len, flags);       /* an address is ignored, as on Linux */
    }
    if (s->shut_wr) return -EPIPE;
    uint32_t dst;
    uint16_t dport;
    if (to && tolen) {
        struct sockaddr_in sin;
        int rc = get_sin(to, tolen, &sin);
        if (rc) return rc;
        dst = sin.sin_addr;
        dport = sin.sin_port;
    } else {
        if (!s->connected) return -EDESTADDRREQ;
        dst = s->raddr;
        dport = s->rport;
    }
    uint64_t fl = irq_save();
    long rc = autobind(s);
    if (!rc) {
        if (s->err) { rc = -s->err; s->err = 0; }       /* report an ICMP error from an earlier send */
        else if (s->proto == IPPROTO_UDP) rc = udp_send(s, buf, len, dst, dport);
        else rc = icmp_sock_send(s, buf, len, dst);
    }
    irq_restore(fl);
    return rc;
}

static long inet_recvfrom_(struct file *f, void *buf, size_t len, int flags, struct sockaddr_in *from, int *msg_flags)
{
    struct isock *s = isk(f);
    if (s->type == SOCK_STREAM) {
        long n = tcp_recv(s, f, buf, len, flags);
        if (from && n >= 0) {
            memset(from, 0, sizeof *from);
            from->sin_family = AF_INET;
            from->sin_addr = s->raddr;
            from->sin_port = s->rport;
        }
        return n;
    }
    return dgram_recv(s, f, buf, len, flags, from, msg_flags);
}

static int inet_shutdown_(struct file *f, int how)
{
    struct isock *s = isk(f);
    if (s->type == SOCK_STREAM) return tcp_shutdown(s, how);
    if (!s->connected) return -ENOTCONN;
    if (how != SHUT_WR) s->shut_rd = true;
    if (how != SHUT_RD) s->shut_wr = true;
    wakeup(s);
    return 0;
}

int inet_name(struct file *f, bool peer, struct sockaddr_in *out)
{
    struct isock *s = isk(f);
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    if (peer) {
        bool conn = s->type == SOCK_STREAM ? s->tcb && tcp_info_state(s) != 7 && tcp_info_state(s) != 10 &&
                                                 tcp_info_state(s) != 2 && tcp_info_state(s) != 3
                                           : s->connected;
        if (!conn) return -ENOTCONN;
        out->sin_addr = s->raddr;
        out->sin_port = s->rport;
        return 0;
    }
    out->sin_port = s->lport;
    out->sin_addr = s->laddr;
    if (s->type == SOCK_STREAM && s->tcb && !s->laddr) {
        extern uint32_t tcp_local_addr(struct isock *s);
        out->sin_addr = tcp_local_addr(s);
    } else if (!s->laddr && s->connected) out->sin_addr = ip_source_for(s->raddr);
    if (s->type == SOCK_DGRAM && s->proto == IPPROTO_ICMP) out->sin_port = s->lport;
    return 0;
}

/* ---- options ------------------------------------------------------------------------ */
static int opt_int(const void *val, uint32_t len, int *out)
{
    if (len < 4) { if (len == 1) { *out = *(const uint8_t *)val; return 0; } return -EINVAL; }
    memcpy(out, val, 4);
    return 0;
}

static int inet_setsockopt_(struct file *f, int level, int opt, const void *val, uint32_t len)
{
    struct isock *s = isk(f);
    int v = 0;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            if (len < 16) return -EINVAL;
            int64_t tv[2];
            memcpy(tv, val, 16);
            if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) return -EDOM;
            uint64_t ms = (uint64_t)tv[0] * 1000 + (uint64_t)(tv[1] + 999) / 1000;
            if (ms > 0xffffffffu) ms = 0;
            if (opt == SO_RCVTIMEO) s->rcvtimeo = (uint32_t)ms; else s->sndtimeo = (uint32_t)ms;
            return 0;
        }
        case SO_LINGER: {
            if (len < 8) return -EINVAL;
            int32_t l[2];
            memcpy(l, val, 8);
            s->linger = l[0] ? l[1] : -1;
            return 0;
        }
        case SO_BINDTODEVICE: return 0;                 /* one route table: nothing to bind */
        }
        int rc = opt_int(val, len, &v);
        if (rc) return rc;
        switch (opt) {
        case SO_REUSEADDR: s->reuseaddr = v != 0; return 0;
        case SO_REUSEPORT: s->reuseport = v != 0; return 0;
        case SO_BROADCAST: s->broadcast = v != 0; return 0;
        case SO_KEEPALIVE:
            s->keepalive = v != 0;
            if (s->tcb) { uint64_t fl = irq_save(); tcp_setopt_nodelay(s); irq_restore(fl); }
            return 0;
        case SO_RCVBUF: case SO_RCVBUFFORCE:
            if (v < 256) v = 256;
            s->rcvbuf = v > (8 << 20) ? (8u << 20) : (uint32_t)v * 2;   /* Linux doubles it */
            return 0;
        case SO_SNDBUF: case SO_SNDBUFFORCE:
            if (v < 256) v = 256;
            s->sndbuf = v > (8 << 20) ? (8u << 20) : (uint32_t)v * 2;
            return 0;
        case SO_DEBUG: case SO_DONTROUTE: case SO_OOBINLINE: case SO_TIMESTAMP: case SO_RCVLOWAT: case SO_SNDLOWAT:
            return 0;
        }
        return -ENOPROTOOPT;
    }
    if (level == SOL_IP) {
        int rc = opt_int(val, len, &v);
        if (rc) return rc;
        switch (opt) {
        case IP_TTL: if (v == -1) v = 64; if (v < 1 || v > 255) return -EINVAL; s->ttl = (uint8_t)v; break;
        case IP_TOS: s->tos = (uint8_t)v; break;
        case IP_HDRINCL: if (s->type != SOCK_RAW) return -ENOPROTOOPT; if (v) return -EOPNOTSUPP; break;
        case IP_RECVERR: case IP_MTU_DISCOVER: case IP_MULTICAST_TTL: case IP_MULTICAST_LOOP: break;
        default: return -ENOPROTOOPT;
        }
        if (s->tcb) { uint64_t fl = irq_save(); tcp_setopt_nodelay(s); irq_restore(fl); }
        return 0;
    }
    if (level == SOL_TCP) {
        if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
        int rc = opt_int(val, len, &v);
        if (rc) return rc;
        switch (opt) {
        case TCP_NODELAY:
            s->nodelay = v != 0;
            { uint64_t fl = irq_save(); tcp_setopt_nodelay(s); irq_restore(fl); }
            return 0;
        case TCP_CORK: case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT: case TCP_QUICKACK:
        case TCP_MAXSEG: case TCP_USER_TIMEOUT: case TCP_FASTOPEN:
            return 0;
        }
        return -ENOPROTOOPT;
    }
    if (level == SOL_UDP) return 0;
    return -ENOPROTOOPT;
}

int inet_getsockopt(struct file *f, int level, int opt, void *val, uint32_t *len)
{
    struct isock *s = isk(f);
    int v;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_TYPE:      v = s->type; break;
        case SO_PROTOCOL:  v = s->proto; break;
        case SO_DOMAIN:    v = AF_INET; break;
        case SO_ERROR: {
            uint64_t fl = irq_save();
            v = s->err;
            s->err = 0;
            if (!v && s->tcb) v = tcp_sock_error(s, true);
            irq_restore(fl);
            break;
        }
        case SO_REUSEADDR: v = s->reuseaddr; break;
        case SO_REUSEPORT: v = s->reuseport; break;
        case SO_BROADCAST: v = s->broadcast; break;
        case SO_KEEPALIVE: v = s->keepalive; break;
        case SO_ACCEPTCONN: v = s->tcb && tcp_info_state(s) == 10; break;
        case SO_RCVBUF:    v = s->tcb ? 131072 : (int)s->rcvbuf; break;
        case SO_SNDBUF:    v = s->tcb ? 131072 : (int)s->sndbuf; break;
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            uint32_t ms = opt == SO_RCVTIMEO ? s->rcvtimeo : s->sndtimeo;
            int64_t tv[2] = { ms / 1000, (int64_t)(ms % 1000) * 1000 };
            if (*len < 16) return -EINVAL;
            memcpy(val, tv, 16);
            *len = 16;
            return 0;
        }
        case SO_LINGER: {
            int32_t l[2] = { s->linger >= 0, s->linger >= 0 ? s->linger : 0 };
            if (*len < 8) return -EINVAL;
            memcpy(val, l, 8);
            *len = 8;
            return 0;
        }
        default: return -ENOPROTOOPT;
        }
    } else if (level == SOL_IP) {
        switch (opt) {
        case IP_TTL: v = s->ttl; break;
        case IP_TOS: v = s->tos; break;
        default: return -ENOPROTOOPT;
        }
    } else if (level == SOL_TCP) {
        if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
        switch (opt) {
        case TCP_NODELAY: v = s->nodelay; break;
        case TCP_MAXSEG:  v = 1460; break;
        case TCP_INFO: {                                /* the first byte is the state */
            if (!*len) return -EINVAL;
            uint32_t n = *len < 104 ? *len : 104;
            memset(val, 0, n);
            ((uint8_t *)val)[0] = (uint8_t)tcp_info_state(s);
            *len = n;
            return 0;
        }
        default: return -ENOPROTOOPT;
        }
    } else return -ENOPROTOOPT;
    if (*len < 4) return -EINVAL;
    memcpy(val, &v, 4);
    *len = 4;
    return 0;
}

/* ---- interface ioctls ------------------------------------------------------------------- */
#define SIOCADDRT       0x890B
#define SIOCDELRT       0x890C
#define SIOCGIFNAME     0x8910
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFBRDADDR  0x8919
#define SIOCSIFBRDADDR  0x891A
#define SIOCGIFNETMASK  0x891B
#define SIOCSIFNETMASK  0x891C
#define SIOCGIFMETRIC   0x891D
#define SIOCGIFMTU      0x8921
#define SIOCSIFMTU      0x8922
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCGIFTXQLEN   0x8942

struct ifreq_k {
    char name[IFNAMSIZ];
    union {
        struct sockaddr_in addr;
        struct { uint16_t family; uint8_t data[14]; } hw;
        int16_t flags;
        int32_t ival;
        uint8_t pad[24];
    } u;
};
_Static_assert(sizeof(struct ifreq_k) == 40, "struct ifreq is 40 bytes");

/* struct rtentry (x86_64): rt_pad1, rt_dst, rt_gateway, rt_genmask, rt_flags ... */
struct rtentry_k {
    uint64_t pad1;
    struct sockaddr_in dst, gw, mask;
    uint16_t flags;
    uint8_t  rest[120 - 8 - 48 - 2];
};
_Static_assert(sizeof(struct rtentry_k) == 120, "struct rtentry is 120 bytes");
#define RTF_UP      0x1
#define RTF_GATEWAY 0x2
#define RTF_HOST    0x4

bool net_ioctl_is(unsigned long req)
{
    return (req >= 0x8910 && req <= 0x8942) || req == SIOCADDRT || req == SIOCDELRT;
}

size_t net_ioctl_size(unsigned long req, const void *karg)
{
    (void)karg;
    if (req == SIOCADDRT || req == SIOCDELRT) return sizeof(struct rtentry_k);
    if (req == SIOCGIFCONF) return 16;
    return sizeof(struct ifreq_k);
}

static void set_sin(struct sockaddr_in *a, uint32_t ip)
{
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr = ip;
}

/* SIOCGIFCONF fills a user array; the system call layer passes a kernel
 * buffer of `len` bytes in ifc->buf's place (see kernel/syscall.c). */
int net_ifconf(void *kbuf, int len)
{
    int n = 0;
    for (struct netif *i = netif_list(); i; i = i->next) {
        if (!i->ip) continue;
        if (kbuf) {
            if ((n + 1) * (int)sizeof(struct ifreq_k) > len) break;
            struct ifreq_k *r = (struct ifreq_k *)kbuf + n;
            memset(r, 0, sizeof *r);
            strlcpy(r->name, i->name, IFNAMSIZ);
            set_sin(&r->u.addr, i->ip);
        }
        n++;
    }
    return n * (int)sizeof(struct ifreq_k);
}

int net_ioctl(unsigned long req, void *arg)
{
    if (req == SIOCADDRT || req == SIOCDELRT) {
        struct rtentry_k *rt = arg;
        if (current_task()->uid != 0) return -EPERM;
        uint32_t dst = rt->dst.sin_addr, mask = (rt->flags & RTF_HOST) ? 0xffffffffu : rt->mask.sin_addr;
        uint32_t gw = (rt->flags & RTF_GATEWAY) ? rt->gw.sin_addr : 0;
        uint64_t fl = irq_save();
        int rc;
        if (req == SIOCDELRT) rc = route_del(dst, mask) ? -ESRCH : 0;
        else {
            struct netif *n = gw ? route_lookup(gw, NULL) : NULL;
            if (!n) for (n = netif_list(); n && (n == netif_loopback() || !n->ip); n = n->next) {}
            rc = !n ? -ENETUNREACH : route_add(dst, mask, gw, n) ? -ENOSPC : 0;
        }
        irq_restore(fl);
        return rc;
    }
    struct ifreq_k *r = arg;
    r->name[IFNAMSIZ - 1] = 0;
    if (req == SIOCGIFNAME) {
        struct netif *n = netif_by_index(r->u.ival);
        if (!n) return -ENODEV;
        strlcpy(r->name, n->name, IFNAMSIZ);
        return 0;
    }
    struct netif *n = netif_by_name(r->name);
    if (!n) return -ENODEV;
    bool set = req == SIOCSIFFLAGS || req == SIOCSIFADDR || req == SIOCSIFNETMASK || req == SIOCSIFMTU ||
               req == SIOCSIFBRDADDR;
    if (set && current_task()->uid != 0) return -EPERM;
    uint64_t fl = irq_save();
    int rc = 0;
    switch (req) {
    case SIOCGIFFLAGS:   r->u.flags = (int16_t)n->flags; break;
    case SIOCSIFFLAGS: {
        uint32_t nf = (uint16_t)r->u.flags;
        if (n == netif_loopback()) nf |= IFF_LOOPBACK;
        bool was_up = n->flags & IFF_UP;
        n->flags = (n->flags & ~(uint32_t)(IFF_UP | IFF_RUNNING)) | (nf & IFF_UP);
        if (!(nf & IFF_UP) && was_up) arp_flush_if(n);
        if ((nf & IFF_UP) && (!n->link_up || n->link_up(n))) n->flags |= IFF_RUNNING;   /* lo has no link to lose */
        break;
    }
    case SIOCGIFADDR:    if (!n->ip) { rc = -EADDRNOTAVAIL; break; } set_sin(&r->u.addr, n->ip); break;
    case SIOCSIFADDR: {
        uint32_t mask = n->mask;
        if (!mask) {                                    /* classful default, as ifconfig expects */
            uint8_t a = (uint8_t)r->u.addr.sin_addr;
            mask = a < 128 ? IP4(255, 0, 0, 0) : a < 192 ? IP4(255, 255, 0, 0) : IP4(255, 255, 255, 0);
        }
        netif_set_addr(n, r->u.addr.sin_addr, mask);
        break;
    }
    case SIOCGIFNETMASK: if (!n->ip) { rc = -EADDRNOTAVAIL; break; } set_sin(&r->u.addr, n->mask); break;
    case SIOCSIFNETMASK: netif_set_addr(n, n->ip, r->u.addr.sin_addr); break;
    case SIOCGIFBRDADDR: if (!n->ip) { rc = -EADDRNOTAVAIL; break; } set_sin(&r->u.addr, n->ip | ~n->mask); break;
    case SIOCSIFBRDADDR: break;
    case SIOCGIFMTU:     r->u.ival = n->mtu; break;
    case SIOCSIFMTU:
        if (r->u.ival < 68 || r->u.ival > (n == netif_loopback() ? 65535 : ETH_MTU)) rc = -EINVAL;
        else n->mtu = (uint16_t)r->u.ival;
        break;
    case SIOCGIFHWADDR:
        memset(&r->u, 0, sizeof r->u);
        r->u.hw.family = n == netif_loopback() ? 772 : 1;      /* ARPHRD_LOOPBACK / ARPHRD_ETHER */
        memcpy(r->u.hw.data, n->mac, ETH_ALEN);
        break;
    case SIOCGIFINDEX:   r->u.ival = n->index; break;
    case SIOCGIFMETRIC:  r->u.ival = 0; break;
    case SIOCGIFTXQLEN:  r->u.ival = n == netif_loopback() ? 1000 : 1000; break;
    default: rc = -ENOTTY;
    }
    irq_restore(fl);
    return rc;
}

/* ---- file operations ------------------------------------------------------------------- */
long inet_sendto(struct file *f, const void *buf, size_t len, int flags, const void *to, uint32_t tolen);
long inet_recvfrom(struct file *f, void *buf, size_t len, int flags, struct sockaddr_in *from, int *msg_flags);
static ssize_t inet_fread(struct file *f, void *buf, size_t len) { return inet_recvfrom(f, buf, len, 0, NULL, NULL); }
static ssize_t inet_fwrite(struct file *f, const void *buf, size_t len) { return inet_sendto(f, buf, len, 0, NULL, 0); }

static int inet_poll(struct file *f, int events)
{
    struct isock *s = isk(f);
    int r;
    uint64_t fl = irq_save();
    poll_wait(s);
    if (s->type == SOCK_STREAM && s->tcb) poll_wait(s->tcb);    /* tcp.c wakes the connection */
    if (s->type == SOCK_STREAM) r = tcp_poll(s);
    else {
        r = POLLOUT | POLLWRNORM;
        if (s->rq || s->shut_rd) r |= POLLIN | POLLRDNORM;
        if (s->err) r |= POLLERR;
        if (s->shut_rd && s->shut_wr) r |= POLLHUP;
    }
    irq_restore(fl);
    return r & (events | POLLERR | POLLHUP | POLLNVAL);
}

#define FIONREAD 0x541B
static int inet_ioctl(struct vnode *vn, unsigned long req, void *arg)
{
    struct isock *s = vn->data;
    if (req == FIONREAD) {
        uint64_t fl = irq_save();
        *(int *)arg = s->type == SOCK_STREAM ? tcp_readable(s) : s->rq ? (int)s->rq->len : 0;
        irq_restore(fl);
        return 0;
    }
    if (net_ioctl_is(req)) return net_ioctl(req, arg);
    return -ENOTTY;
}

static void inet_release(struct file *f)
{
    struct isock *s = isk(f);
    uint64_t fl = irq_save();
    if (s->type == SOCK_STREAM) tcp_close(s);
    else { dgram_unregister(s); dgram_flush(s); }
    wakeup(s);
    irq_restore(fl);
    kfree(s);
    net_lo_flush();                         /* the FIN or RST of a loopback connection */
}

static const struct vnode_ops inet_ops = {
    .fread = inet_fread, .fwrite = inet_fwrite, .poll = inet_poll, .release = inet_release, .ioctl = inet_ioctl,
};

/* ---- entry points: deliver loopback traffic before returning ---------------------- */
int inet_connect(struct file *f, const void *addr, uint32_t len)
{ int r = inet_connect_(f, addr, len); net_lo_flush(); return r; }
int inet_accept(struct file *f, int flags, struct file **out, struct sockaddr_in *peer)
{ int r = inet_accept_(f, flags, out, peer); net_lo_flush(); return r; }
long inet_sendto(struct file *f, const void *buf, size_t len, int flags, const void *to, uint32_t tolen)
{ long r = inet_sendto_(f, buf, len, flags, to, tolen); net_lo_flush(); return r; }
long inet_recvfrom(struct file *f, void *buf, size_t len, int flags, struct sockaddr_in *from, int *msg_flags)
{ long r = inet_recvfrom_(f, buf, len, flags, from, msg_flags); net_lo_flush(); return r; }
int inet_shutdown(struct file *f, int how)
{ int r = inet_shutdown_(f, how); net_lo_flush(); return r; }
int inet_setsockopt(struct file *f, int level, int opt, const void *val, uint32_t len)
{ int r = inet_setsockopt_(f, level, opt, val, len); net_lo_flush(); return r; }
