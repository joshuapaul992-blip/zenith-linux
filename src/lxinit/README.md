# lxinit — zenith-linux native-mode bootstrap

A native NT subsystem binary (`/SUBSYSTEM:NATIVE,5.02`) for **Windows XP x64 /
Server 2003 (NT 5.2)**, registered under
`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\BootExecute`. It runs in
smss.exe's native environment — after the kernel and ntdll, **before** csrss and
the video drivers — using ntdll syscalls only (no CRT, no Win32).

## What it does

1. Reads config from the registry (`Session Manager\Zenith`).
2. Identifies the distro from `<rootfs>\etc\os-release` (best effort).
3. Locates the init binary for **any** x86-64 rootfs — probing `/sbin/init`,
   systemd, busybox (Alpine), bash, sh, … or an explicit `InitPath` — and maps
   + validates it as an ELF64 via `NtOpenFile` / `NtCreateSection` /
   `NtMapViewOfSection`.
4. Honors an X11 request without deadlocking boot, then terminates so smss
   proceeds to start csrss and load the video miniports.
5. Prints progress via `NtDisplayString` (the only early-boot console path).

## Honest scope

`lxinit` **maps and validates** the Linux init; it does not execute it.
Running ELF code needs a kernel-side syscall-translation layer (the role
`lxcore.sys` / a pico-process provider plays), which is out of scope for a
user-mode BootExecute app. See the header comment in `lxinit.c` for the full
rationale, including why `SEC_IMAGE` can't be used on an ELF and why process
termination — not a flag or thread — is what unblocks the Win32 subsystem.

## Configuration

`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Zenith`

| Value        | Type      | Meaning                                              |
|--------------|-----------|------------------------------------------------------|
| `RootfsPath` | REG_SZ    | DOS path to the rootfs root (default `C:\rootfs`)    |
| `InitPath`   | REG_SZ    | rootfs-relative init; omit to auto-detect            |
| `EnableX11`  | REG_DWORD | nonzero → request post-csrss Xorg layer              |

## Build

From an **x64** MSVC/DDK command prompt (e.g. the Server 2003 R2 DDK for a true
NT 5.2 target, or a WDK x64 prompt):

```
build.cmd          REM or: nmake
build.cmd debug    REM unoptimized + symbols
```

## Install

Copy `lxinit.exe` to `%SystemRoot%\System32`, then either:

- `zenith-install.cmd [RootfsPath] [EnableX11]` — **recommended**; appends
  `lxinit` to `BootExecute` while preserving existing entries, or
- `regedit /s zenith-install.reg` — only if your `BootExecute` is the stock
  `autocheck autochk *` (this replaces the whole value).

Reboot to run.
