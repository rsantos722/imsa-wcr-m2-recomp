// clt_registers.h — Triangle Engine command-list register and field facts.
//
// Provenance: this file defines the numeric facts needed to emit the CLT words
// used by the font path. The values are interface facts described by the
// public M2 Toolkit command-list definition (cltmacros.h), limited to the
// registers and fields this path uses, and checked against the command words
// runtime/src/font_folio.c emits. The table is authored in this project and
// records only the facts the implementation needs.
//
// The register/field numbers are the M2 Triangle Engine wire format. Field
// shifts use the PowerPC bit-number convention: bit 0 is the most
// significant bit, so shift = 31 - bit.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register identifiers (RA_* in the toolkit vocabulary).
enum {
    M2_CLT_REG_DBALUCNTL = 0x8070,
    M2_CLT_REG_DBAMULTCNTL = 0x8058,
    M2_CLT_REG_DBBMULTCNTL = 0x8064,
    M2_CLT_REG_DBCONSTIN = 0x8054,
    M2_CLT_REG_DBDESTALPHACNTL = 0x8078,
    M2_CLT_REG_DBDISCARDCONTROL = 0x800c,
    M2_CLT_REG_DBSRCCNTL = 0x8030,
    M2_CLT_REG_DBSSBDSBCNTL = 0x8050,
    M2_CLT_REG_DBUSERCONTROL = 0x8008,
    M2_CLT_REG_DCNTL = 0x0014,
    M2_CLT_REG_DCNTLDATA = 0x0010,
    M2_CLT_REG_TXTADDRCNTL = 0x6408,
    M2_CLT_REG_TXTCONST0 = 0x6440,
    M2_CLT_REG_TXTCOUNT = 0x6428,
    M2_CLT_REG_TXTEXPTYPE = 0x643c,
    M2_CLT_REG_TXTLDCNTL = 0x6404,
    M2_CLT_REG_TXTLDSRCADDR = 0x6424,
    M2_CLT_REG_TXTLODBASE0 = 0x6414,
    M2_CLT_REG_TXTPIPCNTL = 0x640c,
    M2_CLT_REG_TXTTABCNTL = 0x6410,
    M2_CLT_REG_TXTUVMASK = 0x6430,
    M2_CLT_REG_TXTUVMAX = 0x642c,
};

typedef struct {
    uint16_t reg;
    uint8_t shift;
    uint32_t mask;
} m2_clt_field_def;

// Field identifiers, one per (register, field) pair used by this project.
enum {
    M2_CLT_FIELD_DBAMULTCNTL_AINPUTSELECT = 0,
    M2_CLT_FIELD_DBAMULTCNTL_AMULTCOEFSELECT = 1,
    M2_CLT_FIELD_DBAMULTCNTL_AMULTCONSTCONTROL = 2,
    M2_CLT_FIELD_DBAMULTCNTL_AMULTRJUSTIFY = 3,
    M2_CLT_FIELD_DBBMULTCNTL_BINPUTSELECT = 4,
    M2_CLT_FIELD_DBBMULTCNTL_BMULTCOEFSELECT = 5,
    M2_CLT_FIELD_DBDESTALPHACNTL_DESTCONSTSELECT = 6,
    M2_CLT_FIELD_DBDISCARDCONTROL_ALPHA0 = 7,
    M2_CLT_FIELD_DBSRCCNTL_SRC32BPP = 8,
    M2_CLT_FIELD_DBSRCCNTL_SRCMSBREP = 9,
    M2_CLT_FIELD_DBUSERCONTROL_BLENDEN = 10,
    M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK = 11,
    M2_CLT_FIELD_DBUSERCONTROL_DITHEREN = 12,
    M2_CLT_FIELD_DBUSERCONTROL_SRCEN = 13,
    M2_CLT_FIELD_DBUSERCONTROL_WINCLIPINEN = 14,
    M2_CLT_FIELD_DBUSERCONTROL_WINCLIPOUTEN = 15,
    M2_CLT_FIELD_DBUSERCONTROL_ZBUFFEN = 16,
    M2_CLT_FIELD_DBUSERCONTROL_ZOUTEN = 17,
    M2_CLT_FIELD_DCNTL_JA = 18,
    M2_CLT_FIELD_DCNTL_SYNC = 19,
    M2_CLT_FIELD_DCNTL_TLD = 20,
    M2_CLT_FIELD_TRIANGLE_COUNT = 21,
    M2_CLT_FIELD_TRIANGLE_NEW = 22,
    M2_CLT_FIELD_TRIANGLE_PERSPECTIVE = 23,
    M2_CLT_FIELD_TRIANGLE_SHADING = 24,
    M2_CLT_FIELD_TRIANGLE_STRIPFAN = 25,
    M2_CLT_FIELD_TRIANGLE_TEXTURE = 26,
    M2_CLT_FIELD_TXTADDRCNTL_INTERFILTER = 27,
    M2_CLT_FIELD_TXTADDRCNTL_LODMAX = 28,
    M2_CLT_FIELD_TXTADDRCNTL_MAGFILTER = 29,
    M2_CLT_FIELD_TXTADDRCNTL_MINFILTER = 30,
    M2_CLT_FIELD_TXTADDRCNTL_TEXTUREENABLE = 31,
    M2_CLT_FIELD_TXTEXPTYPE_ADEPTH = 32,
    M2_CLT_FIELD_TXTEXPTYPE_HASALPHA = 33,
    M2_CLT_FIELD_TXTLDCNTL_LOADMODE = 34,
    M2_CLT_FIELD_TXTPIPCNTL_PIPALPHASELECT = 35,
    M2_CLT_FIELD_TXTPIPCNTL_PIPCOLORSELECT = 36,
    M2_CLT_FIELD_TXTPIPCNTL_PIPINDEXOFFSET = 37,
    M2_CLT_FIELD_TXTPIPCNTL_PIPSSBSELECT = 38,
    M2_CLT_FIELD_TXTTABCNTL_ALPHAOUT = 39,
    M2_CLT_FIELD_TXTTABCNTL_BLENDOP = 40,
    M2_CLT_FIELD_TXTTABCNTL_COLOROUT = 41,
    M2_CLT_FIELD_TXTTABCNTL_FIRSTALPHA = 42,
    M2_CLT_FIELD_TXTTABCNTL_FIRSTCOLOR = 43,
    M2_CLT_FIELD_TXTTABCNTL_SECONDALPHA = 44,
    M2_CLT_FIELD_TXTTABCNTL_SECONDCOLOR = 45,
    M2_CLT_FIELD_TXTTABCNTL_THIRDCOLOR = 46,
    M2_CLT_FIELD_TXTUVMASK_UMASK = 47,
    M2_CLT_FIELD_TXTUVMASK_VMASK = 48,
    M2_CLT_FIELD_TXTUVMAX_UMAX = 49,
    M2_CLT_FIELD_TXTUVMAX_VMAX = 50,
    M2_CLT_FIELD_COUNT
};

// Rows are in M2_CLT_FIELD_* order. Keep this order aligned with the enum so
// the helper functions index the intended field.
static const m2_clt_field_def m2_clt_field_defs[M2_CLT_FIELD_COUNT] = {
    { M2_CLT_REG_DBAMULTCNTL, 6, 0x000000c0u },  // DBAMULTCNTL.AINPUTSELECT
    { M2_CLT_REG_DBAMULTCNTL, 1, 0x00000032u },  // DBAMULTCNTL.AMULTCOEFSELECT
    { M2_CLT_REG_DBAMULTCNTL, 2, 0x0000000cu },  // DBAMULTCNTL.AMULTCONSTCONTROL
    { M2_CLT_REG_DBAMULTCNTL, 0, 0x00000001u },  // DBAMULTCNTL.AMULTRJUSTIFY
    { M2_CLT_REG_DBBMULTCNTL, 6, 0x000000c0u },  // DBBMULTCNTL.BINPUTSELECT
    { M2_CLT_REG_DBBMULTCNTL, 1, 0x00000032u },  // DBBMULTCNTL.BMULTCOEFSELECT
    { M2_CLT_REG_DBDESTALPHACNTL, 0, 0x0000000fu },  // DBDESTALPHACNTL.DESTCONSTSELECT
    { M2_CLT_REG_DBDISCARDCONTROL, 0, 0x00000001u },  // DBDISCARDCONTROL.ALPHA0
    { M2_CLT_REG_DBSRCCNTL, 0, 0x00000001u },  // DBSRCCNTL.SRC32BPP
    { M2_CLT_REG_DBSRCCNTL, 1, 0x00000002u },  // DBSRCCNTL.SRCMSBREP
    { M2_CLT_REG_DBUSERCONTROL, 6, 0x00000040u },  // DBUSERCONTROL.BLENDEN
    { M2_CLT_REG_DBUSERCONTROL, 0, 0x0000000fu },  // DBUSERCONTROL.DESTOUTMASK
    { M2_CLT_REG_DBUSERCONTROL, 4, 0x00000010u },  // DBUSERCONTROL.DITHEREN
    { M2_CLT_REG_DBUSERCONTROL, 5, 0x00000020u },  // DBUSERCONTROL.SRCEN
    { M2_CLT_REG_DBUSERCONTROL, 8, 0x00000100u },  // DBUSERCONTROL.WINCLIPINEN
    { M2_CLT_REG_DBUSERCONTROL, 7, 0x00000080u },  // DBUSERCONTROL.WINCLIPOUTEN
    { M2_CLT_REG_DBUSERCONTROL, 10, 0x00000400u },  // DBUSERCONTROL.ZBUFFEN
    { M2_CLT_REG_DBUSERCONTROL, 9, 0x00000200u },  // DBUSERCONTROL.ZOUTEN
    { M2_CLT_REG_DCNTL, 1, 0x00000002u },  // DCNTL.JA
    { M2_CLT_REG_DCNTL, 5, 0x00000020u },  // DCNTL.SYNC
    { M2_CLT_REG_DCNTL, 0, 0x00000001u },  // DCNTL.TLD
    { 0x0000, 0, 0x0000ffffu },  // TRIANGLE.COUNT
    { 0x0000, 19, 0x00080000u },  // TRIANGLE.NEW
    { 0x0000, 18, 0x00040000u },  // TRIANGLE.PERSPECTIVE
    { 0x0000, 16, 0x00010000u },  // TRIANGLE.SHADING
    { 0x0000, 20, 0x00100000u },  // TRIANGLE.STRIPFAN
    { 0x0000, 17, 0x00020000u },  // TRIANGLE.TEXTURE
    { M2_CLT_REG_TXTADDRCNTL, 7, 0x00000380u },  // TXTADDRCNTL.INTERFILTER
    { M2_CLT_REG_TXTADDRCNTL, 0, 0x0000000fu },  // TXTADDRCNTL.LODMAX
    { M2_CLT_REG_TXTADDRCNTL, 4, 0x00000070u },  // TXTADDRCNTL.MAGFILTER
    { M2_CLT_REG_TXTADDRCNTL, 10, 0x00001c00u },  // TXTADDRCNTL.MINFILTER
    { M2_CLT_REG_TXTADDRCNTL, 13, 0x00002000u },  // TXTADDRCNTL.TEXTUREENABLE
    { M2_CLT_REG_TXTEXPTYPE, 4, 0x000000f0u },  // TXTEXPTYPE.ADEPTH
    { M2_CLT_REG_TXTEXPTYPE, 11, 0x00000800u },  // TXTEXPTYPE.HASALPHA
    { M2_CLT_REG_TXTLDCNTL, 8, 0x00000300u },  // TXTLDCNTL.LOADMODE
    { M2_CLT_REG_TXTPIPCNTL, 11, 0x00003800u },  // TXTPIPCNTL.PIPALPHASELECT
    { M2_CLT_REG_TXTPIPCNTL, 8, 0x00000700u },  // TXTPIPCNTL.PIPCOLORSELECT
    { M2_CLT_REG_TXTPIPCNTL, 0, 0x000000ffu },  // TXTPIPCNTL.PIPINDEXOFFSET
    { M2_CLT_REG_TXTPIPCNTL, 14, 0x0001c000u },  // TXTPIPCNTL.PIPSSBSELECT
    { M2_CLT_REG_TXTTABCNTL, 15, 0x00018000u },  // TXTTABCNTL.ALPHAOUT
    { M2_CLT_REG_TXTTABCNTL, 17, 0x00020000u },  // TXTTABCNTL.BLENDOP
    { M2_CLT_REG_TXTTABCNTL, 9, 0x00000600u },  // TXTTABCNTL.COLOROUT
    { M2_CLT_REG_TXTTABCNTL, 11, 0x00001800u },  // TXTTABCNTL.FIRSTALPHA
    { M2_CLT_REG_TXTTABCNTL, 0, 0x00000007u },  // TXTTABCNTL.FIRSTCOLOR
    { M2_CLT_REG_TXTTABCNTL, 13, 0x00006000u },  // TXTTABCNTL.SECONDALPHA
    { M2_CLT_REG_TXTTABCNTL, 3, 0x00000038u },  // TXTTABCNTL.SECONDCOLOR
    { M2_CLT_REG_TXTTABCNTL, 6, 0x000001c0u },  // TXTTABCNTL.THIRDCOLOR
    { M2_CLT_REG_TXTUVMASK, 16, 0x03ff0000u },  // TXTUVMASK.UMASK
    { M2_CLT_REG_TXTUVMASK, 0, 0x000003ffu },  // TXTUVMASK.VMASK
    { M2_CLT_REG_TXTUVMAX, 16, 0x03ff0000u },  // TXTUVMAX.UMAX
    { M2_CLT_REG_TXTUVMAX, 0, 0x000003ffu },  // TXTUVMAX.VMAX
};

// Constant values (RC_* in the toolkit vocabulary), named by field.
enum {
    M2_CLT_CONST_DBAMULTCNTL_AINPUTSELECT_TEXCOLOR = 0x0,
    M2_CLT_CONST_DBAMULTCNTL_AMULTCOEFSELECT_TEXALPHA = 0x0,
    M2_CLT_CONST_DBAMULTCNTL_AMULTCONSTCONTROL_TEXSSB = 0x0,
    M2_CLT_CONST_DBBMULTCNTL_BINPUTSELECT_SRCCOLOR = 0x0,
    M2_CLT_CONST_DBBMULTCNTL_BMULTCOEFSELECT_TEXALPHACOMPLEMENT = 0x1,
    M2_CLT_CONST_DBDESTALPHACNTL_DESTCONSTSELECT_TEXALPHA = 0x0,
    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_ALPHA = 0x8,
    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_BLUE = 0x1,
    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_GREEN = 0x2,
    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_RED = 0x4,
    M2_CLT_CONST_TXTADDRCNTL_INTERFILTER_POINT = 0x0,
    M2_CLT_CONST_TXTADDRCNTL_MAGFILTER_POINT = 0x0,
    M2_CLT_CONST_TXTADDRCNTL_MINFILTER_POINT = 0x0,
    M2_CLT_CONST_TXTLDCNTL_LOADMODE_MMDMA = 0x1,
    M2_CLT_CONST_TXTPIPCNTL_PIPALPHASELECT_TEXTURE = 0x1,
    M2_CLT_CONST_TXTPIPCNTL_PIPCOLORSELECT_CONSTANT = 0x0,
    M2_CLT_CONST_TXTPIPCNTL_PIPSSBSELECT_CONSTANT = 0x0,
    M2_CLT_CONST_TXTTABCNTL_ALPHAOUT_PRIMALPHA = 0x0,
    M2_CLT_CONST_TXTTABCNTL_ALPHAOUT_TEXALPHA = 0x1,
    M2_CLT_CONST_TXTTABCNTL_BLENDOP_LERP = 0x0,
    M2_CLT_CONST_TXTTABCNTL_COLOROUT_PRIMCOLOR = 0x0,
    M2_CLT_CONST_TXTTABCNTL_COLOROUT_TEXCOLOR = 0x1,
    M2_CLT_CONST_TXTTABCNTL_FIRSTALPHA_PRIMALPHA = 0x0,
    M2_CLT_CONST_TXTTABCNTL_FIRSTCOLOR_PRIMALPHA = 0x0,
    M2_CLT_CONST_TXTTABCNTL_SECONDALPHA_PRIMALPHA = 0x0,
    M2_CLT_CONST_TXTTABCNTL_SECONDCOLOR_PRIMALPHA = 0x0,
    M2_CLT_CONST_TXTTABCNTL_THIRDCOLOR_PRIMALPHA = 0x0,
};

// Command-list control words and vertex-list selectors.
enum {
    M2_CLT_CMD_WRITE_REGISTER = 0x10000000u,
    M2_CLT_CMD_SET_REGISTER = 0x10001000u,
    M2_CLT_CMD_CLEAR_REGISTER = 0x10001800u,
    M2_CLT_CMD_WRITE_VERTEX = 0x20000000u,
    M2_CLT_VERTEX_FAN = 0x1u,
};

static inline const m2_clt_field_def* m2_clt_field(unsigned field) {
    return &m2_clt_field_defs[field];
}

static inline uint32_t m2_clt_field_mask(unsigned field) {
    return m2_clt_field(field)->mask;
}

static inline uint32_t m2_clt_bits(unsigned field, uint32_t value) {
    const m2_clt_field_def* f = m2_clt_field(field);
    return (value << f->shift) & f->mask;
}

static inline uint32_t m2_clt_const(unsigned field, uint32_t constant) {
    return m2_clt_bits(field, constant);
}

static inline uint32_t m2_clt_registers_header(uint32_t reg, uint32_t count) {
    return (((count - 1u) & 0xffu) << 16) | (reg & 0xffffu);
}

static inline uint32_t m2_clt_write_header(uint32_t reg, uint32_t count) {
    return M2_CLT_CMD_WRITE_REGISTER | m2_clt_registers_header(reg, count);
}

static inline uint32_t m2_clt_set_header(uint32_t reg, uint32_t count) {
    return M2_CLT_CMD_SET_REGISTER | m2_clt_registers_header(reg, count);
}

static inline uint32_t m2_clt_clear_header(uint32_t reg, uint32_t count) {
    return M2_CLT_CMD_CLEAR_REGISTER | m2_clt_registers_header(reg, count);
}

// Composite word builders for the registers the font path writes as a unit.
static inline uint32_t m2_clt_dbusercontrol(
    uint32_t zbuffen, uint32_t zouten, uint32_t winclipinen,
    uint32_t winclipouten, uint32_t blenden, uint32_t srcen,
    uint32_t ditheren, uint32_t destoutmask) {
    return m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_ZBUFFEN, zbuffen) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_ZOUTEN, zouten) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_WINCLIPINEN, winclipinen) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_WINCLIPOUTEN, winclipouten) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_BLENDEN, blenden) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_SRCEN, srcen) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_DITHEREN, ditheren) |
           m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK, destoutmask);
}

static inline uint32_t m2_clt_dbamultcntl(
    uint32_t ainputselect, uint32_t amultcoefselect,
    uint32_t amultconstcontrol, uint32_t amultrjustify) {
    return m2_clt_bits(M2_CLT_FIELD_DBAMULTCNTL_AINPUTSELECT, ainputselect) |
           m2_clt_bits(M2_CLT_FIELD_DBAMULTCNTL_AMULTCOEFSELECT, amultcoefselect) |
           m2_clt_bits(M2_CLT_FIELD_DBAMULTCNTL_AMULTCONSTCONTROL, amultconstcontrol) |
           m2_clt_bits(M2_CLT_FIELD_DBAMULTCNTL_AMULTRJUSTIFY, amultrjustify);
}

static inline uint32_t m2_clt_dbdestalphacntl(uint32_t destconstselect) {
    return m2_clt_bits(M2_CLT_FIELD_DBDESTALPHACNTL_DESTCONSTSELECT,
                       destconstselect);
}

static inline uint32_t m2_clt_dbsrccntl(uint32_t srcmsbrep, uint32_t src32bpp) {
    return m2_clt_bits(M2_CLT_FIELD_DBSRCCNTL_SRCMSBREP, srcmsbrep) |
           m2_clt_bits(M2_CLT_FIELD_DBSRCCNTL_SRC32BPP, src32bpp);
}

static inline uint32_t m2_clt_txtpipcntl(
    uint32_t pipssbselect, uint32_t pipalphaselect,
    uint32_t pipcolorselect, uint32_t pipindexoffset) {
    return m2_clt_bits(M2_CLT_FIELD_TXTPIPCNTL_PIPSSBSELECT, pipssbselect) |
           m2_clt_bits(M2_CLT_FIELD_TXTPIPCNTL_PIPALPHASELECT, pipalphaselect) |
           m2_clt_bits(M2_CLT_FIELD_TXTPIPCNTL_PIPCOLORSELECT, pipcolorselect) |
           m2_clt_bits(M2_CLT_FIELD_TXTPIPCNTL_PIPINDEXOFFSET, pipindexoffset);
}

static inline uint32_t m2_clt_triangle(
    uint32_t new_primitive, uint32_t stripfan, uint32_t perspective,
    uint32_t texture, uint32_t shading, uint32_t count) {
    return M2_CLT_CMD_WRITE_VERTEX |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_NEW, new_primitive) |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_STRIPFAN, stripfan) |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_PERSPECTIVE, perspective) |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_TEXTURE, texture) |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_SHADING, shading) |
           m2_clt_bits(M2_CLT_FIELD_TRIANGLE_COUNT, count - 1u);
}

#ifdef __cplusplus
}
#endif
