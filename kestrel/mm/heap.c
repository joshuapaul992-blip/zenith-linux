/* mm/heap.c -- first-fit kernel heap with block splitting and coalescing.
 * The heap starts as one physically contiguous, identity-mapped arena
 * reserved at boot and grows by further arenas taken from the frame
 * allocator when it runs out (at least 1 MiB each). Blocks of all arenas
 * sit on one address-ordered list; two neighbours on the list are merged
 * only when they also touch in memory, so arenas never merge. All
 * operations are IRQ-safe. A failed allocation returns NULL: callers turn
 * that into -ENOMEM, never into a crash. */
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>

#define HEAP_MAGIC 0x4B4845415055ull      /* "KHEAPU" */
#define ALIGN      16

struct block {
    uint64_t magic;
    size_t   size;            /* payload bytes */
    struct block *next, *prev;
    uint64_t free;
    uint64_t pad;             /* keep header a multiple of 16 */
};

static struct block *head;
static size_t arena_size, used_bytes;

#define GROW_MIN    (1u << 20)

static bool adjacent(const struct block *a, const struct block *b)
{
    return (const uint8_t *)(a + 1) + a->size == (const uint8_t *)b;
}

/* Add a new arena of at least `need` payload bytes; interrupts are off. */
static bool heap_grow(size_t need)
{
    size_t bytes = need + 2 * sizeof(struct block);
    if (bytes < GROW_MIN) bytes = GROW_MIN;
    size_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base = pmm_alloc_contig(frames);
    if (!base) return false;
    struct block *nb = (struct block *)base;
    nb->magic = HEAP_MAGIC;
    nb->size = frames * PAGE_SIZE - sizeof(struct block);
    nb->free = 1;
    /* keep the list in address order */
    struct block *prev = NULL, *b = head;
    while (b && b < nb) { prev = b; b = b->next; }
    nb->prev = prev;
    nb->next = b;
    if (b) b->prev = nb;
    if (prev) prev->next = nb; else head = nb;
    arena_size += frames * PAGE_SIZE;
    return true;
}

void heap_init(size_t bytes)
{
    size_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base = pmm_alloc_contig(frames);
    if (!base) panic("heap: cannot reserve %zu KiB", bytes >> 10);
    arena_size = frames * PAGE_SIZE;
    head = (struct block *)base;
    head->magic = HEAP_MAGIC;
    head->size = arena_size - sizeof(struct block);
    head->next = head->prev = NULL;
    head->free = 1;
    kprintf("heap: %zu KiB arena at %p\n", arena_size >> 10, (void *)base);
}

void *kmalloc(size_t n)
{
    if (!n) return NULL;
    if (n > (1ull << 30)) return NULL;
    n = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    uint64_t f = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
    for (struct block *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(struct block) + 64) {      /* split */
            struct block *nb = (struct block *)((uint8_t *)(b + 1) + n);
            nb->magic = HEAP_MAGIC;
            nb->size = b->size - n - sizeof(struct block);
            nb->free = 1;
            nb->next = b->next; nb->prev = b;
            if (b->next) b->next->prev = nb;
            b->next = nb;
            b->size = n;
        }
        b->free = 0;
        used_bytes += b->size + sizeof(struct block);
        irq_restore(f);
        return b + 1;
    }
    if (attempt == 0 && !heap_grow(n)) break;
    }
    irq_restore(f);
    kprintf("heap: out of memory (request %zu bytes)\n", n);
    return NULL;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    struct block *b = (struct block *)p - 1;
    if (b->magic != HEAP_MAGIC || b->free) panic("kfree: bad pointer %p", p);
    uint64_t f = irq_save();
    b->free = 1;
    used_bytes -= b->size + sizeof(struct block);
    if (b->next && b->next->free && adjacent(b, b->next)) { /* merge right */
        b->size += sizeof(struct block) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free && adjacent(b->prev, b)) { /* merge left  */
        b->prev->size += sizeof(struct block) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    irq_restore(f);
}

void *krealloc(void *p, size_t n)
{
    if (!p) return kmalloc(n);
    struct block *b = (struct block *)p - 1;
    if (b->size >= n) return p;
    void *q = kmalloc(n);
    if (q) { memcpy(q, p, b->size); kfree(p); }
    return q;
}

/* Give back the tail of a block that turned out larger than needed. */
void *krealloc_shrink(void *p, size_t n)
{
    if (!p) return NULL;
    struct block *b = (struct block *)p - 1;
    if (b->magic != HEAP_MAGIC || b->free) panic("krealloc_shrink: bad pointer %p", p);
    n = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    uint64_t f = irq_save();
    if (b->size >= n + sizeof(struct block) + 64) {
        struct block *nb = (struct block *)((uint8_t *)(b + 1) + n);
        nb->magic = HEAP_MAGIC;
        nb->size = b->size - n - sizeof(struct block);
        nb->free = 0;
        nb->next = b->next; nb->prev = b;
        if (b->next) b->next->prev = nb;
        b->next = nb;
        used_bytes -= b->size - n;
        b->size = n;
        used_bytes += nb->size + sizeof(struct block);
        irq_restore(f);
        kfree(nb + 1);                                      /* merges with any free neighbour */
        return p;
    }
    irq_restore(f);
    return p;
}

char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

size_t heap_used(void) { return used_bytes; }
size_t heap_size(void) { return arena_size; }
