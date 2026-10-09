/* include/kernel/syscall.h -- POSIX system-call interface
 *
 * Entry points:
 *   int $0x80   IDT gate 0x80 (DPL 3)            -> syscall_dispatch()
 *   syscall     IA32_LSTAR = syscall_entry (asm) -> syscall_dispatch()
 * Register convention (Linux x86_64 ABI):
 *   rax = number, rdi rsi rdx r10 r8 r9 = args, return value in rax
 *   (negative errno on failure). */
#ifndef KESTREL_SYSCALL_H
#define KESTREL_SYSCALL_H

#include <stdint.h>

struct int_frame;

/* Linux x86_64 numbering, so a ported libc needs no renumbering */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect       10
#define SYS_munmap         11
#define SYS_brk            12
#define SYS_rt_sigaction   13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl          16
#define SYS_readv          19
#define SYS_writev         20
#define SYS_sched_yield    24
#define SYS_madvise        28
#define SYS_nanosleep      35
#define SYS_getpid         39
#define SYS_fork           57
#define SYS_execve         59
#define SYS_exit           60
#define SYS_wait4          61
#define SYS_kill           62
#define SYS_uname          63
#define SYS_getcwd         79
#define SYS_chdir          80
#define SYS_mkdir          83
#define SYS_rmdir          84
#define SYS_rename         82
#define SYS_unlink         87
#define SYS_chmod          90
#define SYS_chown          92
#define SYS_gettimeofday   96
#define SYS_getuid        102
#define SYS_getgid        104
#define SYS_getppid       110
#define SYS_arch_prctl    158
#define SYS_reboot        169
#define SYS_gettid        186
#define SYS_getdents64    217
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_exit_group    231
#define SYS_utimensat     280
#define SYS_MAX           320

typedef int64_t (*syscall_fn)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);

void syscall_init(void);
void syscall_dispatch(struct int_frame *f);
const char *syscall_name(int nr);
int  syscall_implemented(int nr);
uint64_t syscall_count(int nr);

#endif
