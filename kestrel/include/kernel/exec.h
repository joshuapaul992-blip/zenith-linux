/* include/kernel/exec.h -- user processes from ELF64 files
 *
 * Kestrel runs statically linked x86_64 ELF programs in ring 3, each in its
 * own address space (uvm.h). Accepted: ET_EXEC linked inside the user half,
 * and ET_DYN (static-PIE, e.g. musl `-static-pie`), loaded at
 * UVM_IMAGE_BASE. Programs needing an interpreter (PT_INTERP) are refused.
 * The initial stack follows the System V ABI / Linux layout (argc, argv,
 * envp, auxv with AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_ENTRY,
 * AT_RANDOM), so a musl crt1 starts unmodified. */
#ifndef KESTREL_EXEC_H
#define KESTREL_EXEC_H

#define EXEC_MAX_ARGS   32

/* Start `path` with argv[0..argc) as a new process sharing the caller's TTY.
 * Returns its pid, or a negative errno if it could not even be created
 * (load errors after that make the process exit with status 127). */
int exec_spawn(const char *path, int argc, char *const argv[]);

/* The same with stdin/stdout/stderr on device `stdio` (e.g. "/dev/kmsg"). */
int exec_spawn_io(const char *path, int argc, char *const argv[], const char *stdio);

/* Wait for process `pid` to exit; returns its exit status, or -ECHILD. */
int exec_wait(int pid);

#endif
