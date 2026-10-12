/* linux/firmware.h -- Kestrel's Linux KPI: firmware files (linuxkpi/src/firmware.c)
 *
 * request_firmware(&fw, "vendor/blob.bin", dev) reads /lib/firmware/
 * vendor/blob.bin from the root file system through Kestrel's VFS. */
#ifndef _LKPI_LINUX_FIRMWARE_H
#define _LKPI_LINUX_FIRMWARE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/gfp_types.h>

#define FW_ACTION_NOUEVENT      0
#define FW_ACTION_UEVENT        1

struct device;
struct module;

struct firmware {
    size_t          size;
    const u8       *data;
    void           *priv;
};

int  request_firmware(const struct firmware **fw, const char *name, struct device *device);
int  firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device);
int  request_firmware_direct(const struct firmware **fw, const char *name, struct device *device);
int  firmware_request_platform(const struct firmware **fw, const char *name, struct device *device);
int  request_firmware_into_buf(const struct firmware **fw, const char *name, struct device *device,
                               void *buf, size_t size);
int  request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device,
                             gfp_t gfp, void *context, void (*cont)(const struct firmware *fw, void *context));
void release_firmware(const struct firmware *fw);

#endif
