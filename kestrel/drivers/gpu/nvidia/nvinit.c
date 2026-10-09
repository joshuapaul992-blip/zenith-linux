/* =============================================================================
 *  nvinit.c -- VBIOS init-script interpreter (see nvinit.h)
 *
 *  Opcode by opcode a port of nouveau nvkm/subdev/bios/init.c, Copyright 2012
 *  Red Hat Inc., MIT licence (third_party/nouveau/LICENSE). The execute flag
 *  logic, the register address mangling (head / OR / link bits) and every
 *  operand length are nouveau's. Differences, all logged when hit:
 *    - bit-banged I2C opcodes (0x4c-0x4e, 0x5e, 0x9a) and GPIO resets (0x8e,
 *      0xa9) are decoded and skipped: display scripts on DP-only boards do
 *      not use them, and Kestrel has no GPU I2C or GPIO driver
 *    - pre-NV50 opcodes (TMDS 0x4f/0x50, memory/clock set-up 0x63-0x68) are
 *      decoded and skipped; on NV50+ BIOSes they never execute anyway
 *    - INIT_RAM_RESTRICT uses the memory strap without the M0203 remap
 * ============================================================================= */
#include "nvinit.h"
#include "nvfmt.h"

#define MAX_OPCODES     100000          /* runaway guard per nvinit_run() */
#define MAX_NESTING     8

/* ---- logging ------------------------------------------------------------------ */
static void ilog(struct nvinit *init, const char *what, const char *f, ...) __attribute__((format(printf, 3, 4)));
static void ilog(struct nvinit *init, const char *what, const char *f, ...)
{
    if (!init->ops->log) return;
    char line[200];
    size_t n = nv_snprintf(line, sizeof line, "nvinit: %s 0x%04x[%c]: ", what, init->offset,
                           ((init->execute == 1) || ((init->execute & 5) == 5)) ? '0' + (init->nested - 1) : ' ');
    if (n < sizeof line) {
        va_list ap; va_start(ap, f);
        nv_vsnprintf(line + n, sizeof line - n, f, ap);
        va_end(ap);
    }
    init->ops->log(init->ops->ctx, line);
}
#define trace(...) do { if (init->trace) ilog(init, "trace", __VA_ARGS__); } while (0)
#define warn(...)  do { init->warnings++; ilog(init, "warn", __VA_ARGS__); } while (0)
#define error(...) do { init->warnings++; ilog(init, "ERROR", __VA_ARGS__); } while (0)

#define B08(a) nvbios_rd08(init->bios, (a))
#define B16(a) nvbios_rd16(init->bios, (a))
#define B32(a) nvbios_rd32(init->bios, (a))

/* ---- control flow helpers --------------------------------------------------------- */
static inline bool init_exec(struct nvinit *init) { return (init->execute == 1) || ((init->execute & 5) == 5); }
static inline void init_exec_set(struct nvinit *init, bool exec) { if (exec) init->execute &= 0xfd; else init->execute |= 0x02; }
static inline void init_exec_inv(struct nvinit *init) { init->execute ^= 0x02; }
static inline void init_exec_force(struct nvinit *init, bool exec) { if (exec) init->execute |= 0x04; else init->execute &= 0xfb; }

/* ---- accessors ----------------------------------------------------------------------- */
static int init_or(struct nvinit *init)
{
    if (init_exec(init)) {
        if (init->or >= 0) return init->or;
        error("script needs OR");
    }
    return 0;
}

static int init_link(struct nvinit *init)
{
    if (init_exec(init)) {
        if (init->link) return init->link == 2;
        error("script needs OR link");
    }
    return 0;
}

static int init_head(struct nvinit *init)
{
    if (init_exec(init)) {
        if (init->head >= 0) return init->head;
        error("script needs head");
    }
    return 0;
}

static uint8_t init_conn(struct nvinit *init)
{
    if (init_exec(init)) {
        if (init->outp && init->outp->connector < init->bios->nconns)
            return init->bios->conn[init->outp->connector].type;
        error("script needs connector type");
    }
    return 0xff;
}

/* GF8+ display scripts mangle addresses to select a head / OR / sub-link. */
static uint32_t init_nvreg(struct nvinit *init, uint32_t reg)
{
    reg &= ~0x00000003u;
    if (reg & 0x80000000u) { reg += (uint32_t)init_head(init) * 0x800; reg &= ~0x80000000u; }
    if (reg & 0x40000000u) {
        reg += (uint32_t)init_or(init) * 0x800;
        reg &= ~0x40000000u;
        if (reg & 0x20000000u) { reg += (uint32_t)init_link(init) * 0x80; reg &= ~0x20000000u; }
    }
    if (reg & ~0x00fffffcu) warn("unknown bits in register 0x%08x", reg);
    return reg;
}

static uint32_t init_rd32(struct nvinit *init, uint32_t reg)
{
    reg = init_nvreg(init, reg);
    if (init_exec(init)) return init->ops->rd32(init->ops->ctx, reg);
    return 0;
}

static void init_wr32(struct nvinit *init, uint32_t reg, uint32_t val)
{
    reg = init_nvreg(init, reg);
    if (init_exec(init)) {
        if (init->trace) ilog(init, "trace", "  R[%06x] = %08x", reg, val);
        init->ops->wr32(init->ops->ctx, reg, val);
        init->writes++;
    }
}

static uint32_t init_mask(struct nvinit *init, uint32_t reg, uint32_t mask, uint32_t val)
{
    reg = init_nvreg(init, reg);
    if (init_exec(init)) {
        uint32_t tmp = init->ops->rd32(init->ops->ctx, reg);
        if (init->trace) ilog(init, "trace", "  R[%06x] = %08x (was %08x)", reg, (tmp & ~mask) | val, tmp);
        init->ops->wr32(init->ops->ctx, reg, (tmp & ~mask) | val);
        init->writes++;
        return tmp;
    }
    return 0;
}

/* VGA ports: on NV50+ all of them live at 0x601000 + port. */
static uint8_t init_rdport(struct nvinit *init, uint16_t port)
{
    if (init_exec(init) && init->ops->rd08) return init->ops->rd08(init->ops->ctx, 0x601000u + port);
    return 0;
}

static void init_wrport(struct nvinit *init, uint16_t port, uint8_t value)
{
    if (init_exec(init) && init->ops->wr08) init->ops->wr08(init->ops->ctx, 0x601000u + port, value);
}

static uint8_t init_rdvgai(struct nvinit *init, uint16_t port, uint8_t index)
{
    if (port != 0x03c4 && port != 0x03ce && port != 0x03d4) return 0;
    init_wrport(init, port, index);
    return init_rdport(init, (uint16_t)(port + 1));
}

static void init_wrvgai(struct nvinit *init, uint16_t port, uint8_t index, uint8_t value)
{
    if (port != 0x03c4 && port != 0x03ce && port != 0x03d4) return;
    init_wrport(init, port, index);
    init_wrport(init, (uint16_t)(port + 1), value);
}

static uint8_t init_rdauxr(struct nvinit *init, uint32_t addr)
{
    if (!init->outp) {
        if (init_exec(init)) error("script needs output for aux");
        return 0;
    }
    uint8_t v = 0;
    if (init_exec(init) && init->ops->aux_rd) {
        int ret = init->ops->aux_rd(init->ops->ctx, init->outp, addr, &v);
        if (ret) { trace("auxch read failed with %d", ret); return 0; }
    }
    return v;
}

static void init_wrauxr(struct nvinit *init, uint32_t addr, uint8_t data)
{
    if (!init->outp) {
        if (init_exec(init)) error("script needs output for aux");
        return;
    }
    if (init_exec(init) && init->ops->aux_wr) {
        int ret = init->ops->aux_wr(init->ops->ctx, init->outp, addr, data);
        if (ret) trace("auxch write failed with %d", ret);
    }
}

static void init_prog_pll(struct nvinit *init, uint32_t id, uint32_t freq)
{
    if (!init_exec(init)) return;
    if (!init->ops->pll_set || init->ops->pll_set(init->ops->ctx, id, freq))
        warn("failed to prog pll 0x%08x to %ukHz", id, freq);
}

static void init_delay_us(struct nvinit *init, uint32_t us)
{
    if (init->ops->delay_us) init->ops->delay_us(init->ops->ctx, us);
}

/* ---- BIOS structures scripts refer to ----------------------------------------------- */
static uint32_t init_table_(struct nvinit *init, uint16_t offset, const char *name)
{
    uint8_t ver; uint16_t off, len;
    if (!nvbios_bit_entry(init->bios, 'I', &ver, &off, &len)) { warn("init data not found"); return 0; }
    if (len < offset + 2) { warn("init data too short for %s pointer", name); return 0; }
    uint32_t data = B16(off + offset);
    if (!data) warn("%s pointer invalid", name);
    return data;
}
#define init_script_table(i)            init_table_((i), 0x00, "script table")
#define init_macro_table(i)             init_table_((i), 0x04, "macro table")
#define init_condition_table(i)         init_table_((i), 0x06, "condition table")
#define init_io_condition_table(i)      init_table_((i), 0x08, "io condition table")
#define init_io_flag_condition_table(i) init_table_((i), 0x0a, "io flag condition table")
#define init_xlat_table(i)              init_table_((i), 0x10, "xlat table")

static uint8_t init_ram_restrict_group_count(struct nvinit *init)
{
    uint8_t ver; uint16_t off, len;
    if (nvbios_bit_entry(init->bios, 'M', &ver, &off, &len)) {
        if (ver == 1 && len >= 5) return B08(off + 2);
        if (ver == 2 && len >= 3) return B08(off + 0);
    }
    return 0;
}

static uint8_t init_ram_restrict(struct nvinit *init)
{
    if (!init->ramcfg || init->bios->version[0] < 0x70) {
        uint8_t strap = (uint8_t)((init->ops->rd32(init->ops->ctx, 0x101000) & 0x3c) >> 2);
        uint8_t ver; uint16_t off, len; uint32_t xlat = 0;
        if (nvbios_bit_entry(init->bios, 'M', &ver, &off, &len)) {
            if (ver == 1 && len >= 5) xlat = B16(off + 3);
            if (ver == 2 && len >= 3) xlat = B16(off + 1);
        }
        if (xlat) strap = B08(xlat + strap);
        init->ramcfg = 0x80000000u | strap;
    }
    return (uint8_t)(init->ramcfg & 0x7fffffffu);
}

static uint8_t init_xlat_(struct nvinit *init, uint8_t index, uint8_t offset)
{
    uint32_t table = init_xlat_table(init);
    if (table) {
        uint32_t data = B16(table + index * 2u);
        if (data) return B08(data + offset);
        warn("xlat table pointer %d invalid", index);
    }
    return 0;
}

static bool init_condition_met(struct nvinit *init, uint8_t cond)
{
    uint32_t table = init_condition_table(init);
    if (!table) return false;
    uint32_t reg = B32(table + cond * 12u + 0);
    uint32_t msk = B32(table + cond * 12u + 4);
    uint32_t val = B32(table + cond * 12u + 8);
    trace("\t[0x%02x] (R[0x%06x] & 0x%08x) == 0x%08x", cond, reg, msk, val);
    return (init_rd32(init, reg) & msk) == val;
}

static bool init_io_condition_met(struct nvinit *init, uint8_t cond)
{
    uint32_t table = init_io_condition_table(init);
    if (!table) return false;
    uint16_t port = B16(table + cond * 5u + 0);
    uint8_t index = B08(table + cond * 5u + 2);
    uint8_t mask  = B08(table + cond * 5u + 3);
    uint8_t value = B08(table + cond * 5u + 4);
    return (init_rdvgai(init, port, index) & mask) == value;
}

static bool init_io_flag_condition_met(struct nvinit *init, uint8_t cond)
{
    uint32_t table = init_io_flag_condition_table(init);
    if (!table) return false;
    uint16_t port  = B16(table + cond * 9u + 0);
    uint8_t  index = B08(table + cond * 9u + 2);
    uint8_t  mask  = B08(table + cond * 9u + 3);
    uint8_t  shift = B08(table + cond * 9u + 4);
    uint16_t data  = B16(table + cond * 9u + 5);
    uint8_t  dmask = B08(table + cond * 9u + 7);
    uint8_t  value = B08(table + cond * 9u + 8);
    uint8_t  ioval = (uint8_t)((init_rdvgai(init, port, index) & mask) >> shift);
    return (B08(data + ioval) & dmask) == value;
}

static inline uint32_t init_shift(uint32_t data, uint8_t shift)
{
    if (shift < 0x80) return data >> shift;
    return data << (0x100 - shift);
}

static int nvinit_exec(struct nvinit *init);

/* ======================================================================== */
/*  opcodes                                                                    */
/* ======================================================================== */
static void init_reserved(struct nvinit *init)
{
    uint8_t opcode = B08(init->offset);
    trace("RESERVED 0x%02x", opcode);
    init->offset += opcode == 0xaa ? 4 : 1;
}

static void init_done(struct nvinit *init) { trace("DONE"); init->offset = 0; }

static void init_io_restrict_prog(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t index = B08(init->offset + 3), mask = B08(init->offset + 4), shift = B08(init->offset + 5);
    uint8_t count = B08(init->offset + 6);
    uint32_t reg = B32(init->offset + 7);
    trace("IO_RESTRICT_PROG R[0x%06x] = ((0x%04x[0x%02x] & 0x%02x) >> %d)", reg, port, index, mask, shift);
    init->offset += 11;
    uint8_t conf = (uint8_t)((init_rdvgai(init, port, index) & mask) >> shift);
    for (uint8_t i = 0; i < count; i++) {
        uint32_t data = B32(init->offset);
        if (i == conf) init_wr32(init, reg, data);
        init->offset += 4;
    }
}

static void init_repeat(struct nvinit *init)
{
    uint8_t count = B08(init->offset + 1);
    uint32_t repeat = init->repeat;
    trace("REPEAT 0x%02x", count);
    init->offset += 2;
    init->repeat = init->offset;
    init->repend = init->offset;
    while (count--) {
        init->offset = init->repeat;
        if (nvinit_exec(init) < 0) return;
    }
    init->offset = init->repend;
    init->repeat = repeat;
}

static void init_io_restrict_pll(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t index = B08(init->offset + 3), mask = B08(init->offset + 4), shift = B08(init->offset + 5);
    int8_t  iofc  = (int8_t)B08(init->offset + 6);
    uint8_t count = B08(init->offset + 7);
    uint32_t reg  = B32(init->offset + 8);
    trace("IO_RESTRICT_PLL R[0x%06x]", reg);
    init->offset += 12;
    uint8_t conf = (uint8_t)((init_rdvgai(init, port, index) & mask) >> shift);
    for (uint8_t i = 0; i < count; i++) {
        uint32_t freq = B16(init->offset) * 10u;
        if (i == conf) {
            if (iofc > 0 && init_io_flag_condition_met(init, (uint8_t)iofc)) freq *= 2;
            init_prog_pll(init, reg, freq);
        }
        init->offset += 2;
    }
}

static void init_end_repeat(struct nvinit *init)
{
    trace("END_REPEAT");
    init->offset += 1;
    if (init->repeat) { init->repend = init->offset; init->offset = 0; }
}

static void init_copy(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1);
    uint8_t shift = B08(init->offset + 5), smask = B08(init->offset + 6);
    uint16_t port = B16(init->offset + 7);
    uint8_t index = B08(init->offset + 9), mask = B08(init->offset + 10);
    trace("COPY 0x%04x[0x%02x] from R[0x%06x]", port, index, reg);
    init->offset += 11;
    uint8_t data = init_rdvgai(init, port, index) & mask;
    data |= (uint8_t)(init_shift(init_rd32(init, reg), shift) & smask);
    init_wrvgai(init, port, index, data);
}

static void init_not(struct nvinit *init) { trace("NOT"); init->offset += 1; init_exec_inv(init); }

static void init_io_flag_condition(struct nvinit *init)
{
    uint8_t cond = B08(init->offset + 1);
    trace("IO_FLAG_CONDITION 0x%02x", cond);
    init->offset += 2;
    if (!init_io_flag_condition_met(init, cond)) init_exec_set(init, false);
}

static void init_generic_condition(struct nvinit *init)
{
    uint8_t cond = B08(init->offset + 1), size = B08(init->offset + 2);
    trace("GENERIC_CONDITION 0x%02x 0x%02x", cond, size);
    init->offset += 3;
    switch (cond) {
    case 0:                                         /* CONDITION_ID_INT_DP */
        if (init_conn(init) != DCB_CONNECTOR_eDP) init_exec_set(init, false);
        break;
    case 1: case 2: {                               /* CONDITION_ID_USE_SPPLL0/1 */
        struct nvbios_dpout info;
        if (init->outp && nvbios_dpout_match(init->bios, DCB_OUTPUT_DP,
                                             (uint16_t)(init->outp->or | init->outp->link << 6), &info)) {
            if (!(info.flags & cond)) init_exec_set(init, false);
            break;
        }
        if (init_exec(init)) warn("script needs dp output table data");
        break;
    }
    case 5:                                         /* CONDITION_ID_ASSR_SUPPORT */
        if (!(init_rdauxr(init, 0x0d) & 1)) init_exec_set(init, false);
        break;
    case 7:                                         /* CONDITION_ID_NO_PANEL_SEQ_DELAYS */
        init_exec_set(init, false);
        break;
    default:
        warn("INIT_GENERIC_CONDITION: unknown 0x%02x", cond);
        init->offset += size;
        break;
    }
}

static void init_io_mask_or(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1);
    uint8_t or = (uint8_t)init_or(init);
    trace("IO_MASK_OR 0x03d4[0x%02x] &= ~(1 << 0x%02x)", index, or);
    init->offset += 2;
    uint8_t data = init_rdvgai(init, 0x03d4, index);
    init_wrvgai(init, 0x03d4, index, (uint8_t)(data & ~(1u << or)));
}

static void init_io_or(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1);
    uint8_t or = (uint8_t)init_or(init);
    trace("IO_OR 0x03d4[0x%02x] |= (1 << 0x%02x)", index, or);
    init->offset += 2;
    uint8_t data = init_rdvgai(init, 0x03d4, index);
    init_wrvgai(init, 0x03d4, index, (uint8_t)(data | (1u << or)));
}

static void init_andn_reg(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), mask = B32(init->offset + 5);
    trace("ANDN_REG R[0x%06x] &= ~0x%08x", reg, mask);
    init->offset += 9;
    init_mask(init, reg, mask, 0);
}

static void init_or_reg(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), mask = B32(init->offset + 5);
    trace("OR_REG R[0x%06x] |= 0x%08x", reg, mask);
    init->offset += 9;
    init_mask(init, reg, 0, mask);
}

static void init_idx_addr_latched(struct nvinit *init)
{
    uint32_t creg = B32(init->offset + 1), dreg = B32(init->offset + 5);
    uint32_t mask = B32(init->offset + 9), data = B32(init->offset + 13);
    uint8_t count = B08(init->offset + 17);
    trace("INDEX_ADDRESS_LATCHED R[0x%06x] : R[0x%06x]", creg, dreg);
    init->offset += 18;
    while (count--) {
        uint8_t iaddr = B08(init->offset + 0), idata = B08(init->offset + 1);
        init->offset += 2;
        init_wr32(init, dreg, idata);
        init_mask(init, creg, ~mask, data | iaddr);
    }
}

static void init_io_restrict_pll2(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t index = B08(init->offset + 3), mask = B08(init->offset + 4), shift = B08(init->offset + 5);
    uint8_t count = B08(init->offset + 6);
    uint32_t reg = B32(init->offset + 7);
    trace("IO_RESTRICT_PLL2 R[0x%06x]", reg);
    init->offset += 11;
    uint8_t conf = (uint8_t)((init_rdvgai(init, port, index) & mask) >> shift);
    for (uint8_t i = 0; i < count; i++) {
        uint32_t freq = B32(init->offset);
        if (i == conf) init_prog_pll(init, reg, freq);
        init->offset += 4;
    }
}

static void init_pll2(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), freq = B32(init->offset + 5);
    trace("PLL2 R[0x%06x] =PLL= %ukHz", reg, freq);
    init->offset += 9;
    init_prog_pll(init, reg, freq);
}

/* I2C opcodes: decoded, not executed (no bit-banged I2C driver). A read
 * that fails behaves like nouveau's failed read: the write is skipped. */
static void i2c_skipped(struct nvinit *init, const char *name, uint8_t index, uint8_t addr)
{
    if (init_exec(init)) { init->skipped++; warn("%s on I2C bus 0x%02x addr 0x%02x skipped (no GPU I2C driver)", name, index, addr); }
}

static void init_i2c_byte(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1), addr = B08(init->offset + 2) >> 1, count = B08(init->offset + 3);
    i2c_skipped(init, "I2C_BYTE", index, addr);
    init->offset += 4 + 3u * count;
}

static void init_zm_i2c_byte(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1), addr = B08(init->offset + 2) >> 1, count = B08(init->offset + 3);
    i2c_skipped(init, "ZM_I2C_BYTE", index, addr);
    init->offset += 4 + 2u * count;
}

static void init_zm_i2c(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1), addr = B08(init->offset + 2) >> 1, count = B08(init->offset + 3);
    i2c_skipped(init, "ZM_I2C", index, addr);
    init->offset += 4u + count;
}

static void init_tmds(struct nvinit *init)
{
    if (init_exec(init)) { init->skipped++; warn("TMDS (pre-NV50) skipped"); }
    init->offset += 5;
}

static void init_zm_tmds_group(struct nvinit *init)
{
    uint8_t count = B08(init->offset + 2);
    if (init_exec(init)) { init->skipped++; warn("TMDS_ZM_GROUP (pre-NV50) skipped"); }
    init->offset += 3 + 2u * count;
}

static void init_cr_idx_adr_latch(struct nvinit *init)
{
    uint8_t addr0 = B08(init->offset + 1), addr1 = B08(init->offset + 2);
    uint8_t base = B08(init->offset + 3), count = B08(init->offset + 4);
    trace("CR_INDEX_ADDR C[%02x] C[%02x]", addr0, addr1);
    init->offset += 5;
    uint8_t save0 = init_rdvgai(init, 0x03d4, addr0);
    while (count--) {
        uint8_t data = B08(init->offset);
        init->offset += 1;
        init_wrvgai(init, 0x03d4, addr0, base++);
        init_wrvgai(init, 0x03d4, addr1, data);
    }
    init_wrvgai(init, 0x03d4, addr0, save0);
}

static void init_cr(struct nvinit *init)
{
    uint8_t addr = B08(init->offset + 1), mask = B08(init->offset + 2), data = B08(init->offset + 3);
    trace("CR C[0x%02x] &= 0x%02x |= 0x%02x", addr, mask, data);
    init->offset += 4;
    uint8_t val = init_rdvgai(init, 0x03d4, addr) & mask;
    init_wrvgai(init, 0x03d4, addr, val | data);
}

static void init_zm_cr(struct nvinit *init)
{
    uint8_t addr = B08(init->offset + 1), data = B08(init->offset + 2);
    trace("ZM_CR C[0x%02x] = 0x%02x", addr, data);
    init->offset += 3;
    init_wrvgai(init, 0x03d4, addr, data);
}

static void init_zm_cr_group(struct nvinit *init)
{
    uint8_t count = B08(init->offset + 1);
    trace("ZM_CR_GROUP");
    init->offset += 2;
    while (count--) {
        uint8_t addr = B08(init->offset + 0), data = B08(init->offset + 1);
        init->offset += 2;
        init_wrvgai(init, 0x03d4, addr, data);
    }
}

static void init_condition_time(struct nvinit *init)
{
    uint8_t cond = B08(init->offset + 1), retry = B08(init->offset + 2);
    uint8_t wait = (uint8_t)(retry * 50u < 100 ? retry * 50u : 100);
    trace("CONDITION_TIME 0x%02x 0x%02x", cond, retry);
    init->offset += 3;
    if (!init_exec(init)) return;
    while (wait--) {
        if (init_condition_met(init, cond)) return;
        init_delay_us(init, 20000);
    }
    init_exec_set(init, false);
}

static void init_ltime(struct nvinit *init)
{
    uint16_t msec = B16(init->offset + 1);
    trace("LTIME 0x%04x", msec);
    init->offset += 3;
    if (init_exec(init)) init_delay_us(init, msec * 1000u);
}

static void init_zm_reg_sequence(struct nvinit *init)
{
    uint32_t base = B32(init->offset + 1);
    uint8_t count = B08(init->offset + 5);
    trace("ZM_REG_SEQUENCE 0x%02x", count);
    init->offset += 6;
    while (count--) {
        uint32_t data = B32(init->offset);
        init->offset += 4;
        init_wr32(init, base, data);
        base += 4;
    }
}

static void init_pll_indirect(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1);
    uint16_t addr = B16(init->offset + 5);
    uint32_t freq = (uint32_t)B16(addr) * 1000;
    trace("PLL_INDIRECT R[0x%06x] =PLL= VBIOS[%04x] = %ukHz", reg, addr, freq);
    init->offset += 7;
    init_prog_pll(init, reg, freq);
}

static void init_zm_reg_indirect(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1);
    uint16_t addr = B16(init->offset + 5);
    uint32_t data = B32(addr);
    trace("ZM_REG_INDIRECT R[0x%06x] = VBIOS[0x%04x] = 0x%08x", reg, addr, data);
    init->offset += 7;
    init_wr32(init, addr, data);                    /* sic: nouveau writes to `addr` */
}

static void init_sub_direct(struct nvinit *init)
{
    uint16_t addr = B16(init->offset + 1);
    trace("SUB_DIRECT 0x%04x", addr);
    if (init_exec(init)) {
        uint32_t save = init->offset;
        init->offset = addr;
        if (nvinit_exec(init) < 0) { error("error parsing sub-table"); return; }
        init->offset = save;
    }
    init->offset += 3;
}

static void init_jump(struct nvinit *init)
{
    uint16_t offset = B16(init->offset + 1);
    trace("JUMP 0x%04x", offset);
    if (init_exec(init)) init->offset = offset;
    else init->offset += 3;
}

static void init_i2c_if(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1), addr = B08(init->offset + 2);
    uint8_t mask = B08(init->offset + 4), data = B08(init->offset + 5);
    init->offset += 6;
    init_exec_force(init, true);
    i2c_skipped(init, "I2C_IF", index, addr);
    if ((0 & mask) != data) init_exec_set(init, false);     /* no I2C: treat the byte read as 0 */
    init_exec_force(init, false);
}

static void init_copy_nv_reg(struct nvinit *init)
{
    uint32_t sreg = B32(init->offset + 1);
    uint8_t shift = B08(init->offset + 5);
    uint32_t smask = B32(init->offset + 6), sxor = B32(init->offset + 10);
    uint32_t dreg = B32(init->offset + 14), dmask = B32(init->offset + 18);
    trace("COPY_NV_REG R[0x%06x] from R[0x%06x]", dreg, sreg);
    init->offset += 22;
    uint32_t data = init_shift(init_rd32(init, sreg), shift);
    init_mask(init, dreg, ~dmask, (data & smask) ^ sxor);
}

static void init_zm_index_io(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t index = B08(init->offset + 3), data = B08(init->offset + 4);
    trace("ZM_INDEX_IO I[0x%04x][0x%02x] = 0x%02x", port, index, data);
    init->offset += 5;
    init_wrvgai(init, port, index, data);
}

static void init_compute_mem(struct nvinit *init)
{
    init->offset += 1;
    init_exec_force(init, true);
    if (init_exec(init)) { init->skipped++; warn("COMPUTE_MEM skipped (memory is set up by the firmware)"); }
    init_exec_force(init, false);
}

static void init_reset(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), data1 = B32(init->offset + 5), data2 = B32(init->offset + 9);
    trace("RESET R[0x%08x] = 0x%08x, 0x%08x", reg, data1, data2);
    init->offset += 13;
    init_exec_force(init, true);
    uint32_t savepci19 = init_mask(init, 0x00184c, 0x00000f00, 0x00000000);
    init_wr32(init, reg, data1);
    init_delay_us(init, 10);
    init_wr32(init, reg, data2);
    init_wr32(init, 0x00184c, savepci19);
    init_mask(init, 0x001850, 0x00000001, 0x00000000);
    init_exec_force(init, false);
}

/* CONFIGURE_MEM / CLK / PREINIT: BMP-era (version < 3), "done" on NV50+. */
static void init_configure_old(struct nvinit *init)
{
    init->offset += 1;
    if (init->bios->version[0] > 2) { init_done(init); return; }
    if (init_exec(init)) { init->skipped++; warn("BMP-era CONFIGURE opcode skipped"); }
}

static void init_io(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t mask = (uint8_t)B16(init->offset + 3), data = (uint8_t)B16(init->offset + 4);
    trace("IO I[0x%04x] &= 0x%02x |= 0x%02x", port, mask, data);
    init->offset += 5;
    if (port == 0x03c3 && data == 0x01) {           /* nouveau's NV50+ special case */
        init_mask(init, 0x614100, 0xf0800000, 0x00800000);
        init_mask(init, 0x00e18c, 0x00020000, 0x00020000);
        init_mask(init, 0x614900, 0xf0800000, 0x00800000);
        init_mask(init, 0x000200, 0x40000000, 0x00000000);
        if (init_exec(init)) init_delay_us(init, 10000);
        init_mask(init, 0x00e18c, 0x00020000, 0x00000000);
        init_mask(init, 0x000200, 0x40000000, 0x40000000);
        init_wr32(init, 0x614100, 0x00800018);
        init_wr32(init, 0x614900, 0x00800018);
        if (init_exec(init)) init_delay_us(init, 10000);
        init_wr32(init, 0x614100, 0x10000018);
        init_wr32(init, 0x614900, 0x10000018);
    }
    uint8_t value = init_rdport(init, port) & mask;
    init_wrport(init, port, (uint8_t)(data | value));
}

static void init_sub(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1);
    trace("SUB 0x%02x", index);
    uint32_t table = init_script_table(init), addr = table ? B16(table + index * 2u) : 0;
    if (addr && init_exec(init)) {
        uint32_t save = init->offset;
        init->offset = addr;
        if (nvinit_exec(init) < 0) { error("error parsing sub-table"); return; }
        init->offset = save;
    }
    init->offset += 2;
}

static void init_ram_condition(struct nvinit *init)
{
    uint8_t mask = B08(init->offset + 1), value = B08(init->offset + 2);
    trace("RAM_CONDITION (R[0x100000] & 0x%02x) == 0x%02x", mask, value);
    init->offset += 3;
    if ((init_rd32(init, 0x100000) & mask) != value) init_exec_set(init, false);
}

static void init_nv_reg(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), mask = B32(init->offset + 5), data = B32(init->offset + 9);
    trace("NV_REG R[0x%06x] &= 0x%08x |= 0x%08x", reg, mask, data);
    init->offset += 13;
    init_mask(init, reg, ~mask, data);
}

static void init_macro(struct nvinit *init)
{
    uint8_t macro = B08(init->offset + 1);
    trace("MACRO 0x%02x", macro);
    uint32_t table = init_macro_table(init);
    if (table) {
        uint32_t addr = B32(table + macro * 8u + 0), data = B32(table + macro * 8u + 4);
        init_wr32(init, addr, data);
    }
    init->offset += 2;
}

static void init_resume(struct nvinit *init) { trace("RESUME"); init->offset += 1; init_exec_set(init, true); }

static void init_strap_condition(struct nvinit *init)
{
    uint32_t mask = B32(init->offset + 1), value = B32(init->offset + 5);
    trace("STRAP_CONDITION (R[0x101000] & 0x%08x) == 0x%08x", mask, value);
    init->offset += 9;
    if ((init_rd32(init, 0x101000) & mask) != value) init_exec_set(init, false);
}

static void init_time(struct nvinit *init)
{
    uint16_t usec = B16(init->offset + 1);
    trace("TIME 0x%04x", usec);
    init->offset += 3;
    if (init_exec(init)) init_delay_us(init, usec < 1000 ? usec : ((usec + 900u) / 1000u) * 1000u);
}

static void init_condition(struct nvinit *init)
{
    uint8_t cond = B08(init->offset + 1);
    trace("CONDITION 0x%02x", cond);
    init->offset += 2;
    if (!init_condition_met(init, cond)) init_exec_set(init, false);
}

static void init_io_condition(struct nvinit *init)
{
    uint8_t cond = B08(init->offset + 1);
    trace("IO_CONDITION 0x%02x", cond);
    init->offset += 2;
    if (!init_io_condition_met(init, cond)) init_exec_set(init, false);
}

static void init_zm_reg16(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1);
    uint16_t data = B16(init->offset + 5);
    trace("ZM_REG R[0x%06x] = 0x%04x", addr, data);
    init->offset += 7;
    init_wr32(init, addr, data);
}

static void init_index_io(struct nvinit *init)
{
    uint16_t port = B16(init->offset + 1);
    uint8_t index = (uint8_t)B16(init->offset + 3), mask = B08(init->offset + 4), data = B08(init->offset + 5);
    trace("INDEX_IO I[0x%04x][0x%02x] &= 0x%02x |= 0x%02x", port, index, mask, data);
    init->offset += 6;
    uint8_t value = init_rdvgai(init, port, index) & mask;
    init_wrvgai(init, port, index, data | value);
}

static void init_pll(struct nvinit *init)
{
    uint32_t reg = B32(init->offset + 1), freq = B16(init->offset + 5) * 10u;
    trace("PLL R[0x%06x] =PLL= %ukHz", reg, freq);
    init->offset += 7;
    init_prog_pll(init, reg, freq);
}

static void init_zm_reg(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1), data = B32(init->offset + 5);
    trace("ZM_REG R[0x%06x] = 0x%08x", addr, data);
    init->offset += 9;
    if (addr == 0x000200) data |= 0x00000001;
    init_wr32(init, addr, data);
}

static void init_ram_restrict_pll(struct nvinit *init)
{
    uint8_t type = B08(init->offset + 1);
    uint8_t count = init_ram_restrict_group_count(init);
    uint8_t strap = init_ram_restrict(init);
    trace("RAM_RESTRICT_PLL 0x%02x", type);
    init->offset += 2;
    for (uint8_t cconf = 0; cconf < count; cconf++) {
        uint32_t freq = B32(init->offset);
        if (cconf == strap) init_prog_pll(init, type, freq);
        init->offset += 4;
    }
}

static void init_reset_begun(struct nvinit *init) { trace("RESET_BEGUN"); init->offset += 1; }
static void init_reset_end(struct nvinit *init) { trace("RESET_END"); init->offset += 1; }

static void init_gpio(struct nvinit *init)
{
    init->offset += 1;
    if (init_exec(init)) { init->skipped++; warn("GPIO reset skipped (no GPU GPIO driver)"); }
}

static void init_ram_restrict_zm_reg_group(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1);
    uint8_t incr = B08(init->offset + 5), num = B08(init->offset + 6);
    uint8_t count = init_ram_restrict_group_count(init);
    uint8_t index = init_ram_restrict(init);
    trace("RAM_RESTRICT_ZM_REG_GROUP R[0x%08x] 0x%02x 0x%02x", addr, incr, num);
    init->offset += 7;
    for (uint8_t i = 0; i < num; i++) {
        for (uint8_t j = 0; j < count; j++) {
            uint32_t data = B32(init->offset);
            if (j == index) init_wr32(init, addr, data);
            init->offset += 4;
        }
        addr += incr;
    }
}

static void init_copy_zm_reg(struct nvinit *init)
{
    uint32_t sreg = B32(init->offset + 1), dreg = B32(init->offset + 5);
    trace("COPY_ZM_REG R[0x%06x] = R[0x%06x]", dreg, sreg);
    init->offset += 9;
    init_wr32(init, dreg, init_rd32(init, sreg));
}

static void init_zm_reg_group(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1);
    uint8_t count = B08(init->offset + 5);
    trace("ZM_REG_GROUP R[0x%06x] =", addr);
    init->offset += 6;
    while (count--) {
        uint32_t data = B32(init->offset);
        init_wr32(init, addr, data);
        init->offset += 4;
    }
}

static void init_xlat(struct nvinit *init)
{
    uint32_t saddr = B32(init->offset + 1);
    uint8_t sshift = B08(init->offset + 5), smask = B08(init->offset + 6), index = B08(init->offset + 7);
    uint32_t daddr = B32(init->offset + 8), dmask = B32(init->offset + 12);
    uint8_t shift = B08(init->offset + 16);
    trace("INIT_XLAT R[0x%06x] from R[0x%06x]", daddr, saddr);
    init->offset += 17;
    uint32_t data = init_shift(init_rd32(init, saddr), sshift) & smask;
    data = (uint32_t)init_xlat_(init, index, (uint8_t)data) << shift;
    init_mask(init, daddr, ~dmask, data);
}

static void init_zm_mask_add(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1), mask = B32(init->offset + 5), add = B32(init->offset + 9);
    trace("ZM_MASK_ADD R[0x%06x] &= 0x%08x += 0x%08x", addr, mask, add);
    init->offset += 13;
    uint32_t data = init_rd32(init, addr);
    data = (data & mask) | ((data + add) & ~mask);
    init_wr32(init, addr, data);
}

static void init_auxch(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1);
    uint8_t count = B08(init->offset + 5);
    trace("AUXCH AUX[0x%08x] 0x%02x", addr, count);
    init->offset += 6;
    while (count--) {
        uint8_t mask = B08(init->offset + 0), data = B08(init->offset + 1);
        mask = init_rdauxr(init, addr) & mask;
        init_wrauxr(init, addr, mask | data);
        init->offset += 2;
    }
}

static void init_zm_auxch(struct nvinit *init)
{
    uint32_t addr = B32(init->offset + 1);
    uint8_t count = B08(init->offset + 5);
    trace("ZM_AUXCH AUX[0x%08x] 0x%02x", addr, count);
    init->offset += 6;
    while (count--) {
        uint8_t data = B08(init->offset + 0);
        init_wrauxr(init, addr, data);
        init->offset += 1;
    }
}

static void init_i2c_long_if(struct nvinit *init)
{
    uint8_t index = B08(init->offset + 1), addr = B08(init->offset + 2) >> 1;
    init->offset += 7;
    i2c_skipped(init, "I2C_LONG_IF", index, addr);
    init_exec_set(init, false);                     /* nouveau: no adapter -> condition false */
}

static void init_gpio_ne(struct nvinit *init)
{
    uint8_t count = B08(init->offset + 1);
    init->offset += 2;
    if (init_exec(init)) { init->skipped++; warn("GPIO_NE skipped (no GPU GPIO driver)"); }
    init->offset += count;
}

typedef void (*opfn)(struct nvinit *);
static const opfn init_opcode[256] = {
    [0x32] = init_io_restrict_prog,  [0x33] = init_repeat,            [0x34] = init_io_restrict_pll,
    [0x36] = init_end_repeat,        [0x37] = init_copy,              [0x38] = init_not,
    [0x39] = init_io_flag_condition, [0x3a] = init_generic_condition, [0x3b] = init_io_mask_or,
    [0x3c] = init_io_or,             [0x47] = init_andn_reg,          [0x48] = init_or_reg,
    [0x49] = init_idx_addr_latched,  [0x4a] = init_io_restrict_pll2,  [0x4b] = init_pll2,
    [0x4c] = init_i2c_byte,          [0x4d] = init_zm_i2c_byte,       [0x4e] = init_zm_i2c,
    [0x4f] = init_tmds,              [0x50] = init_zm_tmds_group,     [0x51] = init_cr_idx_adr_latch,
    [0x52] = init_cr,                [0x53] = init_zm_cr,             [0x54] = init_zm_cr_group,
    [0x56] = init_condition_time,    [0x57] = init_ltime,             [0x58] = init_zm_reg_sequence,
    [0x59] = init_pll_indirect,      [0x5a] = init_zm_reg_indirect,   [0x5b] = init_sub_direct,
    [0x5c] = init_jump,              [0x5e] = init_i2c_if,            [0x5f] = init_copy_nv_reg,
    [0x62] = init_zm_index_io,       [0x63] = init_compute_mem,       [0x65] = init_reset,
    [0x66] = init_configure_old,     [0x67] = init_configure_old,     [0x68] = init_configure_old,
    [0x69] = init_io,                [0x6b] = init_sub,               [0x6d] = init_ram_condition,
    [0x6e] = init_nv_reg,            [0x6f] = init_macro,             [0x71] = init_done,
    [0x72] = init_resume,            [0x73] = init_strap_condition,   [0x74] = init_time,
    [0x75] = init_condition,         [0x76] = init_io_condition,      [0x77] = init_zm_reg16,
    [0x78] = init_index_io,          [0x79] = init_pll,               [0x7a] = init_zm_reg,
    [0x87] = init_ram_restrict_pll,  [0x8c] = init_reset_begun,       [0x8d] = init_reset_end,
    [0x8e] = init_gpio,              [0x8f] = init_ram_restrict_zm_reg_group,
    [0x90] = init_copy_zm_reg,       [0x91] = init_zm_reg_group,      [0x92] = init_reserved,
    [0x96] = init_xlat,              [0x97] = init_zm_mask_add,       [0x98] = init_auxch,
    [0x99] = init_zm_auxch,          [0x9a] = init_i2c_long_if,       [0xa9] = init_gpio_ne,
    [0xaa] = init_reserved,
};

/* nouveau nvbios_exec(), plus a runaway guard (a corrupt or misparsed
 * script must not hang the boot). */
static int nvinit_exec(struct nvinit *init)
{
    if (init->nested >= MAX_NESTING) { error("scripts nested too deeply"); return -1; }
    init->nested++;
    while (init->offset) {
        uint8_t opcode = B08(init->offset);
        if (!init_opcode[opcode]) { error("unknown opcode 0x%02x", opcode); init->nested--; return -1; }
        if (++init->opcodes > MAX_OPCODES) { error("more than %d opcodes, giving up", MAX_OPCODES); init->nested--; return -1; }
        init_opcode[opcode](init);
    }
    init->nested--;
    return 0;
}

int nvinit_run(const struct nvbios *bios, const struct nvinit_ops *ops, uint32_t offset,
               const struct nvbios_output *outp, int or, int link, int head, bool trace,
               struct nvinit *stats_out)
{
    struct nvinit init = {
        .bios = bios, .ops = ops, .offset = offset, .outp = outp,
        .or = or, .link = link, .head = head, .execute = 1, .trace = trace,
    };
    int ret = offset ? nvinit_exec(&init) : 0;
    if (stats_out) *stats_out = init;
    return ret;
}
