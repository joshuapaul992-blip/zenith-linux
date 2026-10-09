/* =============================================================================
 *  usb_msc.c -- USB Mass Storage Bulk-Only Transport + SCSI block commands
 *  References: USB MSC Bulk-Only Transport 1.0 (sections 5, 6.6, 6.7);
 *              SPC-4 (INQUIRY, TEST UNIT READY, REQUEST SENSE);
 *              SBC-3 (READ CAPACITY 10/16, READ 10/16, WRITE 10/16,
 *              SYNCHRONIZE CACHE 10).
 * ============================================================================= */
#include "usb_msc.h"
#include <stdarg.h>

#define CBW_SIGNATURE   0x43425355u     /* "USBC" little endian */
#define CSW_SIGNATURE   0x53425355u     /* "USBS" little endian */
#define CBW_FLAG_IN     0x80

#define BOT_REQ_RESET       0xFF        /* Bulk-Only Mass Storage Reset */
#define BOT_REQ_MAX_LUN     0xFE

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_READ16           0x88
#define SCSI_WRITE10          0x2A
#define SCSI_WRITE16          0x8A
#define SCSI_SYNC_CACHE10     0x35
#define SCSI_SERVICE_ACTION16 0x9E
#define SAI_READ_CAPACITY16   0x10

#define CBW_TIMEOUT_MS      3000
#define CSW_TIMEOUT_MS      3000
#define SMALL_DATA_TIMEOUT  5000
#define READ_RETRIES        4

struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;             /* bit 7: direction (1 = device to host) */
    uint8_t  lun;               /* bits 3:0                              */
    uint8_t  cb_length;         /* 1..16                                 */
    uint8_t  cb[16];
} __attribute__((packed));

struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;            /* 0 passed, 1 failed, 2 phase error     */
} __attribute__((packed));

_Static_assert(sizeof(struct cbw) == 31, "CBW is 31 bytes");
_Static_assert(sizeof(struct csw) == 13, "CSW is 13 bytes");

static const struct usb_msc_platform *P;

void usb_msc_set_platform(const struct usb_msc_platform *plat) { P = plat; }

/* ---- small freestanding helpers ------------------------------------------- */
static void mzero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void mcopy(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    while (n--) *a++ = *b++;
}

static uint64_t now_ms(void) { return P && P->now_ms ? P->now_ms() : 0; }
static void sleep_ms(uint32_t ms) { if (P && P->delay_us) while (ms--) P->delay_us(1000); }

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

/* ---- logging: a printf subset (%s %c %d %u %x with l, width, 0, -) ------- */
static void lputc(char c) { if (P && P->log_putc) P->log_putc(c); }

static void lnum(uint64_t v, bool neg, unsigned base, int width, char pad)
{
    char tmp[24]; int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) tmp[n++] = '-';
    while (width-- > n) lputc(pad);
    while (n) lputc(tmp[--n]);
}

__attribute__((format(printf, 1, 2)))
static void msc_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { lputc(*f); continue; }
        f++;
        bool left = false; char pad = ' '; int width = 0, lng = 0;
        if (*f == '-') { left = true; f++; }
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; while (s[len]) len++;
            if (!left) for (int i = len; i < width; i++) lputc(' ');
            for (int i = 0; i < len; i++) lputc(s[i]);
            if (left) for (int i = len; i < width; i++) lputc(' ');
            break;
        }
        case 'c': lputc((char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = lng ? va_arg(ap, long) : va_arg(ap, int);
            lnum(v < 0 ? (uint64_t)-v : (uint64_t)v, v < 0, 10, width, pad);
            break;
        }
        case 'u': lnum(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), false, 10, width, pad); break;
        case 'x': lnum(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), false, 16, width, pad); break;
        case '%': lputc('%'); break;
        default:  lputc('%'); lputc(*f); if (!*f) f--; break;
        }
    }
    va_end(ap);
}

/* ---- names ------------------------------------------------------------------- */
const char *usb_msc_strerror(int err)
{
    switch (err) {
    case MSC_OK:              return "success";
    case MSC_ERR_NODEV:       return "device not present";
    case MSC_ERR_TIMEOUT:     return "transport timeout";
    case MSC_ERR_IO:          return "I/O error";
    case MSC_ERR_PROTO:       return "BOT protocol error";
    case MSC_ERR_CHECK:       return "SCSI check condition";
    case MSC_ERR_NOMEDIUM:    return "no medium";
    case MSC_ERR_NOTREADY:    return "unit not ready";
    case MSC_ERR_RANGE:       return "block out of range";
    case MSC_ERR_UNSUPPORTED: return "unsupported device";
    case MSC_ERR_READONLY:    return "write protected";
    default:                  return "unknown error";
    }
}

const char *scsi_sense_key_name(uint8_t key)
{
    static const char *const n[16] = {
        "NO SENSE", "RECOVERED ERROR", "NOT READY", "MEDIUM ERROR", "HARDWARE ERROR",
        "ILLEGAL REQUEST", "UNIT ATTENTION", "DATA PROTECT", "BLANK CHECK", "VENDOR SPECIFIC",
        "COPY ABORTED", "ABORTED COMMAND", "0xC", "VOLUME OVERFLOW", "MISCOMPARE", "0xF",
    };
    return n[key & 15];
}

static const char *opcode_name(uint8_t op)
{
    switch (op) {
    case SCSI_TEST_UNIT_READY:  return "TEST UNIT READY";
    case SCSI_REQUEST_SENSE:    return "REQUEST SENSE";
    case SCSI_INQUIRY:          return "INQUIRY";
    case SCSI_READ_CAPACITY10:  return "READ CAPACITY(10)";
    case SCSI_SERVICE_ACTION16: return "READ CAPACITY(16)";
    case SCSI_READ10:           return "READ(10)";
    case SCSI_READ16:           return "READ(16)";
    case SCSI_WRITE10:          return "WRITE(10)";
    case SCSI_WRITE16:          return "WRITE(16)";
    case SCSI_SYNC_CACHE10:     return "SYNCHRONIZE CACHE";
    default:                    return "SCSI command";
    }
}

static int map_xhci(int rc)
{
    switch (rc) {
    case XHCI_OK:          return MSC_OK;
    case XHCI_ERR_NODEV:   return MSC_ERR_NODEV;
    case XHCI_ERR_TIMEOUT: return MSC_ERR_TIMEOUT;
    case XHCI_ERR_BUSY:    return MSC_ERR_TIMEOUT;
    default:               return MSC_ERR_IO;
    }
}

/* ======================================================================== */
/*  Bulk-Only Transport                                                        */
/* ======================================================================== */
bool usb_msc_alive(const struct usb_msc_dev *d)
{
    struct xhci_msc_info i;
    return d && d->present && xhci_msc_lookup(d->slot, &i) == XHCI_OK && i.generation == d->generation;
}

/* Probe-time variant: `present` is not set yet. */
static bool attached(const struct usb_msc_dev *d)
{
    struct xhci_msc_info i;
    return xhci_msc_lookup(d->slot, &i) == XHCI_OK && i.generation == d->generation;
}

/* BOT 5.3.4: Bulk-Only Mass Storage Reset, then clear the halt on both
 * bulk pipes. Required after a phase error, an invalid CSW, or a stage
 * that failed in a way the stall handling cannot resynchronise. */
int usb_msc_reset_recovery(struct usb_msc_dev *d)
{
    d->resets++;
    msc_log("usb-msc: slot %u: reset recovery (#%u)\n", d->slot, d->resets);
    int rc = xhci_control_transfer(d->slot, 0x21 /* OUT | class | interface */, BOT_REQ_RESET,
                                   0, d->iface, 0, NULL);
    if (rc == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
    if (rc != XHCI_OK) msc_log("usb-msc: slot %u: mass storage reset failed (%d)\n", d->slot, rc);
    sleep_ms(5);
    int a = xhci_clear_halt(d->slot, true);
    int b = xhci_clear_halt(d->slot, false);
    if (a == XHCI_ERR_NODEV || b == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
    return (rc == XHCI_OK && a == XHCI_OK && b == XHCI_OK) ? MSC_OK : MSC_ERR_IO;
}

/* One complete CBW -> data -> CSW exchange. *moved receives the number of
 * data bytes actually transferred. Returns MSC_OK, MSC_ERR_CHECK (CSW
 * status 1) or a transport error (after reset recovery). */
static int bot_command(struct usb_msc_dev *d, const uint8_t *cdb, uint8_t cdb_len, bool in,
                       void *data, uint32_t len, uint32_t data_timeout_ms, uint32_t *moved)
{
    if (moved) *moved = 0;
    if (!attached(d)) return MSC_ERR_NODEV;
    d->commands++;
    d->last_opcode = cdb[0];

    struct cbw c;
    mzero(&c, sizeof c);
    c.signature = CBW_SIGNATURE;
    if (++d->tag == 0) d->tag = 1;
    c.tag = d->tag;
    c.data_length = len;
    c.flags = (len && in) ? CBW_FLAG_IN : 0;
    c.lun = d->lun & 0x0F;
    c.cb_length = cdb_len;
    mcopy(c.cb, cdb, cdb_len);

    /* -- command stage -- */
    uint32_t got = 0;
    int rc = xhci_bulk_transfer(d->slot, false, &c, sizeof c, &got, CBW_TIMEOUT_MS);
    if (rc != XHCI_OK || got != sizeof c) {
        msc_log("usb-msc: slot %u: %s: CBW stage failed (xhci %d, %u/31 bytes)\n",
                d->slot, opcode_name(cdb[0]), rc, got);
        if (rc == XHCI_ERR_TIMEOUT) d->timeouts++;
        if (rc == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
        int rr = usb_msc_reset_recovery(d);
        return rr == MSC_ERR_NODEV ? rr : rc == XHCI_OK ? MSC_ERR_IO : map_xhci(rc);
    }

    /* -- data stage -- */
    uint32_t xferred = 0;
    if (len) {
        rc = xhci_bulk_transfer(d->slot, in, data, len, &xferred, data_timeout_ms);
        if (rc == XHCI_ERR_STALL) {
            /* BOT 6.7.2/6.7.3: the device stalls the data pipe to end the
             * stage early; clear it and go on to read the CSW. */
            msc_log("usb-msc: slot %u: %s: data stage stalled after %u bytes, clearing halt\n",
                    d->slot, opcode_name(cdb[0]), xferred);
            if (xhci_clear_halt(d->slot, in) == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
        } else if (rc != XHCI_OK) {
            msc_log("usb-msc: slot %u: %s: data stage failed (xhci %d)\n", d->slot, opcode_name(cdb[0]), rc);
            if (rc == XHCI_ERR_TIMEOUT) d->timeouts++;
            if (rc == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
            int rr = usb_msc_reset_recovery(d);
            return rr == MSC_ERR_NODEV ? rr : map_xhci(rc);
        }
    }
    if (moved) *moved = xferred;

    /* -- status stage: one retry after a stall (BOT 6.7.2) -- */
    struct csw s;
    for (int attempt = 0; attempt < 2; attempt++) {
        mzero(&s, sizeof s);
        rc = xhci_bulk_transfer(d->slot, true, &s, sizeof s, &got, CSW_TIMEOUT_MS);
        if (rc != XHCI_ERR_STALL) break;
        msc_log("usb-msc: slot %u: CSW stalled, clearing halt and retrying\n", d->slot);
        if (xhci_clear_halt(d->slot, true) == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
    }
    if (rc != XHCI_OK) {
        msc_log("usb-msc: slot %u: %s: CSW stage failed (xhci %d)\n", d->slot, opcode_name(cdb[0]), rc);
        if (rc == XHCI_ERR_TIMEOUT) d->timeouts++;
        if (rc == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
        int rr = usb_msc_reset_recovery(d);
        return rr == MSC_ERR_NODEV ? rr : map_xhci(rc);
    }

    /* -- CSW validity (BOT 6.3) and meaning (6.7) -- */
    if (got != sizeof s || s.signature != CSW_SIGNATURE || s.tag != c.tag) {
        msc_log("usb-msc: slot %u: %s: invalid CSW (%u bytes, signature %08x, tag %08x, expected %08x)\n",
                d->slot, opcode_name(cdb[0]), got, s.signature, s.tag, c.tag);
        int rr = usb_msc_reset_recovery(d);
        return rr == MSC_ERR_NODEV ? rr : MSC_ERR_PROTO;
    }
    if (s.status == 2) {
        msc_log("usb-msc: slot %u: %s: phase error\n", d->slot, opcode_name(cdb[0]));
        int rr = usb_msc_reset_recovery(d);
        return rr == MSC_ERR_NODEV ? rr : MSC_ERR_PROTO;
    }
    if (s.status > 2 || s.residue > len) {
        msc_log("usb-msc: slot %u: %s: meaningless CSW (status %u, residue %u > %u)\n",
                d->slot, opcode_name(cdb[0]), s.status, s.residue, len);
        int rr = usb_msc_reset_recovery(d);
        return rr == MSC_ERR_NODEV ? rr : MSC_ERR_PROTO;
    }
    if (s.status == 1) return MSC_ERR_CHECK;
    return MSC_OK;
}

/* ======================================================================== */
/*  SCSI                                                                       */
/* ======================================================================== */
static int request_sense(struct usb_msc_dev *d)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t buf[18];
    mzero(buf, sizeof buf);
    uint32_t got = 0;
    int rc = bot_command(d, cdb, 6, true, buf, sizeof buf, SMALL_DATA_TIMEOUT, &got);
    if (rc != MSC_OK) return rc;
    uint8_t code = buf[0] & 0x7F;
    if ((code == 0x70 || code == 0x71) && got >= 14) {          /* fixed format */
        d->sense_key = buf[2] & 0x0F; d->asc = buf[12]; d->ascq = buf[13];
    } else if ((code == 0x72 || code == 0x73) && got >= 4) {   /* descriptor format */
        d->sense_key = buf[1] & 0x0F; d->asc = buf[2]; d->ascq = buf[3];
    } else {
        d->sense_key = SENSE_NO_SENSE; d->asc = d->ascq = 0;
        msc_log("usb-msc: slot %u: unrecognised sense data (response code %02x, %u bytes)\n", d->slot, code, got);
    }
    return MSC_OK;
}

/* Run one SCSI command; on CHECK CONDITION fetch and log the sense data.
 * `out` selects a data-out (host to device) stage, e.g. for WRITE. */
static int scsi_dir(struct usb_msc_dev *d, const uint8_t *cdb, uint8_t cdb_len, bool out, void *buf,
                    uint32_t len, uint32_t timeout_ms, uint32_t *moved, bool quiet)
{
    int rc = bot_command(d, cdb, cdb_len, !out, buf, len, timeout_ms, moved);
    if (rc != MSC_ERR_CHECK) return rc;
    int sr = request_sense(d);
    if (sr != MSC_OK) {
        msc_log("usb-msc: slot %u: %s failed and REQUEST SENSE failed too (%s)\n",
                d->slot, opcode_name(cdb[0]), usb_msc_strerror(sr));
        d->sense_key = SENSE_NO_SENSE; d->asc = d->ascq = 0;
        return sr == MSC_ERR_NODEV ? sr : MSC_ERR_CHECK;
    }
    if (!quiet || (d->sense_key != SENSE_UNIT_ATTENTION && d->sense_key != SENSE_NOT_READY))
        msc_log("usb-msc: slot %u: %s: CHECK CONDITION, sense %s (ASC %02x ASCQ %02x)\n", d->slot,
                opcode_name(cdb[0]), scsi_sense_key_name(d->sense_key), d->asc, d->ascq);
    return MSC_ERR_CHECK;
}

static int scsi(struct usb_msc_dev *d, const uint8_t *cdb, uint8_t cdb_len, void *buf, uint32_t len,
                uint32_t timeout_ms, uint32_t *moved, bool quiet)
{
    return scsi_dir(d, cdb, cdb_len, false, buf, len, timeout_ms, moved, quiet);
}

int usb_msc_test_unit_ready(struct usb_msc_dev *d)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return scsi(d, cdb, 6, NULL, 0, SMALL_DATA_TIMEOUT, NULL, true);
}

static void copy_trim(char *dst, const uint8_t *src, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    for (size_t i = 0; i < n; i++) if (dst[i] != ' ') len = i + 1;
    dst[len] = 0;
}

static int inquiry(struct usb_msc_dev *d)
{
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t buf[36];
    int rc = MSC_ERR_IO;
    uint32_t got = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        mzero(buf, sizeof buf);
        rc = scsi(d, cdb, 6, buf, sizeof buf, SMALL_DATA_TIMEOUT, &got, false);
        if (rc == MSC_OK || rc == MSC_ERR_NODEV) break;
        sleep_ms(50);
    }
    if (rc != MSC_OK) return rc;
    if (got < 5) { msc_log("usb-msc: slot %u: INQUIRY returned only %u bytes\n", d->slot, got); return MSC_ERR_PROTO; }
    if ((buf[0] >> 5) == 3) {
        msc_log("usb-msc: slot %u: LUN %u not supported by the device\n", d->slot, d->lun);
        return MSC_ERR_NODEV;
    }
    d->peripheral_type = buf[0] & 0x1F;
    d->removable = (buf[1] & 0x80) != 0;
    copy_trim(d->vendor, buf + 8, 8);
    copy_trim(d->product, buf + 16, 16);
    copy_trim(d->revision, buf + 32, 4);
    return MSC_OK;
}

/* Wait for the unit to report ready. Sense data steers the wait:
 *   UNIT ATTENTION (power-on reset, medium changed): retry at once
 *   NOT READY 04/xx (becoming ready, initialising):   keep waiting
 *   NOT READY 3A/xx (medium not present):             keep waiting, then NOMEDIUM */
static int wait_ready(struct usb_msc_dev *d, uint32_t timeout_ms)
{
    uint64_t start = now_ms(), end = start + timeout_ms;
    int rc = MSC_ERR_NOTREADY, unit_attentions = 0;
    bool told = false;
    for (;;) {
        rc = usb_msc_test_unit_ready(d);
        if (rc == MSC_OK) {
            if (told) msc_log("usb-msc: slot %u: unit ready after %u ms\n", d->slot, (unsigned)(now_ms() - start));
            return MSC_OK;
        }
        if (rc == MSC_ERR_NODEV) return rc;
        if (rc == MSC_ERR_CHECK && d->sense_key == SENSE_UNIT_ATTENTION && ++unit_attentions < 8) continue;
        if (rc == MSC_ERR_CHECK && d->sense_key == SENSE_NOT_READY) {
            rc = d->asc == 0x3A ? MSC_ERR_NOMEDIUM : MSC_ERR_NOTREADY;
            if (!told) {
                msc_log("usb-msc: slot %u: %s (ASC %02x/%02x), waiting up to %u ms\n", d->slot,
                        d->asc == 0x3A ? "no medium" : "unit becoming ready", d->asc, d->ascq, timeout_ms);
                told = true;
            }
        }
        if (now_ms() >= end) {
            msc_log("usb-msc: slot %u: not ready after %u ms: %s\n", d->slot, timeout_ms, usb_msc_strerror(rc));
            return rc == MSC_ERR_CHECK ? MSC_ERR_NOTREADY : rc;
        }
        sleep_ms(100);
    }
}

static bool valid_block_size(uint32_t bs) { return bs >= 512 && bs <= 4096 && !(bs & (bs - 1)); }

static int read_capacity(struct usb_msc_dev *d)
{
    int rc = MSC_ERR_IO;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint8_t cdb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t buf[8];
        uint32_t got = 0;
        mzero(buf, sizeof buf);
        rc = scsi(d, cdb, 10, buf, 8, SMALL_DATA_TIMEOUT, &got, true);
        if (rc == MSC_ERR_NODEV) return rc;
        if (rc != MSC_OK) { sleep_ms(50); continue; }
        if (got < 8) { rc = MSC_ERR_PROTO; continue; }
        uint32_t last = be32(buf);
        d->block_size = be32(buf + 4);
        d->blocks = (uint64_t)last + 1;
        d->use_16 = false;
        if (last == 0xFFFFFFFFu) {              /* > 2 TiB: READ CAPACITY(16) */
            uint8_t c16[16];
            uint8_t b16[32];
            mzero(c16, sizeof c16);
            mzero(b16, sizeof b16);
            c16[0] = SCSI_SERVICE_ACTION16;
            c16[1] = SAI_READ_CAPACITY16;
            put_be32(c16 + 10, sizeof b16);
            rc = scsi(d, c16, 16, b16, sizeof b16, SMALL_DATA_TIMEOUT, &got, true);
            if (rc != MSC_OK || got < 12) { if (rc == MSC_ERR_NODEV) return rc; rc = MSC_ERR_IO; continue; }
            d->blocks = be64(b16) + 1;
            d->block_size = be32(b16 + 8);
            d->use_16 = true;
        }
        return MSC_OK;
    }
    return rc;
}

int usb_msc_probe(const struct xhci_msc_info *info, uint8_t lun, struct usb_msc_dev *out,
                  uint32_t ready_timeout_ms)
{
    struct usb_msc_dev *d = out;
    mzero(d, sizeof *d);
    d->slot = info->slot;
    d->port = info->port;
    d->iface = info->iface;
    d->lun = lun;
    d->generation = info->generation;
    d->io_timeout_ms = 10000;
    d->tag = 0x4B530000u;                   /* "KS": recognisable in USB traces */
    if (info->dev) { d->vendor_id = info->dev->vendor; d->product_id = info->dev->product; }

    msc_log("usb-msc: slot %u port %u: probing LUN %u (interface %u, subclass %02x, protocol %02x, "
            "bulk IN 0x%02x/%u OUT 0x%02x/%u)\n", d->slot, d->port, lun, info->iface, info->subclass,
            info->protocol, info->in_ep, info->in_mps, info->out_ep, info->out_mps);
    if (info->protocol != 0x50) {
        msc_log("usb-msc: slot %u: protocol %02x is not Bulk-Only, skipped\n", d->slot, info->protocol);
        return MSC_ERR_UNSUPPORTED;
    }
    if (info->subclass != 0x06)
        msc_log("usb-msc: slot %u: subclass %02x is not SCSI transparent; trying SCSI anyway\n",
                d->slot, info->subclass);

    /* Get Max LUN; a stall means "only LUN 0" (BOT 3.2) */
    uint8_t maxlun = 0;
    int rc = xhci_control_transfer(d->slot, 0xA1 /* IN | class | interface */, BOT_REQ_MAX_LUN,
                                   0, info->iface, 1, &maxlun);
    if (rc == XHCI_ERR_NODEV) return MSC_ERR_NODEV;
    if (rc != XHCI_OK || maxlun > 15) maxlun = 0;
    d->max_lun = maxlun;
    if (lun > maxlun) return MSC_ERR_NODEV;

    if ((rc = inquiry(d)) != MSC_OK) { d->last_error = rc; return rc; }
    msc_log("usb-msc: slot %u lun %u: \"%s\" \"%s\" rev \"%s\", type %02x%s, max LUN %u\n", d->slot, lun,
            d->vendor, d->product, d->revision, d->peripheral_type, d->removable ? ", removable" : "", maxlun);
    if (d->peripheral_type != 0x00 && d->peripheral_type != 0x05 && d->peripheral_type != 0x07 &&
        d->peripheral_type != 0x0E) {
        msc_log("usb-msc: slot %u: peripheral type %02x is not a block device\n", d->slot, d->peripheral_type);
        return MSC_ERR_UNSUPPORTED;
    }

    if ((rc = wait_ready(d, ready_timeout_ms)) != MSC_OK) { d->last_error = rc; return rc; }
    if ((rc = read_capacity(d)) != MSC_OK) {
        msc_log("usb-msc: slot %u: READ CAPACITY failed: %s\n", d->slot, usb_msc_strerror(rc));
        d->last_error = rc;
        return rc;
    }
    if (!valid_block_size(d->block_size) || d->blocks == 0) {
        msc_log("usb-msc: slot %u: unusable geometry: %u blocks of %u bytes\n", d->slot,
                (unsigned)d->blocks, d->block_size);
        return MSC_ERR_UNSUPPORTED;
    }
    uint64_t mib = (d->blocks * d->block_size) >> 20;
    msc_log("usb-msc: slot %u lun %u: %lu blocks x %u bytes = %lu MiB%s\n", d->slot, lun,
            (unsigned long)d->blocks, d->block_size, (unsigned long)mib, d->use_16 ? " (16-byte CDBs)" : "");
    d->present = true;
    return MSC_OK;
}

/* ======================================================================== */
/*  READ(10) / READ(16)                                                        */
/* ======================================================================== */
static int read_chunk(struct usb_msc_dev *d, uint64_t lba, uint32_t n, uint8_t *buf)
{
    uint8_t cdb[16];
    mzero(cdb, sizeof cdb);
    uint8_t len;
    if (d->use_16 || lba + n - 1 > 0xFFFFFFFFull) {
        cdb[0] = SCSI_READ16;
        put_be64(cdb + 2, lba);
        put_be32(cdb + 10, n);
        len = 16;
    } else {
        cdb[0] = SCSI_READ10;
        put_be32(cdb + 2, (uint32_t)lba);
        put_be16(cdb + 7, (uint16_t)n);
        len = 10;
    }
    uint32_t want = n * d->block_size, got = 0;
    int rc = scsi(d, cdb, len, buf, want, d->io_timeout_ms, &got, false);
    if (rc == MSC_OK && got != want) {
        msc_log("usb-msc: slot %u: short read at LBA %lu: %u of %u bytes\n", d->slot,
                (unsigned long)lba, got, want);
        rc = MSC_ERR_IO;
    }
    return rc;
}

int usb_msc_read(struct usb_msc_dev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (!d || !d->present) return MSC_ERR_NODEV;
    if (count == 0) return MSC_OK;
    if (lba >= d->blocks || count > d->blocks - lba) {
        msc_log("usb-msc: slot %u: read of %u blocks at LBA %lu beyond the medium (%lu blocks)\n",
                d->slot, count, (unsigned long)lba, (unsigned long)d->blocks);
        return MSC_ERR_RANGE;
    }
    uint32_t per = XHCI_BULK_MAX / d->block_size;
    uint8_t *out = buf;
    d->reads++;

    while (count) {
        uint32_t n = count < per ? count : per;
        int rc = MSC_ERR_IO;
        for (int attempt = 0; attempt < READ_RETRIES; attempt++) {
            if (attempt) {
                d->retries++;
                msc_log("usb-msc: slot %u: retrying READ at LBA %lu (attempt %d/%d)\n", d->slot,
                        (unsigned long)lba, attempt + 1, READ_RETRIES);
                sleep_ms(20u * (uint32_t)attempt);
            }
            rc = read_chunk(d, lba, n, out);
            if (rc == MSC_OK || rc == MSC_ERR_NODEV) break;
            if (!usb_msc_alive(d)) { rc = MSC_ERR_NODEV; break; }
            if (rc == MSC_ERR_CHECK) {
                if (d->sense_key == SENSE_NOT_READY && d->asc == 0x3A) { rc = MSC_ERR_NOMEDIUM; break; }
                if (d->sense_key == SENSE_ILLEGAL_REQ) break;   /* retrying cannot help */
            }
        }
        if (rc != MSC_OK) {
            d->errors++;
            d->last_error = rc;
            if (rc == MSC_ERR_NODEV) d->present = false;
            msc_log("usb-msc: slot %u: READ of %u blocks at LBA %lu failed: %s\n", d->slot, n,
                    (unsigned long)lba, usb_msc_strerror(rc));
            return rc;
        }
        d->blocks_read += n;
        lba += n;
        count -= n;
        out += (size_t)n * d->block_size;
    }
    return MSC_OK;
}

/* ======================================================================== */
/*  WRITE(10) / WRITE(16), SYNCHRONIZE CACHE                                   */
/* ======================================================================== */
static int write_chunk(struct usb_msc_dev *d, uint64_t lba, uint32_t n, const uint8_t *buf)
{
    uint8_t cdb[16];
    mzero(cdb, sizeof cdb);
    uint8_t len;
    if (d->use_16 || lba + n - 1 > 0xFFFFFFFFull) {
        cdb[0] = SCSI_WRITE16;
        put_be64(cdb + 2, lba);
        put_be32(cdb + 10, n);
        len = 16;
    } else {
        cdb[0] = SCSI_WRITE10;
        put_be32(cdb + 2, (uint32_t)lba);
        put_be16(cdb + 7, (uint16_t)n);
        len = 10;
    }
    uint32_t want = n * d->block_size, got = 0;
    /* The xHCI driver copies OUT data into its bounce buffer, never writes `buf`. */
    int rc = scsi_dir(d, cdb, len, true, (void *)buf, want, d->io_timeout_ms, &got, false);
    if (rc == MSC_OK && got != want) {
        msc_log("usb-msc: slot %u: short write at LBA %lu: %u of %u bytes\n", d->slot,
                (unsigned long)lba, got, want);
        rc = MSC_ERR_IO;
    }
    return rc;
}

int usb_msc_write(struct usb_msc_dev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (!d || !d->present) return MSC_ERR_NODEV;
    if (count == 0) return MSC_OK;
    if (lba >= d->blocks || count > d->blocks - lba) {
        msc_log("usb-msc: slot %u: write of %u blocks at LBA %lu beyond the medium (%lu blocks)\n",
                d->slot, count, (unsigned long)lba, (unsigned long)d->blocks);
        return MSC_ERR_RANGE;
    }
    uint32_t per = XHCI_BULK_MAX / d->block_size;
    const uint8_t *in = buf;
    d->writes++;

    while (count) {
        uint32_t n = count < per ? count : per;
        int rc = MSC_ERR_IO;
        for (int attempt = 0; attempt < READ_RETRIES; attempt++) {
            if (attempt) {
                d->retries++;
                msc_log("usb-msc: slot %u: retrying WRITE at LBA %lu (attempt %d/%d)\n", d->slot,
                        (unsigned long)lba, attempt + 1, READ_RETRIES);
                sleep_ms(20u * (uint32_t)attempt);
            }
            rc = write_chunk(d, lba, n, in);
            if (rc == MSC_OK || rc == MSC_ERR_NODEV) break;
            if (!usb_msc_alive(d)) { rc = MSC_ERR_NODEV; break; }
            if (rc == MSC_ERR_CHECK) {
                if (d->sense_key == SENSE_NOT_READY && d->asc == 0x3A) { rc = MSC_ERR_NOMEDIUM; break; }
                if (d->sense_key == SENSE_ILLEGAL_REQ) break;
                if (d->sense_key == SENSE_DATA_PROTECT) { rc = MSC_ERR_READONLY; break; }
            }
        }
        if (rc != MSC_OK) {
            d->errors++;
            d->last_error = rc;
            if (rc == MSC_ERR_NODEV) d->present = false;
            msc_log("usb-msc: slot %u: WRITE of %u blocks at LBA %lu failed: %s\n", d->slot, n,
                    (unsigned long)lba, usb_msc_strerror(rc));
            return rc;
        }
        d->blocks_written += n;
        lba += n;
        count -= n;
        in += (size_t)n * d->block_size;
    }
    return MSC_OK;
}

/* SYNCHRONIZE CACHE(10) over the whole medium. Sticks without a write cache
 * may reject it with ILLEGAL REQUEST; that is not an error. */
int usb_msc_sync(struct usb_msc_dev *d)
{
    if (!d || !d->present) return MSC_ERR_NODEV;
    uint8_t cdb[10];
    mzero(cdb, sizeof cdb);
    cdb[0] = SCSI_SYNC_CACHE10;
    int rc = scsi(d, cdb, 10, NULL, 0, d->io_timeout_ms, NULL, true);
    if (rc == MSC_ERR_CHECK && d->sense_key == SENSE_ILLEGAL_REQ) return MSC_OK;
    return rc;
}
