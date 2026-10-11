/* net/ipv4.c -- IPv4 (RFC 791) and ICMP (RFC 792)
 *
 * Input checks the header (version, length, checksum, our address or a
 * broadcast) and hands the payload to ICMP, UDP or TCP; raw ICMP sockets
 * get a copy of every ICMP packet. Fragments are reassembled (up to 8
 * datagrams of 64 KiB at once, 30 s each). Output picks the route and the
 * source address, numbers the datagram and sends it through ARP or the
 * loopback. Packets larger than the interface MTU are fragmented unless
 * DF is set (TCP sizes its segments to fit, so only UDP/ICMP fragment).
 * ICMP answers echo requests and turns "unreachable" errors into socket
 * errors (ECONNREFUSED for a closed UDP port, EHOSTUNREACH, ...). */
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/mm.h>
#include <kernel/posix.h>

struct ip_hdr {
    uint8_t  vihl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

#define IP_DF   0x4000
#define IP_MF   0x2000
#define IP_OFF  0x1fff

int  netif_output(struct netif *nif, uint32_t nexthop, struct pkt *p);
void raw_input(struct pkt *p, uint32_t src, uint32_t dst);      /* net/socket.c */

static uint16_t ip_id;
static uint64_t st_in, st_out, st_drop, st_frag;
uint64_t ip_stats(int which) { return which == 0 ? st_in : which == 1 ? st_out : which == 2 ? st_drop : st_frag; }

/* ---- reassembly ------------------------------------------------------------- */
#define FRAG_SLOTS  8
#define FRAG_MAX    65535
#define FRAG_TTL_MS 30000

static struct frag {
    bool     used;
    uint32_t src, dst;
    uint16_t id;
    uint8_t  proto;
    uint8_t *data;                  /* FRAG_MAX payload bytes */
    uint8_t *have;                  /* one bit per 8-byte block */
    uint32_t total;                 /* payload length, known once the last fragment came */
    uint64_t expires;
    struct pkt *first;              /* fragment 0, for its header (ICMP errors) */
} frags[FRAG_SLOTS];

static void frag_free(struct frag *f)
{
    kfree(f->data); kfree(f->have);
    pkt_free(f->first);
    memset(f, 0, sizeof *f);
}

static bool frag_complete(struct frag *f)
{
    if (!f->total) return false;
    for (uint32_t b = 0; b < (f->total + 7) / 8; b++) if (!(f->have[b / 8] & (1u << (b % 8)))) return false;
    return true;
}

/* Returns the reassembled datagram (IP header + payload) or NULL. Takes p. */
static struct pkt *reassemble(struct pkt *p, struct ip_hdr *h, unsigned hl)
{
    uint16_t fr = be16(h->frag);
    uint32_t off = (uint32_t)(fr & IP_OFF) * 8;
    uint32_t plen = (uint32_t)be16(h->len) - hl;
    st_frag++;
    if (off + plen > FRAG_MAX || ((fr & IP_MF) && (plen & 7))) { pkt_free(p); return NULL; }
    struct frag *f = NULL, *free = NULL;
    uint64_t now = net_now();
    for (int i = 0; i < FRAG_SLOTS; i++) {
        if (frags[i].used && now >= frags[i].expires) frag_free(&frags[i]);
        if (frags[i].used && frags[i].src == h->src && frags[i].dst == h->dst && frags[i].id == h->id &&
            frags[i].proto == h->proto) f = &frags[i];
        if (!frags[i].used && !free) free = &frags[i];
    }
    if (!f) {
        if (!(f = free)) { pkt_free(p); return NULL; }
        f->data = kmalloc(FRAG_MAX);
        f->have = kzalloc(FRAG_MAX / 64 + 1);
        if (!f->data || !f->have) { kfree(f->data); kfree(f->have); f->data = f->have = NULL; pkt_free(p); return NULL; }
        f->used = true;
        f->src = h->src; f->dst = h->dst; f->id = h->id; f->proto = h->proto;
        f->expires = now + FRAG_TTL_MS;
        net_set_timer(f->expires);
    }
    memcpy(f->data + off, pkt_data(p) + hl, plen);
    for (uint32_t b = off / 8; b < (off + plen + 7) / 8; b++) f->have[b / 8] |= (uint8_t)(1u << (b % 8));
    if (!(fr & IP_MF)) f->total = off + plen;
    if (off == 0 && !f->first) f->first = p; else pkt_free(p);
    if (!frag_complete(f) || !f->first) return NULL;
    /* The whole datagram, in a normal pkt when it fits. */
    uint32_t total = 20 + f->total;
    struct pkt *big = total <= 65535 ? pkt_alloc_big(total) : NULL;
    if (!big) { frag_free(f); return NULL; }
    big->len = (uint16_t)total;
    struct ip_hdr *nh = (struct ip_hdr *)pkt_data(big);
    memcpy(nh, pkt_data(f->first), 20);
    nh->vihl = 0x45;
    nh->len = be16((uint16_t)total);
    nh->frag = 0;
    memcpy((uint8_t *)nh + 20, f->data, f->total);
    frag_free(f);
    return big;
}


/* ---- input --------------------------------------------------------------------- */
void ip_input(struct netif *nif, struct pkt *p)
{
    st_in++;
    if (p->len < 20) goto drop;
    struct ip_hdr *h = (struct ip_hdr *)pkt_data(p);
    unsigned hl = (h->vihl & 15) * 4u;
    if ((h->vihl >> 4) != 4 || hl < 20 || hl > p->len || be16(h->len) < hl || be16(h->len) > p->len) goto drop;
    if (nif != netif_loopback() && inet_csum(h, hl, 0)) goto drop;
    p->len = be16(h->len);                              /* strip Ethernet padding */
    bool bcast = h->dst == 0xffffffffu || (nif->mask && h->dst == (nif->ip | ~nif->mask));
    bool mine = h->dst == nif->ip || ip_is_local(h->dst) || bcast || (h->dst & 0xf0) == 0xe0;
    /* Before DHCP finishes there is no address: take what is sent to us (the OFFER). */
    if (!mine && !(nif->ip == 0 && h->proto == IPPROTO_UDP)) goto drop;
    if (be16(h->frag) & (IP_MF | IP_OFF)) {
        if (!(p = reassemble(p, h, hl))) return;
        h = (struct ip_hdr *)pkt_data(p);
        hl = 20;
    }
    uint32_t src = h->src, dst = h->dst;
    uint8_t proto = h->proto;
    if (proto == IPPROTO_ICMP) raw_input(p, src, dst);  /* raw sockets see the IP header */
    pkt_pull(p, (uint16_t)hl);
    switch (proto) {
    case IPPROTO_ICMP: icmp_input(nif, p, src, dst); return;
    case IPPROTO_UDP:  udp_input(nif, p, src, dst); return;
    case IPPROTO_TCP:  if (!bcast) { tcp_input(nif, p, src, dst); return; } break;
    default:
        if (!bcast) { pkt_push(p, (uint16_t)hl); icmp_send_unreach(p, 2); }   /* protocol unreachable */
        break;
    }
    pkt_free(p);
    return;
drop:
    st_drop++;
    pkt_free(p);
}

/* ---- output ----------------------------------------------------------------------- */
static int send_frags(struct netif *nif, uint32_t nexthop, struct pkt *p, struct ip_hdr *h)
{
    /* p holds header + payload; send MTU-sized pieces, each a fresh pkt */
    uint16_t mtu = nif->mtu;
    uint32_t plen = p->len - 20u, per = (uint32_t)(mtu - 20) & ~7u;
    const uint8_t *payload = pkt_data(p) + 20;
    for (uint32_t off = 0; off < plen; off += per) {
        uint32_t n = plen - off < per ? plen - off : per;
        struct pkt *q = pkt_alloc_big(20 + n);         /* lo's MTU makes big fragments */
        if (!q) break;
        q->len = (uint16_t)(20 + n);
        struct ip_hdr *qh = (struct ip_hdr *)pkt_data(q);
        *qh = *h;
        qh->len = be16((uint16_t)(20 + n));
        qh->frag = be16((uint16_t)(off / 8) | (off + n < plen ? IP_MF : 0));
        qh->csum = 0;
        qh->csum = inet_csum(qh, 20, 0);
        memcpy((uint8_t *)qh + 20, payload + off, n);
        netif_output(nif, nexthop, q);
    }
    pkt_free(p);
    return 0;
}

static int output(struct netif *nif, uint32_t nexthop, struct pkt *p, uint32_t src, uint32_t dst,
                  uint8_t proto, uint8_t ttl, uint8_t tos, bool df)
{
    struct ip_hdr *h = (struct ip_hdr *)pkt_push(p, 20);
    h->vihl = 0x45;
    h->tos = tos;
    h->len = be16(p->len);
    h->id = be16(ip_id++);
    h->frag = df ? be16(IP_DF) : 0;
    h->ttl = ttl;
    h->proto = proto;
    h->src = src;
    h->dst = dst;
    h->csum = 0;
    h->csum = inet_csum(h, 20, 0);
    st_out++;
    if (p->len > nif->mtu) {
        if (df) { pkt_free(p); return -EMSGSIZE; }
        return send_frags(nif, nexthop, p, h);
    }
    return netif_output(nif, nexthop, p);
}

int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl, uint8_t tos, bool df)
{
    uint32_t nexthop = dst;
    struct netif *nif;
    if (ip_is_local(dst)) nif = netif_loopback();
    else if (dst == 0xffffffffu) {                      /* limited broadcast: the first up Ethernet */
        for (nif = netif_list(); nif && (nif == netif_loopback() || !(nif->flags & IFF_UP)); nif = nif->next) {}
    } else nif = route_lookup(dst, &nexthop);
    if (!nif) { pkt_free(p); return -ENETUNREACH; }
    if (!src) src = nif == netif_loopback() ? (ip_is_local(dst) && (dst & 0xff) != 127 ? dst : IP4(127, 0, 0, 1)) : nif->ip;
    return output(nif, nexthop, p, src, dst, proto, ttl ? ttl : 64, tos, df);
}

int ip_output_if(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    return output(nif, dst, p, src, dst, proto, ttl ? ttl : 64, 0, false);
}

/* ---- ICMP --------------------------------------------------------------------------- */
struct icmp_hdr { uint8_t type, code; uint16_t csum; uint16_t id, seq; } __attribute__((packed));

void icmp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    (void)nif;
    if (p->len < 8 || inet_csum(pkt_data(p), p->len, 0)) goto out;
    struct icmp_hdr *ic = (struct icmp_hdr *)pkt_data(p);
    if (ic->type == 8 && ic->code == 0) {               /* echo request -> reply, same data */
        if (dst == 0xffffffffu || (dst & 0xf0) == 0xe0) goto out;
        if (p->big) goto out;
        ic->type = 0;
        ic->csum = 0;
        ic->csum = inet_csum(ic, p->len, 0);
        struct pkt *q = pkt_alloc();
        if (!q) goto out;
        q->len = p->len;
        memcpy(pkt_data(q), ic, p->len);
        ip_output(q, ip_is_local(dst) ? dst : 0, src, IPPROTO_ICMP, 64, 0, false);
    } else if ((ic->type == 3 || ic->type == 11 || ic->type == 4) && p->len >= 8 + 20 + 8) {
        icmp_error_input(ic->type, ic->code, pkt_data(p) + 8, p->len - 8u);
    }
out:
    pkt_free(p);
}

/* Destination unreachable about `orig` (IP header at pkt_data), to its sender.
 * Never about ICMP errors, broadcasts or fragments after the first. */
void icmp_send_unreach(struct pkt *orig, uint8_t code)
{
    if (orig->len < 20) return;
    struct ip_hdr *h = (struct ip_hdr *)pkt_data(orig);
    unsigned hl = (h->vihl & 15) * 4u;
    if (h->src == 0 || h->dst == 0xffffffffu || (be16(h->frag) & IP_OFF)) return;
    if (h->proto == IPPROTO_ICMP && orig->len >= hl + 1 && pkt_data(orig)[hl] != 8 && pkt_data(orig)[hl] != 0) return;
    struct pkt *q = pkt_alloc();
    if (!q) return;
    size_t keep = orig->len < hl + 8 ? orig->len : hl + 8;
    struct icmp_hdr *ic = (struct icmp_hdr *)pkt_data(q);
    memset(ic, 0, 8);
    ic->type = 3;
    ic->code = code;
    memcpy((uint8_t *)ic + 8, h, keep);
    q->len = (uint16_t)(8 + keep);
    ic->csum = inet_csum(ic, q->len, 0);
    ip_output(q, 0, h->src, IPPROTO_ICMP, 64, 0, false);
}
