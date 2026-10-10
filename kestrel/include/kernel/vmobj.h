/* include/kernel/vmobj.h -- page-backed memory objects
 *
 * A vm_object is a sparse array of reference-counted pages plus a size in
 * bytes. It is the storage of every ramfs file (so read()/write() and
 * shared mappings of the same file see the same bytes), of memfd and
 * /dev/shm objects, and of MAP_SHARED|MAP_ANONYMOUS memory. Pages are
 * allocated on first touch, zero-filled. The object holds one reference to
 * each of its pages; every mapping of a page adds one more (page_ref). */
#ifndef KESTREL_VMOBJ_H
#define KESTREL_VMOBJ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/posix.h>

struct vm_object {
    int       refs;             /* holders: vnodes, memfds, mappings (atomic) */
    uint64_t  size;             /* bytes                                      */
    uint64_t  npages;           /* capacity of pages[]                        */
    uint64_t *pages;            /* physical addresses, 0 = not yet allocated  */
    uint32_t  seals;            /* memfd F_SEAL_*                              */
};

#define VMOBJ_MAX_SIZE  (1ull << 36)                /* 64 GiB: refuse anything larger */

struct vm_object *vmobj_new(uint64_t size);         /* 1 reference; NULL on OOM */
void     vmobj_get(struct vm_object *o);
void     vmobj_put(struct vm_object *o);            /* frees at 0 */
/* Physical address of page `idx` (object-relative); allocated if `alloc`.
 * 0 if absent (and !alloc) or out of memory. */
uint64_t vmobj_page(struct vm_object *o, uint64_t idx, bool alloc);
ssize_t  vmobj_read(struct vm_object *o, void *buf, size_t len, uint64_t off);
ssize_t  vmobj_write(struct vm_object *o, const void *buf, size_t len, uint64_t off);
int      vmobj_truncate(struct vm_object *o, uint64_t size);
uint64_t vmobj_resident(const struct vm_object *o);  /* pages allocated */

#endif
