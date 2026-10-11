/* net/arp.c -- Ethernet framing and ARP (RFC 826)
 *
 * A small cache: resolved entries live 5 minutes (refreshed by any ARP
 * packet from the host); an unresolved one holds up to 4 packets while
 * a request is resent every second, 3 times, before they are dropped
 * with "host unreachable". Gratuitous ARP and requests for our address
 * update the cache too. */
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/klog.h>

#define ARP_ENTRIES     64
#define ARP_TTL_MS      (5 * 60 * 1000)
#define ARP_RETRY_MS    1000
#define ARP_TRIES       3
#define ARP_HOLD        4

struct arp_hdr {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct arp_entry {
    struct netif *nif;
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    bool     used, resolved;
    uint64_t expires;               /* resolved: forget; pending: next retry */
    int      tries;
    struct pkt *hold;               /* waiting packets (IP header first) */
    int      nhold;
};

static struct arp_entry cache[ARP_ENTRIES];
static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int eth_xmit(struct netif *nif, const uint8_t *dmac, uint16_t type, struct pkt *p)
{
    uint8_t *h = pkt_push(p, ETH_HLEN);
    memcpy(h, dmac, ETH_ALEN);
    memcpy(h + 6, nif->mac, ETH_ALEN);
    h[12] = (uint8_t)(type >> 8);
    h[13] = (uint8_t)type;
    int rc;
    if (p->len < 60) { memset(h + p->len, 0, 60 - p->len); p->len = 60; }   /* minimum frame */
    if (!(nif->flags & IFF_UP)) { nif->tx_dropped++; rc = -1; }
    else if ((rc = nif->xmit(nif, h, p->len)) < 0) nif->tx_dropped++;
    else { nif->tx_packets++; nif->tx_bytes += p->len; }
    pkt_free(p);
    return rc;
}

static struct arp_entry *lookup(struct netif *nif, uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (cache[i].used && cache[i].nif == nif && cache[i].ip == ip) return &cache[i];
    return NULL;
}

static void drop_hold(struct arp_entry *e, bool unreachable)
{
    while (e->hold) {
        struct pkt *p = e->hold;
        e->hold = p->next;
        if (unreachable) icmp_send_unreach(p, 1);          /* host unreachable, to the local sender */
        pkt_free(p);
    }
    e->nhold = 0;
}

static struct arp_entry *alloc_entry(struct netif *nif, uint32_t ip)
{
    struct arp_entry *victim = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!cache[i].used) { victim = &cache[i]; break; }
        if (cache[i].resolved && (!victim || cache[i].expires < victim->expires)) victim = &cache[i];
    }
    if (!victim) return NULL;
    drop_hold(victim, false);
    memset(victim, 0, sizeof *victim);
    victim->used = true;
    victim->nif = nif;
    victim->ip = ip;
    return victim;
}

static void send_arp(struct netif *nif, uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *dmac)
{
    struct pkt *p = pkt_alloc();
    if (!p) return;
    struct arp_hdr *a = (struct arp_hdr *)pkt_push(p, sizeof *a);
    a->htype = be16(1);
    a->ptype = be16(ETHTYPE_IP);
    a->hlen = 6; a->plen = 4;
    a->op = be16(op);
    memcpy(a->sha, nif->mac, 6);
    a->spa = nif->ip;
    memcpy(a->tha, tha, 6);
    a->tpa = tpa;
    eth_xmit(nif, dmac, ETHTYPE_ARP, p);
}

static void learn(struct arp_entry *e, const uint8_t *mac)
{
    memcpy(e->mac, mac, ETH_ALEN);
    e->resolved = true;
    e->expires = net_now() + ARP_TTL_MS;
    while (e->hold) {                                   /* send what waited */
        struct pkt *p = e->hold;
        e->hold = p->next;
        e->nhold--;
        eth_xmit(e->nif, e->mac, ETHTYPE_IP, p);
    }
}

void arp_input(struct netif *nif, struct pkt *p)
{
    if (p->len < sizeof(struct arp_hdr)) { pkt_free(p); return; }
    struct arp_hdr *a = (struct arp_hdr *)pkt_data(p);
    if (a->htype != be16(1) || a->ptype != be16(ETHTYPE_IP) || a->hlen != 6 || a->plen != 4) { pkt_free(p); return; }
    uint16_t op = be16(a->op);
    struct arp_entry *e = a->spa ? lookup(nif, a->spa) : NULL;
    if (e) learn(e, a->sha);
    if (nif->ip && a->tpa == nif->ip) {
        if (!e && a->spa && (e = alloc_entry(nif, a->spa))) learn(e, a->sha);   /* they will talk to us */
        if (op == 1) send_arp(nif, 2, a->sha, a->spa, a->sha);
    }
    pkt_free(p);
}

/* Send an IP packet to nexthop on an Ethernet interface. Takes p. */
int arp_output(struct netif *nif, uint32_t nexthop, struct pkt *p)
{
    if (nexthop == 0xffffffffu || (nif->mask && nexthop == (nif->ip | ~nif->mask)))
        return eth_xmit(nif, bcast, ETHTYPE_IP, p);
    if ((nexthop & 0xf0) == 0xe0) {                     /* multicast 224/4: 01:00:5e + low 23 bits */
        uint8_t m[6] = { 0x01, 0x00, 0x5e, (uint8_t)(nexthop >> 8 & 0x7f), (uint8_t)(nexthop >> 16), (uint8_t)(nexthop >> 24) };
        return eth_xmit(nif, m, ETHTYPE_IP, p);
    }
    struct arp_entry *e = lookup(nif, nexthop);
    if (e && e->resolved) {
        if (net_now() < e->expires) return eth_xmit(nif, e->mac, ETHTYPE_IP, p);
        e->resolved = false;                            /* stale: resolve it again */
        e->tries = 0;
    }
    if (!e && !(e = alloc_entry(nif, nexthop))) { nif->tx_dropped++; pkt_free(p); return -1; }
    if (e->nhold >= ARP_HOLD) {                         /* keep the newest */
        struct pkt *old = e->hold;
        e->hold = old->next;
        e->nhold--;
        pkt_free(old);
    }
    p->next = NULL;
    struct pkt **pp = &e->hold;
    while (*pp) pp = &(*pp)->next;
    *pp = p;
    e->nhold++;
    if (!e->tries) {
        static const uint8_t zero[6];
        send_arp(nif, 1, zero, nexthop, bcast);
        e->tries = 1;
        e->expires = net_now() + ARP_RETRY_MS;
        net_set_timer(e->expires);
    }
    return 0;
}

void arp_timer(uint64_t now)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &cache[i];
        if (!e->used) continue;
        if (e->resolved) {
            if (now >= e->expires) e->used = false;
            else net_set_timer(e->expires);
            continue;
        }
        if (now < e->expires) { net_set_timer(e->expires); continue; }
        if (e->tries >= ARP_TRIES) { drop_hold(e, true); e->used = false; continue; }
        static const uint8_t zero[6];
        send_arp(e->nif, 1, zero, e->ip, bcast);
        e->tries++;
        e->expires = now + ARP_RETRY_MS;
        net_set_timer(e->expires);
    }
}

void arp_flush_if(struct netif *nif)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (cache[i].used && cache[i].nif == nif) { drop_hold(&cache[i], false); cache[i].used = false; }
}

/* /proc/net/arp, Linux format */
size_t arp_proc(char *buf, size_t cap)
{
    size_t n = (size_t)snprintf(buf, cap, "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (int i = 0; i < ARP_ENTRIES && n < cap; i++) {
        struct arp_entry *e = &cache[i];
        if (!e->used) continue;
        char ip[16];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", e->ip & 0xff, e->ip >> 8 & 0xff, e->ip >> 16 & 0xff, e->ip >> 24);
        n += (size_t)snprintf(buf + n, cap - n, "%-16s 0x1         0x%x         %02x:%02x:%02x:%02x:%02x:%02x     *        %s\n",
                              ip, e->resolved ? 2 : 0, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                              e->nif->name);
    }
    return n < cap ? n : cap;
}
