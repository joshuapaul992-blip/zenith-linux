/* asm/byteorder.h -- Kestrel's Linux KPI: x86 is little-endian */
#ifndef _LKPI_ASM_BYTEORDER_H
#define _LKPI_ASM_BYTEORDER_H

#include <linux/swab.h>

#define __LITTLE_ENDIAN         1234
#define __LITTLE_ENDIAN_BITFIELD

#define cpu_to_le64(x)          ((__le64)(u64)(x))
#define le64_to_cpu(x)          ((u64)(__le64)(x))
#define cpu_to_le32(x)          ((__le32)(u32)(x))
#define le32_to_cpu(x)          ((u32)(__le32)(x))
#define cpu_to_le16(x)          ((__le16)(u16)(x))
#define le16_to_cpu(x)          ((u16)(__le16)(x))
#define cpu_to_be64(x)          ((__be64)swab64(x))
#define be64_to_cpu(x)          swab64((u64)(__be64)(x))
#define cpu_to_be32(x)          ((__be32)swab32(x))
#define be32_to_cpu(x)          swab32((u32)(__be32)(x))
#define cpu_to_be16(x)          ((__be16)swab16(x))
#define be16_to_cpu(x)          swab16((u16)(__be16)(x))
#define __cpu_to_le16(x)        cpu_to_le16(x)
#define __cpu_to_le32(x)        cpu_to_le32(x)
#define __cpu_to_le64(x)        cpu_to_le64(x)
#define __le16_to_cpu(x)        le16_to_cpu(x)
#define __le32_to_cpu(x)        le32_to_cpu(x)
#define __le64_to_cpu(x)        le64_to_cpu(x)
#define __cpu_to_be16(x)        cpu_to_be16(x)
#define __cpu_to_be32(x)        cpu_to_be32(x)
#define __be16_to_cpu(x)        be16_to_cpu(x)
#define __be32_to_cpu(x)        be32_to_cpu(x)
#define htonl(x)                cpu_to_be32(x)
#define ntohl(x)                be32_to_cpu(x)
#define htons(x)                cpu_to_be16(x)
#define ntohs(x)                be16_to_cpu(x)

static inline u16 le16_to_cpup(const __le16 *p) { return *p; }
static inline u32 le32_to_cpup(const __le32 *p) { return *p; }
static inline u64 le64_to_cpup(const __le64 *p) { return *p; }
static inline u16 be16_to_cpup(const __be16 *p) { return swab16(*p); }
static inline u32 be32_to_cpup(const __be32 *p) { return swab32(*p); }
static inline u64 be64_to_cpup(const __be64 *p) { return swab64(*p); }
static inline void le16_add_cpu(__le16 *v, u16 val) { *v = cpu_to_le16(le16_to_cpu(*v) + val); }
static inline void le32_add_cpu(__le32 *v, u32 val) { *v = cpu_to_le32(le32_to_cpu(*v) + val); }
static inline void cpu_to_le32_array(u32 *buf, unsigned int words) { (void)buf; (void)words; }
static inline void le32_to_cpu_array(u32 *buf, unsigned int words) { (void)buf; (void)words; }

#endif
