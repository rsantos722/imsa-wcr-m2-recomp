// trace_stubs.c — weak no-op definitions for discovery harness C ABI.
//
// When m2recomp is linked without the host trace library (standalone tests
// like m2_savestate_test, m2recomp_smoke), these weak definitions satisfy the
// recomp_call_selector / RECOMP_INDIRECT references. When m2host links with
// host/diagnostics/trace.cpp, the host's strong definitions override these weak
// ones (Mach-O and ELF both prefer strong over weak in the final link).
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "ppc_ctx.h"
#include "guest_kernel.h"
#include "selector_observed.h"
#include "shim_abi.h"

__attribute__((weak)) void m2_trace_selector_call_c(uint32_t folio, uint32_t ordinal, uint32_t pc, uint32_t gpr3, uint32_t gpr4) { (void)folio;(void)ordinal;(void)pc;(void)gpr3;(void)gpr4; }
__attribute__((weak)) void m2_trace_selector_return_c(uint32_t folio, uint32_t ordinal, uint32_t pc, uint32_t ret) { (void)folio;(void)ordinal;(void)pc;(void)ret; }
__attribute__((weak)) void m2_trace_guest_enter_c(uint32_t pc, const char* sym) { (void)pc;(void)sym; }
__attribute__((weak)) void m2_trace_guest_exit_c(uint32_t pc, const char* sym, uint32_t ret) { (void)pc;(void)sym;(void)ret; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc) { (void)ea;(void)size;(void)pc; }
__attribute__((weak)) void m2_trace_indirect_target_c(uint32_t ctr, uint32_t target, uint32_t pc, int resolved) { (void)ctr;(void)target;(void)pc;(void)resolved; }
__attribute__((weak)) void m2_trace_unknown_behavior_c(const char* desc, uint32_t pc) { (void)desc;(void)pc; }

// The race powf observation is diagnosable only if the original operands are
// captured at the helper boundary.  Keep this default implementation in the
// weak trace TU so it works for both m2host and standalone tests without
// adding a host-game implementation.  It is bounded, opt-in, and observational.
static _Thread_local uint32_t g_powf_x_bits;
static _Thread_local uint32_t g_powf_y_bits;
static _Thread_local uint32_t g_powf_entry_lr;
static _Thread_local int g_powf_pending;
static atomic_uint g_powf_trace_count;
static int g_powf_trace_enabled = -1;

static int m2_powf_trace_is_enabled(void) {
    if (g_powf_trace_enabled < 0) {
        const char* value = getenv("M2_POWF_TRACE");
        g_powf_trace_enabled = value &&
            (value[0] == '1' || value[0] == 'y' || value[0] == 'Y');
    }
    return g_powf_trace_enabled;
}

static uint32_t m2_powf_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float m2_powf_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

__attribute__((weak)) void m2_trace_powf_enter_c(PPCContext* c) {
    if (!m2_powf_trace_is_enabled() || !c) return;
    g_powf_x_bits = m2_powf_bits(c->fpr[1]);
    g_powf_y_bits = m2_powf_bits(c->fpr[2]);
    g_powf_entry_lr = c->lr;
    g_powf_pending = 1;
}

__attribute__((weak)) void m2_trace_powf_error_c(PPCContext* c) {
    if (!m2_powf_trace_is_enabled() || !c || !g_powf_pending) return;
    // pow_errf is a separately exported helper.  Pair only its three static
    // calls from powf; this avoids attaching a stale prior powf entry to a
    // direct helper invocation in a focused ABI test or another guest caller.
    if (c->lr != 0x728bcu && c->lr != 0x72a38u && c->lr != 0x72a68u) {
        g_powf_pending = 0;
        return;
    }
    const unsigned n = atomic_fetch_add_explicit(&g_powf_trace_count, 1u,
                                                  memory_order_relaxed);
    if (n < 16u) {
        fprintf(stderr,
                "[powf-trace] n=%u x_bits=%08x x=% .9g y_bits=%08x y=% .9g "
                "error_code=%u error_value_bits=%08x error_value=% .9g entry_lr=%#x "
                "error_lr=%#x error_pc=%#x\n",
                n + 1u, g_powf_x_bits, (double)m2_powf_float(g_powf_x_bits),
                g_powf_y_bits, (double)m2_powf_float(g_powf_y_bits),
                c->gpr[3],
                m2_powf_bits(c->fpr[1]), (double)c->fpr[1],
                g_powf_entry_lr, c->lr, c->pc);
        if (n == 15u)
            fprintf(stderr, "[powf-trace] further records suppressed\n");
    }
    g_powf_pending = 0;
}
__attribute__((weak)) void m2_trace_frame_complete_c(uint32_t seq, uint32_t pc, uint32_t state) { (void)seq;(void)pc;(void)state; }
__attribute__((weak)) void m2_trace_state_transition_c(uint32_t from_state, uint32_t to_state, uint32_t state_ea, uint32_t pc) { (void)from_state;(void)to_state;(void)state_ea;(void)pc; }
__attribute__((weak)) void m2_perf_guest_frame_begin(uint32_t seq) { (void)seq; }
__attribute__((weak)) void m2_perf_guest_frame_end(uint32_t seq) { (void)seq; }
__attribute__((weak)) void m2_trace_init_file(const char* path) { (void)path; }
__attribute__((weak)) void m2_trace_shutdown(void) {}
__attribute__((weak)) void m2_tracepoint_event_c(const M2TracepointCapture* capture) { (void)capture; }
// Compatibility bridge for focused tests and standalone users that still
// override the pre-consolidation tracepoint callback. Production links the
// strong shared event collector in host/diagnostics/trace.cpp instead.
__attribute__((weak)) void m2_guest_event_c(const M2GuestEvent* event) {
    if (event && event->kind == M2_GUEST_EVENT_TRACEPOINT &&
        m2_tracepoint_event_c && event->tracepoint) {
        m2_tracepoint_event_c(event->tracepoint);
    }
}
// Production audio_shim.cpp overrides this with the number of 44.1 kHz
// output frames rendered by the SDL stream. Standalone recomp tests keep the
// source global clock at its initial zero until a host clock is present.
__attribute__((weak)) uint32_t m2_host_audio_frame_count(void) { return 0; }
__attribute__((weak)) int32_t m2_host_audio_start_cbd2_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t *bytes,
    uint32_t byte_count, float gain) {
    (void)stream_key;
    (void)attachment_id;
    (void)bytes;
    (void)byte_count;
    (void)gain;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_link_cbd2_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t *bytes, uint32_t byte_count) {
    (void)stream_key;
    (void)first_attachment;
    (void)next_attachment;
    (void)bytes;
    (void)byte_count;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_start_pcm16be_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t *bytes,
    uint32_t byte_count, float gain) {
    (void)stream_key;
    (void)attachment_id;
    (void)bytes;
    (void)byte_count;
    (void)gain;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_link_pcm16be_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t *bytes, uint32_t byte_count) {
    (void)stream_key;
    (void)first_attachment;
    (void)next_attachment;
    (void)bytes;
    (void)byte_count;
    return 0;
}
__attribute__((weak)) void m2_host_audio_unlink_cbd2_stream(
    uint32_t stream_key, uint32_t first_attachment) {
    (void)stream_key;
    (void)first_attachment;
}
__attribute__((weak)) void m2_host_audio_reset_cbd2_stream(uint32_t stream_key) {
    (void)stream_key;
}
__attribute__((weak)) int32_t m2_host_audio_pop_cbd2_completion(
    uint32_t *attachment_id) {
    (void)attachment_id;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_pop_attachment_completion(
    uint32_t *attachment_id) {
    (void)attachment_id;
    return 0;
}
__attribute__((weak)) uint32_t m2_host_audio_add_sample(const int16_t *pcm,
                                                         uint32_t frames) {
    (void)pcm;
    (void)frames;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_start_voice(uint32_t sample_id,
                                                         int looping) {
    (void)sample_id;
    (void)looping;
    return -1;
}
// The indexed voice helpers are used by the recovered instrument restart
// path.  Keep standalone recompilation tests linkable when the SDL-backed
// host audio shim is not part of the executable; focused audio tests and
// m2host provide strong implementations.
__attribute__((weak)) int32_t m2_host_audio_start_voice_indexed(
    uint32_t sample_id, int looping) {
    return m2_host_audio_start_voice(sample_id, looping);
}
__attribute__((weak)) int32_t
m2_host_audio_start_voice_indexed_for_attachment(uint32_t sample_id,
                                                 int looping,
                                                 uint32_t attachment_id) {
    (void)attachment_id;
    return m2_host_audio_start_voice_indexed(sample_id, looping);
}
__attribute__((weak)) int32_t
m2_host_audio_start_voice_indexed_for_attachment_with_loops(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end) {
    (void)attachment_id;
    (void)sustain_begin;
    (void)sustain_end;
    (void)release_begin;
    (void)release_end;
    return m2_host_audio_start_voice_indexed(sample_id, 0);
}
__attribute__((weak)) int32_t
m2_host_audio_start_voice_indexed_for_attachment_with_params(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end,
    float rate_hz, float gain) {
    (void)rate_hz;
    (void)gain;
    return m2_host_audio_start_voice_indexed_for_attachment_with_loops(
        sample_id, attachment_id, sustain_begin, sustain_end, release_begin,
        release_end);
}
__attribute__((weak)) void m2_host_audio_set_voice_params(
    uint32_t voice_index, float rate_hz, float gain) {
    (void)voice_index;
    (void)rate_hz;
    (void)gain;
}
__attribute__((weak)) int32_t
m2_host_audio_release_voice(uint32_t voice_index) {
    (void)voice_index;
    return -1;
}
__attribute__((weak)) int32_t
m2_host_audio_release_voice_for_attachment(uint32_t attachment_id) {
    (void)attachment_id;
    return -1;
}
__attribute__((weak)) uint32_t m2_host_audio_voice_sample(uint32_t voice_index) {
    (void)voice_index;
    return 0;
}
__attribute__((weak)) int32_t m2_host_audio_stop_voice(uint32_t voice_index) {
    (void)voice_index;
    return -1;
}
// DSPP push sink: the SDL-backed host audio shim overrides this with a
// bounded mixer FIFO; standalone recomp consumers drop without audio.
__attribute__((weak)) void m2_host_audio_push_dspp_frames(const int16_t *pcm,
                                                          uint32_t frames) {
    (void)pcm;
    (void)frames;
}

// Amplitude changes for a playing stream; only the SDL-backed shim has streams.
__attribute__((weak)) void m2_host_audio_set_stream_gain(uint32_t stream_key,
                                                         float gain) {
    (void)stream_key;
    (void)gain;
}

// Output-queue room for the asynchronous DSPP worker; only the SDL-backed shim
// runs that worker.
__attribute__((weak)) uint32_t m2_host_audio_dspp_fill_request(void) {
    return 0;
}

// The host audio shim optionally overrides this typed boundary. The shared
// fixed-width bridge is safe for standalone recomp consumers because it uses
// the same local Item/state contracts as the guest kernel pool; SDL, DSP
// resource loading, and playback remain host-owned.
static int m2_audio_dispatch_c_impl(PPCContext* c, uint32_t ordinal,
                                    uint32_t pc) {
    (void)pc;
    if (!c) return 0;
    if (ordinal == 8) {
        c->gpr[3] = (uint32_t)m2_guest_audio_create_mixer_template(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 27) {
        c->gpr[3] = (uint32_t)m2_guest_audio_load_instrument(
            c, c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 17) {
        c->gpr[3] = (uint32_t)m2_guest_audio_unload_instrument(c->gpr[3]);
        return 1;
    }
    if (ordinal == 32) {
        c->gpr[3] = (uint32_t)m2_guest_audio_load_ins_template(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 35) {
        c->gpr[3] = m2_guest_audio_get_audio_time(c);
        return 1;
    }
    if (ordinal == 37) {
        c->gpr[3] = m2_guest_audio_get_audio_frame_count(c);
        return 1;
    }
    if (ordinal == 34) {
        c->gpr[3] = (uint32_t)m2_guest_audio_arm_trigger_named(
            c, c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]);
        return 1;
    }
    if (ordinal == 23) {
        c->gpr[3] = m2_guest_audio_get_cue_signal(c, c->gpr[3]);
        return 1;
    }
    if (ordinal == 19) {
        c->gpr[3] = (uint32_t)m2_guest_audio_get_resource_info(
            c, c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 28) {
        c->gpr[3] = (uint32_t)m2_guest_audio_get_item_info(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == M2_AUDIO_SET_ITEM_INFO_ORDINAL) {
        c->gpr[3] = (uint32_t)m2_guest_audio_set_item_info(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 58) {
        c->gpr[3] = (uint32_t)m2_guest_audio_signal_at_audio_time(
            c, c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 24) {
        c->gpr[3] = (uint32_t)m2_guest_audio_sleep_until_audio_time(
            c, c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 52) {
        c->gpr[3] = (uint32_t)m2_guest_audio_stop_attachment(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 56) {
        c->gpr[3] = (uint32_t)m2_guest_audio_set_audio_clock_rate(
            c, c->gpr[3], c->fpr[1]);
        return 1;
    }
    if (ordinal == 71) {
        c->gpr[3] = (uint32_t)m2_guest_audio_read_audio_clock(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 38) {
        c->gpr[3] = (uint32_t)m2_guest_audio_read_probe_part(
            c, c->gpr[3], (int32_t)c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 41) {
        c->gpr[3] = (uint32_t)m2_guest_audio_abort_timer_cue(c, c->gpr[3]);
        return 1;
    }
    if (ordinal == 44) {
        c->gpr[3] = (uint32_t)m2_guest_audio_resume_instrument(c, c->gpr[3]);
        return 1;
    }
    if (ordinal == 45) {
        c->gpr[3] = (uint32_t)m2_guest_audio_pause_instrument(c, c->gpr[3]);
        return 1;
    }
    if (ordinal == 50) {
        c->gpr[3] = (uint32_t)m2_guest_audio_monitor_attachment(
            c, c->gpr[3], c->gpr[4], (int32_t)c->gpr[5]);
        return 1;
    }
    if (ordinal == 51) {
        c->gpr[3] = (uint32_t)m2_guest_audio_link_attachments(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 54) {
        c->gpr[3] = (uint32_t)m2_guest_audio_start_attachment(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 67) {
        c->gpr[3] = (uint32_t)m2_guest_audio_stop_instrument(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 68) {
        c->gpr[3] = (uint32_t)m2_guest_audio_release_instrument(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 57) {
        c->gpr[3] = (uint32_t)m2_guest_audio_connect_instrument_parts(
            c, c->gpr[3], c->gpr[4], (int32_t)c->gpr[5], c->gpr[6],
            c->gpr[7], (int32_t)c->gpr[8]);
        return 1;
    }
    if (ordinal == 59) {
        c->gpr[3] = (uint32_t)m2_guest_audio_disconnect_instrument_parts(
            c, c->gpr[3], c->gpr[4], (int32_t)c->gpr[5]);
        return 1;
    }
    if (ordinal == 60) {
        c->gpr[3] = (uint32_t)m2_guest_audio_read_knob_part(
            c, c->gpr[3], (int32_t)c->gpr[4], c->gpr[5]);
        return 1;
    }
    if (ordinal == 69) {
        c->gpr[3] = (uint32_t)m2_guest_audio_start_instrument(
            c, c->gpr[3], c->gpr[4]);
        return 1;
    }
    if (ordinal == 70) {
        c->gpr[3] = (uint32_t)m2_guest_audio_set_knob_part(
            c, c->gpr[3], (int32_t)c->gpr[4], c->fpr[1]);
        return 1;
    }
    return 0;
}

// Public audio boundary entry point. Notes the (folio 6, ordinal) dispatch in
// the observed record (selector_observed.h) whether the call arrives through
// recomp_call_selector or directly from a focused test.
__attribute__((weak)) int m2_audio_dispatch_c(PPCContext* c, uint32_t ordinal,
                                               uint32_t pc) {
    const int handled = m2_audio_dispatch_c_impl(c, ordinal, pc);
    m2_selector_note(M2_FOLIO_AUDIO, ordinal, handled);
    return handled;
}
