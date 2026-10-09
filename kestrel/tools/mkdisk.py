#!/usr/bin/env python3
"""
tools/mkdisk.py -- build a sparse raw disk image with a recognisable MBR,
used to exercise the AHCI driver in QEMU (make run-pci).

    python3 tools/mkdisk.py build/sata.img 200G

Layout:
  LBA 0         MBR: boot-code banner, disk identifier 0x4B455354 ("KEST"),
                partition 1 = FAT32 LBA (active), partition 2 = Linux,
                boot signature 55 AA
  LBA 1..63     each sector starts with "LBA nnnnnnnnnn" (ordering checks)
  last LBA      "KESTREL-LAST-SECTOR" marker (lies beyond 2^28 for >128 GiB
                images, so reading it proves 48-bit addressing works)
The file is sparse: a 200 GiB image uses a few KiB on the host.
"""
import struct
import sys

SECTOR = 512


def parse_size(s):
    units = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}
    return int(s[:-1]) * units[s[-1].upper()] if s[-1].upper() in units else int(s)


def part_entry(active, ptype, start, count):
    # CHS fields set to the LBA-only "maxed out" value 0xFE 0xFF 0xFF
    return struct.pack("<B3sB3sII", 0x80 if active else 0, b"\xfe\xff\xff", ptype,
                       b"\xfe\xff\xff", start, count)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, size = sys.argv[1], parse_size(sys.argv[2])
    total = size // SECTOR

    mbr = bytearray(SECTOR)
    mbr[0:3] = b"\xeb\x63\x90"                                  # jmp short + nop
    banner = b"KESTREL AHCI TEST DISK - not bootable"
    mbr[3:3 + len(banner)] = banner
    struct.pack_into("<I", mbr, 0x1B8, 0x4B455354)               # disk identifier
    p1_start, p1_count = 2048, 204800                            # 100 MiB FAT32
    p2_start = p1_start + p1_count
    p2_count = min(total - p2_start - 2048, 0xFFFFFFFF)
    mbr[0x1BE:0x1CE] = part_entry(True, 0x0C, p1_start, p1_count)
    mbr[0x1CE:0x1DE] = part_entry(False, 0x83, p2_start, p2_count)
    mbr[0x1FE:0x200] = b"\x55\xaa"

    with open(path, "wb") as f:
        f.truncate(size)
        f.write(mbr)
        for lba in range(1, 64):
            f.seek(lba * SECTOR)
            f.write(b"LBA %010d " % lba + bytes([lba & 0xFF]) * 32)
        f.seek((total - 1) * SECTOR)
        f.write(b"KESTREL-LAST-SECTOR lba=%d" % (total - 1))
    print(f"{path}: {size >> 30} GiB, {total} sectors, last LBA {total - 1}"
          f"{' (> 2^28: needs LBA48)' if total - 1 >= 1 << 28 else ''}")


if __name__ == "__main__":
    main()
