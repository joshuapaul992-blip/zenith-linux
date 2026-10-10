/* include/kernel/bootinfo.h -- facts gathered at boot, shared by procfs/sysfs/UI */
#ifndef KESTREL_BOOTINFO_H
#define KESTREL_BOOTINFO_H

#include <stdint.h>
#include <stdbool.h>

#define KESTREL_NAME     "Kestrel"
#define KESTREL_VERSION  "0.2.0"
#define KESTREL_RELEASE  "0.2.0-release"
#define KESTREL_CODENAME "Stable Storage Engine"
#define KESTREL_HOSTNAME "kestrel-pc"
#define KESTREL_MACHINE  "x86_64"

/* Advanced boot options (F8 menu) */
#define BOOTOPT_SAFE_MODE    0x01   /* cooperative scheduling, no background workers */
#define BOOTOPT_DEBUG        0x02   /* mirror the kernel log to the screen            */
#define BOOTOPT_VERBOSE_VFS  0x04   /* list the VFS tree at boot                      */
#define BOOTOPT_NO_DBLBUF    0x08   /* draw straight to video memory                   */

struct bootinfo {
    char     loader[64];
    char     cmdline[1024];
    bool     uefi;                  /* EFI system table tag present */
    char     cpu_vendor[13];
    char     cpu_brand[49];
    uint32_t boot_entry;            /* index chosen in the boot manager */
    char     boot_entry_name[64];
    uint32_t boot_flags;            /* BOOTOPT_* */
    uint64_t boot_tsc;
    uintptr_t mbi;                  /* Multiboot2 information structure */
};

extern struct bootinfo g_boot;

#endif
