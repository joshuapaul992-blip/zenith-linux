/* linux/slab.h -- Kestrel's Linux KPI: kmalloc and object caches (linuxkpi/src/slab.c)
 *
 * Allocations come from Kestrel's heap (physically contiguous, identity
 * mapped, usable from interrupt handlers) with a small header in front,
 * so that, as in Linux, a power-of-two size is aligned to itself (up to a
 * page) and ksize() works. kfree() checks the header: freeing memory that
 * did not come from here is reported, not followed. */
#ifndef _LKPI_LINUX_SLAB_H
#define _LKPI_LINUX_SLAB_H

#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/overflow.h>
#include <linux/compiler.h>

#define ZERO_SIZE_PTR           ((void *)16)
#define ZERO_OR_NULL_PTR(x)     ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)
#define ARCH_KMALLOC_MINALIGN   16
#define ARCH_DMA_MINALIGN       64
#define KMALLOC_MAX_SIZE        (1UL << 30)

void *lkpi_kmalloc(size_t size, gfp_t gfp);
void *lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp);
void  lkpi_kfree(const void *p);
size_t ksize(const void *p);

static inline __malloc void *kmalloc(size_t size, gfp_t gfp) { return lkpi_kmalloc(size, gfp); }
static inline __malloc void *kzalloc(size_t size, gfp_t gfp) { return lkpi_kmalloc(size, gfp | __GFP_ZERO); }
static inline void kfree(const void *p) { lkpi_kfree(p); }
static inline void kfree_sensitive(const void *p) { lkpi_kfree(p); }
static inline void *krealloc(const void *p, size_t n, gfp_t gfp) { return lkpi_krealloc(p, n, gfp); }
static inline __malloc void *kmalloc_array(size_t n, size_t size, gfp_t gfp)
{ size_t b; return __builtin_mul_overflow(n, size, &b) ? NULL : kmalloc(b, gfp); }
static inline __malloc void *kcalloc(size_t n, size_t size, gfp_t gfp) { return kmalloc_array(n, size, gfp | __GFP_ZERO); }
static inline void *krealloc_array(void *p, size_t n, size_t size, gfp_t gfp)
{ size_t b; return __builtin_mul_overflow(n, size, &b) ? NULL : krealloc(p, b, gfp); }
#define kmalloc_node(s, gfp, node)      kmalloc(s, gfp)
#define kzalloc_node(s, gfp, node)      kzalloc(s, gfp)
#define kcalloc_node(n, s, gfp, node)   kcalloc(n, s, gfp)
#define kmalloc_array_node(n, s, gfp, node) kmalloc_array(n, s, gfp)
#define kmalloc_track_caller(s, gfp)    kmalloc(s, gfp)
#define kmalloc_size_roundup(s)         (s)

/* kvmalloc: kmalloc or vmalloc, all one here */
#define kvmalloc(s, gfp)                kmalloc(s, gfp)
#define kvzalloc(s, gfp)                kzalloc(s, gfp)
#define kvmalloc_node(s, gfp, node)     kmalloc(s, gfp)
#define kvzalloc_node(s, gfp, node)     kzalloc(s, gfp)
#define kvmalloc_array(n, s, gfp)       kmalloc_array(n, s, gfp)
#define kvcalloc(n, s, gfp)             kcalloc(n, s, gfp)
#define kvrealloc(p, n, gfp)            krealloc(p, n, gfp)
static inline void kvfree(const void *p) { lkpi_kfree(p); }
#define kvfree_sensitive(p, len)        kvfree(p)

/* ---- object caches ---- */
#define SLAB_HWCACHE_ALIGN      ((slab_flags_t)0x00002000U)
#define SLAB_CACHE_DMA          ((slab_flags_t)0x00004000U)
#define SLAB_CACHE_DMA32        ((slab_flags_t)0x00008000U)
#define SLAB_PANIC              ((slab_flags_t)0x00040000U)
#define SLAB_TYPESAFE_BY_RCU    ((slab_flags_t)0x00080000U)
#define SLAB_RECLAIM_ACCOUNT    ((slab_flags_t)0x00020000U)
#define SLAB_ACCOUNT            ((slab_flags_t)0x04000000U)
#define SLAB_POISON             ((slab_flags_t)0x00000800U)

struct kmem_cache;
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
                                     slab_flags_t flags, void (*ctor)(void *));
struct kmem_cache *kmem_cache_create_usercopy(const char *name, unsigned int size, unsigned int align,
                                              slab_flags_t flags, unsigned int useroffset, unsigned int usersize,
                                              void (*ctor)(void *));
void  kmem_cache_destroy(struct kmem_cache *s);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfp);
void  kmem_cache_free(struct kmem_cache *s, void *obj);
static inline void *kmem_cache_zalloc(struct kmem_cache *s, gfp_t gfp) { return kmem_cache_alloc(s, gfp | __GFP_ZERO); }
#define kmem_cache_alloc_node(s, gfp, node) kmem_cache_alloc(s, gfp)
#define kmem_cache_shrink(s)            0
#define KMEM_CACHE(__struct, __flags) \
    kmem_cache_create(#__struct, sizeof(struct __struct), __alignof__(struct __struct), (__flags), NULL)

#endif
