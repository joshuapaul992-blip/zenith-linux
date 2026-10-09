/* =============================================================================
 *  usb_msc.h -- USB Mass Storage Class, Bulk-Only Transport + SCSI (freestanding)
 *
 *  Sits on top of the xHCI driver's class-driver interface (xhci.h):
 *  xhci_msc_*(), xhci_bulk_transfer(), xhci_control_transfer(),
 *  xhci_clear_halt(). Implements:
 *
 *    BOT     Command Block Wrapper (31 bytes, 'USBC') -> optional data stage
 *            -> Command Status Wrapper (13 bytes, 'USBS'); CSW validation
 *            (signature, tag, residue, status); stall handling on every
 *            stage; Reset Recovery (class request 0xFF + CLEAR_FEATURE
 *            ENDPOINT_HALT on both pipes); Get Max LUN (0xFE).
 *    SCSI    INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10),
 *            READ CAPACITY(16) for > 2 TiB, READ(10), READ(16) for LBAs
 *            beyond 32 bits.
 *
 *  Every stage has a timeout; every failure is logged with its sense data
 *  and retried a bounded number of times. A device that is unplugged
 *  (or re-enumerated into a different slot generation) is detected and
 *  reported as MSC_ERR_NODEV instead of being retried forever.
 *
 *  Not thread safe per device: callers serialise access to one usb_msc_dev
 *  (the xHCI layer itself serialises the controller).
 * ============================================================================= */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "xhci.h"

#define MSC_OK              0
#define MSC_ERR_NODEV      -1      /* device gone (unplugged / re-enumerated)    */
#define MSC_ERR_TIMEOUT    -2      /* a transport stage timed out                */
#define MSC_ERR_IO         -3      /* transfer error after all retries           */
#define MSC_ERR_PROTO      -4      /* invalid CSW / phase error                  */
#define MSC_ERR_CHECK      -5      /* SCSI CHECK CONDITION (see sense fields)    */
#define MSC_ERR_NOMEDIUM   -6      /* NOT READY, medium not present              */
#define MSC_ERR_NOTREADY   -7      /* unit never became ready                    */
#define MSC_ERR_RANGE      -8      /* LBA outside the medium                     */
#define MSC_ERR_UNSUPPORTED -9     /* block size / device type not usable        */

/* SCSI sense keys used by the driver */
#define SENSE_NO_SENSE       0x0
#define SENSE_RECOVERED      0x1
#define SENSE_NOT_READY      0x2
#define SENSE_MEDIUM_ERROR   0x3
#define SENSE_HARDWARE_ERROR 0x4
#define SENSE_ILLEGAL_REQ    0x5
#define SENSE_UNIT_ATTENTION 0x6

struct usb_msc_platform {
    void     (*delay_us)(uint32_t us);
    uint64_t (*now_ms)(void);           /* monotonic ms; required for timeouts */
    void     (*log_putc)(char c);
};

struct usb_msc_dev {
    bool     present;
    uint8_t  slot, port, iface, lun, max_lun;
    uint32_t generation;                /* xHCI slot generation at probe time  */
    uint16_t vendor_id, product_id;

    /* INQUIRY */
    uint8_t  peripheral_type;           /* 0 = direct access block device      */
    bool     removable;
    char     vendor[9], product[17], revision[5];

    /* READ CAPACITY */
    uint64_t blocks;                    /* number of logical blocks            */
    uint32_t block_size;                /* bytes per block                     */
    bool     use_16;                    /* READ(16)/READ CAPACITY(16) needed   */

    /* transport state */
    uint32_t tag;
    uint32_t io_timeout_ms;             /* data stage of READ (default 10000)  */

    /* statistics and the most recent error, for diagnostics */
    uint32_t commands, reads, blocks_read, retries, resets, errors, timeouts;
    uint8_t  sense_key, asc, ascq;
    int      last_error;
    uint8_t  last_opcode;
};

void usb_msc_set_platform(const struct usb_msc_platform *plat);

/* Probe one LUN of an MSC interface reported by the xHCI driver:
 * Get Max LUN, INQUIRY, wait (up to ready_timeout_ms) for TEST UNIT READY
 * while decoding sense data, then READ CAPACITY(10/16). */
int  usb_msc_probe(const struct xhci_msc_info *info, uint8_t lun,
                   struct usb_msc_dev *out, uint32_t ready_timeout_ms);

/* Read `count` blocks at `lba` into `buf` (any memory; no alignment needs).
 * Splits into <= 64 KiB transfers, retries each up to 3 times with Reset
 * Recovery in between, and validates the residue of every transfer. */
int  usb_msc_read(struct usb_msc_dev *d, uint64_t lba, uint32_t count, void *buf);

/* Device still attached in the same enumeration generation? */
bool usb_msc_alive(const struct usb_msc_dev *d);

int  usb_msc_test_unit_ready(struct usb_msc_dev *d);
int  usb_msc_reset_recovery(struct usb_msc_dev *d);

const char *usb_msc_strerror(int err);
const char *scsi_sense_key_name(uint8_t key);

#endif
