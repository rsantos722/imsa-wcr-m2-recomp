// font_folio.c — font folio (id 9) boundary for the static recompilation of
// shooter2d, plus the kernel:133 FindAndOpenItem item loader that backs it.
//
// This is a project-authored host implementation of the M2 font folio behavior
// used by the game. The covered paths are the font file parser and item loader
// (ParseFont, LoadFont, GetFontCharInfo, FindAndOpenItem), the text-state
// builders (CreateTextState, BuildRenderTable, CalculateRectangles,
// ClipRectangle, texel address/offset, CharWidth, DeleteTextState), and the
// Triangle Engine list builders (CreateTELists, DrawText). Behavior is verified
// against the recovered guest image.
//
// The CLT instruction words are emitted through the project-owned table in
// shims/clt_registers.h.
//
// Two deliberate, visible limits:
//
//  * Only ord 0/1/2 are implemented, because those are the only vectors the
//    on-disc module exports to this image. Every other folio-9 ordinal keeps
//    the fail-closed gpr[3] = 0 path in recomp_call_selector_inner.
//
//  * DeleteTextState and DrawText enforce the M2 BUILD_PARANOIA
//    CheckTextState (ts_Cookie == ts). The M2 implementation compiles it out,
//    but the lifted shooter2d call site passes whatever the previous frame left
//    in the table, so an unchecked FreeMem on a stale pointer would corrupt the
//    host heap. The check returns the same FONT_ERR_BADTS the paranoia build
//    does.
//
// The guest owns the FontTextArray, the string, the pen and the GState; this
// file only parses the font file it is handed, sizes the Triangle Engine list
// the M2 implementation sizes, and links that list into the caller's GState.

#include "ppc_ctx.h"
#include "recomp_helpers.h"
#include "guest_kernel.h"
#include "guest_module.h"
#include "shim_abi.h"
#include "font_folio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clt_registers.h"

// ---- the 32-bit M2 offsets in font_folio.h are the contract. Pin the ones
// the guest actually writes: shooter2d textstuff.c inittextstuff stores
// fta_StructSize at 0x2dee08, pen_X at 0x2dee0c, ... fta_NumChars at 0x2dee48,
// i.e. FontTextArray base 0x2dee08 + the offsets below; drawscoretext rewrites
// +0x3c (fta_String) and +0x40 (fta_NumChars) on that same base. --------------
_Static_assert(M2_FTA_SIZE == 68u, "M2 sizeof(FontTextArray) is 68 bytes");
_Static_assert(M2_FTA_STRING == 0x3cu, "fta_String is the 61st byte");
_Static_assert(M2_FTA_NUM_CHARS == 0x40u, "fta_NumChars is the 65th byte");
_Static_assert(M2_FTA_CLIP_MIN_X == 0x2cu,
               "fta_Clip follows the 40-byte PenInfo at +0x04");
_Static_assert(M2_FD_SIZE == 0x6cu, "FontDescriptor = OpeningItemNode + 17 words");
_Static_assert(M2_TS_HEADER_SIZE == 0x90u,
               "TextState header precedes the trailing vertexInfo array");

// ---- bounded working limits ----------------------------------------------
// The M2 implementation derives its scratch size from fta_NumChars and the string
// contents, both guest-owned, so each one is bounded here rather than trusted.
#define M2_FONT_MAX_ARRAYS 64u
#define M2_FONT_MAX_CHARS 4096u        // per string and across all strings
#define M2_FONT_MAX_RECTS 256u
#define M2_FONT_MAX_RANGES 64u
#define M2_FONT_MAX_FONTS 4u
#define M2_FONT_MAX_NAME 64u
#define M2_FONT_MAX_IMAGE (16u << 20)

// TRAM_SIZE / TRAM_WIDTH, the TE's 16 KiB texture memory.
#define M2_FONT_TRAM_SIZE (16u * 1024u)
#define M2_FONT_TRAM_WIDTH 1024u

// the render table: default pen W values used when FLAG_PI_W_VALID is not set.
#define M2_FONT_DEFAULT_BGW 0.999900f
#define M2_FONT_DEFAULT_FGW 0.999998f

// the font file header: the labels and the only chunk version / pixel depth ParseFont
// accepts.
#define M2_FONT_FORM_LABEL 0x464f524du  /* 'FORM' */
#define M2_FONT_CHUNK_LABEL 0x464f4e54u /* 'FONT' */

// graphics/clt/gstate.h: gs_SendList, gs_EndList, gs_ListPtr are the first
// three words; the same gs_ListPtr offset the host's list-chain probe reads.
#define M2_GS_LIST_PTR 0x08u

// Real disc-FS access for font files goes through the same weak host bridge the
// recovered RawFile / File-Item window already uses (strong definitions in
// host/core/filesystem_shim.cpp); the guest never sees a host pointer.
extern uint32_t m2_host_raw_open(const char* guest_path) __attribute__((weak));
extern uint32_t m2_host_raw_size(uint32_t host_token) __attribute__((weak));
extern int32_t m2_host_raw_read(uint32_t host_token, void* dst,
                                uint32_t n) __attribute__((weak));
extern void m2_host_raw_close(uint32_t host_token) __attribute__((weak));

// ---------------------------------------------------------------- host scratch
// Host-side mirrors of the M2 render-table structures. These are the
// folio's own scratch and never cross into guest RAM: the values that reach
// guest memory are only the immediates and guest EAs written into the TE list,
// which are exactly the values the M2 implementation writes.
typedef struct m2_font_char_info {
    float    X;
    float    Y;
    float    W;
    uint32_t BgColor;
    uint32_t FgColor;
    uint32_t offset;
    float    width;
    float    height;
    float    twidth;
    uint8_t  entry;
    uint8_t  pad[3];
} m2_font_char_info;

typedef struct m2_font_rth {
    uint32_t texel;         // guest EA of the range's first texel
    uint32_t charCnt;       // number of charInfo entries in this range
    uint32_t height;        // fd_charHeight
    uint32_t bpp;           // fd_bitsPerPixel
    uint32_t bytesToLoad;
    uint32_t offset;        // texel offset of minChar inside the range
    uint8_t  minChar;
    uint8_t  maxChar;
    uint8_t  pad[2];
} m2_font_rth;

typedef struct m2_font_bg_rect {
    float    box[4];        // min.x, min.y, max.x, max.y
    float    w;
    uint32_t rgb;
    uint32_t clipped;
} m2_font_bg_rect;

_Static_assert(sizeof(m2_font_char_info) == M2_CHARINFO_SIZE, "charInfo size");
_Static_assert(sizeof(m2_font_rth) == M2_RENDER_TABLE_HEADER_SIZE, "rth size");
_Static_assert(sizeof(m2_font_bg_rect) == M2_BG_RECT_SIZE, "bgRectInfo size");

// One render-table range: the M2 renderTableHeader plus the charInfo
// entries it lays out immediately after it (here a slice of a shared pool, so
// the folio's scratch stays O(total characters) instead of O(ranges*max)).
typedef struct m2_font_range {
    m2_font_rth hdr;
    uint32_t    first;      // index into the shared charInfo pool
} m2_font_range;

// ---------------------------------------------------------------- font items
// A font Item owns a guest-visible copy of the font file plus a published
// FontDescriptor. The Item handle itself is minted by the shared guest-kernel
// item pool through CreateItem - the same pool the filesystem folio's File
// Items use - so CloseItem/DeleteItem/CheckItem see a genuine Item. Only the
// font-private payload lives here, mirroring the g_file_slots pattern in
// recomp_runtime.c.
typedef struct m2_font_slot {
    uint8_t  used;
    uint32_t item;        // guest-visible Item handle (shared item pool)
    uint32_t name_ea;     // guest EA of a NUL-terminated copy of the name
    uint32_t image_ea;    // guest EA of the whole font file
    uint32_t image_size;  // exactly the size AllocMem was asked for
    uint32_t desc_ea;     // guest EA of the published FontDescriptor
} m2_font_slot;

static m2_font_slot g_font_slots[M2_FONT_MAX_FONTS];

static m2_font_slot* m2_font_find(uint32_t item) {
    if (item == 0) return NULL;
    for (uint32_t i = 0; i < M2_FONT_MAX_FONTS; ++i) {
        m2_font_slot* slot = &g_font_slots[i];
        if (slot->used && slot->item == item) return slot;
    }
    return NULL;
}

static void m2_font_slot_release(m2_font_slot* slot) {
    if (!slot) return;
    // FreeMem takes a guest EA and a size and needs no PPCContext, so this must
    // not be gated on m2_guest_kernel_get_active_context(): that global is set
    // only for the duration of a kernel-folio dispatch (recomp_helpers.h:1725),
    // so any other path that reached DeleteItem would silently leak the whole
    // font payload instead of releasing it.
    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (r && r->FreeMem) {
        if (slot->image_ea)
            r->FreeMem((void*)(uintptr_t)slot->image_ea, (int32_t)slot->image_size);
        if (slot->desc_ea)
            r->FreeMem((void*)(uintptr_t)slot->desc_ea, (int32_t)M2_FD_SIZE);
        if (slot->name_ea)
            r->FreeMem((void*)(uintptr_t)slot->name_ea, (int32_t)M2_FONT_MAX_NAME);
    }
    memset(slot, 0, sizeof(*slot));
}

// Guest EA of the FontDescriptor for a live font Item, or 0. This is the
// folio-internal equivalent of the M2
// `fd = CheckItem(font, NST_FONT, FONT_FONT_NODE)` in doCreateTextState; the
// descriptor carries the node header at ItemNode+8/+9, so a guest CheckItem
// agrees with it.
static uint32_t m2_font_item_fontdescriptor(uint32_t item) {
    const m2_font_slot* slot = m2_font_find(item);
    return slot ? slot->desc_ea : 0u;
}

// Release a font Item's guest-visible payload when the guest deletes the Item.
// Called from the kernel DeleteItem handler so an Item cannot outlive its copy.
void m2_font_release_item(uint32_t item);

// ---- FontDescriptor field access (32-bit guest layout) -------------------
static uint32_t fd_u32(PPCContext* c, uint32_t fd, uint32_t off) {
    return rd32(c, fd + off);
}
static void fd_set_u32(PPCContext* c, uint32_t fd, uint32_t off, uint32_t v) {
    wr32(c, fd + off, v);
}

// ParseFont(): validate the FORM/FONT header, then fill the
// descriptor exactly the way the M2 implementation does.
static int32_t m2_font_parse(PPCContext* c, uint32_t fd, uint32_t image_ea,
                             uint32_t image_size) {
    // The M2 compares against GetMemTrackSize(image) - 8 because the M2
    // allocator prefixes a tracked-size word. This host's guest copy starts at
    // the FORM label, so the tracked total is image_size + 8 and formSize is
    // exactly image_size.
    if (image_size < M2_FONT_HDR_SIZE ||
        image_ea > RECOMP_GUEST_MEM - M2_FONT_HDR_SIZE) {
        return M2_FONT_ERR_BADFONTFILE;
    }
    const uint32_t label   = rd32(c, image_ea + M2_FONT_HDR_FORM_LABEL);
    const uint32_t formsz  = rd32(c, image_ea + M2_FONT_HDR_FORM_SIZE);
    const uint32_t id      = rd32(c, image_ea + M2_FONT_HDR_FORM_ID);
    const uint32_t chunkid = rd32(c, image_ea + M2_FONT_HDR_CHUNK_ID);
    const uint32_t chunksz = rd32(c, image_ea + M2_FONT_HDR_CHUNK_SIZE);
    const uint16_t chunkver = (uint16_t)(
        ((uint16_t)rd8(c, image_ea + M2_FONT_HDR_CHUNK_VERSION) << 8) |
         (uint16_t)rd8(c, image_ea + M2_FONT_HDR_CHUNK_VERSION + 1u));
    if (label != M2_FONT_FORM_LABEL || formsz > image_size ||
        id != M2_FONT_CHUNK_LABEL || chunkid != M2_FONT_CHUNK_LABEL ||
        chunksz > (image_size - 12u)) {
        return M2_FONT_ERR_BADFONTFILE;
    }
    if (chunkver != M2_FONT_HDR_CHUNK_VERSION_VALUE) return M2_FONT_ERR_BADVERSION;

    const uint32_t desc   = image_ea + M2_FONT_HDR_DESC;
    const uint32_t bpp    = rd32(c, desc + M2_FONT_DESC_BPP * 4u);
    if (bpp != 4u) return M2_FONT_ERR_BADBPP;

    const uint32_t wtable_off  = rd32(c, image_ea + M2_FONT_HDR_WTABLE_OFFSET);
    const uint32_t data_off    = rd32(c, image_ea + M2_FONT_HDR_DATA_OFFSET);
    const uint32_t row_bytes   = rd32(c, image_ea + M2_FONT_HDR_CHAR_ROW_BYTES);
    const uint32_t char_height = rd32(c, image_ea + M2_FONT_HDR_CHAR_HEIGHT);
    if (wtable_off > image_size || data_off > image_size ||
        row_bytes == 0u || char_height == 0u) {
        return M2_FONT_ERR_BADFONTFILE;
    }

    wr8(c, fd + M2_FONT_FD_N_VERSION, rd8(c, image_ea + M2_FONT_HDR_VERSION));
    wr8(c, fd + M2_FONT_FD_N_REVISION, rd8(c, image_ea + M2_FONT_HDR_REVISION));
    fd_set_u32(c, fd, M2_FONT_FD_FONT_FLAGS, rd32(c, desc + 5u * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_BITS_PER_PIXEL, bpp);
    fd_set_u32(c, fd, M2_FONT_FD_FIRST_CHAR, rd32(c, desc + M2_FONT_DESC_FIRST_CHAR * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_LAST_CHAR,  rd32(c, desc + M2_FONT_DESC_LAST_CHAR * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_ASCENT,     rd32(c, desc + M2_FONT_DESC_ASCENT * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_DESCENT,    rd32(c, desc + M2_FONT_DESC_DESCENT * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_CHAR_EXTRA,  rd32(c, desc + M2_FONT_DESC_SPACING * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_LEADING,    rd32(c, desc + M2_FONT_DESC_LEADING * 4u));
    fd_set_u32(c, fd, M2_FONT_FD_BYTES_PER_ROW, row_bytes);
    fd_set_u32(c, fd, M2_FONT_FD_CHAR_WIDTH,
               rd32(c, image_ea + M2_FONT_HDR_CHAR_WIDTH));
    fd_set_u32(c, fd, M2_FONT_FD_CHAR_HEIGHT, char_height);
    fd_set_u32(c, fd, M2_FONT_FD_FONT_HEADER, image_ea);
    fd_set_u32(c, fd, M2_FONT_FD_CHAR_INFO, image_ea + wtable_off);
    fd_set_u32(c, fd, M2_FONT_FD_CHAR_DATA, image_ea + data_off);

    // how many characters fit in TRAM, then narrow to TRAM width. Both
    // values come from the font file; bound them before multiplying so a
    // corrupt header cannot wrap to zero and divide by zero.
    if (row_bytes == 0 || char_height == 0 ||
        row_bytes > M2_FONT_TRAM_SIZE || char_height > M2_FONT_TRAM_SIZE) {
        return 0;
    }
    const uint64_t cell_bytes = (uint64_t)row_bytes * char_height;
    if (cell_bytes == 0 || cell_bytes > M2_FONT_TRAM_SIZE) return 0;
    const uint32_t texels_per_byte = 8u / bpp;
    uint32_t char_count = (uint32_t)(M2_FONT_TRAM_SIZE / cell_bytes);
    const uint32_t chars_width = row_bytes * char_count * texels_per_byte;
    if (chars_width > M2_FONT_TRAM_WIDTH)
        char_count = M2_FONT_TRAM_WIDTH / (row_bytes * texels_per_byte);
    fd_set_u32(c, fd, M2_FONT_FD_MAX_CHARS_LOAD, char_count);
    fd_set_u32(c, fd, M2_FONT_FD_RANGE_GAP, 4u);   // "Just a heuristic"
    return 0;
}

// GetFontCharInfo(): the big-endian bitfield gives fci_charOffset
// the top 22 bits of the word and fci_charWidth the low 8.
static uint32_t m2_font_fci(PPCContext* c, uint32_t fd, int32_t the_char,
                            uint32_t* out_offset, uint32_t* out_width) {
    const uint32_t first = fd_u32(c, fd, M2_FONT_FD_FIRST_CHAR);
    const uint32_t last  = fd_u32(c, fd, M2_FONT_FD_LAST_CHAR);
    const uint32_t info  = fd_u32(c, fd, M2_FONT_FD_CHAR_INFO);
    uint32_t word = 0;
    if (the_char >= (int32_t)first && the_char <= (int32_t)last) {
        const uint32_t slot =
            info + ((uint32_t)((int32_t)the_char - (int32_t)first)) * 4u;
        if (slot < RECOMP_GUEST_MEM - 3u) word = rd32(c, slot);
    }
    if (out_offset) *out_offset = word >> 10;
    if (out_width) *out_width = word & 0xffu;
    return word;
}

static int32_t m2_font_char_width(PPCContext* c, uint32_t fd, int32_t ch) {
    uint32_t w = 0;
    m2_font_fci(c, fd, ch, NULL, &w);
    return (int32_t)w;
}

// CalcTexelOffset(): texels are charHeight per character inside a
// loaded range, not bytes.
static uint32_t m2_font_texel_offset(PPCContext* c, uint32_t fd, int32_t ch,
                                     int32_t first) {
    return (uint32_t)((int32_t)ch - first) *
           fd_u32(c, fd, M2_FONT_FD_CHAR_HEIGHT);
}

// CalcTexelAddress(): fd_charData + the character's byte offset.
static uint32_t m2_font_texel_address(PPCContext* c, uint32_t fd, int32_t ch,
                                      uint32_t* out_offset) {
    uint32_t off = 0;
    m2_font_fci(c, fd, ch, &off, NULL);
    if (out_offset) *out_offset = off;
    return fd_u32(c, fd, M2_FONT_FD_CHAR_DATA) + off;
}

// ------------------------------------------------------------ TE list writer
// TE words are emitted into guest memory through this cursor, which bounds
// every write against the allocation CreateTELists made. A short write is a
// hard error rather than a silent truncation.
typedef struct m2_font_te {
    PPCContext* c;
    uint32_t    ea;
    uint32_t    limit;    // first guest EA past the end of the allocation
    int         overflow;
} m2_font_te;

static void m2_font_te_word(m2_font_te* te, uint32_t word) {
    if (te->limit < te->ea || te->limit - te->ea < 4u) { te->overflow = 1; return; }
    wr32(te->c, te->ea, word);
    te->ea += 4u;
}

static void m2_font_te_f32(m2_font_te* te, float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    m2_font_te_word(te, bits);
}

// CLT_VertexRgbaW(): seven floats per shaded vertex.
static void m2_font_te_rgbaw(m2_font_te* te, float x, float y, float r, float g,
                             float b, float a, float w) {
    m2_font_te_f32(te, x); m2_font_te_f32(te, y);
    m2_font_te_f32(te, r); m2_font_te_f32(te, g);
    m2_font_te_f32(te, b); m2_font_te_f32(te, a);
    m2_font_te_f32(te, w);
}

// CLT_CopySnippetData(&te, &CltNoTextureSnippet) - libs/clt/cltglobals.c:12,
// built here through the project-owned CLT register table, so the register map
// is the one the rest of this list uses. The copy semantics are the guest's own:
// microcard.c CLT_CopySnippetData@0x11c64 copies snippet->size words from
// snippet->data to the list cursor and advances the cursor by 4 bytes per word
// (the folio 1:31 call it makes in between is the same word-copy the host
// already performs for InitList/DBlendList).
static void m2_font_te_no_texture(m2_font_te* te) {
    m2_font_te_word(te, m2_clt_write_header(M2_CLT_REG_DCNTL, 1));
    m2_font_te_word(te, m2_clt_bits(M2_CLT_FIELD_DCNTL_SYNC, 1));
    m2_font_te_word(te, m2_clt_clear_header(M2_CLT_REG_TXTADDRCNTL, 1));
    m2_font_te_word(
        te, m2_clt_field_mask(M2_CLT_FIELD_TXTADDRCNTL_TEXTUREENABLE));
    m2_font_te_word(te, m2_clt_clear_header(M2_CLT_REG_TXTTABCNTL, 1));
    m2_font_te_word(
        te, m2_clt_field_mask(M2_CLT_FIELD_TXTTABCNTL_COLOROUT) |
                m2_clt_field_mask(M2_CLT_FIELD_TXTTABCNTL_ALPHAOUT));
    m2_font_te_word(te, m2_clt_set_header(M2_CLT_REG_TXTTABCNTL, 1));
    m2_font_te_word(
        te,
        m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_COLOROUT,
                     M2_CLT_CONST_TXTTABCNTL_COLOROUT_PRIMCOLOR) |
            m2_clt_const(M2_CLT_FIELD_TXTTABCNTL_ALPHAOUT,
                         M2_CLT_CONST_TXTTABCNTL_ALPHAOUT_PRIMALPHA));
}

// CLT_CopySnippetData(&te, &prlsnippet) - the M2 list builder's "pixel rectangle
// line" setup that disables the destination mask and takes alpha from the
// texture register.
static void m2_font_te_prl(m2_font_te* te) {
    m2_font_te_word(te, m2_clt_set_header(M2_CLT_REG_DBUSERCONTROL, 1));
    m2_font_te_word(
        te, m2_clt_dbusercontrol(
                0, 0, 0, 0, 0, 0, 0,
                M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_RED |
                    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_GREEN |
                    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_BLUE |
                    M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_ALPHA));
    m2_font_te_word(te, m2_clt_write_header(M2_CLT_REG_DBAMULTCNTL, 1));
    m2_font_te_word(
        te, m2_clt_dbamultcntl(
                M2_CLT_CONST_DBAMULTCNTL_AINPUTSELECT_TEXCOLOR,
                M2_CLT_CONST_DBAMULTCNTL_AMULTCOEFSELECT_TEXALPHA,
                M2_CLT_CONST_DBAMULTCNTL_AMULTCONSTCONTROL_TEXSSB, 0));
    m2_font_te_word(te,
                    m2_clt_write_header(M2_CLT_REG_DBDESTALPHACNTL, 1));
    m2_font_te_word(
        te,
        m2_clt_dbdestalphacntl(
            M2_CLT_CONST_DBDESTALPHACNTL_DESTCONSTSELECT_TEXALPHA));
}

static void m2_font_te_sync(m2_font_te* te) {
    m2_font_te_word(te, m2_clt_write_header(M2_CLT_REG_DCNTL, 1));
    m2_font_te_word(te, m2_clt_bits(M2_CLT_FIELD_DCNTL_SYNC, 1));
}

// Where CreateTELists records one vertexInfo per emitted destination, exactly
// like the M2 STORE_VERTEX / vertexInfo array.
typedef struct m2_font_vertex_sink {
    PPCContext* c;
    uint32_t    ea;        // guest EA of the TextState's vertexInfo array
    uint32_t    slots;
    uint32_t    used;
    float       origin_x;  // first array's pen_X, the M2 dx/dy datum
    float       origin_y;
} m2_font_vertex_sink;

static void m2_font_store_vertex(m2_font_vertex_sink* sink, uint32_t triangle_ea,
                                 float x, float y) {
    if (sink->used >= sink->slots) return;
    const uint32_t rec = sink->ea + sink->used * M2_VERTEX_INFO_SIZE;
    wr32(sink->c, rec + 0x00u, triangle_ea);
    wrf32(sink->c, rec + 0x04u, x - sink->origin_x);
    wrf32(sink->c, rec + 0x08u, y - sink->origin_y);
    wr32(sink->c, rec + 0x0cu, 0u);
    wr32(sink->c, rec + 0x10u, 0u);
    wr32(sink->c, rec + 0x14u, 0u);
    wr32(sink->c, rec + 0x18u, 0u);
    ++sink->used;
}

static void m2_font_store_vertex_end(m2_font_vertex_sink* sink) {
    if (sink->used >= sink->slots) return;
    wr32(sink->c, sink->ea + sink->used * M2_VERTEX_INFO_SIZE + 0x00u, 0u);
}

// CreateTELists(): turn the render table into the TE list the GState
// jumps into. On success returns the guest EA of the reserved JumpAbsolute slot
// (the M2 ts_TERet), or a negative Err.
static int32_t m2_font_create_te_lists(PPCContext* c, uint32_t ts,
                                       uint32_t te_ea, uint32_t te_size,
                                       uint32_t range_count, uint32_t total_chars,
                                       uint32_t array_count,
                                       const m2_font_range* ranges,
                                       const m2_font_char_info* chars,
                                       const m2_font_bg_rect* rects,
                                       uint32_t rect_count,
                                       m2_font_vertex_sink* sink) {
    // The render table can only carry the background rectangles if
    // CalculateRectangles filled them in. A count with no table is a broken
    // internal precondition, and the only error the M2 table path can return is
    // FONT_ERR_NOMEM.
    if (rect_count != 0u && rects == NULL) return M2_FONT_ERR_NOMEM;

    m2_font_te te = { c, te_ea, te_ea + te_size, 0 };

    // Background rectangles come first, before the sync and the InitList:
    // the M2 implementation turns texture mapping off, runs the "pixel
    // rectangle line" register setup, then emits one shaded, untextured
    // four-vertex fan per rectangle that survived ClipRectangle.
    if (rect_count != 0u) {
        m2_font_te_no_texture(&te);
        m2_font_te_prl(&te);
        for (uint32_t i = 0; i < rect_count; ++i) {
            const m2_font_bg_rect* ri = &rects[i];
            if (ri->clipped) continue;
            const float r = (float)((ri->rgb >> 16) / 255.0);
            const float g = (float)(((ri->rgb >> 8) & 0xffu) / 255.0);
            const float b = (float)((ri->rgb & 0xffu) / 255.0);
            m2_font_store_vertex(sink, te.ea, ri->box[0], ri->box[1]);
            m2_font_te_word(
                &te, m2_clt_triangle(1, M2_CLT_VERTEX_FAN, 1, 0, 1, 4));
            m2_font_te_rgbaw(&te, ri->box[0], ri->box[1], r, g, b, 1.0f, ri->w);
            m2_font_te_rgbaw(&te, ri->box[0], ri->box[3], r, g, b, 1.0f, ri->w);
            m2_font_te_rgbaw(&te, ri->box[2], ri->box[3], r, g, b, 1.0f, ri->w);
            m2_font_te_rgbaw(&te, ri->box[2], ri->box[1], r, g, b, 1.0f, ri->w);
        }
    }

    m2_font_te_sync(&te);

    // InitList, with ADEPTH taken from the range's bpp.
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTEXPTYPE, 1));
    m2_font_te_word(&te, m2_clt_bits(M2_CLT_FIELD_TXTEXPTYPE_HASALPHA, 1) |
                             m2_clt_bits(M2_CLT_FIELD_TXTEXPTYPE_ADEPTH,
                                         ranges[0].hdr.bpp));
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTLDCNTL, 1));
    m2_font_te_word(&te,
                    m2_clt_const(M2_CLT_FIELD_TXTLDCNTL_LOADMODE,
                                 M2_CLT_CONST_TXTLDCNTL_LOADMODE_MMDMA));
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTADDRCNTL, 1));
    m2_font_te_word(
        &te, m2_clt_bits(M2_CLT_FIELD_TXTADDRCNTL_TEXTUREENABLE, 1) |
                 m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_MINFILTER,
                              M2_CLT_CONST_TXTADDRCNTL_MINFILTER_POINT) |
                 m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_INTERFILTER,
                              M2_CLT_CONST_TXTADDRCNTL_INTERFILTER_POINT) |
                 m2_clt_const(M2_CLT_FIELD_TXTADDRCNTL_MAGFILTER,
                              M2_CLT_CONST_TXTADDRCNTL_MAGFILTER_POINT) |
                 m2_clt_bits(M2_CLT_FIELD_TXTADDRCNTL_LODMAX, 0));
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTPIPCNTL, 1));
    m2_font_te_word(
        &te, m2_clt_txtpipcntl(
                 M2_CLT_CONST_TXTPIPCNTL_PIPSSBSELECT_CONSTANT,
                 M2_CLT_CONST_TXTPIPCNTL_PIPALPHASELECT_TEXTURE,
                 M2_CLT_CONST_TXTPIPCNTL_PIPCOLORSELECT_CONSTANT, 0));
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTTABCNTL, 1));
    m2_font_te_word(
        &te,
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
                         M2_CLT_CONST_TXTTABCNTL_BLENDOP_LERP));
    m2_font_te_word(&te, m2_clt_set_header(M2_CLT_REG_DBUSERCONTROL, 1));
    m2_font_te_word(
        &te, m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_BLENDEN, 1) |
                 m2_clt_bits(M2_CLT_FIELD_DBUSERCONTROL_SRCEN, 1) |
                 m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                              M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_ALPHA) |
                 m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                              M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_RED) |
                 m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                              M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_GREEN) |
                 m2_clt_const(M2_CLT_FIELD_DBUSERCONTROL_DESTOUTMASK,
                              M2_CLT_CONST_DBUSERCONTROL_DESTOUTMASK_BLUE));
    m2_font_te_word(&te,
                    m2_clt_write_header(M2_CLT_REG_DBSSBDSBCNTL, 3));
    m2_font_te_word(&te, 0u);   // DBSSBDSBCNTL
    m2_font_te_word(&te, 0u);   // DBCONSTIN
    m2_font_te_word(&te, 0u);   // DBAMULTCNTL
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_DBALUCNTL, 1));
    m2_font_te_word(&te, 0u);

    // DBlendList: ts_dBlend points at the DBSRCCNTL header word and
    // DrawText fills the destination buffer, stride and source base.
    const uint32_t dblend_ea = te.ea;
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_DBSRCCNTL, 4));
    m2_font_te_word(&te, 0u);   // DBSRCCNTL
    m2_font_te_word(&te, 0u);   // DBSRCBASEADDR
    m2_font_te_word(&te, 0u);   // DBSRCXSTRIDE
    m2_font_te_word(&te, 0u);   // DBSRCOFFSET
    wr32(c, ts + M2_TS_DBLEND, dblend_ea);

    // Character ranges.
    const uint32_t fd = rd32(c, ts + M2_TS_FONT);
    const uint32_t bpr = fd_u32(c, fd, M2_FONT_FD_BYTES_PER_ROW);
    uint32_t last_fg = 0xffffffffu;
    uint32_t colour_changes = 0;
    for (uint32_t r = 0; r < range_count; ++r) {
        const m2_font_rth* rth = &ranges[r].hdr;
        if (!rth->charCnt) continue;
        // TextureLoadList.
        const uint32_t texels_per_byte = 8u / (rth->bpp ? rth->bpp : 4u);
        const uint32_t font_chars =
            (uint32_t)rth->maxChar - (uint32_t)rth->minChar + 1u;
        m2_font_te_sync(&te);
        m2_font_te_word(&te,
                        m2_clt_write_header(M2_CLT_REG_TXTLDSRCADDR, 1));
        m2_font_te_word(&te, rth->texel);
        m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTLODBASE0, 1));
        m2_font_te_word(&te, 0u);
        m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTCOUNT, 1));
        m2_font_te_word(&te, rth->bytesToLoad);
        m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_DCNTL, 1));
        m2_font_te_word(&te, m2_clt_bits(M2_CLT_FIELD_DCNTL_TLD, 1));
        m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTLODBASE0, 1));
        m2_font_te_word(&te, 0u);
        m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_TXTUVMAX, 2));
        m2_font_te_word(
            &te, m2_clt_bits(M2_CLT_FIELD_TXTUVMAX_UMAX,
                             (bpr * texels_per_byte) - 1u) |
                     m2_clt_bits(M2_CLT_FIELD_TXTUVMAX_VMAX,
                                 (rth->height * font_chars) - 1u));
        m2_font_te_word(
            &te, m2_clt_bits(M2_CLT_FIELD_TXTUVMASK_UMASK, 0x3ffu) |
                     m2_clt_bits(M2_CLT_FIELD_TXTUVMASK_VMASK, 0x3ffu));

        for (uint32_t i = 0; i < rth->charCnt; ++i) {
            const m2_font_char_info* ci = &chars[ranges[r].first + i];
            if (ci->FgColor != last_fg) {
                m2_font_te_sync(&te);
                m2_font_te_word(
                    &te, m2_clt_write_header(M2_CLT_REG_TXTCONST0, 2));
                m2_font_te_word(&te, ci->FgColor);
                m2_font_te_word(&te, ci->FgColor);
                m2_font_te_word(
                    &te, m2_clt_write_header(M2_CLT_REG_DBCONSTIN, 1));
                m2_font_te_word(&te, ci->BgColor);
                m2_font_te_word(
                    &te,
                    m2_clt_write_header(M2_CLT_REG_DBDISCARDCONTROL, 1));
                m2_font_te_word(
                    &te,
                    m2_clt_bits(M2_CLT_FIELD_DBDISCARDCONTROL_ALPHA0, 1));
                m2_font_te_word(
                    &te, m2_clt_write_header(M2_CLT_REG_DBBMULTCNTL, 1));
                m2_font_te_word(
                    &te,
                    m2_clt_const(M2_CLT_FIELD_DBBMULTCNTL_BINPUTSELECT,
                                 M2_CLT_CONST_DBBMULTCNTL_BINPUTSELECT_SRCCOLOR) |
                        m2_clt_const(
                            M2_CLT_FIELD_DBBMULTCNTL_BMULTCOEFSELECT,
                            M2_CLT_CONST_DBBMULTCNTL_BMULTCOEFSELECT_TEXALPHACOMPLEMENT));
                last_fg = ci->FgColor;
                ++colour_changes;
            }
            // Textured, unshaded 4-vertex triangle fan. CLT_VertexUvW emits
            // (x, y, w, u, v), so w precedes the texture coordinates.
            const uint32_t tri_ea = te.ea;
            m2_font_te_word(
                &te, m2_clt_triangle(1, M2_CLT_VERTEX_FAN, 1, 1, 0, 4));
            m2_font_te_f32(&te, ci->X);
            m2_font_te_f32(&te, ci->Y);
            m2_font_te_f32(&te, ci->W);
            m2_font_te_f32(&te, 0.0f);
            m2_font_te_f32(&te, (float)ci->offset);
            m2_font_te_f32(&te, ci->X + ci->width);
            m2_font_te_f32(&te, ci->Y);
            m2_font_te_f32(&te, ci->W);
            m2_font_te_f32(&te, ci->twidth);
            m2_font_te_f32(&te, (float)ci->offset);
            m2_font_te_f32(&te, ci->X + ci->width);
            m2_font_te_f32(&te, ci->Y + ci->height);
            m2_font_te_f32(&te, ci->W);
            m2_font_te_f32(&te, ci->twidth);
            m2_font_te_f32(&te, (float)(ci->offset + rth->height));
            m2_font_te_f32(&te, ci->X);
            m2_font_te_f32(&te, ci->Y + ci->height);
            m2_font_te_f32(&te, ci->W);
            m2_font_te_f32(&te, 0.0f);
            m2_font_te_f32(&te, (float)(ci->offset + rth->height));
            m2_font_store_vertex(sink, tri_ea, ci->X, ci->Y);
        }
    }
    wr32(c, ts + M2_TS_COLOR_CHANGE_CNT,
         rd32(c, ts + M2_TS_COLOR_CHANGE_CNT) + colour_changes);

    m2_font_te_sync(&te);
    m2_font_store_vertex_end(sink);
    (void)total_chars;
    (void)array_count;
    if (te.overflow) return M2_FONT_ERR_NOMEM;
    return (int32_t)te.ea;   // ts_TERet
}

// ---- the guest FontTextArray, read one field at a time ---------------------
typedef struct m2_font_array {
    uint32_t ea;                 // guest EA of this FontTextArray
    float    pen_x, pen_y;
    uint32_t fg_color, bg_color;
    float    x_scale, y_scale;
    uint32_t flags;
    float    fg_w, bg_w;
    float    clip_min_x, clip_min_y, clip_max_x, clip_max_y;
    uint32_t string_ea;
    uint32_t num_chars;
    // Bounded host copy of the string bytes so the M2 look-ahead reads
    // (skip leading '\n', peek at *string after a '\n') can never walk off the
    // end of guest memory.
    char     text[M2_FONT_MAX_CHARS + 1u];
} m2_font_array;

// Returns 1 when the array is readable and self-consistent. fta_StructSize is
// checked by the caller because its failure code differs (BADVERSION).
static int m2_font_read_array(PPCContext* c, uint32_t ea, m2_font_array* out) {
    if (ea > RECOMP_GUEST_MEM - M2_FTA_SIZE) return 0;
    memset(out, 0, sizeof(*out));
    out->ea = ea;
    out->pen_x      = rdf32(c, ea + M2_FTA_PEN_X);
    out->pen_y      = rdf32(c, ea + M2_FTA_PEN_Y);
    out->fg_color   = rd32(c, ea + M2_FTA_PEN_FG_COLOR);
    out->bg_color   = rd32(c, ea + M2_FTA_PEN_BG_COLOR);
    out->x_scale    = rdf32(c, ea + M2_FTA_PEN_X_SCALE);
    out->y_scale    = rdf32(c, ea + M2_FTA_PEN_Y_SCALE);
    out->flags      = rd32(c, ea + M2_FTA_PEN_FLAGS);
    out->fg_w       = rdf32(c, ea + M2_FTA_PEN_FG_W);
    out->bg_w       = rdf32(c, ea + M2_FTA_PEN_BG_W);
    out->clip_min_x = rdf32(c, ea + M2_FTA_CLIP_MIN_X);
    out->clip_min_y = rdf32(c, ea + M2_FTA_CLIP_MIN_Y);
    out->clip_max_x = rdf32(c, ea + M2_FTA_CLIP_MAX_X);
    out->clip_max_y = rdf32(c, ea + M2_FTA_CLIP_MAX_Y);
    out->string_ea  = rd32(c, ea + M2_FTA_STRING);
    out->num_chars  = rd32(c, ea + M2_FTA_NUM_CHARS);
    if (out->num_chars > M2_FONT_MAX_CHARS) return 0;
    // Copy at most num_chars bytes; the NUL terminator is synthesised so the
    // M2 `*string` tests are always defined.
    for (uint32_t i = 0; i < out->num_chars; ++i) {
        if (out->string_ea > RECOMP_GUEST_MEM - 1u) return 0;
        out->text[i] = (char)rd8(c, out->string_ea + i);
    }
    out->text[out->num_chars] = '\0';
    return 1;
}

// ------------------------------------------------------------ CreateTextState
// BuildRenderTable(): position every character in [first, last],
// advancing through every array/range combination and accumulating the
// TextState bounding box, exactly as the M2 implementation does.
static uint32_t m2_font_build_render_table(PPCContext* c, uint32_t ts, uint32_t fd,
                                           m2_font_array* arrays, uint32_t count,
                                           m2_font_char_info* chars,
                                           int32_t first, int32_t last) {
    const uint32_t ascent  = fd_u32(c, fd, M2_FONT_FD_ASCENT);
    const uint32_t descent = fd_u32(c, fd, M2_FONT_FD_DESCENT);
    const uint32_t height  = fd_u32(c, fd, M2_FONT_FD_CHAR_HEIGHT);
    const uint32_t leading = fd_u32(c, fd, M2_FONT_FD_LEADING);
    const uint32_t extra   = fd_u32(c, fd, M2_FONT_FD_CHAR_EXTRA);
    float top_left_x     = rdf32(c, ts + M2_TS_TOP_LEFT + 0u);
    float top_left_y     = rdf32(c, ts + M2_TS_TOP_LEFT + 4u);
    float bottom_right_x = rdf32(c, ts + M2_TS_BOTTOM_RIGHT + 0u);
    float bottom_right_y = rdf32(c, ts + M2_TS_BOTTOM_RIGHT + 4u);
    uint32_t produced = 0;

    for (uint32_t i = 0; i < count; ++i) {
        m2_font_array* a = &arrays[i];
        const char* string = a->text;
        float x_pos = a->pen_x;
        float y_pos = a->pen_y;
        const float sx = a->x_scale;
        const float sy = a->y_scale;
        const float w = (a->flags & M2_FTA_FLAG_PI_W_VALID) ? a->fg_w
                                                             : M2_FONT_DEFAULT_FGW;

        // Wholly outside the clip region: the M2 implementation discards the whole string.
        float y_bot = y_pos + (float)descent * sy;
        if (y_bot > a->clip_max_y ||
            (y_pos - (float)ascent * sy) < a->clip_min_y ||
            a->pen_x > a->clip_max_x) {
            continue;
        }
        if (y_bot < top_left_y) top_left_y = y_bot;
        if (y_bot > bottom_right_y) bottom_right_y = y_bot;

        for (uint32_t j = 0; j < a->num_chars; ++j) {
            const char letter = *string++;
            if (letter == '\n') {
                y_pos += (float)(height + leading) * sy;
                x_pos = a->pen_x;
                y_bot = y_pos + (float)descent * sy;
                if (y_bot > a->clip_max_y) break;   // nothing more in this string
                if (y_bot < top_left_y) top_left_y = y_bot;
                if (y_bot > bottom_right_y) bottom_right_y = y_bot;
                continue;
            }
            const int32_t code = (int32_t)(uint8_t)letter;
            const uint32_t char_width = (uint32_t)m2_font_char_width(c, fd, code);
            // Off the right-hand side of the clip region: drop the remainder.
            if ((x_pos + (float)char_width * sx) > a->clip_max_x) break;

            if (code >= first && code <= last && x_pos >= a->clip_min_x) {
                if (produced < M2_FONT_MAX_CHARS) {
                    m2_font_char_info* info = &chars[produced];
                    memset(info, 0, sizeof(*info));
                    info->X       = x_pos;
                    info->Y       = y_pos - (float)ascent * sy;
                    info->W       = w;
                    info->BgColor = a->bg_color;
                    info->FgColor = a->fg_color;
                    info->offset  = m2_font_texel_offset(c, fd, code, first);
                    info->width   = (float)char_width * sx;
                    info->height  = (float)height * sy;
                    info->twidth  = (float)char_width;
                    info->entry   = (uint8_t)letter;
                    ++produced;
                    if ((x_pos + info->width) < top_left_x)
                        top_left_x = x_pos + info->width;
                    if ((x_pos + info->width) > bottom_right_x)
                        bottom_right_x = x_pos + info->width;
                }
            }
            x_pos += (float)(char_width + extra) * sx;
        }
    }
    wrf32(c, ts + M2_TS_TOP_LEFT + 0u, top_left_x);
    wrf32(c, ts + M2_TS_TOP_LEFT + 4u, top_left_y);
    wrf32(c, ts + M2_TS_BOTTOM_RIGHT + 0u, bottom_right_x);
    wrf32(c, ts + M2_TS_BOTTOM_RIGHT + 4u, bottom_right_y);
    return produced;
}

// ClipRectangle(): clip a background box to the string's clip box.
static void m2_font_clip_rectangle(m2_font_bg_rect* ri, const m2_font_array* a) {
    if (ri->box[2] < a->clip_min_x || ri->box[3] < a->clip_min_y ||
        ri->box[0] > a->clip_max_x || ri->box[1] > a->clip_max_y) {
        ri->clipped = 1u;
        return;
    }
    if (ri->box[0] < a->clip_min_x) ri->box[0] = a->clip_min_x;
    // Partially off the top counts as wholly clipped.
    if (ri->box[1] < a->clip_min_y) { ri->clipped = 1u; return; }
    if (ri->box[2] > a->clip_max_x) ri->box[2] = a->clip_max_x;
    if (ri->box[3] > a->clip_max_y) { ri->clipped = 1u; return; }
}

// CalculateRectangles(): one background box per line of text that
// carries a pen_BgColor.
static uint32_t m2_font_calculate_rectangles(PPCContext* c, uint32_t fd,
                                             m2_font_array* arrays, uint32_t count,
                                             m2_font_bg_rect* rects,
                                             uint32_t rect_count) {
    const uint32_t ascent    = fd_u32(c, fd, M2_FONT_FD_ASCENT);
    const uint32_t descent   = fd_u32(c, fd, M2_FONT_FD_DESCENT);
    const uint32_t height    = fd_u32(c, fd, M2_FONT_FD_CHAR_HEIGHT);
    const uint32_t leading   = fd_u32(c, fd, M2_FONT_FD_LEADING);
    const uint32_t first_ch  = fd_u32(c, fd, M2_FONT_FD_FIRST_CHAR);
    const uint32_t last_ch   = fd_u32(c, fd, M2_FONT_FD_LAST_CHAR);
    const uint32_t extra     = fd_u32(c, fd, M2_FONT_FD_CHAR_EXTRA);
    const float line_height  = (float)(height + leading);
    uint32_t produced = 0;

    for (uint32_t i = 0; i < count; ++i) {
        m2_font_array* a = &arrays[i];
        if (a->bg_color == 0u) continue;
        float x_pos = a->pen_x;
        float y_pos = a->pen_y;
        const float sx = a->x_scale;
        const float sy = a->y_scale;
        const float w = (a->flags & M2_FTA_FLAG_PI_W_VALID) ? a->bg_w
                                                             : M2_FONT_DEFAULT_BGW;
        uint32_t j = 0;
        while (a->text[j] == '\n') { ++j; y_pos += line_height * sy; }
        char letter = 0;
        for (; j < a->num_chars; ++j) {
            letter = a->text[j];
            if (letter == '\n') {
                // No rectangle for contiguous newlines.
                if (a->text[j - 1u] != '\n' && produced < rect_count) {
                    m2_font_bg_rect* ri = &rects[produced++];
                    memset(ri, 0, sizeof(*ri));
                    ri->box[0] = a->pen_x;
                    ri->box[1] = y_pos - (float)ascent * sy;
                    ri->box[2] = x_pos - (float)extra;
                    ri->box[3] = y_pos + (float)descent * sy;
                    ri->rgb = a->bg_color;
                    ri->w = w;
                    m2_font_clip_rectangle(ri, a);
                }
                y_pos += line_height * sy;
                x_pos = a->pen_x;
            } else {
                const uint32_t ch = (uint8_t)letter;
                if (ch >= first_ch && ch <= last_ch) {
                    x_pos += (float)((uint32_t)m2_font_char_width(c, fd, (int32_t)ch) +
                                     extra) * sx;
                }
            }
        }
        // The trailing box, unless the string ended on a newline.
        if (letter != '\n' && produced < rect_count) {
            m2_font_bg_rect* ri = &rects[produced++];
            memset(ri, 0, sizeof(*ri));
            ri->box[0] = a->pen_x;
            ri->box[1] = y_pos - (float)ascent * sy;
            ri->box[2] = x_pos - (float)extra;
            ri->box[3] = y_pos + (float)descent * sy;
            ri->rgb = a->bg_color;
            ri->w = w;
            m2_font_clip_rectangle(ri, a);
        }
    }
    return produced;
}

// doCreateTextState(): the whole of CreateTextState.
static int32_t m2_font_create_text_state(PPCContext* c, uint32_t out_ts_ea,
                                         uint32_t font_item, uint32_t fta_ea,
                                         uint32_t array_count) {
    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (!r || !r->AllocMem || !r->FreeMem) return M2_FONT_ERR_NOMEM;

    // CheckItem(font, NST_FONT, FONT_FONT_NODE)
    const uint32_t fd = m2_font_item_fontdescriptor(font_item);
    if (fd == 0u) return M2_FONT_ERR_BADITEM;
    if (array_count == 0u || array_count > M2_FONT_MAX_ARRAYS ||
        fta_ea > RECOMP_GUEST_MEM - (uint64_t)array_count * M2_FTA_SIZE) {
        return M2_FONT_ERR_BADTAG;
    }

    m2_font_array* arrays =
        (m2_font_array*)calloc(array_count, sizeof(m2_font_array));
    m2_font_range* ranges = (m2_font_range*)calloc(M2_FONT_MAX_RANGES,
                                                  sizeof(m2_font_range));
    m2_font_char_info* chars =
        (m2_font_char_info*)calloc(M2_FONT_MAX_CHARS, sizeof(m2_font_char_info));
    if (!arrays || !ranges || !chars) {
        free(arrays); free(ranges); free(chars);
        return M2_FONT_ERR_NOMEM;
    }
    int32_t result = 0;
    uint32_t ts = 0;
    uint32_t te_ea = 0;
    uint32_t te_size = 0;

    uint32_t char_cnt[256];
    memset(char_cnt, 0, sizeof(char_cnt));
    uint32_t total_chars = 0;
    uint32_t different_chars = 0;
    uint32_t rect_count = 0;

    // First pass: validate every array, then count total/different characters
    // and the background rectangles the M2 implementation would emit.
    for (uint32_t i = 0; i < array_count; ++i) {
        const uint32_t ea = fta_ea + i * M2_FTA_SIZE;
        if (ea + M2_FTA_SIZE > RECOMP_GUEST_MEM) { result = M2_FONT_ERR_BADTAG; goto done; }
        if (rd32(c, ea + M2_FTA_STRUCT_SIZE) != M2_FTA_SIZE) {
            result = M2_FONT_ERR_BADVERSION;
            goto done;
        }
        if (!m2_font_read_array(c, ea, &arrays[i])) { result = M2_FONT_ERR_BADTAG; goto done; }
        if (total_chars + arrays[i].num_chars > M2_FONT_MAX_CHARS) {
            result = M2_FONT_ERR_NOMEM;
            goto done;
        }
        total_chars += arrays[i].num_chars;

        uint32_t j = 0;
        while (arrays[i].text[j] == '\n') ++j;
        if (arrays[i].text[j] == '\0') continue;

        if (arrays[i].bg_color != 0u) ++rect_count;
        char letter = '\0';
        for (; j < arrays[i].num_chars; ++j) {
            letter = arrays[i].text[j];
            if (letter == '\n' && arrays[i].text[j + 1u] != '\n' &&
                arrays[i].bg_color != 0u) {
                ++rect_count;
            }
            const uint32_t ch = (uint8_t)letter;
            if (char_cnt[ch] == 0u) ++different_chars;
            ++char_cnt[ch];
        }
        if (letter == '\n' && arrays[i].bg_color != 0u) --rect_count;
    }
    (void)different_chars;
    if (rect_count > M2_FONT_MAX_RECTS) { result = M2_FONT_ERR_NOMEM; goto done; }

    // An empty FontTextArray still clears *ts and reports success.
    if (total_chars == 0u) {
        if (out_ts_ea != 0u && out_ts_ea < RECOMP_GUEST_MEM - 3u)
            wr32(c, out_ts_ea, 0u);
        goto done;
    }

    // Allocate the TextState: the M2 header plus a vertexInfo per
    // rendered character and per background rectangle.
    const uint32_t vertex_slots = total_chars + rect_count;
    const uint32_t ts_size = M2_TS_HEADER_SIZE + vertex_slots * M2_VERTEX_INFO_SIZE;
    ts = (uint32_t)(uintptr_t)r->AllocMem((int32_t)ts_size, 0u);
    if (ts == 0u) { result = M2_FONT_ERR_NOMEM; goto done; }
    for (uint32_t o = 0; o + 3u < ts_size; o += 4u) wr32(c, ts + o, 0u);

    const uint32_t ascent  = fd_u32(c, fd, M2_FONT_FD_ASCENT);
    const uint32_t descent = fd_u32(c, fd, M2_FONT_FD_DESCENT);
    wr32(c, ts + M2_TS_FONT, fd);
    wr32(c, ts + M2_TS_COOKIE, ts);
    wr32(c, ts + M2_TS_SIZE, ts_size);
    wrf32(c, ts + M2_TS_TOP_LEFT + 0u, arrays[0].pen_x);
    wrf32(c, ts + M2_TS_TOP_LEFT + 4u, arrays[0].pen_y - (float)ascent);
    // Seed max.x the way the M2 implementation does so the first character widens it.
    wrf32(c, ts + M2_TS_BOTTOM_RIGHT + 0u, arrays[0].pen_x + 1.0f);
    wrf32(c, ts + M2_TS_BOTTOM_RIGHT + 4u, arrays[0].pen_y + (float)descent);
    wr32(c, ts + M2_TS_BG_COUNT, rect_count);
    // PrepList(&ts_ClipList): Head == Tail == &list.
    wr32(c, ts + M2_TS_CLIP_LIST + 0u, ts + M2_TS_CLIP_LIST);
    wr32(c, ts + M2_TS_CLIP_LIST + 4u, ts + M2_TS_CLIP_LIST);
    wr32(c, ts + M2_TS_NEXT_CLIP, ts + M2_TS_CLIP_LIST);
    if (out_ts_ea != 0u && out_ts_ea < RECOMP_GUEST_MEM - 3u)
        wr32(c, out_ts_ea, ts);

    // sizes the TE list from the same terms the M2 uses.
    const uint32_t texture_size = 16u * 4u;   // sizeof(TextureLoadList)
    const uint32_t init_size    = 18u * 4u;   // sizeof(InitList)
    const uint32_t dblend_size  = 5u * 4u;    // sizeof(DBLendList)
    const uint32_t ret_size     = 3u * 4u;    // JumpAbsolute
    const uint32_t sync_size    = 4u * 4u;    // two syncs
    const uint32_t char_size    = 21u * 4u;   // CHAR_SIZE
    const uint32_t colour_size  = 11u * 4u;   // COLOUR_SIZE per array*range
    // RECT_SIZE(rectCount) = sizeof(NoTextureList) (8 words) +
    // sizeof(prllist) (6 words) - both emitted once - plus 29 words per
    // rectangle: one triangle command and four 7-word shaded vertices.
    const uint32_t rect_size    = (14u + 29u * rect_count) * 4u;

    // Background rectangles, computed so ts_bgCount, the vertexInfo slots and
    // the TE size match the M2 for any caller.
    m2_font_bg_rect* rects = NULL;
    uint32_t rects_made = 0;
    if (rect_count) {
        rects = (m2_font_bg_rect*)calloc(rect_count, sizeof(m2_font_bg_rect));
        if (!rects) { result = M2_FONT_ERR_NOMEM; goto done; }
        rects_made = m2_font_calculate_rectangles(c, fd, arrays, array_count,
                                                  rects, rect_count);
        if (rects_made != rect_count) { result = M2_FONT_ERR_NOMEM; goto done; }
    }

    // Build the TRAM ranges: find the first character to render, then walk up
    // to fd_lastChar, splitting on fd_rangeGap / fd_maxCharsLoad.
    const uint32_t fd_first    = fd_u32(c, fd, M2_FONT_FD_FIRST_CHAR);
    const uint32_t fd_last     = fd_u32(c, fd, M2_FONT_FD_LAST_CHAR);
    const uint32_t range_gap   = fd_u32(c, fd, M2_FONT_FD_RANGE_GAP);
    const uint32_t max_load    = fd_u32(c, fd, M2_FONT_FD_MAX_CHARS_LOAD);
    const uint32_t row_bytes   = fd_u32(c, fd, M2_FONT_FD_BYTES_PER_ROW);
    const uint32_t char_height = fd_u32(c, fd, M2_FONT_FD_CHAR_HEIGHT);
    const uint32_t bpp         = fd_u32(c, fd, M2_FONT_FD_BITS_PER_PIXEL);

    int32_t first = -1;
    int32_t last  = 256;
    for (uint32_t i = fd_first; i <= fd_last; ++i) {
        if (char_cnt[i] > 0u) {
            last = first = (int32_t)i;
            break;
        }
    }

    uint32_t range_count = 0;
    uint32_t chars_used = 0;
    for (uint32_t i = (uint32_t)(first + 1); i <= fd_last && fd_last > fd_first; ++i) {
        if (char_cnt[i] == 0u) continue;
        const int32_t next = (int32_t)i;
        if (((uint32_t)(next - last) > range_gap ||
             (uint32_t)(next - first) >= max_load) &&
            range_count < M2_FONT_MAX_RANGES) {
            m2_font_range* range = &ranges[range_count];
            range->hdr.texel = m2_font_texel_address(c, fd, first,
                                                     &range->hdr.offset);
            range->hdr.height = char_height;
            range->hdr.bpp = bpp;
            range->hdr.minChar = (uint8_t)(uint8_t)(uint32_t)first;
            range->hdr.maxChar = (uint8_t)(uint8_t)(uint32_t)last;
            range->hdr.bytesToLoad =
                row_bytes * ((uint32_t)(last - first) + 1u) * char_height;
            range->first = chars_used;
            range->hdr.charCnt = m2_font_build_render_table(
                c, ts, fd, arrays, array_count, chars + chars_used, first, last);
            chars_used += range->hdr.charCnt;
            ++range_count;
            first = last = next;
        } else {
            last = next;
        }
    }
    if (range_count < M2_FONT_MAX_RANGES) {
        m2_font_range* range = &ranges[range_count];
        range->hdr.texel = m2_font_texel_address(c, fd, first, &range->hdr.offset);
        range->hdr.height = char_height;
        range->hdr.bpp = bpp;
        range->hdr.minChar = (uint8_t)(uint8_t)(uint32_t)first;
        range->hdr.maxChar = (uint8_t)(uint8_t)(uint32_t)last;
        range->hdr.bytesToLoad =
            row_bytes * ((uint32_t)(last - first) + 1u) * char_height;
        range->first = chars_used;
        range->hdr.charCnt = m2_font_build_render_table(
            c, ts, fd, arrays, array_count, chars + chars_used, first, last);
        chars_used += range->hdr.charCnt;
        ++range_count;
    }

    te_size = total_chars * char_size +
              range_count * texture_size +
              array_count * range_count * colour_size +
              rect_size +
              init_size + dblend_size + ret_size + sync_size;
    if (te_size > (1u << 20)) { result = M2_FONT_ERR_NOMEM; goto done; }
    te_ea = (uint32_t)(uintptr_t)r->AllocMem((int32_t)te_size, 0u);
    if (te_ea == 0u) { result = M2_FONT_ERR_NOMEM; goto done; }
    wr32(c, ts + M2_TS_TE_LIST, te_ea);
    wr32(c, ts + M2_TS_TE_SIZE, te_size);

    m2_font_vertex_sink sink;
    sink.c = c;
    sink.ea = ts + M2_TS_HEADER_SIZE;
    sink.slots = vertex_slots;
    sink.used = 0;
    sink.origin_x = arrays[0].pen_x;
    sink.origin_y = arrays[0].pen_y;
    wr32(c, ts + M2_TS_VERTEX, sink.ea);

    // CreateTELists returns the guest EA of the reserved JumpAbsolute slot on
    // success and a negative Err on failure. Keep it out of `result`, which is
    // strictly the Err, so the cleanup below cannot mistake a positive EA for
    // one.
    const int32_t te_ret = m2_font_create_te_lists(c, ts, te_ea, te_size,
                                                   range_count, total_chars,
                                                   array_count, ranges, chars,
                                                   rects, rect_count, &sink);
    free(rects);
    if (te_ret < 0) { result = te_ret; goto done; }
    wr32(c, ts + M2_TS_TE_RET, (uint32_t)te_ret);

    if (getenv("M2_FONT_DIAG"))
        fprintf(stderr, "[font] ranges=%u charCnt0=%u texel0=%#x\n",
                range_count, ranges[0].hdr.charCnt, ranges[0].hdr.texel);
    // Complete the bounding box (tail).
    const float top_left_x      = rdf32(c, ts + M2_TS_TOP_LEFT + 0u);
    const float top_left_y      = rdf32(c, ts + M2_TS_TOP_LEFT + 4u);
    const float bottom_left_y   = rdf32(c, ts + M2_TS_BOTTOM_RIGHT + 4u);
    const float bottom_right_x  = rdf32(c, ts + M2_TS_BOTTOM_RIGHT + 0u);
    const float baseline_left_y = top_left_y + (float)ascent;
    wrf32(c, ts + M2_TS_TOP_RIGHT + 0u, bottom_right_x);
    wrf32(c, ts + M2_TS_TOP_RIGHT + 4u, top_left_y);
    wrf32(c, ts + M2_TS_BOTTOM_LEFT + 0u, top_left_x);
    wrf32(c, ts + M2_TS_BOTTOM_LEFT + 4u, bottom_left_y);
    wrf32(c, ts + M2_TS_BASELINE_LEFT + 0u, top_left_x);
    wrf32(c, ts + M2_TS_BASELINE_LEFT + 4u, baseline_left_y);
    wrf32(c, ts + M2_TS_BASELINE_RIGHT + 0u, bottom_right_x);
    wrf32(c, ts + M2_TS_BASELINE_RIGHT + 4u, baseline_left_y);
    wrf32(c, ts + M2_TS_BASELINE_ORIG_LEN + 0u, bottom_right_x - top_left_x);
    wrf32(c, ts + M2_TS_BASELINE_ORIG_LEN + 4u, baseline_left_y - top_left_y);

done:
    if (result != 0 && getenv("M2_FONT_DIAG"))
        fprintf(stderr, "[font] CTS err=%#x\n",
                (uint32_t)result);
    if (result != 0) {
        if (te_ea) r->FreeMem((void*)(uintptr_t)te_ea, (int32_t)te_size);
        if (ts) r->FreeMem((void*)(uintptr_t)ts, (int32_t)ts_size);
        if (out_ts_ea != 0u && out_ts_ea < RECOMP_GUEST_MEM - 3u)
            wr32(c, out_ts_ea, 0u);
    }
    free(arrays); free(ranges); free(chars);
    return result;
}

// ------------------------------------------------------------ DeleteTextState
static int32_t m2_font_delete_text_state(PPCContext* c, uint32_t ts) {
    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (!r || !r->FreeMem) return M2_FONT_ERR_NOMEM;
    // The M2 BUILD_PARANOIA CheckTextState, enforced here because the
    // lifted DeleteTextState call site passes whatever the previous frame left
    // in the caller's table.
    if (ts == 0u || ts > RECOMP_GUEST_MEM - M2_TS_HEADER_SIZE ||
        rd32(c, ts + M2_TS_COOKIE) != ts) {
        return M2_FONT_ERR_BADTS;
    }
    const uint32_t ts_size = rd32(c, ts + M2_TS_SIZE);
    if (ts_size < M2_TS_HEADER_SIZE || ts_size > RECOMP_GUEST_MEM - ts) {
        return M2_FONT_ERR_BADTS;
    }
    // ts_ClipList stays empty (CreateTextState preps it and nothing in this
    // folio populates it), so there are no ClippedVertices to free.
    const uint32_t te_ea   = rd32(c, ts + M2_TS_TE_LIST);
    const uint32_t te_size = rd32(c, ts + M2_TS_TE_SIZE);
    if (te_ea != 0u && te_size != 0u && te_ea <= RECOMP_GUEST_MEM - te_size) {
        r->FreeMem((void*)(uintptr_t)te_ea, (int32_t)te_size);
    }
    r->FreeMem((void*)(uintptr_t)ts, (int32_t)ts_size);
    return 0;
}

// ------------------------------------------------------------------- DrawText
static int32_t m2_font_draw_text(PPCContext* c, uint32_t gs, uint32_t ts,
                                 uint32_t pc) {
    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (!c || !c->mem) return M2_FONT_ERR_BADPTR_HOST;
    if (gs == 0u || gs > RECOMP_GUEST_MEM - M2_GS_LIST_PTR - 4u)
        return M2_FONT_ERR_BADPTR_HOST;
    if (ts == 0u || ts > RECOMP_GUEST_MEM - M2_TS_HEADER_SIZE ||
        rd32(c, ts + M2_TS_COOKIE) != ts) {
        return M2_FONT_ERR_BADTS;
    }
    const uint32_t te_ea   = rd32(c, ts + M2_TS_TE_LIST);
    const uint32_t ret_slot= rd32(c, ts + M2_TS_TE_RET);
    const uint32_t dblend  = rd32(c, ts + M2_TS_DBLEND);
    if (te_ea == 0u || te_ea > RECOMP_GUEST_MEM - 8u ||
        ret_slot == 0u || ret_slot > RECOMP_GUEST_MEM - 12u ||
        dblend == 0u || dblend > RECOMP_GUEST_MEM - 16u) {
        return M2_FONT_ERR_BADTS;
    }

    // GS_GetDestBuffer(gs): the driving image's own GState routine, so the
    // GState bookkeeping stays guest-owned.
    c->gpr[3] = gs;
    if (!recomp_call_gstate_dispatch(c, 9u, pc)) return M2_FONT_ERR_BADPTR_HOST;
    const uint32_t bm_item = c->gpr[3];
    if ((int32_t)bm_item < 0) return (int32_t)bm_item;

    // LookupItem(bm_item): the host's published Bitmap object.
    c->gpr[3] = bm_item;
    if (!recomp_call_kernel_dispatch(c, M2_FOLIO_KERNEL, M2_KSEL_LOOKUP_ITEM,
                                     pc, r)) {
        return M2_FONT_ERR_BADPTR_HOST;
    }
    const uint32_t bm = c->gpr[3];
    if (bm == 0u || bm > RECOMP_GUEST_MEM - M2_BM_TYPE - 4u)
        return M2_FONT_ERR_BADPTR_HOST;

    // DrawText(): the dither width is the destination stride and the
    // source base is the destination buffer itself.
    wr32(c, dblend + 4u,
         (rd32(c, bm + M2_BM_TYPE) == M2_BMTYPE_32)
             ? m2_clt_dbsrccntl(0, 1)
             : m2_clt_dbsrccntl(1, 0));
    wr32(c, dblend + 8u, rd32(c, bm + M2_BM_BUFFER));
    wr32(c, dblend + 12u, rd32(c, bm + M2_BM_WIDTH));

    // GS_Reserve(gs, 3), again through the guest's own routine.
    c->gpr[3] = gs;
    c->gpr[4] = 3u;
    if (!recomp_call_gstate_dispatch(c, 6u, pc)) return M2_FONT_ERR_BADPTR_HOST;

    const uint32_t list = rd32(c, gs + M2_GS_LIST_PTR);
    if (list == 0u || list > RECOMP_GUEST_MEM - 32u)
        return M2_FONT_ERR_BADPTR_HOST;
    m2_font_te te = { c, list, RECOMP_GUEST_MEM, 0 };
    m2_font_te_sync(&te);
    m2_font_te_word(&te, m2_clt_write_header(M2_CLT_REG_DCNTLDATA, 2));
    m2_font_te_word(&te, te_ea);
    m2_font_te_word(&te, m2_clt_bits(M2_CLT_FIELD_DCNTL_JA, 1));
    const uint32_t after_jump = te.ea;
    wr32(c, gs + M2_GS_LIST_PTR, after_jump);

    // Return jump, written into the TextState's reserved slot.
    wr32(c, ret_slot + 0u,
         m2_clt_write_header(M2_CLT_REG_DCNTLDATA, 2));
    wr32(c, ret_slot + 4u, after_jump);
    wr32(c, ret_slot + 8u, m2_clt_bits(M2_CLT_FIELD_DCNTL_JA, 1));

    // ts_NextClip = FirstNode(&ts_ClipList)
    wr32(c, ts + M2_TS_NEXT_CLIP, rd32(c, ts + M2_TS_CLIP_LIST));
    return 0;
}

// -------------------------------------------------- kernel:133 FindAndOpenItem
// LoadFont(): try the name as given in the current directory, then
// System.m2/Fonts/<name>.font, exactly as the M2 implementation does.
static uint32_t m2_font_open_image(PPCContext* c, const char* name,
                                   uint32_t* out_size) {
    *out_size = 0u;
    if (!m2_host_raw_open || !m2_host_raw_size || !m2_host_raw_read) return 0u;
    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (!r || !r->AllocMem) return 0u;

    uint32_t token = m2_host_raw_open(name);
    if (token == 0u) {
        char alt[96];
        snprintf(alt, sizeof(alt), "System.m2/Fonts/%.26s.font", name);
        token = m2_host_raw_open(alt);
    }
    if (token == 0u) return 0u;

    const uint32_t size = m2_host_raw_size(token);
    if (size < M2_FONT_HDR_SIZE || size > M2_FONT_MAX_IMAGE) {
        if (m2_host_raw_close) m2_host_raw_close(token);
        return 0u;
    }
    const uint32_t ea = (uint32_t)(uintptr_t)r->AllocMem((int32_t)size, 0u);
    if (ea == 0u) {
        if (m2_host_raw_close) m2_host_raw_close(token);
        return 0u;
    }
    const int32_t got = m2_host_raw_read(token, c->mem + ea, size);
    if (m2_host_raw_close) m2_host_raw_close(token);
    if (got != (int32_t)size) {
        r->FreeMem((void*)(uintptr_t)ea, (int32_t)size);
        return 0u;
    }
    *out_size = size;
    return ea;
}

// The kernel:133 handler body. Signature matches m2fn_FindAndOpenItem_t so the
// typed dispatch in recomp_helpers.h can install it in the shared registry.
uint32_t m2_font_find_and_open_item(int32_t ctype, uint32_t taglist_ea) {
    PPCContext* c = m2_guest_kernel_get_active_context();
    if (!c || !c->mem) return (uint32_t)M2_FONT_ERR_NOTFOUND;
    // internalFindAndOpenItem: WhichFolio(SUBSYSPART(cntype)) is null (or the
    // node type is out of range) for anything the font folio does not claim,
    // and that is MAKEKERR(ER_SEVERE, ER_C_NSTND, ER_Kr_BadType).
    if ((uint32_t)ctype != M2_FONT_CTYPE) return (uint32_t)M2_KERNEL_ERR_BAD_TYPE;

    const m2_kernel_dispatch* r = m2_guest_kernel_handlers();
    if (!r || !r->AllocMem || !r->FreeMem || !r->CreateItem)
        return (uint32_t)M2_FONT_ERR_NOTFOUND;

    // FindTagArg(tags, TAG_ITEM_NAME)
    uint32_t name_ea = 0u;
    if (taglist_ea != 0u && taglist_ea < RECOMP_GUEST_MEM - 7u) {
        for (uint32_t i = 0; i < 64u; ++i) {
            const uint32_t ea = taglist_ea + i * 8u;
            if (ea + 8u > RECOMP_GUEST_MEM) break;
            const uint32_t tag = rd32(c, ea);
            if (tag == 0u /* TAG_END */ || tag == 0xffffffffu) break;
            if (tag == 1u /* TAG_ITEM_NAME */) name_ea = rd32(c, ea + 4u);
        }
    }
    if (name_ea == 0u) return (uint32_t)M2_FONT_ERR_BADTAG;

    char name[M2_FONT_MAX_NAME];
    if (m2m_copy_guest_string_ctx(c, name_ea, name, sizeof(name)) < 0)
        return (uint32_t)M2_FONT_ERR_BADTAG;

    // internalFindItem(cntype, tags): a font already loaded under the same name
    // is found in memory, so only internalOpenItem runs.
    for (uint32_t i = 0; i < M2_FONT_MAX_FONTS; ++i) {
        m2_font_slot* slot = &g_font_slots[i];
        if (!slot->used || slot->name_ea == 0u) continue;
        char existing[M2_FONT_MAX_NAME];
        if (m2m_copy_guest_string_ctx(c, slot->name_ea, existing,
                                      sizeof(existing)) < 0) continue;
        if (strcmp(existing, name) != 0) continue;
        const uint32_t open = slot->desc_ea + M2_FONT_FD_N_OPENCOUNT;
        wr32(c, open, rd32(c, open) + 1u);
        return slot->item;
    }

    // ir_Load(ntype, tags) -> LoadFont()
    m2_font_slot* slot = NULL;
    for (uint32_t i = 0; i < M2_FONT_MAX_FONTS; ++i) {
        if (!g_font_slots[i].used) { slot = &g_font_slots[i]; break; }
    }
    if (!slot) return (uint32_t)M2_FONT_ERR_NOMEM;

    uint32_t image_size = 0u;
    const uint32_t image_ea = m2_font_open_image(c, name, &image_size);
    if (image_ea == 0u) return (uint32_t)M2_FONT_ERR_NOTFOUND;

    const uint32_t name_copy =
        (uint32_t)(uintptr_t)r->AllocMem((int32_t)M2_FONT_MAX_NAME, 0u);
    if (name_copy == 0u) {
        r->FreeMem((void*)(uintptr_t)image_ea, (int32_t)image_size);
        return (uint32_t)M2_FONT_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < M2_FONT_MAX_NAME; ++i)
        wr8(c, name_copy + i, (i < sizeof(name)) ? (uint8_t)name[i] : 0u);

    const uint32_t desc_ea =
        (uint32_t)(uintptr_t)r->AllocMem((int32_t)M2_FD_SIZE, 0u);
    if (desc_ea == 0u) {
        r->FreeMem((void*)(uintptr_t)name_copy, (int32_t)M2_FONT_MAX_NAME);
        r->FreeMem((void*)(uintptr_t)image_ea, (int32_t)image_size);
        return (uint32_t)M2_FONT_ERR_NOMEM;
    }
    for (uint32_t o = 0; o + 3u < M2_FD_SIZE; o += 4u) wr32(c, desc_ea + o, 0u);
    wr8(c, desc_ea + M2_FONT_FD_SUBSYS_TYPE, (uint8_t)M2_FONT_NST);
    wr8(c, desc_ea + M2_FONT_FD_NODE_TYPE, (uint8_t)M2_FONT_FONT_NODE);
    wr32(c, desc_ea + M2_FONT_FD_N_SIZE, M2_FD_SIZE);
    wr32(c, desc_ea + M2_FONT_FD_NAME, name_copy);
    wr32(c, desc_ea + M2_FONT_FD_N_OPENCOUNT, 1u);   // internalOpenItem
    wr32(c, desc_ea + M2_FONT_FD_OPEN_COUNT, 1u);

    const int32_t parsed = m2_font_parse(c, desc_ea, image_ea, image_size);
    if (parsed != 0) {
        r->FreeMem((void*)(uintptr_t)desc_ea, (int32_t)M2_FD_SIZE);
        r->FreeMem((void*)(uintptr_t)name_copy, (int32_t)M2_FONT_MAX_NAME);
        r->FreeMem((void*)(uintptr_t)image_ea, (int32_t)image_size);
        return (uint32_t)parsed;
    }

    // CreateFolioItem(): the real Item comes from the shared kernel item pool.
    const uint32_t item = r->CreateItem((uint32_t)ctype, taglist_ea);
    if (item == 0u) {
        r->FreeMem((void*)(uintptr_t)desc_ea, (int32_t)M2_FD_SIZE);
        r->FreeMem((void*)(uintptr_t)name_copy, (int32_t)M2_FONT_MAX_NAME);
        r->FreeMem((void*)(uintptr_t)image_ea, (int32_t)image_size);
        return (uint32_t)M2_FONT_ERR_BADITEM;
    }
    wr32(c, desc_ea + M2_FONT_FD_N_ITEM, item);

    slot->used = 1u;
    slot->item = item;
    slot->name_ea = name_copy;
    slot->image_ea = image_ea;
    slot->image_size = image_size;
    slot->desc_ea = desc_ea;
    return item;
}

void m2_font_release_item(uint32_t item) {
    m2_font_slot_release(m2_font_find(item));
}

// ------------------------------------------------------------- folio dispatch
static int m2_font_dispatch_impl(PPCContext* c, uint32_t ordinal, uint32_t pc) {
    if (!c || !c->mem) return 1;   // typed path; leave the guest's gpr[3] alone
    // Ownership is m2_font_selector_owned's set (font_folio.h), which the boot
    // census consults too; the switch below only serves owned ordinals.
    if (!m2_font_selector_owned(ordinal)) return 0;
    switch (ordinal) {
    case M2_FONT_ORD_CREATE_TEXT_STATE:
        // Err CreateTextState(TextState **ts, Item font,
        //                     const FontTextArray *fta, uint32 arrayCount);
        c->gpr[3] = (uint32_t)m2_font_create_text_state(
            c, c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]);
        return 1;
    case M2_FONT_ORD_DELETE_TEXT_STATE:
        // Err DeleteTextState(const TextState *ts);
        c->gpr[3] = (uint32_t)m2_font_delete_text_state(c, c->gpr[3]);
        return 1;
    case M2_FONT_ORD_DRAW_TEXT:
        // Err DrawText(GState *gs, TextState *ts);
        c->gpr[3] = (uint32_t)m2_font_draw_text(c, c->gpr[3], c->gpr[4], pc);
        return 1;
    default:
        return 0;
    }
}

// Public entry point; see the filesystem/gstate dispatch wrappers
// (selector_observed.h).
int recomp_call_font_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc) {
    const int handled = m2_font_dispatch_impl(c, ordinal, pc);
    m2_selector_note(M2_FOLIO_FONT, ordinal, handled);
    return handled;
}