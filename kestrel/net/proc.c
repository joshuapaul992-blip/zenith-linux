/* net/proc.c -- /proc/net: dev, route, arp, tcp, udp, pnp (Linux formats)
 *
 * /proc/net/pnp lists the name servers the way Linux's kernel DHCP client
 * does, so /etc/resolv.conf can be a symbolic link to it. */
#include <kernel/net.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>

static size_t gen_dev(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    uint64_t f = irq_save();
    size_t n = (size_t)snprintf(buf, cap,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (struct netif *i = netif_list(); i && n < cap; i = i->next)
        n += (size_t)snprintf(buf + n, cap - n, "%6s: %7lu %7lu %4lu %4lu    0     0          0         0 %8lu %7lu %4lu %4lu    0     0       0          0\n",
                              i->name, i->rx_bytes, i->rx_packets, i->rx_errors, i->rx_dropped,
                              i->tx_bytes, i->tx_packets, i->tx_errors, i->tx_dropped);
    irq_restore(f);
    return n < cap ? n : cap;
}

static size_t gen_route(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    uint64_t f = irq_save();
    size_t n = (size_t)snprintf(buf, cap, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    for (int i = 0; i < MAX_ROUTES && n < cap; i++) {
        struct route *r = &routes[i];
        if (!r->used || r->nif == netif_loopback()) continue;
        unsigned flags = 0x1 | (r->gw ? 0x2 : 0) | (r->mask == 0xffffffffu ? 0x4 : 0);
        n += (size_t)snprintf(buf + n, cap - n, "%s\t%08X\t%08X\t%04X\t0\t0\t0\t%08X\t0\t0\t0\n",
                              r->nif->name, r->dst, r->gw, flags, r->mask);
    }
    irq_restore(f);
    return n < cap ? n : cap;
}

static size_t gen_arp(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    uint64_t f = irq_save();
    size_t n = arp_proc(buf, cap);
    irq_restore(f);
    return n;
}

static size_t gen_tcp(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    uint64_t f = irq_save();
    size_t n = tcp_proc(buf, cap);
    irq_restore(f);
    return n;
}

static size_t gen_udp(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    uint64_t f = irq_save();
    size_t n = udp_proc(buf, cap);
    irq_restore(f);
    return n;
}

static size_t gen_pnp(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    size_t n = 0;
    if (net_domain[0]) n += (size_t)snprintf(buf + n, cap - n, "domain %s\nsearch %s\n", net_domain, net_domain);
    for (int i = 0; i < 3 && n < cap; i++)
        if (net_dns[i])
            n += (size_t)snprintf(buf + n, cap - n, "nameserver %u.%u.%u.%u\n",
                                  net_dns[i] & 0xff, net_dns[i] >> 8 & 0xff, net_dns[i] >> 16 & 0xff, net_dns[i] >> 24);
    return n < cap ? n : cap;
}

static size_t gen_snmp(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    return (size_t)snprintf(buf, cap, "Ip: InReceives OutRequests InDiscards ReasmReqds\nIp: %lu %lu %lu %lu\n",
                            ip_stats(0), ip_stats(1), ip_stats(2), ip_stats(3));
}

void net_proc_init(void)
{
    struct vnode *proc;
    if (vfs_lookup("/proc", &proc) < 0) return;
    struct vnode *d = pseudo_dir(proc, "net");
    pseudo_file(d, "dev", gen_dev, NULL);
    pseudo_file(d, "route", gen_route, NULL);
    pseudo_file(d, "arp", gen_arp, NULL);
    pseudo_file(d, "tcp", gen_tcp, NULL);
    pseudo_file(d, "udp", gen_udp, NULL);
    pseudo_file(d, "pnp", gen_pnp, NULL);
    pseudo_file(d, "snmp", gen_snmp, NULL);
}
