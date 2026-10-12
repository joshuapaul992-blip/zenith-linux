/* linuxkpi/include/lkpi/glue.h -- what the Linux KPI asks of Kestrel
 *
 * The Linux side (linuxkpi/src, third_party/linux) is compiled against the
 * KPI's own <linux/...> headers and never sees a Kestrel header: the two
 * kernels use the same names for different things (kmalloc, schedule,
 * copy_to_user, panic ...). This file is the only bridge. It is included by
 * both sides, so it uses plain C types only, and every function in it is
 * implemented in linuxkpi/glue/ with Kestrel's own interfaces. */
#ifndef LKPI_GLUE_H
#define LKPI_GLUE_H

/* ---- memory -------------------------------------------------------------- */
void         *lkpi_heap_alloc(unsigned long n);            /* 16-byte aligned, NULL on OOM   */
void          lkpi_heap_free(void *p);
unsigned long lkpi_frames_alloc(unsigned long n, int dma32);    /* zeroed, contiguous; 0 on OOM */
void          lkpi_frames_free(unsigned long phys, unsigned long n);
unsigned long lkpi_ram_total(void);
unsigned long lkpi_ram_free(void);
/* Make [phys, phys+size) usable as MMIO: uncached, or write-combining. */
int           lkpi_map_mmio(unsigned long phys, unsigned long size, int wc);

/* ---- time ---------------------------------------------------------------- */
unsigned long long lkpi_time_ns(void);                     /* monotonic, since boot          */
unsigned long long lkpi_time_ms(void);
void          lkpi_udelay(unsigned int us);                 /* busy-wait                      */
long long     lkpi_realtime_ns(void);                       /* wall clock                     */

/* ---- CPUs and the scheduler ---------------------------------------------- */
int           lkpi_cpu_id(void);                            /* interrupts off                 */
int           lkpi_cpu_count(void);
void         *lkpi_task_self(void);                         /* the running thread (opaque)    */
int           lkpi_task_slot(void *task);                   /* 0 .. lkpi_task_slots()-1, or -1 */
int           lkpi_task_slots(void);
int           lkpi_task_pid(void *task);
const char   *lkpi_task_name(void *task);
int           lkpi_task_is_user(void *task);
/* A kernel thread running fn(arg); NULL if the task table is full. */
void         *lkpi_task_create(const char *name, int (*fn)(void *), void *arg);
void          lkpi_task_exit(int code) __attribute__((noreturn));
/* The sleep protocol, all with interrupts off: mark the current thread
 * asleep (until lkpi_task_wake or deadline_ms of lkpi_time_ms, 0: none),
 * then lkpi_task_switch() gives up the CPU. A wake between the two keeps
 * the thread runnable, so no wakeup is lost. */
void          lkpi_task_block(void *chan, unsigned long long deadline_ms);
void          lkpi_task_switch(void);
void          lkpi_task_wake(void *task);
void          lkpi_task_yield(void);
int           lkpi_task_signal_pending(void);
/* Boot thread only: run other threads until *flag becomes non-zero. */
void          lkpi_boot_wait(volatile int *flag);

/* ---- logging and boot parameters ----------------------------------------- */
void          lkpi_log(const char *s);                      /* one formatted string          */
const char   *lkpi_cmdline(void);
/* Called with the message at panic(), once, before the machine halts;
 * and at power off, reboot or halt (what: 0 off, 1 reboot, 2 halt). */
void          lkpi_set_hooks(void (*panic_fn)(const char *msg), void (*power_fn)(int what));
/* Kestrel's panic, power off (0), reboot (1) and halt (2) */
void          lkpi_die(const char *msg) __attribute__((noreturn));
void          lkpi_power(int what) __attribute__((noreturn));

/* ---- user memory --------------------------------------------------------- */
/* 0 or -EFAULT (Kestrel semantics; the KPI turns them into byte counts). */
int           lkpi_copy_from_user_glue(void *dst, const void *usrc, unsigned long n);
int           lkpi_copy_to_user_glue(void *udst, const void *src, unsigned long n);
int           lkpi_user_range_ok(unsigned long addr, unsigned long n);

/* ---- PCI ----------------------------------------------------------------- */
struct lkpi_pci_info {
    void          *handle;                  /* Kestrel's struct pci_device   */
    unsigned char  bus, dev, fn;
    unsigned short vendor, device, subvendor, subdevice;
    unsigned char  class_code, subclass, prog_if, revision;
    unsigned char  irq_line, irq_pin, header_type;
    struct {
        unsigned long long base, size;
        int            kind;                /* 0 unused, 1 I/O, 2 mem32, 3 mem64, 4 mem16, 5 upper half */
        int            prefetch;
    } bar[6];
    void          *parent;                  /* upstream bridge's handle, or NULL */
};
int           lkpi_pci_count(void);
int           lkpi_pci_get(int index, struct lkpi_pci_info *out);
unsigned int  lkpi_pci_read(void *handle, int off, int size);      /* size 1, 2 or 4 */
void          lkpi_pci_write(void *handle, int off, int size, unsigned int v);
/* Route the device's MSI-X entry 0 or MSI message to fn(arg): the Kestrel
 * vector, or -1. lkpi_pci_msi_disable gives the vector back. */
int           lkpi_pci_msi_enable(void *handle, void (*fn)(void *), void *arg, const char *name);
void          lkpi_pci_msi_disable(void *handle, int vector);
int           lkpi_pci_msi_is_msix(int vector);

/* ---- files and device nodes ---------------------------------------------- */
/* The whole file at `path` in a buffer from lkpi_heap_alloc: its size, or
 * a negative errno. */
long          lkpi_file_read_all(const char *path, void **data, unsigned long max);

/* A character device in /dev. The callbacks get the open's private
 * pointer; read/write buffers are kernel memory, ioctl arguments are
 * user addresses (or values). */
struct lkpi_cdev_ops {
    int  (*open)(void *dev, int flags, void **file);
    void (*release)(void *file);
    long (*read)(void *file, char *buf, unsigned long n, long long *pos);
    long (*write)(void *file, const char *buf, unsigned long n, long long *pos);
    long (*ioctl)(void *file, unsigned int cmd, unsigned long arg);
    int  (*poll)(void *file, int events);
};
void         *lkpi_cdev_add(const char *name, unsigned int mode, unsigned int major, unsigned int minor,
                            const struct lkpi_cdev_ops *ops, void *dev);
void          lkpi_cdev_remove(void *node);

#endif
