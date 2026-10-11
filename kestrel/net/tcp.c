/* net/tcp.c -- TCP (RFC 9293), with the parts real peers expect:
 *
 *   - MSS and window scaling options (RFC 7323); 128 KiB send and receive
 *     buffers per connection
 *   - retransmission timeout from measured round trips (RFC 6298, Karn),
 *     exponential backoff, 15 retries (6 for a SYN), then ETIMEDOUT
 *   - Reno congestion control: slow start, congestion avoidance, fast
 *     retransmit and recovery after three duplicate ACKs (RFC 5681)
 *   - out-of-order segments kept and merged when the gap fills
 *   - delayed ACKs (40 ms, or at once every second full segment), Nagle
 *     unless TCP_NODELAY, window updates that avoid silly windows
 *   - zero-window probes, keepalive (SO_KEEPALIVE), TIME_WAIT (60 s),
 *     orphaned connections finishing their close after the file is gone
 *   - RST and SYN handling hardened as in RFC 5961 (challenge ACKs)
 *   - listen backlog with the connections still in their handshake counted
 *
 * A struct tcpcb is the connection; the socket (struct isock) points at it
 * while a file is open. A tcb outlives its file when it still has to close
 * gracefully, and exists before any file while it waits in a listener's
 * accept queue. All of it runs with interrupts off (net.h). Sequence numbers
 * compare with SEQ_LT and friends (modulo 2^32). */
#include "sock.h"
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/process.h>

#define SEQ_LT(a, b)    ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)   ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)    ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)   ((int32_t)((a) - (b)) >= 0)

#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PSH  0x08
#define TH_ACK  0x10
#define TH_URG  0x20

#define TCP_BUF         (128 * 1024)
#define TCP_MSS_DEFAULT 536
#define RTO_INIT        1000
#define RTO_MIN         200
#define RTO_MAX         120000
#define MAX_RETRIES     15
#define SYN_RETRIES     6
#define DELACK_MS       40
#define TIMEWAIT_MS     60000
#define FIN_WAIT2_MS    60000
#define KEEP_IDLE_MS    7200000
#define KEEP_INTVL_MS   75000
#define KEEP_PROBES     9
#define MAX_BACKLOG     4096

enum tstate { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT1, FIN_WAIT2,
              CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
static const char *const state_names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED",
    "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
/* Linux's numbering, for /proc/net/tcp */
static const uint8_t linux_state[] = { 7, 10, 2, 3, 1, 4, 5, 8, 11, 9, 6 };

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off, flags;
    uint16_t win, csum, urg;
} __attribute__((packed));

struct tcpcb {
    struct isock *sock;                 /* NULL: no file (orphan, or not yet accepted) */
    struct tcpcb   *listener;             /* SYN_RCVD / accept queue: the listening tcb */
    struct tcpcb   *next;                 /* all tcbs */
    struct tcpcb   *aq_next;              /* the listener's accept queue */
    enum tstate   state;
    uint32_t laddr, raddr;
    uint16_t lport, rport;
    int      err;                       /* positive errno for the socket */
    bool     ever_connected;
    /* send side: sbuf holds the bytes from snd_una on (sent or not) */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint32_t snd_max;                   /* highest sequence sent (snd_nxt goes back on a timeout) */
    uint8_t  snd_wscale, rcv_wscale;
    bool     wscale_ok;
    uint8_t *sbuf;
    uint32_t sbuf_head, sbuf_len, sbuf_size;
    bool     fin_queued, fin_sent, fin_acked;
    uint16_t mss;                       /* what we send per segment */
    uint32_t cwnd, ssthresh, recover;
    int      dupacks;
    bool     in_recovery;
    /* receive side */
    uint32_t irs, rcv_nxt, rcv_adv;     /* rcv_adv: right edge last advertised */
    uint8_t *rbuf;
    uint32_t rbuf_head, rbuf_len, rbuf_size;
    bool     fin_rcvd;
    struct pkt *ooo;                    /* out-of-order segments, by sequence */
    int      nooo;
    uint32_t unacked_rx;                /* bytes received since our last ACK */
    bool     ack_now;
    /* timers (ms, 0 = off) */
    uint64_t rto_at, delack_at, keep_at, tw_at;
    uint32_t rto, srtt, rttvar;         /* srtt/rttvar 0: no sample yet */
    int      retries;
    uint32_t rtt_seq;                   /* timing the segment ending here */
    uint64_t rtt_start;                 /* 0: not timing */
    int      keep_probes;
    uint64_t last_rx;
    /* listener */
    struct tcpcb *aq, *aq_tail;
    int      aq_len, syn_len, backlog;
    /* options mirrored from the socket (orphans keep them) */
    bool     nodelay, keepalive;
    uint8_t  ttl, tos;
};

static struct tcpcb *tcbs;

static bool tcbs_contains(struct tcpcb *x)
{
    for (struct tcpcb *t = tcbs; t; t = t->next) if (t == x) return true;
    return false;
}
static uint64_t st_active, st_passive, st_fails, st_resets, st_retrans, st_in, st_out, st_rst_out;

static void tcb_free(struct tcpcb *t);
static void tcp_output(struct tcpcb *t);
static void send_ack(struct tcpcb *t);

/* ---- helpers ----------------------------------------------------------------- */
static uint32_t isn(void)
{
    static uint32_t salt;
    if (!salt) salt = (uint32_t)rdtsc() * 2654435761u;
    return (uint32_t)(time_us() >> 2) + (salt = salt * 1103515245u + 12345u);   /* RFC 6528-ish: clock + noise */
}

static void set_timer(uint64_t *field, uint64_t when)
{
    *field = when;
    if (when) net_set_timer(when);
}

static uint32_t rwnd_free(struct tcpcb *t) { return t->rbuf_size - t->rbuf_len; }

static void wake(struct tcpcb *t)
{
    wakeup(t);
    if (t->listener) wakeup(t->listener);
}

/* Become CLOSED: an orphan disappears, a socket keeps the tcb (and its error). */
static void enter_closed(struct tcpcb *t, int err)
{
    if (err && !t->err) t->err = err;
    t->state = CLOSED;
    t->rto_at = t->delack_at = t->keep_at = t->tw_at = 0;
    while (t->ooo) { struct pkt *p = t->ooo; t->ooo = p->next; pkt_free(p); }
    t->nooo = 0;
    wake(t);
    if (!t->sock && !t->listener) tcb_free(t);
}

static struct tcpcb *tcb_new(void)
{
    struct tcpcb *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->rbuf = kmalloc(TCP_BUF);
    t->sbuf = kmalloc(TCP_BUF);
    if (!t->rbuf || !t->sbuf) { kfree(t->rbuf); kfree(t->sbuf); kfree(t); return NULL; }
    t->rbuf_size = t->sbuf_size = TCP_BUF;
    t->rto = RTO_INIT;
    t->mss = TCP_MSS_DEFAULT;
    t->rcv_wscale = 2;                  /* 128 KiB / 4 fits in 16 bits */
    t->ttl = 64;
    t->next = tcbs;
    tcbs = t;
    return t;
}

static void tcb_free(struct tcpcb *t)
{
    for (struct tcpcb **pp = &tcbs; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    while (t->ooo) { struct pkt *p = t->ooo; t->ooo = p->next; pkt_free(p); }
    wakeup(t);
    kfree(t->rbuf);
    kfree(t->sbuf);
    kfree(t);
}

/* ---- segments out ---------------------------------------------------------------- */
static uint16_t adv_window(struct tcpcb *t)
{
    uint32_t w = rwnd_free(t);
    /* never shrink the window we promised */
    if (SEQ_LT(t->rcv_nxt + w, t->rcv_adv)) w = t->rcv_adv - t->rcv_nxt;
    /* receiver-side silly window avoidance: don't offer crumbs */
    if (w < t->rbuf_size / 4 && w < t->mss && SEQ_GEQ(t->rcv_nxt + w, t->rcv_adv)) w = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
    uint8_t sh = t->wscale_ok ? t->rcv_wscale : 0;
    uint32_t v = w >> sh;
    if (v > 0xffff) v = 0xffff;
    if (SEQ_GT(t->rcv_nxt + (v << sh), t->rcv_adv)) t->rcv_adv = t->rcv_nxt + (v << sh);
    return (uint16_t)v;
}

/* Build and send one segment: `len` bytes of sbuf at sequence `seq`. */
static void send_segment(struct tcpcb *t, uint32_t seq, uint32_t len, uint8_t flags)
{
    size_t optlen = (flags & TH_SYN) ? 8 : 0;          /* MSS + window scale (+NOP) */
    struct pkt *p = pkt_alloc_big(20 + optlen + len);
    if (!p) return;
    struct tcp_hdr *h = (struct tcp_hdr *)pkt_data(p);
    p->len = (uint16_t)(20 + optlen + len);
    h->sport = t->lport;
    h->dport = t->rport;
    h->seq = be32(seq);
    bool syn_unacked = t->state == SYN_SENT;
    h->ack = syn_unacked ? 0 : be32(t->rcv_nxt);
    if (!syn_unacked) flags |= TH_ACK;
    h->off = (uint8_t)((20 + optlen) / 4) << 4;
    h->flags = flags;
    h->urg = 0;
    uint8_t *o = (uint8_t *)(h + 1);
    if (flags & TH_SYN) {
        uint16_t ourmss = (uint16_t)(netif_mtu_for(t->raddr) - 40);
        o[0] = 2; o[1] = 4; o[2] = (uint8_t)(ourmss >> 8); o[3] = (uint8_t)ourmss;
        o[4] = 1; o[5] = 3; o[6] = 3; o[7] = t->rcv_wscale;   /* NOP, window scale */
        h->win = be16((uint16_t)(rwnd_free(t) > 0xffff ? 0xffff : rwnd_free(t)));   /* unscaled in a SYN */
        t->rcv_adv = t->rcv_nxt + be16(h->win);
    } else {
        h->win = be16(adv_window(t));
    }
    if (len) {
        uint32_t off = seq - t->snd_una;
        uint32_t start = (t->sbuf_head + off) % t->sbuf_size;
        uint32_t first = t->sbuf_size - start < len ? t->sbuf_size - start : len;
        memcpy(o + optlen, t->sbuf + start, first);
        memcpy(o + optlen + first, t->sbuf, len - first);
    }
    h->csum = 0;
    h->csum = inet_csum(h, p->len, inet_pseudo(t->laddr, t->raddr, IPPROTO_TCP, p->len));
    if (flags & TH_ACK) { t->unacked_rx = 0; t->ack_now = false; t->delack_at = 0; }
    st_out++;
    ip_output(p, t->laddr, t->raddr, IPPROTO_TCP, t->ttl, t->tos, true);
}

static void send_ack(struct tcpcb *t) { send_segment(t, t->snd_nxt, 0, 0); }

/* A RST answering a segment that has no connection (RFC 9293 3.10.7.1). */
static void send_rst(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, bool with_ack)
{
    struct pkt *p = pkt_alloc();
    if (!p) return;
    struct tcp_hdr *h = (struct tcp_hdr *)pkt_data(p);
    p->len = 20;
    memset(h, 0, 20);
    h->sport = dport;
    h->dport = sport;
    h->seq = be32(seq);
    h->ack = be32(ack);
    h->off = 5 << 4;
    h->flags = TH_RST | (with_ack ? TH_ACK : 0);
    h->csum = inet_csum(h, 20, inet_pseudo(dst, src, IPPROTO_TCP, 20));
    st_rst_out++;
    ip_output(p, dst, src, IPPROTO_TCP, 64, 0, true);
}

static void abort_conn(struct tcpcb *t, int err)
{
    if (t->state != CLOSED && t->state != LISTEN && t->state != SYN_SENT && t->state != TIME_WAIT)
        send_segment(t, t->snd_nxt, 0, TH_RST);
    enter_closed(t, err);
}

static uint32_t flight(struct tcpcb *t) { return t->snd_max - t->snd_una; }

static void arm_rto(struct tcpcb *t)
{
    set_timer(&t->rto_at, net_now() + t->rto);
}

/* Send what the windows and Nagle allow; FIN when the data is all out. */
static void tcp_output(struct tcpcb *t)
{
    if (t->state < ESTABLISHED || t->state == TIME_WAIT || t->state == CLOSED) return;
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una;         /* data already sent */
        if (t->fin_sent || off > t->sbuf_len) break;
        uint32_t unsent = t->sbuf_len - off;
        uint32_t wnd = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
        uint32_t usable = wnd > off ? wnd - off : 0;
        uint32_t len = unsent < t->mss ? unsent : t->mss;
        if (len > usable) len = usable;
        bool fin = t->fin_queued && len == unsent;
        if (!len && !fin) break;
        if (len && len < t->mss && len == unsent && off && !t->nodelay && !fin) break;   /* Nagle */
        if (len && len < t->mss && len < unsent && off) break;                           /* sender SWS avoidance */
        uint8_t flags = fin ? TH_FIN : 0;
        if (len && len == unsent) flags |= TH_PSH;
        if (!t->rtt_start && len && t->snd_nxt == t->snd_max) { t->rtt_start = net_now(); t->rtt_seq = t->snd_nxt + len; }
        send_segment(t, t->snd_nxt, len, flags);
        t->snd_nxt += len;
        if (fin) { t->fin_sent = true; t->snd_nxt++; }
        if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        if (!t->rto_at) arm_rto(t);
        if (fin) break;
    }
    /* zero window with data waiting and nothing in flight: probe on the RTO timer */
    if (t->snd_una == t->snd_max && t->sbuf_len && !t->rto_at) arm_rto(t);
}

/* ---- lookup ---------------------------------------------------------------------------- */
static struct tcpcb *find(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport)
{
    struct tcpcb *lst = NULL;
    for (struct tcpcb *t = tcbs; t; t = t->next) {
        if (t->lport != dport) continue;
        if (t->state == LISTEN) {
            if (!t->laddr || t->laddr == dst) { if (!lst || t->laddr) lst = t; }
            continue;
        }
        if (t->state != CLOSED && t->raddr == src && t->rport == sport && t->laddr == dst) return t;
    }
    return lst;
}

bool tcp_port_used(uint32_t addr, uint16_t port, struct isock *except, bool reuse)
{
    for (struct tcpcb *t = tcbs; t; t = t->next) {
        if (t->lport != port || (except && t == except->tcb)) continue;
        if (t->laddr && addr && t->laddr != addr) continue;
        if (t->state == CLOSED && !t->sock) continue;
        /* SO_REUSEADDR: a port whose connections are only closing may be reused */
        if (reuse && t->state != LISTEN) continue;
        if (t->state == TIME_WAIT && reuse) continue;
        return true;
    }
    return false;
}

/* ---- options ---------------------------------------------------------------------- */
static void parse_options(struct tcpcb *t, const uint8_t *o, size_t n, bool syn)
{
    bool ws = false;
    while (n) {
        uint8_t k = o[0];
        if (k == 0) break;
        if (k == 1) { o++; n--; continue; }
        if (n < 2 || o[1] < 2 || o[1] > n) break;
        if (syn && k == 2 && o[1] == 4) {
            uint16_t m = (uint16_t)(o[2] << 8 | o[3]);
            uint16_t ours = (uint16_t)(netif_mtu_for(t->raddr) - 40);
            t->mss = m < ours ? m : ours;
            if (t->mss < 64) t->mss = 64;
        } else if (syn && k == 3 && o[1] == 3) {
            ws = true;
            t->snd_wscale = o[2] > 14 ? 14 : o[2];
        }
        n -= o[1];
        o += o[1];
    }
    if (syn) {
        t->wscale_ok = ws;
        if (!ws) t->snd_wscale = 0;
    }
}

/* ---- receiving data ------------------------------------------------------------------------- */
static uint32_t rbuf_put(struct tcpcb *t, const uint8_t *d, uint32_t len)
{
    uint32_t room = rwnd_free(t);
    if (len > room) len = room;
    uint32_t tail = (t->rbuf_head + t->rbuf_len) % t->rbuf_size;
    uint32_t first = t->rbuf_size - tail < len ? t->rbuf_size - tail : len;
    memcpy(t->rbuf + tail, d, first);
    memcpy(t->rbuf, d + first, len - first);
    t->rbuf_len += len;
    return len;
}

/* Keep a copy of an out-of-order segment's data (seq in p->seq). */
static void ooo_insert(struct tcpcb *t, uint32_t seq, const uint8_t *d, uint32_t len)
{
    if (t->nooo >= 64 || !len) return;
    struct pkt **pp = &t->ooo;
    while (*pp && SEQ_LT((*pp)->seq, seq)) pp = &(*pp)->next;
    if (*pp && (*pp)->seq == seq && (*pp)->len >= len) return;  /* have it */
    struct pkt *p = pkt_alloc_big(len);
    if (!p) return;
    memcpy(pkt_data(p), d, len);
    p->len = (uint16_t)len;
    p->seq = seq;
    p->next = *pp;
    *pp = p;
    t->nooo++;
}

static void ooo_drain(struct tcpcb *t)
{
    while (t->ooo && SEQ_LEQ(t->ooo->seq, t->rcv_nxt)) {
        struct pkt *p = t->ooo;
        t->ooo = p->next;
        t->nooo--;
        uint32_t end = p->seq + p->len;
        if (SEQ_GT(end, t->rcv_nxt)) {
            uint32_t skip = t->rcv_nxt - p->seq;
            uint32_t got = rbuf_put(t, pkt_data(p) + skip, p->len - skip);
            t->rcv_nxt += got;
        }
        pkt_free(p);
    }
}

static void schedule_ack(struct tcpcb *t, uint32_t len)
{
    t->unacked_rx += len;
    if (t->ack_now || t->unacked_rx >= 2u * t->mss) { send_ack(t); return; }
    if (!t->delack_at) set_timer(&t->delack_at, net_now() + DELACK_MS);
}

/* ---- RTT and congestion --------------------------------------------------------------------- */
static void rtt_sample(struct tcpcb *t, uint32_t r)
{
    if (!r) r = 1;
    if (!t->srtt) { t->srtt = r; t->rttvar = r / 2; }
    else {
        uint32_t d = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3 * t->rttvar + d) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
    }
    uint32_t rto = t->srtt + (4 * t->rttvar > 10 ? 4 * t->rttvar : 10);
    t->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
}

static void new_ack(struct tcpcb *t, uint32_t ack)
{
    uint32_t acked = ack - t->snd_una;
    uint32_t data = acked;
    if (t->fin_queued && acked > t->sbuf_len) { data = t->sbuf_len; t->fin_acked = t->fin_sent = true; }
    if (data > t->sbuf_len) data = t->sbuf_len;
    t->sbuf_head = (t->sbuf_head + data) % t->sbuf_size;
    t->sbuf_len -= data;
    t->snd_una = ack;
    if (SEQ_GT(ack, t->snd_nxt)) t->snd_nxt = ack;     /* acks what we sent before going back */
    if (t->rtt_start && SEQ_GEQ(ack, t->rtt_seq)) {
        rtt_sample(t, (uint32_t)(net_now() - t->rtt_start));
        t->rtt_start = 0;
    }
    t->retries = 0;
    /* congestion window */
    if (t->in_recovery) {
        if (SEQ_GEQ(ack, t->recover)) { t->in_recovery = false; t->cwnd = t->ssthresh; }
        else if (t->sbuf_len) send_segment(t, t->snd_una, t->sbuf_len < t->mss ? t->sbuf_len : t->mss, 0);   /* NewReno partial ACK */
    } else if (t->cwnd < t->ssthresh) t->cwnd += data < t->mss ? data : t->mss;
    else t->cwnd += (uint32_t)((uint64_t)t->mss * t->mss / t->cwnd) + 1;
    if (t->cwnd > 4 * TCP_BUF) t->cwnd = 4 * TCP_BUF;
    t->dupacks = 0;
    if (t->snd_una == t->snd_max && !(t->sbuf_len && !t->snd_wnd)) t->rto_at = 0;
    else arm_rto(t);
    wake(t);
}

static void dup_ack(struct tcpcb *t)
{
    if (++t->dupacks == 3 && !t->in_recovery) {
        uint32_t fl = flight(t);
        t->ssthresh = fl / 2 > 2u * t->mss ? fl / 2 : 2u * t->mss;
        t->cwnd = t->ssthresh + 3u * t->mss;
        t->recover = t->snd_max;
        t->in_recovery = true;
        uint32_t len = t->sbuf_len < t->mss ? t->sbuf_len : t->mss;
        st_retrans++;
        t->rtt_start = 0;                               /* Karn */
        send_segment(t, t->snd_una, len, 0);
    } else if (t->in_recovery) {
        t->cwnd += t->mss;
    }
}

/* ---- segment arrives ---------------------------------------------------------------------- */
static void listen_input(struct tcpcb *l, const struct tcp_hdr *h, uint32_t src, uint32_t dst, const uint8_t *opt, size_t optlen)
{
    uint32_t seq = be32(h->seq);
    if (h->flags & TH_RST) return;
    if (h->flags & TH_ACK) { send_rst(src, dst, h->sport, h->dport, be32(h->ack), 0, false); return; }
    if (!(h->flags & TH_SYN)) return;
    if (l->aq_len + l->syn_len >= l->backlog) return;  /* full: the peer retries */
    struct tcpcb *t = tcb_new();
    if (!t) return;
    t->listener = l;
    t->laddr = dst; t->raddr = src;
    t->lport = h->dport; t->rport = h->sport;
    t->nodelay = l->nodelay; t->keepalive = l->keepalive; t->ttl = l->ttl; t->tos = l->tos;
    t->irs = seq;
    t->rcv_nxt = seq + 1;
    t->mss = TCP_MSS_DEFAULT;
    parse_options(t, opt, optlen, true);
    t->snd_wnd = be16(h->win);                          /* unscaled in a SYN */
    t->snd_wl1 = seq;
    t->iss = isn();
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1;
    t->cwnd = 10u * t->mss;
    t->ssthresh = 0xffffffffu;
    t->state = SYN_RCVD;
    t->last_rx = net_now();
    l->syn_len++;
    st_passive++;
    send_segment(t, t->iss, 0, TH_SYN);
    arm_rto(t);
}

static void syn_sent_input(struct tcpcb *t, const struct tcp_hdr *h, const uint8_t *opt, size_t optlen)
{
    uint32_t seq = be32(h->seq), ack = be32(h->ack);
    bool ackok = false;
    if (h->flags & TH_ACK) {
        if (SEQ_LEQ(ack, t->iss) || SEQ_GT(ack, t->snd_max)) {
            if (!(h->flags & TH_RST)) send_rst(t->raddr, t->laddr, t->rport, t->lport, ack, 0, false);
            return;
        }
        ackok = true;
    }
    if (h->flags & TH_RST) {
        if (ackok) { st_fails++; enter_closed(t, ECONNREFUSED); }
        return;
    }
    if (!(h->flags & TH_SYN)) return;
    t->irs = seq;
    t->rcv_nxt = seq + 1;
    t->rcv_adv = t->rcv_nxt;                            /* nothing promised yet in this sequence space */
    parse_options(t, opt, optlen, true);
    t->snd_wnd = be16(h->win);
    t->snd_wl1 = seq;
    t->snd_wl2 = ack;
    t->cwnd = 10u * t->mss;
    t->ssthresh = 0xffffffffu;
    if (ackok) {
        t->snd_una = ack;
        t->rto_at = 0;
        t->retries = 0;
        if (t->rtt_start) { rtt_sample(t, (uint32_t)(net_now() - t->rtt_start)); t->rtt_start = 0; }
        t->state = ESTABLISHED;
        t->ever_connected = true;
        send_ack(t);
        wake(t);
        tcp_output(t);
    } else {                                            /* simultaneous open */
        t->state = SYN_RCVD;
        send_segment(t, t->iss, 0, TH_SYN);
    }
}

void tcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    st_in++;
    if (p->len < 20) goto drop;
    struct tcp_hdr *h = (struct tcp_hdr *)pkt_data(p);
    unsigned hl = (h->off >> 4) * 4u;
    if (hl < 20 || hl > p->len) goto drop;
    if (nif != netif_loopback() && inet_csum(h, p->len, inet_pseudo(src, dst, IPPROTO_TCP, p->len))) goto drop;
    const uint8_t *opt = (const uint8_t *)(h + 1);
    size_t optlen = hl - 20;
    uint32_t seq = be32(h->seq), ack = be32(h->ack);
    uint32_t len = p->len - hl;
    const uint8_t *data = pkt_data(p) + hl;
    uint8_t fl = h->flags;
    struct tcpcb *t = find(src, h->sport, dst, h->dport);
    if (!t || t->state == CLOSED) {
        if (!(fl & TH_RST)) {
            if (fl & TH_ACK) send_rst(src, dst, h->sport, h->dport, ack, 0, false);
            else send_rst(src, dst, h->sport, h->dport, 0, seq + len + ((fl & TH_SYN) ? 1 : 0) + ((fl & TH_FIN) ? 1 : 0), true);
        }
        goto drop;
    }
    if (t->state == LISTEN) { listen_input(t, h, src, dst, opt, optlen); goto drop; }
    if (t->state == SYN_SENT) { syn_sent_input(t, h, opt, optlen); goto drop; }
    t->last_rx = net_now();
    t->keep_probes = 0;
    if (t->keepalive) set_timer(&t->keep_at, net_now() + KEEP_IDLE_MS);

    /* 1. sequence check */
    uint32_t wnd = rwnd_free(t);
    uint32_t seglen = len + ((fl & TH_SYN) ? 1 : 0) + ((fl & TH_FIN) ? 1 : 0);
    bool ok;
    if (!seglen) ok = wnd ? (SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd)) || seq == t->rcv_nxt : seq == t->rcv_nxt;
    else ok = wnd && ((SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd)) ||
                      (SEQ_GT(seq + seglen - 1, t->rcv_nxt) && SEQ_LT(seq + seglen - 1, t->rcv_nxt + wnd)) ||
                      (SEQ_LT(seq, t->rcv_nxt) && SEQ_GT(seq + seglen, t->rcv_nxt)));
    if (!ok) {
        /* A retransmitted FIN or old data: ACK it. (A probe into a zero window too.) */
        if (!(fl & TH_RST)) { t->ack_now = true; send_ack(t); }
        goto drop;
    }
    /* trim what we already have */
    if (SEQ_LT(seq, t->rcv_nxt)) {
        uint32_t skip = t->rcv_nxt - seq;
        if (fl & TH_SYN) { fl &= (uint8_t)~TH_SYN; skip--; seq++; }
        if (skip > len) skip = len;
        data += skip; len -= skip; seq += skip;
    }
    /* 2. RST: exact match resets, otherwise a challenge ACK (RFC 5961) */
    if (fl & TH_RST) {
        if (seq != t->rcv_nxt) { send_ack(t); goto drop; }
        st_resets++;
        if (t->state == SYN_RCVD && t->listener) {
            t->listener->syn_len--;
            t->listener = NULL;
            enter_closed(t, 0);
        } else enter_closed(t, t->state == CLOSE_WAIT || t->state == ESTABLISHED || t->state == FIN_WAIT1 ||
                                  t->state == FIN_WAIT2 ? ECONNRESET : t->state == SYN_RCVD ? ECONNREFUSED : 0);
        goto drop;
    }
    /* 3. a SYN on a synchronized connection: challenge ACK */
    if (fl & TH_SYN) { send_ack(t); goto drop; }
    /* 4. everything else carries an ACK */
    if (!(fl & TH_ACK)) goto drop;
    if (t->state == SYN_RCVD) {
        if (SEQ_LEQ(ack, t->snd_una) || SEQ_GT(ack, t->snd_max)) {
            send_rst(src, dst, h->sport, h->dport, ack, 0, false);
            goto drop;
        }
        t->snd_una = ack;
        t->snd_wnd = (uint32_t)be16(h->win) << t->snd_wscale;
        t->snd_wl1 = seq; t->snd_wl2 = ack;
        t->rto_at = 0;
        t->retries = 0;
        t->state = ESTABLISHED;
        t->ever_connected = true;
        struct tcpcb *l = t->listener;
        if (l) {                                        /* into the accept queue */
            l->syn_len--;
            t->aq_next = NULL;
            if (l->aq_tail) l->aq_tail->aq_next = t; else l->aq = t;
            l->aq_tail = t;
            l->aq_len++;
            wakeup(l);
        }
        wake(t);
    }
    /* 5. ACK */
    if (SEQ_GT(ack, t->snd_max)) { send_ack(t); goto drop; }
    if (SEQ_GT(ack, t->snd_una)) new_ack(t, ack);
    else if (ack == t->snd_una && !len && !(fl & TH_FIN) && t->snd_una != t->snd_max &&
             ((uint32_t)be16(h->win) << t->snd_wscale) == t->snd_wnd) dup_ack(t);
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
        uint32_t nw = (uint32_t)be16(h->win) << t->snd_wscale;
        if (nw && !t->snd_wnd) wake(t);
        t->snd_wnd = nw;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
    }
    bool fin_acked = t->fin_acked;
    switch (t->state) {
    case FIN_WAIT1:
        if (fin_acked) { t->state = FIN_WAIT2; if (!t->sock) set_timer(&t->tw_at, net_now() + FIN_WAIT2_MS); wake(t); }
        break;
    case CLOSING:
        if (fin_acked) { t->state = TIME_WAIT; t->rto_at = 0; set_timer(&t->tw_at, net_now() + TIMEWAIT_MS); wake(t); }
        break;
    case LAST_ACK:
        if (fin_acked) { enter_closed(t, 0); goto drop; }
        break;
    case TIME_WAIT:
        if (fl & TH_FIN) { send_ack(t); set_timer(&t->tw_at, net_now() + TIMEWAIT_MS); }
        goto drop;
    default:
        break;
    }
    /* 6. data */
    if (len && (t->state == ESTABLISHED || t->state == FIN_WAIT1 || t->state == FIN_WAIT2)) {
        if (t->sock && t->sock->shut_rd) {              /* nobody will read it: just accept it */
            t->rcv_nxt = seq + len;
            schedule_ack(t, len);
        } else if (seq == t->rcv_nxt) {
            uint32_t got = rbuf_put(t, data, len);
            t->rcv_nxt += got;
            if (got < len) fl &= (uint8_t)~TH_FIN;      /* the FIN comes after what we dropped */
            if (t->ooo) { ooo_drain(t); t->ack_now = true; }
            if (got) wake(t);
            schedule_ack(t, got);
        } else {
            ooo_insert(t, seq, data, len);
            fl &= (uint8_t)~TH_FIN;
            t->ack_now = true;
            send_ack(t);                                /* duplicate ACK: tells the sender of the gap */
        }
    } else if (len) {
        fl &= (uint8_t)~TH_FIN;
    }
    /* 7. FIN, once everything before it is in */
    if ((fl & TH_FIN) && seq + len == t->rcv_nxt) {
        t->rcv_nxt++;
        t->fin_rcvd = true;
        t->ack_now = true;
        send_ack(t);
        switch (t->state) {
        case ESTABLISHED: t->state = CLOSE_WAIT; break;
        case FIN_WAIT1:   t->state = fin_acked ? TIME_WAIT : CLOSING; break;
        case FIN_WAIT2:   t->state = TIME_WAIT; break;
        default: break;
        }
        if (t->state == TIME_WAIT) { t->rto_at = 0; set_timer(&t->tw_at, net_now() + TIMEWAIT_MS); }
        wake(t);
    }
    tcp_output(t);
drop:
    pkt_free(p);
}

void tcp_icmp_error(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, int err, uint8_t type, uint8_t code)
{
    for (struct tcpcb *t = tcbs; t; t = t->next) {
        if (t->laddr != src || t->lport != sport || t->raddr != dst || t->rport != dport) continue;
        if (type == 3 && code == 4) return;             /* fragmentation needed: we send DF and small anyway */
        /* hard errors end a connection attempt; on a live connection they are soft */
        if (t->state == SYN_SENT) { st_fails++; enter_closed(t, err); }
        else t->err = err;
        return;
    }
}

/* ---- timers -------------------------------------------------------------------------- */
static void retransmit_timeout(struct tcpcb *t, uint64_t now)
{
    t->rto_at = 0;
    if (t->snd_una == t->snd_max && t->sbuf_len && !t->snd_wnd) {
        /* zero-window probe: one byte past the window, backing off */
        send_segment(t, t->snd_nxt, 1, 0);
        t->snd_nxt++;
        t->snd_max = t->snd_nxt;
        t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
        arm_rto(t);
        return;
    }
    if (t->snd_una == t->snd_max) return;
    int limit = (t->state == SYN_SENT || t->state == SYN_RCVD) ? SYN_RETRIES : MAX_RETRIES;
    if (++t->retries > limit) {
        st_fails++;
        if (t->state == SYN_RCVD && t->listener) { t->listener->syn_len--; t->listener = NULL; enter_closed(t, 0); return; }
        abort_conn(t, ETIMEDOUT);
        return;
    }
    st_retrans++;
    t->rtt_start = 0;                                   /* Karn: don't time retransmissions */
    t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
    if (t->state == SYN_SENT || t->state == SYN_RCVD) {
        send_segment(t, t->iss, 0, TH_SYN);
        set_timer(&t->rto_at, now + t->rto);
        return;
    }
    uint32_t fl = flight(t);
    t->ssthresh = fl / 2 > 2u * t->mss ? fl / 2 : 2u * t->mss;
    t->cwnd = t->mss;
    t->in_recovery = false;
    t->dupacks = 0;
    t->snd_nxt = t->snd_una;                            /* go back: resend from the first unacked byte */
    if (!t->fin_acked) t->fin_sent = false;
    tcp_output(t);
    if (!t->rto_at) set_timer(&t->rto_at, now + t->rto);
}

void tcp_timer(uint64_t now)
{
    struct tcpcb *next;
    for (struct tcpcb *t = tcbs; t; t = next) {
        next = t->next;
        if (t->tw_at && now >= t->tw_at) {
            t->tw_at = 0;
            if (t->state == TIME_WAIT || t->state == FIN_WAIT2) { enter_closed(t, 0); continue; }
        }
        if (t->rto_at && now >= t->rto_at) {
            retransmit_timeout(t, now);
            if (!tcbs_contains(t)) continue;
        }
        if (t->delack_at && now >= t->delack_at) { t->delack_at = 0; send_ack(t); }
        if (t->keep_at && now >= t->keep_at) {
            t->keep_at = 0;
            if (t->keepalive && t->state == ESTABLISHED) {
                if (++t->keep_probes > KEEP_PROBES) { abort_conn(t, ETIMEDOUT); continue; }
                send_segment(t, t->snd_una - 1, 0, 0);  /* keepalive probe: an old sequence number */
                set_timer(&t->keep_at, now + KEEP_INTVL_MS);
            }
        }
        if (t->rto_at) net_set_timer(t->rto_at);
        if (t->delack_at) net_set_timer(t->delack_at);
        if (t->keep_at) net_set_timer(t->keep_at);
        if (t->tw_at) net_set_timer(t->tw_at);
    }
}

/* ---- socket calls ------------------------------------------------------------------------ */
int tcp_attach(struct isock *s)
{
    struct tcpcb *t = tcb_new();
    if (!t) return -ENOMEM;
    t->sock = s;
    s->tcb = t;
    return 0;
}

static void sync_opts(struct isock *s)
{
    struct tcpcb *t = s->tcb;
    t->nodelay = s->nodelay;
    t->keepalive = s->keepalive;
    t->ttl = s->ttl ? s->ttl : 64;
    t->tos = s->tos;
}

void tcp_setopt_nodelay(struct isock *s)
{
    sync_opts(s);
    if (s->keepalive && s->tcb->state == ESTABLISHED && !s->tcb->keep_at) set_timer(&s->tcb->keep_at, net_now() + KEEP_IDLE_MS);
    tcp_output(s->tcb);
}

int tcp_bind_check(struct isock *s, uint32_t addr, uint16_t port)
{
    if (tcp_port_used(addr, port, s, s->reuseaddr)) return -EADDRINUSE;
    s->tcb->laddr = addr;
    s->tcb->lport = port;
    return 0;
}

int tcp_listen(struct isock *s, int backlog)
{
    struct tcpcb *t = s->tcb;
    if (t->state != CLOSED && t->state != LISTEN) return -EINVAL;
    if (!s->lport) {
        s->lport = port_ephemeral(IPPROTO_TCP);
        if (!s->lport) return -EADDRINUSE;
        t->lport = s->lport;
    }
    if (backlog <= 0) backlog = 1;
    if (backlog > MAX_BACKLOG) backlog = MAX_BACKLOG;
    sync_opts(s);
    t->backlog = backlog + 1;                           /* Linux accepts backlog + 1 */
    t->state = LISTEN;
    return 0;
}

int tcp_connect(struct isock *s, struct file *f, uint32_t dst, uint16_t dport)
{
    struct tcpcb *t = s->tcb;
    uint64_t fl = irq_save();
    if (t->state == SYN_SENT) { irq_restore(fl); return -EALREADY; }
    if (t->state == LISTEN) { irq_restore(fl); return -EINVAL; }
    if (t->state != CLOSED) { irq_restore(fl); return -EISCONN; }
    if (t->ever_connected) { irq_restore(fl); return -EISCONN; }
    if (t->err) {                                       /* a failed non-blocking connect, reported once */
        int e = t->err; t->err = 0;
        irq_restore(fl);
        return -e;
    }
    if (!dport) { irq_restore(fl); return -ECONNREFUSED; }
    if (dst == 0) dst = IP4(127, 0, 0, 1);
    uint32_t src = s->laddr ? s->laddr : ip_source_for(dst);
    if (!src) { irq_restore(fl); return -ENETUNREACH; }
    if (!s->lport) {
        s->lport = port_ephemeral(IPPROTO_TCP);
        if (!s->lport) { irq_restore(fl); return -EADDRNOTAVAIL; }
    }
    for (struct tcpcb *o = tcbs; o; o = o->next)          /* the same 4-tuple must not exist */
        if (o != t && o->state != CLOSED && o->state != LISTEN && o->lport == s->lport && o->raddr == dst &&
            o->rport == dport && o->laddr == src) { irq_restore(fl); return -EADDRNOTAVAIL; }
    sync_opts(s);
    t->laddr = src; t->lport = s->lport;
    t->raddr = dst; t->rport = dport;
    s->raddr = dst; s->rport = dport;
    t->mss = (uint16_t)(netif_mtu_for(dst) - 40);
    t->iss = isn();
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1;
    t->rcv_nxt = 0;
    t->state = SYN_SENT;
    t->rtt_start = net_now();
    t->rtt_seq = t->iss + 1;
    st_active++;
    send_segment(t, t->iss, 0, TH_SYN);
    arm_rto(t);
    if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EINPROGRESS; }
    uint64_t deadline = sock_deadline(s->sndtimeo);
    while (t->state == SYN_SENT || t->state == SYN_RCVD) {
        int rc = sock_wait(t, deadline);
        if (rc == -EAGAIN) { irq_restore(fl); return -EINPROGRESS; }
        if (rc) { irq_restore(fl); return rc == -ERESTARTSYS ? -EINTR : rc; }   /* the attempt goes on */
    }
    int rc = 0;
    if (t->state == CLOSED) { rc = t->err ? -t->err : -ECONNREFUSED; t->err = 0; }
    irq_restore(fl);
    return rc;
}

int tcp_accept(struct isock *s, struct file *f, struct isock **out)
{
    struct tcpcb *l = s->tcb;
    uint64_t fl = irq_save();
    if (l->state != LISTEN) { irq_restore(fl); return -EINVAL; }
    uint64_t deadline = sock_deadline(s->rcvtimeo);
    while (!l->aq) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        int rc = sock_wait(l, deadline);
        if (rc) { irq_restore(fl); return rc; }
        if (l->state != LISTEN) { irq_restore(fl); return -EINVAL; }
    }
    struct tcpcb *t = l->aq;
    struct isock *ns = isock_new(SOCK_STREAM, IPPROTO_TCP);
    if (!ns) { irq_restore(fl); return -ENOMEM; }
    l->aq = t->aq_next;
    if (!l->aq) l->aq_tail = NULL;
    l->aq_len--;
    t->aq_next = NULL;
    t->listener = NULL;
    t->sock = ns;
    ns->tcb = t;
    ns->laddr = t->laddr; ns->lport = t->lport;
    ns->raddr = t->raddr; ns->rport = t->rport;
    ns->nodelay = t->nodelay; ns->keepalive = t->keepalive;
    if (t->keepalive) set_timer(&t->keep_at, net_now() + KEEP_IDLE_MS);
    *out = ns;
    irq_restore(fl);
    return 0;
}

long tcp_send(struct isock *s, struct file *f, const void *buf, size_t len, int flags)
{
    struct tcpcb *t = s->tcb;
    uint64_t fl = irq_save();
    uint64_t deadline = sock_deadline(s->sndtimeo);
    size_t done = 0;
    for (;;) {
        if (t->err && t->state == CLOSED) { int e = t->err; t->err = 0; irq_restore(fl); return done ? (long)done : -e; }
        if (t->state == SYN_SENT || t->state == SYN_RCVD) {
            if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) { irq_restore(fl); return -EAGAIN; }
            int rc = sock_wait(t, deadline);
            if (rc) { irq_restore(fl); return rc; }
            continue;
        }
        if (s->shut_wr || t->fin_queued || (t->state != ESTABLISHED && t->state != CLOSE_WAIT)) {
            irq_restore(fl);
            if (done) return (long)done;
            return t->ever_connected ? -EPIPE : -ENOTCONN;
        }
        uint32_t room = t->sbuf_size - t->sbuf_len;
        if (room) {
            uint32_t n = len - done < room ? (uint32_t)(len - done) : room;
            uint32_t tail = (t->sbuf_head + t->sbuf_len) % t->sbuf_size;
            uint32_t first = t->sbuf_size - tail < n ? t->sbuf_size - tail : n;
            memcpy(t->sbuf + tail, (const uint8_t *)buf + done, first);
            memcpy(t->sbuf, (const uint8_t *)buf + done + first, n - first);
            t->sbuf_len += n;
            done += n;
            if (!(flags & MSG_MORE)) tcp_output(t);
        }
        if (done == len) break;
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) break;
        int rc = sock_wait(t, deadline);
        if (rc) { irq_restore(fl); return done ? (long)done : rc; }
    }
    irq_restore(fl);
    return done ? (long)done : -EAGAIN;
}

/* After the reader made room: announce a window that grew enough to matter. */
static void window_update(struct tcpcb *t)
{
    if (t->state < ESTABLISHED || t->state == CLOSED || t->state == TIME_WAIT) return;
    uint32_t right = t->rcv_nxt + rwnd_free(t);
    uint32_t grow = SEQ_GT(right, t->rcv_adv) ? right - t->rcv_adv : 0;
    if (grow >= 2u * t->mss || grow >= t->rbuf_size / 2) send_ack(t);
}

long tcp_recv(struct isock *s, struct file *f, void *buf, size_t len, int flags)
{
    struct tcpcb *t = s->tcb;
    uint64_t fl = irq_save();
    uint64_t deadline = sock_deadline(s->rcvtimeo);
    size_t done = 0;
    for (;;) {
        if (t->rbuf_len) {
            uint32_t n = len - done < t->rbuf_len ? (uint32_t)(len - done) : t->rbuf_len;
            uint32_t first = t->rbuf_size - t->rbuf_head < n ? t->rbuf_size - t->rbuf_head : n;
            if (buf) {
                memcpy((uint8_t *)buf + done, t->rbuf + t->rbuf_head, first);
                memcpy((uint8_t *)buf + done + first, t->rbuf, n - first);
            }
            done += n;
            if (flags & MSG_PEEK) break;
            t->rbuf_head = (t->rbuf_head + n) % t->rbuf_size;
            t->rbuf_len -= n;
            window_update(t);
            if (done == len || !(flags & MSG_WAITALL)) break;
            continue;
        }
        if (done && !(flags & MSG_WAITALL)) break;
        if (t->fin_rcvd || s->shut_rd) break;           /* EOF */
        if (t->state == CLOSED) {
            if (t->err) { int e = t->err; t->err = 0; irq_restore(fl); return done ? (long)done : -e; }
            break;
        }
        if (t->state == LISTEN || (t->state == CLOSED && !t->ever_connected)) { irq_restore(fl); return -ENOTCONN; }
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) { irq_restore(fl); return done ? (long)done : -EAGAIN; }
        int rc = sock_wait(t, deadline);
        if (rc) { irq_restore(fl); return done ? (long)done : rc; }
    }
    irq_restore(fl);
    return (long)done;
}

static void queue_fin(struct tcpcb *t)
{
    if (t->fin_queued || t->fin_sent) return;
    if (t->state == ESTABLISHED) t->state = FIN_WAIT1;
    else if (t->state == CLOSE_WAIT) t->state = LAST_ACK;
    else if (t->state == SYN_RCVD) t->state = FIN_WAIT1;
    else return;
    t->fin_queued = true;
    tcp_output(t);
}

int tcp_shutdown(struct isock *s, int how)
{
    struct tcpcb *t = s->tcb;
    uint64_t fl = irq_save();
    if (t->state == LISTEN || t->state == CLOSED || t->state == SYN_SENT) {
        bool listening = t->state == LISTEN;
        irq_restore(fl);
        if (listening && how != SHUT_WR) { s->shut_rd = true; wakeup(t); return 0; }
        return -ENOTCONN;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) { s->shut_rd = true; t->rbuf_len = 0; }
    if (how == SHUT_WR || how == SHUT_RDWR) { s->shut_wr = true; queue_fin(t); }
    wake(t);
    irq_restore(fl);
    return 0;
}

void tcp_close(struct isock *s)
{
    struct tcpcb *t = s->tcb;
    if (!t) return;
    uint64_t f = irq_save();
    t->sock = NULL;
    s->tcb = NULL;
    switch (t->state) {
    case LISTEN: {
        for (struct tcpcb *c = tcbs; c;) {                /* unaccepted and half-open children */
            if (c->listener != t) { c = c->next; continue; }
            c->listener = NULL;
            abort_conn(c, 0);                           /* frees c: start over */
            c = tcbs;
        }
        t->aq = t->aq_tail = NULL;
        enter_closed(t, 0);
        break;
    }
    case CLOSED: case SYN_SENT:
        enter_closed(t, 0);
        break;
    default:
        if (t->rbuf_len || s->linger == 0) {           /* unread data, or SO_LINGER 0: abort */
            abort_conn(t, 0);
            break;
        }
        queue_fin(t);
        if (t->state == FIN_WAIT2) set_timer(&t->tw_at, net_now() + FIN_WAIT2_MS);
        break;
    }
    irq_restore(f);
}

int tcp_poll(struct isock *s)
{
    struct tcpcb *t = s->tcb;
    int r = 0;
    if (!t) return POLLNVAL;
    if (t->state == LISTEN) return t->aq ? POLLIN | POLLRDNORM : 0;
    if (t->err) r |= POLLERR;
    if (t->state == CLOSED) {
        if (t->ever_connected || t->err) r |= POLLIN | POLLRDNORM | POLLOUT | POLLHUP | POLLRDHUP;
        else r |= POLLOUT | POLLHUP;                    /* never connected: like Linux */
        if (t->rbuf_len) r |= POLLIN;
        return r;
    }
    if (t->state == SYN_SENT || t->state == SYN_RCVD) return r;
    if (t->rbuf_len || t->fin_rcvd || s->shut_rd) r |= POLLIN | POLLRDNORM;
    if (t->fin_rcvd) r |= POLLRDHUP;
    if (t->fin_rcvd && (s->shut_wr || t->fin_queued || t->fin_sent)) r |= POLLHUP;
    if ((t->state == ESTABLISHED || t->state == CLOSE_WAIT) && !s->shut_wr &&
        t->sbuf_size - t->sbuf_len >= t->sbuf_size / 3) r |= POLLOUT | POLLWRNORM;
    return r;
}

uint32_t tcp_local_addr(struct isock *s) { return s->tcb->laddr; }
int tcp_readable(struct isock *s) { return s->tcb ? (int)s->tcb->rbuf_len : 0; }
int tcp_info_state(struct isock *s) { return s->tcb ? linux_state[s->tcb->state] : 7; }

int tcp_sock_error(struct isock *s, bool clear)
{
    struct tcpcb *t = s->tcb;
    if (!t || !t->err) return 0;
    int e = t->err;
    if (clear) t->err = 0;
    return e;
}

const char *tcp_state_name(struct isock *s) { return s->tcb ? state_names[s->tcb->state] : "CLOSED"; }

/* /proc/net/tcp, Linux layout */
size_t tcp_proc(char *buf, size_t cap)
{
    size_t n = (size_t)snprintf(buf, cap, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int i = 0;
    for (struct tcpcb *t = tcbs; t && n < cap; t = t->next) {
        if (t->state == CLOSED) continue;
        n += (size_t)snprintf(buf + n, cap - n, "%4d: %08X:%04X %08X:%04X %02X %08X:%08X 00:00000000 %08X     0        0 0\n",
                              i++, t->laddr, be16(t->lport), t->raddr, be16(t->rport), linux_state[t->state],
                              t->sbuf_len, t->state == LISTEN ? (unsigned)t->aq_len : t->rbuf_len, (unsigned)t->retries);
    }
    return n < cap ? n : cap;
}
