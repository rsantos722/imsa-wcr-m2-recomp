// font_folio.h — 32-bit M2 font folio interface facts.
//
// These are the ordinals, node identities, error words, and guest structure
// offsets the host needs to read the font objects the game owns and to build
// the Triangle Engine list for DrawText.
//
// Every offset is the 32-bit big-endian M2 layout (four bytes per word). Host
// structs must never be memcpy'd into guest RAM: the host is LP64 while the
// guest is 32-bit, so these offsets are the contract and accessors are
// rd32/wr32/rdf32/wrf32 on the shared guest image.
//
// The values are interface facts for the M2 font folio and are checked against
// the recovered guest image.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- folio 9 node identity --------------------------------------------
#define M2_FONT_NST 6u
#define M2_FONT_FONT_NODE 1u
// Guest cType for a font node, passed to kernel FindAndOpenItem.
#define M2_FONT_MKNODEID(node_type) (((uint32_t)M2_FONT_NST << 8) | (node_type))
#define M2_FONT_CTYPE M2_FONT_MKNODEID(M2_FONT_FONT_NODE)

// --- folio 9 ordinals -------------------------------------------------
#define M2_FONT_ORD_CREATE_TEXT_STATE 0u
#define M2_FONT_ORD_DELETE_TEXT_STATE 1u
#define M2_FONT_ORD_DRAW_TEXT 2u

// Ownership predicate for folio 9, in the dispatch's own terms: these are the
// ordinals m2_font_dispatch_impl claims. Returns int (not bool) because this
// header is C and only includes <stdint.h>.
static inline int m2_font_selector_owned(uint32_t o) {
    return o == M2_FONT_ORD_CREATE_TEXT_STATE ||
           o == M2_FONT_ORD_DELETE_TEXT_STATE ||
           o == M2_FONT_ORD_DRAW_TEXT;
}

// --- error codes -----------------------------------------------------
// M2 error words encode the error identity, severity, class, and object in a
// single 32-bit value. These are the font folio values used by the item and
// text-state paths.
#define M2_FONT_ERR_BADTAG ((int32_t)0xd5567002u)
#define M2_FONT_ERR_NOMEM ((int32_t)0xd5567006u)
#define M2_FONT_ERR_NOTFOUND ((int32_t)0xd5567005u)
#define M2_FONT_ERR_BADITEM ((int32_t)0xd5567001u)
#define M2_FONT_ERR_BADSUBTYPE ((int32_t)0xd5567007u)
#define M2_FONT_ERR_BADFONTFILE ((int32_t)0xd5567101u)
#define M2_FONT_ERR_BADVERSION ((int32_t)0xd5567102u)
#define M2_FONT_ERR_BADBPP ((int32_t)0xd5567103u)
#define M2_FONT_ERR_BADTS ((int32_t)0xd5567104u)
// Refusal returned by the item loader for a type no folio claims.
#define M2_KERNEL_ERR_BAD_TYPE ((int32_t)0xd57ed101u)

// Host-side refusal for a GState/Item pointer the folio cannot resolve through
// the typed boundary. DrawText returns it instead of writing through an
// unvalidated pointer.
#define M2_FONT_ERR_BADPTR_HOST ((int32_t)0xd5567108u)

// --- 32-bit font file header -----------------------------------------
// FORM header, FONT chunk header, FontFileDesc (14 words), and 8 uint32.
#define M2_FONT_HDR_FORM_LABEL 0x00u
#define M2_FONT_HDR_FORM_SIZE 0x04u
#define M2_FONT_HDR_FORM_ID 0x08u
#define M2_FONT_HDR_CHUNK_ID 0x0cu
#define M2_FONT_HDR_CHUNK_SIZE 0x10u
#define M2_FONT_HDR_VERSION 0x14u  // uint8
#define M2_FONT_HDR_REVISION 0x15u // uint8
#define M2_FONT_HDR_CHUNK_VERSION 0x16u // int16
#define M2_FONT_HDR_DESC 0x18u          // FontFileDesc, 14 words
#define M2_FONT_HDR_CHAR_WIDTH 0x50u
#define M2_FONT_HDR_CHAR_HEIGHT 0x54u
#define M2_FONT_HDR_CHAR_ROW_BYTES 0x58u
#define M2_FONT_HDR_CHAR_SIZE 0x5cu
#define M2_FONT_HDR_WTABLE_OFFSET 0x60u
#define M2_FONT_HDR_WTABLE_SIZE 0x64u
#define M2_FONT_HDR_DATA_OFFSET 0x68u
#define M2_FONT_HDR_DATA_SIZE 0x6cu
#define M2_FONT_HDR_SIZE 0x80u
// FontFileDesc word indices relative to M2_FONT_HDR_DESC. The file descriptor
// and the runtime FontDescriptor below are different objects; keep their
// names distinct.
#define M2_FONT_DESC_BPP 2u
#define M2_FONT_DESC_ASCENT 7u
#define M2_FONT_DESC_DESCENT 8u
#define M2_FONT_DESC_SPACING 9u
#define M2_FONT_DESC_LEADING 10u
#define M2_FONT_DESC_FIRST_CHAR 11u
#define M2_FONT_DESC_LAST_CHAR 12u
#define M2_FONT_HDR_CHUNK_VERSION_VALUE 4 // font file chunk version

// --- 32-bit opening item node ----------------------------------------
#define M2_FONT_FD_NEXT 0x00u
#define M2_FONT_FD_PREV 0x04u
#define M2_FONT_FD_SUBSYS_TYPE 0x08u // uint8
#define M2_FONT_FD_NODE_TYPE 0x09u   // uint8
#define M2_FONT_FD_PRIORITY 0x0au    // uint8
#define M2_FONT_FD_FLAGS 0x0bu       // uint8
#define M2_FONT_FD_N_SIZE 0x0cu
#define M2_FONT_FD_NAME 0x10u
#define M2_FONT_FD_N_VERSION 0x14u // uint8
#define M2_FONT_FD_N_REVISION 0x15u // uint8
#define M2_FONT_FD_N_RESERVED0 0x16u // uint8
#define M2_FONT_FD_N_ITEMFLAGS 0x17u // uint8
#define M2_FONT_FD_N_ITEM 0x18u
#define M2_FONT_FD_N_OWNER 0x1cu
#define M2_FONT_FD_N_RESERVED1 0x20u
#define M2_FONT_FD_N_OPENCOUNT 0x24u
#define M2_FONT_FD_NODE_SIZE 0x28u

// --- 32-bit font descriptor ------------------------------------------
#define M2_FONT_FD_OPEN_COUNT 0x28u
#define M2_FONT_FD_FONT_FLAGS 0x2cu
#define M2_FONT_FD_CHAR_HEIGHT 0x30u
#define M2_FONT_FD_CHAR_WIDTH 0x34u
#define M2_FONT_FD_BITS_PER_PIXEL 0x38u
#define M2_FONT_FD_FIRST_CHAR 0x3cu
#define M2_FONT_FD_LAST_CHAR 0x40u
#define M2_FONT_FD_CHAR_EXTRA 0x44u
#define M2_FONT_FD_ASCENT 0x48u
#define M2_FONT_FD_DESCENT 0x4cu
#define M2_FONT_FD_LEADING 0x50u
#define M2_FONT_FD_BYTES_PER_ROW 0x54u
#define M2_FONT_FD_FONT_HEADER 0x58u
#define M2_FONT_FD_CHAR_INFO 0x5cu
#define M2_FONT_FD_CHAR_DATA 0x60u
#define M2_FONT_FD_MAX_CHARS_LOAD 0x64u
#define M2_FONT_FD_RANGE_GAP 0x68u
// Total FontDescriptor size: the 40-byte OpeningItemNode plus 17 words.
#define M2_FD_SIZE 0x6cu

// --- 32-bit font text array, 68 bytes --------------------------------
#define M2_FTA_STRUCT_SIZE 0x00u
#define M2_FTA_PEN_X 0x04u
#define M2_FTA_PEN_Y 0x08u
#define M2_FTA_PEN_FG_COLOR 0x0cu
#define M2_FTA_PEN_BG_COLOR 0x10u
#define M2_FTA_PEN_X_SCALE 0x14u
#define M2_FTA_PEN_Y_SCALE 0x18u
#define M2_FTA_PEN_FLAGS 0x1cu
#define M2_FTA_PEN_FG_W 0x20u
#define M2_FTA_PEN_BG_W 0x24u
#define M2_FTA_PEN_RESERVED 0x28u
#define M2_FTA_CLIP_MIN_X 0x2cu
#define M2_FTA_CLIP_MIN_Y 0x30u
#define M2_FTA_CLIP_MAX_X 0x34u
#define M2_FTA_CLIP_MAX_Y 0x38u
#define M2_FTA_STRING 0x3cu
#define M2_FTA_NUM_CHARS 0x40u
#define M2_FTA_SIZE 0x44u
// Pen flag: pen_fgW and pen_bgW are valid.
#define M2_FTA_FLAG_PI_W_VALID 1u

// --- 32-bit text state -----------------------------------------------
#define M2_TS_FONT 0x00u
#define M2_TS_TE_LIST 0x04u
#define M2_TS_TE_SIZE 0x08u
#define M2_TS_TE_RET 0x0cu
#define M2_TS_COOKIE 0x10u
#define M2_TS_SIZE 0x14u
#define M2_TS_TOP_LEFT 0x18u
#define M2_TS_TOP_RIGHT 0x20u
#define M2_TS_BOTTOM_LEFT 0x28u
#define M2_TS_BOTTOM_RIGHT 0x30u
#define M2_TS_BASELINE_LEFT 0x38u
#define M2_TS_BASELINE_RIGHT 0x40u
#define M2_TS_BASELINE_ORIG_LEN 0x48u
#define M2_TS_TRACK 0x50u // uint8
#define M2_TS_CLIP 0x51u  // uint8
#define M2_TS_LEFTMOST 0x54u
#define M2_TS_RIGHTMOST 0x58u
#define M2_TS_TOPMOST 0x5cu
#define M2_TS_BOTTOMMOST 0x60u
#define M2_TS_LEFT_EDGE 0x64u
#define M2_TS_RIGHT_EDGE 0x68u
#define M2_TS_TOP_EDGE 0x6cu
#define M2_TS_BOTTOM_EDGE 0x70u
#define M2_TS_BG_COUNT 0x74u
#define M2_TS_COLOR_CHANGE_CNT 0x78u
#define M2_TS_CLIP_LIST 0x7cu
#define M2_TS_NEXT_CLIP 0x84u
#define M2_TS_DBLEND 0x88u
#define M2_TS_VERTEX 0x8cu
#define M2_TS_HEADER_SIZE 0x90u

// --- 32-bit bitmap, opening item prefix 0x24 -------------------------
#define M2_BM_WIDTH 0x24u
#define M2_BM_HEIGHT 0x28u
#define M2_BM_BUFFER 0x4cu
#define M2_BM_TYPE 0x50u
#define M2_BMTYPE_32 2u

// --- render-table records --------------------------------------------
// charInfo is 10 words: X, Y, W, BgColor, FgColor, offset, width, height,
// twidth, entry (char).
#define M2_CHARINFO_SIZE 0x28u
// renderTableHeader is 7 words: texel, charCnt, height, bpp, bytesToLoad,
// offset, minChar/maxChar (two chars).
#define M2_RENDER_TABLE_HEADER_SIZE 0x1cu
// bgRectInfo is Box2 (4 words) + w + rgb + clipped.
#define M2_BG_RECT_SIZE 0x1cu
// vertexInfo is 8 words: vertex, dx, dy, saved[3], flags.
#define M2_VERTEX_INFO_SIZE 0x20u

#ifdef __cplusplus
}
#endif
