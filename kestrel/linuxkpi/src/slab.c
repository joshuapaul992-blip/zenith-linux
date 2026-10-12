/* linuxkpi/src/slab.c -- kmalloc, object caches and vmalloc
 *
 * Every allocation comes from Kestrel's heap with a header just before
 * the returned pointer: the heap block it lives in, the size asked for,
 * and a magic number so that kfree() can tell a bad pointer from a good
 * one. Alignment follows Linux: 16 bytes at least, a power-of-two size up
 * to a page is aligned to itself, and anything of a page or more is
 * page aligned. GFP_DMA/GFP_DMA32 requests get whole frames below 4 GiB.
 *
 * Memory is identity mapped, so vmalloc() is a page-aligned kmalloc
 * (physically contiguous as well). */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/log2.h>
#include <linux/printk.h>
#include <linux/atomic.h>
#include "internal.h"

#define KM_MAGIC        0x4c4b504948454144ull      /* "DAEHIPKL" */
#define KM_VMALLOC      1                           /* hdr->flags */

struct km_hdr {
    void           *block;          /* heap block, or the first frame */
    size_t          size;           /* as asked */
    unsigned int    frames;         /* != 0: whole frames from lkpi_frames_alloc */
    unsigned int    flags;
    u64             magic;
};

static atomic_long_t km_live = ATOMIC_LONG_INIT(0);

static size_t km_align(size_t size)
{
    if (size >= PAGE_SIZE) return PAGE_SIZE;
    if (is_power_of_2(size) && size > 16) return size;
    return 16;
}

static void *km_alloc(size_t size, size_t align, gfp_t gfp, unsigned int flags)
{
    struct km_hdr *h;
    void *p;

    if (!size) return ZERO_SIZE_PTR;
    if (size > KMALLOC_MAX_SIZE) return NULL;

    if (gfp & (__GFP_DMA | __GFP_DMA32)) {
        /* the header takes a page of its own in front so the object stays page aligned */
        unsigned long n = (sizeof *h + size + PAGE_SIZE - 1) / PAGE_SIZE + (align > 16 ? 1 : 0);
        unsigned long phys = lkpi_frames_alloc(n, 1);
        if (!phys) goto fail;
        p = (void *)(phys + (align > 16 ? PAGE_SIZE : ALIGN(sizeof *h, 16)));
        h = (struct km_hdr *)p - 1;
        h->block = (void *)phys;
        h->frames = (unsigned int)n;
    } else {
        size_t total = size + sizeof *h + align - 1;
        void *block = lkpi_heap_alloc(total < size ? (size_t)-1 : total);
        if (!block) goto fail;
        p = (void *)ALIGN((unsigned long)block + sizeof *h, align);
        h = (struct km_hdr *)p - 1;
        h->block = block;
        h->frames = 0;
    }
    h->size = size;
    h->flags = flags;
    h->magic = KM_MAGIC;
    if (gfp & __GFP_ZERO) memset(p, 0, size);
    atomic_long_inc(&km_live);
    return p;

fail:
    if (!(gfp & __GFP_NOWARN))
        printk(KERN_WARNING "linuxkpi: out of memory allocating %zu bytes\n", size);
    return NULL;
}

static struct km_hdr *km_hdr(const void *p)
{
    struct km_hdr *h = (struct km_hdr *)p - 1;
    if (((unsigned long)p & 15) || h->magic != KM_MAGIC) {
        printk(KERN_ERR "linuxkpi: kfree of %p, which kmalloc did not return\n", p);
        WARN_ON(1);
        return NULL;
    }
    return h;
}

void *lkpi_kmalloc(size_t size, gfp_t gfp) { return km_alloc(size, km_align(size), gfp, 0); }

void lkpi_kfree(const void *p)
{
    if (ZERO_OR_NULL_PTR(p)) return;
    struct km_hdr *h = km_hdr(p);
    if (!h) return;
    h->magic = 0;
    atomic_long_dec(&km_live);
    if (h->frames) lkpi_frames_free((unsigned long)h->block, h->frames);
    else lkpi_heap_free(h->block);
}

size_t ksize(const void *p)
{
    if (ZERO_OR_NULL_PTR(p)) return 0;
    struct km_hdr *h = km_hdr(p);
    return h ? h->size : 0;
}

void *lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp)
{
    if (!new_size) { lkpi_kfree(p); return ZERO_SIZE_PTR; }
    if (ZERO_OR_NULL_PTR(p)) return lkpi_kmalloc(new_size, gfp);
    struct km_hdr *h = km_hdr(p);
    if (!h) return NULL;
    if (new_size <= h->size) return (void *)p;
    void *n = lkpi_kmalloc(new_size, gfp & ~__GFP_ZERO);
    if (!n) return NULL;
    memcpy(n, p, h->size);
    if (gfp & __GFP_ZERO) memset((char *)n + h->size, 0, new_size - h->size);
    lkpi_kfree(p);
    return n;
}

/* ---- object caches: named, fixed-size kmalloc ---------------------------------- */
struct kmem_cache {
    const char     *name;
    unsigned int    size, align;
    slab_flags_t    flags;
    void          (*ctor)(void *);
    atomic_t        live;
};

struct kmem_cache *kmem_cache_create_usercopy(const char *name, unsigned int size, unsigned int align,
                                              slab_flags_t flags, unsigned int useroffset, unsigned int usersize,
                                              void (*ctor)(void *))
{
    struct kmem_cache *s = kzalloc(sizeof *s, GFP_KERNEL);
    if (!s) {
        if (flags & SLAB_PANIC) panic("kmem_cache_create: %s", name);
        return NULL;
    }
    s->name = name;
    s->size = size ? size : 1;
    if (flags & SLAB_HWCACHE_ALIGN && align < 64) align = 64;
    s->align = align < 16 ? 16 : roundup_pow_of_two(align);
    s->flags = flags;
    s->ctor = ctor;
    atomic_set(&s->live, 0);
    return s;
}

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
                                     slab_flags_t flags, void (*ctor)(void *))
{
    return kmem_cache_create_usercopy(name, size, align, flags, 0, 0, ctor);
}

void kmem_cache_destroy(struct kmem_cache *s)
{
    if (!s) return;
    if (atomic_read(&s->live))
        printk(KERN_ERR "linuxkpi: kmem_cache %s destroyed with %d objects in use\n", s->name, atomic_read(&s->live));
    kfree(s);
}

void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfp)
{
    if (s->flags & (SLAB_CACHE_DMA | SLAB_CACHE_DMA32)) gfp |= __GFP_DMA32;
    void *p = km_alloc(s->size, s->align, gfp, 0);
    if (!p) return NULL;
    atomic_inc(&s->live);
    if (s->ctor) s->ctor(p);            /* every time: objects are not kept constructed */
    return p;
}

void kmem_cache_free(struct kmem_cache *s, void *obj)
{
    if (!obj) return;
    atomic_dec(&s->live);
    lkpi_kfree(obj);
}

/* ---- vmalloc ---------------------------------------------------------------------- */
void *vmalloc(unsigned long size) { return km_alloc(size, PAGE_SIZE, GFP_KERNEL, KM_VMALLOC); }
void *vzalloc(unsigned long size) { return km_alloc(size, PAGE_SIZE, GFP_KERNEL | __GFP_ZERO, KM_VMALLOC); }
void vfree(const void *addr) { lkpi_kfree(addr); }

void *vmalloc_array(size_t n, size_t size)
{
    size_t b;
    return __builtin_mul_overflow(n, size, &b) ? NULL : vmalloc(b);
}

void *vcalloc(size_t n, size_t size)
{
    size_t b;
    return __builtin_mul_overflow(n, size, &b) ? NULL : vzalloc(b);
}

/* only pointers vmalloc returned (page aligned, our header in front) count */
bool lkpi_is_vmalloc_addr(const void *a)
{
    if (!a || ((unsigned long)a & (PAGE_SIZE - 1))) return false;
    const struct km_hdr *h = (const struct km_hdr *)a - 1;
    return h->magic == KM_MAGIC && (h->flags & KM_VMALLOC);
}

struct page *vmalloc_to_page(const void *addr) { return virt_to_page(addr); }
unsigned long vmalloc_to_pfn(const void *addr) { return virt_to_pfn(addr); }

/* nothing to map: pages that happen to be contiguous are already a range */
void *vmap(struct page **pages, unsigned int count, unsigned long flags, unsigned long prot)
{
    for (unsigned int i = 1; i < count; i++) {
        if (page_to_pfn(pages[i]) != page_to_pfn(pages[0]) + i) {
            printk(KERN_ERR "linuxkpi: vmap of scattered pages is not supported\n");
            return NULL;
        }
    }
    return count ? page_address(pages[0]) : NULL;
}

void vunmap(const void *addr) { }
