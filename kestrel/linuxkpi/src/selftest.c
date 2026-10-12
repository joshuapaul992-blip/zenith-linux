/* linuxkpi/src/selftest.c -- the KPI's self-test (boot with linuxkpi.selftest=1)
 *
 * Runs in the lkpi-init thread after the initcalls, through the same Linux
 * API a driver uses. Each group logs "linuxkpi selftest: <group>: pass" or
 * "FAIL" with the first failed check, and a summary line comes last.
 * Timing checks are generous: QEMU without acceleration is slow and the
 * boot thread is busy too. */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/semaphore.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/idr.h>
#include <linux/bitmap.h>
#include <linux/kstrtox.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/rcupdate.h>
#include <linux/err.h>
#include <linux/printk.h>
#include "internal.h"

static int n_pass, n_fail;
static const char *failed_at;           /* the first failed check of the running group */

#define CHECK(cond) do { if (!(cond) && !failed_at) failed_at = #cond; } while (0)

static void run(const char *name, void (*fn)(void))
{
    failed_at = NULL;
    fn();
    if (failed_at) {
        n_fail++;
        printk(KERN_ERR "linuxkpi selftest: %s: FAIL (%s)\n", name, failed_at);
    } else {
        n_pass++;
        printk(KERN_INFO "linuxkpi selftest: %s: pass\n", name);
    }
}

/* ---- workqueues ------------------------------------------------------------ */
static atomic_t work_count;
static int order_log[8], order_n;
static struct completion work_done;

static void count_fn(struct work_struct *w) { atomic_inc(&work_count); }

static void done_fn(struct work_struct *w) { complete(&work_done); }

struct order_item { struct work_struct work; int seq; };
static void ordered_fn(struct work_struct *w)
{
    struct order_item *it = container_of(w, struct order_item, work);
    if (order_n < 8) order_log[order_n++] = it->seq;
    msleep(2);
}

static void test_workqueue(void)
{
    struct work_struct w;
    struct delayed_work dw, never;
    struct order_item items[4];
    int i;

    /* queue and flush on the system queue; a pending work is not queued twice */
    atomic_set(&work_count, 0);
    INIT_WORK(&w, count_fn);
    CHECK(schedule_work(&w));
    flush_work(&w);
    CHECK(atomic_read(&work_count) == 1);

    /* a delayed work runs after its delay, not before */
    init_completion(&work_done);
    INIT_DELAYED_WORK(&dw, done_fn);
    unsigned long t0 = jiffies;
    CHECK(schedule_delayed_work(&dw, msecs_to_jiffies(50)));
    CHECK(!queue_delayed_work(system_wq, &dw, msecs_to_jiffies(50)));
    CHECK(wait_for_completion_timeout(&work_done, msecs_to_jiffies(2000)) > 0);
    CHECK(time_after_eq(jiffies, t0 + msecs_to_jiffies(50)));

    /* cancelling a far delayed work: it never runs */
    atomic_set(&work_count, 0);
    INIT_DELAYED_WORK(&never, count_fn);
    schedule_delayed_work(&never, msecs_to_jiffies(10000));
    CHECK(cancel_delayed_work_sync(&never));
    CHECK(!delayed_work_pending(&never));
    CHECK(atomic_read(&work_count) == 0);

    /* an ordered queue runs its items one at a time, in order */
    struct workqueue_struct *wq = alloc_ordered_workqueue("lkpi-selftest", 0);
    CHECK(wq != NULL);
    if (!wq) return;
    order_n = 0;
    for (i = 0; i < 4; i++) {
        items[i].seq = i;
        INIT_WORK(&items[i].work, ordered_fn);
        queue_work(wq, &items[i].work);
    }
    flush_workqueue(wq);
    CHECK(order_n == 4);
    for (i = 0; i < order_n; i++) CHECK(order_log[i] == i);
    destroy_workqueue(wq);
}

/* ---- timers ---------------------------------------------------------------------- */
struct test_timer {
    struct timer_list   t;
    struct completion   fired;
    unsigned long       at;
    int                 count;
};

static void timer_fn(struct timer_list *t)
{
    struct test_timer *tt = from_timer(tt, t, t);
    tt->at = jiffies;
    tt->count++;
    complete(&tt->fired);
}

static void test_timer(void)
{
    struct test_timer a, b;

    timer_setup(&a.t, timer_fn, 0);
    init_completion(&a.fired);
    a.count = 0;
    unsigned long t0 = jiffies;
    mod_timer(&a.t, t0 + msecs_to_jiffies(30));
    CHECK(timer_pending(&a.t));
    CHECK(wait_for_completion_timeout(&a.fired, msecs_to_jiffies(2000)) > 0);
    CHECK(time_after_eq(a.at, t0 + msecs_to_jiffies(30)));
    CHECK(!timer_pending(&a.t));

    /* re-arming a pending timer moves it; deleting one stops it */
    timer_setup(&b.t, timer_fn, 0);
    init_completion(&b.fired);
    b.count = 0;
    mod_timer(&b.t, jiffies + msecs_to_jiffies(5000));
    mod_timer(&b.t, jiffies + msecs_to_jiffies(20));
    CHECK(wait_for_completion_timeout(&b.fired, msecs_to_jiffies(2000)) > 0);
    mod_timer(&b.t, jiffies + msecs_to_jiffies(30));
    CHECK(timer_delete_sync(&b.t) == 1);
    msleep(60);
    CHECK(b.count == 1);
    timer_shutdown_sync(&a.t);
    timer_shutdown_sync(&b.t);
}

/* ---- completions -------------------------------------------------------------- */
static struct completion c_go;
static atomic_t c_woken;

static int waiter_fn(void *arg)
{
    wait_for_completion(&c_go);
    atomic_inc(&c_woken);
    return 0;
}

static void test_completion(void)
{
    struct task_struct *t[3];
    int i;

    /* a timeout returns 0 and takes about as long as asked */
    init_completion(&c_go);
    unsigned long t0 = jiffies;
    CHECK(wait_for_completion_timeout(&c_go, msecs_to_jiffies(40)) == 0);
    CHECK(time_after_eq(jiffies, t0 + msecs_to_jiffies(40)));
    CHECK(!completion_done(&c_go));

    /* complete() counts: two completes, two waits that do not block */
    complete(&c_go);
    complete(&c_go);
    CHECK(try_wait_for_completion(&c_go));
    CHECK(wait_for_completion_timeout(&c_go, 1) > 0);
    CHECK(!try_wait_for_completion(&c_go));

    /* complete_all() wakes every waiter */
    reinit_completion(&c_go);
    atomic_set(&c_woken, 0);
    for (i = 0; i < 3; i++) t[i] = kthread_run(waiter_fn, NULL, "lkpi-st-wait%d", i);
    msleep(20);
    CHECK(atomic_read(&c_woken) == 0);
    complete_all(&c_go);
    for (i = 0; i < 100 && atomic_read(&c_woken) < 3; i++) msleep(5);
    CHECK(atomic_read(&c_woken) == 3);
    CHECK(completion_done(&c_go));
    (void)t;
}

/* ---- kthreads ---------------------------------------------------------------- */
static atomic_t kt_loops;

static int loop_fn(void *arg)
{
    while (!kthread_should_stop()) {
        if (kthread_should_park()) kthread_parkme();
        atomic_inc(&kt_loops);
        msleep(2);
    }
    return 42;
}

static int never_fn(void *arg) { atomic_inc(&kt_loops); return 7; }

static void test_kthread(void)
{
    /* a running thread: stop returns its result */
    atomic_set(&kt_loops, 0);
    struct task_struct *t = kthread_run(loop_fn, NULL, "lkpi-st-%s", "loop");
    CHECK(!IS_ERR(t));
    if (IS_ERR(t)) return;
    msleep(30);
    CHECK(atomic_read(&kt_loops) > 0);

    /* parked, it makes no progress; unparked, it goes on */
    CHECK(kthread_park(t) == 0);
    int at_park = atomic_read(&kt_loops);
    msleep(30);
    CHECK(atomic_read(&kt_loops) == at_park);
    kthread_unpark(t);
    msleep(30);
    CHECK(atomic_read(&kt_loops) > at_park);
    CHECK(kthread_stop(t) == 42);

    /* stopped before it was ever woken: threadfn never runs, -EINTR */
    atomic_set(&kt_loops, 0);
    t = kthread_create(never_fn, NULL, "lkpi-st-never");
    CHECK(!IS_ERR(t));
    if (IS_ERR(t)) return;
    CHECK(kthread_stop(t) == -EINTR);
    CHECK(atomic_read(&kt_loops) == 0);
}

/* ---- wait queues ---------------------------------------------------------------- */
static DECLARE_WAIT_QUEUE_HEAD(wq_head);
static int wq_cond;
static atomic_t wq_seen;

static int wq_waiter(void *arg)
{
    wait_event(wq_head, READ_ONCE(wq_cond) >= 1);
    atomic_inc(&wq_seen);
    return 0;
}

static void test_waitqueue(void)
{
    int i;

    /* a condition that never comes true times out */
    unsigned long t0 = jiffies;
    CHECK(wait_event_timeout(wq_head, READ_ONCE(wq_cond) == 99, msecs_to_jiffies(30)) == 0);
    CHECK(time_after_eq(jiffies, t0 + msecs_to_jiffies(30)));
    /* one already true returns at once, with time left */
    WRITE_ONCE(wq_cond, 0);
    CHECK(wait_event_timeout(wq_head, READ_ONCE(wq_cond) == 0, msecs_to_jiffies(1000)) > 0);

    /* waiters sleep until the condition holds and wake_up_all() is called */
    atomic_set(&wq_seen, 0);
    for (i = 0; i < 2; i++) kthread_run(wq_waiter, NULL, "lkpi-st-wq%d", i);
    msleep(20);
    wake_up_all(&wq_head);                          /* spurious: condition false */
    msleep(10);
    CHECK(atomic_read(&wq_seen) == 0);
    WRITE_ONCE(wq_cond, 1);
    wake_up_all(&wq_head);
    for (i = 0; i < 100 && atomic_read(&wq_seen) < 2; i++) msleep(5);
    CHECK(atomic_read(&wq_seen) == 2);
}

/* ---- locks ------------------------------------------------------------------------ */
static DEFINE_MUTEX(st_mutex);
static DEFINE_SPINLOCK(st_lock);
static long st_counter;
static struct completion lock_done;

static int lock_worker(void *arg)
{
    int i;
    for (i = 0; i < 200; i++) {
        mutex_lock(&st_mutex);
        long v = st_counter;
        if (!(i % 50)) msleep(1);                   /* sleep holding it */
        st_counter = v + 1;
        mutex_unlock(&st_mutex);
        unsigned long flags;
        spin_lock_irqsave(&st_lock, flags);
        st_counter += 1000;
        spin_unlock_irqrestore(&st_lock, flags);
    }
    complete(&lock_done);
    return 0;
}

static void test_locks(void)
{
    struct rw_semaphore rw;
    struct semaphore sem;
    int i;

    st_counter = 0;
    init_completion(&lock_done);
    for (i = 0; i < 3; i++) kthread_run(lock_worker, NULL, "lkpi-st-lock%d", i);
    for (i = 0; i < 3; i++) CHECK(wait_for_completion_timeout(&lock_done, msecs_to_jiffies(5000)) > 0);
    CHECK(st_counter == 3 * 200 * 1001);
    CHECK(!mutex_is_locked(&st_mutex));
    CHECK(mutex_trylock(&st_mutex));
    CHECK(!mutex_trylock(&st_mutex));
    mutex_unlock(&st_mutex);

    init_rwsem(&rw);
    down_read(&rw);
    CHECK(down_read_trylock(&rw));
    CHECK(!down_write_trylock(&rw));
    up_read(&rw);
    up_read(&rw);
    CHECK(down_write_trylock(&rw));
    CHECK(!down_read_trylock(&rw));
    downgrade_write(&rw);
    CHECK(down_read_trylock(&rw));
    up_read(&rw);
    up_read(&rw);
    CHECK(!rwsem_is_locked(&rw));

    sema_init(&sem, 2);
    CHECK(down_trylock(&sem) == 0);
    CHECK(down_trylock(&sem) == 0);
    CHECK(down_trylock(&sem) != 0);
    CHECK(down_timeout(&sem, msecs_to_jiffies(20)) == -ETIME);
    up(&sem);
    CHECK(down_timeout(&sem, msecs_to_jiffies(20)) == 0);
}

/* ---- time ------------------------------------------------------------------------ */
static void test_time(void)
{
    ktime_t k0 = ktime_get();
    unsigned long j0 = jiffies;
    msleep(50);
    s64 ms = ktime_ms_delta(ktime_get(), k0);
    CHECK(ms >= 50 && ms < 1000);
    CHECK(time_after_eq(jiffies, j0 + msecs_to_jiffies(50)));
    k0 = ktime_get();
    udelay(500);
    CHECK(ktime_us_delta(ktime_get(), k0) >= 500);
    k0 = ktime_get();
    usleep_range(2000, 3000);
    CHECK(ktime_us_delta(ktime_get(), k0) >= 2000);
    CHECK(jiffies_to_msecs(msecs_to_jiffies(1234)) == 1234);
}

/* ---- memory ---------------------------------------------------------------------- */
struct st_obj { int a; char pad[52]; };
static int ctor_calls;
static void st_ctor(void *p) { ctor_calls++; ((struct st_obj *)p)->a = 0x5a; }

static void test_memory(void)
{
    int i;
    void *p[16];

    for (i = 0; i < 16; i++) {
        size_t sz = (size_t)8 << i;                 /* 8 B .. 256 KiB */
        p[i] = kmalloc(sz, GFP_KERNEL);
        CHECK(p[i] != NULL);
        if (!p[i]) continue;
        CHECK(((unsigned long)p[i] & (min_t(size_t, sz, PAGE_SIZE) - 1)) == 0);
        CHECK(ksize(p[i]) >= sz);
        memset(p[i], 0xa5, sz);
    }
    for (i = 0; i < 16; i++) kfree(p[i]);

    u8 *z = kzalloc(300, GFP_KERNEL);
    CHECK(z && !z[0] && !z[299]);
    z = krealloc(z, 5000, GFP_KERNEL);
    CHECK(z && !z[299]);
    kfree(z);
    int *arr = kcalloc(100, sizeof(int), GFP_KERNEL);
    CHECK(arr && arr[99] == 0);
    kfree(arr);
    CHECK(kmalloc_array(SIZE_MAX / 2, 4, GFP_KERNEL) == NULL);

    struct kmem_cache *kc = kmem_cache_create("lkpi_st", sizeof(struct st_obj), 0, 0, st_ctor);
    CHECK(kc != NULL);
    if (kc) {
        struct st_obj *o = kmem_cache_alloc(kc, GFP_KERNEL);
        CHECK(o && o->a == 0x5a && ctor_calls >= 1);
        kmem_cache_free(kc, o);
        kmem_cache_destroy(kc);
    }

    void *v = vzalloc(3 * PAGE_SIZE + 5);
    CHECK(v && is_vmalloc_addr(v));
    vfree(v);

    struct page *pg = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
    CHECK(pg != NULL);
    if (pg) {
        u8 *a = page_address(pg);
        CHECK(a && !((unsigned long)a & (PAGE_SIZE - 1)) && !a[4 * PAGE_SIZE - 1]);
        CHECK(virt_to_page(a + PAGE_SIZE) == pg + 1);
        __free_pages(pg, 2);
    }
    unsigned long gp = get_zeroed_page(GFP_KERNEL);
    CHECK(gp && !*(u8 *)gp);
    free_page(gp);
}

/* ---- DMA through a platform device -------------------------------------------- */
static void test_dma(void)
{
    struct platform_device *pd = platform_device_register_simple("lkpi-selftest", -1, NULL, 0);
    CHECK(!IS_ERR(pd));
    if (IS_ERR(pd)) return;
    struct device *dev = &pd->dev;
    CHECK(dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32)) == 0);
    dma_addr_t h;
    void *c = dma_alloc_coherent(dev, 8192, &h, GFP_KERNEL);
    CHECK(c != NULL && h != 0 && h + 8192 <= (1ULL << 32));
    CHECK(c && !((u8 *)c)[8191]);
    if (c) dma_free_coherent(dev, 8192, c, h);

    void *buf = kmalloc(512, GFP_KERNEL);
    dma_addr_t m = dma_map_single(dev, buf, 512, DMA_TO_DEVICE);
    CHECK(!dma_mapping_error(dev, m));
    dma_unmap_single(dev, m, 512, DMA_TO_DEVICE);
    kfree(buf);

    void *d = devm_kzalloc(dev, 128, GFP_KERNEL);
    CHECK(d != NULL);
    platform_device_unregister(pd);                 /* frees d */
}

/* ---- library: atomics, kref, idr, bitmaps, strings ------------------------------- */
static int kref_freed;
static void st_release(struct kref *k) { kref_freed++; }

static void test_lib(void)
{
    char buf[64];
    int v;
    unsigned long u;

    atomic_t a = ATOMIC_INIT(5);
    CHECK(atomic_inc_return(&a) == 6);
    CHECK(atomic_cmpxchg(&a, 6, 9) == 6 && atomic_read(&a) == 9);
    atomic_t one = ATOMIC_INIT(1);
    CHECK(atomic_dec_and_test(&one));

    struct kref k;
    kref_init(&k);
    kref_get(&k);
    CHECK(!kref_put(&k, st_release));
    CHECK(kref_put(&k, st_release) && kref_freed == 1);

    DEFINE_IDR(idr);
    int id1 = idr_alloc(&idr, &v, 1, 0, GFP_KERNEL);
    int id2 = idr_alloc(&idr, &u, 1, 0, GFP_KERNEL);
    CHECK(id1 == 1 && id2 == 2);
    CHECK(idr_find(&idr, 2) == &u);
    CHECK(idr_remove(&idr, 1) == &v);
    CHECK(idr_alloc(&idr, &v, 1, 0, GFP_KERNEL) == 1);
    idr_destroy(&idr);
    DEFINE_IDA(ida);
    CHECK(ida_alloc(&ida, GFP_KERNEL) == 0 && ida_alloc(&ida, GFP_KERNEL) == 1);
    ida_free(&ida, 0);
    CHECK(ida_alloc(&ida, GFP_KERNEL) == 0);
    ida_destroy(&ida);

    unsigned long *bm = bitmap_zalloc(130, GFP_KERNEL);
    CHECK(bm != NULL);
    if (bm) {
        set_bit(3, bm);
        set_bit(129, bm);
        CHECK(find_first_bit(bm, 130) == 3);
        CHECK(find_next_bit(bm, 130, 4) == 129);
        CHECK(bitmap_weight(bm, 130) == 2);
        CHECK(find_first_zero_bit(bm, 130) == 0);
        bitmap_free(bm);
    }

    CHECK(kstrtoint("-42", 10, &v) == 0 && v == -42);
    CHECK(kstrtoul("0x1f", 0, &u) == 0 && u == 31);
    CHECK(kstrtoint("12x", 10, &v) == -EINVAL);
    snprintf(buf, sizeof buf, "%pe %d %05x %-3s|", ERR_PTR(-ENOMEM), -7, 0xab, "a");
    CHECK(!strcmp(buf, "-ENOMEM -7 000ab a  |"));
    u8 mac[6] = { 0x52, 0x54, 0, 0x12, 0x34, 0x56 };
    snprintf(buf, sizeof buf, "%pM", mac);
    CHECK(!strcmp(buf, "52:54:00:12:34:56"));
    CHECK(strscpy(buf, "too long for it", 8) == -E2BIG && !strcmp(buf, "too lon"));
    char *s = kasprintf(GFP_KERNEL, "%s-%u", "dev", 7u);
    CHECK(s && !strcmp(s, "dev-7"));
    kfree(s);
}

/* ---- tasklets and RCU ---------------------------------------------------------- */
static struct completion tl_done;
static void tl_fn(struct tasklet_struct *t) { complete(&tl_done); }

struct rcu_obj { int v; struct rcu_head rcu; };
static struct completion rcu_done;
static void rcu_cb(struct rcu_head *h) { complete(&rcu_done); }

static void test_deferred(void)
{
    struct tasklet_struct tl;
    init_completion(&tl_done);
    tasklet_setup(&tl, tl_fn);
    tasklet_schedule(&tl);
    CHECK(wait_for_completion_timeout(&tl_done, msecs_to_jiffies(2000)) > 0);
    tasklet_kill(&tl);

    struct rcu_obj *o = kzalloc(sizeof *o, GFP_KERNEL);
    rcu_read_lock();
    CHECK(o != NULL);
    rcu_read_unlock();
    synchronize_rcu();
    init_completion(&rcu_done);
    if (o) call_rcu(&o->rcu, rcu_cb);
    CHECK(wait_for_completion_timeout(&rcu_done, msecs_to_jiffies(2000)) > 0);
    rcu_barrier();
    kfree(o);
    o = kzalloc(sizeof *o, GFP_KERNEL);
    if (o) kfree_rcu(o, rcu);
}

/* ---- firmware through the VFS ------------------------------------------------ */
static void test_firmware(void)
{
    const struct firmware *fw;
    int r = request_firmware(&fw, "linuxkpi/selftest.bin", NULL);
    CHECK(r == 0);
    if (!r) {
        CHECK(fw->size >= 8 && !memcmp(fw->data, "LINUXKPI", 8));
        release_firmware(fw);
    }
    CHECK(firmware_request_nowarn(&fw, "linuxkpi/missing.bin", NULL) == -ENOENT && fw == NULL);
}

void lkpi_selftest_run(void)
{
    printk(KERN_INFO "linuxkpi selftest: starting\n");
    unsigned long t0 = jiffies;
    n_pass = n_fail = 0;
    run("workqueue", test_workqueue);
    run("timer", test_timer);
    run("completion", test_completion);
    run("kthread", test_kthread);
    run("waitqueue", test_waitqueue);
    run("locks", test_locks);
    run("time", test_time);
    run("memory", test_memory);
    run("dma", test_dma);
    run("lib", test_lib);
    run("tasklet+rcu", test_deferred);
    run("firmware", test_firmware);
    printk(KERN_INFO "linuxkpi selftest: %d passed, %d failed (%u ms)%s\n", n_pass, n_fail,
           jiffies_to_msecs(jiffies - t0), n_fail ? "" : " -- all passed");
}
