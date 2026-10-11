/* include/kernel/net.h -- network interfaces and the IPv4 stack (net/)
 *
 * Interfaces. A driver fills in a struct netif (name, MAC, MTU and three
 * operations) and calls netif_register(). `xmit` sends one Ethernet frame;
 * `poll` hands every received frame to net_rx_frame() and reclaims sent
 * buffers; `pending` is a cheap check (a ring index compare) that the 1 kHz
 * system tick makes to wake the network thread. No interrupt is needed, and
 * an idle network costs one memory read per tick. "lo" (127.0.0.1/8) is
 * built in.
 *
 * Context. All stack state is changed with interrupts off on the CPU that
 * holds the big kernel lock, as in the rest of the kernel, so neither a
 * preemption nor the network thread can run in the middle of an update.
 * The network thread ("netd") receives, runs the timers (ARP, TCP
 * retransmission, DHCP) and sleeps until the next deadline or new frames.
 * System calls send directly.
 *
 * Byte order. Addresses and ports are kept in network byte order
 * everywhere (as in struct sockaddr_in); `be32`/`be16` convert numbers.
 */
#ifndef KESTREL_NET_H
#define KESTREL_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static inline int mask_bits(uint32_t m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }
#define IP4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HLEN + ETH_MTU)
#define ETHTYPE_IP      0x0800
#define ETHTYPE_ARP     0x0806

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

/* ---- packet buffers ------------------------------------------------------
 * One packet in one flat buffer, with headroom so that each layer can put
 * its header in front of the payload without copying. */
#define PKT_HEADROOM    64
#define PKT_SIZE        (PKT_HEADROOM + ETH_FRAME_MAX + 64)
struct pkt {
    uint16_t off, len;              /* data = buf + off, len bytes          */
    struct pkt *next;               /* queues (ARP pending, loopback, OOO)  */
    uint32_t seq;                   /* TCP: first sequence number (OOO queue) */
    uint32_t saddr;                 /* datagram queues: sender address/port */
    uint16_t sport;
    bool     big;                   /* from pkt_alloc_big: buf is longer, freed with kfree */
    uint8_t  buf[PKT_SIZE];
};
struct pkt *pkt_alloc(void);                    /* off = PKT_HEADROOM + ETH_HLEN + 20 + 60 */
void        pkt_free(struct pkt *p);
/* A packet of `len` bytes of data, more than a pkt holds (IP datagrams up
 * to 64 KiB: reassembly, large UDP sends); off = PKT_HEADROOM + 14 + 20 + 8. */
struct pkt *pkt_alloc_big(size_t len);
static inline uint8_t *pkt_data(struct pkt *p) { return p->buf + p->off; }
static inline uint8_t *pkt_push(struct pkt *p, uint16_t n) { p->off -= n; p->len += n; return p->buf + p->off; }
static inline void     pkt_pull(struct pkt *p, uint16_t n) { p->off += n; p->len -= n; }

/* ---- interfaces ------------------------------------------------------------ */
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_RUNNING     0x40
#define IFF_MULTICAST   0x1000
#define IFNAMSIZ        16

struct netif {
    char      name[IFNAMSIZ];
    int       index;                /* 1 = lo, then in registration order   */
    uint8_t   mac[ETH_ALEN];
    uint16_t  mtu;
    uint32_t  flags;                /* IFF_*                                */
    uint32_t  ip, mask;             /* network order; 0 = not configured    */
    const char *driver;             /* "virtio-net", "e1000", "loopback"    */
    /* driver operations (not used for loopback) */
    int     (*xmit)(struct netif *nif, const void *frame, size_t len);
    void    (*poll)(struct netif *nif);
    bool    (*pending)(struct netif *nif);
    bool    (*link_up)(struct netif *nif);
    void     *priv;
    /* counters (/proc/net/dev) */
    uint64_t  rx_packets, rx_bytes, rx_dropped, rx_errors;
    uint64_t  tx_packets, tx_bytes, tx_dropped, tx_errors;
    struct netif *next;
};

void          net_init(void);           /* after the VFS and the scheduler */
void          netif_register(struct netif *nif);
struct netif *netif_list(void);
struct netif *netif_by_name(const char *name);
struct netif *netif_by_index(int index);
struct netif *netif_loopback(void);
void          net_rx_frame(struct netif *nif, const void *frame, size_t len);   /* from a driver's poll */
void          net_tick(void);           /* the boot CPU's timer interrupt */
bool          net_lo_flush(void);       /* deliver queued loopback packets; true if there were any */
void          net_kick(void);           /* wake netd now (a timer was armed) */
uint64_t      net_now(void);            /* ms */
void          net_set_timer(uint64_t when_ms);  /* netd wakes no later than this */

/* drivers (drivers/net/) */
void virtio_net_probe(void);
void e1000_probe(void);
void rtl8139_probe(void);
void rtl8169_probe(void);

/* ---- routing ---------------------------------------------------------------- */
struct route { uint32_t dst, mask, gw; struct netif *nif; bool used; };
#define MAX_ROUTES 16
extern struct route routes[MAX_ROUTES];
struct netif *route_lookup(uint32_t dst, uint32_t *nexthop);    /* NULL: unreachable */
int  route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct netif *nif);
int  route_del(uint32_t dst, uint32_t mask);
struct netif *route_default_if(void);   /* where the default route goes, or NULL */
void route_flush_if(struct netif *nif);
void netif_set_addr(struct netif *nif, uint32_t ip, uint32_t mask);   /* also the subnet route */
bool ip_is_local(uint32_t ip);          /* one of ours (any interface)   */
uint32_t ip_source_for(uint32_t dst);   /* address a packet to dst leaves with */
uint16_t netif_mtu_for(uint32_t dst);   /* MTU of the route to dst */
extern uint32_t net_dns[3];             /* name servers from DHCP / the command line */
extern char net_domain[64];

/* ---- layers (net/) ---------------------------------------------------------- */
uint16_t inet_csum(const void *data, size_t len, uint32_t sum);
uint32_t inet_csum_add(const void *data, size_t len, uint32_t sum);   /* partial (unfolded) */
uint32_t inet_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

void arp_input(struct netif *nif, struct pkt *p);
int  arp_output(struct netif *nif, uint32_t nexthop, struct pkt *p);   /* takes p */
void arp_timer(uint64_t now);
void arp_flush_if(struct netif *nif);
size_t arp_proc(char *buf, size_t cap);

void ip_input(struct netif *nif, struct pkt *p);
/* Send p (payload at pkt_data) to dst; src 0 = pick. Takes p. */
int  ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl, uint8_t tos, bool df);
int  ip_output_if(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl);
uint64_t ip_stats(int which);

void icmp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void icmp_send_unreach(struct pkt *orig, uint8_t code);     /* orig: IP header at pkt_data */
void udp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void tcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void tcp_timer(uint64_t now);
void icmp_error_input(uint8_t type, uint8_t code, const uint8_t *iph, size_t len);  /* to TCP/UDP sockets */

void dhcp_start(struct netif *nif);
void dhcp_timer(uint64_t now);
void dhcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint16_t sport, uint16_t dport);
bool dhcp_busy(void);

void net_proc_init(void);
size_t tcp_proc(char *buf, size_t cap);
size_t udp_proc(char *buf, size_t cap);

#endif
