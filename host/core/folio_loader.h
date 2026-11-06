// folio_loader.h — selector-keyed shim dispatch table.
#pragma once

#include <cstdint>
#include <cstdio>

// Subsystem registration hooks
void kernel_shim_register();
void iff_shim_register();
void graphics_shim_register();
void filesystem_shim_register();
void audiopatch_shim_register();
void audio_shim_register();
void mpeg_decoder_register();

namespace m2host {

using ShimFn = void();

// Register a C function under a (folio, ordinal) selector.
void folio_register(uint32_t folio, uint32_t ordinal, void* fn);

// Install every subsystem's vectors, then fill all remaining slots with the
// logging stub. Safe to call exactly once before game entry.
void folio_install_all();

// Optional live-guest selector probe: given a selector, returns whatever
// handler (nonnull) the guest's own dispatch registry currently owns for it,
// or null when that ordinal has no guest-side owner.
using GuestSelectorProbeFn = void* (*)(uint32_t folio, uint32_t ordinal);

void folio_set_guest_probe(GuestSelectorProbeFn probe);

// Read-only observed-census probe: returns 1 when served, 0 when failed closed,
// -1 when never dispatched.
using SelectorObservedFn = int (*)(uint32_t folio, uint32_t ordinal);

// Select which generated name table the report reads: driving/game (false) or shooter2d (true).
void folio_set_name_source(bool shooter2d);

void folio_set_observed_probe(SelectorObservedFn probe);

// Startup report: per folio, a STATIC prediction and a MEASURED observed column.
void folio_print_report(FILE* out);

// Post-run MEASURED pass over the same name table.
void folio_print_observed_report(FILE* out);

}  // namespace m2host
