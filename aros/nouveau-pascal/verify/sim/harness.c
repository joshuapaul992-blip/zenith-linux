/*
 * Host simulator for the nouveau falcon RTOS message queue as built for AROS.
 *
 * The real msgqueue.c / msgqueue_0148cdec.c / msgqueue_0137c63d.c and the
 * drm-compat completion code are compiled unmodified with -D__AROS__ and
 * linked against:
 *   - a fake SEC2 falcon: register file + DMEM, with a "firmware" thread that
 *     speaks the 0x0148cdec RTOS protocol (init message, command queue with
 *     REWIND markers, ACR_CMD_BOOTSTRAP_FALCON replies on the message queue),
 *   - a fake interrupt -> work queue thread that calls nvkm_msgqueue_recv(),
 *     like nvkm_sec2_intr() -> schedule_work() -> NouveauWorkQueue,
 *   - exec.library stand-ins on pthreads.
 * get_jiffies() aborts, mirroring NOT_IMPLEMENTED_STOP on AROS.
 *
 * Build with -DSIM_ORIGINAL_HEADERS to run against an unpatched tree, which
 * lacks the completion API: it then stops in NOT_IMPLEMENTED_STOP.
 */
#include "msgqueue.h"
#include <engine/falcon.h>
#include <subdev/secboot.h>
#include <core/device.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- exec */

static pthread_mutex_t intr_lock;  /* recursive, initialised in main() */

static void sleep_us(unsigned long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) != 0)
        ;
}

void Disable(void) { pthread_mutex_lock(&intr_lock); }
void Enable(void)  { pthread_mutex_unlock(&intr_lock); }
void Forbid(void)  { pthread_mutex_lock(&intr_lock); }
void Permit(void)  { pthread_mutex_unlock(&intr_lock); }

void InitSemaphore(struct SignalSemaphore *s)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s->m, &a);
}
void ObtainSemaphore(struct SignalSemaphore *s)  { pthread_mutex_lock(&s->m); }
void ReleaseSemaphore(struct SignalSemaphore *s) { pthread_mutex_unlock(&s->m); }
ULONG AttemptSemaphore(struct SignalSemaphore *s) { return pthread_mutex_trylock(&s->m) == 0; }

int bug(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    printf("    [driver] ");
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* --------------------------------------------------------- drm-compat */

APTR HIDDNouveauAlloc(ULONG size) { return calloc(1, size); }
VOID HIDDNouveauFree(APTR memory) { free(memory); }

void udelay(unsigned long usecs) { sleep_us(usecs); }
void Delay(ULONG ticks) { sleep_us(ticks * 20000); }

unsigned long get_jiffies()
{
    fprintf(stderr, "FAIL: get_jiffies() called - it is NOT_IMPLEMENTED_STOP on AROS\n");
    abort();
}

/* copies of drm-compat/drm_compat_funcs.c */
unsigned int jiffies_to_usecs(const unsigned long j)
{
    return j * (1000 / HZ) /* ms */ * 1000;
}
unsigned long usecs_to_jiffies(unsigned int us)
{
    return (us + ((1000 / HZ) * 1000) - 1) / ((1000 / HZ) /* ms */ * 1000);
}

/* ------------------------------------------------------- fake hardware */

#define CMD_HEAD   0xa00
#define CMD_TAIL   0xa04
#define MSG_HEAD   0xa30
#define MSG_TAIL   0xa34
#define CMDQ_OFF   0x100
#define CMDQ_SIZE  0x40     /* small, so the ring wraps and REWIND is used */
#define MSGQ_OFF   0x400
#define MSGQ_SIZE  0x200

static u32 regs[0x1000 / 4];
static u8 dmem[0x1000];
static pthread_mutex_t dmem_lock = PTHREAD_MUTEX_INITIALIZER;

unsigned int ioread32(void *addr) { return __atomic_load_n((u32 *)addr, __ATOMIC_SEQ_CST); }
void iowrite32(u32 val, void *addr) { __atomic_store_n((u32 *)addr, val, __ATOMIC_SEQ_CST); }

static u32 reg_rd(u32 r) { return __atomic_load_n(&regs[r / 4], __ATOMIC_SEQ_CST); }
static void reg_wr(u32 r, u32 v) { __atomic_store_n(&regs[r / 4], v, __ATOMIC_SEQ_CST); }

void nvkm_falcon_load_dmem(struct nvkm_falcon *f, void *data, u32 start, u32 size, u8 port)
{
    pthread_mutex_lock(&dmem_lock);
    memcpy(&dmem[start], data, size);
    pthread_mutex_unlock(&dmem_lock);
}

void nvkm_falcon_read_dmem(struct nvkm_falcon *f, u32 start, u32 size, u8 port, void *data)
{
    pthread_mutex_lock(&dmem_lock);
    memcpy(data, &dmem[start], size);
    pthread_mutex_unlock(&dmem_lock);
}

const char *nvkm_subdev_name[NVKM_SUBDEV_NR] = { [NVKM_ENGINE_SEC2] = "sec2" };
const char *nvkm_secboot_falcon_name[] = {
    [NVKM_SECBOOT_FALCON_PMU] = "PMU", [NVKM_SECBOOT_FALCON_RESERVED] = "<reserved>",
    [NVKM_SECBOOT_FALCON_FECS] = "FECS", [NVKM_SECBOOT_FALCON_GPCCS] = "GPCCS",
    [4] = "<invalid>", [5] = "<invalid>", [6] = "<invalid>",
    [NVKM_SECBOOT_FALCON_SEC2] = "SEC2", [NVKM_SECBOOT_FALCON_END] = "<invalid>",
};

static struct nvkm_device device;
static struct nvkm_subdev sec2_subdev;
static struct nvkm_falcon falcon;
static struct nvkm_msgqueue *queue;

/* ------------------------------------------- interrupt -> work queue */

static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static int work_pending;

static void raise_irq(void)
{
    pthread_mutex_lock(&work_lock);
    work_pending = 1;           /* queue_work() ignores an already scheduled work */
    pthread_cond_signal(&work_cond);
    pthread_mutex_unlock(&work_lock);
}

static void *work_thread(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&work_lock);
        while (!work_pending)
            pthread_cond_wait(&work_cond, &work_lock);
        work_pending = 0;
        pthread_mutex_unlock(&work_lock);
        nvkm_msgqueue_recv(queue);  /* what nvkm_sec2_recv() does */
    }
    return NULL;
}

/* ----------------------------------------------------- fake firmware */

struct init_msg {   /* layout taken from init_callback() in msgqueue_0148cdec.c */
    struct nvkm_msgqueue_msg base;
    u8 num_queues;
    u16 os_debug_entry_point;
    struct { u32 offset; u16 size; u8 index; u8 id; } queue_info[2];
    u16 sw_managed_area_offset;
    u16 sw_managed_area_size;
};
struct acr_cmd {    /* from acr_boot_falcon() */
    struct nvkm_msgqueue_hdr hdr;
    u8 cmd_type;
    u32 flags;
    u32 falcon_id;
};
struct acr_msg {    /* from acr_boot_falcon_callback() */
    struct nvkm_msgqueue_msg base;
    u32 error_code;
    u32 falcon_id;
};

enum { FW_REPLY, FW_NO_REPLY };
static volatile int fw_mode = FW_REPLY;
static volatile int fw_reply_delay_ms;
static volatile int fw_rewinds, fw_booted_count, fw_bad_cmds;
static volatile u32 fw_last_ids[2];

static void fw_send_init(void)
{
    struct init_msg m;
    memset(&m, 0, sizeof(m));
    m.base.hdr.unit_id = 0x01;          /* MSGQUEUE_0148CDEC_UNIT_INIT */
    m.base.hdr.size = sizeof(m);
    m.base.msg_type = 0;                /* INIT_MSG_INIT */
    m.num_queues = 2;
    m.queue_info[0].id = 0; m.queue_info[0].index = 0;
    m.queue_info[0].offset = CMDQ_OFF; m.queue_info[0].size = CMDQ_SIZE;
    m.queue_info[1].id = 1; m.queue_info[1].index = 0;
    m.queue_info[1].offset = MSGQ_OFF; m.queue_info[1].size = MSGQ_SIZE;

    reg_wr(CMD_HEAD, CMDQ_OFF);
    reg_wr(CMD_TAIL, CMDQ_OFF);
    pthread_mutex_lock(&dmem_lock);
    memcpy(&dmem[MSGQ_OFF], &m, sizeof(m));
    pthread_mutex_unlock(&dmem_lock);
    reg_wr(MSG_TAIL, MSGQ_OFF);
    reg_wr(MSG_HEAD, MSGQ_OFF + ALIGN(sizeof(m), 4));
    raise_irq();
}

static void fw_post_msg(void *msg, u32 size)
{
    u32 head = reg_rd(MSG_HEAD);
    if (head + ALIGN(size, 4) > MSGQ_OFF + MSGQ_SIZE)
        head = MSGQ_OFF;                /* driver follows when head < position */
    pthread_mutex_lock(&dmem_lock);
    memcpy(&dmem[head], msg, size);
    pthread_mutex_unlock(&dmem_lock);
    reg_wr(MSG_HEAD, head + ALIGN(size, 4));
    raise_irq();
}

static void *fw_thread(void *arg)
{
    for (;;) {
        u32 tail = reg_rd(CMD_TAIL);
        struct nvkm_msgqueue_hdr hdr;
        struct acr_cmd cmd;

        if (reg_rd(CMD_HEAD) == tail) {
            sleep_us(200);
            continue;
        }
        pthread_mutex_lock(&dmem_lock);
        memcpy(&hdr, &dmem[tail], sizeof(hdr));
        pthread_mutex_unlock(&dmem_lock);

        if (hdr.unit_id == 0x00) {          /* REWIND */
            reg_wr(CMD_TAIL, CMDQ_OFF);
            fw_rewinds++;
            continue;
        }

        pthread_mutex_lock(&dmem_lock);
        memcpy(&cmd, &dmem[tail], sizeof(cmd));
        pthread_mutex_unlock(&dmem_lock);
        reg_wr(CMD_TAIL, tail + ALIGN(hdr.size, 4));

        if (hdr.unit_id != 0x08 || hdr.size != sizeof(cmd) || cmd.cmd_type != 0 ||
            cmd.flags != 0 /* RESET_YES */ ||
            !(cmd.falcon_id == NVKM_SECBOOT_FALCON_FECS || cmd.falcon_id == NVKM_SECBOOT_FALCON_GPCCS)) {
            fw_bad_cmds++;
            continue;
        }
        fw_last_ids[fw_booted_count % 2] = cmd.falcon_id;
        fw_booted_count++;

        if (fw_mode == FW_NO_REPLY)
            continue;
        if (fw_reply_delay_ms)
            sleep_us(fw_reply_delay_ms * 1000);

        struct acr_msg reply;
        memset(&reply, 0, sizeof(reply));
        reply.base.hdr.unit_id = 0x08;
        reply.base.hdr.size = sizeof(reply);
        reply.base.hdr.seq_id = cmd.hdr.seq_id;
        reply.falcon_id = cmd.falcon_id;
        fw_post_msg(&reply, sizeof(reply));
    }
    return NULL;
}

/* --------------------------------------------------------------- tests */

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("  ok   "); else { printf("  FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#ifndef SIM_ORIGINAL_HEADERS
static struct completion xc;
static void *completer(void *arg)
{
    sleep_us(50 * 1000);
    complete(&xc);
    return NULL;
}

static void test_completions(void)
{
    DECLARE_COMPLETION_ONSTACK(c);
    pthread_t t;
    double t0, el;
    unsigned long left;

    printf("completion primitives:\n");
    CHECK(msecs_to_jiffies(1000) == 1000 && msecs_to_jiffies(1) == 1, "msecs_to_jiffies with HZ=1000");
    CHECK(!completion_done(&c), "DECLARE_COMPLETION_ONSTACK starts not done");

    t0 = now_ms();
    left = wait_for_completion_timeout(&c, msecs_to_jiffies(30));
    el = now_ms() - t0;
    CHECK(left == 0 && el >= 30 && el < 200, "timeout returns 0 after ~30ms (%.0f ms)", el);

    complete(&c);
    complete(&c);
    CHECK(wait_for_completion_timeout(&c, 0 + 1) > 0, "complete() x2 releases first waiter");
    CHECK(wait_for_completion_timeout(&c, 1) > 0, "complete() x2 releases second waiter");
    CHECK(wait_for_completion_timeout(&c, 5) == 0, "third waiter is not released");

    complete_all(&c);
    CHECK(try_wait_for_completion(&c) && try_wait_for_completion(&c) && completion_done(&c),
          "complete_all() releases every waiter");
    reinit_completion(&c);
    CHECK(!try_wait_for_completion(&c), "reinit_completion() resets");

    init_completion(&xc);
    pthread_create(&t, NULL, completer, NULL);
    t0 = now_ms();
    left = wait_for_completion_timeout(&xc, msecs_to_jiffies(1000));
    el = now_ms() - t0;
    pthread_join(t, NULL);
    CHECK(left > 800 && left <= 1000 && el >= 45 && el < 300,
          "complete() from another thread wakes waiter (%.0f ms, %lu jiffies left)", el, left);
}

#endif

static int boot(double *el)
{
    double t0 = now_ms();
    int ret = nvkm_msgqueue_acr_boot_falcons(queue,
            BIT(NVKM_SECBOOT_FALCON_FECS) | BIT(NVKM_SECBOOT_FALCON_GPCCS));
    *el = now_ms() - t0;
    return ret;
}

static void *late_init(void *arg)
{
    sleep_us(150 * 1000);
    fw_send_init();
    return NULL;
}

static void test_msgqueue(void)
{
    pthread_t fw, wk, li;
    double el;
    int ret, i, before;
    u8 cmdline[NVKM_MSGQUEUE_CMDLINE_SIZE];

    printf("SEC2 RTOS message queue (0x0148cdec):\n");

    device.pri = (void *)regs;
    sec2_subdev.device = &device;
    sec2_subdev.index = NVKM_ENGINE_SEC2;
    sec2_subdev.debug = NV_DBG_ERROR;
    falcon.owner = &sec2_subdev;
    falcon.name = "SEC2";
    falcon.addr = 0;

    ret = nvkm_msgqueue_new(0x0148cdec, &falcon, NULL, &queue);
    CHECK(ret == 0 && queue, "nvkm_msgqueue_new() returns (was NOT_IMPLEMENTED_STOP in ctor)");

    /* what acr_ls_msgqueue_post_run() does before starting the falcon */
    memset(cmdline, 0, sizeof(cmdline));
    nvkm_msgqueue_write_cmdline(queue, cmdline);
    nvkm_msgqueue_reinit(queue);
    CHECK(1, "nvkm_msgqueue_reinit() returns (was NOT_IMPLEMENTED_STOP)");

    pthread_create(&wk, NULL, work_thread, NULL);
    pthread_create(&fw, NULL, fw_thread, NULL);
    /* RTOS comes up 150ms after the driver starts posting: exercises init_done */
    pthread_create(&li, NULL, late_init, NULL);

    ret = boot(&el);
    pthread_join(li, NULL);
    CHECK(ret == 0 && fw_booted_count == 2 && fw_last_ids[0] == NVKM_SECBOOT_FALCON_FECS &&
          fw_last_ids[1] == NVKM_SECBOOT_FALCON_GPCCS && el >= 140,
          "boot FECS+GPCCS waits for RTOS init, then succeeds (ret=%d, %.0f ms)", ret, el);

    for (i = 0, ret = 0; i < 40 && !ret; i++)
        ret = boot(&el);
    CHECK(ret == 0 && fw_booted_count == 82 && fw_bad_cmds == 0,
          "40 more boots, every command well formed (booted=%d bad=%d)", fw_booted_count, fw_bad_cmds);
    CHECK(fw_rewinds > 0, "command ring wrapped with REWIND (%d rewinds)", fw_rewinds);

    fw_reply_delay_ms = 300;
    ret = boot(&el);
    fw_reply_delay_ms = 0;
    CHECK(ret == 0 && el >= 590 && el < 1500, "slow replies (300ms each) still succeed (%.0f ms)", el);

    fw_mode = FW_NO_REPLY;
    before = fw_booted_count;
    ret = boot(&el);
    CHECK(ret == -ETIMEDOUT && el >= 990 && el < 1500 && fw_booted_count == before + 1,
          "no reply -> -ETIMEDOUT after ~1s, stops at first falcon (ret=%d, %.0f ms)", ret, el);
    fw_mode = FW_REPLY;

    /* re-arm without a new init message: posting must not proceed */
    nvkm_msgqueue_reinit(queue);
    before = fw_booted_count;
    ret = boot(&el);
    CHECK(ret == -ETIMEDOUT && fw_booted_count == before,
          "after reinit, nothing is sent until RTOS re-inits (ret=%d, %.0f ms)", ret, el);

    fw_send_init();
    sleep_us(20 * 1000);
    ret = boot(&el);
    CHECK(ret == 0, "after RTOS re-init, boot works again (ret=%d, %.0f ms)", ret, el);
}

int main(void)
{
    pthread_mutexattr_t a;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&intr_lock, &a);
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef SIM_ORIGINAL_HEADERS
    test_completions();
#endif
    test_msgqueue();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
