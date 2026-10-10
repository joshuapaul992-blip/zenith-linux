/* kernel/bootvol.c -- boot volume discovery, root policy and failover
 * (see bootvol.h for the on-disk format and command line options) */
#include <kernel/bootvol.h>
#include <kernel/block.h>
#include <kernel/tarfs.h>
#include <kernel/recovery.h>
#include <kernel/usbhost.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/crc32.h>
#include <kernel/bootinfo.h>
#include <kernel/posix.h>

#define ROOTWAIT_DEFAULT    3000u
#define ROOTWAIT_MAX        60000u
#define CRC_CHUNK           (64u * 1024)

static struct bootvol_status st;
static uint8_t want_uuid[16];
static bool    have_want_uuid;

const struct bootvol_status *bootvol_status(void) { return &st; }

void bootvol_uuid_str(const uint8_t u[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* ---- command line ---------------------------------------------------------- */
static bool cmdline_get(const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *s = g_boot.cmdline; *s; s++)
        if ((s == g_boot.cmdline || s[-1] == ' ') && !strncmp(s, key, k) && s[k] == '=') {
            s += k + 1;
            size_t n = 0;
            while (s[n] && s[n] != ' ' && n + 1 < cap) { out[n] = s[n]; n++; }
            out[n] = 0;
            return true;
        }
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_uuid(const char *s, uint8_t out[16])
{
    int n = 0;
    for (; *s && n < 32; s++) {
        if (*s == '-') continue;
        int v = hexval(*s);
        if (v < 0) return false;
        if (n & 1) out[n / 2] = (uint8_t)(out[n / 2] | v); else out[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    return n == 32 && !*s;
}

static void fail(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.error, sizeof st.error, fmt, ap);
    va_end(ap);
    if (err) st.last_errno = err;
    kprintf("bootvol: %s\n", st.error);
}

/* ======================================================================== */
/*  one candidate: header -> payload CRC -> tarfs                              */
/* ======================================================================== */
static int verify_payload(struct blkdev *dev, uint64_t off, uint64_t len, uint32_t want)
{
    uint8_t *buf = kmalloc(CRC_CHUNK);
    if (!buf) return -ENOMEM;
    uint32_t crc = 0;
    uint64_t t0 = time_ms(), done = 0;
    int rc = 0;
    while (done < len) {
        size_t n = len - done < CRC_CHUNK ? (size_t)(len - done) : CRC_CHUNK;
        if ((rc = blk_read_bytes(dev, off + done, buf, n)) < 0) break;
        crc = crc32(crc, buf, n);
        done += n;
    }
    kfree(buf);
    if (rc < 0) {
        fail(rc, "%s: read error at byte %lu of the payload: %s", dev->name, done, blk_strerror(rc));
        return rc;
    }
    uint64_t ms = time_ms() - t0;
    if (crc != want) {
        fail(-EIO, "%s: payload CRC %08x does not match header %08x (corrupt volume)", dev->name, crc, want);
        return -EIO;
    }
    kprintf("bootvol: %s: payload CRC %08x ok (%lu KiB in %lu ms)\n", dev->name, crc, len >> 10, ms);
    return 0;
}

static int try_candidate(struct blkdev *dev, uint64_t hdr_block, const char *method)
{
    st.candidates++;
    struct kestrel_volhdr h;
    uint64_t hdr_off = hdr_block * dev->block_size;
    kprintf("bootvol: candidate %s block %lu (%s)\n", dev->name, hdr_block, method);

    int rc = blk_read_bytes(dev, hdr_off, &h, sizeof h);
    if (rc < 0) {
        fail(rc, "%s: cannot read the volume header at block %lu: %s", dev->name, hdr_block, blk_strerror(rc));
        return rc;
    }
    if (memcmp(h.magic, KESTREL_MAGIC, KESTREL_MAGIC_LEN)) {
        fail(0, "%s: block %lu has no KESTREL_BOOT header", dev->name, hdr_block);
        return -ENOENT;
    }
    uint32_t want = h.header_crc;
    h.header_crc = 0;
    uint32_t got = crc32(0, &h, sizeof h);
    h.header_crc = want;
    if (got != want) {
        fail(-EIO, "%s: volume header CRC %08x, expected %08x", dev->name, got, want);
        return -EIO;
    }
    if (h.version != KESTREL_VOL_VERSION || h.header_size != sizeof h) {
        fail(0, "%s: unsupported volume version %u (header %u bytes)", dev->name, h.version, h.header_size);
        return -ENOENT;
    }
    char label[33];
    memcpy(label, h.label, 32);
    label[32] = 0;
    char u[37];
    bootvol_uuid_str(h.uuid, u);
    if (have_want_uuid && memcmp(h.uuid, want_uuid, 16)) {
        bool gpt_match = false;
        if (dev->kind == BLK_PART && dev->gpt) {
            char g[37], w[37];
            blk_guid_str(dev->part_guid, g);
            bootvol_uuid_str(want_uuid, w);
            gpt_match = !strcmp(g, w);
        }
        if (!gpt_match) {
            kprintf("bootvol: %s: volume %s \"%s\" is not the requested %s, ignored\n", dev->name, u, label, st.want_uuid);
            return -ENOENT;
        }
    }
    uint64_t size = blk_size_bytes(dev);
    if ((h.payload_offset % 512) || h.payload_offset < sizeof h || !(h.flags & KESTREL_FLAG_USTAR) ||
        h.payload_bytes == 0 || hdr_off + h.payload_offset > size || h.payload_bytes > size - hdr_off - h.payload_offset) {
        fail(-EINVAL, "%s: volume %s: payload (offset %lu, %lu bytes, flags %x) does not fit the device (%lu bytes)",
             dev->name, u, h.payload_offset, h.payload_bytes, h.flags, size);
        return -EINVAL;
    }
    kprintf("bootvol: %s: volume \"%s\" uuid %s, %lu KiB ustar payload\n", dev->name, label, u, h.payload_bytes >> 10);

    uint64_t payload = hdr_off + h.payload_offset;
    if (st.verify && (rc = verify_payload(dev, payload, h.payload_bytes, h.payload_crc)) < 0) return rc;

    struct tarfs_stats ts;
    if ((rc = tarfs_mount(dev, payload, h.payload_bytes, "/boot", &ts)) < 0) {
        fail(rc, "%s: mounting the ustar payload failed: %s", dev->name, blk_strerror(rc));
        return rc;
    }
    st.result = BV_MOUNTED;
    vfs_bind("/boot/bin", "/bin");                      /* /bin/sh and friends where POSIX expects them */
    strlcpy(st.device, dev->name, sizeof st.device);
    st.method = method;
    memcpy(st.uuid, h.uuid, 16);
    strlcpy(st.label, label, sizeof st.label);
    st.header_block = hdr_block;
    st.payload_bytes = h.payload_bytes;
    st.files = ts.files;
    st.error[0] = 0;
    return 0;
}

/* Option A, then option B, on one whole disk. */
static int try_disk(struct blkdev *disk)
{
    st.disks_seen++;
    if (!disk->partitions_scanned) blk_scan_partitions(disk);
    int io_err = 0;

    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *p = blk_get(i);
        if (p->parent != disk || p->removed) continue;
        const char *method = NULL;
        if (p->gpt && !memcmp(p->type_guid, KESTREL_GPT_TYPE, 16)) method = "option A: GPT partition type KESTREL-BOOT-VOL";
        else if (!p->gpt && p->mbr_type == KESTREL_MBR_TYPE) method = "option A: MBR partition type 0x4b";
        if (!method) continue;
        int rc = try_candidate(p, 0, method);
        if (rc == 0) return 0;
        if (rc != -ENOENT && rc != -EINVAL) io_err = rc;
    }

    uint8_t *sec = kmalloc(disk->block_size);
    if (!sec) return -ENOMEM;
    int rc = blk_read(disk, 0, 1, sec);
    if (rc < 0) {
        fail(rc, "%s: block 0 unreadable: %s", disk->name, blk_strerror(rc));
        kfree(sec);
        return rc;
    }
    if (!memcmp(sec + KESTREL_SECTOR0_OFFSET, KESTREL_MAGIC, KESTREL_MAGIC_LEN)) {
        uint64_t hdr;
        memcpy(&hdr, sec + KESTREL_SECTOR0_OFFSET + KESTREL_MAGIC_LEN, 8);
        kfree(sec);
        if (hdr == 0 || hdr >= disk->blocks) {
            fail(-EINVAL, "%s: sector-0 signature points at block %lu, outside the disk", disk->name, hdr);
            return -EINVAL;
        }
        rc = try_candidate(disk, hdr, "option B: KESTREL_BOOT signature in sector 0");
        return rc == 0 ? 0 : (rc == -ENOENT || rc == -EINVAL) && io_err ? io_err : rc;
    }
    kfree(sec);
    return io_err ? io_err : -ENOENT;
}

/* ======================================================================== */
/*  the wait loop                                                              */
/* ======================================================================== */
static bool wanted(const struct blkdev *b)
{
    if (b->parent || b->removed) return false;
    if (!strcmp(st.policy, "usb")) return b->kind == BLK_DISK_USB;
    return true;                                    /* auto / any */
}

static int discover(void)
{
    bool mandatory = strcmp(st.policy, "auto") != 0;
    bool examined[BLK_MAX] = { false };
    uint64_t start = time_ms(), deadline = start + st.rootwait_ms, last_note = start;
    int io_err = 0;
    st.attempts++;
    st.disks_seen = st.candidates = 0;
    st.error[0] = 0;
    st.last_errno = 0;

    kprintf("bootvol: searching (kestrel.root=%s, rootwait %u ms%s%s, clock %s)\n", st.policy, st.rootwait_ms,
            have_want_uuid ? ", uuid " : "", have_want_uuid ? st.want_uuid : "", time_source());
    for (;;) {
        int settling = usb_settle();                /* non-blocking PORTSC pass */
        uint64_t now = time_ms();
        uint32_t left = now < deadline ? (uint32_t)(deadline - now) : 0;
        usb_storage_scan(left < 2000 ? (left > 500 ? left : 500) : 2000);

        for (int i = 0; i < blk_count() && i < BLK_MAX; i++) {
            struct blkdev *b = blk_get(i);
            if (examined[i] || !wanted(b)) continue;
            examined[i] = true;
            int rc = try_disk(b);
            if (rc == 0) {
                st.elapsed_ms = time_ms() - start;
                return 0;
            }
            if (rc != -ENOENT && rc != -EINVAL) io_err = rc;
            if (rc == -ENOENT && !st.error[0]) fail(0, "%s: no Kestrel boot volume on this disk", b->name);
        }

        now = time_ms();
        bool busy = settling != 0 || usb_storage_pending() > 0;
        if (now >= deadline) break;
        if (!mandatory && !busy) break;             /* auto: nothing more is coming */
        if (now - last_note >= 1000) {
            last_note = now;
            int usb = 0;
            for (int i = 0; i < blk_count(); i++) if (blk_get(i)->kind == BLK_DISK_USB && !blk_get(i)->removed) usb++;
            kprintf("bootvol: waiting for the boot volume... %lu.%lu / %u.%u s (%d USB disk(s)%s)\n",
                    (now - start) / 1000, (now - start) % 1000 / 100, st.rootwait_ms / 1000,
                    st.rootwait_ms % 1000 / 100, usb, busy ? ", USB ports settling" : "");
        }
        mdelay(20);
    }

    st.elapsed_ms = time_ms() - start;
    st.usb_disks = 0;
    for (int i = 0; i < blk_count(); i++) if (blk_get(i)->kind == BLK_DISK_USB && !blk_get(i)->removed) st.usb_disks++;
    if (io_err) {
        st.result = BV_IO_ERROR;
        return io_err;
    }
    st.result = BV_NOT_FOUND;
    if (!st.disks_seen && mandatory)
        fail(-ENODEV, "no %sstorage device appeared within %u ms%s", !strcmp(st.policy, "usb") ? "USB " : "",
             st.rootwait_ms, usb_present() ? "" : " (no working USB host controller)");
    else if (!st.disks_seen)
        fail(-ENODEV, "no storage device attached (searched for %lu ms)", st.elapsed_ms);
    else if (!st.candidates)
        fail(-ENOENT, "%u disk(s) examined, none carries a Kestrel boot volume (no 0x4b/GPT partition, "
             "no sector-0 signature)", st.disks_seen);
    return -ENOENT;
}

/* ======================================================================== */
/*  public                                                                     */
/* ======================================================================== */
static void after_mount(void)
{
    char u[37];
    bootvol_uuid_str(st.uuid, u);
    kprintf("bootvol: \033[92mboot volume \"%s\" (%s) mounted on /boot from /dev/%s\033[0m, %u files, found by %s in %lu ms\n",
            st.label, u, st.device, st.files, st.method, st.elapsed_ms);
    /* /boot/etc/motd replaces the built-in message of the day */
    struct file *f;
    if (vfs_open("/boot/etc/motd", O_RDONLY, 0, &f) == 0) {
        char *buf = kzalloc(4096);
        ssize_t n = buf ? vfs_read(f, buf, 4095) : -1;
        if (n > 0 && ramfs_write_file("/etc/motd", buf)) kprintf("bootvol: /etc/motd taken from the boot volume\n");
        else if (n < 0) kprintf("bootvol: /boot/etc/motd unreadable (%ld)\n", (long)n);
        kfree(buf);
        vfs_close(f);
    }
}

int bootvol_mount_root(void)
{
    char v[48];
    memset(&st, 0, sizeof st);
    strlcpy(st.policy, "auto", sizeof st.policy);
    if (cmdline_get("kestrel.root", v, sizeof v)) {
        if (!strcmp(v, "auto") || !strcmp(v, "usb") || !strcmp(v, "any") || !strcmp(v, "none"))
            strlcpy(st.policy, v, sizeof st.policy);
        else
            kprintf("bootvol: unknown kestrel.root=%s, using auto\n", v);
    }
    st.rootwait_ms = ROOTWAIT_DEFAULT;
    if (cmdline_get("kestrel.rootwait", v, sizeof v)) {
        long ms = strtol(v, NULL, 10);
        st.rootwait_ms = ms < 0 ? 0 : ms > (long)ROOTWAIT_MAX ? ROOTWAIT_MAX : (uint32_t)ms;
    }
    st.verify = !(cmdline_get("kestrel.rootverify", v, sizeof v) && v[0] == '0');
    if (cmdline_get("kestrel.uuid", v, sizeof v)) {
        have_want_uuid = parse_uuid(v, want_uuid);
        if (have_want_uuid) bootvol_uuid_str(want_uuid, st.want_uuid);
        else kprintf("bootvol: malformed kestrel.uuid=%s ignored\n", v);
    }
    if (!strcmp(st.policy, "none")) {
        st.result = BV_DISABLED;
        kprintf("bootvol: disabled (kestrel.root=none)\n");
        return -ENOENT;
    }

    for (;;) {
        int rc = discover();
        if (rc == 0) { after_mount(); return 0; }
        if (!strcmp(st.policy, "auto")) {
            kprintf("bootvol: no boot volume (%s); continuing without /boot\n", st.error[0] ? st.error : "none found");
            return rc;
        }
        /* mandatory volume missing or unreadable: never fall through silently */
        char reason[96];
        snprintf(reason, sizeof reason, st.result == BV_IO_ERROR ? "Boot volume read failure" :
                 "Boot volume not found");
        enum recovery_action a = recovery_enter(reason, st.error);
        if (a == RECOVERY_CONTINUE) {
            st.result = BV_CONTINUED;
            kprintf("bootvol: continuing without a boot volume (operator request)\n");
            return rc;
        }
        if (st.result == BV_MOUNTED) { after_mount(); return 0; }   /* 'boot <dev>' succeeded */
        kprintf("bootvol: retrying discovery (operator request)\n");
    }
}

int bootvol_try_device(const char *name)
{
    struct blkdev *b = blk_find(name);
    if (!b) return -ENODEV;
    st.disks_seen = st.candidates = 0;
    uint64_t t0 = time_ms();
    int rc = b->parent ? try_candidate(b, 0, "operator: partition") : try_disk(b);
    st.elapsed_ms = time_ms() - t0;
    return rc;
}

size_t bootvol_proc(char *buf, size_t cap)
{
    static const char *const res[] = { "not run", "disabled", "mounted", "not found", "I/O error", "continued without" };
    size_t n = 0;
#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))
    P("policy      kestrel.root=%s rootwait=%u ms verify=%s%s%s\n", st.policy[0] ? st.policy : "-", st.rootwait_ms,
      st.verify ? "yes" : "no", have_want_uuid ? " uuid=" : "", have_want_uuid ? st.want_uuid : "");
    P("result      %s after %lu ms (%u attempt(s), %u disk(s), %u candidate(s))\n", res[st.result],
      st.elapsed_ms, st.attempts, st.disks_seen, st.candidates);
    if (st.result == BV_MOUNTED) {
        char u[37];
        bootvol_uuid_str(st.uuid, u);
        P("volume      \"%s\" uuid %s\n", st.label, u);
        P("device      /dev/%s, header block %lu, %lu payload bytes, %u files on /boot\n", st.device,
          st.header_block, st.payload_bytes, st.files);
        P("found by    %s\n", st.method);
    }
    if (st.error[0]) P("last error  %s\n", st.error);
#undef P
    return n;
}
