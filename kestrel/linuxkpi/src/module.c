/* linuxkpi/src/module.c -- bring-up: the KPI's threads, PCI, parameters, initcalls
 *
 * linuxkpi_init() is called once by Kestrel's boot thread, after PCI, MSI,
 * the scheduler and the root file system are up. The boot thread cannot
 * sleep, so the work is done by an "lkpi-init" kernel thread while the boot
 * thread waits for it:
 *
 *   1. the timer, workqueue and tasklet threads start;
 *   2. the platform and PCI buses are built (every device Kestrel found);
 *   3. module parameters ("module.param=value") are read from the command line;
 *   4. the initcalls run by level, in link order within a level: built-in
 *      Linux drivers register here and probe their devices;
 *   5. drivers that deferred their probe are retried;
 *   6. with linuxkpi.selftest=1, the self-test runs (selftest.c).
 *
 * Nothing is ever unloaded: module_exit functions are compiled but not run. */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/kstrtox.h>
#include <linux/printk.h>
#include <linux/err.h>
#include "internal.h"

/* linker.ld */
extern const struct lkpi_initcall __lkpi_initcall_start[], __lkpi_initcall_end[];
extern const struct lkpi_param __lkpi_param_start[], __lkpi_param_end[];
extern const char __lkpi_modinfo_start[], __lkpi_modinfo_end[];

/* ---- the command line --------------------------------------------------------- */
/* '-' and '_' are the same in parameter names, as in Linux */
static bool name_eq(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        char x = a[i] == '-' ? '_' : a[i], y = b[i] == '-' ? '_' : b[i];
        if (!y || x != y) return false;
    }
    return b[i] == '\0';
}

/* Walk "key[=value]" words (a value may be in double quotes): fn(key, klen,
 * value or NULL, vlen, arg) for each, until fn returns true. */
static bool cmdline_walk(bool (*fn)(const char *k, size_t kl, const char *v, size_t vl, void *arg), void *arg)
{
    const char *p = lkpi_cmdline();
    if (!p) return false;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *k = p, *v = NULL;
        size_t kl, vl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '=') p++;
        kl = (size_t)(p - k);
        if (*p == '=') {
            p++;
            if (*p == '"') {
                v = ++p;
                while (*p && *p != '"') p++;
                vl = (size_t)(p - v);
                if (*p) p++;
            } else {
                v = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                vl = (size_t)(p - v);
            }
        }
        if (fn(k, kl, v, vl, arg)) return true;
    }
    return false;
}

struct cmdline_query {
    const char *key;
    char       *buf;
    size_t      n;
};

static bool query_one(const char *k, size_t kl, const char *v, size_t vl, void *arg)
{
    struct cmdline_query *q = arg;
    if (!name_eq(k, kl, q->key)) return false;
    if (q->n) {
        size_t c = vl < q->n - 1 ? vl : q->n - 1;
        if (v) memcpy(q->buf, v, c);
        else c = 0;
        q->buf[c] = '\0';
    }
    return true;
}

bool lkpi_cmdline_get(const char *key, char *buf, size_t n)
{
    struct cmdline_query q = { key, buf, n };
    return cmdline_walk(query_one, &q);
}

bool lkpi_cmdline_flag(const char *key)
{
    char v[8];
    bool on;
    if (!lkpi_cmdline_get(key, v, sizeof v)) return false;
    if (!v[0]) return true;                             /* a bare key */
    return !kstrtobool(v, &on) && on;
}

static int debug_flag = -1;

bool lkpi_debug_enabled(void)
{
    if (debug_flag < 0) debug_flag = lkpi_cmdline_flag("linuxkpi.debug");
    return debug_flag > 0;
}

/* ---- module parameters ------------------------------------------------------- */
static int param_set(const struct lkpi_param *p, const char *val)
{
    const char *t = p->type;
    bool b;
    int r = 0;

    if (!strcmp(t, "bool") || !strcmp(t, "invbool")) {
        if (!val || !*val) b = true;
        else if ((r = kstrtobool(val, &b))) return r;
        *(bool *)p->arg = !strcmp(t, "bool") ? b : !b;
        return 0;
    }
    if (!val) return -EINVAL;
    if (!strcmp(t, "int"))          r = kstrtoint(val, 0, p->arg);
    else if (!strcmp(t, "uint"))    r = kstrtouint(val, 0, p->arg);
    else if (!strcmp(t, "hexint"))  r = kstrtouint(val, 16, p->arg);
    else if (!strcmp(t, "long"))    r = kstrtol(val, 0, p->arg);
    else if (!strcmp(t, "ulong"))   r = kstrtoul(val, 0, p->arg);
    else if (!strcmp(t, "short"))   r = kstrtos16(val, 0, p->arg);
    else if (!strcmp(t, "ushort"))  r = kstrtou16(val, 0, p->arg);
    else if (!strcmp(t, "byte"))    r = kstrtou8(val, 0, p->arg);
    else if (!strcmp(t, "charp")) {
        char *s = kstrdup(val, GFP_KERNEL);
        if (!s) return -ENOMEM;
        *(char **)p->arg = s;                           /* the default is not freed */
    } else if (!strcmp(t, "string")) {
        if (strlen(val) >= p->len) return -ENOSPC;
        strscpy(p->arg, val, p->len);
    } else return -EOPNOTSUPP;                          /* arrays: not yet */
    return r;
}

static bool param_one(const char *k, size_t kl, const char *v, size_t vl, void *arg)
{
    const struct lkpi_param *p;
    if (!memchr(k, '.', kl)) return false;              /* not a module parameter */
    for (p = __lkpi_param_start; p < __lkpi_param_end; p++) {
        if (!name_eq(k, kl, p->name)) continue;
        char *val = v ? kstrndup(v, vl, GFP_KERNEL) : NULL;
        int r = param_set(p, val);
        if (r) printk(KERN_WARNING "linuxkpi: parameter %s=%s: %pe\n", p->name, val ? val : "", ERR_PTR(r));
        else printk(KERN_INFO "linuxkpi: parameter %s=%s\n", p->name, val ? val : "1");
        kfree(val);
    }
    return false;
}

/* ---- module information ----------------------------------------------------------- */
/* The value of "modname.tag=" in .lkpi_modinfo (the first one), or NULL. The
 * strings are packed back to back, NUL-terminated. */
static const char *modinfo_get(const char *modname, const char *tag)
{
    size_t ml = strlen(modname), tl = strlen(tag);
    const char *s = __lkpi_modinfo_start;
    while (s < __lkpi_modinfo_end) {
        size_t l = strlen(s);
        if (l > ml + tl + 1 && !strncmp(s, modname, ml) && s[ml] == '.' &&
            !strncmp(s + ml + 1, tag, tl) && s[ml + 1 + tl] == '=')
            return s + ml + tl + 2;
        s += l + 1;
        while (s < __lkpi_modinfo_end && !*s) s++;      /* alignment padding */
    }
    return NULL;
}

/* Log a module once, the first time one of its initcalls runs */
static void module_announce(const struct lkpi_initcall *ic)
{
    const struct lkpi_initcall *prev;
    for (prev = __lkpi_initcall_start; prev < ic; prev++)
        if (!strcmp(prev->modname, ic->modname)) return;
    const char *desc = modinfo_get(ic->modname, "description");
    const char *lic = modinfo_get(ic->modname, "license");
    if (!desc && !lic) return;                          /* the KPI's own */
    printk(KERN_INFO "linuxkpi: module %s: %s [license: %s]\n", ic->modname,
           desc ? desc : "(no description)", lic ? lic : "none");
}

static void run_initcalls(void)
{
    const struct lkpi_initcall *ic;
    for (ic = __lkpi_initcall_start; ic < __lkpi_initcall_end; ic++) {
        module_announce(ic);
        if (lkpi_debug_enabled())
            printk(KERN_DEBUG "linuxkpi: calling %s (%s)\n", ic->name, ic->modname);
        int r = ic->fn();
        if (r == -ENODEV)                               /* no hardware: normal for a driver */
            printk(KERN_INFO "linuxkpi: %s: %s: no device\n", ic->modname, ic->name);
        else if (r)
            printk(KERN_WARNING "linuxkpi: %s: initcall %s failed: %pe\n", ic->modname, ic->name, ERR_PTR(r));
    }
}

/* ---- bring-up ----------------------------------------------------------------- */
static volatile int init_done;

static int lkpi_init_thread(void *unused)
{
    lkpi_timer_core_init();
    lkpi_workqueue_core_init();
    lkpi_tasklet_core_init();
    lkpi_device_core_init();
    lkpi_pci_core_init();

    cmdline_walk(param_one, NULL);
    run_initcalls();
    lkpi_deferred_probe_flush();
    printk(KERN_INFO "linuxkpi: %d initcalls done\n", (int)(__lkpi_initcall_end - __lkpi_initcall_start));

    if (lkpi_cmdline_flag("linuxkpi.selftest")) lkpi_selftest_run();

    __atomic_store_n(&init_done, 1, __ATOMIC_RELEASE);
    return 0;
}

void linuxkpi_init(void);
void linuxkpi_init(void)
{
    lkpi_sched_init();
    lkpi_set_hooks(lkpi_notifier_panic, lkpi_notifier_power);
    struct task_struct *t = kthread_run(lkpi_init_thread, NULL, "lkpi-init");
    if (IS_ERR(t)) {
        printk(KERN_ERR "linuxkpi: cannot start: %pe\n", t);
        return;
    }
    lkpi_boot_wait(&init_done);
}
