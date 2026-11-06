// graphics_folio.h — graphics folio (id 3) vectors used by `driving/game`.
//
// The game imports exactly two view-list operations (from its import
// relocations). Everything else the game renders goes through its own
// in-module `clt*` cel blitters, which the recomp keeps and which draw via
// the host cel-blitter backend (see host/graphics/cel_blitter.h).
//
// Public vectors 3/5 are the library wrappers; the folio entry points are
// view.c SuperAddViewToViewList / SuperRemoveView
// (Portfolio OS folios/graphics/view.c,
// syscall_glue.s GRAPHICSFOLIO+1/+2). Both take Item handles, never host
// pointers. The live recomp path implements the recovered contract in
// recomp_runtime.c:recomp_call_graphics_dispatch; the host-table functions
// below are the launcher path and remain log-only.
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// vector 3: AddViewToViewList x1 -> Err AddViewToViewList(viewItem,
// viewListItem); viewListItem == 0 selects the default Projector.
m2_err_t m2_AddViewToViewList(m2_item_t view_item, m2_item_t view_list_item);
// vector 5: RemoveView x1 -> Err RemoveView(viewItem).
m2_err_t m2_RemoveView(m2_item_t view_item);

#ifdef __cplusplus
}  // extern "C"
#endif
