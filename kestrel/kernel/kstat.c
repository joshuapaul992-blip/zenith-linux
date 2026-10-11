/* kernel/kstat.c -- performance counters (see kstat.h) */
#include <kernel/kstat.h>
#include <kernel/smp.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/arch.h>
#include <kernel/syscall.h>
#include <kernel/block.h>
#include <kernel/ext2.h>
#include <kernel/pagecache.h>
#include <kernel/report.h>
#include <kernel/string.h>

#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))

uint64_t kstat_us(uint64_t cyc)
{
    uint64_t hz = tsc_hz();
    if (!hz) return 0;
    return cyc / hz * 1000000ull + cyc % hz * 1000000ull / hz;
}

void kstat_cpu_online(void)
{
    struct kstat_cpu *k = &this_cpu()->ks;
    k->online_tsc = k->switch_tsc = rdtsc();
}

void kstat_switch(struct tcb *prev, struct tcb *next)
{
    (void)next;
    struct kstat_cpu *k = &this_cpu()->ks;
    uint64_t now = rdtsc();
    prev->ks.run_cyc += now - k->switch_tsc;
    k->switch_tsc = now;
    if (k->idle_tsc) { k->idle_cyc += now - k->idle_tsc; k->idle_tsc = 0; }    /* woken into a task */
}

uint64_t kstat_run_cycles(struct tcb *t)
{
    return t->ks.run_cyc + (rdtsc() - this_cpu()->ks.switch_tsc);
}

static void add(struct kstat_task *to, const struct kstat_task *from)
{
    const uint64_t *s = (const uint64_t *)from;
    uint64_t *d = (uint64_t *)to;
    for (size_t i = 0; i < sizeof *to / sizeof(uint64_t); i++) d[i] += s[i];
}

void kstat_thread_exit(struct tcb *t)
{
    t->ks.run_cyc += rdtsc() - this_cpu()->ks.switch_tsc;
    this_cpu()->ks.switch_tsc = rdtsc();
    if (t->signal) add(&t->signal->dead, &t->ks);
}

void kstat_fault(struct tcb *t, int kind, uint64_t cyc)
{
    struct kstat_cpu *k = &this_cpu()->ks;
    if (kind < 0 || kind >= KF_KINDS) kind = KF_OTHER;
    k->fault_n[kind]++;
    k->fault_cyc[kind] += cyc;
    if (!t) return;
    t->ks.fault_n++;
    t->ks.fault_cyc += cyc;
    if (kind == KF_FILE) t->ks.file_pages++;
}

void kstat_io(uint64_t bytes, uint64_t cyc)
{
    struct kstat_cpu *k = &this_cpu()->ks;
    k->io_n++; k->io_bytes += bytes; k->io_cyc += cyc;
    struct tcb *t = current_task();
    if (t) { t->ks.io_n++; t->ks.io_bytes += bytes; t->ks.io_cyc += cyc; }
}

/* ---- /proc/kstat ------------------------------------------------------------------ */
/* A name usable in a key: no spaces or '=' */
static void keyname(char *out, size_t cap, const char *in)
{
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = (in[i] == ' ' || in[i] == '=' || in[i] == '\t') ? '_' : in[i];
    out[i] = 0;
    if (!i && cap > 1) { out[0] = '?'; out[1] = 0; }
}

static size_t task_fields(char *buf, size_t cap, const struct kstat_task *s)
{
    size_t n = 0;
    P(" run_us=%lu sys=%lu sys_us=%lu sys_wall_us=%lu fast=%lu lockwaits=%lu lockwait_us=%lu",
      kstat_us(s->run_cyc), s->sys_n, kstat_us(s->sys_cyc), kstat_us(s->sys_wall), s->fast_n,
      s->lock_n, kstat_us(s->lock_cyc));
    P(" faults=%lu fault_us=%lu file_pages=%lu io=%lu io_kib=%lu io_us=%lu\n",
      s->fault_n, kstat_us(s->fault_cyc), s->file_pages, s->io_n, s->io_bytes / 1024, kstat_us(s->io_cyc));
    return n;
}

/* The live threads of process tgid (zombies were folded into ->dead) plus
 * the exited ones. */
static int process_sum(int tgid, struct kstat_task *out, struct tcb **leader)
{
    memset(out, 0, sizeof *out);
    *leader = NULL;
    int threads = 0;
    const struct signal_struct *sig = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED || t->tgid != tgid || !t->user) continue;
        if (t->pid == tgid) *leader = t;
        if (!sig) sig = t->signal;
        if (t->state == TASK_ZOMBIE) continue;
        struct kstat_task s = t->ks;
        if (t == current_task()) s.run_cyc += rdtsc() - this_cpu()->ks.switch_tsc;
        add(out, &s);
        threads++;
    }
    if (sig) add(out, &sig->dead);
    return threads;
}

static const char *const fault_names[KF_KINDS] = { "anon", "file", "shared", "cow", "other" };

size_t kstat_gen(char *buf, size_t cap)
{
    size_t n = 0;
    uint64_t now = rdtsc();
    P("# Kestrel performance counters: totals since boot, times in microseconds.\n");
    P("# `kstat N` shows the change over N seconds, `kstat CMD ARGS` while CMD runs.\n");
    P("# TSC %lu kHz, %d CPUs\n", tsc_hz() / 1000, ncpus);
    P("time uptime_us=%lu\n", uptime_ms() * 1000);

    /* CPUs: user_us is everything outside the big lock (user code, and the
     * few kernel paths that run without it) */
    struct kstat_cpu sum;
    memset(&sum, 0, sizeof sum);
    uint64_t tot_all = 0, tlb_all = 0, ipi_all = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = &cpus[i];
        if (!c->online || !c->ks.online_tsc) continue;
        struct kstat_cpu k = c->ks;
        if (k.idle_tsc) k.idle_cyc += now - k.idle_tsc;
        if (k.lock_since && __atomic_load_n(&c->bkl_depth, __ATOMIC_RELAXED)) k.lock_hold_cyc += now - k.lock_since;
        uint64_t total = now - k.online_tsc;
        uint64_t busy = k.lock_hold_cyc + k.lock_spin_cyc + k.idle_cyc;
        uint64_t user = total > busy ? total - busy : 0;
        P("cpu%d total_us=%lu user_us=%lu kernel_us=%lu lockwait_us=%lu idle_us=%lu irqs=%lu irq_us=%lu"
          " ticks=%lu tlb_flushes=%lu ipis=%lu\n", i, kstat_us(total), kstat_us(user), kstat_us(k.lock_hold_cyc),
          kstat_us(k.lock_spin_cyc), kstat_us(k.idle_cyc), k.irq_n, kstat_us(k.irq_cyc), c->ticks, c->tlb_flushes, c->ipis);
        tot_all += total; tlb_all += c->tlb_flushes; ipi_all += c->ipis;
        sum.lock_hold_cyc += k.lock_hold_cyc; sum.lock_spin_cyc += k.lock_spin_cyc; sum.idle_cyc += k.idle_cyc;
        sum.lock_acq += k.lock_acq; sum.lock_spin_n += k.lock_spin_n;
        sum.irq_n += k.irq_n; sum.irq_cyc += k.irq_cyc;
        sum.sys_n += k.sys_n; sum.sys_cyc += k.sys_cyc; sum.fast_n += k.fast_n;
        for (int f = 0; f < KF_KINDS; f++) { sum.fault_n[f] += k.fault_n[f]; sum.fault_cyc[f] += k.fault_cyc[f]; }
        sum.fault_fast_n += k.fault_fast_n;
        sum.io_n += k.io_n; sum.io_bytes += k.io_bytes; sum.io_cyc += k.io_cyc;
    }
    uint64_t busy = sum.lock_hold_cyc + sum.lock_spin_cyc + sum.idle_cyc;
    P("cpus total_us=%lu user_us=%lu kernel_us=%lu lockwait_us=%lu idle_us=%lu irqs=%lu irq_us=%lu"
      " tlb_flushes=%lu ipis=%lu\n", kstat_us(tot_all), kstat_us(tot_all > busy ? tot_all - busy : 0),
      kstat_us(sum.lock_hold_cyc), kstat_us(sum.lock_spin_cyc), kstat_us(sum.idle_cyc), sum.irq_n,
      kstat_us(sum.irq_cyc), tlb_all, ipi_all);
    P("lock acquired=%lu contended=%lu wait_us=%lu hold_us=%lu\n", sum.lock_acq, sum.lock_spin_n,
      kstat_us(sum.lock_spin_cyc), kstat_us(sum.lock_hold_cyc));
    P("sched switches=%lu\n", sched_context_switches());
    P("syscalls slow=%lu fast=%lu cpu_us=%lu\n", sum.sys_n, sum.fast_n, kstat_us(sum.sys_cyc));
    P("faults");
    for (int f = 0; f < KF_KINDS; f++) P(" %s=%lu %s_us=%lu", fault_names[f], sum.fault_n[f], fault_names[f], kstat_us(sum.fault_cyc[f]));
    P(" lockless=%lu\n", sum.fault_fast_n);
    P("io requests=%lu kib=%lu wait_us=%lu\n", sum.io_n, sum.io_bytes / 1024, kstat_us(sum.io_cyc));
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *b = blk_get(i);
        if (!b || b->parent) continue;
        P("disk.%s reads=%lu read_kib=%lu writes=%lu write_kib=%lu busy_us=%lu\n", b->name, b->rd_n,
          b->rd_bytes / 1024, b->wr_n, b->wr_bytes / 1024, kstat_us(b->busy_cyc));
    }
    uint64_t hits, misses;
    ext2_cache_stats(&hits, &misses);
    P("ext2 cache_hits=%lu cache_misses=%lu\n", hits, misses);
    struct pcache_stats pc;
    pcache_stats(&pc);
    P("pagecache cached_files=%lu cached_pages=%lu hits=%lu misses=%lu readahead=%lu evicted=%lu\n",
      pc.files, pc.pages, pc.hits, pc.misses, pc.ahead, pc.evicted);

    /* system calls by on-CPU time */
    P("# system calls by time on the CPU (wall includes sleeping; max_cpu_us is the longest single call)\n");
    static int order[SYS_MAX];
    int cnt = 0;
    for (int i = 0; i < SYS_MAX; i++) {
        uint64_t c, cpu, wall, mx;
        if (!syscall_cost(i, &c, &cpu, &wall, &mx) || !c) continue;
        int j = cnt++;
        while (j > 0) {                                 /* insertion: biggest first */
            uint64_t c2, cpu2, w2, m2;
            syscall_cost(order[j - 1], &c2, &cpu2, &w2, &m2);
            if (cpu2 >= cpu) break;
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int k = 0; k < cnt; k++) {
        uint64_t c, cpu, wall, mx;
        syscall_cost(order[k], &c, &cpu, &wall, &mx);
        const char *nm = syscall_name(order[k]);
        if (nm) P("sys.%s calls=%lu cpu_us=%lu wall_us=%lu max_cpu_us=%lu\n", nm, c, kstat_us(cpu), kstat_us(wall), kstat_us(mx));
        else P("sys.%d calls=%lu cpu_us=%lu wall_us=%lu max_cpu_us=%lu\n", order[k], c, kstat_us(cpu), kstat_us(wall), kstat_us(mx));
    }

    /* processes */
    P("# processes (all threads, exited ones included)\n");
    for (int i = 0; i < MAX_TASKS && n < cap; i++) {
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED || !t->user || t->pid != t->tgid) continue;
        struct kstat_task s;
        struct tcb *leader;
        int threads = process_sum(t->tgid, &s, &leader);
        char name[32];
        keyname(name, sizeof name, t->name);
        P("proc.%d.%s threads=%d", t->tgid, name, threads);
        n += task_fields(buf + n, n < cap ? cap - n : 0, &s);
    }
    return n;
}

size_t kstat_gen_process(int tgid, char *buf, size_t cap)
{
    size_t n = 0;
    struct kstat_task s;
    struct tcb *leader;
    int threads = process_sum(tgid, &s, &leader);
    if (!leader) return 0;
    P("# times in microseconds, totals since the process started\n");
    P("process threads=%d", threads);
    n += task_fields(buf + n, n < cap ? cap - n : 0, &s);
    for (int i = 0; i < MAX_TASKS && n < cap; i++) {
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || t->tgid != tgid || !t->user) continue;
        struct kstat_task ts = t->ks;
        if (t == current_task()) ts.run_cyc += rdtsc() - this_cpu()->ks.switch_tsc;
        char name[32];
        keyname(name, sizeof name, t->name);
        P("thread.%d.%s", t->pid, name);
        n += task_fields(buf + n, n < cap ? cap - n : 0, &ts);
    }
    if (leader->signal) {
        P("exited");
        n += task_fields(buf + n, n < cap ? cap - n : 0, &leader->signal->dead);
    }
    return n;
}

static size_t report_section(char *buf, size_t cap) { return kstat_gen(buf, cap); }

void kstat_init(void)
{
    report_add_section("Performance counters (/proc/kstat)", report_section);
}
