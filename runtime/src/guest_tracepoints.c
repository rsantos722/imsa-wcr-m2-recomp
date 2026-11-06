// guest_tracepoints.c — bounded runtime for transpiler-generated tracepoints.
//
// The generated table is configuration, not guest behavior.  A hit only reads
// the current PPC context and guest memory, then forwards a fixed-width copy to
// the host trace writer.  It never writes guest memory or changes registers.

#include "ppc_ctx.h"
#include "guest_tracepoints.h"

#include <stdatomic.h>
#include <string.h>

static atomic_uint g_tracepoint_hits[M2_TRACEPOINT_MAX_SPECS];

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void copy_text(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t guest_frame(const PPCContext* c) {
    if (!c || !c->mem || M2_GUEST_FRAME_COUNTER_EA > RECOMP_GUEST_MEM - 4u)
        return 0;
    return read_be32(c->mem + M2_GUEST_FRAME_COUNTER_EA);
}

static int claim_hit(uint32_t id, uint32_t limit, uint32_t* out_hit) {
    if (id >= M2_TRACEPOINT_MAX_SPECS) return 0;

    atomic_uint* counter = &g_tracepoint_hits[id];
    unsigned observed = atomic_load_explicit(counter, memory_order_relaxed);
    for (;;) {
        if (limit != 0 && observed >= limit) return 0;
        if (observed == UINT32_MAX) return 0;
        unsigned next = observed + 1u;
        if (atomic_compare_exchange_weak_explicit(
                counter, &observed, next, memory_order_relaxed,
                memory_order_relaxed)) {
            if (out_hit) *out_hit = next;
            return 1;
        }
    }
}

static uint32_t relative_ea(uint32_t base, int32_t offset) {
    // PPC effective-address arithmetic is 32-bit and wraps naturally.
    return (uint32_t)((uint64_t)base + (int64_t)offset);
}

static void copy_provenance(M2TracepointMemoryCapture* dst, uint32_t ea,
                            uint32_t length) {
    if (!dst || !dst->provenance_requested) return;

    // Provenance entries are word-oriented in the shared engine.  Start at
    // each captured 4-byte window, while retaining exact byte/halfword writes
    // when the range starts at their recorded EA.
    uint32_t count = 0;
    for (uint32_t off = 0; off < length && count < M2_TRACEPOINT_MAX_PROVENANCE;) {
        const M2ProvEntry* entry = m2_provenance_find_entry(ea + off);
        if (entry) {
            M2TracepointProvenance* out = &dst->provenance[count++];
            memset(out, 0, sizeof(*out));
            out->ea = entry->ea;
            out->writer_pc = entry->writer_pc;
            out->caller_lr = entry->caller_lr;
            out->frame_seq = entry->frame_seq;
            out->write_count = entry->write_count;
            out->src_ea = entry->src_ea;
            out->copy_pc = entry->copy_pc;
            out->src_writer_pc = entry->src_writer_pc;
            copy_text(out->alloc_tag, sizeof(out->alloc_tag), entry->alloc_tag);
        }
        if (off > UINT32_MAX - 4u) break;
        off += 4u;
    }
    dst->provenance_count = count;
}

static void capture_memory(PPCContext* c, const M2TracepointMemorySpec* spec,
                           M2TracepointMemoryCapture* out) {
    memset(out, 0, sizeof(*out));
    out->kind = spec->kind;
    out->base_register = spec->base_register;
    out->offset = spec->offset;
    out->provenance_requested = spec->provenance ? 1u : 0u;
    out->length = spec->length;
    copy_text(out->label, sizeof(out->label), spec->label);

    uint32_t base = 0;
    if (spec->kind == M2_TRACEPOINT_MEMORY_RELATIVE) {
        if (spec->base_register >= 32u) return;
        base = c->gpr[spec->base_register];
        out->base_value = base;
        out->ea = relative_ea(base, spec->offset);
    } else if (spec->kind == M2_TRACEPOINT_MEMORY_FIXED) {
        out->base_register = UINT32_MAX;
        out->ea = spec->ea;
    } else {
        return;
    }

    if (!c->mem || out->length == 0 ||
        out->length > M2_TRACEPOINT_MAX_CAPTURE_BYTES ||
        out->ea > RECOMP_GUEST_MEM ||
        out->length > RECOMP_GUEST_MEM - out->ea) {
        return;
    }

    memcpy(out->bytes, c->mem + out->ea, out->length);
    out->valid = 1u;
    copy_provenance(out, out->ea, out->length);
}

void m2_tracepoint_reset(void) {
    for (uint32_t i = 0; i < M2_TRACEPOINT_MAX_SPECS; ++i)
        atomic_store_explicit(&g_tracepoint_hits[i], 0u, memory_order_relaxed);
}

void m2_tracepoint_hit(PPCContext* c, uint32_t id, uint32_t pc) {
    if (id >= g_m2_tracepoint_count) return;
    m2_tracepoint_capture(c, &g_m2_tracepoint_specs[id], id, pc);
}

void m2_tracepoint_capture(PPCContext* c, const M2TracepointSpec* spec,
                           uint32_t id, uint32_t pc) {
    if (!c || !spec || (!m2_guest_event_c && !m2_tracepoint_event_c) ||
        id >= M2_TRACEPOINT_MAX_SPECS)
        return;
    if (spec->pc != pc) return;

    if (spec->when_kind == M2_TRACEPOINT_WHEN_OUTSIDE_GUEST_RAM) {
        if (spec->when_register >= 32u ||
            c->gpr[spec->when_register] < RECOMP_M2_RAM_LIMIT)
            return;
    } else if (spec->when_kind != M2_TRACEPOINT_WHEN_NONE) {
        return;
    }

    uint32_t hit = 0;
    if (!claim_hit(id, spec->limit, &hit)) return;

    M2TracepointCapture capture;
    memset(&capture, 0, sizeof(capture));
    capture.id = id;
    capture.pc = pc;
    capture.hit = hit;
    capture.guest_frame = guest_frame(c);
    capture.task_id = 0;
    copy_text(capture.name, sizeof(capture.name), spec->name);
    capture.gpr_mask = spec->gpr_mask;
    for (uint32_t i = 0; i < 32u; ++i) {
        if (spec->gpr_mask & (1u << i)) capture.gpr[i] = c->gpr[i];
    }
    capture.has_lr = spec->capture_lr ? 1u : 0u;
    capture.lr = c->lr;
    capture.has_ctr = spec->capture_ctr ? 1u : 0u;
    capture.ctr = c->ctr;
    capture.has_cr = spec->capture_cr ? 1u : 0u;
    capture.cr = c->cr;

    capture.memory_count = spec->memory_count;
    if (capture.memory_count > M2_TRACEPOINT_MAX_MEMORY)
        capture.memory_count = M2_TRACEPOINT_MAX_MEMORY;
    for (uint32_t i = 0; i < capture.memory_count; ++i)
        capture_memory(c, &spec->memory[i], &capture.memory[i]);

    M2GuestEvent event;
    memset(&event, 0, sizeof(event));
    event.schema_version = M2_GUEST_EVENT_SCHEMA_VERSION;
    event.kind = M2_GUEST_EVENT_TRACEPOINT;
    event.pc = capture.pc;
    event.frame_seq = capture.guest_frame;
    event.tracepoint_id = id;
    event.tracepoint = &capture;
    if (m2_guest_event_c) m2_guest_event_c(&event);
    else m2_tracepoint_event_c(&capture);
}
