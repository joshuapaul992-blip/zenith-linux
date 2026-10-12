/* linuxkpi/src/pci.c -- PCI devices and drivers on top of Kestrel's enumeration
 *
 * At start-up every function Kestrel's PCI scan found becomes a struct
 * pci_dev named "0000:BB:DD.F" on the "pci" bus, with its BARs as
 * resources and its capabilities located. pci_register_driver() matches
 * the id table against them (bus.c/device.c does the binding).
 *
 * Configuration space goes through Kestrel (legacy mechanism #1, 256
 * bytes). BARs are already assigned by the firmware; mapping one makes it
 * uncached in Kestrel's identity map. Region requests only record
 * ownership per device. The pcim_ (managed) calls undo themselves when
 * the driver is unbound, through devres. */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/string.h>
#include "internal.h"

static LIST_HEAD(pci_buses);

/* every device sits on its bus's list; this list links them all, in scan order */
struct pci_node { struct list_head node; struct pci_dev *pdev; };
static LIST_HEAD(all_devs);

/* ---- configuration space ---------------------------------------------------- */
static int cfg_check(const struct pci_dev *dev, int where, int size)
{
    if (!dev || !dev->lkpi_handle) return PCIBIOS_DEVICE_NOT_FOUND;
    if (where < 0 || where + size > 256 || (where & (size - 1))) return PCIBIOS_BAD_REGISTER_NUMBER;
    return PCIBIOS_SUCCESSFUL;
}

#define CFG_READ(name, type, size) \
    int pci_read_config_##name(const struct pci_dev *dev, int where, type *val) \
    { \
        int r = cfg_check(dev, where, size); \
        if (r) { *val = (type)~0u; return r; } \
        *val = (type)lkpi_pci_read(dev->lkpi_handle, where, size); \
        return PCIBIOS_SUCCESSFUL; \
    }
#define CFG_WRITE(name, type, size) \
    int pci_write_config_##name(const struct pci_dev *dev, int where, type val) \
    { \
        int r = cfg_check(dev, where, size); \
        if (r) return r; \
        lkpi_pci_write(dev->lkpi_handle, where, size, val); \
        return PCIBIOS_SUCCESSFUL; \
    }
CFG_READ(byte, u8, 1)
CFG_READ(word, u16, 2)
CFG_READ(dword, u32, 4)
CFG_WRITE(byte, u8, 1)
CFG_WRITE(word, u16, 2)
CFG_WRITE(dword, u32, 4)

static struct pci_dev *find_slot(unsigned int busnr, unsigned int devfn)
{
    struct pci_node *n;
    list_for_each_entry(n, &all_devs, node)
        if (n->pdev->bus->number == busnr && n->pdev->devfn == devfn) return n->pdev;
    return NULL;
}

#define BUS_CFG_READ(name, type) \
    int pci_bus_read_config_##name(struct pci_bus *bus, unsigned int devfn, int where, type *val) \
    { \
        struct pci_dev *d = find_slot(bus->number, devfn); \
        if (!d) { *val = (type)~0u; return PCIBIOS_DEVICE_NOT_FOUND; } \
        return pci_read_config_##name(d, where, val); \
    }
#define BUS_CFG_WRITE(name, type) \
    int pci_bus_write_config_##name(struct pci_bus *bus, unsigned int devfn, int where, type val) \
    { \
        struct pci_dev *d = find_slot(bus->number, devfn); \
        if (!d) return PCIBIOS_DEVICE_NOT_FOUND; \
        return pci_write_config_##name(d, where, val); \
    }
BUS_CFG_READ(byte, u8)
BUS_CFG_READ(word, u16)
BUS_CFG_READ(dword, u32)
BUS_CFG_WRITE(byte, u8)
BUS_CFG_WRITE(word, u16)
BUS_CFG_WRITE(dword, u32)

void pci_clear_and_set_config_dword(const struct pci_dev *dev, int pos, u32 clear, u32 set)
{
    u32 v;
    pci_read_config_dword(dev, pos, &v);
    pci_write_config_dword(dev, pos, (v & ~clear) | set);
}

static void update_command(struct pci_dev *dev, u16 clear, u16 set)
{
    u16 cmd;
    pci_read_config_word(dev, PCI_COMMAND, &cmd);
    u16 n = (cmd & ~clear) | set;
    if (n != cmd) pci_write_config_word(dev, PCI_COMMAND, n);
}

/* ---- capabilities ------------------------------------------------------------- */
u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
    u8 next;
    int ttl = 48;
    pci_read_config_byte(dev, pos, &next);
    while (ttl-- && next >= 0x40) {
        u8 id;
        next &= ~3;
        pci_read_config_byte(dev, next, &id);
        if (id == 0xff) break;
        if (id == cap) return next;
        pci_read_config_byte(dev, next + 1, &next);
    }
    return 0;
}

u8 pci_find_capability(struct pci_dev *dev, int cap)
{
    u16 status;
    pci_read_config_word(dev, PCI_STATUS, &status);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;
    u8 pos = (dev->hdr_type & 0x7f) == PCI_HEADER_TYPE_CARDBUS ? 0x14 : PCI_CAPABILITY_LIST;
    return pci_find_next_capability(dev, pos, cap);
}

u16 pci_find_vsec_capability(struct pci_dev *dev, u16 vendor, int cap) { return 0; }   /* extended space */

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
    *val = 0;
    if (!dev->pcie_cap || pos + 2 > 0x3c) return dev->pcie_cap ? PCIBIOS_BAD_REGISTER_NUMBER : 0;
    return pci_read_config_word(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
    *val = 0;
    if (!dev->pcie_cap || pos + 4 > 0x3c) return dev->pcie_cap ? PCIBIOS_BAD_REGISTER_NUMBER : 0;
    return pci_read_config_dword(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
    if (!dev->pcie_cap) return 0;
    return pci_write_config_word(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
    if (!dev->pcie_cap) return 0;
    return pci_write_config_dword(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
    u16 v;
    int r = pcie_capability_read_word(dev, pos, &v);
    return r ? r : pcie_capability_write_word(dev, pos, (v & ~clear) | set);
}

int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set)
{
    u32 v;
    int r = pcie_capability_read_dword(dev, pos, &v);
    return r ? r : pcie_capability_write_dword(dev, pos, (v & ~clear) | set);
}

/* ---- enabling ------------------------------------------------------------------- */
static int do_enable(struct pci_dev *dev, unsigned long types)
{
    if (atomic_inc_return(&dev->enable_cnt) > 1) return 0;
    u16 set = 0, old;
    for (int i = 0; i < PCI_NUM_RESOURCES; i++) {
        unsigned long f = dev->resource[i].flags & types;
        if (!dev->resource[i].end) continue;
        if (f & IORESOURCE_IO) set |= PCI_COMMAND_IO;
        if (f & IORESOURCE_MEM) set |= PCI_COMMAND_MEMORY;
    }
    pci_read_config_word(dev, PCI_COMMAND, &old);
    if ((old | set) != old) {
        dev_info(&dev->dev, "enabling device (%04x -> %04x)\n", old, old | set);
        update_command(dev, 0, set);
    }
    lkpi_pci_irq_setup(dev);
    return 0;
}

int pci_enable_device(struct pci_dev *dev) { return do_enable(dev, IORESOURCE_IO | IORESOURCE_MEM); }
int pci_enable_device_mem(struct pci_dev *dev) { return do_enable(dev, IORESOURCE_MEM); }
int pci_enable_device_io(struct pci_dev *dev) { return do_enable(dev, IORESOURCE_IO); }

void pci_disable_device(struct pci_dev *dev)
{
    if (atomic_dec_return(&dev->enable_cnt) != 0) return;
    pci_clear_master(dev);
}

void pci_set_master(struct pci_dev *dev)
{
    update_command(dev, 0, PCI_COMMAND_MASTER);
    dev->is_busmaster = 1;
}

void pci_clear_master(struct pci_dev *dev)
{
    update_command(dev, PCI_COMMAND_MASTER, 0);
    dev->is_busmaster = 0;
}

void pci_intx(struct pci_dev *dev, int enable)
{
    if (enable) update_command(dev, PCI_COMMAND_INTX_DISABLE, 0);
    else update_command(dev, 0, PCI_COMMAND_INTX_DISABLE);
}

int pci_save_state(struct pci_dev *dev)
{
    for (int i = 0; i < 16; i++) pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]);
    dev->state_saved = true;
    return 0;
}

void pci_restore_state(struct pci_dev *dev)
{
    if (!dev->state_saved) return;
    for (int i = 15; i >= 0; i--) {                     /* command register last */
        u32 v;
        pci_read_config_dword(dev, i * 4, &v);
        if (v != dev->saved_config_space[i]) pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]);
    }
    dev->state_saved = false;
}

/* the managed side of pcim_enable_device(): all of it undone at unbind */
static void pcim_release(void *arg)
{
    struct pci_dev *pdev = arg;
    lkpi_pci_irq_release(pdev);
    pdev->lkpi_regions = 0;
    memset(pdev->lkpi_iomap, 0, sizeof pdev->lkpi_iomap);
    if (pci_is_enabled(pdev)) {
        atomic_set(&pdev->enable_cnt, 1);
        pci_disable_device(pdev);
    }
    pdev->lkpi_managed = false;
}

int pcim_enable_device(struct pci_dev *pdev)
{
    if (pdev->lkpi_managed) return 0;
    int r = devm_add_action(&pdev->dev, pcim_release, pdev);
    if (r) return r;
    r = pci_enable_device(pdev);
    if (r) { devm_remove_action(&pdev->dev, pcim_release, pdev); return r; }
    pdev->lkpi_managed = true;
    return 0;
}

/* ---- BARs ---------------------------------------------------------------------------- */
int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
    int bars = 0;
    for (int i = 0; i < PCI_STD_NUM_BARS; i++)
        if (pci_resource_flags(dev, i) & flags) bars |= 1 << i;
    return bars;
}

int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
    if (bar < 0 || bar >= PCI_STD_NUM_BARS) return -EINVAL;
    if (!pci_resource_len(pdev, bar)) return 0;
    if (pdev->lkpi_regions & (1u << bar)) {
        dev_warn(&pdev->dev, "BAR %d: region already requested\n", bar);
        return -EBUSY;
    }
    pdev->lkpi_regions |= 1u << bar;
    return 0;
}

void pci_release_region(struct pci_dev *pdev, int bar)
{
    if (bar >= 0 && bar < PCI_STD_NUM_BARS) pdev->lkpi_regions &= ~(1u << bar);
}

int pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (!(bars & (1 << i))) continue;
        int r = pci_request_region(pdev, i, name);
        if (r) {
            while (--i >= 0)
                if (bars & (1 << i)) pci_release_region(pdev, i);
            return r;
        }
    }
    return 0;
}

void pci_release_selected_regions(struct pci_dev *pdev, int bars)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++)
        if (bars & (1 << i)) pci_release_region(pdev, i);
}

int pci_request_regions(struct pci_dev *pdev, const char *name)
{
    return pci_request_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1, name);
}

void pci_release_regions(struct pci_dev *pdev)
{
    pci_release_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1);
}

static void __iomem *map_bar(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen, bool wc)
{
    if (bar < 0 || bar >= PCI_STD_NUM_BARS) return NULL;
    resource_size_t start = pci_resource_start(dev, bar), len = pci_resource_len(dev, bar);
    unsigned long flags = pci_resource_flags(dev, bar);
    if (!len || offset >= len) return NULL;
    len -= offset;
    start += offset;
    if (maxlen && len > maxlen) len = maxlen;
    if (flags & IORESOURCE_IO) return ioport_map((unsigned long)start, (unsigned int)len);
    if (flags & IORESOURCE_MEM) return wc ? ioremap_wc(start, len) : ioremap(start, len);
    return NULL;
}

void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen)
{ return map_bar(dev, bar, offset, maxlen, false); }
void __iomem *pci_iomap_wc_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen)
{ return map_bar(dev, bar, offset, maxlen, true); }
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen) { return map_bar(dev, bar, 0, maxlen, false); }
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen) { return map_bar(dev, bar, 0, maxlen, true); }

void __iomem *pci_ioremap_bar(struct pci_dev *pdev, int bar)
{
    if (bar < 0 || bar >= PCI_STD_NUM_BARS || !(pci_resource_flags(pdev, bar) & IORESOURCE_MEM)) {
        dev_warn(&pdev->dev, "BAR %d is not memory\n", bar);
        return NULL;
    }
    return ioremap(pci_resource_start(pdev, bar), pci_resource_len(pdev, bar));
}

void __iomem *pci_ioremap_wc_bar(struct pci_dev *pdev, int bar)
{
    if (bar < 0 || bar >= PCI_STD_NUM_BARS || !(pci_resource_flags(pdev, bar) & IORESOURCE_MEM)) return NULL;
    return ioremap_wc(pci_resource_start(pdev, bar), pci_resource_len(pdev, bar));
}

/* ---- managed BARs --------------------------------------------------------------------- */
struct pcim_bar { struct pci_dev *pdev; int bar; };

static void pcim_iomap_release(struct device *dev, void *res)
{
    struct pcim_bar *b = res;
    b->pdev->lkpi_iomap[b->bar] = NULL;
}

static void pcim_region_release(struct device *dev, void *res)
{
    struct pcim_bar *b = res;
    pci_release_region(b->pdev, b->bar);
}

static int pcim_bar_match(struct device *dev, void *res, void *data)
{
    return ((struct pcim_bar *)res)->bar == *(int *)data;
}

static int pcim_add(struct pci_dev *pdev, int bar, dr_release_t release)
{
    struct pcim_bar *b = devres_alloc(release, sizeof *b, GFP_KERNEL);
    if (!b) return -ENOMEM;
    b->pdev = pdev;
    b->bar = bar;
    devres_add(&pdev->dev, b);
    return 0;
}

void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
    if (bar < 0 || bar >= PCI_STD_NUM_BARS) return NULL;
    if (pdev->lkpi_iomap[bar]) return pdev->lkpi_iomap[bar];
    void __iomem *addr = pci_iomap(pdev, bar, maxlen);
    if (!addr) return NULL;
    if (pcim_add(pdev, bar, pcim_iomap_release)) return NULL;
    pdev->lkpi_iomap[bar] = addr;
    return addr;
}

void pcim_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++)
        if (pdev->lkpi_iomap[i] == addr) devres_release(&pdev->dev, pcim_iomap_release, pcim_bar_match, &i);
}

void __iomem *const *pcim_iomap_table(struct pci_dev *pdev) { return pdev->lkpi_iomap; }

void __iomem *pcim_iomap_range(struct pci_dev *pdev, int bar, unsigned long offset, unsigned long len)
{
    return pci_iomap_range(pdev, bar, offset, len);
}

int pcim_request_region(struct pci_dev *pdev, int bar, const char *name)
{
    int r = pci_request_region(pdev, bar, name);
    if (r) return r;
    r = pcim_add(pdev, bar, pcim_region_release);
    if (r) pci_release_region(pdev, bar);
    return r;
}

int pcim_request_all_regions(struct pci_dev *pdev, const char *name)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (!pci_resource_len(pdev, i)) continue;
        int r = pcim_request_region(pdev, i, name);
        if (r) return r;
    }
    return 0;
}

void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name)
{
    int r = pcim_request_region(pdev, bar, name);
    if (r) return (void __iomem *)ERR_PTR(r);
    void __iomem *addr = pcim_iomap(pdev, bar, 0);
    if (!addr) {
        devres_release(&pdev->dev, pcim_region_release, pcim_bar_match, &bar);
        return (void __iomem *)ERR_PTR(-ENOMEM);
    }
    return addr;
}

void pcim_iounmap_region(struct pci_dev *pdev, int bar)
{
    devres_release(&pdev->dev, pcim_iomap_release, pcim_bar_match, &bar);
    devres_release(&pdev->dev, pcim_region_release, pcim_bar_match, &bar);
}

int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (!(mask & (1 << i))) continue;
        void __iomem *addr = pcim_iomap_region(pdev, i, name);
        if (IS_ERR(addr)) {
            pcim_iounmap_regions(pdev, mask & ((1 << i) - 1));
            return (int)PTR_ERR(addr);
        }
    }
    return 0;
}

int pcim_iomap_regions_request_all(struct pci_dev *pdev, int mask, const char *name)
{
    int r = pcim_request_all_regions(pdev, name);
    if (r) return r;
    for (int i = 0; i < PCI_STD_NUM_BARS; i++)
        if ((mask & (1 << i)) && !pcim_iomap(pdev, i, 0)) return -ENOMEM;
    return 0;
}

void pcim_iounmap_regions(struct pci_dev *pdev, int mask)
{
    for (int i = 0; i < PCI_STD_NUM_BARS; i++)
        if (mask & (1 << i)) pcim_iounmap_region(pdev, i);
}

/* ---- drivers ------------------------------------------------------------------------------ */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
    if (!ids) return NULL;
    for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
        if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
            (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
            (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
            (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device) &&
            !((ids->class ^ dev->class) & ids->class_mask))
            return ids;
    }
    return NULL;
}

static int pci_bus_match(struct device *dev, const struct device_driver *drv)
{
    const struct pci_driver *pdrv = to_pci_driver(drv);
    return pci_match_id(pdrv->id_table, to_pci_dev(dev)) != NULL;
}

static int pci_bus_probe(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = (struct pci_driver *)to_pci_driver(dev->driver);
    const struct pci_device_id *id = pci_match_id(pdrv->id_table, pdev);
    if (!id || !pdrv->probe) return -ENODEV;
    printk(KERN_INFO "linuxkpi: pci %s: [%04x:%04x] class %06x: probing with %s\n",
           pci_name(pdev), pdev->vendor, pdev->device, pdev->class, pdrv->name);
    pdev->driver = pdrv;
    get_device(dev);
    int r = pdrv->probe(pdev, id);
    if (r) {
        pdev->driver = NULL;
        put_device(dev);
        if (r > 0) r = -EINVAL;                         /* Linux treats positive as failure too */
    }
    return r;
}

static void pci_bus_remove(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = pdev->driver;
    if (pdrv && pdrv->remove) pdrv->remove(pdev);
    lkpi_pci_irq_release(pdev);
    pdev->driver = NULL;
    put_device(dev);
}

static void pci_bus_shutdown(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    if (pdev->driver && pdev->driver->shutdown) pdev->driver->shutdown(pdev);
}

const struct bus_type pci_bus_type = {
    .name = "pci",
    .match = pci_bus_match,
    .probe = pci_bus_probe,
    .remove = pci_bus_remove,
    .shutdown = pci_bus_shutdown,
};

int __pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name)
{
    drv->driver.name = drv->name;
    drv->driver.bus = &pci_bus_type;
    drv->driver.owner = owner;
    drv->driver.mod_name = mod_name;
    if (drv->dev_groups) drv->driver.dev_groups = drv->dev_groups;
    if (drv->groups) drv->driver.groups = drv->groups;
    return driver_register(&drv->driver);
}

void pci_unregister_driver(struct pci_driver *drv) { driver_unregister(&drv->driver); }

/* ---- looking devices up -------------------------------------------------------------------- */
struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
    if (dev) get_device(&dev->dev);
    return dev;
}

void pci_dev_put(struct pci_dev *dev)
{
    if (dev) put_device(&dev->dev);
}

/* the next device after `from` (whose reference is dropped) that matches */
static struct pci_dev *next_match(struct pci_dev *from, bool (*match)(struct pci_dev *, const void *), const void *data)
{
    struct pci_node *n;
    bool started = !from;
    struct pci_dev *found = NULL;
    list_for_each_entry(n, &all_devs, node) {
        if (!started) { started = n->pdev == from; continue; }
        if (match(n->pdev, data)) { found = pci_dev_get(n->pdev); break; }
    }
    pci_dev_put(from);
    return found;
}

static bool match_id(struct pci_dev *d, const void *data)
{
    const struct pci_device_id *id = data;
    struct pci_device_id one[2] = { *id, { 0 } };
    one[0].class_mask = 0;
    one[0].class = 0;
    return pci_match_id(one, d) != NULL;
}

struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device, unsigned int ss_vendor,
                               unsigned int ss_device, struct pci_dev *from)
{
    struct pci_device_id id = { .vendor = vendor, .device = device, .subvendor = ss_vendor, .subdevice = ss_device };
    return next_match(from, match_id, &id);
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
    return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}

static bool match_class(struct pci_dev *d, const void *data) { return d->class == *(const unsigned int *)data; }

struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from)
{
    return next_match(from, match_class, &class);
}

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn)
{
    return domain ? NULL : pci_dev_get(find_slot(bus, devfn));
}

struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{
    return pci_dev_get(find_slot(bus->number, devfn));
}

int pci_dev_present(const struct pci_device_id *ids)
{
    struct pci_node *n;
    list_for_each_entry(n, &all_devs, node)
        if (pci_match_id(ids, n->pdev)) return 1;
    return 0;
}

/* ---- building the device list ---------------------------------------------------------- */
static struct pci_bus *get_bus(unsigned int number)
{
    struct pci_bus *b;
    list_for_each_entry(b, &pci_buses, node)
        if (b->number == number) return b;
    b = kzalloc(sizeof *b, GFP_KERNEL);
    if (!b) return NULL;
    INIT_LIST_HEAD(&b->devices);
    b->number = (unsigned char)number;
    list_add_tail(&b->node, &pci_buses);
    return b;
}

static void pci_release_dev(struct device *dev) { }    /* devices stay for the life of the system */

static struct pci_dev *find_handle(void *handle)
{
    struct pci_node *n;
    list_for_each_entry(n, &all_devs, node)
        if (n->pdev->lkpi_handle == handle) return n->pdev;
    return NULL;
}

void lkpi_pci_core_init(void)
{
    struct lkpi_pci_info info;
    int count = lkpi_pci_count(), made = 0;

    if (bus_register(&pci_bus_type)) lkpi_die("linuxkpi: cannot register the PCI bus");

    for (int i = 0; i < count; i++) {
        if (lkpi_pci_get(i, &info)) continue;
        struct pci_dev *pdev = kzalloc(sizeof *pdev, GFP_KERNEL);
        struct pci_node *node = kzalloc(sizeof *node, GFP_KERNEL);
        struct pci_bus *bus = get_bus(info.bus);
        if (!pdev || !node || !bus) { kfree(pdev); kfree(node); break; }

        pdev->lkpi_handle = info.handle;
        pdev->lkpi_vector = -1;
        pdev->bus = bus;
        pdev->devfn = PCI_DEVFN(info.dev, info.fn);
        pdev->vendor = info.vendor;
        pdev->device = info.device;
        pdev->subsystem_vendor = info.subvendor;
        pdev->subsystem_device = info.subdevice;
        pdev->class = (unsigned)info.class_code << 16 | (unsigned)info.subclass << 8 | info.prog_if;
        pdev->revision = info.revision;
        pdev->hdr_type = info.header_type;
        pdev->multifunction = !!(info.header_type & 0x80);
        pdev->pin = info.irq_pin;
        pdev->cfg_size = 256;
        pdev->current_state = PCI_D0;
        pdev->dma_mask = DMA_BIT_MASK(32);
        atomic_set(&pdev->enable_cnt, 0);
        for (int b = 0; b < PCI_STD_NUM_BARS; b++) {
            struct resource *r = &pdev->resource[b];
            int kind = info.bar[b].kind;
            if (!info.bar[b].size || kind == 0 || kind == 5) continue;
            r->start = info.bar[b].base;
            r->end = info.bar[b].base + info.bar[b].size - 1;
            r->flags = kind == 1 ? IORESOURCE_IO : IORESOURCE_MEM;
            if (kind == 3) r->flags |= IORESOURCE_MEM_64;
            if (info.bar[b].prefetch) r->flags |= IORESOURCE_PREFETCH;
        }
        pdev->msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
        pdev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
        pdev->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
        if (pdev->pcie_cap) pci_read_config_word(pdev, pdev->pcie_cap + PCI_EXP_FLAGS, &pdev->pcie_flags_reg);
        if (info.parent) {
            struct pci_dev *bridge = find_handle(info.parent);
            if (bridge) {
                pdev->dev.parent = &bridge->dev;
                bus->self = bridge;
                bridge->subordinate = bus;
                bus->parent = bridge->bus;
            }
        }

        device_initialize(&pdev->dev);
        pdev->dev.bus = &pci_bus_type;
        pdev->dev.dma_mask = &pdev->dma_mask;
        pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
        pdev->dev.dma_parms = &pdev->dma_parms;
        pdev->dev.release = pci_release_dev;
        dev_set_name(&pdev->dev, "0000:%02x:%02x.%x", info.bus, info.dev, info.fn);
        for (int b = 0; b < PCI_STD_NUM_BARS; b++) pdev->resource[b].name = pci_name(pdev);

        list_add_tail(&pdev->bus_list, &bus->devices);
        node->pdev = pdev;
        list_add_tail(&node->node, &all_devs);
        if (device_add(&pdev->dev) == 0) made++;
    }
    printk(KERN_INFO "linuxkpi: pci: %d devices from Kestrel's scan\n", made);
}
