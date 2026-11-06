#pragma once
// selector_observed.h — per-process record of which selectors were dispatched.
//
// Every dispatch entry point notes each (folio, ordinal) it is asked to serve,
// and whether a typed handler served it or the call fell through to the
// fail-closed path. folio_print_report() reads the record for the boot
// report's observed column and folio_print_observed_report() for the post-run
// pass.
//
// It is observational only and never changes dispatch behavior. A missing
// entry means only "this run did not dispatch that pair".

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Defined once in src/recomp_runtime.c so every TU (generated code, dispatch
// entry points, host audio boundary, standalone tests) shares one record.
void m2_selector_note(uint32_t folio, uint32_t ordinal, int handled);

// Read-only view of the record. `*out_handled` / `*out_unhandled` receive the
// number of distinct pairs recorded so far at each level. Never records, never
// changes dispatch.
//
// Returns 1 when the pair was recorded as handled, 0 when recorded as
// unhandled, and -1 when the pair was never dispatched in this process.
int m2_selector_observed(uint32_t folio, uint32_t ordinal,
                         uint32_t* out_handled,
                         uint32_t* out_unhandled);

#ifdef __cplusplus
}
#endif
