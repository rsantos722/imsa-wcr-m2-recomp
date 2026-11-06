// shim_abi.h — M2 folio dispatch selector ABI for the host shim layer.
//
// Authoritative source: the selector ABI manifest and notes.
// External calls in a guest image are REL24 `bl` instructions whose
// relocation packs the selector (folio_id << 16) | vector_ordinal; the M2
// loader patches each site to an OS trampoline. The host replaces the loader
// with a static dispatch table keyed by selector — never by name.
//
// The static recompiler emits calls through these selectors:
//     void* fn = m2_selector_lookup(folio, ordinal);
// and casts to the prototype declared in the matching folio header.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Folio ids for driving/game (1..7). The companion
// shooter2d image adds gstate/font at 8/9; those ids are assigned from its
// ordered .imp3do module records and the vendor folio vector exports.
enum {
    M2_FOLIO_KERNEL = 1,
    M2_FOLIO_IFF = 2,
    M2_FOLIO_GRAPHICS = 3,
    M2_FOLIO_FILESYSTEM = 4,
    M2_FOLIO_AUDIOPATCH = 5,
    M2_FOLIO_AUDIO = 6,
    // Declared by .imp3do but never dispatched by this game (unused import).
    // Kept so the mpegaudiodecoder shim has a home if that changes.
    M2_FOLIO_MPEGAUDIODECODER = 7,
    M2_FOLIO_GSTATE = 8,
    M2_FOLIO_FONT = 9,
};

#define M2_FOLIO_COUNT 10  // slots 0..9; slot 0 unused
#define M2_VECTORS_PER_FOLIO 1024
// OS tables have 1024 slots per folio; audio ordinals >52 and filesystem >=32
// are shifted by build insertions.

static inline uint32_t m2_make_selector(uint32_t folio, uint32_t ordinal) {
    return (folio << 16) | ordinal;
}

static inline uint32_t m2_selector_folio(uint32_t sel) { return sel >> 16; }
static inline uint32_t m2_selector_ordinal(uint32_t sel) { return sel & 0xFFFFu; }

// Dispatch table lookup: returns the C function registered for this selector,
// or a logging no-op stub if the vector is not implemented yet (never NULL).
void* m2_selector_lookup(uint32_t folio, uint32_t ordinal);

#ifdef __cplusplus
}  // extern "C"
#endif
