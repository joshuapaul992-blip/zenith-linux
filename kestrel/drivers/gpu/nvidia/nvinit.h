/* =============================================================================
 *  nvinit.h -- interpreter for NVIDIA VBIOS init scripts
 *
 *  Display code runs small byte-code scripts from the VBIOS: the IED scripts
 *  of the display table (what to do when an output is attached to or
 *  detached from a head) and the DP table's link training scripts. They poke
 *  registers (with per-head / per-OR / per-link address mangling), test
 *  conditions, delay, talk DPCD over AUX and occasionally program PLLs.
 *
 *  A port of nouveau's nvkm/subdev/bios/init.c (Copyright 2012 Red Hat Inc.,
 *  MIT licence) for GF119-and-later display scripts: every opcode nouveau
 *  knows is decoded with its exact length, so scripts can be walked and
 *  skipped correctly; opcodes that need subsystems Kestrel does not drive
 *  (bit-banged I2C, GPIO reset, pre-NV50 memory set-up) are logged and
 *  skipped instead of executed.
 *
 *  Hardware access goes through `ops`, so the interpreter runs on the host
 *  (test/nvinit_test.c) as well as against BAR0.
 * ============================================================================= */
#ifndef NVINIT_H
#define NVINIT_H

#include "nvbios.h"

struct nvinit_ops {
    void     *ctx;
    uint32_t (*rd32)(void *ctx, uint32_t reg);
    void     (*wr32)(void *ctx, uint32_t reg, uint32_t val);
    uint8_t  (*rd08)(void *ctx, uint32_t reg);
    void     (*wr08)(void *ctx, uint32_t reg, uint8_t val);
    void     (*delay_us)(void *ctx, uint32_t us);
    /* DPCD over the AUX channel of DCB output `outp` (may be NULL). */
    int      (*aux_rd)(void *ctx, const struct nvbios_output *outp, uint32_t addr, uint8_t *val);
    int      (*aux_wr)(void *ctx, const struct nvbios_output *outp, uint32_t addr, uint8_t val);
    /* Program a PLL (type < 0x100 or register) to kHz (may be NULL). */
    int      (*pll_set)(void *ctx, uint32_t type_or_reg, uint32_t khz);
    void     (*log)(void *ctx, const char *line);  /* one line, no newline; may be NULL */
};

struct nvinit {
    const struct nvbios *bios;
    const struct nvinit_ops *ops;
    uint32_t offset;                    /* script being executed */
    const struct nvbios_output *outp;   /* context for OR/link/AUX opcodes, may be NULL */
    int      or, link, head;            /* -1 / 0 = not known */
    uint8_t  execute;
    int      nested;
    uint32_t repeat, repend;
    uint32_t ramcfg;
    bool     trace;                     /* log every opcode */

    /* statistics, for the report */
    uint32_t opcodes, writes, skipped, warnings;
};

/* Run the script at `offset` with the given context. Returns 0, or < 0 if
 * the script could not be parsed (unknown opcode, runaway). */
int nvinit_run(const struct nvbios *bios, const struct nvinit_ops *ops, uint32_t offset,
               const struct nvbios_output *outp, int or, int link, int head, bool trace,
               struct nvinit *stats_out);

#endif /* NVINIT_H */
