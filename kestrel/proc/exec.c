/* proc/exec.c -- load a static ELF64 program and run it in ring 3
 *
 * exec_spawn() creates a kernel thread; that thread builds the address
 * space from inside the new process (so it can write user memory directly
 * with its own CR3 loaded), opens stdin/stdout/stderr on its TTY and drops
 * to ring 3 through enter_user(). From then on the thread re-enters the
 * kernel only through syscalls, interrupts and faults. */
#include <kernel/exec.h>
#include <kernel/task.h>
#include <kernel/uvm.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/time.h>

extern void enter_user(uint64_t rip, uint64_t rsp) __attribute__((noreturn));

/* ---- ELF64 ----------------------------------------------------------------- */
#define EI_NIDENT   16
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_INTERP   3
#define PT_PHDR     6

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
#define AT_EXECFN   31

#define MAX_PHDRS   16

struct spawn_args {
    char  path[128];
    char  stdio[32];            /* device for fds 0-2 ("" = the caller's TTY) */
    int   argc;
    char *argv[EXEC_MAX_ARGS];
};

static uint64_t page_down(uint64_t a) { return a & ~(UVM_PAGE - 1); }
static uint64_t page_up(uint64_t a)   { return (a + UVM_PAGE - 1) & ~(UVM_PAGE - 1); }

static int read_at(struct file *f, uint64_t off, void *buf, uint64_t len)
{
    if (vfs_lseek(f, (off_t)off, 0) != (off_t)off) return -EIO;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = vfs_read(f, p, len);
        if (n <= 0) return n < 0 ? (int)n : -EIO;
        p += n; len -= (uint64_t)n;
    }
    return 0;
}

/* Load the PT_LOAD segments of `path` into `t`'s (current) address space. */
static int load_elf(struct tcb *t, const char *path, uint64_t *entry, uint64_t *phdr_va, int *phnum)
{
    struct file *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) { kprintf("exec: %s: open failed (%d)\n", path, rc); return rc; }

    struct elf64_ehdr eh;
    struct elf64_phdr ph[MAX_PHDRS];
    rc = read_at(f, 0, &eh, sizeof eh);
    if (rc) goto out;
    rc = -ENOEXEC;
    if (memcmp(eh.e_ident, "\177ELF", 4) || eh.e_ident[4] != 2 || eh.e_ident[5] != 1) {
        kprintf("exec: %s: not a 64-bit little-endian ELF file\n", path); goto out;
    }
    if (eh.e_machine != EM_X86_64 || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_phentsize != sizeof(struct elf64_phdr) || !eh.e_phnum || eh.e_phnum > MAX_PHDRS) {
        kprintf("exec: %s: unsupported ELF (type %u, machine %u, %u program headers)\n",
                path, eh.e_type, eh.e_machine, eh.e_phnum);
        goto out;
    }
    if ((rc = read_at(f, eh.e_phoff, ph, (uint64_t)eh.e_phnum * sizeof *ph))) goto out;

    const uint64_t base = eh.e_type == ET_DYN ? UVM_IMAGE_BASE : 0;
    uint64_t end = 0;
    *phdr_va = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            kprintf("exec: %s: dynamically linked programs are not supported yet\n", path);
            rc = -ENOEXEC; goto out;
        }
        if (ph[i].p_type == PT_PHDR) *phdr_va = base + ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        uint64_t va = base + ph[i].p_vaddr;
        if (ph[i].p_filesz > ph[i].p_memsz || !uvm_range_ok(page_down(va), page_up(va + ph[i].p_memsz) - page_down(va))) {
            kprintf("exec: %s: segment %d at %lx+%lx is outside the user half\n", path, i, va, ph[i].p_memsz);
            rc = -ENOEXEC; goto out;
        }
        /* W^X comes later: every segment is mapped writable for now */
        if (!uvm_map(t->pml4, page_down(va), page_up(va + ph[i].p_memsz) - page_down(va), UVM_W | UVM_X)) {
            rc = -ENOMEM; goto out;
        }
        if (ph[i].p_filesz && (rc = read_at(f, ph[i].p_offset, (void *)va, ph[i].p_filesz))) goto out;
        memset((void *)(va + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
        if (!*phdr_va && ph[i].p_offset == 0) *phdr_va = va + eh.e_phoff;   /* headers inside segment */
        if (va + ph[i].p_memsz > end) end = va + ph[i].p_memsz;
        kprintf("exec: %s: segment %lx-%lx (%lu bytes from file)\n", path, va, va + ph[i].p_memsz, ph[i].p_filesz);
    }
    if (!end) { kprintf("exec: %s: no loadable segment\n", path); rc = -ENOEXEC; goto out; }
    *entry = base + eh.e_entry;
    *phnum = eh.e_phnum;
    t->brk_start = t->brk = page_up(end);
    t->mmap_next = UVM_MMAP_BASE;
    rc = 0;
out:
    vfs_close(f);
    return rc;
}

/* System V initial stack: strings at the top, then (16-byte aligned at
 * argc) argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL. */
static uint64_t build_stack(struct tcb *t, const struct spawn_args *a, uint64_t entry, uint64_t phdr_va, int phnum)
{
    static const char *const envp[] = { "PATH=/boot/bin", "HOME=/", "TERM=kestrel" };
    const int nenv = (int)(sizeof envp / sizeof *envp);
    uint64_t sp = UVM_STACK_TOP;
    uint64_t argv_va[EXEC_MAX_ARGS], env_va[3];

#define PUSH_BYTES(src, n) do { sp -= (n); memcpy((void *)sp, (src), (n)); } while (0)
    for (int i = a->argc - 1; i >= 0; i--) { PUSH_BYTES(a->argv[i], strlen(a->argv[i]) + 1); argv_va[i] = sp; }
    for (int i = nenv - 1; i >= 0; i--) { PUSH_BYTES(envp[i], strlen(envp[i]) + 1); env_va[i] = sp; }
    PUSH_BYTES("x86_64", 7);
    uint64_t platform = sp;
    uint64_t execfn = argv_va[0];
    uint8_t rnd[16];
    uint64_t seed = time_ms() ^ (uint64_t)t->pid << 32 ^ rdtsc();
    for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; rnd[i] = (uint8_t)(seed >> 56); }
    PUSH_BYTES(rnd, 16);
    uint64_t random = sp;
#undef PUSH_BYTES

    const uint64_t auxv[][2] = {
        { AT_PHDR, phdr_va }, { AT_PHENT, sizeof(struct elf64_phdr) }, { AT_PHNUM, (uint64_t)phnum },
        { AT_PAGESZ, UVM_PAGE }, { AT_BASE, 0 }, { AT_ENTRY, entry }, { AT_UID, t->uid }, { AT_EUID, t->uid },
        { AT_GID, t->gid }, { AT_EGID, t->gid }, { AT_PLATFORM, platform }, { AT_HWCAP, 0 },
        { AT_CLKTCK, 100 }, { AT_SECURE, 0 }, { AT_RANDOM, random }, { AT_EXECFN, execfn }, { AT_NULL, 0 },
    };
    const uint64_t words = 1 + (uint64_t)a->argc + 1 + (uint64_t)nenv + 1 + 2 * (sizeof auxv / sizeof *auxv);
    sp = (sp - words * 8) & ~0xFull;                        /* rsp % 16 == 0 at argc */
    uint64_t *w = (uint64_t *)sp;
    *w++ = (uint64_t)a->argc;
    for (int i = 0; i < a->argc; i++) *w++ = argv_va[i];
    *w++ = 0;
    for (int i = 0; i < nenv; i++) *w++ = env_va[i];
    *w++ = 0;
    for (size_t i = 0; i < sizeof auxv / sizeof *auxv; i++) { *w++ = auxv[i][0]; *w++ = auxv[i][1]; }
    return sp;
}

static void open_stdio(struct tcb *t, const char *dev)
{
    for (int fd = 0; fd < 3; fd++) {
        if (t->fds[fd]) continue;
        struct file *f;
        if ((dev[0] && vfs_open(dev, O_RDWR, 0, &f) == 0) || vfs_open("/dev/tty", O_RDWR, 0, &f) == 0 || vfs_open("/dev/console", O_RDWR, 0, &f) == 0) t->fds[fd] = f;
    }
}

static int process_main(void *arg)
{
    struct spawn_args *a = arg;
    struct tcb *t = current_task();

    uint64_t pml4 = uvm_create();
    if (!pml4) { kprintf("exec: %s: out of memory\n", a->path); kfree(a); return 127; }
    uint64_t f = irq_save();
    t->pml4 = pml4;
    t->cr3 = pml4;
    write_cr3(pml4);
    irq_restore(f);

    uint64_t entry = 0, phdr_va = 0, sp;
    int phnum = 0;
    if (load_elf(t, a->path, &entry, &phdr_va, &phnum) < 0 ||
        !uvm_map(pml4, UVM_STACK_TOP - UVM_STACK_SIZE, UVM_STACK_SIZE, UVM_W)) {
        kfree(a);
        return 127;
    }
    sp = build_stack(t, a, entry, phdr_va, phnum);
    open_stdio(t, a->stdio);
    kprintf("exec: pid %d: %s entry %lx, stack %lx, brk %lx, %lu user pages\n", t->pid, a->path, entry, sp, t->brk,
            uvm_pages(pml4));
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

int exec_spawn_io(const char *path, int argc, char *const argv[], const char *stdio)
{
    if (!path || argc < 1 || argc > EXEC_MAX_ARGS) return -EINVAL;
    struct stat st;
    if (vfs_stat(path, &st) < 0) return -ENOENT;
    struct spawn_args *a = kzalloc(sizeof *a);
    if (!a) return -ENOMEM;
    strlcpy(a->path, path, sizeof a->path);
    if (stdio) strlcpy(a->stdio, stdio, sizeof a->stdio);
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
