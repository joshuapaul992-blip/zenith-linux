/* linux/sysfs.h -- Kestrel's Linux KPI: attributes
 *
 * Attribute groups are accepted and their show/store callbacks work when
 * called (the KPI self-test does), but they do not appear under /sys yet. */
#ifndef _LKPI_LINUX_SYSFS_H
#define _LKPI_LINUX_SYSFS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/stat.h>

struct kobject;

struct attribute {
    const char         *name;
    umode_t             mode;
};

struct bin_attribute;
struct attribute_group {
    const char          *name;
    umode_t            (*is_visible)(struct kobject *, struct attribute *, int);
    umode_t            (*is_bin_visible)(struct kobject *, const struct bin_attribute *, int);
    struct attribute   **attrs;
    const struct bin_attribute *const *bin_attrs;
};

#define __ATTR(_name, _mode, _show, _store) \
    { .attr = { .name = __stringify(_name), .mode = (_mode) }, .show = _show, .store = _store }
#define __ATTR_RO(_name)        { .attr = { .name = __stringify(_name), .mode = 0444 }, .show = _name##_show }
#define __ATTR_RO_MODE(_name, _mode) { .attr = { .name = __stringify(_name), .mode = (_mode) }, .show = _name##_show }
#define __ATTR_WO(_name)        { .attr = { .name = __stringify(_name), .mode = 0200 }, .store = _name##_store }
#define __ATTR_RW(_name)        __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_RW_MODE(_name, _mode) __ATTR(_name, _mode, _name##_show, _name##_store)
#define __ATTR_NULL             { .attr = { .name = NULL } }
#define ATTRIBUTE_GROUPS(_name) \
    static const struct attribute_group _name##_group = { .attrs = _name##_attrs }; \
    static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL }

/* format into a sysfs buffer (one page) */
__printf(2, 3) int sysfs_emit(char *buf, const char *fmt, ...);
__printf(3, 4) int sysfs_emit_at(char *buf, int at, const char *fmt, ...);
static inline int sysfs_create_group(struct kobject *k, const struct attribute_group *g) { return 0; }
static inline void sysfs_remove_group(struct kobject *k, const struct attribute_group *g) { }
static inline int sysfs_create_groups(struct kobject *k, const struct attribute_group **g) { return 0; }
static inline void sysfs_remove_groups(struct kobject *k, const struct attribute_group **g) { }
static inline int sysfs_create_file(struct kobject *k, const struct attribute *a) { return 0; }
static inline void sysfs_remove_file(struct kobject *k, const struct attribute *a) { }
static inline void sysfs_notify(struct kobject *k, const char *dir, const char *attr) { }
static inline int sysfs_create_link(struct kobject *k, struct kobject *t, const char *name) { return 0; }
static inline void sysfs_remove_link(struct kobject *k, const char *name) { }

#endif
