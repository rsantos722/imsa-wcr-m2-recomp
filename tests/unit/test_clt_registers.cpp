// test_clt_registers.cpp — pin the project-owned CLT register table.
//
// The expected words below are the command-list wire values the font path
// emits. A table edit that changes a shift, mask, constant, or header encoding
// fails here before it can silently change the game's draw lists.
#include "clt_registers.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void expect_eq(const char* what, uint32_t actual, uint32_t expected) {
    if (actual == expected) return;
    std::printf("FAIL %s: got %08x expected %08x\n", what, actual, expected);
    ++g_failures;
}

}  // namespace

int main() {
    expect_eq("write DCNTL", m2_clt_write_header(M2_CLT_REG_DCNTL, 1),
              0x10000014u);
    expect_eq("set TXTTABCNTL", m2_clt_set_header(M2_CLT_REG_TXTTABCNTL, 1),
              0x10007410u);
    expect_eq("clear TXTADDRCNTL",
              m2_clt_clear_header(M2_CLT_REG_TXTADDRCNTL, 1), 0x10007c08u);
    expect_eq("clear TXTTABCNTL",
              m2_clt_clear_header(M2_CLT_REG_TXTTABCNTL, 1), 0x10007c10u);
    expect_eq("write DCNTLDATA", m2_clt_write_header(M2_CLT_REG_DCNTLDATA, 2),
              0x10010010u);
    expect_eq("write DBSSBDSBCNTL", m2_clt_write_header(M2_CLT_REG_DBSSBDSBCNTL, 3),
              0x10028050u);
    expect_eq("write DBSRCCNTL", m2_clt_write_header(M2_CLT_REG_DBSRCCNTL, 4),
              0x10038030u);
    expect_eq("write TXTUVMAX", m2_clt_write_header(M2_CLT_REG_TXTUVMAX, 2),
              0x1001642cu);
    expect_eq("write TXTCONST0", m2_clt_write_header(M2_CLT_REG_TXTCONST0, 2),
              0x10016440u);

    expect_eq("DCNTL.SYNC", m2_clt_bits(M2_CLT_FIELD_DCNTL_SYNC, 1), 0x20u);
    expect_eq("DCNTL.JA", m2_clt_bits(M2_CLT_FIELD_DCNTL_JA, 1), 0x02u);
    expect_eq("TXTADDRCNTL.TEXTUREENABLE mask",
              m2_clt_field_mask(M2_CLT_FIELD_TXTADDRCNTL_TEXTUREENABLE),
              0x2000u);
    expect_eq("TXTTABCNTL color/alpha masks",
              m2_clt_field_mask(M2_CLT_FIELD_TXTTABCNTL_COLOROUT) |
                  m2_clt_field_mask(M2_CLT_FIELD_TXTTABCNTL_ALPHAOUT),
              0x00018600u);
    expect_eq("DBUSERCONTROL blend/source/mask",
              m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_BLENDEN, 1) |
                  m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_SRCEN, 1) |
                  m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                               M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_ALPHA) |
                  m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                               M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_RED) |
                  m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                               M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_GREEN) |
                  m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                               M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_BLUE),
              0x0000006fu);

    expect_eq("TXTTABCNTL full",
              m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_FIRSTCOLOR,
                           M2_CLT_CONST_TXTTABCNTL_FIRSTCOLOR_PRIMALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_SECONDCOLOR,
                               M2_CLT_CONST_TXTTABCNTL_SECONDCOLOR_PRIMALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_THIRDCOLOR,
                               M2_CLT_CONST_TXTTABCNTL_THIRDCOLOR_PRIMALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_COLOROUT,
                               M2_CLT_CONST_TXTTABCNTL_COLOROUT_TEXCOLOR) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_FIRSTALPHA,
                               M2_CLT_CONST_TXTTABCNTL_FIRSTALPHA_PRIMALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_SECONDALPHA,
                               M2_CLT_CONST_TXTTABCNTL_SECONDALPHA_PRIMALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_ALPHAOUT,
                               M2_CLT_CONST_TXTTABCNTL_ALPHAOUT_TEXALPHA) |
                  m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_BLENDOP,
                               M2_CLT_CONST_TXTTABCNTL_BLENDOP_LERP),
              0x00008200u);

    expect_eq("TXTADDRCNTL full",
              m2_clt_bits(M2_CLT_FIELD_TXTADDRCNTL_TEXTUREENABLE, 1) |
                  m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_MINFILTER,
                               M2_CLT_CONST_TXTADDRCNTL_MINFILTER_POINT) |
                  m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_INTERFILTER,
                               M2_CLT_CONST_TXTADDRCNTL_INTERFILTER_POINT) |
                  m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_MAGFILTER,
                               M2_CLT_CONST_TXTADDRCNTL_MAGFILTER_POINT) |
                  m2_clt_bits(M2_CLT_FIELD_TXTADDRCNTL_LODMAX, 0),
              0x00002000u);

    expect_eq("DBUSERCONTROL pixel rectangle",
              m2_clt_dbusercontrol(
                  0, 0, 0, 0, 0, 0, 0,
                  M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_RED |
                      M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_GREEN |
                      M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_BLUE |
                      M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_ALPHA),
              0x0000000fu);

    expect_eq("DBAMULTCNTL pixel rectangle",
              m2_clt_dbamultcntl(
                  M2_CLT_CONST_DBAMULTCNTL_AINPUTSELECT_TEXCOLOR,
                  M2_CLT_CONST_DBAMULTCNTL_AMULTCOEFSELECT_TEXALPHA,
                  M2_CLT_CONST_DBAMULTCNTL_AMULTCONSTCONTROL_TEXSSB, 0),
              0x00000000u);

    expect_eq("DBDESTALPHACNTL pixel rectangle",
              m2_clt_dbdestalphacntl(
                  M2_CLT_CONST_DBDESTALPHACNTL_DESTCONSTSELECT_TEXALPHA),
              0x00000000u);

    expect_eq("TXTPIPCNTL",
              m2_clt_txtpipcntl(
                  M2_CLT_CONST_TXTPIPCNTL_PIPSSBSELECT_CONSTANT,
                  M2_CLT_CONST_TXTPIPCNTL_PIPALPHASELECT_TEXTURE,
                  M2_CLT_CONST_TXTPIPCNTL_PIPCOLORSELECT_CONSTANT, 0),
              0x00000800u);

    expect_eq("triangle untextured",
              m2_clt_triangle(1, M2_CLT_VERTEX_FAN, 1, 0, 1, 4), 0x201d0003u);
    expect_eq("triangle textured",
              m2_clt_triangle(1, M2_CLT_VERTEX_FAN, 1, 1, 0, 4), 0x201e0003u);

    expect_eq("DBSRCCNTL 32-bit", m2_clt_dbsrccntl(0, 1), 0x00000001u);
    expect_eq("DBSRCCNTL 16-bit", m2_clt_dbsrccntl(1, 0), 0x00000002u);

    expect_eq("DBBMULTCNTL source color/complement",
              m2_clt_bits(M2_CLT_FIELD_DBBMULTCNTL_BINPUTSELECT,
                          M2_CLT_CONST_DBBMULTCNTL_BINPUTSELECT_SRCCOLOR) |
                  m2_clt_bits(
                      M2_CLT_FIELD_DBBMULTCNTL_BMULTCOEFSELECT,
                      M2_CLT_CONST_DBBMULTCNTL_BMULTCOEFSELECT_TEXALPHACOMPLEMENT),
              0x00000002u);

    for (unsigned i = 0; i < M2_CLT_FIELD_COUNT; ++i) {
        const m2_clt_field_def* f = m2_clt_field(i);
        if (f->mask == 0u || f->shift >= 32u) {
            std::printf("FAIL field %u has invalid shift/mask\n", i);
            ++g_failures;
        }
    }

    if (g_failures != 0) {
        std::printf("m2_clt_registers_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_clt_registers_test: PASS\n");
    return 0;
}
