// diab_runtime.h — Diab 3.7a PPC runtime helpers the recomp must link against.
//
// Evidence (the compiler profile and selector ABI notes):
//  - `__itof`/`__utof` arrive as kernel vectors 325/326 (201/55 calls): classic
//    32-bit PPC has no int->FP register move, so Diab emits OS helper calls.
//  - `_savegpr_*/_restgpr_*/_savegprs_*_l/_restgprs_*_l` and the fpr families
//    are out-of-line prologue/epilogue helpers (0 real refs in our count =
//    emitted by compiler, dispatched via kernel vectors). On the host they are
//    pure no-ops: recompiled code keeps its own register state.
//  - `__tagCall_0/1/2` + `__va_arg` support tagged variadic OS calls.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// int/uint -> float conversion helpers (kernel vectors 325/326 on the wire).
float m2___itof(int32_t v);
float m2___utof(uint32_t v);

// Prologue/epilogue helper families. Declared for linkage completeness of the
// future recomp object; definitions are empty no-ops on host architectures.
#define M2_DIAB_GPR_RANGE(X) \
    X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27)

#define M2_DECL_SAVEGPR(n) void _savegpr_##n(void);
#define M2_DECL_RESTGPR(n) void _restgpr_##n(void);
#define M2_DECL_SAVEGPRL(n) void _savegprs_##n##_l(void);
#define M2_DECL_RESTGPRL(n) void _restgprs_##n##_l(void);
#define M2_DECL_SAVEFPR(n) void _savefprs_##n##_l(void);
M2_DIAB_GPR_RANGE(M2_DECL_SAVEGPR)
M2_DIAB_GPR_RANGE(M2_DECL_RESTGPR)
M2_DIAB_GPR_RANGE(M2_DECL_SAVEGPRL)
M2_DIAB_GPR_RANGE(M2_DECL_RESTGPRL)
M2_DIAB_GPR_RANGE(M2_DECL_SAVEFPR)
#undef M2_DECL_SAVEGPR
#undef M2_DECL_RESTGPR
#undef M2_DECL_SAVEGPRL
#undef M2_DECL_RESTGPRL
#undef M2_DECL_SAVEFPR

// Tagged variadic call trampolines (kernel vectors 89..91 usage sites).
void m2___tagCall_0(void);
void m2___tagCall_1(void);
void m2___tagCall_2(void);

#ifdef __cplusplus
}  // extern "C"
#endif
