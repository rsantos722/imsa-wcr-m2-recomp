// trace.h — discovery and observability harness.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "guest_tracepoints.h"

namespace m2host::trace {

constexpr int kTraceSchemaVersion = 1;
constexpr const char* kSourceImageHash =
    "bb91fcd900424e0894755153b850892b39a704aa4493947b2aa632abaf290865";
constexpr const char* kSourceImagePath = "driving/game";

enum class EventKind : uint8_t {
    Input = 0,
    GuestEnter,
    GuestExit,
    StateTransition,
    SelectorCall,
    SelectorReturn,
    AssetLoad,
    GraphicsRequest,
    AudioRequest,
    MemoryFault,
    IndirectTarget,
    UnknownBehavior,
    FrameComplete,
    Tracepoint,
    Provenance,
    Shutdown,
    kCount
};

const char* event_kind_name(EventKind k);
bool event_kind_from_name(std::string_view s, EventKind* out);

struct TraceConfig {
    std::string file_path;
    bool enable_enter_exit = true;
    bool mirror_stderr = false;
    uint32_t max_frames = 0;
    uint64_t max_bytes = 0;
    std::vector<EventKind> filter;
    std::string build_id;
    std::string run_id;
};

bool is_kind_enabled(EventKind k, const TraceConfig& cfg);

bool trace_init(const TraceConfig& cfg);
void trace_shutdown();
bool trace_is_enabled();
// Lock-free checks for hot paths. trace_kind_enabled() is true only while the
// trace is active and the given event kind passes the configured filter.
// Safe to call from any thread.
bool trace_kind_enabled(EventKind k) noexcept;
inline bool trace_input_enabled() noexcept {
    return trace_kind_enabled(EventKind::Input);
}
const TraceConfig& trace_config();
uint64_t trace_sequence();
bool trace_byte_limit_reached();

void trace_emit(EventKind kind,
                uint32_t guest_pc,
                uint32_t task_id,
                uint32_t guest_frame,
                std::string_view payload_json);

void trace_input(uint32_t buttons, uint32_t pressed, uint32_t released,
                 uint8_t stick_x, uint8_t stick_y, uint8_t device,
                 uint32_t guest_frame);

void trace_selector_call(uint32_t folio, uint32_t ordinal, uint32_t pc,
                         uint32_t gpr3, uint32_t gpr4, uint32_t guest_frame,
                         uint32_t task_id);
void trace_selector_return(uint32_t folio, uint32_t ordinal, uint32_t pc,
                           uint32_t ret_gpr3, uint32_t guest_frame,
                           uint32_t task_id);

void trace_guest_enter(uint32_t pc, const char* sym, uint32_t guest_frame,
                       uint32_t task_id);
void trace_guest_exit(uint32_t pc, const char* sym, uint32_t ret, uint32_t guest_frame,
                      uint32_t task_id);

void trace_state_transition(uint32_t from_state, uint32_t to_state,
                            uint32_t state_ea, uint32_t pc,
                            uint32_t guest_frame, uint32_t task_id);

void trace_asset_load(std::string_view path, uint32_t resource_id, uint32_t size,
                      uint32_t guest_pc, uint32_t guest_frame);

void trace_graphics_request(std::string_view op, std::string_view payload_json,
                            uint32_t guest_pc, uint32_t guest_frame);

void trace_audio_request(std::string_view op, std::string_view payload_json,
                         uint32_t guest_pc, uint32_t guest_frame);

void trace_memory_fault(uint32_t fault_ea, uint32_t access_size, uint32_t pc,
                        uint32_t guest_frame, uint32_t task_id);

void trace_indirect_target(uint32_t ctr, uint32_t target, uint32_t pc,
                           bool resolved, uint32_t guest_frame);

void trace_unknown_behavior(std::string_view description, uint32_t pc,
                            uint32_t guest_frame, uint32_t task_id);

void trace_frame_complete(uint32_t seq, uint32_t pc, uint32_t state,
                          const uint32_t* snapshot_words, uint32_t snapshot_len,
                          uint32_t guest_frame);

void trace_guest_tracepoint(const M2TracepointCapture* capture);
void trace_guest_event(const M2GuestEvent* event);

void trace_shutdown(std::string_view reason, int exit_code, uint32_t guest_frame);

struct SnapshotField {
    const char* name;
    uint32_t guest_ea;
    uint32_t size;
    const char* source;
};

std::string snapshot_collect_json(const SnapshotField* fields, size_t count,
                                  uint32_t guest_frame);

const SnapshotField* default_snapshot_fields(size_t* out_count);

std::string build_id_resolve();
std::string run_id_generate();
std::string json_escape(std::string_view s);

}  // namespace m2host::trace

#ifdef __cplusplus
extern "C" {
#endif
void m2_trace_init_file(const char* path);
void m2_trace_shutdown(void);
void m2_trace_selector_call_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                              uint32_t gpr3, uint32_t gpr4);
void m2_trace_selector_return_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                                uint32_t ret);
void m2_trace_guest_enter_c(uint32_t pc, const char* sym);
void m2_trace_guest_exit_c(uint32_t pc, const char* sym, uint32_t ret);
void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc);
void m2_trace_indirect_target_c(uint32_t ctr, uint32_t target, uint32_t pc,
                                int resolved);
void m2_trace_unknown_behavior_c(const char* desc, uint32_t pc);
void m2_trace_frame_complete_c(uint32_t seq, uint32_t pc, uint32_t state);
void m2_trace_state_transition_c(uint32_t from_state, uint32_t to_state, uint32_t state_ea, uint32_t pc);
#ifdef __cplusplus
}
#endif
