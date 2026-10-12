/* linux/dma-direction.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_DMA_DIRECTION_H
#define _LKPI_LINUX_DMA_DIRECTION_H
enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
static inline int valid_dma_direction(enum dma_data_direction d) { return d == DMA_BIDIRECTIONAL || d == DMA_TO_DEVICE || d == DMA_FROM_DEVICE; }
#endif
