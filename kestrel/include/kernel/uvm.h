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
 *     UVM_IMAGE_BASE   0x4000_0000_0000  ET_DYN programs (PIE) load here
 *     UVM_INTERP_BASE  0x5000_0000_0000  their ELF interpreter (ld.so)
 *     UVM_MMAP_BASE    0x6000_0000_0000  mmap() region, first fit, grows up
 *     UVM_STACK_TOP    0x7fff_ff00_0000  main-thread stack (8 MiB), grows down
 *
 * The regions, demand paging and copy-on-write of the user half are in
 * mm/vm.c (vm.h); this file only sets up the shared kernel half.
 *   PML4[256..511]                       unused (no higher-half kernel yet)
 *
 * Kernel code reaches user memory only through uaccess.h. */
#ifndef KESTREL_UVM_H
#define KESTREL_UVM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UVM_USER_START      0x0000400000000000ul
/* One page short of the canonical boundary, as on Linux (TASK_SIZE): if user
 * code could sit in the last page, a `syscall` there would make SYSRET
 * return to a non-canonical RIP, which Intel CPUs fault on in ring 0 with
 * the user's stack (CVE-2012-0217). */
#define UVM_USER_END        0x00007ffffffff000ul
#define UVM_IMAGE_BASE      0x0000400000000000ull
#define UVM_MMAP_BASE       0x0000600000000000ull
#define UVM_STACK_TOP       0x00007fffff000000ull
#define UVM_STACK_SIZE      (8ull << 20)    /* main-thread stack (RLIMIT_STACK), demand paged */
#define UVM_INTERP_BASE     0x0000500000000000ull   /* the ELF interpreter (ld.so) */
#define UVM_PAGE            4096ull

void     uvm_init(void);                    /* after pmm_init, before any user process */
uint64_t uvm_kernel_pml4(void);
uint64_t uvm_create(void);                  /* new PML4 (phys) sharing the kernel half; 0 on OOM */
bool     uvm_range_ok(uint64_t va, uint64_t len);       /* inside the user half */

#endif
