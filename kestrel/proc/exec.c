/* proc/exec.c -- load an ELF64 program (and its interpreter) and run it in ring 3
 *
 * exec_spawn() creates a kernel thread; that thread builds the address
 * space from inside the new process, opens stdin/stdout/stderr and drops to
 * ring 3 through enter_user(). execve() (proc/process.c) uses the same
 * exec_load_image() on a fresh address space.
 *
 * The loader treats the file as hostile:
 *   - header: ELF64, little endian, version 1, x86-64, ET_EXEC or ET_DYN,
 *     sane header and program-header sizes; the program headers lie inside
 *     the file
 *   - every PT_LOAD: file range inside the file, p_filesz <= p_memsz, no
 *     arithmetic overflow, p_align 0/1 or a power of two (its value is not
 *     otherwise trusted), p_vaddr and p_offset congruent modulo the page
 *     size, segments ascending, everything inside the user half
 *   - the entry point lies in an executable segment
 *   - segments are mapped with exactly their p_flags: text R-X, data RW-,
 *     read-only data R--; a segment asking for W+X is refused
 *     (kestrel.allow_wx=1 permits it); the stack is not executable unless
 *     PT_GNU_STACK asks for it
 * Segments are mapped from the file and paged in on demand (vm.h); the
 * zero-filled tail (bss) is anonymous memory.
 *
 * Dynamically linked programs: PT_INTERP names the interpreter (musl's
 * /lib/ld-musl-x86_64.so.1), loaded at UVM_INTERP_BASE; the process starts
 * there with AT_BASE, AT_PHDR, AT_ENTRY in the auxiliary vector. */
#include <kernel/exec.h>
#include <kernel/task.h>
#include <kernel/uvm.h>
#include <kernel/vm.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/termios.h>
#include <kernel/uaccess.h>
#include <kernel/bootinfo.h>

extern void enter_user(uint64_t rip, uint64_t rsp) __attribute__((noreturn));

/* ---- ELF64 ----------------------------------------------------------------- */
#define EI_NIDENT   16
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_INTERP   3
#define PT_PHDR     6
#define PT_GNU_STACK 0x6474e551
#define PF_X        1
#define PF_W        2
#define PF_R        4

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31
#define AT_MINSIGSTKSZ 51

#define MAX_PHDRS   128
#define INTERP_MAX  256

struct spawn_args {
    char  path[128];
    char  stdio[32];            /* device for fds 0-2 ("" = the caller's TTY) */
    bool  ctty;                 /* stdio is a terminal: make it the controlling one */
    int   argc;
    char *argv[EXEC_MAX_ARGS];
};

/* What loading one ELF file produced. */
struct elf_image {
    uint64_t bias;              /* added to every p_vaddr (ET_DYN)           */
    uint64_t entry;
    uint64_t phdr;              /* VA of the program headers                */
    uint16_t phnum;
    uint64_t end;               /* end of the highest segment               */
    bool     exec_stack;
    char     interp[INTERP_MAX];
};

static uint64_t page_down(uint64_t a) { return a & ~(UVM_PAGE - 1); }
static uint64_t page_up(uint64_t a)   { return (a + UVM_PAGE - 1) & ~(UVM_PAGE - 1); }
static bool add_ok(uint64_t a, uint64_t b, uint64_t *sum) { *sum = a + b; return *sum >= a; }

static int read_exact(struct file *f, uint64_t off, void *buf, uint64_t len)
{
    ssize_t n = vfs_pread(f, buf, (size_t)len, off);
    return n == (ssize_t)len ? 0 : (n < 0 ? (int)n : -ENOEXEC);
}

#define BAD(...) do { kprintf("exec: %s: ", path); kprintf(__VA_ARGS__); kprintf("\n"); rc = -ENOEXEC; goto out; } while (0)

/* Validate and map `path` into mm. bias_dyn: where an ET_DYN file goes.
 * interp: fill img->interp from PT_INTERP (main program); otherwise a
 * PT_INTERP is refused (an interpreter has none). */
static int load_elf(struct mm *mm, const char *path, uint64_t bias_dyn, bool interp, struct elf_image *img)
{
    struct file *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) return rc;
    struct elf64_phdr *ph = NULL;
    memset(img, 0, sizeof *img);

    struct stat st;
    if ((rc = vfs_fstat(f, &st)) < 0) goto out;
    uint64_t fsize = (uint64_t)st.st_size;
    struct elf64_ehdr eh;
    if (fsize < sizeof eh) BAD("too small for an ELF header");
    if ((rc = read_exact(f, 0, &eh, sizeof eh))) goto out;
    if (memcmp(eh.e_ident, "\177ELF", 4)) BAD("not an ELF file");
    if (eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_ident[6] != 1 || eh.e_version != 1)
        BAD("not a version-1 64-bit little-endian ELF file");
    if (eh.e_machine != EM_X86_64) BAD("not an x86-64 program (machine %u)", eh.e_machine);
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) BAD("not an executable (type %u)", eh.e_type);
    if (eh.e_ehsize < sizeof eh || eh.e_phentsize != sizeof(struct elf64_phdr)) BAD("bad header sizes");
    if (!eh.e_phnum || eh.e_phnum > MAX_PHDRS) BAD("%u program headers", eh.e_phnum);
    uint64_t phbytes = (uint64_t)eh.e_phnum * sizeof *ph, phend;
    if ((eh.e_phoff & 7) || !add_ok(eh.e_phoff, phbytes, &phend) || phend > fsize) BAD("program headers outside the file");
    if (!(ph = kmalloc(phbytes))) { rc = -ENOMEM; goto out; }
    if ((rc = read_exact(f, eh.e_phoff, ph, phbytes))) goto out;

    const uint64_t bias = eh.e_type == ET_DYN ? bias_dyn : 0;
    img->bias = bias;
    uint64_t last = 0, phdr_va = 0;
    bool any = false, entry_ok = false;
    const bool allow_wx = strstr(g_boot.cmdline, "kestrel.allow_wx=1") != NULL;

    /* pass 1: check everything before mapping anything */
    for (int i = 0; i < eh.e_phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type == PT_GNU_STACK) img->exec_stack = p->p_flags & PF_X;
        if (p->p_type == PT_PHDR) phdr_va = bias + p->p_vaddr;
        if (p->p_type == PT_INTERP) {
            if (!interp) BAD("an interpreter cannot have an interpreter");
            uint64_t e;
            if (p->p_filesz < 2 || p->p_filesz > INTERP_MAX || !add_ok(p->p_offset, p->p_filesz, &e) || e > fsize)
                BAD("bad PT_INTERP");
            if ((rc = read_exact(f, p->p_offset, img->interp, p->p_filesz))) goto out;
            if (img->interp[p->p_filesz - 1] != 0 || strlen(img->interp) != p->p_filesz - 1 || img->interp[0] != '/')
                BAD("PT_INTERP is not one absolute path");
        }
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        uint64_t fend, va, vend;
        if (p->p_filesz > p->p_memsz) BAD("segment %d: file size above memory size", i);
        if (!add_ok(p->p_offset, p->p_filesz, &fend) || fend > fsize) BAD("segment %d: outside the file", i);
        if (p->p_align > 1 && (p->p_align & (p->p_align - 1))) BAD("segment %d: alignment %lx", i, p->p_align);
        if ((p->p_vaddr - p->p_offset) & (UVM_PAGE - 1)) BAD("segment %d: address and offset not congruent", i);
        if (!add_ok(bias, p->p_vaddr, &va) || !add_ok(va, p->p_memsz, &vend) || !uvm_range_ok(page_down(va), page_up(vend) - page_down(va)))
            BAD("segment %d at %lx+%lx is outside the user half", i, va, p->p_memsz);
        if (any && va < last) BAD("segments not in ascending order");
        if ((p->p_flags & PF_W) && (p->p_flags & PF_X) && !allow_wx)
            BAD("segment %d is writable and executable (refused; kestrel.allow_wx=1 permits it)", i);
        uint64_t e = bias + eh.e_entry;
        if ((p->p_flags & PF_X) && e >= va && e < vend) entry_ok = true;
        if (!phdr_va && eh.e_phoff >= p->p_offset && phend <= fend) phdr_va = va + (eh.e_phoff - p->p_offset);
        last = vend;
        any = true;
    }
    if (!any) BAD("no loadable segment");
    if (!entry_ok) BAD("entry point %lx is not in an executable segment", bias + eh.e_entry);

    /* pass 2: map (MAP_FIXED; this address space is brand new) */
    for (int i = 0; i < eh.e_phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        int prot = ((p->p_flags & PF_R) ? PROT_READ : 0) | ((p->p_flags & PF_W) ? PROT_WRITE : 0) |
                   ((p->p_flags & PF_X) ? PROT_EXEC : 0);
        uint64_t va = bias + p->p_vaddr, start = page_down(va);
        uint64_t fdata_end = va + p->p_filesz;           /* file bytes end here */
        uint64_t file_pages_end = p->p_filesz ? page_up(fdata_end) : start;
        uint64_t mem_end = page_up(va + p->p_memsz);
        int64_t r;
        if (file_pages_end > start) {
            r = vm_mmap(mm, start, file_pages_end - start, prot, MAP_PRIVATE | MAP_FIXED, f, NULL,
                        page_down(p->p_offset), fdata_end);
            if (r < 0) { rc = (int)r; goto out; }
        }
        if (mem_end > file_pages_end) {                 /* bss beyond the last file page */
            r = vm_mmap(mm, file_pages_end, mem_end - file_pages_end, prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                        NULL, NULL, 0, 0);
            if (r < 0) { rc = (int)r; goto out; }
        }
        if (va + p->p_memsz > img->end) img->end = va + p->p_memsz;
    }
    img->entry = bias + eh.e_entry;
    img->phdr = phdr_va;
    img->phnum = eh.e_phnum;
    rc = 0;
out:
    kfree(ph);
    vfs_close(f);                                       /* the mappings hold their own references */
    return rc;
}

/* System V initial stack: strings at the top, then (16-byte aligned at
 * argc) argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL. Built in
 * kernel memory and copied to the (demand-paged) stack in one checked copy. */
static uint64_t build_stack(struct tcb *t, const struct exec_args *a, const struct elf_image *main_img,
                            uint64_t interp_base)
{
    size_t strbytes = 7 + 16;                           /* "x86_64" + AT_RANDOM */
    for (int i = 0; i < a->argc; i++) strbytes += strlen(a->argv[i]) + 1;
    for (int i = 0; i < a->envc; i++) strbytes += strlen(a->envp[i]) + 1;
    const int nauxv = 21;
    size_t words = 1 + (size_t)a->argc + 1 + (size_t)a->envc + 1 + 2 * (size_t)nauxv;
    size_t total = ((strbytes + 15) & ~(size_t)15) + words * 8 + 16;
    if (total > (1u << 20)) return 0;
    uint8_t *k = kzalloc(total);
    if (!k) return 0;

    /* strings, top down, at their final addresses */
    const uint64_t top = UVM_STACK_TOP;
    uint64_t base = top - total;                        /* user address of k[0] */
    uint64_t pos = top;
    uint64_t *argv_va = kmalloc(sizeof(uint64_t) * (size_t)(a->argc + a->envc + 1));
    if (!argv_va) { kfree(k); return 0; }
    uint64_t *env_va = argv_va + a->argc;
#define PUT(src, n) do { pos -= (n); memcpy(k + (pos - base), (src), (n)); } while (0)
    for (int i = a->argc - 1; i >= 0; i--) { PUT(a->argv[i], strlen(a->argv[i]) + 1); argv_va[i] = pos; }
    for (int i = a->envc - 1; i >= 0; i--) { PUT(a->envp[i], strlen(a->envp[i]) + 1); env_va[i] = pos; }
    PUT("x86_64", 7);
    uint64_t platform = pos;
    uint8_t rnd[16];
    uint64_t seed = time_ns() ^ (uint64_t)t->pid << 32 ^ rdtsc();
    for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; rnd[i] = (uint8_t)(seed >> 56); }
    PUT(rnd, 16);
    uint64_t random = pos;
#undef PUT
    uint32_t ca, cb, cc, cd;
    cpuid(1, 0, &ca, &cb, &cc, &cd);
    const uint64_t auxv[][2] = {
        { AT_PHDR, main_img->phdr }, { AT_PHENT, sizeof(struct elf64_phdr) }, { AT_PHNUM, main_img->phnum },
        { AT_PAGESZ, UVM_PAGE }, { AT_BASE, interp_base }, { AT_FLAGS, 0 }, { AT_ENTRY, main_img->entry },
        { AT_UID, t->uid }, { AT_EUID, t->uid }, { AT_GID, t->gid }, { AT_EGID, t->gid },
        { AT_PLATFORM, platform }, { AT_HWCAP, cd }, { AT_HWCAP2, 0 }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 },
        { AT_RANDOM, random }, { AT_EXECFN, a->argc ? argv_va[0] : platform }, { AT_MINSIGSTKSZ, 2048 },
        { AT_NULL, 0 }, { AT_NULL, 0 },
    };
    _Static_assert(sizeof auxv / sizeof *auxv == 21, "nauxv");
    uint64_t sp = (pos - words * 8) & ~0xFull;          /* rsp % 16 == 0 at argc */
    uint64_t *w = (uint64_t *)(k + (sp - base));
    *w++ = (uint64_t)a->argc;
    for (int i = 0; i < a->argc; i++) *w++ = argv_va[i];
    *w++ = 0;
    for (int i = 0; i < a->envc; i++) *w++ = env_va[i];
    *w++ = 0;
    for (int i = 0; i < nauxv; i++) { *w++ = auxv[i][0]; *w++ = auxv[i][1]; }
    int bad = copy_to_user((void *)sp, k + (sp - base), top - sp);
    kfree(argv_va);
    kfree(k);
    return bad ? 0 : sp;
}

static int exec_load_image_elf(struct tcb *t, const char *path, const struct exec_args *a,
                               uint64_t *entry, uint64_t *sp)
{
    size_t n = 0;                                       /* /proc/PID/cmdline */
    for (int i = 0; i < a->argc && n < sizeof t->cmdline; i++) {
        size_t l = strlen(a->argv[i]) + 1;
        if (n + l > sizeof t->cmdline) l = sizeof t->cmdline - n;
        memcpy(t->cmdline + n, a->argv[i], l);
        n += l;
    }
    t->cmdline_len = (uint16_t)n;

    struct mm *mm = t->mm;
    struct elf_image img, interp;
    int rc = load_elf(mm, path, UVM_IMAGE_BASE, true, &img);
    if (rc < 0) return rc;
    uint64_t interp_base = 0, start = img.entry;
    if (img.interp[0]) {
        if ((rc = load_elf(mm, img.interp, UVM_INTERP_BASE, false, &interp)) < 0) {
            kprintf("exec: %s: interpreter %s: cannot load (%d)\n", path, img.interp, rc);
            return rc == -ENOENT ? -ENOENT : rc;
        }
        if (interp.bias != UVM_INTERP_BASE) {
            kprintf("exec: %s: interpreter %s must be position independent\n", path, img.interp);
            return -ENOEXEC;
        }
        interp_base = interp.bias;
        start = interp.entry;
    }
    int sprot = PROT_READ | PROT_WRITE | (img.exec_stack ? PROT_EXEC : 0);
    int64_t r = vm_mmap(mm, UVM_STACK_TOP - UVM_STACK_SIZE, UVM_STACK_SIZE, sprot,
                        MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, NULL, NULL, 0, 0);
    if (r < 0) return (int)r;
    vm_mark(mm, UVM_STACK_TOP - 1, VMA_STACK);
    mm->brk_start = mm->brk = page_up(img.end);
    *entry = start;
    *sp = build_stack(t, a, &img, interp_base);
    return *sp ? 0 : -ENOMEM;
}

/* "#!interpreter [arg]" on the first line: returns 1 and fills interp/arg,
 * 0 for anything else (ELF files), or -errno. */
static int read_shebang(const char *path, char *interp, size_t icap, char *arg, size_t acap)
{
    struct file *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) return rc;
    char line[128];
    ssize_t n = vfs_read(f, line, sizeof line - 1);
    vfs_close(f);
    if (n < 2 || line[0] != '#' || line[1] != '!') return 0;
    line[n] = 0;
    char *p = line + 2, *end = strchr(p, '\n');
    if (!end) return -ENOEXEC;                          /* line too long */
    *end = 0;
    while (*p == ' ' || *p == '\t') p++;
    char *q = p;
    while (*q && *q != ' ' && *q != '\t') q++;
    if (q == p) return -ENOEXEC;
    char *rest = q;
    if (*rest) { *rest++ = 0; while (*rest == ' ' || *rest == '\t') rest++; }
    for (char *e = rest + strlen(rest); e > rest && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
    strlcpy(interp, p, icap);
    strlcpy(arg, rest, acap);                           /* Linux: the rest is one argument */
    return 1;
}

int exec_load_image(struct tcb *t, const char *path, const struct exec_args *a, uint64_t *entry, uint64_t *sp)
{
    /* scripts: run the interpreter with [interp, arg?, path, argv[1..]] */
    char interp[128], iarg[128];
    int sb = read_shebang(path, interp, sizeof interp, iarg, sizeof iarg);
    if (sb < 0) return sb;
    if (sb == 1) {
        int extra = iarg[0] ? 2 : 1;
        char **v = kmalloc(sizeof(char *) * (size_t)(a->argc + extra + 1));
        if (!v) return -ENOMEM;
        int k = 0;
        v[k++] = interp;
        if (iarg[0]) v[k++] = iarg;
        v[k++] = (char *)path;
        for (int i = 1; i < a->argc; i++) v[k++] = a->argv[i];
        struct exec_args sa = { k, a->envc, v, a->envp };
        char i2[8], a2[8];
        int r = read_shebang(interp, i2, sizeof i2, a2, sizeof a2);
        if (r == 1) r = -ENOEXEC;                       /* the interpreter must be a program */
        else if (r == 0) r = exec_load_image_elf(t, interp, &sa, entry, sp);
        kfree(v);
        return r;
    }
    return exec_load_image_elf(t, path, a, entry, sp);
}

static void open_stdio(struct tcb *t, const char *dev)
{
    for (int fd = 0; fd < 3; fd++) {
        if (t->files->fd[fd]) continue;
        struct file *f;
        if ((dev[0] && vfs_open(dev, O_RDWR, 0, &f) == 0) || vfs_open("/dev/tty", O_RDWR, 0, &f) == 0 || vfs_open("/dev/console", O_RDWR, 0, &f) == 0) t->files->fd[fd] = f;
    }
}

static int process_main(void *arg)
{
    struct spawn_args *a = arg;
    struct tcb *t = current_task();

    struct mm *mm = mm_create();
    if (!mm) { kprintf("exec: %s: out of memory\n", a->path); kfree(a); return 127; }
    uint64_t f = irq_save();
    t->mm = mm;
    t->user = false;                                    /* kernel thread until enter_user */
    t->pml4 = t->cr3 = mm->pml4;
    write_cr3(mm->pml4);
    irq_restore(f);

    static char *const envp[] = { "PATH=/bin", "HOME=/", "TERM=kestrel", "DISPLAY=:0", "SHELL=/bin/sh" };
    static char *const console_envp[] = { "PATH=/bin", "HOME=/root", "TERM=linux", "SHELL=/bin/sh", "USER=root",
                                           "LOGNAME=root" };
    struct exec_args ea = { a->argc, (int)(sizeof envp / sizeof *envp), a->argv, (char **)envp };
    if (a->ctty) { ea.envc = (int)(sizeof console_envp / sizeof *console_envp); ea.envp = (char **)console_envp; }
    uint64_t entry = 0, sp = 0;
    t->user = true;                                     /* uaccess: copies target the user half */
    int rc = exec_load_image(t, a->path, &ea, &entry, &sp);
    t->user = false;
    if (rc < 0) {
        kprintf("exec: %s: cannot run (%d)\n", a->path, rc);
        kfree(a);
        return 127;
    }
    open_stdio(t, a->stdio);
    struct file *in = t->files->fd[0];
    if (a->ctty && in && in->vn->ops->fioctl)          /* a new session leader takes it */
        in->vn->ops->fioctl(in, TIOCSCTTY, 0);
    kprintf("exec: pid %d: %s entry %lx, stack %lx, brk %lx\n", t->pid, a->path, entry, sp, mm->brk);
    strlcpy(t->exe, a->path, sizeof t->exe);
    kfree(a);

    /* clean SSE state for the process: x87 reset, MXCSR default. Interrupts
     * stay off from here: the scheduler starts saving this state as soon
     * as t->user is set. */
    cli();
    __asm__ volatile("fninit; fxsave %0" : "=m"(t->fpu));
    *(uint32_t *)(t->fpu + 24) = 0x1f80;
    __asm__ volatile("fxrstor %0" :: "m"(t->fpu));
    t->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    t->user = true;
    enter_user(entry, sp);
}

int exec_wait(int pid) { return task_wait(pid); }

int exec_spawn(const char *path, int argc, char *const argv[]) { return exec_spawn_io(path, argc, argv, NULL); }

static int spawn(const char *path, int argc, char *const argv[], const char *stdio, bool ctty)
{
    if (!path || argc < 1 || argc > EXEC_MAX_ARGS) return -EINVAL;
    struct stat st;
    if (vfs_stat(path, &st) < 0) return -ENOENT;
    struct spawn_args *a = kzalloc(sizeof *a);
    if (!a) return -ENOMEM;
    strlcpy(a->path, path, sizeof a->path);
    if (stdio) strlcpy(a->stdio, stdio, sizeof a->stdio);
    a->ctty = ctty;
    a->argc = argc;
    for (int i = 0; i < argc; i++) {
        a->argv[i] = kstrdup(argv[i]);
        if (!a->argv[i]) { for (int j = 0; j < i; j++) kfree(a->argv[j]); kfree(a); return -ENOMEM; }
    }
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    struct tcb *t = task_create(name, process_main, a);
    if (!t) { for (int i = 0; i < argc; i++) kfree(a->argv[i]); kfree(a); return -EAGAIN; }
    return t->pid;
}

int exec_spawn_io(const char *path, int argc, char *const argv[], const char *stdio)
{
    return spawn(path, argc, argv, stdio, false);
}

int exec_spawn_tty(const char *path, int argc, char *const argv[], const char *tty)
{
    return spawn(path, argc, argv, tty, true);
}
