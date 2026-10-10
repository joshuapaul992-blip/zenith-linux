/* mm/vmobj.c -- page-backed memory objects (see vmobj.h)
 *
 * Mutations of the page array run with interrupts off (one CPU at a time,
 * page faults and system calls of the holders); the bulk copies of read and
 * write happen outside, on pages the object keeps a reference to. */
#include <kernel/vmobj.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/klog.h>

struct vm_object *vmobj_new(uint64_t size)
{
    if (size > VMOBJ_MAX_SIZE) return NULL;
    struct vm_object *o = kzalloc(sizeof *o);
    if (!o) return NULL;
    o->refs = 1;
    o->size = size;
    return o;
}

void vmobj_get(struct vm_object *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL); }

void vmobj_put(struct vm_object *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (uint64_t i = 0; i < o->npages; i++) if (o->pages[i]) page_unref(o->pages[i]);
    kfree(o->pages);
    kfree(o);
}

/* Make room for page index idx (capacity grows by doubling). */
static bool reserve(struct vm_object *o, uint64_t idx)
{
    if (idx < o->npages) return true;
    if (idx >= VMOBJ_MAX_SIZE / PAGE_SIZE) return false;
    uint64_t cap = o->npages ? o->npages : 16;
    while (cap <= idx) cap *= 2;
    uint64_t *np = kzalloc(cap * sizeof *np);
    if (!np) return false;
    uint64_t f = irq_save();
    if (o->npages) memcpy(np, o->pages, o->npages * sizeof *np);
    uint64_t *old = o->pages;
    o->pages = np;
    o->npages = cap;
    irq_restore(f);
    kfree(old);
    return true;
}

uint64_t vmobj_page(struct vm_object *o, uint64_t idx, bool alloc)
{
    if (idx < o->npages && o->pages[idx]) return o->pages[idx];
    if (!alloc) return 0;
    if (!reserve(o, idx)) return 0;
    uint64_t p = page_alloc();                          /* zeroed, our reference */
    if (!p) return 0;
    uint64_t f = irq_save();
    if (o->pages[idx]) { irq_restore(f); page_unref(p); return o->pages[idx]; }   /* raced */
    o->pages[idx] = p;
    irq_restore(f);
    return p;
}

ssize_t vmobj_read(struct vm_object *o, void *buf, size_t len, uint64_t off)
{
    if (off >= o->size) return 0;
    if (len > o->size - off) len = (size_t)(o->size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? (size_t)(PAGE_SIZE - in) : len - done;
        uint64_t p = vmobj_page(o, idx, false);
        if (p) memcpy((uint8_t *)buf + done, (const uint8_t *)p + in, n);   /* identity mapped */
        else memset((uint8_t *)buf + done, 0, n);       /* a hole reads as zeros */
        done += n;
    }
    return (ssize_t)len;
}

ssize_t vmobj_write(struct vm_object *o, const void *buf, size_t len, uint64_t off)
{
    if (off > VMOBJ_MAX_SIZE || len > VMOBJ_MAX_SIZE - off) return -EFBIG;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? (size_t)(PAGE_SIZE - in) : len - done;
        uint64_t p = vmobj_page(o, idx, true);
        if (!p) { if (!done) return -ENOSPC; break; }
        memcpy((uint8_t *)p + in, (const uint8_t *)buf + done, n);
        done += n;
    }
    if (off + done > o->size) o->size = off + done;
    return (ssize_t)done;
}

int vmobj_truncate(struct vm_object *o, uint64_t size)
{
    if (size > VMOBJ_MAX_SIZE) return -EFBIG;
    if (size < o->size) {
        /* drop whole pages past the end; zero the tail of the last one, so a
         * later extension reads zeros there (POSIX) */
        uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t f = irq_save();
        for (uint64_t i = keep; i < o->npages; i++)
            if (o->pages[i]) { uint64_t p = o->pages[i]; o->pages[i] = 0; irq_restore(f); page_unref(p); f = irq_save(); }
        irq_restore(f);
        if (size % PAGE_SIZE) {
            uint64_t p = vmobj_page(o, size / PAGE_SIZE, false);
            if (p) memset((uint8_t *)p + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        }
    }
    o->size = size;
    return 0;
}

uint64_t vmobj_resident(const struct vm_object *o)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < o->npages; i++) if (o->pages[i]) n++;
    return n;
}
