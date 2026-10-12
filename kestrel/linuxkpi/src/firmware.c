/* linuxkpi/src/firmware.c -- request_firmware from /lib/firmware
 *
 * The file is read whole through Kestrel's VFS from the root file system,
 * so a driver must not ask for firmware before the root is mounted (the
 * KPI's initcalls run after it is). There is no user-space fallback and
 * no built-in firmware. */
#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/workqueue.h>
#include <linux/printk.h>
#include "internal.h"

#define FW_PATH         "/lib/firmware/"
#define FW_MAX_SIZE     (256UL << 20)

static int load(const struct firmware **out, const char *name, struct device *dev, bool warn)
{
    char path[256];
    void *data = NULL;

    *out = NULL;
    if (!name || !*name || strstr(name, "..")) return -EINVAL;
    if (snprintf(path, sizeof path, FW_PATH "%s", name) >= (int)sizeof path) return -ENAMETOOLONG;

    struct firmware *fw = kzalloc(sizeof *fw, GFP_KERNEL);
    if (!fw) return -ENOMEM;
    long n = lkpi_file_read_all(path, &data, FW_MAX_SIZE);
    if (n < 0) {
        kfree(fw);
        if (warn) {
            if (dev) dev_warn(dev, "Direct firmware load for %s failed with error %ld\n", name, n);
            else printk(KERN_WARNING "Direct firmware load for %s failed with error %ld\n", name, n);
        }
        return (int)n;
    }
    fw->data = data;
    fw->size = (size_t)n;
    fw->priv = data;                                    /* from Kestrel's heap */
    *out = fw;
    if (dev) dev_dbg(dev, "loaded firmware %s (%zu bytes)\n", name, fw->size);
    return 0;
}

int request_firmware(const struct firmware **fw, const char *name, struct device *device)
{ return load(fw, name, device, true); }
int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device)
{ return load(fw, name, device, false); }
int request_firmware_direct(const struct firmware **fw, const char *name, struct device *device)
{ return load(fw, name, device, false); }
int firmware_request_platform(const struct firmware **fw, const char *name, struct device *device)
{ return load(fw, name, device, true); }

int request_firmware_into_buf(const struct firmware **out, const char *name, struct device *device,
                              void *buf, size_t size)
{
    const struct firmware *tmp;
    int r = load(&tmp, name, device, true);
    if (r) return r;
    if (tmp->size > size) {
        release_firmware(tmp);
        *out = NULL;
        return -EFBIG;
    }
    struct firmware *fw = kzalloc(sizeof *fw, GFP_KERNEL);
    if (!fw) { release_firmware(tmp); return -ENOMEM; }
    memcpy(buf, tmp->data, tmp->size);
    fw->data = buf;
    fw->size = tmp->size;
    release_firmware(tmp);
    *out = fw;
    return 0;
}

void release_firmware(const struct firmware *fw)
{
    if (!fw) return;
    if (fw->priv) lkpi_heap_free(fw->priv);
    kfree(fw);
}

struct fw_async {
    struct work_struct  work;
    char               *name;
    struct device      *dev;
    void               *context;
    bool                uevent;
    void              (*cont)(const struct firmware *fw, void *context);
};

static void fw_async_work(struct work_struct *w)
{
    struct fw_async *a = container_of(w, struct fw_async, work);
    const struct firmware *fw;
    load(&fw, a->name, a->dev, a->uevent);
    a->cont(fw, a->context);
    put_device(a->dev);
    kfree(a->name);
    kfree(a);
}

int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device,
                            gfp_t gfp, void *context, void (*cont)(const struct firmware *fw, void *context))
{
    struct fw_async *a = kzalloc(sizeof *a, gfp);
    if (!a) return -ENOMEM;
    a->name = kstrdup(name, gfp);
    if (!a->name) { kfree(a); return -ENOMEM; }
    a->dev = get_device(device);
    a->context = context;
    a->uevent = uevent;
    a->cont = cont;
    INIT_WORK(&a->work, fw_async_work);
    schedule_work(&a->work);
    return 0;
}
