/* linux/kconfig.h -- Kestrel's Linux KPI: the configuration a Linux file sees
 *
 * Force-included into every Linux-side file (as Kbuild does). The CONFIG_
 * symbols describe what the KPI provides, not a Linux .config. */
#ifndef _LKPI_LINUX_KCONFIG_H
#define _LKPI_LINUX_KCONFIG_H

#define CONFIG_X86                  1
#define CONFIG_X86_64               1
#define CONFIG_64BIT                1
#define CONFIG_MMU                  1
#define CONFIG_SMP                  1
#define CONFIG_NR_CPUS              16
#define CONFIG_HZ                   1000
#define CONFIG_PCI                  1
#define CONFIG_PCI_MSI              1
#define CONFIG_HAS_IOMEM            1
#define CONFIG_HAS_IOPORT           1
#define CONFIG_HAS_DMA              1
#define CONFIG_PM                   1
#define CONFIG_WATCHDOG_CORE        1
#define CONFIG_PVPANIC              1
#define CONFIG_PVPANIC_PCI          1
#define CONFIG_PRINTK               1
#define CONFIG_PANIC_TIMEOUT        0
#define CONFIG_PAGE_OFFSET          0

/* IS_ENABLED(CONFIG_FOO): 1 when CONFIG_FOO is defined to 1, else 0,
 * usable in C expressions and in #if. The trick: a defined option expands
 * to "1", which pastes into a macro that inserts an extra argument. */
#define __LKPI_ARG_PLACEHOLDER_1                0,
#define __lkpi_take_second_arg(ignored, val, ...) val
#define __lkpi_is_defined(x)                    ___lkpi_is_defined(x)
#define ___lkpi_is_defined(val)                 ____lkpi_is_defined(__LKPI_ARG_PLACEHOLDER_##val)
#define ____lkpi_is_defined(arg1_or_junk)       __lkpi_take_second_arg(arg1_or_junk 1, 0)

#define IS_BUILTIN(option)      __lkpi_is_defined(option)
#define IS_MODULE(option)       0
#define IS_ENABLED(option)      IS_BUILTIN(option)
#define IS_REACHABLE(option)    IS_BUILTIN(option)

#endif
