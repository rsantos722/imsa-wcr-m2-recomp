// guest_module.h — deterministic guest module/item identity model.
//
// Deterministic guest module/item identity model for kernel selectors 262
// (FindCurrentModule) and 103 (ImportByName), used by the typed production
// handlers described in the audio runtime bridge notes. The model
// keeps guest-visible handles fixed-width and never exposes a host pointer.
//
// Model (minimum demanded by the audio boot chain):
//   * FindCurrentModule mints exactly one Item denoting the caller's module
//     (the game image). The handle is minted once per context and returned
//     unchanged for the context's lifetime, so every call site observes the
//     same value.
//   * ImportByName(module, name) copies the NUL-terminated name out of guest
//     RAM through a bounded byte reader (big-endian image, read in address
//     order) and resolves it against a fixed export table. The current model
//     carries the minimum audio-boot export: "audio" -> M2_FOLIO_AUDIO;
//     lookup follows the vendor loader's ASCII case-insensitive comparison.
//   * Repeat-import identity: importing the same name again returns the SAME
//     handle (same generation+slot). No slot churn, no refcount drift — five
//     OpenAudioFolio sites per boot all observe one stable Item, like the
//     real OS returning the existing folio Item.
//   * Deterministic negative errors: unknown names, bad/stale/foreign module
//     handles, unterminated strings and reader faults each map to one fixed
//     negative code, so guarded call sites (`cmpwi r3, 0`) always see the
//     same verdict for the same input sequence.
//
// Guest-safety contract (mirrors include/guest_kernel.h):
//   * Every guest-visible value is a fixed-width uint32_t item handle or a
//     negative Err cast to uint32_t. No host pointer is ever returned or
//     stored in a guest-visible field.
//   * Handles are (generation << 12) | (slot << 4) | type; generations come
//     from a monotonic counter seeded at m2m_init, so identical op sequences
//     on identically initialized contexts produce identical handle streams.
//   * Single-threaded by contract: calls come from the recomp thread only,
//     so no locks are needed (the kernel slice owns the blocking surfaces).
#pragma once

#include <stdint.h>

#include "ppc_ctx.h"   // PPCContext + rd8 for the _ctx convenience path

#ifdef __cplusplus
extern "C" {
#endif

// ---- error codes (3DO convention: nonnegative ok, negative Err) ----------
enum {
    M2M_OK           = 0,
    M2M_ERR_BAD_ITEM = -1,   // zero/stale/wrong-type handle where a module was required
    M2M_ERR_NAME     = -2,   // name not found in the module export table
    M2M_ERR_RANGE    = -3,   // cap == 0 or no NUL terminator within cap - 1 bytes
    M2M_ERR_NO_SLOT  = -4,   // import pool exhausted
    M2M_ERR_FAULT    = -5,   // reader rejected an EA (out-of-bounds guest access)
};

#define M2M_ITEM_NONE 0u

// ---- 32-bit guest item handles -------------------------------------------
// Same layout as the kernel slice: bits [3:0] type | bits [11:4] slot |
// bits [31:12] generation (1-based, never reused within a context).
typedef uint32_t m2m_item_t;

enum {
    M2M_TYPE_MODULE = 1,     // what FindCurrentModule mints
    M2M_TYPE_FOLIO  = 2,     // what ImportByName resolves (a folio Item)
};

#define M2M_HANDLE_TYPE_MASK  0x0000000Fu
#define M2M_HANDLE_SLOT_SHIFT 4
#define M2M_HANDLE_SLOT_MASK  0x00000FF0u
#define M2M_HANDLE_GEN_SHIFT  12
#define M2M_HANDLE_GEN_MASK   0xFFFFF000u

static inline uint32_t m2m_handle_type(uint32_t h) { return h & M2M_HANDLE_TYPE_MASK; }
static inline uint32_t m2m_handle_slot(uint32_t h) { return (h & M2M_HANDLE_SLOT_MASK) >> M2M_HANDLE_SLOT_SHIFT; }
static inline uint32_t m2m_handle_gen(uint32_t h)  { return (h & M2M_HANDLE_GEN_MASK) >> M2M_HANDLE_GEN_SHIFT; }

// ---- pools ------------------------------------------------------------------
// Modules: slot 0 is the game image ("current module"); headroom exists in
// case a future surface mints a second module. Imports: fixed pool of folio
// Items claimed lowest-free-first; one slot per distinct resolved folio keeps
// repeat imports at identical handle values forever.
#define M2M_MAX_MODULES 2u
#define M2M_MAX_IMPORTS 8u
#define M2M_MAX_NAME    63u   // max copied name length, excluding the NUL

typedef struct {
    uint8_t  used;
    uint32_t gen;            // pinned at claim time; never regenerated
    uint32_t folio;          // M2_FOLIO_* id this import denotes
} m2m_import;

typedef struct m2m_context {
    uint8_t     module_used[M2M_MAX_MODULES];
    uint32_t    module_gen[M2M_MAX_MODULES];
    m2m_import  imports[M2M_MAX_IMPORTS];
    uint32_t    gen_counter; // monotonic generation source (never reset)
} m2m_context;

// Resets everything to the pristine state; identical op sequences after
// identically initialized contexts produce identical handle streams.
void m2m_init(m2m_context* ctx);

// ---- kernel selector 262 semantics ------------------------------------------
// Returns the Item denoting the caller's module (the game image, slot 0).
// Minted on first use, then stable for the context's lifetime.
m2m_item_t m2m_find_current_module(m2m_context* ctx);

// ---- bounded big-endian guest string copy ------------------------------------
// Reader abstraction: returns the byte 0..255 at `ea`, or a negative Err to
// fault the scan (e.g. EA outside the guest image). Lets tests inject an
// explicit reader without allocating guest RAM, and lets the PPCContext path
// enforce its own bounds.
typedef int32_t (*m2m_read8_fn)(void* ud, uint32_t ea);

// Copies the NUL-terminated string starting at guest `ea` into `out`
// (capacity `cap`, including the NUL), reading bytes strictly through `rd`.
// Never reads past the bound: the scan touches exactly ea .. ea + cap - 1 and
// stops at the first NUL. On success returns the string length (excluding
// the NUL); negative M2M_ERR_RANGE when cap == 0 or no NUL fits in those cap
// bytes, and any reader fault propagates as M2M_ERR_FAULT.
int32_t m2m_copy_guest_string(void* ud, m2m_read8_fn rd, uint32_t ea,
                              char* out, uint32_t cap);

// PPCContext flavor of the reader: faults EAs outside the RECOMP_GUEST_MEM
// image instead of indexing past it, otherwise reads c->mem[ea] directly
// (byte-exact big-endian image, address order). Non-const per the ppc_ctx.h
// accessor convention; no write ever happens through it.
int32_t m2m_rd8_guest(void* ud, uint32_t ea);

static inline int32_t m2m_copy_guest_string_ctx(PPCContext* c, uint32_t ea,
                                                char* out, uint32_t cap) {
    return m2m_copy_guest_string((void*)c, m2m_rd8_guest, ea, out, cap);
}

// ---- kernel selector 103 semantics -------------------------------------------
// Resolves `name_ea` (guest EA of a NUL-terminated name) against the current
// module's exports. Returns the folio Item (stable across repeat imports) or
// a negative Err cast to uint32_t per the Portfolio convention:
//   BAD_ITEM  module operand is not a live module handle of this context
//   RANGE     the name has no NUL within the copy bound
//   FAULT     the reader rejected an EA while copying the name
//   NAME      the name is empty or not in the export table
//   NO_SLOT   import pool exhausted
m2m_item_t m2m_import_by_name(m2m_context* ctx, m2m_item_t module,
                              void* rd_ud, m2m_read8_fn rd, uint32_t name_ea);

// PPCContext convenience wrapper over m2m_import_by_name.
static inline m2m_item_t m2m_import_by_name_ctx(m2m_context* ctx, m2m_item_t module,
                                                PPCContext* c, uint32_t name_ea) {
    return m2m_import_by_name(ctx, module, (void*)c, m2m_rd8_guest, name_ea);
}

// ---- host-side introspection (never crosses into guest view) -----------------
// Returns the M2_FOLIO_* id an import Item denotes, or 0 when the handle is
// not a live import of this context (including module handles and errors).
uint32_t m2m_import_folio(const m2m_context* ctx, m2m_item_t item);

#ifdef __cplusplus
}
#endif
