/* net/udp.c -- UDP (RFC 768), ICMP "ping" sockets and raw ICMP sockets
 *
 * Every datagram socket is on one list. A UDP datagram goes to the socket
 * whose binding fits best (connected peer > bound address > wildcard);
 * broadcasts and multicasts go to every socket on the port. Nobody
 * listening: ICMP port unreachable, except for broadcasts, and port 68,
 * which is the DHCP client's (net/dhcp.c). Each socket queues whole
 * datagrams up to its receive buffer size (Linux's default, 208 KiB),
 * dropping what does not fit.
 *
 * Ping sockets (SOCK_DGRAM, IPPROTO_ICMP) send echo requests whose
 * identifier the kernel sets to the socket's "port" and receive the
 * matching replies without the IP header, as on Linux. Raw ICMP sockets
 * (SOCK_RAW) receive every ICMP packet with its IP header and send ICMP
 * messages the kernel puts an IP header on. */
#include "sock.h"
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>

struct udp_hdr { uint16_t sport, dport, len, csum; } __attribute__((packed));

static struct isock *dgrams;
static uint64_t udp_in, udp_out, udp_noport, udp_rcvbuferr, udp_inerr;

void dgram_register(struct isock *s) { s->next = dgrams; dgrams = s; }

void dgram_unregister(struct isock *s)
{
    for (struct isock **pp = &dgrams; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
}

void dgram_flush(struct isock *s)
{
    while (s->rq) { struct pkt *p = s->rq; s->rq = p->next; pkt_free(p); }
    s->rq_tail = NULL;
    s->rq_bytes = 0;
}

bool dgram_port_used(int proto, uint32_t addr, uint16_t port, struct isock *except, bool reuse)
{
    for (struct isock *s = dgrams; s; s = s->next) {
        if (s == except || s->proto != proto || s->lport != port) continue;
        if (s->laddr && addr && s->laddr != addr) continue;
        if (reuse && (s->reuseaddr || s->reuseport)) continue;
        return true;
    }
    return false;
}

/* Queue p (payload at pkt_data) on s; takes p. */
static void enqueue(struct isock *s, struct pkt *p, uint32_t src, uint16_t sport)
{
    if (s->shut_rd || s->rq_bytes + p->len > s->rcvbuf) { udp_rcvbuferr++; pkt_free(p); return; }
    p->saddr = src;
    p->sport = sport;
    p->next = NULL;
    if (s->rq_tail) s->rq_tail->next = p; else s->rq = p;
    s->rq_tail = p;
    s->rq_bytes += p->len;
    wakeup(s);
}

static struct pkt *clone(struct pkt *p)
{
    struct pkt *q = p->big ? pkt_alloc_big(p->len) : pkt_alloc();
    if (!q) return NULL;
    q->len = p->len;
    memcpy(pkt_data(q), pkt_data(p), p->len);
    return q;
}

void udp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    udp_in++;
    if (p->len < 8) goto err;
    struct udp_hdr *u = (struct udp_hdr *)pkt_data(p);
    uint16_t ulen = be16(u->len);
    if (ulen < 8 || ulen > p->len) goto err;
    p->len = ulen;
    if (u->csum && nif != netif_loopback() && inet_csum(u, ulen, inet_pseudo(src, dst, IPPROTO_UDP, ulen))) goto err;
    uint16_t sport = u->sport, dport = u->dport;
    pkt_pull(p, 8);
    bool multi = dst == 0xffffffffu || (dst & 0xf0) == 0xe0 || (nif->mask && dst == (nif->ip | ~nif->mask));
    if (multi) {
        struct isock *last = NULL;
        for (struct isock *s = dgrams; s; s = s->next) {
            if (s->proto != IPPROTO_UDP || s->lport != dport || (s->laddr && s->laddr != dst && s->laddr != nif->ip)) continue;
            if (last) { struct pkt *q = clone(p); if (q) enqueue(last, q, src, sport); }
            last = s;
        }
        if (last) { enqueue(last, p, src, sport); return; }
    } else {
        struct isock *best = NULL;
        int score = -1;
        for (struct isock *s = dgrams; s; s = s->next) {
            if (s->proto != IPPROTO_UDP || s->lport != dport) continue;
            if (s->laddr && s->laddr != dst) continue;
            int sc = s->laddr ? 1 : 0;
            if (s->connected) { if (s->raddr != src || s->rport != sport) continue; sc += 2; }
            if (sc > score) { score = sc; best = s; }
        }
        if (best) { enqueue(best, p, src, sport); return; }
    }
    if (dport == be16(68)) { dhcp_input(nif, p, src, sport, dport); return; }
    udp_noport++;
    if (!multi) {                                       /* port unreachable, quoting the datagram */
        pkt_push(p, 8);
        struct pkt *q = pkt_alloc();
        if (q) {
            uint8_t *h = pkt_push(q, 20);               /* rebuild an IP header to quote */
            memset(h, 0, 20);
            h[0] = 0x45; h[8] = 64; h[9] = IPPROTO_UDP;
            uint16_t tl = be16((uint16_t)(20 + (p->len < 8 ? p->len : 8)));
            memcpy(h + 2, &tl, 2);
            memcpy(h + 12, &src, 4);
            memcpy(h + 16, &dst, 4);
            memcpy(h + 20, pkt_data(p), 8);
            q->len = 28;
            icmp_send_unreach(q, 3);
            pkt_free(q);
        }
    }
    pkt_free(p);
    return;
err:
    udp_inerr++;
    pkt_free(p);
}

/* Raw ICMP sockets get every ICMP packet (IP header included); ping
 * sockets get echo replies addressed to their identifier. */
void raw_input(struct pkt *p, uint32_t src, uint32_t dst)
{
    (void)dst;
    unsigned hl = (pkt_data(p)[0] & 15) * 4u;
    const uint8_t *ic = pkt_data(p) + hl;
    for (struct isock *s = dgrams; s; s = s->next) {
        if (s->proto != IPPROTO_ICMP) continue;
        if (s->connected && s->raddr != src) continue;
        if (s->type == SOCK_RAW) {
            struct pkt *q = clone(p);
            if (q) enqueue(s, q, src, 0);
        } else if (p->len >= hl + 8 && ic[0] == 0) {    /* echo reply for this ping socket */
            uint16_t id;
            memcpy(&id, ic + 4, 2);
            if (id != s->lport) continue;
            struct pkt *q = clone(p);
            if (q) { pkt_pull(q, (uint16_t)hl); enqueue(s, q, src, 0); }
        }
    }
}

/* An ICMP error quoting one of our datagrams: tell the socket (connected
 * UDP sockets only, as Linux does without IP_RECVERR), then TCP. */
void icmp_error_input(uint8_t type, uint8_t code, const uint8_t *iph, size_t len)
{
    unsigned hl = (iph[0] & 15) * 4u;
    if (len < hl + 8) return;
    uint32_t qsrc, qdst;
    memcpy(&qsrc, iph + 12, 4);
    memcpy(&qdst, iph + 16, 4);
    uint16_t sport, dport;
    memcpy(&sport, iph + hl, 2);
    memcpy(&dport, iph + hl + 2, 2);
    int err = type == 3 ? (code == 3 ? ECONNREFUSED : code == 2 ? ENOPROTOOPT : code == 4 ? EMSGSIZE :
                           code == 0 || code == 6 ? ENETUNREACH : EHOSTUNREACH) : EHOSTUNREACH;
    if (iph[9] == IPPROTO_UDP) {
        for (struct isock *s = dgrams; s; s = s->next)
            if (s->proto == IPPROTO_UDP && s->lport == sport && s->connected && s->raddr == qdst && s->rport == dport) {
                s->err = err;
                wakeup(s);
            }
    } else if (iph[9] == IPPROTO_TCP) {
        void tcp_icmp_error(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, int err, uint8_t type, uint8_t code);
        tcp_icmp_error(qsrc, sport, qdst, dport, err, type, code);
    }
}

/* ---- sending -------------------------------------------------------------------------- */
long udp_send(struct isock *s, const void *buf, size_t len, uint32_t dst, uint16_t dport)
{
    if (len > 65507) return -EMSGSIZE;
    if (!dport) return -EINVAL;
    if (dst == 0) dst = IP4(127, 0, 0, 1);              /* Linux: INADDR_ANY means this host */
    bool bcast = dst == 0xffffffffu;
    if (!bcast) {
        uint32_t nh;
        struct netif *n = ip_is_local(dst) ? netif_loopback() : route_lookup(dst, &nh);
        if (!n) return -ENETUNREACH;
        if (n->mask && dst == (n->ip | ~n->mask)) bcast = true;
    }
    if (bcast && !s->broadcast) return -EACCES;
    struct pkt *p = pkt_alloc_big(len + 8);
    if (!p) return -ENOMEM;
    uint32_t src = s->laddr ? s->laddr : ip_source_for(dst);
    if (bcast && !src) {                                /* broadcast before any address */
        for (struct netif *n = netif_list(); n; n = n->next) if (n != netif_loopback() && n->ip) { src = n->ip; break; }
    }
    struct udp_hdr *u = (struct udp_hdr *)pkt_data(p);
    u->sport = s->lport;
    u->dport = dport;
    u->len = be16((uint16_t)(len + 8));
    u->csum = 0;
    memcpy(u + 1, buf, len);
    p->len = (uint16_t)(len + 8);
    uint16_t c = inet_csum(u, p->len, inet_pseudo(src, dst, IPPROTO_UDP, p->len));
    u->csum = c ? c : 0xffff;
    udp_out++;
    int rc = ip_output(p, src, dst, IPPROTO_UDP, s->ttl, s->tos, false);
    return rc < 0 ? rc : (long)len;
}

long icmp_sock_send(struct isock *s, const void *buf, size_t len, uint32_t dst)
{
    if (len < 8 && s->type != SOCK_RAW) return -EINVAL;
    if (len > 65507) return -EMSGSIZE;
    struct pkt *p = pkt_alloc_big(len);
    if (!p) return -ENOMEM;
    uint8_t *ic = pkt_data(p);
    memcpy(ic, buf, len);
    p->len = (uint16_t)len;
    if (s->type != SOCK_RAW) {                          /* ping socket: only echo requests, our id */
        if (ic[0] != 8 || ic[1] != 0) { pkt_free(p); return -EINVAL; }
        memcpy(ic + 4, &s->lport, 2);
        ic[2] = ic[3] = 0;
        uint16_t c = inet_csum(ic, len, 0);
        memcpy(ic + 2, &c, 2);
    }
    int rc = ip_output(p, s->laddr, dst ? dst : IP4(127, 0, 0, 1), IPPROTO_ICMP, s->ttl, s->tos, false);
    return rc < 0 ? rc : (long)len;
}

/* ---- receiving ---------------------------------------------------------------------- */
long dgram_recv(struct isock *s, struct file *f, void *buf, size_t len, int flags, struct sockaddr_in *from,
                int *msg_flags)
{
    uint64_t fl = irq_save();
    uint64_t deadline = sock_deadline(s->rcvtimeo);
    while (!s->rq) {
        if (s->err) { int e = s->err; s->err = 0; irq_restore(fl); return -e; }
        if (s->shut_rd) { irq_restore(fl); return 0; }
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) { irq_restore(fl); return -EAGAIN; }
        int rc = sock_wait(s, deadline);
        if (rc) { irq_restore(fl); return rc; }
    }
    struct pkt *p = s->rq;
    size_t n = p->len < len ? p->len : len;
    if (buf && n) memcpy(buf, pkt_data(p), n);
    if (from) {
        memset(from, 0, sizeof *from);
        from->sin_family = AF_INET;
        from->sin_port = p->sport;
        from->sin_addr = p->saddr;
    }
    long ret = (long)n;
    if (p->len > len) {                                 /* the rest of the datagram is lost */
        if (msg_flags) *msg_flags |= MSG_TRUNC;
        if (flags & MSG_TRUNC) ret = (long)p->len;
    }
    if (!(flags & MSG_PEEK)) {
        s->rq = p->next;
        if (!s->rq) s->rq_tail = NULL;
        s->rq_bytes -= p->len;
        pkt_free(p);
    }
    irq_restore(fl);
    return ret;
}

/* /proc/net/udp, Linux layout (enough for netstat/ss) */
size_t udp_proc(char *buf, size_t cap)
{
    size_t n = (size_t)snprintf(buf, cap, "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int i = 0;
    for (struct isock *s = dgrams; s && n < cap; s = s->next) {
        if (s->proto != IPPROTO_UDP) continue;
        n += (size_t)snprintf(buf + n, cap - n, "%5d: %08X:%04X %08X:%04X %02X %08X:%08X 00:00000000 00000000     0        0 0\n",
                              i++, s->laddr, be16(s->lport), s->raddr, be16(s->rport), s->connected ? 1 : 7,
                              0u, (unsigned)s->rq_bytes);
    }
    return n < cap ? n : cap;
}
