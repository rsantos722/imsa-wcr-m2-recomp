// game_entry_stub.cpp — stand-in for the recompiled guest entry and audio runtime.
#include <cstdio>
#include <cstdint>
#include "audio_dispatch.h"

struct PPCContext;

extern "C" {

int m2_game_entry(int argc, char** argv) {
    std::printf("[game] m2_game_entry STUB reached (argc=%d) — "
                "reconfigure with -DIMSA_DISC=<iso> or -DIMSA_DATA_DIR=<dir> for the real recomp\n",
                argc);
    return 0;
}

void m2_recomp_shutdown(void) {}
int m2_guest_live_task_count(void) { return 0; }
void m2_kernel_lock_diag_dump_pairs(void) {}
void m2_audio_graph_lock_diag_dump(void) {}
struct PPCContext* m2_recomp_context(void) { return nullptr; }
void m2_recomp_set_game_root(const char* /*root*/) {}
const char* m2_recomp_get_game_root(void) { return ""; }
uint64_t m2_oob_trap_total(void) { return 0; }
void m2_guest_set_controller_type(int /*analog*/) {}

void m2_audio_diag_set_current_ordinal(uint32_t /*ordinal*/) {}
void m2_selector_note(uint32_t /*folio*/, uint32_t /*ordinal*/, int /*handled*/) {}

void FN_CompressedBfrRead__63c9c(PPCContext* /*c*/) {}
void FN_GetCompressedBfr__63be8(PPCContext* /*c*/) {}

// Audio dispatch stubs
int32_t m2_guest_audio_create_mixer_template(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_load_instrument(struct PPCContext*, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_unload_instrument(uint32_t) { return 0; }
int32_t m2_guest_audio_load_ins_template(struct PPCContext*, uint32_t, uint32_t) { return 0; }
uint32_t m2_guest_audio_get_audio_time(struct PPCContext*) { return 0; }
uint32_t m2_guest_audio_get_audio_frame_count(struct PPCContext*) { return 0; }
uint32_t m2_guest_audio_get_cue_signal(struct PPCContext*, uint32_t) { return 0; }
void m2_guest_audio_pump_timers(void) {}
void m2_guest_audio_pump_timers_for_tick(int) {}
uint32_t m2_guest_audio_pump_dspp_output(void) { return 0u; }
uint64_t m2_guest_audio_dspp_backoff_sleeps(void) { return 0u; }
void m2_guest_audio_set_async_dspp(int) {}
int m2_guest_audio_async_dspp_enabled(void) { return 0; }
void m2_guest_audio_graph_lock(void) {}
void m2_guest_audio_graph_unlock(void) {}
void m2_guest_audio_setter_enter(void) {}
void m2_guest_audio_setter_leave(void) {}
int m2_guest_audio_setter_pending(void) { return 0; }
int32_t m2_guest_audio_get_resource_info(struct PPCContext*, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_get_item_info(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_set_item_info(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_signal_at_audio_time(struct PPCContext*, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_read_audio_clock(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_set_audio_clock_rate(struct PPCContext*, uint32_t, float) { return 0; }
int32_t m2_guest_audio_sleep_until_audio_time(struct PPCContext*, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_stop_attachment(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_read_probe_part(struct PPCContext*, uint32_t, int32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_abort_timer_cue(struct PPCContext*, uint32_t) { return 0; }
int32_t m2_guest_audio_resume_instrument(struct PPCContext*, uint32_t) { return 0; }
int32_t m2_guest_audio_pause_instrument(struct PPCContext*, uint32_t) { return 0; }
int32_t m2_guest_audio_monitor_attachment(struct PPCContext*, uint32_t, uint32_t, int32_t) { return 0; }
int32_t m2_guest_audio_connect_instrument_parts(struct PPCContext*, uint32_t, uint32_t, int32_t, uint32_t, uint32_t, int32_t) { return 0; }
int32_t m2_guest_audio_disconnect_instrument_parts(struct PPCContext*, uint32_t, uint32_t, int32_t) { return 0; }
int32_t m2_guest_audio_link_attachments(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_start_attachment(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_start_instrument(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_stop_instrument(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_release_instrument(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_arm_trigger_named(struct PPCContext*, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_read_knob_part(struct PPCContext*, uint32_t, int32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_set_knob_part(struct PPCContext*, uint32_t, int32_t, float) { return 0; }
int32_t m2_guest_audiopatch_create_patch_template(struct PPCContext*, uint32_t, uint32_t) { return 0; }
int32_t m2_guest_audio_get_instrument_patch_block_count(uint32_t, uint32_t*) { return 0; }

}  // extern "C"
