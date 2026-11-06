// ppc_ctx.h -- guest CPU state + memory model for the recompiled guest.
//
// Generated code (game_text_*.c) includes this header through
// recomp_helpers.h; selector calls reach shim_dispatch() in recomp_runtime.c
// using the ABI in shims/.
//
// Conventions:
//  * Guest is big-endian PPC602; guest memory is stored byte-exact (BE), so
//    all multi-byte accesses go through rd8/rd16/rd32/wr16/wr32 helpers which
//    assemble/scatter bytes explicitly. This keeps the .text/.data images
//    byte-identical to the disc ELF (diffable, hashable).
//  * FPRs are IEEE binary32 (`float`). For determinism, fused ops go through
//    FMADDS()/FMSUBS() helpers: host-FMA path when RECOMP_HAVE_FMA else the
//    double-intermediate fallback, both correctly rounded for fp32 inputs.
//  * CR is one 32-bit word, 8 fields x 4 bits (standard layout). XER models
//    only SO (bit 31) + CA (29) which the game's code can observe.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "guest_provenance.h"
#include "guest_tracepoints.h"

// Forward declaration so the trace externs below can take a PPCContext*; the
// full typedef appears a few lines further down.
typedef struct PPCContext PPCContext;

#ifdef __cplusplus
extern "C" {
#endif

// Trace events (C ABI). Defined in host/diagnostics/trace.cpp; weak so
// standalone runtime tests link without the host trace library.
extern void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc) __attribute__((weak));
extern void m2_trace_indirect_target_c(uint32_t ctr, uint32_t target, uint32_t pc, int resolved) __attribute__((weak));
extern void m2_trace_unknown_behavior_c(const char* desc, uint32_t pc) __attribute__((weak));

// Bounded powf operand/error trace.  The generated libm helper calls these
// hooks only when the opt-in M2_POWF_TRACE diagnostic is enabled; they never
// change guest state or math behavior.
extern void m2_trace_powf_enter_c(PPCContext* c) __attribute__((weak));
extern void m2_trace_powf_error_c(PPCContext* c) __attribute__((weak));

// The original guest image has a 128 MiB addressable-RAM boundary in the
// recovered layout.  The host model deliberately exposes a larger, zeroed
// compatibility window so an address at that boundary is observable instead
// of immediately becoming an OOB shadow read.  This is a host adaptation for
// testing/release builds; it does not alter the source image or its hashes.
#define RECOMP_M2_RAM_LIMIT (128u << 20)
#define RECOMP_GUEST_MEM    (256u << 20)

// Guest-owned frame boundary counter; the runtime ownership contract is
// recorded in the StateDriver boundary contract.
// Guest EA carve for StateDriver per-iteration tick. All EAs reside in the
// 0x003F0000 gap (between broker 0x00300140 and heap 0x00400000), zeroed at
// guest_mem_init and mutated only by guest code at the proven loop head
// (StateDriver L2fb88). Host reads are snapshot-only, never mutated.
#define M2_GUEST_FRAME_COUNTER_EA  0x003F0000u  // u32 seq, incremented at 0x2fb88
#define M2_GUEST_FRAME_STATE_EA    0x003F0004u  // u32 primary state (BSS 0x1877d8)
#define M2_GUEST_FRAME_PC_EA       0x003F0008u  // u32 pc that performed tick (0x2fb88)
#define M2_GUEST_FRAME_SNAPSHOT_EA 0x003F0010u  // u32[4] (BSS 0x1877dc, BSS 0x1877e0, DATA 0x17d94b, r27)
#define M2_GUEST_FRAME_SNAPSHOT_WORDS 4u

// StateDriver's dotrans request word (BSS 0x18783c). It writes 2 for the
// random effect and clears it after the swap; Dsply_SwapBuffers_trans reads
// the same cell. M2_FORCE_TRANSITION substitutes the forced effect only when
// the guest asked for the random one, so explicit transitions keep working.
#define M2_DOTRANS_TYPE_EA 0x0018783cu
#define M2_DOTRANS_RANDOM 2u

// Parse M2_FORCE_TRANSITION: a dotrans type 0..16 or one of the named
// effects below. Returns 0 (off) for an empty/unknown value.
static inline uint32_t m2_parse_forced_transition(const char* value)
{
    if (!value || !*value) return 0u;
    struct { const char* name; uint32_t type; } names[] = {
        {"rotate", 15u},        {"rotate-reverse", 16u},
        {"scroll", 7u},         {"scroll2", 8u},
        {"scroll3", 9u},        {"scroll4", 10u},
        {"wipe", 3u},           {"wipe2", 4u},
        {"wipe3", 5u},          {"wipe4", 6u},
        {"stripes", 11u},       {"grid", 12u},
        {"square1", 13u},       {"square2", 14u},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strcmp(value, names[i].name) == 0) return names[i].type;
    char* end = 0;
    const unsigned long type = strtoul(value, &end, 0);
    return (end && *end == '\0' && type <= 16u) ? (uint32_t)type : 0u;
}

static inline uint32_t m2_forced_transition_type(void)
{
    static uint32_t forced = 0u;
    static int initialized = 0;
    if (!initialized) {
        forced = m2_parse_forced_transition(getenv("M2_FORCE_TRANSITION"));
        initialized = 1;
    }
    return forced;
}

typedef struct PPCContext {
    uint32_t gpr[32];
    float    fpr[32];
    uint32_t cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint8_t* mem;              // guest RAM (BE image)
    uint32_t pc;               // last translated guest PC that touched memory
} PPCContext;

// ---- guest memory access (big-endian) ----------------------------------
#include <stdio.h>
#include <stdlib.h>
// M2 hardware MMIO carve: high addresses like 0x20202028 are texture-engine
// registers, not guest RAM. On real hardware they are memory-mapped IO; in
// recomp they remain an explicitly classified adapter boundary. Other
// out-of-range accesses are still diagnosed and never alias guest RAM.
static inline void m2_expanded_access_note(PPCContext* c, uint32_t a, uint32_t sz) {
    if (a < RECOMP_M2_RAM_LIMIT && sz <= RECOMP_M2_RAM_LIMIT - a) return;
    if (__builtin_expect(m2_provenance_active, 0) && a < RECOMP_GUEST_MEM) {
        uint32_t invalid_ea = a >= RECOMP_M2_RAM_LIMIT ? a : RECOMP_M2_RAM_LIMIT;
        m2_provenance_report_oob(invalid_ea, (uint8_t)sz,
                                 c ? c->pc : 0, c ? c->lr : 0);
    }
    // Keep the compatibility-window diagnostic separate from a genuine OOB
    // fault.  It identifies accesses that the original map would reject,
    // while the expanded host window lets the guest continue to execute.
    static int _m2_expanded_cnt = 0;
    if (_m2_expanded_cnt < 32) {
        fprintf(stderr,
                "[recomp] host-expanded guest access pc=%#x ea=%#x sz=%u "
                "(M2_RAM_LIMIT=%#x, RECOMP_GUEST_MEM=%#x) r1=%#x r3=%#x "
                "r8=%#x r9=%#x r10=%#x r11=%#x r12=%#x r21=%#x\n",
                c ? c->pc : 0, a, sz, RECOMP_M2_RAM_LIMIT, RECOMP_GUEST_MEM,
                c ? c->gpr[1] : 0, c ? c->gpr[3] : 0,
                c ? c->gpr[8] : 0, c ? c->gpr[9] : 0,
                c ? c->gpr[10] : 0, c ? c->gpr[11] : 0,
                c ? c->gpr[12] : 0, c ? c->gpr[21] : 0);
        _m2_expanded_cnt++;
        if (_m2_expanded_cnt == 32)
            fprintf(stderr, "[recomp] (further expanded-window accesses suppressed)\n");
    }
    // Optional register-context capture for the first invalid guest data
    // access in a path under investigation.  This is diagnostic-only: it
    // reads no guest state and does not alter the compatibility-window
    // behavior above.
    static int _m2_invalid_trace_enabled = -1;
    static int _m2_invalid_trace_cnt = 0;
    if (_m2_invalid_trace_enabled < 0)
        _m2_invalid_trace_enabled = (getenv("M2_INVALID_ACCESS_TRACE") != NULL) ? 1 : 0;
    if (_m2_invalid_trace_enabled && _m2_invalid_trace_cnt < 16) {
        fprintf(stderr,
                "[recomp] invalid-access-context pc=%#x ea=%#x sz=%u "
                "lr=%#x ctr=%#x "
                "r0=%#x r1=%#x r3=%#x r4=%#x r5=%#x r6=%#x r7=%#x "
                "r8=%#x r9=%#x r10=%#x r11=%#x r12=%#x r13=%#x r14=%#x "
                "r17=%#x r18=%#x r23=%#x r27=%#x r29=%#x r30=%#x r31=%#x\n",
                c ? c->pc : 0, a, sz,
                c ? c->lr : 0, c ? c->ctr : 0,
                c ? c->gpr[0] : 0, c ? c->gpr[1] : 0,
                c ? c->gpr[3] : 0, c ? c->gpr[4] : 0,
                c ? c->gpr[5] : 0, c ? c->gpr[6] : 0,
                c ? c->gpr[7] : 0, c ? c->gpr[8] : 0,
                c ? c->gpr[9] : 0, c ? c->gpr[10] : 0,
                c ? c->gpr[11] : 0, c ? c->gpr[12] : 0,
                c ? c->gpr[13] : 0, c ? c->gpr[14] : 0,
                c ? c->gpr[17] : 0, c ? c->gpr[18] : 0,
                c ? c->gpr[23] : 0, c ? c->gpr[27] : 0,
                c ? c->gpr[29] : 0, c ? c->gpr[30] : 0,
                c ? c->gpr[31] : 0);
        _m2_invalid_trace_cnt++;
        if (_m2_invalid_trace_cnt == 16)
            fprintf(stderr, "[recomp] (further invalid-access context suppressed)\n");
    }
}

// Optional target-field writer trace for an evidence-gated guest pointer
// investigation.  It is disabled unless explicitly requested and observes
// only the current State 2 lighting object carve.
static inline int m2_light_write_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = (getenv("M2_LIGHT_WRITE_TRACE") != NULL) ? 1 : 0;
    return enabled;
}
static inline void m2_trace_light_write(PPCContext* c, uint32_t a,
                                        uint32_t value, uint32_t size) {
    if (!m2_light_write_trace_enabled() || !c ||
        a < 0x93ab00u || a >= 0x93ab40u)
        return;
    fprintf(stderr,
            "[recomp] light-write ea=%#x sz=%u value=%#x pc=%#x "
            "r3=%#x r4=%#x r8=%#x r9=%#x r10=%#x r11=%#x r12=%#x\n",
            a, size, value, c->pc, c->gpr[3], c->gpr[4], c->gpr[8],
            c->gpr[9], c->gpr[10], c->gpr[11], c->gpr[12]);
}

// Optional exact-read trace for the same evidence-gated fields.  This keeps
// the final consumer visible when a host-side bulk write bypasses wr32/wr16/
// wr8, without changing the guest load semantics.
static inline int m2_light_read_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = (getenv("M2_LIGHT_READ_TRACE") != NULL) ? 1 : 0;
    return enabled;
}
static inline void m2_trace_light_read(PPCContext* c, uint32_t a,
                                       uint32_t value) {
    static unsigned count;
    if (!m2_light_read_trace_enabled() || !c ||
        (a != 0x93ab1cu && a != 0x93ab24u) || count >= 128u)
        return;
    fprintf(stderr, "[recomp] light-read ea=%#x value=%#x pc=%#x "
                    "lr=%#x r8=%#x r11=%#x r12=%#x\n",
            a, value, c->pc, c->lr, c->gpr[8], c->gpr[11], c->gpr[12]);
    ++count;
    if (count == 128u)
        fprintf(stderr, "[recomp] (further light reads suppressed)\n");
}

static inline void m2_trace_light_half_read(PPCContext* c, uint32_t a,
                                            uint16_t value) {
    static unsigned count;
    if (!m2_light_read_trace_enabled() || !c ||
        (a != 0x93ab20u && a != 0x93ab22u) || count >= 64u)
        return;
    fprintf(stderr, "[recomp] light-read16 ea=%#x value=%#x pc=%#x "
                    "lr=%#x r8=%#x r10=%#x r21=%#x\n",
            a, value, c->pc, c->lr, c->gpr[8], c->gpr[10], c->gpr[21]);
    ++count;
    if (count == 64u)
        fprintf(stderr, "[recomp] (further light 16-bit reads suppressed)\n");
}

// Optional bounded trace of the guest dynamic-light opcode stream.  The
// stream's loop tests CR7 after each lhau, so retain the exact cursor, value,
// and condition bits at those reads while diagnosing its continuation.
static inline void m2_trace_light_list_read(PPCContext* c, uint32_t a,
                                            uint16_t value) {
    static unsigned count;
    if (!m2_light_read_trace_enabled() || !c ||
        a < 0x93afb0u || a >= 0x93b100u || count >= 320u)
        return;
    if (c->pc != 0x5384cu && c->pc != 0x53874u && c->pc != 0x5388cu &&
        c->pc != 0x53898u && c->pc != 0x53b60u)
        return;
    fprintf(stderr,
            "[recomp] light-list-read pc=%#x ea=%#x value=%#x cr=%#x "
            "cr7lt=%u cr7eq=%u ctr=%#x r10=%#x r21=%#x\n",
            c->pc, a, value, c->cr, (c->cr >> 3) & 1u, (c->cr >> 1) & 1u,
            c->ctr, c->gpr[10], c->gpr[21]);
    ++count;
    if (count == 320u)
        fprintf(stderr, "[recomp] (further light-list reads suppressed)\n");
}

// Optional bounded trace for the guest continuation loaded by
// M_LightSoftSpot's terminal lwzu/mtlr pair.  The value is guest data; this
// observer exists only to distinguish a bad callback/table image from a host
// dispatch normalization defect.
static inline void m2_trace_light_return_read(PPCContext* c, uint32_t a,
                                              uint32_t value) {
    static int enabled = -1;
    static unsigned count;
    if (enabled < 0)
        enabled = (getenv("M2_LIGHT_RETURN_TRACE") != NULL) ? 1 : 0;
    if (!enabled || !c || c->pc != 0x52550u || count >= 64u)
        return;
    fprintf(stderr,
            "[recomp] light-return-read ea=%#x value=%#x r13=%#x "
            "lr=%#x r4=%#x r8=%#x r30=%#x r31=%#x\n",
            a, value, c->gpr[13], c->lr, c->gpr[4], c->gpr[8],
            c->gpr[30], c->gpr[31]);
    ++count;
    if (count == 64u)
        fprintf(stderr, "[recomp] (further light return reads suppressed)\n");
}

// Census total (single definition in recomp_runtime.c); declared here so
// m2_oob_trap below can count without depending on declaration order.
extern uint64_t _m2_oob_total;

static inline void m2_oob_trap(PPCContext* c, uint32_t a, uint32_t sz) {
    // Uncapped census (opt-in: M2_OOB_COUNT=1; reported on the m2host exit
    // path via m2_oob_trap_total). The 32-line log throttle below is
    // unchanged; this counter is exact. Off by default: one predictable
    // branch per trap, negligible against the shadow scan that follows.
    static int count_enabled = -1;
    if (count_enabled < 0)
        count_enabled = (getenv("M2_OOB_COUNT") != NULL) ? 1 : 0;
    if (count_enabled)
        __atomic_fetch_add(&_m2_oob_total, 1u, __ATOMIC_RELAXED);
    if (__builtin_expect(m2_provenance_active, 0)) {
        m2_provenance_report_oob(a, (uint8_t)sz, c ? c->pc : 0, c ? c->lr : 0);
    }
    // Throttle: only log first 32 OOBs to avoid flooding the frame loop.
    static int _m2_oob_cnt = 0;
    if (_m2_oob_cnt < 32) {
        fprintf(stderr,
                "[recomp] OOB guest access pc=%#x ea=%#x sz=%u "
                "(RECOMP_GUEST_MEM=%#x) — stubbed\n",
                c ? c->pc : 0, a, sz, RECOMP_GUEST_MEM);
        _m2_oob_cnt++;
        if (_m2_oob_cnt == 32) fprintf(stderr, "[recomp] (further OOBs suppressed)\n");
    }
    // Trace: emit memory_fault event without host pointers
    if (m2_trace_memory_fault_c)
        m2_trace_memory_fault_c(a, sz, c ? c->pc : 0);
}
// Tiny MMIO shadow for high-address polling: OOB reads return the last
// written value at that EA (or the adapter's documented ready sentinel if
// never written) so TE/IO poll loops can terminate without a full device
// model. This is not guest behavior and remains visible in memory-fault traces.
// Shared across all recomp TUs (single definition in recomp_runtime.c).
#define M2_OOB_SHADOW_SLOTS 256
extern uint32_t _m2_oob_shadow_ea[M2_OOB_SHADOW_SLOTS];
extern uint32_t _m2_oob_shadow_val[M2_OOB_SHADOW_SLOTS];
extern uint8_t  _m2_oob_shadow_used[M2_OOB_SHADOW_SLOTS];
extern int _m2_oob_cnt;
uint64_t m2_oob_trap_total(void);
void m2_oob_shadow_write(uint32_t a, uint32_t v);
uint32_t m2_oob_shadow_read(uint32_t a);
static inline uint32_t m2_guest_frame_seq(PPCContext* c) {
    if (!c || !c->mem || M2_GUEST_FRAME_COUNTER_EA > RECOMP_GUEST_MEM - 4u)
        return 0;
    const uint8_t* p = c->mem + M2_GUEST_FRAME_COUNTER_EA;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint32_t rd32(PPCContext* c, uint32_t a) {
    m2_expanded_access_note(c, a, 4);
    if (a > RECOMP_GUEST_MEM - 4u || !c->mem) {
        m2_oob_trap(c, a, 4);
        return m2_oob_shadow_read(a);
    }
    const uint8_t* p = c->mem + a;
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | p[3];
    if (a == M2_DOTRANS_TYPE_EA && v == M2_DOTRANS_RANDOM) {
        const uint32_t forced = m2_forced_transition_type();
        if (forced != 0u) v = forced;
    }
    if (__builtin_expect(m2_provenance_reads_active, 0)) {
        m2_provenance_record_read(a, v, 4, c ? c->pc : 0, c ? c->lr : 0,
                                  m2_guest_frame_seq(c));
    }
    m2_trace_light_read(c, a, v);
    m2_trace_light_return_read(c, a, v);
    return v;
}
static inline uint16_t rd16(PPCContext* c, uint32_t a) {
    m2_expanded_access_note(c, a, 2);
    if (a > RECOMP_GUEST_MEM - 2u || !c->mem) {
        m2_oob_trap(c, a, 2);
        return (uint16_t)(m2_oob_shadow_read(a) & 0xFFFFu);
    }
    const uint8_t* p = c->mem + a;
    const uint16_t v = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    if (__builtin_expect(m2_provenance_reads_active, 0)) {
        m2_provenance_record_read(a, v, 2, c ? c->pc : 0, c ? c->lr : 0,
                                  m2_guest_frame_seq(c));
    }
    m2_trace_light_half_read(c, a, v);
    m2_trace_light_list_read(c, a, v);
    return v;
}
static inline uint8_t rd8(PPCContext* c, uint32_t a) {
    m2_expanded_access_note(c, a, 1);
    if (a >= RECOMP_GUEST_MEM || !c->mem) {
        m2_oob_trap(c, a, 1);
        return (uint8_t)(m2_oob_shadow_read(a) & 0xFFu);
    }
    const uint8_t v = c->mem[a];
    if (__builtin_expect(m2_provenance_reads_active, 0)) {
        m2_provenance_record_read(a, v, 1, c ? c->pc : 0, c ? c->lr : 0,
                                  m2_guest_frame_seq(c));
    }
    return v;
}
static inline void wr32(PPCContext* c, uint32_t a, uint32_t v) {
    m2_expanded_access_note(c, a, 4);
    m2_trace_light_write(c, a, v, 4);
    if (__builtin_expect(m2_provenance_active, 0) && a < RECOMP_GUEST_MEM) {
        m2_provenance_record_write(a, v, 4, c ? c->pc : 0, c ? c->lr : 0, c && c->mem ? rd32(c, M2_GUEST_FRAME_COUNTER_EA) : 0);
    }
    if (a > RECOMP_GUEST_MEM - 4u || !c->mem) {
        m2_oob_trap(c, a, 4);
        m2_oob_shadow_write(a, v);
        return;
    }
    uint8_t* p = c->mem + a;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void wr16(PPCContext* c, uint32_t a, uint16_t v) {
    m2_expanded_access_note(c, a, 2);
    m2_trace_light_write(c, a, v, 2);
    if (__builtin_expect(m2_provenance_active, 0) && a < RECOMP_GUEST_MEM) {
        m2_provenance_record_write(a, (uint32_t)v, 2, c ? c->pc : 0, c ? c->lr : 0, c && c->mem ? rd32(c, M2_GUEST_FRAME_COUNTER_EA) : 0);
    }
    if (a > RECOMP_GUEST_MEM - 2u || !c->mem) {
        m2_oob_trap(c, a, 2);
        m2_oob_shadow_write(a, (uint32_t)v);
        return;
    }
    uint8_t* p = c->mem + a;
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void wr8(PPCContext* c, uint32_t a, uint8_t v) {
    m2_expanded_access_note(c, a, 1);
    m2_trace_light_write(c, a, v, 1);
    if (__builtin_expect(m2_provenance_active, 0) && a < RECOMP_GUEST_MEM) {
        m2_provenance_record_write(a, (uint32_t)v, 1, c ? c->pc : 0, c ? c->lr : 0, c && c->mem ? rd32(c, M2_GUEST_FRAME_COUNTER_EA) : 0);
    }
    if (a >= RECOMP_GUEST_MEM || !c->mem) {
        m2_oob_trap(c, a, 1);
        m2_oob_shadow_write(a, (uint32_t)v);
        return;
    }
    c->mem[a] = v;
}

static inline float rdf32(PPCContext* c, uint32_t a) {
    uint32_t b = rd32(c, a); float f; memcpy(&f, &b, 4); return f;
}
static inline void wrf32(PPCContext* c, uint32_t a, float f) {
    uint32_t b; memcpy(&b, &f, 4); wr32(c, a, b);
}

// ---- integer helpers ----------------------------------------------------
static inline int32_t sext16(uint32_t v) { return (int16_t)(uint16_t)v; }

// Compare helpers set CR field bits: [LT|GT|EQ|SO]
#define CR_LT 8
#define CR_GT 4
#define CR_EQ 2
#define CR_SO 1

static inline void cr_set_i(PPCContext* c, unsigned sh, int32_t a, int32_t b) {
    uint32_t m = 0xFu << sh, v = 0;
    if (a < b) v = CR_LT << sh; else if (a > b) v = CR_GT << sh;
    else v = CR_EQ << sh;
    if (c->xer & 0x80000000u) v |= CR_SO << sh;
    c->cr = (c->cr & ~m) | v;
}
static inline void cr_set_u(PPCContext* c, unsigned sh, uint32_t a, uint32_t b) {
    uint32_t m = 0xFu << sh, v;
    if (a < b) v = CR_LT << sh; else if (a > b) v = CR_GT << sh;
    else v = CR_EQ << sh;
    if (c->xer & 0x80000000u) v |= CR_SO << sh;
    c->cr = (c->cr & ~m) | v;
}
static inline void cr_set_f(PPCContext* c, unsigned sh, float a, float b) {
    // fcmpu: unordered sets the SO-position bit (field pattern 0x1)
    int un = !((a < b) || (a >= b));   // true iff either is NaN
    uint32_t m = 0xFu << sh, v;
    if (un) v = (CR_SO << sh);
    else if (a < b) v = CR_LT << sh;
    else if (a > b) v = CR_GT << sh;
    else v = CR_EQ << sh;
    c->cr = (c->cr & ~m) | v;
}
static inline void cr_set_rec(PPCContext* c, unsigned sh, int32_t r) {
    cr_set_i(c, sh, r, 0);
}

// ---- rotate/shift helpers ------------------------------------------------
static inline uint32_t SLW(uint32_t a, uint32_t b) {
    return b >= 32 ? 0 : (a << b);
}
static inline uint32_t SRW(uint32_t a, uint32_t b) {
    return b >= 32 ? 0 : (a >> b);
}
static inline uint32_t SRAW(PPCContext* c, uint32_t a, uint32_t b) {
    unsigned n = b >= 32 ? 32 : b;
    int32_t r = n == 32 ? ((int32_t)a >> 31) : ((int32_t)a >> n);
    // PPC CA: set iff the operand is negative and any shifted-out low bit
    // is one; otherwise cleared (same rule as SRAWI below). The old form
    // tested only bit n-1 and never cleared stale CA.
    uint32_t discarded = n >= 32u ? a : a & ((n == 0u) ? 0u : ((1u << n) - 1u));
    uint32_t carry = ((int32_t)a < 0 && discarded != 0u) ? 1u : 0u;
    c->xer = (c->xer & ~0x20000000u) | (carry << 29);
    return (uint32_t)r;
}
static inline uint32_t SRAWI(PPCContext* c, uint32_t a, unsigned n) {
    // PowerPC sets CA only when a negative operand loses nonzero bits while
    // shifting. Clear stale CA for positive or exact inputs; this matters for
    // the source UCEIL(x, 8) sequence, which is SRAWI followed by ADDZE.
    int32_t signed_a = (int32_t)a;
    int32_t r = n >= 32u ? (signed_a < 0 ? -1 : 0) : signed_a >> n;
    uint32_t discarded = n == 0u ? 0u
                                  : n >= 32u ? a : a & ((1u << n) - 1u);
    uint32_t carry = signed_a < 0 && discarded != 0u;
    c->xer = (c->xer & ~0x20000000u) | (carry << 29);
    return (uint32_t)r;
}
static inline uint32_t CNTLZW(uint32_t a) {
    unsigned n = 0;
    while (n < 32 && !(a & 0x80000000u)) { a <<= 1; n++; }
    return n;
}
#define XER_CA(c) (((c)->xer >> 29) & 1u)

static inline uint32_t RL32(uint32_t x, unsigned sh) {
    sh &= 31;
    return (x << sh) | (x >> ((32 - sh) & 31));
}

static inline uint32_t PPC_MASK(unsigned mb, unsigned me) {
    uint32_t left = 0xFFFFFFFFu >> mb;
    uint32_t right = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? left & right : left | right;
}

static inline uint32_t RLWINM(uint32_t x, unsigned sh, unsigned mb, unsigned me) {
    return RL32(x, sh) & PPC_MASK(mb, me);
}

/* rlwimi: insert the rotated source into the destination's unmasked bits.
 * First argument is the destination's current value (read-modify-write). */
static inline uint32_t RLWIMI(uint32_t ra, uint32_t rs, unsigned sh, unsigned mb, unsigned me) {
    uint32_t mask = PPC_MASK(mb, me);
    return (ra & ~mask) | (RL32(rs, sh) & mask);
}

static inline int32_t PPC_DIVW(int32_t a, int32_t b) {
    if (b == 0) return a < 0 ? 1 : (a > 0 ? -1 : 0);
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}

static inline uint32_t PPC_DIVWU(uint32_t a, uint32_t b) {
    return b == 0 ? 0xFFFFFFFFu : a / b;
}

// Scalar PPC single-precision operations. Volatile temporaries force the
// architectural single rounding even when the host compiler keeps float
// expressions in a wider register.
static inline float PPC_FADD(float a, float b) { volatile float r = a + b; return r; }
static inline float PPC_FSUB(float a, float b) { volatile float r = a - b; return r; }
static inline float PPC_FMUL(float a, float b) { volatile float r = a * b; return r; }
static inline float PPC_FDIV(float a, float b) { volatile float r = a / b; return r; }

// ---- condition-register field logic ops ----------------------------------
#define CR_BIT(c, n) (((c)->cr >> ((7 - ((n) >> 2)) * 4 + (3 - ((n) & 3)))) & 1u)
#define CR_PUTBIT(c, n, v) do { \
        unsigned _sh = (7 - ((n) >> 2)) * 4 + (3 - ((n) & 3)); \
        (c)->cr = ((c)->cr & ~(1u << _sh)) | ((uint32_t)(v) << _sh); } while (0)

static inline void CRXOR(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, CR_BIT(c, a) ^ CR_BIT(c, b));
}
static inline void CROR(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, CR_BIT(c, a) | CR_BIT(c, b));
}
static inline void CRAND(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, CR_BIT(c, a) & CR_BIT(c, b));
}
static inline void CRNOR(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, !(CR_BIT(c, a) | CR_BIT(c, b)));
}
static inline void CRORC(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, CR_BIT(c, a) | !CR_BIT(c, b));
}
static inline void CRANDC(PPCContext* c, unsigned d, unsigned a, unsigned b) {
    CR_PUTBIT(c, d, CR_BIT(c, a) & !CR_BIT(c, b));
}
static inline void MCRF(PPCContext* c, unsigned d, unsigned s) {
    unsigned sd = (7 - d) * 4, ss = (7 - s) * 4;
    c->cr = (c->cr & ~(0xFu << sd)) | ((c->cr >> ss) & 0xFu) << sd;
}

// ---- branch conditional evaluation (bc BO/BI semantics) --------------------
// Returns 1 if the branch is taken; decrements CTR when the counter test is
// enabled per BO[2].
static inline int BC_TAKEN(PPCContext* c, unsigned bo, unsigned bi) {
    int t = 1;
    if (!(bo & 0x10)) {                    // test CR bit bi
        unsigned want = (bo >> 3) & 1;
        t = t && (CR_BIT(c, bi) == want);
    }
    if (!(bo & 0x04)) {                    // decrement + test CTR
        c->ctr--;
        unsigned want = (bo >> 1) & 1;
        t = t && ((c->ctr != 0) != want);
    }
    return t;
}

// mtcrf: move mask of CRM nibbles from rs into CR.
static inline void MTCRF(PPCContext* c, uint8_t crm, uint32_t rs) {
    for (unsigned f = 0; f < 8; f++) {
        if (crm & (0x80u >> f)) {
            // CRM bit 7 selects CR0 (the high nibble), and bit 0 selects
            // CR7 (the low nibble). Keep the architectural field ordering;
            // masked fields are used by the guest light opcode stream.
            unsigned sh = 28u - f * 4u;
            c->cr = (c->cr & ~(0xFu << sh)) | ((rs >> sh) & 0xFu) << sh;
        }
    }
}

// ---- float fused ops (determinism policy) --------------------------------
// fmadds computes a*b+c with ONE rounding (round-to-nearest). The host's fmaf()
// is not used because its result depends on the C runtime and CPU: mingw-w64's
// software fmaf and ucrtbase.dll's non-FMA3 path on many Windows builds are not
// correctly rounded.
//
// A float product is exact in a double. Adding c with round-to-odd keeps enough
// information for the final narrowing to round as if from the exact sum, which
// a plain double add does not (it can land on a false float tie). TwoSum yields
// the add's exact error, so an inexact sum with an even significand steps to
// the neighbor on the exact sum's side, which is odd. This needs doubles
// evaluated at double precision in round-to-nearest, without fast-math.
//
// Non-finite sums make err NaN and pass through the plain double computation.
// `m2_ppc_fma` checks finite results against exact integer arithmetic.
static inline float ppc_fma_single(float a, float b, float cc) {
    const double product = (double)a * (double)b;
    const double addend = (double)cc;
    double sum = product + addend;
    const double addend_part = sum - product;
    const double err = (product - (sum - addend_part)) + (addend - addend_part);
    if (err != 0.0 && err == err) {
        uint64_t bits;
        memcpy(&bits, &sum, sizeof(bits));
        if (!(bits & 1u)) {
            if ((err > 0.0) == (sum > 0.0)) bits += 1u;  // away from zero
            else bits -= 1u;                             // toward zero
            memcpy(&sum, &bits, sizeof(bits));
        }
    }
    return (float)sum;
}
static inline float FMADDS(float a, float b, float cc) { return ppc_fma_single(a, b, cc); }     // a*b + c
static inline float FMSUBS(float a, float b, float cc) { return ppc_fma_single(a, b, -cc); }    // a*b - c
static inline float FNMADDS(float a, float b, float cc) { return -ppc_fma_single(a, b, cc); }   // -(a*b + c)
static inline float FNMSUBS(float a, float b, float cc) { return -ppc_fma_single(a, b, -cc); }  // -(a*b - c)
// fnmadds/fnmsubs negate the ROUNDED fmadds/fmsubs result (PPC ISA: single
// rounding, then exact sign flip), so the negation goes outside the helper --
// NOT ppc_fma_single(-a,b,c), which would flip only the product term's sign.

// frsqrte: PPC reciprocal-square-root ESTIMATE (PEM "Floating-Point Estimate"
// instructions). The architecture fixes the special-value results exactly and
// bounds the estimate's relative error at 1/32 for positive finite inputs; the
// estimate itself is a documented hardware table lookup whose 602 single-
// precision coefficients are not recovered in this repository.
//
// This helper satisfies the architectural contract:
//   +-0        -> +-Inf (ZX)          +Inf -> +0        -Inf -> QNaN (VXSQRT)
//   negative   -> QNaN (VXSQRT)       NaN  -> quieted NaN (sign/payload kept)
//   positive finite -> 1/sqrt(x) rounded to fp32, relative error < 2^-23
//                       (well inside the 1/32 bound, gradual underflow kept)
// The exact seed still matters to this image: the shared rsqrtf-style path
// applies two Newton steps, while direct Mercury lighting consumers generally
// apply one step and the point/spot reciprocal-distance path uses a distinct
// reciprocal correction. `m2_frsqrte_sensitivity` bounds those consumer
// shapes without pretending its synthetic endpoint seeds are the 602 table.
// The previous magic-constant seed both exceeded the bound (3.44% > 3.125%)
// and returned finite garbage for every special value above; see
// the PPC semantic-risk notes.
static inline float FRSQRTE(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    const uint32_t sign = u & 0x80000000u;
    const uint32_t exp  = u & 0x7F800000u;
    const uint32_t mant = u & 0x007FFFFFu;
    if (exp == 0x7F800000u) {              // Inf or NaN
        if (mant) {                        // quiet the NaN, keep sign/payload
            union { uint32_t u; float f; } q;
            q.u = u | 0x00400000u;
            return q.f;
        }
        return sign ? (float)NAN : 0.0f;   // -Inf -> QNaN, +Inf -> +0
    }
    if (exp == 0u && mant == 0u)           // +-0 -> +-Inf (zero divide)
        return sign ? -(float)INFINITY : (float)INFINITY;
    if (sign)                              // negative nonzero -> QNaN
        return (float)NAN;
    return (float)(1.0 / sqrt((double)f));
}

// fsel: PPC Book I -> frD = (frA >= 0) ? frC : frB. The comparison is an
// IEEE comparison on the value, so -0.0 counts as >= 0 (selects frC) and a
// NaN of EITHER sign is unordered, fails the test, and selects frB -- the
// sign bit is not what is tested. (The old sign-bit form selected frC for
// +NaN and frB for -0.0, both backwards; live: 76 fsel sites.)
static inline float FSEL(float ca, float cb, float cc) {
    return (ca >= 0.0f) ? cb : cc;
}

// fctiwz: convert with round-toward-zero; out-of-range saturates like hw.
static inline int32_t FCTIWZ(float f) {
    if (!(f == f)) return 0x80000000u;              // NaN
    if (f >= 2147483648.0f)  return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return (int32_t)0x80000000u;
    return (int32_t)f;
}

// frsp: round double->single; our world is single already, identity.
static inline float FRSP(float f) { return f; }

#ifdef __cplusplus
}
#endif
