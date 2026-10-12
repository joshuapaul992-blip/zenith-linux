/* linux/gfp_types.h -- Kestrel's Linux KPI: allocation flags
 *
 * Kestrel's allocators never sleep and are safe in interrupt handlers,
 * so only __GFP_ZERO, __GFP_DMA32 (and __GFP_DMA) and __GFP_NOWARN
 * change anything; the others are accepted for source compatibility. */
#ifndef _LKPI_LINUX_GFP_TYPES_H
#define _LKPI_LINUX_GFP_TYPES_H

#include <linux/types.h>

#define __GFP_DMA               ((gfp_t)0x01u)
#define __GFP_HIGHMEM           ((gfp_t)0x02u)
#define __GFP_DMA32             ((gfp_t)0x04u)
#define __GFP_MOVABLE           ((gfp_t)0x08u)
#define __GFP_RECLAIMABLE       ((gfp_t)0x10u)
#define __GFP_HIGH              ((gfp_t)0x20u)
#define __GFP_IO                ((gfp_t)0x40u)
#define __GFP_FS                ((gfp_t)0x80u)
#define __GFP_ZERO              ((gfp_t)0x100u)
#define __GFP_DIRECT_RECLAIM    ((gfp_t)0x400u)
#define __GFP_KSWAPD_RECLAIM    ((gfp_t)0x800u)
#define __GFP_WRITE             ((gfp_t)0x1000u)
#define __GFP_NOWARN            ((gfp_t)0x2000u)
#define __GFP_RETRY_MAYFAIL     ((gfp_t)0x4000u)
#define __GFP_NOFAIL            ((gfp_t)0x8000u)
#define __GFP_NORETRY           ((gfp_t)0x10000u)
#define __GFP_MEMALLOC          ((gfp_t)0x20000u)
#define __GFP_COMP              ((gfp_t)0x40000u)
#define __GFP_NOMEMALLOC        ((gfp_t)0x80000u)
#define __GFP_HARDWALL          ((gfp_t)0x100000u)
#define __GFP_THISNODE          ((gfp_t)0x200000u)
#define __GFP_ACCOUNT           ((gfp_t)0x400000u)
#define __GFP_NOLOCKDEP         ((gfp_t)0x800000u)
#define __GFP_RECLAIM           (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)

#define GFP_ATOMIC              (__GFP_HIGH | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL              (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT      (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT              (__GFP_KSWAPD_RECLAIM)
#define GFP_NOIO                (__GFP_RECLAIM)
#define GFP_NOFS                (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER                (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA                 __GFP_DMA
#define GFP_DMA32               __GFP_DMA32
#define GFP_HIGHUSER            (GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE    (GFP_HIGHUSER | __GFP_MOVABLE)
#define GFP_TRANSHUGE_LIGHT     (GFP_HIGHUSER_MOVABLE | __GFP_COMP | __GFP_NOMEMALLOC | __GFP_NOWARN)

static inline bool gfpflags_allow_blocking(const gfp_t gfp) { return !!(gfp & __GFP_DIRECT_RECLAIM); }

#endif
