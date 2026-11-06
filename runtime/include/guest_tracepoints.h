#ifndef GUEST_TRACEPOINTS_H
#define GUEST_TRACEPOINTS_H

// Declarative guest-PC tracepoints.  The transpiler emits a bounded table of
// these records from a JSON specification; generated code calls the generic
// m2_tracepoint_hit() helper at the selected instruction boundaries.

#include <stdint.h>
#include "guest_events.h"

#define M2_TRACEPOINT_MAX_SPECS 64u
#define M2_TRACEPOINT_MAX_MEMORY 16u
#define M2_TRACEPOINT_MAX_CAPTURE_BYTES 256u
#define M2_TRACEPOINT_MAX_PROVENANCE 8u

enum {
    M2_TRACEPOINT_MEMORY_FIXED = 0,
    M2_TRACEPOINT_MEMORY_RELATIVE = 1,
};

enum {
    M2_TRACEPOINT_WHEN_NONE = 0,
    M2_TRACEPOINT_WHEN_OUTSIDE_GUEST_RAM = 1,
};

typedef struct {
    uint32_t ea;
    int32_t offset;
    uint32_t length;
    uint8_t kind;
    uint8_t base_register;
    uint8_t provenance;
    uint8_t reserved;
    char label[32];
} M2TracepointMemorySpec;

typedef struct {
    char name[64];
    uint32_t pc;
    uint32_t limit; // 0 = unlimited
    uint32_t gpr_mask;
    uint8_t capture_lr;
    uint8_t capture_ctr;
    uint8_t capture_cr;
    uint8_t when_kind;
    uint8_t when_register;
    uint8_t reserved[3];
    uint32_t memory_count;
    M2TracepointMemorySpec memory[M2_TRACEPOINT_MAX_MEMORY];
} M2TracepointSpec;

typedef struct {
    uint32_t ea;
    uint32_t writer_pc;
    uint32_t caller_lr;
    uint32_t frame_seq;
    uint32_t write_count;
    uint32_t src_ea;
    uint32_t copy_pc;
    uint32_t src_writer_pc;
    char alloc_tag[32];
} M2TracepointProvenance;

typedef struct {
    uint32_t ea;
    uint32_t base_value;
    int32_t offset;
    uint32_t length;
    uint32_t valid;
    uint32_t kind;
    uint32_t base_register;
    uint32_t provenance_requested;
    char label[32];
    uint8_t bytes[M2_TRACEPOINT_MAX_CAPTURE_BYTES];
    uint32_t provenance_count;
    M2TracepointProvenance provenance[M2_TRACEPOINT_MAX_PROVENANCE];
} M2TracepointMemoryCapture;

typedef struct M2TracepointCapture {
    uint32_t id;
    uint32_t pc;
    uint32_t hit;
    uint32_t guest_frame;
    uint32_t task_id;
    char name[64];
    uint32_t gpr_mask;
    uint32_t gpr[32];
    uint32_t has_lr;
    uint32_t lr;
    uint32_t has_ctr;
    uint32_t ctr;
    uint32_t has_cr;
    uint32_t cr;
    uint32_t memory_count;
    M2TracepointMemoryCapture memory[M2_TRACEPOINT_MAX_MEMORY];
} M2TracepointCapture;

struct PPCContext;

#ifdef __cplusplus
extern "C" {
#endif

extern M2TracepointSpec g_m2_tracepoint_specs[M2_TRACEPOINT_MAX_SPECS];
extern const uint32_t g_m2_tracepoint_count;

void m2_tracepoint_reset(void);
void m2_tracepoint_hit(struct PPCContext* c, uint32_t id, uint32_t pc);

// Exposed for focused unit tests and for generated-table dispatch.  Normal
// generated code should call m2_tracepoint_hit(), not this lower-level entry.
void m2_tracepoint_capture(struct PPCContext* c,
                           const M2TracepointSpec* spec,
                           uint32_t id, uint32_t pc);

// Strongly overridden by the host discovery trace writer; standalone guest
// tests link the weak no-op in trace_stubs.c.
extern void m2_tracepoint_event_c(const M2TracepointCapture* capture)
    __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif // GUEST_TRACEPOINTS_H
