/* net/sock.h -- AF_INET sockets, shared by net/socket.c, net/udp.c, net/tcp.c */
#ifndef KESTREL_NET_SOCK_H
#define KESTREL_NET_SOCK_H

#include <kernel/net.h>
#include <kernel/vfs.h>
#include <kernel/inet.h>

struct tcpcb;

struct isock {
    struct vnode vn;                    /* vn.data == this */
    int      type;                      /* SOCK_STREAM, SOCK_DGRAM, SOCK_RAW */
    int      proto;                     /* IPPROTO_TCP, _UDP, _ICMP */
    uint32_t laddr, raddr;              /* network order; laddr 0 = any */
    uint16_t lport, rport;              /* network order; lport 0 = unbound */
    bool     connected;                 /* datagram: a default destination */
    bool     shut_rd, shut_wr;
    int      err;                       /* pending error (SO_ERROR), positive errno */
    /* options */
    bool     reuseaddr, reuseport, broadcast, keepalive, nodelay, hdrincl;
    uint8_t  ttl, tos;
    uint32_t rcvtimeo, sndtimeo;        /* ms, 0 = forever */
    uint32_t rcvbuf, sndbuf;
    int      linger;                    /* -1: off; else seconds */
    /* datagram sockets: received datagrams (pkt->saddr/sport), oldest first */
    struct pkt *rq, *rq_tail;
    size_t   rq_bytes;
    /* stream sockets */
    struct tcpcb *tcb;
    struct isock *next;                 /* all datagram sockets (udp.c) */
};

static inline struct isock *isk(struct file *f) { return f->vn->data; }

/* blocking: wait on chan until woken, a signal (-ERESTARTSYS) or deadline (-EAGAIN).
 * Interrupts must be off. */
int  sock_wait(void *chan, uint64_t deadline);
uint64_t sock_deadline(uint32_t timeo_ms);       /* 0 = none */

/* port allocation (net/socket.c): an unused port for proto, network order */
uint16_t port_ephemeral(int proto);
bool     port_in_use(int proto, uint32_t addr, uint16_t port, struct isock *except, bool reuse);

/* datagrams (net/udp.c) */
void dgram_register(struct isock *s);
void dgram_unregister(struct isock *s);
long udp_send(struct isock *s, const void *buf, size_t len, uint32_t dst, uint16_t dport);
long icmp_sock_send(struct isock *s, const void *buf, size_t len, uint32_t dst);
long dgram_recv(struct isock *s, struct file *f, void *buf, size_t len, int flags,
                struct sockaddr_in *from, int *msg_flags);
void dgram_flush(struct isock *s);
bool dgram_port_used(int proto, uint32_t addr, uint16_t port, struct isock *except, bool reuse);

/* streams (net/tcp.c) */
int  tcp_attach(struct isock *s);               /* new socket: allocate its tcb */
void tcp_close(struct isock *s);                /* the file is closing */
int  tcp_bind_check(struct isock *s, uint32_t addr, uint16_t port);
int  tcp_listen(struct isock *s, int backlog);
int  tcp_connect(struct isock *s, struct file *f, uint32_t dst, uint16_t dport);
int  tcp_accept(struct isock *s, struct file *f, struct isock **out);
long tcp_send(struct isock *s, struct file *f, const void *buf, size_t len, int flags);
long tcp_recv(struct isock *s, struct file *f, void *buf, size_t len, int flags);
int  tcp_shutdown(struct isock *s, int how);
int  tcp_poll(struct isock *s);
int  tcp_readable(struct isock *s);             /* FIONREAD */
int  tcp_info_state(struct isock *s);
bool tcp_port_used(uint32_t addr, uint16_t port, struct isock *except, bool reuse);
void tcp_setopt_nodelay(struct isock *s);
struct isock *isock_new(int type, int proto);   /* net/socket.c */

#endif
