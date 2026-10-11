/* include/kernel/kstat.h -- performance counters: where the time goes
 *
 * Cheap counters on the paths that decide how fast the kernel is: the big
 * kernel lock (waiting for it, holding it), system calls (on-CPU and wall
 * time, per call number), page faults by kind, block I/O, interrupts and
 * halted time. Times are TSC cycles; nothing is locked or atomic, because
 * each CPU writes only its own struct kstat_cpu and each thread only its
 * own struct kstat_task (the per-call-number table is written with the
 * big lock held).
 *
 *   /proc/kstat       kernel-wide totals, the system calls by cost, and a
 *                     table of processes
 *   /proc/PID/kstat   one process: all its threads, those that have
 *                     exited included
 *   kstat             (user/kstat.c) the change between two snapshots:
 *                     over N seconds, or while a command runs
 *   REPORT.TXT        the same text, when the report is saved (the kernel
 *                     shell's `report` command), for machines without a
 *                     serial port
 *
 * Every line of /proc/kstat is "key name=value ...", all values cumulative
 * since boot (times in microseconds) except max_* and threads, so two
 * snapshots can be subtracted field by field. */
#ifndef KESTREL_KSTAT_H
#define KESTREL_KSTAT_H

#include <stdint.h>
#include <stddef.h>

/* What a page fault had to do */
enum kfault { KF_ANON, KF_FILE, KF_SHARED, KF_COW, KF_OTHER, KF_KINDS };

struct kstat_task {                     /* one thread; folded into its process at exit */
    uint64_t run_cyc;                   /* on a CPU (user and kernel)                 */
    uint64_t sys_n, sys_cyc, sys_wall;  /* system calls: count, on-CPU, wall           */
    uint64_t fast_n;                    /* answered without the big lock              */
    uint64_t lock_n, lock_cyc;          /* waits for the big lock, cycles spinning    */
    uint64_t fault_n, fault_cyc;        /* page faults resolved (wall)                */
    uint64_t file_pages;                /* pages read from files by page faults       */
    uint64_t io_n, io_bytes, io_cyc;    /* block requests issued (wall)               */
};

struct kstat_cpu {                      /* one CPU */
    uint64_t online_tsc;                /* when it started                            */
    uint64_t switch_tsc;                /* when its current task started running      */
    uint64_t idle_tsc;                  /* halted since (0: not halted)               */
    uint64_t idle_cyc;
    uint64_t lock_acq, lock_spin_n, lock_spin_cyc, lock_hold_cyc, lock_since;
    uint64_t irq_n, irq_cyc;            /* device interrupts (handler time)           */
    uint64_t sys_n, sys_cyc, fast_n;
    uint64_t fault_n[KF_KINDS], fault_cyc[KF_KINDS];
    uint64_t io_n, io_bytes, io_cyc;
};

struct tcb;

void     kstat_cpu_online(void);                        /* each CPU, as it starts */
void     kstat_switch(struct tcb *prev, struct tcb *next); /* schedule(), interrupts off */
uint64_t kstat_run_cycles(struct tcb *t);               /* t = the current task */
void     kstat_thread_exit(struct tcb *t);              /* fold into the process */
void     kstat_fault(struct tcb *t, int kind, uint64_t cyc);
void     kstat_io(uint64_t bytes, uint64_t cyc);        /* current task and CPU */
uint64_t kstat_us(uint64_t cyc);                        /* cycles -> microseconds */

size_t   kstat_gen(char *buf, size_t cap);              /* /proc/kstat */
size_t   kstat_gen_process(int tgid, char *buf, size_t cap);   /* /proc/PID/kstat */
void     kstat_init(void);                              /* the REPORT.TXT section */

#endif
