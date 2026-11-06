// audio_dispatch.h — the small audio boundary shared by the recomp and host.
//
// The recovered audio boundary is split by return contract. Typed status
// selectors retain only the bounded guest state proven at their call boundary;
// item selectors use the shared fixed-width guest item pool and carry no host
// pointers across the boundary.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// MKNODEID(AUDIONODE, AUDIO_*_NODE) from the M2 audio headers.
#define M2_AUDIO_TEMPLATE_CTYPE   0x401u
#define M2_AUDIO_INSTRUMENT_CTYPE 0x402u
#define M2_AUDIO_KNOB_CTYPE       0x403u
#define M2_AUDIO_SAMPLE_CTYPE     0x404u
#define M2_AUDIO_CUE_CTYPE        0x405u
#define M2_AUDIO_ENVELOPE_CTYPE   0x406u
#define M2_AUDIO_ATTACHMENT_CTYPE 0x407u
#define M2_AUDIO_PROBE_CTYPE      0x409u
#define M2_AUDIO_CLOCK_CTYPE     0x40au
#define M2_AUDIO_CUE_AT_END       (-2)
#define M2_AUDIO_SET_ITEM_INFO_ORDINAL 46u
#define M2_AUDIO_TAG_WIDTH        0x16u
#define M2_AUDIO_TAG_FRAMES       0x18u // AF_TAG_FRAMES
#define M2_AUDIO_TAG_BASENOTE     0x19u // AF_TAG_BASENOTE
#define M2_AUDIO_TAG_NUMBYTES     0x23u
#define M2_AUDIO_TAG_ADDRESS      0x24u
#define M2_AUDIO_TAG_NAME         0x0cu
#define M2_AUDIO_TAG_AUTO_DELETE_SLAVE 0x40u
#define M2_AUDIO_TAG_SUPPRESS_WRITE_BACK_D_CACHE 0x41u
#define M2_AUDIO_TAG_SET_FLAGS    0x28u
#define M2_AUDIO_TAG_CLEAR_FLAGS  0x29u
#define M2_AUDIO_TAG_START_AT     0x2du
#define M2_AUDIO_TAG_NOTESPEROCTAVE 0x32u // AF_TAG_NOTESPEROCTAVE
#define M2_AUDIO_TAG_SUSTAINTIME_FP 0x34u // AF_TAG_SUSTAINTIME_FP
#define M2_AUDIO_TAG_RELEASETIME_FP 0x35u // AF_TAG_RELEASETIME_FP
#define M2_AUDIO_TAG_RELEASEJUMP   0x3au // AF_TAG_RELEASEJUMP
#define M2_AUDIO_TAG_TYPE          0x4bu // AF_TAG_TYPE
#define M2_AUDIO_SAMPLE_FLAG_CLEAR_ON_STOP 0x0001u
#define M2_AUDIO_ENVELOPE_FLAG_LOCKTIMESCALE 0x0001u
#define M2_AUDIO_ENVELOPE_FLAG_FATLADYSINGS  0x0002u
#define M2_AUDIO_ENVELOPE_FLAGS_LEGAL \
    (M2_AUDIO_ENVELOPE_FLAG_LOCKTIMESCALE | \
     M2_AUDIO_ENVELOPE_FLAG_FATLADYSINGS)
// Serialized `.imsasound` sample tags (see docs/formats/imsasound.md and
// SampleInfoToTags in the on-disc aiff2imsasound tool). The CreateSample
// path presents these in the TagArg header; the host bridge retains and
// decodes the supported combinations and leaves unknown ones without audio.
#define M2_AUDIO_TAG_CHANNELS         0x17u
#define M2_AUDIO_TAG_SUSTAINBEGIN     0x1fu
#define M2_AUDIO_TAG_SUSTAINEND       0x20u
#define M2_AUDIO_TAG_RELEASEBEGIN     0x21u
#define M2_AUDIO_TAG_RELEASEEND       0x22u
#define M2_AUDIO_TAG_COMPRESSIONRATIO 0x2fu
#define M2_AUDIO_TAG_COMPRESSIONTYPE  0x30u
#define M2_AUDIO_TAG_NUMBITS          0x31u
#define M2_AUDIO_TAG_DELAY_LINE_TEMPLATE 0x38u
#define M2_AUDIO_TAG_DELAY_LINE       0x39u
// Portfolio Audio Folio tags from audio.h
#define M2_AUDIO_TAG_PITCH             0x0eu // AF_TAG_PITCH
#define M2_AUDIO_TAG_VELOCITY          0x0fu // AF_TAG_VELOCITY
#define M2_AUDIO_TAG_LOWNOTE           0x1bu // AF_TAG_LOWNOTE
#define M2_AUDIO_TAG_HIGHNOTE          0x1cu // AF_TAG_HIGHNOTE
#define M2_AUDIO_TAG_LOWVELOCITY       0x1du // AF_TAG_LOWVELOCITY
#define M2_AUDIO_TAG_HIGHVELOCITY      0x1eu // AF_TAG_HIGHVELOCITY
#define M2_AUDIO_TAG_SQUARE_VELOCITY   0x25u // AF_TAG_SQUARE_VELOCITY
#define M2_AUDIO_TAG_EXPONENTIAL_VELOCITY 0x26u // AF_TAG_EXPONENTIAL_VELOCITY
#define M2_AUDIO_TAG_EXP_VELOCITY_SCALAR  0x2bu // AF_TAG_EXP_VELOCITY_SCALAR
#define M2_AUDIO_TAG_TIME_SCALE_FP     0x3du // AF_TAG_TIME_SCALE_FP
#define M2_AUDIO_TAG_RATE_FP           0x4cu // AF_TAG_RATE_FP
#define M2_AUDIO_TAG_FREQUENCY_FP      0x4du // AF_TAG_FREQUENCY_FP
#define M2_AUDIO_TAG_AMPLITUDE_FP      0x4eu // AF_TAG_AMPLITUDE_FP
#define M2_AUDIO_TAG_START_0x4E        0x4eu // legacy alias for AF_TAG_AMPLITUDE_FP
#define M2_AUDIO_TAG_SAMPLE_RATE_FP    0x4fu // AF_TAG_SAMPLE_RATE_FP
#define M2_AUDIO_TAG_LEGACY_0x50       0x50u
#define M2_AUDIO_TAG_DETUNE_FP         0x51u // AF_TAG_DETUNE_FP
// Portfolio Audio Folio instrument-creation tags (audio.h enum values).
#define M2_AUDIO_TAG_TEMPLATE          0x10u
#define M2_AUDIO_TAG_PRIORITY          0x27u
#define M2_AUDIO_TAG_CALCRATE_DIVIDE   0x48u
#define M2_AUDIO_COMPRESSION_SQS2     0x53515332u // ASCII 'SQS2'
#define M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART 0x0001u
#define M2_AUDIO_ATTACHMENT_FLAG_FATLADYSINGS 0x0002u
#define M2_AUDIO_ATTACHMENT_FLAGS_LEGAL \
    (M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART | \
     M2_AUDIO_ATTACHMENT_FLAG_FATLADYSINGS)
#define M2_AUDIO_RESOURCE_TYPE_TICKS     0u
#define M2_AUDIO_RESOURCE_TYPE_CODE_MEM  1u
#define M2_AUDIO_RESOURCE_TYPE_DATA_MEM  2u
#define M2_AUDIO_RESOURCE_TYPE_FIFOS     3u
#define M2_AUDIO_RESOURCE_TYPE_TRIGGERS  4u
#define M2_AUDIO_RESOURCE_TYPE_COUNT     5u

// Source-backed Audio Folio errors used by the recovered connection/knob
// boundaries.
// These are MAKEAERR(ER_SEVERE, class, error) values from the original
// includes/audio/audio.h. Nonstandard audio errors include the class bit at
// 0x100. Keep them separate from the small negative errors used by the generic
// host kernel pool.
#define M2_AUDIO_ERR_BAD_ITEM     ((int32_t)0xd52bf001u) // AF_ERR_BADITEM
#define M2_AUDIO_ERR_BAD_TAG      ((int32_t)0xd52bf002u) // AF_ERR_BADTAG
#define M2_AUDIO_ERR_BAD_TAG_VALUE ((int32_t)0xd52bf003u) // AF_ERR_BADTAGVAL
#define M2_AUDIO_ERR_BAD_RESOURCE_TYPE ((int32_t)0xd52bf10bu) // AF_ERR_BADRSRCTYPE
#define M2_AUDIO_ERR_MISMATCH     ((int32_t)0xd52bf103u) // AF_ERR_MISMATCH
#define M2_AUDIO_ERR_BAD_PTR      ((int32_t)0xd52bf009u) // AF_ERR_BADPTR
#define M2_AUDIO_ERR_NAME_MISSING ((int32_t)0xd52bf105u) // AF_ERR_NAME_NOT_FOUND
#define M2_AUDIO_ERR_NO_MEMORY    ((int32_t)0xd52bf006u) // AF_ERR_NOMEM
#define M2_AUDIO_ERR_IN_USE       ((int32_t)0xd52bf10fu) // AF_ERR_INUSE
#define M2_AUDIO_ERR_UNIMPLEMENTED ((int32_t)0xd52bf010u) // AF_ERR_UNIMPLEMENTED
#define M2_AUDIO_ERR_OUT_OF_RANGE ((int32_t)0xd52bf117u) // AF_ERR_OUTOFRANGE
#define M2_AUDIO_ERR_SECURITY     ((int32_t)0xd52bf11bu) // AF_ERR_SECURITY
#define M2_AUDIO_ERR_TAGCONFLICT  ((int32_t)0xd52bf11fu) // AF_ERR_TAGCONFLICT
#define M2_AUDIO_ERR_BAD_SIGNAL_TYPE ((int32_t)0xd52bf11bu) // AF_ERR_BAD_SIGNAL_TYPE
// Audio-specific nonstandard error from the on-disc audio module:
// AF_ERR_EXTERNALREF = MAKEAERR(ER_SEVERE,ER_C_NSTND,AF_ERR_BASE+9)
// (audio.h:348). Returned when an instrument's imported DSP resource
// (e.g. gMixer from nanokernel.dsp) cannot be satisfied.
#define M2_AUDIO_ERR_EXTERNALREF  ((int32_t)0xd52bf10au)
#define M2_AUDIO_ERR_NOT_OWNER    ((int32_t)0xd52bf012u) // AF_ERR_NOTOWNER

static inline bool m2_is_audio_status_selector(uint32_t o) {
    switch (o) {
    case 34: // ArmTrigger/DisarmTrigger: named DSPP trigger -> Cue state
    case 51: // LinkAttachments: bounded ownership validation + unlink
    case 54: // StartAttachment: bounded Item/tag validation
    case 67: // StopInstrument: bounded Item/tag validation
    case 68: // ReleaseInstrument: bounded Item validation
    case 57: // ConnectInstrumentParts: bounded state + Output/Input strings
    case 59: // DisconnectInstrumentParts: bounded destination cleanup
    case 60: // ReadKnobPart: bounded retained-value output
    case 69: // StartInstrument: all 26 callers consume/discard an Err result
    case 70: // SetKnobPart: bounded knob state; hottest audio call
    case 41: // AbortTimerCue: Cue ownership and unposted-event validation
    case 44: // ResumeInstrument: bounded instrument Item validation
    case 45: // PauseInstrument: bounded instrument Item validation
    case 50: // MonitorAttachment: bounded attachment/Cue association
    case 19: // GetAudioResourceInfo: bounded type/output validation
    case 28: // GetAudioItemInfo: bounded Item/sample metadata query
    case M2_AUDIO_SET_ITEM_INFO_ORDINAL: // SetAudioItemInfo: sample metadata
    case 58: // SignalAtAudioTime: bounded clock/Cue/ownership validation
    case 24: // SleepUntilAudioTime: SignalAtAudioTime + GetCueSignal + WaitSignal
    case 52: // StopAttachment: bounded Item/tag validation + attachment stop
    case 56: // SetAudioClockRate: bounded clock-Item validation + tick duration
    case 71: // ReadAudioClock: bounded clock-Item validation + AudioTime out-slot
        return true;
    default:
        return false;
    }
}

static inline bool m2_is_audio_cue_signal_selector(uint32_t o) {
    return o == 23; // GetCueSignal: Cue signal-mask query
}

static inline bool m2_is_audio_item_selector(uint32_t o) {
    switch (o) {
    case 8:  // CreateMixerTemplate: Item, source-backed mixer template
    case 17: // UnloadInstrument: Err, consumes a LoadInstrument Item
    case 32: // LoadInsTemplate: Item, source-backed audio template
    case 27: // LoadInstrument: Item, source-backed audio Item result
        return true;
    default:
        return false;
    }
}

static inline bool m2_is_audio_time_selector(uint32_t o) {
    return o == 35; // GetAudioTime: AudioTime value from the global clock
}

static inline bool m2_is_audio_frame_count_selector(uint32_t o) {
    return o == 37; // GetAudioFrameCount: 32-bit sample-frame count
}

static inline bool m2_is_audio_probe_selector(uint32_t o) {
    return o == 38; // ReadProbePart: probe value/status boundary
}

static inline bool m2_is_audio_typed_selector(uint32_t o) {
    return m2_is_audio_status_selector(o) || m2_is_audio_item_selector(o) ||
           m2_is_audio_time_selector(o) || m2_is_audio_frame_count_selector(o) ||
           m2_is_audio_probe_selector(o) || m2_is_audio_cue_signal_selector(o);
}

#ifdef __cplusplus
extern "C" {
#endif

// Implemented by the recomp runtime. The functions return only fixed-width
// guest Item/Err values. Original template loading, retained graph state, and
// the source-backed DSPP execution subset are present, including the authored
// graph breadth and the race-audio branch channel lifecycle. See
// docs/game/audio.md and docs/m2/dsp-audio.md.
int32_t m2_guest_audio_create_mixer_template(struct PPCContext *cpu,
                                             uint32_t mixer_spec,
                                             uint32_t taglist_ea);
int32_t m2_guest_audio_load_instrument(struct PPCContext *cpu,
                                       uint32_t name_ea,
                                       uint32_t calc_rate_divider,
                                       uint32_t priority);
int32_t m2_guest_audio_unload_instrument(uint32_t instrument);
int32_t m2_guest_audio_load_ins_template(struct PPCContext *cpu,
                                         uint32_t name_ea,
                                         uint32_t taglist_ea);
uint32_t m2_guest_audio_get_audio_time(struct PPCContext *cpu);
uint32_t m2_guest_audio_get_audio_frame_count(struct PPCContext *cpu);
uint32_t m2_guest_audio_get_cue_signal(struct PPCContext *cpu,
                                       uint32_t cue);
// Host frame hook for the recovered global AudioClock/Cue path. Device-backed
// time follows the published rendered-frame counter; standalone calls advance
// the deterministic 60 Hz fallback by one explicit emulated tick.
void m2_guest_audio_pump_timers(void);
// Device-backed audio always follows its rendered-frame counter. This variant
// controls only whether a silent fallback advances on this emulated tick.
void m2_guest_audio_pump_timers_for_tick(int advance_idle_clock);
// Bounded MPEG device queue observation for deterministic contract tests.
// This reports host-side pending counts only; it does not alter guest input or
// select a playback route.
void m2_guest_mpeg_pending_counts(uint32_t* reads_out, uint32_t* writes_out);
// Per-host-frame DSPP render: ticks started pure-synth sims at the mixer's
// published 44.1 kHz cadence (deficit since the last pump, bounded catch-up)
// and pushes the summed first-OUTPUT stereo pairs into the host mixer. With
// no mixer (rendered count zero) it only advances the observation tick.
// Returns frames produced, so a device worker sleeps only after catching up.
uint32_t m2_guest_audio_pump_dspp_output(void);
// Counts voluntary backoff requests for production-boundary diagnostics.
uint64_t m2_guest_audio_dspp_backoff_sleeps(void);
// Interactive platform service selection. When enabled, the host calls the
// DSPP output pump from a bounded audio-clock worker and the timer pump skips
// its synchronous duplicate. Focused/headless tests leave this disabled.
void m2_guest_audio_set_async_dspp(int enabled);
int m2_guest_audio_async_dspp_enabled(void);
// Setter-priority handshake between graph-lock waiters and the asynchronous
// DSPP worker: a blocking acquisition announces itself in
// m2_guest_audio_graph_lock and clears the count once it owns the lock; the
// worker yields between sample frames while the count is nonzero, so a guest's
// blocked time is bounded by one sample frame instead of the in-flight lock
// chunk. The count is advisory and bounded by the worker's own backoff, never
// a correctness gate.
void m2_guest_audio_setter_enter(void);
void m2_guest_audio_setter_leave(void);
int m2_guest_audio_setter_pending(void);
int32_t m2_guest_audio_get_resource_info(struct PPCContext *cpu,
                                         uint32_t info_ea,
                                         uint32_t info_size,
                                         uint32_t resource_type);
int32_t m2_guest_audio_get_item_info(struct PPCContext *cpu,
                                     uint32_t audio_item,
                                     uint32_t taglist_ea);
int32_t m2_guest_audio_set_item_info(struct PPCContext *cpu,
                                     uint32_t audio_item,
                                     uint32_t taglist_ea);
int32_t m2_guest_audio_signal_at_audio_time(struct PPCContext *cpu,
                                            uint32_t clock,
                                            uint32_t cue,
                                            uint32_t time);
int32_t m2_guest_audio_read_audio_clock(struct PPCContext *cpu,
                                        uint32_t clock,
                                        uint32_t time_ptr_ea);
int32_t m2_guest_audio_set_audio_clock_rate(struct PPCContext *cpu,
                                            uint32_t clock,
                                            float hertz);
int32_t m2_guest_audio_sleep_until_audio_time(struct PPCContext *cpu,
                                              uint32_t clock,
                                              uint32_t cue,
                                              uint32_t time);
int32_t m2_guest_audio_stop_attachment(struct PPCContext *cpu,
                                       uint32_t attachment,
                                       uint32_t taglist_ea);
int32_t m2_guest_audio_read_probe_part(struct PPCContext *cpu,
                                       uint32_t probe,
                                       int32_t part,
                                       uint32_t value_ptr_ea);
int32_t m2_guest_audio_abort_timer_cue(struct PPCContext *cpu,
                                       uint32_t cue);
int32_t m2_guest_audio_resume_instrument(struct PPCContext *cpu,
                                         uint32_t instrument);
int32_t m2_guest_audio_pause_instrument(struct PPCContext *cpu,
                                        uint32_t instrument);
int32_t m2_guest_audio_monitor_attachment(struct PPCContext *cpu,
                                          uint32_t attachment,
                                          uint32_t cue,
                                          int32_t cue_at);
int32_t m2_guest_audio_connect_instrument_parts(struct PPCContext *cpu,
                                                uint32_t src_instrument,
                                                uint32_t src_name_ea,
                                                int32_t src_part,
                                                uint32_t dst_instrument,
                                                uint32_t dst_name_ea,
                                                int32_t dst_part);
int32_t m2_guest_audio_disconnect_instrument_parts(struct PPCContext *cpu,
                                                   uint32_t dst_instrument,
                                                   uint32_t dst_name_ea,
                                                   int32_t dst_part);
int32_t m2_guest_audio_link_attachments(struct PPCContext *cpu,
                                        uint32_t attachment1,
                                        uint32_t attachment2);
int32_t m2_guest_audio_start_attachment(struct PPCContext *cpu,
                                        uint32_t attachment,
                                        uint32_t taglist_ea);
int32_t m2_guest_audio_start_instrument(struct PPCContext *cpu,
                                         uint32_t instrument,
                                         uint32_t taglist_ea);
int32_t m2_guest_audio_stop_instrument(struct PPCContext *cpu,
                                       uint32_t instrument,
                                       uint32_t taglist_ea);
int32_t m2_guest_audio_release_instrument(struct PPCContext *cpu,
                                          uint32_t instrument,
                                          uint32_t taglist_ea);
int32_t m2_guest_audio_arm_trigger_named(struct PPCContext *cpu,
                                          uint32_t instrument,
                                          uint32_t trigger_name_ea,
                                          uint32_t cue,
                                          uint32_t flags);
int32_t m2_guest_audio_read_knob_part(struct PPCContext *cpu,
                                      uint32_t knob,
                                      int32_t part,
                                      uint32_t value_ptr_ea);
int32_t m2_guest_audio_set_knob_part(struct PPCContext *cpu,
                                     uint32_t knob,
                                     int32_t part,
                                     float value);
int32_t m2_guest_audiopatch_create_patch_template(struct PPCContext *cpu,
                                                  uint32_t patch_cmds_ea,
                                                  uint32_t taglist_ea);
int32_t m2_guest_audio_get_instrument_patch_block_count(uint32_t instrument,
                                                        uint32_t *count_out);

#ifdef __cplusplus
}
#endif
