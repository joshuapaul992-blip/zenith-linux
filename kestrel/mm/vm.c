/* mm/vm.c -- user address spaces: page-fault resolution (see vm.h)
 * Every user page is mapped eagerly for now, so no fault can be resolved. */
#include <kernel/vm.h>
#include <kernel/posix.h>

int vm_fault(uint64_t addr, uint64_t err, bool user_mode)
{
    (void)addr; (void)err; (void)user_mode;
    return -EFAULT;
}
