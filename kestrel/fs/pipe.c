/* fs/pipe.c -- anonymous pipes (pipe(2), pipe2(2))
 *
 * One 16 KiB ring buffer shared by a read end and a write end, each an
 * anonymous vnode outside the file tree. Blocking follows POSIX: readers
 * wait for data while a writer exists (then read 0 = EOF), writers wait for
 * room while a reader exists (none: -EPIPE; there are no signals yet, so no
 * SIGPIPE). O_NONBLOCK on either end turns waiting into -EAGAIN. Waiting
 * uses the scheduler's sleep_on()/wakeup() on the pipe itself. */
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>

#define PIPE_BUF_SIZE   16384

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    size_t   head, count;           /* read position, bytes stored */
    int      readers, writers;
    struct vnode rd, wr;
};

static struct pipe *pipe_of(struct file *f) { return f->vn->data; }

static ssize_t pipe_read(struct file *f, void *dst, size_t len)
{
    struct pipe *p = pipe_of(f);
    if (!len) return 0;
    uint64_t fl = irq_save();
    while (!p->count) {
        if (!p->writers) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        sleep_on(p);
    }
    size_t n = len < p->count ? len : p->count;
    for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = p->buf[(p->head + i) % PIPE_BUF_SIZE];
    p->head = (p->head + n) % PIPE_BUF_SIZE;
    p->count -= n;
    wakeup(p);
    irq_restore(fl);
    return (ssize_t)n;
}

static ssize_t pipe_write(struct file *f, const void *src, size_t len)
{
    struct pipe *p = pipe_of(f);
    size_t done = 0;
    uint64_t fl = irq_save();
    while (done < len) {
        if (!p->readers) { irq_restore(fl); return done ? (ssize_t)done : -EPIPE; }
        if (p->count == PIPE_BUF_SIZE) {
            if (f->flags & O_NONBLOCK) break;
            sleep_on(p);
            continue;
        }
        size_t room = PIPE_BUF_SIZE - p->count, n = len - done < room ? len - done : room;
        for (size_t i = 0; i < n; i++)
            p->buf[(p->head + p->count + i) % PIPE_BUF_SIZE] = ((const uint8_t *)src)[done + i];
        p->count += n;
        done += n;
        wakeup(p);
    }
    irq_restore(fl);
    return done ? (ssize_t)done : -EAGAIN;
}

static int pipe_poll(struct file *f, int events)
{
    struct pipe *p = pipe_of(f);
    int r = 0;
    if (f->vn == &p->rd) {
        if (p->count) r |= POLLIN;
        if (!p->writers) r |= POLLHUP;
    } else {
        if (p->count < PIPE_BUF_SIZE) r |= POLLOUT;
        if (!p->readers) r |= POLLERR;
    }
    return r & (events | POLLERR | POLLHUP);
}

static void pipe_release(struct file *f)
{
    struct pipe *p = pipe_of(f);
    uint64_t fl = irq_save();
    if (f->vn == &p->rd) p->readers--; else p->writers--;
    bool gone = !p->readers && !p->writers;
    wakeup(p);
    irq_restore(fl);
    if (gone) kfree(p);
}

static const struct vnode_ops pipe_ops = {
    .fread = pipe_read, .fwrite = pipe_write, .poll = pipe_poll, .release = pipe_release,
};

int pipe_create(struct file **rd, struct file **wr, int flags)
{
    struct pipe *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    struct vnode *v[2] = { &p->rd, &p->wr };
    for (int i = 0; i < 2; i++) {
        strlcpy(v[i]->name, i ? "pipe:[w]" : "pipe:[r]", sizeof v[i]->name);
        v[i]->type = VCHR;
        v[i]->mode = 0010600;                       /* S_IFIFO | rw------- */
        v[i]->ops = &pipe_ops;
        v[i]->data = p;
    }
    *rd = vfs_file_new(&p->rd, O_RDONLY | (flags & O_NONBLOCK));
    *wr = vfs_file_new(&p->wr, O_WRONLY | (flags & O_NONBLOCK));
    if (!*rd || !*wr) { kfree(*rd); kfree(*wr); kfree(p); return -ENOMEM; }
    p->readers = p->writers = 1;
    return 0;
}
