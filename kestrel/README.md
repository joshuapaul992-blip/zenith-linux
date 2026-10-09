# Kestrel

Kestrel (0.2.0, "Stable Storage Engine") is a freestanding x86_64 kernel that boots through GRUB 2 (Multiboot2). It draws its own boot manager on a 1024×768 linear frame buffer, styled after the classic BIOS-based Windows Boot Manager. After a choice is made it brings up a small POSIX-style kernel: system calls, a virtual file system with `/dev`, `/proc` and `/sys`, and preemptive kernel threads.

Tested in QEMU 9.2 with three boot paths:
- legacy BIOS from the ISO, using a VBE frame buffer
- UEFI with OVMF from the ISO, using a GOP frame buffer
- legacy BIOS from the 1.44 MB floppy image
- legacy BIOS from a USB stick image on an emulated xHCI port (`make run-usb`)

## Building

```sh
# Debian / Ubuntu
sudo apt install build-essential nasm grub-pc-bin grub-efi-amd64-bin grub-common \
                 xorriso mtools qemu-system-x86 ovmf

make            # build/kernel.elf
make iso        # build/kestrel.iso          hybrid BIOS + UEFI (grub-mkrescue)
make floppy     # build/kestrel-floppy.img   1.44 MB, BIOS (grub-mkimage + memdisk)
make usbimg     # build/kestrel-usb.img      64 MiB USB stick: GRUB + Kestrel boot volume (rootfs/)
make run-usb    # QEMU q35, boot from the stick on xHCI (kernel runs with kestrel.root=usb)
make run        # QEMU, legacy BIOS
make run-uefi   # QEMU, OVMF (set OVMF=/path/to/OVMF_CODE.fd if not auto-detected)
make run-floppy
make run-pci    # QEMU q35: 200 GiB SATA disk on AHCI, NVMe behind a root port, xHCI (+ USB keyboard
                #           and mouse) behind two bridges
make iso KERNEL_CMDLINE="ahci.selftest=rw"   # bake a kernel command line into the ISO
make test-pci   # host unit test of the PCI enumerator against a simulated config space
make check      # grub-file --is-x86-multiboot2
make debug      # QEMU paused with a gdb stub on :1234
```

A host x86_64 `gcc` works because everything is compiled `-ffreestanding -nostdlib -mno-red-zone -mgeneral-regs-only`. An `x86_64-elf-gcc` cross compiler is used automatically if one is on `PATH`.

On Fedora and Arch the GRUB tools are called `grub2-*`. The Makefile finds both names; override `GRUB_LIB` if `boot.img` lives somewhere unusual.

The kernel log goes to COM1, so `make run` prints it in your terminal (`-serial stdio`).

## Boot manager

| Key | Action |
|---|---|
| ↑ / ↓, Home / End | Slide the silver highlight bar (6-frame eased animation) |
| Enter | Start the highlighted entry, or run the highlighted tool |
| Tab | Switch between the OS list and the **Tools:** block |
| F8 | **Advanced Boot Options**: Safe Mode, Debugging Mode, Verbose File System Report, Disable Double Buffering |
| Esc | Leave the Tools block / cancel the F8 screen |
| any key | Stops the 10-second countdown; the countdown line disappears, as in bootmgr |

When the countdown reaches 0, the highlighted entry starts.

The tools are:
- **Memory Diagnostic**: three-pattern test over up to 64 MiB of free RAM, with a progress bar.
- **System Information**: firmware, boot loader, video mode, CPU, the memory map and IRQ counts.
- **Restart Computer**.

Layout is an 85×32 character grid drawn with a 12×24 font:
- a silver title bar
- white prompt text on black
- the entry list, with a `>` at the end of the highlighted entry
- the countdown line
- the F8 hint
- the **Tools:** block
- a silver legend bar at the bottom: `ENTER=Choose  TAB=Menu  ESC=Cancel`

## After boot: terminal and shell

The screen becomes a 128 × 48 text terminal (`kernel/term.c`) drawn with `kestrel_font[256][16]`. `init` (pid 1) prints a short system summary, read through system calls, and then starts `kush`, the Kestrel shell (`kernel/kush.c`).

The terminal is built in layers:
- **Glyph rendering:** `draw_char(c, x, y, fg, bg)` draws the 16 rows of a glyph, setting fg pixels for 1 bits and bg pixels for 0 bits (bit 7 is the leftmost pixel). It clips to the screen and publishes the 8 × 16 cell to video memory.
- **Layout:**
  - `term_col` and `term_row` track the cursor. Lines wrap at column 128.
  - `scroll_screen()` copies pixel rows 16–767 up one text row with per-scan-line `memcpy`, so source and destination never overlap. It then blanks the bottom 16 rows to `0x00000000` and sets `term_row` to 47.
  - ANSI colour codes are supported.
- **Cursor:** a solid inverse-video block toggled every 500 ms from the 1 kHz timer interrupt (`pit_add_tick_hook`). A shadow copy of every cell lets the cursor restore exactly what it covered. Every keystroke redraws the cursor at the new position and restarts the blink, so it is never hidden while you type.
- **Input:**
  - The PS/2 or USB HID interrupt converts the key to ASCII and puts it in the keyboard driver's interrupt-safe ring buffer, which wakes the shell.
  - The shell appends printable characters to its 1024-byte line buffer and draws them at `term_col`/`term_row`.
  - Backspace steps left, back across a line wrap if needed, and blanks the cell with a space in the background colour.
  - Enter prints a newline and hands the line to the parser.

`kush` is the shell. Its prompt shows the working directory (`kestrel:/root# `), and it starts in `/root`. Its command line understands:
- `'single'` and `"double"` quotes, and backslash escapes
- `$?`, the last exit status
- redirection: `> file`, `>> file` and `< file`

Shell built-ins:

| Command | What it does |
|---|---|
| `help`, `clear`, `sysinfo` | tools list; black screen with the cursor at 0,0; OS, CPU, memory and device summary |
| `ps`, `mem`, `pci`, `usb` | views of `/proc/tasks`, `/proc/meminfo`, `/proc/pci`, `/proc/usb` |
| `disk`, `log`, `font` | SATA disks and the MBR of `/dev/sda`; kernel log; all 256 glyphs |
| `lsblk`, `bootvol`, `recovery` | block devices and partitions; how the boot volume was found; the recovery console |
| `reboot`, `poweroff` | restart, or power off through `reboot(2)` |

### Core utilities (`kernel/coreutils/`)

Each utility is a separate `int name_main(struct cu_io *io, int argc, char **argv)`.
- **Kernel interface:** utilities reach the kernel only through the system-call gate (`int 0x80`, Linux numbering). They use `open`, `read`, `write`, `close`, `stat`, `fstat`, `lseek`, `getdents64`, `mkdir`, `rmdir`, `unlink`, `rename`, `chmod`, `chown`, `utimensat`, `getcwd`, `chdir`, `getuid`, `uname`, `clock_gettime` and `ioctl`.
- **Input and output:** they read standard input from `io->in` and write to `io->out` and `io->err`, which is how `kush` redirection works.
- **Buffers:** buffers are fixed-size and bounds-checked: 4 KiB I/O chunks, `snprintf`/`strlcpy` everywhere, and paths limited to 255 bytes.
- **Exit status:** 0 is success, 1 a failure, 2 a usage error; `grep` returns 0 for a match, 1 for none and 2 for an error.

| Command | Notes |
|---|---|
| `pwd`, `cd [DIR\|-]` | The working directory is the process's `cwd` vnode. `cd` resolves relative and absolute paths, goes to `/root` with no argument, back with `-`, and reports *No such file or directory* or *Not a directory*. |
| `ls [-a] [-l] [-1]` | Columns sized to the terminal (one name per line when redirected). `-l` shows mode, links, owner, group, size and modification time. |
| `mkdir [-p] [-m MODE]`, `rmdir` | `rmdir` reports *Directory not empty*, *Device or resource busy* (mount points) and *Not a directory*. |
| `touch [-c]` | Creates the file with `open(O_CREAT)`, or sets its modification time with `utimensat`. |
| `rm [-f] [-r]` | Refuses `.`, `..` and `/`. |
| `cp [-r]`, `mv` | `cp` copies through a 4 KiB buffer and keeps the permission bits. `mv` uses `rename`; across file systems (EXDEV), e.g. out of the read-only `/boot` volume, it copies and then deletes. |
| `cat [-n]`, `head [-n N]`, `tail [-n N]` | `tail` seeks back from the end in 4 KiB blocks on seekable files and buffers non-seekable input. |
| `grep [-i] [-n] [-v] [-c] [-q] PATTERN` | Fixed-string match, line by line; matches are highlighted on the terminal. |
| `echo [-n] [-e\|-E]` | Escapes `\n \t \\ \a \b \e \f \r \v \0NNN \c` |
| `chmod MODE`, `chown USER[:GROUP]` | Octal (`755`) or symbolic (`u+x,go-w`) modes. Names come from `/etc/passwd` and `/etc/group`, or numbers. |
| `date [+FORMAT]` | The CMOS RTC is read at boot and advanced by the HPET. Kestrel has no time zones, so the time is UTC. |
| `uname [-asnrvm]` | Without options it prints `Sysname: Kestrel`, `Nodename: kestrel-pc`, `Release: 0.2.0-release`, `Version: Stable Storage Engine`, `Machine: x86_64`. |
| `neofetch` | A falcon in columns 2–28 (silver) and live system information from column 32, never past column 127 (no wrapping). Below it, the 16-colour strip. |

`neofetch` reads every value at run time from these sources:
- `uname(2)` and `getuid(2)` + `/etc/passwd` for the user and host
- `/etc/os-release`
- `/proc/uptime` and `/proc/meminfo`
- `/sys/class/graphics/fb0/{virtual_size,name}` and `TIOCGWINSZ` for the screen
- the CPUID brand-string leaves `0x80000002`–`0x80000004` for the CPU

## PCI enumerator (`drivers/pci/`)

`pci.h` and `pci.c` are standalone: no Kestrel headers, no libc, no allocator. They have their own port I/O and COM1 output, and the compiled object has no undefined symbols. The pair can be copied into any freestanding x86_64 kernel.

- **Access:** Configuration Mechanism #1 (0xCF8 address latch, 0xCFC data window). Each access runs with interrupts disabled.
- **Structures:** packed type-0 and type-1 headers with compile-time size checks, plus a `union pci_config_header` for raw dword access.
- **Scan modes:**
  - `PCI_SCAN_RECURSIVE` walks from the host bridge (one root bus per function if 00:00.0 is multi-function) through every PCI-to-PCI bridge, including PCIe root and switch ports. A visited-bus bitmap and a depth limit stop loops from misprogrammed bridges.
  - `PCI_SCAN_BRUTE_FORCE` probes all 256 × 32 × 8 bus/device/function combinations and rebuilds parent links afterwards. It also finds root buses that no host-bridge function points at.
- **Multi-function devices:** functions 1–7 are probed only when function 0 sets bit 7 of Header Type, which avoids "ghost" duplicates.
- **BARs:** I/O, 32-bit, 64-bit (the upper slot is marked as consumed), legacy 16-bit and prefetchable are all decoded. With `PCI_SCAN_SIZE_BARS`, sizes are measured with decoding switched off, and the original BAR and Command values are restored.
- **Filters:** `pci_filter_and_log()` logs each function and stores a reference under every category it matches. Results are read back with `pci_match_count()` / `pci_match_get()`.

| Category | Rule |
|---|---|
| `PCI_MATCH_AHCI` | class 01, subclass 06 |
| `PCI_MATCH_NVME` | class 01, subclass 08 |
| `PCI_MATCH_NVIDIA_DISPLAY` | vendor 10DE **and** class 03, so the card's HD-Audio function does not match |
| `PCI_MATCH_XHCI` | class 0C, subclass 03, prog-if 30 |

In Kestrel, results go to COM1 and the kernel log at boot. You can also see them in `/proc/pci` (the `pci` shell command) and in the boot manager's System Information tool.

Mechanism #1 only reaches the first 256 bytes of configuration space. PCIe extended capabilities (offset 0x100 and up) need ECAM from the ACPI MCFG table.

## AHCI SATA driver (`drivers/ahci/`)

`ahci.h` and `ahci.c` are standalone in the same way as the PCI module. Kestrel connects them in `kernel/storage.c`: it finds class 01.06 through the PCI scan, takes BAR5 (ABAR), enables MMIO decoding and bus mastering, and maps the registers uncached.

- **Register layouts:** packed, 4-byte-aligned structs for the generic HBA block (CAP, GHC, IS, PI, VS, CAP2, BOHC) and the 32 per-port blocks (PxCLB, PxFB, PxIS, PxIE, PxCMD, PxTFD, PxSIG, PxSSTS, PxSCTL, PxSERR, PxCI, ...). Every offset is checked at compile time. Also included: the H2D/D2H FIS, the 256-byte received-FIS area, the 32-byte command header, the PRD and the command table.
- **HBA bring-up:**
  1. BIOS/OS handoff (BOHC), then `GHC.AE` with interrupts off.
  2. For each implemented port: stop the DMA engines (ST, then CR; FRE, then FR, 500 ms each, with a COMRESET fallback).
  3. Allocate the command list (1 KiB, 1 KiB aligned), the received-FIS area (256 B aligned) and one 128-byte-aligned command table per slot.
  4. Clear SERR/IS, power up / spin up, enable FIS receive, check `SSTS.DET` and the signature, wait for BSY/DRQ to clear, start the engine.
  5. IDENTIFY: model, serial, LBA48 support, capacity, and logical sector size (512e/4Kn).
- **I/O:** `ahci_read()`, `ahci_write()` and `ahci_flush()` are synchronous and polled.
  - Commands are READ/WRITE DMA EXT (48-bit LBA), with a 28-bit fallback for drives without LBA48.
  - The PRDT is built page by page and physically contiguous pages are merged. Large requests are split across commands.
  - If the device reports an error, the driver logs PxIS/PxTFD/PxSERR and runs the AHCI §6.2.2 port recovery.
- **Verify loop:** `ahci_verify_mbr()` reads LBA 0 N times (each into a poisoned buffer) and checks the copies match. It hex-dumps the boot code and partition table, then reports the `55 AA` signature, the disk ID and the partitions. On a protective MBR it also checks for a GPT header.
- **Self-test:** `ahci_selftest_rw()` saves the last sector, writes a pattern, reads it back, restores the original and checks the restore. It runs only with `ahci.selftest=rw`.
- **`/dev/sda` …:** read-only byte-addressable device nodes. Writes return `EROFS` on purpose; use `ahci_write()` from kernel code.

Tested in QEMU (ICH9 AHCI) under SeaBIOS and OVMF with a 200 GiB disk. The tests covered:
- reads and writes at LBA 419,430,399, which needs 48-bit addressing
- a 63-sector single-command read with per-sector ordering checks
- an error injected on sector 10 through QEMU's blkdebug layer, followed by recovery

`tools/mkdisk.py` builds the sparse test image.

## xHCI USB driver (`drivers/usb/`)

`xhci.h`, `usb.h` and `xhci.c` are standalone in the same way as the other drivers. Kestrel connects them in `kernel/usbhost.c`.

- **Registers:** packed layouts for the capability, operational, port, runtime/interrupter registers and the doorbells. Offsets are checked at compile time. Also defined: TRBs, the ERST entry, and slot, endpoint and input-control contexts (32- or 64-byte per `HCCPARAMS1.CSZ`).
- **Controller bring-up:**
  1. BIOS handoff and SMI disable (USB Legacy Support capability), and USB2/USB3 port ranges from the Supported Protocol capabilities.
  2. Halt, then HCRST (waiting for CNR to clear). Set `CONFIG.MaxSlotsEn`.
  3. Build the DCBAA. Its entry 0 points at the scratchpad array; each scratchpad buffer is one page.
  4. Set up the command ring, a single-segment event ring and the ERST, then interrupter 0 (IMAN, IMOD, ERSTSZ, ERDP, ERSTBA).
  5. Set Run/Stop, then send a No-Op command to check both rings.
- **Rings:**
  - `xhci_ring_enqueue()` writes the TRB body first and the cycle bit last. At the end of the ring it hands over the Link TRB (Toggle Cycle, chain bit inherited) and flips the producer cycle.
  - `xhci_event_dequeue()` consumes events by cycle state. The ERDP is updated with EHB after each batch.
- **Enumeration:**
  1. Port reset: USB2 ports through PR; USB3 ports through link training, with a warm-reset fallback.
  2. Enable Slot, then Address Device with an input context (slot plus EP0). Max packet size starts at 8 for low/full speed, 64 for high speed and 512 for SuperSpeed.
  3. Device descriptor (8 bytes, then Evaluate Context if the max packet size differs, then all 18 bytes), and manufacturer/product strings.
  4. Configuration bundle, then SET_CONFIGURATION.
  5. For HID boot interfaces: SET_PROTOCOL(boot) and SET_IDLE(0). One Configure Endpoint adds the interrupt-IN endpoints, with the interval converted per speed. Eight report TRBs are kept queued per endpoint.
- **HID:**
  - Keyboard reports are compared with the previous report to produce press and release events. Caps Lock is tracked, US-layout ASCII is generated, and the driver repeats held keys itself (500 ms, then 30 per second).
  - Mouse reports deliver dx/dy/wheel/buttons.
- **Concurrency:**
  - `xhci_poll()` drains the event ring from the 1 kHz timer interrupt. It completes commands and transfers, re-arms report buffers and records port changes.
  - `xhci_service()` runs in the `usbd` thread. It enumerates hot-plugged devices, frees removed ones, and recovers halted endpoints (Reset Endpoint, then Set TR Dequeue).
- **Kestrel integration:** USB keys go into the same queue as PS/2, so the boot manager works with a USB-only keyboard. The mouse drives an overlay pointer. Status is in `/proc/usb` (the `usb` shell command) and in the System Information tool.

Tested in QEMU `qemu-xhci` under SeaBIOS and OVMF. On OVMF, BAR0 sat at 0xC0_0000_0000, above 4 GiB.
- **Devices:** high-speed and full-speed keyboards, a mouse, and a SuperSpeed mass-storage device (enumerated only).
- **Not enumerated:** a tablet (non-boot HID) and a hub, both detected and reported.
- **Behaviour:** typematic repeat, hot-add of a keyboard, and hot-removal of the mouse.

Not implemented yet:
- devices behind external hubs (route strings, transaction translators)
- HID report-descriptor parsing for non-boot devices
- MSI-X or INTx interrupts in place of the timer poll
- more than one controller
- isochronous transfers; the only bulk class driver is mass storage (below)

## USB boot strapping and storage discovery

The path from "the firmware owns the USB controllers" to "the boot volume is mounted on `/boot`" is built to survive slow devices, late devices, bad media and missing media. Every step has a timeout and logs what it saw.

**1. Firmware → OS handoff** (`drivers/usb/usb_legacy.c`, plus `xhci.c` for xHCI itself). This runs before any USB driver touches a controller.
- **EHCI:** walk the EECP list in PCI config space and set *HC OS Owned*. Poll up to 1 s for *HC BIOS Owned* to clear, and force it if SMM never answers. Then write `USBLEGCTLSTS = 0xE0000000` (SMIs off, status acknowledged), halt the controller and clear `CONFIGFLAG` so its ports fall back to the companions.
- **UHCI / OHCI:** UHCI gets `LEGSUP = 0x8F00`. OHCI gets an `OCR` ownership request, a wait (up to 1 s) for `IR` to clear, and a forced release on timeout.
- **Intel PCH xHCI** (7/8/9 series): `USB3_PSSEN`/`XUSB2PR` are written from their BIOS-provided mask registers, so shared ports switch to the xHCI.
- **xHCI USBLEGSUP:** requested right before the controller reset, with the same wait-then-force logic. The outcome appears in the diagnostic trace.

**2. Calibrated clock** (`kernel/time.c`). The HPET is found through ACPI (RSDP from the Multiboot2 tags, else the EBDA/BIOS area). If there is none, the TSC is calibrated against PIT channel 2 by polling. Either source works with interrupts off, so settling and transfer timeouts are real milliseconds from the first instruction on.

**3. Non-blocking settling** (`xhci_settle_step()`). One call is one pass over every root port's `PORTSC`; it never depends on Port Status Change events alone.
- A new connection is debounced for 100 ms, then enumerated.
- A failed enumeration is retried after 250 ms and then 500 ms; after three failures the port is marked FAILED.
- A disconnect, or a fast unplug/replug (CSC on an enumerated port), frees the slot.

The function returns the number of ports still in progress. `xhci_init()` runs it for up to 600 ms, and root discovery keeps calling it for the whole `kestrel.rootwait` budget.

**4. Mass storage: Bulk-Only Transport + SCSI** (`drivers/usb/usb_msc.c`, standalone).
- **Transport:** a 31-byte CBW (`USBC`) is followed by the data stage and a 13-byte CSW (`USBS`). The CSW must have the right signature, an echoed tag, `residue ≤ length` and a valid status.
- **Stall handling:** a stall in the data stage is cleared, and the CSW is still read. A stalled CSW is retried once. A phase error or invalid CSW triggers Reset Recovery (class request `0xFF`, then `CLEAR_FEATURE(ENDPOINT_HALT)` on both pipes).
- **Commands:** Get Max LUN, INQUIRY, TEST UNIT READY with REQUEST SENSE, READ CAPACITY(10), and READ CAPACITY(16) for disks over 2 TiB. Reads use READ(10), or READ(16) for LBAs beyond 32 bits, in 64 KiB transfers.
- **Readiness:** UNIT ATTENTION is retried at once. NOT READY / becoming ready (`04/xx`) waits. NOT READY / medium not present (`3A/xx`) reports *no medium*.
- **Read retries:** each read gets 4 attempts with back-off, and a short transfer counts as an error. An unplugged or re-enumerated device is detected through the slot generation and reported as *device removed*, never retried forever.
- **xHCI side:** `xhci_bulk_transfer()` sends Normal TRBs through a 64 KiB bounce buffer that does not cross a 64 KiB boundary.
  - On timeout, it cancels the TD with Stop Endpoint and Set TR Dequeue.
  - On a stall or transaction error, it resets the host endpoint.
  - Control transfers, bulk transfers and settling are serialised by a controller lock.

**5. Block layer** (`kernel/block.c`). Disks are named by kind (`usb0`, `sata0`) and their partitions are named `usb0p1`, ….
- **MBR:** primary and logical partitions (EBR chain with loop guard).
- **GPT:** header and entry-array CRC32 are checked; if the primary GPT is invalid, the backup is used.
- Requests are bounds-checked and serialised per disk.
- Devices that disappear are marked removed, and later reads return `-ENODEV`.
- Every device gets a `/dev` node; `/proc/partitions` (`lsblk`) lists them.

**6. Root volume discovery without device names** (`kernel/bootvol.c`). A 512-byte, CRC32-protected volume header (`include/kernel/bootvol.h`) describes a ustar payload. It is found in two independent ways:
- **Option A — partition table:** an MBR partition of type `0x4B`, or a GPT partition whose type GUID is the 16 bytes `KESTREL-BOOT-VOL`. With `kestrel.uuid=` the volume UUID (or the GPT unique partition GUID) must match.
- **Option B — sector 0:** `KESTREL_BOOT` at byte 3 of block 0 (the BPB area that GRUB's boot.img leaves free), followed by the u64 block number of the header. This also works on unpartitioned media.

Once a header is found, its payload CRC is verified (unless `kestrel.rootverify=0`) and the ustar archive is mounted read-only on `/boot` (`fs/tarfs.c`). If present, `/boot/etc/motd` replaces `/etc/motd`.

| Command line | Meaning |
|---|---|
| `kestrel.root=auto` (default) | Mount a volume if one is attached. Waits only while USB ports are still settling. |
| `kestrel.root=usb` / `any` | The volume is **required** (USB disks only, or any disk). Wait up to `rootwait`, else recovery. |
| `kestrel.root=none` | Skip discovery. |
| `kestrel.rootwait=MS` | Wait budget, default 3000, max 60000. |
| `kestrel.uuid=UUID` | Accept only this volume. |
| `kestrel.rootverify=0` | Skip the payload CRC. |

**7. Failover to the recovery console** (`kernel/recovery.c`). A required volume may be missing, fail its CRC, or hit read errors or timeouts. In each case the kernel clears the 1024×768 terminal and prints a diagnostic trace, also on COM1:
- the reason
- the policy and how long it waited
- the clock source
- `USBCMD`/`USBSTS` decoded, `CRCR`, `DCBAAP`, `CONFIG`, and interrupter 0 (`IMAN`, `ERSTSZ`, `ERDP` against the driver's dequeue index)
- the handoff result
- the last command and transfer completion codes, plus timeout, stall and failed-port counters
- every port's `PORTSC` decoded: connect/enable/power/reset/over-current, link state, speed and settle state
- per-disk BOT statistics with the last SCSI sense
- the block devices and the tail of the kernel log

A command interpreter then reads from the keyboard (PS/2 or USB) and from the serial line at the same time:

`help` · `diag` · `usb` · `devs` · `log [n]` · `read <dev> [block]` (hex dump) · `scan` · `boot <dev>` · `retry` · `continue` · `reboot` · `poweroff`

The shell's `recovery` command opens the same console at any time. `bootvol` and `lsblk` show what was found.

**The USB stick image** (`tools/mkusbimg.py`, `make usbimg`) has this layout:

| Location | Contents |
|---|---|
| LBA 0 | GRUB `boot.img` plus the option-B marker |
| From LBA 1 | `core.img`, carrying the stripped kernel and a `grub.cfg` with `kestrel.root=usb` in its memdisk |
| LBA 2048 | the `0x4B` partition with the header and a ustar of `rootfs/` |

Options:
- `--layout gpt` builds a protective MBR + GPT. A BIOS-boot partition holds `core.img`, and the blocklists are patched the way grub-setup does it.
- `--no-marker`, `--plain-type`, `--no-grub` and `--corrupt-payload` build the test variants.

Tested in QEMU (q35, `qemu-xhci`, `usb-storage`) for each of these situations:

| Situation | Result |
|---|---|
| Boot from the stick (MBR and GPT layouts) under SeaBIOS | mounted via option A in ~15–150 ms |
| ISO + data stick under OVMF | mounted (`auto`) |
| Option-B-only stick | mounted via the sector-0 signature |
| Stick hot-added 3 s into `rootwait` | found at 3.9 s |
| No stick with `kestrel.root=usb` | recovery after the budget; commands typed on the USB keyboard and over COM1; `continue` boots on |
| Corrupted payload | CRC mismatch on both candidates → recovery |
| `blkdebug`-injected read errors | 4 retries per candidate with sense data → recovery |
| Unplug after boot | removal detected; reads return *device removed*; the replugged stick appears as `usb1` with partitions, through the `usbd` thread |
| EHCI with three UHCI companions (pc machine) | handoff logged |
| Floppy and q35/AHCI | boots without regressions |

## Source layout

```
boot/multiboot2_header.asm   MB2 header: info request + framebuffer tag 1024x768x32
boot/boot.asm                32-bit entry, CPUID/LM checks, 4 GiB identity map (2 MiB pages),
                             EFER.LME + paging, far jump to 64-bit, call kernel_main
boot/grub/*.cfg              GRUB configs (ISO, floppy memdisk, USB stick memdisk)
linker.ld                    kernel at 2 MiB physical, MB2 header first in .text

arch/x86_64/  gdt.c          GDT (kernel/user code+data, SYSRET-compatible order) + TSS (RSP0, IST1)
              idt.c          256-entry IDT, exception dump, IRQ dispatch, int 0x80 gate (DPL3)
              isr.asm        vector stubs -> struct int_frame -> isr_dispatch()
              pic.c pit.c    8259A remap to 32-47; 8254 at 1000 Hz
              switch.asm     context_switch(), thread_trampoline
              syscall_entry.asm  SYSCALL/LSTAR fast path (for ring 3)

drivers/      fb.c           put_pixel, draw_rect, draw_rect_outline, draw_char, draw_string,
                             clipping, scrolling, back buffer + dirty-rect flush (24/32 bpp)
              console.c      text-mode emulation over the LFB (ANSI colours, scrolling)
              keyboard.c     PS/2 IRQ1 driver, scancode set 1 incl. E0 arrows/F-keys, ring buffer
              serial.c       COM1 log
              font_*.c       Spleen 8x16 and 12x24 (boot manager), generated by tools/bdf2c.py
              kestrel_font.c kestrel_font[256][16] for the terminal, from tools/sheet2font.py
              pci/           standalone PCI enumerator (pci.h, pci.c, test/pci_sim_test.c)
              ahci/          standalone AHCI SATA driver (ahci.h, ahci.c)
              usb/           standalone xHCI driver + HID boot keyboard/mouse (xhci.h, usb.h, xhci.c),
                             firmware handoff (usb_legacy.c), mass storage BOT/SCSI (usb_msc.c)

mm/           pmm.c          bitmap frame allocator from the MB2 memory map
              heap.c         kmalloc/kfree (first fit, split, coalesce, IRQ-safe)
              vmm.c          extra identity mappings (e.g. GOP frame buffer above 4 GiB)

proc/task.c                  TCBs, round-robin scheduler (10 ms quantum), sleep_on/wakeup,
                             timed sleep, zombie reaping, idle thread
fs/           vfs.c          vnodes, mounts, path walk (., .., mount crossing), file objects,
                             getdents64, ramfs files, generated "pseudo" files
              devfs.c        /dev: null zero random urandom kmsg tty console ttyS0 fb0 (+ block devices)
              tarfs.c        read-only ustar file system on a block device (the /boot volume)
              procfs.c       /proc: version uptime meminfo cpuinfo mounts filesystems tasks
                             interrupts cmdline kmsg syscalls
              sysfs.c        /sys: kernel/ class/graphics/fb0/ firmware/ devices/system/cpu/ power/
kernel/       kernel.c       initialisation sequence, init and worker threads
              syscall.c      syscall table (Linux x86_64 numbers), handlers, ENOSYS stubs
              klog.c         dmesg ring buffer, panic screen
              term.c         128x48 terminal: draw_char, cell model, scrolling, blinking cursor
              kush.c         kush shell: line editor, quoting, redirection, $?, built-ins
              coreutils/     pwd cd ls mkdir rmdir touch rm cp mv cat head tail grep echo
                             chmod chown date uname neofetch (+ cu_lib.c helpers, table.c)
              storage.c      PCI -> AHCI glue, boot-time MBR verify, /dev/sdX
              usbhost.c      PCI -> xHCI glue, key injection, mouse pointer, usbd thread, /proc/usb,
                             firmware handoff, mass storage -> block devices
              time.c         HPET (ACPI) / PIT-calibrated TSC clock usable before interrupts
              block.c        block device registry, MBR/GPT partitions, /proc/partitions
              bootvol.c      boot volume discovery (option A/B), root policy, failover
              recovery.c     diagnostic trace + recovery console (keyboard and COM1)
lib/crc32.c                  CRC-32 (GPT, volume header, payload)
tools/mkusbimg.py            bootable USB stick image builder
rootfs/                      contents of the boot volume payload
ui/bootmgr.c                 boot manager, F8 screen, tools
include/kernel/posix.h       errno, O_* flags, struct stat, dirent64, utsname (Linux ABI layout)
include/kernel/usyscall.h    int 0x80 wrappers used by init
```

## System calls

The calling convention is the Linux x86_64 one: the call number goes in `rax`, the arguments in `rdi rsi rdx r10 r8 r9`, and the result (a negative errno on failure) comes back in `rax`.

Calls can enter through two paths:
- `int $0x80` works today.
- `syscall` is wired through `IA32_STAR/LSTAR/FMASK` and is ready for ring 3.

**Implemented:** read, write, open, close, stat, fstat, lseek, ioctl, sched_yield, nanosleep, getpid, getppid, getuid, getgid, exit, exit_group, uname, getcwd, chdir, mkdir, rmdir, unlink, rename, chmod, chown, utimensat, getdents64, clock_gettime (`CLOCK_REALTIME` from the RTC, `CLOCK_MONOTONIC` from the HPET), reboot.

**Stubs returning `-ENOSYS`:** mmap, brk, fork, execve, wait4, kill, gettimeofday.

`cat /proc/syscalls` lists every call with its status and call count.

## Memory map at boot

| Range | Use |
|---|---|
| 0 – 1 MiB | reserved (firmware, BIOS data) |
| 2 MiB – ~2.3 MiB | kernel image (+ bss: boot page tables, 32 KiB boot stack) |
| after the kernel | 8 MiB heap, 3 MiB frame-buffer back buffer (from the PMM) |
| e.g. `0xFD000000` (VBE), `0x80000000` (OVMF GOP) | linear frame buffer |

## Next steps

The structure is laid out for these, in order:

1. **User mode:** per-process PML4 (`tcb.cr3` exists), an ELF64 loader, `iretq`/`sysretq` to ring 3, and `copy_from_user` in `syscall.c`'s `bad_ptr()`.
2. **fork/execve/wait4** on top of that.
3. **Signals.**
4. **A real libc port** (musl, newlib). The ABI types are already Linux-compatible.
5. **A writable file system** (FAT or ext2) on the block layer, next to the read-only tarfs.
6. **ACPI parsing** for power-off on real hardware. The emulator ports are used today.
7. **Hardware support:** HPET/APIC interrupts (the HPET is only used as a clocksource today), SMP, USB hubs, an EHCI driver for machines without xHCI.

## Licenses

Kestrel source: yours to license as you wish.

Spleen fonts (`third_party/spleen`): BSD-2-Clause, © Frederic Cambus.
