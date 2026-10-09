/* nvpriv.h -- helpers shared by the NVIDIA driver's own files (not an API) */
#ifndef NVPRIV_H
#define NVPRIV_H

#include "nvidia.h"
#include "nvfmt.h"

extern const struct nv_platform *nv_plat;

#ifdef NV_MMIO_HOOKS            /* host test: a simulated BAR0 (test/nvidia_sim_test.c) */
uint32_t nv_sim_rd32(uint32_t reg);
void     nv_sim_wr32(uint32_t reg, uint32_t v);
static inline uint32_t nv_rd32(const struct nv_device *d, uint32_t reg) { (void)d; return nv_sim_rd32(reg); }
static inline void nv_wr32(struct nv_device *d, uint32_t reg, uint32_t v) { (void)d; nv_sim_wr32(reg, v); }
static inline uint8_t nv_rd08(const struct nv_device *d, uint32_t reg)
{ return (uint8_t)(nv_rd32(d, reg & ~3u) >> ((reg & 3) * 8)); }
static inline void nv_wr08(struct nv_device *d, uint32_t reg, uint8_t v)
{
    uint32_t s = (reg & 3) * 8, w = nv_rd32(d, reg & ~3u);
    nv_wr32(d, reg & ~3u, (w & ~(0xffu << s)) | (uint32_t)v << s);
}
#else
static inline uint32_t nv_rd32(const struct nv_device *d, uint32_t reg) { return *(const volatile uint32_t *)(d->mmio + reg); }
static inline void nv_wr32(struct nv_device *d, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(d->mmio + reg) = v; }
static inline uint8_t nv_rd08(const struct nv_device *d, uint32_t reg) { return *(const volatile uint8_t *)(d->mmio + reg); }
static inline void nv_wr08(struct nv_device *d, uint32_t reg, uint8_t v) { *(volatile uint8_t *)(d->mmio + reg) = v; }
#endif

static inline uint32_t nv_mask(struct nv_device *d, uint32_t reg, uint32_t mask, uint32_t data)
{
    uint32_t old = nv_rd32(d, reg);
    nv_wr32(d, reg, (old & ~mask) | data);
    return old;
}

static inline void nv_udelay(uint32_t us) { nv_plat->delay_us(us); }

/* Poll until (reg & mask) == val; false after `us` microseconds. */
static inline bool nv_wait(struct nv_device *d, uint32_t reg, uint32_t mask, uint32_t val, uint32_t us)
{
    for (uint32_t t = 0; ; t++) {
        if ((nv_rd32(d, reg) & mask) == val) return true;
        if (t >= us) return false;
        nv_udelay(1);
    }
}

void nv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* DP AUX on GM20x/GP10x (nvidia.c) */
int  nv_aux_xfer(struct nv_device *d, int ch, bool retry, uint8_t type, uint32_t addr, uint8_t *data, uint8_t *size);
void nv_aux_pad_mode(struct nv_device *d, int share);

#endif
