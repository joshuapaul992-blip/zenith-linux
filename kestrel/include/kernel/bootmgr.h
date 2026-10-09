/* include/kernel/bootmgr.h -- BIOS-style boot manager screen */
#ifndef KESTREL_BOOTMGR_H
#define KESTREL_BOOTMGR_H

#include <stdint.h>

#define BOOTMGR_TIMEOUT_SEC 10

struct boot_choice {
    int      entry;         /* index into the OS entry list        */
    uint32_t flags;         /* BOOTOPT_* from bootinfo.h           */
    const char *name;
};

/* Runs the menu until an entry is launched (ENTER, or the countdown
 * reaching zero). Tools are run in place and return to the menu. */
struct boot_choice bootmgr_run(void);

#endif
