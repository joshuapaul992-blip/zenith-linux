/*
 * lxrun.c  --  zenith-linux user-mode static-ELF loader + syscall emulator
 *
 * A developer harness that actually *executes* a statically-linked x86-64
 * Linux binary on Windows x64 by:
 *
 *   1. Loading its PT_LOAD segments (ET_EXEC or static-PIE ET_DYN).
 *   2. Building a System V initial stack (argc/argv/envp/auxv).
 *   3. Rewriting every Linux `syscall` (0F 05) to `ud2` (0F 0B) in the
 *      executable segments, then catching the resulting #UD in a vectored
 *      exception handler and emulating the Linux syscall against NT/Win32.
 *   4. Running the guest natively on a Windows thread between syscalls.
 *
 * This is the user-mode prototype of zenith-linux's execution engine: it is
 * fully debuggable with an ordinary debugger and prints the exact syscall
 * trace a target needs, which is what a future kernel-side provider must
 * implement. It is NOT a sandbox -- the guest shares this process's address
 * space and can call anything; run only trusted test binaries.
 *
 * -------------------------------------------------------------------------
 * WHY NOT "just execute syscall and trap it"
 * -------------------------------------------------------------------------
 * On Windows x64 the `syscall` instruction does not fault; it enters the
 * Windows system-call dispatcher (KiSystemCall64) with RAX as a *Windows*
 * service number. So Linux syscalls cannot be trapped as exceptions. We
 * instead rewrite each 2-byte `syscall` to the 2-byte `ud2`, which raises
 * EXCEPTION_ILLEGAL_INSTRUCTION we can intercept. Scanning is limited to
 * executable segments; a stray 0F 05 in in-segment data becomes a ud2 that
 * is never executed, so it is harmless.
 *
 * TLS: the guest sets its thread pointer via arch_prctl(ARCH_SET_FS). We honor
 * it with `wrfsbase`, so the guest's fs:-relative accesses (stack canary at
 * fs:0x28, errno, TLS) resolve. `wrfsbase` needs CR4.FSGSBASE, which Windows
 * 10 1607+ x64 enables; on older hosts TLS-using guests will fault (documented).
 *
 * Build (x64 MSVC command prompt):
 *     cl /O2 /W4 /nologo lxrun.c
 * Usage:
 *     lxrun [-v] [--root <winpath>] <elf-file> [guest args...]
 *       -v            trace every emulated syscall
 *       --root PATH   Windows dir treated as the guest filesystem root "/"
 *
 * Smoke test (no rootfs, no libc, no TLS needed):
 *     see test\hello.c  ->  lxrun -v hello
 * Fuller test (Alpine musl static busybox recommended):
 *     lxrun --root C:\rootfs C:\rootfs\bin\busybox true
 *     lxrun --root C:\rootfs C:\rootfs\bin\busybox echo hello
 */

#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

/* ========================================================================= */
/*  ELF64                                                                     */
/* ========================================================================= */

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

#pragma pack(push, 1)
typedef struct {
    unsigned char e_ident[16];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;
#pragma pack(pop)

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 0x3E
#define PT_LOAD    1
#define PT_INTERP  3
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* auxv types */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

/* Linux x86-64 errno subset (returned as negative values) */
#define L_EPERM   1
#define L_ENOENT  2
#define L_ESRCH   3
#define L_EINTR   4
#define L_ENOEXEC 8
#define L_EBADF   9
#define L_ECHILD  10
#define L_ENOMEM  12
#define L_EFAULT  14
#define L_EINVAL  22
#define L_ENOTTY  25
#define L_ENOSYS  38

/* Linux x86-64 syscall numbers used below */
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl 16
#define SYS_writev 20
#define SYS_access 21
#define SYS_getpid 39
#define SYS_exit 60
#define SYS_uname 63
#define SYS_readlink 89
#define SYS_gettimeofday 96
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_arch_prctl 158
#define SYS_gettid 186
#define SYS_time 201
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_openat 257
#define SYS_newfstatat 262
#define SYS_set_robust_list 273
#define SYS_prlimit64 302
#define SYS_getrandom 318
#define SYS_rseq 334
#define SYS_rt_sigreturn 15
#define SYS_clone 56
#define SYS_fork 57
#define SYS_vfork 58
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_getppid 110
#define SYS_sigaltstack 131
#define SYS_tgkill 234
#define SYS_pread64 17
#define SYS_faccessat 269
#define SYS_faccessat2 439

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

/* sigaction flags / special handlers */
#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000
#define SA_NODEFER  0x40000000
#define SIG_DFL 0
#define SIG_IGN 1

/* mmap flags / prot bits */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define GUEST_STACK_SIZE (8 * 1024 * 1024)
#define BRK_ARENA_SIZE   (256 * 1024 * 1024)
#define PAGE 0x1000ULL

/* ========================================================================= */
/*  Globals (single guest thread)                                            */
/* ========================================================================= */

static int      g_verbose = 0;
static wchar_t  g_root[MAX_PATH] = L"";   /* guest "/" -> this Windows dir   */

static uint64_t g_brk_base = 0, g_brk_cur = 0, g_brk_end = 0;

static uint64_t g_guest_lo = 0, g_guest_hi = 0;  /* main image span (info)     */
static uint64_t g_self_lo = 0, g_self_hi = 0;    /* lxrun.exe's own module range */

/* Current image regions, for execve teardown. */
static uint64_t g_image_base = 0;   /* reserved PIE region base; 0 if ET_EXEC */
static int      g_image_is_dyn = 0;

/* Signal model (single guest thread prototype; see docs/process-model.md). */
typedef struct { uint64_t handler, flags, restorer, mask; } ksigaction;
static ksigaction g_sig[65];
static uint64_t   g_blocked = 0;    /* blocked mask  */
static uint64_t   g_pending = 0;    /* pending mask  */
static uint64_t   g_restorer = 0;   /* injected "mov rax,15; ud2" trampoline */

typedef struct { CONTEXT ctx; uint64_t oldblocked; } sigsave_t;
static sigsave_t g_sigstack[8];     /* saved contexts for rt_sigreturn */
static int       g_sigsp = 0;

/* fd table: Linux fd -> Windows HANDLE (0/1/2 are the console). */
#define MAX_FDS 256
static HANDLE   g_fds[MAX_FDS];

static HANDLE   g_out, g_in, g_err;

/* ========================================================================= */
/*  Helpers                                                                   */
/* ========================================================================= */

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "lxrun: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    ExitProcess(1);
}

static void trace(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "  [syscall] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }

static DWORD prot_to_win(uint32_t pf)
{
    int r = (pf & PF_R) != 0, w = (pf & PF_W) != 0, x = (pf & PF_X) != 0;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* mmap PROT_* -> PAGE_* */
static DWORD mmap_prot_to_win(uint64_t prot)
{
    int r = (prot & 1) != 0, w = (prot & 2) != 0, x = (prot & 4) != 0;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* Copy a NUL-terminated guest string; returns 0 on fault. */
static int guest_strcpy(char *dst, size_t cap, uint64_t gptr)
{
    size_t i = 0;
    __try {
        const char *s = (const char *)gptr;
        for (; i + 1 < cap && s[i]; i++) dst[i] = s[i];
        dst[i] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Translate a Linux path under g_root into a Windows path. */
static void translate_path(const char *lpath, wchar_t *out, size_t outcap)
{
    wchar_t rel[MAX_PATH];
    size_t i = 0;
    const char *p = lpath;
    if (*p == '/') p++;                       /* strip leading slash */
    for (; p[i] && i + 1 < MAX_PATH; i++)
        rel[i] = (p[i] == '/') ? L'\\' : (wchar_t)(unsigned char)p[i];
    rel[i] = 0;
    if (g_root[0])
        _snwprintf(out, outcap, L"%s\\%s", g_root, rel);
    else
        _snwprintf(out, outcap, L"%s", rel);
    out[outcap - 1] = 0;
}

static int alloc_fd(HANDLE h)
{
    for (int i = 3; i < MAX_FDS; i++)
        if (g_fds[i] == NULL) { g_fds[i] = h; return i; }
    return -1;
}

/* ========================================================================= */
/*  ELF loader                                                                */
/* ========================================================================= */

typedef struct {
    uint64_t entry;       /* where to jump (interpreter entry if dynamic)   */
    uint64_t main_entry;  /* the main executable's own entry (AT_ENTRY)     */
    uint64_t at_base;     /* interpreter load base (AT_BASE), 0 if static   */
    uint64_t phdr_va;     /* main executable's program headers in memory    */
    uint16_t phent, phnum;
    int      is_dyn;
} loaded_t;

/* One mapped ELF object (main executable or the ld.so interpreter). */
typedef struct {
    uint64_t bias;        /* load bias (0 for ET_EXEC)                      */
    uint64_t base;        /* lowest mapped address                          */
    uint64_t load_hi;     /* highest mapped address                         */
    uint64_t entry;       /* bias + e_entry                                 */
    uint64_t phdr_va;     /* program headers in memory                      */
    uint16_t phent, phnum;
    int      is_dyn;
    int      has_interp;
    char     interp[256]; /* PT_INTERP path, if present                     */
} objinfo_t;

static unsigned char *read_file(const wchar_t *path, size_t *outlen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) die("cannot open %ls (err %lu)", path, GetLastError());
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) die("GetFileSizeEx failed");
    unsigned char *buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf) die("out of memory reading file");
    DWORD got = 0, total = 0;
    while (total < sz.QuadPart) {
        if (!ReadFile(h, buf + total, (DWORD)(sz.QuadPart - total), &got, NULL) || got == 0)
            die("ReadFile failed");
        total += got;
    }
    CloseHandle(h);
    *outlen = (size_t)sz.QuadPart;
    return buf;
}

/* Rewrite every Linux syscall (0F 05) to ud2 (0F 0B) in a region. Used for
 * loaded images and for executable memory the guest maps itself (libraries). */
static int patch_syscalls(void *base, size_t len)
{
    unsigned char *b = (unsigned char *)base;
    int n = 0;
    for (size_t i = 0; i + 1 < len; i++)
        if (b[i] == 0x0F && b[i + 1] == 0x05) { b[i + 1] = 0x0B; n++; }
    return n;
}

/* Map one ELF object into memory: segments at bias, bss zeroed, syscalls
 * rewritten, final protections applied. Static-PIE / dynamic objects are not
 * relocated here (ld.so / self-reloc handle that). die()s on malformed input. */
static void map_object(const wchar_t *path, objinfo_t *info)
{
    size_t flen = 0;
    unsigned char *f = read_file(path, &flen);
    if (flen < sizeof(Elf64_Ehdr)) die("file too small for ELF header: %ls", path);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)f;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'))
        die("not an ELF file: %ls", path);
    if (eh->e_ident[4] != 2)        die("not ELFCLASS64: %ls", path);
    if (eh->e_ident[5] != 1)        die("not little-endian: %ls", path);
    if (eh->e_machine != EM_X86_64) die("not x86-64 (e_machine=0x%x): %ls", eh->e_machine, path);
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        die("unsupported e_type %u: %ls", eh->e_type, path);

    Elf64_Phdr *ph = (Elf64_Phdr *)(f + eh->e_phoff);
    memset(info, 0, sizeof(*info));

    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_INTERP) {
            size_t n = (size_t)ph[i].p_filesz;
            if (n >= sizeof(info->interp)) n = sizeof(info->interp) - 1;
            memcpy(info->interp, f + ph[i].p_offset, n);
            info->interp[n] = 0;
            info->has_interp = 1;
        }

    uint64_t vmin = ~0ULL, vmax = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < vmin) vmin = ph[i].p_vaddr;
        uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > vmax) vmax = end;
    }
    if (vmin == ~0ULL) die("no PT_LOAD segments: %ls", path);

    uint64_t vlo = align_down(vmin, PAGE), vhi = align_up(vmax, PAGE);
    int is_dyn = (eh->e_type == ET_DYN);
    uint64_t bias = 0;
    if (is_dyn) {
        void *region = VirtualAlloc(NULL, (SIZE_T)(vhi - vlo), MEM_RESERVE, PAGE_NOACCESS);
        if (!region) die("reserve %llu bytes failed (err %lu): %ls",
                         (unsigned long long)(vhi - vlo), GetLastError(), path);
        bias = (uint64_t)region - vlo;
    }

    typedef struct { uint64_t va; size_t len; uint32_t flags; } seg_t;
    seg_t segs[32]; int nseg = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t slo = align_down(ph[i].p_vaddr, PAGE);
        uint64_t shi = align_up(ph[i].p_vaddr + ph[i].p_memsz, PAGE);
        void *addr = (void *)(bias + slo);
        DWORD at = is_dyn ? MEM_COMMIT : (MEM_RESERVE | MEM_COMMIT);
        void *got = VirtualAlloc(addr, (SIZE_T)(shi - slo), at, PAGE_READWRITE);
        if (!got || (uint64_t)got != bias + slo)
            die("VirtualAlloc seg %p failed (err %lu): %ls", addr, GetLastError(), path);
        memcpy((void *)(bias + ph[i].p_vaddr), f + ph[i].p_offset, (size_t)ph[i].p_filesz);
        if (nseg < 32) {
            segs[nseg].va = bias + slo; segs[nseg].len = (size_t)(shi - slo);
            segs[nseg].flags = ph[i].p_flags; nseg++;
        }
    }

    int patched = 0;
    for (int s = 0; s < nseg; s++)
        if (segs[s].flags & PF_X) patched += patch_syscalls((void *)segs[s].va, segs[s].len);
    for (int s = 0; s < nseg; s++) {
        DWORD op;
        if (!VirtualProtect((void *)segs[s].va, segs[s].len, prot_to_win(segs[s].flags), &op))
            die("VirtualProtect failed (err %lu): %ls", GetLastError(), path);
    }
    if (g_verbose)
        fprintf(stderr, "lxrun: mapped %ls bias=%p (%d syscall site(s))\n",
                path, (void *)bias, patched);

    uint64_t phdr_va = bias + eh->e_phoff;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (eh->e_phoff >= ph[i].p_offset && eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
            phdr_va = bias + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }

    info->bias = bias; info->base = bias + vlo; info->load_hi = bias + vhi;
    info->entry = bias + eh->e_entry; info->phdr_va = phdr_va;
    info->phent = eh->e_phentsize; info->phnum = eh->e_phnum; info->is_dyn = is_dyn;
    free(f);
}

static void load_elf(const wchar_t *path, loaded_t *out)
{
    objinfo_t m;
    map_object(path, &m);

    g_image_base   = m.is_dyn ? m.base : 0;   /* PIE region, freed on execve */
    g_image_is_dyn = m.is_dyn;
    g_guest_lo = m.base; g_guest_hi = m.load_hi;

    uint64_t jump_entry = m.entry, at_base = 0, brk_anchor = m.load_hi;

    if (m.has_interp) {
        wchar_t wp[MAX_PATH];
        translate_path(m.interp, wp, MAX_PATH);
        objinfo_t it;
        map_object(wp, &it);               /* the ld.so interpreter */
        jump_entry = it.entry;             /* control starts in ld.so */
        at_base    = it.base;              /* AT_BASE                 */
        if (it.load_hi > brk_anchor) brk_anchor = it.load_hi;
        if (it.base    < g_guest_lo) g_guest_lo = it.base;
        if (it.load_hi > g_guest_hi) g_guest_hi = it.load_hi;
        if (g_verbose)
            fprintf(stderr, "lxrun: dynamic; interp=%s base=%p entry=%p\n",
                    m.interp, (void *)at_base, (void *)jump_entry);
    }

    out->entry      = jump_entry;
    out->main_entry = m.entry;
    out->at_base    = at_base;
    out->phdr_va    = m.phdr_va;
    out->phent      = m.phent;
    out->phnum      = m.phnum;
    out->is_dyn     = m.is_dyn;

    /* Program break above everything mapped. */
    void *arena = VirtualAlloc((void *)align_up(brk_anchor, PAGE), BRK_ARENA_SIZE,
                               MEM_RESERVE, PAGE_NOACCESS);
    if (!arena) arena = VirtualAlloc(NULL, BRK_ARENA_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!arena) die("cannot reserve brk arena");
    g_brk_base = g_brk_cur = (uint64_t)arena;
    g_brk_end  = g_brk_base + BRK_ARENA_SIZE;
}

/* ========================================================================= */
/*  Initial stack (System V x86-64 ABI)                                       */
/* ========================================================================= */

static uint64_t build_stack(const loaded_t *ld, int argc, char **argv,
                            int envc, char **envs)
{
    void *stk = VirtualAlloc(NULL, GUEST_STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stk) die("cannot allocate guest stack");
    uint64_t top = (uint64_t)stk + GUEST_STACK_SIZE;

    if (envc > 16) envc = 16;
    uint64_t p = top;
    uint64_t argp[64], envp[16];
    if (argc > 64) argc = 64;

    /* Copy arg/env strings high on the stack. */
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        p -= n; memcpy((void *)p, argv[i], n); argp[i] = p;
    }
    for (int i = 0; i < envc; i++) {
        size_t n = strlen(envs[i]) + 1;
        p -= n; memcpy((void *)p, envs[i], n); envp[i] = p;
    }

    /* 16 random bytes for AT_RANDOM (stack canary seed). */
    p -= 16;
    uint64_t at_random = p;
    { unsigned __int64 a = __rdtsc(); memcpy((void *)p, &a, 8);
      a ^= (unsigned __int64)GetTickCount64() * 0x9E3779B97F4A7C15ULL;
      memcpy((void *)(p + 8), &a, 8); }

    uint64_t at_execfn = argc > 0 ? argp[0] : at_random;

    /* auxv (type,val) pairs, AT_NULL terminated. */
    uint64_t aux[64]; int na = 0;
    #define AUX(t,v) do { aux[na++]=(t); aux[na++]=(uint64_t)(v); } while (0)
    AUX(AT_PHDR, ld->phdr_va);
    AUX(AT_PHENT, ld->phent);
    AUX(AT_PHNUM, ld->phnum);
    AUX(AT_PAGESZ, PAGE);
    AUX(AT_BASE, ld->at_base);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, ld->main_entry);
    AUX(AT_UID, 0); AUX(AT_EUID, 0); AUX(AT_GID, 0); AUX(AT_EGID, 0);
    AUX(AT_CLKTCK, 100);
    AUX(AT_HWCAP, 0);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, at_random);
    AUX(AT_EXECFN, at_execfn);
    AUX(AT_NULL, 0);
    #undef AUX

    /* Slots: argc + (argc+1) + (envc+1) + na. Align rsp to 16. */
    int nslots = 1 + (argc + 1) + (envc + 1) + na;
    uint64_t rsp = align_down(p - (uint64_t)nslots * 8, 16);

    uint64_t *sp = (uint64_t *)rsp;
    int idx = 0;
    sp[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) sp[idx++] = argp[i];
    sp[idx++] = 0;
    for (int i = 0; i < envc; i++) sp[idx++] = envp[i];
    sp[idx++] = 0;
    for (int i = 0; i < na; i++) sp[idx++] = aux[i];

    return rsp;
}

/* ========================================================================= */
/*  Process + signal helpers                                                  */
/* ========================================================================= */

static void init_signals(void)
{
    /* Trampoline a handler returns to when no SA_RESTORER is set:
     * mov rax, 15 (rt_sigreturn) ; ud2  -> our #UD handler restores context. */
    static const unsigned char code[] = {
        0x48, 0xC7, 0xC0, 0x0F, 0x00, 0x00, 0x00,   /* mov rax, 15 */
        0x0F, 0x0B                                   /* ud2         */
    };
    void *p = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) die("cannot allocate signal restorer trampoline");
    memcpy(p, code, sizeof(code));
    g_restorer = (uint64_t)p;
    for (int s = 0; s < 65; s++) {
        g_sig[s].handler = SIG_DFL; g_sig[s].flags = 0;
        g_sig[s].restorer = 0;      g_sig[s].mask = 0;
    }
}

/* Deliver one pending, unblocked signal by building its handler frame. Called
 * on return to user mode (after each emulated syscall). */
static void deliver_pending(CONTEXT *c)
{
    uint64_t d = g_pending & ~g_blocked;
    if (!d) return;
    int sig = 0;
    for (int i = 0; i < 64; i++) if (d & (1ULL << i)) { sig = i + 1; break; }
    if (!sig) return;
    g_pending &= ~(1ULL << (sig - 1));

    ksigaction *sa = &g_sig[sig];
    uint64_t h = sa->handler;
    if (h == SIG_IGN) return;
    if (h == SIG_DFL) {
        /* default-ignore: SIGCHLD(17) SIGCONT(18) SIGURG(23) SIGWINCH(28) */
        if (sig == 17 || sig == 18 || sig == 23 || sig == 28) return;
        fflush(NULL);
        ExitProcess(128 + sig);          /* default action: terminate */
    }
    if (g_sigsp >= (int)(sizeof(g_sigstack) / sizeof(g_sigstack[0]))) return;

    uint64_t sp = c->Rsp;
    sp -= 128;                            /* preserve the red zone */
    sp = align_down(sp, 16);
    uint64_t ret = (sa->flags & SA_RESTORER) ? sa->restorer : g_restorer;
    sp -= 8;
    *(uint64_t *)sp = ret;                /* handler returns here -> rt_sigreturn */

    g_sigstack[g_sigsp].ctx = *c;
    g_sigstack[g_sigsp].oldblocked = g_blocked;
    g_sigsp++;

    if (!(sa->flags & SA_NODEFER)) g_blocked |= (1ULL << (sig - 1));
    g_blocked |= sa->mask;

    c->Rsp = sp;
    c->Rip = h;
    c->Rdi = (uint64_t)sig;               /* arg1: signo                       */
    c->Rsi = 0;                           /* arg2: siginfo_t*  (SA_SIGINFO)    */
    c->Rdx = 0;                           /* arg3: ucontext_t*                 */
    if (g_verbose) fprintf(stderr, "  [signal] deliver %d -> %p\n", sig, (void *)h);
}

/* Copy a guest char*[] (NULL-terminated) into a host pool; out[] points in. */
static int read_guest_strv(uint64_t arr, char **out, int max, char *pool, size_t poolsz)
{
    size_t used = 0; int n = 0;
    if (arr) __try {
        uint64_t *a = (uint64_t *)arr;
        for (; n < max - 1 && a[n]; n++) {
            const char *s = (const char *)a[n];
            size_t len = strlen(s) + 1;
            if (used + len > poolsz) break;
            memcpy(pool + used, s, len);
            out[n] = pool + used; used += len;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    out[n] = NULL;
    return n;
}

/* execve: replace the image in this address space. Returns 0 (context set to
 * the new entry) or a negative errno. See docs/process-model.md section 2. */
static char  g_apool[32768], g_epool[32768];
static char *g_av[128], *g_ev[128];

static int do_execve(CONTEXT *c)
{
    char lpath[1024];
    if (!guest_strcpy(lpath, sizeof(lpath), c->Rdi)) return -L_EFAULT;
    wchar_t wp[MAX_PATH];
    translate_path(lpath, wp, MAX_PATH);
    if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES) return -L_ENOENT;

    int ac = read_guest_strv(c->Rsi, g_av, 128, g_apool, sizeof(g_apool));
    int ec = read_guest_strv(c->Rdx, g_ev, 128, g_epool, sizeof(g_epool));
    if (ac == 0) { g_av[0] = lpath; g_av[1] = NULL; ac = 1; }

    uint64_t old_image = g_image_base;    /* PIE region base or 0 (ET_EXEC)    */
    uint64_t old_brk   = g_brk_base;

    loaded_t ld;
    load_elf(wp, &ld);                    /* NB: malformed ELF die()s (prototype) */
    uint64_t rsp = build_stack(&ld, ac, g_av, ec, g_ev);

    if (old_image) VirtualFree((void *)old_image, 0, MEM_RELEASE);
    if (old_brk)   VirtualFree((void *)old_brk, 0, MEM_RELEASE);
    /* The old stack is abandoned deliberately: we are executing on it. */

    /* Reset dispositions: non-ignored -> default. Mask & pending preserved. */
    for (int s = 1; s <= 64; s++)
        if (g_sig[s].handler != SIG_IGN) {
            g_sig[s].handler = SIG_DFL; g_sig[s].flags = 0;
            g_sig[s].restorer = 0;      g_sig[s].mask = 0;
        }
    g_sigsp = 0;

    c->Rip = ld.entry; c->Rsp = rsp;
    c->Rax = c->Rbx = c->Rcx = c->Rdx = c->Rsi = c->Rdi = c->Rbp = 0;
    c->R8 = c->R9 = c->R10 = c->R11 = c->R12 = c->R13 = c->R14 = c->R15 = 0;
    if (g_verbose)
        fprintf(stderr, "lxrun: execve -> %ls (%s) entry=%p\n",
                wp, ld.is_dyn ? "PIE" : "ET_EXEC", (void *)ld.entry);
    return 0;
}

/* ========================================================================= */
/*  Syscall emulation                                                         */
/* ========================================================================= */

static void fill_stat(uint64_t gptr, int is_char, int64_t size)
{
    __try {
        unsigned char *s = (unsigned char *)gptr;
        memset(s, 0, 144);
        uint32_t mode = is_char ? 0020000 : 0100000;  /* S_IFCHR / S_IFREG */
        mode |= 0644;
        *(uint32_t *)(s + 24) = mode;        /* st_mode  */
        *(int64_t  *)(s + 48) = size;        /* st_size  */
        *(int64_t  *)(s + 56) = 4096;        /* st_blksize */
        *(int64_t  *)(s + 64) = (size + 511) / 512; /* st_blocks */
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* caller already returned addr */ }
}

/* Returns the syscall result (Linux convention: negative errno on error). */
static int64_t emulate(CONTEXT *c)
{
    uint64_t nr  = c->Rax;
    uint64_t a0 = c->Rdi, a1 = c->Rsi, a2 = c->Rdx,
             a3 = c->R10, a4 = c->R8,  a5 = c->R9;
    (void)a4; (void)a5;

    switch (nr) {

    case SYS_write: {
        HANDLE h = (a0 < MAX_FDS) ? g_fds[a0] : NULL;
        if (!h) return -L_EBADF;
        DWORD wrote = 0;
        __try { WriteFile(h, (const void *)a1, (DWORD)a2, &wrote, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        trace("write(%llu, %p, %llu) = %lu", a0, (void*)a1, a2, wrote);
        return (int64_t)wrote;
    }

    case SYS_writev: {
        HANDLE h = (a0 < MAX_FDS) ? g_fds[a0] : NULL;
        if (!h) return -L_EBADF;
        int64_t total = 0;
        __try {
            struct { uint64_t base; uint64_t len; } *iov = (void *)a1;
            for (uint64_t i = 0; i < a2; i++) {
                DWORD wrote = 0;
                WriteFile(h, (const void *)iov[i].base, (DWORD)iov[i].len, &wrote, NULL);
                total += wrote;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        trace("writev(%llu, %p, %llu) = %lld", a0, (void*)a1, a2, total);
        return total;
    }

    case SYS_read: {
        HANDLE h = (a0 < MAX_FDS) ? g_fds[a0] : NULL;
        if (!h) return -L_EBADF;
        DWORD got = 0;
        __try { if (!ReadFile(h, (void *)a1, (DWORD)a2, &got, NULL)) return 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        trace("read(%llu, %p, %llu) = %lu", a0, (void*)a1, a2, got);
        return (int64_t)got;
    }

    case SYS_open:
    case SYS_openat: {
        uint64_t pathp = (nr == SYS_open) ? a0 : a1;
        char lpath[1024];
        if (!guest_strcpy(lpath, sizeof(lpath), pathp)) return -L_EFAULT;
        wchar_t wp[MAX_PATH]; translate_path(lpath, wp, MAX_PATH);
        HANDLE h = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { trace("open(%s) = -ENOENT", lpath); return -L_ENOENT; }
        int fd = alloc_fd(h);
        if (fd < 0) { CloseHandle(h); return -L_ENOMEM; }
        trace("open(\"%s\") = %d", lpath, fd);
        return fd;
    }

    case SYS_close: {
        if (a0 < 3 || a0 >= MAX_FDS || !g_fds[a0]) return -L_EBADF;
        CloseHandle(g_fds[a0]); g_fds[a0] = NULL;
        trace("close(%llu) = 0", a0);
        return 0;
    }

    case SYS_lseek: {
        HANDLE h = (a0 < MAX_FDS) ? g_fds[a0] : NULL;
        if (!h) return -L_EBADF;
        LARGE_INTEGER dist; dist.QuadPart = (int64_t)a1;
        LARGE_INTEGER np;
        DWORD method = (a2 == 1) ? FILE_CURRENT : (a2 == 2) ? FILE_END : FILE_BEGIN;
        if (!SetFilePointerEx(h, dist, &np, method)) return -L_EINVAL;
        return np.QuadPart;
    }

    case SYS_fstat:
    case SYS_newfstatat: {
        /* fstat(fd, statbuf); newfstatat(dirfd, path, statbuf, flags).
         * Prototype approximation: stat the fd/dirfd, ignore the path. */
        uint64_t fd  = a0;
        uint64_t stp = (nr == SYS_fstat) ? a1 : a2;
        HANDLE h = (fd < MAX_FDS) ? g_fds[fd] : NULL;
        int is_char = (fd <= 2);
        int64_t size = 0;
        if (h && !is_char) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(h, &sz)) size = sz.QuadPart;
        }
        fill_stat(stp, is_char, size);
        trace("fstat(%llu) = 0", fd);
        return 0;
    }

    case SYS_ioctl:
        /* Make isatty()/tcgetattr() report "not a tty" so libc writes plainly. */
        trace("ioctl(%llu, 0x%llx) = -ENOTTY", a0, a1);
        return -L_ENOTTY;

    case SYS_access:
    case SYS_faccessat:
    case SYS_faccessat2: {
        uint64_t pathp = (nr == SYS_access) ? a0 : a1;
        char lp[1024];
        if (!guest_strcpy(lp, sizeof(lp), pathp)) return -L_EFAULT;
        wchar_t wp[MAX_PATH]; translate_path(lp, wp, MAX_PATH);
        DWORD attr = GetFileAttributesW(wp);
        return (attr == INVALID_FILE_ATTRIBUTES) ? -L_ENOENT : 0;
    }

    case SYS_pread64: {
        HANDLE h = (a0 < MAX_FDS) ? g_fds[a0] : NULL;
        if (!h) return -L_EBADF;
        DWORD got = 0;
        __try {
            OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
            ov.Offset = (DWORD)a3; ov.OffsetHigh = (DWORD)(a3 >> 32);
            if (!ReadFile(h, (void *)a1, (DWORD)a2, &got, &ov)) {
                if (GetLastError() == ERROR_HANDLE_EOF) return 0;
                return -L_EINVAL;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        return (int64_t)got;
    }

    case SYS_brk: {
        if (a0 == 0) return (int64_t)g_brk_cur;
        if (a0 < g_brk_base || a0 > g_brk_end) return (int64_t)g_brk_cur;
        if (a0 > g_brk_cur) {
            uint64_t from = align_up(g_brk_cur, PAGE), to = align_up(a0, PAGE);
            if (to > from &&
                !VirtualAlloc((void *)from, (SIZE_T)(to - from), MEM_COMMIT, PAGE_READWRITE))
                return (int64_t)g_brk_cur;
        }
        g_brk_cur = a0;
        trace("brk(%p) = %p", (void*)a0, (void*)g_brk_cur);
        return (int64_t)g_brk_cur;
    }

    case SYS_mmap: {
        uint64_t len = align_up(a1, PAGE);
        if (len == 0) return -L_EINVAL;
        int fixed = (a3 & MAP_FIXED) != 0;
        int anon  = (a3 & MAP_ANONYMOUS) != 0;
        int64_t fd = (int64_t)a4;
        void *want = (fixed || a0) ? (void *)a0 : NULL;
        DWORD finalprot = mmap_prot_to_win(a2);
        void *got;

        /* Pure reservation (ld.so reserves a library's whole span first). */
        if (anon && a2 == 0 && !fixed) {
            got = VirtualAlloc(want, (SIZE_T)len, MEM_RESERVE, PAGE_NOACCESS);
            if (!got && want) got = VirtualAlloc(NULL, (SIZE_T)len, MEM_RESERVE, PAGE_NOACCESS);
            if (!got) return -L_ENOMEM;
            trace("mmap(reserve %p, %llu) = %p", want, len, got);
            return (int64_t)(uint64_t)got;
        }

        if (fixed) {   /* commit into an already-reserved region (MAP_FIXED overlay) */
            got = VirtualAlloc(want, (SIZE_T)len, MEM_COMMIT, PAGE_READWRITE);
            if (!got) got = VirtualAlloc(want, (SIZE_T)len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        } else {
            got = VirtualAlloc(want, (SIZE_T)len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!got && want) got = VirtualAlloc(NULL, (SIZE_T)len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (!got) return -L_ENOMEM;

        if (!anon && fd >= 0 && fd < MAX_FDS && g_fds[fd]) {
            __try {
                OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
                ov.Offset = (DWORD)a5; ov.OffsetHigh = (DWORD)(a5 >> 32);
                DWORD rd = 0; ReadFile(g_fds[fd], got, (DWORD)a1, &rd, &ov);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }

        if (a2 & PROT_EXEC) patch_syscalls(got, (size_t)len);   /* libraries have syscalls too */
        if (finalprot != PAGE_READWRITE) { DWORD op; VirtualProtect(got, (SIZE_T)len, finalprot, &op); }

        trace("mmap(%p, %llu, prot=%llu, flags=0x%llx, fd=%lld) = %p",
              (void *)a0, a1, a2, a3, fd, got);
        return (int64_t)(uint64_t)got;
    }

    case SYS_mprotect: {
        uint64_t len = align_up(a1, PAGE);
        DWORD oldp, newp = mmap_prot_to_win(a2);
        if (a2 & PROT_EXEC) {   /* gaining +X: make writable, patch, then set target */
            DWORD tmp;
            if (VirtualProtect((void *)a0, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &tmp))
                patch_syscalls((void *)a0, (size_t)len);
        }
        if (!VirtualProtect((void *)a0, (SIZE_T)len, newp, &oldp))
            return -L_EINVAL;
        return 0;
    }

    case SYS_munmap:
        VirtualFree((void *)a0, 0, MEM_RELEASE);
        return 0;

    case SYS_arch_prctl:
        if (a0 == ARCH_SET_FS) {
            _writefsbase_u64(a1);             /* set guest thread pointer */
            trace("arch_prctl(SET_FS, %p)", (void*)a1);
            return 0;
        }
        if (a0 == ARCH_GET_FS) {
            __try { *(uint64_t *)a1 = _readfsbase_u64(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
            return 0;
        }
        return -L_EINVAL;

    case SYS_set_tid_address: trace("set_tid_address() = 1"); return 1;
    case SYS_set_robust_list: return 0;
    case SYS_rseq:            return -L_ENOSYS;
    case SYS_prlimit64:       return 0;
    case SYS_sigaltstack:     return 0;

    case SYS_rt_sigaction: {
        if (a0 < 1 || a0 > 64) return -L_EINVAL;
        if (a2) __try {
            ksigaction *o = (ksigaction *)a2; *o = g_sig[a0];
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        if (a1) __try {
            ksigaction *n = (ksigaction *)a1; g_sig[a0] = *n;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        trace("rt_sigaction(%llu)", a0);
        return 0;
    }

    case SYS_rt_sigprocmask: {
        if (a2) __try { *(uint64_t *)a2 = g_blocked; }
                __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        if (a1) {
            uint64_t set;
            __try { set = *(uint64_t *)a1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
            if (a0 == 0)      g_blocked |= set;     /* SIG_BLOCK   */
            else if (a0 == 1) g_blocked &= ~set;    /* SIG_UNBLOCK */
            else if (a0 == 2) g_blocked = set;      /* SIG_SETMASK */
        }
        g_blocked &= ~((1ULL << 8) | (1ULL << 18)); /* SIGKILL/SIGSTOP unblockable */
        return 0;
    }

    case SYS_kill:
    case SYS_tgkill: {
        uint64_t sig = (nr == SYS_kill) ? a1 : a2;
        if (sig == 0) return 0;                 /* existence probe */
        if (sig < 1 || sig > 64) return -L_EINVAL;
        g_pending |= 1ULL << (sig - 1);         /* prototype: target is always self */
        trace("%s(sig=%llu)", nr == SYS_kill ? "kill" : "tgkill", sig);
        return 0;
    }

    case SYS_fork:
    case SYS_vfork:
    case SYS_clone:
        fprintf(stderr, "lxrun: fork/clone requires lxcore.sys "
                        "(see docs/process-model.md) -> -ENOSYS\n");
        return -L_ENOSYS;

    case SYS_wait4:
        return -L_ECHILD;                       /* nothing to wait for w/o fork */

    case SYS_getpid:  return 1;
    case SYS_getppid: return 0;
    case SYS_gettid:  return 1;
    case SYS_getuid:  case SYS_geteuid: return 0;
    case SYS_getgid:  case SYS_getegid: return 0;

    case SYS_uname: {
        __try {
            char *u = (char *)a0;          /* struct utsname: 6 x 65-byte fields */
            memset(u, 0, 6 * 65);
            strcpy(u + 0 * 65, "Linux");
            strcpy(u + 1 * 65, "zenith");
            strcpy(u + 2 * 65, "5.2.0-zenith");
            strcpy(u + 3 * 65, "#1 zenith-linux");
            strcpy(u + 4 * 65, "x86_64");
            strcpy(u + 5 * 65, "(none)");
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        return 0;
    }

    case SYS_readlink:
        return -L_ENOENT;

    case SYS_getrandom: {
        __try {
            unsigned char *b = (unsigned char *)a0;
            unsigned __int64 s = __rdtsc() ^ 0xA5A5A5A5DEADBEEFULL;
            for (uint64_t i = 0; i < a1; i++) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                b[i] = (unsigned char)s;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        return (int64_t)a1;
    }

    case SYS_gettimeofday: {
        __try {
            FILETIME ft; GetSystemTimeAsFileTime(&ft);
            uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            t = (t - 116444736000000000ULL) / 10;        /* -> microseconds */
            uint64_t *tv = (uint64_t *)a0;
            tv[0] = t / 1000000; tv[1] = t % 1000000;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        return 0;
    }

    case SYS_clock_gettime: {
        __try {
            FILETIME ft; GetSystemTimeAsFileTime(&ft);
            uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            t = (t - 116444736000000000ULL) * 100;        /* -> nanoseconds */
            uint64_t *ts = (uint64_t *)a1;
            ts[0] = t / 1000000000ULL; ts[1] = t % 1000000000ULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -L_EFAULT; }
        return 0;
    }

    case SYS_time: {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        t = (t - 116444736000000000ULL) / 10000000ULL;
        if (a0) __try { *(uint64_t *)a0 = t; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return (int64_t)t;
    }

    case SYS_exit:
    case SYS_exit_group:
        if (g_verbose) fprintf(stderr, "lxrun: guest exit_group(%lld)\n", (int64_t)a0);
        fflush(NULL);
        ExitProcess((UINT)a0);               /* does not return */

    default:
        fprintf(stderr, "lxrun: UNIMPLEMENTED syscall %llu "
                "(args %llx %llx %llx %llx) -> -ENOSYS\n",
                nr, a0, a1, a2, a3);
        return -L_ENOSYS;
    }
}

/* ========================================================================= */
/*  Vectored exception handler: catch ud2 (former syscall) + guest faults     */
/* ========================================================================= */

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    uint64_t rip = (uint64_t)er->ExceptionAddress;

    if (er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        unsigned char *p = (unsigned char *)rip;
        int is_ud2 = 0;
        __try { is_ud2 = (p[0] == 0x0F && p[1] == 0x0B); }
        __except (EXCEPTION_EXECUTE_HANDLER) { is_ud2 = 0; }
        if (is_ud2) {
            uint64_t nr = c->Rax;            /* Linux syscall number */

            if (nr == SYS_rt_sigreturn) {    /* restore pre-signal context */
                if (g_sigsp > 0) {
                    g_sigsp--;
                    uint64_t ob = g_sigstack[g_sigsp].oldblocked;
                    *c = g_sigstack[g_sigsp].ctx;
                    g_blocked = ob;
                }
                return EXCEPTION_CONTINUE_EXECUTION;   /* c fully restored */
            }

            if (nr == SYS_execve) {          /* replaces image; sets c on success */
                int64_t r = do_execve(c);
                if (r != 0) { c->Rax = (uint64_t)r; c->Rip = rip + 2; }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            int64_t r = emulate(c);          /* may ExitProcess for exit_group */
            c->Rax = (uint64_t)r;
            c->Rip = rip + 2;                /* step over the ud2 */
            deliver_pending(c);              /* signals delivered on return to user */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    /* A genuine guest fault -> a synchronous Linux signal. Anything outside
     * lxrun's own module is guest code (main image, ld.so, mapped libraries,
     * the guest stack); a fault inside lxrun itself is a harness bug. */
    if (rip < g_self_lo || rip >= g_self_hi) {
        int sig = (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)      ? 11  /* SIGSEGV */
                : (er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)    ? 8   /* SIGFPE  */
                : (er->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION)      ? 4   /* SIGILL  */
                                                                         : 11;
        if (g_sig[sig].handler != SIG_DFL && g_sig[sig].handler != SIG_IGN) {
            g_pending |= 1ULL << (sig - 1);
            deliver_pending(c);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        fprintf(stderr, "lxrun: guest fault code=0x%08lx at rip=%p "
                "(rax=%llx rdi=%llx rsi=%llx) -> signal %d\n",
                er->ExceptionCode, (void *)rip, c->Rax, c->Rdi, c->Rsi, sig);
        ExitProcess(128 + sig);
    }
    return EXCEPTION_CONTINUE_SEARCH;         /* not ours (harness bug) */
}

/* ========================================================================= */
/*  Entry                                                                     */
/* ========================================================================= */

static DWORD WINAPI guest_stub(LPVOID arg) { (void)arg; return 0; }

static int have_fsgsbase(void)
{
    int regs[4];
    __cpuidex(regs, 7, 0);
    return (regs[1] /* EBX */ & 1) != 0;      /* FSGSBASE bit */
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, g_root, MAX_PATH);
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr,
            "usage: lxrun [-v] [--root <winpath>] <elf-file> [guest args...]\n");
        return 2;
    }

    const char *elf_mb = argv[i];
    wchar_t elf_w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, elf_mb, -1, elf_w, MAX_PATH);

    if (!have_fsgsbase())
        fprintf(stderr, "lxrun: WARNING CPU/OS lacks user FSGSBASE; TLS-using "
                        "guests (arch_prctl SET_FS) will fault.\n");

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in  = GetStdHandle(STD_INPUT_HANDLE);
    g_err = GetStdHandle(STD_ERROR_HANDLE);
    g_fds[0] = g_in; g_fds[1] = g_out; g_fds[2] = g_err;

    /* Record lxrun's own module range so the fault handler can tell guest
     * code (everything else) from a harness bug (inside lxrun). */
    {
        HMODULE self = GetModuleHandleW(NULL);
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)self;
        IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)((unsigned char *)self + dos->e_lfanew);
        g_self_lo = (uint64_t)self;
        g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    }

    init_signals();

    loaded_t ld;
    load_elf(elf_w, &ld);

    /* Guest argv = [elf basename or path] + remaining host args. */
    int gargc = argc - i;
    char *gargv[64];
    for (int k = 0; k < gargc && k < 64; k++) gargv[k] = argv[i + k];
    static char *defenv[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "TERM=linux", "HOME=/root", "PWD=/", "USER=root",
    };
    uint64_t rsp = build_stack(&ld, gargc, gargv, 5, defenv);

    if (g_verbose)
        fprintf(stderr, "lxrun: entry=%p rsp=%p (%s)\n",
                (void *)ld.entry, (void *)rsp, ld.is_dyn ? "PIE" : "ET_EXEC");

    if (!AddVectoredExceptionHandler(1, veh)) die("AddVectoredExceptionHandler failed");

    HANDLE th = CreateThread(NULL, 64 * 1024, guest_stub, NULL, CREATE_SUSPENDED, NULL);
    if (!th) die("CreateThread failed");

    CONTEXT c; memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) die("GetThreadContext failed");
    c.Rip = ld.entry;
    c.Rsp = rsp;
    c.Rdx = 0;                 /* ABI: rtld_fini pointer = NULL */
    c.Rax = c.Rbx = c.Rcx = c.Rbp = c.Rsi = c.Rdi = 0;
    c.R8 = c.R9 = c.R10 = c.R11 = c.R12 = c.R13 = c.R14 = c.R15 = 0;
    c.EFlags = 0x202;          /* IF + reserved; no trap flag (patch mode) */
    if (!SetThreadContext(th, &c)) die("SetThreadContext failed");
    ResumeThread(th);

    WaitForSingleObject(th, INFINITE);   /* guest normally ExitProcess()es */
    return 0;
}
