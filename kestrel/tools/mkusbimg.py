#!/usr/bin/env python3
"""Build a bootable Kestrel USB stick image (raw disk).

Layout (MBR, the default):
  LBA 0       GRUB boot.img; bytes 3..22 carry the option-B marker
              "KESTREL_BOOT" + u64 LBA of the volume header
  LBA 1..     GRUB core.img (kernel + grub.cfg inside its memdisk)
  LBA 2048..  partition type 0x4B (option A): Kestrel volume header,
              then the ustar payload built from --rootfs
  then        optional EFI System Partition (type 0xEF, from --esp-img) with
              EFI/BOOT/BOOTX64.EFI, so the same stick boots on UEFI machines
  after it    FAT16 partition "KESTREL RPT" (MBR type 0x0E, first table entry
              so that every OS mounts it): README.TXT, plus REPORT.TXT and
              VBIOS.ROM, which the kernel overwrites in place with its
              diagnostic report and the graphics card's video BIOS

GPT layout (--layout gpt): protective MBR + GPT; partition 1 is a BIOS boot
partition holding core.img, partition 2 has the Kestrel type GUID
("KESTREL-BOOT-VOL" as raw bytes) and a unique partition GUID equal to the
volume UUID, partition 3 is the report volume (Microsoft basic data).

--no-report leaves out the report volume. Test knobs: --no-marker (option A only), --plain-type (partition type 0x83
so that only option B finds it), --no-grub (data-only stick),
--corrupt-payload (flip one payload byte after the CRC is computed).
"""
import argparse, io, os, struct, tarfile, time, uuid, zlib

SECTOR = 512
MAGIC = b"KESTREL_BOOT"
GPT_TYPE = b"KESTREL-BOOT-VOL"
BIOS_BOOT_GUID = uuid.UUID("21686148-6449-6e6f-744e-656564454649")
PAYLOAD_OFFSET = 4096
BASIC_DATA_GUID = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
ESP_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")

# Report volume: the kernel finds it by this exact FAT volume label and only
# ever overwrites the data clusters of REPORT.TXT and VBIOS.ROM.
REPORT_LABEL = b"KESTREL RPT"
REPORT_MIB = 24
REPORT_FILES = [  # (8.3 name, size, initial contents)
    (b"README  TXT", None, None),
    (b"REPORT  TXT", 2 * 1024 * 1024, None),
    (b"VBIOS   ROM", 1024 * 1024, b"\0"),
]
README = b"""Kestrel report volume\r
=====================\r
\r
Kestrel writes two files here every time it boots from this stick:\r
\r
  REPORT.TXT  kernel version, command line, graphics card report (NVIDIA:\r
              chip, video BIOS tables, DisplayPort outputs, monitors and\r
              their EDID) and the complete kernel log\r
  VBIOS.ROM   the graphics card's video BIOS image (NVIDIA cards)\r
\r
Both files keep their size; unused space is padded. Copy them off the\r
stick to share them. The shell command 'report' writes them again with the\r
log up to that moment; boot with kestrel.report=0 to stop automatic writes.\r
"""


def fat16_volume(sectors, hidden):
    """A FAT16 file system of `sectors` sectors holding REPORT_FILES, each
    allocated contiguously."""
    spc, reserved, nfats, root_entries = 4, 4, 2, 512
    root_secs = root_entries * 32 // SECTOR
    fat_secs = 1
    while True:
        data = sectors - reserved - nfats * fat_secs - root_secs
        clusters = data // spc
        need = (clusters + 2) * 2 // SECTOR + 1
        if need <= fat_secs:
            break
        fat_secs = need
    assert 4085 <= clusters < 65525, "not a FAT16 cluster count"
    vol = bytearray(sectors * SECTOR)
    bpb = struct.pack("<3s8sHBHBHHBHHHII", b"\xeb\x3c\x90", b"KESTREL ", SECTOR, spc, reserved,
                      nfats, root_entries, sectors if sectors < 65536 else 0, 0xF8, fat_secs,
                      63, 255, hidden, sectors if sectors >= 65536 else 0)
    bpb += struct.pack("<BBBI11s8s", 0x80, 0, 0x29, 0x4B524550, REPORT_LABEL, b"FAT16   ")
    vol[0:len(bpb)] = bpb
    vol[510:512] = b"\x55\xaa"

    fat = bytearray(fat_secs * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(root_secs * SECTOR)
    t = time.localtime()
    fdate = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    ftime = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)

    def dirent(name, attr, clus, size):     # 32 bytes, FAT spec section 6
        return struct.pack("<11sBBBHHHHHHHI", name, attr, 0, 0, ftime, fdate, fdate, 0,
                           ftime, fdate, clus, size)
    root[0:32] = dirent(REPORT_LABEL, 0x08, 0, 0)
    data_lba = reserved + nfats * fat_secs + root_secs
    cluster = 2
    for i, (name, size, fill) in enumerate(REPORT_FILES, start=1):
        if size is None:
            body = README
        else:
            body = (b"Kestrel has not written a report to this stick yet.\r\n" if fill is None else b"")
            body = body.ljust(size, b" " if fill is None else fill)
        n = (len(body) + spc * SECTOR - 1) // (spc * SECTOR)
        for c in range(cluster, cluster + n):
            struct.pack_into("<H", fat, c * 2, c + 1 if c < cluster + n - 1 else 0xFFFF)
        off = (data_lba + (cluster - 2) * spc) * SECTOR
        vol[off:off + len(body)] = body
        root[i * 32:(i + 1) * 32] = dirent(name, 0x20, cluster, len(body))
        cluster += n
    for k in range(nfats):
        off = (reserved + k * fat_secs) * SECTOR
        vol[off:off + len(fat)] = fat
    off = (reserved + nfats * fat_secs) * SECTOR
    vol[off:off + len(root)] = root
    return vol


def crc32(b, crc=0):
    return zlib.crc32(b, crc) & 0xFFFFFFFF


def ustar_from_dir(root):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            rel = os.path.relpath(dirpath, root)
            if rel != ".":
                ti = tar.gettarinfo(dirpath, arcname=rel)
                ti.uid = ti.gid = 0; ti.uname = ti.gname = "root"; ti.mode = 0o755
                tar.addfile(ti)
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                arc = os.path.normpath(os.path.join(rel, name))
                ti = tar.gettarinfo(path, arcname=arc)
                ti.uid = ti.gid = 0; ti.uname = ti.gname = "root"
                ti.mode = 0o555 if os.stat(path).st_mode & 0o111 else 0o444   # keep "executable"
                with open(path, "rb") as f:
                    tar.addfile(ti, f)
    data = buf.getvalue()
    return data + b"\0" * (-len(data) % SECTOR)


def volume_header(vol_uuid, label, payload):
    hdr = struct.pack("<12sHHI16s32sQQIIQ", MAGIC, 1, 512, 0, vol_uuid.bytes,
                      label.encode()[:32].ljust(32, b"\0"), PAYLOAD_OFFSET, len(payload),
                      crc32(payload), 1, int(time.time()))
    hdr = hdr.ljust(512, b"\0")
    c = crc32(hdr)
    return hdr[:16] + struct.pack("<I", c) + hdr[20:]


def mbr_entry(boot, ptype, start, count):
    return struct.pack("<B3sB3sII", boot, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, count)


def gpt_guid_bytes(u):
    return u.bytes_le


def write_gpt(img, total, parts, disk_guid):
    """parts: list of (type_guid_bytes, unique_guid_bytes, first, last, name)"""
    entries = bytearray(128 * 128)
    for i, (t, g, first, last, name) in enumerate(parts):
        entries[i * 128:(i + 1) * 128] = struct.pack("<16s16sQQQ72s", t, g, first, last, 0,
                                                     name.encode("utf-16-le")[:72].ljust(72, b"\0"))
    ecrc = crc32(bytes(entries))

    def header(my, alt, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0, my, alt, 34,
                        total - 34, disk_guid, entries_lba, 128, 128, ecrc)
        h = h[:16] + struct.pack("<I", crc32(h)) + h[20:]
        return h.ljust(SECTOR, b"\0")

    img[1 * SECTOR:2 * SECTOR] = header(1, total - 1, 2)
    img[2 * SECTOR:34 * SECTOR] = entries
    img[(total - 33) * SECTOR:(total - 1) * SECTOR] = entries
    img[(total - 1) * SECTOR:total * SECTOR] = header(total - 1, 1, total - 33)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--rootfs", required=True, help="directory packed into the ustar payload")
    ap.add_argument("--boot-img", help="GRUB i386-pc boot.img")
    ap.add_argument("--core-img", help="GRUB i386-pc core.img (with memdisk)")
    ap.add_argument("--size", type=int, default=64, help="image size in MiB (default 64)")
    ap.add_argument("--layout", choices=["mbr", "gpt"], default="mbr")
    ap.add_argument("--uuid", default=None, help="volume UUID (default: random)")
    ap.add_argument("--label", default="KESTREL")
    ap.add_argument("--no-grub", action="store_true")
    ap.add_argument("--no-marker", action="store_true", help="omit the sector-0 marker (option A only)")
    ap.add_argument("--plain-type", action="store_true", help="partition type 0x83 / Linux data (option B only)")
    ap.add_argument("--corrupt-payload", action="store_true")
    ap.add_argument("--no-report", action="store_true", help="no FAT report volume")
    ap.add_argument("--esp-img", help="FAT image placed in an EFI System Partition (UEFI boot)")
    ap.add_argument("--ramdisk", action="store_true",
                    help="write only a small disk image holding the boot volume (sector-0 marker, header,"
                         " payload), for loading as a Multiboot2 module (the ISO)")
    a = ap.parse_args()

    total = a.size * 1024 * 1024 // SECTOR
    img = bytearray(total * SECTOR)
    vol_uuid = uuid.UUID(a.uuid) if a.uuid else uuid.uuid4()
    payload = ustar_from_dir(a.rootfs)
    header = volume_header(vol_uuid, a.label, payload)

    if a.ramdisk:               # sector 0: option-B marker -> header at block 1
        sec0 = bytearray(SECTOR)
        sec0[3:15] = MAGIC
        sec0[15:23] = struct.pack("<Q", 1)
        with open(a.out, "wb") as f:
            f.write(bytes(sec0) + header + b"\0" * (PAYLOAD_OFFSET - len(header)) + payload)
        print(f"{a.out}: boot volume ramdisk, payload {len(payload)} bytes crc {crc32(payload):08x}, uuid {vol_uuid}")
        return

    core = b""
    if not a.no_grub:
        boot = open(a.boot_img, "rb").read()
        core = open(a.core_img, "rb").read()
        core += b"\0" * (-len(core) % SECTOR)
        assert len(boot) == SECTOR
        img[0:440] = boot[0:440]                  # code only; table + signature are ours
    core_sectors = len(core) // SECTOR

    if a.layout == "mbr":
        part_start = max(2048, ((1 + core_sectors + 2047) // 2048) * 2048)
        core_lba = 1
    else:
        core_lba = 34
        part_start = max(2048, ((core_lba + core_sectors + 2047) // 2048) * 2048)
    end = total - (34 if a.layout == "gpt" else 0)             # first sector past the usable area
    report_sectors = 0 if a.no_report else REPORT_MIB * 1024 * 1024 // SECTOR
    report_start = end - report_sectors
    report_start -= report_start % 2048                         # 1 MiB aligned
    esp = open(a.esp_img, "rb").read() if a.esp_img else b""
    esp_sectors = (len(esp) + SECTOR - 1) // SECTOR
    esp_start = report_start - esp_sectors
    esp_start -= esp_start % 2048
    part_count = (esp_start if esp else report_start if report_sectors else end) - part_start
    need = (PAYLOAD_OFFSET + len(payload)) // SECTOR
    if need > part_count:
        raise SystemExit(f"payload ({len(payload)} bytes) does not fit; use a larger --size")
    if report_sectors:
        img[report_start * SECTOR:(report_start + report_sectors) * SECTOR] = fat16_volume(report_sectors, report_start)
    if esp:
        img[esp_start * SECTOR:esp_start * SECTOR + len(esp)] = esp

    if core:
        img[core_lba * SECTOR:core_lba * SECTOR + len(core)] = core
        if core_lba != 1:
            # grub-setup's job: kernel_sector in boot.img, first blocklist in diskboot
            img[0x5C:0x64] = struct.pack("<Q", core_lba)
            off = core_lba * SECTOR + 0x1F4
            img[off:off + 8] = struct.pack("<Q", core_lba + 1)

    hdr_off = part_start * SECTOR
    img[hdr_off:hdr_off + SECTOR] = header
    p = hdr_off + PAYLOAD_OFFSET
    img[p:p + len(payload)] = payload
    if a.corrupt_payload:
        img[p + len(payload) // 2] ^= 0xFF

    if not a.no_marker:
        img[3:15] = MAGIC
        img[15:23] = struct.pack("<Q", part_start)

    if a.layout == "mbr":
        ptype = 0x83 if a.plain_type else 0x4B
        entries = [mbr_entry(0x80, ptype, part_start, part_count)]
        if esp:
            entries.insert(0, mbr_entry(0x00, 0xEF, esp_start, esp_sectors))
        if report_sectors:      # first entry: older Windows mounts only that one on removable media
            entries.insert(0, mbr_entry(0x00, 0x0E, report_start, report_sectors))
        for i, e in enumerate(entries):
            img[446 + 16 * i:462 + 16 * i] = e
    else:
        img[446:462] = mbr_entry(0x00, 0xEE, 1, min(total - 1, 0xFFFFFFFF))
        ktype = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le if a.plain_type else GPT_TYPE
        parts = [(gpt_guid_bytes(BIOS_BOOT_GUID), uuid.uuid4().bytes_le, 34, part_start - 1, "BIOS boot")] if core else []
        parts.append((ktype, vol_uuid.bytes_le, part_start, part_start + part_count - 1, a.label))
        if esp:
            parts.append((ESP_GUID.bytes_le, uuid.uuid4().bytes_le, esp_start, esp_start + esp_sectors - 1, "EFI system"))
        if report_sectors:
            parts.append((BASIC_DATA_GUID.bytes_le, uuid.uuid4().bytes_le, report_start,
                          report_start + report_sectors - 1, "KESTREL RPT"))
        write_gpt(img, total, parts, uuid.uuid4().bytes_le)
    img[510:512] = b"\x55\xaa"

    with open(a.out, "wb") as f:
        f.write(img)
    print(f"{a.out}: {a.size} MiB {a.layout.upper()}, GRUB core {core_sectors} sectors, "
          f"Kestrel partition at LBA {part_start} ({'0x83/linux' if a.plain_type else 'kestrel'} type), "
          f"payload {len(payload)} bytes crc {crc32(payload):08x}, uuid {vol_uuid}"
          f"{f', ESP at LBA {esp_start}' if esp else ''}"
          f"{f', report volume at LBA {report_start}' if report_sectors else ''}"
          f"{', no sector-0 marker' if a.no_marker else ''}{', CORRUPTED payload' if a.corrupt_payload else ''}")


if __name__ == "__main__":
    main()
