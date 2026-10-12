/* asm/processor.h -- Kestrel's Linux KPI */
#ifndef _LKPI_ASM_PROCESSOR_H
#define _LKPI_ASM_PROCESSOR_H
static inline void cpu_relax(void) { __asm__ __volatile__("pause" ::: "memory"); }
#endif
