/* mm/pmm.c -- bitmap physical frame allocator fed by the Multiboot2 memory map */
#include <kernel/mm.h>
#include <kernel/multiboot2.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>

#define MAX_FRAMES (PMM_MAX_PHYS / PAGE_SIZE)

static uint8_t  bitmap[MAX_FRAMES / 8];      /* 1 = used */
static uint64_t total_frames, free_frames, highest;
static uint64_t search_hint;

static inline void set_used(uint64_t f) { bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void set_free(uint64_t f) { bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline bool is_used(uint64_t f)  { return bitmap[f >> 3] & (1u << (f & 7)); }

static void reserve_range(uint64_t start, uint64_t end)
{
    for (uint64_t f = start / PAGE_SIZE; f < (end + PAGE_SIZE - 1) / PAGE_SIZE && f < MAX_FRAMES; f++)
        if (!is_used(f)) { set_used(f); free_frames--; }
}

void pmm_init(uintptr_t mbi)
{
    memset(bitmap, 0xFF, sizeof bitmap);

    for (struct mb2_tag *t = mb2_first_tag(mbi); t->type != MB2_TAG_END; t = mb2_next_tag(t)) {
        if (t->type != MB2_TAG_MMAP) continue;
        struct mb2_tag_mmap *mm = (struct mb2_tag_mmap *)t;
        for (uint8_t *p = (uint8_t *)mm->entries; p < (uint8_t *)t + t->size; p += mm->entry_size) {
            struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
            static const char *types[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad" };
            kprintf("  mem %016lx-%016lx %s\n", e->addr, e->addr + e->len - 1,
                    types[e->type <= 5 ? e->type : 0]);
            if (e->type != 1) continue;
            uint64_t s = (e->addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t end = (e->addr + e->len) & ~(PAGE_SIZE - 1);
            if (end > PMM_MAX_PHYS) end = PMM_MAX_PHYS;
            for (uint64_t a = s; a < end; a += PAGE_SIZE) {
                uint64_t f = a / PAGE_SIZE;
                if (is_used(f)) { set_free(f); free_frames++; total_frames++; }
            }
            if (end > highest) highest = end;
        }
    }

    /* keep firmware/legacy area, the kernel image and the boot info safe */
    reserve_range(0, 0x100000);
    reserve_range((uint64_t)_kernel_start, (uint64_t)_kernel_end);
    reserve_range(mbi, mbi + ((struct mb2_info *)mbi)->total_size);
    search_hint = 0x100000 / PAGE_SIZE;

    kprintf("pmm: %lu MiB usable, %lu MiB free, kernel %p-%p\n",
            total_frames * PAGE_SIZE >> 20, free_frames * PAGE_SIZE >> 20,
            _kernel_start, _kernel_end);
}

uint64_t pmm_alloc_contig(size_t n)
{
    uint64_t flags = irq_save();
    uint64_t limit = highest / PAGE_SIZE;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t run = 0, start = pass ? 0x100 : search_hint;
        for (uint64_t f = start; f < limit; f++) {
            if (is_used(f)) { run = 0; continue; }
            if (++run == n) {
                uint64_t first = f + 1 - n;
                for (uint64_t i = first; i <= f; i++) set_used(i);
                free_frames -= n;
                if (n == 1) search_hint = f + 1;
                irq_restore(flags);
                memset((void *)(first * PAGE_SIZE), 0, n * PAGE_SIZE);
                return first * PAGE_SIZE;
            }
        }
    }
    irq_restore(flags);
    return 0;
}

uint64_t pmm_alloc(void) { return pmm_alloc_contig(1); }

void pmm_free_contig(uint64_t phys, size_t n)
{
    uint64_t flags = irq_save();
    for (size_t i = 0; i < n; i++) {
        uint64_t f = phys / PAGE_SIZE + i;
        if (f < MAX_FRAMES && is_used(f)) { set_free(f); free_frames++; }
    }
    if (phys / PAGE_SIZE < search_hint) search_hint = phys / PAGE_SIZE;
    irq_restore(flags);
}

void pmm_free(uint64_t phys) { pmm_free_contig(phys, 1); }

uint64_t pmm_total_bytes(void)   { return total_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)    { return free_frames * PAGE_SIZE; }
uint64_t pmm_highest_usable(void){ return highest; }
