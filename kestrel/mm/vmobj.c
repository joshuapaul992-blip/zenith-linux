/* mm/vmobj.c -- page-backed memory objects (see vmobj.h)
 *
 * The page array and the size are guarded by the object's spinlock, so
 * reads and writes of a ramfs file or memfd run without the big kernel
 * lock. The bulk copies of read and write happen outside it, on pages they
 * hold a reference to: a truncate on another CPU cannot free them midway. */
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
    uint64_t f = spin_lock_irqsave(&o->lock);
    if (o->npages > idx) {                              /* grown meanwhile */
        spin_unlock_irqrestore(&o->lock, f);
        kfree(np);
        return true;
    }
    if (o->npages) memcpy(np, o->pages, o->npages * sizeof *np);
    uint64_t *old = o->pages;
    o->pages = np;
    o->npages = cap;
    spin_unlock_irqrestore(&o->lock, f);
    kfree(old);
    return true;
}

/* Page idx, with a reference for the caller if `ref`. */
static uint64_t page_at(struct vm_object *o, uint64_t idx, bool alloc, bool ref)
{
    uint64_t f = spin_lock_irqsave(&o->lock);
    uint64_t p = idx < o->npages ? o->pages[idx] : 0;
    if (p && ref) page_ref(p);
    spin_unlock_irqrestore(&o->lock, f);
    if (p || !alloc) return p;
    if (!reserve(o, idx)) return 0;
    uint64_t n = page_alloc();                          /* zeroed, our reference */
    if (!n) return 0;
    f = spin_lock_irqsave(&o->lock);
    p = o->pages[idx];
    if (!p) { o->pages[idx] = p = n; n = 0; }           /* else raced: theirs */
    if (ref) page_ref(p);
    spin_unlock_irqrestore(&o->lock, f);
    if (n) page_unref(n);
    return p;
}

uint64_t vmobj_page(struct vm_object *o, uint64_t idx, bool alloc) { return page_at(o, idx, alloc, false); }

ssize_t vmobj_read(struct vm_object *o, void *buf, size_t len, uint64_t off)
{
    uint64_t size = __atomic_load_n(&o->size, __ATOMIC_ACQUIRE);
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? (size_t)(PAGE_SIZE - in) : len - done;
        uint64_t p = page_at(o, idx, false, true);
        if (p) { memcpy((uint8_t *)buf + done, (const uint8_t *)p + in, n); page_unref(p); }   /* identity mapped */
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
        uint64_t p = page_at(o, idx, true, true);
        if (!p) { if (!done) return -ENOSPC; break; }
        memcpy((uint8_t *)p + in, (const uint8_t *)buf + done, n);
        page_unref(p);
        done += n;
    }
    uint64_t f = spin_lock_irqsave(&o->lock);
    if (off + done > o->size) __atomic_store_n(&o->size, off + done, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&o->lock, f);
    return (ssize_t)done;
}

int vmobj_truncate(struct vm_object *o, uint64_t size)
{
    if (size > VMOBJ_MAX_SIZE) return -EFBIG;
    if (size < o->size) {
        /* drop whole pages past the end; zero the tail of the last one, so a
         * later extension reads zeros there (POSIX) */
        uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t f = spin_lock_irqsave(&o->lock);
        __atomic_store_n(&o->size, size, __ATOMIC_RELEASE);  /* readers stop at the new end first */
        for (uint64_t i = keep; i < o->npages; i++)
            if (o->pages[i]) {
                uint64_t p = o->pages[i];
                o->pages[i] = 0;
                spin_unlock_irqrestore(&o->lock, f);
                page_unref(p);                          /* a reader's own reference keeps it a while */
                f = spin_lock_irqsave(&o->lock);
            }
        spin_unlock_irqrestore(&o->lock, f);
        if (size % PAGE_SIZE) {
            uint64_t p = page_at(o, size / PAGE_SIZE, false, true);
            if (p) { memset((uint8_t *)p + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE); page_unref(p); }
        }
    }
    uint64_t f = spin_lock_irqsave(&o->lock);
    __atomic_store_n(&o->size, size, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&o->lock, f);
    return 0;
}

uint64_t vmobj_resident(const struct vm_object *o)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < o->npages; i++) if (o->pages[i]) n++;
    return n;
}
