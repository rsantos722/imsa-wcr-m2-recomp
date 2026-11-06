// audio_folio.h — audio folio (id 6) vectors used by `driving/game`.
//
// 24 distinct vectors, 263 calls. Ordinals are raw slots from the on-disc
// System.m2/Modules/audio export table; names are the corresponding public
// export ranks. The complete used-ordinal repin is recorded in
// the audio selector repin notes.
//
// All prototypes PROVISIONAL pending M2 SDK cross-check; argument registers
// counts come from the call sites in driving/game.
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m2_instrument m2_instrument_t;    // opaque host voice group
typedef struct m2_attachment m2_attachment_t;    // opaque playable binding

// --- evidence-backed host entry points ---------------------------------------
// vector 57: ConnectInstrumentParts x51. Evidence: two part-name strings
// ("Output"/"Input") among six argument registers; exact guest-EA marshalling
// remains pending the typed audio dispatch.
int32_t m2_ConnectInstrumentParts(m2_item_t a, const char* part_a,
                                  m2_item_t b, const char* part_b,
                                  int32_t a5, int32_t a6);
// vector 51: LinkAttachments x2
m2_err_t m2_LinkAttachments(m2_attachment_t* att, m2_item_t instrument, int32_t flags);
// vector 54: StartAttachment x3
m2_err_t m2_StartAttachment(m2_attachment_t* att);
// vector 60: ReadKnobPart x3 — knob value read from a named instrument part.
float m2_ReadKnobPart(m2_item_t instrument, const char* part);
// vector 68: ReleaseInstrument x11
m2_err_t m2_ReleaseInstrument(m2_item_t instrument);

#ifdef __cplusplus
}  // extern "C"
#endif
