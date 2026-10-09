/* include/kernel/bootvol.h -- locating and mounting the Kestrel boot volume
 *
 * Nothing is found by device name. A disk qualifies as the boot volume when
 * one of two independent markers leads to a valid volume header:
 *
 *   Option A  partition table: a GPT partition whose type GUID is
 *             KESTREL_GPT_TYPE (the 16 ASCII bytes "KESTREL-BOOT-VOL"), or
 *             an MBR partition of type KESTREL_MBR_TYPE (0x4B). The volume
 *             header is the partition's first block. With kestrel.uuid= the
 *             header UUID (or the GPT unique partition GUID) must match.
 *   Option B  sector-0 signature: "KESTREL_BOOT" at byte 3 of the disk's
 *             block 0 (the BPB area that boot loaders leave free), followed
 *             by a little-endian u64: the block number of the volume header.
 *
 * The header (512 bytes, CRC32-protected) describes a ustar payload that is
 * CRC-checked and mounted read-only on /boot.
 *
 * Kernel command line:
 *   kestrel.root=auto   (default) use a volume if one is attached; boot on
 *                       without one. Waits only while USB ports are settling.
 *   kestrel.root=usb    a volume on USB storage is REQUIRED: wait up to
 *                       rootwait for it, else recovery
 *   kestrel.root=any    same, any disk (USB or SATA)
 *   kestrel.root=none   skip discovery
 *   kestrel.rootwait=MS settle/wait budget (default 3000, max 60000)
 *   kestrel.uuid=UUID   accept only the volume with this UUID
 *   kestrel.rootverify=0  skip the payload CRC check
 */
#ifndef KESTREL_BOOTVOL_H
#define KESTREL_BOOTVOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KESTREL_MAGIC           "KESTREL_BOOT"      /* 12 bytes, no NUL */
#define KESTREL_MAGIC_LEN       12
#define KESTREL_SECTOR0_OFFSET  3                   /* option B marker  */
#define KESTREL_MBR_TYPE        0x4B
#define KESTREL_GPT_TYPE        "KESTREL-BOOT-VOL"  /* 16 bytes, raw    */
#define KESTREL_VOL_VERSION     1
#define KESTREL_FLAG_USTAR      0x1

struct kestrel_volhdr {
    char     magic[12];             /* "KESTREL_BOOT"                              */
    uint16_t version;               /* 1                                            */
    uint16_t header_size;           /* 512                                          */
    uint32_t header_crc;            /* CRC32 of all 512 bytes with this field = 0   */
    uint8_t  uuid[16];              /* RFC 4122 byte order (as printed)             */
    char     label[32];             /* NUL padded                                   */
    uint64_t payload_offset;        /* bytes from the header, multiple of 512       */
    uint64_t payload_bytes;
    uint32_t payload_crc;           /* CRC32 of the payload                         */
    uint32_t flags;                 /* KESTREL_FLAG_*                               */
    uint64_t created;               /* Unix time                                    */
    uint8_t  reserved[412];
} __attribute__((packed));
_Static_assert(sizeof(struct kestrel_volhdr) == 512, "volume header is one 512-byte sector");

enum bootvol_result {
    BV_NOT_RUN = 0, BV_DISABLED, BV_MOUNTED, BV_NOT_FOUND, BV_IO_ERROR, BV_CONTINUED,
};

struct bootvol_status {
    enum bootvol_result result;
    char     policy[8];
    uint32_t rootwait_ms;
    bool     verify;
    char     want_uuid[40];

    /* the mounted volume */
    char     device[16];
    const char *method;
    uint8_t  uuid[16];
    char     label[33];
    uint64_t header_block;
    uint64_t payload_bytes;
    uint32_t files;

    /* the search */
    uint64_t elapsed_ms;
    uint32_t attempts, disks_seen, candidates, usb_disks;
    char     error[160];            /* most specific failure seen */
    int      last_errno;
};

/* Run discovery according to the command line; on a mandatory policy that
 * fails, show diagnostics and enter the recovery shell (which may retry).
 * Returns 0 when a volume is mounted on /boot, -errno otherwise. */
int  bootvol_mount_root(void);

/* One discovery pass over a single device (recovery 'boot <dev>'). */
int  bootvol_try_device(const char *name);

const struct bootvol_status *bootvol_status(void);
size_t bootvol_proc(char *buf, size_t cap);
void   bootvol_uuid_str(const uint8_t u[16], char out[37]);

#endif
