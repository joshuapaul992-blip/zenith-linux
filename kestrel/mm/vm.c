/* mm/vm.c -- user address spaces: regions, demand paging, copy-on-write
 * (see include/kernel/vm.h for the model).
 *
 * Locking: each mm has a sleeping, recursive mutex (mm->lock) held while its
 * regions or page tables change, and while a fault is resolved -- a fault
 * may sleep (reading a file, waiting for the OOM killer), so it is not a
 * spinlock. A uaccess copy made while the lock is held can fault again; the
 * mutex is recursive, so that nests. Page frames are reference counted with
 * locked instructions (mm.h). TLB entries of the current address space are
 * invalidated with invlpg (or a CR3 reload for large ranges) right after a
 * page-table entry loses rights or changes frames. */
#include <kernel/vm.h>
#include <kernel/vmobj.h>
#include <kernel/uvm.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include <kernel/process.h>
#include <kernel/uaccess.h>

#define PTE_P        0x001ull
#define PTE_W        0x002ull
#define PTE_U        0x004ull
#define PTE_HUGE     0x080ull
#define PTE_COW      0x200ull           /* software: copy-on-write          */
#define PTE_PROTNONE 0x400ull           /* software: frame kept, no access   */
#define PTE_NX       (1ull << 63)
#define ADDR_MASK    0x000FFFFFFFFFF000ull
#define PAGE         4096ull
#define KERNEL_SLOTS 128                /* PML4[0..127]: shared kernel half  */

#define UVM_MMAP_END (UVM_STACK_TOP - UVM_STACK_SIZE - (1ull << 30))

static bool nx_on;

static uint64_t pgdown(uint64_t a) { return a & ~(PAGE - 1); }
static uint64_t pgup(uint64_t a)   { return (a + PAGE - 1) & ~(PAGE - 1); }
static bool has_frame(uint64_t e)  { return e & (PTE_P | PTE_PROTNONE); }

/* EFER.NXE: the NX bit (63) of page-table entries takes effect. */
void vm_init(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ull << 11));
            nx_on = true;
        }
    }
    kprintf("vm: demand paging, copy-on-write, %s\n",
            nx_on ? "NX enforced (EFER.NXE)" : "no NX on this CPU: data pages stay executable");
}

/* ---- page tables ------------------------------------------------------------ */
static uint64_t *pte_walk(uint64_t pml4, uint64_t va, bool alloc)
{
    uint64_t *t = (uint64_t *)pml4;
    static const int shift[3] = { 39, 30, 21 };
    for (int l = 0; l < 3; l++) {
        uint64_t *e = &t[(va >> shift[l]) & 511];
        if (!(*e & PTE_P)) {
            if (!alloc) return NULL;
            uint64_t n = pmm_alloc();                   /* zeroed table */
            if (!n) return NULL;
            *e = n | PTE_P | PTE_W | PTE_U;             /* rights are decided at the leaf */
        }
        if (*e & PTE_HUGE) return NULL;                 /* never in the user half */
        t = (uint64_t *)(*e & ADDR_MASK);
    }
    return &t[(va >> 12) & 511];
}

/* Visit every existing leaf entry in [start, end), skipping missing tables. */
typedef void (*pte_fn)(uint64_t *pte, uint64_t va, void *ctx);
static void walk_range(uint64_t pml4, uint64_t start, uint64_t end, pte_fn fn, void *ctx)
{
    uint64_t va = start;
    while (va < end) {
        uint64_t *l4 = (uint64_t *)pml4, e4 = l4[(va >> 39) & 511];
        if (!(e4 & PTE_P)) { va = (va + (1ull << 39)) & ~((1ull << 39) - 1); continue; }
        uint64_t *l3 = (uint64_t *)(e4 & ADDR_MASK), e3 = l3[(va >> 30) & 511];
        if (!(e3 & PTE_P)) { va = (va + (1ull << 30)) & ~((1ull << 30) - 1); continue; }
        uint64_t *l2 = (uint64_t *)(e3 & ADDR_MASK), e2 = l2[(va >> 21) & 511];
        if (!(e2 & PTE_P)) { va = (va + (1ull << 21)) & ~((1ull << 21) - 1); continue; }
        uint64_t *l1 = (uint64_t *)(e2 & ADDR_MASK);
        for (uint64_t i = (va >> 12) & 511; i < 512 && va < end; i++, va += PAGE)
            if (l1[i]) fn(&l1[i], va, ctx);
    }
}

static bool mm_active(const struct mm *mm) { return (read_cr3() & ADDR_MASK) == mm->pml4; }

static void tlb_page(struct mm *mm, uint64_t va) { if (mm_active(mm)) invlpg(va); }

static void tlb_range(struct mm *mm, uint64_t start, uint64_t end)
{
    if (!mm_active(mm)) return;                         /* other CR3: nothing cached */
    if (end - start > 64 * PAGE) { write_cr3(read_cr3()); return; }
    for (uint64_t va = start; va < end; va += PAGE) invlpg(va);
}

static uint64_t leaf_flags(const struct vma *v, bool writable)
{
    uint64_t f = PTE_P | PTE_U;
    if (writable) f |= PTE_W;
    if (nx_on && !(v->prot & PROT_EXEC)) f |= PTE_NX;
    return f;
}

/* 4 KiB copy with string moves: both frames are page aligned (cache-line
 * aligned), identity mapped, and rep movsq moves whole lines efficiently. */
static void copy_page(uint64_t dst, uint64_t src)
{
    uint64_t n = PAGE / 8;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
}

/* ---- regions ---------------------------------------------------------------- */
static struct vma *vma_find(struct mm *mm, uint64_t addr)
{
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (addr < v->end) return addr >= v->start ? v : NULL;
    return NULL;
}

static void vma_insert(struct mm *mm, struct vma *nv)
{
    struct vma **pp = &mm->vmas;
    while (*pp && (*pp)->start < nv->start) pp = &(*pp)->next;
    nv->next = *pp;
    *pp = nv;
}

static void vma_free(struct vma *v)
{
    if (v->obj) vmobj_put(v->obj);
    if (v->file) vfs_close(v->file);
    kfree(v);
}

/* Split v at `at` (start < at < end); the new upper part follows v. */
static struct vma *vma_split(struct vma *v, uint64_t at)
{
    struct vma *n = kmalloc(sizeof *n);
    if (!n) return NULL;
    *n = *v;
    n->start = at;
    n->off = v->off + (at - v->start);
    if (n->obj) vmobj_get(n->obj);
    if (n->file) vfs_file_get(n->file);
    v->end = at;
    v->next = n;
    return n;
}

static bool vma_mergeable(const struct vma *a, const struct vma *b)
{
    return a->end == b->start && a->prot == b->prot && a->flags == b->flags &&
           !a->obj && !b->obj && !a->file && !b->file;
}

/* Merge anonymous neighbours (brk growth, malloc, mprotect undo). */
static void vma_merge_all(struct mm *mm)
{
    for (struct vma *v = mm->vmas; v && v->next; ) {
        struct vma *n = v->next;
        if (vma_mergeable(v, n)) { v->end = n->end; v->next = n->next; kfree(n); }
        else v = n;
    }
}

/* Make region boundaries fall at `a` (split the region containing it). */
static int split_at(struct mm *mm, uint64_t a)
{
    struct vma *v = vma_find(mm, a);
    if (v && v->start < a) return vma_split(v, a) ? 0 : -ENOMEM;
    return 0;
}

struct zap { struct mm *mm; uint64_t n; };
static void zap_fn(uint64_t *pte, uint64_t va, void *ctx)
{
    (void)va;
    struct zap *z = ctx;
    uint64_t e = *pte;
    *pte = 0;
    if (has_frame(e)) { page_unref(e & ADDR_MASK); z->mm->rss--; z->n++; }
}

/* Unmap the pages of [start, end) (regions stay). */
static void zap_range(struct mm *mm, uint64_t start, uint64_t end)
{
    struct zap z = { mm, 0 };
    walk_range(mm->pml4, start, end, zap_fn, &z);
    if (z.n) tlb_range(mm, start, end);
}

/* Remove [start, end) completely: pages and regions. */
static int unmap_range(struct mm *mm, uint64_t start, uint64_t end)
{
    if (split_at(mm, start) || split_at(mm, end)) return -ENOMEM;
    zap_range(mm, start, end);
    for (struct vma **pp = &mm->vmas; *pp; ) {
        struct vma *v = *pp;
        if (v->start >= start && v->end <= end) { *pp = v->next; vma_free(v); }
        else pp = &v->next;
    }
    return 0;
}

static bool range_free(struct mm *mm, uint64_t start, uint64_t end)
{
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (v->start < end && v->end > start) return false;
    return true;
}

/* Lowest free gap of len bytes at or above `from`, below `limit`. */
static uint64_t find_gap(struct mm *mm, uint64_t from, uint64_t limit, uint64_t len)
{
    uint64_t cand = from;
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->end <= cand) continue;
        if (cand + len <= v->start) break;
        cand = v->end;
    }
    return cand + len <= limit && cand >= from ? cand : 0;
}

/* ---- address spaces --------------------------------------------------------- */
struct mm *mm_create(void)
{
    struct mm *mm = kzalloc(sizeof *mm);
    if (!mm) return NULL;
    mm->pml4 = uvm_create();
    if (!mm->pml4) { kfree(mm); return NULL; }
    mm->users = 1;
    return mm;
}

void mm_get(struct mm *mm) { __atomic_add_fetch(&mm->users, 1, __ATOMIC_ACQ_REL); }

/* Free the user half: frames (by reference) and page tables. */
static void free_tables(uint64_t *t, int level)
{
    for (int i = (level == 4 ? KERNEL_SLOTS : 0); i < (level == 4 ? 256 : 512); i++) {
        uint64_t e = t[i];
        if (!e) continue;
        if (level == 1) { if (has_frame(e)) page_unref(e & ADDR_MASK); }
        else if (e & PTE_P) { free_tables((uint64_t *)(e & ADDR_MASK), level - 1); pmm_free(e & ADDR_MASK); }
        t[i] = 0;
    }
}

void mm_put(struct mm *mm)
{
    if (!mm || __atomic_sub_fetch(&mm->users, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (mm_active(mm)) panic("mm_put: freeing the loaded address space");
    while (mm->vmas) { struct vma *v = mm->vmas; mm->vmas = v->next; vma_free(v); }
    free_tables((uint64_t *)mm->pml4, 4);
    pmm_free(mm->pml4);
    kfree(mm);
}

struct forkctx { struct mm *dst; const struct vma *v; int err; };
static void fork_fn(uint64_t *pte, uint64_t va, void *ctx)
{
    struct forkctx *c = ctx;
    if (c->err) return;
    uint64_t e = *pte;
    if (!has_frame(e)) return;
    /* private writable pages become copy-on-write in both processes */
    if (!(c->v->flags & VMA_SHARED) && (e & PTE_W)) {
        e = (e & ~PTE_W) | PTE_COW;
        *pte = e;
    }
    uint64_t *d = pte_walk(c->dst->pml4, va, true);
    if (!d) { c->err = -ENOMEM; return; }
    *d = e;
    page_ref(e & ADDR_MASK);
    c->dst->rss++;
}

struct mm *mm_fork(struct mm *src)
{
    struct mm *dst = mm_create();
    if (!dst) return NULL;
    kmutex_lock(&src->lock);
    dst->brk_start = src->brk_start;
    dst->brk = src->brk;
    int err = 0;
    for (struct vma *v = src->vmas; v && !err; v = v->next) {
        struct vma *n = kmalloc(sizeof *n);
        if (!n) { err = -ENOMEM; break; }
        *n = *v;
        n->next = NULL;
        if (n->obj) vmobj_get(n->obj);
        if (n->file) vfs_file_get(n->file);
        vma_insert(dst, n);
        struct forkctx c = { dst, v, 0 };
        walk_range(src->pml4, v->start, v->end, fork_fn, &c);
        err = c.err;
    }
    if (mm_active(src)) write_cr3(read_cr3());          /* parent lost write access to private pages */
    kmutex_unlock(&src->lock);
    if (err) { mm_put(dst); return NULL; }
    return dst;
}

/* ---- faults --------------------------------------------------------------- */
static int fault_missing(struct mm *mm, struct vma *v, uint64_t *pte, uint64_t va)
{
    bool writable = v->prot & PROT_WRITE;
    uint64_t phys;
    if (v->flags & VMA_SHARED) {
        uint64_t off = v->off + (va - v->start);
        if (off >= pgup(v->obj->size)) return -EIO;     /* past the end of the object: SIGBUS */
        phys = vmobj_page(v->obj, off / PAGE, true);
        if (!phys) return -ENOMEM;
        page_ref(phys);
    } else {
        phys = page_alloc();                            /* zeroed */
        if (!phys) return -ENOMEM;
        if (v->file && va < v->file_end) {
            uint64_t off = v->off + (va - v->start);
            uint64_t fsize = v->file->vn && v->file->vn->ops && v->file->vn->ops->size ?
                             v->file->vn->ops->size(v->file->vn) : v->file->vn->size;
            if (off >= pgup(fsize)) { page_unref(phys); return -EIO; }   /* past EOF: SIGBUS */
            uint64_t n = v->file_end - va < PAGE ? v->file_end - va : PAGE;
            ssize_t r = vfs_pread(v->file, (void *)phys, (size_t)n, off);
            if (r < 0) { page_unref(phys); return -EIO; }
        }
    }
    *pte = phys | leaf_flags(v, writable);
    mm->rss++;
    return 0;
}

static int fault_cow(struct mm *mm, struct vma *v, uint64_t *pte, uint64_t va)
{
    uint64_t e = *pte, old = e & ADDR_MASK;
    if ((v->flags & VMA_SHARED) || page_refs(old) == 1) {
        *pte = (e | PTE_W) & ~PTE_COW;                  /* nobody else has it: take it back */
        tlb_page(mm, va);
        return 0;
    }
    uint64_t n = page_alloc();
    if (!n) return -ENOMEM;
    copy_page(n, old);
    *pte = n | leaf_flags(v, true);
    tlb_page(mm, va);                                   /* drop the read-only translation now */
    page_unref(old);
    return 0;
}

static int fault_once(struct mm *mm, uint64_t addr, uint64_t err)
{
    uint64_t va = pgdown(addr);
    struct vma *v = vma_find(mm, addr);
    if (!v) return -EFAULT;
    bool write = err & PF_WRITE, fetch = err & PF_INSN;
    if (v->prot == PROT_NONE || (write && !(v->prot & PROT_WRITE)) || (fetch && !(v->prot & PROT_EXEC)))
        return -EACCES;
    uint64_t *pte = pte_walk(mm->pml4, va, true);
    if (!pte) return -ENOMEM;
    uint64_t e = *pte;
    if (e & PTE_P) {
        if (write && !(e & PTE_W)) return fault_cow(mm, v, pte, va);
        tlb_page(mm, va);                               /* stale translation: retry */
        return 0;
    }
    if (e & PTE_PROTNONE) {                             /* rights came back (mprotect) */
        bool w = (v->prot & PROT_WRITE) && !(e & PTE_COW);
        *pte = (e & ADDR_MASK) | (e & PTE_COW) | leaf_flags(v, w);
        return write && !w ? fault_cow(mm, v, pte, va) : 0;
    }
    return fault_missing(mm, v, pte, va);
}

int vm_fault(uint64_t addr, uint64_t err, bool user_mode)
{
    struct tcb *t = current_task();
    struct mm *mm = t ? t->mm : NULL;
    if (!mm || !t->user || !user_range_ok(addr, 1)) return -EFAULT;
    for (int attempt = 0;; attempt++) {
        kmutex_lock(&mm->lock);
        int r = fault_once(mm, addr, err);
        kmutex_unlock(&mm->lock);
        if (r != -ENOMEM) return r;
        /* Out of memory: free some by ending the largest process, then try
         * again. Give up (the caller gets SIGKILL/EFAULT) when nothing helps. */
        if (attempt >= 20 || signal_pending() || !oom_kill(NULL)) {
            if (user_mode) signal_send(t, SIGKILL);
            return -ENOMEM;
        }
        task_sleep_ms(50);
    }
}

int vm_populate(struct mm *mm, uint64_t addr, uint64_t len, bool write)
{
    kmutex_lock(&mm->lock);
    int r = 0;
    for (uint64_t va = pgdown(addr); va < addr + len && !r; va += PAGE) {
        uint64_t *pte = pte_walk(mm->pml4, va, false);
        if (pte && (*pte & PTE_P) && (!write || (*pte & PTE_W))) continue;
        struct vma *v = vma_find(mm, va);
        if (!v || v->prot == PROT_NONE) continue;
        r = fault_once(mm, va, write && (v->prot & PROT_WRITE) ? PF_WRITE : 0);
    }
    kmutex_unlock(&mm->lock);
    return r == -EACCES || r == -EIO ? 0 : r;
}

/* ---- mmap and friends ------------------------------------------------------- */
int64_t vm_mmap(struct mm *mm, uint64_t addr, uint64_t len, int prot, int flags,
                struct file *file, struct vm_object *obj, uint64_t off, uint64_t file_end)
{
    if (!len) return -EINVAL;
    len = pgup(len);
    if (!len || len > UVM_USER_END - UVM_USER_START) return -ENOMEM;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    kmutex_lock(&mm->lock);
    int64_t r;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if ((addr & (PAGE - 1)) || !user_range_ok(addr, len)) { r = -EINVAL; goto out; }
        if (flags & MAP_FIXED_NOREPLACE) {
            if (!range_free(mm, addr, addr + len)) { r = -EEXIST; goto out; }
        } else if ((r = unmap_range(mm, addr, addr + len))) {
            goto out;
        }
    } else {
        uint64_t hint = pgdown(addr);
        if (hint && user_range_ok(hint, len) && range_free(mm, hint, hint + len)) addr = hint;
        else if (!(addr = find_gap(mm, UVM_MMAP_BASE, UVM_MMAP_END, len))) { r = -ENOMEM; goto out; }
    }
    struct vma *v = kzalloc(sizeof *v);
    if (!v) { r = -ENOMEM; goto out; }
    v->start = addr;
    v->end = addr + len;
    v->prot = (uint32_t)prot;
    v->flags = obj ? VMA_SHARED : 0;
    v->off = off;
    if (obj) { v->obj = obj; vmobj_get(obj); }
    if (file) { v->file = vfs_file_get(file); v->file_end = file_end; }
    vma_insert(mm, v);
    vma_merge_all(mm);
    r = (int64_t)addr;
out:
    kmutex_unlock(&mm->lock);
    if (r > 0 && (flags & MAP_POPULATE)) vm_populate(mm, (uint64_t)r, len, false);
    return r;
}

int vm_munmap(struct mm *mm, uint64_t addr, uint64_t len)
{
    if ((addr & (PAGE - 1)) || !len) return -EINVAL;
    len = pgup(len);
    if (!user_range_ok(addr, len)) return -EINVAL;
    kmutex_lock(&mm->lock);
    int r = unmap_range(mm, addr, addr + len);
    kmutex_unlock(&mm->lock);
    return r;
}

struct protctx { struct mm *mm; const struct vma *v; };
static void prot_fn(uint64_t *pte, uint64_t va, void *ctx)
{
    (void)va;
    struct protctx *c = ctx;
    uint64_t e = *pte;
    if (!has_frame(e)) return;
    uint64_t frame = e & ADDR_MASK, cow = e & PTE_COW;
    if (c->v->prot == PROT_NONE) { *pte = frame | cow | PTE_PROTNONE; return; }
    /* a private page still shared with another process stays read-only
     * (copy-on-write) even if the region becomes writable */
    bool w = (c->v->prot & PROT_WRITE) && ((c->v->flags & VMA_SHARED) || page_refs(frame) == 1);
    if ((c->v->prot & PROT_WRITE) && !w && !(c->v->flags & VMA_SHARED)) cow = PTE_COW;
    if (w) cow = 0;
    *pte = frame | cow | leaf_flags(c->v, w);
}

int vm_mprotect(struct mm *mm, uint64_t addr, uint64_t len, int prot)
{
    if ((addr & (PAGE - 1))) return -EINVAL;
    prot &= ~0x03000000;                                /* PROT_GROWSDOWN/UP: accepted */
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    len = pgup(len);
    if (!len) return 0;
    if (!user_range_ok(addr, len)) return -ENOMEM;
    uint64_t end = addr + len;
    kmutex_lock(&mm->lock);
    int r = 0;
    for (uint64_t a = addr; a < end; ) {                /* all of it must be mapped */
        struct vma *v = vma_find(mm, a);
        if (!v) { r = -ENOMEM; goto out; }
        if ((prot & PROT_WRITE) && (v->flags & VMA_SHARED) && v->file == NULL && v->obj && v->obj->seals & 0x0008) {
            r = -EACCES; goto out;                      /* F_SEAL_WRITE */
        }
        a = v->end;
    }
    if ((r = split_at(mm, addr)) || (r = split_at(mm, end))) goto out;
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->start < addr || v->end > end) continue;
        v->prot = (uint32_t)prot;
        struct protctx c = { mm, v };
        walk_range(mm->pml4, v->start, v->end, prot_fn, &c);
    }
    tlb_range(mm, addr, end);
    vma_merge_all(mm);
out:
    kmutex_unlock(&mm->lock);
    return r;
}

struct movectx { struct mm *mm; int64_t delta; int err; };
static void move_fn(uint64_t *pte, uint64_t va, void *ctx)
{
    struct movectx *c = ctx;
    if (c->err) return;
    uint64_t *d = pte_walk(c->mm->pml4, (uint64_t)((int64_t)va + c->delta), true);
    if (!d) { c->err = -ENOMEM; return; }
    *d = *pte;
    *pte = 0;
}

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

int64_t vm_mremap(struct mm *mm, uint64_t old, uint64_t olen, uint64_t nlen, int flags, uint64_t naddr)
{
    if ((old & (PAGE - 1)) || (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED))) return -EINVAL;
    if ((flags & MREMAP_FIXED) && (!(flags & MREMAP_MAYMOVE) || (naddr & (PAGE - 1)))) return -EINVAL;
    olen = pgup(olen);
    nlen = pgup(nlen);
    if (!nlen || !olen || !user_range_ok(old, olen)) return -EINVAL;
    kmutex_lock(&mm->lock);
    int64_t r;
    struct vma *v = vma_find(mm, old);
    if (!v || old + olen > v->end) { r = -EFAULT; goto out; }     /* must lie in one region */
    if (!(flags & MREMAP_FIXED)) {
        if (nlen <= olen) {                                       /* shrink in place */
            r = unmap_range(mm, old + nlen, old + olen);
            if (!r) r = (int64_t)old;
            goto out;
        }
        if (old + olen == v->end && user_range_ok(old, nlen) && range_free(mm, old + olen, old + nlen)) {
            v->end = old + nlen;                                  /* grow in place */
            r = (int64_t)old;
            goto out;
        }
        if (!(flags & MREMAP_MAYMOVE)) { r = -ENOMEM; goto out; }
        if (!(naddr = find_gap(mm, UVM_MMAP_BASE, UVM_MMAP_END, nlen))) { r = -ENOMEM; goto out; }
    } else {
        if (!user_range_ok(naddr, nlen) || (naddr < old + olen && old < naddr + nlen)) { r = -EINVAL; goto out; }
        if ((r = unmap_range(mm, naddr, naddr + nlen))) goto out;
        v = vma_find(mm, old);
    }
    /* move: carve [old, old+olen) out as its own region, then relocate it */
    if ((r = split_at(mm, old)) || (r = split_at(mm, old + olen))) goto out;
    v = vma_find(mm, old);
    uint64_t keep = olen < nlen ? olen : nlen;
    if (olen > keep) zap_range(mm, old + keep, old + olen);
    struct movectx c = { mm, (int64_t)naddr - (int64_t)old, 0 };
    walk_range(mm->pml4, old, old + keep, move_fn, &c);
    if (c.err) { r = c.err; goto out; }
    tlb_range(mm, old, old + olen);
    /* detach v and re-insert it at the new place */
    for (struct vma **pp = &mm->vmas; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
    if (v->file) v->file_end = v->file_end + (naddr - old);
    v->start = naddr;
    v->end = naddr + nlen;
    vma_insert(mm, v);
    vma_merge_all(mm);
    r = (int64_t)naddr;
out:
    kmutex_unlock(&mm->lock);
    return r;
}

int vm_madvise_dontneed(struct mm *mm, uint64_t addr, uint64_t len)
{
    if (addr & (PAGE - 1)) return -EINVAL;
    len = pgup(len);
    if (!user_range_ok(addr, len)) return -EINVAL;
    kmutex_lock(&mm->lock);
    zap_range(mm, addr, addr + len);                    /* next touch: zeros / file / object page */
    kmutex_unlock(&mm->lock);
    return 0;
}

int vm_mincore(struct mm *mm, uint64_t addr, uint64_t len, uint8_t *vec)
{
    kmutex_lock(&mm->lock);
    int r = 0;
    uint64_t i = 0;
    for (uint64_t va = addr; va < addr + len; va += PAGE, i++) {
        if (!vma_find(mm, va)) { r = -ENOMEM; break; }
        uint64_t *pte = pte_walk(mm->pml4, va, false);
        vec[i] = pte && has_frame(*pte) ? 1 : 0;
    }
    kmutex_unlock(&mm->lock);
    return r;
}

int64_t vm_brk(struct mm *mm, uint64_t addr)
{
    kmutex_lock(&mm->lock);
    uint64_t cur = mm->brk;
    if (addr < mm->brk_start || addr >= UVM_MMAP_BASE) goto out;
    uint64_t oend = pgup(mm->brk), nend = pgup(addr);
    if (nend > oend) {
        if (!range_free(mm, oend, nend)) goto out;      /* would run into a mapping */
        struct vma *v = kzalloc(sizeof *v);
        if (!v) goto out;
        v->start = oend;
        v->end = nend;
        v->prot = PROT_READ | PROT_WRITE;
        v->flags = VMA_HEAP;
        vma_insert(mm, v);
        vma_merge_all(mm);
    } else if (nend < oend) {
        unmap_range(mm, nend, oend);
    }
    mm->brk = cur = addr;
out:
    kmutex_unlock(&mm->lock);
    return (int64_t)cur;
}

void vm_mark(struct mm *mm, uint64_t addr, uint32_t flag)
{
    kmutex_lock(&mm->lock);
    struct vma *v = vma_find(mm, addr);
    if (v) v->flags |= flag;
    kmutex_unlock(&mm->lock);
}

/* ---- /proc/PID/maps --------------------------------------------------------- */
size_t vm_maps(struct mm *mm, char *buf, size_t cap)
{
    size_t n = 0;
    kmutex_lock(&mm->lock);
    for (struct vma *v = mm->vmas; v && n + 160 < cap; v = v->next) {
        char path[VFS_PATH_MAX] = "";
        if (v->file && v->file->vn) vfs_path_of(v->file->vn, path, sizeof path);
        else if (v->flags & VMA_STACK) strlcpy(path, "[stack]", sizeof path);
        else if (v->flags & VMA_HEAP) strlcpy(path, "[heap]", sizeof path);
        else if (v->obj) strlcpy(path, "/memfd: (deleted)", sizeof path);
        n += (size_t)snprintf(buf + n, cap - n, "%012lx-%012lx %c%c%c%c %08lx 00:00 %-10lu %s\n",
                              v->start, v->end, (v->prot & PROT_READ) ? 'r' : '-',
                              (v->prot & PROT_WRITE) ? 'w' : '-', (v->prot & PROT_EXEC) ? 'x' : '-',
                              (v->flags & VMA_SHARED) ? 's' : 'p', v->off,
                              v->file && v->file->vn ? v->file->vn->ino : 0, path);
    }
    kmutex_unlock(&mm->lock);
    return n < cap ? n : cap;
}

/* ---- out of memory ------------------------------------------------------------ */
bool oom_kill(struct tcb *spare)
{
    struct tcb *victim = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (t == spare || !t->user || !t->mm || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        if (!victim || t->mm->rss > victim->mm->rss) victim = t;
    }
    if (!victim) return false;
    if (!victim->oom_killed) {
        victim->oom_killed = true;
        kprintf("oom: out of memory: killing pid %d (%s), %lu KiB resident\n", victim->pid, victim->name,
                victim->mm->rss * 4);
        signal_send(victim, SIGKILL);
    }
    return true;
}
