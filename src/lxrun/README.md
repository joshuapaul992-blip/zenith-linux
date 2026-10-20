# lxrun — user-mode static-ELF loader + syscall emulator

The zenith-linux **execution engine**, prototyped in user mode. `lxrun.exe`
loads a statically-linked x86-64 Linux binary into a Windows x64 process and
actually runs it, emulating Linux syscalls against NT/Win32. Its purpose is to
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
- Only **statically-linked** guests. A binary with `PT_INTERP` (needs `ld.so`)
  is rejected with a clear message.

## Build

```
build.cmd            REM from an x64 MSVC prompt
```

## Try it

Zero-dependency smoke test (no rootfs, no libc, no TLS) — see `test/hello.c`:

```
lxrun -v hello_exec        REM expect the greeting + exit code 7
```

Fuller test with a **static musl** busybox (Alpine's is ideal — musl static
startup is light and traps cleanly):

```
lxrun --root C:\rootfs C:\rootfs\bin\busybox true
lxrun --root C:\rootfs C:\rootfs\bin\busybox echo hello
lxrun -v --root C:\rootfs C:\rootfs\bin\busybox uname -a
```

`-v` prints the syscall trace; any `UNIMPLEMENTED syscall N` line tells you
exactly what to add to `emulate()` next.

## Implemented syscalls

`read write open/openat close lseek fstat/newfstatat ioctl access brk mmap
mprotect munmap arch_prctl set_tid_address set_robust_list getpid/getppid/gettid
getuid/geteuid/getgid/getegid uname readlink getrandom gettimeofday
clock_gettime time prlimit64 rseq exit/exit_group`

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
