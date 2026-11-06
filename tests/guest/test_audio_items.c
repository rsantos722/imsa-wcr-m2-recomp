// Focused unit test — source-backed CreateMixerTemplate and
// LoadInstrument/UnloadInstrument Item ownership, plus bounded CreateKnob /
// SetKnobPart, probe validation, Cue cancellation, and attachment state. The audio boundary returns the same
// fixed-width local Item handles as CreateItemVA; DSP execution and playback
// remain outside it. R1 mix nodes (patch/mixer-template instruments as
// gain-routing nodes with probe readback and a resident line_out-bound mix)
// are covered by the R1 section at the end of main.

/* R3.1 oracle declaration — the expected values asserted by this
 * focused gate are taken from the original source/architecture named
 * below, not from the host implementation. */
/* M2_SELECTOR_ORACLE(5:0, 6:8, 6:17, 6:19, 6:23, 6:27, 6:28, 6:32, 6:35, 6:37, 6:38, 6:41, 6:44, 6:45, 6:50, 6:51, 6:54, 6:57, 6:58, 6:59, 6:60, 6:67, 6:68, 6:69, 6:70) — source-backed audio folio contract: template/instrument ownership and loading, cue/timer signals, attachment linkage and physical binding, knob read/write, audio clock and frame count, probe validation, and instrument transport state */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

#include "dsp_template_loader.h"

// Strong raw-file bridge scoped to the DSPP regression: serves the extracted
// disc DSP templates (System.m2/Audio/dsp/<name>) without the host shim.
static uint32_t test_dsp_raw_open(const char *path);
static uint32_t test_dsp_raw_size(uint32_t token);
static int32_t test_dsp_raw_read(uint32_t token, void *dst, uint32_t n);
static void test_dsp_raw_close(uint32_t token);

uint32_t m2_host_raw_open(const char *path) {
    return test_dsp_raw_open(path);
}
uint32_t m2_host_raw_size(uint32_t token) { return test_dsp_raw_size(token); }
int32_t m2_host_raw_read(uint32_t token, void *dst, uint32_t n) {
    return test_dsp_raw_read(token, dst, n);
}
void m2_host_raw_close(uint32_t token) { test_dsp_raw_close(token); }

#include "ppc_ctx.h"
#include "recomp_helpers.h"
#include "guest_kernel.h"
#include "generated/game_recomp.h"
#include "posix_compat.h"

static int g_fails;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_fails; \
    } \
} while (0)

static PPCContext g_cpu;
static m2k_context g_kctx;
static uint8_t *g_mem;
static uint32_t g_audio_test_frames;
static uint32_t g_task_cue;
static uint32_t g_task_cue_signal;
static int32_t g_task_cue_abort_rc;
static uint32_t g_delivery_cue;
static uint32_t g_delivery_signal;
static int32_t g_delivery_post_rc;
static int32_t g_delivery_wait_rc;
static _Atomic int g_delivery_ready;
static uint32_t g_attachment_completion;
static uint32_t g_attachment_monitor_target;
static uint32_t g_attachment_monitor_cue;
static uint32_t g_attachment_monitor_signal;
static int32_t g_attachment_monitor_rc;
static int32_t g_attachment_monitor_wait_rc;
static _Atomic int g_attachment_monitor_ready;
static uint32_t g_stream_completion_expected;

// ProcessAttachmentDMA starts a linked successor and signals its monitoring
// Cue without a guest StartAttachment call. Poll instead of blocking so the
// old stopped-successor regression fails an assertion rather than hanging.
static int32_t check_stream_completion_task(m2k_context *ctx, uint32_t target) {
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    uint32_t cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    uint32_t signal = m2_guest_audio_get_cue_signal(&g_cpu, cue);
    CHECK(signal != 0u);
    CHECK(m2_guest_audio_monitor_attachment(&g_cpu, target, cue,
                                            M2_AUDIO_CUE_AT_END) == M2K_OK);
    g_attachment_completion = target;
    m2_guest_audio_pump_timers();
    CHECK(m2k_poll_signal(ctx, signal) ==
          (g_stream_completion_expected ? (int32_t)signal : 0));
    hdl->DeleteItem(cue);
    return M2K_OK;
}

static void check_stream_completion(uint32_t target, uint32_t expected) {
    g_stream_completion_expected = expected;
    uint32_t task = m2k_create_task(&g_kctx, check_stream_completion_task, target);
    CHECK(task != M2K_ITEM_NONE);
    CHECK(m2k_join_task(&g_kctx, task) == M2K_OK);
    CHECK(m2k_delete_task(&g_kctx, task) == M2K_OK);
}
static uint32_t g_envelope_monitor_target;
static uint32_t g_envelope_monitor_cue;
static uint32_t g_envelope_monitor_signal;
static int32_t g_envelope_monitor_rc;
static int32_t g_envelope_monitor_poll;
static int32_t g_envelope_monitor_poll_end;
static _Atomic int g_envelope_monitor_ready;
static _Atomic int g_envelope_monitor_stage1;
static _Atomic int g_envelope_monitor_seen;
static _Atomic int g_envelope_monitor_stage2;

extern int32_t m2_guest_audio_resolve_attachment_channel(
    uint32_t attachment, uint32_t *channel_out);

static int32_t create_owned_cue_task(m2k_context *ctx, uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_task_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_task_cue_signal = m2_guest_audio_get_cue_signal(&g_cpu, g_task_cue);
    g_task_cue_abort_rc = m2_guest_audio_abort_timer_cue(&g_cpu, g_task_cue);
    return g_task_cue_abort_rc;
}

static int32_t wait_for_cue_task(m2k_context *ctx, uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_delivery_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_delivery_signal = m2_guest_audio_get_cue_signal(&g_cpu,
                                                       g_delivery_cue);
    g_delivery_post_rc = m2_guest_audio_signal_at_audio_time(
        &g_cpu, 0, g_delivery_cue, 7u);
    atomic_store_explicit(&g_delivery_ready, 1, memory_order_release);
    if (g_delivery_post_rc != M2K_OK) return g_delivery_post_rc;
    g_delivery_wait_rc = hdl->WaitSignal(g_delivery_signal);
    return g_delivery_wait_rc < 0 ? g_delivery_wait_rc : M2K_OK;
}

static int32_t wait_for_attachment_completion_task(m2k_context *ctx,
                                                   uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_attachment_monitor_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_attachment_monitor_signal = m2_guest_audio_get_cue_signal(
        &g_cpu, g_attachment_monitor_cue);
    g_attachment_monitor_rc = m2_guest_audio_monitor_attachment(
        &g_cpu, g_attachment_monitor_target, g_attachment_monitor_cue,
        M2_AUDIO_CUE_AT_END);
    atomic_store_explicit(&g_attachment_monitor_ready, 1,
                          memory_order_release);
    if (g_attachment_monitor_rc != M2K_OK)
        return g_attachment_monitor_rc;
    g_attachment_monitor_wait_rc = hdl->WaitSignal(
        g_attachment_monitor_signal);
    return g_attachment_monitor_wait_rc < 0 ? g_attachment_monitor_wait_rc
                                             : M2K_OK;
}

// Envelope release monitor: keep the task out of WaitSignal until the main
// thread has completed a bounded pump window, then poll the exact monitoring
// Cue bit. This preserves a non-blocking negative result if the release event
// is never scheduled.
static int32_t envelope_release_monitor_task(m2k_context *ctx,
                                             uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_envelope_monitor_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_envelope_monitor_signal = m2_guest_audio_get_cue_signal(
        &g_cpu, g_envelope_monitor_cue);
    if (g_envelope_monitor_cue == M2K_ITEM_NONE ||
        g_envelope_monitor_signal == 0u) {
        atomic_store_explicit(&g_envelope_monitor_ready, 1,
                              memory_order_release);
        return M2K_ERR_BAD_ITEM;
    }
    g_envelope_monitor_rc = m2_guest_audio_monitor_attachment(
        &g_cpu, g_envelope_monitor_target, g_envelope_monitor_cue,
        M2_AUDIO_CUE_AT_END);
    atomic_store_explicit(&g_envelope_monitor_ready, 1,
                          memory_order_release);
    if (g_envelope_monitor_rc != M2K_OK)
        return g_envelope_monitor_rc;
    while (!atomic_load_explicit(&g_envelope_monitor_stage1,
                                 memory_order_acquire))
        sched_yield();
    g_envelope_monitor_poll = m2k_poll_signal(
        &g_kctx, g_envelope_monitor_signal);
    atomic_store_explicit(&g_envelope_monitor_seen, 1,
                          memory_order_release);
    while (!atomic_load_explicit(&g_envelope_monitor_stage2,
                                 memory_order_acquire))
        sched_yield();
    g_envelope_monitor_poll_end = m2k_poll_signal(
        &g_kctx, g_envelope_monitor_signal);
    return M2K_OK;
}

// ---- Raw-DSPP completion pump test state --------------------------------------
// Staged no-wakeup observation for the numeric-completion -> Cue pump half:
// the task monitors an attachment and records polled signal state at two
// main-driven pump stages, so a fabricating pump fails loudly instead of
// hanging the suite in WaitSignal/join.
static uint32_t g_rawcomp_target;
static uint32_t g_rawcomp_cue;
static uint32_t g_rawcomp_signal;
static int32_t g_rawcomp_monitor_rc;
static int32_t g_rawcomp_poll_mid;
static int32_t g_rawcomp_poll_end;
static _Atomic int g_rawcomp_ready;
static _Atomic int g_rawcomp_stage1;
static _Atomic int g_rawcomp_stage2;
static _Atomic int g_rawcomp_mid_seen;

// Two-stage monitor for the physical current/next attachment contract. The
// task reuses one Cue: after the first completion is consumed, it monitors
// the linked successor and records a second distinct completion. All waits
// are staged so a missing promotion cannot deadlock the test teardown.
static uint32_t g_rawchain_target1;
static uint32_t g_rawchain_target2;
static uint32_t g_rawchain_cue;
static uint32_t g_rawchain_signal;
static int32_t g_rawchain_monitor_rc;
static int32_t g_rawchain_poll_first;
static int32_t g_rawchain_poll_second;
static int32_t g_rawchain_poll_restart;
static int32_t g_rawchain_poll_end;
static _Atomic int g_rawchain_ready;
static _Atomic int g_rawchain_stage1;
static _Atomic int g_rawchain_first_seen;
static _Atomic int g_rawchain_second_ready;
static _Atomic int g_rawchain_stage2;
static _Atomic int g_rawchain_second_seen;
static _Atomic int g_rawchain_stage3;
static _Atomic int g_rawchain_restart_seen;
static _Atomic int g_rawchain_stage4;

static void rawchain_reset_stages(void) {
    g_rawchain_target1 = 0;
    g_rawchain_target2 = 0;
    g_rawchain_cue = 0;
    g_rawchain_signal = 0;
    g_rawchain_monitor_rc = M2K_ERR_BAD_ITEM;
    g_rawchain_poll_first = -1;
    g_rawchain_poll_second = -1;
    g_rawchain_poll_restart = -1;
    g_rawchain_poll_end = -1;
    atomic_store_explicit(&g_rawchain_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_stage1, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_first_seen, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_second_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_stage2, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_second_seen, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_stage3, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_restart_seen, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawchain_stage4, 0, memory_order_relaxed);
}

static int32_t rawchain_monitor_task(m2k_context *ctx, uint32_t arg) {
    const m2_kernel_dispatch *hdl;
    (void)ctx;
    (void)arg;
    hdl = m2_guest_kernel_handlers();
    g_rawchain_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_rawchain_signal = m2_guest_audio_get_cue_signal(&g_cpu,
                                                       g_rawchain_cue);
    if (g_rawchain_cue == M2K_ITEM_NONE || g_rawchain_signal == 0u)
        return -1;
    g_rawchain_monitor_rc = m2_guest_audio_monitor_attachment(
        &g_cpu, g_rawchain_target1, g_rawchain_cue, M2_AUDIO_CUE_AT_END);
    atomic_store_explicit(&g_rawchain_ready, 1, memory_order_release);
    if (g_rawchain_monitor_rc != M2K_OK) return g_rawchain_monitor_rc;
    while (!atomic_load_explicit(&g_rawchain_stage1, memory_order_acquire))
        sched_yield();
    g_rawchain_poll_first = m2k_poll_signal(&g_kctx, g_rawchain_signal);
    atomic_store_explicit(&g_rawchain_first_seen, 1, memory_order_release);

    g_rawchain_monitor_rc = m2_guest_audio_monitor_attachment(
        &g_cpu, g_rawchain_target2, g_rawchain_cue, M2_AUDIO_CUE_AT_END);
    atomic_store_explicit(&g_rawchain_second_ready, 1, memory_order_release);
    if (g_rawchain_monitor_rc != M2K_OK) {
        atomic_store_explicit(&g_rawchain_second_seen, 1,
                              memory_order_release);
        return g_rawchain_monitor_rc;
    }
    while (!atomic_load_explicit(&g_rawchain_stage2, memory_order_acquire))
        sched_yield();
    g_rawchain_poll_second = m2k_poll_signal(&g_kctx, g_rawchain_signal);
    atomic_store_explicit(&g_rawchain_second_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_rawchain_stage3, memory_order_acquire))
        sched_yield();
    g_rawchain_poll_restart = m2k_poll_signal(&g_kctx, g_rawchain_signal);
    atomic_store_explicit(&g_rawchain_restart_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_rawchain_stage4, memory_order_acquire))
        sched_yield();
    g_rawchain_poll_end = m2k_poll_signal(&g_kctx, g_rawchain_signal);
    return M2K_OK;
}

static void rawcomp_reset_stages(void) {
    g_rawcomp_target = 0;
    g_rawcomp_cue = 0;
    g_rawcomp_signal = 0;
    g_rawcomp_monitor_rc = M2K_ERR_BAD_ITEM;
    g_rawcomp_poll_mid = -1;
    g_rawcomp_poll_end = -1;
    atomic_store_explicit(&g_rawcomp_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawcomp_stage1, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawcomp_stage2, 0, memory_order_relaxed);
    atomic_store_explicit(&g_rawcomp_mid_seen, 0, memory_order_relaxed);
}

// Monitor the target attachment CUE_AT_END, then report polled signal state
// at both main-driven pump stages without ever blocking in WaitSignal.
static int32_t rawcomp_monitor_task(m2k_context *ctx, uint32_t arg) {
    const m2_kernel_dispatch *hdl;
    (void)ctx;
    (void)arg;
    hdl = m2_guest_kernel_handlers();
    g_rawcomp_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_rawcomp_signal = m2_guest_audio_get_cue_signal(&g_cpu, g_rawcomp_cue);
    if (g_rawcomp_cue == M2K_ITEM_NONE || g_rawcomp_signal == 0u) return -1;
    g_rawcomp_monitor_rc = m2_guest_audio_monitor_attachment(
        &g_cpu, g_rawcomp_target, g_rawcomp_cue, M2_AUDIO_CUE_AT_END);
    atomic_store_explicit(&g_rawcomp_ready, 1, memory_order_release);
    if (g_rawcomp_monitor_rc != M2K_OK) return g_rawcomp_monitor_rc;
    while (!atomic_load_explicit(&g_rawcomp_stage1, memory_order_acquire))
        sched_yield();
    g_rawcomp_poll_mid = m2k_poll_signal(&g_kctx, g_rawcomp_signal);
    atomic_store_explicit(&g_rawcomp_mid_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_rawcomp_stage2, memory_order_acquire))
        sched_yield();
    g_rawcomp_poll_end = m2k_poll_signal(&g_kctx, g_rawcomp_signal);
    return M2K_OK;
}

// ---- timer/Cue delivery gate state -----------------------------------------
// One task, one Cue, four main-driven stages. Vendor audio_timer.c: a Cue
// holds exactly one AudioEvent (PostAudioEvent returns AF_ERR_INUSE when it is
// already in a list), AbortTimerCue unposts it so the wakeup never fires, and
// an already-elapsed AudioTime is due on the next timer interrupt. Every
// observation is a non-blocking poll so a missing cancel or a missing
// delivery fails loudly instead of hanging the suite in WaitSignal.
static uint32_t g_timercue_item;
static uint32_t g_timercue_signal;
static uint32_t g_timercue_due;
static int32_t  g_timercue_post_rc;
static int32_t  g_timercue_abort_rc;
static int32_t  g_timercue_repost_rc;
static int32_t  g_timercue_poll_cancelled;
static int32_t  g_timercue_poll_repost;
static int32_t  g_timercue_poll_end;
static _Atomic int g_timercue_ready;
static _Atomic int g_timercue_stage1;
static _Atomic int g_timercue_seen1;
static _Atomic int g_timercue_stage2;
static _Atomic int g_timercue_seen2;
static _Atomic int g_timercue_stage3;
static _Atomic int g_timercue_seen3;
static _Atomic int g_timercue_stage4;
static _Atomic int g_timercue_seen4;

static void timercue_reset_stages(void) {
    g_timercue_item = 0;
    g_timercue_signal = 0;
    g_timercue_due = 0;
    g_timercue_post_rc = M2K_ERR_BAD_ITEM;
    g_timercue_abort_rc = M2K_ERR_BAD_ITEM;
    g_timercue_repost_rc = M2K_ERR_BAD_ITEM;
    g_timercue_poll_cancelled = -1;
    g_timercue_poll_repost = -1;
    g_timercue_poll_end = -1;
    atomic_store_explicit(&g_timercue_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_stage1, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_seen1, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_stage2, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_seen2, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_stage3, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_seen3, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_stage4, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timercue_seen4, 0, memory_order_relaxed);
}

// SignalAtTimeCue form of the recovered 0x3c690 call shape
// (SignalAtAudioTime(AF_GLOBAL_CLOCK, cue, time)) followed by the SleepUntilTimeCue
// equivalent the guest inlines (GetCueSignal + WaitSignal). The host boundary
// is exercised directly because the guest image imports no ord-13 selector.
static int32_t timercue_abort_task(m2k_context *ctx, uint32_t arg) {
    const m2_kernel_dispatch *hdl;
    (void)ctx;
    (void)arg;
    hdl = m2_guest_kernel_handlers();
    g_timercue_item = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_timercue_signal = m2_guest_audio_get_cue_signal(&g_cpu,
                                                      g_timercue_item);
    if (g_timercue_item == M2K_ITEM_NONE || g_timercue_signal == 0u)
        return -1;
    g_timercue_due = m2_guest_audio_get_audio_time(&g_cpu) + 8u;
    g_timercue_post_rc = m2_guest_audio_signal_at_audio_time(
        &g_cpu, 0u, g_timercue_item, g_timercue_due);
    g_timercue_abort_rc = m2_guest_audio_abort_timer_cue(&g_cpu,
                                                         g_timercue_item);
    atomic_store_explicit(&g_timercue_ready, 1, memory_order_release);

    while (!atomic_load_explicit(&g_timercue_stage1, memory_order_acquire))
        sched_yield();
    g_timercue_poll_cancelled = m2k_poll_signal(&g_kctx, g_timercue_signal);
    atomic_store_explicit(&g_timercue_seen1, 1, memory_order_release);

    while (!atomic_load_explicit(&g_timercue_stage2, memory_order_acquire))
        sched_yield();
    // Reuse the same Cue after the abort at an already-elapsed AudioTime, the
    // vendor's "time has already passed => return immediately" case.
    g_timercue_repost_rc = m2_guest_audio_signal_at_audio_time(
        &g_cpu, 0u, g_timercue_item, m2_guest_audio_get_audio_time(&g_cpu));
    atomic_store_explicit(&g_timercue_seen2, 1, memory_order_release);

    while (!atomic_load_explicit(&g_timercue_stage3, memory_order_acquire))
        sched_yield();
    g_timercue_poll_repost = m2k_poll_signal(&g_kctx, g_timercue_signal);
    atomic_store_explicit(&g_timercue_seen3, 1, memory_order_release);

    while (!atomic_load_explicit(&g_timercue_stage4, memory_order_acquire))
        sched_yield();
    g_timercue_poll_end = m2k_poll_signal(&g_kctx, g_timercue_signal);
    atomic_store_explicit(&g_timercue_seen4, 1, memory_order_release);
    return M2K_OK;
}

uint32_t m2_host_audio_frame_count(void) {
    return g_audio_test_frames;
}

static uint32_t g_cbd2_stream_starts;
static uint32_t g_pcm16be_stream_starts;
static int32_t g_cbd2_stream_start_rc = 1;
static int32_t g_cbd2_stream_link_rc = 1;
static int32_t g_pcm16be_stream_start_rc = 1;
static int32_t g_pcm16be_stream_link_rc = 1;

int32_t m2_host_audio_start_cbd2_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t *bytes,
    uint32_t byte_count, float gain) {
    (void)stream_key;
    (void)attachment_id;
    (void)bytes;
    (void)byte_count;
    (void)gain;
    ++g_cbd2_stream_starts;
    return g_cbd2_stream_start_rc;
}

int32_t m2_host_audio_link_cbd2_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t *bytes, uint32_t byte_count) {
    (void)stream_key;
    (void)first_attachment;
    (void)next_attachment;
    (void)bytes;
    (void)byte_count;
    return g_cbd2_stream_link_rc;
}

int32_t m2_host_audio_start_pcm16be_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t *bytes,
    uint32_t byte_count, float gain) {
    (void)stream_key;
    (void)attachment_id;
    (void)bytes;
    (void)byte_count;
    (void)gain;
    ++g_pcm16be_stream_starts;
    return g_pcm16be_stream_start_rc;
}

int32_t m2_host_audio_link_pcm16be_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t *bytes, uint32_t byte_count) {
    (void)stream_key;
    (void)first_attachment;
    (void)next_attachment;
    (void)bytes;
    (void)byte_count;
    return g_pcm16be_stream_link_rc;
}

// Capturing backend stub for the CreateSample bridge: records uploads so the
// PCM/SQS2 decode path is verified without a real SDL mixer. The weak
// trace_stubs.c default returns 0 (no backend); this strong override mints
// ascending ids and retains the first stereo frame for value checks.
static uint32_t g_sample_uploads;
static uint32_t g_last_upload_frames;
static int16_t g_last_upload_l0;
static int16_t g_last_upload_r0;
static int16_t g_last_upload_l1;
static int16_t g_last_upload_r1;
static uint32_t g_next_sid;
static uint32_t g_started_sample;
static uint32_t g_started_attachment;
static int32_t g_started_attachment_voice;
static int32_t g_start_voice_rc;
static int32_t g_started_sustain_begin;
static int32_t g_started_sustain_end;
static int32_t g_started_release_begin;
static int32_t g_started_release_end;
static float g_started_rate_hz;
static float g_started_gain;
static uint32_t g_release_attachment;

uint32_t m2_host_audio_add_sample(const int16_t *pcm, uint32_t frames) {
    if (!pcm || frames == 0) return 0;
    ++g_sample_uploads;
    g_last_upload_frames = frames;
    g_last_upload_l0 = pcm[0];
    g_last_upload_r0 = pcm[1];
    if (frames >= 2u) {
        g_last_upload_l1 = pcm[2];
        g_last_upload_r1 = pcm[3];
    } else {
        g_last_upload_l1 = 0;
        g_last_upload_r1 = 0;
    }
    return ++g_next_sid;
}

int32_t m2_host_audio_start_voice(uint32_t sample_id, int looping) {
    (void)looping;
    if (sample_id == 0) return -1;
    g_started_sample = sample_id;
    return g_start_voice_rc;
}
static uint32_t g_voice_sids[32];

int32_t m2_host_audio_start_voice_indexed(uint32_t sample_id, int looping) {
    (void)looping;
    if (sample_id == 0 || g_start_voice_rc != 0) return -1;
    for (int i = 0; i < 32; ++i) {
        if (g_voice_sids[i] == 0) {
            g_voice_sids[i] = sample_id;
            g_started_sample = sample_id;
            return i;
        }
    }
    return -1;
}

int32_t m2_host_audio_start_voice_indexed_for_attachment(
    uint32_t sample_id, int looping, uint32_t attachment_id) {
    g_started_attachment = attachment_id;
    g_started_sustain_begin = -1;
    g_started_sustain_end = -1;
    g_started_release_begin = -1;
    g_started_release_end = -1;
    g_started_rate_hz = 0.0f;
    g_started_gain = 0.0f;
    g_started_attachment_voice = m2_host_audio_start_voice_indexed(
        sample_id, looping);
    return g_started_attachment_voice;
}

int32_t m2_host_audio_start_voice_indexed_for_attachment_with_loops(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end) {
    g_started_attachment = attachment_id;
    g_started_sustain_begin = sustain_begin;
    g_started_sustain_end = sustain_end;
    g_started_release_begin = release_begin;
    g_started_release_end = release_end;
    g_started_attachment_voice = m2_host_audio_start_voice_indexed(
        sample_id, sustain_begin >= 0 && sustain_end > sustain_begin);
    return g_started_attachment_voice;
}

int32_t m2_host_audio_start_voice_indexed_for_attachment_with_params(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end,
    float rate_hz, float gain) {
    g_started_rate_hz = rate_hz;
    g_started_gain = gain;
    return m2_host_audio_start_voice_indexed_for_attachment_with_loops(
        sample_id, attachment_id, sustain_begin, sustain_end, release_begin,
        release_end);
}

int32_t m2_host_audio_release_voice(uint32_t voice_index) {
    (void)voice_index;
    return 0;
}

int32_t m2_host_audio_release_voice_for_attachment(uint32_t attachment_id) {
    g_release_attachment = attachment_id;
    return 0;
}

int32_t m2_host_audio_pop_attachment_completion(uint32_t *attachment_id) {
    if (!attachment_id || g_attachment_completion == 0u) return 0;
    *attachment_id = g_attachment_completion;
    g_attachment_completion = 0u;
    return 1;
}

uint32_t m2_host_audio_voice_sample(uint32_t voice_index) {
    return voice_index < 32u ? g_voice_sids[voice_index] : 0;
}

static int g_voice_paused[32];

int32_t m2_host_audio_pause_voice(uint32_t voice_index) {
    if (voice_index >= 32u) return -1;
    g_voice_paused[voice_index] = 1;
    return 0;
}

int32_t m2_host_audio_resume_voice(uint32_t voice_index) {
    if (voice_index >= 32u) return -1;
    g_voice_paused[voice_index] = 0;
    return 0;
}

int32_t m2_host_audio_stop_voice(uint32_t voice_index) {
    if (voice_index >= 32u) return -1;
    g_voice_sids[voice_index] = 0;
    g_voice_paused[voice_index] = 0;
    return 0;
}

// R2-full partition backend (test-local strong override of the weak
// default): records the guest-computed direct/SDL flag per voice slot so the
// connection partition is observable without an SDL mixer. Production honors
// it in AudioShim::mix_into; suppressed voices stay silent on direct out
// while the tap still feeds them to the graph.
static int g_voice_direct[32];
void m2_host_audio_set_voice_direct(uint32_t voice_index, int on) {
    if (voice_index < 32u) g_voice_direct[voice_index] = on != 0;
}

void m2_host_audio_set_voice_params(uint32_t voice_index, float rate_hz,
                                    float gain) {
    if (voice_index >= 32u || g_voice_sids[voice_index] == 0u) return;
    g_started_rate_hz = rate_hz;
    g_started_gain = gain;
}

// R2 voice-tap backend (test-local strong override of the weak default):
// frame-sequenced stereo pairs consumed one per source per ticked frame.
static int16_t g_tap_script_l[64];
static int16_t g_tap_script_r[64];
static uint32_t g_tap_script_n = 0;
static uint32_t g_tap_calls = 0;
int m2_host_audio_voice_tap_frame(uint32_t voice_index, int16_t *l_out,
                                  int16_t *r_out) {
    if (voice_index < 32u && g_voice_paused[voice_index]) {
        if (l_out) *l_out = 0;
        if (r_out) *r_out = 0;
        return 0;
    }
    int16_t l = 0, r = 0;
    int have = 0;
    if (g_tap_calls < g_tap_script_n) {
        l = g_tap_script_l[g_tap_calls];
        r = g_tap_script_r[g_tap_calls];
        have = 1;
    }
    ++g_tap_calls;
    if (l_out) *l_out = l;
    if (r_out) *r_out = r;
    return have;
}

// Capturing sink for the DSPP mixer render: records pushed frames so the
// tick->sum->push producer path is verified without an SDL mixer. The weak
// trace_stubs.c default drops; this strong override retains the count and
// the first stereo pair.
static uint32_t g_dspp_push_frames;
static uint32_t g_dspp_push_calls;
static int16_t g_dspp_first_l;
static int16_t g_dspp_first_r;
static int g_dspp_check_constant;
static uint32_t g_dspp_bad_frames;
// Output queue room for the asynchronous worker. The queue here is modelled by
// the test's device counter: frames consumed and not yet pushed. A nonnegative
// forced value replaces the model.
static int64_t g_dspp_fill_forced = -1;
uint32_t m2_host_audio_dspp_fill_request(void) {
    static uint32_t last_frames, last_pushed, owed;
    const uint32_t frames = g_audio_test_frames;
    const uint32_t pushed = g_dspp_push_frames;
    owed = frames < last_frames ? frames : owed + (frames - last_frames);
    const uint32_t delta = pushed - last_pushed;
    owed -= delta < owed ? delta : owed;
    last_frames = frames;
    last_pushed = pushed;
    if (g_dspp_fill_forced >= 0) {
        owed = 0u;
        return (uint32_t)g_dspp_fill_forced;
    }
    return owed;
}
void m2_host_audio_push_dspp_frames(const int16_t *pcm, uint32_t frames) {
    if (!pcm || frames == 0) return;
    if (g_dspp_push_calls == 0) {
        g_dspp_first_l = pcm[0];
        g_dspp_first_r = pcm[1];
    }
    if (g_dspp_check_constant) {
        for (uint32_t frame = 0; frame < frames; ++frame)
            if (pcm[frame * 2u] != 0x4000 || pcm[frame * 2u + 1u] != 0x4000)
                ++g_dspp_bad_frames;
    }
    ++g_dspp_push_calls;
    g_dspp_push_frames += frames;
}

static void setup(void) {
    g_mem = (uint8_t *)calloc(1, RECOMP_GUEST_MEM);
    CHECK(g_mem != NULL);
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.mem = g_mem;
    g_cpu.gpr[1] = 0x001a0000u;
    g_audio_test_frames = 0;
    g_sample_uploads = 0;
    g_last_upload_frames = 0;
    g_last_upload_l0 = 0;
    g_last_upload_r0 = 0;
    g_last_upload_l1 = 0;
    g_last_upload_r1 = 0;
    g_next_sid = 100u;
    g_started_sample = 0;
    g_started_attachment = 0;
    g_started_attachment_voice = -1;
    g_started_sustain_begin = -1;
    g_started_sustain_end = -1;
    g_started_release_begin = -1;
    g_started_release_end = -1;
    g_release_attachment = 0;
    g_attachment_completion = 0;
    g_attachment_monitor_target = 0;
    g_attachment_monitor_cue = 0;
    g_attachment_monitor_signal = 0;
    g_attachment_monitor_rc = M2K_ERR_BAD_ITEM;
    g_attachment_monitor_wait_rc = 0;
    atomic_store_explicit(&g_attachment_monitor_ready, 0,
                          memory_order_relaxed);
    g_envelope_monitor_target = 0;
    g_envelope_monitor_cue = 0;
    g_envelope_monitor_signal = 0;
    g_envelope_monitor_rc = M2K_ERR_BAD_ITEM;
    g_envelope_monitor_poll = -1;
    g_envelope_monitor_poll_end = -1;
    atomic_store_explicit(&g_envelope_monitor_ready, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_envelope_monitor_stage1, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_envelope_monitor_seen, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_envelope_monitor_stage2, 0,
                          memory_order_relaxed);
    g_start_voice_rc = 0;
    memset(g_voice_sids, 0, sizeof(g_voice_sids));
    memset(g_voice_paused, 0, sizeof(g_voice_paused));
    for (int i = 0; i < 32; ++i) g_voice_direct[i] = 1;
    g_dspp_push_frames = 0;
    g_dspp_push_calls = 0;
    g_dspp_first_l = 0;
    g_dspp_first_r = 0;
    g_dspp_check_constant = 0;
    g_dspp_bad_frames = 0;
    m2k_init(&g_kctx);
    CHECK(m2_guest_kernel_bind(&g_kctx, &g_cpu) == 0);
}

static void teardown(void) {
    m2_guest_kernel_unbind();
    free(g_mem);
    g_mem = NULL;
    memset(g_voice_paused, 0, sizeof(g_voice_paused));
}

// ---- DSPP regression raw-file bridge ----------------------------------------

#define TEST_DSP_RAW_CAP 64
static struct {
    uint8_t *data;
    uint32_t size;
    uint32_t pos;
    char path[160];
} g_test_dsp_files[TEST_DSP_RAW_CAP];
static uint32_t g_test_dsp_count;
static char g_test_dsp_root[1024];
static int g_test_dsp_root_found;

static void test_dsp_find_root(void) {
    snprintf(g_test_dsp_root, sizeof(g_test_dsp_root), "%s", IMSA_TEST_GAME_DIR);
    g_test_dsp_root_found = 1;
}

static uint32_t test_dsp_raw_open(const char *path) {
    if (!path) return 0;
    // Cache hit: reopen resets the read cursor.
    for (uint32_t i = 0; i < g_test_dsp_count; ++i) {
        if (strcmp(g_test_dsp_files[i].path, path) == 0) {
            g_test_dsp_files[i].pos = 0;
            return 0xB0000000u | (i + 1u);
        }
    }
    if (!g_test_dsp_root_found) test_dsp_find_root();
    if (!g_test_dsp_root_found) return 0;
    if (g_test_dsp_count >= TEST_DSP_RAW_CAP) return 0;
    char abs[1200];
    snprintf(abs, sizeof(abs), "%s/%s", g_test_dsp_root, path);
    FILE *f = fopen(abs, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    uint32_t idx = g_test_dsp_count++;
    g_test_dsp_files[idx].data = buf;
    g_test_dsp_files[idx].size = (uint32_t)sz;
    g_test_dsp_files[idx].pos = 0;
    snprintf(g_test_dsp_files[idx].path, sizeof(g_test_dsp_files[idx].path),
             "%s", path);
    return 0xB0000000u | (idx + 1u);
}

static uint32_t test_dsp_raw_size(uint32_t token) {
    if ((token & 0xF0000000u) != 0xB0000000u) return 0;
    uint32_t idx = (token & 0x0FFFFFFFu) - 1u;
    if (idx >= g_test_dsp_count) return 0;
    return g_test_dsp_files[idx].size;
}

static int32_t test_dsp_raw_read(uint32_t token, void *dst, uint32_t n) {
    if ((token & 0xF0000000u) != 0xB0000000u || !dst) return -1;
    uint32_t idx = (token & 0x0FFFFFFFu) - 1u;
    if (idx >= g_test_dsp_count) return -1;
    uint32_t sz = g_test_dsp_files[idx].size;
    uint32_t pos = g_test_dsp_files[idx].pos;
    if (pos >= sz) return 0;
    if (n > sz - pos) n = sz - pos;
    memcpy(dst, g_test_dsp_files[idx].data + pos, n);
    g_test_dsp_files[idx].pos = pos + n;
    return (int32_t)n;
}

static void test_dsp_raw_close(uint32_t token) {
    (void)token; // file lifetime is the test process
}

static int test_load_disc_dsp_template(const char *name, dspp_template *out) {
    char path[192];
    uint32_t token;
    uint32_t size;
    uint8_t *bytes;
    int rc;
    if (!name || !out) return -1;
    snprintf(path, sizeof(path), "System.m2/Audio/dsp/%s", name);
    token = test_dsp_raw_open(path);
    if (token == 0u) return -1;
    size = test_dsp_raw_size(token);
    if (size == 0u) return -1;
    bytes = (uint8_t *)malloc(size);
    if (!bytes) return -1;
    rc = test_dsp_raw_read(token, bytes, size) == (int32_t)size
             ? dspp_load_template(bytes, size, out)
             : -1;
    free(bytes);
    test_dsp_raw_close(token);
    return rc;
}

static int test_dspp_find_alloc(const dspp_sim *sim, const char *name,
                                uint8_t type, uint32_t *out) {
    const dspp_template *resources = dspp_sim_resources(sim);
    if (!resources || !name || !out) return -1;
    for (uint32_t i = 0; i < resources->resource_count; ++i) {
        const dspp_resource *resource = &resources->resources[i];
        if (resource->type == type &&
            strcmp(resource->name, name) == 0 &&
            resource->alloc_value != 0xFFFFFFFFu) {
            *out = resource->alloc_value;
            return 0;
        }
    }
    return -1;
}

// ---- Synthetic DSPP template file (no disc dependency) ------------------------
// Minimal big-endian FORM 3INS image: one IN_FIFO resource ("InFIFO") and a
// single SLEEP code word. Served through the test raw bridge under a catalog
// filename so the folio builds a live sim without reading the game data.
// Layout (all integers big-endian, IFF odd-size pad after DNMS):
// FORM(106) 3INS [ FORM(94) DSPP [ DHDR(16) MRSC(20: 1x IN_FIFO) DNMS(7+pad)
// DCOD(14: header + SLEEP) ] ].
#define TEST_SYNTH_FIFO_FILE_SIZE 114u
static uint8_t g_synth_fifo_file[TEST_SYNTH_FIFO_FILE_SIZE];
static int g_synth_fifo_built;

static void test_synth_fifo_put32(uint32_t off, uint32_t v) {
    g_synth_fifo_file[off] = (uint8_t)(v >> 24);
    g_synth_fifo_file[off + 1u] = (uint8_t)(v >> 16);
    g_synth_fifo_file[off + 2u] = (uint8_t)(v >> 8);
    g_synth_fifo_file[off + 3u] = (uint8_t)v;
}

static void test_synth_fifo_build(void) {
    uint8_t *b;
    if (g_synth_fifo_built) return;
    b = g_synth_fifo_file;
    memcpy(b, "FORM", 4);
    test_synth_fifo_put32(4, 106u);
    memcpy(b + 8, "3INS", 4);
    memcpy(b + 12, "FORM", 4);
    test_synth_fifo_put32(16, 94u);
    memcpy(b + 20, "DSPP", 4);
    memcpy(b + 24, "DHDR", 4);
    test_synth_fifo_put32(28, 16u);
    test_synth_fifo_put32(32, 1u); // function id
    test_synth_fifo_put32(36, 1u); // silicon version (Bulldog floor)
    test_synth_fifo_put32(40, 4u); // format version (DHDR_SUPPORTED)
    test_synth_fifo_put32(44, 0u); // flags
    memcpy(b + 48, "MRSC", 4);
    test_synth_fifo_put32(52, 20u);
    b[56] = 0; // rec[0] unused
    b[57] = 0; // subtype
    b[58] = 0; // flags (not IMPORT: the folio must build, not EXTERNALREF)
    b[59] = 5; // type IN_FIFO
    test_synth_fifo_put32(60, 1u); // many
    test_synth_fifo_put32(64, 0u); // allocated
    test_synth_fifo_put32(68, 0u); // bind_index
    test_synth_fifo_put32(72, 0u); // default value
    memcpy(b + 76, "DNMS", 4);
    test_synth_fifo_put32(80, 7u);
    memcpy(b + 84, "InFIFO", 7); // includes NUL; pad byte at 91 stays 0
    memcpy(b + 92, "DCOD", 4);
    test_synth_fifo_put32(96, 14u);
    test_synth_fifo_put32(100, 0u); // ctype DCOD_RUN_DSPP
    test_synth_fifo_put32(104, 12u); // coff: image right after the header
    test_synth_fifo_put32(108, 1u); // csize: one word
    b[112] = 0x83; // SLEEP
    b[113] = 0x80;
    // Static storage is zero-initial, so the DNMS pad byte at 91 is 0.
    g_synth_fifo_built = 1;
}

// Shadow a bridge path with in-memory bytes, saving any previously cached
// entry (real disc bytes from an earlier block) for restore on removal, so
// disc-backed blocks sharing this process cache are undisturbed. Returns 1
// on success, 0 when the 8-entry cache is full.
static int g_shadow_active = 0;
static uint32_t g_shadow_idx = 0;
static uint8_t *g_shadow_old_data = NULL;
static uint32_t g_shadow_old_size = 0;
static uint32_t g_shadow_old_pos = 0;
static int g_shadow_old_had = 0;

static int test_dsp_shadow_install(const char *path, uint8_t *data,
                                   uint32_t size) {
    uint32_t i;
    if (!path || !data || size == 0u) return 0;
    for (i = 0; i < g_test_dsp_count; ++i) {
        if (strcmp(g_test_dsp_files[i].path, path) == 0) break;
    }
    if (i < g_test_dsp_count) {
        g_shadow_old_had = 1;
        g_shadow_old_data = g_test_dsp_files[i].data;
        g_shadow_old_size = g_test_dsp_files[i].size;
        g_shadow_old_pos = g_test_dsp_files[i].pos;
        g_test_dsp_files[i].data = data;
        g_test_dsp_files[i].size = size;
        g_test_dsp_files[i].pos = 0;
        g_shadow_idx = i;
    } else {
        if (g_test_dsp_count >= TEST_DSP_RAW_CAP) return 0;
        g_shadow_old_had = 0;
        g_shadow_old_data = NULL;
        g_shadow_idx = g_test_dsp_count++;
        g_test_dsp_files[g_shadow_idx].data = data;
        g_test_dsp_files[g_shadow_idx].size = size;
        g_test_dsp_files[g_shadow_idx].pos = 0;
        snprintf(g_test_dsp_files[g_shadow_idx].path,
                 sizeof(g_test_dsp_files[g_shadow_idx].path), "%s", path);
    }
    g_shadow_active = 1;
    return 1;
}

static void test_dsp_shadow_remove(void) {
    uint32_t i;
    if (!g_shadow_active) return;
    g_shadow_active = 0;
    if (g_shadow_old_had) {
        g_test_dsp_files[g_shadow_idx].data = g_shadow_old_data;
        g_test_dsp_files[g_shadow_idx].size = g_shadow_old_size;
        g_test_dsp_files[g_shadow_idx].pos = g_shadow_old_pos;
        g_shadow_old_data = NULL;
        return;
    }
    // Appended slot: shift the tail down (later opens only append, so this
    // is normally the last entry, but the loop keeps it exact regardless).
    for (i = g_shadow_idx; i + 1u < g_test_dsp_count; ++i)
        g_test_dsp_files[i] = g_test_dsp_files[i + 1u];
    --g_test_dsp_count;
}

static void write_guest_string(uint32_t ea, const char *s) {
    size_t len = strlen(s);
    for (size_t i = 0; i <= len; ++i)
        g_mem[ea + i] = (uint8_t)s[i];
}

// R1 resident-mix seam (defined in recomp_runtime.c; the shared audio
// header is outside this test's ownership, so declare it here).
void m2_guest_audio_read_resident_mix(int32_t stereo_out[2]);
void m2_guest_audio_tick_dspp_frames(uint32_t frames);
// Trigger -> Cue bridge (defined in recomp_runtime.c; same ownership rule).
int32_t m2_guest_audio_arm_trigger(PPCContext *cpu, uint32_t trignum,
                                   uint32_t cue_item, uint32_t flags);
int32_t m2_guest_audio_arm_trigger_named(PPCContext *cpu,
                                         uint32_t instrument,
                                         uint32_t trigger_name_ea,
                                         uint32_t cue_item,
                                         uint32_t flags);
int m2_audio_dispatch_c(PPCContext *c, uint32_t ordinal, uint32_t pc);
void m2_guest_audio_graph_lock(void);
void m2_guest_audio_graph_unlock(void);

typedef struct {
    PPCContext cpu;
    uint32_t knob;
    uint32_t completed;
    uint32_t failures;
} audio_setter_fixture;

static void *audio_concurrent_setter(void *arg) {
    audio_setter_fixture *fixture = arg;
    for (uint32_t n = 0; n < 1024u; ++n) {
        m2_guest_audio_graph_lock();
        fixture->cpu.gpr[3] = fixture->knob;
        fixture->cpu.gpr[4] = 0;
        fixture->cpu.fpr[1] = 0.25f;
        if (m2_audio_dispatch_c(&fixture->cpu, 70u, 0x61048u) != 1 ||
            fixture->cpu.gpr[3] != M2K_OK)
            ++fixture->failures;
        ++fixture->completed;
        m2_guest_audio_graph_unlock();
    }
    return NULL;
}
int32_t m2_guest_audio_raise_trigger(PPCContext *cpu, uint32_t trignum);

// ---- DSPP trigger -> Cue test state -------------------------------------------
// Staged observation without blocking waits: the task records polled signal
// state at two main-driven stages, so a broken bridge fails loudly instead
// of hanging the suite in WaitSignal/join.
static uint32_t g_trig_cue;
static uint32_t g_trig_signal;
static uint32_t g_trig_flags;
static _Atomic int g_trig_ready;
static _Atomic int g_trig_stage1;
static _Atomic int g_trig_stage2;
static _Atomic int g_trig_mid_seen;
static int32_t g_trig_poll_mid;
static int32_t g_trig_poll_end;
static uint32_t g_trig_named_instrument;
static uint32_t g_trig_named_name_ea;

static void trig_reset_stages(void) {
    g_trig_cue = 0;
    g_trig_signal = 0;
    g_trig_flags = 0;
    atomic_store_explicit(&g_trig_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&g_trig_stage1, 0, memory_order_relaxed);
    atomic_store_explicit(&g_trig_stage2, 0, memory_order_relaxed);
    atomic_store_explicit(&g_trig_mid_seen, 0, memory_order_relaxed);
    g_trig_poll_mid = -1;
    g_trig_poll_end = -1;
    g_trig_named_instrument = 0;
    g_trig_named_name_ea = 0;
}

static int trig_spin_until(_Atomic int *flag) {
    for (int i = 0; i < 10000 &&
                    !atomic_load_explicit(flag, memory_order_acquire); ++i)
        sched_yield();
    return atomic_load_explicit(flag, memory_order_acquire) != 0;
}
// Create a cue and arm it on the arg bit with the staged flags, then report
// polled signal state at both stages.
static int32_t trigger_armed_task(m2k_context *ctx, uint32_t arg) {
    (void)ctx;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_trig_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_trig_signal = m2_guest_audio_get_cue_signal(&g_cpu, g_trig_cue);
    if (g_trig_cue == M2K_ITEM_NONE || g_trig_signal == 0u) return -1;
    if (m2_guest_audio_arm_trigger(&g_cpu, arg, g_trig_cue, g_trig_flags) !=
        M2K_OK)
        return -2;
    atomic_store_explicit(&g_trig_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage1, memory_order_acquire))
        sched_yield();
    g_trig_poll_mid = m2k_poll_signal(&g_kctx, g_trig_signal);
    atomic_store_explicit(&g_trig_mid_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage2, memory_order_acquire))
        sched_yield();
    g_trig_poll_end = m2k_poll_signal(&g_kctx, g_trig_signal);
    return M2K_OK;
}

// Selector-shaped ArmTrigger task. The named path must resolve the Trigger
// resource allocated by the loaded schmidt_trigger.dsp template; it must not
// accept a caller-supplied raw bit as production code does not expose one.
static int32_t trigger_named_task(m2k_context *ctx, uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_trig_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_trig_signal = m2_guest_audio_get_cue_signal(&g_cpu, g_trig_cue);
    if (g_trig_cue == M2K_ITEM_NONE || g_trig_signal == 0u) return -1;
    g_cpu.gpr[3] = g_trig_named_instrument;
    g_cpu.gpr[4] = g_trig_named_name_ea;
    g_cpu.gpr[5] = g_trig_cue;
    g_cpu.gpr[6] = g_trig_flags;
    if (m2_audio_dispatch_c(&g_cpu, 34, 0x123456u) != 1 ||
        (int32_t)g_cpu.gpr[3] != M2K_OK)
        return -2;
    atomic_store_explicit(&g_trig_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage1, memory_order_acquire))
        sched_yield();
    g_trig_poll_mid = m2k_poll_signal(&g_kctx, g_trig_signal);
    atomic_store_explicit(&g_trig_mid_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage2, memory_order_acquire))
        sched_yield();
    g_trig_poll_end = m2k_poll_signal(&g_kctx, g_trig_signal);
    return M2K_OK;
}

// Create a cue but leave the arg bit unarmed; main arms it late (or never).
static int32_t trigger_unarmed_task(m2k_context *ctx, uint32_t arg) {
    (void)ctx;
    (void)arg;
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    g_trig_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    g_trig_signal = m2_guest_audio_get_cue_signal(&g_cpu, g_trig_cue);
    if (g_trig_cue == M2K_ITEM_NONE || g_trig_signal == 0u) return -1;
    atomic_store_explicit(&g_trig_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage1, memory_order_acquire))
        sched_yield();
    g_trig_poll_mid = m2k_poll_signal(&g_kctx, g_trig_signal);
    atomic_store_explicit(&g_trig_mid_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&g_trig_stage2, memory_order_acquire))
        sched_yield();
    g_trig_poll_end = m2k_poll_signal(&g_kctx, g_trig_signal);
    return M2K_OK;
}

// R4.5: number of host voice slots currently playing a sample. A stranded
// race-audio channel (the persistent-drone fingerprint) shows up here as an
// extra live voice that no StopInstrument/ReleaseInstrument retired.
static int race_live_voice_count(void) {
    int live = 0;
    for (int i = 0; i < 32; ++i)
        if (g_voice_sids[i] != 0u) ++live;
    return live;
}

// ---- Audio Folio Behavioral Equivalence Gate -------------------------------
// Validates:
// (1) Start tag parsing & math per vendor portfolio_os_m2 audio.h / dspp_instrument.c:
//     - AMPLITUDE_FP, VELOCITY, SQUARE_VELOCITY, EXPONENTIAL_VELOCITY (+SCALAR)
//     - SAMPLE_RATE_FP, RATE_FP, PITCH (MIDI note -> 440*2^((n-69)/12))
// (2) Conflict detection: M2_AUDIO_ERR_TAGCONFLICT on duplicate amplitude or frequency tags
// (3) Invalid tag rejection: M2_AUDIO_ERR_BAD_TAG
// (4) Untagged knob retention across Start / Stop / Start
// (5) Attachment selection:
//     - AF_ATTF_NOAUTOSTART filtering at StartInstrument time
//     - ScanMultiSamples note/velocity range matching per FIFO hook
//     - Direct StartAttachment execution for NOAUTOSTART attachments
// (6) PauseInstrument (6:45) and ResumeInstrument (6:44) lifecycle:
//     - M2K_OK on stopped instrument (ains_Status <= AF_STOPPED)
//     - Pause sets paused state, pauses host voices, silences taps
//     - Resume clears paused state, resumes host voices, restores taps
//     - Bad item handle returns M2_AUDIO_ERR_BAD_ITEM
static void test_phase1_audio_behavioral_equivalence(void) {
    setup();
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    m2_kernel_dispatch_set_global(hdl);

    // 1. Create a base sample with note/velocity bounds:
    // Sample A: low_note=0, high_note=60, low_vel=0, high_vel=127, rate=22050
    const uint32_t sA_tags = 0xA000u;
    const uint32_t sA_data = 0xA100u;
    for (uint32_t i = 0; i < 16u; ++i) g_mem[sA_data + i] = (uint8_t)(i + 1);
    float rA = 22050.0f;
    uint32_t rA_bits; memcpy(&rA_bits, &rA, 4);
    wr32(&g_cpu, sA_tags + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sA_tags + 4u, sA_data);
    wr32(&g_cpu, sA_tags + 8u, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, sA_tags + 12u, rA_bits);
    wr32(&g_cpu, sA_tags + 16u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sA_tags + 20u, 16u);
    wr32(&g_cpu, sA_tags + 24u, M2_AUDIO_TAG_LOWNOTE);
    wr32(&g_cpu, sA_tags + 28u, 0u);
    wr32(&g_cpu, sA_tags + 32u, M2_AUDIO_TAG_HIGHNOTE);
    wr32(&g_cpu, sA_tags + 36u, 60u);
    wr32(&g_cpu, sA_tags + 40u, M2_AUDIO_TAG_LOWVELOCITY);
    wr32(&g_cpu, sA_tags + 44u, 0u);
    wr32(&g_cpu, sA_tags + 48u, M2_AUDIO_TAG_HIGHVELOCITY);
    wr32(&g_cpu, sA_tags + 52u, 127u);
    wr32(&g_cpu, sA_tags + 56u, M2_TAG_END);
    wr32(&g_cpu, sA_tags + 60u, 0u);
    uint32_t sampleA = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, sA_tags);
    CHECK(sampleA != M2K_ITEM_NONE);

    // Verify GetAudioItemInfo on sampleA
    const uint32_t get_tags_ea = 0xA200u;
    wr32(&g_cpu, get_tags_ea + 0u, M2_AUDIO_TAG_LOWNOTE);
    wr32(&g_cpu, get_tags_ea + 4u, 0xFFFFFFFFu);
    wr32(&g_cpu, get_tags_ea + 8u, M2_AUDIO_TAG_HIGHNOTE);
    wr32(&g_cpu, get_tags_ea + 12u, 0xFFFFFFFFu);
    wr32(&g_cpu, get_tags_ea + 16u, M2_AUDIO_TAG_LOWVELOCITY);
    wr32(&g_cpu, get_tags_ea + 20u, 0xFFFFFFFFu);
    wr32(&g_cpu, get_tags_ea + 24u, M2_AUDIO_TAG_HIGHVELOCITY);
    wr32(&g_cpu, get_tags_ea + 28u, 0xFFFFFFFFu);
    wr32(&g_cpu, get_tags_ea + 32u, M2_TAG_END);
    wr32(&g_cpu, get_tags_ea + 36u, 0u);
    g_cpu.gpr[3] = sampleA;
    g_cpu.gpr[4] = get_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rd32(&g_cpu, get_tags_ea + 4u) == 0u);
    CHECK(rd32(&g_cpu, get_tags_ea + 12u) == 60u);
    CHECK(rd32(&g_cpu, get_tags_ea + 20u) == 0u);
    CHECK(rd32(&g_cpu, get_tags_ea + 28u) == 127u);

    // Sample B: note 61..127
    const uint32_t sB_tags = 0xA300u;
    const uint32_t sB_data = 0xA400u;
    for (uint32_t i = 0; i < 16u; ++i) g_mem[sB_data + i] = (uint8_t)(i + 1);
    float rB = 44100.0f;
    uint32_t rB_bits; memcpy(&rB_bits, &rB, 4);
    wr32(&g_cpu, sB_tags + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sB_tags + 4u, sB_data);
    wr32(&g_cpu, sB_tags + 8u, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, sB_tags + 12u, rB_bits);
    wr32(&g_cpu, sB_tags + 16u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sB_tags + 20u, 16u);
    wr32(&g_cpu, sB_tags + 24u, M2_AUDIO_TAG_LOWNOTE);
    wr32(&g_cpu, sB_tags + 28u, 61u);
    wr32(&g_cpu, sB_tags + 32u, M2_AUDIO_TAG_HIGHNOTE);
    wr32(&g_cpu, sB_tags + 36u, 127u);
    wr32(&g_cpu, sB_tags + 40u, M2_TAG_END);
    wr32(&g_cpu, sB_tags + 44u, 0u);
    uint32_t sampleB = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, sB_tags);
    CHECK(sampleB != M2K_ITEM_NONE);

    // Create instrument using sampler_16_f2.dsp
    const uint32_t ins_name_ea = 0xA500u;
    write_guest_string(ins_name_ea, "sampler_16_f2.dsp");
    g_cpu.gpr[3] = ins_name_ea;
    g_cpu.gpr[4] = 1u;
    g_cpu.gpr[5] = 100u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 27, 0x29614u) == 1);
    const int32_t ins = (int32_t)g_cpu.gpr[3];
    CHECK(ins > 0);

    // Attach sampleA and sampleB to ins on the SAME FIFO hook ("FIFO")
    const uint32_t attA_tags = 0xA600u;
    const uint32_t hook_ea = 0xA680u;
    write_guest_string(hook_ea, "FIFO");
    wr32(&g_cpu, attA_tags + 0u, M2_AUDIO_TAG_NAME);
    wr32(&g_cpu, attA_tags + 4u, hook_ea);
    wr32(&g_cpu, attA_tags + 8u, M2_TAG_END);
    wr32(&g_cpu, attA_tags + 12u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = sampleA;
    g_cpu.gpr[5] = attA_tags;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attA = g_cpu.gpr[3];
    CHECK(attA != M2K_ITEM_NONE);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = sampleB;
    g_cpu.gpr[5] = attA_tags;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attB = g_cpu.gpr[3];
    CHECK(attB != M2K_ITEM_NONE);

    // Test 1: Tag conflict - amplitude + velocity
    const uint32_t conflict_tags_ea = 0xA700u;
    float amp_val = 0.5f;
    uint32_t amp_bits; memcpy(&amp_bits, &amp_val, 4);
    wr32(&g_cpu, conflict_tags_ea + 0u, M2_AUDIO_TAG_AMPLITUDE_FP);
    wr32(&g_cpu, conflict_tags_ea + 4u, amp_bits);
    wr32(&g_cpu, conflict_tags_ea + 8u, M2_AUDIO_TAG_VELOCITY);
    wr32(&g_cpu, conflict_tags_ea + 12u, 64u);
    wr32(&g_cpu, conflict_tags_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, conflict_tags_ea + 20u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = conflict_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_TAGCONFLICT);

    // Test 2: Frequency conflict - sample_rate_fp + rate_fp
    float rate_val = 1.5f;
    uint32_t rate_bits; memcpy(&rate_bits, &rate_val, 4);
    wr32(&g_cpu, conflict_tags_ea + 0u, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, conflict_tags_ea + 4u, amp_bits);
    wr32(&g_cpu, conflict_tags_ea + 8u, M2_AUDIO_TAG_RATE_FP);
    wr32(&g_cpu, conflict_tags_ea + 12u, rate_bits);
    wr32(&g_cpu, conflict_tags_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, conflict_tags_ea + 20u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = conflict_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_TAGCONFLICT);

    // Test 3: Bad tag (0x9999) -> M2_AUDIO_ERR_BAD_TAG
    const uint32_t bad_tags_ea = 0xA780u;
    wr32(&g_cpu, bad_tags_ea + 0u, 0x9999u);
    wr32(&g_cpu, bad_tags_ea + 4u, 0u);
    wr32(&g_cpu, bad_tags_ea + 8u, M2_TAG_END);
    wr32(&g_cpu, bad_tags_ea + 12u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = bad_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_TAG);

    // Test 4: Multisample ScanMultiSamples - Note=40 -> selects sampleA (low_note 0..60)
    const uint32_t start_tags_note40 = 0xA800u;
    wr32(&g_cpu, start_tags_note40 + 0u, M2_AUDIO_TAG_PITCH);
    wr32(&g_cpu, start_tags_note40 + 4u, 40u); // pitch note 40
    wr32(&g_cpu, start_tags_note40 + 8u, M2_AUDIO_TAG_AMPLITUDE_FP);
    wr32(&g_cpu, start_tags_note40 + 12u, amp_bits); // gain 0.5
    wr32(&g_cpu, start_tags_note40 + 16u, M2_TAG_END);
    wr32(&g_cpu, start_tags_note40 + 20u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = start_tags_note40;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    int live_v = 0;
    int live_idx = -1;
    for (int i = 0; i < 32; ++i) {
        if (g_voice_sids[i] != 0) {
            ++live_v;
            live_idx = i;
        }
    }
    CHECK(live_v == 1);
    CHECK(live_idx >= 0);
    float exp_pitch_freq = 440.0f * powf(2.0f, (40.0f - 69.0f) / 12.0f);
    CHECK(fabsf(g_started_rate_hz - exp_pitch_freq) < 0.1f);
    CHECK(fabsf(g_started_gain - 0.5f) < 0.001f);

    // Test 5: PauseInstrument & ResumeInstrument on running instrument
    CHECK(g_voice_paused[live_idx] == 0);
    g_cpu.gpr[3] = (uint32_t)ins;
    CHECK(m2_audio_dispatch_c(&g_cpu, 45, 0x70350u) == 1); // PauseInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_voice_paused[live_idx] == 1);

    // Tap frame while paused returns 0
    int16_t tap_l = 999, tap_r = 999;
    g_tap_script_l[0] = 1234; g_tap_script_r[0] = 5678;
    g_tap_script_n = 1; g_tap_calls = 0;
    CHECK(m2_host_audio_voice_tap_frame((uint32_t)live_idx, &tap_l, &tap_r) == 0);
    CHECK(tap_l == 0 && tap_r == 0);

    // ResumeInstrument
    g_cpu.gpr[3] = (uint32_t)ins;
    CHECK(m2_audio_dispatch_c(&g_cpu, 44, 0x703a0u) == 1); // ResumeInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_voice_paused[live_idx] == 0);

    // Tap frame while resumed returns sample
    g_tap_calls = 0;
    CHECK(m2_host_audio_voice_tap_frame((uint32_t)live_idx, &tap_l, &tap_r) == 1);
    CHECK(tap_l == 1234 && tap_r == 5678);

    // Stop instrument
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1); // StopInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // Test 6: Untagged start preserves knobs:
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u; // NULL taglist
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(fabsf(g_started_gain - 0.5f) < 0.001f);
    CHECK(fabsf(g_started_rate_hz - exp_pitch_freq) < 0.1f);

    // Stop instrument
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // Test 7: Multisample ScanMultiSamples - Note=80 -> selects sampleB (low_note 61..127)
    const uint32_t start_tags_note80 = 0xA880u;
    wr32(&g_cpu, start_tags_note80 + 0u, M2_AUDIO_TAG_PITCH);
    wr32(&g_cpu, start_tags_note80 + 4u, 80u); // pitch note 80
    wr32(&g_cpu, start_tags_note80 + 8u, M2_TAG_END);
    wr32(&g_cpu, start_tags_note80 + 12u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = start_tags_note80;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    float exp_pitch80 = 440.0f * powf(2.0f, (80.0f - 69.0f) / 12.0f);
    CHECK(fabsf(g_started_rate_hz - exp_pitch80) < 0.1f);

    // Stop instrument
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // Test 8: AF_ATTF_NOAUTOSTART attachment filtering:
    g_cpu.gpr[3] = ins_name_ea;
    g_cpu.gpr[4] = 1u;
    g_cpu.gpr[5] = 100u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 27, 0x29614u) == 1);
    const int32_t ins_noauto = (int32_t)g_cpu.gpr[3];
    CHECK(ins_noauto > 0);

    const uint32_t noauto_att_tags = 0xA900u;
    wr32(&g_cpu, noauto_att_tags + 0u, M2_AUDIO_TAG_SET_FLAGS);
    wr32(&g_cpu, noauto_att_tags + 4u, M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
    wr32(&g_cpu, noauto_att_tags + 8u, M2_TAG_END);
    wr32(&g_cpu, noauto_att_tags + 12u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins_noauto;
    g_cpu.gpr[4] = sampleA;
    g_cpu.gpr[5] = noauto_att_tags;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t att_noauto = g_cpu.gpr[3];
    CHECK(att_noauto != M2K_ITEM_NONE);

    // StartInstrument on ins_noauto: NOAUTOSTART prevents auto-starting voice, returns M2K_OK
    g_cpu.gpr[3] = (uint32_t)ins_noauto;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    int noauto_live = 0;
    for (int i = 0; i < 32; ++i) {
        if (g_voice_sids[i] != 0) ++noauto_live;
    }
    CHECK(noauto_live == 0);

    // Now call StartAttachment directly on att_noauto: starts 1 voice!
    g_cpu.gpr[3] = att_noauto;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_guest_audio_start_attachment(&g_cpu, att_noauto, 0) == M2K_OK);
    int direct_live = 0;
    int direct_idx = -1;
    for (int i = 0; i < 32; ++i) {
        if (g_voice_sids[i] != 0) {
            ++direct_live;
            direct_idx = i;
        }
    }
    CHECK(direct_live == 1);
    CHECK(direct_idx >= 0);

    // PauseInstrument pauses this voice started via StartAttachment
    g_cpu.gpr[3] = (uint32_t)ins_noauto;
    CHECK(m2_audio_dispatch_c(&g_cpu, 45, 0x70350u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_voice_paused[direct_idx] == 1);

    // StopInstrument stops it
    g_cpu.gpr[3] = (uint32_t)ins_noauto;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_voice_sids[direct_idx] == 0);

    // Test 9: Velocity tags (square velocity, exponential velocity)
    const uint32_t vel_tags_ea = 0xAA00u;
    // Square velocity: vel=64 -> amp = (64/127)^2 ~ 0.253968
    wr32(&g_cpu, vel_tags_ea + 0u, M2_AUDIO_TAG_SQUARE_VELOCITY);
    wr32(&g_cpu, vel_tags_ea + 4u, 64u);
    wr32(&g_cpu, vel_tags_ea + 8u, M2_TAG_END);
    wr32(&g_cpu, vel_tags_ea + 12u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = vel_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    float exp_sq_vel = (64.0f / 127.0f) * (64.0f / 127.0f);
    CHECK(fabsf(g_started_gain - exp_sq_vel) < 0.01f);

    // Stop instrument
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // Exponential velocity: vel=100, scalar=0.03 -> amp = 2^((100-127)*0.03) = 2^(-0.81) ~ 0.570378
    float scalar = 0.03f;
    uint32_t scalar_bits; memcpy(&scalar_bits, &scalar, 4);
    wr32(&g_cpu, vel_tags_ea + 0u, M2_AUDIO_TAG_EXPONENTIAL_VELOCITY);
    wr32(&g_cpu, vel_tags_ea + 4u, 100u);
    wr32(&g_cpu, vel_tags_ea + 8u, M2_AUDIO_TAG_EXP_VELOCITY_SCALAR);
    wr32(&g_cpu, vel_tags_ea + 12u, scalar_bits);
    wr32(&g_cpu, vel_tags_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, vel_tags_ea + 20u, 0u);

    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = vel_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    float exp_exp_vel = powf(2.0f, (100.0f - 127.0f) * 0.03f);
    CHECK(fabsf(g_started_gain - exp_exp_vel) < 0.01f);

    // Stop instrument
    g_cpu.gpr[3] = (uint32_t)ins;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    teardown();
}

static void test_phase2_audio_behavioral_equivalence(void) {
    setup();
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    m2_kernel_dispatch_set_global(hdl);

    // =========================================================================
    // Part 1: Connection Graph Lifecycle & Strict Error Handling
    // Vectors: ConnectInstrumentParts (6:57), DisconnectInstrumentParts (6:59)
    // =========================================================================
    const uint32_t add_name_ea = 0x8000u;
    const uint32_t output_name_ea = 0x8020u;
    const uint32_t input_a_name_ea = 0x8040u;
    const uint32_t input_b_name_ea = 0x8060u;
    const uint32_t bad_name_ea = 0x8080u;

    write_guest_string(add_name_ea, "add.dsp");
    write_guest_string(output_name_ea, "Output");
    write_guest_string(input_a_name_ea, "InputA");
    write_guest_string(input_b_name_ea, "InputB");
    write_guest_string(bad_name_ea, "NoSuchPort");

    int32_t addA = m2_guest_audio_load_instrument(&g_cpu, add_name_ea, 1, 100);
    int32_t addB = m2_guest_audio_load_instrument(&g_cpu, add_name_ea, 1, 100);
    CHECK(addA > 0 && addB > 0);

    // 1.1: Bad source item -> M2_AUDIO_ERR_BAD_ITEM
    g_cpu.gpr[3] = 0xDEADBEEFu;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // 1.2: Bad destination item -> M2_AUDIO_ERR_BAD_ITEM
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = 0xDEADBEEFu;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // 1.3: Bad name pointer (> RAM) -> M2_AUDIO_ERR_BAD_PTR
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = RECOMP_GUEST_MEM + 4u;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_PTR);

    // 1.4: Invalid source port name -> M2_AUDIO_ERR_NAME_MISSING
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = bad_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NAME_MISSING);

    // 1.5: Source port is not an OUTPUT (e.g. InputA used as source) -> M2_AUDIO_ERR_NAME_MISSING
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = input_a_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NAME_MISSING);

    // 1.6: Dest port is not an INPUT/KNOB (e.g. Output used as dest) -> M2_AUDIO_ERR_NAME_MISSING
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = output_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NAME_MISSING);

    // 1.7: Out-of-range part on source -> M2_AUDIO_ERR_OUT_OF_RANGE
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 1; // 1-part Output has only part 0
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);

    // 1.8: Out-of-range part on destination -> M2_AUDIO_ERR_OUT_OF_RANGE
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 1; // 1-part InputA has only part 0
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);

    // 1.9: Valid connection addA.Output[0] -> addB.InputA[0] -> M2K_OK
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // 1.10: Re-connecting to already connected destination -> M2_AUDIO_ERR_IN_USE
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_IN_USE);

    // 1.11: Disconnect on unconnected port -> M2_AUDIO_ERR_BAD_PTR
    g_cpu.gpr[3] = (uint32_t)addB;
    g_cpu.gpr[4] = input_b_name_ea;
    g_cpu.gpr[5] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 59, 0x618bcu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_PTR);

    // 1.12: Disconnect on connected port -> M2K_OK
    g_cpu.gpr[3] = (uint32_t)addB;
    g_cpu.gpr[4] = input_a_name_ea;
    g_cpu.gpr[5] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 59, 0x618bcu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // 1.13: Repeat disconnect on now-disconnected port -> M2_AUDIO_ERR_BAD_PTR
    g_cpu.gpr[3] = (uint32_t)addB;
    g_cpu.gpr[4] = input_a_name_ea;
    g_cpu.gpr[5] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 59, 0x618bcu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_PTR);

    // 1.14: Reconnection succeeds after disconnect -> M2K_OK
    g_cpu.gpr[3] = (uint32_t)addA;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    g_cpu.gpr[6] = (uint32_t)addB;
    g_cpu.gpr[7] = input_a_name_ea;
    g_cpu.gpr[8] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // Clean up addA and addB
    CHECK(m2_guest_audio_unload_instrument((uint32_t)addA) == M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)addB) == M2K_OK);

    // =========================================================================
    // Part 2: Knob Interaction, Defaults & Connection Independence
    // Vectors: SetKnobPart (6:70), ReadKnobPart (6:60)
    // =========================================================================
    const uint32_t mixer_2x1_spec = (2u << 24) | (1u << 16) | 0x01u; // 2 in, 1 out, Amplitude knob
    g_cpu.gpr[3] = mixer_2x1_spec;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 8, 0x291f4u) == 1);
    uint32_t mix_tpl = g_cpu.gpr[3];
    CHECK(mix_tpl > 0);

    g_cpu.gpr[3] = mix_tpl;
    g_cpu.gpr[4] = 0;
    FN_CreateInstrumentVA__7376c(&g_cpu);
    uint32_t mix_ins = g_cpu.gpr[3];
    CHECK(mix_ins > 0);

    const uint32_t gain_name_ea = 0x8100u;
    const uint32_t amp_name_ea = 0x8120u;
    const uint32_t read_val_ea = 0x8140u;
    write_guest_string(gain_name_ea, "Gain");
    write_guest_string(amp_name_ea, "Amplitude");

    g_cpu.gpr[3] = mix_ins;
    g_cpu.gpr[4] = gain_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t gain_knob = g_cpu.gpr[3];
    CHECK(gain_knob > 0);

    g_cpu.gpr[3] = mix_ins;
    g_cpu.gpr[4] = amp_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t amp_knob = g_cpu.gpr[3];
    CHECK(amp_knob > 0);

    // 2.1: Default knob readback before any SetKnobPart (Gain defaults to 0.0, Amplitude defaults to 1.0)
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rdf32(&g_cpu, read_val_ea) == 0.0f);

    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 1; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rdf32(&g_cpu, read_val_ea) == 0.0f);

    g_cpu.gpr[3] = amp_knob; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rdf32(&g_cpu, read_val_ea) == 1.0f);

    // 2.2: SetKnobPart with custom values
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 0; g_cpu.fpr[1] = 0.5f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 1; g_cpu.fpr[1] = -0.25f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    g_cpu.gpr[3] = amp_knob; g_cpu.gpr[4] = 0; g_cpu.fpr[1] = 0.8f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // 2.3: ReadKnobPart reads back exact updated values
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 0.5f) < 1e-5f);

    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 1; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (-0.25f)) < 1e-5f);

    g_cpu.gpr[3] = amp_knob; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 0.8f) < 1e-5f);

    // 2.4: Out of range part on ReadKnobPart -> M2_AUDIO_ERR_OUT_OF_RANGE
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 2; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);

    // 2.5: Bad Item on ReadKnobPart -> M2_AUDIO_ERR_BAD_ITEM
    g_cpu.gpr[3] = 0xDEADBEEFu; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = read_val_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // 2.6: Bad output pointer on ReadKnobPart -> M2_AUDIO_ERR_SECURITY
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 0; g_cpu.gpr[5] = RECOMP_GUEST_MEM + 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x61100u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_SECURITY);

    // 2.7: Out of range part on SetKnobPart -> M2_AUDIO_ERR_OUT_OF_RANGE
    g_cpu.gpr[3] = gain_knob; g_cpu.gpr[4] = 2; g_cpu.fpr[1] = 1.0f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);

    // 2.8: Bad Item on SetKnobPart -> M2_AUDIO_ERR_BAD_ITEM
    g_cpu.gpr[3] = 0xDEADBEEFu; g_cpu.gpr[4] = 0; g_cpu.fpr[1] = 1.0f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // =========================================================================
    // Part 3: Mixer Node Math vs Independent Mathematical Oracle
    // Math: acc = sum((in[i] * gain[i]) >> 15), out = (acc * amp) >> 15
    // =========================================================================
    int32_t src0 = m2_guest_audio_load_instrument(&g_cpu, add_name_ea, 1, 100);
    int32_t src1 = m2_guest_audio_load_instrument(&g_cpu, add_name_ea, 1, 100);
    CHECK(src0 > 0 && src1 > 0);

    g_cpu.gpr[3] = (uint32_t)src0; g_cpu.gpr[4] = input_a_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t src0_knob_a = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)src0; g_cpu.gpr[4] = input_b_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t src0_knob_b = g_cpu.gpr[3];

    g_cpu.gpr[3] = (uint32_t)src1; g_cpu.gpr[4] = input_a_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t src1_knob_a = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)src1; g_cpu.gpr[4] = input_b_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t src1_knob_b = g_cpu.gpr[3];

    // Set src0 = 10000, src1 = 8000
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src0_knob_a, 0, 10000.0f / 32768.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src0_knob_b, 0, 0.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src1_knob_a, 0, 8000.0f / 32768.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src1_knob_b, 0, 0.0f) == M2K_OK);

    CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)src0, 0) == M2K_OK);
    CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)src1, 0) == M2K_OK);

    // Connect src0 -> mix_ins.Input[0], src1 -> mix_ins.Input[1]
    const uint32_t mix_input_name_ea = 0x8160u;
    write_guest_string(mix_input_name_ea, "Input");
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)src0, output_name_ea, 0,
              mix_ins, mix_input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)src1, output_name_ea, 0,
              mix_ins, mix_input_name_ea, 1) == M2K_OK);

    // Start mixer instrument
    CHECK(m2_guest_audio_start_instrument(&g_cpu, mix_ins, 0) == M2K_OK);

    // Create probe on mixer Output[0]
    g_cpu.gpr[3] = mix_ins;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t mix_probe = g_cpu.gpr[3];
    CHECK(mix_probe > 0);

    // Advance 1 frame
    m2_guest_audio_tick_dspp_frames(1u);

    // Theoretical oracle calculation:
    // in0 = 10000, in1 = 8000
    // gain0 = 0.5f (sf15 16384), gain1 = -0.25f (sf15 -8192)
    // amp = 0.8f (sf15 26214)
    // contrib0 = (10000 * 16384) >> 15 = 5000
    // contrib1 = (8000 * -8192) >> 15 = -2000
    // acc = 5000 - 2000 = 3000
    // out = (3000 * 26214) >> 15 = 2400
    // probe_val = 2400 / 32768.0 = 0.0732421875f
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, mix_probe, 0, read_val_ea) == M2K_OK);
    float observed_mix = rdf32(&g_cpu, read_val_ea);
    float expected_mix = 2400.0f / 32768.0f;
    CHECK(fabsf(observed_mix - expected_mix) < 1e-4f);

    // Test Saturation / Saturated Clipping:
    // Set Gain(0) = 1.0f, Gain(1) = 1.0f, Amp = 1.0f
    // Input src0 = 25000, src1 = 20000
    // Sum = 45000 -> saturated to 32767
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain_knob, 0, 1.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain_knob, 1, 1.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, amp_knob, 0, 1.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src0_knob_a, 0, 25000.0f / 32768.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src1_knob_a, 0, 20000.0f / 32768.0f) == M2K_OK);

    m2_guest_audio_tick_dspp_frames(1u);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, mix_probe, 0, read_val_ea) == M2K_OK);
    float observed_sat = rdf32(&g_cpu, read_val_ea);
    float expected_sat = 32767.0f / 32768.0f;
    CHECK(fabsf(observed_sat - expected_sat) < 1e-4f);

    // =========================================================================
    // Part 4: State Variable Filter (svfilter.dsp) Math vs Chamberlin Oracle
    // =========================================================================
    const uint32_t svf_name_ea = 0x8200u;
    write_guest_string(svf_name_ea, "svfilter.dsp");
    int32_t svf = m2_guest_audio_load_instrument(&g_cpu, svf_name_ea, 1, 100);
    CHECK(svf > 0);

    const uint32_t freq_name_ea = 0x8220u;
    const uint32_t res_name_ea = 0x8240u;
    const uint32_t svf_amp_name_ea = 0x8260u;
    const uint32_t svf_in_name_ea = 0x8280u;
    const uint32_t hp_name_ea = 0x82A0u;
    const uint32_t lp_name_ea = 0x82C0u;
    const uint32_t bp_name_ea = 0x82E0u;
    write_guest_string(freq_name_ea, "Frequency");
    write_guest_string(res_name_ea, "Resonance");
    write_guest_string(svf_amp_name_ea, "Amplitude");
    write_guest_string(svf_in_name_ea, "Input");
    write_guest_string(hp_name_ea, "HighPass");
    write_guest_string(lp_name_ea, "LowPass");
    write_guest_string(bp_name_ea, "BandPass");

    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = freq_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t svf_freq = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = res_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t svf_res = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = svf_amp_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t svf_amp = g_cpu.gpr[3];

    // Knobs: Frequency=0.25 (0x2000), Resonance=0.125 (0x1000), Amplitude=1.0 (0x7FFF)
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, svf_freq, 0, 0.25f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, svf_res, 0, 0.125f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, svf_amp, 0, 1.0f) == M2K_OK);

    // Create probes on svfilter outputs
    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = hp_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t probe_hp = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = lp_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t probe_lp = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)svf; g_cpu.gpr[4] = bp_name_ea; g_cpu.gpr[5] = 0;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t probe_bp = g_cpu.gpr[3];
    CHECK(probe_hp > 0 && probe_lp > 0 && probe_bp > 0);

    // Unstarted svfilter: without a built DSP sim, ReadProbePart returns UNIMPLEMENTED
    // and preserves destination memory
    wr32(&g_cpu, read_val_ea, 0xa5a5a5a5u);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2_AUDIO_ERR_UNIMPLEMENTED);
    CHECK(rd32(&g_cpu, read_val_ea) == 0xa5a5a5a5u);

    // Start svfilter
    CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)svf, 0) == M2K_OK);

    // Connect src0 (set to constant 1200) to svfilter Input
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src0_knob_a, 0, 1200.0f / 32768.0f) == M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, src0_knob_b, 0, 0.0f) == M2K_OK);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)src0, output_name_ea, 0,
              (uint32_t)svf, svf_in_name_ea, 0) == M2K_OK);

    // Frame 1 Tick
    m2_guest_audio_tick_dspp_frames(1u);
    // Oracle frame 1:
    // LowPass = 0
    // ACCUME = 0
    // HighPass = 1200 - 0 = 1200
    // BandPass = (0x2000 * 1200) >> 15 = 300
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (1200.0f / 32768.0f)) < 1e-4f);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_lp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 0.0f) < 1e-4f);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_bp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (300.0f / 32768.0f)) < 1e-4f);

    // Frame 2 Tick
    m2_guest_audio_tick_dspp_frames(1u);
    // Oracle frame 2:
    // LowPass = (0x2000 * 300) >> 15 + 0 = 75
    // ACCUME = (0x1000 * 300) >> 15 + 75 = 37 + 75 = 112
    // HighPass = 1200 - 112 = 1088
    // BandPass = (0x2000 * 1088) >> 15 + 300 = 272 + 300 = 572
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (1088.0f / 32768.0f)) < 1e-4f);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_lp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (75.0f / 32768.0f)) < 1e-4f);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_bp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (572.0f / 32768.0f)) < 1e-4f);

    // Lifecycle check: PauseInstrument mutes probe readback to 0.0f
    g_cpu.gpr[3] = (uint32_t)svf;
    CHECK(m2_audio_dispatch_c(&g_cpu, 45, 0x70350u) == 1); // PauseInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, read_val_ea) == 0.0f);

    // ResumeInstrument restores live state
    g_cpu.gpr[3] = (uint32_t)svf;
    CHECK(m2_audio_dispatch_c(&g_cpu, 44, 0x703a0u) == 1); // ResumeInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - (1088.0f / 32768.0f)) < 1e-4f);

    // StopInstrument retains its allocation; the existing inactive-probe
    // observation returns zero without executing the retained DSP program.
    g_cpu.gpr[3] = (uint32_t)svf;
    g_cpu.gpr[4] = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1); // StopInstrument
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, read_val_ea) == 0.0f);
    // svfilter's state words are initialized at allocation, not at start.
    // Restart resumes the source recurrence from frame 2's state.
    CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)svf, 0) == M2K_OK);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 1088.0f / 32768.0f) < 1e-4f);
    m2_guest_audio_tick_dspp_frames(1u);
    // LP = 75 + 572/4 = 218; HP = 1200 - (218 + 572/8) = 910.5.
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_lp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 218.0f / 32768.0f) < 1e-4f);
    CHECK(m2_guest_audio_read_probe_part(&g_cpu, probe_hp, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 910.0f / 32768.0f) < 1e-4f);
    CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)svf, 0) == M2K_OK);

    // =========================================================================
    // Part 5: Control vs Audio Routing Partitioning (envelope.dsp)
    // =========================================================================
    const uint32_t env_name_ea = 0x8300u;
    write_guest_string(env_name_ea, "envelope.dsp");
    int32_t env = m2_guest_audio_load_instrument(&g_cpu, env_name_ea, 1, 100);
    CHECK(env > 0);
    CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)env, 0) == M2K_OK);

    // Connect envelope.dsp Output -> mix_ins Gain (KNOB port)
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)env, output_name_ea, 0,
              mix_ins, gain_name_ea, 0) == M2K_OK);

    // While connected exclusively to a knob, envelope is control-only.
    // Verify ReadKnobPart on Gain(0) still returns the last explicitly set value (1.0f)
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, gain_knob, 0, read_val_ea) == M2K_OK);
    CHECK(fabsf(rdf32(&g_cpu, read_val_ea) - 1.0f) < 1e-5f);

    // Connect envelope.dsp Output to an INPUT port as well (mix_ins.Input[0])
    // Disconnect prior source from mix_ins.Input[0] first
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, mix_ins, mix_input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)env, output_name_ea, 0,
              mix_ins, mix_input_name_ea, 0) == M2K_OK);

    // Clean up all items
    hdl->DeleteItem(mix_probe);
    hdl->DeleteItem(probe_hp);
    hdl->DeleteItem(probe_lp);
    hdl->DeleteItem(probe_bp);
    hdl->DeleteItem(gain_knob);
    hdl->DeleteItem(amp_knob);
    hdl->DeleteItem(svf_freq);
    hdl->DeleteItem(svf_res);
    hdl->DeleteItem(svf_amp);
    hdl->DeleteItem(src0_knob_a);
    hdl->DeleteItem(src0_knob_b);
    hdl->DeleteItem(src1_knob_a);
    hdl->DeleteItem(src1_knob_b);
    m2_guest_audio_unload_instrument((uint32_t)env);
    m2_guest_audio_unload_instrument((uint32_t)svf);
    m2_guest_audio_unload_instrument((uint32_t)src0);
    m2_guest_audio_unload_instrument((uint32_t)src1);
    m2_guest_audio_unload_instrument(mix_ins);
    hdl->DeleteItem(mix_tpl);

    teardown();
}

// Source-backed Audio Folio envelope contract. A single-point sustain loop
// holds the envelope until ReleaseInstrument; the vendor then advances to the
// next segment and applies AF_TAG_TIME_SCALE_FP to that segment's duration.
// Compare two fresh production-path runs so the assertion does not depend on
// a host wall clock or on an unbounded task wait.
static int envelope_release_pump_case(float time_scale) {
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    uint32_t envelope_item = M2K_ITEM_NONE;
    uint32_t attachment = M2K_ITEM_NONE;
    uint32_t monitor_task = M2K_ITEM_NONE;
    int32_t instrument = -1;
    int instrument_started = 0;
    int monitor_joined = 0;
    int release_pumps = -1;

    setup();
    m2_kernel_dispatch_set_global(hdl);

    const uint32_t instrument_name_ea = 0x1C0000u;
    const uint32_t envelope_tags_ea = 0x1C0040u;
    const uint32_t envelope_points_ea = 0x1C0100u;
    const uint32_t attachment_tags_ea = 0x1C0140u;
    const uint32_t release_tags_ea = 0x1C0180u;
    write_guest_string(instrument_name_ea, "envelope.dsp");

    instrument = m2_guest_audio_load_instrument(
        &g_cpu, instrument_name_ea, 1u, 100u);
    CHECK(instrument > 0);
    if (instrument <= 0) goto cleanup;

    // EnvelopeSegment values and durations are guest big-endian float words.
    const float points[3][2] = {
        {0.0f, 0.001f},
        {1.0f, 0.040f},
        {0.0f, 0.000f},
    };
    for (uint32_t i = 0; i < 3u; ++i) {
        uint32_t value_bits = 0;
        uint32_t duration_bits = 0;
        memcpy(&value_bits, &points[i][0], sizeof(value_bits));
        memcpy(&duration_bits, &points[i][1], sizeof(duration_bits));
        wr32(&g_cpu, envelope_points_ea + i * 8u, value_bits);
        wr32(&g_cpu, envelope_points_ea + i * 8u + 4u, duration_bits);
    }
    wr32(&g_cpu, envelope_tags_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, envelope_tags_ea + 4u, envelope_points_ea);
    wr32(&g_cpu, envelope_tags_ea + 8u, M2_AUDIO_TAG_FRAMES);
    wr32(&g_cpu, envelope_tags_ea + 12u, 3u);
    wr32(&g_cpu, envelope_tags_ea + 16u, M2_AUDIO_TAG_SUSTAINBEGIN);
    wr32(&g_cpu, envelope_tags_ea + 20u, 1u);
    wr32(&g_cpu, envelope_tags_ea + 24u, M2_AUDIO_TAG_SUSTAINEND);
    wr32(&g_cpu, envelope_tags_ea + 28u, 1u);
    wr32(&g_cpu, envelope_tags_ea + 32u, M2_AUDIO_TAG_SUSTAINTIME_FP);
    wr32(&g_cpu, envelope_tags_ea + 36u, 0u);
    wr32(&g_cpu, envelope_tags_ea + 40u, M2_TAG_END);
    wr32(&g_cpu, envelope_tags_ea + 44u, 0u);
    envelope_item = hdl->CreateItem(M2_AUDIO_ENVELOPE_CTYPE,
                                     envelope_tags_ea);
    CHECK(envelope_item != M2K_ITEM_NONE);
    if (envelope_item == M2K_ITEM_NONE) goto cleanup;

    // Null attachment tags select the source envelope hook named "Env".
    wr32(&g_cpu, attachment_tags_ea + 0u, M2_TAG_END);
    wr32(&g_cpu, attachment_tags_ea + 4u, 0u);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)instrument;
    g_cpu.gpr[4] = envelope_item;
    g_cpu.gpr[5] = attachment_tags_ea;
    FN_CreateAttachment__73394(&g_cpu);
    attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(attachment != M2K_ITEM_NONE);
    if (attachment == M2K_ITEM_NONE) goto cleanup;

    g_envelope_monitor_target = attachment;
    monitor_task = m2k_create_task(&g_kctx,
                                   envelope_release_monitor_task, 0u);
    CHECK(monitor_task != M2K_ITEM_NONE);
    if (monitor_task == M2K_ITEM_NONE) goto cleanup;
    CHECK(trig_spin_until(&g_envelope_monitor_ready));
    CHECK(g_envelope_monitor_rc == M2K_OK);
    CHECK(g_envelope_monitor_signal != 0u);
    if (g_envelope_monitor_rc != M2K_OK) goto cleanup;

    const int32_t start_rc = m2_guest_audio_start_instrument(
        &g_cpu, (uint32_t)instrument, 0u);
    CHECK(start_rc == M2K_OK);
    instrument_started = 1;
    // Advance through the initial point and into the single-point sustain.
    for (uint32_t i = 0; i < 8u; ++i)
        m2_guest_audio_pump_timers();

    uint32_t time_scale_bits = 0;
    memcpy(&time_scale_bits, &time_scale, sizeof(time_scale_bits));
    wr32(&g_cpu, release_tags_ea + 0u, M2_AUDIO_TAG_TIME_SCALE_FP);
    wr32(&g_cpu, release_tags_ea + 4u, time_scale_bits);
    wr32(&g_cpu, release_tags_ea + 8u, M2_TAG_END);
    wr32(&g_cpu, release_tags_ea + 12u, 0u);
    const int32_t release_rc = m2_guest_audio_release_instrument(
        &g_cpu, (uint32_t)instrument, release_tags_ea);
    CHECK(release_rc == M2K_OK);

    // The fallback AudioClock advances by one host frame quantum per pump
    // when this focused test supplies no rendered-frame count. Stop at the
    // first pending monitor bit, retaining the measured release schedule.
    for (uint32_t i = 1u; i <= 64u; ++i) {
        m2_guest_audio_pump_timers();
        uint32_t pending = 0u;
        CHECK(m2k_task_signal_bits(&g_kctx, monitor_task, &pending) == M2K_OK);
        if ((pending & g_envelope_monitor_signal) != 0u) {
            release_pumps = (int)i;
            break;
        }
    }
    CHECK(release_pumps > 0);

    atomic_store_explicit(&g_envelope_monitor_stage1, 1,
                          memory_order_release);
    CHECK(trig_spin_until(&g_envelope_monitor_seen));
    CHECK(g_envelope_monitor_poll == (int32_t)g_envelope_monitor_signal);
    atomic_store_explicit(&g_envelope_monitor_stage2, 1,
                          memory_order_release);
    CHECK(m2k_join_task(&g_kctx, monitor_task) == M2K_OK);
    monitor_joined = 1;
    CHECK(g_envelope_monitor_poll_end == 0);

cleanup:
    if (monitor_task != M2K_ITEM_NONE && !monitor_joined) {
        atomic_store_explicit(&g_envelope_monitor_stage1, 1,
                              memory_order_release);
        atomic_store_explicit(&g_envelope_monitor_stage2, 1,
                              memory_order_release);
        CHECK(m2k_join_task(&g_kctx, monitor_task) == M2K_OK);
    }
    if (monitor_task != M2K_ITEM_NONE)
        CHECK(m2k_delete_task(&g_kctx, monitor_task) == M2K_OK);
    if (instrument_started)
        CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)instrument,
                                             0u) == M2K_OK);
    if (g_envelope_monitor_cue != M2K_ITEM_NONE)
        hdl->DeleteItem(g_envelope_monitor_cue);
    if (attachment != M2K_ITEM_NONE) hdl->DeleteItem(attachment);
    if (envelope_item != M2K_ITEM_NONE) hdl->DeleteItem(envelope_item);
    if (instrument > 0)
        CHECK(m2_guest_audio_unload_instrument((uint32_t)instrument) == M2K_OK);
    m2_kernel_dispatch_set_global(NULL);
    teardown();
    return release_pumps;
}

static void test_envelope_release_time_scale(void) {
    const int normal_pumps = envelope_release_pump_case(1.0f);
    const int slow_pumps = envelope_release_pump_case(2.0f);
    CHECK(normal_pumps > 0);
    CHECK(slow_pumps > normal_pumps);
}

int main(void) {
    {
        // The DSPP sections run the disc's own templates.
        char probe[1200];
        snprintf(probe, sizeof(probe), "%s/System.m2/Audio/dsp/line_out.dsp",
                 IMSA_TEST_GAME_DIR);
        FILE *templates = fopen(probe, "rb");
        if (!templates) {
            printf("m2_audio_items_test: SKIP (no DSP templates at %s)\n",
                   IMSA_TEST_GAME_DIR);
            return 77;
        }
        fclose(templates);
    }
    const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
    CHECK(hdl != NULL);
    CHECK(m2_is_audio_item_selector(17));
    // R2-full probe (2): open the CALCRATE census gate for the whole run so
    // the first CreateInstrument loads exercise the log body (cap 32,
    // log-only — no contract result depends on it).
    setenv("M2_AUDIO_CALCRATE_DIAG", "1", 1);
    CHECK(m2_is_audio_item_selector(27));
    CHECK(m2_is_audio_item_selector(8));
    CHECK(m2_is_audio_item_selector(32));
    CHECK(m2_is_audio_time_selector(35));
    CHECK(m2_is_audio_frame_count_selector(37));
    CHECK(m2_is_audio_cue_signal_selector(23));
    CHECK(m2_is_audio_status_selector(19));
    CHECK(m2_is_audio_status_selector(34));
    CHECK(m2_is_audio_status_selector(28));
    CHECK(m2_is_audio_probe_selector(38));
    CHECK(m2_is_audio_status_selector(41));
    CHECK(m2_is_audio_status_selector(44));
    CHECK(m2_is_audio_status_selector(45));
    CHECK(m2_is_audio_status_selector(50));
    CHECK(m2_is_audio_status_selector(51));
    CHECK(m2_is_audio_status_selector(54));
    CHECK(m2_is_audio_status_selector(67));
    CHECK(m2_is_audio_status_selector(68));
    CHECK(m2_is_audio_status_selector(59));
    CHECK(m2_is_audio_status_selector(60));
    CHECK(m2_is_audio_status_selector(70));
    CHECK(m2_is_audio_status_selector(58));
    CHECK(m2_is_audio_status_selector(M2_AUDIO_SET_ITEM_INFO_ORDINAL));
    CHECK(!m2_is_generic_audio_stub(59));
    CHECK(!m2_is_generic_audio_stub(51));
    CHECK(!m2_is_generic_audio_stub(54));
    CHECK(!m2_is_generic_audio_stub(67));
    CHECK(!m2_is_generic_audio_stub(68));
    CHECK(!m2_is_generic_audio_stub(60));
    CHECK(!m2_is_generic_audio_stub(17));
    CHECK(!m2_is_generic_audio_stub(27));
    CHECK(!m2_is_generic_audio_stub(8));
    CHECK(!m2_is_generic_audio_stub(32));
    CHECK(!m2_is_generic_audio_stub(35));
    CHECK(!m2_is_generic_audio_stub(37));
    CHECK(!m2_is_generic_audio_stub(23));
    CHECK(!m2_is_generic_audio_stub(19));
    CHECK(!m2_is_generic_audio_stub(28));
    CHECK(!m2_is_generic_audio_stub(38));
    CHECK(!m2_is_generic_audio_stub(41));
    CHECK(!m2_is_generic_audio_stub(44));
    CHECK(!m2_is_generic_audio_stub(45));
    CHECK(!m2_is_generic_audio_stub(50));
    CHECK(!m2_is_generic_audio_stub(58));
    CHECK(M2_AUDIO_ERR_MISMATCH == (int32_t)0xd52bf103u);
    CHECK(M2_AUDIO_ERR_BAD_TAG == (int32_t)0xd52bf002u);
    CHECK(M2_AUDIO_ERR_BAD_RESOURCE_TYPE == (int32_t)0xd52bf10bu);
    CHECK(M2_AUDIO_ERR_NAME_MISSING == (int32_t)0xd52bf105u);
    CHECK(M2_AUDIO_ERR_IN_USE == (int32_t)0xd52bf10fu);
    CHECK(M2_AUDIO_ERR_OUT_OF_RANGE == (int32_t)0xd52bf117u);
    CHECK(M2_AUDIO_ERR_SECURITY == (int32_t)0xd52bf11bu);
    CHECK(M2_AUDIO_ERR_NOT_OWNER == (int32_t)0xd52bf012u);

    test_phase1_audio_behavioral_equivalence();
    test_phase2_audio_behavioral_equivalence();
    test_envelope_release_time_scale();

    setup();

    // GetAudioTime is a value-returning no-argument selector. The original
    // global clock uses 184 DSP sample frames per tick; the test provider
    // supplies the host stream's fixed-width 44.1 kHz frame counter.
    g_cpu.gpr[3] = 0xdeadbeefu;
    g_cpu.gpr[4] = 0xcafebabeu;
    CHECK(m2_audio_dispatch_c(&g_cpu, 35, 0x67a98u) == 1);
    CHECK(g_cpu.gpr[3] == 0u);
    g_audio_test_frames = 183u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 35, 0x67a98u) == 1);
    CHECK(g_cpu.gpr[3] == 0u);
    g_audio_test_frames = 184u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 35, 0x67a98u) == 1);
    CHECK(g_cpu.gpr[3] == 1u);
    g_audio_test_frames = 367u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 35, 0x67a98u) == 1);
    CHECK(g_cpu.gpr[3] == 1u);
    g_audio_test_frames = 368u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 35, 0x67a98u) == 1);
    CHECK(g_cpu.gpr[3] == 2u);

    // GetAudioFrameCount has the same no-argument register boundary, but its
    // source value is the 32-bit sample-frame count rather than AudioTime.
    g_cpu.gpr[3] = 0xdeadbeefu;
    g_cpu.gpr[4] = 0xcafebabeu;
    g_audio_test_frames = 0u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 37, 0x2b7b4u) == 1);
    CHECK(g_cpu.gpr[3] == 0u);
    g_audio_test_frames = 0x12345678u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 37, 0x2b7b4u) == 1);
    CHECK(g_cpu.gpr[3] == 0x12345678u);
    g_audio_test_frames = 0xffffffffu;
    CHECK(m2_audio_dispatch_c(&g_cpu, 37, 0x6c100u) == 1);
    CHECK(g_cpu.gpr[3] == 0xffffffffu);

    // GetAudioResourceInfo receives (info EA, sizeof info, resource type) at
    // the four sndPrintDSPInfo call sites. The source accepts resource types
    // 0..4 and rejects others before copying the result. The local bridge has
    // no DSP allocator totals, so valid bounded queries leave the result
    // untouched and stop at the explicit unresolved boundary.
    const uint32_t resource_info_ea = 0x6500u;
    wr32(&g_cpu, resource_info_ea + 0, 0xaaaaaaaau);
    wr32(&g_cpu, resource_info_ea + 4, 0xbbbbbbbbu);
    wr32(&g_cpu, resource_info_ea + 8, 0xccccccccu);
    for (uint32_t resource_type = 0;
         resource_type < M2_AUDIO_RESOURCE_TYPE_COUNT; ++resource_type) {
        g_cpu.gpr[3] = resource_info_ea;
        g_cpu.gpr[4] = 12u;
        g_cpu.gpr[5] = resource_type;
        CHECK(m2_audio_dispatch_c(&g_cpu, 19, 0x29118u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(rd32(&g_cpu, resource_info_ea + 0) == 0xaaaaaaaau);
        CHECK(rd32(&g_cpu, resource_info_ea + 4) == 0xbbbbbbbbu);
        CHECK(rd32(&g_cpu, resource_info_ea + 8) == 0xccccccccu);
    }
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = 12u;
    g_cpu.gpr[5] = M2_AUDIO_RESOURCE_TYPE_COUNT;
    CHECK(m2_audio_dispatch_c(&g_cpu, 19, 0x29118u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_RESOURCE_TYPE);
    g_cpu.gpr[3] = RECOMP_GUEST_MEM - 4u;
    g_cpu.gpr[4] = 12u;
    g_cpu.gpr[5] = M2_AUDIO_RESOURCE_TYPE_TICKS;
    CHECK(m2_audio_dispatch_c(&g_cpu, 19, 0x29118u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_PTR);

    // CreateMixerTemplate decodes the source MixerSpec and returns a template
    // Item.  The following CreateInstrument-shaped VA call proves the
    // template can cross the existing kernel CreateItemVA boundary without
    // becoming a fabricated pointer.
    const uint32_t mixer_2x1 = (2u << 24) | (1u << 16);
    g_cpu.gpr[3] = mixer_2x1;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 8, 0x291f4u) == 1);
    int32_t mixer = (int32_t)g_cpu.gpr[3];
    CHECK(mixer > 0);
    CHECK(m2k_handle_type((uint32_t)mixer) == 8u);
    CHECK(hdl->CheckItem((uint32_t)mixer) == 0);
    uint32_t mixer_instrument = hdl->CreateItemVA(
        M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)mixer,
        M2_TAG_JUMP, 0, 0, 0, 0);
    CHECK(mixer_instrument != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(mixer_instrument) == 0);
    hdl->DeleteItem(mixer_instrument);
    hdl->DeleteItem((uint32_t)mixer);
    CHECK(hdl->CheckItem((uint32_t)mixer) != 0);

    // The production VA alias is a local DECVASTUB_1 thunk. It must rebuild
    // the guest varargs window, enter CreateInstrument, and return a real
    // instrument Item rather than leaving the mixer template in r3.
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = mixer_2x1;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 8, 0x291f4u) == 1);
    uint32_t va_mixer = g_cpu.gpr[3];
    uint32_t initial_sp = g_cpu.gpr[1];
    CHECK(hdl->CheckItem(va_mixer) == 0);
    g_cpu.gpr[3] = va_mixer;
    g_cpu.gpr[4] = 0;
    FN_CreateInstrumentVA__7376c(&g_cpu);
    uint32_t va_instrument = g_cpu.gpr[3];
    CHECK(va_instrument != 0 && va_instrument != va_mixer);
    CHECK(m2k_handle_type(va_instrument) == 8u);
    CHECK(hdl->CheckItem(va_instrument) == 0);
    CHECK(g_cpu.gpr[1] == initial_sp);
    CHECK(m2_guest_audio_unload_instrument(va_instrument) == M2K_OK);
    CHECK(hdl->CheckItem(va_mixer) != 0);
    m2_kernel_dispatch_set_global(NULL);

    // The folio accepts one optional TAG_ITEM_NAME and rejects unknown tags;
    // retain the name EA for FindItem without storing a host pointer.
    const uint32_t mixer_name_ea = 0x5200u;
    const uint32_t mixer_tags_ea = 0x5300u;
    write_guest_string(mixer_name_ea, "guest-mixer");
    wr32(&g_cpu, mixer_tags_ea + 0, 1u);
    wr32(&g_cpu, mixer_tags_ea + 4, mixer_name_ea);
    wr32(&g_cpu, mixer_tags_ea + 8, M2_TAG_END);
    wr32(&g_cpu, mixer_tags_ea + 12, 0);
    int32_t named_mixer = m2_guest_audio_create_mixer_template(
        &g_cpu, (2u << 24) | (2u << 16) | 2u, mixer_tags_ea);
    CHECK(named_mixer > 0);
    CHECK(hdl->FindItem(M2_AUDIO_TEMPLATE_CTYPE, mixer_name_ea) ==
          (uint32_t)named_mixer);
    hdl->DeleteItem((uint32_t)named_mixer);

    // Source validation: inputs 1..32, outputs 1..8, flags 0x01/0x02, and
    // line-out only with two outputs.
    CHECK(m2_guest_audio_create_mixer_template(
              &g_cpu, 0x00010000u, 0) == M2K_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_create_mixer_template(
              &g_cpu, (33u << 24) | (1u << 16), 0) == M2K_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_create_mixer_template(
              &g_cpu, (1u << 24) | (9u << 16), 0) == M2K_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_create_mixer_template(
              &g_cpu, (2u << 24) | (1u << 16) | 4u, 0) == M2K_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_create_mixer_template(
              &g_cpu, (2u << 24) | (1u << 16) | 2u, 0) == M2K_ERR_BAD_ITEM);
    teardown();

    setup();
    const uint32_t name_ea = 0x5000u;
    write_guest_string(name_ea, "sampler_16_f2.dsp");

    // LoadInsTemplate has 12 recovered r3=name-EA/r4=NULL call sites. The
    // source accepts no tags, and the catalog-backed template Item is the
    // ownership boundary shared with LoadInstrument.
    g_cpu.gpr[3] = name_ea;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 32, 0x60ba4u) == 1);
    int32_t loaded_template = (int32_t)g_cpu.gpr[3];
    CHECK(loaded_template > 0);
    CHECK(m2k_handle_type((uint32_t)loaded_template) == 8u);
    CHECK(hdl->CheckItem((uint32_t)loaded_template) == 0);
    hdl->DeleteItem((uint32_t)loaded_template);
    g_cpu.gpr[3] = name_ea;
    g_cpu.gpr[4] = 0x6400u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 32, 0x60ba4u) == 1);
    loaded_template = (int32_t)g_cpu.gpr[3];
    CHECK(loaded_template > 0);
    hdl->DeleteItem((uint32_t)loaded_template);
    wr32(&g_cpu, 0x6400u, 1u);
    g_cpu.gpr[3] = name_ea;
    g_cpu.gpr[4] = 0x6400u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 32, 0x60ba4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_TAG);
    wr32(&g_cpu, 0x6400u, M2_TAG_END);
    write_guest_string(name_ea, "missing.dsp");
    g_cpu.gpr[3] = name_ea;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 32, 0x60ba4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NAME_MISSING);
    write_guest_string(name_ea, "sampler_16_f2.dsp");

    int32_t first = m2_guest_audio_load_instrument(&g_cpu, name_ea, 0, 100);
    CHECK(first > 0);
    CHECK(m2k_handle_type((uint32_t)first) == 8u);
    CHECK(hdl->CheckItem((uint32_t)first) == 0);

    // GetAudioItemInfo's production call at guest PC 0x6ac8c passes the
    // recovered instrument Item with a null tag list. The source reader for a
    // supported audio Item returns success without reading any item fields in
    // that case; non-empty tag queries remain outside this bounded slice.
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    int32_t second = m2_guest_audio_load_instrument(&g_cpu, name_ea, 2, 120);
    CHECK(second > 0);
    CHECK((uint32_t)second != (uint32_t)first);
    CHECK(hdl->CheckItem((uint32_t)second) == 0);

    // Production audio calls can execute on a guest task context. It must
    // share the bound guest-RAM image, but need not be the boot context.
    PPCContext task_cpu = g_cpu;
    int32_t task_item = m2_guest_audio_load_instrument(
        &task_cpu, name_ea, 1, 100);
    CHECK(task_item > 0);
    CHECK(hdl->CheckItem((uint32_t)task_item) == 0);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)task_item) == M2K_OK);

    // CreateKnob's generated VA thunk carries AF_TAG_INSTRUMENT=17 and
    // AF_TAG_NAME=12. sampler_16_f2.dsp publishes a case-insensitive,
    // single-part Amplitude knob.
    const uint32_t knob_name_ea = 0x6280u;
    const uint32_t missing_knob_name_ea = 0x62a0u;
    const uint32_t probe_name_ea = 0x62c0u;
    write_guest_string(knob_name_ea, "amplitude");
    write_guest_string(missing_knob_name_ea, "missing");
    write_guest_string(probe_name_ea, "Output");
    const uint32_t knob_sp = g_cpu.gpr[1];
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = knob_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t knob = g_cpu.gpr[3];
    CHECK(knob > 0 && m2k_handle_type(knob) == 8u);
    CHECK(hdl->CheckItem(knob) == 0);
    CHECK(g_cpu.gpr[1] == knob_sp);
    m2_kernel_dispatch_set_global(NULL);

    // The typed audio selector forwards r3/r4 and the single-precision fpr1
    // value. Part 0 succeeds; part 1 is outside Amplitude[1].
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 0;
    g_cpu.fpr[1] = 0.75f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    const uint32_t knob_value_ea = 0x6300u;
    wr32(&g_cpu, knob_value_ea, 0xdeadbeefu);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = knob_value_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x6b1f0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_value_ea) == 0.75f);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = RECOMP_GUEST_MEM + 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x6b1f0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_SECURITY);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 1;
    g_cpu.gpr[5] = RECOMP_GUEST_MEM + 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 60, 0x6b1f0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 1;
    g_cpu.fpr[1] = 0.25f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // CreateProbe's generated wrapper carries AF_TAG_INSTRUMENT=17 and
    // AF_TAG_NAME=12 before a NULL optional list. sampler_16_f2.dsp publishes
    // Output[2], so the probe Item retains its owner and part cardinality.
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = probe_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t probe = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(probe > 0 && m2k_handle_type(probe) == 8u);
    CHECK(hdl->CheckItem(probe) == 0);
    const uint32_t item_info_tags_ea = 0x6380u;
    wr32(&g_cpu, item_info_tags_ea + 0, M2_TAG_END);
    wr32(&g_cpu, item_info_tags_ea + 4, 0);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = item_info_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    wr32(&g_cpu, item_info_tags_ea, 0x7fffu);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = item_info_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = RECOMP_GUEST_MEM - 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_PTR);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // ReadProbePart preserves the source's Item -> signed part -> writable
    // float32 pointer ordering. Valid DSP readback/conversion is unresolved,
    // so it returns UNIMPLEMENTED and leaves the sentinel untouched.
    const uint32_t probe_value_ea = 0x6340u;
    wr32(&g_cpu, probe_value_ea, 0xa5a5a5a5u);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = probe_value_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 38, 0x6b090u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
    CHECK(rd32(&g_cpu, probe_value_ea) == 0xa5a5a5a5u);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 1;
    g_cpu.gpr[5] = probe_value_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 38, 0x6b0c8u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 2;
    g_cpu.gpr[5] = RECOMP_GUEST_MEM + 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 38, 0x6b090u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_OUT_OF_RANGE);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = RECOMP_GUEST_MEM + 4u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 38, 0x6b180u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_SECURITY);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = probe_value_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 38, 0x6b090u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // CreateCue is a generic CreateItem with AUDIO_CUE_NODE (0x405). The
    // production ReturnAllFrames call passes that Item to AbortTimerCue;
    // this host-only caller has no CURRENTTASKITEM, so the source's
    // unposted-event success is bounded without fabricating timer state.
    uint32_t cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    CHECK(cue > 0 && hdl->CheckItem(cue) == 0);
    // GetCueSignal preserves the source's zero-on-bad-item behavior and
    // publishes the task-relative SuperAllocSignal mask captured by CreateCue.
    g_cpu.gpr[3] = cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    const uint32_t cue_signal = g_cpu.gpr[3];
    CHECK(cue_signal != 0u && (cue_signal & (cue_signal - 1u)) == 0u);
    g_cpu.gpr[3] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    CHECK(g_cpu.gpr[3] == 0u);
    g_cpu.gpr[3] = probe;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    CHECK(g_cpu.gpr[3] == 0u);
    g_cpu.gpr[3] = cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    CHECK(g_cpu.gpr[3] == cue_signal);
    uint32_t cue2 = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    CHECK(cue2 > 0 && hdl->CheckItem(cue2) == 0);
    g_cpu.gpr[3] = cue2;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    const uint32_t cue2_signal = g_cpu.gpr[3];
    CHECK(cue2_signal != 0u && cue2_signal != cue_signal &&
          (cue2_signal & (cue2_signal - 1u)) == 0u);

    // SignalAtAudioTime receives the production call shape
    // (AF_GLOBAL_CLOCK, Cue, AudioTime) at guest PC 0x3c690. The bounded
    // bridge now retains the source PostAudioEvent in-list state; interrupt
    // delivery remains unresolved.
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = cue;
    g_cpu.gpr[5] = 123u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = cue;
    g_cpu.gpr[5] = 124u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_IN_USE);
    g_cpu.gpr[3] = 0x1234u;
    g_cpu.gpr[4] = cue;
    g_cpu.gpr[5] = 123u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = probe;
    g_cpu.gpr[5] = 123u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    g_cpu.gpr[3] = cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 41, 0x3c59cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = cue;
    g_cpu.gpr[5] = 123u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = probe;
    CHECK(m2_audio_dispatch_c(&g_cpu, 41, 0x3c59cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    hdl->DeleteItem(cue);
    hdl->DeleteItem(cue2);
    g_cpu.gpr[3] = cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 41, 0x3c59cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    uint32_t recycled_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    CHECK(recycled_cue > 0 && hdl->CheckItem(recycled_cue) == 0);
    g_cpu.gpr[3] = recycled_cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    CHECK(g_cpu.gpr[3] == cue_signal);
    hdl->DeleteItem(recycled_cue);

    // CreateCue captures CURRENTTASKITEM. The same task can abort its live
    // unposted Cue, while a later host-thread call is not the owner.
    g_task_cue = 0;
    g_task_cue_signal = 0;
    g_task_cue_abort_rc = M2K_ERR_BAD_ITEM;
    uint32_t owner_task = m2k_create_task(&g_kctx, create_owned_cue_task, 0);
    CHECK(owner_task != M2K_ITEM_NONE);
    CHECK(m2k_join_task(&g_kctx, owner_task) == M2K_OK);
    CHECK(g_task_cue_abort_rc == M2K_OK);
    CHECK(g_task_cue != M2K_ITEM_NONE && hdl->CheckItem(g_task_cue) == 0);
    CHECK(g_task_cue_signal == 0x100u);
    g_cpu.gpr[3] = g_task_cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 23, 0x3cdc8u) == 1);
    CHECK(g_cpu.gpr[3] == g_task_cue_signal);
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = g_task_cue;
    g_cpu.gpr[5] = 123u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 58, 0x3c690u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NOT_OWNER);
    g_cpu.gpr[3] = g_task_cue;
    CHECK(m2_audio_dispatch_c(&g_cpu, 41, 0x3c59cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NOT_OWNER);
    hdl->DeleteItem(g_task_cue);
    CHECK((g_kctx.tasks[m2k_handle_slot(owner_task)].sig_allocated &
           g_task_cue_signal) == 0u);
    CHECK(hdl->CheckItem(g_task_cue) != 0);
    CHECK(m2k_delete_task(&g_kctx, owner_task) == M2K_OK);

    // A task-owned Cue blocks on its allocated signal until the host's
    // rendered-frame clock reaches the posted AudioTime. In a headless test
    // the timer pump advances a 44.1 kHz-equivalent 60 Hz fallback by 735
    // frames per emulated boundary (AudioTime 3, then 7). An extra host-loop
    // service pass without a boundary must not advance that clock; the first
    // real tick stays below 7 and the second
    // consumes exactly the Cue mask through the normal WaitSignal path.
    g_delivery_cue = 0;
    g_delivery_signal = 0;
    g_delivery_post_rc = M2K_ERR_BAD_ITEM;
    g_delivery_wait_rc = 0;
    atomic_store_explicit(&g_delivery_ready, 0, memory_order_relaxed);
    g_audio_test_frames = 0;
    uint32_t delivery_task = m2k_create_task(&g_kctx, wait_for_cue_task, 0);
    for (int i = 0; i < 10000 &&
                    !atomic_load_explicit(&g_delivery_ready,
                                          memory_order_acquire); ++i)
        sched_yield();
    CHECK(delivery_task != M2K_ITEM_NONE);
    CHECK(atomic_load_explicit(&g_delivery_ready, memory_order_acquire) != 0);
    CHECK(g_delivery_post_rc == M2K_OK);
    CHECK(g_delivery_signal == 0x100u);
    m2_guest_audio_pump_timers_for_tick(0);
    CHECK(g_delivery_wait_rc == 0);
    m2_guest_audio_pump_timers_for_tick(1);
    CHECK(g_delivery_wait_rc == 0);
    m2_guest_audio_pump_timers_for_tick(1);
    CHECK(m2k_join_task(&g_kctx, delivery_task) == M2K_OK);
    CHECK(g_delivery_wait_rc == (int32_t)g_delivery_signal);
    CHECK(m2k_delete_task(&g_kctx, delivery_task) == M2K_OK);
    hdl->DeleteItem(g_delivery_cue);

    // ---- timer/Cue delivery, cancel, and reuse ---------------------------
    // Vendor ground (folios/audio/audio_timer.c): CreateCue allocates one
    // task-relative signal and captures the creating task; SignalAtAudioTime
    // posts that Cue's single AudioEvent and reports AF_ERR_INUSE when it is
    // already in a list; AbortTimerCue unposts it so the associated signal is
    // never sent; an AudioTime that has already passed is due on the next
    // timer pass; and the guest's SleepUntilTimeCue macro is SignalAtAudioTime
    // + GetCueSignal + WaitSignal, which the recovered 0x3c690 caller inlines.
    // This gate drives all four transitions through the production boundary in
    // one task, with the cancel checked as a real negative control.
    timercue_reset_stages();
    uint32_t timercue_task = m2k_create_task(&g_kctx, timercue_abort_task, 0);
    CHECK(timercue_task != M2K_ITEM_NONE);
    CHECK(trig_spin_until(&g_timercue_ready));
    CHECK(g_timercue_item != M2K_ITEM_NONE && g_timercue_signal != 0u);
    CHECK(g_timercue_post_rc == M2K_OK);
    CHECK(g_timercue_abort_rc == M2K_OK);
    // Pump until AudioTime passes the aborted wakeup time. If AbortTimerCue
    // had left the event posted, the timer daemon would deliver it here and
    // the task would poll the Cue mask instead of zero.
    for (int tc = 0; tc < 4; ++tc) m2_guest_audio_pump_timers();
    CHECK(m2_guest_audio_get_audio_time(&g_cpu) >= g_timercue_due);
    atomic_store_explicit(&g_timercue_stage1, 1, memory_order_release);
    CHECK(trig_spin_until(&g_timercue_seen1));
    CHECK(g_timercue_poll_cancelled == 0);
    // The same Cue accepts a fresh request after the abort, and an elapsed
    // wakeup time is due on the next timer pass (SleepUntilTimeCue returns
    // without waiting when the source time has already passed).
    atomic_store_explicit(&g_timercue_stage2, 1, memory_order_release);
    CHECK(trig_spin_until(&g_timercue_seen2));
    CHECK(g_timercue_repost_rc == M2K_OK);
    m2_guest_audio_pump_timers();
    atomic_store_explicit(&g_timercue_stage3, 1, memory_order_release);
    CHECK(trig_spin_until(&g_timercue_seen3));
    CHECK(g_timercue_poll_repost == (int32_t)g_timercue_signal);
    // Exactly one delivery: the source unposts the event as it performs it, so
    // a further timer pass must not re-signal the completed Cue.
    m2_guest_audio_pump_timers();
    atomic_store_explicit(&g_timercue_stage4, 1, memory_order_release);
    CHECK(trig_spin_until(&g_timercue_seen4));
    CHECK(g_timercue_poll_end == 0);
    CHECK(m2k_join_task(&g_kctx, timercue_task) == M2K_OK);
    CHECK(m2k_delete_task(&g_kctx, timercue_task) == M2K_OK);
    CHECK(hdl->CheckItem(g_timercue_item) == 0);
    hdl->DeleteItem(g_timercue_item);

    // ResumeInstrument receives only the SoundBufferNode's instrument Item
    // at the recovered site. The source returns success for a stopped item
    // (vendor swiResumeInstrument returns 0 when ains_Status <= AF_STOPPED).
    g_cpu.gpr[3] = (uint32_t)first;
    CHECK(m2_audio_dispatch_c(&g_cpu, 44, 0x703a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = probe;
    CHECK(m2_audio_dispatch_c(&g_cpu, 44, 0x703a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // PauseInstrument receives the same SoundBufferNode instrument Item at
    // the recovered site. The source returns success for a stopped item
    // (vendor swiPauseInstrument returns 0 when ains_Status <= AF_STOPPED).
    g_cpu.gpr[3] = (uint32_t)first;
    CHECK(m2_audio_dispatch_c(&g_cpu, 45, 0x70350u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = probe;
    CHECK(m2_audio_dispatch_c(&g_cpu, 45, 0x70350u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // StartInstrument on the fresh sampler instrument: the disc
    // template's code runs without an imported resource, so the DSPP
    // execution boundary starts it (the spooler path starts sample-backed
    // instruments earlier in this file). The wrong-typed case goes through
    // the same prefix and returns BAD_ITEM. Use a fresh load so earlier test
    // mutations do not invalidate the slot, and reset gpr[4] so the call
    // picks up a NULL taglist (not whatever the previous test left there).
    write_guest_string(name_ea, "sampler_16_f2.dsp");
    int32_t first_fresh = m2_guest_audio_load_instrument(&g_cpu, name_ea, 0, 100);
    CHECK(first_fresh > 0);
    g_cpu.gpr[3] = (uint32_t)first_fresh;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x70350u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = probe;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x70350u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // A dynamic 2x2 mixer publishes Gain[4], exercising the same catalog
    // lookup with a multi-part knob.
    int32_t gain_template = m2_guest_audio_create_mixer_template(
        &g_cpu, (2u << 24) | (2u << 16), 0);
    CHECK(gain_template > 0);
    uint32_t gain_instrument = hdl->CreateItemVA(
        M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)gain_template,
        M2_TAG_JUMP, 0, 0, 0, 0);
    CHECK(hdl->CheckItem(gain_instrument) == 0);
    write_guest_string(knob_name_ea, "Gain");
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = gain_instrument;
    g_cpu.gpr[4] = knob_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t gain_knob = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(gain_knob > 0 && hdl->CheckItem(gain_knob) == 0);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain_knob, 3, 0.5f) ==
          M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain_knob, 4, 0.5f) ==
          M2_AUDIO_ERR_OUT_OF_RANGE);
    wr32(&g_cpu, knob_value_ea, 0xa5a5a5a5u);
    CHECK(m2_guest_audio_read_knob_part(
              &g_cpu, gain_knob, 3, knob_value_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_value_ea) == 0.5f);
    CHECK(m2_guest_audio_read_knob_part(
              &g_cpu, gain_knob, 4, knob_value_ea) ==
          M2_AUDIO_ERR_OUT_OF_RANGE);
    CHECK(m2_guest_audio_read_knob_part(
              &g_cpu, gain_knob, 3, 0) == M2_AUDIO_ERR_SECURITY);
    hdl->DeleteItem(gain_instrument);
    CHECK(hdl->CheckItem(gain_knob) != 0);
    hdl->DeleteItem((uint32_t)gain_template);

    // Missing resources fail with the source-backed audio error, and deleting
    // the instrument reclaims all of its remaining knobs.
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)first;
    g_cpu.gpr[4] = missing_knob_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NAME_MISSING);
    m2_kernel_dispatch_set_global(NULL);
    CHECK(hdl->CheckItem(knob) == 0);

    // Each LoadInstrument owns a template. Unloading both releases both
    // slots, so the next instrument reuses the original instrument slot with
    // a fresh generation rather than aliasing the stale handle.
    CHECK(m2_guest_audio_unload_instrument((uint32_t)second) == M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)first) == M2K_OK);
    CHECK(hdl->CheckItem(knob) != 0);
    CHECK(hdl->CheckItem(probe) != 0);
    g_cpu.gpr[3] = knob;
    g_cpu.gpr[4] = 0;
    g_cpu.fpr[1] = 0.1f;
    CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_read_knob_part(
              &g_cpu, knob, 0, knob_value_ea) == M2_AUDIO_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)first) ==
          M2K_ERR_BAD_ITEM);
    int32_t replacement = m2_guest_audio_load_instrument(
        &g_cpu, name_ea, 8, 200);
    CHECK(replacement > 0);
    CHECK(m2k_handle_slot((uint32_t)replacement) ==
          m2k_handle_slot((uint32_t)first));
    CHECK(m2k_handle_gen((uint32_t)replacement) !=
          m2k_handle_gen((uint32_t)first));
    CHECK(m2_guest_audio_unload_instrument((uint32_t)replacement) == M2K_OK);

    // ConnectInstrumentParts uses the on-disc resource catalog. A stereo
    // sampler exposes Output[2], while line_out exposes Input[2].
    const uint32_t output_name_ea = 0x6200u;
    const uint32_t input_name_ea = 0x6220u;
    const uint32_t line_name_ea = 0x6240u;
    const uint32_t line2_name_ea = 0x6260u;
    write_guest_string(output_name_ea, "Output");
    write_guest_string(input_name_ea, "Input");
    write_guest_string(line_name_ea, "line_out.dsp");
    write_guest_string(line2_name_ea, "line_out.dsp");

    int32_t source = m2_guest_audio_load_instrument(
        &g_cpu, name_ea, 1, 100);
    int32_t line = m2_guest_audio_load_instrument(
        &g_cpu, line_name_ea, 1, 100);
    int32_t line2 = m2_guest_audio_load_instrument(
        &g_cpu, line2_name_ea, 1, 100);
    CHECK(source > 0 && line > 0 && line2 > 0);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, output_name_ea, 0,
              (uint32_t)line, input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, output_name_ea, 0,
              (uint32_t)line, input_name_ea, 0) == M2_AUDIO_ERR_IN_USE);

    // The typed dispatch path carries r3/r4/r5/r6/r7/r8 exactly as the
    // six-argument folio call does. Names remain case-insensitive.
    write_guest_string(output_name_ea, "output");
    write_guest_string(input_name_ea, "input");
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = output_name_ea;
    g_cpu.gpr[5] = 1;
    g_cpu.gpr[6] = (uint32_t)line;
    g_cpu.gpr[7] = input_name_ea;
    g_cpu.gpr[8] = 1;
    CHECK(m2_audio_dispatch_c(&g_cpu, 57, 0x6100cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

    // DisconnectInstrumentParts identifies only the destination endpoint.
    // The recovered call is r3=destination Item, r4=destination name, and
    // r5=destination part; the original name match is case-insensitive.
    write_guest_string(input_name_ea, "INPUT");
    g_cpu.gpr[3] = (uint32_t)line;
    g_cpu.gpr[4] = input_name_ea;
    g_cpu.gpr[5] = 1;
    CHECK(m2_audio_dispatch_c(&g_cpu, 59, 0x618bcu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, input_name_ea, 1) ==
          M2_AUDIO_ERR_BAD_PTR);
    write_guest_string(input_name_ea, "input");
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, input_name_ea, 0) ==
          M2_AUDIO_ERR_BAD_PTR);
    // Removing a record makes the same destination part connectable again.
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, output_name_ea, 0,
              (uint32_t)line, input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, 0, input_name_ea, 0) == M2_AUDIO_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, RECOMP_GUEST_MEM + 4u, 0) ==
          M2_AUDIO_ERR_BAD_PTR);
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, input_name_ea, 1) ==
          M2_AUDIO_ERR_BAD_PTR);

    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, output_name_ea, 2,
              (uint32_t)line2, input_name_ea, 0) ==
          M2_AUDIO_ERR_OUT_OF_RANGE);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)line2, input_name_ea, 0,
              (uint32_t)line2, input_name_ea, 0) ==
          M2_AUDIO_ERR_NAME_MISSING);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, output_name_ea, 0,
              (uint32_t)source, output_name_ea, 0) ==
          M2_AUDIO_ERR_NAME_MISSING);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)source, RECOMP_GUEST_MEM + 4u, 0,
              (uint32_t)line2, input_name_ea, 0) == M2_AUDIO_ERR_BAD_PTR);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, 0, output_name_ea, 0, (uint32_t)line2,
              input_name_ea, 0) == M2_AUDIO_ERR_BAD_ITEM);

    // SetAudioItemInfoVA is an imported DECVASTUB_1 thunk. Its original
    // varargs glue turns r4..r10 into the guest TagArg window consumed by
    // audio:46. The bounded sample bridge retains only guest address/length;
    // GetAudioItemInfo reads those fields back without exposing host pointers.
    uint32_t sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
    CHECK(hdl->CheckItem(sample) == 0);
    const uint32_t sample_sp = g_cpu.gpr[1];
    const uint32_t sample_data_ea = 0x9000u;
    const uint32_t sample_data_bytes = 16u;
    g_cpu.gpr[3] = sample;
    g_cpu.gpr[4] = M2_AUDIO_TAG_ADDRESS;
    g_cpu.gpr[5] = sample_data_ea;
    g_cpu.gpr[6] = M2_AUDIO_TAG_NUMBYTES;
    g_cpu.gpr[7] = sample_data_bytes;
    g_cpu.gpr[8] = M2_AUDIO_TAG_SUPPRESS_WRITE_BACK_D_CACHE;
    g_cpu.gpr[9] = 1u;
    g_cpu.gpr[10] = M2_TAG_END;
    FN_SetAudioItemInfoVA__73fe0(&g_cpu);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_cpu.gpr[1] == sample_sp);

    const uint32_t sample_query_ea = 0x6800u;
    wr32(&g_cpu, sample_query_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sample_query_ea + 4u, 0u);
    wr32(&g_cpu, sample_query_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sample_query_ea + 12u, 0u);
    wr32(&g_cpu, sample_query_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, sample_query_ea + 20u, 0u);
    g_cpu.gpr[3] = sample;
    g_cpu.gpr[4] = sample_query_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rd32(&g_cpu, sample_query_ea + 4u) == sample_data_ea);
    CHECK(rd32(&g_cpu, sample_query_ea + 12u) == sample_data_bytes);

    // Sample updates are transactional: an out-of-range final address/length
    // pair fails and leaves the previously published metadata untouched.
    const uint32_t bad_sample_tags_ea = 0x6840u;
    wr32(&g_cpu, bad_sample_tags_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, bad_sample_tags_ea + 4u, RECOMP_GUEST_MEM - 2u);
    wr32(&g_cpu, bad_sample_tags_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, bad_sample_tags_ea + 12u, 4u);
    wr32(&g_cpu, bad_sample_tags_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, bad_sample_tags_ea + 20u, 0u);
    CHECK(m2_guest_audio_set_item_info(&g_cpu, sample,
                                       bad_sample_tags_ea) ==
          M2_AUDIO_ERR_BAD_PTR);
    g_cpu.gpr[3] = sample;
    g_cpu.gpr[4] = sample_query_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK(rd32(&g_cpu, sample_query_ea + 4u) == sample_data_ea);
    CHECK(rd32(&g_cpu, sample_query_ea + 12u) == sample_data_bytes);
    hdl->DeleteItem(sample);
    CHECK(hdl->CheckItem(sample) != 0);

    // CreateSample (.imsasound) bridge: kernel CreateItem with ctype 0x404
    // and the authentic 12-TagArg header retains ADDRESS/NUMBYTES and
    // uploads supported payloads. Tag order mirrors backfire.imsasound.
    const uint32_t cs_tags_ea = 0x9300u;
    const uint32_t cs_data_ea = 0x9400u;
    // 4 mono frames BE: 1, 2, -2, 32767.
    g_mem[cs_data_ea + 0] = 0x00; g_mem[cs_data_ea + 1] = 0x01;
    g_mem[cs_data_ea + 2] = 0x00; g_mem[cs_data_ea + 3] = 0x02;
    g_mem[cs_data_ea + 4] = 0xFF; g_mem[cs_data_ea + 5] = 0xFE;
    g_mem[cs_data_ea + 6] = 0x7F; g_mem[cs_data_ea + 7] = 0xFF;
    wr32(&g_cpu, cs_tags_ea + 0, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, cs_tags_ea + 4, cs_data_ea);
    wr32(&g_cpu, cs_tags_ea + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, cs_tags_ea + 12, 0x46ac4400u); // 22050 Hz
    wr32(&g_cpu, cs_tags_ea + 16, M2_AUDIO_TAG_CHANNELS);
    wr32(&g_cpu, cs_tags_ea + 20, 1u);
    wr32(&g_cpu, cs_tags_ea + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);
    wr32(&g_cpu, cs_tags_ea + 28, 1u);
    wr32(&g_cpu, cs_tags_ea + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);
    wr32(&g_cpu, cs_tags_ea + 36, 0u);
    wr32(&g_cpu, cs_tags_ea + 40, M2_AUDIO_TAG_NUMBITS);
    wr32(&g_cpu, cs_tags_ea + 44, 16u);
    wr32(&g_cpu, cs_tags_ea + 48, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, cs_tags_ea + 52, 8u);
    wr32(&g_cpu, cs_tags_ea + 56, M2_AUDIO_TAG_RELEASEBEGIN);
    wr32(&g_cpu, cs_tags_ea + 60, 0xFFFFFFFFu);
    wr32(&g_cpu, cs_tags_ea + 64, M2_AUDIO_TAG_RELEASEEND);
    wr32(&g_cpu, cs_tags_ea + 68, 0xFFFFFFFFu);
    wr32(&g_cpu, cs_tags_ea + 72, M2_AUDIO_TAG_SUSTAINBEGIN);
    wr32(&g_cpu, cs_tags_ea + 76, 1u);
    wr32(&g_cpu, cs_tags_ea + 80, M2_AUDIO_TAG_SUSTAINEND);
    wr32(&g_cpu, cs_tags_ea + 84, 3u);
    wr32(&g_cpu, cs_tags_ea + 88, M2_TAG_END);
    wr32(&g_cpu, cs_tags_ea + 92, 0u);
    const uint32_t uploads_before_pcm = g_sample_uploads;
    uint32_t pcm_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                          cs_tags_ea);
    CHECK(pcm_sample != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(pcm_sample) == 0);
    CHECK(g_sample_uploads == uploads_before_pcm + 1u);
    CHECK(g_last_upload_frames == 4u);
    CHECK(g_last_upload_l0 == 1 && g_last_upload_r0 == 1);
    CHECK(g_last_upload_l1 == 2 && g_last_upload_r1 == 2);
    // Metadata reads back through the recovered GetAudioItemInfo sample path,
    // including the frame ranges that control sustain/release transitions.
    wr32(&g_cpu, sample_query_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sample_query_ea + 4u, 0u);
    wr32(&g_cpu, sample_query_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sample_query_ea + 12u, 0u);
    wr32(&g_cpu, sample_query_ea + 16u, M2_AUDIO_TAG_SUSTAINBEGIN);
    wr32(&g_cpu, sample_query_ea + 20u, 0u);
    wr32(&g_cpu, sample_query_ea + 24u, M2_AUDIO_TAG_SUSTAINEND);
    wr32(&g_cpu, sample_query_ea + 28u, 0u);
    wr32(&g_cpu, sample_query_ea + 32u, M2_AUDIO_TAG_RELEASEBEGIN);
    wr32(&g_cpu, sample_query_ea + 36u, 0u);
    wr32(&g_cpu, sample_query_ea + 40u, M2_AUDIO_TAG_RELEASEEND);
    wr32(&g_cpu, sample_query_ea + 44u, 0u);
    wr32(&g_cpu, sample_query_ea + 48u, M2_TAG_END);
    wr32(&g_cpu, sample_query_ea + 52u, 0u);
    g_cpu.gpr[3] = pcm_sample;
    g_cpu.gpr[4] = sample_query_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rd32(&g_cpu, sample_query_ea + 4u) == cs_data_ea);
    CHECK(rd32(&g_cpu, sample_query_ea + 12u) == 8u);
    CHECK((int32_t)rd32(&g_cpu, sample_query_ea + 20u) == 1);
    CHECK((int32_t)rd32(&g_cpu, sample_query_ea + 28u) == 3);
    CHECK((int32_t)rd32(&g_cpu, sample_query_ea + 36u) == -1);
    CHECK((int32_t)rd32(&g_cpu, sample_query_ea + 44u) == -1);

    // Shooter M2_PlayVoice publishes NUMBITS=8 and CHANNELS=1 through its
    // imported VA call. Vendor dspsServiceInputFDMA converts signed int8 to
    // int16 with scale 256. Positive and negative limits catch unsigned or
    // paired-16-bit decoding; stereo then catches accidental mono duplication.
    {
        const uint32_t tags = 0x69c0u, data = 0x6a80u;
        const uint32_t query = 0x6a00u;
        g_mem[data] = 0x80; g_mem[data + 1] = 0x7f;
        g_mem[data + 2] = 0xff; g_mem[data + 3] = 0x01;
        const uint32_t values[] = {
            M2_AUDIO_TAG_ADDRESS, data, M2_AUDIO_TAG_NUMBYTES, 4,
            M2_AUDIO_TAG_NUMBITS, 8, M2_AUDIO_TAG_CHANNELS, 1,
            M2_TAG_END, 0,
        };
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            wr32(&g_cpu, tags + 4u * i, values[i]);
        const uint32_t before = g_sample_uploads;
        const uint32_t s8 = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, tags);
        CHECK(s8 != M2K_ITEM_NONE && g_sample_uploads == before + 1);
        CHECK(g_last_upload_frames == 4 && g_last_upload_l0 == -32768 &&
              g_last_upload_r0 == -32768 && g_last_upload_l1 == 32512 &&
              g_last_upload_r1 == 32512);
        // Format changes and payload updates share the creation decoder.
        wr32(&g_cpu, tags + 28, 2);
        g_cpu.gpr[3] = s8; g_cpu.gpr[4] = tags;
        CHECK(m2_audio_dispatch_c(&g_cpu, 46, 0x122bcu) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(g_last_upload_frames == 2 && g_last_upload_l0 == -32768 &&
              g_last_upload_r0 == 32512 && g_last_upload_l1 == -256 &&
              g_last_upload_r1 == 256);
        const uint32_t queries[] = {
            M2_AUDIO_TAG_NUMBITS, 0, M2_AUDIO_TAG_WIDTH, 0,
            M2_AUDIO_TAG_CHANNELS, 0, M2_TAG_END, 0,
        };
        for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i)
            wr32(&g_cpu, query + 4u * i, queries[i]);
        CHECK(m2_guest_audio_get_item_info(&g_cpu, s8, query) == M2K_OK);
        CHECK(rd32(&g_cpu, query + 4) == 8 && rd32(&g_cpu, query + 12) == 1 &&
              rd32(&g_cpu, query + 20) == 2);
        // Invalid format update returns the vendor error transactionally.
        const uint32_t uploads = g_sample_uploads;
        wr32(&g_cpu, tags + 20, 0);
        CHECK(m2_guest_audio_set_item_info(&g_cpu, s8, tags) == M2_AUDIO_ERR_OUT_OF_RANGE);
        CHECK(g_sample_uploads == uploads);
        CHECK(m2_guest_audio_get_item_info(&g_cpu, s8, query) == M2K_OK);
        CHECK(rd32(&g_cpu, query + 4) == 8 && rd32(&g_cpu, query + 20) == 2);
        // Executed plausible wrong format: same four bytes as 16-bit stereo
        // have one frame, unlike the two 8-bit stereo frames above.
        wr32(&g_cpu, tags + 20, 16);
        CHECK(m2_guest_audio_set_item_info(&g_cpu, s8, tags) == M2K_OK);
        CHECK(g_last_upload_frames == 1 && g_last_upload_l0 != -32768 &&
              g_last_upload_r0 != 32512);
        hdl->DeleteItem(s8);
    }

    // SQS2 mono: DecodeSQXD() clears the mode bit before expanding; even bytes
    // are exact and odd bytes add a quarter-scale delta.
    const uint32_t sq_tags_ea = 0x9500u;
    const uint32_t sq_data_ea = 0x9600u;
    g_mem[sq_data_ea + 0] = 0x02;
    g_mem[sq_data_ea + 1] = 0x03;
    g_mem[sq_data_ea + 2] = 0xfe;
    g_mem[sq_data_ea + 3] = 0xff;
    wr32(&g_cpu, sq_tags_ea + 0, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sq_tags_ea + 4, sq_data_ea);
    wr32(&g_cpu, sq_tags_ea + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, sq_tags_ea + 12, 0x46ac4400u);
    wr32(&g_cpu, sq_tags_ea + 16, M2_AUDIO_TAG_CHANNELS);
    wr32(&g_cpu, sq_tags_ea + 20, 1u);
    wr32(&g_cpu, sq_tags_ea + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);
    wr32(&g_cpu, sq_tags_ea + 28, 2u);
    wr32(&g_cpu, sq_tags_ea + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);
    wr32(&g_cpu, sq_tags_ea + 36, M2_AUDIO_COMPRESSION_SQS2);
    wr32(&g_cpu, sq_tags_ea + 40, M2_AUDIO_TAG_NUMBITS);
    wr32(&g_cpu, sq_tags_ea + 44, 16u);
    wr32(&g_cpu, sq_tags_ea + 48, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sq_tags_ea + 52, 4u);
    wr32(&g_cpu, sq_tags_ea + 56, M2_AUDIO_TAG_RELEASEBEGIN);
    wr32(&g_cpu, sq_tags_ea + 60, 0xFFFFFFFFu);
    wr32(&g_cpu, sq_tags_ea + 64, M2_AUDIO_TAG_RELEASEEND);
    wr32(&g_cpu, sq_tags_ea + 68, 0xFFFFFFFFu);
    wr32(&g_cpu, sq_tags_ea + 72, M2_AUDIO_TAG_SUSTAINBEGIN);
    wr32(&g_cpu, sq_tags_ea + 76, 0xFFFFFFFFu);
    wr32(&g_cpu, sq_tags_ea + 80, M2_AUDIO_TAG_SUSTAINEND);
    wr32(&g_cpu, sq_tags_ea + 84, 0xFFFFFFFFu);
    wr32(&g_cpu, sq_tags_ea + 88, M2_TAG_END);
    wr32(&g_cpu, sq_tags_ea + 92, 0u);
    const uint32_t uploads_before_sqs2 = g_sample_uploads;
    uint32_t sqs2_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                           sq_tags_ea);
    CHECK(sqs2_sample != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(sqs2_sample) == 0);
    CHECK(g_sample_uploads == uploads_before_sqs2 + 1u);
    CHECK(g_last_upload_frames == 4u);
    CHECK(g_last_upload_l0 == 8 && g_last_upload_r0 == 8);
    CHECK(g_last_upload_l1 == 10 && g_last_upload_r1 == 10);

    // The race engine channels set sampler knobs before StartInstrument.
    // dsppSetKnob writes those allocated words even while stopped, and the
    // subsequent start must use them rather than sampler defaults. Once live,
    // later writes update the same voice immediately.
    const uint32_t variable_sampler_name_ea = 0x9800u;
    const uint32_t variable_amp_name_ea = 0x9820u;
    const uint32_t variable_rate_name_ea = 0x9840u;
    write_guest_string(variable_sampler_name_ea, "sampler_sqs2_v1.dsp");
    write_guest_string(variable_amp_name_ea, "Amplitude");
    write_guest_string(variable_rate_name_ea, "SampleRate");
    int32_t variable_sampler = m2_guest_audio_load_instrument(
        &g_cpu, variable_sampler_name_ea, 1, 100);
    CHECK(variable_sampler > 0);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)variable_sampler;
    g_cpu.gpr[4] = sqs2_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t variable_attachment = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)variable_sampler;
    g_cpu.gpr[4] = variable_amp_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t variable_amp = g_cpu.gpr[3];
    g_cpu.gpr[3] = (uint32_t)variable_sampler;
    g_cpu.gpr[4] = variable_rate_name_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t variable_rate = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(variable_attachment != M2K_ITEM_NONE);
    CHECK(variable_amp != M2K_ITEM_NONE);
    CHECK(variable_rate != M2K_ITEM_NONE);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, variable_amp, 0, 0.25f) ==
          M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, variable_rate, 0, 11025.0f) ==
          M2K_OK);
    g_cpu.gpr[3] = (uint32_t)variable_sampler;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2b60cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_started_gain == 0.25f);
    CHECK(g_started_rate_hz == 11025.0f);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, variable_amp, 0, 0.5f) ==
          M2K_OK);
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, variable_rate, 0, 22050.0f) ==
          M2K_OK);
    CHECK(g_started_gain == 0.5f);
    CHECK(g_started_rate_hz == 22050.0f);
    CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)variable_sampler,
                                         0) == M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)variable_sampler) ==
          M2K_OK);

    // Unknown compression mints with metadata but uploads nothing.
    const uint32_t badcomp_tags_ea = 0x9700u;
    wr32(&g_cpu, badcomp_tags_ea + 0, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, badcomp_tags_ea + 4, cs_data_ea);
    wr32(&g_cpu, badcomp_tags_ea + 8, M2_AUDIO_TAG_CHANNELS);
    wr32(&g_cpu, badcomp_tags_ea + 12, 1u);
    wr32(&g_cpu, badcomp_tags_ea + 16, M2_AUDIO_TAG_COMPRESSIONRATIO);
    wr32(&g_cpu, badcomp_tags_ea + 20, 1u);
    wr32(&g_cpu, badcomp_tags_ea + 24, M2_AUDIO_TAG_COMPRESSIONTYPE);
    wr32(&g_cpu, badcomp_tags_ea + 28, 0x12345678u);
    wr32(&g_cpu, badcomp_tags_ea + 32, M2_AUDIO_TAG_NUMBITS);
    wr32(&g_cpu, badcomp_tags_ea + 36, 16u);
    wr32(&g_cpu, badcomp_tags_ea + 40, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, badcomp_tags_ea + 44, 8u);
    wr32(&g_cpu, badcomp_tags_ea + 48, M2_TAG_END);
    wr32(&g_cpu, badcomp_tags_ea + 52, 0u);
    const uint32_t uploads_before_bad = g_sample_uploads;
    uint32_t badcomp_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                              badcomp_tags_ea);
    CHECK(badcomp_sample != M2K_ITEM_NONE);
    CHECK(g_sample_uploads == uploads_before_bad);
    g_cpu.gpr[3] = badcomp_sample;
    g_cpu.gpr[4] = sample_query_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 28, 0x6ac8cu) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(rd32(&g_cpu, sample_query_ea + 4u) == cs_data_ea);
    CHECK(rd32(&g_cpu, sample_query_ea + 12u) == 8u);

    // OOB sample address fails creation instead of escaping a bad EA.
    const uint32_t oob_tags_ea = 0x9800u;
    wr32(&g_cpu, oob_tags_ea + 0, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, oob_tags_ea + 4, RECOMP_GUEST_MEM - 2u);
    wr32(&g_cpu, oob_tags_ea + 8, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, oob_tags_ea + 12, 8u);
    wr32(&g_cpu, oob_tags_ea + 16, M2_TAG_END);
    wr32(&g_cpu, oob_tags_ea + 20, 0u);
    CHECK(hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, oob_tags_ea) ==
          M2K_ITEM_NONE);

    // An uploaded sample starts a host voice through the existing
    // StartAttachment path (ord 54), proving the CreateSample -> attachment
    // -> voice chain without a real mixer.
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = pcm_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t pcm_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(pcm_attachment != M2K_ITEM_NONE);
    g_started_sample = 0;
    g_cpu.gpr[3] = pcm_attachment;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_started_sample != 0u);
    CHECK(g_started_attachment == pcm_attachment);
    CHECK(g_started_sustain_begin == 1);
    CHECK(g_started_sustain_end == 3);
    CHECK(g_started_release_begin == -1);
    CHECK(g_started_release_end == -1);
    // With no StartInstrument frequency override, Portfolio derives the
    // sampler rate from the attached Sample's AF_TAG_SAMPLE_RATE_FP.
    CHECK(g_started_rate_hz == 22050.0f);
    CHECK(g_started_gain == 1.0f);

    // A monitored uploaded sample now carries its attachment identity through
    // the host voice boundary. Completion is delivered at the normal guest
    // pump, then wakes the task that owns the Cue just like ProcessAttachmentDMA
    // -> SignalMonitoringCue in the vendor audio folio.
    g_attachment_monitor_target = pcm_attachment;
    g_attachment_monitor_ready = 0;
    g_attachment_monitor_rc = M2K_ERR_BAD_ITEM;
    g_attachment_monitor_wait_rc = 0;
    g_attachment_completion = 0;
    uint32_t attachment_delivery_task = m2k_create_task(
        &g_kctx, wait_for_attachment_completion_task, 0);
    for (int i = 0; i < 10000 &&
                    !atomic_load_explicit(&g_attachment_monitor_ready,
                                          memory_order_acquire); ++i)
        sched_yield();
    CHECK(attachment_delivery_task != M2K_ITEM_NONE);
    CHECK(atomic_load_explicit(&g_attachment_monitor_ready,
                               memory_order_acquire) != 0);
    CHECK(g_attachment_monitor_rc == M2K_OK);
    CHECK(g_attachment_monitor_signal == 0x100u);
    g_attachment_completion = pcm_attachment;
    m2_guest_audio_pump_timers();
    CHECK(m2k_join_task(&g_kctx, attachment_delivery_task) == M2K_OK);
    CHECK(g_attachment_monitor_wait_rc ==
          (int32_t)g_attachment_monitor_signal);
    CHECK(m2k_delete_task(&g_kctx, attachment_delivery_task) == M2K_OK);
    hdl->DeleteItem(g_attachment_monitor_cue);
    if (g_started_attachment_voice >= 0)
        CHECK(m2_host_audio_stop_voice((uint32_t)g_started_attachment_voice) ==
              M2K_OK);

    // StartInstrument (ord 69) on this sample-backed instrument starts a
    // host voice and reports M2K_OK, unblocking the spooler chain that
    // waits on success before StartAttachment. StopInstrument (ord 67)
    // stops the owned voice and reports M2K_OK; a second stop of the same
    // now-stopped instrument reports the vendor swiStopInstrument status
    // short circuit (ains_Status <= AF_STOPPED => 0), not an error.
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_release_attachment = 0;
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 68, 0x2aa44u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_release_attachment != 0u);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x70400u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x70400u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    // The observed spooler start tags (Amplitude/SampleRate float values) are
    // accepted and retained across an untagged restart, matching
    // DSPPStartInstrument's current-knob contract. Unknown tags still fail
    // closed with BAD_TAG. A busy backend reports NO_MEMORY instead of
    // fabricating a start.
    const uint32_t start_tags_ea = 0x9900u;
    wr32(&g_cpu, start_tags_ea + 0, M2_AUDIO_TAG_START_0x4E);
    wr32(&g_cpu, start_tags_ea + 4, 0x3f1b26cau);
    wr32(&g_cpu, start_tags_ea + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
    wr32(&g_cpu, start_tags_ea + 12, 0x462c4400u); // 11025 Hz
    wr32(&g_cpu, start_tags_ea + 16, M2_TAG_END);
    wr32(&g_cpu, start_tags_ea + 20, 0u);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = start_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    const float retained_gain = g_started_gain;
    CHECK(retained_gain > 0.5f && retained_gain < 0.7f);
    CHECK(g_started_rate_hz == 11025.0f);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_started_gain == retained_gain);
    CHECK(g_started_rate_hz == 11025.0f);
    wr32(&g_cpu, start_tags_ea + 0, 0x7eu);
    wr32(&g_cpu, start_tags_ea + 8, M2_TAG_END);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = start_tags_ea;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_TAG);
    g_start_voice_rc = -1;
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_NO_MEMORY);
    g_start_voice_rc = 0;
    hdl->DeleteItem(pcm_sample);
    hdl->DeleteItem(sqs2_sample);
    hdl->DeleteItem(badcomp_sample);

    // SoundSpooler creates AF_TAG_WIDTH=1 sample placeholders and starts the
    // sampler before its fill thread publishes buffer bytes. The bridge must
    // accept that stream-only StartInstrument/StartAttachment sequence while
    // retaining SetAudioItemInfo metadata without treating CBD2 bytes as PCM.
    const uint32_t stream_name_ea = 0x9a00u;
    const uint32_t stream_tags_ea = 0x9a40u;
    const uint32_t stream_set_ea = 0x9a80u;
    const uint32_t stream_data_ea = 0x9ac0u;
    write_guest_string(stream_name_ea, "sampler_cbd2_f2.dsp");
    int32_t stream_source = m2_guest_audio_load_instrument(
        &g_cpu, stream_name_ea, 1, 100);
    CHECK(stream_source > 0);
    wr32(&g_cpu, stream_tags_ea + 0u, M2_AUDIO_TAG_WIDTH);
    wr32(&g_cpu, stream_tags_ea + 4u, 1u);
    wr32(&g_cpu, stream_tags_ea + 8u, M2_TAG_END);
    wr32(&g_cpu, stream_tags_ea + 12u, 0u);
    uint32_t stream_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                             stream_tags_ea);
    CHECK(stream_sample != M2K_ITEM_NONE);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)stream_source;
    g_cpu.gpr[4] = stream_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t stream_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(stream_attachment != M2K_ITEM_NONE);
    g_started_sample = 0;
    g_cpu.gpr[3] = (uint32_t)stream_source;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_started_sample == 0u);
    g_cpu.gpr[3] = stream_attachment;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    CHECK(g_started_sample == 0u);

    g_mem[stream_data_ea + 0u] = 0x01u;
    g_mem[stream_data_ea + 1u] = 0x02u;
    g_mem[stream_data_ea + 2u] = 0x03u;
    g_mem[stream_data_ea + 3u] = 0x04u;
    wr32(&g_cpu, stream_set_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, stream_set_ea + 4u, stream_data_ea);
    wr32(&g_cpu, stream_set_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, stream_set_ea + 12u, 4u);
    wr32(&g_cpu, stream_set_ea + 16u,
         M2_AUDIO_TAG_SUPPRESS_WRITE_BACK_D_CACHE);
    wr32(&g_cpu, stream_set_ea + 20u, 1u);
    wr32(&g_cpu, stream_set_ea + 24u, M2_TAG_END);
    wr32(&g_cpu, stream_set_ea + 28u, 0u);
    const uint32_t uploads_before_stream = g_sample_uploads;
    CHECK(m2_guest_audio_set_item_info(&g_cpu, stream_sample,
                                       stream_set_ea) == M2K_OK);
    CHECK(g_sample_uploads == uploads_before_stream);
    wr32(&g_cpu, sample_query_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, sample_query_ea + 4u, 0u);
    wr32(&g_cpu, sample_query_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, sample_query_ea + 12u, 0u);
    wr32(&g_cpu, sample_query_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, sample_query_ea + 20u, 0u);
    CHECK(m2_guest_audio_get_item_info(&g_cpu, stream_sample,
                                       sample_query_ea) == M2K_OK);
    CHECK(rd32(&g_cpu, sample_query_ea + 4u) == stream_data_ea);
    CHECK(rd32(&g_cpu, sample_query_ea + 12u) == 4u);
    const uint32_t cbd2_starts_before = g_cbd2_stream_starts;
    const uint32_t pcm_starts_before = g_pcm16be_stream_starts;
    CHECK(m2_guest_audio_start_attachment(&g_cpu, stream_attachment, 0) ==
          M2K_OK);
    CHECK(g_cbd2_stream_starts == cbd2_starts_before + 1u);
    CHECK(g_pcm16be_stream_starts == pcm_starts_before);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)stream_source) ==
          M2K_OK);
    CHECK(hdl->CheckItem(stream_attachment) != 0);
    hdl->DeleteItem(stream_sample);

    // The same generic width-1 SoundSpooler sample attached to MPEG's
    // sampler_16_f2 must select the already-decoded big-endian PCM bridge,
    // rather than the CBD2 bridge above.
    write_guest_string(stream_name_ea, "sampler_16_f2.dsp");
    int32_t mpeg_stream_source = m2_guest_audio_load_instrument(
        &g_cpu, stream_name_ea, 1, 100);
    CHECK(mpeg_stream_source > 0);
    uint32_t mpeg_stream_sample = hdl->CreateItem(
        M2_AUDIO_SAMPLE_CTYPE, stream_tags_ea);
    CHECK(mpeg_stream_sample != M2K_ITEM_NONE);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)mpeg_stream_source;
    g_cpu.gpr[4] = mpeg_stream_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t mpeg_stream_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(mpeg_stream_attachment != M2K_ITEM_NONE);
    CHECK(m2_guest_audio_set_item_info(&g_cpu, mpeg_stream_sample,
                                       stream_set_ea) == M2K_OK);
    CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                          (uint32_t)mpeg_stream_source,
                                          0u) == M2K_OK);
    const uint32_t cbd2_before_mpeg = g_cbd2_stream_starts;
    const uint32_t pcm_before_mpeg = g_pcm16be_stream_starts;
    CHECK(m2_guest_audio_start_attachment(&g_cpu, mpeg_stream_attachment, 0) ==
          M2K_OK);
    CHECK(g_pcm16be_stream_starts == pcm_before_mpeg + 1u);
    CHECK(g_cbd2_stream_starts == cbd2_before_mpeg);

    // A failed host materialization is a failed StartAttachment, not a
    // successful guest transition with a silent stream. This negative
    // control models the finite SoundSpooler/message-pool allocation boundary
    // returning no memory from the platform adapter.
    g_pcm16be_stream_start_rc = 0;
    CHECK(m2_guest_audio_start_attachment(&g_cpu, mpeg_stream_attachment, 0) ==
          M2_AUDIO_ERR_NO_MEMORY);
    g_pcm16be_stream_start_rc = 1;

    // LinkAttachments may be called after StartInstrument but before the
    // successor is started. A failed host append must be visible and must not
    // publish a guest-only successor; retrying with a working adapter then
    // establishes the normal link.
    uint32_t mpeg_stream_sample2 = hdl->CreateItem(
        M2_AUDIO_SAMPLE_CTYPE, stream_tags_ea);
    CHECK(mpeg_stream_sample2 != M2K_ITEM_NONE);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)mpeg_stream_source;
    g_cpu.gpr[4] = mpeg_stream_sample2;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t mpeg_stream_attachment2 = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(mpeg_stream_attachment2 != M2K_ITEM_NONE);
    CHECK(m2_guest_audio_set_item_info(&g_cpu, mpeg_stream_sample2,
                                       stream_set_ea) == M2K_OK);
    g_pcm16be_stream_link_rc = 0;
    CHECK(m2_guest_audio_link_attachments(&g_cpu, mpeg_stream_attachment,
                                          mpeg_stream_attachment2) ==
          M2_AUDIO_ERR_NO_MEMORY);
    check_stream_completion(mpeg_stream_attachment2, 0u);
    // Callback retirement won the race: retain the guest link without
    // claiming the successor was published or making its Cue eligible.
    g_pcm16be_stream_link_rc = 2;
    CHECK(m2_guest_audio_link_attachments(&g_cpu, mpeg_stream_attachment,
                                          mpeg_stream_attachment2) == M2K_OK);
    check_stream_completion(mpeg_stream_attachment2, 0u);
    g_pcm16be_stream_link_rc = 1;
    CHECK(m2_guest_audio_link_attachments(&g_cpu, mpeg_stream_attachment,
                                          mpeg_stream_attachment2) == M2K_OK);
    check_stream_completion(mpeg_stream_attachment2, 1u);
    // A chain retained while the instrument is stopped must also become
    // eligible when StartAttachment publishes that chain to the host stream.
    CHECK(m2_guest_audio_stop_instrument(&g_cpu,
                                         (uint32_t)mpeg_stream_source, 0u) == M2K_OK);
    uint32_t mpeg_stream_attachment3;
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)mpeg_stream_source;
    g_cpu.gpr[4] = mpeg_stream_sample2;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    mpeg_stream_attachment3 = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(m2_guest_audio_link_attachments(&g_cpu, mpeg_stream_attachment,
                                          mpeg_stream_attachment3) == M2K_OK);
    check_stream_completion(mpeg_stream_attachment3, 0u);
    CHECK(m2_guest_audio_start_attachment(&g_cpu, mpeg_stream_attachment, 0u) == M2K_OK);
    check_stream_completion(mpeg_stream_attachment3, 1u);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)mpeg_stream_source) ==
          M2K_OK);
    hdl->DeleteItem(mpeg_stream_sample);
    hdl->DeleteItem(mpeg_stream_sample2);

    // CreateAttachment's generated wrapper publishes ctype 0x407,
    // AF_TAG_MASTER=13, AF_TAG_SLAVE=18, and a NULL optional list. The
    // bounded Item bridge retains ownership without crossing a host pointer.
    uint32_t attachment_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
    uint32_t attachment_sample2 = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
    CHECK(hdl->CheckItem(attachment_sample) == 0);
    CHECK(hdl->CheckItem(attachment_sample2) == 0);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = attachment_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attachment1 = g_cpu.gpr[3];
    CHECK(attachment1 != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(attachment1) == 0);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = attachment_sample2;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attachment2 = g_cpu.gpr[3];
    CHECK(attachment2 != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(attachment2) == 0);

    // Recovered production callers use both forms: sound-3D attaches a
    // delay line with a hook name and auto-delete flag; the sound spooler
    // sets AF_ATTF_NOAUTOSTART. Exercise both bounded guest-list forms.
    const uint32_t attachment_name_ea = 0x6880u;
    const uint32_t attachment_name_tags_ea = 0x68a0u;
    write_guest_string(attachment_name_ea, "SendFIFO");
    wr32(&g_cpu, attachment_name_tags_ea + 0u, M2_AUDIO_TAG_NAME);
    wr32(&g_cpu, attachment_name_tags_ea + 4u, attachment_name_ea);
    wr32(&g_cpu, attachment_name_tags_ea + 8u,
         M2_AUDIO_TAG_AUTO_DELETE_SLAVE);
    wr32(&g_cpu, attachment_name_tags_ea + 12u, 1u);
    wr32(&g_cpu, attachment_name_tags_ea + 16u, M2_TAG_END);
    wr32(&g_cpu, attachment_name_tags_ea + 20u, 0u);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = attachment_sample2;
    g_cpu.gpr[5] = attachment_name_tags_ea;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attachment_named = g_cpu.gpr[3];
    CHECK(attachment_named != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(attachment_named) == 0);

    const uint32_t attachment_flags_tags_ea = 0x68c0u;
    wr32(&g_cpu, attachment_flags_tags_ea + 0u, M2_AUDIO_TAG_SET_FLAGS);
    wr32(&g_cpu, attachment_flags_tags_ea + 4u,
         M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
    wr32(&g_cpu, attachment_flags_tags_ea + 8u, M2_TAG_END);
    wr32(&g_cpu, attachment_flags_tags_ea + 12u, 0u);
    g_cpu.gpr[3] = (uint32_t)source;
    g_cpu.gpr[4] = attachment_sample2;
    g_cpu.gpr[5] = attachment_flags_tags_ea;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t attachment_flags = g_cpu.gpr[3];
    CHECK(attachment_flags != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(attachment_flags) == 0);

    m2_kernel_dispatch_set_global(NULL);

    // MonitorAttachment receives the attachment, Cue, and CUE_AT_END index
    // at the sole recovered production site. The source permits Cue=0 to
    // remove monitoring, validates nonzero Cues, and rejects other indexes;
    // Cue delivery runs at the frame-boundary pump (host-voice proven by the
    // attachment-completion block above; raw-DSPP pump landed with a synthetic
    // fabrication guard at the end of main, positive delivery still unpinned).
    uint32_t monitor_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
    CHECK(monitor_cue > 0 && hdl->CheckItem(monitor_cue) == 0);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = monitor_cue;
    g_cpu.gpr[5] = (uint32_t)M2_AUDIO_CUE_AT_END;
    CHECK(m2_audio_dispatch_c(&g_cpu, 50, 0x70550u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = monitor_cue;
    g_cpu.gpr[5] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 50, 0x70550u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = attachment_sample;
    g_cpu.gpr[5] = (uint32_t)M2_AUDIO_CUE_AT_END;
    CHECK(m2_audio_dispatch_c(&g_cpu, 50, 0x70550u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = 0;
    g_cpu.gpr[5] = 123;
    CHECK(m2_audio_dispatch_c(&g_cpu, 50, 0x70550u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    hdl->DeleteItem(monitor_cue);

    // Both attachments have the same instrument host. LinkAttachments
    // retains the guest-visible successor chain, and Att2=0 removes it.
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = attachment2;
    CHECK(m2_audio_dispatch_c(&g_cpu, 51, 0x6a0a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 51, 0x6a0a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = attachment_sample;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 51, 0x6a0a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = (uint32_t)source;
    CHECK(m2_audio_dispatch_c(&g_cpu, 51, 0x6a0a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // StartAttachment receives the attachment Item in r3 and a NULL tag
    // list in r4 at all three production sites. Ordinary uploaded-sample
    // attachments in this typed-selector fixture stop at the unrecovered DSP
    // start operation; the physical sampler path is tested below. The source
    // validation errors remain observable before that boundary.
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_UNIMPLEMENTED);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = 0x6400u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_TAG);
    g_cpu.gpr[3] = attachment_sample;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // StopInstrument receives the instrument Item in r3 and a NULL tag list
    // at the recovered call sites. Vendor swiStopInstrument validates the
    // Item, rejects any tag list (AF_ERR_BADTAG), and only then takes the
    // ains_Status <= AF_STOPPED short circuit, so the stopped `line` fixture
    // reports success for the NULL form while the non-NULL form stays
    // BAD_TAG. The DSP stop itself is only reachable once started.
    g_cpu.gpr[3] = (uint32_t)line;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x70400u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = (uint32_t)line;
    g_cpu.gpr[4] = 0x6400u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x70400u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_TAG);
    g_cpu.gpr[3] = attachment_sample;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x70400u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // ReleaseInstrument receives the same r3 Item/r4 shape, but vendor
    // swiReleaseInstrument checks ains_Status <= AF_RELEASED *before*
    // DSPPReleaseInstrument walks the tag list, so the stopped fixture
    // reports success for both call forms. A started no-attachment instrument
    // follows the vendor's empty FIFO scan; optional time-scale tags and DSP
    // release remain unresolved for attachment-bearing cases, and the finite
    // physical no-loop case is exercised below.
    g_cpu.gpr[3] = (uint32_t)line;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 68, 0x2aa44u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = (uint32_t)line;
    g_cpu.gpr[4] = 0x6400u;
    CHECK(m2_audio_dispatch_c(&g_cpu, 68, 0x2aa44u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
    g_cpu.gpr[3] = attachment_sample;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 68, 0x2aa44u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);

    // A template is a valid CreateAttachment master but not a valid
    // LinkAttachments host, matching the source's AF_ERR_MISMATCH result.
    int32_t attachment_template = m2_guest_audio_create_mixer_template(
        &g_cpu, (1u << 24) | (1u << 16), 0);
    uint32_t template_slave = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
    CHECK(attachment_template > 0);
    CHECK(hdl->CheckItem(template_slave) == 0);
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = (uint32_t)attachment_template;
    g_cpu.gpr[4] = template_slave;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t template_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(template_attachment != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(template_attachment) == 0);
    g_cpu.gpr[3] = template_attachment;
    g_cpu.gpr[4] = 0;
    CHECK(m2_audio_dispatch_c(&g_cpu, 54, 0x70948u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_BAD_ITEM);
    g_cpu.gpr[3] = attachment1;
    g_cpu.gpr[4] = template_attachment;
    CHECK(m2_audio_dispatch_c(&g_cpu, 51, 0x6a0a0u) == 1);
    CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_MISMATCH);
    hdl->DeleteItem((uint32_t)attachment_template);
    CHECK(hdl->CheckItem(template_attachment) != 0);
    hdl->DeleteItem(template_slave);

    // The source instrument owns the attachment Items, while the default
    // attachment policy leaves the sample slave Items alive. This also checks
    // stale Att1/Att2 handles fail after the master is unloaded.
    CHECK(m2_guest_audio_unload_instrument((uint32_t)source) == M2K_OK);
    CHECK(hdl->CheckItem(attachment1) != 0);
    CHECK(hdl->CheckItem(attachment2) != 0);
    CHECK(hdl->CheckItem(attachment_sample) == 0);
    CHECK(hdl->CheckItem(attachment_sample2) == 0);
    hdl->DeleteItem(attachment_sample);
    hdl->DeleteItem(attachment_sample2);

    // UnloadInstrument deletes the owned template and removes every graph
    // record that names the deleted source, making the destination reusable.
    CHECK(m2_guest_audio_disconnect_instrument_parts(
              &g_cpu, (uint32_t)line, input_name_ea, 0) ==
          M2_AUDIO_ERR_BAD_PTR);
    int32_t replacement_source = m2_guest_audio_load_instrument(
        &g_cpu, name_ea, 1, 100);
    CHECK(replacement_source > 0);
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)replacement_source, output_name_ea, 0,
              (uint32_t)line, input_name_ea, 0) == M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)replacement_source) ==
          M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)line) == M2K_OK);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)line2) == M2K_OK);
    teardown();

    setup();
    // Guest-owned string validation is bounded and rejects malformed input.
    CHECK(m2_guest_audio_load_instrument(&g_cpu, RECOMP_GUEST_MEM + 4u,
                                         1, 100) == M2K_ERR_BAD_ITEM);
    memset(g_mem + 0x6000, 'A', 64);
    CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100) ==
          M2K_ERR_BAD_ITEM);
    write_guest_string(0x6000u, "missing.dsp");
    CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100) ==
          M2_AUDIO_ERR_NAME_MISSING);
    write_guest_string(0x6000u, "sampler_16_f2.dsp");
    CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 4, 100) ==
          M2K_ERR_BAD_ITEM);
    CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 201) ==
          M2K_ERR_BAD_ITEM);
    teardown();

    setup();
    // Test CreatePatchTemplate (audiopatch folio 5 selector 0):
    // Constructs an instrument template from a PatchCmd list with DEFINE_PORT,
    // DEFINE_KNOB, EXPOSE, and JUMP.
    const uint32_t str_input_ea = 0x7000u;
    const uint32_t str_output_ea = 0x7020u;
    const uint32_t str_drift_ea = 0x7040u;
    const uint32_t str_amp_ea = 0x7060u;
    const uint32_t str_del_ea = 0x7080u;
    const uint32_t str_delrate_ea = 0x70a0u;
    const uint32_t str_fifo_ea = 0x70c0u;
    const uint32_t str_fifo_left_ea = 0x70e0u;
    const uint32_t str_fifo_right_ea = 0x7100u;
    const uint32_t str_patch_name_ea = 0x7120u;

    write_guest_string(str_input_ea, "Input");
    write_guest_string(str_output_ea, "Output");
    write_guest_string(str_drift_ea, "Drift");
    write_guest_string(str_amp_ea, "Amplitude");
    write_guest_string(str_del_ea, "Delay");
    write_guest_string(str_delrate_ea, "DelayRate");
    write_guest_string(str_fifo_ea, "SendFIFO");
    write_guest_string(str_fifo_left_ea, "LeftTapFIFO");
    write_guest_string(str_fifo_right_ea, "RightTapFIFO");
    write_guest_string(str_patch_name_ea, "Sound3DPatch");

    // Block 1 commands at 0x8000:
    // 0: DEFINE_PORT "Input", 2 parts, port_type=0 (AF_PORT_TYPE_INPUT)
    // 1: DEFINE_PORT "Output", 2 parts, port_type=1 (AF_PORT_TYPE_OUTPUT)
    // 2: DEFINE_PORT "Drift", 2 parts, port_type=1 (AF_PORT_TYPE_OUTPUT)
    // 3: DEFINE_KNOB "Amplitude", 2 parts, def=1.0f
    // 4: DEFINE_KNOB "Delay", 2 parts, def=0.0f
    // 5: DEFINE_KNOB "DelayRate", 1 part, def=-1.0f
    // 6: EXPOSE "SendFIFO"
    // 7: JUMP to Block 2 at 0x8200
    const uint32_t block1_ea = 0x8000u;
    const uint32_t block2_ea = 0x8200u;

    // Cmd 0: DEFINE_PORT "Input" (257)
    wr32(&g_cpu, block1_ea + 0 * 28 + 0, 257u);
    wr32(&g_cpu, block1_ea + 0 * 28 + 4, str_input_ea);
    wr32(&g_cpu, block1_ea + 0 * 28 + 8, 2u);
    wr32(&g_cpu, block1_ea + 0 * 28 + 12, 0u); // AF_PORT_TYPE_INPUT
    wr32(&g_cpu, block1_ea + 0 * 28 + 16, 1u);

    // Cmd 1: DEFINE_PORT "Output" (257)
    wr32(&g_cpu, block1_ea + 1 * 28 + 0, 257u);
    wr32(&g_cpu, block1_ea + 1 * 28 + 4, str_output_ea);
    wr32(&g_cpu, block1_ea + 1 * 28 + 8, 2u);
    wr32(&g_cpu, block1_ea + 1 * 28 + 12, 1u); // AF_PORT_TYPE_OUTPUT
    wr32(&g_cpu, block1_ea + 1 * 28 + 16, 1u);

    // Cmd 2: DEFINE_PORT "Drift" (257)
    wr32(&g_cpu, block1_ea + 2 * 28 + 0, 257u);
    wr32(&g_cpu, block1_ea + 2 * 28 + 4, str_drift_ea);
    wr32(&g_cpu, block1_ea + 2 * 28 + 8, 2u);
    wr32(&g_cpu, block1_ea + 2 * 28 + 12, 1u); // AF_PORT_TYPE_OUTPUT
    wr32(&g_cpu, block1_ea + 2 * 28 + 16, 1u);

    // Cmd 3: DEFINE_KNOB "Amplitude" (258)
    wr32(&g_cpu, block1_ea + 3 * 28 + 0, 258u);
    wr32(&g_cpu, block1_ea + 3 * 28 + 4, str_amp_ea);
    wr32(&g_cpu, block1_ea + 3 * 28 + 8, 2u);
    wr32(&g_cpu, block1_ea + 3 * 28 + 12, 1u);
    wrf32(&g_cpu, block1_ea + 3 * 28 + 16, 1.0f);

    // Cmd 4: DEFINE_KNOB "Delay" (258)
    wr32(&g_cpu, block1_ea + 4 * 28 + 0, 258u);
    wr32(&g_cpu, block1_ea + 4 * 28 + 4, str_del_ea);
    wr32(&g_cpu, block1_ea + 4 * 28 + 8, 2u);
    wr32(&g_cpu, block1_ea + 4 * 28 + 12, 1u);
    wrf32(&g_cpu, block1_ea + 4 * 28 + 16, 0.0f);

    // Cmd 5: DEFINE_KNOB "DelayRate" (258)
    wr32(&g_cpu, block1_ea + 5 * 28 + 0, 258u);
    wr32(&g_cpu, block1_ea + 5 * 28 + 4, str_delrate_ea);
    wr32(&g_cpu, block1_ea + 5 * 28 + 8, 1u);
    wr32(&g_cpu, block1_ea + 5 * 28 + 12, 1u);
    wrf32(&g_cpu, block1_ea + 5 * 28 + 16, -1.0f);

    // Cmd 6: EXPOSE "SendFIFO" (259)
    wr32(&g_cpu, block1_ea + 6 * 28 + 0, 259u);
    wr32(&g_cpu, block1_ea + 6 * 28 + 4, str_fifo_ea);

    // Cmd 7: JUMP to block2 (0xfffffffeu)
    wr32(&g_cpu, block1_ea + 7 * 28 + 0, 0xfffffffeu);
    wr32(&g_cpu, block1_ea + 7 * 28 + 4, block2_ea);

    // Block 2:
    // Cmd 0: EXPOSE "LeftTapFIFO" (259)
    wr32(&g_cpu, block2_ea + 0 * 28 + 0, 259u);
    wr32(&g_cpu, block2_ea + 0 * 28 + 4, str_fifo_left_ea);

    // Cmd 1: EXPOSE "RightTapFIFO" (259)
    wr32(&g_cpu, block2_ea + 1 * 28 + 0, 259u);
    wr32(&g_cpu, block2_ea + 1 * 28 + 4, str_fifo_right_ea);

    // Cmd 2: END (0)
    wr32(&g_cpu, block2_ea + 2 * 28 + 0, 0u);

    // Tag list for CreatePatchTemplate: TAG_ITEM_NAME, str_patch_name_ea, TAG_END
    const uint32_t patch_tags_ea = 0x8500u;
    wr32(&g_cpu, patch_tags_ea + 0, 1u /* TAG_ITEM_NAME */);
    wr32(&g_cpu, patch_tags_ea + 4, str_patch_name_ea);
    wr32(&g_cpu, patch_tags_ea + 8, 0u /* TAG_END */);
    wr32(&g_cpu, patch_tags_ea + 12, 0u);

    // Invoke CreatePatchTemplate via selector 5:0 at lifted site 0x6d9a4
    g_cpu.gpr[3] = block1_ea;
    g_cpu.gpr[4] = patch_tags_ea;
    recomp_call_selector(&g_cpu, 5, 0, 0x6d9a4u);
    uint32_t patch_template = g_cpu.gpr[3];
    CHECK(patch_template > 0 && (int32_t)patch_template > 0);
    CHECK(hdl->CheckItem(patch_template) == 0);

    // CreateDelayLine is the source convenience wrapper around a Sample Item
    // with AF_TAG_DELAY_LINE/CHANNELS/RELEASE*; keep the generated wrapper in
    // this contract test so a delay line cannot silently fall through the
    // ordinary guest-PCM upload path.
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = 200u; // bytes
    g_cpu.gpr[4] = 1u;   // mono
    g_cpu.gpr[5] = 1u;   // circular release loop
    FN_CreateDelayLine__733e0(&g_cpu);
    uint32_t dl_sample = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(dl_sample != M2K_ITEM_NONE);

    // A normal uploaded sample attached to a patch template is the observable
    // propagation witness: CreateInstrument must clone the attachment to the
    // instrument before StartInstrument starts its sample voice.
    const uint32_t propagated_tags_ea = 0x8a00u;
    const uint32_t propagated_data_ea = 0x8b00u;
    g_mem[propagated_data_ea + 0u] = 0x00u;
    g_mem[propagated_data_ea + 1u] = 0x07u;
    g_mem[propagated_data_ea + 2u] = 0x00u;
    g_mem[propagated_data_ea + 3u] = 0x09u;
    wr32(&g_cpu, propagated_tags_ea + 0u, M2_AUDIO_TAG_ADDRESS);
    wr32(&g_cpu, propagated_tags_ea + 4u, propagated_data_ea);
    wr32(&g_cpu, propagated_tags_ea + 8u, M2_AUDIO_TAG_NUMBYTES);
    wr32(&g_cpu, propagated_tags_ea + 12u, 4u);
    wr32(&g_cpu, propagated_tags_ea + 16u, M2_AUDIO_TAG_CHANNELS);
    wr32(&g_cpu, propagated_tags_ea + 20u, 1u);
    wr32(&g_cpu, propagated_tags_ea + 24u, M2_AUDIO_TAG_NUMBITS);
    wr32(&g_cpu, propagated_tags_ea + 28u, 16u);
    wr32(&g_cpu, propagated_tags_ea + 32u, M2_TAG_END);
    wr32(&g_cpu, propagated_tags_ea + 36u, 0u);
    uint32_t propagated_sample = hdl->CreateItem(
        M2_AUDIO_SAMPLE_CTYPE, propagated_tags_ea);
    CHECK(propagated_sample != M2K_ITEM_NONE);

    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_template;
    g_cpu.gpr[4] = dl_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t dl_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(dl_attachment != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(dl_attachment) == 0);

    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_template;
    g_cpu.gpr[4] = propagated_sample;
    g_cpu.gpr[5] = 0;
    FN_CreateAttachment__73394(&g_cpu);
    uint32_t propagated_attachment = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(propagated_attachment != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(propagated_attachment) == 0);

    // Create an instrument from the patch template via CreateInstrument
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_template;
    g_cpu.gpr[4] = 0;
    FN_CreateInstrument__73734(&g_cpu);
    uint32_t patch_ins = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(patch_ins != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(patch_ins) == 0);

    g_started_sample = 0u;
    g_started_attachment = 0u;
    CHECK(m2_guest_audio_start_instrument(&g_cpu, patch_ins, 0) == M2K_OK);
    CHECK(g_started_sample != 0u);
    CHECK(g_started_attachment != 0u);
    CHECK(g_started_attachment != propagated_attachment);

    // Create knobs on the patch instrument and verify default values:
    // 1. "Amplitude" (2 parts, def=1.0)
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_ins;
    g_cpu.gpr[4] = str_amp_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t amp_knob = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(amp_knob != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(amp_knob) == 0);

    const uint32_t knob_read_ea = 0x8600u;
    // Verify default amplitude is 1.0f on both parts
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, amp_knob, 0, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == 1.0f);
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, amp_knob, 1, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == 1.0f);

    // SetKnobPart updates the value
    CHECK(m2_guest_audio_set_knob_part(&g_cpu, amp_knob, 0, 0.75f) == M2K_OK);
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, amp_knob, 0, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == 0.75f);
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, amp_knob, 1, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == 1.0f);

    // 2. "Delay" (2 parts, def=0.0)
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_ins;
    g_cpu.gpr[4] = str_del_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t del_knob = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(del_knob != M2K_ITEM_NONE);
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, del_knob, 0, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == 0.0f);

    // 3. "DelayRate" (1 part, def=-1.0)
    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_ins;
    g_cpu.gpr[4] = str_delrate_ea;
    g_cpu.gpr[5] = 0;
    FN_CreateKnob__73778(&g_cpu);
    uint32_t delrate_knob = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(delrate_knob != M2K_ITEM_NONE);
    CHECK(m2_guest_audio_read_knob_part(&g_cpu, delrate_knob, 0, knob_read_ea) == M2K_OK);
    CHECK(rdf32(&g_cpu, knob_read_ea) == -1.0f);

    // Create probe with AF_TAG_TYPE (75) tag list
    const uint32_t probe_taglist_ea = 0x8700u;
    wr32(&g_cpu, probe_taglist_ea + 0, 75u /* AF_TAG_TYPE */);
    wr32(&g_cpu, probe_taglist_ea + 4, 1u /* AF_SIGNAL_TYPE_WHOLE_NUMBER */);
    wr32(&g_cpu, probe_taglist_ea + 8, 0u /* TAG_END */);
    wr32(&g_cpu, probe_taglist_ea + 12, 0u);

    m2_kernel_dispatch_set_global(hdl);
    g_cpu.gpr[3] = patch_ins;
    g_cpu.gpr[4] = str_drift_ea;
    g_cpu.gpr[5] = probe_taglist_ea;
    FN_CreateProbe__7392c(&g_cpu);
    uint32_t drift_probe = g_cpu.gpr[3];
    m2_kernel_dispatch_set_global(NULL);
    CHECK(drift_probe != M2K_ITEM_NONE);
    CHECK(hdl->CheckItem(drift_probe) == 0);

    // Connect source to patch "Input"
    write_guest_string(0x6000u, "sampler_16_f2.dsp");
    int32_t snd_source = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100);
    CHECK(snd_source > 0);
    const uint32_t dsp_out_str_ea = 0x7200u;
    write_guest_string(dsp_out_str_ea, "Output");
    CHECK(m2_guest_audio_connect_instrument_parts(
              &g_cpu, (uint32_t)snd_source, dsp_out_str_ea, 0,
              patch_ins, str_input_ea, 0) == M2K_OK);

    // Clean up: delete instrument and template
    hdl->DeleteItem(patch_ins);
    CHECK(hdl->CheckItem(g_started_attachment) != 0);
    CHECK(hdl->CheckItem(amp_knob) != 0);
    CHECK(hdl->CheckItem(del_knob) != 0);
    CHECK(hdl->CheckItem(delrate_knob) != 0);
    CHECK(hdl->CheckItem(drift_probe) != 0);

    hdl->DeleteItem(patch_template);
    CHECK(hdl->CheckItem(dl_attachment) != 0);
    hdl->DeleteItem(dl_sample);
    hdl->DeleteItem(propagated_sample);
    CHECK(m2_guest_audio_unload_instrument((uint32_t)snd_source) == M2K_OK);
    teardown();

    // ---- pure-synth DSPP execution boundary (2026-09-08) --------------------
    // RBASE-to-FIFO binding uses dsppBindResource's FIFO attribute lookup,
    // not the raw channel number. Keep this small loader contract explicit
    // even though the disc templates currently expose only data-RBASE binds.
    {
        dspp_template fifo_bind = {0};
        fifo_bind.code_word_count = 1;
        fifo_bind.code_words[0] = 0x8380u; // DSPN_OPCODE_SLEEP
        fifo_bind.resource_count = 2;
        snprintf(fifo_bind.resources[0].name,
                 sizeof(fifo_bind.resources[0].name), "InFIFO");
        fifo_bind.resources[0].type = 5; // DRSC_TYPE_IN_FIFO
        fifo_bind.resources[0].many = 1;
        snprintf(fifo_bind.resources[1].name,
                 sizeof(fifo_bind.resources[1].name), "FIFOBase");
        fifo_bind.resources[1].type = 8; // DRSC_TYPE_RBASE
        fifo_bind.resources[1].flags = 0x20; // DRSC_F_BIND
        fifo_bind.resources[1].many = 1;
        fifo_bind.resources[1].allocated = 0; // DRSC_FIFO_NORMAL
        fifo_bind.resources[1].bind_index = 0;
        dspp_sim *private_fifo = dspp_sim_create(&fifo_bind);
        dspp_sim_space *fifo_space = dspp_sim_space_create();
        dspp_sim *shared_fifo = dspp_sim_create_in_space(&fifo_bind,
                                                         fifo_space);
        CHECK(private_fifo != NULL && fifo_space != NULL &&
              shared_fifo != NULL);
        if (private_fifo && shared_fifo) {
            const dspp_template *private_res =
                dspp_sim_resources(private_fifo);
            const dspp_template *shared_res =
                dspp_sim_resources(shared_fifo);
            CHECK(private_res->resources[1].alloc_value == 0x304u);
            CHECK(shared_res->resources[1].alloc_value == 0x304u);
        }
        dspp_sim_destroy(shared_fifo);
        dspp_sim_space_destroy(fifo_space);
        dspp_sim_destroy(private_fifo);
    }

    // The vendor simulator core (dsp_template_loader.c) executes the original
    // on-disc DCOD bytes. Two template-math regressions, straight from the
    // vendor .ins sources:
    //   add.dsp:  Out = InputA + InputB  (test: 1000 + 2000 = 3000)
    //   svfilter.dsp state-variable filter with Frequency=0x2000,
    //   Resonance=0x1000, Amplitude=0x7FFF, input 1200: the first-tick
    //   high-pass output is 1200 (HighPass = InSamp - A with A starting 0).
    // The svfilter test also pins the INPUT resource contract: the Input
    // resource relocates to packed immediate 0, so the source value enters
    // through DSPI_INPUT0, not data memory.
    setup();
    {
        // Strong raw-file bridge scoped to this test: serve the disc DSP
        // templates from the configured game data.
        static int dsp_file_loaded;

        // Locate the disc root from the test CWD (same walk as the bridge).
        const char *root = IMSA_TEST_GAME_DIR;
        char add_path[1200], svf_path[1200], lo_path[1200], nk_path[1200];
        char envelope_path[1200];
        char sampler_path[1200], trigger_path[1200];
        snprintf(add_path, sizeof(add_path),
                 "%s/System.m2/Audio/dsp/add.dsp", root);
        snprintf(svf_path, sizeof(svf_path),
                 "%s/System.m2/Audio/dsp/svfilter.dsp", root);
        snprintf(lo_path, sizeof(lo_path),
                 "%s/System.m2/Audio/dsp/line_out.dsp", root);
        snprintf(nk_path, sizeof(nk_path),
                 "%s/System.m2/Audio/dsp/nanokernel.dsp",
                 root);
        snprintf(envelope_path, sizeof(envelope_path),
                 "%s/System.m2/Audio/dsp/envelope.dsp",
                 root);
        snprintf(sampler_path, sizeof(sampler_path),
                 "%s/System.m2/Audio/dsp/sampler_16_f2.dsp",
                 root);
        snprintf(trigger_path, sizeof(trigger_path),
                 "%s/System.m2/Audio/dsp/schmidt_trigger.dsp",
                 root);
        FILE *f = fopen(add_path, "rb");
        if (f) {
            fclose(f);
            dsp_file_loaded = 1;
        }
        if (dsp_file_loaded) {
            // The Bulldog folio owns one FIFO-channel table and one trigger
            // table for the whole DSPP, not one namespace per instrument.
            // Create two copies of each on-disc resource-bearing template in
            // one shared space and verify that relocation receives distinct
            // global allocations. The sampler feeds are pushed through those
            // actual channel numbers so the resource addresses are exercised
            // by the simulator, not only compared as metadata.
            FILE *fsampler = fopen(sampler_path, "rb");
            FILE *ftrigger = fopen(trigger_path, "rb");
            if (fsampler && ftrigger) {
                static uint8_t sampler_bytes[65536];
                static uint8_t trigger_bytes[65536];
                uint32_t sampler_n =
                    (uint32_t)fread(sampler_bytes, 1,
                                    sizeof(sampler_bytes), fsampler);
                uint32_t trigger_n =
                    (uint32_t)fread(trigger_bytes, 1,
                                    sizeof(trigger_bytes), ftrigger);
                fclose(fsampler);
                fclose(ftrigger);
                dspp_template sampler_t, trigger_t;
                CHECK(sampler_n > 0 && trigger_n > 0);
                CHECK(dspp_load_template(sampler_bytes, sampler_n,
                                         &sampler_t) == 0);
                CHECK(dspp_load_template(trigger_bytes, trigger_n,
                                         &trigger_t) == 0);
                dspp_sim_space *shared = dspp_sim_space_create();
                CHECK(shared != NULL);
                dspp_sim *sampler_a =
                    dspp_sim_create_in_space(&sampler_t, shared);
                dspp_sim *sampler_b =
                    dspp_sim_create_in_space(&sampler_t, shared);
                dspp_sim *trigger_a =
                    dspp_sim_create_in_space(&trigger_t, shared);
                dspp_sim *trigger_b =
                    dspp_sim_create_in_space(&trigger_t, shared);
                CHECK(sampler_a && sampler_b && trigger_a && trigger_b);
                if (sampler_a && sampler_b && trigger_a && trigger_b) {
                    const dspp_template *sa_res =
                        dspp_sim_resources(sampler_a);
                    const dspp_template *sb_res =
                        dspp_sim_resources(sampler_b);
                    const dspp_template *ta_res =
                        dspp_sim_resources(trigger_a);
                    const dspp_template *tb_res =
                        dspp_sim_resources(trigger_b);
                    int fifo_a = -1, fifo_b = -1;
                    int trig_a = -1, trig_b = -1;
                    int out_a = -1, out_b = -1;
                    for (uint32_t r = 0; r < sa_res->resource_count;
                         ++r) {
                        if (strcmp(sa_res->resources[r].name, "InFIFO") == 0)
                            fifo_a = (int)sa_res->resources[r].alloc_value;
                        if (strcmp(sa_res->resources[r].name, "Output") == 0)
                            out_a = (int)sa_res->resources[r].alloc_value;
                    }
                    for (uint32_t r = 0; r < sb_res->resource_count;
                         ++r) {
                        if (strcmp(sb_res->resources[r].name, "InFIFO") == 0)
                            fifo_b = (int)sb_res->resources[r].alloc_value;
                        if (strcmp(sb_res->resources[r].name, "Output") == 0)
                            out_b = (int)sb_res->resources[r].alloc_value;
                    }
                    for (uint32_t r = 0; r < ta_res->resource_count;
                         ++r)
                        if (strcmp(ta_res->resources[r].name, "Trigger") == 0)
                            trig_a = (int)ta_res->resources[r].alloc_value;
                    for (uint32_t r = 0; r < tb_res->resource_count;
                         ++r)
                        if (strcmp(tb_res->resources[r].name, "Trigger") == 0)
                            trig_b = (int)tb_res->resources[r].alloc_value;
                    CHECK(fifo_a >= 0 && fifo_b >= 0 && fifo_a != fifo_b);
                    CHECK(trig_a >= 0 && trig_b >= 0 && trig_a != trig_b);
                    CHECK(out_a >= 0 && out_b >= 0 && out_a != out_b);
                    dspp_sim_fifo_push(sampler_a, (uint32_t)fifo_a, 1000);
                    dspp_sim_fifo_push(sampler_b, (uint32_t)fifo_b, 2000);
                    dspp_sim_tick(sampler_a);
                    dspp_sim_tick(sampler_b);
                    // The sampler's Output resource is a data-memory
                    // endpoint; the template's separate DSPI output writes
                    // are not the resource word checked above. The ticks
                    // nevertheless execute the two distinct FIFO addresses.
                }
                dspp_sim_destroy(trigger_b);
                dspp_sim_destroy(trigger_a);
                dspp_sim_destroy(sampler_b);
                dspp_sim_destroy(sampler_a);
                dspp_sim_space_destroy(shared);
            } else {
                if (fsampler) fclose(fsampler);
                if (ftrigger) fclose(ftrigger);
                printf("m2_audio_items_test: FIFO/trigger templates not "
                       "found; shared resource regression skipped\n");
            }

            // Bulldog DSPI frequency, phase, and last-oscillator-count
            // registers are single global registers. The FIFO channel in the
            // address selects which queue the oscillator advances; it does
            // not select a private frequency or phase bank. Exercise both
            // channels and the vendor readback contract.
            {
                dspp_template oscillator = {0};
                oscillator.code_word_count = 1;
                oscillator.code_words[0] = 0x8380; // SLEEP
                dspp_sim_space *osc_space = dspp_sim_space_create();
                dspp_sim *osc_a =
                    dspp_sim_create_in_space(&oscillator, osc_space);
                dspp_sim *osc_b =
                    dspp_sim_create_in_space(&oscillator, osc_space);
                CHECK(osc_space != NULL && osc_a != NULL && osc_b != NULL);
                if (osc_a && osc_b) {
                    dspp_sim_fifo_push(osc_a, 0, 111);
                    dspp_sim_fifo_push(osc_a, 0, 222);
                    dspp_sim_fifo_push(osc_b, 1, 333);

                    // FIFO contents and cursors are also one physical
                    // per-channel state block, not private to the sim that
                    // happened to feed the channel.
                    dspp_sim_fifo_push(osc_a, 2, 444);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x310u) == 444u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x315u) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x314u) == 444u);
                    CHECK(dspp_sim_dspi_read(osc_a, 0x315u) == 0u);

                    // phase + frequency = 0xFFFF: one sample consumed from
                    // channel 0, then phase clips to 15 fractional bits.
                    dspp_sim_dspi_write(osc_a, 0x303u, 0x7FFFu);
                    dspp_sim_dspi_write(osc_a, 0x302u, 0x8000u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x302u) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x30Au) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x303u) == 0x7FFFu);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x30Bu) == 0x7FFFu);
                    CHECK(dspp_sim_dspi_read(osc_a, 0x305u) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x30Du) == 1u);

                    // A phase write through the second shared sim changes
                    // the same global phase; the next frequency write
                    // advances channel 1 and leaves channel 0's queued
                    // sample count untouched.
                    dspp_sim_dspi_write(osc_b, 0x30Bu, 0u);
                    dspp_sim_dspi_write(osc_b, 0x30Au, 0x8000u);
                    CHECK(dspp_sim_dspi_read(osc_a, 0x30Au) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_a, 0x303u) == 0u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x30Bu) == 0u);
                    CHECK(dspp_sim_dspi_read(osc_a, 0x305u) == 1u);
                    CHECK(dspp_sim_dspi_read(osc_b, 0x30Du) == 0u);
                }
                dspp_sim_destroy(osc_b);
                dspp_sim_destroy(osc_a);
                dspp_sim_space_destroy(osc_space);
            }

            // The vendor FDMA service runs before each DSP instruction,
            // moves at most four samples, advances the current descriptor,
            // and promotes a valid next descriptor with a DMANext signal.
            // Exercise input, 8-bit input, and DSPP-to-RAM output through two
            // shared sims so descriptor ownership is visibly global.
            {
                dspp_template dma_template = {0};
                dma_template.code_word_count = 1;
                dma_template.code_words[0] = 0x8380; // SLEEP
                dspp_sim_space *dma_space = dspp_sim_space_create();
                dspp_sim *dma_a =
                    dspp_sim_create_in_space(&dma_template, dma_space);
                dspp_sim *dma_b =
                    dspp_sim_create_in_space(&dma_template, dma_space);
                CHECK(dma_space != NULL && dma_a != NULL && dma_b != NULL);
                if (dma_a && dma_b) {
                    uint8_t input0[4] = {0x12, 0x34, 0x56, 0x78};
                    uint8_t input1[4] = {0x9A, 0xBC, 0xDE, 0xF0};
                    CHECK(dspp_sim_dma_set_current(dma_a, 3, input0, 2,
                                                   0) == 0);
                    CHECK(dspp_sim_dma_set_next(dma_a, 3, input1, 2) == 0);
                    dspp_sim_dma_set_control(dma_a, 3, 1, 0);
                    CHECK(dspp_sim_dma_step(dma_b) == 3);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x318u) == 0x1234u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x319u) == 0x5678u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x31Du) == 2u);
                    CHECK((dspp_sim_dma_dmanext_interrupts(dma_b) &
                           (1u << 3)) != 0u);
                    CHECK(dspp_sim_dma_step(dma_b) == 3);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x31Du) == 4u);
                    dspp_sim_dma_clear_dmanext_interrupts(dma_a, 1u << 3);
                    CHECK((dspp_sim_dma_dmanext_interrupts(dma_b) &
                           (1u << 3)) == 0u);

                    // The vendor DSPX stack writes address/count fields into
                    // shared shadow registers; current/next control writes
                    // commit those fields atomically. Exercise the raw
                    // register boundary through the second shared sim, then
                    // cover the separate channel-mask and interrupt banks.
                    {
                        const uint32_t raw_channel = 7u;
                        const uint32_t raw_mask = 1u << raw_channel;
                        const uint32_t raw_stack =
                            0x65000u + raw_channel * 0x10u;
                        const uint32_t raw_control =
                            0x65200u + raw_channel * 0x10u;
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 0u) ==
                              0u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 4u) ==
                              0u);
                        dspp_sim_dma_register_write(dma_a, raw_stack + 0u,
                                                    0x00123400u);
                        dspp_sim_dma_register_write(dma_a, raw_stack + 4u,
                                                    3u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 0u) ==
                              0u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 4u) ==
                              0u);
                        dspp_sim_dma_register_write(
                            dma_a, raw_control, 0x80000000u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 0u) ==
                              0x00123400u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 4u) ==
                              3u);

                        dspp_sim_dma_register_write(dma_a, raw_stack + 8u,
                                                    0x00ABC000u);
                        dspp_sim_dma_register_write(dma_a, raw_stack + 12u,
                                                    5u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 8u) ==
                              0u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 12u) ==
                              0u);
                        dspp_sim_dma_register_write(
                            dma_a, raw_control + 8u,
                            0x80000000u | 0x00010000u | 0x00020000u |
                                0x00040000u | 0x0004u | 0x0002u | 0x0001u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 8u) ==
                              0x00ABC000u);
                        CHECK(dspp_sim_dma_register_read(dma_b,
                                                          raw_stack + 12u) ==
                              5u);
                        CHECK((dspp_sim_dma_register_read(dma_b,
                                                          raw_control + 8u) &
                               0x3u) == 0x3u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64018u) &
                               raw_mask) != 0u);

                        // dspsGenMainInt exposes non-DMA partial bits plus
                        // enabled DMANext/Consumed categories; UnderOver is
                        // intentionally not folded into this value by the
                        // vendor simulator.
                        dspp_sim_dma_register_write(dma_a, 0x64008u,
                                                    0x3u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64008u) &
                               0x3u) == 0x3u);
                        dspp_sim_dma_register_write(dma_a, 0x64010u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x1u) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64014u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x1u) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64028u,
                                                    raw_mask);
                        dspp_sim_dma_register_write(dma_a, 0x64020u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x2u) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64024u,
                                                    raw_mask);
                        dspp_sim_dma_register_write(dma_a, 0x64000u,
                                                    0x100u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x100u) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64004u,
                                                    0x100u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x100u) == 0u);

                        dspp_sim_dma_register_write(dma_a, 0x66000u,
                                                    raw_mask);
                        dspp_sim_dma_register_write(dma_a, 0x66008u,
                                                    raw_mask);
                        dspp_sim_dma_register_write(dma_a, 0x66010u,
                                                    raw_mask);
                        dspp_sim_dma_register_write(dma_a, 0x66018u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x66000u) &
                               raw_mask) != 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x66008u) &
                               raw_mask) != 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x66010u) &
                               raw_mask) != 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x66018u) &
                               raw_mask) != 0u);

                        dspp_sim_dma_register_write(dma_a, 0x64010u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64010u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64014u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64010u) &
                               raw_mask) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64030u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64030u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64034u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64030u) &
                               raw_mask) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64020u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64020u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64024u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64020u) &
                               raw_mask) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64028u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64028u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x6402Cu,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64028u) &
                               raw_mask) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64038u,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64038u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x6403Cu,
                                                    raw_mask);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64038u) &
                               raw_mask) == 0u);

                        dspp_sim_dspi_write(dma_a, 0x33Cu, 0x1111u);
                        CHECK(dspp_sim_dspi_read(dma_b, 0x33Du) == 1u);
                        dspp_sim_dma_register_write(dma_a, 0x66030u,
                                                    raw_mask);
                        CHECK(dspp_sim_dspi_read(dma_b, 0x33Du) == 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x6603Cu) &
                               raw_mask) == 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64038u,
                                                    raw_mask);
                        CHECK(dspp_sim_dspi_read(dma_b, 0x33Cu) == 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64030u) &
                               raw_mask) != 0u);
                        dspp_sim_dma_register_write(dma_a, 0x64034u,
                                                    raw_mask);
                        dspp_sim_fifo_push(dma_a, raw_channel, 0x2468);
                        uint16_t underflow_sample =
                            dspp_sim_dspi_read(dma_b, 0x33Cu);
                        CHECK(underflow_sample == 0x2468u);
                        CHECK(dspp_sim_dspi_read(dma_b, 0x33Cu) == 0x2468u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64030u) &
                               raw_mask) != 0u);
                        CHECK((dspp_sim_dma_register_read(dma_b, 0x64000u) &
                               0x8u) == 0u);
                    }

                    uint8_t input8[2] = {0x80, 0x7F};
                    CHECK(dspp_sim_dma_set_current(
                              dma_a, 4, input8, 2, DSPP_SIM_DMA_8BIT) == 0);
                    dspp_sim_dma_set_control(dma_a, 4, 1, 0);
                    CHECK(dspp_sim_dma_step(dma_b) == 4);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x320u) == 0x8000u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x324u) == 0x8000u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x324u) == 0x7F00u);

                    uint8_t sqxd[3] = {0x10, 0x01, 0x02};
                    CHECK(dspp_sim_dma_set_current(
                              dma_a, 6, sqxd, 3,
                              DSPP_SIM_DMA_8BIT | DSPP_SIM_DMA_SQXD) == 0);
                    dspp_sim_dma_set_control(dma_a, 6, 1, 0);
                    CHECK(dspp_sim_dma_step(dma_b) == 6);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x330u) == 0x0200u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x334u) == 0x0200u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x334u) == 0x0200u);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x334u) == 0x0008u);

                    uint8_t output[8] = {0};
                    // DSPP-to-RAM service consumes the DMA cursor, so fill
                    // this direction through the DSPI FIFO data register,
                    // not the host/DMA input cursor used by fifo_push.
                    dspp_sim_dspi_write(dma_a, 0x32Cu, 0x1122u);
                    dspp_sim_dspi_write(dma_a, 0x32Cu, 0x3344u);
                    dspp_sim_dspi_write(dma_a, 0x32Cu, 0x5566u);
                    dspp_sim_dspi_write(dma_a, 0x32Cu, 0x7788u);
                    CHECK(dspp_sim_dma_set_current(
                              dma_a, 5, output, 4, DSPP_SIM_DMA_TO_RAM) == 0);
                    dspp_sim_dma_set_control(dma_a, 5, 1, 0);
                    CHECK(dspp_sim_dma_step(dma_b) == 5);
                    CHECK(output[0] == 0x11 && output[1] == 0x22 &&
                          output[2] == 0x33 && output[3] == 0x44 &&
                          output[4] == 0x55 && output[5] == 0x66 &&
                          output[6] == 0x77 && output[7] == 0x88);
                    CHECK(dspp_sim_dspi_read(dma_b, 0x32Du) == 0u);
                }
                dspp_sim_destroy(dma_b);
                dspp_sim_destroy(dma_a);
                dspp_sim_space_destroy(dma_space);
            }

            // The vendor simulator has one global output-register bank and
            // one four-pair output FIFO. OUTPUT_CONTROL bit 0 commits the
            // current stereo pair, echoes it to INPUT0/1, and advances the
            // writer by two words; INPUT_CONTROL is explicitly a no-op and
            // INPUT_STATUS reads as ready in that bounded simulator.
            {
                dspp_template io = {0};
                io.code_word_count = 1;
                io.code_words[0] = 0x8380; // SLEEP
                dspp_sim_space *io_space = dspp_sim_space_create();
                dspp_sim *io_a =
                    dspp_sim_create_in_space(&io, io_space);
                dspp_sim *io_b =
                    dspp_sim_create_in_space(&io, io_space);
                CHECK(io_space != NULL && io_a != NULL && io_b != NULL);
                if (io_a && io_b) {
                    CHECK(dspp_sim_dspi_read(io_a, 0x3DFu) == 0u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x3DEu) == 1u);
                    dspp_sim_set_input(io_a, 0, 0x1111);
                    dspp_sim_set_input(io_a, 1, 0x2222);
                    dspp_sim_dspi_write(io_a, 0x3D6u, 1u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x2F0u) == 0x1111u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x2F1u) == 0x2222u);

                    dspp_sim_dspi_write(io_a, 0x2E0u, 0x1234u);
                    dspp_sim_dspi_write(io_a, 0x2E1u, 0x5678u);
                    CHECK(dspp_sim_output(io_b, 0) == (int16_t)0x1234);
                    CHECK(dspp_sim_output(io_b, 1) == (int16_t)0x5678);
                    dspp_sim_dspi_write(io_a, 0x3D7u, 0u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x3DFu) == 0u);
                    dspp_sim_dspi_write(io_a, 0x3D7u, 1u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x2F0u) == 0x1234u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x2F1u) == 0x5678u);
                    CHECK(dspp_sim_dspi_read(io_b, 0x3DFu) == 2u);

                    dspp_sim_dspi_write(io_b, 0x2E0u, 0x9ABCu);
                    dspp_sim_dspi_write(io_b, 0x2E1u, 0xDEF0u);
                    dspp_sim_dspi_write(io_b, 0x3D7u, 1u);
                    CHECK(dspp_sim_dspi_read(io_a, 0x2F0u) == 0x9ABCu);
                    CHECK(dspp_sim_dspi_read(io_a, 0x2F1u) == 0xDEF0u);
                    CHECK(dspp_sim_dspi_read(io_a, 0x3DFu) == 4u);
                }
                dspp_sim_destroy(io_b);
                dspp_sim_destroy(io_a);
                dspp_sim_space_destroy(io_space);
            }

            // Vendor dspsAccessPeripheral (Bulldog/M2 single-register
            // form): a DSPP write to DSPI_CPU_INT (0x3E6) raises
            // soft-interrupt bits (Val << 16) in the shared main-interrupt
            // partial bank; the folio FIRQ handler clears them through
            // INTERRUPT_CLR. Pre-fix the write fell into the open-boundary
            // comment and raised nothing.
            {
                dspp_template trig = {0};
                trig.code_word_count = 1;
                trig.code_words[0] = 0x8380; // SLEEP
                dspp_sim_space *trig_space = dspp_sim_space_create();
                dspp_sim *trig_a =
                    dspp_sim_create_in_space(&trig, trig_space);
                dspp_sim *trig_b =
                    dspp_sim_create_in_space(&trig, trig_space);
                CHECK(trig_space != NULL && trig_a != NULL && trig_b != NULL);
                if (trig_a && trig_b) {
                    CHECK((dspp_sim_dma_register_read(trig_b, 0x64000u) &
                           0xFFFF0000u) == 0u);
                    dspp_sim_dspi_write(trig_a, 0x3E6u, 0x0005u);
                    CHECK((dspp_sim_dma_register_read(trig_b, 0x64000u) &
                           0xFFFF0000u) == 0x00050000u);
                    dspp_sim_dspi_write(trig_b, 0x3E6u, 0x8000u);
                    CHECK((dspp_sim_dma_register_read(trig_a, 0x64000u) &
                           0xFFFF0000u) == 0x80050000u);
                    CHECK((dspp_sim_dma_register_read(trig_a, 0x64000u) &
                           0x3u) == 0u);
                    dspp_sim_dma_register_write(trig_a, 0x64004u,
                                                0x00050000u);
                    CHECK((dspp_sim_dma_register_read(trig_b, 0x64000u) &
                           0xFFFF0000u) == 0x80000000u);
                    dspp_sim_dma_register_write(trig_b, 0x64004u,
                                                0x80000000u);
                    CHECK((dspp_sim_dma_register_read(trig_a, 0x64000u) &
                           0xFFFF0000u) == 0u);
                }
                dspp_sim_destroy(trig_b);
                dspp_sim_destroy(trig_a);
                dspp_sim_space_destroy(trig_space);
            }

            // LoadInstrument("add.dsp") -> template Item + instrument Item.
            write_guest_string(0x6000u, "add.dsp");
            int32_t add_ins = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100);
            CHECK(add_ins > 0);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0;
            CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

            // Drive two frames through the simulator's own API to pin the
            // template math: the knobs live at their allocated data words.
            // (The bridge retained a dspp_sim on the instrument; the math
            // regression below goes through the public loader API so the
            // template bytes are the only input.)
            FILE *fa = fopen(add_path, "rb");
            CHECK(fa != NULL);
            static uint8_t add_bytes[65536];
            uint32_t add_n = (uint32_t)fread(add_bytes, 1, sizeof(add_bytes), fa);
            fclose(fa);
            dspp_template t;
            CHECK(dspp_load_template(add_bytes, add_n, &t) == 0);
            dspp_sim *sim = dspp_sim_create(&t);
            CHECK(sim != NULL);
            const dspp_template *res = dspp_sim_resources(sim);
            int ia = -1, ib = -1, outi = -1;
            for (uint32_t r = 0; r < res->resource_count; ++r) {
                if (strcmp(res->resources[r].name, "InputA") == 0) ia = (int)res->resources[r].alloc_value;
                if (strcmp(res->resources[r].name, "InputB") == 0) ib = (int)res->resources[r].alloc_value;
                if (strcmp(res->resources[r].name, "Output") == 0) outi = (int)res->resources[r].alloc_value;
            }
            CHECK(ia >= 0 && ib >= 0 && outi >= 0);
            dspp_sim_poke_data(sim, (uint32_t)ia, 1000);
            dspp_sim_poke_data(sim, (uint32_t)ib, 2000);
            dspp_sim_tick(sim);
            CHECK(dspp_sim_peek_data(sim, (uint32_t)outi) == 3000u);
            dspp_sim_destroy(sim);
            // Connect patches (resource, part) sites only: a wrong-part
            // connect leaves the KNOB relocation intact, a same-sim connect
            // of InputA <- InputB word sums B+B, and disconnect restores the
            // loader default (A+B) while preserving poked data words. Links
            // always re-walk from the pristine image, so loader relocation
            // can never consume the chain (vendor dsppRelocate).
            dspp_sim *csim = dspp_sim_create(&t);
            CHECK(csim != NULL);
            dspp_sim_poke_data(csim, (uint32_t)ia, 1000);
            dspp_sim_poke_data(csim, (uint32_t)ib, 2000);
            dspp_sim_connect(csim, "InputA", 1,
                             (uint16_t)(0x8000u | ((uint32_t)ib & 0x3FFu)));
            dspp_sim_tick(csim);
            CHECK(dspp_sim_peek_data(csim, (uint32_t)outi) == 3000u);
            dspp_sim_connect(csim, "InputA", 0,
                             (uint16_t)(0x8000u | ((uint32_t)ib & 0x3FFu)));
            dspp_sim_tick(csim);
            CHECK(dspp_sim_peek_data(csim, (uint32_t)outi) == 4000u);
            dspp_sim_disconnect(csim, "InputA", 0);
            dspp_sim_tick(csim);
            CHECK(dspp_sim_peek_data(csim, (uint32_t)outi) == 3000u);
            dspp_sim_destroy(csim);

            // svfilter: first-tick high-pass = input - accumulated low state.
            FILE *fs = fopen(svf_path, "rb");
            CHECK(fs != NULL);
            static uint8_t svf_bytes[65536];
            uint32_t svf_n = (uint32_t)fread(svf_bytes, 1, sizeof(svf_bytes), fs);
            fclose(fs);
            dspp_template ts;
            CHECK(dspp_load_template(svf_bytes, svf_n, &ts) == 0);
            dspp_sim *ssim = dspp_sim_create(&ts);
            CHECK(ssim != NULL);
            const dspp_template *sres = dspp_sim_resources(ssim);
            int hp = -1, freq_i = -1, res_i = -1, amp_i = -1, in_i = -1;
            for (uint32_t r = 0; r < sres->resource_count; ++r) {
                const dspp_resource *rr = &sres->resources[r];
                if (strcmp(rr->name, "HighPass") == 0) hp = (int)rr->alloc_value;
                else if (strcmp(rr->name, "Frequency") == 0) freq_i = (int)rr->alloc_value;
                else if (strcmp(rr->name, "Resonance") == 0) res_i = (int)rr->alloc_value;
                else if (strcmp(rr->name, "Amplitude") == 0) amp_i = (int)rr->alloc_value;
                else if (strcmp(rr->name, "Input") == 0) in_i = (int)rr->alloc_value;
            }
            CHECK(hp >= 0 && freq_i >= 0 && res_i >= 0 && amp_i >= 0);
            CHECK(in_i == 0); // INPUT carries no data word (sentinel alloc 0)
            // The unconnected INPUT relocates to the vendor's default
            // immediate 0. A connection patches the operand to the source
            // address (dsppConnectInstrumentParts); connect the test input to
            // the DSPI_INPUT0 register so the sample enters the filter.
            dspp_sim_connect(ssim, "Input", 0, (uint16_t)(0x8000u | 0x02F0u));
            dspp_sim_poke_data(ssim, (uint32_t)freq_i, 0x2000);
            dspp_sim_poke_data(ssim, (uint32_t)res_i, 0x1000);
            dspp_sim_poke_data(ssim, (uint32_t)amp_i, 0x7FFF);
            dspp_sim_set_input(ssim, 0, 1200);
            dspp_sim_tick(ssim);
            // HighPass = InSamp - LowState; first tick the low state is 0.
            CHECK(dspp_sim_peek_data(ssim, (uint32_t)hp) == 1200u);
            dspp_sim_destroy(ssim);

            // line_out.dsp imports gMixer from the nanokernel. Since the
            // importer-kernel repair the shared folio space starts the
            // on-disc nanokernel head first (vendor folio init) and its
            // DRSC_F_EXPORT gMixer resolves the import, so StartInstrument
            // succeeds instead of failing AF_ERR_EXTERNALREF.
            FILE *fl = fopen(lo_path, "rb");
            CHECK(fl != NULL);
            static uint8_t lo_bytes[65536];
            uint32_t lo_n = (uint32_t)fread(lo_bytes, 1, sizeof(lo_bytes), fl);
            fclose(fl);
            dspp_template tl;
            CHECK(dspp_load_template(lo_bytes, lo_n, &tl) == 0);
            int has_import = 0;
            for (uint32_t r = 0; r < tl.resource_count; ++r)
                if (tl.resources[r].flags & 0x80u) has_import = 1;
            CHECK(has_import);
            write_guest_string(0x6100u, "line_out.dsp");
            int32_t lo_ins = m2_guest_audio_load_instrument(&g_cpu, 0x6100u, 2, 100);
            CHECK(lo_ins > 0);
            g_cpu.gpr[3] = (uint32_t)lo_ins;
            g_cpu.gpr[4] = 0;
            CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            // Import resolution contract, loader level: in a fresh space
            // with the nanokernel head ensured, a line_out open resolves its
            // gMixer import to the head's allocated data words (inside the
            // M2 data memory), while an unknown name still fails.
            {
                dspp_sim_space *imp_space = dspp_sim_space_create();
                CHECK(dspp_sim_space_ensure_kernel(imp_space) == 0);
                dspp_sim *lo2 = dspp_sim_create_in_space(&tl, imp_space);
                CHECK(lo2 != NULL);
                uint32_t lo_gmixer = 0xFFFFFFFFu;
                const dspp_template *lo_res = dspp_sim_resources(lo2);
                for (uint32_t r = 0; lo_res && r < lo_res->resource_count;
                     ++r) {
                    if (strcmp(lo_res->resources[r].name, "gMixer") == 0)
                        lo_gmixer = lo_res->resources[r].alloc_value;
                }
                CHECK(lo_gmixer < 0x2E0u);
                // Poking the head's gMixer word (same shared image) is
                // visible through the importer: one M2 data memory.
                dspp_sim_space_poke_kernel_data(imp_space, lo_gmixer, 0x1234u);
                CHECK(dspp_sim_peek_data(lo2, lo_gmixer) == 0x1234u);
                dspp_sim_destroy(lo2);
                dspp_sim_space_destroy(imp_space);
            }
            // Stop the line_out instrument so later blocks start clean.
            CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)lo_ins, 0) ==
                  M2K_OK);

            // Dynamic-link CODE contract: envelope.dsp imports the exported
            // sub_envelope label from sub_envelope.dsp. The vendor audio
            // folio opens one shared library instrument before allocating
            // each client, reference-counts it across clients, and removes
            // it after the final close (audio_instr.c). Exercise the original
            // disc pair so a successful parse or StartInstrument cannot hide
            // an unresolved JSR target.
            {
                FILE *fe = fopen(envelope_path, "rb");
                CHECK(fe != NULL);
                if (fe) {
                    static uint8_t envelope_bytes[65536];
                    uint32_t envelope_n = (uint32_t)fread(
                        envelope_bytes, 1, sizeof(envelope_bytes), fe);
                    fclose(fe);
                    dspp_template envelope;
                    CHECK(dspp_load_template(envelope_bytes, envelope_n,
                                             &envelope) == 0);
                    CHECK(envelope.dynamic_link_count == 1u);
                    CHECK(strcmp(envelope.dynamic_links[0],
                                 "sub_envelope.dsp") == 0);

                    dspp_sim_space *env_space = dspp_sim_space_create();
                    CHECK(env_space != NULL);
                    CHECK(dspp_sim_space_ensure_kernel(env_space) == 0);

                    // A failed dependency open must roll its library slot and
                    // all partial client state back before the real pair is
                    // opened in the same shared address space.
                    dspp_template missing = envelope;
                    snprintf(missing.dynamic_links[0], DSPP_MAX_NAME,
                             "missing_library.dsp");
                    CHECK(dspp_sim_create_in_space(&missing, env_space) ==
                          NULL);

                    dspp_sim *env_a =
                        dspp_sim_create_in_space(&envelope, env_space);
                    dspp_sim *env_b =
                        dspp_sim_create_in_space(&envelope, env_space);
                    CHECK(env_a != NULL && env_b != NULL);

                    uint32_t import_a = 0xFFFFFFFFu, import_b = 0xFFFFFFFFu;
                    uint32_t entry_a = 0xFFFFFFFFu, entry_b = 0xFFFFFFFFu;
                    uint32_t rbase_a = 0xFFFFFFFFu, rbase_b = 0xFFFFFFFFu;
                    uint32_t output_a = 0xFFFFFFFFu;
                    uint32_t amp_a = 0xFFFFFFFFu, request_a = 0xFFFFFFFFu;
                    uint32_t incr_a = 0xFFFFFFFFu, target_a = 0xFFFFFFFFu;
                    uint32_t output_b = 0xFFFFFFFFu;
                    uint32_t amp_b = 0xFFFFFFFFu, request_b = 0xFFFFFFFFu;
                    uint32_t incr_b = 0xFFFFFFFFu, target_b = 0xFFFFFFFFu;
                    const dspp_template *env_res_a =
                        dspp_sim_resources(env_a);
                    const dspp_template *env_res_b =
                        dspp_sim_resources(env_b);
                    for (uint32_t r = 0;
                         env_res_a && r < env_res_a->resource_count; ++r) {
                        const dspp_resource *rr = &env_res_a->resources[r];
                        if (strcmp(rr->name, "sub_envelope") == 0)
                            import_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Entry") == 0)
                            entry_a = rr->alloc_value;
                        else if (strcmp(rr->name, "DataRBase") == 0)
                            rbase_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Output") == 0)
                            output_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Amplitude") == 0)
                            amp_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.request") == 0)
                            request_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.incr") == 0)
                            incr_a = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.target") == 0)
                            target_a = rr->alloc_value;
                    }
                    for (uint32_t r = 0;
                         env_res_b && r < env_res_b->resource_count; ++r) {
                        const dspp_resource *rr = &env_res_b->resources[r];
                        if (strcmp(rr->name, "sub_envelope") == 0)
                            import_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Entry") == 0)
                            entry_b = rr->alloc_value;
                        else if (strcmp(rr->name, "DataRBase") == 0)
                            rbase_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Output") == 0)
                            output_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Amplitude") == 0)
                            amp_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.request") == 0)
                            request_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.incr") == 0)
                            incr_b = rr->alloc_value;
                        else if (strcmp(rr->name, "Env.target") == 0)
                            target_b = rr->alloc_value;
                    }
                    CHECK(import_a != 0xFFFFFFFFu && import_a == import_b);
                    CHECK(entry_a != 0xFFFFFFFFu && entry_a != import_a);
                    CHECK(entry_b != 0xFFFFFFFFu && entry_b != entry_a);
                    CHECK(rbase_a < DSPP_DATA_MEMORY_WORDS);
                    CHECK(rbase_b < DSPP_DATA_MEMORY_WORDS &&
                          rbase_b != rbase_a);

                    // envelope.ins/sub_envelope.ins: a request change first
                    // captures source/target; the next pass advances phase
                    // and produces a nonzero Amplitude-scaled Output. This
                    // crosses the relocated JSR and returns into the client.
                    CHECK(output_a < DSPP_DATA_MEMORY_WORDS &&
                          amp_a < DSPP_DATA_MEMORY_WORDS &&
                          request_a < DSPP_DATA_MEMORY_WORDS &&
                          incr_a < DSPP_DATA_MEMORY_WORDS &&
                          target_a < DSPP_DATA_MEMORY_WORDS);
                    dspp_sim_poke_data(env_a, amp_a, 0x7FFFu);
                    dspp_sim_poke_data(env_a, request_a, 0x4000u);
                    dspp_sim_poke_data(env_a, incr_a, 0x4000u);
                    dspp_sim_poke_data(env_a, target_a, 0u);
                    dspp_sim_tick(env_a);
                    CHECK(dspp_sim_peek_data(env_a, output_a) == 0u);
                    dspp_sim_tick_n(env_a, 4u);
                    CHECK(dspp_sim_peek_data(env_a, output_a) != 0u);

                    // The second client keeps the shared library alive after
                    // the first closes and retains its own RBASE state.
                    dspp_sim_destroy(env_a);
                    CHECK(output_b < DSPP_DATA_MEMORY_WORDS &&
                          amp_b < DSPP_DATA_MEMORY_WORDS &&
                          request_b < DSPP_DATA_MEMORY_WORDS &&
                          incr_b < DSPP_DATA_MEMORY_WORDS &&
                          target_b < DSPP_DATA_MEMORY_WORDS);
                    dspp_sim_poke_data(env_b, amp_b, 0x7FFFu);
                    dspp_sim_poke_data(env_b, request_b, 0x2000u);
                    dspp_sim_poke_data(env_b, incr_b, 0x4000u);
                    dspp_sim_poke_data(env_b, target_b, 0u);
                    dspp_sim_tick(env_b);
                    dspp_sim_tick_n(env_b, 4u);
                    CHECK(dspp_sim_peek_data(env_b, output_b) != 0u);

                    // A live library owns the exported CODE name. After the
                    // final client closes, the same export can be registered
                    // again, proving final-reference teardown.
                    dspp_template export_probe = {0};
                    export_probe.resource_count = 1;
                    snprintf(export_probe.resources[0].name, DSPP_MAX_NAME,
                             "sub_envelope");
                    export_probe.resources[0].type = 0u;     // CODE
                    export_probe.resources[0].flags = 0x40u; // EXPORT
                    export_probe.resources[0].many = 1u;
                    export_probe.code_word_count = 1u;
                    export_probe.code_words[0] = 0x8380u; // SLEEP
                    CHECK(dspp_sim_create_in_space(&export_probe,
                                                   env_space) == NULL);
                    dspp_sim_destroy(env_b);
                    dspp_sim *export_after_close =
                        dspp_sim_create_in_space(&export_probe, env_space);
                    CHECK(export_after_close != NULL);
                    dspp_sim_destroy(export_after_close);

                    // Shared code RAM is finite (0x400 words). Exhaustion
                    // must fail without aliasing an owner, and releasing the
                    // clients must make the same span allocatable again.
                    dspp_template code_probe = {0};
                    code_probe.code_word_count = 1u;
                    code_probe.code_words[0] = 0x8380u; // SLEEP
                    dspp_sim *code_clients[0x400] = {0};
                    uint32_t code_clients_n = 0;
                    while (code_clients_n < 0x400u &&
                           (code_clients[code_clients_n] =
                                dspp_sim_create_in_space(&code_probe,
                                                         env_space)) != NULL)
                        ++code_clients_n;
                    CHECK(code_clients_n > 0u && code_clients_n < 0x400u);
                    CHECK(dspp_sim_create_in_space(&code_probe, env_space) ==
                          NULL);
                    for (uint32_t i = 0; i < code_clients_n; ++i)
                        dspp_sim_destroy(code_clients[i]);
                    dspp_sim *code_after_release =
                        dspp_sim_create_in_space(&code_probe, env_space);
                    CHECK(code_after_release != NULL);
                    dspp_sim_destroy(code_after_release);
                    dspp_sim_space_destroy(env_space);

                    // Runtime seam for the concrete FMV failure: the exact
                    // original template now starts through folio dispatch.
                    write_guest_string(0x6180u, "envelope.dsp");
                    int32_t envelope_ins = m2_guest_audio_load_instrument(
                        &g_cpu, 0x6180u, 1, 100);
                    CHECK(envelope_ins > 0);
                    CHECK(m2_guest_audio_start_instrument(
                              &g_cpu, (uint32_t)envelope_ins, 0) == M2K_OK);
                    CHECK(m2_guest_audio_unload_instrument(
                              (uint32_t)envelope_ins) == M2K_OK);
                }
            }

            // sampler_drift_v1.dsp is the original variable-rate wrapper
            // used by Sound3D delay taps. Its code resource is imported from
            // sub_sampler_16_v1.dsp through the on-disc DLNK dependency; a
            // successful parse alone would not prove that the relocated JSR
            // and the wrapper's Drift update execute in one shared code/data
            // space.
            {
                dspp_template drift_template = {0};
                dspp_sim_space *drift_space = NULL;
                dspp_sim *drift_sim = NULL;
                uint32_t drift_fifo = UINT32_MAX;
                uint32_t drift_output = UINT32_MAX;
                uint32_t drift_phase = UINT32_MAX;
                uint32_t drift_amplitude = UINT32_MAX;
                uint32_t drift_rate = UINT32_MAX;
                CHECK(test_load_disc_dsp_template("sampler_drift_v1.dsp",
                                                   &drift_template) == 0);
                CHECK(drift_template.dynamic_link_count == 1u);
                CHECK(strcmp(drift_template.dynamic_links[0],
                             "sub_sampler_16_v1.dsp") == 0);
                drift_space = dspp_sim_space_create();
                CHECK(drift_space != NULL);
                if (drift_space)
                    drift_sim = dspp_sim_create_in_space(&drift_template,
                                                         drift_space);
                CHECK(drift_sim != NULL);
                if (drift_sim) {
                    CHECK(test_dspp_find_alloc(drift_sim, "InFIFO", 5u,
                                               &drift_fifo) == 0);
                    CHECK(test_dspp_find_alloc(drift_sim, "Drift", 4u,
                                               &drift_output) == 0);
                    CHECK(test_dspp_find_alloc(drift_sim, "Phase", 2u,
                                               &drift_phase) == 0);
                    CHECK(test_dspp_find_alloc(drift_sim, "Amplitude", 1u,
                                               &drift_amplitude) == 0);
                    CHECK(test_dspp_find_alloc(drift_sim, "SampleRate", 1u,
                                               &drift_rate) == 0);
                    CHECK(drift_fifo != UINT32_MAX &&
                          drift_output < DSPP_DATA_MEMORY_WORDS &&
                          drift_phase < DSPP_DATA_MEMORY_WORDS &&
                          drift_amplitude < DSPP_DATA_MEMORY_WORDS &&
                          drift_rate < DSPP_DATA_MEMORY_WORDS);
                    if (drift_fifo != UINT32_MAX &&
                        drift_output < DSPP_DATA_MEMORY_WORDS &&
                        drift_phase < DSPP_DATA_MEMORY_WORDS &&
                        drift_amplitude < DSPP_DATA_MEMORY_WORDS &&
                        drift_rate < DSPP_DATA_MEMORY_WORDS) {
                        dspp_sim_poke_data(drift_sim, drift_amplitude,
                                           0x7FFFu);
                        dspp_sim_poke_data(drift_sim, drift_rate, 0x7FFFu);
                        dspp_sim_poke_data(drift_sim, drift_phase, 0u);
                        dspp_sim_fifo_push(drift_sim, drift_fifo, 0x1234);
                        dspp_sim_tick(drift_sim);
                        // At 0x7FFF the first oscillator pass consumes zero
                        // samples and leaves phase 0x7FFF; Drift therefore
                        // reports (#samples - frames) == -1, as the source
                        // `_--` wrapper specifies.
                        CHECK(dspp_sim_peek_data(drift_sim, drift_output) ==
                              0xFFFFu);
                    }
                }
                dspp_sim_destroy(drift_sim);
                dspp_sim_space_destroy(drift_space);
            }

            // The vendor nanokernel binds fixed hardware-resource types to
            // DSPI registers rather than data-memory allocations. Verify the
            // original on-disc template receives those addresses in a shared
            // loader space; this is the prerequisite for its status/control
            // loop to execute against the DSPI model.
            FILE *fn = fopen(nk_path, "rb");
            CHECK(fn != NULL);
            if (fn) {
                static uint8_t nk_bytes[65536];
                uint32_t nk_n =
                    (uint32_t)fread(nk_bytes, 1, sizeof(nk_bytes), fn);
                fclose(fn);
                dspp_template nk;
                CHECK(dspp_load_template(nk_bytes, nk_n, &nk) == 0);
                dspp_sim_space *nk_space = dspp_sim_space_create();
                dspp_sim *nk_sim =
                    dspp_sim_create_in_space(&nk, nk_space);
                CHECK(nk_space != NULL && nk_sim != NULL);
                if (nk_sim) {
                    uint32_t input_status = 0xFFFFFFFFu;
                    uint32_t input_control = 0xFFFFFFFFu;
                    uint32_t output_status = 0xFFFFFFFFu;
                    uint32_t output_control = 0xFFFFFFFFu;
                    uint32_t output_to_dac = 0xFFFFFFFFu;
                    uint32_t down_counter = 0xFFFFFFFFu;
                    const dspp_template *nk_res =
                        dspp_sim_resources(nk_sim);
                    for (uint32_t r = 0; r < nk_res->resource_count; ++r) {
                        const dspp_resource *rr = &nk_res->resources[r];
                        if (strcmp(rr->name, "InputStatus") == 0)
                            input_status = rr->alloc_value;
                        else if (strcmp(rr->name, "InputControl") == 0)
                            input_control = rr->alloc_value;
                        else if (strcmp(rr->name, "OutputStatus") == 0)
                            output_status = rr->alloc_value;
                        else if (strcmp(rr->name, "OutputControl") == 0)
                            output_control = rr->alloc_value;
                        else if (strcmp(rr->name, "OutputToDAC") == 0)
                            output_to_dac = rr->alloc_value;
                        else if (strcmp(rr->name, "DownCounter") == 0)
                            down_counter = rr->alloc_value;
                    }
                    CHECK(input_status == 0x3DEu);
                    CHECK(input_control == 0x3D6u);
                    CHECK(output_status == 0x3DFu);
                    CHECK(output_control == 0x3D7u);
                    CHECK(output_to_dac == 0x2E0u);
                    CHECK(down_counter == 0x3F7u);
                }
                dspp_sim_destroy(nk_sim);
                dspp_sim_space_destroy(nk_space);
            }

            // Runtime knob feed: CreateKnob InputA/InputB on the started
            // add.dsp instrument, SetKnobPart 0.5 each. dsppSetKnob converts
            // 0.5 -> raw 16384 (ConvertFP_SF15, signed clip); Out = A+B ticks
            // 32768 into the output word, pushed as S16 stereo (-32768).
            write_guest_string(0x6a00u, "InputA");
            write_guest_string(0x6a20u, "InputB");
            write_guest_string(0x6a40u, "Output");
            m2_kernel_dispatch_set_global(hdl);
            const uint32_t dspp_sp = g_cpu.gpr[1];
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0x6a00u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_a = g_cpu.gpr[3];
            CHECK(knob_a > 0);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0x6a20u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_b = g_cpu.gpr[3];
            CHECK(knob_b > 0);
            CHECK(g_cpu.gpr[1] == dspp_sp);
            m2_kernel_dispatch_set_global(NULL);
            g_cpu.gpr[3] = knob_a;
            g_cpu.gpr[4] = 0;
            g_cpu.fpr[1] = 0.25f;
            CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            g_cpu.gpr[3] = knob_b;
            g_cpu.gpr[4] = 0;
            g_cpu.fpr[1] = 0.25f;
            CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x61048u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            // Mixer render tracks the published frame counter: pumps push
            // exactly the deficit since the previous pump. Earlier blocks in
            // this binary already advanced the counter, so assert deltas and
            // re-arm the first-sample capture for this block's content.
            // Knob 0.25 -> raw 8192 each (ConvertFP_SF15, signed clip);
            // Out = A+B ticks 16384 (linear range; large sums saturate in
            // the authentic barrel shifter, unpinned here).
            // Arm the device producer before its first callback, even when
            // the rendered clock is zero. Its first block is real demand,
            // not a new boot baseline that can swallow the playback reserve.
            g_audio_test_frames = 0u;
            m2_guest_audio_set_async_dspp(1);
            const uint32_t startup_base = g_dspp_push_frames;
            g_audio_test_frames = 128u;
            CHECK(m2_guest_audio_pump_dspp_output() == 128u);
            CHECK(g_dspp_push_frames - startup_base == 128u);
            CHECK(m2_guest_audio_pump_dspp_output() == 0u);
            m2_guest_audio_set_async_dspp(0);
            g_dspp_push_calls = 0;
            g_audio_test_frames = 1000u;
            m2_guest_audio_pump_dspp_output();
            uint32_t dspp_base = g_dspp_push_frames;
            g_audio_test_frames = 1064u;
            m2_guest_audio_pump_dspp_output();
            CHECK(g_dspp_push_frames - dspp_base == 64u);
            CHECK(g_dspp_first_l == (int16_t)0x4000u);
            CHECK(g_dspp_first_r == (int16_t)0x4000u);
            // Interactive ownership split: with async DSPP service armed,
            // the platform timer pump must not duplicate the sample deficit;
            // the explicit worker entry consumes it exactly once.
            const uint32_t async_base = g_dspp_push_frames;
            m2_guest_audio_set_async_dspp(1);
            CHECK(m2_guest_audio_async_dspp_enabled() == 1);
            g_audio_test_frames = 1128u;
            m2_guest_audio_pump_timers();
            CHECK(g_dspp_push_frames == async_base);
            m2_guest_audio_pump_dspp_output();
            CHECK(g_dspp_push_frames - async_base == 64u);
            m2_guest_audio_set_async_dspp(0);
            CHECK(m2_guest_audio_async_dspp_enabled() == 0);
            // Setter-priority yield: while a guest setter is announced (its
            // real guard clears the flag only once it owns the lock, which a
            // single-threaded test cannot reach), the asynchronous worker must
            // still simulate the exact deficit — one sample frame per lock at
            // the longest — and must not drop or reorder output. The flag also
            // exercises a continuously pending waiter. Count sleep requests,
            // independently of the OS scheduler's elapsed-time variation.
            m2_guest_audio_set_async_dspp(1);
            const uint32_t yield_base = g_dspp_push_frames;
            const uint64_t sleeps_base = m2_guest_audio_dspp_backoff_sleeps();
            g_audio_test_frames = 1192u;
            m2_guest_audio_setter_enter();
            m2_guest_audio_pump_dspp_output();
            m2_guest_audio_setter_leave();
            CHECK(g_dspp_push_frames - yield_base == 64u);
            CHECK(m2_guest_audio_dspp_backoff_sleeps() - sleeps_base == 10u);
            // A device callback publishes its whole block. Keep servicing a
            // multi-batch deficit without idling between 128-frame batches;
            // stop only when every original 44.1kHz frame is accounted for.
            g_audio_test_frames += 384u;
            for (uint32_t batch = 0; batch < 3u; ++batch) {
                CHECK(m2_guest_audio_pump_dspp_output() == 128u);
                CHECK(g_dspp_push_frames - yield_base == 64u + (batch + 1u) * 128u);
            }
            CHECK(m2_guest_audio_pump_dspp_output() == 0u);
            CHECK(g_dspp_push_frames - yield_base == 448u);
            CHECK(m2_guest_audio_dspp_backoff_sleeps() - sleeps_base == 10u);
            // A real blocked setter must also progress while the worker
            // drains a multi-batch backlog. Both use the production graph
            // lock; constant source-derived knob values preserve add.dsp's
            // independently established output, rather than tuning gameplay.
            audio_setter_fixture setter = {.cpu = g_cpu, .knob = knob_a};
            pthread_t setter_thread;
            m2_guest_audio_graph_lock();
            const int created = pthread_create(&setter_thread, NULL,
                                                audio_concurrent_setter, &setter);
            CHECK(created == 0);
            if (created == 0) {
                for (uint32_t spin = 0; spin < 1000000u &&
                     !m2_guest_audio_setter_pending(); ++spin)
                    sched_yield();
                CHECK(m2_guest_audio_setter_pending());
            }
            m2_guest_audio_graph_unlock();
            const uint32_t concurrent_base = g_dspp_push_frames;
            g_dspp_check_constant = 1;
            g_dspp_bad_frames = 0;
            g_audio_test_frames += 4096u;
            for (uint32_t batch = 0; batch < 32u; ++batch)
                CHECK(m2_guest_audio_pump_dspp_output() == 128u);
            if (created == 0) {
                CHECK(pthread_join(setter_thread, NULL) == 0);
                CHECK(setter.completed == 1024u);
                CHECK(setter.failures == 0u);
            }
            CHECK(g_dspp_push_frames - concurrent_base == 4096u);
            CHECK(g_dspp_bad_frames == 0u);
            g_dspp_check_constant = 0;
            CHECK(m2_guest_audio_pump_dspp_output() == 0u);
            // The worker fills to the host's request, not the device counter:
            // frames the device already played as silence are never produced,
            // so a stall cannot leave them queued as added latency.
            const uint32_t gap_base = g_dspp_push_frames;
            g_dspp_fill_forced = 0;
            g_audio_test_frames += 1000u;
            CHECK(m2_guest_audio_pump_dspp_output() == 0u);
            CHECK(g_dspp_push_frames == gap_base);
            g_dspp_fill_forced = 300;
            CHECK(m2_guest_audio_pump_dspp_output() == 128u);
            CHECK(g_dspp_push_frames - gap_base == 128u);
            g_dspp_fill_forced = 44;
            CHECK(m2_guest_audio_pump_dspp_output() == 44u);
            CHECK(g_dspp_push_frames - gap_base == 172u);
            g_dspp_fill_forced = 0;
            (void)m2_host_audio_dspp_fill_request();
            g_dspp_fill_forced = -1;
            CHECK(m2_guest_audio_pump_dspp_output() == 0u);
            m2_guest_audio_set_async_dspp(0);
            // Runtime connect plumbing on live sims: self-connect
            // Output -> InputA applies, duplicates report IN_USE, disconnect
            // rebuilds the image so a reconnect applies again.
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0,
                      (uint32_t)add_ins, 0x6a00u, 0) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0,
                      (uint32_t)add_ins, 0x6a00u, 0) == M2_AUDIO_ERR_IN_USE);
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a00u, 0) == M2K_OK);
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a00u, 0) ==
                  M2_AUDIO_ERR_BAD_PTR);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0,
                      (uint32_t)add_ins, 0x6a00u, 0) == M2K_OK);
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a00u, 0) == M2K_OK);

            // M2 DSPP data memory is global, not one private array per
            // instrument. Connect the first on-disc add.dsp's OUTPUT to a
            // second add.dsp's InputA and verify that the destination reads
            // the source's allocated output word. Before the shared-space
            // repair both templates started their allocator at zero, so the
            // patched cross-instrument operand read the destination's own
            // InputA word instead of the source output.
            int32_t add2_ins = m2_guest_audio_load_instrument(
                &g_cpu, 0x6000u, 1, 100);
            CHECK(add2_ins > 0);
            // Lower the source and destination values so the aggregate DAC
            // tap remains below S16 saturation and distinguishes shared
            // (0.5 + 0.25) from private (0.25 + 0.25) data memory.
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, knob_a, 0, 0.1f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, knob_b, 0, 0.1f) ==
                  M2K_OK);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)add2_ins;
            g_cpu.gpr[4] = 0x6a00u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t add2_a = g_cpu.gpr[3];
            g_cpu.gpr[3] = (uint32_t)add2_ins;
            g_cpu.gpr[4] = 0x6a20u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t add2_b = g_cpu.gpr[3];
            g_cpu.gpr[3] = (uint32_t)add2_ins;
            g_cpu.gpr[4] = 0x6a20u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t add2_b_last = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(add2_a > 0 && add2_b > 0 && add2_b_last > 0);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, add2_a, 0, 0.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, add2_b, 0, 0.1f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, add2_b_last, 0,
                                               0.125f) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                  (uint32_t)add2_ins, 0) ==
                  M2K_OK);
            // The two SetKnobPart calls above happened before StartInstrument.
            // The recovered start boundary must carry those retained values
            // into the newly allocated DSP image; no post-start rewrite is
            // needed here.
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0,
                      (uint32_t)add2_ins, 0x6a00u, 0) == M2K_OK);
            // Source add.dsp = 0.1 + 0.1 = 6552; the last of two destination
            // InputB knobs sets 0.125 = 4096. The destination reads the
            // source's shared word through InputA (a KNOB) and outputs
            // 10648. The source is a control-only modulation feed (its OUTPUT
            // drives a destination KNOB), so it is not added to the DAC
            // fallback a second time. With private simulator data the
            // destination would read its own InputA zero and contribute only
            // 4096; without the start-boundary replay, InputB would remain at
            // its initialized zero and the destination would output 6552.
            g_dspp_push_calls = 0;
            g_audio_test_frames = 1u;
            m2_guest_audio_pump_dspp_output();
            g_audio_test_frames = 2u;
            m2_guest_audio_pump_dspp_output();
            CHECK(g_dspp_first_l == 10648 && g_dspp_first_r == 10648);
            // Control-vs-audio OUTPUT classification: a started sim whose
            // OUTPUT only feeds a destination KNOB is a modulation source
            // (envelope.dsp -> mixer Gain is the FMV case) and must not be
            // added to the DAC fallback sum; the same source connected to an
            // INPUT keeps the direct-to-DAC fallback. Disconnect the shared-
            // word route first so add_ins is control-only, then swap ports.
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, (uint32_t)add2_ins, 0x6a00u, 0) == M2K_OK);
            write_guest_string(0x6a80u, "Gain");
            write_guest_string(0x6aa0u, "Input");
            int32_t ctl_mt = m2_guest_audio_create_mixer_template(
                &g_cpu, (2u << 24) | (2u << 16) | 2u, 0);
            CHECK(ctl_mt > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)ctl_mt;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t ctl_ins = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(ctl_ins != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0, ctl_ins, 0x6a80u,
                      0) == M2K_OK);
            g_dspp_push_calls = 0;
            g_audio_test_frames = 1u;
            m2_guest_audio_pump_dspp_output();
            g_audio_test_frames = 2u;
            m2_guest_audio_pump_dspp_output();
            // add_ins (6552) is control-only and excluded; add2_ins reads its
            // own InputB knob (0.125 -> 4096) after the disconnect.
            CHECK(g_dspp_first_l == 4096 && g_dspp_first_r == 4096);
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, ctl_ins, 0x6a80u, 0) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x6a40u, 0, ctl_ins, 0x6aa0u,
                      0) == M2K_OK);
            g_dspp_push_calls = 0;
            g_audio_test_frames = 1u;
            m2_guest_audio_pump_dspp_output();
            g_audio_test_frames = 2u;
            m2_guest_audio_pump_dspp_output();
            // INPUT destination: the fallback sums 6552 + 4096 again.
            CHECK(g_dspp_first_l == 10648 && g_dspp_first_r == 10648);
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, ctl_ins, 0x6aa0u, 0) == M2K_OK);
            hdl->DeleteItem(ctl_ins);
            hdl->DeleteItem((uint32_t)ctl_mt);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)add2_ins) ==
                  M2K_OK);

            (void)m2_guest_audio_unload_instrument((uint32_t)add_ins);
            (void)m2_guest_audio_unload_instrument((uint32_t)lo_ins);
            (void)dsp_file_loaded;
        } else {
            printf("m2_audio_items_test: disc DSP templates not found; "
                   "DSPP math regression skipped\n");
        }
    }
    // ---- BIND+RBASE MOVE connection (dsppConnectWithMove) ------------------
    // Synthetic envelope-shaped template: an OUTPUT word plus a KNOB bound
    // at offset 2 into an 8-word RBASE block (envelope.dsp's Amplitude
    // shape). Vendor dsppConnectInstrumentParts routes such destinations to
    // a per-frame MOVE (vendor folios/audio/dspp_instr.c) instead of
    // patching destination code; the host records the copy and applies it
    // at each tick entry, and disconnect drops the copy while leaving data
    // undisturbed. Loader-only and disc-independent, so it runs even when
    // the extracted templates are absent.
    {
        dspp_template mvt;
        memset(&mvt, 0, sizeof(mvt));
        snprintf(mvt.name, sizeof(mvt.name), "move_test");
        mvt.resource_count = 3;
        snprintf(mvt.resources[0].name, DSPP_MAX_NAME, "DataRBase");
        mvt.resources[0].type = 8u;     // DRSC_TYPE_RBASE
        mvt.resources[0].flags = 0x10u; // DRSC_F_BOUND_TO
        mvt.resources[0].many = 8;
        snprintf(mvt.resources[1].name, DSPP_MAX_NAME, "Output");
        mvt.resources[1].type = 4u; // DRSC_TYPE_OUTPUT
        mvt.resources[1].many = 1;
        snprintf(mvt.resources[2].name, DSPP_MAX_NAME, "Amp");
        mvt.resources[2].type = 1u;     // DRSC_TYPE_KNOB
        mvt.resources[2].flags = 0x20u; // DRSC_F_BIND
        mvt.resources[2].many = 1;
        mvt.resources[2].allocated = 2; // bind offset into DataRBase
        mvt.resources[2].bind_index = 0;
        mvt.code_word_count = 1;
        mvt.code_words[0] = 0x8380u; // SLEEP: one-step cyclic pass
        dspp_sim *msim = dspp_sim_create(&mvt);
        CHECK(msim != NULL);
        const dspp_template *mvres = dspp_sim_resources(msim);
        CHECK(mvres != NULL);
        uint32_t move_out = 0xFFFFFFFFu, move_amp = 0xFFFFFFFFu;
        for (uint32_t r = 0; r < mvres->resource_count; ++r) {
            if (strcmp(mvres->resources[r].name, "Output") == 0)
                move_out = mvres->resources[r].alloc_value;
            else if (strcmp(mvres->resources[r].name, "Amp") == 0)
                move_amp = mvres->resources[r].alloc_value;
        }
        // Non-vacuous copy: the bound word must differ from the source word
        // (RBASE base + bind offset, not the data allocator's next word).
        CHECK(move_out != 0xFFFFFFFFu && move_amp != 0xFFFFFFFFu);
        CHECK(move_out != move_amp);
        dspp_sim_poke_data(msim, move_out, 1234);
        CHECK(dspp_sim_peek_data(msim, move_amp) == 0u);
        // BIND destination: connect records the MOVE and patches no code.
        dspp_sim_connect(msim, "Amp", 0, (uint16_t)(0x8000u | move_out));
        dspp_sim_tick(msim);
        CHECK(dspp_sim_peek_data(msim, move_amp) == 1234u);
        // Per-frame copy: a new source value arrives on the next tick, so
        // this is not a one-shot connect-time write.
        dspp_sim_poke_data(msim, move_out, 5678);
        dspp_sim_tick(msim);
        CHECK(dspp_sim_peek_data(msim, move_amp) == 5678u);
        // Disconnect drops the copy; the last MOVEd value stays put.
        dspp_sim_disconnect(msim, "Amp", 0);
        dspp_sim_poke_data(msim, move_out, 9999);
        dspp_sim_tick(msim);
        CHECK(dspp_sim_peek_data(msim, move_amp) == 5678u);
        dspp_sim_destroy(msim);
    }
    teardown();

    setup();
    // ---- R1 mix nodes: patch/mixer-template instruments start (2026-09-08)
    // Mixer lifecycle (disc-independent): a 2x1 mixer instrument starts as
    // a gain-routing node (pre-fix: UNIMPLEMENTED), stops, and restarts.
    // With gains unset the vendor default (Gain 0) mixes silence, probed
    // through the guest probe API.
    {
        g_cpu.gpr[3] = (2u << 24) | (1u << 16);
        g_cpu.gpr[4] = 0;
        CHECK(m2_audio_dispatch_c(&g_cpu, 8, 0x291f4u) == 1);
        uint32_t mx = g_cpu.gpr[3];
        CHECK((int32_t)mx > 0);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = mx;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrumentVA__7376c(&g_cpu);
        uint32_t mx_ins = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(mx_ins != 0 && mx_ins != mx);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, mx_ins, 0) == M2K_OK);
        CHECK(m2_guest_audio_stop_instrument(&g_cpu, mx_ins, 0) == M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, mx_ins, 0) == M2K_OK);
        write_guest_string(0x9000u, "Output");
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = mx_ins;
        g_cpu.gpr[4] = 0x9000u;
        g_cpu.gpr[5] = 0;
        FN_CreateProbe__7392c(&g_cpu);
        uint32_t mx_probe = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(mx_probe != M2K_ITEM_NONE);
        m2_guest_audio_tick_dspp_frames(1u);
        CHECK(m2_guest_audio_read_probe_part(&g_cpu, mx_probe, 0, 0x9100u) ==
              M2K_OK);
        CHECK(rdf32(&g_cpu, 0x9100u) == 0.0f);
        // The 0x4E/0x4F-only start-tag rule stays closed: an unknown tag
        // still rejects with BAD_TAG on a valid node instrument.
        wr32(&g_cpu, 0x9200u, 0x1234u);
        wr32(&g_cpu, 0x9204u, 0u);
        wr32(&g_cpu, 0x9208u, 0u /* TAG_END */);
        wr32(&g_cpu, 0x920Cu, 0u);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, mx_ins, 0x9200u) ==
              M2_AUDIO_ERR_BAD_TAG);
        hdl->DeleteItem(mx_ins);
        hdl->DeleteItem(mx);
    }
    teardown();

    setup();
    // Patch lifecycle (disc-independent): a ports-only patch instrument
    // starts (pre-fix: UNIMPLEMENTED); the shell drives no output, so its
    // Output probe reads 0 through the same guest API.
    {
        write_guest_string(0x9300u, "Input");
        write_guest_string(0x9320u, "Output");
        const uint32_t pc_ea = 0x9400u;
        wr32(&g_cpu, pc_ea + 0, 257u); // DEFINE_PORT Input 1 INPUT
        wr32(&g_cpu, pc_ea + 4, 0x9300u);
        wr32(&g_cpu, pc_ea + 8, 1u);
        wr32(&g_cpu, pc_ea + 12, 0u);
        wr32(&g_cpu, pc_ea + 16, 1u);
        wr32(&g_cpu, pc_ea + 28, 257u); // DEFINE_PORT Output 1 OUTPUT
        wr32(&g_cpu, pc_ea + 32, 0x9320u);
        wr32(&g_cpu, pc_ea + 36, 1u);
        wr32(&g_cpu, pc_ea + 40, 1u);
        wr32(&g_cpu, pc_ea + 44, 1u);
        wr32(&g_cpu, pc_ea + 56, 0u); // END
        g_cpu.gpr[3] = pc_ea;
        g_cpu.gpr[4] = 0;
        recomp_call_selector(&g_cpu, 5, 0, 0x6d9a4u);
        uint32_t pt = g_cpu.gpr[3];
        CHECK((int32_t)pt > 0);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = pt;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrument__73734(&g_cpu);
        uint32_t pt_ins = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(pt_ins != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, pt_ins, 0) == M2K_OK);
        write_guest_string(0x9500u, "Output");
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = pt_ins;
        g_cpu.gpr[4] = 0x9500u;
        g_cpu.gpr[5] = 0;
        FN_CreateProbe__7392c(&g_cpu);
        uint32_t pt_probe = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(pt_probe != M2K_ITEM_NONE);
        m2_guest_audio_tick_dspp_frames(1u);
        CHECK(m2_guest_audio_read_probe_part(&g_cpu, pt_probe, 0, 0x9600u) ==
              M2K_OK);
        CHECK(rdf32(&g_cpu, 0x9600u) == 0.0f);
        hdl->DeleteItem(pt_ins);
        hdl->DeleteItem(pt);
    }
    teardown();

    setup();
    // Node graph (disc-independent): mixer->mixer routes resolve through
    // the connection table in either order, and a self-connected node
    // terminates (cycle guard) mixing silence.
    {
        write_guest_string(0x9700u, "Input");
        write_guest_string(0x9720u, "Output");
        write_guest_string(0x9740u, "Gain");
        int32_t tplA = m2_guest_audio_create_mixer_template(
            &g_cpu, (1u << 24) | (1u << 16), 0);
        int32_t tplB = m2_guest_audio_create_mixer_template(
            &g_cpu, (1u << 24) | (1u << 16), 0);
        int32_t tplC = m2_guest_audio_create_mixer_template(
            &g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(tplA > 0 && tplB > 0 && tplC > 0);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)tplA;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrument__73734(&g_cpu);
        uint32_t insA = g_cpu.gpr[3];
        g_cpu.gpr[3] = (uint32_t)tplB;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrument__73734(&g_cpu);
        uint32_t insB = g_cpu.gpr[3];
        g_cpu.gpr[3] = (uint32_t)tplC;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrument__73734(&g_cpu);
        uint32_t insC = g_cpu.gpr[3];
        g_cpu.gpr[3] = insA;
        g_cpu.gpr[4] = 0x9740u;
        g_cpu.gpr[5] = 0;
        FN_CreateKnob__73778(&g_cpu);
        uint32_t gainA = g_cpu.gpr[3];
        g_cpu.gpr[3] = insB;
        g_cpu.gpr[4] = 0x9720u;
        g_cpu.gpr[5] = 0;
        FN_CreateProbe__7392c(&g_cpu);
        uint32_t probeB = g_cpu.gpr[3];
        g_cpu.gpr[3] = insC;
        g_cpu.gpr[4] = 0x9720u;
        g_cpu.gpr[5] = 0;
        FN_CreateProbe__7392c(&g_cpu);
        uint32_t probeC = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(insA != M2K_ITEM_NONE && insB != M2K_ITEM_NONE);
        CHECK(insC != M2K_ITEM_NONE && gainA != M2K_ITEM_NONE);
        CHECK(probeB != M2K_ITEM_NONE && probeC != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_set_knob_part(&g_cpu, gainA, 0, 1.0f) == M2K_OK);
        // Start-then-connect order on A->B; self-connect on C.
        CHECK(m2_guest_audio_start_instrument(&g_cpu, insA, 0) == M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, insB, 0) == M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, insC, 0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, insA, 0x9720u, 0, insB, 0x9700u, 0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, insC, 0x9720u, 0, insC, 0x9700u, 0) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        CHECK(m2_guest_audio_read_probe_part(&g_cpu, probeB, 0, 0x9760u) ==
              M2K_OK);
        CHECK(rdf32(&g_cpu, 0x9760u) == 0.0f);
        CHECK(m2_guest_audio_read_probe_part(&g_cpu, probeC, 0, 0x9780u) ==
              M2K_OK);
        CHECK(rdf32(&g_cpu, 0x9780u) == 0.0f);
        hdl->DeleteItem(insA);
        hdl->DeleteItem(insB);
        hdl->DeleteItem(insC);
        hdl->DeleteItem((uint32_t)tplA);
        hdl->DeleteItem((uint32_t)tplB);
        hdl->DeleteItem((uint32_t)tplC);
    }
    teardown();

    setup();
    // Mixer math + resident mix (needs the extracted disc templates for an
    // authentic signal source: add.dsp sums two knob-driven words).
    {
        if (test_dsp_raw_open("System.m2/Audio/dsp/add.dsp") != 0) {
            write_guest_string(0x6000u, "add.dsp");
            int32_t add_ins = m2_guest_audio_load_instrument(&g_cpu, 0x6000u,
                                                             1, 100);
            CHECK(add_ins > 0);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0;
            CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            write_guest_string(0x9800u, "InputA");
            write_guest_string(0x9820u, "InputB");
            write_guest_string(0x9840u, "Output");
            write_guest_string(0x9860u, "Input");
            write_guest_string(0x9880u, "Gain");
            write_guest_string(0x98A0u, "Amplitude");
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0x9800u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knobA = g_cpu.gpr[3];
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = 0x9820u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knobB = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(knobA != M2K_ITEM_NONE && knobB != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, knobA, 0,
                                               1000.0f / 32768.0f) == M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, knobB, 0,
                                               2000.0f / 32768.0f) == M2K_OK);
            // 2x1 mixer, connect-before-start order: in0 <- add (gain
            // 1.0 at CalcMixerGainPart(spec,0,0)=0), in1 unconnected
            // (gain 0.5, stages nothing).
            int32_t mtl = m2_guest_audio_create_mixer_template(
                &g_cpu, (2u << 24) | (1u << 16), 0);
            CHECK(mtl > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)mtl;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t m_ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = m_ins;
            g_cpu.gpr[4] = 0x9880u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t mgain = g_cpu.gpr[3];
            g_cpu.gpr[3] = m_ins;
            g_cpu.gpr[4] = 0x9840u;
            g_cpu.gpr[5] = 0;
            FN_CreateProbe__7392c(&g_cpu);
            uint32_t mprobe = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(m_ins != M2K_ITEM_NONE && mgain != M2K_ITEM_NONE);
            CHECK(mprobe != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, mgain, 0, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, mgain, 1, 0.5f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x9840u, 0, m_ins, 0x9860u,
                      0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, m_ins, 0) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            // add outputs 1000+2000=3000; mixer scales by SF15(1.0)=32767:
            // (3000*32767)>>15 = 2999, probed as 2999/32768 (exact).
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, mprobe, 0,
                                                 0x9900u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x9900u) == 2999.0f / 32768.0f);
            // Node-to-node wire: a 1x2 mixer feeds a 1x1 mixer, so the downstream
            // input resolves a named output part of another node. Rewiring the
            // same input to the other part must read the new part.
            {
                int32_t split_tl = m2_guest_audio_create_mixer_template(
                    &g_cpu, (1u << 24) | (2u << 16), 0);
                int32_t sink_tl = m2_guest_audio_create_mixer_template(
                    &g_cpu, (1u << 24) | (1u << 16), 0);
                CHECK(split_tl > 0 && sink_tl > 0);
                m2_kernel_dispatch_set_global(hdl);
                g_cpu.gpr[3] = (uint32_t)split_tl;
                g_cpu.gpr[4] = 0;
                FN_CreateInstrument__73734(&g_cpu);
                const uint32_t split_ins = g_cpu.gpr[3];
                g_cpu.gpr[3] = (uint32_t)sink_tl;
                g_cpu.gpr[4] = 0;
                FN_CreateInstrument__73734(&g_cpu);
                const uint32_t sink_ins = g_cpu.gpr[3];
                g_cpu.gpr[3] = split_ins;
                g_cpu.gpr[4] = 0x9880u;
                g_cpu.gpr[5] = 0;
                FN_CreateKnob__73778(&g_cpu);
                const uint32_t split_gain = g_cpu.gpr[3];
                g_cpu.gpr[3] = sink_ins;
                g_cpu.gpr[4] = 0x9880u;
                g_cpu.gpr[5] = 0;
                FN_CreateKnob__73778(&g_cpu);
                const uint32_t sink_gain = g_cpu.gpr[3];
                g_cpu.gpr[3] = sink_ins;
                g_cpu.gpr[4] = 0x9840u;
                g_cpu.gpr[5] = 0;
                FN_CreateProbe__7392c(&g_cpu);
                const uint32_t sink_probe = g_cpu.gpr[3];
                m2_kernel_dispatch_set_global(NULL);
                CHECK(split_ins != M2K_ITEM_NONE && sink_ins != M2K_ITEM_NONE);
                CHECK(split_gain != M2K_ITEM_NONE && sink_gain != M2K_ITEM_NONE);
                CHECK(sink_probe != M2K_ITEM_NONE);
                // CalcMixerGainPart: Output[0] part 0 at 1.0, Output[1] part 1 at 0.5.
                CHECK(m2_guest_audio_set_knob_part(&g_cpu, split_gain, 0, 1.0f) == M2K_OK);
                CHECK(m2_guest_audio_set_knob_part(&g_cpu, split_gain, 1, 0.5f) == M2K_OK);
                CHECK(m2_guest_audio_set_knob_part(&g_cpu, sink_gain, 0, 1.0f) == M2K_OK);
                CHECK(m2_guest_audio_connect_instrument_parts(
                          &g_cpu, (uint32_t)add_ins, 0x9840u, 0, split_ins, 0x9860u, 0) == M2K_OK);
                CHECK(m2_guest_audio_connect_instrument_parts(
                          &g_cpu, split_ins, 0x9840u, 0, sink_ins, 0x9860u, 0) == M2K_OK);
                CHECK(m2_guest_audio_start_instrument(&g_cpu, split_ins, 0) == M2K_OK);
                CHECK(m2_guest_audio_start_instrument(&g_cpu, sink_ins, 0) == M2K_OK);
                m2_guest_audio_tick_dspp_frames(1u);
                // add 3000: Output[0] (3000*32767)>>15 = 2999, then (2999*32767)>>15 = 2998.
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, sink_probe, 0, 0x9910u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0x9910u) == 2998.0f / 32768.0f);
                CHECK(m2_guest_audio_disconnect_instrument_parts(
                          &g_cpu, sink_ins, 0x9860u, 0) == M2K_OK);
                m2_guest_audio_tick_dspp_frames(1u);
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, sink_probe, 0, 0x9910u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0x9910u) == 0.0f);
                CHECK(m2_guest_audio_connect_instrument_parts(
                          &g_cpu, split_ins, 0x9840u, 1, sink_ins, 0x9860u, 0) == M2K_OK);
                m2_guest_audio_tick_dspp_frames(1u);
                // Output[1]: (3000*16384)>>15 = 1500, then (1500*32767)>>15 = 1499.
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, sink_probe, 0, 0x9910u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0x9910u) == 1499.0f / 32768.0f);
                hdl->DeleteItem(sink_ins);
                hdl->DeleteItem(split_ins);
            }
            // WITH_AMPLITUDE mixer, start-then-connect order: the default
            // Amplitude 1.0 passes the mix through unchanged.
            int32_t atl = m2_guest_audio_create_mixer_template(
                &g_cpu, (1u << 24) | (1u << 16) | 1u, 0);
            CHECK(atl > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)atl;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t a_ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = a_ins;
            g_cpu.gpr[4] = 0x9880u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t again = g_cpu.gpr[3];
            g_cpu.gpr[3] = a_ins;
            g_cpu.gpr[4] = 0x98A0u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t aamp = g_cpu.gpr[3];
            g_cpu.gpr[3] = a_ins;
            g_cpu.gpr[4] = 0x9840u;
            g_cpu.gpr[5] = 0;
            FN_CreateProbe__7392c(&g_cpu);
            uint32_t aprobe = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(a_ins != M2K_ITEM_NONE && again != M2K_ITEM_NONE);
            CHECK(aamp != M2K_ITEM_NONE && aprobe != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, again, 0, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, a_ins, 0) ==
                  M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x9840u, 0, a_ins, 0x9860u,
                      0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            // The default Amplitude 1.0 is SF15 0x7FFF, so the hardware
            // TIMES scales (2999*32767)>>15 = 2998, probed exact.
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, aprobe, 0,
                                                 0x9920u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x9920u) == 2998.0f / 32768.0f);
            // Staging the Amplitude to 0.5 halves the mix:
            // (2999*16384)>>15 = 1499.
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, aamp, 0, 0.5f) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, aprobe, 0,
                                                 0x9940u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x9940u) == 1499.0f / 32768.0f);
            // WITH_LINE_OUT mixer (R2 direction): outputs accumulate into
            // the resident stereo mix — (3000*32767)>>15 = 2999 left,
            // (3000*16384)>>15 = 1500 right.
            int32_t ltl = m2_guest_audio_create_mixer_template(
                &g_cpu, (1u << 24) | (2u << 16) | 2u, 0);
            CHECK(ltl > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)ltl;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t l_ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = l_ins;
            g_cpu.gpr[4] = 0x9880u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t lgain = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(l_ins != M2K_ITEM_NONE && lgain != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, lgain, 0, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, lgain, 1, 0.5f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x9840u, 0, l_ins, 0x9860u,
                      0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, l_ins, 0) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            {
                int32_t mix[2] = {0, 0};
                m2_guest_audio_read_resident_mix(mix);
                CHECK(mix[0] == 2999 && mix[1] == 1500);
            }
            // Patch route-through (game-authored shape): a ports-only patch
            // with NULL-block internal connects Input[0]->Output[0..1]
            // copies the fed input word to both outputs with no gain —
            // straight wires per the vendor ParseCmdConnect rule.
            const uint32_t rpc_ea = 0x9A00u;
            wr32(&g_cpu, rpc_ea + 0, 257u); // DEFINE_PORT Input 1 INPUT
            wr32(&g_cpu, rpc_ea + 4, 0x9860u);
            wr32(&g_cpu, rpc_ea + 8, 1u);
            wr32(&g_cpu, rpc_ea + 12, 0u);
            wr32(&g_cpu, rpc_ea + 16, 1u);
            wr32(&g_cpu, rpc_ea + 28, 257u); // DEFINE_PORT Output 2 OUTPUT
            wr32(&g_cpu, rpc_ea + 32, 0x9840u);
            wr32(&g_cpu, rpc_ea + 36, 2u);
            wr32(&g_cpu, rpc_ea + 40, 1u);
            wr32(&g_cpu, rpc_ea + 44, 1u);
            wr32(&g_cpu, rpc_ea + 56, 260u); // CONNECT Input[0]->Output[0]
            wr32(&g_cpu, rpc_ea + 60, 0u);
            wr32(&g_cpu, rpc_ea + 64, 0x9860u);
            wr32(&g_cpu, rpc_ea + 68, 0u);
            wr32(&g_cpu, rpc_ea + 72, 0u);
            wr32(&g_cpu, rpc_ea + 76, 0x9840u);
            wr32(&g_cpu, rpc_ea + 80, 0u);
            wr32(&g_cpu, rpc_ea + 84, 260u); // CONNECT Input[0]->Output[1]
            wr32(&g_cpu, rpc_ea + 88, 0u);
            wr32(&g_cpu, rpc_ea + 92, 0x9860u);
            wr32(&g_cpu, rpc_ea + 96, 0u);
            wr32(&g_cpu, rpc_ea + 100, 0u);
            wr32(&g_cpu, rpc_ea + 104, 0x9840u);
            wr32(&g_cpu, rpc_ea + 108, 1u);
            wr32(&g_cpu, rpc_ea + 112, 0u); // END
            g_cpu.gpr[3] = rpc_ea;
            g_cpu.gpr[4] = 0;
            recomp_call_selector(&g_cpu, 5, 0, 0x6d9a4u);
            uint32_t rpt = g_cpu.gpr[3];
            CHECK((int32_t)rpt > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = rpt;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t rp_ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = rp_ins;
            g_cpu.gpr[4] = 0x9840u;
            g_cpu.gpr[5] = 0;
            FN_CreateProbe__7392c(&g_cpu);
            uint32_t rp_probe = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(rp_ins != M2K_ITEM_NONE && rp_probe != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x9840u, 0, rp_ins, 0x9860u,
                      0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, rp_ins, 0) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, rp_probe, 0,
                                                 0x9960u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x9960u) == 3000.0f / 32768.0f);
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, rp_probe, 1,
                                                 0x9980u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x9980u) == 3000.0f / 32768.0f);

            // Recognized Sound3D patch: the source-defined filter_3d equation
            // executes for both ears. Distance gain and Doppler are applied
            // by the game's source sampler, outside this patch.
            write_guest_string(0x9B00u, "Drift");
            write_guest_string(0x9B20u, "Alpha");
            write_guest_string(0x9B40u, "Beta");
            write_guest_string(0x9B60u, "Feed");
            write_guest_string(0x9B80u, "Delay");
            write_guest_string(0x9BA0u, "DelayRate");
            const uint32_t s3pc = 0x9C00u;
#define S3_DEF_PORT(n, name_ea, parts, type) do {                         \
                wr32(&g_cpu, s3pc + (n) * 28u + 0u, 257u);               \
                wr32(&g_cpu, s3pc + (n) * 28u + 4u, (name_ea));          \
                wr32(&g_cpu, s3pc + (n) * 28u + 8u, (parts));            \
                wr32(&g_cpu, s3pc + (n) * 28u + 12u, (type));            \
                wr32(&g_cpu, s3pc + (n) * 28u + 16u, 1u);                \
            } while (0)
#define S3_DEF_KNOB(n, name_ea, parts, def) do {                          \
                wr32(&g_cpu, s3pc + (n) * 28u + 0u, 258u);               \
                wr32(&g_cpu, s3pc + (n) * 28u + 4u, (name_ea));          \
                wr32(&g_cpu, s3pc + (n) * 28u + 8u, (parts));            \
                wr32(&g_cpu, s3pc + (n) * 28u + 12u, 1u);                \
                wrf32(&g_cpu, s3pc + (n) * 28u + 16u, (def));            \
            } while (0)
            S3_DEF_PORT(0u, 0x9B00u, 2u, 1u); // Drift output
            S3_DEF_PORT(1u, 0x9860u, 1u, 0u); // Input
            S3_DEF_PORT(2u, 0x9840u, 2u, 1u); // Output
            S3_DEF_KNOB(3u, 0x9B20u, 2u, 0.0f); // Alpha
            S3_DEF_KNOB(4u, 0x9B40u, 2u, 0.0f); // Beta
            S3_DEF_KNOB(5u, 0x9B60u, 2u, 1.0f); // Feed
            S3_DEF_KNOB(6u, 0x9B80u, 2u, 0.0f); // Delay
            S3_DEF_KNOB(7u, 0x9BA0u, 1u, -1.0f); // DelayRate
#undef S3_DEF_KNOB
#undef S3_DEF_PORT
            wr32(&g_cpu, s3pc + 8u * 28u, 0u); // END
            g_cpu.gpr[3] = s3pc;
            g_cpu.gpr[4] = 0;
            recomp_call_selector(&g_cpu, 5, 0, 0x6d9a4u);
            uint32_t s3tpl = g_cpu.gpr[3];
            CHECK((int32_t)s3tpl > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = s3tpl;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t s3ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = s3ins;
            g_cpu.gpr[4] = 0x9B20u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t s3alpha = g_cpu.gpr[3];
            g_cpu.gpr[3] = s3ins;
            g_cpu.gpr[4] = 0x9B40u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t s3beta = g_cpu.gpr[3];
            g_cpu.gpr[3] = s3ins;
            g_cpu.gpr[4] = 0x9B60u;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t s3feed = g_cpu.gpr[3];
            g_cpu.gpr[3] = s3ins;
            g_cpu.gpr[4] = 0x9840u;
            g_cpu.gpr[5] = 0;
            FN_CreateProbe__7392c(&g_cpu);
            uint32_t s3probe = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(s3ins != M2K_ITEM_NONE && s3alpha != M2K_ITEM_NONE);
            CHECK(s3beta != M2K_ITEM_NONE && s3feed != M2K_ITEM_NONE);
            CHECK(s3probe != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, 0x9840u, 0, s3ins,
                      0x9860u, 0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, s3ins, 0) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, s3probe, 0,
                                                 0x99A0u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x99A0u) == 2999.0f / 32768.0f);
            CHECK(m2_guest_audio_read_probe_part(&g_cpu, s3probe, 1,
                                                 0x99C0u) == M2K_OK);
            CHECK(rdf32(&g_cpu, 0x99C0u) == 2999.0f / 32768.0f);
            hdl->DeleteItem(s3ins);
            hdl->DeleteItem(s3tpl);
            hdl->DeleteItem(rp_ins);
            hdl->DeleteItem(rpt);
            hdl->DeleteItem(m_ins);
            hdl->DeleteItem(a_ins);
            hdl->DeleteItem(l_ins);
            hdl->DeleteItem((uint32_t)mtl);
            hdl->DeleteItem((uint32_t)atl);
            hdl->DeleteItem((uint32_t)ltl);
            (void)m2_guest_audio_unload_instrument((uint32_t)add_ins);
        } else {
            printf("m2_audio_items_test: disc DSP templates not found; "
                   "mix-node math regression skipped\n");
        }
    }
    teardown();

    setup();

    // Shared gMixer staging: line_out's destination rate selects the bucket,
    // then nanokernel.ins averages the raw half-rate result on even frames
    // and reuses that raw result on the intervening odd frame. The add.dsp
    // and line_out.dsp templates are read from the extracted disc bridge.
    {
        if (test_dsp_raw_open("System.m2/Audio/dsp/add.dsp") != 0 &&
            test_dsp_raw_open("System.m2/Audio/dsp/line_out.dsp") != 0) {
            const uint32_t add_name_ea = 0xA800u;
            const uint32_t line_name_ea = 0xA820u;
            const uint32_t output_name_ea = 0xA840u;
            const uint32_t input_a_name_ea = 0xA860u;
            const uint32_t input_b_name_ea = 0xA880u;
            const uint32_t input_name_ea = 0xA8A0u;
            write_guest_string(add_name_ea, "add.dsp");
            write_guest_string(line_name_ea, "line_out.dsp");
            write_guest_string(output_name_ea, "Output");
            write_guest_string(input_a_name_ea, "InputA");
            write_guest_string(input_b_name_ea, "InputB");
            write_guest_string(input_name_ea, "Input");

            int32_t add_ins = m2_guest_audio_load_instrument(
                &g_cpu, add_name_ea, 1, 100);
            int32_t line_ins = m2_guest_audio_load_instrument(
                &g_cpu, line_name_ea, 2, 100);
            CHECK(add_ins > 0 && line_ins > 0);
            // Build the DSP sim before setting knobs; the vendor start path
            // owns template execution state, while SetKnobPart then writes
            // the live resource words.
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                   (uint32_t)add_ins, 0) ==
                  M2K_OK);

            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = input_a_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_a = g_cpu.gpr[3];
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = input_b_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_b = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(knob_a != M2K_ITEM_NONE && knob_b != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(
                      &g_cpu, knob_a, 0, 1000.0f / 32768.0f) == M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(
                      &g_cpu, knob_b, 0, 2000.0f / 32768.0f) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, output_name_ea, 0,
                      (uint32_t)line_ins, input_name_ea, 0) == M2K_OK);

            int32_t mix[2] = {0, 0};
            m2_guest_audio_tick_dspp_frames(1u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 0 && mix[1] == 0);
            m2_guest_audio_tick_dspp_frames(1u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 1500 && mix[1] == 0);
            m2_guest_audio_tick_dspp_frames(1u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 3000 && mix[1] == 0);

            // New even-frame half result 2000 is averaged with the prior
            // raw half result 3000: (3000 >> 1) + (2000 >> 1) = 2500.
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, knob_a, 0, 0.0f) ==
                  M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 2500 && mix[1] == 0);

            CHECK(m2_guest_audio_unload_instrument((uint32_t)add_ins) ==
                  M2K_OK);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)line_ins) ==
                  M2K_OK);
        } else {
            printf("m2_audio_items_test: add/line_out DSP templates not "
                   "found; gMixer staging regression skipped\n");
        }
    }
    teardown();

    setup();
    // DSPPStopInstrument removes execution and disables DMA, but keeps the
    // instrument's data allocation and last Output. Deletion instead calls
    // DisconnectRemaining before freeing it (audio_instr.c). A surviving
    // line_out must never read the knobs of a subsequently allocated filter.
    {
        if (test_dsp_raw_open("System.m2/Audio/dsp/add.dsp") != 0 &&
            test_dsp_raw_open("System.m2/Audio/dsp/line_out.dsp") != 0 &&
            test_dsp_raw_open("System.m2/Audio/dsp/svfilter.dsp") != 0) {
            write_guest_string(0xB000u, "add.dsp");
            write_guest_string(0xB020u, "InputA");
            write_guest_string(0xB040u, "Output");
            write_guest_string(0xB060u, "Input");
            write_guest_string(0xB080u, "line_out.dsp");
            write_guest_string(0xB0A0u, "svfilter.dsp");
            const int32_t source = m2_guest_audio_load_instrument(&g_cpu, 0xB000u, 1, 100);
            const int32_t output = m2_guest_audio_load_instrument(&g_cpu, 0xB080u, 1, 100);
            CHECK(source > 0 && output > 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)source, 0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)output, 0) == M2K_OK);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)source; g_cpu.gpr[4] = 0xB020u; g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            const uint32_t input_knob = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(input_knob != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, input_knob, 0, 1000.0f / 32768.0f) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(&g_cpu, (uint32_t)source, 0xB040u, 0,
                                                         (uint32_t)output, 0xB060u, 0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(2u);
            int32_t mix[2];
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 999 && mix[1] == 0);
            CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)source, 0) == M2K_OK);
            const int32_t filter = m2_guest_audio_load_instrument(&g_cpu, 0xB0A0u, 1, 100);
            CHECK(filter > 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)filter, 0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(3u);
            m2_guest_audio_read_resident_mix(mix);
            // A stopped source holds 1000: (1000 * $7fff) >> 15 = 999.
            // The old lifecycle reuses that word as filter Amplitude and
            // produces 32766, despite no sample or filter input playing.
            CHECK(mix[0] == 999 && mix[1] == 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)source, 0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(2u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 999 && mix[1] == 0);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)source) == M2K_OK);
            const int32_t replacement = m2_guest_audio_load_instrument(&g_cpu, 0xB0A0u, 1, 100);
            CHECK(replacement > 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)replacement, 0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(3u);
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 0 && mix[1] == 0);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)filter) == M2K_OK);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)replacement) == M2K_OK);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)output) == M2K_OK);

            // minimum.ins declares Output=$7fff with DRSC_AT_START.
            // Start resets that word while retaining allocation-only knobs.
            if (test_dsp_raw_open("System.m2/Audio/dsp/minimum.dsp") != 0) {
                write_guest_string(0xB0C0u, "minimum.dsp");
                const int32_t minimum = m2_guest_audio_load_instrument(&g_cpu, 0xB0C0u, 1, 100);
                CHECK(minimum > 0);
                CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)minimum, 0) == M2K_OK);
                m2_kernel_dispatch_set_global(hdl);
                g_cpu.gpr[3] = (uint32_t)minimum; g_cpu.gpr[4] = 0xB040u; g_cpu.gpr[5] = 0;
                FN_CreateProbe__7392c(&g_cpu);
                const uint32_t minimum_probe = g_cpu.gpr[3];
                g_cpu.gpr[3] = (uint32_t)minimum; g_cpu.gpr[4] = 0xB020u; g_cpu.gpr[5] = 0;
                FN_CreateKnob__73778(&g_cpu);
                const uint32_t minimum_knob = g_cpu.gpr[3];
                m2_kernel_dispatch_set_global(NULL);
                CHECK(minimum_probe != M2K_ITEM_NONE && minimum_knob != M2K_ITEM_NONE);
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, minimum_probe, 0, 0xB100u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0xB100u) == 32767.0f / 32768.0f);
                m2_guest_audio_tick_dspp_frames(1u);
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, minimum_probe, 0, 0xB100u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0xB100u) == 0.0f);
                CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)minimum, 0) == M2K_OK);
                CHECK(m2_guest_audio_set_knob_part(&g_cpu, minimum_knob, 0, -1000.0f / 32768.0f) == M2K_OK);
                CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)minimum, 0) == M2K_OK);
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, minimum_probe, 0, 0xB100u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0xB100u) == 32767.0f / 32768.0f);
                m2_guest_audio_tick_dspp_frames(1u);
                CHECK(m2_guest_audio_read_probe_part(&g_cpu, minimum_probe, 0, 0xB100u) == M2K_OK);
                CHECK(rdf32(&g_cpu, 0xB100u) == -1000.0f / 32768.0f);
                CHECK(m2_guest_audio_unload_instrument((uint32_t)minimum) == M2K_OK);
            }
        } else {
            printf("m2_audio_items_test: disc templates absent; stopped-source allocation regression skipped\n");
        }
    }
    teardown();

    setup();
    // Generated-mixer hop, started line_out (single delivery): a started
    // generated-mixer node's Output feeds a STARTED line_out through the
    // reserved carrier words — ConnectInstrumentParts patches line_out's
    // Input operand with the mixer's reserved shared-image address (vendor
    // generated-mixer image semantics), each tick publishes the mixer's
    // computed output into that word, and line_out's on-disc code
    // accumulates it into the head's gMixer for the DAC staging. The host
    // bucket arm retires for a started sim destination (single-delivery
    // guard), so the exact resident value pins reserve + patch + publish +
    // staging with no double count.
    {
        if (test_dsp_raw_open("System.m2/Audio/dsp/add.dsp") != 0 &&
            test_dsp_raw_open("System.m2/Audio/dsp/line_out.dsp") != 0) {
            const uint32_t add_name_ea = 0xB000u;
            const uint32_t ina_name_ea = 0xB020u;
            const uint32_t inb_name_ea = 0xB040u;
            const uint32_t out_name_ea = 0xB060u;
            const uint32_t in_name_ea = 0xB080u;
            const uint32_t gain_name_ea = 0xB0A0u;
            const uint32_t lo_name_ea = 0xB0C0u;
            write_guest_string(add_name_ea, "add.dsp");
            write_guest_string(ina_name_ea, "InputA");
            write_guest_string(inb_name_ea, "InputB");
            write_guest_string(out_name_ea, "Output");
            write_guest_string(in_name_ea, "Input");
            write_guest_string(gain_name_ea, "Gain");

            int32_t add_ins = m2_guest_audio_load_instrument(
                &g_cpu, add_name_ea, 1, 100);
            CHECK(add_ins > 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                   (uint32_t)add_ins, 0) ==
                  M2K_OK);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = ina_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_a = g_cpu.gpr[3];
            g_cpu.gpr[3] = (uint32_t)add_ins;
            g_cpu.gpr[4] = inb_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t knob_b = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(knob_a != M2K_ITEM_NONE && knob_b != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(
                      &g_cpu, knob_a, 0, 1000.0f / 32768.0f) == M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(
                      &g_cpu, knob_b, 0, 2000.0f / 32768.0f) == M2K_OK);

            // 2x1 generated-mixer node fed by add; output 2999 per the
            // mixer-math regression above (gain 1.0, (3000*32767)>>15).
            int32_t mtl = m2_guest_audio_create_mixer_template(
                &g_cpu, (2u << 24) | (1u << 16), 0);
            CHECK(mtl > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)mtl;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t m_ins = g_cpu.gpr[3];
            g_cpu.gpr[3] = m_ins;
            g_cpu.gpr[4] = gain_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t mgain = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(m_ins != M2K_ITEM_NONE && mgain != M2K_ITEM_NONE);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, mgain, 0, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)add_ins, out_name_ea, 0, m_ins,
                      in_name_ea, 0) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, m_ins, 0) ==
                  M2K_OK);

            // line_out starts now: its gMixer import resolves through the
            // importer kernel (the pre-repair EXTERNALREF boundary). Divider
            // 1 keeps the sim ticking every frame so the assert window below
            // is not half-rate timing-sensitive.
            write_guest_string(lo_name_ea, "line_out.dsp");
            int32_t lo_ins = m2_guest_audio_load_instrument(
                &g_cpu, lo_name_ea, 1, 100);
            CHECK(lo_ins > 0);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)lo_ins,
                                                  0) == M2K_OK);

            // The hop: mixer Output[0] -> line_out Input[0] installs through
            // the reserved carrier (start-then-connect order; the connect
            // arm sees both endpoints live).
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, m_ins, out_name_ea, 0, (uint32_t)lo_ins,
                      in_name_ea, 0) == M2K_OK);

            // Frame 1: line_out reads the carrier's initial 0. Frame 2:
            // publishes frame 1's mixer output; line_out reads 2999 into
            // gMixer and the staging transfers gMixer x Amplitude($7FFF)
            // (nanokernel.ins OutputToDAC block) = (2999*32767)>>15 = 2998.
            // Assert stable at frame 3. 2998 pins reserve + patch + publish
            // + staging in one value: 0 would mean no carrier, 2999 would
            // mean the Amplitude scale was skipped, 5996+ would mean the
            // retired bucket arm double-counted.
            m2_guest_audio_tick_dspp_frames(3u);
            {
                int32_t mix[2] = {0, 0};
                m2_guest_audio_read_resident_mix(mix);
                CHECK(mix[0] == 2998 && mix[1] == 0);
            }

            // Disconnect frees the carrier and restores the relocation
            // default (immediate 0): line_out adds nothing, the resident mix
            // returns to 0 next frame.
            CHECK(m2_guest_audio_disconnect_instrument_parts(
                      &g_cpu, (uint32_t)lo_ins, in_name_ea, 0) == M2K_OK);
            m2_guest_audio_tick_dspp_frames(1u);
            {
                int32_t mix[2] = {0, 0};
                m2_guest_audio_read_resident_mix(mix);
                CHECK(mix[0] == 0 && mix[1] == 0);
            }

            CHECK(m2_guest_audio_unload_instrument((uint32_t)add_ins) ==
                  M2K_OK);
            hdl->DeleteItem(m_ins);
            hdl->DeleteItem(mgain);
            hdl->DeleteItem((uint32_t)mtl);
        } else {
            printf("m2_audio_items_test: add/line_out DSP templates not "
                   "found; generated-mixer hop regression skipped\n");
        }
    }
    teardown();

    setup();

    // R2 voice-fed resident mix: a started

    // sample-voice instrument feeds a 2x2 WITH_LINE_OUT mixer; both wires

    // share one tap frame and the seam reads the mixed pair.

    {

        write_guest_string(0xA000u, "sampler_16_f2.dsp");

        int32_t vsrc = m2_guest_audio_load_instrument(&g_cpu, 0xA000u, 1, 100);

        CHECK(vsrc > 0);

        const uint32_t VDATA = 0xA100u;

        const uint32_t VTAGS = 0xA200u;

        g_mem[VDATA + 0] = 0x00; g_mem[VDATA + 1] = 0x01;

        g_mem[VDATA + 2] = 0x00; g_mem[VDATA + 3] = 0x02;

        g_mem[VDATA + 4] = 0xFF; g_mem[VDATA + 5] = 0xFE;

        g_mem[VDATA + 6] = 0x7F; g_mem[VDATA + 7] = 0xFF;

        wr32(&g_cpu, VTAGS + 0, M2_AUDIO_TAG_ADDRESS);

        wr32(&g_cpu, VTAGS + 4, VDATA);

        wr32(&g_cpu, VTAGS + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);

        wr32(&g_cpu, VTAGS + 12, 0x46ac4400u);

        wr32(&g_cpu, VTAGS + 16, M2_AUDIO_TAG_CHANNELS);

        wr32(&g_cpu, VTAGS + 20, 1u);

        wr32(&g_cpu, VTAGS + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);

        wr32(&g_cpu, VTAGS + 28, 1u);

        wr32(&g_cpu, VTAGS + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);

        wr32(&g_cpu, VTAGS + 36, 0u);

        wr32(&g_cpu, VTAGS + 40, M2_AUDIO_TAG_NUMBITS);

        wr32(&g_cpu, VTAGS + 44, 16u);

        wr32(&g_cpu, VTAGS + 48, M2_AUDIO_TAG_NUMBYTES);

        wr32(&g_cpu, VTAGS + 52, 8u);

        wr32(&g_cpu, VTAGS + 56, M2_AUDIO_TAG_RELEASEBEGIN);

        wr32(&g_cpu, VTAGS + 60, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 64, M2_AUDIO_TAG_RELEASEEND);

        wr32(&g_cpu, VTAGS + 68, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 72, M2_AUDIO_TAG_SUSTAINBEGIN);

        wr32(&g_cpu, VTAGS + 76, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 80, M2_AUDIO_TAG_SUSTAINEND);

        wr32(&g_cpu, VTAGS + 84, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 88, M2_TAG_END);

        wr32(&g_cpu, VTAGS + 92, 0u);

        uint32_t vsample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, VTAGS);

        CHECK(vsample != M2K_ITEM_NONE);

        m2_kernel_dispatch_set_global(hdl);

        g_cpu.gpr[3] = (uint32_t)vsrc;

        g_cpu.gpr[4] = vsample;

        g_cpu.gpr[5] = 0;

        FN_CreateAttachment__73394(&g_cpu);

        uint32_t vatt = g_cpu.gpr[3];

        m2_kernel_dispatch_set_global(NULL);

        CHECK(vatt != M2K_ITEM_NONE);

        g_cpu.gpr[3] = (uint32_t)vsrc;

        g_cpu.gpr[4] = 0;

        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);

        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

        write_guest_string(0xA300u, "Output");

        write_guest_string(0xA320u, "Input");

        write_guest_string(0xA340u, "Gain");

        int32_t vmt = m2_guest_audio_create_mixer_template(

            &g_cpu, (2u << 24) | (2u << 16) | 2u, 0);

        CHECK(vmt > 0);

        m2_kernel_dispatch_set_global(hdl);

        g_cpu.gpr[3] = (uint32_t)vmt;

        g_cpu.gpr[4] = 0;

        FN_CreateInstrument__73734(&g_cpu);

        uint32_t vins = g_cpu.gpr[3];

        g_cpu.gpr[3] = vins;

        g_cpu.gpr[4] = 0xA340u;

        g_cpu.gpr[5] = 0;

        FN_CreateKnob__73778(&g_cpu);

        uint32_t vgain = g_cpu.gpr[3];

        m2_kernel_dispatch_set_global(NULL);

        CHECK(vins != M2K_ITEM_NONE && vgain != M2K_ITEM_NONE);

        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 0, 1.0f) == M2K_OK);

        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 3, 1.0f) == M2K_OK);

        CHECK(m2_guest_audio_start_instrument(&g_cpu, vins, 0) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, (uint32_t)vsrc, 0xA300u, 0, vins, 0xA320u,

                  0) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, (uint32_t)vsrc, 0xA300u, 1, vins, 0xA320u,

                  1) == M2K_OK);

        g_tap_script_n = 1;

        g_tap_script_l[0] = 1000;

        g_tap_script_r[0] = 2000;

        g_tap_calls = 0;

        m2_guest_audio_tick_dspp_frames(1u);

        CHECK(g_tap_calls == 1);

        {

            int32_t mix[2] = {0, 0};

            m2_guest_audio_read_resident_mix(mix);

            CHECK(mix[0] == 999 && mix[1] == 1999);

        }

        hdl->DeleteItem(vins);

        hdl->DeleteItem((uint32_t)vmt);

        hdl->DeleteItem((uint32_t)vsrc);

    }

    teardown();



    setup();

    // R2 clip: two line_out endpoints fed 32766 saturate the

    // resident mix at +32767 (line_out.ins gMixer _%A Input _A _CLIP _+;

    // multi-line_out accumulation is the vendor-explicit shape per the

    // line_out autodoc: "the output of other line_out.dsp(@)").

    {

        write_guest_string(0xA000u, "sampler_16_f2.dsp");

        int32_t vsrc = m2_guest_audio_load_instrument(&g_cpu, 0xA000u, 1, 100);

        CHECK(vsrc > 0);

        const uint32_t VDATA = 0xA100u;

        const uint32_t VTAGS = 0xA200u;

        g_mem[VDATA + 0] = 0x00; g_mem[VDATA + 1] = 0x01;

        g_mem[VDATA + 2] = 0x00; g_mem[VDATA + 3] = 0x02;

        g_mem[VDATA + 4] = 0xFF; g_mem[VDATA + 5] = 0xFE;

        g_mem[VDATA + 6] = 0x7F; g_mem[VDATA + 7] = 0xFF;

        wr32(&g_cpu, VTAGS + 0, M2_AUDIO_TAG_ADDRESS);

        wr32(&g_cpu, VTAGS + 4, VDATA);

        wr32(&g_cpu, VTAGS + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);

        wr32(&g_cpu, VTAGS + 12, 0x46ac4400u);

        wr32(&g_cpu, VTAGS + 16, M2_AUDIO_TAG_CHANNELS);

        wr32(&g_cpu, VTAGS + 20, 1u);

        wr32(&g_cpu, VTAGS + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);

        wr32(&g_cpu, VTAGS + 28, 1u);

        wr32(&g_cpu, VTAGS + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);

        wr32(&g_cpu, VTAGS + 36, 0u);

        wr32(&g_cpu, VTAGS + 40, M2_AUDIO_TAG_NUMBITS);

        wr32(&g_cpu, VTAGS + 44, 16u);

        wr32(&g_cpu, VTAGS + 48, M2_AUDIO_TAG_NUMBYTES);

        wr32(&g_cpu, VTAGS + 52, 8u);

        wr32(&g_cpu, VTAGS + 56, M2_AUDIO_TAG_RELEASEBEGIN);

        wr32(&g_cpu, VTAGS + 60, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 64, M2_AUDIO_TAG_RELEASEEND);

        wr32(&g_cpu, VTAGS + 68, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 72, M2_AUDIO_TAG_SUSTAINBEGIN);

        wr32(&g_cpu, VTAGS + 76, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 80, M2_AUDIO_TAG_SUSTAINEND);

        wr32(&g_cpu, VTAGS + 84, 0xFFFFFFFFu);

        wr32(&g_cpu, VTAGS + 88, M2_TAG_END);

        wr32(&g_cpu, VTAGS + 92, 0u);

        uint32_t vsample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, VTAGS);

        CHECK(vsample != M2K_ITEM_NONE);

        m2_kernel_dispatch_set_global(hdl);

        g_cpu.gpr[3] = (uint32_t)vsrc;

        g_cpu.gpr[4] = vsample;

        g_cpu.gpr[5] = 0;

        FN_CreateAttachment__73394(&g_cpu);

        uint32_t vatt = g_cpu.gpr[3];

        m2_kernel_dispatch_set_global(NULL);

        CHECK(vatt != M2K_ITEM_NONE);

        g_cpu.gpr[3] = (uint32_t)vsrc;

        g_cpu.gpr[4] = 0;

        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x708c4u) == 1);

        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

        write_guest_string(0xA300u, "Output");

        write_guest_string(0xA320u, "Input");

        write_guest_string(0xA340u, "Gain");

        write_guest_string(0xA360u, "line_out.dsp");

        // Plain 1x2 mixer (no WITH_LINE_OUT): voice 32767 through gain 1.0

        // is (32767*32767)>>15 = 32766 per output.

        int32_t vmt = m2_guest_audio_create_mixer_template(

            &g_cpu, (1u << 24) | (2u << 16), 0);

        CHECK(vmt > 0);

        m2_kernel_dispatch_set_global(hdl);

        g_cpu.gpr[3] = (uint32_t)vmt;

        g_cpu.gpr[4] = 0;

        FN_CreateInstrument__73734(&g_cpu);

        uint32_t vins = g_cpu.gpr[3];

        g_cpu.gpr[3] = vins;

        g_cpu.gpr[4] = 0xA340u;

        g_cpu.gpr[5] = 0;

        FN_CreateKnob__73778(&g_cpu);

        uint32_t vgain = g_cpu.gpr[3];

        m2_kernel_dispatch_set_global(NULL);

        CHECK(vins != M2K_ITEM_NONE && vgain != M2K_ITEM_NONE);

        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 0, 1.0f) == M2K_OK);

        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 1, 1.0f) == M2K_OK);

        CHECK(m2_guest_audio_start_instrument(&g_cpu, vins, 0) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, (uint32_t)vsrc, 0xA300u, 0, vins, 0xA320u,

                  0) == M2K_OK);

        // Two unstarted line_out endpoints (still route endpoints: the tick

        // checks the template name, not started state).

        int32_t vl1 = m2_guest_audio_load_instrument(&g_cpu, 0xA360u, 1, 100);

        int32_t vl2 = m2_guest_audio_load_instrument(&g_cpu, 0xA360u, 1, 100);

        CHECK(vl1 > 0 && vl2 > 0);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, vins, 0xA300u, 0, (uint32_t)vl1, 0xA320u,

                  0) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, vins, 0xA300u, 1, (uint32_t)vl1, 0xA320u,

                  1) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, vins, 0xA300u, 0, (uint32_t)vl2, 0xA320u,

                  0) == M2K_OK);

        CHECK(m2_guest_audio_connect_instrument_parts(

                  &g_cpu, vins, 0xA300u, 1, (uint32_t)vl2, 0xA320u,

                  1) == M2K_OK);

        g_tap_script_n = 1;

        g_tap_script_l[0] = 32767;

        g_tap_script_r[0] = 0;

        g_tap_calls = 0;

        m2_guest_audio_tick_dspp_frames(1u);

        CHECK(g_tap_calls == 1);

        {

            int32_t mix[2] = {0, 0};

            m2_guest_audio_read_resident_mix(mix);

            CHECK(mix[0] == 32767 && mix[1] == 32767);

        }

        hdl->DeleteItem(vins);

        hdl->DeleteItem((uint32_t)vmt);

        hdl->DeleteItem((uint32_t)vl1);

        hdl->DeleteItem((uint32_t)vl2);

        hdl->DeleteItem((uint32_t)vsrc);

    }

    setup();
    // R2-full render, node-mediated route (DAC push + connection partition):
    // a started sampler routed into a started WITH_LINE_OUT mixer is
    // graph-routed (direct SDL suppressed: no double-play) while a bare
    // started sampler stays direct; the per-tick DAC transfer carries the
    // mixed frames into the mixer FIFO; stopping or disconnecting that route
    // silences the still-running source rather than exposing it directly.
    {
        write_guest_string(0xA000u, "sampler_16_f2.dsp");
        int32_t vsrc = m2_guest_audio_load_instrument(&g_cpu, 0xA000u, 1, 100);
        CHECK(vsrc > 0);
        const uint32_t VDATA = 0xA100u;
        const uint32_t VTAGS = 0xA200u;
        g_mem[VDATA + 0] = 0x00; g_mem[VDATA + 1] = 0x01;
        g_mem[VDATA + 2] = 0x00; g_mem[VDATA + 3] = 0x02;
        g_mem[VDATA + 4] = 0xFF; g_mem[VDATA + 5] = 0xFE;
        g_mem[VDATA + 6] = 0x7F; g_mem[VDATA + 7] = 0xFF;
        wr32(&g_cpu, VTAGS + 0, M2_AUDIO_TAG_ADDRESS);
        wr32(&g_cpu, VTAGS + 4, VDATA);
        wr32(&g_cpu, VTAGS + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
        wr32(&g_cpu, VTAGS + 12, 0x46ac4400u);
        wr32(&g_cpu, VTAGS + 16, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, VTAGS + 20, 1u);
        wr32(&g_cpu, VTAGS + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);
        wr32(&g_cpu, VTAGS + 28, 1u);
        wr32(&g_cpu, VTAGS + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);
        wr32(&g_cpu, VTAGS + 36, 0u);
        wr32(&g_cpu, VTAGS + 40, M2_AUDIO_TAG_NUMBITS);
        wr32(&g_cpu, VTAGS + 44, 16u);
        wr32(&g_cpu, VTAGS + 48, M2_AUDIO_TAG_NUMBYTES);
        wr32(&g_cpu, VTAGS + 52, 8u);
        wr32(&g_cpu, VTAGS + 56, M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, VTAGS + 60, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 64, M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, VTAGS + 68, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 72, M2_AUDIO_TAG_SUSTAINBEGIN);
        wr32(&g_cpu, VTAGS + 76, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 80, M2_AUDIO_TAG_SUSTAINEND);
        wr32(&g_cpu, VTAGS + 84, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 88, M2_TAG_END);
        wr32(&g_cpu, VTAGS + 92, 0u);
        uint32_t vsample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, VTAGS);
        CHECK(vsample != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)vsrc;
        g_cpu.gpr[4] = vsample;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t vatt = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(vatt != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        // Bare (no routes yet): the voice stays direct.
        CHECK(g_voice_direct[0] == 1);
        write_guest_string(0xA300u, "Output");
        write_guest_string(0xA320u, "Input");
        write_guest_string(0xA340u, "Gain");
        int32_t vmt = m2_guest_audio_create_mixer_template(
            &g_cpu, (2u << 24) | (2u << 16) | 2u, 0);
        CHECK(vmt > 0);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)vmt;
        g_cpu.gpr[4] = 0;
        FN_CreateInstrument__73734(&g_cpu);
        uint32_t vins = g_cpu.gpr[3];
        g_cpu.gpr[3] = vins;
        g_cpu.gpr[4] = 0xA340u;
        g_cpu.gpr[5] = 0;
        FN_CreateKnob__73778(&g_cpu);
        uint32_t vgain = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(vins != M2K_ITEM_NONE && vgain != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 0, 1.0f) == M2K_OK);
        CHECK(m2_guest_audio_set_knob_part(&g_cpu, vgain, 3, 1.0f) == M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, vins, 0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 0, vins, 0xA320u,
                  0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 1, vins, 0xA320u,
                  1) == M2K_OK);
        // Routed into a DAC-bound sink: direct suppressed (no double-play).
        CHECK(g_voice_direct[0] == 0);
        // Bare control: a second sampler started with no routes stays direct.
        const uint32_t BDATA = 0xA400u;
        const uint32_t BTAGS = 0xA500u;
        g_mem[BDATA + 0] = 0x00; g_mem[BDATA + 1] = 0x01;
        g_mem[BDATA + 2] = 0x00; g_mem[BDATA + 3] = 0x02;
        g_mem[BDATA + 4] = 0xFF; g_mem[BDATA + 5] = 0xFE;
        g_mem[BDATA + 6] = 0x7F; g_mem[BDATA + 7] = 0xFF;
        wr32(&g_cpu, BTAGS + 0, M2_AUDIO_TAG_ADDRESS);
        wr32(&g_cpu, BTAGS + 4, BDATA);
        wr32(&g_cpu, BTAGS + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
        wr32(&g_cpu, BTAGS + 12, 0x46ac4400u);
        wr32(&g_cpu, BTAGS + 16, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, BTAGS + 20, 1u);
        wr32(&g_cpu, BTAGS + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);
        wr32(&g_cpu, BTAGS + 28, 1u);
        wr32(&g_cpu, BTAGS + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);
        wr32(&g_cpu, BTAGS + 36, 0u);
        wr32(&g_cpu, BTAGS + 40, M2_AUDIO_TAG_NUMBITS);
        wr32(&g_cpu, BTAGS + 44, 16u);
        wr32(&g_cpu, BTAGS + 48, M2_AUDIO_TAG_NUMBYTES);
        wr32(&g_cpu, BTAGS + 52, 8u);
        wr32(&g_cpu, BTAGS + 56, M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, BTAGS + 60, 0xFFFFFFFFu);
        wr32(&g_cpu, BTAGS + 64, M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, BTAGS + 68, 0xFFFFFFFFu);
        wr32(&g_cpu, BTAGS + 72, M2_AUDIO_TAG_SUSTAINBEGIN);
        wr32(&g_cpu, BTAGS + 76, 0xFFFFFFFFu);
        wr32(&g_cpu, BTAGS + 80, M2_AUDIO_TAG_SUSTAINEND);
        wr32(&g_cpu, BTAGS + 84, 0xFFFFFFFFu);
        wr32(&g_cpu, BTAGS + 88, M2_TAG_END);
        wr32(&g_cpu, BTAGS + 92, 0u);
        int32_t bsrc = m2_guest_audio_load_instrument(&g_cpu, 0xA000u, 1, 100);
        CHECK(bsrc > 0);
        uint32_t bsample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, BTAGS);
        CHECK(bsample != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)bsrc;
        g_cpu.gpr[4] = bsample;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t batt = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(batt != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)bsrc, 0) ==
              M2K_OK);
        CHECK(g_voice_direct[1] == 1);
        // Graph feed still carries the routed voice (tap [1000 2000] mixes
        // to [999 1999] through unity SF15 gains, as in the R2 voice block).
        g_tap_script_n = 1;
        g_tap_script_l[0] = 1000;
        g_tap_script_r[0] = 2000;
        g_tap_calls = 0;
        m2_guest_audio_tick_dspp_frames(1u);
        CHECK(g_tap_calls == 1);
        {
            int32_t mix[2] = {0, 0};
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 999 && mix[1] == 1999);
        }
        // DAC transfer: the mixer FIFO carries the mixed frames.
        g_tap_calls = 0;
        g_dspp_push_calls = 0;
        // DAC transfer: the mixer FIFO carries the mixed frames. Reset the
        // pump's static deficit baseline first (frames=1 < any prior
        // counter), so the two armed pumps measure exactly, regardless of
        // earlier blocks' counters.
        g_tap_calls = 0;
        g_dspp_push_calls = 0;
        g_audio_test_frames = 1u;
        m2_guest_audio_pump_dspp_output();
        g_audio_test_frames = 513u;
        m2_guest_audio_pump_dspp_output();
        uint32_t r2_base = g_dspp_push_frames;
        g_audio_test_frames = 577u;
        m2_guest_audio_pump_dspp_output();
        CHECK(g_dspp_push_frames - r2_base == 64u);
        CHECK(g_dspp_first_l == 999 && g_dspp_first_r == 1999);
        CHECK(m2_guest_audio_stop_instrument(&g_cpu, vins, 0) == M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, vins, 0) == M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        CHECK(m2_guest_audio_disconnect_instrument_parts(
                  &g_cpu, vins, 0xA320u, 0) == M2K_OK);
        CHECK(m2_guest_audio_disconnect_instrument_parts(
                  &g_cpu, vins, 0xA320u, 1) == M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        CHECK(g_voice_direct[1] == 1);
        hdl->DeleteItem(vins);
        hdl->DeleteItem((uint32_t)vmt);
        hdl->DeleteItem((uint32_t)vsrc);
        hdl->DeleteItem((uint32_t)bsrc);
    }
    teardown();

    setup();
    // R2-full direct voice -> line_out route + lifecycle refresh paths: the
    // direct feed keeps a partitioned voice single-play (audible via FIFO,
    // silent on direct out); a new bare voice can play directly, while a
    // still-running voice remains silent after its route is deleted.
    {
        write_guest_string(0xA000u, "sampler_16_f2.dsp");
        int32_t vsrc = m2_guest_audio_load_instrument(&g_cpu, 0xA000u, 1, 100);
        CHECK(vsrc > 0);
        const uint32_t VDATA = 0xA100u;
        const uint32_t VTAGS = 0xA200u;
        g_mem[VDATA + 0] = 0x00; g_mem[VDATA + 1] = 0x01;
        g_mem[VDATA + 2] = 0x00; g_mem[VDATA + 3] = 0x02;
        g_mem[VDATA + 4] = 0xFF; g_mem[VDATA + 5] = 0xFE;
        g_mem[VDATA + 6] = 0x7F; g_mem[VDATA + 7] = 0xFF;
        wr32(&g_cpu, VTAGS + 0, M2_AUDIO_TAG_ADDRESS);
        wr32(&g_cpu, VTAGS + 4, VDATA);
        wr32(&g_cpu, VTAGS + 8, M2_AUDIO_TAG_SAMPLE_RATE_FP);
        wr32(&g_cpu, VTAGS + 12, 0x46ac4400u);
        wr32(&g_cpu, VTAGS + 16, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, VTAGS + 20, 1u);
        wr32(&g_cpu, VTAGS + 24, M2_AUDIO_TAG_COMPRESSIONRATIO);
        wr32(&g_cpu, VTAGS + 28, 1u);
        wr32(&g_cpu, VTAGS + 32, M2_AUDIO_TAG_COMPRESSIONTYPE);
        wr32(&g_cpu, VTAGS + 36, 0u);
        wr32(&g_cpu, VTAGS + 40, M2_AUDIO_TAG_NUMBITS);
        wr32(&g_cpu, VTAGS + 44, 16u);
        wr32(&g_cpu, VTAGS + 48, M2_AUDIO_TAG_NUMBYTES);
        wr32(&g_cpu, VTAGS + 52, 8u);
        wr32(&g_cpu, VTAGS + 56, M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, VTAGS + 60, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 64, M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, VTAGS + 68, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 72, M2_AUDIO_TAG_SUSTAINBEGIN);
        wr32(&g_cpu, VTAGS + 76, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 80, M2_AUDIO_TAG_SUSTAINEND);
        wr32(&g_cpu, VTAGS + 84, 0xFFFFFFFFu);
        wr32(&g_cpu, VTAGS + 88, M2_TAG_END);
        wr32(&g_cpu, VTAGS + 92, 0u);
        uint32_t vsample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, VTAGS);
        CHECK(vsample != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)vsrc;
        g_cpu.gpr[4] = vsample;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t vatt = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(vatt != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        CHECK(g_voice_direct[0] == 1);
        write_guest_string(0xA300u, "Output");
        write_guest_string(0xA320u, "Input");
        write_guest_string(0xA360u, "line_out.dsp");
        int32_t vl1 = m2_guest_audio_load_instrument(&g_cpu, 0xA360u, 1, 100);
        CHECK(vl1 > 0);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 0, (uint32_t)vl1, 0xA320u,
                  0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 1, (uint32_t)vl1, 0xA320u,
                  1) == M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        // Direct feed: unmixed tap [1000 2000] lands in the resident mix.
        g_tap_script_n = 1;
        g_tap_script_l[0] = 1000;
        g_tap_script_r[0] = 2000;
        g_tap_calls = 0;
        m2_guest_audio_tick_dspp_frames(1u);
        {
            int32_t mix[2] = {0, 0};
            m2_guest_audio_read_resident_mix(mix);
            CHECK(mix[0] == 1000 && mix[1] == 2000);
        }
        // ... and the DAC transfer pushes it to the mixer FIFO (baseline
        // reset first, as in the node-mediated block above).
        g_tap_calls = 0;
        g_dspp_push_calls = 0;
        g_audio_test_frames = 1u;
        m2_guest_audio_pump_dspp_output();
        g_audio_test_frames = 513u;
        m2_guest_audio_pump_dspp_output();
        uint32_t r2d_base = g_dspp_push_frames;
        g_audio_test_frames = 577u;
        m2_guest_audio_pump_dspp_output();
        CHECK(g_dspp_push_frames - r2d_base == 64u);
        CHECK(g_dspp_first_l == 1000 && g_dspp_first_r == 2000);
        // Restart with no routes: fresh claim plays direct again.
        CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        CHECK(m2_guest_audio_disconnect_instrument_parts(
                  &g_cpu, (uint32_t)vl1, 0xA320u, 0) == M2K_OK);
        CHECK(m2_guest_audio_disconnect_instrument_parts(
                  &g_cpu, (uint32_t)vl1, 0xA320u, 1) == M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        CHECK(g_voice_direct[0] == 1);
        // Retained routes suppress again at start (start-after-connect).
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 0, (uint32_t)vl1, 0xA320u,
                  0) == M2K_OK);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)vsrc, 0xA300u, 1, (uint32_t)vl1, 0xA320u,
                  1) == M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        CHECK(m2_guest_audio_stop_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)vsrc, 0) ==
              M2K_OK);
        CHECK(g_voice_direct[0] == 0);
        // Deleting the sink drops the routes but does not publish the still
        // running sampler to the host output.
        hdl->DeleteItem((uint32_t)vl1);
        CHECK(g_voice_direct[0] == 0);
        hdl->DeleteItem((uint32_t)vsrc);
    }
    teardown();

    setup();
    // R2-full probes (2026-09-08, diagnostics only): CALCRATE_DIVIDE census
    // at CreateInstrument (probe 2) + connect template/voice-mask extension
    // (probe 1). Opening the new calcrate gate here is log-only (cap 32)
    // and cannot change any contract result.
    {
        setenv("M2_AUDIO_CALCRATE_DIAG", "1", 1);
        write_guest_string(0x6000u, "sampler_16_f2.dsp");
        int32_t div1 = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100);
        int32_t div2 = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 2, 100);
        int32_t div8 = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 8, 100);
        CHECK(div1 > 0 && div2 > 0 && div8 > 0);
        // Validation is unchanged under the open gate: other dividers and
        // over-range priorities still reject.
        CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 4, 100) ==
              M2K_ERR_BAD_ITEM);
        CHECK(m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 201) ==
              M2K_ERR_BAD_ITEM);
        // Probe-1 path: a sampler Output[0] -> line_out Input[0] connect
        // exercises the extended [audio-dspp] template/mask fields (voice
        // mask zero pre-start); the contract result is unchanged.
        write_guest_string(0x6100u, "line_out.dsp");
        write_guest_string(0x6200u, "Output");
        write_guest_string(0x6220u, "Input");
        int32_t psrc = m2_guest_audio_load_instrument(&g_cpu, 0x6000u, 1, 100);
        int32_t pline = m2_guest_audio_load_instrument(&g_cpu, 0x6100u, 1, 100);
        CHECK(psrc > 0 && pline > 0);
        CHECK(m2_guest_audio_connect_instrument_parts(
                  &g_cpu, (uint32_t)psrc, 0x6200u, 0,
                  (uint32_t)pline, 0x6220u, 0) == M2K_OK);
    }

    teardown();

    setup();
    // Reduced-rate DSPP execution lists: route three independent copies of
    // the on-disc stateful sawtooth through a 3x3 guest mixer.  The source
    // outputs are isolated on mixer outputs 0/1/2 so a held value proves that
    // the corresponding instrument did not execute on that DAC frame.
    {
        if (test_dsp_raw_open("System.m2/Audio/dsp/sawtooth.dsp") != 0) {
            const uint32_t saw_name_ea = 0xA600u;
            const uint32_t output_name_ea = 0xA620u;
            const uint32_t input_name_ea = 0xA640u;
            const uint32_t gain_name_ea = 0xA660u;
            write_guest_string(saw_name_ea, "sawtooth.dsp");
            write_guest_string(output_name_ea, "Output");
            write_guest_string(input_name_ea, "Input");
            write_guest_string(gain_name_ea, "Gain");

            int32_t saw_template = m2_guest_audio_load_ins_template(
                &g_cpu, saw_name_ea, 0);
            CHECK(saw_template > 0);
            const uint32_t createitem_tags_ea = 0xA680u;
            wr32(&g_cpu, createitem_tags_ea + 0,
                 M2_AUDIO_TAG_TEMPLATE);
            wr32(&g_cpu, createitem_tags_ea + 4, (uint32_t)saw_template);
            wr32(&g_cpu, createitem_tags_ea + 8,
                 M2_AUDIO_TAG_CALCRATE_DIVIDE);
            wr32(&g_cpu, createitem_tags_ea + 12, 2u);
            wr32(&g_cpu, createitem_tags_ea + 16, M2_AUDIO_TAG_PRIORITY);
            wr32(&g_cpu, createitem_tags_ea + 20, 100u);
            wr32(&g_cpu, createitem_tags_ea + 24, M2_TAG_END);
            wr32(&g_cpu, createitem_tags_ea + 28, 0u);
            uint32_t saw_half = hdl->CreateItem(
                M2_AUDIO_INSTRUMENT_CTYPE, createitem_tags_ea);
            m2_kernel_dispatch_set_global(hdl);
            uint32_t saw_full = hdl->CreateItemVA(
                M2_AUDIO_INSTRUMENT_CTYPE, M2_AUDIO_TAG_TEMPLATE,
                (uint32_t)saw_template,
                M2_AUDIO_TAG_CALCRATE_DIVIDE, 1u, M2_AUDIO_TAG_PRIORITY,
                100u, M2_TAG_JUMP);
            uint32_t saw_eighth = hdl->CreateItemVA(
                M2_AUDIO_INSTRUMENT_CTYPE, M2_AUDIO_TAG_TEMPLATE,
                (uint32_t)saw_template,
                M2_AUDIO_TAG_CALCRATE_DIVIDE, 8u, M2_AUDIO_TAG_PRIORITY,
                100u, M2_TAG_JUMP);
            uint32_t bad_rate = hdl->CreateItemVA(
                M2_AUDIO_INSTRUMENT_CTYPE, M2_AUDIO_TAG_TEMPLATE,
                (uint32_t)saw_template,
                M2_AUDIO_TAG_CALCRATE_DIVIDE, 4u, M2_TAG_JUMP, 0u,
                M2_TAG_JUMP);
            uint32_t bad_priority = hdl->CreateItemVA(
                M2_AUDIO_INSTRUMENT_CTYPE, M2_AUDIO_TAG_TEMPLATE,
                (uint32_t)saw_template,
                M2_AUDIO_TAG_PRIORITY, 201u, M2_TAG_JUMP, 0u,
                M2_TAG_JUMP);
            m2_kernel_dispatch_set_global(NULL);
            CHECK(saw_full != M2K_ITEM_NONE && saw_half != M2K_ITEM_NONE &&
                  saw_eighth != M2K_ITEM_NONE);
            CHECK((int32_t)bad_rate == M2_AUDIO_ERR_BAD_TAG_VALUE);
            CHECK((int32_t)bad_priority == M2_AUDIO_ERR_BAD_TAG_VALUE);

            int32_t mixer_template = m2_guest_audio_create_mixer_template(
                &g_cpu, (3u << 24) | (3u << 16), 0);
            CHECK(mixer_template > 0);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)mixer_template;
            g_cpu.gpr[4] = 0;
            FN_CreateInstrument__73734(&g_cpu);
            uint32_t mixer = g_cpu.gpr[3];
            g_cpu.gpr[3] = mixer;
            g_cpu.gpr[4] = gain_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateKnob__73778(&g_cpu);
            uint32_t gain = g_cpu.gpr[3];
            g_cpu.gpr[3] = mixer;
            g_cpu.gpr[4] = output_name_ea;
            g_cpu.gpr[5] = 0;
            FN_CreateProbe__7392c(&g_cpu);
            uint32_t probe = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(mixer != M2K_ITEM_NONE && gain != M2K_ITEM_NONE);
            CHECK(probe != M2K_ITEM_NONE);

            CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain, 0, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain, 4, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_set_knob_part(&g_cpu, gain, 8, 1.0f) ==
                  M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)saw_full, output_name_ea, 0, mixer,
                      input_name_ea, 0) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)saw_half, output_name_ea, 0, mixer,
                      input_name_ea, 1) == M2K_OK);
            CHECK(m2_guest_audio_connect_instrument_parts(
                      &g_cpu, (uint32_t)saw_eighth, output_name_ea, 0, mixer,
                      input_name_ea, 2) == M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                   (uint32_t)saw_full, 0) ==
                  M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                   (uint32_t)saw_half, 0) ==
                  M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu,
                                                   (uint32_t)saw_eighth, 0) ==
                  M2K_OK);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, mixer, 0) == M2K_OK);

            float initial[3] = {0.0f, 0.0f, 0.0f};
            float frame1[3] = {0.0f, 0.0f, 0.0f};
            float frame2[3] = {0.0f, 0.0f, 0.0f};
            float frame7[3] = {0.0f, 0.0f, 0.0f};
            float frame8[3] = {0.0f, 0.0f, 0.0f};
            const uint32_t read_ea[3] = {0xA700u, 0xA704u, 0xA708u};
            for (int i = 0; i < 3; ++i) {
                CHECK(m2_guest_audio_read_probe_part(
                          &g_cpu, probe, i, read_ea[i]) == M2K_OK);
                initial[i] = rdf32(&g_cpu, read_ea[i]);
            }
            m2_guest_audio_tick_dspp_frames(1u);
            for (int i = 0; i < 3; ++i) {
                CHECK(m2_guest_audio_read_probe_part(
                          &g_cpu, probe, i, read_ea[i]) == M2K_OK);
                frame1[i] = rdf32(&g_cpu, read_ea[i]);
            }
            // FrameCountLow is 1 here: full-rate runs, half/eighth do not.
            CHECK(frame1[0] != initial[0]);
            CHECK(frame1[1] == initial[1]);
            CHECK(frame1[2] == initial[2]);

            m2_guest_audio_tick_dspp_frames(1u);
            for (int i = 0; i < 3; ++i) {
                CHECK(m2_guest_audio_read_probe_part(
                          &g_cpu, probe, i, read_ea[i]) == M2K_OK);
                frame2[i] = rdf32(&g_cpu, read_ea[i]);
            }
            // The half-rate list runs on even FrameCountLow (2).
            CHECK(frame2[1] != frame1[1]);
            CHECK(frame2[2] == frame1[2]);

            m2_guest_audio_tick_dspp_frames(5u);
            for (int i = 0; i < 3; ++i) {
                CHECK(m2_guest_audio_read_probe_part(
                          &g_cpu, probe, i, read_ea[i]) == M2K_OK);
                frame7[i] = rdf32(&g_cpu, read_ea[i]);
            }
            CHECK(frame7[2] == frame2[2]);
            m2_guest_audio_tick_dspp_frames(1u);
            for (int i = 0; i < 3; ++i) {
                CHECK(m2_guest_audio_read_probe_part(
                          &g_cpu, probe, i, read_ea[i]) == M2K_OK);
                frame8[i] = rdf32(&g_cpu, read_ea[i]);
            }
            // The eighth-rate list first runs when FrameCountLow is 8.
            CHECK(frame8[2] != frame7[2]);

            hdl->DeleteItem(mixer);
            hdl->DeleteItem((uint32_t)mixer_template);
            hdl->DeleteItem(saw_full);
            hdl->DeleteItem(saw_half);
            hdl->DeleteItem(saw_eighth);
            hdl->DeleteItem((uint32_t)saw_template);
        } else {
            printf("m2_audio_items_test: sawtooth.dsp not found; "
                   "reduced-rate DSPP regression skipped\n");
        }
    }
    teardown();

    // ---- DSPP trigger -> Cue poll-and-route ---------------------------------
    // The DSPI_CPU_INT soft-bit half raises into the shared main-interrupt
    // bank (loader); this half must poll it after the mix-node tick, clear
    // exactly the armed-received bits before delivering, and route each bit
    // once through the trigger -> Cue table (vendor audio_trigger.c
    // MonitorTrigger/HandleTriggerSignal). Fresh bind so no earlier section
    // can leave a bit, cue, or started sim behind.
    setup();
    {
        const uint32_t trig_name_ea = 0xB000u;
        // No started sim yet: there is no DSP hardware to raise from, so no
        // cue can be fabricated for the unbuilt graph.
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 3u) ==
              M2_AUDIO_ERR_BAD_ITEM);
        write_guest_string(trig_name_ea, "sampler_16_f2.dsp");
        int32_t trig_ins = m2_guest_audio_load_instrument(&g_cpu, trig_name_ea,
                                                          0, 100);
        CHECK(trig_ins > 0);
        g_cpu.gpr[3] = (uint32_t)trig_ins;
        g_cpu.gpr[4] = 0;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x70350u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

        // Production selector 34 resolves the named Trigger resource on the
        // on-disc schmidt_trigger.dsp instrument. It uses the simulator's
        // allocated trigger number (the first trigger in this fresh space is
        // bit 0), then shares the same Cue delivery pump as the synthetic
        // raw-number contract below.
        const uint32_t named_trig_name_ea = 0xB080u;
        write_guest_string(named_trig_name_ea, "schmidt_trigger.dsp");
        int32_t named_trig_ins = m2_guest_audio_load_instrument(
            &g_cpu, named_trig_name_ea, 0, 100);
        CHECK(named_trig_ins > 0);
        g_cpu.gpr[3] = (uint32_t)named_trig_ins;
        g_cpu.gpr[4] = 0;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x70350u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        trig_reset_stages();
        g_trig_named_instrument = (uint32_t)named_trig_ins;
        g_trig_named_name_ea = 0;
        uint32_t named_trig_task = m2k_create_task(
            &g_kctx, trigger_named_task, 0u);
        CHECK(named_trig_task != M2K_ITEM_NONE);
        CHECK(trig_spin_until(&g_trig_ready));
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 0u) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage1, 1, memory_order_release);
        CHECK(trig_spin_until(&g_trig_mid_seen));
        CHECK(g_trig_poll_mid == (int32_t)g_trig_signal);
        atomic_store_explicit(&g_trig_stage2, 1, memory_order_release);
        CHECK(m2k_join_task(&g_kctx, named_trig_task) == M2K_OK);
        CHECK(g_trig_poll_end == 0);
        g_cpu.gpr[3] = (uint32_t)named_trig_ins;
        g_cpu.gpr[4] = 0;
        g_cpu.gpr[5] = 0;
        g_cpu.gpr[6] = 0;
        CHECK(m2_audio_dispatch_c(&g_cpu, 34, 0x123456u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        hdl->DeleteItem(g_trig_cue);
        CHECK(m2k_delete_task(&g_kctx, named_trig_task) == M2K_OK);
        hdl->DeleteItem((uint32_t)named_trig_ins);

        // Table validation: out-of-range bit, non-cue handle, and illegal
        // flags reject; disarming an idle bit succeeds.
        uint32_t trig_probe = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
        CHECK(trig_probe != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 16u, trig_probe, 0u) ==
              M2_AUDIO_ERR_OUT_OF_RANGE);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 3u, (uint32_t)trig_ins,
                                         0u) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 3u, trig_probe, 0x04u) ==
              M2_AUDIO_ERR_OUT_OF_RANGE);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 3u, 0u, 0u) == M2K_OK);
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 16u) ==
              M2_AUDIO_ERR_OUT_OF_RANGE);
        hdl->DeleteItem(trig_probe);

        // Phase 1: a fired bit delivers exactly one cue. The task arms bit 3
        // one-shot; main raises it and ticks once, then ticks four more.
        trig_reset_stages();
        uint32_t trig_t1 = m2k_create_task(&g_kctx, trigger_armed_task, 3u);
        CHECK(trig_t1 != M2K_ITEM_NONE);
        CHECK(trig_spin_until(&g_trig_ready));
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 3u) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage1, 1, memory_order_release);
        CHECK(trig_spin_until(&g_trig_mid_seen));
        m2_guest_audio_tick_dspp_frames(4u);
        atomic_store_explicit(&g_trig_stage2, 1, memory_order_release);
        CHECK(m2k_join_task(&g_kctx, trig_t1) == M2K_OK);
        CHECK(g_trig_signal != 0u);
        CHECK(g_trig_poll_mid == (int32_t)g_trig_signal);
        CHECK(g_trig_poll_end == 0);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 3u, 0u, 0u) == M2K_OK);
        hdl->DeleteItem(g_trig_cue);
        CHECK(m2k_delete_task(&g_kctx, trig_t1) == M2K_OK);

        // Phase 2: the bank was cleared. Re-arming bit 3 with nothing fired
        // delivers nothing across five ticks (a lingering bit would fire).
        trig_reset_stages();
        uint32_t trig_t2 = m2k_create_task(&g_kctx, trigger_armed_task, 3u);
        CHECK(trig_t2 != M2K_ITEM_NONE);
        CHECK(trig_spin_until(&g_trig_ready));
        m2_guest_audio_tick_dspp_frames(4u);
        atomic_store_explicit(&g_trig_stage1, 1, memory_order_release);
        CHECK(trig_spin_until(&g_trig_mid_seen));
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage2, 1, memory_order_release);
        CHECK(m2k_join_task(&g_kctx, trig_t2) == M2K_OK);
        CHECK(g_trig_poll_mid == 0);
        CHECK(g_trig_poll_end == 0);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 3u, 0u, 0u) == M2K_OK);
        hdl->DeleteItem(g_trig_cue);
        CHECK(m2k_delete_task(&g_kctx, trig_t2) == M2K_OK);

        // Phase 3: an unarmed fire fabricates nothing, and the pending bit
        // lingers for a late arm without RESET (vendor-documented). The task
        // only creates the cue; main raises bit 7 unarmed, ticks, polls
        // clean, then arms and ticks once more for the delivery.
        trig_reset_stages();
        uint32_t trig_t3 = m2k_create_task(&g_kctx, trigger_unarmed_task, 7u);
        CHECK(trig_t3 != M2K_ITEM_NONE);
        CHECK(trig_spin_until(&g_trig_ready));
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 7u) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(2u);
        atomic_store_explicit(&g_trig_stage1, 1, memory_order_release);
        CHECK(trig_spin_until(&g_trig_mid_seen));
        CHECK(g_trig_poll_mid == 0);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 7u, g_trig_cue, 0u) ==
              M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage2, 1, memory_order_release);
        CHECK(m2k_join_task(&g_kctx, trig_t3) == M2K_OK);
        CHECK(g_trig_poll_end == (int32_t)g_trig_signal);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 7u, 0u, 0u) == M2K_OK);
        hdl->DeleteItem(g_trig_cue);
        CHECK(m2k_delete_task(&g_kctx, trig_t3) == M2K_OK);

        // Phase 4: continuous mode rearms. Bit 9 stays armed across
        // deliveries, so two separate fires produce two cue signals while a
        // mid-window poll consumes exactly the first.
        trig_reset_stages();
        g_trig_flags = 0x02u; // AF_F_TRIGGER_CONTINUOUS
        uint32_t trig_t4 = m2k_create_task(&g_kctx, trigger_armed_task, 9u);
        CHECK(trig_t4 != M2K_ITEM_NONE);
        CHECK(trig_spin_until(&g_trig_ready));
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 9u) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage1, 1, memory_order_release);
        CHECK(trig_spin_until(&g_trig_mid_seen));
        CHECK(g_trig_poll_mid == (int32_t)g_trig_signal);
        CHECK(m2_guest_audio_raise_trigger(&g_cpu, 9u) == M2K_OK);
        m2_guest_audio_tick_dspp_frames(1u);
        atomic_store_explicit(&g_trig_stage2, 1, memory_order_release);
        CHECK(m2k_join_task(&g_kctx, trig_t4) == M2K_OK);
        CHECK(g_trig_poll_end == (int32_t)g_trig_signal);
        CHECK(m2_guest_audio_arm_trigger(&g_cpu, 9u, 0u, 0u) == M2K_OK);
        hdl->DeleteItem(g_trig_cue);
        CHECK(m2k_delete_task(&g_kctx, trig_t4) == M2K_OK);

        hdl->DeleteItem((uint32_t)trig_ins);
    }
    teardown();
    // ---- Numeric (non-sim) DSPP descriptor attach -------------------------------
    // Vendor audio_attachments.c (DSPX_DMA_ADDRESS_MASK, SetDMANextInt NULL
    // reject, illegal-flag BADTAGVAL) + dspp_instr.c DSPPAttachSample/
    // DSPPStartSampleAttachment (SECURITY/BAD_PORT_TYPE/BAD_NAME/NULLADDRESS)
    // + dspp_simulator.c dspsAccessDMA_Stack (numeric shadow/commit). Pre-fix
    // this block fails (undefined dspp_sim_dma_attach_numeric); post-fix it
    // passes. No disc dependency: synthetic SLEEP template on a fresh shared
    // space. The attach records the validated descriptor on the channel
    // resource only; FIFO/interrupt/completion service still requires a host
    // buffer, so nothing below fabricates samples or signals.
    {
        dspp_template numeric_template = {0};
        numeric_template.code_word_count = 1;
        numeric_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *numeric_space = dspp_sim_space_create();
        dspp_sim *numeric_a =
            dspp_sim_create_in_space(&numeric_template, numeric_space);
        dspp_sim *numeric_b =
            dspp_sim_create_in_space(&numeric_template, numeric_space);
        CHECK(numeric_space != NULL && numeric_a != NULL &&
              numeric_b != NULL);
        if (numeric_a && numeric_b) {
            const uint32_t numeric_ch = 7u;
            CHECK(dspp_sim_dma_numeric_attached(numeric_a, numeric_ch) == 0);
            uint32_t numeric_addr = 0xdeadbeefu;
            uint32_t numeric_count = 0xdeadbeefu;
            uint32_t numeric_flags = 0xdeadbeefu;
            CHECK(dspp_sim_dma_numeric_descriptor(
                      numeric_b, numeric_ch, &numeric_addr, &numeric_count,
                      &numeric_flags) == DSPP_NUMERIC_OK);
            CHECK(numeric_addr == 0u && numeric_count == 0u &&
                  numeric_flags == 0u);
            // Shape rejects: bad channel, NULL sim, >25-bit address,
            // nonzero-count/zero-address, unknown flag bits, SQXD w/o 8BIT.
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, 32u, 0x100u, 1u,
                                              0u) ==
                  DSPP_NUMERIC_ERR_OUT_OF_RANGE);
            CHECK(dspp_sim_dma_attach_numeric(NULL, numeric_ch, 0x100u, 1u,
                                              0u) == DSPP_NUMERIC_ERR_BAD_ITEM);
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch,
                                              0x02000000u, 1u,
                                              0u) ==
                  DSPP_NUMERIC_ERR_OUT_OF_RANGE);
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch, 0u, 4u,
                                              0u) ==
                  DSPP_NUMERIC_ERR_NULL_ADDRESS);
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch, 0x100u,
                                              1u,
                                              0x08u) == DSPP_NUMERIC_ERR_BAD_TYPE);
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch, 0x100u,
                                              1u,
                                              DSPP_SIM_DMA_SQXD) ==
                  DSPP_NUMERIC_ERR_BAD_TYPE);
            CHECK(dspp_sim_dma_numeric_attached(numeric_a, numeric_ch) == 0);
            // Empty descriptor (count 0, address 0) is allowed, mirroring the
            // host-adapter count-0 rule.
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch, 0u, 0u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_attached(numeric_b, numeric_ch) == 1);
            // Valid attach binds to the channel resource, shared across sims.
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch,
                                              0x00123400u, 3u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_attached(numeric_b, numeric_ch) == 1);
            numeric_addr = numeric_count = numeric_flags = 0xdeadbeefu;
            CHECK(dspp_sim_dma_numeric_descriptor(
                      numeric_b, numeric_ch, &numeric_addr, &numeric_count,
                      &numeric_flags) == DSPP_NUMERIC_OK);
            CHECK(numeric_addr == 0x00123400u && numeric_count == 3u &&
                  numeric_flags == 0u);
            // 8BIT+SQXD is the coherent compressed-input combination.
            CHECK(dspp_sim_dma_attach_numeric(
                      numeric_b, numeric_ch, 0x00001000u, 8u,
                      DSPP_SIM_DMA_8BIT | DSPP_SIM_DMA_SQXD) ==
                  DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_descriptor(
                      numeric_a, numeric_ch, &numeric_addr, &numeric_count,
                      &numeric_flags) == DSPP_NUMERIC_OK);
            CHECK(numeric_addr == 0x00001000u && numeric_count == 8u &&
                  numeric_flags ==
                      (DSPP_SIM_DMA_8BIT | DSPP_SIM_DMA_SQXD));
            // Never fabricates: no host buffer was installed and the channel
            // was never enabled, so the FDMA service stays idle with no
            // DMANext for this channel; NULL out-params are tolerated.
            CHECK(dspp_sim_dma_step(numeric_b) == -1);
            CHECK((dspp_sim_dma_dmanext_interrupts(numeric_b) &
                   (1u << numeric_ch)) == 0u);
            CHECK(dspp_sim_dma_numeric_descriptor(numeric_a, numeric_ch, NULL,
                                                  NULL,
                                                  NULL) == DSPP_NUMERIC_OK);
            // Re-attaching replaces the record (documented).
            CHECK(dspp_sim_dma_attach_numeric(numeric_a, numeric_ch, 0x100u,
                                              1u,
                                              DSPP_SIM_DMA_TO_RAM) ==
                  DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_descriptor(
                      numeric_b, numeric_ch, &numeric_addr, &numeric_count,
                      &numeric_flags) == DSPP_NUMERIC_OK);
            CHECK(numeric_addr == 0x100u && numeric_count == 1u &&
                  numeric_flags == DSPP_SIM_DMA_TO_RAM);
            CHECK(dspp_sim_dma_step(numeric_b) == -1);
            // Bad-channel queries read 0 / report the addressing error.
            CHECK(dspp_sim_dma_numeric_attached(numeric_a, 32u) == 0);
            CHECK(dspp_sim_dma_numeric_descriptor(numeric_a, 32u,
                                                  &numeric_addr,
                                                  &numeric_count,
                                                  &numeric_flags) ==
                  DSPP_NUMERIC_ERR_OUT_OF_RANGE);
            CHECK(dspp_sim_dma_numeric_descriptor(NULL, numeric_ch,
                                                  &numeric_addr,
                                                  &numeric_count,
                                                  &numeric_flags) ==
                  DSPP_NUMERIC_ERR_BAD_ITEM);
        }
        dspp_sim_destroy(numeric_b);
        dspp_sim_destroy(numeric_a);
        dspp_sim_space_destroy(numeric_space);
    }

    // ---- Folio numeric-attach seam (dspp_sim_dma_attach_numeric wiring) ----
    // Vendor audio_attachments.c (unknown hook BAD_NAME, non-FIFO
    // BAD_PORT_TYPE, ordinary-to-OUT_FIFO SECURITY) + dspp_instr.c
    // DSPPAttachSample/dsppFindFIFOByName (NULL hook takes the only FIFO) +
    // loader shape validation, wired through the attach seam after the
    // StartAttachment-style master/type validation. Pre-fix this block fails
    // (undefined seam symbols); post-fix it passes. Disc-backed like the
    // other folio tests: sampler_16_f2.dsp carries the single IN_FIFO
    // "InFIFO"; delay_f1.dsp carries the single OUT_FIFO "OutFIFO"; both
    // carry non-FIFO ports for the MISMATCH path.
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_read_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t *address_out, uint32_t *count_out, uint32_t *flags_out);
        setup();
        const uint32_t seam_samp_ea = 0xB000u;
        const uint32_t seam_hook_ea = 0xB040u;
        const uint32_t seam_bad_ea = 0xB060u;
        const uint32_t seam_out_ea = 0xB080u;
        const uint32_t seam_delay_ea = 0xB0A0u;
        const uint32_t seam_outfifo_ea = 0xB0C0u;
        const uint32_t seam_input_ea = 0xB0E0u;
        const uint32_t seam_tags_ea = 0xB100u;
        const uint32_t seam_badtags_ea = 0xB120u;
        write_guest_string(seam_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(seam_hook_ea, "InFIFO");
        write_guest_string(seam_bad_ea, "NoSuchHook");
        write_guest_string(seam_out_ea, "Output");
        write_guest_string(seam_delay_ea, "delay_f1.dsp");
        write_guest_string(seam_outfifo_ea, "OutFIFO");
        write_guest_string(seam_input_ea, "Input");
        // 0x4E pair (value bits uninterpreted) + END.
        wr32(&g_cpu, seam_tags_ea + 0u, M2_AUDIO_TAG_START_0x4E);
        wr32(&g_cpu, seam_tags_ea + 4u, 0x3F800000u);
        wr32(&g_cpu, seam_tags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, seam_tags_ea + 12u, 0u);
        wr32(&g_cpu, seam_badtags_ea + 0u, 0x1234u);
        wr32(&g_cpu, seam_badtags_ea + 4u, 0u);
        wr32(&g_cpu, seam_badtags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, seam_badtags_ea + 12u, 0u);
        int32_t seam_ins = m2_guest_audio_load_instrument(&g_cpu, seam_samp_ea, 1, 100);
        CHECK(seam_ins > 0);
        uint32_t seam_addr = 0xdeadbeefu, seam_count = 0xdeadbeefu, seam_flags = 0xdeadbeefu;
        // Never fabricate for unbuilt nodes: zero handle, wrong-typed Cue
        // handle, and the query path all report BAD_ITEM.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, 0u, seam_hook_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_BAD_ITEM);
        uint32_t seam_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
        CHECK(seam_cue != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, seam_cue, seam_hook_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, seam_cue, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2_AUDIO_ERR_BAD_ITEM);
        // Pure-synth stays UNIMPLEMENTED: a mixer-template instrument builds
        // no DSP image, so there is no DMA hardware to bind or query.
        int32_t seam_mixer_template = m2_guest_audio_create_mixer_template(&g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(seam_mixer_template > 0);
        uint32_t seam_mixer_ins = hdl->CreateItemVA(M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)seam_mixer_template, M2_TAG_JUMP, 0, 0, 0, 0);
        CHECK(seam_mixer_ins != M2K_ITEM_NONE);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, seam_mixer_ins, seam_hook_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, seam_mixer_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2_AUDIO_ERR_UNIMPLEMENTED);
        // Hook resolution on the sampler (builds its sim on first use):
        // unknown hook -> NAME_MISSING, non-FIFO Output -> MISMATCH, OOB
        // hook EA -> BAD_PTR.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_bad_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_NAME_MISSING);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_out_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_MISMATCH);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, RECOMP_GUEST_MEM + 4u, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_BAD_PTR);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_bad_ea, &seam_addr, &seam_count, &seam_flags) == M2_AUDIO_ERR_NAME_MISSING);
        // Loader shape rejects map to the exact folio errors and leave the
        // channel unbound (zeros read back).
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x100u, 1u, 0x08u, 0, 0u) == M2_AUDIO_ERR_BAD_TAG_VALUE);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x100u, 1u, DSPP_SIM_DMA_SQXD, 0, 0u) == M2_AUDIO_ERR_BAD_TAG_VALUE);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x02000000u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_OUT_OF_RANGE);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0u, 4u, 0u, 0, 0u) == M2_AUDIO_ERR_BAD_PTR);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x100u, 1u, 0u, 0, seam_badtags_ea) == M2_AUDIO_ERR_BAD_TAG);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0u && seam_count == 0u && seam_flags == 0u);
        // A valid numeric attach binds through the seam: the default (NULL)
        // hook and the named InFIFO resolve to the same single-FIFO channel.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, 0u, 0x1200u, 16u, 0u, 0, 0u) == M2K_OK);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0x1200u && seam_count == 16u && seam_flags == 0u);
        // 0x4E start tags are retained uninterpreted: the bind still lands.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x1400u, 4u, 0u, 0, seam_tags_ea) == M2K_OK);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0x1400u && seam_count == 4u && seam_flags == 0u);
        // Re-attach replaces, and count-0+address-0 stays allowed.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x2000u, 8u, DSPP_SIM_DMA_TO_RAM, 0, 0u) == M2K_OK);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0x2000u && seam_count == 8u && seam_flags == DSPP_SIM_DMA_TO_RAM);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0u, 0u, 0u, 0, 0u) == M2K_OK);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0u && seam_count == 0u && seam_flags == 0u);
        // OUT_FIFO (delay_f1 "OutFIFO"): ordinary sample -> SECURITY, the
        // stated delay slave binds, non-FIFO Input -> MISMATCH.
        int32_t seam_delay = m2_guest_audio_load_instrument(&g_cpu, seam_delay_ea, 1, 100);
        CHECK(seam_delay > 0);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_delay, seam_outfifo_ea, 0x3000u, 8u, 0u, 0, 0u) == M2_AUDIO_ERR_SECURITY);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_delay, seam_input_ea, 0x3000u, 8u, 0u, 0, 0u) == M2_AUDIO_ERR_MISMATCH);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_delay, seam_outfifo_ea, 0x3000u, 8u, 0u, 1, 0u) == M2K_OK);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_delay, seam_outfifo_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0x3000u && seam_count == 8u && seam_flags == 0u);
        // Busy: starting the sampler seals its channels with NO_MEMORY while
        // the query stays observational.
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)seam_ins, 0) == M2K_OK);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_NO_MEMORY);
        CHECK(m2_guest_audio_read_numeric_descriptor(&g_cpu, (uint32_t)seam_ins, seam_hook_ea, &seam_addr, &seam_count, &seam_flags) == M2K_OK);
        CHECK(seam_addr == 0u && seam_count == 0u && seam_flags == 0u);
        hdl->DeleteItem(seam_cue);
        hdl->DeleteItem(seam_mixer_ins);
        hdl->DeleteItem((uint32_t)seam_mixer_template);
        CHECK(m2_guest_audio_unload_instrument((uint32_t)seam_ins) == M2K_OK);
        CHECK(m2_guest_audio_unload_instrument((uint32_t)seam_delay) == M2K_OK);
        teardown();
    }

    // ---- Numeric consumed generation (raw DSPP DMA) ----
    // Vendor dspp_simulator.c dspsReadFIFOtoDSPP (one successful DSPP-side
    // read drains one word; Consumed pending only when the drain empties the
    // FIFO while ChannelComplete is set; dspsReadFIFOtoDMA and CURRENT/NEXT
    // peeks never consume) + dspsProcessNextDMA/DMANext gating (already
    // covered). Pre-fix this block fails (undefined
    // dspp_sim_dma_numeric_consumed); post-fix it passes. No disc
    // dependency: synthetic SLEEP template on a fresh shared space. The
    // counter is generation only: no interrupt/signal/completion is raised
    // and live current_/host buffers are untouched (delivery stays a
    // separate folio seam).
    {
        dspp_template consumed_template = {0};
        consumed_template.code_word_count = 1;
        consumed_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *consumed_space = dspp_sim_space_create();
        dspp_sim *consumed_a =
            dspp_sim_create_in_space(&consumed_template, consumed_space);
        dspp_sim *consumed_b =
            dspp_sim_create_in_space(&consumed_template, consumed_space);
        CHECK(consumed_space != NULL && consumed_a != NULL &&
              consumed_b != NULL);
        if (consumed_a && consumed_b) {
            const uint32_t cch = 9u;
            const uint32_t cdata = 0x0300u + cch * 8u + 4u;
            const uint32_t ccur = 0x0300u + cch * 8u + 0u;
            const uint32_t cnext = 0x0300u + cch * 8u + 1u;
            // No fabrication: unattached, sibling, bad-channel, and NULL
            // queries all read zero.
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 0u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch + 1u) == 0u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, 32u) == 0u);
            CHECK(dspp_sim_dma_numeric_consumed(NULL, cch) == 0u);
            // Attach binds; the fresh generation starts at zero.
            CHECK(dspp_sim_dma_attach_numeric(consumed_a, cch, 0x1000u, 8u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 0u);
            // Feed 4 words through the shared FIFO from one sim.
            dspp_sim_fifo_push(consumed_a, cch, 0x1111);
            dspp_sim_fifo_push(consumed_a, cch, 0x2222);
            dspp_sim_fifo_push(consumed_a, cch, 0x3333);
            dspp_sim_fifo_push(consumed_a, cch, 0x4444);
            // CURRENT/NEXT peeks observe without draining: no advance.
            CHECK(dspp_sim_dspi_read(consumed_b, ccur) == 0x1111u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 0u);
            CHECK(dspp_sim_dspi_read(consumed_b, cnext) == 0x2222u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 0u);
            // Drain N words via the existing DSPI helper; the generation
            // advances exactly N and is visible from either shared sim.
            CHECK(dspp_sim_dspi_read(consumed_b, cdata) == 0x1111u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 1u);
            CHECK(dspp_sim_dspi_read(consumed_a, cdata) == 0x2222u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 2u);
            CHECK(dspp_sim_dspi_read(consumed_b, cdata) == 0x3333u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 3u);
            CHECK(dspp_sim_dspi_read(consumed_b, cdata) == 0x4444u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 4u);
            // Underflow repeats the previous word without advancing, raises
            // no Consumed (complete is never set by a numeric bind), and the
            // host-buffer service stays idle: no fabrication.
            CHECK(dspp_sim_dspi_read(consumed_a, cdata) == 0x4444u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 4u);
            CHECK(dspp_sim_dma_register_read(consumed_b, 0x64020u) == 0u);
            CHECK(dspp_sim_dma_step(consumed_b) == -1);
            // A failed re-attach leaves the generation undisturbed.
            CHECK(dspp_sim_dma_attach_numeric(consumed_b, cch, 0x100u, 1u,
                                              0x08u) ==
                  DSPP_NUMERIC_ERR_BAD_TYPE);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 4u);
            // Re-attaching replaces the record and resets the generation.
            CHECK(dspp_sim_dma_attach_numeric(consumed_a, cch, 0x2000u, 4u,
                                              DSPP_SIM_DMA_TO_RAM) ==
                  DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 0u);
            // The reset generation counts anew.
            dspp_sim_fifo_push(consumed_b, cch, 0x1234);
            dspp_sim_fifo_push(consumed_b, cch, 0x5678);
            CHECK(dspp_sim_dspi_read(consumed_a, cdata) == 0x1234u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch) == 1u);
            CHECK(dspp_sim_dspi_read(consumed_a, cdata) == 0x5678u);
            CHECK(dspp_sim_dma_numeric_consumed(consumed_b, cch) == 2u);
            // Sibling channel never attached still reads zero.
            CHECK(dspp_sim_dma_numeric_consumed(consumed_a, cch + 1u) == 0u);
        }
        dspp_sim_destroy(consumed_b);
        dspp_sim_destroy(consumed_a);
        dspp_sim_space_destroy(consumed_space);
    }
    // ---- Folio numeric-consumed seam (dspp_sim_dma_numeric_consumed wiring) ----
    // Mirrors the m2_guest_audio_read_numeric_descriptor order: zero the
    // out-param, BAD_ITEM on dead/wrong-type handles, UNIMPLEMENTED when no
    // sim was ever built (the read never builds one), hook errors pass
    // through, else the loader generation with M2K_OK. Observation only: no
    // signals raised, no delivery touched. Disc-backed like the attach seam:
    // sampler_16_f2.dsp carries the single IN_FIFO "InFIFO".
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_read_numeric_consumed(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t *generation_out);
        setup();
        const uint32_t cons_samp_ea = 0xB200u;
        const uint32_t cons_hook_ea = 0xB240u;
        const uint32_t cons_bad_ea = 0xB260u;
        write_guest_string(cons_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(cons_hook_ea, "InFIFO");
        write_guest_string(cons_bad_ea, "NoSuchHook");
        int32_t cons_ins = m2_guest_audio_load_instrument(&g_cpu, cons_samp_ea, 1, 100);
        CHECK(cons_ins > 0);
        uint32_t cons_gen = 0xdeadbeefu;
        // Dead and wrong-typed handles report BAD_ITEM with a zeroed output.
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, 0u, cons_hook_ea, &cons_gen) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(cons_gen == 0u);
        uint32_t cons_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
        CHECK(cons_cue != M2K_ITEM_NONE);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, cons_cue, cons_hook_ea, &cons_gen) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(cons_gen == 0u);
        // Pure-synth stays UNIMPLEMENTED: a mixer-template instrument builds
        // no DSP image, so there is no DMA hardware to query.
        int32_t cons_mixer_template = m2_guest_audio_create_mixer_template(&g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(cons_mixer_template > 0);
        uint32_t cons_mixer_ins = hdl->CreateItemVA(M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)cons_mixer_template, M2_TAG_JUMP, 0, 0, 0, 0);
        CHECK(cons_mixer_ins != M2K_ITEM_NONE);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, cons_mixer_ins, cons_hook_ea, &cons_gen) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(cons_gen == 0u);
        // Observation never builds: a fresh sampler still has no sim, so the
        // read stays UNIMPLEMENTED even on its valid hook.
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, &cons_gen) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(cons_gen == 0u);
        // A failed attach builds the sim but leaves the channel unbound, so
        // the unattached read reports 0 with M2K_OK.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, 0x100u, 1u, 0x08u, 0, 0u) == M2_AUDIO_ERR_BAD_TAG_VALUE);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, &cons_gen) == M2K_OK);
        CHECK(cons_gen == 0u);
        // Hook resolution errors pass through with a zeroed output.
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_bad_ea, &cons_gen) == M2_AUDIO_ERR_NAME_MISSING);
        CHECK(cons_gen == 0u);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, RECOMP_GUEST_MEM + 4u, &cons_gen) == M2_AUDIO_ERR_BAD_PTR);
        CHECK(cons_gen == 0u);
        // NULL out-param is tolerated on the valid path.
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, NULL) == M2K_OK);
        // A valid attach binds a fresh zero generation through the seam, and
        // re-attaching replaces the record (generation stays zero un-drained).
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, 0x1200u, 16u, 0u, 0, 0u) == M2K_OK);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, &cons_gen) == M2K_OK);
        CHECK(cons_gen == 0u);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, 0x2000u, 8u, DSPP_SIM_DMA_TO_RAM, 0, 0u) == M2K_OK);
        cons_gen = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)cons_ins, cons_hook_ea, &cons_gen) == M2K_OK);
        CHECK(cons_gen == 0u);
        hdl->DeleteItem(cons_cue);
        hdl->DeleteItem(cons_mixer_ins);
        hdl->DeleteItem((uint32_t)cons_mixer_template);
        CHECK(m2_guest_audio_unload_instrument((uint32_t)cons_ins) == M2K_OK);
        teardown();
    }

    // ---- Consumed generation drain count (loader helpers) ----
    // The seam above forwards dspp_sim_dma_numeric_consumed verbatim; this
    // small synthetic sequence proves the counter it forwards, reusing the
    // loader helpers: attach, push N words, drain N via the existing DSPI
    // helper, generation advances exactly N. No disc dependency, no signals,
    // no completion.
    {
        dspp_template cons_template = {0};
        cons_template.code_word_count = 1;
        cons_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *cons_space = dspp_sim_space_create();
        dspp_sim *cons_sim =
            dspp_sim_create_in_space(&cons_template, cons_space);
        CHECK(cons_space != NULL && cons_sim != NULL);
        if (cons_sim) {
            const uint32_t cons_ch = 5u;
            const uint32_t cons_data = 0x0300u + cons_ch * 8u + 4u;
            CHECK(dspp_sim_dma_numeric_consumed(cons_sim, cons_ch) == 0u);
            CHECK(dspp_sim_dma_attach_numeric(cons_sim, cons_ch, 0x1000u, 8u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_numeric_consumed(cons_sim, cons_ch) == 0u);
            dspp_sim_fifo_push(cons_sim, cons_ch, 0x3111);
            dspp_sim_fifo_push(cons_sim, cons_ch, 0x3222);
            dspp_sim_fifo_push(cons_sim, cons_ch, 0x3333);
            CHECK(dspp_sim_dspi_read(cons_sim, cons_data) == 0x3111u);
            CHECK(dspp_sim_dma_numeric_consumed(cons_sim, cons_ch) == 1u);
            CHECK(dspp_sim_dspi_read(cons_sim, cons_data) == 0x3222u);
            CHECK(dspp_sim_dma_numeric_consumed(cons_sim, cons_ch) == 2u);
            CHECK(dspp_sim_dspi_read(cons_sim, cons_data) == 0x3333u);
            CHECK(dspp_sim_dma_numeric_consumed(cons_sim, cons_ch) == 3u);
        }
        dspp_sim_destroy(cons_sim);
        dspp_sim_space_destroy(cons_space);
    }
    // ---- Armed DMA-interrupt dispatch (DMANext/Consumed/UnderOver) ----
    // Vendor dspp_simulator.c dspsGenMainInt (armed = pending & enabled) +
    // dspp_irq_bulldog.c dsphHandleDMANEXT (ValidInts = SET & ENABLE, clear
    // after service, daemon-signal delivery separate) + dspp_touch_bulldog.c
    // dsphResetFIFO (clear stale banks before start) +
    // dsphEnable/DisableChannelInterrupt (DMANext enable/disable via the DMA
    // CONTROL shadow bit; no INT_DMANEXT_DISABLE register). Pre-fix this
    // block fails (undefined dspp_sim_dma_armed_interrupts et al.); post-fix
    // it passes. No disc dependency: synthetic SLEEP template on a fresh
    // shared space. Raises go through the existing helpers only (host-buffer
    // DMA service for DMANext, DSPI drain helper for UnderOver, vendor SET
    // register for Consumed — the service path leaves ChannelComplete clear,
    // so SET is the raise site); the dispatch itself raises no signal, Cue,
    // or completion.
    {
        dspp_template irq_template = {0};
        irq_template.code_word_count = 1;
        irq_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *irq_space = dspp_sim_space_create();
        dspp_sim *irq_a =
            dspp_sim_create_in_space(&irq_template, irq_space);
        dspp_sim *irq_b =
            dspp_sim_create_in_space(&irq_template, irq_space);
        CHECK(irq_space != NULL && irq_a != NULL && irq_b != NULL);
        if (irq_a && irq_b) {
            const uint32_t irq_fire = 11u;
            const uint32_t irq_cons = 12u;
            const uint32_t irq_uo = 13u;
            const uint32_t irq_fire_mask = 1u << irq_fire;
            const uint32_t irq_cons_mask = 1u << irq_cons;
            const uint32_t irq_uo_mask = 1u << irq_uo;
            const uint32_t irq_uo_data = 0x0300u + irq_uo * 8u + 4u;
            // No fabrication on a fresh space: raw banks and the armed
            // dispatch all read zero, including through the second shared
            // sim and through NULL.
            CHECK(dspp_sim_dma_register_read(irq_b, 0x64010u) == 0u);
            CHECK(dspp_sim_dma_register_read(irq_b, 0x64020u) == 0u);
            CHECK(dspp_sim_dma_register_read(irq_b, 0x64030u) == 0u);
            {
                dspp_dma_armed irq_fresh =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_fresh.dmanext == 0u &&
                      irq_fresh.consumed == 0u &&
                      irq_fresh.underover == 0u);
                dspp_dma_armed irq_null =
                    dspp_sim_dma_armed_interrupts(NULL);
                CHECK(irq_null.dmanext == 0u && irq_null.consumed == 0u &&
                      irq_null.underover == 0u);
            }
            // NULL enable/disable/clear tolerate (pass = no crash).
            dspp_sim_dma_enable_interrupts(NULL, DSPP_DMA_BANK_ALL,
                                           0xFFFFFFFFu);
            dspp_sim_dma_disable_interrupts(NULL, DSPP_DMA_BANK_ALL,
                                            0xFFFFFFFFu);
            dspp_sim_dma_clear_interrupts(NULL, DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
            // Vendor reset discipline: clear stale banks before raising.
            dspp_sim_dma_clear_interrupts(irq_a, DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
            // Fire DMANext through the existing host-buffer service: current
            // exhausts, the valid next commits, process_next raises pending.
            uint8_t irq_cur[4] = {0x11, 0x22, 0x33, 0x44};
            uint8_t irq_nxt[4] = {0x55, 0x66, 0x77, 0x88};
            CHECK(dspp_sim_dma_set_current(irq_a, irq_fire, irq_cur, 2, 0) ==
                  0);
            CHECK(dspp_sim_dma_set_next(irq_a, irq_fire, irq_nxt, 2) == 0);
            dspp_sim_dma_set_control(irq_a, irq_fire, 1, 0);
            CHECK(dspp_sim_dma_step(irq_b) == (int)irq_fire);
            CHECK((dspp_sim_dma_dmanext_interrupts(irq_b) & irq_fire_mask) !=
                  0u);
            // The fire fabricates nothing outside its bank: Consumed (the
            // service never sets ChannelComplete) and UnderOver stay clear.
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64020u) &
                   irq_cons_mask) == 0u);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64030u) &
                   irq_uo_mask) == 0u);
            // Raise UnderOver through the existing drain helper: a DSPP-side
            // read of an empty FIFO repeats previous and sets the bank.
            CHECK(dspp_sim_dspi_read(irq_b, irq_uo_data) == 0u);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64030u) &
                   irq_uo_mask) != 0u);
            // Enable-off masks everything: both banks pending, armed silent.
            {
                dspp_dma_armed irq_masked =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_masked.dmanext == 0u &&
                      irq_masked.consumed == 0u &&
                      irq_masked.underover == 0u);
            }
            // Enable the two raised banks; the dispatch reports exactly the
            // raised channels through the second shared sim, with the helper
            // landing in the vendor-visible enable banks.
            dspp_sim_dma_enable_interrupts(irq_a, DSPP_DMA_BANK_DMANEXT |
                                                       DSPP_DMA_BANK_UNDEROVER,
                                           irq_fire_mask | irq_uo_mask);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64018u) &
                   irq_fire_mask) != 0u);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64038u) &
                   irq_uo_mask) != 0u);
            {
                dspp_dma_armed irq_armed =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_armed.dmanext == irq_fire_mask);
                CHECK(irq_armed.underover == irq_uo_mask);
                CHECK(irq_armed.consumed == 0u);
            }
            // An enabled-but-unraised bank stays zero: enables alone never
            // fabricate pending for unbuilt/unattached channels.
            dspp_sim_dma_enable_interrupts(irq_a, DSPP_DMA_BANK_CONSUMED,
                                           irq_cons_mask);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).consumed == 0u);
            // Raise Consumed through the vendor SET register (the service
            // path leaves ChannelComplete clear, so SET is the raise site);
            // it lands only in its bank.
            dspp_sim_dma_register_write(irq_a, 0x64020u, irq_cons_mask);
            {
                dspp_dma_armed irq_all =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_all.dmanext == irq_fire_mask);
                CHECK(irq_all.consumed == irq_cons_mask);
                CHECK(irq_all.underover == irq_uo_mask);
            }
            // Disable masks without clearing, per bank: armed goes silent,
            // raw pending retained, re-enable restores. DMANext disable takes
            // the shadow-control clear effect on the shared enable word (the
            // vendor ships no INT_DMANEXT_DISABLE register); Consumed and
            // UnderOver go through their DISABLE registers' word.
            dspp_sim_dma_disable_interrupts(irq_a, DSPP_DMA_BANK_DMANEXT,
                                            irq_fire_mask);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64018u) &
                   irq_fire_mask) == 0u);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).dmanext == 0u);
            CHECK((dspp_sim_dma_dmanext_interrupts(irq_b) & irq_fire_mask) !=
                  0u);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).consumed ==
                  irq_cons_mask);
            dspp_sim_dma_enable_interrupts(irq_a, DSPP_DMA_BANK_DMANEXT,
                                           irq_fire_mask);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).dmanext ==
                  irq_fire_mask);
            dspp_sim_dma_disable_interrupts(irq_a, DSPP_DMA_BANK_CONSUMED,
                                            irq_cons_mask);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64028u) &
                   irq_cons_mask) == 0u);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).consumed == 0u);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64020u) &
                   irq_cons_mask) != 0u);
            dspp_sim_dma_enable_interrupts(irq_a, DSPP_DMA_BANK_CONSUMED,
                                           irq_cons_mask);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).consumed ==
                  irq_cons_mask);
            dspp_sim_dma_disable_interrupts(irq_a, DSPP_DMA_BANK_UNDEROVER,
                                            irq_uo_mask);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).underover == 0u);
            dspp_sim_dma_enable_interrupts(irq_a, DSPP_DMA_BANK_UNDEROVER,
                                           irq_uo_mask);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).underover ==
                  irq_uo_mask);
            // Clear drops pending per bank: DMANext+UnderOver go silent in
            // both raw and armed views while Consumed is retained.
            dspp_sim_dma_clear_interrupts(irq_a, DSPP_DMA_BANK_DMANEXT |
                                                     DSPP_DMA_BANK_UNDEROVER,
                                          irq_fire_mask | irq_uo_mask);
            CHECK((dspp_sim_dma_dmanext_interrupts(irq_b) & irq_fire_mask) ==
                  0u);
            CHECK((dspp_sim_dma_register_read(irq_b, 0x64030u) &
                   irq_uo_mask) == 0u);
            {
                dspp_dma_armed irq_cleared =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_cleared.dmanext == 0u &&
                      irq_cleared.underover == 0u);
                CHECK(irq_cleared.consumed == irq_cons_mask);
            }
            // Clear-then-raise ordering: enables survive the clear, so a
            // fresh underflow reappears with no re-enable.
            CHECK(dspp_sim_dspi_read(irq_a, irq_uo_data) == 0u);
            CHECK(dspp_sim_dma_armed_interrupts(irq_b).underover ==
                  irq_uo_mask);
            // Unknown bank bits are ignored; the dispatch is undisturbed.
            dspp_sim_dma_enable_interrupts(irq_a, 0x08u, 0xFFFFFFFFu);
            dspp_sim_dma_disable_interrupts(irq_a, 0xF0u, 0xFFFFFFFFu);
            dspp_sim_dma_clear_interrupts(irq_a,
                                          0xFFu & (uint32_t)~DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
            {
                dspp_dma_armed irq_kept =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_kept.underover == irq_uo_mask);
                CHECK(irq_kept.consumed == irq_cons_mask);
            }
            dspp_sim_dma_clear_interrupts(irq_a, DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
            {
                dspp_dma_armed irq_done =
                    dspp_sim_dma_armed_interrupts(irq_b);
                CHECK(irq_done.dmanext == 0u && irq_done.consumed == 0u &&
                      irq_done.underover == 0u);
            }
        }
        dspp_sim_destroy(irq_b);
        dspp_sim_destroy(irq_a);
        dspp_sim_space_destroy(irq_space);
    }

    // ---- Folio DMA-armed-interrupt read seam (dspp_sim_dma_armed_interrupts wiring) ----
    // Mirrors the m2_guest_audio_read_numeric_consumed order: zero the out-params,
    // BAD_ITEM on dead/wrong-type handles, UNIMPLEMENTED when no sim was ever built
    // (the read never builds one), hook errors pass through, else the loader's armed
    // banks with M2K_OK. Observation only: no signals raised, no delivery touched.
    // Disc-backed like the consumed seam: sampler_16_f2.dsp carries the single
    // IN_FIFO "InFIFO", so hook EA 0 and the named hook agree.
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_read_dma_armed(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t *dmanext_out, uint32_t *consumed_out, uint32_t *underover_out);
        setup();
        const uint32_t armed_samp_ea = 0xB300u;
        const uint32_t armed_hook_ea = 0xB340u;
        const uint32_t armed_bad_ea = 0xB360u;
        write_guest_string(armed_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(armed_hook_ea, "InFIFO");
        write_guest_string(armed_bad_ea, "NoSuchHook");
        int32_t armed_ins = m2_guest_audio_load_instrument(&g_cpu, armed_samp_ea, 1, 100);
        CHECK(armed_ins > 0);
        uint32_t armed_next = 0xdeadbeefu;
        uint32_t armed_cons = 0xdeadbeefu;
        uint32_t armed_uo = 0xdeadbeefu;
        // Dead and wrong-typed handles report BAD_ITEM with zeroed outputs.
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, 0u, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        uint32_t armed_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
        CHECK(armed_cue != M2K_ITEM_NONE);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, armed_cue, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // Pure-synth stays UNIMPLEMENTED: a mixer-template instrument builds
        // no DSP image, so there is no DMA hardware to query.
        int32_t armed_mixer_template = m2_guest_audio_create_mixer_template(&g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(armed_mixer_template > 0);
        uint32_t armed_mixer_ins = hdl->CreateItemVA(M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)armed_mixer_template, M2_TAG_JUMP, 0, 0, 0, 0);
        CHECK(armed_mixer_ins != M2K_ITEM_NONE);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, armed_mixer_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // Observation never builds: a fresh sampler still has no sim, so the
        // read stays UNIMPLEMENTED even on its valid hook.
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // A failed attach builds the sim but raises nothing: fresh banks read
        // zero through both the default (NULL) hook and the named single FIFO.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, 0x100u, 1u, 0x08u, 0, 0u) == M2_AUDIO_ERR_BAD_TAG_VALUE);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, 0u, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // A successful production bind still arms nothing: the descriptor
        // records address/count only (banks untouched). The folio interrupt
        // init enables the DMANext main bit and the FIRQ dispatch is wired
        // (loader block below); only per-channel/second-level events raise
        // the armed query, and a bind raises none.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, 0x1200u, 2u, 0u, 0, 0u) == M2K_OK);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, 0u, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // Hook resolution errors pass through with zeroed outputs.
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_bad_ea, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_NAME_MISSING);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, RECOMP_GUEST_MEM + 4u, &armed_next, &armed_cons, &armed_uo) == M2_AUDIO_ERR_BAD_PTR);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        // NULL out-params are tolerated on the valid path.
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, NULL, NULL, NULL) == M2K_OK);
        // Starting seals the channels but fabricates no banks: the started read
        // stays zero, a late attach waits with NO_MEMORY, and the query stays
        // observational throughout.
        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)armed_ins, 0) == M2K_OK);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, 0x100u, 1u, 0u, 0, 0u) == M2_AUDIO_ERR_NO_MEMORY);
        armed_next = armed_cons = armed_uo = 0xdeadbeefu;
        CHECK(m2_guest_audio_read_dma_armed(&g_cpu, (uint32_t)armed_ins, armed_hook_ea, &armed_next, &armed_cons, &armed_uo) == M2K_OK);
        CHECK(armed_next == 0u && armed_cons == 0u && armed_uo == 0u);
        hdl->DeleteItem(armed_cue);
        hdl->DeleteItem(armed_mixer_ins);
        hdl->DeleteItem((uint32_t)armed_mixer_template);
        CHECK(m2_guest_audio_unload_instrument((uint32_t)armed_ins) == M2K_OK);
        teardown();
    }
    // ---- Armed-query loader half (what the seam forwards) ----
    // Compact synthetic proof that the forwarded query reports a serviced DMANext
    // with per-bank enable masking: fresh zeros; host-buffer service raises DMANext
    // pending while enable-off keeps it masked; enable reports exactly the fired
    // channel with sibling banks silent; per-bank disable re-masks without clearing;
    // clear drops pending so a re-enable stays silent (no fabrication).
    {
        dspp_template armed_template = {0};
        armed_template.code_word_count = 1;
        armed_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *armed_space = dspp_sim_space_create();
        dspp_sim *armed_a =
            dspp_sim_create_in_space(&armed_template, armed_space);
        dspp_sim *armed_b =
            dspp_sim_create_in_space(&armed_template, armed_space);
        CHECK(armed_space != NULL && armed_a != NULL && armed_b != NULL);
        if (armed_a && armed_b) {
            const uint32_t armed_fire = 7u;
            const uint32_t armed_fire_mask = 1u << armed_fire;
            {
                dspp_dma_armed armed_fresh =
                    dspp_sim_dma_armed_interrupts(armed_b);
                CHECK(armed_fresh.dmanext == 0u &&
                      armed_fresh.consumed == 0u &&
                      armed_fresh.underover == 0u);
            }
            dspp_sim_dma_clear_interrupts(armed_a, DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
            uint8_t armed_cur[4] = {0x11, 0x22, 0x33, 0x44};
            uint8_t armed_nxt[4] = {0x55, 0x66, 0x77, 0x88};
            CHECK(dspp_sim_dma_set_current(armed_a, armed_fire, armed_cur, 2,
                                           0) == 0);
            CHECK(dspp_sim_dma_set_next(armed_a, armed_fire, armed_nxt, 2) == 0);
            dspp_sim_dma_set_control(armed_a, armed_fire, 1, 0);
            CHECK(dspp_sim_dma_step(armed_b) == (int)armed_fire);
            CHECK((dspp_sim_dma_dmanext_interrupts(armed_b) &
                   armed_fire_mask) != 0u);
            CHECK(dspp_sim_dma_armed_interrupts(armed_b).dmanext == 0u);
            dspp_sim_dma_enable_interrupts(armed_a, DSPP_DMA_BANK_DMANEXT,
                                           armed_fire_mask);
            {
                dspp_dma_armed armed_on =
                    dspp_sim_dma_armed_interrupts(armed_b);
                CHECK(armed_on.dmanext == armed_fire_mask);
                CHECK(armed_on.consumed == 0u && armed_on.underover == 0u);
            }
            dspp_sim_dma_disable_interrupts(armed_a, DSPP_DMA_BANK_DMANEXT,
                                            armed_fire_mask);
            CHECK(dspp_sim_dma_armed_interrupts(armed_b).dmanext == 0u);
            CHECK((dspp_sim_dma_dmanext_interrupts(armed_b) &
                   armed_fire_mask) != 0u);
            dspp_sim_dma_clear_interrupts(armed_a, DSPP_DMA_BANK_DMANEXT,
                                          armed_fire_mask);
            dspp_sim_dma_enable_interrupts(armed_a, DSPP_DMA_BANK_DMANEXT,
                                           armed_fire_mask);
            CHECK(dspp_sim_dma_armed_interrupts(armed_b).dmanext == 0u);
            dspp_sim_dma_clear_interrupts(armed_a, DSPP_DMA_BANK_ALL,
                                          0xFFFFFFFFu);
        }
        dspp_sim_destroy(armed_b);
        dspp_sim_destroy(armed_a);
        dspp_sim_space_destroy(armed_space);
    }
    // ---- DSPP FIRQ first-level dispatch + folio interrupt init ----
    // Vendor ground: dspp_irq_bulldog.c dsphFIRQHandler (validInts =
    // SET & ENABLE; DMANext -> timer -> soft precedence), dsphHandleDMANEXT
    // (second-level SET & ENABLE, clear-after-service, all-done channel
    // disable), dsphHandleTimer (clear), dsphHandleSoftInt (disable received
    // bits; daemon dsphGetReceivedTriggers reads/clears), dspp_touch_bulldog.c
    // dsphInitInterruptRegs + InitDSPPInterrupt (clear banks; enable DMANext).
    // Pre-fix this block fails (undefined dspp_sim_dma_firq_dispatch et al.);
    // post-fix it passes. No disc dependency: synthetic SLEEP template on a
    // fresh shared space. No signal, Cue, or completion is raised here.
    {
        dspp_template firq_template = {0};
        firq_template.code_word_count = 1;
        firq_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *firq_space = dspp_sim_space_create();
        CHECK(firq_space != NULL);
        // Folio/device init equivalent (dsphInitInterruptRegs +
        // InitDSPPInterrupt): banks cleared, DMANext enabled in the main bank.
        dspp_sim_space_init_folio_interrupts(firq_space);
        dspp_sim *firq_a = dspp_sim_create_in_space(&firq_template, firq_space);
        dspp_sim *firq_b = dspp_sim_create_in_space(&firq_template, firq_space);
        CHECK(firq_a != NULL && firq_b != NULL);
        if (firq_a && firq_b) {
            CHECK(dspp_sim_dma_main_interrupt_enable(firq_b) ==
                  DSPP_MAIN_INT_DMANEXT);
            CHECK(dspp_sim_dma_main_interrupt(firq_b) == 0u);
            // Fresh space fabricates nothing, including through NULLs.
            {
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.dmanext == 0u && d.timer == 0 && d.soft == 0u);
                d = dspp_sim_dma_firq_dispatch(NULL);
                CHECK(d.dmanext == 0u && d.timer == 0 && d.soft == 0u);
            }
            CHECK(dspp_sim_dma_main_interrupt(NULL) == 0u);
            CHECK(dspp_sim_dma_main_interrupt_enable(NULL) == 0u);
            CHECK(dspp_sim_dma_raise_timer_interrupt(NULL) == 0);
            CHECK(dspp_sim_dma_channel_interrupt_enabled(NULL, 0u) == 0);
            dspp_sim_dma_set_channel_interrupt(NULL, 0u, 1); // tolerated
            // ---- DMANext second-level service ----
            const uint32_t firq_ch = 3u;
            const uint32_t firq_ch_mask = 1u << firq_ch;
            uint8_t firq_cur[4] = {0x11, 0x22, 0x33, 0x44};
            uint8_t firq_nxt[4] = {0x55, 0x66, 0x77, 0x88};
            // Host-buffer service raises the second-level pending bit.
            dspp_sim_dma_set_channel_interrupt(firq_a, firq_ch, 1);
            CHECK(dspp_sim_dma_channel_interrupt_enabled(firq_b, firq_ch) == 1);
            CHECK(dspp_sim_dma_set_current(firq_a, firq_ch, firq_cur, 2, 0) ==
                  0);
            CHECK(dspp_sim_dma_set_next(firq_a, firq_ch, firq_nxt, 2) == 0);
            dspp_sim_dma_set_control(firq_a, firq_ch, 1, 0);
            CHECK(dspp_sim_dma_step(firq_b) == (int)firq_ch);
            CHECK((dspp_sim_dma_dmanext_interrupts(firq_b) & firq_ch_mask) !=
                  0u);
            CHECK((dspp_sim_dma_main_interrupt(firq_b) &
                   DSPP_MAIN_INT_DMANEXT) != 0u);
            {
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.dmanext == firq_ch_mask);
                CHECK(d.timer == 0 && d.soft == 0u);
            }
            // Clear-after-service + vendor all-done channel disable.
            CHECK(dspp_sim_dma_dmanext_interrupts(firq_b) == 0u);
            CHECK(dspp_sim_dma_channel_interrupt_enabled(firq_b, firq_ch) == 0);
            // A fresh pending event with the channel enable off is masked by
            // the first-level ValidInts gate (vendor enable-gated dispatch).
            CHECK(dspp_sim_dma_set_current(firq_a, firq_ch, firq_cur, 2, 0) ==
                  0);
            CHECK(dspp_sim_dma_set_next(firq_a, firq_ch, firq_nxt, 2) == 0);
            dspp_sim_dma_set_control(firq_a, firq_ch, 1, 0);
            CHECK(dspp_sim_dma_step(firq_b) == (int)firq_ch);
            CHECK((dspp_sim_dma_dmanext_interrupts(firq_b) & firq_ch_mask) !=
                  0u);
            CHECK(dspp_sim_dma_firq_dispatch(firq_b).dmanext == 0u);
            dspp_sim_dma_clear_interrupts(firq_b, DSPP_DMA_BANK_DMANEXT,
                                          firq_ch_mask);
            // Re-arm and re-raise: the dispatch services the new event.
            dspp_sim_dma_set_channel_interrupt(firq_a, firq_ch, 1);
            CHECK(dspp_sim_dma_set_current(firq_a, firq_ch, firq_cur, 2, 0) ==
                  0);
            CHECK(dspp_sim_dma_set_next(firq_a, firq_ch, firq_nxt, 2) == 0);
            dspp_sim_dma_set_control(firq_a, firq_ch, 1, 0);
            CHECK(dspp_sim_dma_step(firq_b) == (int)firq_ch);
            CHECK(dspp_sim_dma_firq_dispatch(firq_b).dmanext == firq_ch_mask);
            CHECK(dspp_sim_dma_channel_interrupt_enabled(firq_b, firq_ch) == 0);
            // ---- Timer: level raise + folio enable, single clear ----
            // Vendor dsppSetWakeupFrame enables the timer bit after
            // programming the wakeup; dsphHandleTimer clears the pending bit
            // and leaves the enable set for the next wakeup.
            CHECK(dspp_sim_dma_raise_timer_interrupt(firq_b) == 1);
            CHECK(dspp_sim_dma_raise_timer_interrupt(firq_b) == 0);
            dspp_sim_dma_register_write(firq_a, 0x64008u,
                                        DSPP_MAIN_INT_TIMER);
            CHECK((dspp_sim_dma_main_interrupt(firq_b) &
                   DSPP_MAIN_INT_TIMER) != 0u);
            {
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.timer == 1 && d.dmanext == 0u && d.soft == 0u);
            }
            CHECK((dspp_sim_dma_main_interrupt(firq_b) &
                   DSPP_MAIN_INT_TIMER) == 0u);
            CHECK((dspp_sim_dma_register_read(firq_b, 0x64008u) &
                   DSPP_MAIN_INT_TIMER) != 0u); // enable retained
            // ---- Soft int (trigger) : enable-gated, disable-on-receive ----
            const uint32_t firq_trig = 5u;
            const uint32_t firq_trig_soft =
                (1u << firq_trig) << DSPP_MAIN_INT_SOFT_SHIFT;
            dspp_sim_dspi_write(firq_a, 0x03E6u, (uint16_t)(1u << firq_trig));
            CHECK((dspp_sim_dma_main_interrupt(firq_b) & firq_trig_soft) != 0u);
            {
                // Pending but not enabled: not valid, not dispatched.
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.soft == 0u);
            }
            // Arm (dsphEnableTriggerInterrupts); the same pending bit is now
            // valid and the FIRQ disables exactly the received enable bit.
            dspp_sim_dma_register_write(firq_a, 0x64008u, firq_trig_soft);
            {
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.soft == firq_trig_soft);
            }
            CHECK((dspp_sim_dma_register_read(firq_b, 0x64008u) &
                   firq_trig_soft) == 0u);
            uint32_t soft_pending =
                dspp_sim_dma_register_read(firq_b, 0x64000u) &
                DSPP_MAIN_INT_SOFT_MASK;
            CHECK(soft_pending == firq_trig_soft);
            // Daemon dsphGetReceivedTriggers: read SET, clear exactly those.
            dspp_sim_dma_register_write(firq_b, 0x64004u, soft_pending);
            CHECK((dspp_sim_dma_register_read(firq_b, 0x64000u) &
                   DSPP_MAIN_INT_SOFT_MASK) == 0u);
            // ---- Precedence in one pass: DMANext + timer + soft together ----
            const uint32_t firq_ch2 = 9u;
            const uint32_t firq_ch2_mask = 1u << firq_ch2;
            dspp_sim_dma_set_channel_interrupt(firq_a, firq_ch2, 1);
            CHECK(dspp_sim_dma_set_current(firq_a, firq_ch2, firq_cur, 2, 0) ==
                  0);
            CHECK(dspp_sim_dma_set_next(firq_a, firq_ch2, firq_nxt, 2) == 0);
            dspp_sim_dma_set_control(firq_a, firq_ch2, 1, 0);
            CHECK(dspp_sim_dma_step(firq_b) == (int)firq_ch2);
            CHECK(dspp_sim_dma_raise_timer_interrupt(firq_b) == 1);
            dspp_sim_dspi_write(firq_a, 0x03E6u, (uint16_t)(1u << firq_trig));
            dspp_sim_dma_register_write(firq_a, 0x64008u, firq_trig_soft);
            {
                dspp_firq_dispatch d = dspp_sim_dma_firq_dispatch(firq_b);
                CHECK(d.dmanext == firq_ch2_mask);
                CHECK(d.timer == 1);
                CHECK(d.soft == firq_trig_soft);
            }
            CHECK(dspp_sim_dma_dmanext_interrupts(firq_b) == 0u);
            CHECK(dspp_sim_dma_channel_interrupt_enabled(firq_b, firq_ch2) ==
                  0);
            CHECK((dspp_sim_dma_main_interrupt(firq_b) &
                   DSPP_MAIN_INT_TIMER) == 0u);
            CHECK((dspp_sim_dma_register_read(firq_b, 0x64008u) &
                   firq_trig_soft) == 0u);
        }
        dspp_sim_destroy(firq_b);
        dspp_sim_destroy(firq_a);
        dspp_sim_space_destroy(firq_space);
    }
    // ---- Completion-qualified event record (raw DSPP DMA) ----
    // Vendor audio_attachments.c ProcessAttachmentDMA (SegmentCount reaches 0
    // -> SignalMonitoringCue) + dspp_simulator.c dspsReadFIFOtoDSPP
    // (Consumed only on drain-to-empty while ChannelComplete). Pre-fix this
    // block fails (undefined dspp_sim_dma_poll_completion et al.); post-fix
    // it passes. No disc dependency: synthetic SLEEP template on a fresh
    // shared space. The record is data only: no interrupt bank, signal, or
    // Cue is raised; the folio MonitorAttachment pump seam owns delivery.
    {
        dspp_template done_template = {0};
        done_template.code_word_count = 1;
        done_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *done_space = dspp_sim_space_create();
        dspp_sim *done_a =
            dspp_sim_create_in_space(&done_template, done_space);
        dspp_sim *done_b =
            dspp_sim_create_in_space(&done_template, done_space);
        CHECK(done_space != NULL && done_a != NULL && done_b != NULL);
        if (done_a && done_b) {
            const uint32_t dch = 4u;
            const uint32_t ddata = 0x0300u + dch * 8u + 4u;
            const uint32_t ech = 6u;
            const uint32_t edata = 0x0300u + ech * 8u + 4u;
            uint32_t polled = 0xdeadbeefu;
            // Fresh space fabricates nothing: empty queue, no pending,
            // including through the second shared sim, bad channels, NULL.
            CHECK(dspp_sim_dma_completion_queued(done_b) == 0u);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 0);
            CHECK(dspp_sim_dma_completion_pending(done_b, 32u) == 0);
            CHECK(dspp_sim_dma_completion_pending(NULL, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(NULL) == 0u);
            CHECK(dspp_sim_dma_poll_completion(done_b, &polled) == 0);
            CHECK(polled == 0u);
            CHECK(dspp_sim_dma_poll_completion(NULL, &polled) == 0);
            CHECK(polled == 0u);
            // NULL out-param on the empty path is tolerated.
            CHECK(dspp_sim_dma_poll_completion(done_a, NULL) == 0);
            // Unattached drains never complete: feed and drain with no bind.
            dspp_sim_fifo_push(done_a, dch, 0x1111);
            dspp_sim_fifo_push(done_a, dch, 0x2222);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x1111u);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x2222u);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_b) == 0u);
            CHECK(dspp_sim_dma_poll_completion(done_b, &polled) == 0);
            // Bind count N=3: draining N-1 words queues nothing.
            CHECK(dspp_sim_dma_attach_numeric(done_a, dch, 0x1000u, 3u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 0);
            dspp_sim_fifo_push(done_a, dch, 0x3111);
            dspp_sim_fifo_push(done_a, dch, 0x3222);
            dspp_sim_fifo_push(done_a, dch, 0x3333);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x3111u);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 0);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x3222u);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_a) == 0u);
            CHECK(dspp_sim_dma_poll_completion(done_a, &polled) == 0);
            // The Nth drain records exactly one event, visible from either
            // shared sim, with no bank/signal side effect.
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x3333u);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 1);
            CHECK(dspp_sim_dma_completion_queued(done_a) == 1u);
            CHECK(dspp_sim_dma_register_read(done_b, 0x64020u) == 0u);
            CHECK(dspp_sim_dma_step(done_b) == -1);
            // Pending once: one poll takes it, the next is empty (zeroed).
            CHECK(dspp_sim_dma_poll_completion(done_b, &polled) == 1);
            CHECK(polled == dch);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_b) == 0u);
            polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_poll_completion(done_a, &polled) == 0);
            CHECK(polled == 0u);
            // Once per bind: post-poll over-drains never re-queue.
            dspp_sim_fifo_push(done_b, dch, 0x4444);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x4444u);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_a) == 0u);
            // count == 0 never completes, however much drains.
            CHECK(dspp_sim_dma_attach_numeric(done_a, ech, 0u, 0u,
                                              0u) == DSPP_NUMERIC_OK);
            dspp_sim_fifo_push(done_b, ech, 0x5001);
            dspp_sim_fifo_push(done_b, ech, 0x5002);
            dspp_sim_fifo_push(done_b, ech, 0x5003);
            dspp_sim_fifo_push(done_b, ech, 0x5004);
            dspp_sim_fifo_push(done_b, ech, 0x5005);
            CHECK(dspp_sim_dspi_read(done_a, edata) == 0x5001u);
            CHECK(dspp_sim_dspi_read(done_a, edata) == 0x5002u);
            CHECK(dspp_sim_dspi_read(done_a, edata) == 0x5003u);
            CHECK(dspp_sim_dspi_read(done_a, edata) == 0x5004u);
            CHECK(dspp_sim_dspi_read(done_a, edata) == 0x5005u);
            CHECK(dspp_sim_dma_completion_pending(done_a, ech) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_b) == 0u);
            // Re-attach clears: bind count 2, drain to completion, re-attach
            // before polling -> pending gone, queue empty, generation reset.
            CHECK(dspp_sim_dma_attach_numeric(done_b, dch, 0x2000u, 2u,
                                              0u) == DSPP_NUMERIC_OK);
            dspp_sim_fifo_push(done_a, dch, 0x6111);
            dspp_sim_fifo_push(done_a, dch, 0x6222);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x6111u);
            CHECK(dspp_sim_dspi_read(done_b, ddata) == 0x6222u);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 1);
            CHECK(dspp_sim_dma_completion_queued(done_a) == 1u);
            CHECK(dspp_sim_dma_attach_numeric(done_a, dch, 0x3000u, 4u,
                                              0u) == DSPP_NUMERIC_OK);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 0);
            CHECK(dspp_sim_dma_completion_queued(done_b) == 0u);
            CHECK(dspp_sim_dma_numeric_consumed(done_b, dch) == 0u);
            polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_poll_completion(done_b, &polled) == 0);
            CHECK(polled == 0u);
            // The new bound completes anew: 3 drains queue nothing, 4th does.
            dspp_sim_fifo_push(done_b, dch, 0x7111);
            dspp_sim_fifo_push(done_b, dch, 0x7222);
            dspp_sim_fifo_push(done_b, dch, 0x7333);
            dspp_sim_fifo_push(done_b, dch, 0x7444);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x7111u);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x7222u);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x7333u);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 0);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x7444u);
            CHECK(dspp_sim_dma_completion_pending(done_b, dch) == 1);
            polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_poll_completion(done_a, &polled) == 1);
            CHECK(polled == dch);
            CHECK(dspp_sim_dma_completion_queued(done_a) == 0u);
            // A failed re-attach leaves the lifecycle undisturbed: bind
            // count 1, drain to completion, fail a re-attach, event stays.
            CHECK(dspp_sim_dma_attach_numeric(done_a, dch, 0x4000u, 1u,
                                              0u) == DSPP_NUMERIC_OK);
            dspp_sim_fifo_push(done_b, dch, 0x7511);
            CHECK(dspp_sim_dspi_read(done_a, ddata) == 0x7511u);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 1);
            CHECK(dspp_sim_dma_attach_numeric(done_b, dch, 0x100u, 1u,
                                              0x08u) ==
                  DSPP_NUMERIC_ERR_BAD_TYPE);
            CHECK(dspp_sim_dma_completion_pending(done_a, dch) == 1);
            CHECK(dspp_sim_dma_completion_queued(done_b) == 1u);
            polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_poll_completion(done_b, &polled) == 1);
            CHECK(polled == dch);
        }
        dspp_sim_destroy(done_b);
        dspp_sim_destroy(done_a);
        dspp_sim_space_destroy(done_space);
    }
    // ---- Completion queue bound (all 32 channels) ----
    // The queue is deduped per channel against a 32-entry cap, so completing
    // every channel bounds the depth at DSPP_COMPLETION_QUEUE_CAP with FIFO
    // order and no duplicates; extra drains never grow it. No signals raised.
    {
        dspp_template cap_template = {0};
        cap_template.code_word_count = 1;
        cap_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim_space *cap_space = dspp_sim_space_create();
        dspp_sim *cap_sim =
            dspp_sim_create_in_space(&cap_template, cap_space);
        CHECK(cap_space != NULL && cap_sim != NULL);
        if (cap_sim) {
            uint32_t polled = 0xdeadbeefu;
            uint32_t ch;
            CHECK(DSPP_COMPLETION_QUEUE_CAP == 32u);
            for (ch = 0u; ch < 32u; ++ch) {
                CHECK(dspp_sim_dma_attach_numeric(cap_sim, ch, 0x1000u + ch,
                                                  1u,
                                                  0u) == DSPP_NUMERIC_OK);
                dspp_sim_fifo_push(cap_sim, ch, (int16_t)(0x1000u + ch));
            }
            for (ch = 0u; ch < 32u; ++ch) {
                uint32_t data = 0x0300u + ch * 8u + 4u;
                CHECK(dspp_sim_dspi_read(cap_sim, data) ==
                      (uint16_t)(0x1000u + ch));
            }
            // Bounded at the cap: one event per channel, no duplicates.
            CHECK(dspp_sim_dma_completion_queued(cap_sim) ==
                  DSPP_COMPLETION_QUEUE_CAP);
            // Extra drains on completed channels never grow the queue.
            for (ch = 0u; ch < 32u; ++ch) {
                uint32_t data = 0x0300u + ch * 8u + 4u;
                dspp_sim_fifo_push(cap_sim, ch, 0x7777);
                (void)dspp_sim_dspi_read(cap_sim, data);
            }
            CHECK(dspp_sim_dma_completion_queued(cap_sim) ==
                  DSPP_COMPLETION_QUEUE_CAP);
            // FIFO order: channels dequeue 0..31, then empty (zeroed out).
            for (ch = 0u; ch < 32u; ++ch) {
                polled = 0xdeadbeefu;
                CHECK(dspp_sim_dma_poll_completion(cap_sim, &polled) == 1);
                CHECK(polled == ch);
            }
            CHECK(dspp_sim_dma_completion_queued(cap_sim) == 0u);
            polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_poll_completion(cap_sim, &polled) == 0);
            CHECK(polled == 0u);
            CHECK(dspp_sim_dma_register_read(cap_sim, 0x64020u) == 0u);
        }
        dspp_sim_destroy(cap_sim);
        dspp_sim_space_destroy(cap_space);
    }
    // ---- Completion queue on standalone sims (private queue) ----
    // Sims without a space resolve the same FIFO discipline through private
    // state: attach, drain to bound, poll once, then empty.
    {
        dspp_template solo_template = {0};
        solo_template.code_word_count = 1;
        solo_template.code_words[0] = 0x8380; // SLEEP
        dspp_sim *solo = dspp_sim_create(&solo_template);
        CHECK(solo != NULL);
        if (solo) {
            const uint32_t sch = 2u;
            const uint32_t sdata = 0x0300u + sch * 8u + 4u;
            uint32_t polled = 0xdeadbeefu;
            CHECK(dspp_sim_dma_completion_queued(solo) == 0u);
            CHECK(dspp_sim_dma_attach_numeric(solo, sch, 0x1000u, 2u,
                                              0u) == DSPP_NUMERIC_OK);
            dspp_sim_fifo_push(solo, sch, 0x2111);
            dspp_sim_fifo_push(solo, sch, 0x2222);
            CHECK(dspp_sim_dspi_read(solo, sdata) == 0x2111u);
            CHECK(dspp_sim_dma_completion_pending(solo, sch) == 0);
            CHECK(dspp_sim_dspi_read(solo, sdata) == 0x2222u);
            CHECK(dspp_sim_dma_completion_pending(solo, sch) == 1);
            CHECK(dspp_sim_dma_poll_completion(solo, &polled) == 1);
            CHECK(polled == sch);
            CHECK(dspp_sim_dma_completion_queued(solo) == 0u);
        }
        dspp_sim_destroy(solo);
    }
    // ---- Folio attachment-channel map (completion pump attribution) ----
    // The pump drains the shared-space completion queue once per frame and
    // attributes each channel to the monitored attachments sharing its FIFO
    // through the retained AF_TAG_NAME hook. This block proves the separable
    // half — m2_guest_audio_resolve_attachment_channel mirrors the consumed
    // reader order (BAD_ITEM dead/wrong-type, UNIMPLEMENTED sim-less, hook
    // errors pass through, else the channel with M2K_OK) without building
    // sims, raising signals, or touching delivery. Disc-backed:
    // sampler_16_f2.dsp carries the single IN_FIFO "InFIFO" plus a non-FIFO
    // "Output" for the MISMATCH path. Signal delivery itself stays
    // live-verified (orchestrator mixer run).
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_resolve_attachment_channel(uint32_t attachment, uint32_t *channel_out);
        setup();
        const uint32_t map_samp_ea = 0xB400u;
        const uint32_t map_hook_ea = 0xB440u;
        const uint32_t map_out_ea = 0xB480u;
        const uint32_t map_hookname_ea = 0xB4A0u;
        const uint32_t map_nametags_ea = 0xB4C0u;
        const uint32_t map_badhname_ea = 0xB4E0u;
        const uint32_t map_badtags_ea = 0xB500u;
        const uint32_t map_outtags_ea = 0xB540u;
        write_guest_string(map_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(map_hook_ea, "InFIFO");
        write_guest_string(map_out_ea, "Output");
        write_guest_string(map_hookname_ea, "InFIFO");
        wr32(&g_cpu, map_nametags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, map_nametags_ea + 4u, map_hookname_ea);
        wr32(&g_cpu, map_nametags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, map_nametags_ea + 12u, 0u);
        write_guest_string(map_badhname_ea, "NoSuchHook");
        wr32(&g_cpu, map_badtags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, map_badtags_ea + 4u, map_badhname_ea);
        wr32(&g_cpu, map_badtags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, map_badtags_ea + 12u, 0u);
        wr32(&g_cpu, map_outtags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, map_outtags_ea + 4u, map_out_ea);
        wr32(&g_cpu, map_outtags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, map_outtags_ea + 12u, 0u);
        int32_t map_ins = m2_guest_audio_load_instrument(&g_cpu, map_samp_ea, 1, 100);
        CHECK(map_ins > 0);
        uint32_t map_ch = 0xdeadbeefu;
        // Dead and wrong-typed handles report BAD_ITEM with a zeroed output.
        CHECK(m2_guest_audio_resolve_attachment_channel(0u, &map_ch) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(map_ch == 0u);
        uint32_t map_cue = hdl->CreateItem(M2_AUDIO_CUE_CTYPE, 0);
        CHECK(map_cue != M2K_ITEM_NONE);
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_cue, &map_ch) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(map_ch == 0u);
        // Attachments need a sample slave; mint them through the production
        // wrapper (master, slave, optional hook list).
        uint32_t map_slave = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
        CHECK(map_slave != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)map_ins;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_default = g_cpu.gpr[3];
        CHECK(map_default != M2K_ITEM_NONE);
        g_cpu.gpr[3] = (uint32_t)map_ins;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = map_nametags_ea;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_named = g_cpu.gpr[3];
        CHECK(map_named != M2K_ITEM_NONE);
        g_cpu.gpr[3] = (uint32_t)map_ins;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = map_badtags_ea;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_unknown = g_cpu.gpr[3];
        CHECK(map_unknown != M2K_ITEM_NONE);
        g_cpu.gpr[3] = (uint32_t)map_ins;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = map_outtags_ea;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_nonfifo = g_cpu.gpr[3];
        CHECK(map_nonfifo != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(NULL);
        // Observation never builds: the fresh sampler has no sim, so every
        // attachment read stays UNIMPLEMENTED with a zeroed output.
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_default, &map_ch) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(map_ch == 0u);
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_named, &map_ch) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(map_ch == 0u);
        // Template masters never own DMA hardware: BAD_ITEM, never fabricated.
        int32_t map_template = m2_guest_audio_create_mixer_template(&g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(map_template > 0);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)map_template;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_tatt = g_cpu.gpr[3];
        CHECK(map_tatt != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(NULL);
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_tatt, &map_ch) == M2_AUDIO_ERR_BAD_ITEM);
        CHECK(map_ch == 0u);
        // Pure-synth masters stay UNIMPLEMENTED: a mixer-template instrument
        // builds no DSP image, so there is no FIFO to resolve.
        uint32_t map_mixer_ins = hdl->CreateItemVA(M2_AUDIO_INSTRUMENT_CTYPE, 16u, (uint32_t)map_template, M2_TAG_JUMP, 0, 0, 0, 0);
        CHECK(map_mixer_ins != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = map_mixer_ins;
        g_cpu.gpr[4] = map_slave;
        g_cpu.gpr[5] = 0;
        FN_CreateAttachment__73394(&g_cpu);
        uint32_t map_matt = g_cpu.gpr[3];
        CHECK(map_matt != M2K_ITEM_NONE);
        m2_kernel_dispatch_set_global(NULL);
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_matt, &map_ch) == M2_AUDIO_ERR_UNIMPLEMENTED);
        CHECK(map_ch == 0u);
        // Binding the numeric descriptor builds the sampler sim; the default
        // (NULL) hook and the retained "InFIFO" hook then resolve to the same
        // single-FIFO channel, proving hook retention across creation order.
        CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)map_ins, map_hook_ea, 0x1200u, 16u, 0u, 0, 0u) == M2K_OK);
        uint32_t map_ch_default = 0xdeadbeefu, map_ch_named = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_default, &map_ch_default) == M2K_OK);
        CHECK(map_ch_default < 32u);
        CHECK(m2_guest_audio_resolve_attachment_channel(map_named, &map_ch_named) == M2K_OK);
        CHECK(map_ch_named == map_ch_default);
        // NULL out-param is tolerated on the valid path.
        CHECK(m2_guest_audio_resolve_attachment_channel(map_named, NULL) == M2K_OK);
        // Hook errors pass through with a zeroed output: unknown hook keeps
        // NAME_MISSING, the non-FIFO port reports MISMATCH.
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_unknown, &map_ch) == M2_AUDIO_ERR_NAME_MISSING);
        CHECK(map_ch == 0u);
        map_ch = 0xdeadbeefu;
        CHECK(m2_guest_audio_resolve_attachment_channel(map_nonfifo, &map_ch) == M2_AUDIO_ERR_MISMATCH);
        CHECK(map_ch == 0u);
        hdl->DeleteItem(map_cue);
        hdl->DeleteItem(map_slave);
        hdl->DeleteItem(map_mixer_ins);
        hdl->DeleteItem((uint32_t)map_template);
        CHECK(m2_guest_audio_unload_instrument((uint32_t)map_ins) == M2K_OK);
        teardown();
    }
    // ---- Raw-DSPP completion -> Cue pump fabrication guard (synthetic) ----
    // Vendor folios/audio/audio_attachments.c ProcessAttachmentDMA signals the
    // monitored Cue when the final segment completes; the loader records one
    // completion-qualified event per numeric-attached channel (see the
    // completion-qualified consumed block above) and
    // gk_pump_attachment_completions delivers it at the frame boundary through
    // attachment_cue -> Cue owner/signal_mask -> m2k_send_signal. No disc
    // dependency: a synthetic single-IN_FIFO SLEEP image is shadowed over the
    // sampler bridge path (a catalog name is required by
    // gk_validate_audio_template_name; the shadow is saved and restored so
    // disc-backed blocks sharing this process cache are undisturbed), so the
    // folio builds a live sim without reading the game data. Positive
    // delivery stays unpinned: no production or test path pushes FIFO words
    // into g_gk.audio_dsp_space (the folio exposes attach/query but never
    // feed/drain), so the queue cannot be filled from a unit test. This block
    // pins the half the seam owns end to end today: a bound, attached,
    // monitored attachment with an EMPTY completion queue rides two real
    // frame-boundary pumps and wakes nothing, the consumed generation stays
    // zero, and both hooks resolve to the bound channel the pump would match.
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_resolve_attachment_channel(uint32_t attachment, uint32_t *channel_out);
        extern int32_t m2_guest_audio_read_numeric_consumed(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t *generation_out);
        setup();
        test_synth_fifo_build();
        int raw_shadow_ok = test_dsp_shadow_install(
            "System.m2/Audio/dsp/sampler_16_f2.dsp", g_synth_fifo_file,
            TEST_SYNTH_FIFO_FILE_SIZE);
        CHECK(raw_shadow_ok == 1);
        const uint32_t raw_samp_ea = 0xB600u;
        const uint32_t raw_hook_ea = 0xB640u;
        const uint32_t raw_hookname_ea = 0xB680u;
        const uint32_t raw_nametags_ea = 0xB6C0u;
        const uint32_t raw_out_ea = 0xB6E0u;
        const uint32_t raw_outhname_ea = 0xB700u;
        const uint32_t raw_outtags_ea = 0xB720u;
        write_guest_string(raw_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(raw_hook_ea, "InFIFO");
        write_guest_string(raw_hookname_ea, "InFIFO");
        write_guest_string(raw_out_ea, "Output");
        write_guest_string(raw_outhname_ea, "Output");
        wr32(&g_cpu, raw_outtags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, raw_outtags_ea + 4u, raw_outhname_ea);
        wr32(&g_cpu, raw_outtags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, raw_outtags_ea + 12u, 0u);
        wr32(&g_cpu, raw_nametags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, raw_nametags_ea + 4u, raw_hookname_ea);
        wr32(&g_cpu, raw_nametags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, raw_nametags_ea + 12u, 0u);
        int32_t raw_ins = m2_guest_audio_load_instrument(&g_cpu, raw_samp_ea, 1, 100);
        CHECK(raw_ins > 0);
        if (raw_shadow_ok == 1 && raw_ins > 0) {
            // Bind count 2 through the production seam: builds the synthetic
            // sim and records the descriptor on its single FIFO channel.
            CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)raw_ins, raw_hook_ea, 0x1200u, 2u, 0u, 0, 0u) == M2K_OK);
            uint32_t raw_slave = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
            CHECK(raw_slave != M2K_ITEM_NONE);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)raw_ins;
            g_cpu.gpr[4] = raw_slave;
            g_cpu.gpr[5] = 0;
            FN_CreateAttachment__73394(&g_cpu);
            uint32_t raw_default = g_cpu.gpr[3];
            CHECK(raw_default != M2K_ITEM_NONE);
            g_cpu.gpr[3] = (uint32_t)raw_ins;
            g_cpu.gpr[4] = raw_slave;
            g_cpu.gpr[5] = raw_nametags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            uint32_t raw_named = g_cpu.gpr[3];
            CHECK(raw_named != M2K_ITEM_NONE);
            g_cpu.gpr[3] = (uint32_t)raw_ins;
            g_cpu.gpr[4] = raw_slave;
            g_cpu.gpr[5] = raw_outtags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            uint32_t raw_nonfifo = g_cpu.gpr[3];
            CHECK(raw_nonfifo != M2K_ITEM_NONE);
            m2_kernel_dispatch_set_global(NULL);
            // Both hooks resolve to the bound channel: the exact map the
            // pump's match loop recomputes per monitored attachment.
            uint32_t raw_ch_default = 0xdeadbeefu, raw_ch_named = 0xdeadbeefu;
            CHECK(m2_guest_audio_resolve_attachment_channel(raw_default, &raw_ch_default) == M2K_OK);
            CHECK(m2_guest_audio_resolve_attachment_channel(raw_named, &raw_ch_named) == M2K_OK);
            CHECK(raw_ch_default == raw_ch_named);
            // Self-proof of synthetic bytes: the image carries no "Output"
            // resource, so the hook reports NAME_MISSING. The real disc
            // sampler carries a non-FIFO "Output" and would report MISMATCH,
            // so this CHECK cannot pass on disc bytes.
            uint32_t raw_ch_nonfifo = 0xdeadbeefu;
            CHECK(m2_guest_audio_resolve_attachment_channel(raw_nonfifo, &raw_ch_nonfifo) == M2_AUDIO_ERR_NAME_MISSING);
            CHECK(raw_ch_nonfifo == 0u);
            // Nothing drained: the consumed generation is still zero.
            uint32_t raw_gen = 0xdeadbeefu;
            CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)raw_ins, raw_hook_ea, &raw_gen) == M2K_OK);
            CHECK(raw_gen == 0u);
            // Monitor the default attachment from an owner task, then ride
            // two real frame-boundary pumps with the queue empty: a
            // fabricating pump would wake the task; the staged polls stay 0.
            rawcomp_reset_stages();
            g_rawcomp_target = raw_default;
            uint32_t raw_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0);
            CHECK(raw_task != M2K_ITEM_NONE);
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
            m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1, memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == 0);
            m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage2, 1, memory_order_release);
            CHECK(m2k_join_task(&g_kctx, raw_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, raw_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
            hdl->DeleteItem(raw_slave);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)raw_ins) == M2K_OK);
        }
        test_dsp_shadow_remove();
        teardown();
    }
    // ---- Raw-DSPP completion -> Cue pump positive delivery (disc-backed) ----
    // `SYNTHETICALLY_REACHED`: FIFO words are test-established via the push
    // seam; sampler drain, completion edge, and pump are original lifted code.
    // Closes the half the guard above leaves unpinned: 2 words pushed through
    // the numeric push seam into the folio sim's InFIFO drain through
    // authentic sampler_16_f2.dsp reads as started ticks run, the consumed
    // generation reaches the bound count 2/2, the completion edge queues, and
    // the frame-boundary pump delivers the monitored CUE_AT_END exactly once
    // (mid-stage poll observes the signal bit; the end poll reads 0 because
    // poll consumes). Pre-push-seam this block fails (no feed path into the
    // folio space can fill the queue); a fabricating pump fails the guard.
    {
        extern int32_t m2_guest_audio_attach_numeric_descriptor(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t address, uint32_t count_samples, uint32_t flags, int32_t is_delay, uint32_t start_taglist_ea);
        extern int32_t m2_guest_audio_read_numeric_consumed(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t *generation_out);
        extern int32_t m2_guest_audio_push_numeric_fifo(PPCContext *cpu, uint32_t instrument, uint32_t hook_name_ea, uint32_t words_ea, uint32_t word_count);
        setup();
        const uint32_t pos_samp_ea = 0xB800u;
        const uint32_t pos_hook_ea = 0xB840u;
        const uint32_t pos_nametags_ea = 0xB880u;
        const uint32_t pos_words_ea = 0xB8A0u;
        write_guest_string(pos_samp_ea, "sampler_16_f2.dsp");
        write_guest_string(pos_hook_ea, "InFIFO");
        wr32(&g_cpu, pos_nametags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, pos_nametags_ea + 4u, pos_hook_ea);
        wr32(&g_cpu, pos_nametags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, pos_nametags_ea + 12u, 0u);
        int32_t pos_ins = m2_guest_audio_load_instrument(&g_cpu, pos_samp_ea, 1, 100);
        CHECK(pos_ins > 0);
        if (pos_ins > 0) {
            // Bind count 2 through the production seam, then prove the push
            // seam's own fail-closed edges: oversize count and dead handle.
            CHECK(m2_guest_audio_attach_numeric_descriptor(&g_cpu, (uint32_t)pos_ins, pos_hook_ea, 0x1200u, 2u, 0u, 0, 0u) == M2K_OK);
            CHECK(m2_guest_audio_push_numeric_fifo(&g_cpu, (uint32_t)pos_ins, pos_hook_ea, pos_words_ea, 9u) == M2_AUDIO_ERR_OUT_OF_RANGE);
            CHECK(m2_guest_audio_push_numeric_fifo(&g_cpu, 0u, pos_hook_ea, pos_words_ea, 1u) == M2_AUDIO_ERR_BAD_ITEM);
            uint32_t pos_slave = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE, 0);
            CHECK(pos_slave != M2K_ITEM_NONE);
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = (uint32_t)pos_ins;
            g_cpu.gpr[4] = pos_slave;
            g_cpu.gpr[5] = pos_nametags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            uint32_t pos_att = g_cpu.gpr[3];
            CHECK(pos_att != M2K_ITEM_NONE);
            m2_kernel_dispatch_set_global(NULL);
            // Monitor before any completion can exist, exactly like the guard.
            rawcomp_reset_stages();
            g_rawcomp_target = pos_att;
            uint32_t pos_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0);
            CHECK(pos_task != M2K_ITEM_NONE);
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
            // Feed 2 BE words, then start: ticks drain through authentic DSPP
            // reads (start reuses the attach-built sim, no rebuild, no wipe).
            wr32(&g_cpu, pos_words_ea, (0x1111u << 16) | 0x2222u);
            CHECK(m2_guest_audio_push_numeric_fifo(&g_cpu, (uint32_t)pos_ins, pos_hook_ea, pos_words_ea, 2u) == M2K_OK);
            uint32_t pos_gen = 0xdeadbeefu;
            CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)pos_ins, pos_hook_ea, &pos_gen) == M2K_OK);
            CHECK(pos_gen == 0u);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)pos_ins, 0) == M2K_OK);
            // Bounded drain wait: rate dividers may skip ticks, so poll the
            // generation rather than assuming a tick count.
            uint32_t pos_pumps = 0;
            while (pos_gen != 2u && pos_pumps < 64u) {
                m2_guest_audio_pump_timers();
                ++pos_pumps;
                CHECK(m2_guest_audio_read_numeric_consumed(&g_cpu, (uint32_t)pos_ins, pos_hook_ea, &pos_gen) == M2K_OK);
            }
            CHECK(pos_gen == 2u);
            // The edge queues during the tick half; the next frame boundary
            // delivers it through the identical seam path the guard covers.
            m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1, memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == (int32_t)g_rawcomp_signal);
            atomic_store_explicit(&g_rawcomp_stage2, 1, memory_order_release);
            CHECK(m2k_join_task(&g_kctx, pos_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, pos_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
            hdl->DeleteItem(pos_slave);
            CHECK(m2_guest_audio_unload_instrument((uint32_t)pos_ins) == M2K_OK);
        }
        teardown();
    }

    // ---- Production delay attachment current/next -> Cue completion --------
    // `SYNTHETICALLY_REACHED`: the Item/tag/Cue objects are constructed by
    // this focused contract fixture, while template loading, FIFO selection,
    // finite delay storage, DSPP sampler reads, current/next promotion,
    // completion qualification, and task-signal delivery all use the
    // production bridges. The source's AF_ATTF_NOAUTOSTART distinction is
    // pinned by starting the instrument first and then explicitly starting the
    // first attachment. A natural finite end raises exactly the first Cue at a
    // later frame-boundary pump, promotes the linked successor without
    // resetting the resident FIFO, and raises the same Cue exactly once for
    // the successor's finite end.
    {
        setup();
        const uint32_t prod_name_ea = 0xC000u;
        const uint32_t prod_inst_tags_ea = 0xC040u;
        const uint32_t prod_sample_tags_ea = 0xC080u;
        const uint32_t prod_hook_name_ea = 0xC0C0u;
        const uint32_t prod_attachment_tags_ea = 0xC100u;
        const uint32_t prod_sample2_tags_ea = 0xC140u;
        const uint32_t prod_attachment2_tags_ea = 0xC180u;
        write_guest_string(prod_name_ea, "sampler_raw_f1.dsp");
        write_guest_string(prod_hook_name_ea, "InFIFO");

        g_cpu.gpr[3] = prod_name_ea;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 32u, 0x60ba4u) == 1);
        const uint32_t prod_template = g_cpu.gpr[3];
        CHECK(prod_template != M2K_ITEM_NONE);

        wr32(&g_cpu, prod_inst_tags_ea + 0u, M2_AUDIO_TAG_TEMPLATE);
        wr32(&g_cpu, prod_inst_tags_ea + 4u, prod_template);
        wr32(&g_cpu, prod_inst_tags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, prod_inst_tags_ea + 12u, 0u);
        const uint32_t prod_instrument = hdl->CreateItem(
            M2_AUDIO_INSTRUMENT_CTYPE, prod_inst_tags_ea);
        CHECK(prod_instrument != M2K_ITEM_NONE);

        wr32(&g_cpu, prod_sample_tags_ea + 0u, M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, prod_sample_tags_ea + 4u, 8u);
        wr32(&g_cpu, prod_sample_tags_ea + 8u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, prod_sample_tags_ea + 12u, 1u);
        wr32(&g_cpu, prod_sample_tags_ea + 16u,
             M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, prod_sample_tags_ea + 20u, 0xffffffffu);
        wr32(&g_cpu, prod_sample_tags_ea + 24u,
             M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, prod_sample_tags_ea + 28u, 0xffffffffu);
        wr32(&g_cpu, prod_sample_tags_ea + 32u, M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, prod_sample_tags_ea + 36u,
             M2_AUDIO_SAMPLE_FLAG_CLEAR_ON_STOP);
        wr32(&g_cpu, prod_sample_tags_ea + 40u, M2_TAG_END);
        wr32(&g_cpu, prod_sample_tags_ea + 44u, 0u);
        const uint32_t prod_sample = hdl->CreateItem(
            M2_AUDIO_SAMPLE_CTYPE, prod_sample_tags_ea);
        CHECK(prod_sample != M2K_ITEM_NONE);

        wr32(&g_cpu, prod_sample2_tags_ea + 0u, M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, prod_sample2_tags_ea + 4u, 8u);
        wr32(&g_cpu, prod_sample2_tags_ea + 8u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, prod_sample2_tags_ea + 12u, 1u);
        wr32(&g_cpu, prod_sample2_tags_ea + 16u,
             M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, prod_sample2_tags_ea + 20u, 0xffffffffu);
        wr32(&g_cpu, prod_sample2_tags_ea + 24u,
             M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, prod_sample2_tags_ea + 28u, 0xffffffffu);
        wr32(&g_cpu, prod_sample2_tags_ea + 32u, M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, prod_sample2_tags_ea + 36u,
             M2_AUDIO_SAMPLE_FLAG_CLEAR_ON_STOP);
        wr32(&g_cpu, prod_sample2_tags_ea + 40u, M2_TAG_END);
        wr32(&g_cpu, prod_sample2_tags_ea + 44u, 0u);
        const uint32_t prod_sample2 = hdl->CreateItem(
            M2_AUDIO_SAMPLE_CTYPE, prod_sample2_tags_ea);
        CHECK(prod_sample2 != M2K_ITEM_NONE);

        wr32(&g_cpu, prod_attachment_tags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, prod_attachment_tags_ea + 4u, prod_hook_name_ea);
        wr32(&g_cpu, prod_attachment_tags_ea + 8u,
             M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, prod_attachment_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
        wr32(&g_cpu, prod_attachment_tags_ea + 16u, M2_TAG_END);
        wr32(&g_cpu, prod_attachment_tags_ea + 20u, 0u);
        uint32_t prod_attachment = M2K_ITEM_NONE;
        if (prod_instrument != M2K_ITEM_NONE &&
            prod_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = prod_instrument;
            g_cpu.gpr[4] = prod_sample;
            g_cpu.gpr[5] = prod_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            prod_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(prod_attachment != M2K_ITEM_NONE);

        wr32(&g_cpu, prod_attachment2_tags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, prod_attachment2_tags_ea + 4u, prod_hook_name_ea);
        wr32(&g_cpu, prod_attachment2_tags_ea + 8u,
             M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, prod_attachment2_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
        wr32(&g_cpu, prod_attachment2_tags_ea + 16u, M2_TAG_END);
        wr32(&g_cpu, prod_attachment2_tags_ea + 20u, 0u);
        uint32_t prod_attachment2 = M2K_ITEM_NONE;
        if (prod_instrument != M2K_ITEM_NONE &&
            prod_sample2 != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = prod_instrument;
            g_cpu.gpr[4] = prod_sample2;
            g_cpu.gpr[5] = prod_attachment2_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            prod_attachment2 = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(prod_attachment2 != M2K_ITEM_NONE);

        rawchain_reset_stages();
        g_rawchain_target1 = prod_attachment;
        g_rawchain_target2 = prod_attachment2;
        uint32_t prod_task = M2K_ITEM_NONE;
        if (prod_attachment != M2K_ITEM_NONE &&
            prod_attachment2 != M2K_ITEM_NONE)
            prod_task = m2k_create_task(&g_kctx, rawchain_monitor_task, 0u);
        CHECK(prod_task != M2K_ITEM_NONE);
        if (prod_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawchain_ready));
            CHECK(g_rawchain_monitor_rc == M2K_OK);
            CHECK(g_rawchain_signal != 0u);
        }

        int32_t prod_start_rc = M2_AUDIO_ERR_BAD_ITEM;
        if (prod_instrument != M2K_ITEM_NONE) {
            prod_start_rc = m2_guest_audio_start_instrument(
                &g_cpu, prod_instrument, 0u);
        }
        CHECK(prod_start_rc == M2K_OK);
        if (prod_attachment != M2K_ITEM_NONE &&
            prod_attachment2 != M2K_ITEM_NONE) {
            g_cpu.gpr[3] = prod_attachment;
            g_cpu.gpr[4] = prod_attachment2;
            CHECK(m2_audio_dispatch_c(&g_cpu, 51u, 0x6a0a0u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        }
        if (prod_attachment != M2K_ITEM_NONE) {
            g_cpu.gpr[3] = prod_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        }
        // Advance only this fixed source-shaped chain. The first completion is
        // consumed at a later pump, then the staged monitor is switched to the
        // successor before its completion is observed. The successor is
        // full-started once more to prove stale completion state does not leak
        // across reuse.
        for (uint32_t i = 0u; i < 1u; ++i)
            m2_guest_audio_pump_timers();
        if (prod_task != M2K_ITEM_NONE) {
            atomic_store_explicit(&g_rawchain_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawchain_first_seen));
            CHECK(g_rawchain_poll_first == (int32_t)g_rawchain_signal);
            CHECK(trig_spin_until(&g_rawchain_second_ready));
            CHECK(g_rawchain_monitor_rc == M2K_OK);
            for (uint32_t i = 0u; i < 8u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawchain_stage2, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawchain_second_seen));
            CHECK(g_rawchain_poll_second == (int32_t)g_rawchain_signal);
            g_cpu.gpr[3] = prod_attachment2;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            for (uint32_t i = 0u; i < 4u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawchain_stage3, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawchain_restart_seen));
            CHECK(g_rawchain_poll_restart == (int32_t)g_rawchain_signal);
            for (uint32_t i = 0u; i < 4u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawchain_stage4, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, prod_task) == M2K_OK);
            CHECK(g_rawchain_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, prod_task) == M2K_OK);
            hdl->DeleteItem(g_rawchain_cue);
        }
        // Delete an active physical attachment before its first DMA service.
        // Vendor internalDeleteAudioAttachment stops the attachment before
        // reclaiming it; the pending Cue must not be delivered later, and a
        // fresh attachment on the same FIFO must be able to reuse the channel
        // without inheriting that deleted completion.
        uint32_t abort_attachment = M2K_ITEM_NONE;
        if (prod_instrument != M2K_ITEM_NONE &&
            prod_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = prod_instrument;
            g_cpu.gpr[4] = prod_sample;
            g_cpu.gpr[5] = prod_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            abort_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(abort_attachment != M2K_ITEM_NONE);
        rawcomp_reset_stages();
        g_rawcomp_target = abort_attachment;
        uint32_t abort_task = M2K_ITEM_NONE;
        if (abort_attachment != M2K_ITEM_NONE)
            abort_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(abort_task != M2K_ITEM_NONE);
        if (abort_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
            g_cpu.gpr[3] = abort_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            hdl->DeleteItem(abort_attachment);
            for (uint32_t i = 0u; i < 8u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == 0);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, abort_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, abort_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }

        uint32_t reuse_attachment = M2K_ITEM_NONE;
        if (prod_instrument != M2K_ITEM_NONE &&
            prod_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = prod_instrument;
            g_cpu.gpr[4] = prod_sample;
            g_cpu.gpr[5] = prod_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            reuse_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(reuse_attachment != M2K_ITEM_NONE);
        rawcomp_reset_stages();
        g_rawcomp_target = reuse_attachment;
        uint32_t reuse_task = M2K_ITEM_NONE;
        if (reuse_attachment != M2K_ITEM_NONE)
            reuse_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(reuse_task != M2K_ITEM_NONE);
        if (reuse_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
            g_cpu.gpr[3] = reuse_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            for (uint32_t i = 0u; i < 8u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == (int32_t)g_rawcomp_signal);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, reuse_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, reuse_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }
        if (reuse_attachment != M2K_ITEM_NONE)
            hdl->DeleteItem(reuse_attachment);
        if (prod_instrument != M2K_ITEM_NONE)
            CHECK(m2_guest_audio_unload_instrument(prod_instrument) == M2K_OK);
        if (prod_sample != M2K_ITEM_NONE)
            hdl->DeleteItem(prod_sample);
        if (prod_sample2 != M2K_ITEM_NONE)
            hdl->DeleteItem(prod_sample2);
        if (prod_template != M2K_ITEM_NONE)
            hdl->DeleteItem(prod_template);
        teardown();
    }

    // ---- Production FATLADYSINGS terminal event ---------------------------
    // `SYNTHETICALLY_REACHED`: this is a bounded source-backed event-order
    // fixture, not a route search. The original sampler template, physical
    // delay binding, finite completion, FIRQ/audio-daemon dispatch, owning
    // instrument stop, and monitoring Cue all cross the production boundary.
    // Vendor ProcessAttachmentDMA gives FATLADYSINGS precedence over a
    // successor and calls StopInstrument before SignalMonitoringCue. The
    // post-event StopInstrument probe observes that the owner was stopped;
    // the staged task proves the exact Cue still arrived once.
    {
        setup();
        const uint32_t fat_name_ea = 0xD000u;
        const uint32_t fat_inst_tags_ea = 0xD040u;
        const uint32_t fat_sample_tags_ea = 0xD080u;
        const uint32_t fat_hook_name_ea = 0xD0C0u;
        const uint32_t fat_attachment_tags_ea = 0xD100u;
        write_guest_string(fat_name_ea, "sampler_raw_f1.dsp");
        write_guest_string(fat_hook_name_ea, "InFIFO");

        g_cpu.gpr[3] = fat_name_ea;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 32u, 0x60ba4u) == 1);
        const uint32_t fat_template = g_cpu.gpr[3];
        CHECK(fat_template != M2K_ITEM_NONE);

        wr32(&g_cpu, fat_inst_tags_ea + 0u, M2_AUDIO_TAG_TEMPLATE);
        wr32(&g_cpu, fat_inst_tags_ea + 4u, fat_template);
        wr32(&g_cpu, fat_inst_tags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, fat_inst_tags_ea + 12u, 0u);
        const uint32_t fat_instrument = hdl->CreateItem(
            M2_AUDIO_INSTRUMENT_CTYPE, fat_inst_tags_ea);
        CHECK(fat_instrument != M2K_ITEM_NONE);

        wr32(&g_cpu, fat_sample_tags_ea + 0u, M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, fat_sample_tags_ea + 4u, 8u);
        wr32(&g_cpu, fat_sample_tags_ea + 8u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, fat_sample_tags_ea + 12u, 1u);
        wr32(&g_cpu, fat_sample_tags_ea + 16u,
             M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, fat_sample_tags_ea + 20u, 0xffffffffu);
        wr32(&g_cpu, fat_sample_tags_ea + 24u,
             M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, fat_sample_tags_ea + 28u, 0xffffffffu);
        wr32(&g_cpu, fat_sample_tags_ea + 32u, M2_TAG_END);
        wr32(&g_cpu, fat_sample_tags_ea + 36u, 0u);
        const uint32_t fat_sample = hdl->CreateItem(
            M2_AUDIO_SAMPLE_CTYPE, fat_sample_tags_ea);
        CHECK(fat_sample != M2K_ITEM_NONE);

        wr32(&g_cpu, fat_attachment_tags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, fat_attachment_tags_ea + 4u, fat_hook_name_ea);
        wr32(&g_cpu, fat_attachment_tags_ea + 8u,
             M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, fat_attachment_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART |
                 M2_AUDIO_ATTACHMENT_FLAG_FATLADYSINGS);
        wr32(&g_cpu, fat_attachment_tags_ea + 16u, M2_TAG_END);
        wr32(&g_cpu, fat_attachment_tags_ea + 20u, 0u);
        uint32_t fat_attachment = M2K_ITEM_NONE;
        if (fat_instrument != M2K_ITEM_NONE && fat_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = fat_sample;
            g_cpu.gpr[5] = fat_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            fat_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(fat_attachment != M2K_ITEM_NONE);

        rawcomp_reset_stages();
        g_rawcomp_target = fat_attachment;
        uint32_t fat_task = M2K_ITEM_NONE;
        if (fat_attachment != M2K_ITEM_NONE)
            fat_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(fat_task != M2K_ITEM_NONE);
        if (fat_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
        }

        CHECK(m2_guest_audio_start_instrument(&g_cpu, fat_instrument, 0u) ==
              M2K_OK);
        g_cpu.gpr[3] = fat_attachment;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        // This fixed sample has no sustain or release loop. The vendor's
        // DSPPReleaseSampleAttachment no-loop branch already follows the
        // finite completion/silence schedule, so ReleaseInstrument succeeds
        // without changing the physical DMA state. The valid release tag is
        // accepted even though this sample has no envelope attachment.
        const uint32_t release_tags_ea = 0xD120u;
        wr32(&g_cpu, release_tags_ea + 0u, M2_AUDIO_TAG_TIME_SCALE_FP);
        wr32(&g_cpu, release_tags_ea + 4u, 0x40000000u); // 2.0f
        wr32(&g_cpu, release_tags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, release_tags_ea + 12u, 0u);
        g_release_attachment = 0u;
        g_cpu.gpr[3] = fat_instrument;
        g_cpu.gpr[4] = release_tags_ea;
        CHECK(m2_audio_dispatch_c(&g_cpu, 68u, 0x2aa44u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        // Physical delay attachments are handled by the recovered Audio
        // Folio path, not by the ordinary host voice-release callback.
        CHECK(g_release_attachment == 0u);
        // The vendor has separate sustain/release DMA phases. The physical
        // adapter retains those authored ranges on each DMA binding. The
        // original finite attachment is still live until the next bounded
        // pump, so the second bind correctly reports the channel conflict.
        const uint32_t loop_sample_tags_ea = 0xD140u;
        const uint32_t loop_attachment_tags_ea = 0xD180u;
        wr32(&g_cpu, loop_sample_tags_ea + 0u, M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, loop_sample_tags_ea + 4u, 8u);
        wr32(&g_cpu, loop_sample_tags_ea + 8u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, loop_sample_tags_ea + 12u, 1u);
        wr32(&g_cpu, loop_sample_tags_ea + 16u,
             M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, loop_sample_tags_ea + 20u, 1u);
        wr32(&g_cpu, loop_sample_tags_ea + 24u, M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, loop_sample_tags_ea + 28u, 3u);
        wr32(&g_cpu, loop_sample_tags_ea + 32u, M2_TAG_END);
        wr32(&g_cpu, loop_sample_tags_ea + 36u, 0u);
        const uint32_t loop_sample = hdl->CreateItem(
            M2_AUDIO_SAMPLE_CTYPE, loop_sample_tags_ea);
        CHECK(loop_sample != M2K_ITEM_NONE);
        wr32(&g_cpu, loop_attachment_tags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, loop_attachment_tags_ea + 4u, fat_hook_name_ea);
        wr32(&g_cpu, loop_attachment_tags_ea + 8u,
             M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, loop_attachment_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
        wr32(&g_cpu, loop_attachment_tags_ea + 16u, M2_TAG_END);
        wr32(&g_cpu, loop_attachment_tags_ea + 20u, 0u);
        uint32_t loop_attachment = M2K_ITEM_NONE;
        if (fat_instrument != M2K_ITEM_NONE && loop_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = loop_sample;
            g_cpu.gpr[5] = loop_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            loop_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(loop_attachment != M2K_ITEM_NONE);
        if (loop_attachment != M2K_ITEM_NONE) {
            g_cpu.gpr[3] = loop_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2_AUDIO_ERR_IN_USE);
            hdl->DeleteItem(loop_attachment);
        }
        if (loop_sample != M2K_ITEM_NONE) hdl->DeleteItem(loop_sample);
        // The eight-byte finite sample completes on the first bounded frame
        // pump for sampler_raw_f1.dsp. No input search or timing adjustment is
        // involved; the sample and source template are fixed above.
        m2_guest_audio_pump_timers();
        if (fat_task != M2K_ITEM_NONE) {
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == (int32_t)g_rawcomp_signal);
            // The terminal event stopped the owner before the Cue signal was
            // emitted, so this stop is the vendor swiStopInstrument status
            // short circuit (ains_Status <= AF_STOPPED => 0) rather than an
            // error.
            CHECK(m2_guest_audio_stop_instrument(&g_cpu, fat_instrument, 0u) ==
                  M2K_OK);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, fat_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, fat_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }
        // Sustain-phase control. Vendor DSPPStartSampleAttachment has distinct
        // sustain-loop, release-loop, and release-phase DMA schedules. The
        // physical adapter stays in the sustain loop until ReleaseInstrument
        // schedules the finite tail, which then produces the monitoring Cue.
        const uint32_t sus_sample_tags_ea = 0xD200u;
        const uint32_t sus_attachment_tags_ea = 0xD240u;
        wr32(&g_cpu, sus_sample_tags_ea + 0u, M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, sus_sample_tags_ea + 4u, 8u);
        wr32(&g_cpu, sus_sample_tags_ea + 8u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, sus_sample_tags_ea + 12u, 1u);
        wr32(&g_cpu, sus_sample_tags_ea + 16u, M2_AUDIO_TAG_SUSTAINBEGIN);
        wr32(&g_cpu, sus_sample_tags_ea + 20u, 0u);
        wr32(&g_cpu, sus_sample_tags_ea + 24u, M2_AUDIO_TAG_SUSTAINEND);
        wr32(&g_cpu, sus_sample_tags_ea + 28u, 4u);
        wr32(&g_cpu, sus_sample_tags_ea + 32u, M2_TAG_END);
        wr32(&g_cpu, sus_sample_tags_ea + 36u, 0u);
        const uint32_t sus_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                                    sus_sample_tags_ea);
        CHECK(sus_sample != M2K_ITEM_NONE);
        wr32(&g_cpu, sus_attachment_tags_ea + 0u, M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, sus_attachment_tags_ea + 4u, fat_hook_name_ea);
        wr32(&g_cpu, sus_attachment_tags_ea + 8u, M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, sus_attachment_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
        wr32(&g_cpu, sus_attachment_tags_ea + 16u, M2_TAG_END);
        wr32(&g_cpu, sus_attachment_tags_ea + 20u, 0u);
        uint32_t sus_attachment = M2K_ITEM_NONE;
        if (fat_instrument != M2K_ITEM_NONE && sus_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = sus_sample;
            g_cpu.gpr[5] = sus_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            sus_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(sus_attachment != M2K_ITEM_NONE);
        rawcomp_reset_stages();
        g_rawcomp_target = sus_attachment;
        uint32_t sus_task = M2K_ITEM_NONE;
        if (sus_attachment != M2K_ITEM_NONE)
            sus_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(sus_task != M2K_ITEM_NONE);
        if (sus_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
        }
        if (sus_attachment != M2K_ITEM_NONE) {
            CHECK(m2_guest_audio_start_instrument(&g_cpu, fat_instrument, 0u) ==
                  M2K_OK);
            g_cpu.gpr[3] = sus_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 68u, 0x2aa44u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        }
        m2_guest_audio_pump_timers();
        m2_guest_audio_pump_timers();
        if (sus_task != M2K_ITEM_NONE) {
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == (int32_t)g_rawcomp_signal);
            CHECK(m2_guest_audio_stop_instrument(&g_cpu, fat_instrument, 0u) ==
                  M2K_OK);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, sus_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, sus_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }
        if (sus_attachment != M2K_ITEM_NONE) hdl->DeleteItem(sus_attachment);
        if (sus_sample != M2K_ITEM_NONE) hdl->DeleteItem(sus_sample);

        // Release-loop production vector after the finite attachment has
        // released the FIFO. Vendor DSPPReleaseSampleAttachment starts a
        // release loop directly when no sustain loop exists, and explicitly
        // does not enable a completion interrupt because that loop never
        // reaches a terminal segment. A fixed eight-frame pump crosses the
        // authored four-frame sample once and then observes the loop without
        // fabricating a terminal Cue.
        const uint32_t release_loop_only_sample_tags_ea = 0xD280u;
        const uint32_t release_loop_only_attachment_tags_ea = 0xD2C0u;
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 0u,
             M2_AUDIO_TAG_DELAY_LINE);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 4u, 8u);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 8u,
             M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 12u, 1u);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 16u,
             M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 20u, 1u);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 24u,
             M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 28u, 3u);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 32u, M2_TAG_END);
        wr32(&g_cpu, release_loop_only_sample_tags_ea + 36u, 0u);
        const uint32_t release_loop_only_sample = hdl->CreateItem(
            M2_AUDIO_SAMPLE_CTYPE, release_loop_only_sample_tags_ea);
        CHECK(release_loop_only_sample != M2K_ITEM_NONE);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 0u,
             M2_AUDIO_TAG_NAME);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 4u,
             fat_hook_name_ea);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 8u,
             M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 12u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 16u,
             M2_TAG_END);
        wr32(&g_cpu, release_loop_only_attachment_tags_ea + 20u, 0u);
        uint32_t release_loop_only_attachment = M2K_ITEM_NONE;
        if (fat_instrument != M2K_ITEM_NONE &&
            release_loop_only_sample != M2K_ITEM_NONE) {
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = release_loop_only_sample;
            g_cpu.gpr[5] = release_loop_only_attachment_tags_ea;
            FN_CreateAttachment__73394(&g_cpu);
            release_loop_only_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
        }
        CHECK(release_loop_only_attachment != M2K_ITEM_NONE);
        rawcomp_reset_stages();
        g_rawcomp_target = release_loop_only_attachment;
        uint32_t release_loop_task = M2K_ITEM_NONE;
        if (release_loop_only_attachment != M2K_ITEM_NONE)
            release_loop_task = m2k_create_task(&g_kctx, rawcomp_monitor_task,
                                                0u);
        CHECK(release_loop_task != M2K_ITEM_NONE);
        if (release_loop_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
            CHECK(m2_guest_audio_start_instrument(&g_cpu, fat_instrument,
                                                  0u) == M2K_OK);
            g_cpu.gpr[3] = release_loop_only_attachment;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            g_cpu.gpr[3] = fat_instrument;
            g_cpu.gpr[4] = 0u;
            CHECK(m2_audio_dispatch_c(&g_cpu, 68u, 0x2aa44u) == 1);
            CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
            uint32_t release_loop_channel = UINT32_MAX;
            CHECK(m2_guest_audio_resolve_attachment_channel(
                      release_loop_only_attachment, &release_loop_channel) ==
                  M2K_OK);
            for (uint32_t i = 0u; i < 4u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == 0);
            for (uint32_t i = 0u; i < 4u; ++i)
                m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, release_loop_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2_guest_audio_resolve_attachment_channel(
                      release_loop_only_attachment, &release_loop_channel) ==
                  M2K_OK);
            CHECK(m2_guest_audio_stop_instrument(&g_cpu, fat_instrument, 0u) ==
                  M2K_OK);
            CHECK(m2k_delete_task(&g_kctx, release_loop_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }
        if (release_loop_only_attachment != M2K_ITEM_NONE)
            hdl->DeleteItem(release_loop_only_attachment);
        if (release_loop_only_sample != M2K_ITEM_NONE)
            hdl->DeleteItem(release_loop_only_sample);
        if (fat_instrument != M2K_ITEM_NONE)
            CHECK(m2_guest_audio_unload_instrument(fat_instrument) == M2K_OK);
        if (fat_sample != M2K_ITEM_NONE) hdl->DeleteItem(fat_sample);
        if (fat_template != M2K_ITEM_NONE) hdl->DeleteItem(fat_template);
        teardown();
    }

    // ---- host-voice FATLADYSINGS terminal ordering ----------------------
    // `SYNTHETICALLY_REACHED`: bounded source-backed event-order fixture. The
    // host PCM/CBD2 adapters are this bridge's DMA completion source for
    // uploaded samples, so vendor ProcessAttachmentDMA's terminal branch
    // applies on this route as well: the final segment stops the owning
    // instrument (AF_ATTF_FATLADYSINGS) and only then sends the monitoring
    // Cue. A stop error returns from ProcessAttachmentDMA before
    // SignalMonitoringCue, so a segment whose owner cannot be stopped must not
    // fabricate the completion Cue.
    {
        setup();
        const uint32_t hv_name_ea = 0xE000u;
        const uint32_t hv_sample_tags_ea = 0xE040u;
        const uint32_t hv_att_tags_ea = 0xE0C0u;
        const uint32_t hv_data_ea = 0xE140u;
        write_guest_string(hv_name_ea, "sampler_16_f2.dsp");
        for (uint32_t b = 0; b < 8u; ++b)
            g_mem[hv_data_ea + b] = (uint8_t)(b * 8u);
        // Finite sample with no sustain/release markers: one terminal segment.
        wr32(&g_cpu, hv_sample_tags_ea + 0u, M2_AUDIO_TAG_ADDRESS);
        wr32(&g_cpu, hv_sample_tags_ea + 4u, hv_data_ea);
        wr32(&g_cpu, hv_sample_tags_ea + 8u, M2_AUDIO_TAG_SAMPLE_RATE_FP);
        wr32(&g_cpu, hv_sample_tags_ea + 12u, 0x46ac4400u); // 22050 Hz
        wr32(&g_cpu, hv_sample_tags_ea + 16u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, hv_sample_tags_ea + 20u, 1u);
        wr32(&g_cpu, hv_sample_tags_ea + 24u,
             M2_AUDIO_TAG_COMPRESSIONRATIO);
        wr32(&g_cpu, hv_sample_tags_ea + 28u, 1u);
        wr32(&g_cpu, hv_sample_tags_ea + 32u, M2_AUDIO_TAG_COMPRESSIONTYPE);
        wr32(&g_cpu, hv_sample_tags_ea + 36u, 0u);
        wr32(&g_cpu, hv_sample_tags_ea + 40u, M2_AUDIO_TAG_NUMBITS);
        wr32(&g_cpu, hv_sample_tags_ea + 44u, 16u);
        wr32(&g_cpu, hv_sample_tags_ea + 48u, M2_AUDIO_TAG_NUMBYTES);
        wr32(&g_cpu, hv_sample_tags_ea + 52u, 8u);
        wr32(&g_cpu, hv_sample_tags_ea + 56u, M2_TAG_END);
        wr32(&g_cpu, hv_sample_tags_ea + 60u, 0u);
        uint32_t hv_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                             hv_sample_tags_ea);
        CHECK(hv_sample != M2K_ITEM_NONE);
        CHECK(hdl->CheckItem(hv_sample) == 0);

        int32_t hv_inst = m2_guest_audio_load_instrument(&g_cpu, hv_name_ea,
                                                        1, 100);
        CHECK(hv_inst > 0);
        wr32(&g_cpu, hv_att_tags_ea + 0u, M2_AUDIO_TAG_SET_FLAGS);
        wr32(&g_cpu, hv_att_tags_ea + 4u,
             M2_AUDIO_ATTACHMENT_FLAG_NOAUTOSTART |
                 M2_AUDIO_ATTACHMENT_FLAG_FATLADYSINGS);
        wr32(&g_cpu, hv_att_tags_ea + 8u, M2_TAG_END);
        wr32(&g_cpu, hv_att_tags_ea + 12u, 0u);
        uint32_t hv_attachment = M2K_ITEM_NONE;
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)hv_inst;
        g_cpu.gpr[4] = hv_sample;
        g_cpu.gpr[5] = hv_att_tags_ea;
        FN_CreateAttachment__73394(&g_cpu);
        hv_attachment = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(hv_attachment != M2K_ITEM_NONE);

        rawcomp_reset_stages();
        g_rawcomp_target = hv_attachment;
        uint32_t hv_task = M2K_ITEM_NONE;
        if (hv_attachment != M2K_ITEM_NONE)
            hv_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(hv_task != M2K_ITEM_NONE);
        if (hv_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            CHECK(g_rawcomp_signal != 0u);
        }

        CHECK(m2_guest_audio_start_instrument(&g_cpu, (uint32_t)hv_inst,
                                              0u) == M2K_OK);
        // NOAUTOSTART keeps the attachment stopped at StartInstrument; the
        // explicit start binds it to a host voice.
        g_cpu.gpr[3] = hv_attachment;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 54u, 0x70948u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        const int32_t hv_voice = g_started_attachment_voice;
        CHECK(hv_voice >= 0 &&
              m2_host_audio_voice_sample((uint32_t)hv_voice) != 0u);

        g_attachment_completion = hv_attachment;
        m2_guest_audio_pump_timers();
        // Terminal ordering: the owner's voice is already gone, and the
        // monitored Cue still arrives exactly once with no trailing signal.
        CHECK(m2_host_audio_voice_sample((uint32_t)hv_voice) == 0u);
        if (hv_task != M2K_ITEM_NONE) {
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == (int32_t)g_rawcomp_signal);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, hv_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, hv_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }

        // Negative control for that ordering rule: a template is a legal
        // CreateAttachment master (the source's PropagateTemplateAttachments
        // path), but swiStopInstrument answers AF_ERR_BADITEM for it. The
        // terminal segment must therefore suppress the monitoring Cue instead
        // of fabricating a completion for an owner it could not stop.
        int32_t hv_template = m2_guest_audio_create_mixer_template(
            &g_cpu, (1u << 24) | (1u << 16), 0);
        CHECK(hv_template > 0);
        uint32_t hv_template_attachment = M2K_ITEM_NONE;
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)hv_template;
        g_cpu.gpr[4] = hv_sample;
        g_cpu.gpr[5] = hv_att_tags_ea;
        FN_CreateAttachment__73394(&g_cpu);
        hv_template_attachment = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(hv_template_attachment != M2K_ITEM_NONE);
        rawcomp_reset_stages();
        g_rawcomp_target = hv_template_attachment;
        uint32_t hv_no_task = M2K_ITEM_NONE;
        if (hv_template_attachment != M2K_ITEM_NONE)
            hv_no_task = m2k_create_task(&g_kctx, rawcomp_monitor_task, 0u);
        CHECK(hv_no_task != M2K_ITEM_NONE);
        if (hv_no_task != M2K_ITEM_NONE) {
            CHECK(trig_spin_until(&g_rawcomp_ready));
            CHECK(g_rawcomp_monitor_rc == M2K_OK);
            g_attachment_completion = hv_template_attachment;
            m2_guest_audio_pump_timers();
            atomic_store_explicit(&g_rawcomp_stage1, 1,
                                  memory_order_release);
            CHECK(trig_spin_until(&g_rawcomp_mid_seen));
            CHECK(g_rawcomp_poll_mid == 0);
            atomic_store_explicit(&g_rawcomp_stage2, 1,
                                  memory_order_release);
            CHECK(m2k_join_task(&g_kctx, hv_no_task) == M2K_OK);
            CHECK(g_rawcomp_poll_end == 0);
            CHECK(m2k_delete_task(&g_kctx, hv_no_task) == M2K_OK);
            hdl->DeleteItem(g_rawcomp_cue);
        }
        // The refused completion was consumed, not left queued for a later
        // frame to deliver.
        CHECK(m2_host_audio_pop_attachment_completion(&g_attachment_completion) ==
              0);

        if (hv_template_attachment != M2K_ITEM_NONE)
            hdl->DeleteItem(hv_template_attachment);
        if (hv_attachment != M2K_ITEM_NONE) hdl->DeleteItem(hv_attachment);
        if (hv_sample != M2K_ITEM_NONE) hdl->DeleteItem(hv_sample);
        if (hv_template > 0) hdl->DeleteItem((uint32_t)hv_template);
        if (hv_inst > 0)
            CHECK(m2_guest_audio_unload_instrument((uint32_t)hv_inst) ==
                  M2K_OK);
        teardown();
    }

    // ---- Delay-line FDMA transport (Audio Folio storage boundary) ----------
    // `SYNTHETICALLY_REACHED`: this is a bounded source-backed contract
    // fixture, not a route search.  delay_f1.dsp writes its Input to an
    // OUT_FIFO, while sampler_raw_f1.dsp reads an attached delay line through
    // an IN_FIFO.  The platform seam owns the cleared supervisor buffer and
    // gives each attachment an independent cursor; the guest templates still
    // supply the FIFO direction and sample movement.
    {
        dspp_template delay_template;
        dspp_template tap_template;
        dspp_template drift_template;
        dspp_sim_space *delay_space = NULL;
        dspp_sim *delay_writer = NULL;
        dspp_sim *delay_tap = NULL;
        dspp_sim *drift_tap = NULL;
        dspp_sim *clear_tap = NULL;
        dspp_delay_line *delay_line = NULL;
        dspp_delay_line *finite_line = NULL;
        dspp_delay_line *clear_line = NULL;
        uint32_t source_addr = 0u;
        uint32_t writer_channel = UINT32_MAX;
        uint32_t tap_channel = UINT32_MAX;
        uint32_t tap_output = UINT32_MAX;
        uint32_t clear_tap_channel = UINT32_MAX;
        uint32_t clear_tap_output = UINT32_MAX;
        uint32_t drift_channel = UINT32_MAX;
        uint32_t drift_output = UINT32_MAX;
        uint32_t drift_probe = UINT32_MAX;
        uint32_t drift_phase = UINT32_MAX;
        uint32_t drift_amplitude = UINT32_MAX;
        uint32_t drift_rate = UINT32_MAX;
        int source_reserved = 0;
        int delay_loaded;
        int tap_loaded;
        int drift_loaded;

        memset(&delay_template, 0, sizeof(delay_template));
        memset(&tap_template, 0, sizeof(tap_template));
        memset(&drift_template, 0, sizeof(drift_template));
        delay_loaded = test_load_disc_dsp_template("delay_f1.dsp",
                                                   &delay_template) == 0;
        tap_loaded = test_load_disc_dsp_template("sampler_raw_f1.dsp",
                                                 &tap_template) == 0;
        drift_loaded = test_load_disc_dsp_template("sampler_drift_v1.dsp",
                                                    &drift_template) == 0;
        CHECK(delay_loaded && tap_loaded && drift_loaded);
        if (delay_loaded && tap_loaded) {
            delay_space = dspp_sim_space_create();
            CHECK(delay_space != NULL);
            if (delay_space)
                delay_writer = dspp_sim_create_in_space(&delay_template,
                                                         delay_space);
            CHECK(delay_writer != NULL);
            if (delay_writer) {
                CHECK(test_dspp_find_alloc(delay_writer, "OutFIFO", 6u,
                                           &writer_channel) == 0);
                // The source is a real shared DSPP data word, not an
                // invented host-side sample register.
                source_reserved =
                    dspp_sim_space_reserve_words(delay_space, 1u,
                                                 &source_addr) == 0;
                CHECK(source_reserved);
            }
        }

        if (delay_writer && writer_channel != UINT32_MAX && source_reserved)
            dspp_sim_connect(delay_writer, "Input", 0,
                             (uint16_t)(0x8000u | source_addr));

        if (delay_writer && writer_channel != UINT32_MAX && source_reserved) {
            delay_line = dspp_delay_line_create(16u, 1u, 1);
            CHECK(delay_line != NULL);
            if (delay_line) {
                CHECK(dspp_sim_dma_attach_delay_line(
                          delay_writer, writer_channel, delay_line, 1, 0u) ==
                      0);
                for (uint32_t i = 0; i < 8u; ++i) {
                    dspp_sim_poke_data(delay_writer, source_addr,
                                       (uint16_t)(100u + i));
                    dspp_sim_tick(delay_writer);
                }
                // FDMA is serviced before both the MOVE and the SLEEP
                // instruction.  The fourth and eighth guest frames therefore
                // drain the FIFO synchronously; the tap below verifies that
                // all eight words reached the physical delay storage.
                dspp_sim_dma_detach_delay_line(delay_writer, writer_channel);
            }

            // A non-looping delay line is finite sample storage, not an
            // implicit zero source.  Its output attachment drains the final
            // FIFO words and then stops, matching DSPPCalcSampleStart's
            // no-next-segment case.
            finite_line = dspp_delay_line_create(8u, 1u, 0);
            CHECK(finite_line != NULL);
            if (finite_line) {
                CHECK(dspp_sim_dma_attach_delay_line(
                          delay_writer, writer_channel, finite_line, 1, 0u) ==
                      0);
                for (uint32_t i = 0; i < 4u; ++i) {
                    dspp_sim_poke_data(delay_writer, source_addr,
                                       (uint16_t)(300u + i));
                    dspp_sim_tick(delay_writer);
                }
                dspp_sim_dma_detach_delay_line(delay_writer, writer_channel);
            }

            // AF_SAMPF_CLEARONSTOP clears the physical delay storage when
            // the last running attachment stops.  Stopping retains the
            // binding until deletion, so the test checks both lifecycle
            // phases without fabricating a Cue or an audio-daemon event.
            clear_line = dspp_delay_line_create(8u, 1u, 1);
            CHECK(clear_line != NULL);
            if (clear_line) {
                dspp_delay_line_set_clear_on_stop(clear_line, 1);
                CHECK(dspp_sim_dma_attach_delay_line(
                          delay_writer, writer_channel, clear_line, 1, 0u) ==
                      0);
                for (uint32_t i = 0; i < 4u; ++i) {
                    dspp_sim_poke_data(delay_writer, source_addr,
                                       (uint16_t)(400u + i));
                    dspp_sim_tick(delay_writer);
                }

                // Bind the tap after the writer has populated the line but
                // before either attachment is stopped.  This leaves two
                // live bindings without letting the shared DMA walker drain
                // the tap's FIFO during the writer's setup ticks.
                clear_tap = dspp_sim_create_in_space(&tap_template,
                                                     delay_space);
                CHECK(clear_tap != NULL);
                if (clear_tap) {
                    CHECK(test_dspp_find_alloc(clear_tap, "InFIFO", 5u,
                                               &clear_tap_channel) == 0);
                    CHECK(test_dspp_find_alloc(clear_tap, "Output", 4u,
                                               &clear_tap_output) == 0);
                    if (clear_tap_channel != UINT32_MAX &&
                        clear_tap_output != UINT32_MAX)
                        CHECK(dspp_sim_dma_attach_delay_line(
                                  clear_tap, clear_tap_channel, clear_line,
                                  0, 0u) == 0);
                }
                CHECK(dspp_sim_dma_stop_delay_line(delay_writer,
                                                   writer_channel) == 0);

                if (clear_tap && clear_tap_channel != UINT32_MAX &&
                    clear_tap_output != UINT32_MAX) {
                    // The active tap keeps the line uncleared after the
                    // writer stops; it still sees the written first word.
                    dspp_sim_tick(clear_tap);
                    CHECK(dspp_sim_peek_data(clear_tap,
                                             clear_tap_output) == 400u);
                    CHECK(dspp_sim_dma_stop_delay_line(
                              clear_tap, clear_tap_channel) == 0);
                    dspp_sim_dma_detach_delay_line(clear_tap,
                                                   clear_tap_channel);

                    // Both bindings were stopped before the reattachment,
                    // so AF_SAMPF_CLEARONSTOP has zeroed the line.
                    CHECK(dspp_sim_dma_attach_delay_line(
                              clear_tap, clear_tap_channel, clear_line, 0,
                              0u) == 0);
                    dspp_sim_tick(clear_tap);
                    CHECK(dspp_sim_peek_data(clear_tap,
                                             clear_tap_output) == 0u);
                    dspp_sim_dma_detach_delay_line(clear_tap,
                                                   clear_tap_channel);
                }
                if (clear_tap) {
                    dspp_sim_destroy(clear_tap);
                    clear_tap = NULL;
                }
                dspp_sim_dma_detach_delay_line(delay_writer, writer_channel);
            }
        }
        if (delay_writer) {
            dspp_sim_destroy(delay_writer);
            delay_writer = NULL;
        }
        if (delay_space && source_reserved)
            dspp_sim_space_release_words(delay_space, source_addr, 1u);

        if (delay_space && tap_loaded) {
            delay_tap = dspp_sim_create_in_space(&tap_template, delay_space);
            CHECK(delay_tap != NULL);
            if (delay_tap) {
                CHECK(test_dspp_find_alloc(delay_tap, "InFIFO", 5u,
                                           &tap_channel) == 0);
                CHECK(test_dspp_find_alloc(delay_tap, "Output", 4u,
                                           &tap_output) == 0);
            }
        }
        if (delay_space && drift_loaded) {
            drift_tap = dspp_sim_create_in_space(&drift_template, delay_space);
            CHECK(drift_tap != NULL);
            if (drift_tap) {
                CHECK(test_dspp_find_alloc(drift_tap, "InFIFO", 5u,
                                           &drift_channel) == 0);
                CHECK(test_dspp_find_alloc(drift_tap, "Output", 4u,
                                           &drift_output) == 0);
                CHECK(test_dspp_find_alloc(drift_tap, "Drift", 4u,
                                           &drift_probe) == 0);
                CHECK(test_dspp_find_alloc(drift_tap, "Phase", 2u,
                                           &drift_phase) == 0);
                CHECK(test_dspp_find_alloc(drift_tap, "Amplitude", 1u,
                                           &drift_amplitude) == 0);
                CHECK(test_dspp_find_alloc(drift_tap, "SampleRate", 1u,
                                           &drift_rate) == 0);
            }
        }
        if (drift_tap && delay_line && drift_channel != UINT32_MAX &&
            drift_output != UINT32_MAX && drift_probe != UINT32_MAX &&
            drift_phase != UINT32_MAX && drift_amplitude != UINT32_MAX &&
            drift_rate != UINT32_MAX) {
            // The original Sound3D tap is the source-defined variable-rate
            // sampler, not a raw FIFO copy: its sub_sampler dynamic link
            // uses the FIFO CURRENT/NEXT pair, advances the shared oscillator,
            // and exports the cumulative (#samples - #frames) Drift counter.
            // A physical delay-line binding supplies the same resident words
            // as the Sound3D LeftTapFIFO attachment.
            static const uint16_t expected_output[] = {
                0x0063u, 0x0064u, 0x0066u, 0x0067u};
            static const uint16_t expected_drift[] = {
                0x0000u, 0x0001u, 0x0001u, 0x0002u};
            static const uint16_t expected_phase[] = {
                0x4000u, 0x0000u, 0x4000u, 0x0000u};
            dspp_sim_poke_data(drift_tap, drift_amplitude, 0x7FFFu);
            dspp_sim_poke_data(drift_tap, drift_rate, 0xC000u);
            dspp_sim_poke_data(drift_tap, drift_phase, 0u);
            dspp_sim_poke_data(drift_tap, drift_probe, 0u);
            CHECK(dspp_sim_dma_attach_delay_line(drift_tap, drift_channel,
                                                 delay_line, 0, 0u) == 0);
            for (uint32_t i = 0; i < 4u; ++i) {
                dspp_sim_tick(drift_tap);
                CHECK(dspp_sim_peek_data(drift_tap, drift_output) ==
                      expected_output[i]);
                CHECK(dspp_sim_peek_data(drift_tap, drift_probe) ==
                      expected_drift[i]);
                CHECK(dspp_sim_peek_data(drift_tap, drift_phase) ==
                      expected_phase[i]);
            }
            dspp_sim_dma_detach_delay_line(drift_tap, drift_channel);
        }
        if (delay_tap && delay_line && tap_channel != UINT32_MAX &&
            tap_output != UINT32_MAX) {
            static const uint16_t expected[] = {
                103u, 104u, 105u, 106u, 107u, 100u, 101u, 102u};
            CHECK(dspp_sim_dma_attach_delay_line(delay_tap, tap_channel,
                                                 delay_line, 0, 3u) == 0);
            for (uint32_t i = 0; i < sizeof(expected) / sizeof(expected[0]);
                 ++i) {
                dspp_sim_tick(delay_tap);
                CHECK(dspp_sim_peek_data(delay_tap, tap_output) == expected[i]);
            }
            // Folio CLEARONSTOP uses the same physical storage; the bounded
            // seam exposes the clear operation without synthesizing a Cue or
            // attachment lifecycle event.
            dspp_sim_dma_detach_delay_line(delay_tap, tap_channel);
            dspp_delay_line_clear(delay_line);
            CHECK(dspp_sim_dma_attach_delay_line(delay_tap, tap_channel,
                                                 delay_line, 0, 0u) == 0);
            dspp_sim_tick(delay_tap);
            CHECK(dspp_sim_peek_data(delay_tap, tap_output) == 0u);
            dspp_sim_dma_detach_delay_line(delay_tap, tap_channel);
        }
        if (delay_tap && finite_line && tap_channel != UINT32_MAX &&
            tap_output != UINT32_MAX) {
            static const uint16_t expected_finite[] = {
                300u, 301u, 302u, 303u};
            CHECK(dspp_sim_dma_attach_delay_line(delay_tap, tap_channel,
                                                 finite_line, 0, 0u) == 0);
            for (uint32_t i = 0;
                 i < sizeof(expected_finite) / sizeof(expected_finite[0]);
                 ++i) {
                dspp_sim_tick(delay_tap);
                CHECK(dspp_sim_peek_data(delay_tap, tap_output) ==
                      expected_finite[i]);
            }
            dspp_sim_dma_detach_delay_line(delay_tap, tap_channel);
        }
        if (delay_tap) dspp_sim_destroy(delay_tap);
        if (drift_tap) dspp_sim_destroy(drift_tap);
        if (clear_tap) dspp_sim_destroy(clear_tap);
        if (delay_line) dspp_delay_line_destroy(delay_line);
        if (finite_line) dspp_delay_line_destroy(finite_line);
        if (clear_line) dspp_delay_line_destroy(clear_line);
        if (delay_space) dspp_sim_space_destroy(delay_space);
    }

    // ---- Stereo delay-line frame stride ------------------------------------
    // `delay_f2.dsp` emits a left/right pair into one physical FIFO and
    // `sampler_16_f2.dsp` consumes the same interleaved delay storage.  This
    // pins AF_TAG_START_AT in frames rather than raw words: a start frame of
    // one skips one complete stereo pair, and the loop returns to frame zero
    // after the fourth pair.
    {
        dspp_template delay_template;
        dspp_template tap_template;
        dspp_sim_space *delay_space = NULL;
        dspp_sim *delay_writer = NULL;
        dspp_sim *delay_tap = NULL;
        dspp_delay_line *delay_line = NULL;
        uint32_t source_addr = 0u;
        uint32_t writer_channel = UINT32_MAX;
        uint32_t tap_channel = UINT32_MAX;
        uint32_t tap_output = UINT32_MAX;
        uint32_t tap_amplitude = UINT32_MAX;
        int source_reserved = 0;
        int delay_loaded;
        int tap_loaded;

        memset(&delay_template, 0, sizeof(delay_template));
        memset(&tap_template, 0, sizeof(tap_template));
        delay_loaded = test_load_disc_dsp_template("delay_f2.dsp",
                                                   &delay_template) == 0;
        tap_loaded = test_load_disc_dsp_template("sampler_16_f2.dsp",
                                                 &tap_template) == 0;
        CHECK(delay_loaded && tap_loaded);
        if (delay_loaded && tap_loaded) {
            delay_space = dspp_sim_space_create();
            CHECK(delay_space != NULL);
            if (delay_space)
                delay_writer = dspp_sim_create_in_space(&delay_template,
                                                         delay_space);
            CHECK(delay_writer != NULL);
            if (delay_writer) {
                CHECK(test_dspp_find_alloc(delay_writer, "OutFIFO", 6u,
                                           &writer_channel) == 0);
                source_reserved =
                    dspp_sim_space_reserve_words(delay_space, 2u,
                                                 &source_addr) == 0;
                CHECK(source_reserved);
                if (source_reserved) {
                    dspp_sim_connect(delay_writer, "Input", 0,
                                     (uint16_t)(0x8000u | source_addr));
                    dspp_sim_connect(delay_writer, "Input", 1,
                                     (uint16_t)(0x8000u | (source_addr + 1u)));
                }
            }
        }
        if (delay_writer && writer_channel != UINT32_MAX &&
            source_reserved) {
            delay_line = dspp_delay_line_create(16u, 2u, 1);
            CHECK(delay_line != NULL);
            if (delay_line) {
                CHECK(dspp_sim_dma_attach_delay_line(
                          delay_writer, writer_channel, delay_line, 1, 0u) ==
                      0);
                for (uint32_t i = 0; i < 4u; ++i) {
                    dspp_sim_poke_data(delay_writer, source_addr,
                                       (uint16_t)(100u + i));
                    dspp_sim_poke_data(delay_writer, source_addr + 1u,
                                       (uint16_t)(200u + i));
                    dspp_sim_tick(delay_writer);
                }
                dspp_sim_dma_detach_delay_line(delay_writer, writer_channel);
            }
        }
        if (delay_writer) {
            dspp_sim_destroy(delay_writer);
            delay_writer = NULL;
        }
        if (delay_space && source_reserved)
            dspp_sim_space_release_words(delay_space, source_addr, 2u);

        if (delay_space && tap_loaded) {
            delay_tap = dspp_sim_create_in_space(&tap_template, delay_space);
            CHECK(delay_tap != NULL);
            if (delay_tap) {
                CHECK(test_dspp_find_alloc(delay_tap, "InFIFO", 5u,
                                           &tap_channel) == 0);
                CHECK(test_dspp_find_alloc(delay_tap, "Output", 4u,
                                           &tap_output) == 0);
                CHECK(test_dspp_find_alloc(delay_tap, "Amplitude", 1u,
                                           &tap_amplitude) == 0);
                // sampler_16_f2 declares this source default, while the
                // focused simulator fixture exposes resource initialization
                // explicitly.  Use the vendor default rather than a host
                // effect or an arbitrary gain.
                dspp_sim_poke_data(delay_tap, tap_amplitude, 32767u);
            }
        }
        if (delay_tap && delay_line && tap_channel != UINT32_MAX &&
            tap_output != UINT32_MAX) {
            // sampler_16_f2 applies its SF15 Amplitude knob even at the
            // vendor unity default ($7FFF), so each output is the truncated
            // one-LSB-below-unity product of the delay word.
            static const uint16_t expected_left[] = {100u, 101u, 102u, 99u};
            static const uint16_t expected_right[] = {200u, 201u, 202u, 199u};
            CHECK(dspp_sim_dma_attach_delay_line(delay_tap, tap_channel,
                                                 delay_line, 0, 1u) == 0);
            for (uint32_t i = 0; i < 4u; ++i) {
                uint16_t actual_left;
                uint16_t actual_right;
                dspp_sim_tick(delay_tap);
                actual_left = dspp_sim_peek_data(delay_tap, tap_output);
                actual_right = dspp_sim_peek_data(delay_tap, tap_output + 1u);
                CHECK(actual_left == expected_left[i]);
                CHECK(actual_right == expected_right[i]);
            }
            dspp_sim_dma_detach_delay_line(delay_tap, tap_channel);
        }
        if (delay_tap) dspp_sim_destroy(delay_tap);
        if (delay_line) dspp_delay_line_destroy(delay_line);
        if (delay_space) dspp_sim_space_destroy(delay_space);
    }

    // ---- Production Sound3D patch graph construction ----------------------
    // `SYNTHETICALLY_REACHED`: call the lifted MakeSound3DPatchTemplate with
    // its source-data string EAs, then let CreateInstrument and StartInstrument
    // exercise the real AddTemplate/CONNECT/EXPOSE graph. This is a bounded
    // constructor/lifecycle contract, not a route search. The static strings
    // are copied from the original Matrix_Perspective data image because this
    // unit test's guest RAM is otherwise intentionally zero-filled.
    {
        int available = 1;
        const char *const names[] = {
            "delay_f1.dsp", "sampler_drift_v1.dsp", "times_256.dsp",
            "timesplus_noclip.dsp", "subtract.dsp", "filter_3d.dsp",
            "multiply.dsp"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            dspp_template parsed;
            memset(&parsed, 0, sizeof(parsed));
            if (test_load_disc_dsp_template(names[i], &parsed) != 0)
                available = 0;
        }
        setup();
        if (available) {
            extern int32_t m2_guest_audio_resolve_attachment_channel(
                uint32_t attachment, uint32_t *channel_out);
            static const struct {
                uint32_t ea;
                const char *text;
            } sound3d_strings[] = {
                {0x16daecu, "delay_f1.dsp"},
                {0x16daf9u, "sampler_drift_v1.dsp"},
                {0x16db0eu, "times_256.dsp"},
                {0x16db1cu, "timesplus_noclip.dsp"},
                {0x16db31u, "subtract.dsp"},
                {0x16db3eu, "Send"},
                {0x16db43u, "LeftTap"},
                {0x16db4bu, "RightTap"},
                {0x16db54u, "LShift"},
                {0x16db5bu, "LTimesPlus"},
                {0x16db66u, "LMinus"},
                {0x16db6du, "RShift"},
                {0x16db74u, "RTimesPlus"},
                {0x16db7fu, "RMinus"},
                {0x16db86u, "SendFIFO"},
                {0x16db8fu, "OutFIFO"},
                {0x16db97u, "LeftTapFIFO"},
                {0x16dba3u, "InFIFO"},
                {0x16dbaau, "RightTapFIFO"},
                {0x16dbb7u, "Drift"},
                {0x16dbbdu, "DelayRate"},
                {0x16dbc7u, "Delay"},
                {0x16dbcdU, "Amplitude"},
                {0x16dbd7u, "envelope.dsp"},
                {0x16dbe4u, "REnvAmp"},
                {0x16dbecu, "LEnvAmp"},
                {0x16dbf4u, "AmpProbe"},
                {0x16dbfdu, "EnvRate"},
                {0x16dc05u, "InputA"},
                {0x16dc0cu, "Output"},
                {0x16dc13u, "Input"},
                {0x16dc19u, "InputB"},
                {0x16dc20u, "InputC"},
                {0x16dc27u, "SampleRate"},
                {0x16dc32u, "Env.Request"},
                {0x16dc3eu, "Env.Incr"},
                {0x16dc47u, "multiply.dsp"},
                {0x16dc54u, "LAmp"},
                {0x16dc59u, "RAmp"},
                {0x16dc5eu, "filter_3d.dsp"},
                {0x16dc6cu, "LFilter"},
                {0x16dc74u, "RFilter"},
                {0x16dc7cu, "Alpha"},
                {0x16dc82u, "Beta"},
                {0x16dc87u, "Feed"},
            };
            for (size_t i = 0;
                 i < sizeof(sound3d_strings) / sizeof(sound3d_strings[0]);
                 ++i)
                write_guest_string(sound3d_strings[i].ea,
                                   sound3d_strings[i].text);
            // The constructor loads these source-backed float literals for
            // DelayRate/Delay/Amplitude defaults and SetPatchConstant.  They
            // precede the string table in the original Matrix_Perspective
            // data image; restore them in the zero-filled fixture as well.
            static const struct {
                uint32_t ea;
                uint32_t bits;
            } sound3d_float_literals[] = {
                {0x16dae0u, 0xbf800000u}, // -1.0
                {0x16dae4u, 0x00000000u}, // 0.0
                {0x16dae8u, 0x3f800000u}, // 1.0
            };
            for (size_t i = 0;
                 i < sizeof(sound3d_float_literals) /
                         sizeof(sound3d_float_literals[0]);
                 ++i)
                wr32(&g_cpu, sound3d_float_literals[i].ea,
                     sound3d_float_literals[i].bits);
            // OpenAudioPatchFolio imports the original folio by the static
            // name at Matrix_Perspective data EA 0x173e48.  The fixture's
            // guest RAM is otherwise zero-filled, so preserve that source
            // string explicitly as well.
            write_guest_string(0x173e48u, "audiopatch");
            // The lifted builder helpers copy their first command word from
            // the original Matrix_Perspective .data prototypes before
            // overwriting their argument fields.  Restore those source
            // command IDs in the zero-filled fixture; the remaining prototype
            // words are the original zero values and need no write.
            static const struct {
                uint32_t ea;
                uint32_t command;
            } sound3d_patch_prototypes[] = {
                {0x1810d0u, 256u}, // ADD_TEMPLATE
                {0x1810f0u, 260u}, // CONNECT
                {0x181110u, 258u}, // DEFINE_KNOB
                {0x181130u, 257u}, // DEFINE_PORT
                {0x181150u, 259u}, // EXPOSE
                {0x181170u, 261u}, // SET_CONSTANT
            };
            for (size_t i = 0;
                 i < sizeof(sound3d_patch_prototypes) /
                         sizeof(sound3d_patch_prototypes[0]);
                 ++i)
                wr32(&g_cpu, sound3d_patch_prototypes[i].ea,
                     sound3d_patch_prototypes[i].command);

            const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
            m2_kernel_dispatch_set_global(hdl);
            // Authoring in sndInit3DSounds@0x294e0 sets s3dFlags = 0x6d
            // (S3D_F_PAN_DELAY | S3D_F_PAN_FILTER | S3D_F_DISTANCE_AMPLITUDE_SQUARE |
            //  S3D_F_OUTPUT_HEADPHONES | S3D_F_DOPPLER).
            g_cpu.gpr[3] = 0x6du;
            FN_MakeSound3DPatchTemplate__6c8a8(&g_cpu);
            const uint32_t sound3d_template = g_cpu.gpr[3];
            CHECK(sound3d_template != M2K_ITEM_NONE);
            CHECK(hdl->CheckItem(sound3d_template) == 0);

            g_cpu.gpr[3] = sound3d_template;
            g_cpu.gpr[4] = 0u;
            FN_CreateInstrument__73734(&g_cpu);
            const uint32_t sound3d_instrument = g_cpu.gpr[3];
            CHECK(sound3d_instrument != M2K_ITEM_NONE);
            CHECK(hdl->CheckItem(sound3d_instrument) == 0);
            m2_kernel_dispatch_set_global(NULL);

            uint32_t blocks = UINT32_MAX;
            CHECK(m2_guest_audio_get_instrument_patch_block_count(
                      sound3d_instrument, &blocks) == M2K_OK);
            // 0x6d creates 11 constituent blocks: Send, LeftTap, RightTap,
            // LShift, LTimesPlus, LMinus, RShift, RTimesPlus, RMinus,
            // LFilter, RFilter.
            CHECK(blocks == 11u);

            CHECK(m2_guest_audio_start_instrument(
                      &g_cpu, sound3d_instrument, 0u) == M2K_OK);

            // Confirm an exposed constituent FIFO resolves through the
            // source graph after startup. The temporary attachment is not
            // started; it only exercises the retained hook lookup and is
            // deleted before graph teardown.
            m2_kernel_dispatch_set_global(hdl);
            g_cpu.gpr[3] = 200u;
            g_cpu.gpr[4] = 1u;
            g_cpu.gpr[5] = 1u;
            FN_CreateDelayLine__733e0(&g_cpu);
            const uint32_t probe_delay = g_cpu.gpr[3];
            const uint32_t probe_attachment_tags = 0x16e000u;
            wr32(&g_cpu, probe_attachment_tags + 0u, M2_AUDIO_TAG_NAME);
            wr32(&g_cpu, probe_attachment_tags + 4u, 0x16db97u);
            wr32(&g_cpu, probe_attachment_tags + 8u, M2_TAG_END);
            wr32(&g_cpu, probe_attachment_tags + 12u, 0u);
            g_cpu.gpr[3] = sound3d_instrument;
            g_cpu.gpr[4] = probe_delay;
            g_cpu.gpr[5] = probe_attachment_tags;
            FN_CreateAttachment__73394(&g_cpu);
            const uint32_t probe_attachment = g_cpu.gpr[3];
            m2_kernel_dispatch_set_global(NULL);
            CHECK(probe_delay != M2K_ITEM_NONE);
            CHECK(probe_attachment != M2K_ITEM_NONE);
            if (probe_attachment != M2K_ITEM_NONE) {
                uint32_t channel = UINT32_MAX;
                CHECK(m2_guest_audio_resolve_attachment_channel(
                          probe_attachment, &channel) == M2K_OK);
                CHECK(channel < 32u);
                hdl->DeleteItem(probe_attachment);
            }
            if (probe_delay != M2K_ITEM_NONE) hdl->DeleteItem(probe_delay);

            // `SYNTHETICALLY_REACHED`: drive the original source-defined
            // 11-block production graph with an original add.dsp source.
            if (test_dsp_raw_open("System.m2/Audio/dsp/add.dsp") != 0) {
                const uint32_t src_name = 0x16e100u;
                const uint32_t src_ina = 0x16e120u;
                const uint32_t src_inb = 0x16e140u;
                const uint32_t src_out = 0x16e160u;
                write_guest_string(src_name, "add.dsp");
                write_guest_string(src_ina, "InputA");
                write_guest_string(src_inb, "InputB");
                write_guest_string(src_out, "Output");
                const int32_t src = m2_guest_audio_load_instrument(
                    &g_cpu, src_name, 1u, 100u);
                CHECK(src > 0);
                if (src > 0) {
                    CHECK(m2_guest_audio_start_instrument(
                              &g_cpu, (uint32_t)src, 0u) == M2K_OK);
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = (uint32_t)src;
                    g_cpu.gpr[4] = src_ina;
                    g_cpu.gpr[5] = 0u;
                    FN_CreateKnob__73778(&g_cpu);
                    const uint32_t ka = g_cpu.gpr[3];
                    g_cpu.gpr[3] = (uint32_t)src;
                    g_cpu.gpr[4] = src_inb;
                    g_cpu.gpr[5] = 0u;
                    FN_CreateKnob__73778(&g_cpu);
                    const uint32_t kb = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);
                    CHECK(ka != M2K_ITEM_NONE && kb != M2K_ITEM_NONE);
                    CHECK(m2_guest_audio_set_knob_part(
                              &g_cpu, ka, 0, 2000.0f / 32768.0f) == M2K_OK);
                    CHECK(m2_guest_audio_set_knob_part(
                              &g_cpu, kb, 0, 0.0f) == M2K_OK);
                    CHECK(m2_guest_audio_connect_instrument_parts(
                              &g_cpu, (uint32_t)src, src_out, 0,
                              sound3d_instrument, 0x16dc13u, 0) == M2K_OK);
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = sound3d_instrument;
                    g_cpu.gpr[4] = 0x16dc0cu; // Output
                    g_cpu.gpr[5] = 0u;
                    FN_CreateProbe__7392c(&g_cpu);
                    const uint32_t output_probe = g_cpu.gpr[3];
                    g_cpu.gpr[3] = sound3d_instrument;
                    g_cpu.gpr[4] = 0x16dbb7u; // Drift
                    g_cpu.gpr[5] = 0u;
                    FN_CreateProbe__7392c(&g_cpu);
                    const uint32_t drift_probe = g_cpu.gpr[3];
                    // Create Feed knob on sound3d_instrument to verify live filter_3d simulation
                    g_cpu.gpr[3] = sound3d_instrument;
                    g_cpu.gpr[4] = 0x16dc87u; // Feed
                    g_cpu.gpr[5] = 0u;
                    FN_CreateKnob__73778(&g_cpu);
                    const uint32_t feed_knob = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);

                    CHECK(output_probe != M2K_ITEM_NONE);
                    CHECK(drift_probe != M2K_ITEM_NONE);
                    CHECK(feed_knob != M2K_ITEM_NONE);
                    m2_guest_audio_tick_dspp_frames(128u);
                    uint32_t output_value_ea = 0x16e180u;
                    if (output_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, output_probe, 0,
                                  output_value_ea) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea) ==
                              1998.0f / 32768.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, output_probe, 1,
                                  output_value_ea + 4u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 4u) ==
                              1998.0f / 32768.0f);
                    }
                    if (drift_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, drift_probe, 0,
                                  output_value_ea + 8u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 8u) == 0.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, drift_probe, 1,
                                  output_value_ea + 12u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 12u) == 0.0f);
                        hdl->DeleteItem(drift_probe);
                    }

                    // Live knob response on filter_3d constituent: set Feed part 0 to 0.5f.
                    // Default filter_3d equation: y(n) = Feed * x(n) + Beta * x(n-3) + Alpha * y(n-1).
                    // Feed=0.5 scales part 0 output to 999.0f / 32768.0f while part 1 stays 1998.0f / 32768.0f.
                    if (feed_knob != M2K_ITEM_NONE && output_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_set_knob_part(
                                  &g_cpu, feed_knob, 0, 0.5f) == M2K_OK);
                        m2_guest_audio_tick_dspp_frames(1u);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, output_probe, 0,
                                  output_value_ea) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea) ==
                              999.0f / 32768.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, output_probe, 1,
                                  output_value_ea + 4u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 4u) ==
                              1998.0f / 32768.0f);
                        hdl->DeleteItem(feed_knob);
                    }
                    if (output_probe != M2K_ITEM_NONE) hdl->DeleteItem(output_probe);

                    // Vendor delay-attachment ReleaseInstrument boundary:
                    g_cpu.gpr[3] = sound3d_instrument;
                    g_cpu.gpr[4] = 0u;
                    CHECK(m2_audio_dispatch_c(&g_cpu, 68u, 0x2aa44u) == 1);
                    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);

                    // Disconnect src from sound3d_instrument before teardown
                    m2_guest_audio_disconnect_instrument_parts(
                        &g_cpu, sound3d_instrument, 0x16dc13u, 0);

                    // Teardown: delete instrument and template, verify cleanup
                    hdl->DeleteItem(sound3d_instrument);
                    CHECK(hdl->CheckItem(sound3d_instrument) != 0);
                    hdl->DeleteItem(sound3d_template);
                    CHECK(hdl->CheckItem(sound3d_template) != 0);

                    // ---- Alternative Sound3D Form 1: S3D_F_PAN_FILTER (0x4) ----
                    // 2 constituent blocks (LFilter, RFilter), direct Input -> Filter -> Output.
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = 4u;
                    FN_MakeSound3DPatchTemplate__6c8a8(&g_cpu);
                    const uint32_t filter_tmpl = g_cpu.gpr[3];
                    CHECK(filter_tmpl != M2K_ITEM_NONE);
                    g_cpu.gpr[3] = filter_tmpl;
                    g_cpu.gpr[4] = 0u;
                    FN_CreateInstrument__73734(&g_cpu);
                    const uint32_t filter_inst = g_cpu.gpr[3];
                    CHECK(filter_inst != M2K_ITEM_NONE);
                    m2_kernel_dispatch_set_global(NULL);

                    uint32_t f_blocks = UINT32_MAX;
                    CHECK(m2_guest_audio_get_instrument_patch_block_count(
                              filter_inst, &f_blocks) == M2K_OK);
                    CHECK(f_blocks == 2u);

                    CHECK(m2_guest_audio_start_instrument(&g_cpu, filter_inst, 0u) == M2K_OK);
                    CHECK(m2_guest_audio_connect_instrument_parts(
                              &g_cpu, (uint32_t)src, src_out, 0,
                              filter_inst, 0x16dc13u, 0) == M2K_OK);
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = filter_inst;
                    g_cpu.gpr[4] = 0x16dc0cu; // Output
                    g_cpu.gpr[5] = 0u;
                    FN_CreateProbe__7392c(&g_cpu);
                    const uint32_t f_out_probe = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);
                    CHECK(f_out_probe != M2K_ITEM_NONE);
                    m2_guest_audio_tick_dspp_frames(4u);
                    if (f_out_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, f_out_probe, 0, output_value_ea) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea) == 1999.0f / 32768.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, f_out_probe, 1, output_value_ea + 4u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 4u) == 1999.0f / 32768.0f);
                        hdl->DeleteItem(f_out_probe);
                    }
                    m2_guest_audio_disconnect_instrument_parts(&g_cpu, filter_inst, 0x16dc13u, 0);
                    hdl->DeleteItem(filter_inst);
                    hdl->DeleteItem(filter_tmpl);

                    // ---- Alternative Sound3D Form 2: S3D_F_PAN_AMPLITUDE (0x2) ----
                    // 2 constituent blocks (LAmp, RAmp from multiply.dsp), Amplitude knob.
                    // Input=2000 yields 1000 at amplitude 0.5 and 1999 at
                    // the default SF15 unity value (0x7fff).
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = 2u;
                    FN_MakeSound3DPatchTemplate__6c8a8(&g_cpu);
                    const uint32_t amp_tmpl = g_cpu.gpr[3];
                    CHECK(amp_tmpl != M2K_ITEM_NONE);
                    g_cpu.gpr[3] = amp_tmpl;
                    g_cpu.gpr[4] = 0u;
                    FN_CreateInstrument__73734(&g_cpu);
                    const uint32_t amp_inst = g_cpu.gpr[3];
                    CHECK(amp_inst != M2K_ITEM_NONE);
                    g_cpu.gpr[3] = amp_inst;
                    g_cpu.gpr[4] = 0x16dbcdU; // Amplitude
                    g_cpu.gpr[5] = 0u;
                    FN_CreateKnob__73778(&g_cpu);
                    const uint32_t amp_knob = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);

                    uint32_t a_blocks = UINT32_MAX;
                    CHECK(m2_guest_audio_get_instrument_patch_block_count(
                              amp_inst, &a_blocks) == M2K_OK);
                    CHECK(a_blocks == 2u);
                    CHECK(amp_knob != M2K_ITEM_NONE);
                    CHECK(m2_guest_audio_set_knob_part(&g_cpu, amp_knob, 0, 0.5f) == M2K_OK);
                    CHECK(m2_guest_audio_set_knob_part(&g_cpu, amp_knob, 1, 1.0f) == M2K_OK);

                    CHECK(m2_guest_audio_start_instrument(&g_cpu, amp_inst, 0u) == M2K_OK);
                    CHECK(m2_guest_audio_connect_instrument_parts(
                              &g_cpu, (uint32_t)src, src_out, 0,
                              amp_inst, 0x16dc13u, 0) == M2K_OK);
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = amp_inst;
                    g_cpu.gpr[4] = 0x16dc0cu; // Output
                    g_cpu.gpr[5] = 0u;
                    FN_CreateProbe__7392c(&g_cpu);
                    const uint32_t amp_out_probe = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);
                    CHECK(amp_out_probe != M2K_ITEM_NONE);
                    m2_guest_audio_tick_dspp_frames(4u);
                    if (amp_out_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, amp_out_probe, 0, output_value_ea) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea) == 1000.0f / 32768.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, amp_out_probe, 1, output_value_ea + 4u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 4u) == 1999.0f / 32768.0f);
                        hdl->DeleteItem(amp_out_probe);
                    }
                    if (amp_knob != M2K_ITEM_NONE) hdl->DeleteItem(amp_knob);
                    m2_guest_audio_disconnect_instrument_parts(&g_cpu, amp_inst, 0x16dc13u, 0);
                    hdl->DeleteItem(amp_inst);
                    hdl->DeleteItem(amp_tmpl);

                    // ---- Alternative Sound3D Form 3: Passthrough (0) ----
                    // 0 constituent blocks, direct Input[0] -> Output[0] and Output[1].
                    // The source value 2000 reaches both outputs unchanged.
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = 0u;
                    FN_MakeSound3DPatchTemplate__6c8a8(&g_cpu);
                    const uint32_t pass_tmpl = g_cpu.gpr[3];
                    CHECK(pass_tmpl != M2K_ITEM_NONE);
                    g_cpu.gpr[3] = pass_tmpl;
                    g_cpu.gpr[4] = 0u;
                    FN_CreateInstrument__73734(&g_cpu);
                    const uint32_t pass_inst = g_cpu.gpr[3];
                    CHECK(pass_inst != M2K_ITEM_NONE);
                    m2_kernel_dispatch_set_global(NULL);

                    uint32_t p_blocks = UINT32_MAX;
                    CHECK(m2_guest_audio_get_instrument_patch_block_count(
                              pass_inst, &p_blocks) == M2K_OK);
                    CHECK(p_blocks == 0u);

                    CHECK(m2_guest_audio_start_instrument(&g_cpu, pass_inst, 0u) == M2K_OK);
                    CHECK(m2_guest_audio_connect_instrument_parts(
                              &g_cpu, (uint32_t)src, src_out, 0,
                              pass_inst, 0x16dc13u, 0) == M2K_OK);
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = pass_inst;
                    g_cpu.gpr[4] = 0x16dc0cu; // Output
                    g_cpu.gpr[5] = 0u;
                    FN_CreateProbe__7392c(&g_cpu);
                    const uint32_t pass_out_probe = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);
                    CHECK(pass_out_probe != M2K_ITEM_NONE);
                    m2_guest_audio_tick_dspp_frames(4u);
                    if (pass_out_probe != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, pass_out_probe, 0, output_value_ea) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea) == 2000.0f / 32768.0f);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, pass_out_probe, 1, output_value_ea + 4u) == M2K_OK);
                        CHECK(rdf32(&g_cpu, output_value_ea + 4u) == 2000.0f / 32768.0f);
                        hdl->DeleteItem(pass_out_probe);
                    }
                    // A started pure-synth graph with no sample attachment
                    // reaches the vendor DSPPReleaseInstrument empty-FIFO
                    // result. It must not be reported as UNIMPLEMENTED just
                    // because no host voice was available to release.
                    g_cpu.gpr[3] = pass_inst;
                    g_cpu.gpr[4] = 0u;
                    CHECK(m2_audio_dispatch_c(&g_cpu, 68u, 0x2aa44u) == 1);
                    CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
                    m2_guest_audio_disconnect_instrument_parts(&g_cpu, pass_inst, 0x16dc13u, 0);
                    hdl->DeleteItem(pass_inst);
                    hdl->DeleteItem(pass_tmpl);

                    hdl->DeleteItem(ka);
                    hdl->DeleteItem(kb);
                    CHECK(m2_guest_audio_unload_instrument((uint32_t)src) ==
                          M2K_OK);
                }
            } else {
                hdl->DeleteItem(sound3d_instrument);
                hdl->DeleteItem(sound3d_template);
            }
        } else {
            printf("m2_audio_items_test: Sound3D constituent DSP templates "
                   "not found; production graph regression skipped\n");
        }
        teardown();
    }

    // ---- Source-defined graph execution -----------------------------------
    // `SYNTHETICALLY_REACHED`: this focused graph uses the original add.dsp
    // image as both a constituent and an independent source.  The fixture
    // proves the production patch parser/allocator, a patch-scope INPUT
    // connection, a source-defined SET_CONSTANT, shared DSPP data space, and
    // patch OUTPUT probe readback.  It is an architectural graph contract,
    // not a route search or a replacement game path.
    {
        const int available = test_dsp_raw_open(
                                  "System.m2/Audio/dsp/add.dsp") != 0;
        setup();
        if (available) {
            const m2_kernel_dispatch *hdl = m2_guest_kernel_handlers();
            const uint32_t add_template_name_ea = 0x16e100u;
            const uint32_t add_block_name_ea = 0x16e120u;
            const uint32_t input_name_ea = 0x16e140u;
            const uint32_t output_name_ea = 0x16e160u;
            const uint32_t input_a_name_ea = 0x16e180u;
            const uint32_t input_b_name_ea = 0x16e1a0u;
            const uint32_t source_name_ea = 0x16e1c0u;
            const uint32_t patch_cmds_ea = 0x16e200u;
            const uint32_t patch_tags_ea = 0x16e300u;
            const uint32_t source_input_a_name_ea = 0x16e320u;
            const uint32_t source_input_b_name_ea = 0x16e340u;
            const uint32_t source_output_name_ea = 0x16e360u;
            const uint32_t probe_value_ea = 0x16e380u;

            write_guest_string(add_template_name_ea, "add.dsp");
            write_guest_string(add_block_name_ea, "Add");
            write_guest_string(input_name_ea, "Input");
            write_guest_string(output_name_ea, "Output");
            write_guest_string(input_a_name_ea, "InputA");
            write_guest_string(input_b_name_ea, "InputB");
            write_guest_string(source_name_ea, "add.dsp");
            write_guest_string(source_input_a_name_ea, "InputA");
            write_guest_string(source_input_b_name_ea, "InputB");
            write_guest_string(source_output_name_ea, "Output");

            const int32_t add_template = m2_guest_audio_load_ins_template(
                &g_cpu, add_template_name_ea, 0u);
            CHECK(add_template > 0);

            // PatchCmd is the 28-byte vendor structure used by the lifted
            // AudioPatch helpers: command, four/five u32 arguments, and the
            // float payload where applicable.
            if (add_template > 0) {
                wr32(&g_cpu, patch_cmds_ea + 0u, 257u); // DEFINE_PORT Input
                wr32(&g_cpu, patch_cmds_ea + 4u, input_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 8u, 1u);
                wr32(&g_cpu, patch_cmds_ea + 12u, 0u); // INPUT
                wr32(&g_cpu, patch_cmds_ea + 16u, 0u); // GENERIC_SIGNED

                wr32(&g_cpu, patch_cmds_ea + 28u, 257u); // DEFINE_PORT Output
                wr32(&g_cpu, patch_cmds_ea + 32u, output_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 36u, 1u);
                wr32(&g_cpu, patch_cmds_ea + 40u, 1u); // OUTPUT
                wr32(&g_cpu, patch_cmds_ea + 44u, 0u); // GENERIC_SIGNED

                wr32(&g_cpu, patch_cmds_ea + 56u, 256u); // ADD_TEMPLATE
                wr32(&g_cpu, patch_cmds_ea + 60u, add_block_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 64u, (uint32_t)add_template);

                wr32(&g_cpu, patch_cmds_ea + 84u, 260u); // Input -> Add.InputA
                wr32(&g_cpu, patch_cmds_ea + 88u, 0u);
                wr32(&g_cpu, patch_cmds_ea + 92u, input_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 96u, 0u);
                wr32(&g_cpu, patch_cmds_ea + 100u, add_block_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 104u, input_a_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 108u, 0u);

                wr32(&g_cpu, patch_cmds_ea + 112u, 260u); // Add.Output -> Output
                wr32(&g_cpu, patch_cmds_ea + 116u, add_block_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 120u, output_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 124u, 0u);
                wr32(&g_cpu, patch_cmds_ea + 128u, 0u);
                wr32(&g_cpu, patch_cmds_ea + 132u, output_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 136u, 0u);

                wr32(&g_cpu, patch_cmds_ea + 140u, 261u); // SET_CONSTANT
                wr32(&g_cpu, patch_cmds_ea + 144u, add_block_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 148u, input_b_name_ea);
                wr32(&g_cpu, patch_cmds_ea + 152u, 0u);
                wrf32(&g_cpu, patch_cmds_ea + 156u, 1000.0f / 32768.0f);
                wr32(&g_cpu, patch_cmds_ea + 168u, 0u); // END

                const int32_t patch_template =
                    m2_guest_audiopatch_create_patch_template(
                        &g_cpu, patch_cmds_ea, patch_tags_ea);
                CHECK(patch_template > 0);
                if (patch_template > 0) {
                    m2_kernel_dispatch_set_global(hdl);
                    g_cpu.gpr[3] = (uint32_t)patch_template;
                    g_cpu.gpr[4] = 0u;
                    FN_CreateInstrument__73734(&g_cpu);
                    const uint32_t patch_instrument = g_cpu.gpr[3];
                    m2_kernel_dispatch_set_global(NULL);
                    CHECK(patch_instrument != M2K_ITEM_NONE);

                    const int32_t source_instrument =
                        m2_guest_audio_load_instrument(
                            &g_cpu, source_name_ea, 1u, 100u);
                    CHECK(source_instrument > 0);
                    if (source_instrument > 0 &&
                        patch_instrument != M2K_ITEM_NONE) {
                        CHECK(m2_guest_audio_start_instrument(
                                  &g_cpu, (uint32_t)source_instrument, 0u) ==
                              M2K_OK);

                        m2_kernel_dispatch_set_global(hdl);
                        g_cpu.gpr[3] = (uint32_t)source_instrument;
                        g_cpu.gpr[4] = source_input_a_name_ea;
                        g_cpu.gpr[5] = 0u;
                        FN_CreateKnob__73778(&g_cpu);
                        const uint32_t source_input_a = g_cpu.gpr[3];
                        g_cpu.gpr[3] = (uint32_t)source_instrument;
                        g_cpu.gpr[4] = source_input_b_name_ea;
                        g_cpu.gpr[5] = 0u;
                        FN_CreateKnob__73778(&g_cpu);
                        const uint32_t source_input_b = g_cpu.gpr[3];
                        m2_kernel_dispatch_set_global(NULL);
                        CHECK(source_input_a != M2K_ITEM_NONE);
                        CHECK(source_input_b != M2K_ITEM_NONE);
                        CHECK(m2_guest_audio_set_knob_part(
                                  &g_cpu, source_input_a, 0,
                                  2000.0f / 32768.0f) == M2K_OK);
                        CHECK(m2_guest_audio_set_knob_part(
                                  &g_cpu, source_input_b, 0, 0.0f) == M2K_OK);
                        CHECK(m2_guest_audio_connect_instrument_parts(
                                  &g_cpu, (uint32_t)source_instrument,
                                  source_output_name_ea, 0,
                                  patch_instrument, input_name_ea, 0) ==
                              M2K_OK);

                        CHECK(m2_guest_audio_start_instrument(
                                  &g_cpu, patch_instrument, 0u) == M2K_OK);
                        m2_kernel_dispatch_set_global(hdl);
                        g_cpu.gpr[3] = patch_instrument;
                        g_cpu.gpr[4] = output_name_ea;
                        g_cpu.gpr[5] = 0u;
                        FN_CreateProbe__7392c(&g_cpu);
                        const uint32_t patch_probe = g_cpu.gpr[3];
                        m2_kernel_dispatch_set_global(NULL);
                        CHECK(patch_probe != M2K_ITEM_NONE);

                        // The source sim ticks before the graph node.  The
                        // graph then reads source Output=2000, adds its
                        // source-defined constant 1000, and exposes 3000.
                        m2_guest_audio_tick_dspp_frames(1u);
                        CHECK(m2_guest_audio_read_probe_part(
                                  &g_cpu, patch_probe, 0, probe_value_ea) ==
                              M2K_OK);
                        CHECK(rdf32(&g_cpu, probe_value_ea) ==
                              3000.0f / 32768.0f);
                    }
                    if (source_instrument > 0)
                        CHECK(m2_guest_audio_unload_instrument(
                                  (uint32_t)source_instrument) == M2K_OK);
                    hdl->DeleteItem(patch_instrument);
                    hdl->DeleteItem((uint32_t)patch_template);
                }
            }
        } else {
            printf("m2_audio_items_test: add.dsp not found; source-defined "
                   "graph execution regression skipped\n");
        }
        teardown();
    }

    // ---- R4.5 race-audio branch: swap/restart/teardown voice invariant -----
    // Static guest evidence (docs/research/race-audio-branch.md): every race
    // setter replaces its channel's source on the SAME instrument, and
    // sndKillSnds tears the channel down, through this exact folio-6 selector
    // set, so the identified race-audio branch never fabricates a second
    // channel:
    //   sndSetEng 0x2ad24, sndSet2StageEng 0x2aa64, sndSetScrape 0x2b248:
    //     ReleaseInstrument(68) -> StartInstrument(69) -> SetKnobPart(70)
    //   sndSetSkid 0x2afc0, sndSetCrash 0x2b330, sndSetRoad 0x2b438:
    //     StopInstrument(67) -> StartInstrument(69) -> SetKnobPart(70)
    //   sndKillSnds 0x2baec: StopInstrument(67) for every live channel
    // A persistent unchanging drone is the fingerprint of a channel start that
    // strands its predecessor's host voice, so this block pins exactly one
    // live voice across the branch swap and the repeat start, and zero after
    // the teardown.
    {
        setup();
        CHECK(m2_is_audio_item_selector(17));        // UnloadInstrument
        CHECK(m2_is_audio_item_selector(27));        // LoadInstrument
        CHECK(m2_is_audio_frame_count_selector(37)); // GetAudioFrameCount
        CHECK(m2_is_audio_status_selector(60));      // ReadKnobPart
        CHECK(m2_is_audio_status_selector(67));      // StopInstrument
        CHECK(m2_is_audio_status_selector(68));      // ReleaseInstrument
        CHECK(m2_is_audio_status_selector(69));      // StartInstrument
        CHECK(m2_is_audio_status_selector(70));      // SetKnobPart

        // Authentic finite race-channel sample: plain PCM with absent sustain
        // and release bounds, which is the engine form (the disc carries no
        // release ranges and the engine channels are the no-loop form).
        const uint32_t race_tags_ea = 0x9C00u;
        const uint32_t race_data_ea = 0x9D00u;
        for (uint32_t i = 0; i < 8u; ++i)
            g_mem[race_data_ea + i] = (uint8_t)(i + 1u);
        wr32(&g_cpu, race_tags_ea + 0u, M2_AUDIO_TAG_ADDRESS);
        wr32(&g_cpu, race_tags_ea + 4u, race_data_ea);
        wr32(&g_cpu, race_tags_ea + 8u, M2_AUDIO_TAG_SAMPLE_RATE_FP);
        wr32(&g_cpu, race_tags_ea + 12u, 0x46ac4400u); // 22050 Hz
        wr32(&g_cpu, race_tags_ea + 16u, M2_AUDIO_TAG_CHANNELS);
        wr32(&g_cpu, race_tags_ea + 20u, 1u);
        wr32(&g_cpu, race_tags_ea + 24u, M2_AUDIO_TAG_COMPRESSIONRATIO);
        wr32(&g_cpu, race_tags_ea + 28u, 1u);
        wr32(&g_cpu, race_tags_ea + 32u, M2_AUDIO_TAG_COMPRESSIONTYPE);
        wr32(&g_cpu, race_tags_ea + 36u, 0u);
        wr32(&g_cpu, race_tags_ea + 40u, M2_AUDIO_TAG_NUMBITS);
        wr32(&g_cpu, race_tags_ea + 44u, 16u);
        wr32(&g_cpu, race_tags_ea + 48u, M2_AUDIO_TAG_NUMBYTES);
        wr32(&g_cpu, race_tags_ea + 52u, 8u);
        wr32(&g_cpu, race_tags_ea + 56u, M2_AUDIO_TAG_RELEASEBEGIN);
        wr32(&g_cpu, race_tags_ea + 60u, 0xFFFFFFFFu);
        wr32(&g_cpu, race_tags_ea + 64u, M2_AUDIO_TAG_RELEASEEND);
        wr32(&g_cpu, race_tags_ea + 68u, 0xFFFFFFFFu);
        wr32(&g_cpu, race_tags_ea + 72u, M2_AUDIO_TAG_SUSTAINBEGIN);
        wr32(&g_cpu, race_tags_ea + 76u, 0xFFFFFFFFu);
        wr32(&g_cpu, race_tags_ea + 80u, M2_AUDIO_TAG_SUSTAINEND);
        wr32(&g_cpu, race_tags_ea + 84u, 0xFFFFFFFFu);
        wr32(&g_cpu, race_tags_ea + 88u, M2_TAG_END);
        wr32(&g_cpu, race_tags_ea + 92u, 0u);
        const uint32_t race_uploads_before = g_sample_uploads;
        uint32_t race_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                               race_tags_ea);
        CHECK(race_sample != M2K_ITEM_NONE);
        CHECK(g_sample_uploads == race_uploads_before + 1u);
        const uint32_t race_sid = g_next_sid;

        const uint32_t race_name_ea = 0x9E80u;
        const uint32_t race_amp_ea = 0x9EA0u;
        const uint32_t race_rate_ea = 0x9EC0u;
        write_guest_string(race_name_ea, "sampler_sqs2_v1.dsp");
        write_guest_string(race_amp_ea, "Amplitude");
        write_guest_string(race_rate_ea, "SampleRate");
        g_cpu.gpr[3] = race_name_ea;
        g_cpu.gpr[4] = 1u;
        g_cpu.gpr[5] = 100u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 27, 0x29614u) == 1);
        const int32_t race_ins = (int32_t)g_cpu.gpr[3];
        CHECK(race_ins > 0);

        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = race_sample;
        g_cpu.gpr[5] = 0u;
        FN_CreateAttachment__73394(&g_cpu);
        const uint32_t race_att = g_cpu.gpr[3];
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = race_amp_ea;
        g_cpu.gpr[5] = 0u;
        FN_CreateKnob__73778(&g_cpu);
        const uint32_t race_amp = g_cpu.gpr[3];
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = race_rate_ea;
        g_cpu.gpr[5] = 0u;
        FN_CreateKnob__73778(&g_cpu);
        const uint32_t race_rate = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(race_att != M2K_ITEM_NONE);
        CHECK(race_amp != M2K_ITEM_NONE);
        CHECK(race_rate != M2K_ITEM_NONE);

        // (1) Channel start, as sndSetEng authors it: the two knob writes land
        // while stopped (dsppSetKnob semantics) and the start then uses them.
        g_cpu.gpr[3] = race_amp;
        g_cpu.gpr[4] = 0u;
        g_cpu.fpr[1] = 0.25f;
        CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x2adf0u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        g_cpu.gpr[3] = race_rate;
        g_cpu.gpr[4] = 0u;
        g_cpu.fpr[1] = 11025.0f;
        CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x2ae04u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ae10u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(race_live_voice_count() == 1);
        CHECK(g_started_gain == 0.25f);
        CHECK(g_started_rate_hz == 11025.0f);
        CHECK(g_started_sample == race_sid);

        // (2) Branch swap, as sndSet2StageEng/sndSetScrape author it: release
        // the playing stage, retune, and start the replacement on the same
        // instrument. The predecessor must be retired, not stranded.
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 68, 0x2ab30u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        g_cpu.gpr[3] = race_amp;
        g_cpu.gpr[4] = 0u;
        g_cpu.fpr[1] = 0.75f;
        CHECK(m2_audio_dispatch_c(&g_cpu, 70, 0x2ab40u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ab50u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(race_live_voice_count() == 1);
        CHECK(g_started_gain == 0.75f);

        // (3) Repeat start without an intervening stop, as the skid/crash/road
        // setters author it: still exactly one voice on the channel.
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2b3a0u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(race_live_voice_count() == 1);

        // (4) sndKillSnds teardown: StopInstrument retires the channel.
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(race_live_voice_count() == 0);

        // (5) Skidpad engine-switch host hypothesis: a newly created sample
        // alone must not replace an existing attachment. Then explicitly
        // replace that attachment, through the same lifted CreateAttachment
        // used at track entry: the SAME sampler must now play the new sample.
        // This isolates host binding/voice caching from guest menu sequencing.
        for (uint32_t i = 0; i < 8u; ++i)
            g_mem[race_data_ea + i] = (uint8_t)(0x70u + i);
        uint32_t replacement_sample = hdl->CreateItem(M2_AUDIO_SAMPLE_CTYPE,
                                                     race_tags_ea);
        const uint32_t replacement_sid = g_next_sid;
        CHECK(replacement_sample != M2K_ITEM_NONE);
        CHECK(replacement_sid != race_sid);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ab50u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(g_started_sample == race_sid);
        CHECK(g_started_sample != replacement_sid); // executed wrong-result control
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 67, 0x2bb40u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(hdl->DeleteItem(race_att) == M2K_OK);
        m2_kernel_dispatch_set_global(hdl);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = replacement_sample;
        g_cpu.gpr[5] = 0u;
        FN_CreateAttachment__73394(&g_cpu);
        const uint32_t replacement_att = g_cpu.gpr[3];
        m2_kernel_dispatch_set_global(NULL);
        CHECK(replacement_att != M2K_ITEM_NONE);
        g_cpu.gpr[3] = (uint32_t)race_ins;
        g_cpu.gpr[4] = 0u;
        CHECK(m2_audio_dispatch_c(&g_cpu, 69, 0x2ab50u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        CHECK(race_live_voice_count() == 1);
        CHECK(g_started_sample == replacement_sid);
        CHECK(g_started_attachment == replacement_att);
        CHECK(g_started_sample != race_sid);

        // (6) sndTeardownGameSnd completes the identified selector set.
        g_cpu.gpr[3] = (uint32_t)race_ins;
        CHECK(m2_audio_dispatch_c(&g_cpu, 17, 0x2a3a0u) == 1);
        CHECK((int32_t)g_cpu.gpr[3] == M2K_OK);
        teardown();
    }

    printf("m2_audio_items_test: %s\n", g_fails ? "FAIL" : "OK");
    return g_fails != 0;
}
