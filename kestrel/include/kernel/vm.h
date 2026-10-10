/* include/kernel/vm.h -- page-fault resolution for user address spaces
 *
 * vm_fault() is called for every page fault on a user address, from user
 * mode or from a uaccess routine in the kernel. It returns 0 when the fault
 * was resolved (the access can be retried) and -EFAULT when it is a real
 * error: the caller then sends SIGSEGV/SIGBUS (user mode) or takes the
 * exception-table fixup (kernel mode). */
#ifndef KESTREL_VM_H
#define KESTREL_VM_H

#include <stdint.h>
#include <stdbool.h>

/* Page-fault error code bits */
#define PF_PRESENT  0x01        /* protection violation (else: not present) */
#define PF_WRITE    0x02
#define PF_USER     0x04
#define PF_RSVD     0x08
#define PF_INSN     0x10        /* instruction fetch (NX) */

int vm_fault(uint64_t addr, uint64_t err, bool user_mode);

#endif
