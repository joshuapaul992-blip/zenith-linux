/* linux/lockdep.h -- Kestrel's Linux KPI: no lock validator, the annotations compile away */
#ifndef _LKPI_LINUX_LOCKDEP_H
#define _LKPI_LINUX_LOCKDEP_H
struct lock_class_key { int dummy; };
struct lockdep_map { int dummy; };
#define lockdep_set_class(lock, key)                    do { (void)(key); } while (0)
#define lockdep_set_class_and_name(lock, key, name)     do { (void)(key); } while (0)
#define lockdep_set_subclass(lock, sub)                 do { } while (0)
#define lockdep_register_key(key)                       do { } while (0)
#define lockdep_unregister_key(key)                     do { } while (0)
#define lockdep_assert_held(l)                          do { (void)(l); } while (0)
#define lockdep_assert_held_write(l)                    do { (void)(l); } while (0)
#define lockdep_assert_held_read(l)                     do { (void)(l); } while (0)
#define lockdep_assert_held_once(l)                     do { (void)(l); } while (0)
#define lockdep_assert_not_held(l)                      do { (void)(l); } while (0)
#define lockdep_assert(c)                               do { } while (0)
#define lockdep_assert_none_held_once()                 do { } while (0)
#define lockdep_is_held(l)                              1
#define might_lock(l)                                   do { } while (0)
#define might_lock_nested(l, s)                         do { } while (0)
#define lock_acquire(...)                               do { } while (0)
#define lock_release(...)                               do { } while (0)
#define SINGLE_DEPTH_NESTING                            1
#endif
