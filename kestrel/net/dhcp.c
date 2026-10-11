/* net/dhcp.c -- DHCP client (RFC 2131) run by the kernel, like Linux's ip=dhcp
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK, resent with backoff (4, 8, 16, 32,
 * then every 60 s) for as long as there is no answer. Once bound, the
 * address, netmask, default route, name servers and domain are applied;
 * the lease is renewed with the server at half its time (T1), with anyone
 * at 7/8 (T2), and dropped when it runs out. A NAK starts over. */
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/task.h>

#define MAX_CLIENTS 4
#define DHCP_MAGIC  0x63538263u             /* 99.130.83.99, as read little-endian */

enum dstate { D_OFF, D_SELECTING, D_REQUESTING, D_BOUND, D_RENEWING, D_REBINDING };

struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64], file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed));

static struct client {
    struct netif *nif;
    enum dstate state;
    uint32_t xid;
    uint32_t offered, server;
    uint64_t next;                          /* resend / T1 / T2 / expiry */
    uint32_t backoff;                       /* ms */
    uint64_t start, bound_at;
    uint32_t lease, t1, t2;                 /* seconds */
} clients[MAX_CLIENTS];

bool dhcp_busy(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].nif && clients[i].state != D_BOUND && clients[i].state != D_OFF) return true;
    return false;
}

static uint8_t *opt(uint8_t *o, uint8_t code, uint8_t len, const void *data)
{
    o[0] = code; o[1] = len;
    memcpy(o + 2, data, len);
    return o + 2 + len;
}

static void send_msg(struct client *c, uint8_t type)
{
    struct pkt *p = pkt_alloc();
    if (!p) return;
    struct dhcp_msg *m = (struct dhcp_msg *)(pkt_data(p) + 8);
    memset(m, 0, sizeof *m);
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = c->xid;
    uint64_t secs = (net_now() - c->start) / 1000;
    m->secs = be16((uint16_t)(secs > 0xffff ? 0xffff : secs));
    bool renew = c->state == D_RENEWING || c->state == D_REBINDING;
    if (renew) m->ciaddr = c->nif->ip;
    else m->flags = be16(0x8000);           /* answer by broadcast: we have no address yet */
    memcpy(m->chaddr, c->nif->mac, 6);
    m->magic = DHCP_MAGIC;
    uint8_t *o = m->options;
    o = opt(o, 53, 1, &type);
    uint8_t cid[7] = { 1 };
    memcpy(cid + 1, c->nif->mac, 6);
    o = opt(o, 61, 7, cid);
    if (type == 3 && !renew) {
        o = opt(o, 50, 4, &c->offered);
        o = opt(o, 54, 4, &c->server);
    }
    static const uint8_t want[] = { 1, 3, 6, 15, 26, 28, 51, 58, 59 };
    o = opt(o, 55, sizeof want, want);
    uint16_t maxsz = be16(576);
    o = opt(o, 57, 2, &maxsz);
    o = opt(o, 12, 10, "kestrel-pc");
    *o++ = 255;
    size_t dlen = (size_t)(o - (uint8_t *)m);
    if (dlen < 300) dlen = 300;             /* BOOTP minimum */
    uint8_t *u = pkt_data(p);
    uint16_t sp = be16(68), dp = be16(67), ul = be16((uint16_t)(8 + dlen));
    memcpy(u, &sp, 2); memcpy(u + 2, &dp, 2); memcpy(u + 4, &ul, 2);
    u[6] = u[7] = 0;
    p->len = (uint16_t)(8 + dlen);
    uint32_t src = renew ? c->nif->ip : 0;
    uint32_t dst = c->state == D_RENEWING ? c->server : 0xffffffffu;
    uint16_t cs = inet_csum(u, p->len, inet_pseudo(src, dst, IPPROTO_UDP, p->len));
    if (!cs) cs = 0xffff;
    memcpy(u + 6, &cs, 2);
    if (c->state == D_RENEWING) ip_output(p, src, dst, IPPROTO_UDP, 64, 0, false);
    else ip_output_if(c->nif, p, src, dst, IPPROTO_UDP, 64);
}

static void dhcp_schedule(struct client *c, uint64_t when)
{
    c->next = when;
    net_set_timer(when);
}

static void resend_later(struct client *c)
{
    dhcp_schedule(c, net_now() + c->backoff + (rdtsc() % 1000));
    c->backoff = c->backoff * 2 > 60000 ? 60000 : c->backoff * 2;
}

static void discover(struct client *c)
{
    c->state = D_SELECTING;
    c->xid = (uint32_t)rdtsc() ^ 0x4b455354u;
    c->start = net_now();
    c->backoff = 4000;
    send_msg(c, 1);
    resend_later(c);
}

void dhcp_start(struct netif *nif)
{
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        if (c->nif && c->nif != nif) continue;
        c->nif = nif;
        kprintf("net: %s: asking for an address (DHCP)\n", nif->name);
        discover(c);
        break;
    }
    irq_restore(f);
}

static const uint8_t *find_opt(const struct dhcp_msg *m, size_t len, uint8_t code, uint8_t *olen)
{
    const uint8_t *o = m->options, *end = (const uint8_t *)m + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        if (*o == code) { *olen = o[1]; return o + 2; }
        o += 2 + o[1];
    }
    return NULL;
}

static uint32_t opt_u32(const struct dhcp_msg *m, size_t len, uint8_t code, uint32_t dflt)
{
    uint8_t l;
    const uint8_t *v = find_opt(m, len, code, &l);
    if (!v || l < 4) return dflt;
    uint32_t x;
    memcpy(&x, v, 4);
    return x;
}

#define FMT_IP(a) (a) & 0xff, (a) >> 8 & 0xff, (a) >> 16 & 0xff, (a) >> 24

static void bind_lease(struct client *c, const struct dhcp_msg *m, size_t len)
{
    struct netif *n = c->nif;
    uint32_t ip = m->yiaddr;
    uint32_t mask = opt_u32(m, len, 1, IP4(255, 255, 255, 0));
    uint32_t router = opt_u32(m, len, 3, 0);
    c->lease = be32(opt_u32(m, len, 51, be32(86400)));
    c->t1 = be32(opt_u32(m, len, 58, be32(c->lease / 2)));
    c->t2 = be32(opt_u32(m, len, 59, be32(c->lease / 8 * 7)));
    if (c->t1 > c->lease) c->t1 = c->lease / 2;
    if (c->t2 > c->lease || c->t2 < c->t1) c->t2 = c->lease / 8 * 7;
    bool changed = n->ip != ip || n->mask != mask;
    netif_set_addr(n, ip, mask);
    if (router) route_add(0, 0, router, n);
    uint8_t l;
    const uint8_t *dns = find_opt(m, len, 6, &l);
    if (dns) {
        memset(net_dns, 0, sizeof net_dns);
        for (int i = 0; i < 3 && (i + 1) * 4 <= l; i++) memcpy(&net_dns[i], dns + 4 * i, 4);
    }
    const uint8_t *dom = find_opt(m, len, 15, &l);
    if (dom) {
        size_t k = l < sizeof net_domain - 1 ? l : sizeof net_domain - 1;
        memcpy(net_domain, dom, k);
        net_domain[k] = 0;
    }
    const uint8_t *mtu = find_opt(m, len, 26, &l);
    if (mtu && l == 2) {
        uint16_t v = (uint16_t)(mtu[0] << 8 | mtu[1]);
        if (v >= 576 && v <= ETH_MTU) n->mtu = v;
    }
    c->state = D_BOUND;
    c->bound_at = net_now();
    dhcp_schedule(c, c->bound_at + (uint64_t)c->t1 * 1000);
    if (changed)
        kprintf("net: %s: %u.%u.%u.%u/%d via %u.%u.%u.%u, DNS %u.%u.%u.%u (DHCP server %u.%u.%u.%u, lease %u s)\n",
                n->name, FMT_IP(ip), mask_bits(mask), FMT_IP(router), FMT_IP(net_dns[0]), FMT_IP(c->server), c->lease);
    wakeup(clients);
}

void dhcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint16_t sport, uint16_t dport)
{
    (void)src; (void)dport;
    struct client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].nif == nif) c = &clients[i];
    size_t len = p->len;
    const struct dhcp_msg *m = (const struct dhcp_msg *)pkt_data(p);
    if (!c || sport != be16(67) || len < 240 || m->op != 2 || m->xid != c->xid || m->magic != DHCP_MAGIC ||
        memcmp(m->chaddr, nif->mac, 6)) goto out;
    uint8_t l;
    const uint8_t *t = find_opt(m, len, 53, &l);
    if (!t || l != 1) goto out;
    uint32_t server = opt_u32(m, len, 54, src);
    switch (*t) {
    case 2:                                             /* OFFER */
        if (c->state != D_SELECTING || !m->yiaddr) break;
        c->offered = m->yiaddr;
        c->server = server;
        c->state = D_REQUESTING;
        c->backoff = 4000;
        send_msg(c, 3);
        resend_later(c);
        break;
    case 5:                                             /* ACK */
        if (c->state == D_REQUESTING || c->state == D_RENEWING || c->state == D_REBINDING) {
            c->server = server;
            bind_lease(c, m, len);
        }
        break;
    case 6:                                             /* NAK */
        if (c->state != D_REQUESTING && c->state != D_RENEWING && c->state != D_REBINDING) break;
        kprintf("net: %s: DHCP server refused the address; starting over\n", nif->name);
        netif_set_addr(nif, 0, 0);
        route_flush_if(nif);
        discover(c);
        break;
    }
out:
    pkt_free(p);
}

void dhcp_timer(uint64_t now)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        if (!c->nif || c->state == D_OFF || !c->next) continue;
        if (now < c->next) { net_set_timer(c->next); continue; }
        switch (c->state) {
        case D_SELECTING:  send_msg(c, 1); resend_later(c); break;
        case D_REQUESTING:
            if (c->backoff >= 32000) { discover(c); break; }    /* the offer went stale */
            send_msg(c, 3); resend_later(c);
            break;
        case D_BOUND:
            c->state = D_RENEWING;
            c->xid = (uint32_t)rdtsc();
            c->start = now;
            send_msg(c, 3);
            dhcp_schedule(c, now + 60000 < c->bound_at + (uint64_t)c->t2 * 1000 ? now + 60000 : c->bound_at + (uint64_t)c->t2 * 1000);
            break;
        case D_RENEWING:
            if (now >= c->bound_at + (uint64_t)c->t2 * 1000) c->state = D_REBINDING;
            /* fall through */
        case D_REBINDING:
            if (now >= c->bound_at + (uint64_t)c->lease * 1000) {
                kprintf("net: %s: DHCP lease ran out\n", c->nif->name);
                netif_set_addr(c->nif, 0, 0);
                route_flush_if(c->nif);
                discover(c);
                break;
            }
            send_msg(c, 3);
            dhcp_schedule(c, now + 60000);
            break;
        default: break;
        }
    }
}
