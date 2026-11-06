// dsp_template_loader.c — on-disc M2 DSP template loader and DSPP simulator
// core for the pure-synth StartInstrument boundary.
//
// This is a project-authored implementation of the documented M2 DSPP
// behavior:
//   - Template file: FORM 3INS / FORM DSPP with DHDR/DCOD/MRSC/DRLC/DNMS
//     chunks.
//   - Relocation value policy: KNOB -> ADDR operand, INPUT -> packed
//     immediate 0, TRIGGER -> immediate trigger-bit mask, FIFO/others ->
//     attribute address, with the LIST link walk.
//   - Execution: 20-bit accumulator, 16-bit data words, condition codes,
//     barrel shifter, RBASE register windows, FIFO/oscillator register map,
//     SLEEP/AudLock cyclic wake.
//
// The implementation is independently structured. Function and file names in
// the comments identify the behavior being reproduced; they are not copied
// source.
//
// This is a platform-service layer only: it loads the original disc bytes and
// executes the documented M2 DSPP instruction semantics. No game behavior is
// invented here.
//
// The DSP template files live under the disc's System.m2/Audio/dsp tree,
// outside the driving/data root. They are loaded lazily through the same
// weak m2_host_raw_* bridge the guest filesystem uses, after the caller
// resolves the guest-visible template name to that on-disc path.
#include "dsp_template_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Owner-side view of a live sim's resource table for export resolution.
// Matches the public header declaration; the export records are taken at
// register time, after tpl_resources is assigned.
const dspp_template *dspp_sim_resources(const dspp_sim *s);

// ---- IFF chunk walking ------------------------------------------------------

typedef struct {
    uint32_t offset; // offset of chunk body
    uint32_t size;   // body size
} dspp_chunk;

static int dspp_find_chunks(const uint8_t *data, uint32_t size,
                            const char *form_type, uint32_t form_body_off,
                            uint32_t form_body_end, dspp_chunk *out,
                            int max_out) {
    uint32_t off = form_body_off;
    int count = 0;
    while (off + 8 <= form_body_end && count < max_out) {
        char id[5];
        memcpy(id, data + off, 4);
        id[4] = 0;
        uint32_t csize;
        memcpy(&csize, data + off + 4, 4);
        csize = ((csize & 0xFFu) << 24) | ((csize & 0xFF00u) << 8) |
                ((csize & 0xFF0000u) >> 8) | ((csize & 0xFF000000u) >> 24);
        if (csize > form_body_end - off - 8u) break;
        if (strcmp(id, form_type) == 0) {
            out[count].offset = off + 8;
            out[count].size = csize;
            ++count;
        }
        off += 8 + csize + (csize & 1u);
    }
    return count;
}

// Locate a chunk by 4CC within a range; returns body offset or 0.
static uint32_t dspp_find_chunk(const uint8_t *data, uint32_t lo, uint32_t hi,
                                const char *id4, uint32_t *size_out) {
    uint32_t off = lo;
    while (off + 8 <= hi) {
        char id[5];
        memcpy(id, data + off, 4);
        id[4] = 0;
        uint32_t csize = ((uint32_t)data[off + 4] << 24) |
                         ((uint32_t)data[off + 5] << 16) |
                         ((uint32_t)data[off + 6] << 8) |
                         (uint32_t)data[off + 7];
        if (csize > hi - off - 8u) return 0;
        if (memcmp(id, id4, 4) == 0) {
            if (size_out) *size_out = csize;
            return off + 8;
        }
        off += 8 + csize + (csize & 1u);
    }
    return 0;
}

static int32_t dspp_read_i32(const uint8_t *p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

static uint16_t dspp_read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// ---- resource types (dspp_template.h) ---------------------------------------

#define DRSC_TYPE_CODE 0
#define DRSC_TYPE_KNOB 1
#define DRSC_TYPE_VARIABLE 2
#define DRSC_TYPE_INPUT 3
#define DRSC_TYPE_OUTPUT 4
#define DRSC_TYPE_IN_FIFO 5
#define DRSC_TYPE_OUT_FIFO 6
#define DRSC_TYPE_TICKS 7
#define DRSC_TYPE_RBASE 8
#define DRSC_TYPE_TRIGGER 9
#define DRSC_TYPE_HW_DAC_OUTPUT 10
#define DRSC_TYPE_HW_OUTPUT_STATUS 11
#define DRSC_TYPE_HW_OUTPUT_CONTROL 12
#define DRSC_TYPE_HW_ADC_INPUT 13
#define DRSC_TYPE_HW_INPUT_STATUS 14
#define DRSC_TYPE_HW_INPUT_CONTROL 15
#define DRSC_TYPE_HW_NOISE 16
#define DRSC_TYPE_HW_CPU_INT 17
#define DRSC_TYPE_HW_CLOCK 18

#define DRSC_F_BIND 0x20
#define DRSC_F_EXPORT 0x40
#define DRSC_F_INIT_AT_ALLOC 0x01
#define DRSC_F_IMPORT 0x80

// M2 dspp_resources.c export table: one record per DRSC_F_EXPORT
// resource of a live shared-space instrument, matched by resource name.
// dsppImportResource resolves an importing instrument's DRSC_F_IMPORT
// resource to the exporting resource's allocated address; a missing name
// fails the instrument open with AF_ERR_EXTERNALREF exactly as the M2
// folio does on an empty export list.
#define DSPP_MAX_EXPORTED_RESOURCES 16u
typedef struct {
    char name[DSPP_MAX_NAME];
    uint32_t resource_index; // index into the owner sim's resource table
} dspp_exported_resource;
typedef struct dspp_export_owner {
    struct dspp_export_owner *next;
    dspp_sim *sim;
    uint32_t count;
    dspp_exported_resource resources[DSPP_MAX_EXPORTED_RESOURCES];
} dspp_export_owner;

#define DRSC_FIFO_NORMAL 0
#define DRSC_FIFO_STATUS 1
#define DRSC_FIFO_READ 2
#define DRSC_FIFO_OSC 3

// ---- DSPI address space (dspp_addresses.h) ----------------------------------

#define DSPI_DATA_MEMORY_SIZE 0x2E0u
#define DSPI_INPUT0 0x02F0u
#define DSPI_OUTPUT0 0x02E0u
#define DSPI_INPUT_CONTROL 0x03D6u
#define DSPI_OUTPUT_CONTROL 0x03D7u
#define DSPI_INPUT_STATUS 0x03DEu
#define DSPI_OUTPUT_STATUS 0x03DFu
#define DSPI_FIFO_OSC_BASE 0x0300u
#define DSPI_FIFO_OSC_SIZE 8u
#define DSPI_FIFO_OSC_NUM_REGS 6u
#define DSPI_FIFO_OFFSET_DATA 4u
#define DSPI_FIFO_OFFSET_CONTROL 5u
#define DSPI_NUM_DMA_CHANNELS 32u
_Static_assert(DSPI_NUM_DMA_CHANNELS <= 32u,
               "DMA serviceable-channel mask is 32 bits");
#define DSPR_MAX_TRIGGERS 16u
#define DSPI_OUTPUT_FIFO_MAX_DEPTH 8u
#define DSPI_OUTPUT_FIFO_SIZE (DSPI_OUTPUT_FIFO_MAX_DEPTH * 2u)
#define DSPI_AUDLOCK 0x03F6u
#define DSPI_CLOCK 0x03F7u
#define DSPI_NOISE 0x03FFu
#define DSPI_CPU_INT 0x03E6u

// ---- DSPX DMA/control space (dspp_addresses.h) -----------------------------

#define DSPX_INTERRUPT_SET 0x64000u
#define DSPX_INTERRUPT_CLR 0x64004u
#define DSPX_INTERRUPT_ENABLE 0x64008u
#define DSPX_INTERRUPT_DISABLE 0x6400Cu
#define DSPX_INT_DMANEXT_SET 0x64010u
#define DSPX_INT_DMANEXT_CLR 0x64014u
#define DSPX_INT_DMANEXT_ENABLE 0x64018u
#define DSPX_INT_CONSUMED_SET 0x64020u
#define DSPX_INT_CONSUMED_CLR 0x64024u
#define DSPX_INT_CONSUMED_ENABLE 0x64028u
#define DSPX_INT_CONSUMED_DISABLE 0x6402Cu
#define DSPX_INT_UNDEROVER_SET 0x64030u
#define DSPX_INT_UNDEROVER_CLR 0x64034u
#define DSPX_INT_UNDEROVER_ENABLE 0x64038u
#define DSPX_INT_UNDEROVER_DISABLE 0x6403Cu
#define DSPX_F_INT_DMANEXT 0x00000001u
#define DSPX_F_INT_CONSUMED 0x00000002u
#define DSPX_F_INT_UNDEROVER 0x00000008u
#define DSPX_F_INT_TIMER 0x00000100u
#define DSPX_F_INT_ALL_DMA (DSPX_F_INT_DMANEXT | DSPX_F_INT_CONSUMED | \
                           DSPX_F_INT_UNDEROVER)
#define DSPX_FLD_INT_SOFT_WIDTH 16u
#define DSPX_FLD_INT_SOFT_SHIFT 16u
#define DSPX_FLD_INT_SOFT_MASK 0xFFFF0000u
#define DSPX_DMA_STACK 0x65000u
#define DSPX_DMA_STACK_CHANNEL_SIZE 0x10u
#define DSPX_DMA_STACK_CONTROL 0x65200u
#define DSPX_DMA_STACK_CONTROL_SIZE 0x10u
#define DSPX_CHANNEL_ENABLE 0x66000u
#define DSPX_CHANNEL_DISABLE 0x66004u
#define DSPX_CHANNEL_DIRECTION_SET 0x66008u
#define DSPX_CHANNEL_DIRECTION_CLR 0x6600Cu
#define DSPX_CHANNEL_8BIT_SET 0x66010u
#define DSPX_CHANNEL_8BIT_CLR 0x66014u
#define DSPX_CHANNEL_SQXD_SET 0x66018u
#define DSPX_CHANNEL_SQXD_CLR 0x6601Cu
#define DSPX_CHANNEL_RESET 0x66030u
#define DSPX_CHANNEL_STATUS 0x6603Cu
#define DSPX_F_DMA_NEXTVALID 0x0001u
#define DSPX_F_DMA_GO_FOREVER 0x0002u
#define DSPX_F_INT_DMANEXT_EN 0x0004u
#define DSPX_F_SHADOW_SET_DMANEXT 0x00040000u
#define DSPX_F_SHADOW_SET_FOREVER 0x00020000u
#define DSPX_F_SHADOW_SET_NEXTVALID 0x00010000u
#define DSPX_F_SHADOW_SET_ADDRESS_COUNT 0x80000000u

// ---- instruction constants (dspp_instructions.h) ----------------------------

#define DSPN_OPERAND_IMMED 0xC000u
#define DSPN_IMMED_F_JUSTIFY 0x2000u
#define DSPN_OPERAND_ADDR 0x8000u
#define DSPN_ADDR_F_IND 0x0400u
#define DSPN_ADDR_F_WRITE_BACK 0x0800u
#define DSPN_ADDR_MASK 0x03FFu

#define DSPN_OPCODE_NOP 0x8000u
#define DSPN_OPCODE_BAC 0x8080u
#define DSPN_OPCODE_RTS 0x8200u
#define DSPN_OPCODE_SLEEP 0x8380u

#define DSPN_OPCODEMASK_MOVEREG 0xFC00u
#define DSPN_OPCODE_MOVEREG 0x9000u
#define DSPN_OPCODEMASK_MOVEADDR 0xF800u
#define DSPN_OPCODE_MOVEADDR 0x9800u
#define DSPN_OPCODEMASK_CTRL 0xFC00u
#define DSPN_OPCODE_BULLDOG_RBASE 0x9400u

#define DSPP_MAX_SIGNED 0x00007FFF
#define DSPP_MIN_SIGNED 0xFFFF8000

// Audio Folio delay-line memory.  The original sample item owns a cleared
// supervisor buffer; each FIFO attachment has its own DMA cursor into that
// shared buffer.  Keeping the cursors on the DMA channel is important: a
// send attachment and one or more tap attachments may share the same delay
// line without sharing their current addresses.
struct dspp_delay_line {
    uint16_t *words;
    uint32_t num_bytes;
    uint32_t num_channels;
    uint32_t frame_count;
    uint32_t word_count;
    uint8_t if_loop;
    uint8_t clear_on_stop;
    uint8_t authored_ranges;
    uint8_t has_sustain_loop;
    uint8_t has_release_loop;
    int32_t sustain_begin;
    int32_t sustain_end;
    int32_t release_begin;
    int32_t release_end;
    uint32_t bindings;
    uint32_t stopped_bindings;
};

enum {
    DSPP_DELAY_PHASE_WHOLE = 0,
    DSPP_DELAY_PHASE_FINITE,
    DSPP_DELAY_PHASE_SUSTAIN_INITIAL,
    DSPP_DELAY_PHASE_SUSTAIN_LOOP,
    DSPP_DELAY_PHASE_RELEASE_INITIAL,
    DSPP_DELAY_PHASE_RELEASE_LOOP,
    DSPP_DELAY_PHASE_RELEASE_GAP,
    DSPP_DELAY_PHASE_RELEASE_TAIL,
};

typedef struct dspp_dma_channel {
    uint8_t *current_buffer;
    uint32_t current_count;
    uint32_t current_address;
    uint8_t *next_buffer;
    uint32_t next_count;
    uint32_t next_address;
    uint32_t previous_value;
    uint8_t next_valid;
    uint8_t go_forever;
    uint8_t enabled;
    uint8_t direction;
    uint8_t width8;
    uint8_t sqxd;
    uint8_t complete;
    // Validated numeric (non-sim) descriptor record. Separate from
    // current_*/host buffers so a raw address never fabricates samples; the
    // DMA/FIFO/interrupt/completion service never reads these fields.
    uint8_t numeric_attached;
    uint32_t numeric_address;
    uint32_t numeric_count;
    uint32_t numeric_flags;
    // Consumed generation: words fully drained via successful DSPP-side FIFO
    // reads on this channel while numeric_attached. 32-bit saturating;
    // successful attach resets to 0. Never raises signals; delivery is a
    // separate seam. DMA-side reads and CURRENT/NEXT peeks never count.
    uint32_t numeric_consumed;
    // Completion-qualified event record: set when the consumed generation
    // first reaches the bound numeric_count (count > 0). pending is 1 while
    // the channel's completion event waits in the per-space poll queue;
    // done latches the once-per-bind edge so post-poll over-drains never
    // re-queue. Successful attach clears both (and dequeues); failed
    // attaches leave both undisturbed. Never raises signals; the folio
    // MonitorAttachment pump seam owns delivery.
    uint8_t numeric_completion_pending;
    uint8_t numeric_completion_done;
    // Audio Folio delay-line attachment state.  This is separate from the
    // numeric descriptor and ordinary host-buffer adapters: a delay line is
    // a supervisor-owned circular sample buffer, not a guest EA.
    dspp_delay_line *delay_line;
    dspp_sim *delay_owner;
    uint32_t delay_cursor;
    uint32_t delay_segment_end;
    uint32_t delay_loop_begin;
    uint32_t delay_loop_end;
    uint8_t delay_phase;
    uint8_t delay_released;
    uint8_t delay_release_pending;
    uint8_t delay_finished;
    uint8_t delay_stopped;
    // Natural finite delay-line completion is queued through the same
    // per-space poll FIFO as numeric descriptors, but has its own lifecycle
    // latch so explicit stop never fabricates a Cue event.
    uint8_t delay_completion_pending;
    uint8_t delay_completion_done;
} dspp_dma_channel;

// One M2 DSPP has one addressable data memory shared by every live
// instrument.  The standalone loader path keeps a private 0x400-word image
// for focused simulator tests; dspp_sim_space supplies the hardware-shaped
// allocator and backing words used by runtime-created instruments.
struct dspp_sim_space {
    uint16_t data[DSPI_DATA_MEMORY_SIZE];
    uint8_t used[DSPI_DATA_MEMORY_SIZE];
    uint8_t fifo_used[DSPI_NUM_DMA_CHANNELS];
    uint8_t trigger_used[DSPR_MAX_TRIGGERS];
    // One DSPI register bank serves every live instrument on the DSPP.
    uint16_t dspi_frequency;
    uint16_t dspi_phase;
    uint16_t dspi_last_osc_count;
    uint16_t dspi_inputs[2];
    uint16_t dspi_outputs[8];
    uint16_t dspi_output_fifo[DSPI_OUTPUT_FIFO_SIZE];
    uint32_t dspi_output_write_ptr;
    uint32_t dspi_output_read_ptr;
    // The M2 DSPP has one FDMA FIFO/cursor state block for all live
    // instruments. Shared sims therefore resolve a channel through this
    // global physical state; standalone sims retain their private arrays.
    uint16_t fifo[DSPI_NUM_DMA_CHANNELS][8];
    uint32_t fifo_depth[DSPI_NUM_DMA_CHANNELS];
    uint32_t fifo_dma_ptr[DSPI_NUM_DMA_CHANNELS];
    uint32_t fifo_dspi_ptr[DSPI_NUM_DMA_CHANNELS];
    uint16_t fifo_prev[DSPI_NUM_DMA_CHANNELS];
    dspp_dma_channel dma[DSPI_NUM_DMA_CHANNELS];
    // Bit n is set while channel n is enabled and not complete. The per-
    // instruction FDMA check walks only these channels instead of probing all
    // 32 hardware channels; every DMA state transition refreshes its bit.
    uint32_t dma_serviceable_mask;
    // Nonzero when the FDMA check could give a new answer: the check depends
    // only on channel state and FIFO depth, and every write to either sets
    // this. dspp_sim_tick skips the check while it is clear.
    uint32_t dma_check_pending;
    uint32_t dma_shadow_current_address;
    uint32_t dma_shadow_current_count;
    uint32_t dma_shadow_next_address;
    uint32_t dma_shadow_next_count;
    uint32_t dma_main_interrupt_partial;
    uint32_t dma_main_interrupt_enable;
    // Shared-space parity with the per-sim Next pair (disarmed when zeroed).
    uint32_t dma_next_interrupt;
    uint32_t dma_next_interrupt_enable;
    uint32_t dma_consumed_interrupt;
    uint32_t dma_consumed_interrupt_enable;
    uint32_t dma_underover_interrupt;
    uint32_t dma_underover_interrupt_enable;
    // Completion-qualified event queue: one FIFO per DSPP addressable space,
    // shared across sims like all DMA state. Holds channel ids (0..31) with
    // a pending completion event, oldest at completion_queue[completion_head].
    // Bounded at DSPI_NUM_DMA_CHANNELS entries (deduped per channel, so the
    // bound holds by construction); a defensive overflow drops the oldest
    // entry (documented, event discarded via the retained done latch). Pure
    // data: polled by the folio MonitorAttachment pump seam, never signalled
    // here.
    uint8_t completion_queue[DSPI_NUM_DMA_CHANNELS];
    uint32_t completion_head;
    uint32_t completion_count;
    // M2 dspp_exportedResourceList: one record per DRSC_F_EXPORT
    // resource across every live shared-space instrument (the on-disc
    // nanokernel is the exporter for gMixer/gScratch1/gReserved*/gBenchStart
    // and the rate-list jump labels). dsppImportResource resolves imports
    // here by name at instrument-open time.
    dspp_export_owner *export_owners;
    // Bumped whenever an owner registers or drops. Callers that resolve an
    // export name once (the per-DAC-frame DAC staging) reuse the address until
    // the table changes.
    uint32_t export_generation;
    // The loaded on-disc nanokernel (head) instrument, opened once per
    // space like the M2 folio's single head. Owns the gMixer words and
    // the DSPI hardware relocations the folio DAC staging reads.
    dspp_sim *kernel_sim;
    // The M2 has one 0x400-word code memory. Imports must branch into the
    // exporting instrument while retaining the caller's DSP registers.
    dspp_sim *code_owners[0x400];
    struct {
        char name[DSPP_MAX_NAME];
        dspp_sim *sim;
        uint32_t references;
        uint8_t loading;
    } libraries[DSPP_MAX_DYNAMIC_LINKS];
    // A failed load (no disc bridge, parse failure, allocation failure)
    // latches so later opens report EXTERNALREF instead of retrying per
    // instrument; space recreation resets it.
    uint8_t kernel_open_failed;
};

// M2 dsppImportResource (dspp_resources.c): find the named export and
// return the owner resource's allocation; AF_ERR_EXTERNALREF-equivalent
// (-1) when no live instrument exports that name. dsppUnimportResource is
// a M2 no-op beyond list removal, so unimport needs no host state.
static int dspp_space_import_resource(dspp_sim_space *space,
                                      const char *name, uint32_t *out_addr) {
    if (!space || !name || !out_addr) return -1;
    for (dspp_export_owner *owner = space->export_owners; owner;
         owner = owner->next) {
        if (!owner->sim) continue;
        for (uint32_t i = 0; i < owner->count; ++i) {
            if (strncmp(owner->resources[i].name, name, DSPP_MAX_NAME) != 0)
                continue;
            const dspp_template *res = dspp_sim_resources(owner->sim);
            uint32_t idx = owner->resources[i].resource_index;
            if (!res || idx >= res->resource_count) continue;
            if (res->resources[idx].alloc_value == 0xFFFFFFFFu) continue;
            *out_addr = res->resources[idx].alloc_value;
            return 0;
        }
    }
    return -1;
}

static int16_t dspp_sign16(uint16_t v) {
    return (v & 0x8000u) ? (int16_t)(int32_t)v - 0x10000 : (int16_t)v;
}

static int32_t dspp_sign20(int32_t v) {
    v &= 0xFFFFF;
    return (v & 0x80000) ? v - 0x100000 : v;
}

static uint16_t dspp_pack_immediate(uint16_t value) {
    // dspnPackImmediate: values with low 3 bits set stay right-justified;
    // others left-justify by 3 with the JUSTIFY flag.
    if (value & 0x7u)
        return (uint16_t)(DSPN_OPERAND_IMMED | (value & 0x1FFFu));
    return (uint16_t)(DSPN_OPERAND_IMMED | DSPN_IMMED_F_JUSTIFY |
                      ((value >> 3) & 0x1FFFu));
}

static uint16_t dspp_fifo_part_address(uint32_t channel, uint32_t part) {
    return (uint16_t)(DSPI_FIFO_OSC_BASE + channel * DSPI_FIFO_OSC_SIZE +
                      part);
}

// dsppOpenResource's Bulldog hardware-resource table. These resources do not
// consume data memory; their allocation is the fixed DSPI register address.
// Audio-input enablement is a separate folio gate on hardware, while the
// bounded simulator exposes DSPI_INPUT0 through dspp_sim_set_input().
static uint32_t dspp_hw_resource_address(uint32_t type) {
    switch (type) {
    case DRSC_TYPE_HW_DAC_OUTPUT:
        return DSPI_OUTPUT0;
    case DRSC_TYPE_HW_OUTPUT_STATUS:
        return DSPI_OUTPUT_STATUS;
    case DRSC_TYPE_HW_OUTPUT_CONTROL:
        return DSPI_OUTPUT_CONTROL;
    case DRSC_TYPE_HW_ADC_INPUT:
        return DSPI_INPUT0;
    case DRSC_TYPE_HW_INPUT_STATUS:
        return DSPI_INPUT_STATUS;
    case DRSC_TYPE_HW_INPUT_CONTROL:
        return DSPI_INPUT_CONTROL;
    case DRSC_TYPE_HW_NOISE:
        return DSPI_NOISE;
    case DRSC_TYPE_HW_CPU_INT:
        return DSPI_CPU_INT;
    case DRSC_TYPE_HW_CLOCK:
        return DSPI_CLOCK;
    default:
        return 0xFFFFFFFFu;
    }
}

static uint16_t dspp_resource_attribute(const dspp_resource *r,
                                        uint32_t attribute);

// ---- loader -----------------------------------------------------------------

static int dspp_parse_template(const uint8_t *data, uint32_t size,
                               dspp_template *out) {
    memset(out, 0, sizeof(*out));
    if (size < 12) return -1;
    if (memcmp(data, "FORM", 4) != 0 || memcmp(data + 8, "3INS", 4) != 0)
        return -1;

    // Optional NAME chunk declares the template name.
    uint32_t name_size = 0;
    uint32_t name_off = dspp_find_chunk(data, 12, size, "NAME", &name_size);
    if (name_off && name_size > 0) {
        uint32_t n = name_size < sizeof(out->name) - 1
                         ? name_size
                         : sizeof(out->name) - 1;
        memcpy(out->name, data + name_off, n);
        out->name[n] = 0;
    }

    // Locate the nested FORM DSPP.
    uint32_t off = 12;
    uint32_t dspp_body = 0, dspp_end = 0;
    while (off + 8 <= size) {
        char id[5];
        memcpy(id, data + off, 4);
        id[4] = 0;
        uint32_t csize = ((uint32_t)data[off + 4] << 24) |
                         ((uint32_t)data[off + 5] << 16) |
                         ((uint32_t)data[off + 6] << 8) |
                         (uint32_t)data[off + 7];
        if (csize > size - off - 8u) return -1;
        if (memcmp(id, "FORM", 4) == 0 &&
            memcmp(data + off + 8, "DSPP", 4) == 0) {
            dspp_body = off + 12;
            dspp_end = off + 8 + csize;
            break;
        }
        off += 8 + csize + (csize & 1u);
    }
    if (!dspp_body) return -1;

    // dspp_newloader.c: DLNK is a packed list of NUL-terminated template
    // names; audio_instr.c opens those instruments before client resources.
    uint32_t link_size = 0;
    const uint32_t links = dspp_find_chunk(data, dspp_body, dspp_end, "DLNK", &link_size);
    for (uint32_t pos = 0; links && pos < link_size;) {
        uint32_t end = pos;
        while (end < link_size && data[links + end]) ++end;
        if (end == link_size || end == pos || end - pos >= DSPP_MAX_NAME ||
            out->dynamic_link_count == DSPP_MAX_DYNAMIC_LINKS) return -1;
        memcpy(out->dynamic_links[out->dynamic_link_count++], data + links + pos, end - pos);
        pos = end + 1;
    }

    // DHDR: function id, silicon version, format version, flags.
    uint32_t dhdr_size = 0;
    uint32_t dhdr = dspp_find_chunk(data, dspp_body, dspp_end, "DHDR",
                                    &dhdr_size);
    if (!dhdr || dhdr_size < 16) return -1;
    out->function_id = (uint32_t)dspp_read_i32(data + dhdr);
    out->silicon_version = (uint32_t)dspp_read_i32(data + dhdr + 4);
    out->format_version = (uint32_t)dspp_read_i32(data + dhdr + 8);
    out->flags = (uint32_t)dspp_read_i32(data + dhdr + 12);
    // M2 dsppValidateTemplate rejects only the format version; older
    // silicon (< BULLDOG) takes the dsppWhetherRemapTemplate remap path.
    if (out->silicon_version < 1u || out->silicon_version > 5u ||
        out->format_version != 4u /* DHDR_SUPPORTED_FORMAT_VERSION */)
        return -1;

    // MRSC resources + DNMS name table.
    uint32_t mrsc_size = 0;
    uint32_t mrsc = dspp_find_chunk(data, dspp_body, dspp_end, "MRSC",
                                    &mrsc_size);
    if (!mrsc || mrsc_size % 20u != 0) return -1;
    uint32_t count = mrsc_size / 20u;
    if (count > DSPP_MAX_RESOURCES) return -1;
    uint32_t dnms_size = 0;
    uint32_t dnms = dspp_find_chunk(data, dspp_body, dspp_end, "DNMS",
                                    &dnms_size);
    if (!dnms) return -1;
    // Packed NUL-terminated names in resource order.
    uint32_t name_index = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec = data + mrsc + i * 20u;
        dspp_resource *r = &out->resources[i];
        r->subtype = rec[1];
        r->flags = rec[2];
        r->type = rec[3];
        r->many = (uint32_t)dspp_read_i32(rec + 4);
        r->allocated = (uint32_t)dspp_read_i32(rec + 8);
        r->bind_index = (uint32_t)dspp_read_i32(rec + 12);
        r->default_value = (uint32_t)dspp_read_i32(rec + 16);
        // copy name
        uint32_t start = name_index;
        while (name_index < dnms_size && data[dnms + name_index] != 0)
            ++name_index;
        uint32_t len = name_index - start;
        if (len > DSPP_MAX_NAME - 1u) len = DSPP_MAX_NAME - 1u;
        memcpy(r->name, data + dnms + start, len);
        r->name[len] = 0;
        ++name_index; // skip NUL
        if (name_index > dnms_size) return -1;
    }
    out->resource_count = count;

    // DCOD: header table then code image words.
    uint32_t dcod_size = 0;
    uint32_t dcod = dspp_find_chunk(data, dspp_body, dspp_end, "DCOD",
                                    &dcod_size);
    if (!dcod || dcod_size < 12) return -1;
    int32_t first_offset = dspp_read_i32(data + dcod + 4);
    if (first_offset < 12 || (first_offset % 12u) != 0) return -1;
    uint32_t header_count = (uint32_t)first_offset / 12u;
    if (header_count != 1) return -1; // one run hunk (DCOD_RUN_DSPP)
    int32_t ctype = dspp_read_i32(data + dcod);
    int32_t coff = dspp_read_i32(data + dcod + 4);
    int32_t csize = dspp_read_i32(data + dcod + 8);
    if (ctype != 0 /* DCOD_RUN_DSPP */) return -1;
    if (coff < (int32_t)(header_count * 12u)) return -1;
    if (csize <= 0 || csize > DSPP_MAX_CODE_WORDS) return -1;
    if ((int64_t)coff + (int64_t)csize * 2 > (int64_t)dcod_size) return -1;
    out->code_word_count = (uint32_t)csize;
    for (int32_t i = 0; i < csize; ++i)
        out->code_words[i] = dspp_read_u16(data + dcod + coff + i * 2);

    // DRLC relocations.
    uint32_t drlc_size = 0;
    uint32_t drlc = dspp_find_chunk(data, dspp_body, dspp_end, "DRLC",
                                    &drlc_size);
    if (drlc) {
        if (drlc_size % 8u != 0) return -1;
        uint32_t rcount = drlc_size / 8u;
        if (rcount > DSPP_MAX_RELOCATIONS) return -1;
        for (uint32_t i = 0; i < rcount; ++i) {
            const uint8_t *rec = data + drlc + i * 8u;
            dspp_relocation *rel = &out->relocations[i];
            rel->resource_index = dspp_read_u16(rec);
            rel->part = dspp_read_u16(rec + 2);
            // DSPPRelocation: u16 rsrc, u16 part, u8 reserved, u8 hunk,
            // u16 codeOffset (dspp_template.h).
            rel->code_hunk = rec[5];
            rel->code_offset = dspp_read_u16(rec + 6);
        }
        out->relocation_count = rcount;
    }

    // DINI data initializers.
    uint32_t dini_size = 0;
    uint32_t dini = dspp_find_chunk(data, dspp_body, dspp_end, "DINI",
                                    &dini_size);
    if (dini) {
        uint32_t o = 0;
        while (o + 16 <= dini_size &&
               out->initializer_count < DSPP_MAX_INITIALIZERS) {
            dspp_initializer *ini = &out->initializers[out->initializer_count];
            int32_t many = dspp_read_i32(data + dini + o);
            int32_t iflags = dspp_read_i32(data + dini + o + 4);
            int32_t rsrc = dspp_read_i32(data + dini + o + 8);
            if (many < 0 || (uint32_t)many > DSPP_MAX_INITIALIZER_VALUES)
                return -1;
            // Compare in 64-bit space: many*4 can wrap uint32_t.
            if ((uint64_t)o + 16u + (uint64_t)many * 4u > dini_size)
                return -1;
            ini->flags = (uint32_t)iflags;
            ini->resource_index = (uint32_t)rsrc;
            ini->value_count = (uint32_t)many;
            for (int32_t k = 0; k < many; ++k)
                ini->values[k] = (uint32_t)dspp_read_i32(data + dini + o +
                                                         16 + k * 4);
            ++out->initializer_count;
            o += 16 + (uint32_t)many * 4u;
        }
    }
    return 0;
}

int dspp_load_template(const uint8_t *data, uint32_t size,
                       dspp_template *out) {
    if (!data || !out) return -1;
    return dspp_parse_template(data, size, out);
}

// ---- resource allocation (Bulldog allocator subset) -------------------------

static int dspp_alloc_resources(dspp_template *t) {
    uint32_t data_next = 0;
    uint32_t rbase_next = 4; // DSPR_RBASE8_FIRST_REG
    uint32_t trigger_next = 0;
    uint32_t fifo_next = 0;

    for (uint32_t i = 0; i < t->resource_count; ++i) {
        dspp_resource *r = &t->resources[i];
        r->alloc_value = 0xFFFFFFFFu; // DRSC_ALLOC_INVALID sentinel
        if (r->flags & DRSC_F_IMPORT) continue;
        switch (r->type) {
        case DRSC_TYPE_CODE:
            r->alloc_value = 0; // entry address resolved after code placement
            break;
        case DRSC_TYPE_TICKS:
            r->alloc_value = r->many;
            break;
        case DRSC_TYPE_KNOB:
        case DRSC_TYPE_VARIABLE:
        case DRSC_TYPE_OUTPUT:
            if (data_next + r->many > DSPI_DATA_MEMORY_SIZE) return -1;
            r->alloc_value = data_next;
            data_next += r->many;
            break;
        case DRSC_TYPE_INPUT:
            r->alloc_value = 0;
            break;
        case DRSC_TYPE_HW_DAC_OUTPUT:
        case DRSC_TYPE_HW_OUTPUT_STATUS:
        case DRSC_TYPE_HW_OUTPUT_CONTROL:
        case DRSC_TYPE_HW_ADC_INPUT:
        case DRSC_TYPE_HW_INPUT_STATUS:
        case DRSC_TYPE_HW_INPUT_CONTROL:
        case DRSC_TYPE_HW_NOISE:
        case DRSC_TYPE_HW_CPU_INT:
        case DRSC_TYPE_HW_CLOCK:
            r->alloc_value = dspp_hw_resource_address(r->type);
            break;
        case DRSC_TYPE_RBASE:
            if (rbase_next + r->many > 32u) return -1;
            r->alloc_value = rbase_next;
            rbase_next += 8; // DSPR_RBASE8_NUM_REGS
            break;
        case DRSC_TYPE_TRIGGER:
            if (trigger_next >= 16u) return -1;
            r->alloc_value = trigger_next++;
            break;
        case DRSC_TYPE_IN_FIFO:
        case DRSC_TYPE_OUT_FIFO:
            if (fifo_next >= DSPI_NUM_DMA_CHANNELS) return -1;
            r->alloc_value = fifo_next++;
            break;
        default:
            break;
        }
    }
    // Bound resources resolve through dsppBindResource. RBASE resources bound
    // to a FIFO store its DSPI attribute address, not the channel number;
    // memory/RBASE bindings retain the base word plus the template offset.
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        dspp_resource *r = &t->resources[i];
        if ((r->flags & DRSC_F_BIND) && r->bind_index < t->resource_count) {
            dspp_resource *base = &t->resources[r->bind_index];
            if (base->alloc_value != 0xFFFFFFFFu &&
                (base->type == DRSC_TYPE_IN_FIFO ||
                 base->type == DRSC_TYPE_OUT_FIFO)) {
                if (r->allocated > DRSC_FIFO_OSC) return -1;
                r->alloc_value = dspp_resource_attribute(base, r->allocated);
            } else if (base->alloc_value != 0xFFFFFFFFu) {
                r->alloc_value = base->alloc_value + r->allocated;
            }
        }
    }
    return 0;
}

static int dspp_space_alloc_words(dspp_sim_space *space, uint32_t words,
                                  uint32_t alignment, uint32_t *out) {
    if (!space || !out || words > DSPI_DATA_MEMORY_SIZE) return -1;
    if (words == 0) {
        *out = 0;
        return 0;
    }
    if (alignment == 0) alignment = 1;
    for (uint32_t start = 0; start + words <= DSPI_DATA_MEMORY_SIZE;) {
        int free_range = 1;
        for (uint32_t i = 0; i < words; ++i) {
            if (space->used[start + i]) {
                free_range = 0;
                break;
            }
        }
        if (free_range) {
            for (uint32_t i = 0; i < words; ++i)
                space->used[start + i] = 1;
            *out = start;
            return 0;
        }
        uint32_t next = start + 1u;
        if (alignment > 1u)
            next = (next + alignment - 1u) & ~(alignment - 1u);
        if (next <= start) break;
        start = next;
    }
    return -1;
}

static int dspp_space_alloc_slots(uint8_t *used, uint32_t capacity,
                                  uint32_t count, uint32_t *out) {
    if (!used || !out || count == 0u || count > capacity) return -1;
    for (uint32_t start = 0; start + count <= capacity; ++start) {
        int free_range = 1;
        for (uint32_t i = 0; i < count; ++i) {
            if (used[start + i]) {
                free_range = 0;
                break;
            }
        }
        if (!free_range) continue;
        for (uint32_t i = 0; i < count; ++i)
            used[start + i] = 1;
        *out = start;
        return 0;
    }
    return -1;
}

static void dspp_space_release_resources(dspp_sim_space *space,
                                         const dspp_template *t) {
    if (!space || !t) return;
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        const dspp_resource *r = &t->resources[i];
        if (r->alloc_value == 0xFFFFFFFFu || (r->flags & DRSC_F_IMPORT) ||
            (r->flags & DRSC_F_BIND))
            continue;
        if (r->type == DRSC_TYPE_KNOB || r->type == DRSC_TYPE_VARIABLE ||
            r->type == DRSC_TYPE_OUTPUT || r->type == DRSC_TYPE_RBASE) {
            uint32_t words = r->many;
            if (r->alloc_value >= DSPI_DATA_MEMORY_SIZE ||
                words > DSPI_DATA_MEMORY_SIZE - r->alloc_value)
                continue;
            for (uint32_t k = 0; k < words; ++k) {
                space->used[r->alloc_value + k] = 0;
                space->data[r->alloc_value + k] = 0;
            }
        } else if (r->type == DRSC_TYPE_IN_FIFO ||
                   r->type == DRSC_TYPE_OUT_FIFO) {
            if (r->alloc_value >= DSPI_NUM_DMA_CHANNELS ||
                r->many > DSPI_NUM_DMA_CHANNELS - r->alloc_value)
                continue;
            for (uint32_t k = 0; k < r->many; ++k)
                space->fifo_used[r->alloc_value + k] = 0;
        } else if (r->type == DRSC_TYPE_TRIGGER) {
            if (r->alloc_value >= DSPR_MAX_TRIGGERS ||
                r->many > DSPR_MAX_TRIGGERS - r->alloc_value)
                continue;
            for (uint32_t k = 0; k < r->many; ++k)
                space->trigger_used[r->alloc_value + k] = 0;
        }
    }
}

// Shared equivalent of the M2 dspp_resources_bulldog allocator. Data/RBASE,
// FIFO-channel, and trigger resources all draw from their global hardware
// tables, so an operand or DSPI register patched for one live instrument does
// not alias a second instrument's resource.
static int dspp_alloc_resources_shared(dspp_template *t,
                                       dspp_sim_space *space, uint32_t code_base) {
    if (!t || !space) return -1;
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        dspp_resource *r = &t->resources[i];
        r->alloc_value = 0xFFFFFFFFu;
        if (r->flags & DRSC_F_IMPORT) {
            // dsppOpenResource -> dsppImportResource: resolve the named
            // export from a live instrument in the same space. No exporter
            // yet means the open fails with AF_ERR_EXTERNALREF, exactly as
            // the M2 folio behaves on an empty export list.
            uint32_t imported = 0;
            if (dspp_space_import_resource(space, r->name, &imported) != 0)
                goto fail;
            r->alloc_value = imported;
            continue;
        }
        // A BIND resource aliases its base allocation; it does not consume a
        // second range in the global data allocator.
        if (r->flags & DRSC_F_BIND) continue;
        switch (r->type) {
        case DRSC_TYPE_CODE:
            r->alloc_value = code_base;
            break;
        case DRSC_TYPE_TICKS:
            r->alloc_value = r->many;
            break;
        case DRSC_TYPE_KNOB:
        case DRSC_TYPE_VARIABLE:
        case DRSC_TYPE_OUTPUT:
            if (dspp_space_alloc_words(space, r->many, 1,
                                       &r->alloc_value) != 0)
                goto fail;
            break;
        case DRSC_TYPE_INPUT:
            r->alloc_value = 0;
            break;
        case DRSC_TYPE_HW_DAC_OUTPUT:
        case DRSC_TYPE_HW_OUTPUT_STATUS:
        case DRSC_TYPE_HW_OUTPUT_CONTROL:
        case DRSC_TYPE_HW_ADC_INPUT:
        case DRSC_TYPE_HW_INPUT_STATUS:
        case DRSC_TYPE_HW_INPUT_CONTROL:
        case DRSC_TYPE_HW_NOISE:
        case DRSC_TYPE_HW_CPU_INT:
        case DRSC_TYPE_HW_CLOCK:
            r->alloc_value = dspp_hw_resource_address(r->type);
            break;
        case DRSC_TYPE_RBASE:
            if (dspp_space_alloc_words(space, r->many, 4,
                                       &r->alloc_value) != 0)
                goto fail;
            break;
        case DRSC_TYPE_TRIGGER:
            if (dspp_space_alloc_slots(space->trigger_used,
                                       DSPR_MAX_TRIGGERS, r->many,
                                       &r->alloc_value) != 0)
                goto fail;
            break;
        case DRSC_TYPE_IN_FIFO:
        case DRSC_TYPE_OUT_FIFO:
            if (dspp_space_alloc_slots(space->fifo_used,
                                       DSPI_NUM_DMA_CHANNELS, r->many,
                                       &r->alloc_value) != 0)
                goto fail;
            break;
        default:
            break;
        }
    }
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        dspp_resource *r = &t->resources[i];
        if (!(r->flags & DRSC_F_BIND)) continue;
        if (r->bind_index >= t->resource_count) goto fail;
        dspp_resource *base = &t->resources[r->bind_index];
        if (base->alloc_value == 0xFFFFFFFFu) goto fail;
        if (base->type == DRSC_TYPE_IN_FIFO ||
            base->type == DRSC_TYPE_OUT_FIFO) {
            if (r->allocated > DRSC_FIFO_OSC) goto fail;
            r->alloc_value = dspp_resource_attribute(base, r->allocated);
        } else {
            if (r->allocated > base->many ||
                r->many > base->many - r->allocated)
                goto fail;
            r->alloc_value = base->alloc_value + r->allocated;
        }
    }
    return 0;

fail:
    dspp_space_release_resources(space, t);
    return -1;
}

// dsppGetResourceAttribute (Bulldog variant).
static uint16_t dspp_resource_attribute(const dspp_resource *r,
                                        uint32_t attribute) {
    if (r->type == DRSC_TYPE_IN_FIFO || r->type == DRSC_TYPE_OUT_FIFO) {
        switch (attribute) {
        case DRSC_FIFO_NORMAL:
            return dspp_fifo_part_address(r->alloc_value,
                                          DSPI_FIFO_OFFSET_DATA);
        case DRSC_FIFO_STATUS:
            return dspp_fifo_part_address(r->alloc_value,
                                          DSPI_FIFO_OFFSET_CONTROL);
        case DRSC_FIFO_READ:
        case DRSC_FIFO_OSC:
            return dspp_fifo_part_address(r->alloc_value, 0);
        default:
            return 0;
        }
    }
    if (r->alloc_value == 0xFFFFFFFFu) return 0;
    return (uint16_t)((r->alloc_value + attribute) & 0xFFFFu);
}

// dsppGetResourceRelocationValue (dspp_relocator.c).
static uint16_t dspp_relocation_value(const dspp_resource *r,
                                      uint32_t part) {
    switch (r->type) {
    case DRSC_TYPE_KNOB:
        return (uint16_t)(DSPN_OPERAND_ADDR |
                          ((r->alloc_value + part) & DSPN_ADDR_MASK));
    case DRSC_TYPE_INPUT:
        return dspp_pack_immediate(0);
    case DRSC_TYPE_TRIGGER:
        return dspp_pack_immediate((uint16_t)(1u << r->alloc_value));
    default:
        return dspp_resource_attribute(r, part);
    }
}

static void dspp_apply_relocations(dspp_template *t) {
    // Walk the pre-relocation image: patching consumes link fields, so the
    // chain must come from a pristine snapshot (M2 dsppRelocate walks
    // the template image while fixing up the instrument copy).
    if (t->code_word_count > DSPP_MAX_CODE_WORDS) return;
    uint16_t image[DSPP_MAX_CODE_WORDS];
    memcpy(image, t->code_words, t->code_word_count * 2);
    for (uint32_t i = 0; i < t->relocation_count; ++i) {
        const dspp_relocation *rel = &t->relocations[i];
        if (rel->resource_index >= t->resource_count) continue;
        if (rel->code_hunk != 0) continue; // single run hunk only
        const dspp_resource *r = &t->resources[rel->resource_index];
        uint16_t value = dspp_relocation_value(r, rel->part);
        // GetRelocMasks: KNOB/INPUT/TRIGGER patch the full word and walk
        // links through the 10-bit address field; RBASE patches bits 2..9
        // with no link; everything else patches the 10-bit field with links.
        uint16_t field_mask, link_mask;
        if (r->type == DRSC_TYPE_KNOB || r->type == DRSC_TYPE_INPUT ||
            r->type == DRSC_TYPE_TRIGGER) {
            field_mask = 0xFFFFu;
            link_mask = DSPN_ADDR_MASK;
        } else if (r->type == DRSC_TYPE_RBASE) {
            field_mask = 0x03FCu;
            link_mask = 0;
        } else {
            field_mask = DSPN_ADDR_MASK;
            link_mask = DSPN_ADDR_MASK;
        }
        uint32_t offset = rel->code_offset;
        if (offset >= t->code_word_count) continue;
        for (uint32_t guard = 0; guard <= t->code_word_count; ++guard) {
            uint16_t pad = image[offset];
            uint16_t next = (uint16_t)(pad & link_mask);
            t->code_words[offset] =
                (uint16_t)((pad & ~field_mask) | (value & field_mask));
            if (link_mask == 0 || next == 0) break;
            offset = (uint32_t)(offset + next) % t->code_word_count;
            if (offset >= t->code_word_count) break;
        }
    }
}

// ---- simulator core (M2 dspp_simulator.c semantics) ---------------------

typedef struct {
    int32_t addr;  // -1 when the operand carries an immediate value
    int32_t value; // -1 when the operand must be read from addr
} dspp_operand;

#define DSPP_MAX_OPERANDS 8
#define DSPP_STACK_DEPTH 4
#define DSPP_MAX_MOVES 8

struct dspp_sim {
    uint32_t code_base;
    // Words this sim owns at code_base in its space's code map.
    uint32_t code_owned_words;
    // Data image the interpreter reads: the space's shared one or s->data.
    uint16_t *data_image;
    // Operand slots holding parsed values; the rest still read as empty.
    int ops_in_use;
    uint32_t dependency_count;
    uint32_t dependency_slots[DSPP_MAX_DYNAMIC_LINKS];
    const dspp_template *tpl;
    dspp_template *tpl_resources; // owned relocated copy
    uint16_t code[DSPP_MAX_CODE_WORDS];
    // Pre-relocation code image: patching consumes link fields, and M2
    // dsppRelocate walks the template image while fixing up the instrument
    // copy — connect/disconnect walk this pristine image for links while
    // patching code, so loader relocation can never consume the chain.
    uint16_t pristine[DSPP_MAX_CODE_WORDS];
    uint32_t pristine_count;
    dspp_sim_space *space; // NULL for the standalone private-data path
    uint16_t data[0x400];

    // FIFO/oscillator state per DMA channel.
    uint16_t fifo[DSPI_NUM_DMA_CHANNELS][8];
    uint32_t fifo_depth[DSPI_NUM_DMA_CHANNELS];
    uint32_t fifo_dma_ptr[DSPI_NUM_DMA_CHANNELS];
    uint32_t fifo_dspi_ptr[DSPI_NUM_DMA_CHANNELS];
    uint16_t fifo_prev[DSPI_NUM_DMA_CHANNELS];
    dspp_dma_channel dma[DSPI_NUM_DMA_CHANNELS];
    // Standalone-sim counterparts of the shared-space serviceable mask and
    // pending check.
    uint32_t dma_serviceable_mask;
    uint32_t dma_check_pending;
    uint32_t dma_shadow_current_address;
    uint32_t dma_shadow_current_count;
    uint32_t dma_shadow_next_address;
    uint32_t dma_shadow_next_count;
    uint32_t dma_main_interrupt_partial;
    uint32_t dma_main_interrupt_enable;
    uint32_t dma_next_interrupt;
    uint32_t dma_next_interrupt_enable;
    uint32_t dma_consumed_interrupt;
    uint32_t dma_consumed_interrupt_enable;
    uint32_t dma_underover_interrupt;
    uint32_t dma_underover_interrupt_enable;
    // Private completion-qualified event queue for standalone sims (no
    // space): same FIFO discipline as dspp_sim_space, addressed through the
    // helpers below so shared and standalone paths agree.
    uint8_t completion_queue_private[DSPI_NUM_DMA_CHANNELS];
    uint32_t completion_head_private;
    uint32_t completion_count_private;
    // Private DSPI register state for standalone sims. Production sims use
    // the one register bank in dspp_sim_space so all live instruments share
    // the hardware state.
    uint16_t dspi_frequency_private;
    uint16_t dspi_phase_private;
    uint16_t dspi_last_osc_count_private;

    uint16_t dspi_inputs_private[2];
    uint16_t dspi_outputs_private[8];
    uint16_t dspi_output_fifo_private[DSPI_OUTPUT_FIFO_SIZE];
    uint32_t dspi_output_write_ptr_private;
    uint32_t dspi_output_read_ptr_private;

    uint32_t pc;
    uint32_t clock;
    int32_t accum;
    int sleeping;
    int32_t rbase[4];
    int32_t stack[DSPP_STACK_DEPTH];
    int stack_ptr;
    int32_t write_back;
    dspp_operand ops[DSPP_MAX_OPERANDS];
    int op_index;
    int cc_carry, cc_zero, cc_neg, cc_over, cc_exact;
    int gwilling;
    uint16_t command;
    // dsppConnectWithMove records (dspp_instr.c): per-frame data[dst] =
    // data[src] copies for BIND-to-RBASE destinations. Addresses are masked
    // to the 10-bit MOVE direct decode (cmd & 0x3FF / DSPN_ADDR_MASK) at
    // record time. Standalone private sims read local data; sims attached to
    // a dspp_sim_space resolve the same address in the shared M2 image.
    uint16_t move_dst[DSPP_MAX_MOVES];
    uint16_t move_src[DSPP_MAX_MOVES];
    uint32_t move_count;
};

// M2 dsppExportInstrumentResources (dspp_resources.c): register every
// DRSC_F_EXPORT resource of this shared-space instrument under its template
// name. Names must be unique across the live list (AF_ERR_NAME_NOT_UNIQUE).
// Called from dspp_sim_create_common after s->tpl_resources is assigned.
static int dspp_space_register_exports(dspp_sim_space *space, dspp_sim *sim) {
    if (!space || !sim || !sim->tpl_resources) return -1;
    const dspp_template *res = sim->tpl_resources;
    if (!res) return -1;
    uint32_t count = 0;
    for (uint32_t i = 0; i < res->resource_count; ++i)
        if (res->resources[i].flags & DRSC_F_EXPORT) ++count;
    if (count == 0u) return 0;
    if (count > DSPP_MAX_EXPORTED_RESOURCES) return -1;
    dspp_export_owner *owner =
        (dspp_export_owner *)calloc(1, sizeof(dspp_export_owner));
    if (!owner) return -1;
    owner->sim = sim;
    uint32_t k = 0;
    for (uint32_t i = 0; i < res->resource_count; ++i) {
        const dspp_resource *r = &res->resources[i];
        if (!(r->flags & DRSC_F_EXPORT)) continue;
        uint32_t collision_probe = 0;
        if (dspp_space_import_resource(space, r->name, &collision_probe) == 0) {
            free(owner);
            return -1; // name already exported by a live instrument
        }
        snprintf(owner->resources[k].name, DSPP_MAX_NAME, "%s", r->name);
        owner->resources[k].resource_index = i;
        ++k;
    }
    owner->count = k;
    owner->next = space->export_owners;
    space->export_owners = owner;
    ++space->export_generation;
    return 0;
}

// M2 dsppUnexportInstrumentResources: drop this instrument's export
// records so a later importer of the same name fails again (or a rebuilt
// instrument re-registers). Called from dspp_sim_destroy for shared sims.
static void dspp_space_unregister_exports(dspp_sim_space *space,
                                          dspp_sim *sim) {
    if (!space || !sim) return;
    dspp_export_owner **link = &space->export_owners;
    while (*link) {
        if ((*link)->sim == sim) {
            dspp_export_owner *dead = *link;
            *link = dead->next;
            free(dead);
            ++space->export_generation;
            return;
        }
        link = &(*link)->next;
    }
}

// Real disc-FS access is bridged through the same weak m2_host_raw_*
// accessors recomp_runtime.c declares (strong defs in
// host/core/filesystem_shim.cpp). With no bridge (standalone loader tests
// without one) the nanokernel cannot load and imports keep failing with
// AF_ERR_EXTERNALREF — the pre-exporter behavior.
uint32_t m2_host_raw_open(const char *guest_path) __attribute__((weak));
uint32_t m2_host_raw_size(uint32_t host_token) __attribute__((weak));
int32_t m2_host_raw_read(uint32_t host_token, void *dst,
                         uint32_t n) __attribute__((weak));
void m2_host_raw_close(uint32_t host_token) __attribute__((weak));

// Read a System.m2/Audio/dsp template through the raw-file bridge. Returns
// 0 with a malloc'd buffer the caller frees, or -1 when no bridge/missing
// file/too large (mirrors gk_read_dsp_template_file in recomp_runtime.c).
static int dspp_read_disc_template(const char *name, uint8_t **data_out,
                                   uint32_t *size_out) {
    *data_out = NULL;
    *size_out = 0;
    if (!m2_host_raw_open || !m2_host_raw_size || !m2_host_raw_read ||
        !m2_host_raw_close)
        return -1;
    char path[128];
    int n = snprintf(path, sizeof(path), "System.m2/Audio/dsp/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    uint32_t token = m2_host_raw_open(path);
    if (token == 0u) return -1;
    uint32_t size = m2_host_raw_size(token);
    if (size == 0u || size > (64u << 10)) {
        m2_host_raw_close(token);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        m2_host_raw_close(token);
        return -1;
    }
    int32_t got = m2_host_raw_read(token, buf, size);
    m2_host_raw_close(token);
    if (got < 0 || (uint32_t)got != size) {
        free(buf);
        return -1;
    }
    *data_out = buf;
    *size_out = size;
    return 0;
}

// Write one instruction word into the instrument's code copy (M2
// PatchCode16 against dins code memory; template image stays pristine).
void dspp_sim_poke_code(dspp_sim *s, uint32_t offset, uint16_t value) {
    if (!s || offset >= DSPP_MAX_CODE_WORDS) return;
    s->code[offset] = value;
}

static dspp_sim *dspp_sim_create_common(const dspp_template *t,
                                         dspp_sim_space *space);

static int dspp_open_library(dspp_sim_space *space, const char *name) {
    if (!space) return -1;
    int slot = -1;
    for (unsigned i = 0; i < DSPP_MAX_DYNAMIC_LINKS; ++i) {
        if (strcmp(space->libraries[i].name, name) == 0) {
            if (space->libraries[i].loading) return -1; // cyclic DLNK
            ++space->libraries[i].references;
            return (int)i;
        }
        if (!space->libraries[i].name[0] && slot < 0) slot = (int)i;
    }
    if (slot < 0 || strchr(name, '/') || strchr(name, '\\')) return -1;
    snprintf(space->libraries[slot].name, DSPP_MAX_NAME, "%s", name);
    space->libraries[slot].loading = 1;
    uint8_t *data = NULL;
    uint32_t size = 0;
    dspp_template parsed;
    dspp_sim *library = NULL;
    if (dspp_read_disc_template(name, &data, &size) == 0 &&
        dspp_load_template(data, size, &parsed) == 0)
        library = dspp_sim_create_common(&parsed, space);
    free(data);
    if (!library) {
        memset(&space->libraries[slot], 0, sizeof(space->libraries[slot]));
        return -1;
    }
    space->libraries[slot].loading = 0;
    space->libraries[slot].sim = library;
    space->libraries[slot].references = 1;
    return slot;
}

static void dspp_close_library(dspp_sim_space *space, uint32_t slot) {
    if (!space || slot >= DSPP_MAX_DYNAMIC_LINKS ||
        !space->libraries[slot].references) return;
    if (--space->libraries[slot].references == 0) {
        dspp_sim *sim = space->libraries[slot].sim;
        memset(&space->libraries[slot], 0, sizeof(space->libraries[slot]));
        dspp_sim_destroy(sim);
    }
}

static uint16_t dspp_fetch_code(const dspp_sim *s, uint32_t pc) {
    if (s->space) {
        const uint32_t own_offset = (pc & 0x3ffu) - s->code_base;
        if (own_offset < s->code_owned_words) return s->code[own_offset];
        const dspp_sim *owner = s->space->code_owners[pc & 0x3ffu];
        return owner ? owner->code[(pc & 0x3ffu) - owner->code_base] : DSPN_OPCODE_SLEEP;
    }
    return pc < DSPP_MAX_CODE_WORDS ? s->code[pc] : DSPN_OPCODE_SLEEP;
}

static dspp_sim *dspp_sim_create_common(const dspp_template *t,
                                         dspp_sim_space *space) {
    if (!t || t->code_word_count == 0) return NULL;
    dspp_sim *s = (dspp_sim *)calloc(1, sizeof(dspp_sim));
    if (!s) return NULL;
    s->space = space;
    s->data_image = space ? space->data : s->data;
    s->ops_in_use = DSPP_MAX_OPERANDS;
    if (t->dynamic_link_count && !space) { free(s); return NULL; }
    for (uint32_t i = 0; i < t->dynamic_link_count; ++i) {
        const int slot = dspp_open_library(space, t->dynamic_links[i]);
        if (slot < 0) { dspp_sim_destroy(s); return NULL; }
        s->dependency_slots[s->dependency_count++] = (uint32_t)slot;
    }
    if (space) {
        uint32_t run = 0, end = 0;
        for (; end < 0x400u; ++end) {
            run = space->code_owners[end] ? 0 : run + 1;
            if (run == t->code_word_count) break;
        }
        if (run != t->code_word_count) { dspp_sim_destroy(s); return NULL; }
        s->code_base = end + 1 - run;
        for (uint32_t i = 0; i < run; ++i) space->code_owners[s->code_base + i] = s;
        s->code_owned_words = run;
    }
    dspp_template *copy = (dspp_template *)calloc(1, sizeof(dspp_template));
    if (!copy) {
        dspp_sim_destroy(s);
        return NULL;
    }
    *copy = *t;
    int alloc_rc = space ? dspp_alloc_resources_shared(copy, space, s->code_base)
                         : dspp_alloc_resources(copy);
    if (alloc_rc != 0) {
        free(copy);
        dspp_sim_destroy(s);
        return NULL;
    }
    // Snapshot the pre-relocation image: relocation patching consumes link
    // fields, so only a pristine copy can be re-relocated after a
    // disconnect drops connect patches.
    memcpy(s->pristine, t->code_words, t->code_word_count * 2);
    s->pristine_count = t->code_word_count;
    s->space = space;
    s->tpl_resources = copy;
    // dsppExportInstrumentResources runs after a successful open: the new
    // instrument's DRSC_F_EXPORT resources become importable by name. A
    // name collision with a live instrument fails the open (M2
    // BUILD_PARANOIA check, AF_ERR_NAME_NOT_UNIQUE). Imports were resolved
    // in dspp_alloc_resources_shared against the pre-existing table; this
    // registration must precede any later instrument's allocation.
    if (space && dspp_space_register_exports(space, s) != 0) {
        dspp_space_release_resources(space, copy);
        s->tpl_resources = NULL;
        free(copy);
        dspp_sim_destroy(s);
        return NULL;
    }
    dspp_apply_relocations(copy);
    memcpy(s->code, copy->code_words, copy->code_word_count * 2);
    s->rbase[0] = 0;
    s->rbase[1] = 4;
    s->rbase[2] = 8;
    s->rbase[3] = 12;
    s->gwilling = 1;
    return s;
}

dspp_sim *dspp_sim_create(const dspp_template *t) {
    return dspp_sim_create_common(t, NULL);
}

// Ensure the shared space carries the on-disc nanokernel instrument: load
// System.m2/Audio/dsp/nanokernel.dsp through the raw-file bridge, open it
// in this space (its DRSC_F_EXPORT resources join the export table), and
// retarget the head instrument's enabling patch. The M2 audio folio
// loads the head before any client instrument exists, so its allocations
// sit at the front of the shared image and every later import resolves by
// name exactly as on hardware. The SLEEP-patch word survives in the sim's
// code copy only; the template stays pristine for re-opens.
int dspp_sim_space_ensure_kernel(dspp_sim_space *space) {
    if (!space) return -1;
    if (space->kernel_sim) return 0; // M2: one head instrument
    if (space->kernel_open_failed) return -1;
    uint8_t *data = NULL;
    uint32_t size = 0;
    if (dspp_read_disc_template("nanokernel.dsp", &data, &size) != 0) {
        space->kernel_open_failed = 1;
        return -1;
    }
    dspp_template parsed;
    int rc = dspp_load_template(data, size, &parsed);
    free(data);
    if (rc != 0) {
        space->kernel_open_failed = 1;
        return -1;
    }
    dspp_sim *sim = dspp_sim_create_in_space(&parsed, space);
    if (!sim) {
        space->kernel_open_failed = 1;
        return -1;
    }
    // CR4215 (dspp_instr.c 950126): the head ships disabled with a _NOP at
    // its entry; the folio overwrites that word with _SLEEP so the cyclic
    // program only ever runs from its loop entry. Patch the instrument's
    // code copy only. The sim is not registered as a started instrument:
    // it only provides allocations (gMixer words, FrameCountLow/High,
    // Amplitude, DSPI hardware relocations) and export names. The folio
    // drives the DAC staging through the same DSPI model, mirrored in the
    // audio platform layer.
    dspp_sim_poke_code(sim, 0, DSPN_OPCODE_SLEEP);
    space->kernel_sim = sim;
    return 0;
}

dspp_sim_space *dspp_sim_space_create(void) {
    dspp_sim_space *space = (dspp_sim_space *)calloc(1, sizeof(dspp_sim_space));
    if (space) space->export_generation = 1u;
    return space;
}

void dspp_sim_space_destroy(dspp_sim_space *space) {
    if (!space) return;
    dspp_export_owner *owner = space->export_owners;
    while (owner) {
        dspp_export_owner *next = owner->next;
        free(owner);
        owner = next;
    }
    free(space);
}

dspp_sim *dspp_sim_create_in_space(const dspp_template *t,
                                   dspp_sim_space *space) {
    return dspp_sim_create_common(t, space);
}

// Reserve n words straight from the shared data allocator for a host-side
// mixer-output carrier (see recomp_runtime.c gk_mixnode_reserve_words): a
// generated-mixer node's formula runs in the mix-node layer, but its
// destination connections patch sim relocation sites with a source address,
// so the outputs need real allocated words in the one M2 data image — the
// same allocator a M2-generated mixer image would draw from. Words are
// zeroed on reservation and on release (allocation reset, not signal data).
int dspp_sim_space_reserve_words(dspp_sim_space *space, uint32_t count,
                                 uint32_t *out_addr) {
    if (!space || !out_addr || count == 0) return -1;
    uint32_t addr = 0;
    if (dspp_space_alloc_words(space, count, 1, &addr) != 0) return -1;
    for (uint32_t i = 0; i < count && addr + i < DSPI_DATA_MEMORY_SIZE; ++i)
        space->data[addr + i] = 0;
    *out_addr = addr;
    return 0;
}

// Release a prior reservation back to the shared allocator.
void dspp_sim_space_release_words(dspp_sim_space *space, uint32_t addr,
                                  uint32_t count) {
    if (!space || addr >= DSPI_DATA_MEMORY_SIZE) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t a = addr + i;
        if (a >= DSPI_DATA_MEMORY_SIZE || !space->used[a]) break;
        space->used[a] = 0;
        space->data[a] = 0;
    }
}

// Resolve an exported resource name to its allocated data address in this
// space (the import half of dsppImportResource, exposed for the platform
// layer's DAC staging and for focused contract tests). -1 when absent.
int dspp_sim_space_export_address(dspp_sim_space *space, const char *name,
                                  uint32_t *out_addr) {
    return dspp_space_import_resource(space, name, out_addr);
}

// Revision of the space export table; see export_generation.
uint32_t dspp_sim_space_export_generation(const dspp_sim_space *space) {
    return space ? space->export_generation : 0u;
}

// Run the head instrument's cyclic program for n frames (M2 nanokernel
// execution: FrameCountLow/High advance, the half/eighth-rate staging runs,
// DSPI output-commit state advances). No-op without a resident head.
void dspp_sim_space_tick_kernel(dspp_sim_space *space, uint32_t frames) {
    if (!space || !space->kernel_sim) return;
    dspp_sim_tick_n(space->kernel_sim, frames);
}



static dspp_dma_channel *dspp_dma_state_ptr(dspp_sim *s,
                                            uint32_t channel);

void dspp_sim_destroy(dspp_sim *s) {
    if (!s) return;
    // A delay-line attachment belongs to the instrument that started its
    // physical DMA channel.  Detach only that owner; shared-space channels
    // used by sibling instruments must remain live.
    for (uint32_t channel = 0; channel < DSPI_NUM_DMA_CHANNELS; ++channel) {
        dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
        if (dma && dma->delay_line && dma->delay_owner == s)
            dspp_sim_dma_detach_delay_line(s, channel);
    }
    if (s->space) {
        dspp_space_unregister_exports(s->space, s);
        if (s->tpl_resources) dspp_space_release_resources(s->space, s->tpl_resources);
        for (uint32_t i = 0; i < 0x400u; ++i)
            if (s->space->code_owners[i] == s) s->space->code_owners[i] = NULL;
        for (uint32_t i = 0; i < s->dependency_count; ++i)
            dspp_close_library(s->space, s->dependency_slots[i]);
    }
    free(s->tpl_resources);
    free(s);
}

static uint16_t *dspp_data_mem(dspp_sim *s) {
    return s ? s->data_image : NULL;
}

static const uint16_t *dspp_data_mem_const(const dspp_sim *s) {
    return s && s->space ? s->space->data : (s ? s->data : NULL);
}

// Read/write one data word through the kernel (head) instrument's backing
// image: shared spaces see the one M2 data memory, standalone sims their
// private copy. Bounded like dspp_sim_peek/poke_data.
uint16_t dspp_sim_space_peek_kernel_data(dspp_sim_space *space,
                                         uint32_t addr) {
    if (!space || !space->kernel_sim || addr >= DSPI_DATA_MEMORY_SIZE)
        return 0;
    return dspp_data_mem_const(space->kernel_sim)[addr];
}

void dspp_sim_space_poke_kernel_data(dspp_sim_space *space, uint32_t addr,
                                     uint16_t value) {
    if (!space || !space->kernel_sim || addr >= DSPI_DATA_MEMORY_SIZE) return;
    dspp_data_mem(space->kernel_sim)[addr] = value;
}

static uint16_t *dspp_dspi_frequency_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dspi_frequency
                         : (s ? &s->dspi_frequency_private : NULL);
}

static uint16_t *dspp_dspi_phase_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dspi_phase
                         : (s ? &s->dspi_phase_private : NULL);
}

static uint16_t *dspp_dspi_last_osc_count_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dspi_last_osc_count
                         : (s ? &s->dspi_last_osc_count_private : NULL);
}

static uint16_t *dspp_dspi_inputs_ptr(dspp_sim *s) {
    return s && s->space ? s->space->dspi_inputs
                         : (s ? s->dspi_inputs_private : NULL);
}

static uint16_t *dspp_dspi_outputs_ptr(dspp_sim *s) {
    return s && s->space ? s->space->dspi_outputs
                         : (s ? s->dspi_outputs_private : NULL);
}

static uint16_t *dspp_dspi_output_fifo_ptr(dspp_sim *s) {
    return s && s->space ? s->space->dspi_output_fifo
                         : (s ? s->dspi_output_fifo_private : NULL);
}

static uint32_t *dspp_dspi_output_write_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dspi_output_write_ptr
                         : (s ? &s->dspi_output_write_ptr_private : NULL);
}

static uint32_t *dspp_dspi_output_read_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dspi_output_read_ptr
                         : (s ? &s->dspi_output_read_ptr_private : NULL);
}

static uint16_t *dspp_fifo_words_ptr(dspp_sim *s, uint32_t channel) {
    return s && s->space ? s->space->fifo[channel]
                         : (s ? s->fifo[channel] : NULL);
}

static uint32_t *dspp_fifo_depth_ptr(dspp_sim *s, uint32_t channel) {
    return s && s->space ? &s->space->fifo_depth[channel]
                         : (s ? &s->fifo_depth[channel] : NULL);
}

static const uint32_t *dspp_fifo_depth_const_ptr(const dspp_sim *s,
                                                 uint32_t channel) {
    return s && s->space ? &s->space->fifo_depth[channel]
                         : (s ? &s->fifo_depth[channel] : NULL);
}

static uint32_t *dspp_fifo_dma_ptr(dspp_sim *s, uint32_t channel) {
    return s && s->space ? &s->space->fifo_dma_ptr[channel]
                         : (s ? &s->fifo_dma_ptr[channel] : NULL);
}

static uint32_t *dspp_fifo_dspi_ptr(dspp_sim *s, uint32_t channel) {
    return s && s->space ? &s->space->fifo_dspi_ptr[channel]
                         : (s ? &s->fifo_dspi_ptr[channel] : NULL);
}

static uint16_t *dspp_fifo_prev_ptr(dspp_sim *s, uint32_t channel) {
    return s && s->space ? &s->space->fifo_prev[channel]
                         : (s ? &s->fifo_prev[channel] : NULL);
}

static dspp_dma_channel *dspp_dma_state_ptr(dspp_sim *s,
                                            uint32_t channel) {
    return s && s->space ? &s->space->dma[channel]
                         : (s ? &s->dma[channel] : NULL);
}

static const dspp_dma_channel *dspp_dma_state_const_ptr(
    const dspp_sim *s, uint32_t channel) {
    return s && s->space ? &s->space->dma[channel]
                         : (s ? &s->dma[channel] : NULL);
}

// The FDMA check runs before every DSP instruction. Keep a mask of channels
// that can actually be serviced (enabled and not complete) so the common case
// does not probe all 32 hardware channels. Shared-space sims use the one
// hardware-shaped state block; standalone sims keep a private copy.
static uint32_t *dspp_dma_serviceable_mask_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_serviceable_mask
                         : (s ? &s->dma_serviceable_mask : NULL);
}

static uint32_t *dspp_dma_check_pending_ptr(dspp_sim *s) {
    return s->space ? &s->space->dma_check_pending : &s->dma_check_pending;
}

static void dspp_dma_refresh_serviceable(dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    *dspp_dma_check_pending_ptr(s) = 1u;
    uint32_t *mask = dspp_dma_serviceable_mask_ptr(s);
    if (!mask) return;
    const dspp_dma_channel *dma = dspp_dma_state_const_ptr(s, channel);
    const uint32_t bit = 1u << channel;
    if (dma->enabled && !dma->complete)
        *mask |= bit;
    else
        *mask &= ~bit;
}

// Highest set channel, preserving the hardware priority order (31 down to 0).
static int dspp_dma_highest_channel(uint32_t mask) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    return _BitScanReverse(&index, mask) ? (int)index : -1;
#else
    return mask != 0u ? 31 - __builtin_clz(mask) : -1;
#endif
}

static uint32_t *dspp_dma_next_interrupt_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_next_interrupt
                         : (s ? &s->dma_next_interrupt : NULL);
}

static const uint32_t *dspp_dma_next_interrupt_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_next_interrupt
                         : (s ? &s->dma_next_interrupt : NULL);
}

static uint32_t *dspp_dma_consumed_interrupt_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_consumed_interrupt
                         : (s ? &s->dma_consumed_interrupt : NULL);
}

static const uint32_t *dspp_dma_consumed_interrupt_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_consumed_interrupt
                         : (s ? &s->dma_consumed_interrupt : NULL);
}

static uint32_t *dspp_dma_consumed_interrupt_enable_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_consumed_interrupt_enable
                         : (s ? &s->dma_consumed_interrupt_enable : NULL);
}

static const uint32_t *dspp_dma_consumed_interrupt_enable_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_consumed_interrupt_enable
                         : (s ? &s->dma_consumed_interrupt_enable : NULL);
}

static uint32_t *dspp_dma_underover_interrupt_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_underover_interrupt
                         : (s ? &s->dma_underover_interrupt : NULL);
}

static const uint32_t *dspp_dma_underover_interrupt_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_underover_interrupt
                         : (s ? &s->dma_underover_interrupt : NULL);
}

static uint32_t *dspp_dma_underover_interrupt_enable_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_underover_interrupt_enable
                         : (s ? &s->dma_underover_interrupt_enable : NULL);
}

static const uint32_t *dspp_dma_underover_interrupt_enable_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_underover_interrupt_enable
                         : (s ? &s->dma_underover_interrupt_enable : NULL);
}

static uint32_t *dspp_dma_shadow_current_address_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_shadow_current_address
                         : (s ? &s->dma_shadow_current_address : NULL);
}

static uint32_t *dspp_dma_shadow_current_count_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_shadow_current_count
                         : (s ? &s->dma_shadow_current_count : NULL);
}

static uint32_t *dspp_dma_shadow_next_address_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_shadow_next_address
                         : (s ? &s->dma_shadow_next_address : NULL);
}

static uint32_t *dspp_dma_shadow_next_count_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_shadow_next_count
                         : (s ? &s->dma_shadow_next_count : NULL);
}

static uint32_t *dspp_dma_next_interrupt_enable_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_next_interrupt_enable
                         : (s ? &s->dma_next_interrupt_enable : NULL);
}

static const uint32_t *dspp_dma_next_interrupt_enable_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_next_interrupt_enable
                         : (s ? &s->dma_next_interrupt_enable : NULL);
}

static uint32_t *dspp_dma_main_interrupt_partial_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_main_interrupt_partial
                         : (s ? &s->dma_main_interrupt_partial : NULL);
}

static uint32_t *dspp_dma_main_interrupt_enable_ptr(dspp_sim *s) {
    return s && s->space ? &s->space->dma_main_interrupt_enable
                         : (s ? &s->dma_main_interrupt_enable : NULL);
}

static const uint32_t *dspp_dma_main_interrupt_partial_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_main_interrupt_partial
                         : (s ? &s->dma_main_interrupt_partial : NULL);
}

static const uint32_t *dspp_dma_main_interrupt_enable_const_ptr(
    const dspp_sim *s) {
    return s && s->space ? &s->space->dma_main_interrupt_enable
                         : (s ? &s->dma_main_interrupt_enable : NULL);
}

// M2 dspsGenMainInt (dspp_simulator.c): the main-interrupt word is the
// stored partial bank OR the second-level pending bits gated by their enable
// banks. UnderOver is deliberately not folded (M2 parity).
uint32_t dspp_sim_dma_main_interrupt(const dspp_sim *s) {
    if (!s) return 0;
    uint32_t value = *dspp_dma_main_interrupt_partial_const_ptr(s);
    if ((*dspp_dma_next_interrupt_const_ptr(s) &
         *dspp_dma_next_interrupt_enable_const_ptr(s)) != 0u)
        value |= DSPX_F_INT_DMANEXT;
    if ((*dspp_dma_consumed_interrupt_const_ptr(s) &
         *dspp_dma_consumed_interrupt_enable_const_ptr(s)) != 0u)
        value |= DSPX_F_INT_CONSUMED;
    return value;
}

uint32_t dspp_sim_dma_main_interrupt_enable(const dspp_sim *s) {
    const uint32_t *enable = dspp_dma_main_interrupt_enable_const_ptr(s);
    return enable ? *enable : 0u;
}

int dspp_sim_dma_raise_timer_interrupt(dspp_sim *s) {
    if (!s) return 0;
    uint32_t *partial = dspp_dma_main_interrupt_partial_ptr(s);
    if (!partial || (*partial & DSPX_F_INT_TIMER)) return 0;
    *partial |= DSPX_F_INT_TIMER;
    return 1;
}

void dspp_sim_dma_set_channel_interrupt(dspp_sim *s, uint32_t channel,
                                        int enabled) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    // M2 dsphEnable/DisableChannelInterrupt writes the DMA-control shadow
    // word DSPX_F_SHADOW_SET_DMANEXT, adding DSPX_F_INT_DMANEXT_EN to enable.
    // The raw register path applies the same commit, so the helper and the
    // register boundary always agree.
    uint32_t value = DSPX_F_SHADOW_SET_DMANEXT;
    if (enabled) value |= DSPX_F_INT_DMANEXT_EN;
    dspp_sim_dma_register_write(s, DSPX_DMA_STACK_CONTROL +
                                   channel * DSPX_DMA_STACK_CONTROL_SIZE,
                                value);
}

int dspp_sim_dma_channel_interrupt_enabled(const dspp_sim *s,
                                           uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    const uint32_t *enable = dspp_dma_next_interrupt_enable_const_ptr(s);
    return enable && (*enable & (1u << channel)) ? 1 : 0;
}

const dspp_template *dspp_sim_resources(const dspp_sim *s) {
    return s ? s->tpl_resources : NULL;
}

void dspp_sim_set_gwilling(dspp_sim *s, int willing) {
    if (s) s->gwilling = willing ? 1 : 0;
}

void dspp_sim_set_input(dspp_sim *s, uint32_t index, int16_t value) {
    uint16_t *inputs = dspp_dspi_inputs_ptr(s);
    if (inputs && index < 2u) inputs[index] = (uint16_t)value;
}

int16_t dspp_sim_output(const dspp_sim *s, uint32_t index) {
    const uint16_t *outputs =
        s && s->space ? s->space->dspi_outputs :
        (s ? s->dspi_outputs_private : NULL);
    return (outputs && index < 8u) ? (int16_t)outputs[index] : 0;
}

// Host-side FIFO feed (DMA direction into the DSPP).
void dspp_sim_fifo_push(dspp_sim *s, uint32_t channel, int16_t value) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    uint16_t *fifo = dspp_fifo_words_ptr(s, channel);
    uint32_t *depth = dspp_fifo_depth_ptr(s, channel);
    uint32_t *dma_ptr = dspp_fifo_dma_ptr(s, channel);
    if (*depth >= 8u) return;
    fifo[*dma_ptr] = (uint16_t)value;
    *dma_ptr = (*dma_ptr + 1u) & 7u;
    ++*depth;
    *dspp_dma_check_pending_ptr(s) = 1u;
}

dspp_delay_line *dspp_delay_line_create(uint32_t num_bytes,
                                        uint32_t num_channels, int if_loop) {
    if (num_bytes < 2u || num_channels == 0u ||
        num_channels > UINT32_MAX / 2u ||
        num_bytes % (num_channels * 2u) != 0u)
        return NULL;
    const uint32_t word_count = num_bytes / 2u;
    const uint32_t frame_count = word_count / num_channels;
    if (frame_count == 0u) return NULL;
    dspp_delay_line *line =
        (dspp_delay_line *)calloc(1, sizeof(dspp_delay_line));
    if (!line) return NULL;
    line->words = (uint16_t *)calloc(word_count, sizeof(uint16_t));
    if (!line->words) {
        free(line);
        return NULL;
    }
    line->num_bytes = num_bytes;
    line->num_channels = num_channels;
    line->frame_count = frame_count;
    line->word_count = word_count;
    line->if_loop = if_loop ? 1u : 0u;
    return line;
}

static int dspp_delay_range_valid(const dspp_delay_line *line,
                                  int32_t begin, int32_t end) {
    if (!line) return 0;
    if (begin == -1 && end == -1) return 1;
    if (begin < 0 || end < 0 || end <= begin) return 0;
    return (uint32_t)end <= line->frame_count;
}

int dspp_delay_line_set_sample_loops(dspp_delay_line *line,
                                     int32_t sustain_begin,
                                     int32_t sustain_end,
                                     int32_t release_begin,
                                     int32_t release_end) {
    if (!dspp_delay_range_valid(line, sustain_begin, sustain_end) ||
        !dspp_delay_range_valid(line, release_begin, release_end))
        return -1;
    line->authored_ranges = 1u;
    line->has_sustain_loop = sustain_begin >= 0 ? 1u : 0u;
    line->has_release_loop = release_begin >= 0 ? 1u : 0u;
    line->sustain_begin = sustain_begin;
    line->sustain_end = sustain_end;
    line->release_begin = release_begin;
    line->release_end = release_end;
    // A loop marker changes the initial descriptor to an unbounded schedule;
    // the phase helpers below decide which authored range is active. When
    // called with four absent markers, this explicitly selects the finite
    // sample schedule, matching the create-only API.
    line->if_loop = (line->has_sustain_loop || line->has_release_loop) ? 1u
                                                                        : 0u;
    return 0;
}

void dspp_delay_line_clear(dspp_delay_line *line) {
    if (!line || !line->words) return;
    memset(line->words, 0, line->word_count * sizeof(uint16_t));
}

void dspp_delay_line_set_clear_on_stop(dspp_delay_line *line, int enabled) {
    if (line) line->clear_on_stop = enabled ? 1u : 0u;
}

void dspp_delay_line_destroy(dspp_delay_line *line) {
    if (!line || line->bindings != 0u) return;
    free(line->words);
    free(line);
}

static void dspp_delay_line_reset_fifo(dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    *dspp_fifo_depth_ptr(s, channel) = 0u;
    *dspp_dma_check_pending_ptr(s) = 1u;
    *dspp_fifo_dma_ptr(s, channel) = 0u;
    *dspp_fifo_dspi_ptr(s, channel) = 0u;
    *dspp_fifo_prev_ptr(s, channel) = 0u;
}

// Defined below the delay service so the physical attachment can discard a
// queued completion when it is explicitly detached before the folio pump
// observes it.
static void dspp_completion_remove_locked(dspp_sim *s, uint32_t channel);
static void dspp_completion_note_delay(dspp_sim *s, uint32_t channel);

static uint32_t dspp_delay_frame_word(const dspp_delay_line *line,
                                      int32_t frame) {
    return (uint32_t)frame * line->num_channels;
}

// Advance an authored sample descriptor when its current segment reaches its
// end. The M2 DSPP keeps this state on the DMA attachment, not on the
// AudioSample Item, so each tap of a shared delay line gets an independent
// phase/cursor here as well.
static void dspp_delay_prepare_cursor(dspp_dma_channel *dma);

static void dspp_delay_enter_release(dspp_dma_channel *dma) {
    dspp_delay_line *line = dma ? dma->delay_line : NULL;
    if (!line || !line->has_sustain_loop) return;

    dma->delay_release_pending = 0u;
    dma->delay_released = 1u;
    const uint32_t sustain_end =
        dspp_delay_frame_word(line, line->sustain_end);
    if (line->has_release_loop) {
        const uint32_t release_begin =
            dspp_delay_frame_word(line, line->release_begin);
        const uint32_t release_end =
            dspp_delay_frame_word(line, line->release_end);
        // DSPPReleaseSampleAttachment plays the authored gap when the sustain
        // and release endpoints differ, then installs the release loop.
        // Treating an equal/earlier endpoint as a direct loop avoids making a
        // host-side underflow stand in for a guest DMA descriptor.
        if (release_end > sustain_end) {
            dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_GAP;
            dma->delay_cursor = sustain_end;
            dma->delay_segment_end = release_end;
        } else {
            dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_LOOP;
            dma->delay_cursor = release_begin;
            dma->delay_segment_end = release_end;
        }
        dma->delay_loop_begin = release_begin;
        dma->delay_loop_end = release_end;
        dma->go_forever = 1u;
        return;
    }

    // No release loop: the current release phase is the finite tail after
    // sustain_end. The M2 may append a silence attachment after a short
    // tail; this platform binding has no guest successor, so its natural end
    // is represented by the same finite completion seam.
    dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_TAIL;
    dma->delay_cursor = sustain_end;
    dma->delay_segment_end = line->word_count;
    dma->delay_loop_begin = 0u;
    dma->delay_loop_end = 0u;
    dma->go_forever = 0u;
}

static void dspp_delay_prepare_cursor(dspp_dma_channel *dma) {
    if (!dma || !dma->delay_line || dma->delay_finished) return;
    dspp_delay_line *line = dma->delay_line;
    // A segment transition can immediately install another segment (for
    // example sustain -> release loop), so keep this bounded local state
    // machine rather than relying on a later host tick to repair the cursor.
    for (uint32_t transitions = 0; transitions < 8u; ++transitions) {
        if (dma->delay_phase == DSPP_DELAY_PHASE_WHOLE) {
            if (dma->delay_cursor < line->word_count) return;
            if (line->if_loop) {
                dma->delay_cursor = 0u;
                return;
            }
            dma->delay_cursor = line->word_count;
            dma->delay_finished = 1u;
            return;
        }
        if (dma->delay_cursor < dma->delay_segment_end) return;

        switch (dma->delay_phase) {
        case DSPP_DELAY_PHASE_SUSTAIN_INITIAL:
            dma->delay_phase = DSPP_DELAY_PHASE_SUSTAIN_LOOP;
            dma->delay_cursor =
                dspp_delay_frame_word(line, line->sustain_begin);
            dma->delay_loop_begin = dma->delay_cursor;
            dma->delay_loop_end =
                dspp_delay_frame_word(line, line->sustain_end);
            dma->delay_segment_end = dma->delay_loop_end;
            continue;
        case DSPP_DELAY_PHASE_SUSTAIN_LOOP:
            if (dma->delay_release_pending) {
                dspp_delay_enter_release(dma);
                continue;
            }
            dma->delay_cursor = dma->delay_loop_begin;
            return;
        case DSPP_DELAY_PHASE_RELEASE_INITIAL:
            dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_LOOP;
            dma->delay_cursor = dma->delay_loop_begin;
            dma->delay_segment_end = dma->delay_loop_end;
            dma->go_forever = 1u;
            return;
        case DSPP_DELAY_PHASE_RELEASE_GAP:
            dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_LOOP;
            dma->delay_cursor = dma->delay_loop_begin;
            dma->delay_segment_end = dma->delay_loop_end;
            dma->go_forever = 1u;
            return;
        case DSPP_DELAY_PHASE_RELEASE_LOOP:
            dma->delay_cursor = dma->delay_loop_begin;
            return;
        case DSPP_DELAY_PHASE_FINITE:
        case DSPP_DELAY_PHASE_RELEASE_TAIL:
        default:
            dma->delay_cursor = line->word_count;
            dma->delay_finished = 1u;
            return;
        }
    }
    // Valid authored ranges cannot require this fallback. Keep a malformed
    // or corrupted runtime state finite rather than spinning in the FDMA
    // service loop.
    dma->delay_finished = 1u;
}

static void dspp_delay_schedule_initial(dspp_dma_channel *dma,
                                        dspp_delay_line *line,
                                        uint32_t start_frame) {
    dma->delay_cursor = start_frame * line->num_channels;
    dma->delay_segment_end = line->word_count;
    dma->delay_loop_begin = 0u;
    dma->delay_loop_end = 0u;
    dma->delay_phase = DSPP_DELAY_PHASE_WHOLE;
    dma->delay_released = 0u;
    dma->delay_release_pending = 0u;
    dma->delay_finished = 0u;
    dma->go_forever = line->if_loop;

    if (!line->authored_ranges) {
        dspp_delay_prepare_cursor(dma);
        return;
    }
    if (line->has_sustain_loop) {
        dma->delay_phase = DSPP_DELAY_PHASE_SUSTAIN_INITIAL;
        dma->delay_segment_end =
            dspp_delay_frame_word(line, line->sustain_end);
    } else if (line->has_release_loop) {
        dma->delay_phase = DSPP_DELAY_PHASE_RELEASE_INITIAL;
        dma->delay_loop_begin =
            dspp_delay_frame_word(line, line->release_begin);
        dma->delay_loop_end =
            dspp_delay_frame_word(line, line->release_end);
        dma->delay_segment_end = dma->delay_loop_end;
    } else {
        dma->delay_phase = DSPP_DELAY_PHASE_FINITE;
    }
    dspp_delay_prepare_cursor(dma);
}

static void dspp_delay_line_maybe_clear(dspp_delay_line *line) {
    if (!line || !line->clear_on_stop) return;
    if (line->bindings == 0u || line->stopped_bindings >= line->bindings)
        dspp_delay_line_clear(line);
}

static void dspp_delay_line_mark_stopped(dspp_sim *s, uint32_t channel,
                                         int completed) {
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line) return;
    dma->enabled = 0u;
    dma->complete = 1u;
    dspp_dma_refresh_serviceable(s, channel);
    if (!dma->delay_stopped) {
        dma->delay_stopped = 1u;
        ++dma->delay_line->stopped_bindings;
        dspp_delay_line_maybe_clear(dma->delay_line);
    }
    if (completed && !dma->delay_completion_done &&
        !dma->delay_completion_pending) {
        dspp_completion_note_delay(s, channel);
        // A live Audio Folio attachment arms the ordinary DMANext channel
        // interrupt when it has a Cue, a successor, or FATLADYSINGS. The
        // finite delay-line adapter has no guest address for the descriptor,
        // but its natural end still reaches the same DSPX event vector. Keep
        // the poll record above for the standalone completion API; the
        // production folio consumes this bank through dsphFIRQHandler and
        // HandleDMASignal, just like ProcessAttachmentDMA.
        const uint32_t mask = 1u << channel;
        const uint32_t *enable =
            dspp_dma_next_interrupt_enable_const_ptr(s);
        if (enable && (*enable & mask))
            *dspp_dma_next_interrupt_ptr(s) |= mask;
    }
}

int dspp_sim_dma_attach_delay_line(dspp_sim *s, uint32_t channel,
                                   dspp_delay_line *line, int output,
                                   uint32_t start_frame) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS || !line ||
        (output != 0 && output != 1))
        return -1;
    if (line->word_count == 0u ||
        (!output && start_frame >= line->frame_count) ||
        (output && start_frame != 0u))
        return -1;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || dma->delay_line || dma->numeric_attached || dma->enabled ||
        dma->current_count != 0u || dma->next_valid)
        return -1;

    dspp_delay_line_reset_fifo(s, channel);
    dma->current_buffer = NULL;
    dma->current_count = UINT32_MAX;
    dma->current_address = 0u;
    dma->next_buffer = NULL;
    dma->next_count = 0u;
    dma->next_address = 0u;
    dma->next_valid = 0u;
    dma->enabled = 1u;
    dma->direction = output ? 1u : 0u;
    dma->width8 = 0u;
    dma->sqxd = 0u;
    dma->complete = 0u;
    dspp_dma_refresh_serviceable(s, channel);
    dma->delay_line = line;
    dma->delay_owner = s;
    dspp_delay_schedule_initial(dma, line, output ? 0u : start_frame);
    dma->delay_stopped = 0u;
    dma->delay_completion_done = 0u;
    dma->delay_completion_pending = 0u;
    dspp_completion_remove_locked(s, channel);
    // DSPPStartSampleAttachment's full-start path resets the FIFO and clears
    // stale DMA interrupt banks before publishing the new descriptor. A
    // physical delay binding has no guest address/count descriptor, but it
    // still owns the same channel registers.
    dspp_sim_dma_clear_interrupts(s, DSPP_DMA_BANK_ALL, 1u << channel);
    dspp_sim_dma_set_channel_interrupt(s, channel, 0);
    ++line->bindings;
    return 0;
}

int dspp_sim_dma_restart_delay_line(dspp_sim *s, uint32_t channel,
                                    dspp_delay_line *line, int output,
                                    uint32_t start_frame) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS || !line ||
        (output != 0 && output != 1))
        return -1;
    if (line->word_count == 0u ||
        (!output && start_frame >= line->frame_count) ||
        (output && start_frame != 0u))
        return -1;

    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || dma->delay_line != line || dma->delay_owner != s ||
        dma->direction != (uint8_t)(output != 0))
        return -1;

    // DSPPStopSampleAttachment precedes every full restart when the old
    // attachment is active. It disables the channel and applies
    // AF_SAMPF_CLEARONSTOP before the fresh FIFO start below.
    if (!dma->delay_stopped)
        dspp_delay_line_mark_stopped(s, channel, 0);
    if (line->stopped_bindings != 0u)
        --line->stopped_bindings;
    dspp_delay_line_maybe_clear(line);

    dspp_completion_remove_locked(s, channel);
    dspp_sim_dma_clear_interrupts(s, DSPP_DMA_BANK_ALL, 1u << channel);
    dspp_sim_dma_set_channel_interrupt(s, channel, 0);
    dspp_delay_line_reset_fifo(s, channel);
    dma->current_buffer = NULL;
    dma->current_count = UINT32_MAX;
    dma->current_address = 0u;
    dma->next_buffer = NULL;
    dma->next_count = 0u;
    dma->next_address = 0u;
    dma->next_valid = 0u;
    dma->enabled = 1u;
    dma->width8 = 0u;
    dma->sqxd = 0u;
    dma->complete = 0u;
    dspp_dma_refresh_serviceable(s, channel);
    dspp_delay_schedule_initial(dma, line, output ? 0u : start_frame);
    dma->delay_stopped = 0u;
    dma->delay_completion_done = 0u;
    dma->delay_completion_pending = 0u;
    return 0;
}

int dspp_sim_dma_release_delay_line(dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return -1;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line || dma->delay_owner != s)
        return -1;
    dspp_delay_line *line = dma->delay_line;
    if (!line->authored_ranges || dma->delay_stopped ||
        dma->delay_finished)
        return 0;

    dspp_delay_prepare_cursor(dma);
    if (dma->delay_finished) return 0;
    if (!line->has_sustain_loop) {
        // A release loop without a sustain loop is installed at start, and a
        // finite sample without either loop is already on its terminal path.
        // DSPPReleaseSampleAttachment has no active phase to change here.
        dma->delay_released = 1u;
        return 0;
    }
    if (dma->delay_phase == DSPP_DELAY_PHASE_SUSTAIN_INITIAL ||
        dma->delay_phase == DSPP_DELAY_PHASE_SUSTAIN_LOOP) {
        // Do not jump the cursor at the call site: the original DMA keeps the
        // current segment ordered, then changes the next descriptor at its
        // boundary. This is the observable part of ReleaseInstrument.
        dma->delay_release_pending = 1u;
    }
    return 0;
}

int dspp_sim_dma_stop_delay_line(dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return -1;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line ||
        (dma->delay_owner && dma->delay_owner != s))
        return -1;
    dspp_delay_line_mark_stopped(s, channel, 0);
    // DisableAttachmentSignal clears the DMANext channel interrupt even when
    // a caller stops a still-running attachment before its natural end.
    dspp_sim_dma_set_channel_interrupt(s, channel, 0);
    return 0;
}

void dspp_sim_dma_detach_delay_line(dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line ||
        (dma->delay_owner && dma->delay_owner != s))
        return;
    dspp_delay_line *line = dma->delay_line;
    if (dma->delay_completion_pending) {
        dma->delay_completion_pending = 0u;
        dspp_completion_remove_locked(s, channel);
    }
    if (line->bindings != 0u) --line->bindings;
    if (dma->delay_stopped && line->stopped_bindings != 0u)
        --line->stopped_bindings;
    dspp_delay_line_maybe_clear(line);
    dspp_sim_dma_clear_interrupts(s, DSPP_DMA_BANK_ALL, 1u << channel);
    dspp_sim_dma_set_channel_interrupt(s, channel, 0);
    dma->delay_line = NULL;
    dma->delay_owner = NULL;
    dma->delay_cursor = 0u;
    dma->delay_segment_end = 0u;
    dma->delay_loop_begin = 0u;
    dma->delay_loop_end = 0u;
    dma->delay_phase = DSPP_DELAY_PHASE_WHOLE;
    dma->delay_released = 0u;
    dma->delay_release_pending = 0u;
    dma->delay_finished = 0u;
    dma->delay_stopped = 0u;
    dma->delay_completion_pending = 0u;
    dma->delay_completion_done = 0u;
    dma->current_buffer = NULL;
    dma->current_count = 0u;
    dma->current_address = 0u;
    dma->next_buffer = NULL;
    dma->next_count = 0u;
    dma->next_address = 0u;
    dma->next_valid = 0u;
    dma->go_forever = 0u;
    dma->enabled = 0u;
    dma->direction = 0u;
    dma->complete = 0u;
    dspp_dma_refresh_serviceable(s, channel);
    dspp_delay_line_reset_fifo(s, channel);
}

int dspp_sim_dma_delay_line_attached(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    return dspp_dma_state_const_ptr(s, channel)->delay_line ? 1 : 0;
}

int dspp_sim_dma_promote_delay_line(dspp_sim *s, uint32_t channel,
                                    dspp_delay_line *line, int output,
                                    uint32_t start_frame) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS || !line ||
        (output != 0 && output != 1))
        return -1;
    if (line->word_count == 0u ||
        (!output && start_frame >= line->frame_count) ||
        (output && start_frame != 0u))
        return -1;

    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line || dma->delay_owner != s ||
        !dma->delay_stopped || dma->enabled)
        return -1;

    dspp_delay_line *old_line = dma->delay_line;
    const int same_line = old_line == line;
    // The old finite attachment has stopped, but the DSPP FIFO may still
    // contain values already transferred from it.  Release only the old
    // binding metadata; never reset FIFO pointers here.  If the next sample
    // is the same shared delay line, defer the clear check until its new
    // binding is counted so CLEARONSTOP cannot observe a transient zero.
    if (old_line->bindings != 0u) --old_line->bindings;
    if (dma->delay_stopped && old_line->stopped_bindings != 0u)
        --old_line->stopped_bindings;
    if (!same_line) dspp_delay_line_maybe_clear(old_line);

    dspp_completion_remove_locked(s, channel);
    ++line->bindings;
    dma->delay_line = line;
    dma->delay_owner = s;
    dspp_delay_schedule_initial(dma, line, output ? 0u : start_frame);
    dma->delay_stopped = 0u;
    dma->delay_completion_pending = 0u;
    dma->delay_completion_done = 0u;
    dma->enabled = 1u;
    dma->direction = output ? 1u : 0u;
    dma->width8 = 0u;
    dma->sqxd = 0u;
    dma->complete = 0u;
    dspp_dma_refresh_serviceable(s, channel);
    if (same_line) dspp_delay_line_maybe_clear(line);
    return 0;
}

int dspp_sim_dma_delay_line_stopped(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    const dspp_dma_channel *dma = dspp_dma_state_const_ptr(s, channel);
    return dma->delay_line && dma->delay_stopped ? 1 : 0;
}

int dspp_sim_dma_delay_line_fifo_empty(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    if (!dspp_dma_state_const_ptr(s, channel)->delay_line) return 0;
    const uint32_t *depth = dspp_fifo_depth_const_ptr(s, channel);
    return depth && *depth == 0u ? 1 : 0;
}

// ---- Completion-qualified event queue (one FIFO per space) -------------------
// M2 behavior: audio_attachments.c ProcessAttachmentDMA
// decrements SegmentCount per serviced segment and calls SignalMonitoringCue
// exactly when it reaches 0; dspp_simulator.c dspsReadFIFOtoDSPP
// raises Consumed only on a drain-to-empty while ChannelComplete is set. The
// numeric consumed generation is this layer's segment counter (one successful
// DSPP-side drain = one consumed word); the queue below records the
// completion edge (consumed reaches the bound numeric_count, count > 0) as
// pure data. Delivery (guest Cue signals) stays a folio seam at the existing
// MonitorAttachment pump: nothing here raises interrupts, signals, or bank
// bits, and unattached / count-0 channels never enqueue.
static uint8_t *dspp_completion_queue_ptr(dspp_sim *s) {
    if (!s) return NULL;
    return s->space ? s->space->completion_queue
                    : s->completion_queue_private;
}
static uint32_t *dspp_completion_head_ptr(dspp_sim *s) {
    if (!s) return NULL;
    return s->space ? &s->space->completion_head
                    : &s->completion_head_private;
}
static uint32_t *dspp_completion_count_ptr(dspp_sim *s) {
    if (!s) return NULL;
    return s->space ? &s->space->completion_count
                    : &s->completion_count_private;
}
static uint32_t dspp_completion_count_value(const dspp_sim *s) {
    uint32_t n;
    if (!s) return 0u;
    n = s->space ? s->space->completion_count
                 : s->completion_count_private;
    return n <= DSPI_NUM_DMA_CHANNELS ? n : DSPI_NUM_DMA_CHANNELS;
}
// Drop the queued entry for channel, if present. Clears the pending flag but
// keeps the done latch (caller decides the latch); compacting shift is O(32).
static void dspp_completion_remove_locked(dspp_sim *s, uint32_t channel) {
    uint8_t *q;
    uint32_t *head;
    uint32_t *count;
    uint32_t i;
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    q = dspp_completion_queue_ptr(s);
    head = dspp_completion_head_ptr(s);
    count = dspp_completion_count_ptr(s);
    if (!q || !head || !count || *count == 0u) return;
    for (i = 0u; i < *count; ++i) {
        uint32_t idx = (*head + i) % DSPI_NUM_DMA_CHANNELS;
        uint32_t j;
        if (q[idx] != (uint8_t)channel) continue;
        for (j = i; j + 1u < *count; ++j) {
            uint32_t dst = (*head + j) % DSPI_NUM_DMA_CHANNELS;
            uint32_t src = (*head + j + 1u) % DSPI_NUM_DMA_CHANNELS;
            q[dst] = q[src];
        }
        --*count;
        return;
    }
}
// Record the completion edge for a just-drained channel. Once per bind: the
// done latch suppresses re-queueing after polls and over-drains; only a
// successful re-attach releases it. Defensive overflow (unreachable with the
// per-channel dedup: 32 channels against a 32-entry queue) drops the oldest
// entry — the dropped channel keeps its done latch, so the event is
// discarded, never redelivered — and the newest completion is preserved.
static void dspp_completion_note_consumed(dspp_sim *s, uint32_t channel) {
    dspp_dma_channel *dma;
    uint8_t *q;
    uint32_t *head;
    uint32_t *count;
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->numeric_attached || dma->numeric_count == 0u) return;
    if (dma->numeric_completion_done || dma->numeric_completion_pending)
        return;
    if (dma->numeric_consumed < dma->numeric_count) return;
    q = dspp_completion_queue_ptr(s);
    head = dspp_completion_head_ptr(s);
    count = dspp_completion_count_ptr(s);
    if (!q || !head || !count) return;
    dma->numeric_completion_done = 1;
    dma->numeric_completion_pending = 1;
    if (*count >= DSPI_NUM_DMA_CHANNELS) {
        uint8_t oldest = q[*head];
        if (oldest < DSPI_NUM_DMA_CHANNELS) {
            dspp_dma_channel *old_dma = dspp_dma_state_ptr(s, oldest);
            if (old_dma) {
                old_dma->numeric_completion_pending = 0u;
                old_dma->delay_completion_pending = 0u;
            }
        }
        q[*head] = (uint8_t)channel;
        *head = (*head + 1u) % DSPI_NUM_DMA_CHANNELS;
        return;
    }
    q[(*head + *count) % DSPI_NUM_DMA_CHANNELS] = (uint8_t)channel;
    ++*count;
}

// Record the natural end of a finite Audio Folio delay-line attachment.
// Delay lines do not have a numeric guest address/count descriptor, but their
// finite DMA segment still reaches the same ProcessAttachmentDMA completion
// edge. Explicit stop uses completed=0 above and therefore never enters this
// queue. The channel id is sufficient because a physical FIFO has one live
// binding at a time.
static void dspp_completion_note_delay(dspp_sim *s, uint32_t channel) {
    dspp_dma_channel *dma;
    uint8_t *q;
    uint32_t *head;
    uint32_t *count;
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    dma = dspp_dma_state_ptr(s, channel);
    if (!dma || !dma->delay_line || dma->delay_line->if_loop) return;
    if (dma->delay_completion_done || dma->delay_completion_pending) return;
    q = dspp_completion_queue_ptr(s);
    head = dspp_completion_head_ptr(s);
    count = dspp_completion_count_ptr(s);
    if (!q || !head || !count) return;
    dma->delay_completion_done = 1u;
    dma->delay_completion_pending = 1u;
    if (*count >= DSPI_NUM_DMA_CHANNELS) {
        uint8_t oldest = q[*head];
        if (oldest < DSPI_NUM_DMA_CHANNELS) {
            dspp_dma_channel *old_dma = dspp_dma_state_ptr(s, oldest);
            if (old_dma) {
                old_dma->numeric_completion_pending = 0u;
                old_dma->delay_completion_pending = 0u;
            }
        }
        q[*head] = (uint8_t)channel;
        *head = (*head + 1u) % DSPI_NUM_DMA_CHANNELS;
        return;
    }
    q[(*head + *count) % DSPI_NUM_DMA_CHANNELS] = (uint8_t)channel;
    ++*count;
}

static int16_t dspp_fifo_read_dspi(dspp_sim *s, uint32_t ch) {
    uint16_t *fifo = dspp_fifo_words_ptr(s, ch);
    uint32_t *depth = dspp_fifo_depth_ptr(s, ch);
    uint32_t *dspi_ptr = dspp_fifo_dspi_ptr(s, ch);
    uint16_t *previous = dspp_fifo_prev_ptr(s, ch);
    if (*depth > 0u) {
        int16_t v = (int16_t)fifo[*dspi_ptr];
        *dspi_ptr = (*dspi_ptr + 1u) & 7u;
        --*depth;
        *dspp_dma_check_pending_ptr(s) = 1u;
        // The M2 updates fdma_PreviousCurrent on every successful DSPP
        // read, so an underflow repeats the most recently consumed sample.
        *previous = (uint16_t)v;
        // Raw-DSPP consumed generation: one successful DSPP-side drain on a
        // numeric-attached channel advances the saturating generation by one.
        // Underflows, CURRENT/NEXT peeks, and DMA-side reads never reach
        // here. No interrupt/signal/completion is raised; live current_/host
        // buffers and FIFO words/cursors above are already updated.
        {
            dspp_dma_channel *dma_state = dspp_dma_state_ptr(s, ch);
            if (dma_state->numeric_attached &&
                dma_state->numeric_consumed != 0xFFFFFFFFu)
                ++dma_state->numeric_consumed;
            // Completion-qualified edge: consumed reaches the bound count
            // (count > 0) on a numeric-attached channel. Records the event
            // in the per-space poll queue only; no bank bit, signal, or Cue
            // is raised here.
            dspp_completion_note_consumed(s, ch);
            // In the M2 simulator's completion-aware path, draining the
            // final FIFO word raises the per-channel Consumed interrupt.
            if (*depth == 0u && dma_state->complete)
                *dspp_dma_consumed_interrupt_ptr(s) |= 1u << ch;
        }
        return v;
    }
    // A DSPP-side FIFO underflow repeats the previous current sample and
    // raises UnderOver. DMA-side reads have a different M2 contract and
    // do not set this bit (see dspp_fifo_read_dma below).
    *dspp_dma_underover_interrupt_ptr(s) |= 1u << ch;
    return (int16_t)*previous;
}

static void dspp_fifo_write_dspi(dspp_sim *s, uint32_t ch, uint16_t v) {
    uint16_t *fifo = dspp_fifo_words_ptr(s, ch);
    uint32_t *depth = dspp_fifo_depth_ptr(s, ch);
    uint32_t *dspi_ptr = dspp_fifo_dspi_ptr(s, ch);
    if (*depth >= 8u) return;
    fifo[*dspi_ptr] = v;
    *dspi_ptr = (*dspi_ptr + 1u) & 7u;
    ++*depth;
    *dspp_dma_check_pending_ptr(s) = 1u;
}

static void dspp_run_oscillator(dspp_sim *s, uint32_t ch) {
    // The M2 simulator extracts the two carry bits before clipping the
    // fractional phase back to 15 bits.  Masking first loses every FIFO
    // advance and leaves sampler/oscillator templates permanently reading
    // their initial sample.
    uint16_t *phase_reg = dspp_dspi_phase_ptr(s);
    uint16_t *frequency_reg = dspp_dspi_frequency_ptr(s);
    uint16_t *last_count_reg = dspp_dspi_last_osc_count_ptr(s);
    uint32_t *fifo_depth = dspp_fifo_depth_ptr(s, ch);
    const uint32_t phase = (uint32_t)*phase_reg + (uint32_t)*frequency_reg;
    uint32_t cnt = (phase >> 15u) & 3u;
    *phase_reg = (uint16_t)(phase & 0x7FFFu);
    if (cnt > *fifo_depth) cnt = *fifo_depth;
    for (uint32_t i = 0; i < cnt; ++i) (void)dspp_fifo_read_dspi(s, ch);
    *last_count_reg = (uint16_t)cnt;
}

static uint16_t dspp_dma_decode_sqxd(uint8_t coded, uint32_t *previous) {
    const int32_t temp = (int32_t)(int8_t)(coded & 0xFEu);
    int32_t expanded = temp * (temp < 0 ? -temp : temp);
    expanded *= 2;
    int32_t output;
    if (coded & 1u) {
        expanded >>= 2;
        output = expanded + (int32_t)*previous;
    } else {
        output = expanded;
    }
    uint16_t result = (uint16_t)output;
    *previous = result;
    return result;
}

static uint16_t dspp_fifo_read_dma(dspp_sim *s, uint32_t channel) {
    uint16_t *fifo = dspp_fifo_words_ptr(s, channel);
    uint32_t *depth = dspp_fifo_depth_ptr(s, channel);
    uint32_t *dma_ptr = dspp_fifo_dma_ptr(s, channel);
    uint16_t value = fifo[*dma_ptr];
    if (*depth > 0u) {
        *dma_ptr = (*dma_ptr + 1u) & 7u;
        --*depth;
        *dspp_dma_check_pending_ptr(s) = 1u;
        return value;
    }
    // dspsReadFIFOtoDMA logs the underflow but returns the addressed FIFO
    // slot without changing the DMA cursor or the UnderOver interrupt bank.
    return value;
}

static uint16_t dspp_delay_line_read(dspp_dma_channel *dma) {
    dspp_delay_line *line = dma ? dma->delay_line : NULL;
    if (!line || line->word_count == 0u || dma->delay_finished)
        return 0;
    dspp_delay_prepare_cursor(dma);
    if (dma->delay_finished) return 0;
    uint16_t value = line->words[dma->delay_cursor];
    ++dma->delay_cursor;
    dspp_delay_prepare_cursor(dma);
    return value;
}

static void dspp_delay_line_write(dspp_dma_channel *dma, uint16_t value) {
    dspp_delay_line *line = dma ? dma->delay_line : NULL;
    if (!line || line->word_count == 0u || dma->delay_finished) return;
    dspp_delay_prepare_cursor(dma);
    if (dma->delay_finished) return;
    line->words[dma->delay_cursor] = value;
    ++dma->delay_cursor;
    dspp_delay_prepare_cursor(dma);
}

static void dspp_dma_process_next(dspp_sim *s, uint32_t channel) {
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->current_count != 0u) return;
    if (dma->next_valid) {
        dma->current_buffer = dma->next_buffer;
        dma->current_count = dma->next_count;
        dma->current_address = dma->next_address;
        if (!dma->go_forever) dma->next_valid = 0;
        dma->complete = 0;
        *dspp_dma_next_interrupt_ptr(s) |= 1u << channel;
    } else {
        // Pass-2 dspsProcessNextDMA disables a channel with no next block.
        dma->enabled = 0;
    }
    dspp_dma_refresh_serviceable(s, channel);
}

static int dspp_dma_service_input(dspp_sim *s, uint32_t channel) {
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->delay_line) {
        uint32_t depth = *dspp_fifo_depth_ptr(s, channel);
        if (depth > 4u) return 0;
        dspp_delay_prepare_cursor(dma);
        if (dma->delay_finished) {
            dspp_delay_line_mark_stopped(s, channel, 1);
            return 0;
        }
        uint32_t local_count = 4u;
        if (8u - depth < local_count) local_count = 8u - depth;
        if (local_count == 0u) {
            dspp_delay_line_mark_stopped(s, channel, 1);
            return 0;
        }
        uint32_t transferred = 0u;
        for (uint32_t i = 0; i < local_count; ++i) {
            dspp_delay_prepare_cursor(dma);
            if (dma->delay_finished) break;
            uint16_t sample = dspp_delay_line_read(dma);
            dspp_sim_fifo_push(s, channel, (int16_t)sample);
            ++transferred;
        }
        if (dma->delay_finished) {
            dspp_delay_line_mark_stopped(s, channel, 1);
        }
        return (int)transferred;
    }
    if (dma->current_count == 0u || !dma->current_buffer) return 0;
    uint32_t local_count = dma->current_count;
    if (local_count > 4u) local_count = 4u;
    uint8_t *data = dma->current_buffer;
    for (uint32_t i = 0; i < local_count; ++i) {
        uint16_t sample;
        if (dma->width8) {
            uint8_t coded = *data++;
            if (dma->sqxd)
                sample = dspp_dma_decode_sqxd(coded, &dma->previous_value);
            else
                sample = (uint16_t)((int16_t)(int8_t)coded << 8);
        } else {
            sample = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            data += 2;
        }
        dspp_sim_fifo_push(s, channel, (int16_t)sample);
    }
    dma->current_buffer = data;
    dma->current_count -= local_count;
    dspp_dma_process_next(s, channel);
    return (int)local_count;
}

static int dspp_dma_service_output(dspp_sim *s, uint32_t channel) {
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->delay_line) {
        uint32_t depth = *dspp_fifo_depth_ptr(s, channel);
        if (depth < 4u) return 0;
        uint32_t local_count = depth > 4u ? 4u : depth;
        if (local_count == 0u) {
            dspp_delay_line_mark_stopped(s, channel, 1);
            return 0;
        }
        uint32_t transferred = 0u;
        for (uint32_t i = 0; i < local_count; ++i) {
            dspp_delay_prepare_cursor(dma);
            if (dma->delay_finished) break;
            dspp_delay_line_write(dma, dspp_fifo_read_dma(s, channel));
            ++transferred;
        }
        if (dma->delay_finished) {
            // A non-looping delay line follows the M2's ordinary
            // attachment transition to its next/silence segment.  There is
            // no next segment in this platform-only binding, so stop the
            // line after draining the final FIFO words.
            dspp_delay_line_mark_stopped(s, channel, 1);
        }
        return (int)transferred;
    }
    if (dma->current_count == 0u || !dma->current_buffer) return 0;
    uint32_t local_count = dma->current_count;
    if (local_count > 4u) local_count = 4u;
    uint8_t *data = dma->current_buffer;
    for (uint32_t i = 0; i < local_count; ++i) {
        uint16_t sample = dspp_fifo_read_dma(s, channel);
        *data++ = (uint8_t)(sample >> 8);
        *data++ = (uint8_t)sample;
    }
    dma->current_buffer = data;
    dma->current_count -= local_count;
    dspp_dma_process_next(s, channel);
    return (int)local_count;
}

static int dspp_dma_check(dspp_sim *s) {
    if (!s) return -1;
    uint32_t mask = *dspp_dma_serviceable_mask_ptr(s);
    while (mask != 0u) {
        const int channel = dspp_dma_highest_channel(mask);
        if (channel < 0) break;
        const uint32_t bit = 1u << (uint32_t)channel;
        mask &= ~bit;
        const dspp_dma_channel *dma =
            dspp_dma_state_const_ptr(s, (uint32_t)channel);
        // Revalidate: a missed state transition must fail closed rather than
        // servicing a channel the hardware would have skipped.
        if (!dma->enabled || dma->complete) continue;
        uint32_t depth = *dspp_fifo_depth_ptr(s, (uint32_t)channel);
        if ((dma->direction && depth >= 4u) ||
            (!dma->direction && depth <= 4u))
            return channel;
    }
    return -1;
}

static int dspp_run_fdma(dspp_sim *s) {
    int channel = dspp_dma_check(s);
    if (channel < 0) return -1;
    if (dspp_dma_state_ptr(s, (uint32_t)channel)->direction)
        dspp_dma_service_output(s, (uint32_t)channel);
    else
        dspp_dma_service_input(s, (uint32_t)channel);
    return channel;
}

int dspp_sim_dma_set_current(dspp_sim *s, uint32_t channel, void *buffer,
                             uint32_t sample_count, uint32_t flags) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS ||
        (sample_count != 0u && !buffer))
        return -1;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->delay_line) return -1;
    dma->current_buffer = (uint8_t *)buffer;
    dma->current_count = sample_count;
    dma->current_address = 0;
    dma->direction = (flags & DSPP_SIM_DMA_TO_RAM) != 0;
    dma->width8 = (flags & DSPP_SIM_DMA_8BIT) != 0;
    dma->sqxd = (flags & DSPP_SIM_DMA_SQXD) != 0;
    dma->complete = 0;
    dspp_dma_refresh_serviceable(s, channel);
    return 0;
}

int dspp_sim_dma_set_next(dspp_sim *s, uint32_t channel, void *buffer,
                          uint32_t sample_count) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS ||
        (sample_count != 0u && !buffer))
        return -1;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->delay_line) return -1;
    dma->next_buffer = (uint8_t *)buffer;
    dma->next_count = sample_count;
    dma->next_address = 0;
    dma->next_valid = 1;
    return 0;
}

void dspp_sim_dma_set_control(dspp_sim *s, uint32_t channel, int enabled,
                              int go_forever) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    dma->enabled = enabled ? 1 : 0;
    dma->go_forever = go_forever ? 1 : 0;
    dspp_dma_refresh_serviceable(s, channel);
}

int dspp_sim_dma_step(dspp_sim *s) {
    return s ? dspp_run_fdma(s) : -1;
}

uint32_t dspp_sim_dma_dmanext_interrupts(const dspp_sim *s) {
    const uint32_t *interrupts = dspp_dma_next_interrupt_const_ptr(s);
    return interrupts ? *interrupts : 0;
}

void dspp_sim_dma_clear_dmanext_interrupts(dspp_sim *s, uint32_t mask) {
    uint32_t *interrupts = dspp_dma_next_interrupt_ptr(s);
    if (interrupts) *interrupts &= ~mask;
}
// ---- Armed DMA-interrupt dispatch (FIRQ second level) -------------------------
// M2 behavior: dspp_simulator.c dspsGenMainInt (armed = pending & enabled),
// dspp_irq_bulldog.c dsphHandleDMANEXT (ValidInts = SET & ENABLE; CLR after
// service; daemon signal delivery separate), dspp_touch_bulldog.c
// dsphResetFIFO/dsphInitInterruptRegs (clear stale DMANext/Consumed/UnderOver
// before start), dsphEnable/DisableChannelInterrupt (DMANext enable/disable via
// the DMA CONTROL shadow DMANEXT_EN bit; no INT_DMANEXT_DISABLE register) and
// dspp_touch_dma.c (clear previous DMANext before programming next). The bank
// words below are the same words the DSPX register path reads/writes, so the
// helpers and the raw register boundary always agree. Pure observation and
// bank control only: no signal/Cue/completion is raised; delivery stays a
// folio seam.
void dspp_sim_dma_enable_interrupts(dspp_sim *s, uint32_t banks,
                                    uint32_t channel_mask) {
    if (!s) return;
    if (banks & DSPP_DMA_BANK_DMANEXT)
        *dspp_dma_next_interrupt_enable_ptr(s) |= channel_mask;
    if (banks & DSPP_DMA_BANK_CONSUMED)
        *dspp_dma_consumed_interrupt_enable_ptr(s) |= channel_mask;
    if (banks & DSPP_DMA_BANK_UNDEROVER)
        *dspp_dma_underover_interrupt_enable_ptr(s) |= channel_mask;
}
void dspp_sim_dma_disable_interrupts(dspp_sim *s, uint32_t banks,
                                     uint32_t channel_mask) {
    if (!s) return;
    if (banks & DSPP_DMA_BANK_DMANEXT)
        *dspp_dma_next_interrupt_enable_ptr(s) &= ~channel_mask;
    if (banks & DSPP_DMA_BANK_CONSUMED)
        *dspp_dma_consumed_interrupt_enable_ptr(s) &= ~channel_mask;
    if (banks & DSPP_DMA_BANK_UNDEROVER)
        *dspp_dma_underover_interrupt_enable_ptr(s) &= ~channel_mask;
}
void dspp_sim_dma_clear_interrupts(dspp_sim *s, uint32_t banks,
                                   uint32_t channel_mask) {
    if (!s) return;
    if (banks & DSPP_DMA_BANK_DMANEXT)
        *dspp_dma_next_interrupt_ptr(s) &= ~channel_mask;
    if (banks & DSPP_DMA_BANK_CONSUMED)
        *dspp_dma_consumed_interrupt_ptr(s) &= ~channel_mask;
    if (banks & DSPP_DMA_BANK_UNDEROVER)
        *dspp_dma_underover_interrupt_ptr(s) &= ~channel_mask;
}
dspp_dma_armed dspp_sim_dma_armed_interrupts(const dspp_sim *s) {
    dspp_dma_armed armed = {0u, 0u, 0u};
    if (!s) return armed;
    armed.dmanext =
        *dspp_dma_next_interrupt_const_ptr(s) &
        *dspp_dma_next_interrupt_enable_const_ptr(s);
    armed.consumed =
        *dspp_dma_consumed_interrupt_const_ptr(s) &
        *dspp_dma_consumed_interrupt_enable_const_ptr(s);
    armed.underover =
        *dspp_dma_underover_interrupt_const_ptr(s) &
        *dspp_dma_underover_interrupt_enable_const_ptr(s);
    return armed;
}

// ---- FIRQ first-level dispatch ---------------------------------------------
// M2 behavior: dspp_irq_bulldog.c dsphFIRQHandler (validInts = SET & ENABLE;
// DMANext -> timer -> soft -> unexpected precedence), dsphHandleDMANEXT
// (second-level ValidInts = DSPX_INT_DMANEXT_SET & ENABLE, FindMSB order,
// CLR after service, all-done channel-interrupt disable on the control
// shadow), dsphHandleTimer (clear timer), dsphHandleSoftInt (disable received
// soft bits; daemon calls dsphGetReceivedTriggers to read/clear), and
// dspp_simulator.c dspsGenMainInt composing the main word. No signals, Cues,
// or completions are raised: the folio daemon owns delivery.
dspp_firq_dispatch dspp_sim_dma_firq_dispatch(dspp_sim *s) {
    dspp_firq_dispatch out = {0u, 0, 0u};
    if (!s) return out;
    uint32_t valid = dspp_sim_dma_main_interrupt(s) &
                     dspp_sim_dma_main_interrupt_enable(s);
    // DMA next (highest precedence): service each armed second-level channel.
    if (valid & DSPX_F_INT_DMANEXT) {
        uint32_t pending = dspp_sim_dma_armed_interrupts(s).dmanext;
        while (pending) {
            uint32_t channel = 31u - (uint32_t)__builtin_clz(pending);
            uint32_t mask = 1u << channel;
            pending &= ~mask;
            // Clear after service (M2 dsphHandleDMANEXT); the DMA adapter
            // already committed the queued next descriptor when it raised the
            // event, so both M2 countdowns are zero and the M2's
            // all-done rule disables the channel interrupt until the folio
            // programs another segment.
            *dspp_dma_next_interrupt_ptr(s) &= ~mask;
            dspp_sim_dma_set_channel_interrupt(s, channel, 0);
            out.dmanext |= mask;
        }
        valid &= ~DSPX_F_INT_DMANEXT;
    }
    // Timer: dsphHandleTimer clears so the interrupt does not fire again
    // immediately (the level test would otherwise re-raise while AudioTime
    // stays past WakeupTime).
    if (valid & DSPX_F_INT_TIMER) {
        *dspp_dma_main_interrupt_partial_ptr(s) &= ~DSPX_F_INT_TIMER;
        out.timer = 1;
        valid &= ~DSPX_F_INT_TIMER;
    }
    // Soft ints (triggers): dsphHandleSoftInt disables the received bits and
    // signals the daemon, whose HandleTriggerSignal reads/clears them through
    // dsphGetReceivedTriggers (SET & armed mask).
    if (valid & DSPX_FLD_INT_SOFT_MASK) {
        out.soft = valid & DSPX_FLD_INT_SOFT_MASK;
        *dspp_dma_main_interrupt_enable_ptr(s) &= ~out.soft;
    }
    // Remaining valid bits (input/output over/under): M2
    // dsphHandleUnexpected is a no-op touch. Nothing to do.
    return out;
}

// M2 device + folio interrupt initialization for one DSPP: the hardware
// powers on with zeroed banks (space calloc), dsphInitInterruptRegs clears
// them again explicitly, and InitDSPPInterrupt enables the DMANext main bit
// after WriteHardware(DSPX_INTERRUPT_CLR, ~0). The M2 folio's own trigger
// enables (DSPX_INTERRUPT_ENABLE soft bits) arrive later through ArmTrigger.
void dspp_sim_space_init_folio_interrupts(dspp_sim_space *space) {
    if (!space) return;
    space->dma_main_interrupt_partial = 0;
    space->dma_main_interrupt_enable = DSPX_F_INT_DMANEXT;
    space->dma_next_interrupt = 0;
    space->dma_next_interrupt_enable = 0;
    space->dma_consumed_interrupt = 0;
    space->dma_consumed_interrupt_enable = 0;
    space->dma_underover_interrupt = 0;
    space->dma_underover_interrupt_enable = 0;
}

// Decode the byte-addressed DSPX DMA stack used by dsppAccessDMA_Stack.
// kind=1 is the four-word descriptor stack; kind=2 is its current/next
// control pair. The M2 pass-2 control write uses bit 3 of the control address
// to select the next shadow commit.
static int dspp_dma_register_decode(uint32_t addr, int *kind,
                                    uint32_t *channel, uint32_t *part) {
    if (addr >= DSPX_DMA_STACK &&
        addr < DSPX_DMA_STACK +
                   DSPI_NUM_DMA_CHANNELS * DSPX_DMA_STACK_CHANNEL_SIZE) {
        uint32_t rel = addr - DSPX_DMA_STACK;
        if (rel & 3u) return 0;
        *kind = 1;
        *channel = rel / DSPX_DMA_STACK_CHANNEL_SIZE;
        *part = (rel % DSPX_DMA_STACK_CHANNEL_SIZE) / 4u;
        return 1;
    }
    if (addr >= DSPX_DMA_STACK_CONTROL &&
        addr < DSPX_DMA_STACK_CONTROL +
                   DSPI_NUM_DMA_CHANNELS * DSPX_DMA_STACK_CONTROL_SIZE) {
        uint32_t rel = addr - DSPX_DMA_STACK_CONTROL;
        if (rel & 3u) return 0;
        *kind = 2;
        *channel = rel / DSPX_DMA_STACK_CONTROL_SIZE;
        *part = rel % DSPX_DMA_STACK_CONTROL_SIZE;
        return 1;
    }
    return 0;
}

static uint32_t dspp_dma_channel_mask(const dspp_sim *s, int which) {
    uint32_t mask = 0;
    for (uint32_t channel = 0; channel < DSPI_NUM_DMA_CHANNELS; ++channel) {
        const dspp_dma_channel *dma =
            dspp_dma_state_const_ptr(s, channel);
        int set = which == 0 ? dma->enabled
                 : which == 1 ? dma->direction
                 : which == 2 ? dma->width8
                 : which == 3 ? dma->sqxd
                 : dma->complete;
        if (set) mask |= 1u << channel;
    }
    return mask;
}

uint32_t dspp_sim_dma_register_read(const dspp_sim *s, uint32_t addr) {
    if (!s) return 0;
    if (addr == DSPX_INTERRUPT_SET || addr == DSPX_INTERRUPT_CLR)
        return dspp_sim_dma_main_interrupt(s);
    if (addr == DSPX_INTERRUPT_ENABLE || addr == DSPX_INTERRUPT_DISABLE)
        return *dspp_dma_main_interrupt_enable_const_ptr(s);
    int kind;
    uint32_t channel, part;
    if (dspp_dma_register_decode(addr, &kind, &channel, &part)) {
        const dspp_dma_channel *dma =
            dspp_dma_state_const_ptr(s, channel);
        if (kind == 1) {
            switch (part) {
            case 0: return dma->current_address;
            case 1: return dma->current_count;
            case 2: return dma->next_address;
            case 3: return dma->next_count;
            default: return 0;
            }
        }
        return (dma->next_valid ? DSPX_F_DMA_NEXTVALID : 0u) |
               (dma->go_forever ? DSPX_F_DMA_GO_FOREVER : 0u);
    }
    if (addr == DSPX_CHANNEL_ENABLE || addr == DSPX_CHANNEL_DISABLE)
        return dspp_dma_channel_mask(s, 0);
    if (addr == DSPX_CHANNEL_DIRECTION_SET ||
        addr == DSPX_CHANNEL_DIRECTION_CLR)
        return dspp_dma_channel_mask(s, 1);
    if (addr == DSPX_CHANNEL_8BIT_SET || addr == DSPX_CHANNEL_8BIT_CLR)
        return dspp_dma_channel_mask(s, 2);
    if (addr == DSPX_CHANNEL_SQXD_SET || addr == DSPX_CHANNEL_SQXD_CLR)
        return dspp_dma_channel_mask(s, 3);
    if (addr == DSPX_CHANNEL_STATUS)
        return dspp_dma_channel_mask(s, 4);
    if (addr == DSPX_INT_DMANEXT_SET || addr == DSPX_INT_DMANEXT_CLR)
        return *dspp_dma_next_interrupt_const_ptr(s);
    if (addr == DSPX_INT_DMANEXT_ENABLE) {
        return s->space ? s->space->dma_next_interrupt_enable
                        : s->dma_next_interrupt_enable;
    }
    if (addr == DSPX_INT_CONSUMED_SET || addr == DSPX_INT_CONSUMED_CLR)
        return *dspp_dma_consumed_interrupt_const_ptr(s);
    if (addr == DSPX_INT_CONSUMED_ENABLE ||
        addr == DSPX_INT_CONSUMED_DISABLE)
        return *dspp_dma_consumed_interrupt_enable_const_ptr(s);
    if (addr == DSPX_INT_UNDEROVER_SET || addr == DSPX_INT_UNDEROVER_CLR) {
        return *dspp_dma_underover_interrupt_const_ptr(s);
    }
    if (addr == DSPX_INT_UNDEROVER_ENABLE ||
        addr == DSPX_INT_UNDEROVER_DISABLE)
        return *dspp_dma_underover_interrupt_enable_const_ptr(s);
    return 0;
}

void dspp_sim_dma_register_write(dspp_sim *s, uint32_t addr,
                                 uint32_t value) {
    if (!s) return;
    if (addr == DSPX_INTERRUPT_SET || addr == DSPX_INTERRUPT_CLR) {
        uint32_t *partial = dspp_dma_main_interrupt_partial_ptr(s);
        const uint32_t non_dma = value & ~DSPX_F_INT_ALL_DMA;
        if (addr == DSPX_INTERRUPT_SET)
            *partial |= non_dma;
        else
            *partial &= ~non_dma;
        return;
    }
    if (addr == DSPX_INTERRUPT_ENABLE || addr == DSPX_INTERRUPT_DISABLE) {
        uint32_t *enable = dspp_dma_main_interrupt_enable_ptr(s);
        if (addr == DSPX_INTERRUPT_ENABLE)
            *enable |= value;
        else
            *enable &= ~value;
        return;
    }
    int kind;
    uint32_t channel, part;
    if (dspp_dma_register_decode(addr, &kind, &channel, &part)) {
        dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
        if (kind == 1) {
            switch (part) {
            case 0: *dspp_dma_shadow_current_address_ptr(s) = value; break;
            case 1: *dspp_dma_shadow_current_count_ptr(s) = value; break;
            case 2: *dspp_dma_shadow_next_address_ptr(s) = value; break;
            case 3: *dspp_dma_shadow_next_count_ptr(s) = value; break;
            default: break;
            }
            return;
        }
        if (value & DSPX_F_SHADOW_SET_NEXTVALID)
            dma->next_valid = (value & DSPX_F_DMA_NEXTVALID) != 0;
        if (value & DSPX_F_SHADOW_SET_FOREVER)
            dma->go_forever = (value & DSPX_F_DMA_GO_FOREVER) != 0;
        if (value & DSPX_F_SHADOW_SET_DMANEXT) {
            uint32_t *enable = dspp_dma_next_interrupt_enable_ptr(s);
            if (value & DSPX_F_INT_DMANEXT_EN)
                *enable |= 1u << channel;
            else
                *enable &= ~(1u << channel);
        }
        if (value & DSPX_F_SHADOW_SET_ADDRESS_COUNT) {
            if (part & 8u) {
                dma->next_address = *dspp_dma_shadow_next_address_ptr(s);
                dma->next_count = *dspp_dma_shadow_next_count_ptr(s);
            } else {
                dma->current_address =
                    *dspp_dma_shadow_current_address_ptr(s);
                dma->current_count = *dspp_dma_shadow_current_count_ptr(s);
            }
        }
        return;
    }
    if (addr == DSPX_CHANNEL_ENABLE || addr == DSPX_CHANNEL_DISABLE ||
        addr == DSPX_CHANNEL_DIRECTION_SET ||
        addr == DSPX_CHANNEL_DIRECTION_CLR || addr == DSPX_CHANNEL_8BIT_SET ||
        addr == DSPX_CHANNEL_8BIT_CLR || addr == DSPX_CHANNEL_SQXD_SET ||
        addr == DSPX_CHANNEL_SQXD_CLR) {
        for (uint32_t channel = 0; channel < DSPI_NUM_DMA_CHANNELS;
             ++channel) {
            if (!(value & (1u << channel))) continue;
            dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
            if (addr == DSPX_CHANNEL_ENABLE) dma->enabled = 1;
            else if (addr == DSPX_CHANNEL_DISABLE) dma->enabled = 0;
            else if (addr == DSPX_CHANNEL_DIRECTION_SET) dma->direction = 1;
            else if (addr == DSPX_CHANNEL_DIRECTION_CLR) dma->direction = 0;
            else if (addr == DSPX_CHANNEL_8BIT_SET) dma->width8 = 1;
            else if (addr == DSPX_CHANNEL_8BIT_CLR) dma->width8 = 0;
            else if (addr == DSPX_CHANNEL_SQXD_SET) dma->sqxd = 1;
            else if (addr == DSPX_CHANNEL_SQXD_CLR) dma->sqxd = 0;
            dspp_dma_refresh_serviceable(s, channel);
        }
        return;
    }
    if (addr == DSPX_CHANNEL_RESET) {
        for (uint32_t channel = 0; channel < DSPI_NUM_DMA_CHANNELS;
             ++channel) {
            if (!(value & (1u << channel))) continue;
            *dspp_fifo_depth_ptr(s, channel) = 0;
            *dspp_dma_check_pending_ptr(s) = 1u;
            *dspp_fifo_dma_ptr(s, channel) = 0;
            *dspp_fifo_dspi_ptr(s, channel) = 0;
            dspp_dma_state_ptr(s, channel)->complete = 0;
            dspp_dma_refresh_serviceable(s, channel);
        }
        return;
    }
    if (addr == DSPX_INT_DMANEXT_SET) {
        *dspp_dma_next_interrupt_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_DMANEXT_CLR) {
        *dspp_dma_next_interrupt_ptr(s) &= ~value;
        return;
    }
    if (addr == DSPX_INT_DMANEXT_ENABLE) {
        *dspp_dma_next_interrupt_enable_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_CONSUMED_SET) {
        *dspp_dma_consumed_interrupt_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_CONSUMED_CLR) {
        *dspp_dma_consumed_interrupt_ptr(s) &= ~value;
        return;
    }
    if (addr == DSPX_INT_CONSUMED_ENABLE) {
        *dspp_dma_consumed_interrupt_enable_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_CONSUMED_DISABLE) {
        *dspp_dma_consumed_interrupt_enable_ptr(s) &= ~value;
        return;
    }
    if (addr == DSPX_INT_UNDEROVER_SET) {
        *dspp_dma_underover_interrupt_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_UNDEROVER_CLR) {
        *dspp_dma_underover_interrupt_ptr(s) &= ~value;
        return;
    }
    if (addr == DSPX_INT_UNDEROVER_ENABLE) {
        *dspp_dma_underover_interrupt_enable_ptr(s) |= value;
        return;
    }
    if (addr == DSPX_INT_UNDEROVER_DISABLE) {
        *dspp_dma_underover_interrupt_enable_ptr(s) &= ~value;
        return;
    }
}

static uint16_t dspp_register_read(dspp_sim *s, uint32_t addr) {
    if (addr >= DSPI_FIFO_OSC_BASE &&
        addr < DSPI_FIFO_OSC_BASE + DSPI_NUM_DMA_CHANNELS * DSPI_FIFO_OSC_SIZE &&
        (addr & (DSPI_FIFO_OSC_SIZE - 1u)) < DSPI_FIFO_OSC_NUM_REGS) {
        uint32_t ch = (addr - DSPI_FIFO_OSC_BASE) / DSPI_FIFO_OSC_SIZE;
        uint32_t part = addr & (DSPI_FIFO_OSC_SIZE - 1u);
        uint16_t *fifo = dspp_fifo_words_ptr(s, ch);
        uint32_t *depth = dspp_fifo_depth_ptr(s, ch);
        uint32_t *dspi_ptr = dspp_fifo_dspi_ptr(s, ch);
        uint16_t *previous = dspp_fifo_prev_ptr(s, ch);
        switch (part) {
        case 0: // CURRENT
            if (*depth == 0u) return *previous;
            return fifo[*dspi_ptr];
        case 1: // NEXT
            if (*depth == 0u) return *previous;
            if (*depth == 1u) return fifo[*dspi_ptr];
            return fifo[(*dspi_ptr + 1u) & 7u];
        case 2: // FREQUENCY read = LastOscCount (single shared register)
            return *dspp_dspi_last_osc_count_ptr(s);
        case 3: // PHASE
            return *dspp_dspi_phase_ptr(s);
        case DSPI_FIFO_OFFSET_DATA:
            return (uint16_t)dspp_fifo_read_dspi(s, ch);
        case DSPI_FIFO_OFFSET_CONTROL:
            return (uint16_t)*depth;
        default:
            return 0;
        }
    }
    if (addr >= DSPI_INPUT0 && addr <= DSPI_INPUT0 + 1u)
        return dspp_dspi_inputs_ptr(s)[addr - DSPI_INPUT0];
    if (addr >= DSPI_OUTPUT0 && addr <= DSPI_OUTPUT0 + 7u)
        return 0; // output register reads not modeled
    if (addr == DSPI_OUTPUT_STATUS) {
        uint32_t *write_ptr = dspp_dspi_output_write_ptr(s);
        uint32_t *read_ptr = dspp_dspi_output_read_ptr(s);
        uint32_t distance = *write_ptr - *read_ptr;
        return (uint16_t)(distance & (DSPI_OUTPUT_FIFO_MAX_DEPTH - 1u));
    }
    if (addr == DSPI_INPUT_STATUS)
        return 1; // M2 simulator exposes the input status as ready
    if (addr == 0x03FFu) return 0; // DSPI_NOISE: no synthetic randomness
    return 0;
}

static inline uint16_t dspp_mem_read(dspp_sim *s, uint32_t addr) {
    addr &= 0x3FFu;
    return addr < DSPI_DATA_MEMORY_SIZE ? s->data_image[addr]
                                        : dspp_register_read(s, addr);
}

static void dspp_register_write(dspp_sim *s, uint32_t addr, uint16_t value) {
    if (addr >= DSPI_FIFO_OSC_BASE &&
        addr < DSPI_FIFO_OSC_BASE + DSPI_NUM_DMA_CHANNELS * DSPI_FIFO_OSC_SIZE &&
        (addr & (DSPI_FIFO_OSC_SIZE - 1u)) < DSPI_FIFO_OSC_NUM_REGS) {
        uint32_t ch = (addr - DSPI_FIFO_OSC_BASE) / DSPI_FIFO_OSC_SIZE;
        uint32_t part = addr & (DSPI_FIFO_OSC_SIZE - 1u);
        if (part == 2) { // FREQUENCY write runs the oscillator
            *dspp_dspi_frequency_ptr(s) = value;
            dspp_run_oscillator(s, ch);
        } else if (part == 3) {
            *dspp_dspi_phase_ptr(s) = value;
        } else if (part == DSPI_FIFO_OFFSET_DATA) {
            dspp_fifo_write_dspi(s, ch, value);
        }
        return;
    }
    if (addr >= DSPI_OUTPUT0 && addr <= DSPI_OUTPUT0 + 7u) {
        dspp_dspi_outputs_ptr(s)[addr - DSPI_OUTPUT0] = value;
        return;
    }
    if (addr == DSPI_OUTPUT_CONTROL) {
        if (value & 1u) {
            uint32_t *write_ptr = dspp_dspi_output_write_ptr(s);
            uint16_t *fifo = dspp_dspi_output_fifo_ptr(s);
            uint16_t *outputs = dspp_dspi_outputs_ptr(s);
            uint16_t *inputs = dspp_dspi_inputs_ptr(s);
            uint32_t wi = *write_ptr;
            // The M2 simulator writes the pair even when the reader has
            // caught the writer (it only reports that overflow condition).
            fifo[wi] = outputs[0];
            fifo[wi + 1u] = outputs[1];
            inputs[0] = outputs[0];
            inputs[1] = outputs[1];
            *write_ptr = wi + 2u;
            if (*write_ptr >= DSPI_OUTPUT_FIFO_MAX_DEPTH)
                *write_ptr = 0;
        }
        return;
    }
    if (addr == DSPI_CPU_INT) {
        // M2 dspp_simulator.c (Bulldog/M2 single-register form): a DSPP
        // write to DSPI_CPU_INT raises soft-interrupt bits
        // (Val << DSPX_FLD_INT_SOFT_SHIFT) & DSPX_FLD_INT_SOFT_MASK in the
        // main-interrupt partial bank. The audio-folio FIRQ handler consumes
        // those bits (disable + daemon trigger signal); the loader raises
        // them only, shared across every live sim in the space.
        *dspp_dma_main_interrupt_partial_ptr(s) |=
            (((uint32_t)value << DSPX_FLD_INT_SOFT_SHIFT) &
             DSPX_FLD_INT_SOFT_MASK);
        return;
    }
    // The remaining control/register contracts are kept as explicit open
    // boundaries; do not turn an unmodeled write into data-memory state.
}

static inline void dspp_mem_write(dspp_sim *s, uint32_t addr, uint16_t value) {
    addr &= 0x3FFu;
    if (addr < DSPI_DATA_MEMORY_SIZE)
        s->data_image[addr] = value;
    else
        dspp_register_write(s, addr, value);
}

uint16_t dspp_sim_dspi_read(dspp_sim *s, uint32_t addr) {
    return s ? dspp_mem_read(s, addr) : 0;
}

void dspp_sim_dspi_write(dspp_sim *s, uint32_t addr, uint16_t value) {
    if (s) dspp_mem_write(s, addr, value);
}

static uint32_t dspp_translate_reg(dspp_sim *s, uint32_t reg) {
    if (reg < 4u) return s->rbase[0] + reg;
    if (reg < 8u) return s->rbase[1] + reg - 4u;
    if (reg < 12u) return s->rbase[2] + reg - 8u;
    return s->rbase[3] + reg - 12u;
}

static void dspp_set_rbase(dspp_sim *s, uint32_t which, uint32_t address) {
    switch (which) {
    case 4:
        s->rbase[1] = (int32_t)(address + 4u - which);
        break;
    case 0:
        s->rbase[0] = (int32_t)address;
        s->rbase[1] = (int32_t)(address + 4u);
        s->rbase[2] = (int32_t)(address + 8u);
        s->rbase[3] = (int32_t)(address + 12u);
        break;
    case 8:
        s->rbase[2] = (int32_t)(address + 8u - which);
        break;
    case 12:
        s->rbase[3] = (int32_t)(address + 12u - which);
        break;
    default:
        break;
    }
}

static void dspp_parse_operands(dspp_sim *s, int num) {
    s->op_index = 0;
    for (int i = 0; i < s->ops_in_use; ++i) {
        s->ops[i].addr = -1;
        s->ops[i].value = -1;
    }
    int oi = 0;
    int num_regs = 0;
    while (oi < num) {
        uint16_t operand = dspp_fetch_code(s, s->pc);
        s->pc = (s->pc + 1u) & 0x3FFu;
        if (operand & 0x8000u) {
            if ((operand & 0xC000u) == 0xC000u) {
                int32_t val = operand & 0x1FFFu;
                if (operand & DSPN_IMMED_F_JUSTIFY) {
                    val = (val << 3) & 0xFFFF;
                } else if (val & 0x1000) {
                    val |= 0xE000;
                }
                s->ops[oi].value = val;
                ++oi;
            } else if ((operand & 0xE000u) == 0x8000u) {
                int32_t addr = operand & DSPN_ADDR_MASK;
                if (operand & DSPN_ADDR_F_IND) addr = dspp_mem_read(s, (uint32_t)addr);
                s->ops[oi].addr = addr;
                if (operand & DSPN_ADDR_F_WRITE_BACK) s->write_back = addr;
                ++oi;
            } else if ((operand & 0xE000u) == 0xA000u) {
                num_regs = (operand & 0x0400u) ? 2 : 1;
            }
        } else {
            num_regs = 3;
        }
        if (num_regs > 0) {
            for (int i = 0; i < num_regs; ++i) {
                int shifter = ((num_regs - i) - 1) * 5;
                uint32_t regdi = (operand >> shifter) & 0x1Fu;
                int32_t addr = (int32_t)dspp_translate_reg(s, regdi & 0xFu);
                if (regdi & 0x0010u) addr = dspp_mem_read(s, (uint32_t)addr);
                // dspp_instructions.h: in a two-register operand word,
                // field 1 carries DSPN_REG2_F_WRITE_BACK (0x1000) and field
                // 0 carries DSPN_REG1_F_WRITE_BACK (0x0800). The fields are
                // decoded high-to-low here, so i=0 tests REG2 and i=1 REG1.
                if (num_regs < 3 && i == 0 && (operand & 0x1000u))
                    s->write_back = addr;
                else if (num_regs < 3 && i == 1 &&
                         (operand & DSPN_ADDR_F_WRITE_BACK))
                    s->write_back = addr;
                s->ops[oi].addr = addr;
                ++oi;
            }
            num_regs = 0;
        }
    }
    s->ops_in_use = oi < DSPP_MAX_OPERANDS ? oi : DSPP_MAX_OPERANDS;
}

static int32_t dspp_read_next(dspp_sim *s) {
    dspp_operand *op = &s->ops[s->op_index++];
    if (op->value < 0) return dspp_mem_read(s, (uint32_t)op->addr);
    return op->value;
}

static void dspp_write_next(dspp_sim *s, int32_t value) {
    int32_t addr = s->ops[s->op_index++].addr;
    if (addr < 0) return; // M2 logs an error; keep bounded
    dspp_mem_write(s, (uint32_t)addr, (uint16_t)(value & 0xFFFFu));
}

static void dspp_exec_arith(dspp_sim *s) {
    uint16_t cmd = s->command;
    int num_ops = (cmd >> 13) & 3;
    int mult_select = (cmd >> 12) & 1;
    int mux_a = (cmd >> 10) & 3;
    int mux_b = (cmd >> 8) & 3;
    int alu = (cmd >> 4) & 0xF;
    int barrel = cmd & 0xF;

    if (num_ops == 0 && (mux_a == 1 || mux_a == 2 || mux_b == 1 || mux_b == 2))
        num_ops = 4;
    if (barrel == 8) ++num_ops;

    s->write_back = -1;
    dspp_parse_operands(s, num_ops);

    int32_t mult_result = 0;
    if (mux_a == 3 || mux_b == 3) {
        int32_t mop1 = dspp_sign16((uint16_t)dspp_read_next(s));
        int32_t mop2 = mult_select
                           ? dspp_sign16((uint16_t)dspp_read_next(s))
                           : dspp_sign16((uint16_t)(s->accum >> 4));
        mult_result = (mop1 * mop2) >> 11;
    }

    int32_t op_a = 0, op_b = 0;
    switch (mux_a) {
    case 0: op_a = s->accum; break;
    case 1:
    case 2: op_a = dspp_read_next(s) << 4; break;
    default: op_a = mult_result; break;
    }
    op_a &= 0xFFFFF;
    switch (mux_b) {
    case 0: op_b = s->accum; break;
    case 1:
    case 2: op_b = dspp_read_next(s) << 4; break;
    default: op_b = mult_result; break;
    }
    op_b &= 0xFFFFF;

    const int32_t carry_bit = 0x00100000;
    int32_t result = 0;
    switch (alu) {
    case 0:
        result = op_a;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 1:
        result = -op_b;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 2:
        result = op_a + op_b;
        s->cc_over = (((op_a & 0x80000) == (op_b & 0x80000)) &&
                      ((op_a & 0x80000) != (result & 0x80000)))
                         ? 1
                         : 0;
        s->cc_carry = ((result & carry_bit) != 0) ? 1 : 0;
        break;
    case 3:
        result = op_a + (s->cc_carry << 4);
        s->cc_over = 0;
        s->cc_carry = ((result & carry_bit) != 0) ? 1 : 0;
        break;
    case 4:
        result = op_a - op_b;
        s->cc_over = (((op_a & 0x80000) == ((~op_b) & 0x80000)) &&
                      ((op_a & 0x80000) != (result & 0x80000)))
                         ? 1
                         : 0;
        s->cc_carry = ((result & carry_bit) != 0) ? 1 : 0;
        break;
    case 5:
        result = op_a - (s->cc_carry << 4);
        s->cc_over = 0;
        s->cc_carry = ((result & carry_bit) != 0) ? 1 : 0;
        break;
    case 6:
        result = op_a + 1;
        s->cc_over = (!(op_a & 0x80000) && (result & 0x80000)) ? 1 : 0;
        s->cc_carry = 0;
        break;
    case 7:
        result = op_a - 1;
        s->cc_over = ((op_a & 0x80000) && !(result & 0x80000)) ? 1 : 0;
        s->cc_carry = 0;
        break;
    case 8:
        result = op_a;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 9:
        result = ~op_a;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 10:
        result = op_a & op_b;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 11:
        result = ~(op_a & op_b);
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 12:
        result = op_a | op_b;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 13:
        result = ~(op_a | op_b);
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    case 14:
        result = op_a ^ op_b;
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    default:
        result = ~(op_a ^ op_b);
        s->cc_over = 0;
        s->cc_carry = 0;
        break;
    }

    s->cc_neg = ((result & 0x00080000) != 0) ? 1 : 0;
    s->cc_zero = ((result & 0x000FFFF0) == 0) ? 1 : 0;
    s->cc_exact = ((result & 0x0000000F) == 0) ? 1 : 0;

    // Barrel shifter pass.
    static const int shift_table[8] = {0, 1, 2, 3, 4, 5, 8, 16};
    if (barrel == 8) barrel = dspp_read_next(s);
    if (barrel & 0x8) {
        int shift = shift_table[((~barrel) + 1) & 0x7];
        if (alu < 8)
            s->accum = dspp_sign20(result) >> shift; // arithmetic
        else
            s->accum = (result & 0xFFFFF) >> shift; // logical
    } else {
        int shift = shift_table[barrel];
        if (shift == 16) {
            if (s->cc_over)
                s->accum = s->cc_neg ? 0x7FFFF : 0xFFF80000;
            else
                s->accum = dspp_sign20(result);
        } else {
            s->accum = dspp_sign20(result) << shift;
        }
    }

    if (s->write_back >= 0) {
        dspp_mem_write(s, (uint32_t)s->write_back,
                       (uint16_t)((s->accum >> 4) & 0xFFFFu));
        s->write_back = -1;
    } else if (s->op_index < num_ops) {
        dspp_write_next(s, s->accum >> 4);
    }
}

static void dspp_exec_control(dspp_sim *s) {
    uint16_t cmd = s->command;
    int mode = (cmd >> 13) & 3;
    int select = (cmd >> 12) & 1;
    int mask = (cmd >> 10) & 3;
    uint32_t target = cmd & 0x3FFu;

    if (mode == 0) {
        switch ((cmd >> 10) & 7) {
        case 0: { // super-special
            int sel = (cmd >> 7) & 7;
            switch (sel) {
            case 1: // BAC
                s->pc = ((uint32_t)s->accum >> 4) & 0x3FFu;
                break;
            case 4: // RTS
                if (s->stack_ptr > 0) s->pc = (uint32_t)s->stack[--s->stack_ptr];
                break;
            case 7: // SLEEP
                if (s->pc > 0u) --s->pc;
                s->sleeping = 1;
                break;
            default: // NOP / unused
                break;
            }
            break;
        }
        case 1: // JUMP
            s->pc = target;
            break;
        case 2: // JSR
            if (s->stack_ptr < DSPP_STACK_DEPTH)
                s->stack[s->stack_ptr++] = (int32_t)s->pc;
            s->pc = target;
            break;
        case 3: // BFM unimplemented
            break;
        case 4: { // MOVEREG
            uint32_t regdi = cmd & 0x1Fu;
            uint32_t dst = dspp_translate_reg(s, regdi & 0xFu);
            if (regdi & 0x0010u) dst = dspp_mem_read(s, dst);
            dspp_parse_operands(s, 1);
            int32_t val = dspp_read_next(s);
            dspp_mem_write(s, dst, (uint16_t)(val & 0xFFFFu));
            break;
        }
        case 5: { // RBASE
            uint32_t which = (cmd & 3u) << 2;
            uint32_t reg_addr = cmd & 0x3FCu;
            dspp_set_rbase(s, which, reg_addr);
            break;
        }
        case 6: { // MOVE direct
            uint32_t dst = cmd & 0x3FFu;
            dspp_parse_operands(s, 1);
            int32_t val = dspp_read_next(s);
            dspp_mem_write(s, dst, (uint16_t)(val & 0xFFFFu));
            break;
        }
        case 7: { // MOVE indirect
            uint32_t ptr = cmd & 0x3FFu;
            uint32_t dst = dspp_mem_read(s, ptr);
            dspp_parse_operands(s, 1);
            int32_t val = dspp_read_next(s);
            dspp_mem_write(s, dst, (uint16_t)(val & 0xFFFFu));
            break;
        }
        default:
            break;
        }
    } else if (mode == 1 || mode == 2) {
        int flag0, flag1;
        if (select == 0) {
            flag0 = s->cc_neg;
            flag1 = s->cc_over;
        } else {
            flag0 = s->cc_carry;
            flag1 = s->cc_zero;
        }
        int mask0 = (mask & 2) != 0;
        int mask1 = (mask & 1) != 0;
        int test = (flag0 || !mask0) && (flag1 || !mask1);
        if (mode == 2) test = !test;
        if (test) s->pc = target;
    } else { // mode 3: complex branches
        int type = (select << 2) | mask;
        int test = 0;
        switch (type) {
        case 0: // BLT
            test = (s->cc_neg && !s->cc_over) || (!s->cc_neg && s->cc_over);
            break;
        case 1: // BLE
            test = (s->cc_neg && !s->cc_over) || (!s->cc_neg && s->cc_over) ||
                   s->cc_zero;
            break;
        case 2: // BGE
            test = (s->cc_neg && s->cc_over) || (!s->cc_neg && !s->cc_over);
            break;
        case 3: // BGT
            test = ((s->cc_neg && s->cc_over) ||
                    (!s->cc_neg && !s->cc_over)) &&
                   !s->cc_zero;
            break;
        case 4: // BHI
            test = s->cc_carry && !s->cc_zero;
            break;
        case 5: // BLS
            test = !s->cc_carry || s->cc_zero;
            break;
        case 6: // BXS
            test = s->cc_exact;
            break;
        default: // BXC
            test = !s->cc_exact;
            break;
        }
        if (test) s->pc = target;
    }
}

// Direct data-memory access for DINI initializers and bounded diagnostics.
void dspp_sim_poke_data(dspp_sim *s, uint32_t addr, uint16_t value) {
    if (!s || addr >= DSPI_DATA_MEMORY_SIZE) return;
    dspp_data_mem(s)[addr] = value;
}

uint16_t dspp_sim_peek_data(const dspp_sim *s, uint32_t addr) {
    if (!s || addr >= DSPI_DATA_MEMORY_SIZE) return 0;
    return dspp_data_mem_const(s)[addr];
}

void dspp_sim_reset_cycle(dspp_sim *s) {
    if (!s) return;
    s->pc = s->code_base;
    s->sleeping = 0;
    s->stack_ptr = 0;
    s->rbase[0] = 0;
    s->rbase[1] = 4;
    s->rbase[2] = 8;
    s->rbase[3] = 12;
}

// Bounded, env-gated execution trace (M2_DSPP_EXEC_TRACE=1): off by default,
// capped so a hot loop cannot spam the log.
static int dspp_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("M2_DSPP_EXEC_TRACE");
        enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return enabled;
}

// The host has no shared MOVE code block at the top of DSP code memory
// (DSPP_CON_MOVE_INDEX_TO_NMEM in dspp_instr.c); the recorded copies are the
// block. Each copy is one MOVE-direct word pair (M2 dspp_simulator.c
// case 6: DstAddr = command & 0x3FF, value = next address operand), routed
// through the same data/register accessors the interpreter uses so DSPI
// register sources read live values.
static void dspp_pump_moves(dspp_sim *s) {
    for (uint32_t i = 0; i < s->move_count && i < DSPP_MAX_MOVES; ++i)
        dspp_mem_write(s, s->move_dst[i], dspp_mem_read(s, s->move_src[i]));
}

void dspp_sim_tick(dspp_sim *s) {
    if (!s) return;
    // dsppConnectWithMove block, invoked from the nanokernel header every
    // batch on hardware (dsppCallConnectionMoves repoints the imported
    // CallConnectionMoves JSR at the MOVE block; dspp_instr.c): the recorded
    // copies run each frame ahead of the instrument code, whether or not
    // this pass executes.
    dspp_pump_moves(s);
    if (!s->gwilling) return;
    // One cyclic pass per audio frame: restart at the entry point every
    // frame (the SLEEP wake restarts the program; only data memory, FIFO
    // state, and inputs/outputs persist across frames). Without the restart
    // the second tick would re-fetch the SLEEP word itself and freeze.
    dspp_sim_reset_cycle(s);
    // One cyclic pass: execute until the instrument SLEEPs (AudLock wake).
    uint32_t *fdma_check_pending = dspp_dma_check_pending_ptr(s);
    int steps = 0;
    while (!s->sleeping) {
        // dspsRunSimulator services one highest-priority FDMA channel before
        // each DSP instruction. The host-buffer adapter is idle unless a
        // caller has installed a descriptor through the public DMA boundary.
        // A check that found nothing to service finds nothing again until
        // channel state or a FIFO depth changes, so the walk over the active
        // channels only reruns after such a write.
        if (*fdma_check_pending && dspp_run_fdma(s) < 0)
            *fdma_check_pending = 0u;
        const int trace = dspp_trace_enabled() && steps < 256;
        if (trace)
            fprintf(stderr,
                    "[dspp] pc=%03u cmd=%04X acc=%06X C%dZ%dN%dV%dX%d wb=%d\n",
                    s->pc, dspp_fetch_code(s, s->pc), s->accum & 0xFFFFFu, s->cc_carry,
                    s->cc_zero, s->cc_neg, s->cc_over, s->cc_exact,
                    s->write_back);
        if (++steps > 4096) {
            // The DSPP is a cyclic frame program; a bounded pass that never
            // SLEEPs means the code image or relocation is wrong. Abort with
            // identity instead of hanging the audio path.
            fprintf(stderr,
                    "[dspp] ERROR: no SLEEP within 4096 steps "
                    "(pc=%03u cmd=%04X)\n",
                    s->pc, dspp_fetch_code(s, s->pc));
            s->sleeping = 1;
            break;
        }
        s->write_back = -1;
        s->command = dspp_fetch_code(s, s->pc);
        s->pc = (s->pc + 1u) & 0x3FFu;
        if (s->command & 0x8000u)
            dspp_exec_control(s);
        else
            dspp_exec_arith(s);
    }
    s->sleeping = 0; // wake for the next audio frame
}

void dspp_sim_tick_n(dspp_sim *s, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) dspp_sim_tick(s);
}

// Shared site walker for connect/disconnect: links come from the pristine
// template image while s->code receives the patches — M2 dsppRelocate
// reads links from dtmp->dtmp_Codes and fixes up the instrument copy, so
// loader relocation can never consume the chain out from under later
// connections. Only sites whose relocation part equals part are patched,
// matching the M2's (ri == DstIndex && DstPart == drlc_Part) match.
static void dspp_patch_sites(dspp_sim *s, int rsrc, uint32_t part,
                             uint16_t value, uint16_t field_mask) {
    dspp_template *t = s->tpl_resources;
    for (uint32_t i = 0; i < t->relocation_count; ++i) {
        const dspp_relocation *rel = &t->relocations[i];
        if (rel->resource_index != (uint16_t)rsrc) continue;
        if (rel->part != part) continue;
        if (rel->code_hunk != 0 || rel->code_offset >= t->code_word_count ||
            rel->code_offset >= s->pristine_count)
            continue;
        uint32_t offset = rel->code_offset;
        for (uint32_t guard = 0; guard <= t->code_word_count; ++guard) {
            uint16_t next = (uint16_t)(s->pristine[offset] & 0x03FFu);
            s->code[offset] =
                (uint16_t)((s->code[offset] & ~field_mask) |
                           (value & field_mask));
            if (next == 0) break;
            offset = (uint32_t)(offset + next) % t->code_word_count;
            if (offset >= t->code_word_count || offset >= s->pristine_count)
                break;
        }
    }
}

// dsppConnectWithMove (dspp_instr.c): install the per-frame MOVE copy
// data[dst_addr] = data[src_addr] for a BIND-to-RBASE destination. The M2
// writes two code words (DSPN_OPCODE_MOVEADDR | DstAddress, then
// DSPN_OPERAND_ADDR | SrcAddress) into the shared MOVE block at
// DSPP_CON_MOVE_INDEX_TO_NMEM and repoints the header CallConnectionMoves
// JSR; the host has no shared code space, so the copy is recorded on the
// destination sim and applied at each tick entry instead. Addresses are
// masked to the hardware 10-bit MOVE-direct decode. Re-connecting the same
// destination word replaces the record (the guest folio rejects duplicate
// destination ports before this layer); records past the bound drop without
// fabrication. DstAddr is dsppGetResourceAttribute(dst, part) under the
// dsppBindResource default (base alloc + bind offset), already resolved into
// alloc_value by the allocator; SrcAddr is dsppGetResourceAttribute of the
// source OUTPUT part (dspp_resources_bulldog.c default: alloc + part).
void dspp_sim_connect_move(dspp_sim *dst, uint32_t dst_addr,
                           uint32_t src_addr) {
    if (!dst) return;
    uint16_t d = (uint16_t)(dst_addr & DSPN_ADDR_MASK);
    uint16_t sr = (uint16_t)(src_addr & DSPN_ADDR_MASK);
    for (uint32_t i = 0; i < dst->move_count && i < DSPP_MAX_MOVES; ++i) {
        if (dst->move_dst[i] == d) {
            dst->move_src[i] = sr;
            return;
        }
    }
    if (dst->move_count >= DSPP_MAX_MOVES) return;
    dst->move_dst[dst->move_count] = d;
    dst->move_src[dst->move_count] = sr;
    ++dst->move_count;
}

// dsppConnectInstrumentParts (dspp_instr.c): patch the destination INPUT/KNOB
// operand(s) with the full source address operand. The template loader's
// default INPUT value is the M2's unconnected-immediate-0; a connection
// overwrites the relocation sites for (resource, part) with the source's
// DSPN_OPERAND_ADDR | SrcAddr.
void dspp_sim_connect(dspp_sim *s, const char *input_name, uint32_t dst_part,
                      uint16_t src_addr_operand) {
    if (!s || !input_name || !s->tpl_resources) return;
    dspp_template *t = s->tpl_resources;
    int rsrc = -1;
    uint8_t rtype = 0;
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        if (strncmp(t->resources[i].name, input_name, DSPP_MAX_NAME) == 0) {
            rsrc = (int)i;
            rtype = t->resources[i].type;
            break;
        }
    }
    // Only INPUT (3) and KNOB (1) destinations take full-operand patches.
    if (rsrc < 0) return;
    if (rtype != 3u && rtype != 1u) return;
    // BIND-to-RBASE destinations take a MOVE instruction on hardware, not a
    // relocation patch (dsppConnectInstrumentParts takes the dsppConnectWithMove
    // arm; the relocation scan runs only in the else arm). No destination
    // code word is touched — on-disc BIND KNOBs (e.g. envelope.dsp
    // Amplitude/Env.request/Env.incr) carry no relocation sites at all; the
    // code reaches them through the RBASE window. Record the per-frame copy
    // instead.
    const dspp_resource *br = &t->resources[rsrc];
    if ((br->flags & DRSC_F_BIND) && br->bind_index < t->resource_count &&
        t->resources[br->bind_index].type == DRSC_TYPE_RBASE) {
        // M2-validated shape (dspp_instr.c): DstPart < Many; SrcAddress
        // is the low 10 bits of the full source address operand.
        if (dst_part >= br->many || br->alloc_value == 0xFFFFFFFFu) return;
        dspp_sim_connect_move(s, br->alloc_value + dst_part,
                              (uint32_t)(src_addr_operand & DSPN_ADDR_MASK));
        return;
    }
    dspp_patch_sites(s, rsrc, dst_part, src_addr_operand, 0xFFFFu);
}

// dsppDisconnectInstrumentParts (dspp_instr.c): restore the relocation
// default (dsppGetResourceRelocationValue) at the (resource, part) sites.
// Other connections' patches are undisturbed; data memory (knob pokes,
// filter state) is untouched.
void dspp_sim_disconnect(dspp_sim *s, const char *input_name,
                         uint32_t dst_part) {
    if (!s || !input_name || !s->tpl_resources) return;
    dspp_template *t = s->tpl_resources;
    int rsrc = -1;
    uint8_t rtype = 0;
    for (uint32_t i = 0; i < t->resource_count; ++i) {
        if (strncmp(t->resources[i].name, input_name, DSPP_MAX_NAME) == 0) {
            rsrc = (int)i;
            rtype = t->resources[i].type;
            break;
        }
    }
    if (rsrc < 0) return;
    if (rtype != 3u && rtype != 1u) return;
    // MOVE-connected destinations drop the recorded copy only
    // (dsppDisconnectInstrumentParts returns right after
    // dsppDisconnectWithMove): destination code is never re-relocated and
    // data memory keeps its last MOVEd value.
    const dspp_resource *br = &t->resources[rsrc];
    if ((br->flags & DRSC_F_BIND) && br->bind_index < t->resource_count &&
        t->resources[br->bind_index].type == DRSC_TYPE_RBASE) {
        if (dst_part >= br->many || br->alloc_value == 0xFFFFFFFFu) return;
        uint16_t d = (uint16_t)((br->alloc_value + dst_part) & DSPN_ADDR_MASK);
        for (uint32_t i = 0; i < s->move_count && i < DSPP_MAX_MOVES; ++i) {
            if (s->move_dst[i] == d) {
                uint32_t last = s->move_count - 1u;
                s->move_dst[i] = s->move_dst[last];
                s->move_src[i] = s->move_src[last];
                s->move_count = last;
                break;
            }
        }
        return;
    }
    const dspp_resource *r = &t->resources[rsrc];
    if (r->alloc_value == 0xFFFFFFFFu) return;
    dspp_patch_sites(s, rsrc, dst_part, dspp_relocation_value(r, dst_part),
                     0xFFFFu);
}

// ---- Numeric (non-sim) DSPP DMA descriptor attach -----------------------------
// M2 behavior: dspsAccessDMA_Stack stages numeric address/count in shadows
// and commits via the control word (out-of-range channel is ILLEGAL_ADDRESS);
// DSPX_DMA_ADDRESS_MASK ((1<<25)-1, audio_attachments.c) bounds the physical
// address; SetDMANextInt rejects NULL addresses and DSPPStartSampleAttachment
// returns AF_ERR_NULLADDRESS on NULL sample data (dspp_instr.c); folio attach
// rejects illegal flags with AF_ERR_BADTAGVAL, StartAt/range violations with
// AF_ERR_BADTAGVAL/OUTOFRANGE, ordinary-sample-to-OUT_FIFO with AF_ERR_SECURITY,
// non-FIFO hooks with AF_ERR_BAD_PORT_TYPE, and unknown hooks with AF_ERR_BAD_NAME
// (audio_attachments.c); DRSC_INFIFO_SUBTYPE_SQS2 marks 8-bit compressed input
// (dspp_template.h) and the input service decodes SQXD only when width8.
// This layer validates numeric shape only (channel/address/count/flag
// coherence); FIFO IN/OUT mistype, hook lookup, and busy live at the folio
// attach seam (see report's proposed runtime hunk). On success the validated
// descriptor is recorded on the addressed channel resource (shared across
// sims in a dspp_sim_space via dspp_dma_state_ptr, like all DMA state).
// Live current_*/host buffers, FIFO words, and interrupt banks are untouched,
// so a numeric descriptor alone never moves samples, raises signals, or
// completes: service still requires a host buffer via dspp_sim_dma_set_current
// /next. Re-attaching a channel replaces its record.
int dspp_sim_dma_attach_numeric(dspp_sim *s, uint32_t channel,
                                uint32_t address, uint32_t count,
                                uint32_t flags) {
    if (!s) return DSPP_NUMERIC_ERR_BAD_ITEM;
    if (channel >= DSPI_NUM_DMA_CHANNELS)
        return DSPP_NUMERIC_ERR_OUT_OF_RANGE;
    if (flags & ~(DSPP_SIM_DMA_TO_RAM | DSPP_SIM_DMA_8BIT | DSPP_SIM_DMA_SQXD))
        return DSPP_NUMERIC_ERR_BAD_TYPE;
    if ((flags & DSPP_SIM_DMA_SQXD) && !(flags & DSPP_SIM_DMA_8BIT))
        return DSPP_NUMERIC_ERR_BAD_TYPE;
    if ((address & ~DSPP_NUMERIC_ADDR_MASK) != 0u)
        return DSPP_NUMERIC_ERR_OUT_OF_RANGE;
    if (count != 0u && address == 0u)
        return DSPP_NUMERIC_ERR_NULL_ADDRESS;
    dspp_dma_channel *dma = dspp_dma_state_ptr(s, channel);
    if (dma->delay_line)
        return DSPP_NUMERIC_ERR_BAD_TYPE;
    dma->numeric_address = address;
    dma->numeric_count = count;
    dma->numeric_flags = flags;
    dma->numeric_attached = 1;
    // A fresh bind starts a fresh generation; failed attaches above return
    // early and leave the prior generation undisturbed.
    dma->numeric_consumed = 0u;
    // A fresh bind starts a fresh completion lifecycle: release the
    // once-per-bind latch and dequeue any still-queued event for this
    // channel so the new bound can complete anew. Failed attaches above
    // return early and leave both undisturbed.
    dma->numeric_completion_done = 0;
    if (dma->numeric_completion_pending) {
        dma->numeric_completion_pending = 0;
        dspp_completion_remove_locked(s, channel);
    }
    return DSPP_NUMERIC_OK;
}
int dspp_sim_dma_numeric_attached(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    return dspp_dma_state_const_ptr(s, channel)->numeric_attached ? 1 : 0;
}
int dspp_sim_dma_numeric_descriptor(const dspp_sim *s, uint32_t channel,
                                    uint32_t *address_out, uint32_t *count_out,
                                    uint32_t *flags_out) {
    if (!s) return DSPP_NUMERIC_ERR_BAD_ITEM;
    if (channel >= DSPI_NUM_DMA_CHANNELS)
        return DSPP_NUMERIC_ERR_OUT_OF_RANGE;
    const dspp_dma_channel *dma = dspp_dma_state_const_ptr(s, channel);
    if (address_out) *address_out = dma->numeric_attached ? dma->numeric_address : 0u;
    if (count_out) *count_out = dma->numeric_attached ? dma->numeric_count : 0u;
    if (flags_out) *flags_out = dma->numeric_attached ? dma->numeric_flags : 0u;
    return DSPP_NUMERIC_OK;
}
uint32_t dspp_sim_dma_numeric_consumed(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0u;
    const dspp_dma_channel *dma = dspp_dma_state_const_ptr(s, channel);
    if (!dma->numeric_attached) return 0u;
    return dma->numeric_consumed;
}

// ---- Completion-qualified event poll (bounded, one FIFO per space) ------------
// M2 behavior: audio_attachments.c ProcessAttachmentDMA signals
// the monitored Cue exactly when SegmentCount reaches 0; the folio
// MonitorAttachment pump (gk_pump_attachment_completions) delivers host
// completion events to the owning guest task at frame boundaries. This poll
// family exposes the raw-DSPP completion edge recorded above without raising
// anything: the folio seam owns delivery (see proposed runtime hunk), and
// unattached / count-0 / never-completed channels read empty.
int dspp_sim_dma_completion_pending(const dspp_sim *s, uint32_t channel) {
    if (!s || channel >= DSPI_NUM_DMA_CHANNELS) return 0;
    const dspp_dma_channel *dma = dspp_dma_state_const_ptr(s, channel);
    return (dma->numeric_completion_pending ||
            dma->delay_completion_pending) ? 1 : 0;
}
uint32_t dspp_sim_dma_completion_queued(const dspp_sim *s) {
    if (!s) return 0u;
    return dspp_completion_count_value(s);
}
int dspp_sim_dma_poll_completion(dspp_sim *s, uint32_t *channel_out) {
    uint8_t *q;
    uint32_t *head;
    uint32_t *count;
    uint32_t channel;
    dspp_dma_channel *dma;
    if (channel_out) *channel_out = 0u;
    if (!s) return 0;
    q = dspp_completion_queue_ptr(s);
    head = dspp_completion_head_ptr(s);
    count = dspp_completion_count_ptr(s);
    if (!q || !head || !count || *count == 0u) return 0;
    channel = q[*head];
    *head = (*head + 1u) % DSPI_NUM_DMA_CHANNELS;
    --*count;
    if (channel < DSPI_NUM_DMA_CHANNELS) {
        // Release the queued flag; the done latch is retained so the same
        // bind never re-queues on post-poll over-drains. Only a successful
        // re-attach releases the latch for the new bound. Numeric and
        // physical delay-line events share this channel queue but retain
        // separate pending/latch fields.
        dma = dspp_dma_state_ptr(s, channel);
        if (dma) {
            dma->numeric_completion_pending = 0u;
            dma->delay_completion_pending = 0u;
        }
    }
    if (channel_out) *channel_out = channel;
    return 1;
}
