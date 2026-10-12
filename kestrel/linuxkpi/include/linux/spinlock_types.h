/* linux/spinlock_types.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_SPINLOCK_TYPES_H
#define _LKPI_LINUX_SPINLOCK_TYPES_H

#include <linux/types.h>
#include <linux/lockdep.h>

typedef struct raw_spinlock { volatile unsigned int locked; } raw_spinlock_t;
typedef struct spinlock { raw_spinlock_t rlock; } spinlock_t;
typedef struct { raw_spinlock_t lock; } rwlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED(name)  { .locked = 0 }
#define __SPIN_LOCK_UNLOCKED(name)      { .rlock = { .locked = 0 } }
#define __RW_LOCK_UNLOCKED(name)        { .lock = { .locked = 0 } }
#define DEFINE_RAW_SPINLOCK(x)          raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)
#define DEFINE_SPINLOCK(x)              spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RWLOCK(x)                rwlock_t x = __RW_LOCK_UNLOCKED(x)

#endif
