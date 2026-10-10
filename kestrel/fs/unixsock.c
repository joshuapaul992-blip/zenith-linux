/* fs/unixsock.c -- AF_UNIX stream sockets (see include/kernel/socket.h)
 *
 * Every socket is an anonymous vnode embedded in a struct usock; a file
 * descriptor is a struct file on that vnode. Single CPU: state changes run
 * with interrupts off, and waiting uses sleep_on()/wakeup() on the usock
 * whose state matters (its receive buffer, or its accept queue).
 *
 * Lifetime: a usock lives while its file is open, or while it sits
 * unaccepted in a listener's queue. Closing detaches it from its peer
 * (which then reads EOF and gets EPIPE on write) and frees it. Names are
 * kept in a small table: abstract names directly, path names by the inode
 * of their VSOCK node, so a socket file that was unlinked or replaced can
 * no longer be reached even if its vnode memory is reused. */
#include <kernel/process.h>
#include <kernel/socket.h>
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/klog.h>

#define SOCK_BUF        65536
#define MAX_BACKLOG     64
#define MAX_NAMES       64

enum ustate { US_NEW, US_LISTEN, US_CONNECTED };

struct usock {
    struct vnode vn;                    /* vn.data == this */
    enum ustate state;
    bool     nonblock_unused;
    /* name: bound address (or, for accepted sockets, the listener's) */
    struct sockaddr_un addr;
    uint32_t addrlen;                   /* 0 = unnamed */
    bool     bound;
    /* listener */
    struct usock *queue[MAX_BACKLOG];
    int      qlen, backlog;
    /* connected */
    struct usock *peer;
    bool     peer_gone;                 /* had a peer that closed */
    bool     shut_rd, shut_wr;
    uint8_t *rx;                        /* SOCK_BUF ring: data from the peer */
    size_t   rx_head, rx_count;
    struct ucred cred;                  /* of the process that created it */
};

static struct {
    bool     used, abstract;
    uint64_t ino;                       /* path names: VSOCK node inode */
    char     name[UNIX_PATH_MAX];       /* abstract names (without the leading 0) */
    uint32_t namelen;
    struct usock *s;
} names[MAX_NAMES];

static const struct vnode_ops sock_ops;

static struct usock *us(struct file *f) { return f->vn->data; }
bool usock_is(struct file *f) { return f && f->vn && f->vn->ops == &sock_ops; }

static struct usock *usock_new(void)
{
    struct usock *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->rx = kmalloc(SOCK_BUF);
    if (!s->rx) { kfree(s); return NULL; }
    strlcpy(s->vn.name, "socket", sizeof s->vn.name);
    s->vn.type = VSOCK;
    s->vn.mode = S_IFSOCK | 0777;
    s->vn.ops = &sock_ops;
    s->vn.data = s;
    struct tcb *t = current_task();
    s->cred.pid = t->pid; s->cred.uid = t->uid; s->cred.gid = t->gid;
    return s;
}

static void unname(struct usock *s)
{
    for (int i = 0; i < MAX_NAMES; i++) if (names[i].used && names[i].s == s) names[i].used = false;
}

/* Detach from the peer and free. Interrupts must be off. */
static void usock_destroy(struct usock *s)
{
    unname(s);
    for (int i = 0; i < s->qlen; i++) usock_destroy(s->queue[i]);     /* never-accepted connections */
    s->qlen = 0;
    if (s->peer) {
        s->peer->peer = NULL;
        s->peer->peer_gone = true;
        wakeup(s->peer);
    }
    wakeup(s);
    kfree(s->rx);
    kfree(s);
}

static struct file *file_for(struct usock *s, int flags)
{
    return vfs_file_new(&s->vn, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
}

int usock_create(int domain, int type, int protocol, struct file **out)
{
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if ((type & 0xf) != SOCK_STREAM) return -EPROTONOSUPPORT;   /* DGRAM/SEQPACKET: later */
    if (protocol != 0) return -EPROTONOSUPPORT;
    struct usock *s = usock_new();
    if (!s) return -ENOMEM;
    if (!(*out = file_for(s, type))) { kfree(s->rx); kfree(s); return -ENOMEM; }
    return 0;
}

static void connect_pair(struct usock *a, struct usock *b)
{
    a->peer = b; b->peer = a;
    a->state = b->state = US_CONNECTED;
}

int usock_pair(int type, struct file **fa, struct file **fb)
{
    if ((type & 0xf) != SOCK_STREAM) return -EPROTONOSUPPORT;
    struct usock *a = usock_new(), *b = usock_new();
    if (!a || !b) { if (a) { kfree(a->rx); kfree(a); } if (b) { kfree(b->rx); kfree(b); } return -ENOMEM; }
    connect_pair(a, b);
    *fa = file_for(a, type);
    *fb = file_for(b, type);
    if (!*fa || !*fb) { kfree(*fa); kfree(*fb); a->peer = b->peer = NULL; kfree(a->rx); kfree(a); kfree(b->rx); kfree(b); return -ENOMEM; }
    return 0;
}

/* Validate an address; abstract names keep their exact length. */
static int parse_addr(const struct sockaddr_un *a, uint32_t len, bool *abstract, char *name, uint32_t *namelen)
{
    if (len <= 2 || len > sizeof *a || a->sun_family != AF_UNIX) return -EINVAL;
    uint32_t n = len - 2;
    if (a->sun_path[0] == 0) {
        *abstract = true;
        *namelen = n - 1;
        if (!*namelen) return -EINVAL;
        memcpy(name, a->sun_path + 1, *namelen);
        return 0;
    }
    *abstract = false;
    uint32_t k = 0;
    while (k < n && a->sun_path[k]) k++;
    if (k >= UNIX_PATH_MAX) return -ENAMETOOLONG;
    memcpy(name, a->sun_path, k);
    name[k] = 0;
    *namelen = k;
    return 0;
}

int usock_bind(struct file *f, const struct sockaddr_un *a, uint32_t len)
{
    struct usock *s = us(f);
    bool abstract; char name[UNIX_PATH_MAX]; uint32_t nl;
    int rc = parse_addr(a, len, &abstract, name, &nl);
    if (rc) return rc;
    if (s->bound || s->state != US_NEW) return -EINVAL;
    int slot = -1;
    for (int i = 0; i < MAX_NAMES; i++) {
        if (!names[i].used) { if (slot < 0) slot = i; continue; }
        if (abstract && names[i].abstract && names[i].namelen == nl && !memcmp(names[i].name, name, nl)) return -EADDRINUSE;
    }
    if (slot < 0) return -ENOMEM;
    uint64_t ino = 0;
    if (!abstract) {
        struct vnode *node;
        if ((rc = vfs_mksock(name, 0777, &node)) < 0) return rc;    /* existing file: EADDRINUSE */
        ino = node->ino;
    }
    names[slot].used = true;
    names[slot].abstract = abstract;
    names[slot].ino = ino;
    memcpy(names[slot].name, name, nl);
    names[slot].namelen = nl;
    names[slot].s = s;
    memcpy(&s->addr, a, len);
    s->addrlen = len;
    s->bound = true;
    return 0;
}

int usock_listen(struct file *f, int backlog)
{
    struct usock *s = us(f);
    if (s->state == US_CONNECTED) return -EINVAL;
    if (!s->bound) return -EINVAL;              /* Linux autobinds; X always binds first */
    s->state = US_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > MAX_BACKLOG ? MAX_BACKLOG : backlog;
    return 0;
}

static struct usock *find_listener(bool abstract, const char *name, uint32_t nl)
{
    uint64_t ino = 0;
    if (!abstract) {
        struct vnode *node;
        if (vfs_lookup(name, &node) < 0) return NULL;
        if (node->type != VSOCK) return NULL;
        ino = node->ino;
    }
    for (int i = 0; i < MAX_NAMES; i++) {
        if (!names[i].used || names[i].abstract != abstract) continue;
        if (abstract ? (names[i].namelen == nl && !memcmp(names[i].name, name, nl)) : names[i].ino == ino)
            return names[i].s;
    }
    return NULL;
}

int usock_connect(struct file *f, const struct sockaddr_un *a, uint32_t len)
{
    struct usock *s = us(f);
    bool abstract; char name[UNIX_PATH_MAX]; uint32_t nl;
    int rc = parse_addr(a, len, &abstract, name, &nl);
    if (rc) return rc;
    if (s->state == US_CONNECTED) return -EISCONN;
    if (s->state == US_LISTEN) return -EINVAL;
    if (!abstract) {
        struct vnode *node;
        if ((rc = vfs_lookup(name, &node)) < 0) return rc;
        if (node->type != VSOCK) return -ECONNREFUSED;
    }
    uint64_t fl = irq_save();
    struct usock *l = find_listener(abstract, name, nl);
    if (!l || l->state != US_LISTEN) { irq_restore(fl); return -ECONNREFUSED; }
    if (l->qlen >= l->backlog) { irq_restore(fl); return (f->flags & O_NONBLOCK) ? -EAGAIN : -ECONNREFUSED; }
    struct usock *srv = usock_new();
    if (!srv) { irq_restore(fl); return -ENOMEM; }
    srv->cred = l->cred;                        /* the server process owns it */
    srv->addr = l->addr;
    srv->addrlen = l->addrlen;
    connect_pair(s, srv);
    l->queue[l->qlen++] = srv;
    wakeup(l);
    irq_restore(fl);
    return 0;
}

int usock_accept(struct file *f, int flags, struct file **out, struct sockaddr_un *peer, uint32_t *len)
{
    struct usock *l = us(f);
    if (l->state != US_LISTEN) return -EINVAL;
    uint64_t fl = irq_save();
    while (!l->qlen) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(l);
        if (l->state != US_LISTEN) { irq_restore(fl); return -EINVAL; }
    }
    struct usock *s = l->queue[0];
    memmove(l->queue, l->queue + 1, (size_t)(l->qlen - 1) * sizeof *l->queue);
    l->qlen--;
    irq_restore(fl);
    struct file *nf = file_for(s, flags);
    if (!nf) { fl = irq_save(); usock_destroy(s); irq_restore(fl); return -ENOMEM; }
    if (peer && len) {                          /* the client: normally unnamed */
        struct usock *c = s->peer;
        uint32_t n = c && c->addrlen ? c->addrlen : 2;
        if (n > *len) n = *len;
        struct sockaddr_un tmp = { .sun_family = AF_UNIX };
        if (c && c->addrlen) tmp = c->addr;
        memcpy(peer, &tmp, n);
        *len = c && c->addrlen ? c->addrlen : 2;
    }
    *out = nf;
    return 0;
}

long usock_send(struct file *f, const void *buf, size_t len, int flags)
{
    struct usock *s = us(f);
    bool nb = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    size_t done = 0;
    uint64_t fl = irq_save();
    if (s->state != US_CONNECTED && !s->peer_gone) { irq_restore(fl); return -ENOTCONN; }
    if (s->shut_wr) { irq_restore(fl); return -EPIPE; }
    while (done < len) {
        struct usock *p = s->peer;
        if (!p || p->shut_rd) { irq_restore(fl); return done ? (long)done : -EPIPE; }
        if (p->rx_count == SOCK_BUF) {
            if (nb) break;
            if (signal_pending()) { irq_restore(fl); return done ? (long)done : -ERESTARTSYS; }
            sleep_on(p);
            continue;
        }
        size_t room = SOCK_BUF - p->rx_count, n = len - done < room ? len - done : room;
        size_t tail = (p->rx_head + p->rx_count) % SOCK_BUF;
        size_t first = n < SOCK_BUF - tail ? n : SOCK_BUF - tail;
        memcpy(p->rx + tail, (const uint8_t *)buf + done, first);
        memcpy(p->rx, (const uint8_t *)buf + done + first, n - first);
        p->rx_count += n;
        done += n;
        wakeup(p);
    }
    irq_restore(fl);
    return done ? (long)done : -EAGAIN;
}

long usock_recv(struct file *f, void *buf, size_t len, int flags)
{
    struct usock *s = us(f);
    bool nb = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    if (!len) return 0;
    uint64_t fl = irq_save();
    if (s->state != US_CONNECTED && !s->peer_gone) { irq_restore(fl); return -ENOTCONN; }
    while (!s->rx_count) {
        if (!s->peer || s->shut_rd || (s->peer && s->peer->shut_wr)) { irq_restore(fl); return 0; }    /* EOF */
        if (nb) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(s);
    }
    size_t n = len < s->rx_count ? len : s->rx_count;
    size_t first = n < SOCK_BUF - s->rx_head ? n : SOCK_BUF - s->rx_head;
    memcpy(buf, s->rx + s->rx_head, first);
    memcpy((uint8_t *)buf + first, s->rx, n - first);
    if (!(flags & MSG_PEEK)) {
        s->rx_head = (s->rx_head + n) % SOCK_BUF;
        s->rx_count -= n;
        wakeup(s);                              /* writers waiting for room */
    }
    irq_restore(fl);
    return (long)n;
}

int usock_shutdown(struct file *f, int how)
{
    struct usock *s = us(f);
    if (s->state != US_CONNECTED) return -ENOTCONN;
    uint64_t fl = irq_save();
    if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = true;
    if (how == SHUT_WR || how == SHUT_RDWR) s->shut_wr = true;
    wakeup(s);
    if (s->peer) wakeup(s->peer);
    irq_restore(fl);
    return 0;
}

int usock_name(struct file *f, bool peer, struct sockaddr_un *a, uint32_t *len)
{
    struct usock *s = us(f);
    if (peer && !s->peer) return -ENOTCONN;
    struct usock *o = peer ? s->peer : s;
    struct sockaddr_un tmp = { .sun_family = AF_UNIX };
    uint32_t n = 2;
    if (o->addrlen) { tmp = o->addr; n = o->addrlen; }
    memcpy(a, &tmp, n < *len ? n : *len);
    *len = n;
    return 0;
}

int usock_getsockopt(struct file *f, int level, int opt, void *val, uint32_t *len)
{
    struct usock *s = us(f);
    if (level != SOL_SOCKET) return -EOPNOTSUPP;
    int32_t i;
    switch (opt) {
    case SO_PEERCRED: {
        if (!s->peer) return -ENOTCONN;
        if (*len < sizeof(struct ucred)) return -EINVAL;
        memcpy(val, &s->peer->cred, sizeof(struct ucred));
        *len = sizeof(struct ucred);
        return 0;
    }
    case SO_TYPE:   i = SOCK_STREAM; break;
    case SO_ERROR:  i = 0; break;
    case SO_SNDBUF:
    case SO_RCVBUF: i = SOCK_BUF; break;
    default: return -EOPNOTSUPP;
    }
    if (*len < sizeof i) return -EINVAL;
    memcpy(val, &i, sizeof i);
    *len = sizeof i;
    return 0;
}

/* ---- file operations ---------------------------------------------------------- */
static ssize_t sock_fread(struct file *f, void *buf, size_t len) { return usock_recv(f, buf, len, 0); }
static ssize_t sock_fwrite(struct file *f, const void *buf, size_t len) { return usock_send(f, buf, len, 0); }

static int sock_poll(struct file *f, int events)
{
    struct usock *s = us(f);
    int r = 0;
    if (s->state == US_LISTEN) {
        if (s->qlen) r |= POLLIN;
    } else if (s->state == US_CONNECTED || s->peer_gone) {
        if (s->rx_count) r |= POLLIN;
        if (!s->peer || s->shut_rd) r |= POLLIN | POLLHUP;
        if (s->peer && !s->shut_wr && s->peer->rx_count < SOCK_BUF) r |= POLLOUT;
    } else {
        r |= POLLOUT | POLLHUP;                 /* unconnected: like Linux */
    }
    return r & (events | POLLERR | POLLHUP);
}

#define FIONREAD 0x541B
static int sock_ioctl(struct vnode *vn, unsigned long req, void *arg)
{
    struct usock *s = vn->data;
    if (req == FIONREAD) { *(int *)arg = (int)s->rx_count; return 0; }
    return -ENOTTY;
}

static void sock_release(struct file *f)
{
    uint64_t fl = irq_save();
    usock_destroy(us(f));
    irq_restore(fl);
}

static const struct vnode_ops sock_ops = {
    .fread = sock_fread, .fwrite = sock_fwrite, .poll = sock_poll, .release = sock_release, .ioctl = sock_ioctl,
};
