/* net/core.c -- interfaces, packet buffers, routing, the network thread
 *
 * Command line:
 *   kestrel.net=dhcp           configure every Ethernet interface by DHCP (default)
 *   kestrel.net=off            register the drivers but leave them down
 *   kestrel.ip=ADDR/PREFIX[,GATEWAY[,DNS]]
 *                              static address for the first Ethernet interface
 *                              (e.g. kestrel.ip=192.168.1.50/24,192.168.1.1,1.1.1.1)
 *
 * See include/kernel/net.h for the execution model. */
#include <kernel/net.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/time.h>
#include <kernel/bootinfo.h>
#include <kernel/posix.h>

static struct netif *ifaces;
static int next_index = 1;
static struct netif lo;
static struct tcb *netd;
static volatile bool netd_wake;
static int netd_chan;                   /* netd sleeps here */
static uint64_t next_timer;             /* earliest deadline netd must honour */
static struct pkt *lo_queue, *lo_tail;  /* loopback: delivered by netd, never recursively */
static int lo_qlen;

struct route routes[MAX_ROUTES];
uint32_t net_dns[3];
char net_domain[64];

/* ---- packet buffers: a small cache in front of the heap --------------------- */
#define PKT_CACHE 128
static struct pkt *pkt_cache;
static int pkt_cached;

struct pkt *pkt_alloc(void)
{
    uint64_t f = irq_save();
    struct pkt *p = pkt_cache;
    if (p) { pkt_cache = p->next; pkt_cached--; }
    irq_restore(f);
    if (!p && !(p = kmalloc(sizeof *p))) return NULL;
    p->off = PKT_HEADROOM + ETH_HLEN + 20 + 60;
    p->len = 0;
    p->next = NULL;
    p->seq = 0;
    p->big = false;
    return p;
}

struct pkt *pkt_alloc_big(size_t len)
{
    size_t room = PKT_HEADROOM + ETH_HLEN + 20 + 8;
    if (room + len <= PKT_SIZE) { struct pkt *p = pkt_alloc(); if (p) p->off = (uint16_t)room; return p; }
    if (len > 65535) return NULL;
    struct pkt *p = kmalloc(sizeof *p - PKT_SIZE + room + len);
    if (!p) return NULL;
    p->off = (uint16_t)room;
    p->len = 0;
    p->next = NULL;
    p->seq = 0;
    p->big = true;
    return p;
}

void pkt_free(struct pkt *p)
{
    if (!p) return;
    if (p->big) { kfree(p); return; }
    uint64_t f = irq_save();
    if (pkt_cached < PKT_CACHE) { p->next = pkt_cache; pkt_cache = p; pkt_cached++; p = NULL; }
    irq_restore(f);
    kfree(p);
}

/* ---- checksums (RFC 1071) ------------------------------------------------------ */
uint32_t inet_csum_add(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *b = data;
    uint64_t s = sum;
    while (len >= 8) {                      /* 8 bytes at a time, folded at the end */
        uint64_t w;
        memcpy(&w, b, 8);
        s += (w & 0xffffffffu) + (w >> 32);
        b += 8; len -= 8;
    }
    while (len >= 2) { uint16_t w; memcpy(&w, b, 2); s += w; b += 2; len -= 2; }
    if (len) s += *b;                       /* little-endian: the odd byte is the low half */
    while (s >> 32) s = (s & 0xffffffffu) + (s >> 32);
    return (uint32_t)s;
}

uint16_t inet_csum(const void *data, size_t len, uint32_t sum)
{
    uint64_t s = inet_csum_add(data, len, sum);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

uint32_t inet_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    uint64_t s = (uint64_t)(src & 0xffff) + (src >> 16) + (dst & 0xffff) + (dst >> 16) +
                 be16(proto) + be16(len);
    while (s >> 32) s = (s & 0xffffffffu) + (s >> 32);
    return (uint32_t)s;
}

/* ---- interfaces --------------------------------------------------------------- */
struct netif *netif_list(void) { return ifaces; }
struct netif *netif_loopback(void) { return &lo; }

struct netif *netif_by_name(const char *name)
{
    for (struct netif *n = ifaces; n; n = n->next) if (!strcmp(n->name, name)) return n;
    return NULL;
}

struct netif *netif_by_index(int index)
{
    for (struct netif *n = ifaces; n; n = n->next) if (n->index == index) return n;
    return NULL;
}

void netif_register(struct netif *nif)
{
    uint64_t f = irq_save();
    static int neth;
    if (!nif->name[0]) snprintf(nif->name, sizeof nif->name, "eth%d", neth++);   /* drivers leave it empty */
    nif->index = next_index++;
    nif->next = NULL;
    struct netif **pp = &ifaces;
    while (*pp) pp = &(*pp)->next;
    *pp = nif;
    irq_restore(f);
    if (nif != &lo)
        kprintf("net: %s: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, MTU %u\n", nif->name, nif->driver,
                nif->mac[0], nif->mac[1], nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5], nif->mtu);
}

/* ---- routing ------------------------------------------------------------------ */
static int prefix_len(uint32_t mask) { return mask_bits(mask); }

struct netif *route_lookup(uint32_t dst, uint32_t *nexthop)
{
    struct route *best = NULL;
    for (int i = 0; i < MAX_ROUTES; i++) {
        struct route *r = &routes[i];
        if (!r->used || (dst & r->mask) != r->dst || !(r->nif->flags & IFF_UP)) continue;
        if (!best || prefix_len(r->mask) > prefix_len(best->mask)) best = r;
    }
    if (!best) return NULL;
    if (nexthop) *nexthop = best->gw ? best->gw : dst;
    return best->nif;
}

int route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct netif *nif)
{
    uint64_t f = irq_save();
    int free = -1;
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (routes[i].used && routes[i].dst == (dst & mask) && routes[i].mask == mask) {
            routes[i].gw = gw; routes[i].nif = nif;     /* replace */
            irq_restore(f);
            return 0;
        }
        if (!routes[i].used && free < 0) free = i;
    }
    if (free < 0) { irq_restore(f); return -1; }
    routes[free] = (struct route){ dst & mask, mask, gw, nif, true };
    irq_restore(f);
    return 0;
}

/* The interface the default route leaves through, or NULL. */
struct netif *route_default_if(void)
{
    for (int i = 0; i < MAX_ROUTES; i++)
        if (routes[i].used && !routes[i].mask) return routes[i].nif;
    return NULL;
}

int route_del(uint32_t dst, uint32_t mask)
{
    for (int i = 0; i < MAX_ROUTES; i++)
        if (routes[i].used && routes[i].dst == (dst & mask) && routes[i].mask == mask) { routes[i].used = false; return 0; }
    return -1;
}

void route_flush_if(struct netif *nif)
{
    for (int i = 0; i < MAX_ROUTES; i++) if (routes[i].used && routes[i].nif == nif) routes[i].used = false;
}

void netif_set_addr(struct netif *nif, uint32_t ip, uint32_t mask)
{
    uint64_t f = irq_save();
    if (nif->ip && nif->mask) route_del(nif->ip & nif->mask, nif->mask);
    nif->ip = ip;
    nif->mask = mask;
    if (ip && mask) route_add(ip & mask, mask, 0, nif);
    irq_restore(f);
}

bool ip_is_local(uint32_t ip)
{
    if ((ip & 0xff) == 127) return true;
    for (struct netif *n = ifaces; n; n = n->next) if (n->ip && n->ip == ip) return true;
    return false;
}

uint16_t netif_mtu_for(uint32_t dst)
{
    if (ip_is_local(dst)) return lo.mtu;
    struct netif *n = route_lookup(dst, NULL);
    return n ? n->mtu : 576;
}

uint32_t ip_source_for(uint32_t dst)
{
    if ((dst & 0xff) == 127) return dst;                /* 127.x talks to itself */
    struct netif *n = route_lookup(dst, NULL);
    return n ? n->ip : 0;
}

/* ---- receiving ------------------------------------------------------------------- */
void net_rx_frame(struct netif *nif, const void *frame, size_t len)
{
    if (len < ETH_HLEN || len > ETH_FRAME_MAX) { nif->rx_errors++; return; }
    nif->rx_packets++;
    nif->rx_bytes += len;
    if (!(nif->flags & IFF_UP)) { nif->rx_dropped++; return; }
    const uint8_t *f = frame;
    static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (memcmp(f, nif->mac, ETH_ALEN) && memcmp(f, bcast, ETH_ALEN)) { nif->rx_dropped++; return; }
    uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    if (type != ETHTYPE_IP && type != ETHTYPE_ARP) { nif->rx_dropped++; return; }
    struct pkt *p = pkt_alloc();
    if (!p) { nif->rx_dropped++; return; }
    p->off = PKT_HEADROOM;
    p->len = (uint16_t)len;
    memcpy(pkt_data(p), frame, len);
    pkt_pull(p, ETH_HLEN);
    if (type == ETHTYPE_ARP) arp_input(nif, p);
    else ip_input(nif, p);
}

/* Loopback "transmit": queue the IP packet. The queue is delivered at the
 * end of the socket call that sent it (net_lo_flush, before it sleeps or
 * returns), or by netd: never in the middle of the stack, whose callers
 * still update the state after sending. */
static bool lo_draining;

static void lo_drain(void)
{
    if (lo_draining) return;
    lo_draining = true;
    while (lo_queue) {
        struct pkt *p = lo_queue;
        lo_queue = p->next;
        if (!lo_queue) lo_tail = NULL;
        lo_qlen--;
        lo.rx_packets++; lo.rx_bytes += p->len;
        ip_input(&lo, p);
    }
    lo_draining = false;
}

bool net_lo_flush(void)
{
    if (!lo_queue || lo_draining) return false;
    uint64_t f = irq_save();
    lo_drain();
    irq_restore(f);
    return true;
}

static int lo_send(struct pkt *p)
{
    if (lo_qlen >= 512) { lo.tx_dropped++; pkt_free(p); return -1; }
    lo.tx_packets++; lo.tx_bytes += p->len;
    p->next = NULL;
    if (lo_tail) lo_tail->next = p; else lo_queue = p;
    lo_tail = p;
    lo_qlen++;
    netd_wake = true;                       /* netd delivers it if no socket call does */
    return 0;
}

int ip_output_if(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl);
/* Hand a finished IP packet to an interface (net/ipv4.c calls this). */
int netif_output(struct netif *nif, uint32_t nexthop, struct pkt *p)
{
    if (nif == &lo) return lo_send(p);
    return arp_output(nif, nexthop, p);
}

/* ---- the network thread ------------------------------------------------------ */
uint64_t net_now(void) { return time_ms(); }

void net_set_timer(uint64_t when)
{
    if (!next_timer || when < next_timer) next_timer = when;
}

void net_kick(void)
{
    netd_wake = true;
    if (netd) wakeup(&netd_chan);
}

/* The boot CPU's 1 kHz tick: a received frame or a due timer wakes netd. */
void net_tick(void)
{
    if (!netd) return;
    bool due = netd_wake || (next_timer && time_ms() >= next_timer);
    for (struct netif *n = ifaces; n && !due; n = n->next)
        if (n->pending && n->pending(n)) due = true;
    if (due) wakeup(&netd_chan);
}

static int netd_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        netd_wake = false;
        for (struct netif *n = ifaces; n; n = n->next) if (n->poll) n->poll(n);
        lo_drain();
        uint64_t now = time_ms();
        if (next_timer && now >= next_timer) {
            next_timer = 0;
            arp_timer(now);
            tcp_timer(now);
            dhcp_timer(now);
        }
        if (!netd_wake && !lo_queue) {
            bool more = false;
            for (struct netif *n = ifaces; n; n = n->next) if (n->pending && n->pending(n)) more = true;
            if (!more) sleep_on_until(&netd_chan, next_timer ? next_timer : 0);
        }
        irq_restore(f);
    }
    return 0;
}

/* ---- configuration --------------------------------------------------------------- */
static bool cmdline_get(const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *s = g_boot.cmdline; *s; s++)
        if ((s == g_boot.cmdline || s[-1] == ' ') && !strncmp(s, key, k) && s[k] == '=') {
            s += k + 1;
            size_t n = 0;
            while (s[n] && s[n] != ' ' && n + 1 < cap) { out[n] = s[n]; n++; }
            out[n] = 0;
            return true;
        }
    return false;
}

/* "a.b.c.d" -> network order; false if malformed. */
static bool parse_ip(const char **sp, uint32_t *out)
{
    const char *s = *sp;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned o = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (unsigned)(*s - '0'); s++; if (o > 255) return false; }
        v |= o << (8 * i);
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    *sp = s;
    *out = v;
    return true;
}

static bool static_config(struct netif *nif)
{
    char v[128];
    if (!cmdline_get("kestrel.ip", v, sizeof v)) return false;
    const char *s = v;
    uint32_t ip, gw = 0, dns = 0;
    int plen = 24;
    if (!parse_ip(&s, &ip)) goto bad;
    if (*s == '/') { s++; plen = (int)strtol(s, (char **)&s, 10); if (plen < 1 || plen > 32) goto bad; }
    if (*s == ',') { s++; if (!parse_ip(&s, &gw)) goto bad; }
    if (*s == ',') { s++; if (!parse_ip(&s, &dns)) goto bad; }
    uint32_t mask = be32(plen == 32 ? 0xffffffffu : ~(0xffffffffu >> plen));
    netif_set_addr(nif, ip, mask);
    if (gw) route_add(0, 0, gw, nif);
    if (dns) net_dns[0] = dns;
    kprintf("net: %s: static address %u.%u.%u.%u/%d\n", nif->name, ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24, plen);
    return true;
bad:
    kprintf("net: kestrel.ip=%s not understood (ADDR/PREFIX[,GATEWAY[,DNS]])\n", v);
    return false;
}

void net_init(void)
{
    strlcpy(lo.name, "lo", sizeof lo.name);
    lo.driver = "loopback";
    lo.mtu = 16436;                     /* as Linux had it: big segments, few copies */
    lo.flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
    netif_register(&lo);
    netif_set_addr(&lo, IP4(127, 0, 0, 1), IP4(255, 0, 0, 0));

    virtio_net_probe();
    e1000_probe();
    rtl8169_probe();
    rtl8139_probe();

    char mode[16] = "dhcp";
    cmdline_get("kestrel.net", mode, sizeof mode);
    bool first = true;
    for (struct netif *n = ifaces; n; n = n->next) {
        if (n == &lo) continue;
        if (!strcmp(mode, "off")) { kprintf("net: %s left down (kestrel.net=off)\n", n->name); continue; }
        n->flags |= IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
        if (n->link_up && n->link_up(n)) n->flags |= IFF_RUNNING;
        if (first && static_config(n)) { first = false; continue; }
        first = false;
        dhcp_start(n);
    }
    net_proc_init();
    netd = task_create("netd", netd_main, NULL);
    if (!netd) kprintf("net: cannot start netd\n");
}
