/* asm/io.h -- Kestrel's Linux KPI: MMIO and port I/O on x86-64
 *
 * MMIO addresses are physical addresses (identity map) that ioremap()
 * made uncached. "iomap" cookies follow Linux's lib/iomap.c convention:
 * ports appear as PIO_OFFSET + port, below PIO_RESERVED, so ioread*()
 * can serve both kinds. */
#ifndef _LKPI_ASM_IO_H
#define _LKPI_ASM_IO_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>
#include <asm/page.h>

#define __lkpi_mmio_rd(T, name) \
    static inline T name(const volatile void __iomem *a) { T v = *(const volatile T *)a; barrier(); return v; }
#define __lkpi_mmio_wr(T, name) \
    static inline void name(T v, volatile void __iomem *a) { barrier(); *(volatile T *)a = v; }
__lkpi_mmio_rd(u8,  readb)   __lkpi_mmio_rd(u16, readw)   __lkpi_mmio_rd(u32, readl)   __lkpi_mmio_rd(u64, readq)
__lkpi_mmio_wr(u8,  writeb)  __lkpi_mmio_wr(u16, writew)  __lkpi_mmio_wr(u32, writel)  __lkpi_mmio_wr(u64, writeq)
__lkpi_mmio_rd(u8,  __raw_readb)  __lkpi_mmio_rd(u16, __raw_readw)  __lkpi_mmio_rd(u32, __raw_readl)  __lkpi_mmio_rd(u64, __raw_readq)
__lkpi_mmio_wr(u8,  __raw_writeb) __lkpi_mmio_wr(u16, __raw_writew) __lkpi_mmio_wr(u32, __raw_writel) __lkpi_mmio_wr(u64, __raw_writeq)
#define readb_relaxed(a)        readb(a)
#define readw_relaxed(a)        readw(a)
#define readl_relaxed(a)        readl(a)
#define readq_relaxed(a)        readq(a)
#define writeb_relaxed(v, a)    writeb(v, a)
#define writew_relaxed(v, a)    writew(v, a)
#define writel_relaxed(v, a)    writel(v, a)
#define writeq_relaxed(v, a)    writeq(v, a)
#define mmiowb()                barrier()

static inline void outb(u8 v, unsigned long port)  { __asm__ __volatile__("outb %b0, %w1" :: "a"(v), "Nd"((u16)port)); }
static inline void outw(u16 v, unsigned long port) { __asm__ __volatile__("outw %w0, %w1" :: "a"(v), "Nd"((u16)port)); }
static inline void outl(u32 v, unsigned long port) { __asm__ __volatile__("outl %0, %w1" :: "a"(v), "Nd"((u16)port)); }
static inline u8  inb(unsigned long port) { u8 v;  __asm__ __volatile__("inb %w1, %b0" : "=a"(v) : "Nd"((u16)port)); return v; }
static inline u16 inw(unsigned long port) { u16 v; __asm__ __volatile__("inw %w1, %w0" : "=a"(v) : "Nd"((u16)port)); return v; }
static inline u32 inl(unsigned long port) { u32 v; __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"((u16)port)); return v; }
#define outb_p(v, p)            outb(v, p)
#define outw_p(v, p)            outw(v, p)
#define outl_p(v, p)            outl(v, p)
#define inb_p(p)                inb(p)
#define inw_p(p)                inw(p)
#define inl_p(p)                inl(p)
static inline void insb(unsigned long p, void *b, unsigned long n) { u8 *d = b; while (n--) *d++ = inb(p); }
static inline void insw(unsigned long p, void *b, unsigned long n) { u16 *d = b; while (n--) *d++ = inw(p); }
static inline void insl(unsigned long p, void *b, unsigned long n) { u32 *d = b; while (n--) *d++ = inl(p); }
static inline void outsb(unsigned long p, const void *b, unsigned long n) { const u8 *s = b; while (n--) outb(*s++, p); }
static inline void outsw(unsigned long p, const void *b, unsigned long n) { const u16 *s = b; while (n--) outw(*s++, p); }
static inline void outsl(unsigned long p, const void *b, unsigned long n) { const u32 *s = b; while (n--) outl(*s++, p); }

#define PIO_OFFSET              0x10000UL
#define PIO_MASK                0x0ffffUL
#define PIO_RESERVED            0x40000UL
#define IO_SPACE_LIMIT          0xffff

#define __lkpi_iord(T, name, rd, in) \
    static inline T name(const void __iomem *a) \
    { unsigned long p = (unsigned long)a; return p >= PIO_RESERVED ? rd(a) : (T)in(p & PIO_MASK); }
#define __lkpi_iowr(T, name, wr, out) \
    static inline void name(T v, void __iomem *a) \
    { unsigned long p = (unsigned long)a; if (p >= PIO_RESERVED) wr(v, a); else out(v, p & PIO_MASK); }
__lkpi_iord(u8, ioread8, readb, inb)    __lkpi_iord(u16, ioread16, readw, inw)    __lkpi_iord(u32, ioread32, readl, inl)
__lkpi_iowr(u8, iowrite8, writeb, outb) __lkpi_iowr(u16, iowrite16, writew, outw) __lkpi_iowr(u32, iowrite32, writel, outl)
static inline u64 ioread64(const void __iomem *a) { return readq(a); }
static inline void iowrite64(u64 v, void __iomem *a) { writeq(v, a); }
static inline u16 ioread16be(const void __iomem *a) { return __builtin_bswap16(ioread16(a)); }
static inline u32 ioread32be(const void __iomem *a) { return __builtin_bswap32(ioread32(a)); }
static inline void iowrite16be(u16 v, void __iomem *a) { iowrite16(__builtin_bswap16(v), a); }
static inline void iowrite32be(u32 v, void __iomem *a) { iowrite32(__builtin_bswap32(v), a); }
static inline u64 ioread64_lo_hi(const void __iomem *a)
{ u32 lo = ioread32(a), hi = ioread32((const u8 __iomem *)a + 4); return lo | ((u64)hi << 32); }
static inline void iowrite64_lo_hi(u64 v, void __iomem *a)
{ iowrite32((u32)v, a); iowrite32((u32)(v >> 32), (u8 __iomem *)a + 4); }
static inline void ioread8_rep(const void __iomem *a, void *b, unsigned long n) { u8 *d = b; while (n--) *d++ = ioread8(a); }
static inline void ioread16_rep(const void __iomem *a, void *b, unsigned long n) { u16 *d = b; while (n--) *d++ = ioread16(a); }
static inline void ioread32_rep(const void __iomem *a, void *b, unsigned long n) { u32 *d = b; while (n--) *d++ = ioread32(a); }
static inline void iowrite8_rep(void __iomem *a, const void *b, unsigned long n) { const u8 *s = b; while (n--) iowrite8(*s++, a); }
static inline void iowrite16_rep(void __iomem *a, const void *b, unsigned long n) { const u16 *s = b; while (n--) iowrite16(*s++, a); }
static inline void iowrite32_rep(void __iomem *a, const void *b, unsigned long n) { const u32 *s = b; while (n--) iowrite32(*s++, a); }

static inline void memcpy_fromio(void *dst, const volatile void __iomem *src, size_t n)
{ u8 *d = dst; const volatile u8 *s = (const volatile u8 *)src; while (n--) *d++ = *s++; }
static inline void memcpy_toio(volatile void __iomem *dst, const void *src, size_t n)
{ volatile u8 *d = (volatile u8 *)dst; const u8 *s = src; while (n--) *d++ = *s++; }
static inline void memset_io(volatile void __iomem *dst, int c, size_t n)
{ volatile u8 *d = (volatile u8 *)dst; while (n--) *d++ = (u8)c; }

#endif
