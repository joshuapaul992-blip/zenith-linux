/* linux/pci.h -- Kestrel's Linux KPI: PCI devices and drivers (linuxkpi/src/pci.c)
 *
 * Every function Kestrel's enumerator found becomes a struct pci_dev on
 * the "pci" bus; a pci_driver's id table is matched against them when
 * it registers. Configuration space is the legacy 256 bytes (mechanism
 * #1: no extended capabilities). Interrupts are MSI-X or MSI, one vector
 * per device; legacy INTx is not routed (Kestrel has no _PRT), so a
 * driver asking for pdev->irq gets an MSI vector when the device has one. */
#ifndef _LKPI_LINUX_PCI_H
#define _LKPI_LINUX_PCI_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/pm.h>

#define PCI_DEVFN(slot, func)   ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)         (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)         ((devfn) & 0x07)
#define PCI_BUS_NUM(x)          (((x) >> 8) & 0xff)

#define PCI_STD_NUM_BARS        6
#define PCI_STD_RESOURCES       0
#define PCI_STD_RESOURCE_END    5
#define PCI_ROM_RESOURCE        6
#define PCI_NUM_RESOURCES       7
#define DEVICE_COUNT_RESOURCE   PCI_NUM_RESOURCES

typedef int pci_power_t;
#define PCI_D0                  ((pci_power_t)0)
#define PCI_D1                  ((pci_power_t)1)
#define PCI_D2                  ((pci_power_t)2)
#define PCI_D3hot               ((pci_power_t)3)
#define PCI_D3cold              ((pci_power_t)4)
#define PCI_UNKNOWN             ((pci_power_t)5)
#define PCI_POWER_ERROR         ((pci_power_t)-1)

typedef unsigned int pci_channel_state_t;
typedef unsigned int pci_ers_result_t;
enum pci_channel_state { pci_channel_io_normal = 1, pci_channel_io_frozen = 2, pci_channel_io_perm_failure = 3 };
enum { PCI_ERS_RESULT_NONE = 1, PCI_ERS_RESULT_CAN_RECOVER = 2, PCI_ERS_RESULT_NEED_RESET = 3,
       PCI_ERS_RESULT_DISCONNECT = 4, PCI_ERS_RESULT_RECOVERED = 5, PCI_ERS_RESULT_NO_AER_DRIVER = 6 };

struct pci_dev;
struct pci_error_handlers {
    pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t error);
    pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
    pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
    void (*reset_prepare)(struct pci_dev *dev);
    void (*reset_done)(struct pci_dev *dev);
    void (*resume)(struct pci_dev *dev);
    void (*cor_error_detected)(struct pci_dev *dev);
};

struct pci_bus {
    struct list_head    node;
    struct pci_bus     *parent;
    struct list_head    devices;
    struct pci_dev     *self;           /* the bridge leading here, NULL for bus 0 */
    unsigned char       number;
    unsigned char       primary;
    int                 domain_nr;
};

struct pci_dev {
    struct list_head    bus_list;
    struct pci_bus     *bus;
    struct pci_bus     *subordinate;
    void               *sysdata;
    unsigned int        devfn;
    unsigned short      vendor, device;
    unsigned short      subsystem_vendor, subsystem_device;
    unsigned int        class;          /* class << 16 | subclass << 8 | prog-if */
    u8                  revision;
    u8                  hdr_type;
    u8                  pin;
    u8                  msi_cap, msix_cap, pcie_cap;
    u16                 pcie_flags_reg;
    struct pci_driver  *driver;
    u64                 dma_mask;
    struct device_dma_parameters dma_parms;
    pci_power_t         current_state;
    struct device       dev;
    int                 cfg_size;
    unsigned int        irq;
    struct resource     resource[PCI_NUM_RESOURCES];
    unsigned int        transparent:1, multifunction:1, is_busmaster:1, no_msi:1, msi_enabled:1,
                        msix_enabled:1, is_virtfn:1, is_physfn:1, broken_intx_masking:1, is_hotplug_bridge:1,
                        no_64bit_msi:1, d3cold_allowed:1;
    atomic_t            enable_cnt;
    u32                 saved_config_space[16];
    bool                state_saved;
    /* the KPI's own */
    void               *lkpi_handle;            /* Kestrel's struct pci_device */
    int                 lkpi_vector;            /* MSI vector, or -1 */
    bool                lkpi_managed;           /* pcim_enable_device */
    bool                lkpi_irq_managed;       /* vectors freed at unbind */
    u32                 lkpi_regions;           /* BARs requested */
    void __iomem       *lkpi_iomap[PCI_STD_NUM_BARS];  /* pcim_iomap_table */
};
#define to_pci_dev(n)           container_of(n, struct pci_dev, dev)

struct pci_driver {
    const char                  *name;
    const struct pci_device_id  *id_table;
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
    int  (*suspend)(struct pci_dev *dev, pm_message_t state);
    int  (*resume)(struct pci_dev *dev);
    void (*shutdown)(struct pci_dev *dev);
    int  (*sriov_configure)(struct pci_dev *dev, int num_vfs);
    const struct pci_error_handlers *err_handler;
    const struct attribute_group **groups;
    const struct attribute_group **dev_groups;
    struct device_driver         driver;
    bool                         driver_managed_dma;
};
#define to_pci_driver(drv)      container_of_const(drv, struct pci_driver, driver)

extern const struct bus_type pci_bus_type;

#define PCI_DEVICE(vend, dev) \
    .vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) \
    .vendor = (vend), .device = (dev), .subvendor = (subvend), .subdevice = (subdev)
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) \
    .class = (dev_class), .class_mask = (dev_class_mask), \
    .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_VDEVICE(vend, dev) \
    .vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define PCI_VDEVICE_SUB(vend, dev, subvend, subdev) \
    .vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = (subvend), .subdevice = (subdev), 0, 0
#define PCI_DEVICE_DATA(vend, dev, data) \
    .vendor = PCI_VENDOR_ID_##vend, .device = PCI_DEVICE_ID_##vend##_##dev, \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0, .driver_data = (kernel_ulong_t)(data)

int  __pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name);
#define pci_register_driver(driver)     __pci_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
void pci_unregister_driver(struct pci_driver *drv);
#define module_pci_driver(__pci_driver) module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)
#define builtin_pci_driver(__pci_driver) builtin_driver(__pci_driver, pci_register_driver)
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev);
static inline const char *pci_name(const struct pci_dev *pdev) { return dev_name(&pdev->dev); }
static inline void *pci_get_drvdata(struct pci_dev *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void pci_set_drvdata(struct pci_dev *pdev, void *data) { dev_set_drvdata(&pdev->dev, data); }
static inline struct pci_driver *pci_dev_driver(const struct pci_dev *dev) { return dev->driver; }
static inline int pci_domain_nr(struct pci_bus *bus) { return 0; }
static inline u16 pci_dev_id(struct pci_dev *dev) { return (u16)(dev->bus->number << 8 | dev->devfn); }
static inline bool pci_is_root_bus(struct pci_bus *pbus) { return !pbus->parent; }
static inline struct pci_dev *pci_upstream_bridge(struct pci_dev *dev) { return dev->bus->self; }
static inline bool pci_is_bridge(struct pci_dev *dev) { return (dev->hdr_type & 0x7f) == PCI_HEADER_TYPE_BRIDGE; }

/* ---- configuration space: 0 (PCIBIOS_SUCCESSFUL) or a PCIBIOS_ code ---- */
#define PCIBIOS_SUCCESSFUL              0x00
#define PCIBIOS_FUNC_NOT_SUPPORTED      0x81
#define PCIBIOS_BAD_VENDOR_ID           0x83
#define PCIBIOS_DEVICE_NOT_FOUND        0x86
#define PCIBIOS_BAD_REGISTER_NUMBER     0x87
#define PCIBIOS_SET_FAILED              0x88
#define PCIBIOS_BUFFER_TOO_SMALL        0x89
static inline int pcibios_err_to_errno(int err)
{
    if (err <= PCIBIOS_SUCCESSFUL) return err;
    switch (err) {
    case PCIBIOS_FUNC_NOT_SUPPORTED: return -ENOENT;
    case PCIBIOS_BAD_VENDOR_ID: return -ENOTTY;
    case PCIBIOS_DEVICE_NOT_FOUND: return -ENODEV;
    case PCIBIOS_BAD_REGISTER_NUMBER: return -EFAULT;
    case PCIBIOS_SET_FAILED: return -EIO;
    case PCIBIOS_BUFFER_TOO_SMALL: return -ENOSPC;
    }
    return -ERANGE;
}
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val);
int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val);
int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val);
int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val);
int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 val);
int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val);
void pci_clear_and_set_config_dword(const struct pci_dev *dev, int pos, u32 clear, u32 set);

u8  pci_find_capability(struct pci_dev *dev, int cap);
u8  pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap);
static inline u16 pci_find_ext_capability(struct pci_dev *dev, int cap) { return 0; }
static inline u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 pos, int cap) { return 0; }
u16 pci_find_vsec_capability(struct pci_dev *dev, u16 vendor, int cap);
static inline bool pci_is_pcie(struct pci_dev *dev) { return dev->pcie_cap != 0; }
static inline int pci_pcie_type(const struct pci_dev *dev) { return (dev->pcie_flags_reg & PCI_EXP_FLAGS_TYPE) >> 4; }
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val);
int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set);
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set);
#define pcie_capability_set_word(d, p, s)       pcie_capability_clear_and_set_word(d, p, 0, s)
#define pcie_capability_clear_word(d, p, c)     pcie_capability_clear_and_set_word(d, p, c, 0)
#define pcie_capability_set_dword(d, p, s)      pcie_capability_clear_and_set_dword(d, p, 0, s)
#define pcie_capability_clear_dword(d, p, c)    pcie_capability_clear_and_set_dword(d, p, c, 0)

/* ---- enabling, bus mastering --------------------------------------------- */
int  __must_check pci_enable_device(struct pci_dev *dev);
int  __must_check pci_enable_device_mem(struct pci_dev *dev);
int  __must_check pci_enable_device_io(struct pci_dev *dev);
int  __must_check pcim_enable_device(struct pci_dev *pdev);
void pci_disable_device(struct pci_dev *dev);
static inline int pci_is_enabled(struct pci_dev *pdev) { return atomic_read(&pdev->enable_cnt) > 0; }
static inline bool pci_is_managed(struct pci_dev *pdev) { return pdev->lkpi_managed; }
void pci_set_master(struct pci_dev *dev);
void pci_clear_master(struct pci_dev *dev);
void pci_intx(struct pci_dev *dev, int enable);
static inline int pci_set_mwi(struct pci_dev *dev) { return 0; }
static inline int pci_try_set_mwi(struct pci_dev *dev) { return 0; }
static inline void pci_clear_mwi(struct pci_dev *dev) { }
static inline int pci_set_power_state(struct pci_dev *dev, pci_power_t state) { dev->current_state = state; return 0; }
static inline pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state) { return PCI_D3hot; }
static inline int pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable) { return 0; }
static inline int pci_wake_from_d3(struct pci_dev *dev, bool enable) { return 0; }
static inline int pci_prepare_to_sleep(struct pci_dev *dev) { return 0; }
static inline int pci_back_from_sleep(struct pci_dev *dev) { return 0; }
static inline void pci_d3cold_enable(struct pci_dev *dev) { }
static inline void pci_d3cold_disable(struct pci_dev *dev) { }
static inline void pci_ignore_hotplug(struct pci_dev *dev) { }
int  pci_save_state(struct pci_dev *dev);
void pci_restore_state(struct pci_dev *dev);
static inline int pci_reset_function(struct pci_dev *dev) { return -ENOTTY; }
static inline int pci_reset_function_locked(struct pci_dev *dev) { return -ENOTTY; }
static inline int pci_wait_for_pending_transaction(struct pci_dev *dev) { return 1; }
static inline u64 pci_get_dsn(struct pci_dev *dev) { return 0; }
static inline bool pci_dev_is_disconnected(const struct pci_dev *dev) { return false; }
static inline bool pci_channel_offline(struct pci_dev *pdev) { return false; }
static inline bool pci_device_is_present(struct pci_dev *pdev) { return true; }
static inline int pci_enable_pcie_error_reporting(struct pci_dev *dev) { return 0; }
static inline int pci_disable_pcie_error_reporting(struct pci_dev *dev) { return 0; }
static inline int pcie_set_readrq(struct pci_dev *dev, int rq) { return 0; }
static inline int pcie_get_readrq(struct pci_dev *dev) { return 512; }

/* ---- BARs ------------------------------------------------------------------ */
#define pci_resource_n(dev, bar)        (&(dev)->resource[(bar)])
#define pci_resource_start(dev, bar)    ((dev)->resource[(bar)].start)
#define pci_resource_end(dev, bar)      ((dev)->resource[(bar)].end)
#define pci_resource_flags(dev, bar)    ((dev)->resource[(bar)].flags)
#define pci_resource_len(dev, bar) \
    ((pci_resource_end((dev), (bar)) == 0) ? 0 : (pci_resource_end((dev), (bar)) - pci_resource_start((dev), (bar)) + 1))
int  pci_select_bars(struct pci_dev *dev, unsigned long flags);
int  __must_check pci_request_region(struct pci_dev *pdev, int bar, const char *name);
void pci_release_region(struct pci_dev *pdev, int bar);
int  __must_check pci_request_regions(struct pci_dev *pdev, const char *name);
void pci_release_regions(struct pci_dev *pdev);
int  __must_check pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name);
void pci_release_selected_regions(struct pci_dev *pdev, int bars);
#define pci_request_regions_exclusive(p, n)     pci_request_regions(p, n)
#define pci_request_selected_regions_exclusive(p, b, n) pci_request_selected_regions(p, b, n)
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen);
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen);
void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen);
void __iomem *pci_iomap_wc_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen);
static inline void pci_iounmap(struct pci_dev *dev, void __iomem *addr) { }
void __iomem *pci_ioremap_bar(struct pci_dev *pdev, int bar);
void __iomem *pci_ioremap_wc_bar(struct pci_dev *pdev, int bar);
/* managed: released when the driver is unbound */
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void pcim_iounmap(struct pci_dev *pdev, void __iomem *addr);
void __iomem *const *pcim_iomap_table(struct pci_dev *pdev);
int  pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name);
int  pcim_iomap_regions_request_all(struct pci_dev *pdev, int mask, const char *name);
void pcim_iounmap_regions(struct pci_dev *pdev, int mask);
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name);
void pcim_iounmap_region(struct pci_dev *pdev, int bar);
int  pcim_request_region(struct pci_dev *pdev, int bar, const char *name);
int  pcim_request_all_regions(struct pci_dev *pdev, const char *name);
void __iomem *pcim_iomap_range(struct pci_dev *pdev, int bar, unsigned long offset, unsigned long len);

/* ---- interrupts ---------------------------------------------------------- */
#define PCI_IRQ_INTX            (1 << 0)
#define PCI_IRQ_LEGACY          PCI_IRQ_INTX
#define PCI_IRQ_MSI             (1 << 1)
#define PCI_IRQ_MSIX            (1 << 2)
#define PCI_IRQ_AFFINITY        (1 << 3)
#define PCI_IRQ_ALL_TYPES       (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)
struct irq_affinity;
struct msix_entry { u32 vector; u16 entry; };
int  pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
static inline int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs,
                                                 unsigned int flags, struct irq_affinity *affd)
{ return pci_alloc_irq_vectors(dev, min_vecs, max_vecs, flags & ~PCI_IRQ_AFFINITY); }
void pci_free_irq_vectors(struct pci_dev *dev);
int  pci_irq_vector(struct pci_dev *dev, unsigned int nr);
int  pci_enable_msi(struct pci_dev *dev);
void pci_disable_msi(struct pci_dev *dev);
int  pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec, int maxvec);
static inline int pci_enable_msix_exact(struct pci_dev *dev, struct msix_entry *entries, int nvec)
{ int rc = pci_enable_msix_range(dev, entries, nvec, nvec); return rc < 0 ? rc : 0; }
void pci_disable_msix(struct pci_dev *dev);
static inline int pci_msi_enabled(void) { return 1; }
static inline int pci_dev_msi_enabled(struct pci_dev *pdev) { return pdev->msi_enabled || pdev->msix_enabled; }
static inline int pci_msix_vec_count(struct pci_dev *dev) { return dev->msix_cap ? 1 : -EINVAL; }
static inline int pci_msi_vec_count(struct pci_dev *dev) { return dev->msi_cap ? 1 : -EINVAL; }
__printf(6, 7) int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler,
                                   irq_handler_t thread_fn, void *dev_id, const char *fmt, ...);
void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id);

/* ---- finding devices: a reference the caller drops with pci_dev_put -------- */
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);
struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device, unsigned int ss_vendor,
                               unsigned int ss_device, struct pci_dev *from);
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from);
struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn);
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn);
struct pci_dev *pci_dev_get(struct pci_dev *dev);
void pci_dev_put(struct pci_dev *dev);
int  pci_dev_present(const struct pci_device_id *ids);
#define for_each_pci_dev(d)     while ((d = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, d)) != NULL)
#define for_each_pci_bridge(dev, bus) list_for_each_entry(dev, &(bus)->devices, bus_list) if (!pci_is_bridge(dev)) {} else

/* ---- the old DMA mask calls -------------------------------------------- */
static inline int pci_set_dma_mask(struct pci_dev *dev, u64 mask) { return dma_set_mask(&dev->dev, mask); }
static inline int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask) { return dma_set_coherent_mask(&dev->dev, mask); }

#endif
