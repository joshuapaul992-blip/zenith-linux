/* linux/unaligned.h -- Kestrel's Linux KPI: loads and stores at any address */
#ifndef _LKPI_LINUX_UNALIGNED_H
#define _LKPI_LINUX_UNALIGNED_H

#include <linux/types.h>
#include <asm/byteorder.h>

#define __get_unaligned_t(T, p) ({ const struct { T x; } __packed *__pptr = (typeof(__pptr))(p); __pptr->x; })
#define __put_unaligned_t(T, v, p) do { struct { T x; } __packed *__pptr = (typeof(__pptr))(p); __pptr->x = (v); } while (0)
#define get_unaligned(p)        __get_unaligned_t(typeof(*(p)), (p))
#define put_unaligned(v, p)     __put_unaligned_t(typeof(*(p)), (v), (p))

static inline u16 get_unaligned_le16(const void *p) { return __get_unaligned_t(u16, p); }
static inline u32 get_unaligned_le32(const void *p) { return __get_unaligned_t(u32, p); }
static inline u64 get_unaligned_le64(const void *p) { return __get_unaligned_t(u64, p); }
static inline u16 get_unaligned_be16(const void *p) { return swab16(__get_unaligned_t(u16, p)); }
static inline u32 get_unaligned_be32(const void *p) { return swab32(__get_unaligned_t(u32, p)); }
static inline u64 get_unaligned_be64(const void *p) { return swab64(__get_unaligned_t(u64, p)); }
static inline void put_unaligned_le16(u16 v, void *p) { __put_unaligned_t(u16, v, p); }
static inline void put_unaligned_le32(u32 v, void *p) { __put_unaligned_t(u32, v, p); }
static inline void put_unaligned_le64(u64 v, void *p) { __put_unaligned_t(u64, v, p); }
static inline void put_unaligned_be16(u16 v, void *p) { __put_unaligned_t(u16, swab16(v), p); }
static inline void put_unaligned_be32(u32 v, void *p) { __put_unaligned_t(u32, swab32(v), p); }
static inline void put_unaligned_be64(u64 v, void *p) { __put_unaligned_t(u64, swab64(v), p); }
static inline u32 get_unaligned_le24(const void *p)
{ const u8 *b = p; return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16; }

#endif
