# zenith-linux process & signal model

This document specifies how Linux process primitives — `fork`/`vfork`/`clone`,
`execve`, `wait`/`exit`, and signals — map onto Windows NT, and where the line
falls between the **user-mode host** (`lxrun`, the prototype) and a future
**kernel-side provider** (`lxcore.sys`).

The guiding rule is the same one that shaped `lxrun`: prototype everything that
user mode *can* do, and move to the driver only what user mode genuinely
*cannot*. This document marks each primitive accordingly.

---

## 0. The KPP constraint (read first)

The obvious kernel design — repoint `IA32_LSTAR` so the Linux `syscall`
instruction traps into `lxcore.sys` — is **not available on NT 5.2 x64**.
Kernel Patch Protection (PatchGuard) shipped first on the x64 editions of
XP / Server 2003 and bugchecks (0x109) on any code that rewrites `LSTAR`,
the SSDT, or `KiSystemCall64`.

Consequences:

- The `syscall`→`ud2` rewrite that `lxrun` uses in user mode is **not a
  prototype hack to be replaced** — it stays as the interception mechanism
  even once a driver exists, because it needs no kernel patching.
- `lxcore.sys` therefore is **not** a syscall dispatcher hooked at `LSTAR`.
  It is a helper the user-mode host calls (via a device `IOCTL` fast path or
  a shared ring buffer) for the operations user mode cannot perform itself.
- The per-Linux-process **user-mode host stays in the picture permanently**.
  The architecture is host + driver, not pure kernel. This is the same shape
  WSL1 had (`lxss.sys` + a user broker), adapted to the KPP reality of 5.2.

---

## 1. Identity model

| Linux concept | NT backing | Owner |
|---------------|-----------|-------|
| process (pid/tgid) | one NT process (the host), 1:1 | driver assigns pid |
| thread (tid) | one NT thread in the host process | driver |
| thread group | NT process = thread group | driver |
| pid namespace | a driver-side pid table | driver |

`lxrun` (single-process prototype) fakes `pid=1`, `tid=1`. The driver owns a
real pid allocator and a process table so `getpid`/`getppid`/`wait`/signal
targeting are globally consistent across hosts.

---

## 2. `execve` — address-space **replacement**  *(prototyped in `lxrun`)*

`execve` replaces the current image in the *same* address space. The host can
do this entirely itself:

1. Resolve + validate the new ELF (reuse the loader).
2. Free the old image and brk arena. **Do not free the current stack** — the
   interception context (the `ud2` exception frame) is executing on it; it is
   abandoned and reclaimed lazily (the prototype leaks it; the driver reclaims
   it when the new address space is built).
3. Build a fresh initial stack (new `argv`/`envp`/`auxv`).
4. Reset signal **dispositions** to default — except those set to `SIG_IGN`,
   which are preserved (POSIX). The signal **mask** and **pending** set are
   preserved across `execve`.
5. Close `O_CLOEXEC` descriptors (prototype keeps them; see fd model).
6. Set the thread context to the new entry / new `rsp` and resume.

Because every step stays within one address space — the same thing the host
already does for an ordinary syscall return — `execve` is safe in user mode.
This is the deliberate contrast with `fork` below.

`lxrun` status: implemented for PIE targets (frees the reserved image region)
and ET_EXEC (leaks the old fixed-vaddr image; back-to-back ET_EXEC at the same
vaddr is unsupported in the prototype — use PIE binaries).

---

## 3. `fork` / `vfork` / `clone` — address-space **duplication**  *(driver)*

`fork` is the hard one, and it is why the driver exists.

### Why not in the user-mode host

The host intercepts syscalls from inside a `ud2` **exception context** running
on a worker thread. `fork` must duplicate a *live* process — including that
in-flight exception state — and have the duplicate resume as if it had
returned `0` from the trap. The user-mode primitive for this is
`RtlCloneUserProcess` (which wraps `NtCreateProcessEx` with a COW clone of the
address space and clones the calling thread). But:

- It clones the **calling thread's** register/stack state. Our guest runs
  inside `KiUserExceptionDispatcher`; cloning from there duplicates the
  dispatcher frame, not a clean syscall-return site.
- The clone's new pid, its inherited-and-duplicated NT handles, and the
  parent↔child signal/`wait` relationship all need a coordinator that outlives
  either process.

These are exactly the things a per-process user-mode host cannot arbitrate for
*another* process. So `fork` belongs in `lxcore.sys`.

### Driver design

```
Linux fork():
  1. driver allocates a child pid, creates a child NT process whose address
     space is a COW clone of the parent host (NtCreateProcessEx with the
     parent as the section source — the same mechanism SUA/Interix used for
     real fork, not Cygwin-style userland copy).
  2. driver creates the child's initial thread with the parent guest thread's
     register file, except RAX := 0 (the child's fork() return value) and RIP
     advanced past the trap site.
  3. parent's fork() returns the child pid.
  4. fd table: COW clone duplicates all NT handles into the child; the driver
     records the Linux fd->handle map so both sides agree. O_CLOEXEC honored
     only on execve, not fork (POSIX).
  5. brk arena and mmaps come across as COW pages for free.
```

`vfork` = `fork` with the parent thread suspended by the driver until the child
`execve`s or `_exit`s (they share the address space until then).

`clone` (threads): when `CLONE_VM|CLONE_THREAD` are set, the driver creates a
new NT thread **in the same host process** instead of a new process, assigns a
tid, honors `CLONE_SETTLS` (per-thread `fs` base via `arch_prctl`/`wrfsbase`),
and wires `CLONE_CHILD_CLEARTID` into the futex wake path. `clone` with only
`CLONE_VM` (rare) is a shared-address-space process.

`lxrun` status: returns `-ENOSYS` (by design — see KPP/host limits above). The
prototype proves the single-process path; the moment a second process is
needed, the driver is mandatory.

---

## 4. `exit` / `exit_group` / `wait*`  *(host + driver)*

- `exit_group` terminates the whole thread group → the host process exits with
  the Linux code in the low byte (`ExitProcess(code)`).
- `exit` (single thread) → `ExitThread`; last thread out triggers group exit.
- On exit the driver turns the process into a **zombie** holding its
  `wstatus`, and queues `SIGCHLD` to the parent.
- `wait4`/`waitpid` blocks on child process handles (driver-side
  `KeWaitForMultipleObjects`), reaps the zombie, and encodes status:
  `WIFEXITED`/`WEXITSTATUS` = `code<<8`; `WIFSIGNALED` = `signo`;
  `WIFSTOPPED` = `0x7f | signo<<8`. `__WALL`/`__WNOTHREAD`/`WNOHANG` honored.

`lxrun` status: `exit`/`exit_group` implemented; `wait4` returns `-ECHILD`
(nothing to wait for without `fork`).

---

## 5. Signals  *(model prototyped in `lxrun`; cross-process delivery = driver)*

### 5.1 State (per thread + per thread-group)

- `sigaction[1..64]`: handler, flags, restorer, mask. Signals 1–31 standard,
  32–64 realtime (queued, not just a pending bit — RT delivery order preserved;
  the prototype collapses to a bitmask, the driver keeps the queue).
- `blocked` mask (per thread), `pending` set (per thread + shared group
  pending for process-directed signals).
- Default-action table: **terminate** (most), **ignore** (`SIGCHLD`, `SIGURG`,
  `SIGWINCH`, `SIGCONT`), **stop** (`SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`),
  **core** (`SIGQUIT`, `SIGABRT`, `SIGSEGV`, …). `SIGKILL`/`SIGSTOP` are
  uncatchable/unblockable.

### 5.2 Delivery point

Signals are delivered **on return to user mode**, exactly as a real kernel
does. In the host that return point is the `ud2` exception handler: after any
emulated syscall, the host checks `pending & ~blocked` and, if a deliverable
signal has a handler, builds a frame *then* instead of returning normally.

Synchronous signals (`SIGSEGV`/`SIGILL`/`SIGBUS`/`SIGFPE`) are raised from the
corresponding NT exception (`ACCESS_VIOLATION` → `SIGSEGV`, etc.) caught in the
same handler.

Asynchronous, cross-process signals (`kill` from another process) cannot be
pushed by the sender's host into the target's address space. The **driver**
marks the target's pending set and nudges it; the target observes the signal at
its next syscall return. (The prototype only delivers signals a process raises
on *itself*, e.g. `raise`/`kill(getpid())`/synchronous faults — which is enough
to exercise the full frame/handler/`sigreturn` machinery.)

### 5.3 Frame layout and `rt_sigreturn`

On delivery of signal `n` with a custom handler:

```
  rsp -= 128                      # skip the red zone
  rsp  = align_down(rsp, 16)
  push <return address>           # sa_restorer, or an injected trampoline
  save (pre-signal register file + old blocked mask)   # for rt_sigreturn
  rdi = n                         # SA_SIGINFO handlers also get rsi=siginfo*, rdx=ucontext*
  rip = sa_handler
  blocked |= sa_mask | (1<<(n-1)) # unless SA_NODEFER
```

At handler entry `rsp % 16 == 8` per the SysV ABI (as after a `call`). The
handler eventually `ret`s to the restorer, which executes `rt_sigreturn` (nr
15). The host intercepts that like any syscall and **restores** the saved
register file and blocked mask, resuming the interrupted instruction.

libc always supplies `SA_RESTORER` pointing at its own
`mov rax,15; syscall` stub (which lives in the guest image and is therefore
already rewritten to `ud2`). For handlers installed without `SA_RESTORER`
(raw `syscall(SYS_rt_sigaction, …)`), the host injects a trampoline page
containing `mov rax,15; ud2`.

### 5.4 `SA_RESTART`

If a blocking syscall is interrupted by a signal whose action has `SA_RESTART`,
the host rewinds `rip` to the `ud2` so the syscall re-executes after the
handler returns; otherwise the syscall returns `-EINTR`. (The prototype's
syscalls are all non-blocking, so this is a no-op there; the driver needs it
for real blocking I/O and futex waits.)

---

## 6. Futex  *(driver)*

Threads (`clone`), robust lists, and `pthread` mutexes need `futex`
(`FUTEX_WAIT`/`WAKE`/`REQUEUE`, `FUTEX_PRIVATE`). Backed by driver-side
`KEVENT`s keyed on the physical page+offset of the futex word, so it works
across the COW-shared pages of forked processes. Out of scope for the
single-threaded prototype.

---

## 7. The boundary, at a glance

| Operation | User-mode host (`lxrun`) | Driver (`lxcore.sys`) |
|-----------|--------------------------|------------------------|
| syscall interception (`ud2`) | ✅ permanent | — (KPP forbids LSTAR hook) |
| `execve` (replace image) | ✅ prototyped | reclaims old AS cleanly |
| per-process signals + frames + `rt_sigreturn` | ✅ prototyped | — |
| `fork`/`vfork` (duplicate AS) | ❌ can't clone from trap ctx | ✅ COW via NtCreateProcessEx |
| `clone` (threads) | ❌ needs tid/futex | ✅ |
| cross-process signal delivery | ❌ | ✅ pending + nudge |
| `wait`/zombies/`SIGCHLD` | partial (`exit` only) | ✅ process table |
| `futex`, pid namespace, `/proc` | ❌ | ✅ |

---

## 8. Milestones

1. **(done)** single static binary runs; syscalls traced. (`lxrun`)
2. **(this change)** `execve` + full per-process signal model in the host.
3. `lxcore.sys` skeleton: device, pid table, `fork` via `NtCreateProcessEx`
   COW, `wait`/zombie/`SIGCHLD`, cross-process signal nudge.
4. `clone` threads + `futex`.
5. Post-csrss stage hosts a shell (`busybox sh`) that can `fork`/`exec`/`wait`.
6. TTY/Xorg once a shell is live.
