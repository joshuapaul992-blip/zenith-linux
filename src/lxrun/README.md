# lxrun — user-mode ELF loader + syscall emulator

The zenith-linux **execution engine**, prototyped in user mode. `lxrun.exe`
loads an x86-64 Linux binary — static or dynamically-linked (via its `ld.so`) —
into a Windows x64 process and actually runs it, emulating Linux syscalls
against NT/Win32. Its purpose is to
prove the execution path end-to-end and to print the exact syscall surface a
target binary needs — the spec a future kernel-side provider must implement.

> **Not a sandbox.** The guest shares this process's address space and can call
> anything. Run only trusted test binaries.

## How it works

| Step | Mechanism |
|------|-----------|
| Load | `PT_LOAD` segments mapped at their vaddr (ET_EXEC) or at a reserved bias (static-PIE ET_DYN); bss zero-filled |
| Stack | System V initial stack: `argc`/`argv`/`envp`/`auxv` (incl. `AT_PHDR`, `AT_ENTRY`, `AT_RANDOM`) |
| Syscall intercept | every `syscall` (`0F 05`) in exec segments rewritten to `ud2` (`0F 0B`); `#UD` caught by a vectored exception handler |
| Emulate | Linux ABI regs (`rax`,`rdi`,`rsi`,`rdx`,`r10`,`r8`,`r9`) dispatched to NT/Win32, result in `rax`, step over the `ud2` |
| TLS | `arch_prctl(ARCH_SET_FS)` honored via `wrfsbase` |

### Why rewriting, not fault-trapping

On Windows x64 the `syscall` instruction does **not** fault — it enters the
Windows system-call dispatcher with `rax` as a *Windows* service number. So
Linux syscalls can't be caught as exceptions. Rewriting `syscall`→`ud2` (both
2 bytes) gives us a `#UD` we can intercept. We also omit `AT_SYSINFO_EHDR` from
auxv so libc never takes a vDSO fast path that would bypass the rewrite.

## Requirements

- Build + run on **x64 Windows 10 1607+ / 11** (needs user-mode `wrfsbase`;
  `CR4.FSGSBASE`). `lxrun` warns if the CPU/OS lacks it — TLS-using guests then
  fault. (The eventual XP target handles TLS in a driver instead; this harness
  is a dev tool.)
- **Static and dynamically-linked** guests are both supported. For a dynamic
  binary, `lxrun` maps the `ld.so` named in its `PT_INTERP` and hands control to
  it, so **`--root` must point at a rootfs that actually contains that
  interpreter and the shared libraries** (`ld-linux-x86-64.so.2` + `libc.so.6`,
  or `ld-musl-x86_64.so.1`). `ld.so` then loads libraries itself via the
  emulated `openat`/`mmap`/`mprotect` path (and `lxrun` rewrites `syscall`
  sites in those libraries as they are mapped).

## Build

```
build.cmd            REM from an x64 MSVC prompt
```

## Try it

Zero-dependency smoke test (no rootfs, no libc, no TLS) — see `test/hello.c`:

```
lxrun -v hello_exec        REM expect the greeting + exit code 7
```

Static busybox (Alpine's **static musl** build is ideal — light startup, traps
cleanly):

```
lxrun --root C:\rootfs C:\rootfs\bin\busybox true
lxrun --root C:\rootfs C:\rootfs\bin\busybox echo hello
lxrun -v --root C:\rootfs C:\rootfs\bin\busybox uname -a
```

**Dynamic** binary (Step 1 milestone — exercises `ld.so` + shared libraries).
Point `--root` at a rootfs that has the matching interpreter and libs, e.g. an
Alpine rootfs for a dynamic musl build, or a Debian/Ubuntu rootfs for glibc:

```
lxrun -v --root C:\alpine C:\alpine\bin\busybox echo hi    REM musl ld
lxrun -v --root C:\debian C:\debian\bin\echo hello         REM glibc ld.so
```

With `-v` you'll see `lxrun: dynamic; interp=... base=...` then `ld.so` mapping
`libc` before the program's own output. `-v` prints the full syscall trace; any
`UNIMPLEMENTED syscall N` line tells you exactly what to add to `emulate()`
next — expected as real libraries pull in more of the syscall surface.

## Implemented syscalls

`read write open/openat close lseek pread64 fstat/newfstatat ioctl
access/faccessat brk mmap mprotect munmap arch_prctl set_tid_address
set_robust_list getpid/getppid/gettid getuid/geteuid/getgid/getegid uname
readlink getrandom gettimeofday clock_gettime time prlimit64 rseq
exit/exit_group`

`mmap` supports anonymous, file-backed, and the `ld.so`
reserve-then-`MAP_FIXED` overlay pattern; `mmap`/`mprotect` rewrite `syscall`
sites in any region that becomes executable (so libraries work).

Process & signals (see below): `execve rt_sigaction rt_sigprocmask kill tgkill
rt_sigreturn sigaltstack`. `fork`/`vfork`/`clone` return `-ENOSYS` and `wait4`
returns `-ECHILD` **by design** — they need the kernel-side provider.

Unknown syscalls log and return `-ENOSYS`, so you grow the table by running real
binaries and watching the trace. glibc-static works but is heavier; musl-static
is the recommended target.

## Process & signal model

The full architecture — `fork`/`vfork`/`clone`, `execve`, `wait`/`exit`, and
signals, each mapped to the NT primitive that backs it, plus the user-mode-host
vs. `lxcore.sys` boundary and the PatchGuard constraint that shapes it — is in
[`../../docs/process-model.md`](../../docs/process-model.md).

What `lxrun` implements of that model, in user mode:

- **`execve`** replaces the image in place (frees the old PIE region; rebuilds
  `argv`/`envp`/`auxv`; resets non-ignored signal dispositions to default).
- **Signals**: `rt_sigaction`/`rt_sigprocmask` keep real handler/mask state;
  `kill`/`tgkill` (to self) set the pending set; pending unblocked signals are
  delivered **on return from a syscall** by building the SysV handler frame
  (red zone skipped, 16-aligned, restorer pushed); the handler's return runs
  `rt_sigreturn`, which restores the saved register file and mask. Synchronous
  faults (`SIGSEGV`/`SIGILL`/`SIGFPE`) are raised from the matching NT exception.
- **`fork` is deliberately not emulated.** Duplicating a live emulated process
  from inside the syscall-interception (exception) context is the one thing a
  user-mode host can't do safely; that's the driver's job. See the doc.

Signal smoke test (no libc) — `test/sigtest.c`:

```
lxrun -v sigtest_exec      REM expect the handler line, then exit 0
```

## Relationship to `lxinit`

`lxinit` (the native BootExecute binary) maps and validates the rootfs init but
cannot execute it. `lxrun` is where execution actually happens, in a debuggable
user-mode setting. Once the syscall surface here is solid, the hot path moves
into a kernel-side provider for real process isolation and fork/exec/signals —
the part that genuinely needs a driver.
