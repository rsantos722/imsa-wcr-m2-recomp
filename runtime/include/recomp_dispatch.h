#ifndef M2_RECOMP_DISPATCH_H
#define M2_RECOMP_DISPATCH_H

#include "ppc_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RecompFn)(PPCContext *c);

/* Resolve an absolute guest text address or an allowed text-relative value. */
RecompFn recomp_lookup(unsigned guest);

/* Generated guest image bases used by the lookup/rebase contract. */
extern unsigned g_recomp_text_base;

#ifdef __cplusplus
}
#endif

#endif
