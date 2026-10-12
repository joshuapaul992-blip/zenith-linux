/* linux/smp.h -- Kestrel's Linux KPI: processors */
#ifndef _LKPI_LINUX_SMP_H
#define _LKPI_LINUX_SMP_H
#include <linux/types.h>
int lkpi_smp_processor_id(void);
int lkpi_num_online_cpus(void);
#define smp_processor_id()      lkpi_smp_processor_id()
#define raw_smp_processor_id()  lkpi_smp_processor_id()
#define get_cpu()               ({ preempt_disable(); lkpi_smp_processor_id(); })
#define put_cpu()               preempt_enable()
#define NR_CPUS                 CONFIG_NR_CPUS
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < lkpi_num_online_cpus(); (cpu)++)
#define for_each_online_cpu(cpu)   for_each_possible_cpu(cpu)
#endif
