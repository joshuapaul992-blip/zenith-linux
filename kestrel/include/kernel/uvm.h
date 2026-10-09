/* include/kernel/uvm.h -- per-process (user) address spaces
 *
 * Layout of every address space (4-level paging, 48-bit):
 *
 *   PML4[0..127]    0 .. 64 TiB          kernel: the boot identity map (0-4 GiB)
 *                                        plus MMIO the kernel maps later. The
 *                                        PDPTs are allocated once at boot and
 *                                        shared by every address space, so a
 *                                        mapping the kernel adds later is seen
 *                                        everywhere. Supervisor-only.
 *   PML4[128..255]  64 TiB .. 128 TiB    user, private to the process:
 *     UVM_IMAGE_BASE   0x4000_0000_0000  ET_DYN (static-PIE) images load here
 *     UVM_MMAP_BASE    0x6000_0000_0000  anonymous mmap() region, grows up
 *     UVM_STACK_TOP    0x7fff_ff00_0000  main-thread stack, grows down
 *   PML4[256..511]                       unused (no higher-half kernel yet)
 *
 * Kernel code reaches user memory directly while the process' CR3 is
 * loaded (single CPU; syscalls run on the caller's address space). */
#ifndef KESTREL_UVM_H
#define KESTREL_UVM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UVM_USER_START      0x0000400000000000ul
#define UVM_USER_END        0x0000800000000000ul
#define UVM_IMAGE_BASE      0x0000400000000000ull
#define UVM_MMAP_BASE       0x0000600000000000ull
#define UVM_STACK_TOP       0x00007fffff000000ull
#define UVM_STACK_SIZE      (1ull << 20)
#define UVM_PAGE            4096ull

#define UVM_W               0x1             /* writable                       */
#define UVM_X               0x2             /* executable (no NX yet: ignored) */

void     uvm_init(void);                    /* after pmm_init, before any user process */
uint64_t uvm_kernel_pml4(void);
uint64_t uvm_create(void);                  /* new PML4 (phys), 0 on OOM       */
void     uvm_destroy(uint64_t pml4);        /* frees user pages + tables       */
/* Map zeroed frames over [va, va+len) (page aligned) in `pml4`; pages
 * already present are kept. False on OOM or outside the user range. */
bool     uvm_map(uint64_t pml4, uint64_t va, uint64_t len, int prot);
void     uvm_unmap(uint64_t pml4, uint64_t va, uint64_t len);
bool     uvm_range_ok(uint64_t va, uint64_t len);       /* inside the user half */
bool     uvm_mapped(uint64_t pml4, uint64_t va, uint64_t len);
uint64_t uvm_pages(uint64_t pml4);          /* user pages mapped (statistics)  */

#endif
