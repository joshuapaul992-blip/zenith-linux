# Kestrel

Kestrel (0.3.0, "Stable Storage Engine") is a freestanding x86_64 kernel that boots through GRUB 2 (Multiboot2). It draws its own boot manager on a 1024×768 linear frame buffer, styled after the classic BIOS-based Windows Boot Manager. After a choice is made it brings up a POSIX-style kernel that runs unmodified Linux/musl programs: processes and threads on every CPU, demand-paged copy-on-write memory, signals, futexes, epoll, Unix sockets with descriptor passing, a virtual file system with `/dev`, `/proc` and `/sys`, ext2 on disk, and TCP/IP networking with drivers for virtio-net, Intel e1000 and Realtek RTL8139/RTL8169/RTL8168 cards.

Tested in QEMU 9.2 with three boot paths:
- legacy BIOS from the ISO, using a VBE frame buffer
- UEFI with OVMF from the ISO, using a GOP frame buffer
- legacy BIOS from the 1.44 MB floppy image
- legacy BIOS and UEFI from a USB stick image on an emulated xHCI port (`make run-usb`)

## Building

```sh
# Debian / Ubuntu
sudo apt install build-essential nasm grub-pc-bin grub-efi-amd64-bin grub-common \
                 xorriso mtools qemu-system-x86 ovmf

make            # build/kernel.elf
make iso        # build/kestrel.iso          hybrid BIOS + UEFI (grub-mkrescue); carries the boot
                #                            volume as a Multiboot2 module (build/kestrel.vol)
make floppy     # build/kestrel-floppy.img   1.44 MB, BIOS (grub-mkimage + memdisk)
make usbimg     # build/kestrel-usb.img      64 MiB USB stick, BIOS + UEFI: GRUB, Kestrel boot volume
                #                            (rootfs/) and a FAT report volume (REPORT.TXT, VBIOS.ROM)
make run-usb    # QEMU q35, boot from the stick on xHCI (kernel runs with kestrel.root=usb)
make run        # QEMU, legacy BIOS
make run-uefi   # QEMU, OVMF (set OVMF=/path/to/OVMF_CODE.fd if not auto-detected)
make run-floppy
make run-pci    # QEMU q35: 200 GiB SATA disk on AHCI, NVMe behind a root port, xHCI (+ USB keyboard
                #           and mouse) behind two bridges
make iso KERNEL_CMDLINE="ahci.selftest=rw"   # bake a kernel command line into the ISO
make iso KERNEL_CMDLINE="nvme.selftest=rw"   # NVMe read/compare/write-restore self-test at boot
make test       # all host unit tests:
make test-pci   #   PCI enumerator against a simulated config space
make test-nvbios #  NVIDIA VBIOS parser (NVBIOS_ROM=card.rom also parses a real dump)
make test-nvidia #  NVIDIA probe against a simulated GP106
make check      # grub-file --is-x86-multiboot2
make debug      # QEMU paused with a gdb stub on :1234
```

A host x86_64 `gcc` works because everything is compiled `-ffreestanding -nostdlib -mno-red-zone -mgeneral-regs-only`. An `x86_64-elf-gcc` cross compiler is used automatically if one is on `PATH`.

On Fedora and Arch the GRUB tools are called `grub2-*`. The Makefile finds both names; override `GRUB_LIB` if `boot.img` lives somewhere unusual.

The kernel log goes to COM1, so `make run` prints it in your terminal (`-serial stdio`).

**Full desktop images.** With the ports built first (`make xlibre`, `make icewm`, `make busybox`, `make xterm`; see "X11", "IceWM" and "Terminals"), `make usbimg` and `make iso` include them. Both GRUB menus then offer two entries for 5 seconds:
- **Desktop** (the default): `kestrel.exec=/bin/startx` starts the X server, IceWM and an xterm. Logout in IceWM's menu ends the session, and the screen returns to the text terminals, which stay running underneath.
- **Text console:** a login shell (`/bin/sh`) on every monitor's terminal, no X. Type `startx` there for the desktop.

On the ISO, GRUB loads the boot volume into memory as a Multiboot2 module (`module2 /boot/kestrel.vol`). `kernel/ramdisk.c` turns each module into a read-only `/dev/ramN`, and the usual boot-volume discovery mounts it on `/boot`, found through the volume's sector-0 signature. `tools/mkusbimg.py --ramdisk` writes that volume file. This has been checked under BIOS and UEFI (OVMF) in QEMU.

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

## After boot: one terminal and one shell per monitor

Every monitor gets its own text terminal (`kernel/tty.c`), drawn with `kestrel_font[256][16]`. `init` (pid 1) prints a short system summary on the first monitor, read through system calls. It then starts a shell on every monitor: BusyBox's `/bin/sh` as a login shell on a real Unix terminal (`kernel/vt.c`, see "The shell on the terminals" below). Without `/bin/sh` on the boot volume, or with `kestrel.shell=kush` on the kernel command line, it runs `kush` (`kernel/kush.c`), the shell built into the kernel, instead.

**Font** (`drivers/kestrel_font.c`): 256 glyphs, 8×16 pixels, in Windows-1252 order. It is generated from the glyph sheet `third_party/kestrel_font/kestrel_font_sheet.png` by `tools/sheet2font.py` (`make fonts`). The sheet is a 16×16 grid of 16×16-pixel cells whose ink fits in the middle 8 columns, so the tool cuts the same 8×16 window out of every cell: pixel for pixel, no scaling. To change the font, replace the sheet and run `make fonts`.

**Monitors** (`kernel/display.c`):
- Display drivers register one head per monitor. Each head has its frame buffer, native resolution, pitch and pixel format.
- Monitor 0 is the boot frame buffer (the VBE or GOP mode set by GRUB).
- Further monitors come from drivers that can set modes. Today that is `drivers/video/bochs_dispi.c`, for QEMU's `bochs-display` and `secondary-vga`. It reads the monitor's EDID (`drivers/video/edid.c`) and sets the preferred timing, so each monitor runs at its own native resolution.
- A native GPU driver would add its monitors through the same `display_register()` call. There is no NVIDIA driver yet.

**Terminals** (`struct kestrel_tty`, `system_ttys[MAX_MONITORS]`). Each TTY owns:
- its frame buffer pointer, plus an optional RAM shadow
- its native width and height, and pitch
- `max_cols × max_rows` (width / 8 × height / 16): 128 × 48 at 1024×768, 160 × 64 at 1280×1024, 240 × 67 at 1920×1080
- the cursor position, the blink state and the colours
- the escape-sequence parser and UTF-8 decoder (see below)
- a key queue, and a 1024-byte `input_buffer` (for kush)

No global screen size is used anywhere in the rendering path.

**Rendering:**
- `draw_char(tty, c, x, y, fg, bg)` draws the 16 rows of a glyph into that TTY's surface using that TTY's own pitch, clipped to its own size. Set bits get the fg colour and clear bits the bg colour; bit 7 is the leftmost pixel.
- `scroll_screen(tty)` moves that monitor's text up one row with a per-scan-line `memcpy` (source and destination never overlap) and blanks the bottom row. Only that monitor's memory is read or written.
- A monitor with a RAM shadow is drawn and scrolled in the shadow, and the changed rectangle is then copied to VRAM, because reading VRAM over PCIe is slow.
- **Speed:** frame buffers the CPU writes directly are mapped write-combining (PAT entry 1, as on Linux; firmware MTRRs otherwise make VRAM uncached, one slow bus write per pixel). Terminal output is processed in chunks of up to 4 KiB: inside a chunk only the character cells change and full-screen scrolls are counted, then the pixels move once, the changed rows are drawn and one rectangle goes to VRAM. 5,000 lines of `seq` take 0.7 s in QEMU, against 17 s with a pixel scroll and a VRAM copy per line.

**Cursor:**
- The 1 kHz timer interrupt walks every TTY and toggles each cursor every 500 ms, at that TTY's own cell and on its own phase.
- The monitor with keyboard focus shows a solid inverse block, and the others a two-pixel bar.

**Locking:**
- Every TTY has its own lock, and drawing runs with interrupts enabled. A flood of output on one monitor therefore never stalls the timer.
- The blink skips only the TTY that is busy at that moment, and the scheduler keeps switching between the shells.
- With interrupts off (the kernel log from an interrupt handler), a busy TTY is skipped instead of waited for.

**Keyboard:**
- The PS/2 and USB HID interrupt paths both go through one routine.
- **Ctrl+Alt+F1 … F12** moves the keyboard focus to TTY 1 … 12 right there in the interrupt handler. Every other key is queued on the focused TTY only, and wakes the shell of that TTY.
- Each task is bound to a TTY (`tcb.tty`, inherited by the threads it creates). `keyboard_wait()` and `/dev/tty` read the queue of the caller's TTY.

**Device nodes:**
- `/dev/tty` is the caller's own terminal.
- `/dev/console` is TTY 1.
- `/dev/tty1`, `/dev/tty2`, … are the terminals of monitor 0, 1, …, so `echo hi > /dev/tty2` prints on the second monitor.
- `TIOCGWINSZ` reports the size of the caller's monitor.
- The kernel log goes to TTY 1.

**The shell on the terminals** (`kernel/vt.c`). Each monitor's terminal is a terminal in the Unix sense, built from a console pty (`fs/pty.c`) whose master the kernel holds and whose slave is `/dev/ttyN`:
- **Keyboard:** the thread `vt-in/ttyN` turns the TTY's keys into the bytes a Linux console sends (Enter is CR, Backspace is DEL, the cursor and editing keys and F1–F12 are escape sequences, Alt adds an ESC prefix) and types them into the pty. So the line discipline does the editing, the echo and Ctrl+C/Ctrl+\ (signals), with full termios.
- **Screen:** the thread `vt-out/ttyN` draws the pty's output with `tty_write_vt()`.
- **Sessions:** the thread `getty/ttyN` starts `-sh` (ash as a login shell, which reads `/etc/profile`) as a new session with `/dev/ttyN` as its controlling terminal and `TERM=linux`. When the shell exits, the terminal is hung up (SIGHUP for anything left in the session) and reset, and a new shell starts.
- `tty`, `stty`, line editing with history and Tab completion, `top`, `vi`, `less` and Ctrl+C work as on Linux. `startx` starts the desktop from there; after Logout the terminal is back.

**Escape sequences and text** (`tty_write_vt()`): the Linux console's subset of ECMA-48, which is what `TERM=linux` promises.
- cursor movement and addressing, erase in line and display, insert and delete characters and lines, scroll regions, save and restore the cursor;
- SGR: bold, reverse, 8/16/256 colours and 24-bit colour;
- cursor show/hide, application cursor keys, and answers to the status and cursor-position queries;
- lines wrap the way xterm and the Linux console wrap them: the cursor stays in the last column until the next character arrives.
- **UTF-8:** code points the font has (Latin-1, the euro sign, curly quotes, dashes and the rest of Windows-1252) use their glyph. Box drawing and arrows get a look-alike, and anything else a `?`. A byte that is not valid UTF-8 shows its own Windows-1252 glyph.

The kernel's own output (the log, kush) goes through the same parser, except that a newline also returns the carriage.

**kush line editing** (with `kestrel.shell=kush`): the shell edits its line in its TTY's `input_buffer`, and keeps its history, `$?` and parse buffers in a per-session struct. The shells share no state.
- Printable characters are appended and echoed at the TTY's cursor.
- Backspace steps left, back across a line wrap if needed, and blanks the cell.
- Enter hands the line to the parser.

"Independent" means preemptive threads, spread over every CPU (see "Multiprocessor support"): while a command runs on one monitor (even `cat /dev/random`), the other shells still respond and every cursor keeps blinking.

Try it with three monitors in QEMU:

```sh
make iso
qemu-system-x86_64 -m 256M -vga none -device VGA \
    -device bochs-display,xres=1280,yres=1024 \
    -device secondary-vga,xres=1920,yres=1080 -cdrom build/kestrel.iso
```

`kush` is the shell built into the kernel, used with `kestrel.shell=kush` or when there is no `/bin/sh`. Its prompt shows the working directory (`kestrel:/root# `), and it starts in `/root`. Its command line understands:
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
| `gpu`, `report` | the NVIDIA report; write `REPORT.TXT` and `VBIOS.ROM` to the boot stick |
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
| `uname [-asnrvm]` | Without options it prints `Sysname: Kestrel`, `Nodename: kestrel-pc`, `Release: 0.3.0-release`, `Version: Stable Storage Engine`, `Machine: x86_64`. |
| `neofetch` | A falcon in columns 2–28 (silver) and live system information from column 32, never past column 127 (no wrapping). Below it, the 16-colour strip. |

`neofetch` reads every value at run time from these sources:
- `uname(2)` and `getuid(2)` + `/etc/passwd` for the user and host
- `/etc/os-release`
- `/proc/uptime` and `/proc/meminfo`
- `/sys/class/graphics/fb0/{virtual_size,name}` and `TIOCGWINSZ` for the screen
- the CPUID brand-string leaves `0x80000002`–`0x80000004` for the CPU

## NVIDIA GPUs (`drivers/gpu/nvidia/`): stage 1, detection

Kestrel has no NVIDIA mode-setting driver yet. Stage 1 detects the card and collects everything the mode-setting stage needs, without changing what is on screen. The monitor the firmware lit stays monitor 0, the boot frame buffer. The register knowledge comes from nouveau (Linux v6.6, MIT licence; see `third_party/nouveau/LICENSE`).

- **Chip:** `PMC_BOOT_0` gives the chipset (e.g. `136` = GP106, Quadro P2000) and the family. The VRAM size is read on Pascal and later.
- **VBIOS** (`nvbios.c`, hardware independent):
  - It is copied from PRAMIN (the image the VBIOS left in VRAM, seen through the BAR0 window) or from PROM (the ROM chip, with the PCI ROM shadow turned off).
  - The PCI expansion ROM is used only when both of those fail. Candidates are scored like nouveau does, and the PRAMIN window and shadow bit are restored afterwards.
  - The parser walks the image chain (PCIR, NPDE, checksums) and remaps table pointers into the extended `0xE0` image on Pascal. It reads the BIT table and version, and DCB 4.x outputs, connectors and I2C/AUX ports.
- **DisplayPort** (GM20x and Pascal):
  - The AUX pads are switched to AUX mode.
  - Each DisplayPort output's AUX channel reports whether a monitor is attached.
  - From the monitor it reads the DPCD capabilities, the link the firmware trained, lane status, and the EDID (I2C-over-AUX, both blocks).
- **Firmware state:** the display engine's armed core-channel state shows, for every head:
  - its raster, pixel clock and depth;
  - its scan-out surface;
  - which SOR (output resource) it drives, and how each DCB output is routed.

  A read-only register snapshot of everything nouveau's mode-setting path touches is included as well.

The shell command `gpu` prints the report. It also lands in `REPORT.TXT`, with the VBIOS image in `VBIOS.ROM` (see the report volume). Boot with `nvidia.probe=0` to skip the probe.

**Testing.** No NVIDIA hardware or emulator was available, so the code is checked in two ways:
- **`make test-nvbios`** parses a synthetic Pascal-style ROM: an x86 image with a checksum, an EFI image, and an extended image holding the DCB. `NVBIOS_ROM=VBIOS.ROM make test-nvbios` parses a real dump.
- **`make test-nvidia`** runs the whole probe against a simulated GP106 BAR0. The simulation provides:
  - a PRAMIN window over fake VRAM, and a PROM that only answers with the shadow bit cleared;
  - AUX channels with a DisplayPort monitor (DPCD and EDID);
  - firmware head and SOR state.

  It checks that every temporarily changed register is restored. The simulation encodes the same nouveau semantics as the driver, so it verifies the driver's logic, not the hardware's behaviour.

**Stage 2: experimental modeset** (`nvdisp.c`, `nvbios_disp.c`, `nvinit.c`; Pascal, DisplayPort). It is off unless you add one of these to the kernel command line (press `e` in the GRUB menu and append it to the `multiboot2` line):
- **`nvidia.modeset=1`** lights monitors the firmware left **dark**. Monitors the firmware already lit, including the one GRUB used, are kept exactly as they are. With a single monitor there is nothing to light, so nothing visibly changes; the log then says so.
- **`nvidia.modeset=2`** also takes over the firmware-lit monitors and re-drives every output itself. Afterwards the firmware frame buffer is no longer on screen, so no terminal is put on it. If the modeset fails, that screen stays dark (use the text-console entry, or boot without the option).
- **`nvidia.maxres=WxH`** limits the mode picked from the EDID (default 1920x1200).

Monitors it lights become ordinary Kestrel displays: a terminal each, and an X screen each. Their frame buffers are in VRAM and are written through the PRAMIN window from a shadow copy in RAM.

**Checking the result:**
- in the desktop's xterm or any shell: `cat /proc/gpu` (the modeset outcome first, then the full probe report), or `grep nvidia /proc/kmsg`;
- in the text console: the `gpu` command;
- on the USB stick: `REPORT.TXT` in its "KESTREL RPT" partition, written at boot (the ISO has no report partition).

The stage-2 code follows nouveau and was only checked against the simulated GP106 (`make test-nvidia`); it has not yet run on a real card.

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

## NVMe driver (`drivers/nvme/`)

`nvme.h` and `nvme.c` are standalone in the same way as the AHCI driver. Kestrel connects them in `kernel/storage.c`: it finds class 01.08 with prog-if 02 through the PCI scan, takes BAR0 (64-bit), enables MMIO decoding and bus mastering, disables INTx and maps the registers uncached.

- **Register layouts:** a packed `struct nvme_regs` (CAP, VS, INTMS/INTMC, CC, CSTS, NSSR, AQA, ASQ, ACQ, CMB and boot-partition registers, doorbells at 0x1000). Field macros cover CAP (MQES, CQR, TO, DSTRD, CSS, MPSMIN/MPSMAX), CC (EN, CSS, MPS, AMS, SHN, IOSQES, IOCQES) and CSTS (RDY, CFS, SHST). Also defined: the 64-byte submission queue entry, the 16-byte completion queue entry, and the Identify Controller and Identify Namespace structures. Every offset is checked at compile time.
- **Controller bring-up** (NVMe 1.4 §7.6.1):
  1. Read CAP. Require the NVM command set and 4 KiB pages (`MPSMIN = 0`). Take the doorbell stride (`4 << DSTRD`) and the ready timeout (`CAP.TO`, in 500 ms units).
  2. Disable: clear `CC.EN` and wait for `CSTS.RDY = 0`. If an enable is still in progress, it is allowed to finish first.
  3. Write AQA, ASQ and ACQ with the admin rings (up to 32 entries each, clamped to `MQES + 1`).
  4. Write CC: NVM command set, 4 KiB pages, round robin, 64-byte SQEs and 16-byte CQEs. Then set `EN` and wait for `RDY = 1`, failing early on `CSTS.CFS`. Mask interrupts through INTMS, since the driver polls.
  5. Identify Controller: model, serial, firmware, NN, VWC, RTD3E, MDTS and the minimum SQE/CQE sizes.
  6. Set Features (Number of Queues), then Create I/O Completion Queue and Create I/O Submission Queue. Queue 1 holds up to 64 entries and is physically contiguous; the CQ is created with interrupts off.
  7. Namespaces come from the active namespace list (CNS 02h), or from a scan of every NSID up to NN on NVMe 1.0 controllers. Each one gets an Identify Namespace, which gives its size and active LBA format. Formats with metadata are skipped.
- **Doorbells:** `SQyTDBL` sits at `0x1000 + 2y × stride` and `CQyHDBL` at `0x1000 + (2y + 1) × stride`.
- **Completions:** each queue tracks its head and an expected phase tag, which starts at 1 and flips whenever the head wraps. A CQE is new when its phase bit matches. The driver then checks the command identifier, takes the SQ head from DW2 and rings the CQ head doorbell. A completion with another CID belongs to an abandoned command and is discarded.
- **I/O:** `nvme_read()`, `nvme_write()` and `nvme_flush()` are synchronous. Requests are split at the controller's MDTS, capped by `NVME_MAX_TRANSFER` (default 1 MiB), and NLB is 16 bits. A per-controller lock serialises namespaces that share the I/O queue.
- **PRPs** (§4.3): every 4 KiB page of the caller's buffer is translated on its own through `virt_to_phys()`, so the buffer only has to be dword aligned.
  - PRP1 points at the first byte, which may sit at an offset in its page.
  - A transfer that ends in the next page puts that page in PRP2.
  - Anything longer makes PRP2 point at a PRP list. A list page holds 512 entries; when more follow, its last entry points at the next list page.
- **Errors:**
  - A status in the CQE is logged with its SCT, SC and DNR.
  - A timeout (5 s) or `CSTS.CFS` resets the controller and rebuilds its queues, because only a reset guarantees that the abandoned command's DMA has stopped.
- **Shutdown:** `nvme_shutdown_all()` deletes the I/O queues, sets `CC.SHN` to normal and waits for `CSTS.SHST` to report completion, for between 5 and 60 s depending on RTD3E. `reboot(2)` and the recovery console's `reboot`/`poweroff` call it first.
- **Block layer:** each namespace becomes `nvme0`, `nvme1`, … with MBR/GPT partitions (`nvme0p1`), so boot volume discovery with `kestrel.root=any` also looks at NVMe disks.

With `nvme.selftest=1` on the kernel command line, the boot runs a self-test on every namespace:
- It reads the first 8 MiB in one call and logs the CRC-32.
- It reads the same range again in chunk sizes from 1 block to `max_blocks + 1`, into a buffer 4 bytes past a page boundary, and compares the two copies.
- It checks that a buffer that isn't dword aligned, and a range past the end, are both rejected.
- `nvme.selftest=rw` adds a write, read-back and restore of the last 16 blocks.

Tested in QEMU 8.2 (`-device nvme`, NVMe 1.4) under SeaBIOS and OVMF, with two controllers:
- the default `mdts=7` (512 KiB per command), and `mdts=0` (no limit, so the driver's own cap applies)
- 512-byte and 4 KiB namespaces, including a sparse NSID (1 and 3)
- builds with `NVME_MAX_TRANSFER` at 1 MiB and 3 MiB, the latter with chained PRP lists from a misaligned buffer
- the CRC-32 of the data read matching the image on the host, and the image unchanged after the write test
- a clean shutdown on `poweroff`

QEMU accepts at most 1024 mappings per command. On a controller with `mdts=0`, a 4 MiB build therefore fails misaligned transfers that span 1025 pages, with Internal Error (SC 06h). This is a limit of the emulator, and the default 1 MiB cap stays well below it.

Not implemented yet:
- interrupts (MSI-X); completions are polled
- more than one I/O queue pair, or more than one command in flight
- SGLs, metadata and end-to-end protection
- Write Zeroes, Dataset Management (TRIM) and namespace management

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

**5. Block layer** (`kernel/block.c`). Disks are named by kind (`usb0`, `sata0`, `nvme0`) and their partitions are named `usb0p1`, ….
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
| then | an EFI System Partition (`0xEF`, 8 MiB) with `EFI/BOOT/BOOTX64.EFI`: GRUB x86_64-efi with the same memdisk, so the stick also boots on UEFI machines |
| last 24 MiB | the FAT16 report volume `KESTREL RPT` (`0x0E`, first table entry so that every OS mounts it) |

**Report volume.** Many machines have no serial port, so Kestrel writes its diagnostics to the stick it booted from (`kernel/report.c`):
- **The files:** `REPORT.TXT` (2 MiB) holds the kernel version, firmware, command line, CPU, every driver's report section (the NVIDIA report, for one) and the complete kernel log. `VBIOS.ROM` (1 MiB) holds the graphics card's video BIOS.
- **How it writes them:** the kernel finds the volume by its FAT label, follows the two files' cluster chains and overwrites only their data sectors. Sizes, the FAT and the directory never change, so the stick stays valid for every OS.
- **When:** at every boot (`kestrel.report=0` turns this off), and on demand with the shell command `report`.
- **Write path:** USB mass storage got WRITE(10)/WRITE(16) and SYNCHRONIZE CACHE for this, and the block layer got `blk_write()`/`blk_flush()`, also wired to SATA and NVMe.

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

arch/x86_64/  gdt.c          GDT (kernel/user code+data, SYSRET-compatible order), a TSS per CPU
                             (RSP0; IST1 #DF, IST2 NMI, IST3 #MC stacks)
              idt.c          256-entry IDT, faults -> signals, kernel fault dump with symbolized
                             backtrace, IRQ/IPI/timer dispatch, int 0x80 gate (DPL3)
              isr.asm        256 vector stubs -> struct int_frame -> isr_dispatch(); SWAPGS on
                             ring-3 entry/exit, paranoid GS check for NMI/#DF/#MC
              apic.c         ACPI MADT, local APIC (+ 1 kHz timer calibrated on the HPET/TSC),
                             IO APIC routing of ISA IRQs, IPIs, INIT/STARTUP
              smp.c          percpu blocks (GS base), big kernel lock, TLB shootdowns, AP start-up
              trampoline.asm real mode -> long mode start-up code of the other CPUs (at 0x8000)
              pic.c pit.c    8259A remap to 32-47; 8254 at 1000 Hz (without an APIC)
              switch.asm     context_switch(), thread_trampoline, enter_user/user_return
              syscall_entry.asm  SYSCALL/LSTAR fast path (for ring 3), canonical-RIP SYSRET check
              uaccess.asm    user-memory copies with exception-table fixups

drivers/      fb.c           put_pixel, draw_rect, draw_rect_outline, draw_char, draw_string,
                             clipping, scrolling, back buffer + dirty-rect flush (24/32 bpp)
              console.c      text-mode emulation over the LFB (ANSI colours, scrolling)
              keyboard.c     PS/2 IRQ1 driver, scancode set 1 incl. E0 arrows/F-keys; Ctrl+Alt+Fn focus
                             hotkeys and routing to the focused TTY (PS/2 and USB HID)
              kinput.c       /dev/kinput: evdev-style key/button/motion events for the X server
                             (PS/2 set 1 and USB HID -> Linux codes), grab while open
              serial.c       COM1 log
              font_*.c       Spleen 8x16 and 12x24 (boot manager), generated by tools/bdf2c.py
              kestrel_font.c kestrel_font[256][16] for the terminal, from tools/sheet2font.py
              pci/           standalone PCI enumerator (pci.h, pci.c, test/pci_sim_test.c)
              ahci/          standalone AHCI SATA driver (ahci.h, ahci.c)
              nvme/          standalone NVMe driver: admin + I/O queue pair, PRP lists (nvme.h, nvme.c)
              video/         EDID parser (edid.c), Bochs/QEMU DISPI mode setter (bochs_dispi.c)
              gpu/nvidia/    NVIDIA stage 1: VBIOS parser (nvbios.c), probe (nvidia.c): chip, VBIOS,
                             DP AUX/DPCD/EDID, firmware display state; host tests in test/
              net/           Ethernet cards: virtio_net.c, e1000.c (Intel 8254x/8257x), rtl8139.c,
                             rtl8169.c (RTL8169/8168/8111/8101), nic.h (shared helpers)
              usb/           standalone xHCI driver + HID boot keyboard/mouse (xhci.h, usb.h, xhci.c),
                             firmware handoff (usb_legacy.c), mass storage BOT/SCSI (usb_msc.c)

mm/           pmm.c          bitmap frame allocator from the MB2 memory map, frame reference counts
              heap.c         kmalloc/kfree (first fit, split, coalesce, IRQ-safe)
              vmm.c          identity map changes: MMIO uncached, write-combining (PAT), guard pages
              vm.c           user address spaces: regions (VMAs), demand paging, copy-on-write,
                             NX, mmap/munmap/mprotect/mremap/madvise/mincore/brk, OOM killer
              vmobj.c        page-backed memory objects (ramfs files, memfd, shared anonymous)
              uvm.c          user address-space layout, kernel page-table sharing

proc/task.c                  TCBs, round-robin scheduler on every CPU (10 ms quantum; an idle class for
                             background work such as kworker/0, which runs only when nothing else is
                             ready), sleep_on/wakeup, poll sleeps, timed sleep, zombie reaping,
                             a per-CPU idle thread, kernel stacks with guard pages
proc/exec.c                  hardened ELF64 loader (PT_INTERP, W^X), System V initial stack,
                             kernel-started processes
proc/process.c               fork/vfork/clone (threads), execve, wait4, sessions, signals
fs/           vfs.c          vnodes, mounts, path walk (., .., mount crossing), file objects,
                             getdents64, ramfs files, generated "pseudo" files
              devfs.c        /dev: null zero random urandom kmsg tty console ttyS0, fbN per monitor,
                             ptmx and pts/ (fs/pty.c)
                             (KFB_GET_INFO, KDSETMODE, KFB_BLIT) (+ block devices)
              tarfs.c        read-only ustar file system on a block device (the /boot volume)
              pipe.c         pipes
              unixsock.c     AF_UNIX stream/seqpacket/datagram-pair sockets, SCM_RIGHTS
              epoll.c        epoll
              anonfd.c       eventfd, timerfd, signalfd
              pty.c          pseudo-terminals: line discipline, termios, controlling terminals
              procfs.c       /proc: version uptime meminfo stat loadavg cpuinfo mounts filesystems tasks,
                             PID/{stat,status,cmdline,comm}, self/
                             interrupts cmdline kmsg syscalls
              sysfs.c        /sys: kernel/ class/graphics/fb0/ firmware/ devices/system/cpu/ power/
net/          core.c         packets, interfaces, routes, loopback, the netd thread, boot options
              arp.c ipv4.c   Ethernet and ARP, IPv4 (fragments, reassembly), ICMP
              udp.c tcp.c    UDP, raw and ping sockets; TCP
              socket.c       AF_INET socket calls and SIOC* interface ioctls
              dhcp.c proc.c  DHCP client; /proc/net
kernel/       kernel.c       initialisation sequence, init and worker threads
              syscall.c      syscall table (Linux x86_64 numbers), handlers, ENOSYS stubs
              uaccess.c      copy_from_user/copy_to_user and friends (checked, fault-safe)
              futex.c        futexes: hashed wait queues, requeue, wake-op, robust lists
              klog.c         dmesg ring buffer, panic screen
              report.c       REPORT.TXT / VBIOS.ROM on the stick's FAT report volume
              gpu.c          PCI -> NVIDIA probe glue, 'gpu' command, report section
              display.c      display heads: boot frame buffer + DISPI adapters at their EDID mode
              tty.c          one terminal per monitor: draw_char/scroll_screen per TTY, cell
                             model, per-TTY lock, cursor blink on every monitor, focus hotkeys,
                             per-TTY key queues, Linux-console escape sequences, UTF-8,
                             kush's /dev/tty line discipline
              vt.c           /bin/sh on every terminal: console pty, key and output threads,
                             login sessions (getty/ttyN), respawn
              kush.c         kush shell (one instance per monitor): line editor, quoting,
                             redirection, $?, built-ins
              coreutils/     pwd cd ls mkdir rmdir touch rm cp mv cat head tail grep echo
                             chmod chown date uname neofetch (+ cu_lib.c helpers, table.c)
              ramdisk.c      Multiboot2 modules as read-only /dev/ramN (the ISO's boot volume)
              storage.c      PCI -> AHCI and NVMe glue, boot-time MBR verify, /dev/sdX, nvmeN,
                             NVMe self-test and shutdown
              usbhost.c      PCI -> xHCI glue, key injection, mouse pointer, usbd thread, /proc/usb,
                             firmware handoff, mass storage -> block devices
              time.c         HPET (ACPI) / PIT-calibrated TSC clock usable before interrupts
              block.c        block device registry, MBR/GPT partitions, /proc/partitions
              bootvol.c      boot volume discovery (option A/B), root policy, failover
              recovery.c     diagnostic trace + recovery console (keyboard and COM1)
lib/crc32.c                  CRC-32 (GPT, volume header, payload)
tools/mkusbimg.py            bootable USB stick image builder
ports/xlibre/                X server port: sysroot/server build scripts, hw-kestrel DDX, patches,
                             xdemo test client
ports/icewm/                 IceWM build script and Kestrel configuration
ports/toolchain/             musl-g++ (C++ with libsupc++), pkg-config and CMake wrappers
ports/busybox/               BusyBox build (/bin/sh and the command-line tools)
ports/xterm/                 xterm build and its musl patch
rootfs/                      contents of the boot volume payload
ui/bootmgr.c                 boot manager, F8 screen, tools
include/kernel/posix.h       errno, O_* flags, struct stat, dirent64, utsname (Linux ABI layout)
include/kernel/usyscall.h    int 0x80 wrappers used by init
```

## System calls

The calling convention is the Linux x86_64 one: the call number goes in `rax`, the arguments in `rdi rsi rdx r10 r8 r9`, and the result (a negative errno on failure) comes back in `rax`.

Calls can enter through two paths:
- `int $0x80`, used by the in-kernel shell and tools.
- `syscall`, used by ring-3 programs.

Every pointer a call receives is checked to lie in the user half and copied through `copy_from_user`/`copy_to_user` (`kernel/uaccess.c`): a bad or unmapped pointer gives `-EFAULT`, never a kernel fault. Structures with pointers inside (`msghdr`, iovecs, `epoll_event`, control messages) are copied into the kernel once and only that copy is used, so another thread cannot change them between the check and the use.

**Implemented** (by area):
- **Files:** read, write, readv, writev, pread64, pwrite64, open, openat, close, lseek, stat, lstat (as stat: no symbolic links), fstat, newfstatat, access, faccessat, getdents64, ioctl, fcntl (`F_DUPFD`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL`, `F_ADD_SEALS`/`F_GET_SEALS`), dup, dup2, dup3, ftruncate, truncate, fsync, fdatasync, flock (advisory, per open file description), the twelve `*xattr` calls (no file system stores extended attributes: lists are empty, reads give `ENODATA`, writes `ENOTSUP`), sendfile, readlink/readlinkat (`/proc/self/exe`, `/proc/self/fd/N`), mkdir, mkdirat, rmdir, unlink, unlinkat, rename, chmod, fchmod (accepted), chown, utimensat, chdir, getcwd, umask.
- **Waiting for events:** poll, ppoll, select, pselect6 (with signal masks), epoll_create, epoll_create1, epoll_ctl, epoll_wait, epoll_pwait, eventfd, eventfd2, timerfd_create, timerfd_settime, timerfd_gettime, signalfd, signalfd4, pipe, pipe2.
- **Sockets (`AF_UNIX`):** socket, socketpair, bind, listen, connect, accept, accept4, sendto, recvfrom, sendmsg and recvmsg (with `SCM_RIGHTS`), shutdown, getsockname, getpeername, setsockopt (accepted, ignored), getsockopt (`SO_PEERCRED`, `SO_TYPE`, `SO_ERROR`, buffer sizes).
- **Sockets (`AF_INET`):** the same calls for TCP, UDP, raw ICMP and ping sockets, with `MSG_PEEK`/`MSG_DONTWAIT`/`MSG_TRUNC`/`MSG_NOSIGNAL`, and the `SOL_SOCKET`, `IPPROTO_IP` and `IPPROTO_TCP` options programs use (see Networking). `ioctl` handles `FIONREAD` and the `SIOCGIF*`/`SIOCSIF*`/`SIOCADDRT`/`SIOCDELRT` interface calls.
- **Memory:** brk, mmap (anonymous, private and shared file mappings, `MAP_FIXED`, `MAP_FIXED_NOREPLACE`, `MAP_POPULATE`), munmap, mprotect, mremap, madvise (`MADV_DONTNEED`), mincore, msync, memfd_create (with seals).
- **Processes and threads:** fork, vfork, clone (processes and threads), execve, exit (the thread), exit_group (the process), wait4, getpid, gettid, getppid, set_tid_address, set_robust_list, get_robust_list, futex, arch_prctl (`ARCH_SET_FS`/`GET_FS`/`SET_GS`/`GET_GS`), sched_yield, sched_getaffinity, sched_setaffinity, setsid, setpgid, getpgid, getpgrp, getsid, prctl (`PR_SET_NAME`/`PR_GET_NAME`).
- **Signals:** kill, tkill, tgkill, rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigreturn, rt_sigsuspend, pause, alarm, getitimer, setitimer (`ITIMER_REAL`).
- **Identity and limits:** getuid, getgid, geteuid, getegid, setuid, setgid, setreuid, setregid, setresuid, setresgid, getresuid, getresgid, getgroups, setgroups (one uid and gid per process; only root may change them), getrlimit, setrlimit, prlimit64 (fixed limits; 1024 descriptors).
- **Time and system:** nanosleep, clock_gettime and clock_getres (`CLOCK_REALTIME` from the RTC; `CLOCK_MONOTONIC`, `_RAW`, `_COARSE` and `CLOCK_BOOTTIME` from the HPET/TSC), gettimeofday, times, getrusage, sysinfo, uname, getrandom, sync, syncfs, reboot.

**Power:** `poweroff`, `reboot` and `halt` work from any shell (as root). BusyBox asks init with a signal to pid 1, and the kernel plays that part; `reboot(2)` and the `-f` forms go straight to `kernel/power.c`. Power-off uses ACPI S5 (PM1 control ports from the FADT, the `\_S5_` sleep type from the DSDT), so it works on real machines, with the QEMU/Bochs/VirtualBox ports as a fallback. Restart tries the FADT reset register, port `0xCF9`, the keyboard controller, then a triple fault. Storage is flushed and the other CPUs are stopped first.

`cat /proc/syscalls` lists every call with its status and call count.

## User programs (ring 3)

Kestrel runs x86_64 ELF programs in ring 3, statically or dynamically linked (`proc/exec.c`, `mm/vm.c`, `mm/uvm.c`):
- **Address spaces:** each process has its own PML4. The kernel half (`PML4[0..127]`, 0–64 TiB, which holds the identity map and all MMIO) shares its page tables with every process and is supervisor-only. The user half is `0x4000_0000_0000`–`0x7fff_ffff_f000`: programs load at its start, the ELF interpreter at `0x5000_0000_0000`, `mmap` grows from `0x6000_0000_0000`, and an 8 MiB stack ends at `0x7fff_ff00_0000`. (The top page stays unmapped: SYSRET to a non-canonical address would fault in ring 0 on Intel CPUs, CVE-2012-0217, and the syscall exit path also checks the return address.)
- **Memory** is a sorted list of regions (`struct vma`) and nothing is mapped until it is touched: page faults fill pages in (zeroed, from a file, or from a shared memory object). `fork` shares every page copy-on-write: private writable pages become read-only in both processes (PTE bit 9 marks them), frames are reference counted, and the first write copies the page, or simply takes it back when nobody else has it. Page-table entries mirror the region's protection exactly: user/supervisor, writable only when not shared copy-on-write, and NX (`EFER.NXE`) on everything not executable.
- **Out of memory:** a fault that cannot get a frame asks the OOM killer to end the largest process, waits until its memory is back, and retries; only if nothing helps does the faulting process get SIGKILL. The kernel never dereferences a failed allocation.
- **Programs:** ET_EXEC linked inside the user half, ET_DYN (PIE) at the image base, or with `PT_INTERP` through musl's dynamic linker (`/lib/ld-musl-x86_64.so.1`), which then loads shared libraries with `mmap`. The ELF loader trusts nothing in the file: header sizes, segment counts, offsets inside the file, address ranges inside the user half, alignment and overlaps are all checked. Text is mapped read/execute, data read/write; a segment that is both writable and executable is refused (boot with `kestrel.allow_wx=1` to allow it), and the stack is not executable unless `PT_GNU_STACK` asks for it.
- **Scripts:** a file starting with `#!interpreter [arg]` runs that interpreter with the script's path, as on Linux (`/boot/bin/startx` is one).
- **Start-up:** the initial stack follows the System V / Linux layout (argc, argv, envp, and auxv with `AT_PHDR`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` and more), so a musl `crt1` starts unchanged.
- **Per-thread state:** SSE state (`fxsave`), the TLS base (`FS_BASE`) and the GS base are switched with the thread.
- **Faults** in ring 3 become the signal Linux sends (SIGSEGV with `SEGV_MAPERR`/`SEGV_ACCERR`, SIGBUS, SIGILL, SIGFPE with its `FPE_*` code, SIGTRAP) with `si_addr` and the faulting registers, so a handler can run (a JIT or a garbage collector relies on that); without one the process ends with status 128 + signal. The kernel keeps running either way.
- **Pointers:** every user pointer goes through the checked, fault-safe copy routines (see "System calls"); a bad one gives `-EFAULT`.

`user/hello.c` is a freestanding self-test of all of this, installed as `/boot/bin/hello` on the USB stick:
- From the shell: `run hello` (bare names are looked up in `/boot/bin`).
- Unattended: boot with `kestrel.exec=/boot/bin/hello,arg1,arg2`. Output goes to the kernel log (serial and `REPORT.TXT`), followed by the exit status: 42 means every check passed.
- Several programs: `+` separates them, and an entry of the form `Ns` waits N seconds before starting the rest, e.g. `kestrel.exec=/boot/bin/Xkestrel,:0+2s+/boot/bin/icewm`.
- Environment: programs started by the kernel get `PATH=/bin`, `HOME=/`, `TERM=kestrel`, `DISPLAY=:0` and `SHELL=/bin/sh`. `/bin` shows the boot volume's `/boot/bin` (a bind mount).
- Fault isolation: `hello crash` writes to kernel memory and must end with "killed (signal 11)".

**musl libc:** programs linked with the stock x86_64 musl (`musl-gcc -static-pie`) run unchanged, because Kestrel uses the Linux syscall numbers and ABI. `user/libctest.c` (installed as `/boot/bin/libctest`) checks 35 things a ported program relies on: stdio, malloc (brk and mmap paths), floating-point formatting, files and directories, time, pipes, `poll` timeouts, `select`, `O_NONBLOCK`, `dup`, EOF/`POLLHUP` and `getrandom`. Exit status 0 means every check passed. A ring-3 program that calls an unimplemented syscall is named in the kernel log (first three calls per number).

**Pipes and poll:** pipes have a 16 KiB buffer and follow POSIX blocking rules; a write to a pipe without readers raises SIGPIPE and returns `-EPIPE`. `poll`, `select` and `epoll_wait` sleep until something changes: every wake-up in the kernel advances a sequence number and wakes the pollers, and a poller sleeps only if the number has not moved since it looked, so no event slips through between checking and sleeping. A watched timerfd ends the sleep exactly at its expiry. Files and devices without their own poll hook are always ready.

**Unix-domain sockets** (`fs/unixsock.c`): `AF_UNIX` / `SOCK_STREAM`, the way X11 connects clients locally, plus `SOCK_SEQPACKET` and `SOCK_DGRAM` socket pairs.
- **Names:** a path in the file system (a socket node, e.g. `/tmp/.X11-unix/X0`) or Linux's abstract namespace (`sun_path[0] == 0`), which xtrans tries first.
- **Connecting:** `connect` completes at once by queueing a socket on the listener, and `accept` hands it out.
- **Data:** each direction has a 64 KiB buffer, with blocking, `O_NONBLOCK`/`MSG_DONTWAIT`, `MSG_PEEK`, EOF, `EPIPE`, `FIONREAD` and `poll` readiness as on Linux.
- **Credentials:** `SO_PEERCRED` reports the pid/uid/gid of the process that created the peer.
- **Messages:** `SOCK_SEQPACKET` (sockets and pairs) and `SOCK_DGRAM` (pairs) keep record boundaries; a short buffer truncates the record and sets `MSG_TRUNC`.
- **Descriptor passing:** `SCM_RIGHTS` on every type. The descriptors in flight are counted references to the open files; they arrive with the bytes they were sent with (a stream read never runs past them), `MSG_CMSG_CLOEXEC` works, and without room for them they are closed and `MSG_CTRUNC` is set. Up to 253 per message.
- **Not yet:** unconnected datagram sockets (`sendto` with an address), and a garbage collector for sockets sent through themselves (in-flight descriptors are bounded per socket instead).

`user/socktest.c` (`/boot/bin/socktest`) checks all of this, in one process or as two:
- `socktest`: 20 checks in one process.
- `socktest server` with `socktest client`: a real server and client, started together with `kestrel.exec=/boot/bin/socktest,server+/boot/bin/socktest,client` (`+` separates programs).

**Processes** (`proc/process.c`, `include/kernel/process.h`):
- **fork:** the child shares every page copy-on-write. Descriptors, the working directory, signal handlers and the signal mask are inherited.
- **Threads:** `clone` shares what its flags say: memory (`CLONE_VM`), the descriptor table (`CLONE_FILES`), cwd and umask (`CLONE_FS`), signal actions (`CLONE_SIGHAND`), and with `CLONE_THREAD` the process itself, which is how musl's `pthread_create` works. Every thread has its own tid, kernel stack, TLS base (`CLONE_SETTLS`), signal mask and pending set. `getpid` is the process, `gettid` the thread; `exit` ends a thread, `exit_group` and fatal signals end the process, and `execve` from any thread first ends the others (a non-leader takes over the process id). `CLONE_CHILD_CLEARTID` and robust futex lists are handled at thread exit, so `pthread_join` and robust mutexes work.
- **Futexes** (`kernel/futex.c`): wait queues in 256 hashed buckets, keyed by address space and address (private) or memory object and offset (shared mappings, across processes). The futex word is read again under the bucket lock before sleeping, so a wake-up cannot be lost. WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP, WAIT_BITSET/WAKE_BITSET with relative or absolute timeouts; priority-inheritance futexes return `ENOSYS`.
- **vfork and `clone(CLONE_VM | CLONE_VFORK)`** (musl's `posix_spawn`): the child borrows the parent's address space, and the parent sleeps until the child calls `execve` or exits.
- **execve:** builds the new image in a fresh address space. If loading fails, the old image is untouched and the call returns an error. On success, `O_CLOEXEC` descriptors close, caught signals revert to their defaults, and the FPU state and TLS base are reset.
- **wait4:** a parent waits for a child, a process group or any child, with `WNOHANG`. A process's exited children stay zombies until it collects them, unless SIGCHLD is ignored or has `SA_NOCLDWAIT`. Orphans are reaped by the kernel.
- **setsid, setpgid, getpgid, getsid:** sessions and process groups.

**Signals** follow Linux x86_64:
- **Handlers and masks:** `rt_sigaction` with `SA_RESTART`, `SA_NODEFER`, `SA_RESETHAND` and `SA_NOCLDWAIT`; `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend` and `pause`.
- **Sending:** `kill` to a process, a process group or everyone; `tkill`/`tgkill`, which `raise()` and `abort()` use.
- **Kernel-raised signals:** SIGCHLD to the parent when a child exits, and SIGPIPE on writes to a pipe or socket nobody reads, unless `MSG_NOSIGNAL`.
- **Handlers run on Linux's frame:** a handler gets the standard `rt_sigframe` (siginfo, a ucontext with the saved registers, and the `fxsave` FPU state) and returns through musl's `SA_RESTORER` trampoline into `rt_sigreturn`.
- **When delivery happens:** on every return to ring 3 (after a system call or an interrupt), so a process spinning in user mode can also be killed.
- **Blocking calls:** pipe and socket I/O, `accept`, tty reads, `/dev/kinput`, `wait4`, `poll`, `select`, `nanosleep` and `sigsuspend` stop waiting when a signal arrives. They return EINTR to a handler, or restart under `SA_RESTART`, as on Linux.
- **Default actions:** terminate, or ignore for SIGCHLD, SIGCONT, SIGURG and SIGWINCH. There is no job control: stop signals are ignored.
- **Threads:** the mask and a pending set are per thread; `kill`, the terminal, SIGCHLD and SIGALRM go to the process and are taken by a thread that does not block them; `tgkill` and CPU faults go to one thread.
- **CPU faults** call the handler if there is one (see "User programs"); a blocked or ignored fault signal still kills.

`user/proctest.c` (`/boot/bin/proctest`) checks all of this in 31 tests: fork isolation, pipes across fork, 16 concurrent children, execve (also of a missing file), posix_spawn, vfork, setsid, handlers, blocked and ignored signals, FPU state across a handler, SIGCHLD, SIGTERM/SIGKILL (also against a child spinning in user mode), EINTR versus `SA_RESTART`, sigsuspend, interrupted nanosleep, SIGPIPE, `alarm` and periodic `setitimer`, `readlink("/proc/self/exe")`, and abort.

Not done yet: job control (stop/continue).

`cat /proc/syscalls` lists every call with its status and call count.

## X11: the XLibre server (`ports/xlibre/`)

[XLibre](https://github.com/X11Libre/xserver), the X server forked from xorg-server, runs on Kestrel as an ordinary ring-3 musl program called `Xkestrel`. Clients connect over `/tmp/.X11-unix/X0` (or its abstract-namespace twin) and draw. The server's portable parts (dix, mi, fb, render, xkb, the extensions) are built unchanged. Everything Kestrel-specific is either the DDX in `hw/kestrel/` or a configure-time choice; the XLibre sources themselves get only two small patches.

**Building** (on the build host, apart from the kernel):

```sh
make xlibre XLIBRE_SRC=<checkout of X11Libre/xserver> XLIBRE_DEPS=<library tarballs>
make usbimg       # stages bin/Xkestrel, bin/xdemo and the keymap into /boot
```

`ports/xlibre/build.sh` does four things:
1. Builds the client and server libraries as static musl libraries into `build/xsysroot`: zlib, xorgproto, xtrans, libXau, libXdmcp, xcb-proto, libxcb, libX11, libxkbfile, libfontenc, libXfont2, pixman and a small `libsha1`. Ubuntu's `apt-get source --download-only` tarballs work.
2. Applies `ports/xlibre/patches` and adds the `kestrel` meson option and `hw/kestrel`.
3. Cross-builds the server with musl (`kestrel-cross.txt.in`).
4. Builds the test client `xdemo` and precompiles the default keymap with the host's `xkbcomp`.

Host packages: `musl-tools meson ninja-build pkg-config x11-xkb-utils xkb-data`.

**How the OS-dependent layers map onto Kestrel:**

| XLibre needs | Kestrel provides |
|---|---|
| Listening and client sockets (xtrans, `os/connection.c`) | `AF_UNIX` stream sockets, `SO_PEERCRED` |
| Event loop (`os/ospoll.c`) | `poll(2)`; the epoll backend is configured out |
| Frame buffer (DDX) | `/dev/fbN`, one per monitor: `KFB_GET_INFO`, then `KDSETMODE KD_GRAPHICS` to take the head from the text terminal, then `KFB_BLIT` to copy damaged rectangles from the server's shadow buffer |
| Keyboard and mouse (DDX) | `/dev/kinput`: evdev-style records with Linux key, button and axis codes, so X keycode = code + 8 and the stock evdev keymap applies. `ioctl(KINPUT_GRAB)` takes PS/2 and USB keyboards and USB mice away from the terminals until the file is closed |
| Running `xkbcomp` for each keymap | the precompiled `/boot/share/X11/xkb/compiled/kestrel-default.xkm` (us layout, pc105 model), copied into place by patch 0001 |
| `SIGALRM` smart scheduler, `link()` lock file | configured out (patch 0002); the scheduler falls back to time checks in the dispatch loop |
| Fonts | the built-in `fixed` and `cursor` fonts |
| MIT-SHM, GLX/DRI, XDMCP, udev, input thread | disabled |

**The DDX** (`hw-kestrel/kestrel.c`) has two halves:
- **Screens:** one X screen per monitor (`-screens N` limits the count), each a 24-bit TrueColor fb screen with RENDER. A damage listener on each root window collects changes, and a wrapped screen BlockHandler pushes them to the monitor before the server sleeps, after the software cursor has been drawn back.
- **Input:** a read notification on `/dev/kinput` feeds keys, buttons, relative motion and the wheel (as buttons 4 and 5) into the dix event queue.

Exiting the server returns its monitors to their text terminals.

**Trying it:**
- Interactively: `run Xkestrel :0` in a terminal.
- Unattended, with the test client: boot with `kestrel.exec=/boot/bin/Xkestrel,:0+/boot/bin/xdemo`. xdemo opens a window, draws rectangles, a line and text, redraws every second, and logs its key and button events to the kernel log.
- In QEMU: this has been checked with an xHCI keyboard and mouse. The window appears, the pointer moves, and clicks and keys (including Shift) reach the client with the expected evdev keycodes.

As the X protocol requires, the pointer stays hidden until some client defines a cursor; normally that is the window manager's job, and xdemo does it for its test.

**Not yet:** more clients, MIT-SHM (needs shared memory), keymap choice beyond the precompiled one, PS/2 mice, hardware cursors and acceleration.

## IceWM (`ports/icewm/`)

[IceWM](https://ice-wm.org/) 3.4 runs on Xkestrel as the window manager. It draws the window frames and title-bar buttons (moving a window by its title bar has been checked; resizing, maximising and closing are untested). Its taskbar has the start menu, workspaces, a task list, a CPU graph from `/proc/stat`, a memory meter from `/proc/meminfo` and a clock.

**Building**, after `make xlibre`:

```sh
make icewm ICEWM_SRC=<unpacked icewm 3.x release>   # e.g. Ubuntu's icewm_3.4.5.orig.tar.xz
make usbimg      # stages bin/icewm, share/icewm, etc/icewm into /boot
```

**C++ on musl** (`ports/toolchain/`): IceWM is C++11, and there is no C++ standard library built for musl on the build host. `install.sh` builds `musl-g++` from what is there:
- the host g++ with musl's C headers and start files (the `musl-gcc` specs);
- the host's libstdc++ *headers*, for header-only parts such as `<utility>`, `<algorithm>`, `<new>` and `<typeinfo>`;
- the host's `libsupc++`, for `new`/`delete`, RTTI, `dynamic_cast`, static-local guards and pure virtual calls;
- `libsupcxx-compat.c`, for the four glibc-only symbols that `libsupc++` references.

The limits: C++ exceptions cannot be caught, and the compiled parts of libstdc++ (iostreams, `std::string`, `<regex>`, threads) are not available. IceWM's window manager needs none of them. `install.sh` also writes `kestrel-pkg-config` (the sysroot, static link flags) and a CMake toolchain file.

**Configuration** (`ports/icewm/build.sh`):
- X core fonts, XPM and PNG images.
- No Xft/fontconfig, i18n, session management, RandR, Xinerama, FriBidi, freedesktop menus or sound.
- `/boot/etc/icewm/prefoverride` sets every font to the server's built-in `fixed` font and turns off the network, battery and mail applets.
- `/boot/etc/icewm/programs` and `menu` hold the Kestrel menus.
- The sysroot gained libXext, libXrender, libXfixes, libXdamage, libXcomposite, libXpm and libpng.

**Kernel support it needed:** `select`/`pselect6`, `wait4` (no children), `/proc/stat`, and execute bits on boot-volume files (IceWM only lists programs it can see are executable).

**Trying it:**
- `kestrel.exec=/boot/bin/Xkestrel,:0+2s+/boot/bin/icewm+3s+/boot/bin/xdemo`.
- Checked in QEMU: IceWM frames the xdemo window, Ctrl+Esc opens the start menu, and dragging the title bar moves the window.
- Starting programs: choosing "X demo" in the menu runs IceWM's own `fork()` + `execvp()`, and the new xdemo window appears.
- Closing: the title-bar close button sends `WM_DELETE_WINDOW`; xdemo exits, and IceWM collects it through its SIGCHLD handler and `waitpid`.

**Not yet:** icewmbg (backgrounds), icewm-session, more programs for the menus, and fonts other than `fixed`.

## Terminals: pseudo-terminals, a shell, xterm

**Pseudo-terminals** (`fs/pty.c`) work like Linux's Unix98 ptys, so musl's `posix_openpt`/`grantpt`/`unlockpt`/`ptsname`, `openpty`, `tcgetattr`/`tcsetattr`, `isatty` and `ttyname` work unchanged.
- **Devices:** opening `/dev/ptmx` gives a new master; its slave is `/dev/pts/N` (16 pairs). The text terminals use one pair each, from the top (`/dev/pts/15` down): their slave is `/dev/ttyN` (see "The shell on the terminals").
- **Line discipline:** canonical editing (erase, word erase, kill, EOF), echo with `^X` for control characters, `ICRNL`/`INLCR`/`IGNCR`, output `OPOST`/`ONLCR`, and raw mode with `VMIN`/`VTIME`.
- **Controlling terminal:** set with `TIOCSCTTY`, or when a session leader opens a slave. `/dev/tty` then opens it; `TIOCGPGRP`/`TIOCSPGRP` set the foreground process group.
- **Signals:** Ctrl+C, Ctrl+\ and Ctrl+Z send SIGINT, SIGQUIT and SIGTSTP to the foreground group (SIGTSTP is ignored: no job control). `TIOCSWINSZ` sends SIGWINCH. Closing the master sends SIGHUP to the session.
- **Hang-up:** after the slave side closes, master reads fail with EIO (as on Linux).
- **Test:** `user/ptytest.c` checks all of this in 18 tests.

**A shell** (`ports/busybox`): BusyBox 1.36 provides `/bin/sh` (ash) and about 400 tools in one 1.2 MB static-PIE musl binary. It is the login shell on every text terminal.
- **Install:** every applet is a hard link to it. The boot volume keeps the links: tarfs supports hard links, and `make usbimg` preserves them. `/boot/bin` is bound on `/bin`, so `/bin/sh` is where scripts expect it.
- **Building:** `make busybox BUSYBOX_SRC=<unpacked busybox release>`.
- **Test:** `user/shtest.sh` (`kestrel.exec=/bin/sh,/bin/shtest.sh`) checks pipes, redirection, background jobs, `kill`, traps, here-documents, `/proc`, `ps` and `killall` in 22 tests.
- **One exec** of a BusyBox tool takes about 40–80 ms in QEMU; the binary is read from the boot volume each time.

**`/proc/PID/`** has `stat`, `status`, `cmdline` and `comm` in Linux's formats; `/proc/self/` is the caller. That is what `ps`, `top`, `pidof` and `killall` read.

**xterm** (`ports/xterm`): xterm 390 built against Xt, Xaw, Xmu, SM and ICE, added to the musl sysroot, and ncurses.
- **Configuration:** core fonts, no FreeType, luit, setuid, utmp helper, input methods or Tek4014.
- **Install:** resources go to `/boot/share/X11/app-defaults/XTerm`; the terminfo entries for `xterm*`, and `linux` for the text terminals, go to `/boot/share/terminfo`.
- **Patch:** one patch makes xterm call `setsid()` with musl too. glibc implies `_POSIX_SOURCE`, which xterm tests for.
- **Configure:** `HAVE_GRANTPT_PTY_ISATTY` is set because configure cannot run its test program when cross-compiling.

```sh
make xlibre ...                                  # X server and the X libraries (sysroot)
make xterm XTERM_SRC=<unpacked xterm release>
make busybox BUSYBOX_SRC=<unpacked busybox release>
make usbimg
```

**Trying it:** choose the desktop entry in the boot menu (`kestrel.exec=/bin/startx`). Or start the pieces yourself with `kestrel.exec=/bin/Xkestrel,:0+2s+/bin/icewm` and pick XTerm in IceWM's start menu. In a text terminal, `startx` does the same.

**Checked in QEMU** with an xHCI keyboard and mouse: commands typed into the shell run, including pipes, `ps`, `top` and `stty size`; Ctrl+C interrupts a running `sleep`; `exit` closes the window, and IceWM reaps xterm.

**Not yet:** fonts other than `fixed`, job control (Ctrl+Z, `fg`/`bg`), and a writable disk to keep files across reboots; `/tmp` and the root file system are in RAM.

## Multiprocessor support and robustness

**Processors** (`arch/x86_64/apic.c`, `smp.c`, `trampoline.asm`). The ACPI MADT lists the CPUs and IO APICs. The 8259 PICs are masked; the PS/2 keyboard comes through the IO APIC (honouring the MADT's interrupt source overrides), and every CPU runs its own local APIC timer at 1 kHz, calibrated against the HPET/TSC clock. The other CPUs are started with INIT and STARTUP IPIs into a real-mode trampoline at `0x8000`, which switches to long mode on the kernel's page tables; each gets its GDT/TSS, IST stacks, SYSCALL MSRs, the boot CPU's CR0/CR4/PAT, and an idle thread. Up to 16 CPUs. Boot options: `kestrel.smp=0` (or `maxcpus=1`) keeps one CPU, `kestrel.noapic` stays on the PIC and PIT. Without an MADT the kernel does the same by itself.

**Per-CPU data.** In the kernel, the GS base points at the CPU's `struct percpu` (`include/kernel/smp.h`); entries from ring 3 run `SWAPGS`, and NMI, #DF and #MC (which can arrive between `SYSCALL` and its `SWAPGS`) decide from the GS base itself. The running task is read with a single GS-relative load, so a thread that is preempted and moved to another CPU never reads the wrong one.

**Locking.** The kernel's own critical sections were written for one CPU ("interrupts off"). On several CPUs a recursive big kernel lock keeps that true: a CPU holds it whenever it executes kernel code, so kernel code runs on one CPU at a time while user code runs on all of them. The lock depth is saved per task across context switches, and the idle loop and pure computation (kworker/0) let go of it. This is a transitional design: system-call-heavy work does not scale across CPUs yet. Futex buckets and the other short spinlocks are already per-object, with interrupts off while held, and are the starting point for finer locking.

**TLB shootdowns.** When a mapping loses rights or goes away, every CPU that has that address space loaded drops the translation (`invlpg`, or a CR3 reload for large ranges) through an IPI, and the call waits for each to acknowledge before a frame is freed or reused. Waiting for the kernel lock services pending requests, so a shootdown can always complete. Changes to the kernel's identity map (guard pages, cache types) go to every CPU.

**Faults and crashes.**
- **User memory:** every copy to or from user memory goes through routines with exception-table fixups: a bad pointer is `-EFAULT`, even in the middle of a copy.
- **Exception isolation:** #PF, #GP, #DE, #AC and the rest are separate IDT entries. In ring 3 they become signals. #DF, NMI and #MC run on their own IST stacks, so a kernel stack overflow (which hits a guard page below every kernel stack) is still reported.
- **Kernel faults:** the report has the registers, CR0–CR4, the code bytes and a symbolized backtrace. It goes to the serial port, the kernel log and the panic screen, and other CPUs are stopped with an NMI.
- **Crash test:** `kestrel.crashtest=stack|null|ud` exercises this at boot.

**Tests** (each a musl program in `/boot/bin`, exit status 0 = all passed; `kestrel.exec=` runs them at boot):

| Program | What it checks |
|---|---|
| `faulttest` | 38 checks: faults to signals with the right `si_code`/`si_addr`, handlers that recover, `-EFAULT` for bad pointers in every kind of call, kernel stays up |
| `vmtest` | 43 checks: demand paging, copy-on-write, NX, mprotect, file and shared mappings, memfd and seals, mremap, madvise, mincore, brk, OOM killer |
| `dyntest` | a dynamically linked program and shared library through musl's ld.so |
| `threadtest` | 41 checks: pthreads, TLS, mutex/condvar/semaphore contention, every futex operation and timeout, process-shared futexes, signals and threads, exit/exit_group, fork and execve from threads, robust mutexes, 450 thread lifecycles |
| `eventtest` | 71 checks: epoll (level, edge, one-shot, nesting, fairness), eventfd, timerfd, signalfd, poll/select sleeps, ppoll masks, `SCM_RIGHTS`, record sockets, POSIX shared memory |
| `smptest` | CPU count, real parallel speed-up, TLB shootdown (a thread on another CPU loses an unmapped page at once), thread churn across processes, concurrent system calls |

Together with `libctest`, `proctest`, `ptytest`, `socktest` and the shell script test they pass on 1 and on 4 CPUs (QEMU, `-smp 4 -accel tcg,thread=multi`).

## Files on disk: ext2, symbolic links, a real root

**Root file system** (`fs/rootfs.c`). At boot `/` is an in-memory tree with `/dev`, `/proc`, `/sys` and the boot volume on `/boot`. If any disk has an ext2 volume labelled `kestrel-root`, it becomes `/`:
- The in-memory mounts move into it, and `/tmp` gets a fresh tmpfs.
- The `/boot/bin` → `/bin` and `/boot/lib` → `/lib` binds are dropped when the volume has its own `/bin` and `/lib`.
- Files the system needs in `/etc` (hostname, passwd, group, profile, motd, os-release) are written once if the volume lacks them.
- Only the volume chosen this way is ever written. Other Linux file systems on the machine are left alone.

Boot options:
- `kestrel.rootfs=auto` (default): the volume labelled `kestrel-root`.
- `kestrel.rootfs=LABEL=<name>`: the ext2 volume with that label.
- `kestrel.rootfs=<device>`, e.g. `sata0p2`: that partition or disk.
- `kestrel.rootfs=none`: stay on the in-memory root.
- `kestrel.rootro=1`: mount the root read-only.

The USB image built by `make usbimg` has such a partition: `ROOTDISK_MB` MiB (default 96), made by `mke2fs -d` from the same tree as the boot volume. `ROOTDISK_FLAGS` passes extra `mke2fs` options, e.g. `-b 1024 -I 128`.

**ext2** (`fs/ext2.c`), read and write.

What it supports:
- Revision 0 and 1, block sizes of 1, 2 and 4 KiB, any inode size.
- Direct, indirect, double- and triple-indirect blocks, and sparse files.
- File types in directory entries, sparse superblocks, large files, flexible block groups.
- ext3 volumes whose journal is empty. The journal itself is left alone.

How it handles everything else:
- Directories with an htree index (`dir_index`) are read linearly. A directory Kestrel changes loses its index flag, as `e2fsck` expects from a kernel without htree support.
- Unknown read-only-compatible features (metadata checksums, huge files, ...) mount read-only.
- Extents (ext4), 64-bit block numbers, journals that need recovery and compression are refused.

How it works:
- **Inodes:** hard links share one in-memory copy of the raw on-disk inode, so fields Kestrel does not interpret survive a write.
- **Write-back cache:** metadata and data go through a block cache (4 MiB per volume). A kernel thread writes dirty blocks every 5 seconds; `sync`, `fsync`, `poweroff` and `reboot` write everything.
- **Clean flag:** the superblock is marked clean only at shutdown, like an unmount on Linux, so `e2fsck` checks a volume that was not shut down cleanly.
- **Untrusted disk:** nothing on it is trusted. Block numbers are range-checked before any I/O, and directory records are validated. A corrupt structure gives `EIO`, not a kernel fault.
- **Free blocks:** new blocks come zeroed, so no stale data reaches a file, and an unlinked file that is still open keeps its blocks until the last close.

**VFS** (`fs/vfs.c`):
- **Names and links:** symbolic links (at most 40 per lookup, then `ELOOP`), 255-byte names and 1023-byte paths.
- **`*at` calls:** they take real directory descriptors (`AT_FDCWD`, `AT_EMPTY_PATH`, `AT_SYMLINK_NOFOLLOW`/`FOLLOW`, `AT_REMOVEDIR`, `RENAME_NOREPLACE`).
- **Operations:** `O_NOFOLLOW`, `O_PATH`, hard links, `statfs`, and `chown`/`chmod`/`utimensat` on every kind of path.
- **File system interface:** a disk file system plugs in through `create`/`link`/`remove`/`rename`/`setattr`/`readlink`/`evict` hooks and a per-mount `sync` and `statfs`.
- **Directory offsets:** `getdents` offsets are stable per entry, so a program that deletes entries while reading a directory (`rm -r`) sees each of them once.

**Tests:**
- `fstest` (in `/bin` on the root volume) runs 53 checks:
  - files in every block-map class, sparse files, truncation, `O_APPEND`;
  - fast and slow symbolic links, link loops, `O_NOFOLLOW`;
  - hard links, renames of files and directories (and `..`), 400-entry directories;
  - unlinked open files, `chmod`/`chown`, `flock` and the extended-attribute calls;
  - free space returning exactly to where it started.
- `fstest -r` writes files, restarts the machine, and checks them after the reboot.
- After each run, `e2fsck -fn` on the image finds no errors. This holds with 4 KiB blocks and 256-byte inodes, and with 1 KiB blocks and 128-byte inodes.

**Alpine Linux as the root.** Alpine's own minirootfs (3.24.2, from the official `alpinelinux/docker-alpine` repository) runs unmodified on Kestrel. Put it on an ext2 partition, label it, and boot with `kestrel.rootfs=LABEL=<label>`:
- Alpine's `/bin/sh`, BusyBox and musl dynamic loader start, and its tools (`ps`, `df`, `free`, `ping`, `wget`, `sha256sum`, ...) work.
- `apk` 3.0.8 works on the installed system: `info`, `list`, `policy`, `stats`, `audit`, and `del`, which locks the database with `flock`, rewrites it and removes files; `e2fsck -fn` finds the volume clean afterwards.
- `apk add` and `apk update` need Alpine's package servers, which the test environment cannot reach, so installing packages from the network has not been tried.
- Programs start with `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, as on a Linux distribution.
- A whole disk without a partition table is mounted read-only; a partition is mounted read-write.

**Not yet:**
- Shared writable `mmap` of disk files (read-only shared mappings work).
- ext4 extents.
- Device nodes, FIFOs and sockets on disk (`/dev` is devfs).
- `mount(2)` for other volumes.

## Networking

**Stack** (`net/`). One IPv4 stack for every card:
- **Ethernet and ARP:** a 64-entry cache (5-minute lifetime) that holds up to 4 packets per address while it asks, 3 tries one second apart, then ICMP host unreachable.
- **IPv4:** routes by longest prefix; fragments what does not fit the card's MTU and reassembles what arrives in pieces (64 KiB datagrams); ICMP echo replies, and port, protocol and host unreachable errors in both directions.
- **UDP:** broadcast (with `SO_BROADCAST`), multicast delivery to every matching socket, `connect` for a fixed peer, ICMP errors reported to connected sockets.
- **TCP:** window scaling and the MSS option, RTO per RFC 6298 with Karn's rule, slow start, congestion avoidance and NewReno fast recovery, delayed ACKs, Nagle (`TCP_NODELAY`), zero-window probes, keepalive, `SO_LINGER`, half-close, `TIME_WAIT`, RST on close with unread data, and RFC 5961 challenge ACKs. 128 KiB send and receive buffers per connection.
- **Sockets:** blocking and non-blocking (`connect` returns `EINPROGRESS`), with `poll`/`select`/`epoll`, `SO_RCVTIMEO`/`SO_SNDTIMEO`, `SO_REUSEADDR`, `SO_ERROR`, ephemeral ports 32768–60999. Raw ICMP sockets and unprivileged ping sockets (`SOCK_DGRAM`, `IPPROTO_ICMP`) are there for `ping`.
- **Loopback:** `lo` (127.0.0.1/8, MTU 16436).

**How it runs.** A kernel thread, `netd`, receives frames, runs the ARP, TCP and DHCP timers and sleeps until the next deadline. Cards are polled, not interrupt-driven: on every 1 kHz tick the boot CPU checks one descriptor bit per card and wakes `netd` when a frame has arrived, so an idle network costs one memory read per millisecond. System calls send directly.

**Configuration.** A DHCP client (RFC 2131) runs on every Ethernet card it finds at boot. It applies the address, netmask, default route (the first card to get a lease keeps it), name servers, domain and MTU, and renews the lease at T1 and T2. `/etc/resolv.conf` is a symbolic link to `/proc/net/pnp`, which lists the name servers, so musl's resolver (and BusyBox `nslookup`, `wget`) find them. Boot options:
- `kestrel.net=dhcp` (default) or `kestrel.net=off`.
- `kestrel.ip=ADDR/PREFIX[,GATEWAY[,DNS]]`, e.g. `kestrel.ip=192.168.1.50/24,192.168.1.1,1.1.1.1`: a fixed address on the first card instead of DHCP.

BusyBox `ifconfig`, `route`, `netstat`, `arp`, `ping`, `nc`, `wget`, `httpd` and `nslookup` work through the usual ioctls and `/proc/net/{dev,route,arp,tcp,udp,snmp}`, which use Linux's formats. `ip` needs netlink sockets and `udhcpc` needs packet sockets, which Kestrel does not have yet; the kernel's own DHCP client does `udhcpc`'s job.

**Cards** (`drivers/net/`):

| Driver | Chips | PCI IDs | Tested |
|---|---|---|---|
| `virtio-net` | virtio 1.0 network device (QEMU, KVM, most hypervisors) | 1af4:1000, 1af4:1041 | QEMU `virtio-net-pci` |
| `e1000` | Intel 82540/541/543/544/545/546/547, 82571/572/573/574, 82583 | 8086:100e and 47 others | QEMU `e1000` (82540EM) and `e1000e` (82574L) |
| `rtl8139` | Realtek RTL8139/8139A/B/C/C+/D, 8100, 8130, and boards built on them | 10ec:8139 and 19 others | QEMU `rtl8139`, through I/O ports and through the memory BAR |
| `r8169` | Realtek RTL8169/8110, RTL8168/8111 B through H, 8168EP, 8117; RTL8101E/8102E/8103E/8105E/8106E, 8402, 8411 (Fast Ethernet) | 10ec:8161/8167/8168/8169/8136 and 6 others | not run: QEMU has no model of these chips |

- **e1000:** the integrated Intel LAN of recent chipsets (I217, I218, I219) is a different design and is not claimed.
- **r8169:** the chip version is read from `TxConfig` as Linux does; it decides the receive configuration, the transmit FIFO mode, and whether the PHY is reached through `PHYAR` or (8168G on) the GPHY OCP window. The PLL, the PHY and the RXDV gate are powered back up in case a previous driver shut them down. Linux also loads per-version PHY tuning tables, which this driver does not. The 2.5 and 5 gigabit RTL8125/8126 are a different design and are refused with a message.
- Each card gets a ring of receive and transmit descriptors with 2 KiB buffers (RTL8139: an 8 KiB receive ring and four transmit slots, below 4 GiB). Cards are named `eth0`, `eth1`, ... in the order they are found: virtio-net, e1000, r8169, rtl8139.

**Tests:** `nettest` (in `/boot/bin`) runs 58 checks:
- interfaces, `SIOC*` ioctls and `if_nametoindex`;
- UDP on loopback, including 40 KB fragmented datagrams, `MSG_TRUNC` and ICMP port unreachable (`ECONNREFUSED`);
- TCP: half-close, RST on unread data, `ECONNREFUSED`, `EADDRINUSE`, non-blocking connect with `epoll`, `/proc/net/tcp`, and 8 MiB echoed through loopback with a checksum;
- `nettest HOST:PORT` also waits for DHCP, pings the gateway, echoes 8 MiB of TCP and 20 UDP datagrams through the card with a server on the host, and checks `/etc/resolv.conf`;
- `nettest serve PORT` takes a connection from the host (QEMU `hostfwd`) and answers with the byte count and checksum.

In QEMU (TCG, no KVM) on loopback: 200 one-byte TCP round trips take about 60 ms and bulk TCP runs at about 11 MiB/s each way; through the card, ping to the host is 2–4 ms and TCP 5–7 MiB/s each way.

**Not yet:** IPv6, TCP selective acknowledgement (SACK) and timestamps, IP forwarding, card interrupts, checksum and segmentation offload, `SO_REUSEPORT` load balancing, netlink and packet sockets, and reacting to the cable being unplugged (DHCP keeps asking until it gets an answer, so a cable plugged in after boot does work).

## Memory map at boot

| Range | Use |
|---|---|
| 0 – 1 MiB | reserved (firmware, BIOS data) |
| 2 MiB – ~2.3 MiB | kernel image (+ bss: boot page tables, 32 KiB boot stack) |
| after the kernel | 8 MiB heap, 3 MiB frame-buffer back buffer (from the PMM) |
| e.g. `0xFD000000` (VBE), `0x80000000` (OVMF GOP) | linear frame buffer |

## Next steps

Phase 1 (foundations for real applications) is done: user mode with fault-safe user access, demand-paged copy-on-write memory with W^X and NX, dynamic linking, threads and futexes, epoll and the other event descriptors, descriptor passing, and SMP. Next, in order:

1. **Finer kernel locking:** split the big kernel lock (scheduler run queue, memory manager, VFS, sockets), so system-call-heavy programs scale across CPUs.
2. **A writable file system**: done for ext2, with symbolic links and the root on disk (see above). Next: ext4 extents, a page cache (shared writable mappings of disk files), `mount(2)`.
3. **Networking:** done (see Networking): IPv4, TCP, UDP, DHCP, and drivers for virtio-net, Intel e1000 and Realtek RTL8139/RTL8169/RTL8168. Next: IPv6, card interrupts and offloads, Intel I219 and Realtek RTL8125.
4. **Job control** (stop/continue signals, `SIGTSTP`/`SIGCONT` on the terminal).
5. **ACPI** beyond the MADT and HPET (power-off, PCI interrupt routing) on real hardware.
6. **Native GPU display drivers** (Intel, AMD, NVIDIA) that register their monitors with `display_register()`. The multi-monitor terminals already work with any head.
7. **Hardware support:** MSI/MSI-X interrupts for storage and USB (they are polled today), USB hubs, an EHCI driver for machines without xHCI.

## Licenses

Kestrel source: yours to license as you wish.

Spleen fonts (`third_party/spleen`): BSD-2-Clause, © Frederic Cambus. The terminal font (`third_party/kestrel_font`) is the project's own.

NVIDIA register and VBIOS table knowledge in `drivers/gpu/nvidia/` is derived from nouveau (`third_party/nouveau/LICENSE`): MIT, © Red Hat Inc.
