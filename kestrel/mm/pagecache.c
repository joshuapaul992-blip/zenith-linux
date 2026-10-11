/* mm/pagecache.c -- file pages kept in memory, shared by everyone (see pagecache.h) */
#include <kernel/pagecache.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

struct pcache {
    uint64_t *pages;            /* physical addresses, 0 = not cached     */
    uint64_t  npages;           /* capacity of pages[]                    */
    uint64_t  count;            /* pages cached                           */
    uint64_t  used;             /* LRU stamp: the last lookup             */
    uint64_t  scanned;          /* the last pcache_shrink pass that looked */
    struct pcache *prev, *next; /* every cache                            */
};

static struct spinlock lock;
static struct pcache *all;
static uint64_t stamp, files, total, hits, misses, ahead, evicted, passes;

struct pcache *pcache_new(void)
{
    struct pcache *pc = kzalloc(sizeof *pc);
    if (!pc) return NULL;
    uint64_t f = spin_lock_irqsave(&lock);
    pc->next = all;
    if (all) all->prev = pc;
    all = pc;
    files++;
    spin_unlock_irqrestore(&lock, f);
    return pc;
}

void pcache_free(struct pcache *pc)
{
    if (!pc) return;
    uint64_t f = spin_lock_irqsave(&lock);
    if (pc->prev) pc->prev->next = pc->next; else all = pc->next;
    if (pc->next) pc->next->prev = pc->prev;
    files--;
    total -= pc->count;
    spin_unlock_irqrestore(&lock, f);
    for (uint64_t i = 0; i < pc->npages; i++) if (pc->pages[i]) page_unref(pc->pages[i]);
    kfree(pc->pages);
    kfree(pc);
}

uint64_t pcache_lookup(struct pcache *pc, uint64_t idx, bool count)
{
    uint64_t f = spin_lock_irqsave(&lock);
    uint64_t p = idx < pc->npages ? pc->pages[idx] : 0;
    if (p) page_ref(p);
    pc->used = ++stamp;
    if (count) { if (p) hits++; else misses++; }
    spin_unlock_irqrestore(&lock, f);
    return p;
}

bool pcache_present(struct pcache *pc, uint64_t idx)
{
    uint64_t f = spin_lock_irqsave(&lock);
    bool r = idx < pc->npages && pc->pages[idx];
    spin_unlock_irqrestore(&lock, f);
    return r;
}

/* Low on memory: free cached pages until a sixteenth of memory is free. */
static void balance(void)
{
    uint64_t all_pages = pmm_total_bytes() / PAGE_SIZE, free_pages = pmm_free_bytes() / PAGE_SIZE;
    uint64_t low = all_pages / 32 > 1024 ? all_pages / 32 : 1024;
    if (free_pages < low) pcache_shrink(2 * low - free_pages);
}

uint64_t pcache_insert(struct pcache *pc, uint64_t idx, uint64_t phys, bool ra)
{
    if (idx >= pc->npages) {                            /* grow by doubling, outside the lock */
        uint64_t cap = pc->npages ? pc->npages : 16;
        while (cap <= idx) cap *= 2;
        uint64_t *np = kzalloc(cap * sizeof *np);
        if (!np) return phys;                           /* not cached; still usable */
        uint64_t f = spin_lock_irqsave(&lock);
        uint64_t *old = np;
        if (cap > pc->npages) {
            if (pc->npages) memcpy(np, pc->pages, pc->npages * sizeof *np);
            old = pc->pages;
            pc->pages = np;
            pc->npages = cap;
        }
        spin_unlock_irqrestore(&lock, f);
        kfree(old);
    }
    uint64_t f = spin_lock_irqsave(&lock);
    uint64_t have = pc->pages[idx];
    if (have) {
        page_ref(have);
    } else {
        page_ref(phys);                                 /* the cache's own reference */
        pc->pages[idx] = phys;
        pc->count++;
        total++;
        if (ra) ahead++;
    }
    spin_unlock_irqrestore(&lock, f);
    if (have) { page_unref(phys); return have; }
    balance();
    return phys;
}

void pcache_write(struct pcache *pc, const void *buf, size_t len, uint64_t off)
{
    if (!pc) return;
    for (size_t done = 0; done < len; ) {
        uint64_t pos = off + done, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? (size_t)(PAGE_SIZE - in) : len - done;
        uint64_t p = pcache_lookup(pc, pos / PAGE_SIZE, false);
        if (p) { memcpy((uint8_t *)p + in, (const uint8_t *)buf + done, n); page_unref(p); }
        done += n;
    }
}

void pcache_truncate(struct pcache *pc, uint64_t size)
{
    if (!pc) return;
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&lock);
    for (uint64_t i = keep; i < pc->npages; i++) {
        uint64_t p = pc->pages[i];
        if (!p) continue;
        pc->pages[i] = 0;
        pc->count--;
        total--;
        spin_unlock_irqrestore(&lock, f);
        page_unref(p);                                  /* mappings keep their own reference */
        f = spin_lock_irqsave(&lock);
    }
    spin_unlock_irqrestore(&lock, f);
    if (size % PAGE_SIZE) {                             /* past the end reads as zeros */
        uint64_t p = pcache_lookup(pc, size / PAGE_SIZE, false);
        if (p) { memset((uint8_t *)p + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE); page_unref(p); }
    }
}

uint64_t pcache_shrink(uint64_t want)
{
    uint64_t freed = 0;
    uint64_t f = spin_lock_irqsave(&lock);
    uint64_t pass = ++passes;
    while (freed < want) {
        struct pcache *v = NULL;                        /* least recently used, not yet looked at */
        for (struct pcache *pc = all; pc; pc = pc->next)
            if (pc->count && pc->scanned != pass && (!v || pc->used < v->used)) v = pc;
        if (!v) break;
        v->scanned = pass;
        for (uint64_t i = 0; i < v->npages && freed < want; i++) {
            uint64_t p = v->pages[i];
            if (!p || page_refs(p) != 1) continue;      /* mapped or being read: stays */
            v->pages[i] = 0;
            v->count--;
            total--;
            evicted++;
            freed++;
            page_unref(p);
        }
    }
    spin_unlock_irqrestore(&lock, f);
    return freed;
}

void pcache_stats(struct pcache_stats *s)
{
    uint64_t f = spin_lock_irqsave(&lock);
    s->files = files;
    s->pages = total;
    s->hits = hits;
    s->misses = misses;
    s->ahead = ahead;
    s->evicted = evicted;
    spin_unlock_irqrestore(&lock, f);
}
