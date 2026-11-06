// audiopatch_folio.h — audiopatch folio (id 5) vectors used by `driving/game`.
//
// Exactly one import: CreatePatchTemplate (#0), called once at init to build
// the mixer patch from a TagArg template list. Signature PROVISIONAL.
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m2_patch_template m2_patch_template_t;  // opaque

// vector 0: CreatePatchTemplate x1. OS form takes a TagArg list; host keeps
// it opaque until audio RE lands (docs/formats/imsasound.md).
m2_item_t m2_CreatePatchTemplate(const m2_tag_arg_t* tags);

#ifdef __cplusplus
}  // extern "C"
#endif
