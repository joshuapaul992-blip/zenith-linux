/* include/kernel/multiboot2.h -- subset of the Multiboot2 boot information format */
#ifndef KESTREL_MULTIBOOT2_H
#define KESTREL_MULTIBOOT2_H

#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC 0x36D76289u

enum {
    MB2_TAG_END              = 0,
    MB2_TAG_CMDLINE          = 1,
    MB2_TAG_BOOT_LOADER_NAME = 2,
    MB2_TAG_MODULE           = 3,
    MB2_TAG_BASIC_MEMINFO    = 4,
    MB2_TAG_BOOTDEV          = 5,
    MB2_TAG_MMAP             = 6,
    MB2_TAG_VBE              = 7,
    MB2_TAG_FRAMEBUFFER      = 8,
    MB2_TAG_EFI64            = 12,
    MB2_TAG_ACPI_OLD         = 14,
    MB2_TAG_ACPI_NEW         = 15,
};

struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed));

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_tag_string {
    uint32_t type, size;
    char     string[];
} __attribute__((packed));

struct mb2_tag_module {             /* a file GRUB loaded with module2 */
    uint32_t type, size;
    uint32_t mod_start, mod_end;    /* physical, end exclusive */
    char     string[];              /* the module's command line */
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;      /* 1 = available, 3 = ACPI reclaimable, 4 = NVS, 5 = bad */
    uint32_t zero;
} __attribute__((packed));

struct mb2_tag_mmap {
    uint32_t type, size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

#define MB2_FB_TYPE_INDEXED 0
#define MB2_FB_TYPE_RGB     1
#define MB2_FB_TYPE_EGA     2

struct mb2_tag_framebuffer {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    /* fb_type == RGB: */
    uint8_t  red_pos,   red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos,  blue_size;
} __attribute__((packed));

static inline struct mb2_tag *mb2_first_tag(uintptr_t mbi) {
    return (struct mb2_tag *)(mbi + 8);
}
static inline struct mb2_tag *mb2_next_tag(struct mb2_tag *t) {
    return (struct mb2_tag *)((uint8_t *)t + ((t->size + 7) & ~7u));
}

#endif
