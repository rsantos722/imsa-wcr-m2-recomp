// trace/trace.cpp — discovery and observability harness implementation.
#include "trace.h"

#include "diagnostic_perf.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifdef M2_HAVE_RECOMP
#include "ppc_ctx.h"
extern "C" {
PPCContext* m2_guest_ctx_for_trace(void); // defined in recomp_entry.c
int m2_guest_frame_query(uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t);
}
#endif

namespace m2host::trace {

namespace {
struct TraceState {
    std::mutex mu;
    std::ofstream file;
    std::string file_path;
    TraceConfig cfg;
    std::atomic<uint64_t> seq{0};
    std::chrono::steady_clock::time_point start;
    std::string build_id;
    std::string run_id;
    // Read without the mutex on hot paths; writes happen during setup or
    // shutdown while the mutex is held.  enabled is stored last during init
    // and cleared first during shutdown, so a true enabled always implies a
    // valid enabled_kinds mask.
    std::atomic<bool> enabled{false};
    std::atomic<uint32_t> enabled_kinds{0};
    bool shutdown_emitted = false;
    uint32_t frame_complete_count = 0;
    bool hit_frame_limit = false;
    uint64_t bytes_written = 0;
    bool hit_byte_limit = false;
};

// The recovered guest task is intentionally allowed to remain live while the
// launcher unwinds. Keep this synchronization state alive until process exit:
// destroying its mutex before the detached guest thread stops turns a benign
// late trace callback into std::system_error (EINVAL).
TraceState& g_state = *new TraceState;

uint64_t guest_frame_query_safe() {
#ifdef M2_HAVE_RECOMP
    uint32_t seq = 0, pc = 0, state = 0, snap[4] = {0, 0, 0, 0};
    if (m2_guest_frame_query(&seq, &pc, &state, snap, 4) == 0)
        return seq;
#endif
    return 0;
}

} // namespace

const char* event_kind_name(EventKind k) {
    switch (k) {
        case EventKind::Input: return "input";
        case EventKind::GuestEnter: return "guest_enter";
        case EventKind::GuestExit: return "guest_exit";
        case EventKind::StateTransition: return "state_transition";
        case EventKind::SelectorCall: return "selector_call";
        case EventKind::SelectorReturn: return "selector_return";
        case EventKind::AssetLoad: return "asset_load";
        case EventKind::GraphicsRequest: return "graphics_request";
        case EventKind::AudioRequest: return "audio_request";
        case EventKind::MemoryFault: return "memory_fault";
        case EventKind::IndirectTarget: return "indirect_target";
        case EventKind::UnknownBehavior: return "unknown_behavior";
        case EventKind::FrameComplete: return "frame_complete";
        case EventKind::Tracepoint: return "tracepoint";
        case EventKind::Provenance: return "provenance";
        case EventKind::Shutdown: return "shutdown";
        default: return "unknown_behavior";
    }
}

bool event_kind_from_name(std::string_view s, EventKind* out) {
    for (int i = 0; i < int(EventKind::kCount); ++i) {
        auto k = EventKind(i);
        if (s == event_kind_name(k)) {
            if (out) *out = k;
            return true;
        }
    }
    return false;
}

bool is_kind_enabled(EventKind k, const TraceConfig& cfg) {
    if (!cfg.enable_enter_exit &&
        (k == EventKind::GuestEnter || k == EventKind::GuestExit))
        return false;
    if (cfg.filter.empty()) return true;
    for (auto f : cfg.filter) if (f == k) return true;
    return false;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else out.push_back(c);
        }
    }
    return out;
}

std::string build_id_resolve() {
#ifdef IMSA_VERSION
    std::string ver = IMSA_VERSION;
#else
    std::string ver = "0.0.0-dev";
#endif
    // Try to read VERSION file relative to repo root
    std::filesystem::path repo;
    {
        auto dir = std::filesystem::current_path();
        for (int i = 0; i < 6; ++i) {
            std::error_code ec;
            if (std::filesystem::is_directory(dir / "host", ec) && !ec) { repo = dir; break; }
            if (!dir.has_parent_path()) break;
            dir = dir.parent_path();
        }
    }
    if (!repo.empty()) {
        std::ifstream vf(repo / "VERSION");
        if (vf) {
            std::string v;
            std::getline(vf, v);
            // trim
            size_t a = v.find_first_not_of(" \t\r\n");
            size_t b = v.find_last_not_of(" \t\r\n");
            if (a != std::string::npos && b != std::string::npos) ver = v.substr(a, b-a+1);
            else if (a != std::string::npos) ver = v.substr(a);
        }
        // Try git commit
#if defined(_WIN32)
        // cmd.exe has no /dev/null; an unopenable redirect aborts the command.
#define M2_NULL_REDIRECT " 2>NUL"
#else
#define M2_NULL_REDIRECT " 2>/dev/null"
#endif
        FILE* pp = popen("git -C . rev-parse --short HEAD" M2_NULL_REDIRECT, "r");
        if (!pp) pp = popen("git rev-parse --short HEAD" M2_NULL_REDIRECT, "r");
#undef M2_NULL_REDIRECT
        if (pp) {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), pp)) {
                std::string h = buf;
                size_t e = h.find_first_of(" \r\n");
                if (e != std::string::npos) h = h.substr(0,e);
                if (!h.empty()) ver += "+" + h;
            }
            pclose(pp);
        }
    }
    return ver;
}

std::string run_id_generate() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // pid + ms — getpid may be unavailable on Windows, use thread id fallback
    std::ostringstream oss;
#if defined(_WIN32)
    oss << "run-" << ms << "-" << std::hash<std::thread::id>{}(std::this_thread::get_id());
#else
    oss << "run-" << ms << "-" << ::getpid();
#endif
    return oss.str();
}

bool trace_init(const TraceConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_state.mu);
    g_state.enabled.store(false, std::memory_order_release);
    g_state.enabled_kinds.store(0, std::memory_order_release);
    // Close previous
    if (g_state.file.is_open()) g_state.file.close();
    g_state.cfg = cfg;
    g_state.build_id = cfg.build_id.empty() ? build_id_resolve() : cfg.build_id;
    g_state.run_id = cfg.run_id.empty() ? run_id_generate() : cfg.run_id;
    g_state.start = std::chrono::steady_clock::now();
    g_state.seq.store(0);
    g_state.frame_complete_count = 0;
    g_state.hit_frame_limit = false;
    g_state.bytes_written = 0;
    g_state.hit_byte_limit = false;
    g_state.shutdown_emitted = false;
    g_state.file_path = cfg.file_path;
    if (!cfg.file_path.empty()) {
        std::filesystem::path p(cfg.file_path);
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        g_state.file.open(cfg.file_path, std::ios::out | std::ios::trunc);
        if (!g_state.file.is_open()) {
            std::fprintf(stderr, "[trace] failed to open %s\n", cfg.file_path.c_str());
            g_state.enabled.store(false, std::memory_order_release);
            g_state.enabled_kinds.store(0, std::memory_order_release);
            return false;
        }
    }
    uint32_t kind_mask = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(EventKind::kCount); ++i) {
        if (is_kind_enabled(static_cast<EventKind>(i), g_state.cfg))
            kind_mask |= 1u << i;
    }
    g_state.enabled_kinds.store(kind_mask, std::memory_order_release);
    g_state.enabled.store(true, std::memory_order_release);
    return true;
}

void trace_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_state.mu);
        if (!g_state.enabled.load(std::memory_order_relaxed) || g_state.shutdown_emitted)
            return;
        g_state.shutdown_emitted = true;
    }
    // Use trace_emit for the shutdown record so sequencing stays monotonic and serialized
    trace_emit(EventKind::Shutdown, 0, 0, 0, "{\"reason\":\"trace_shutdown\",\"exit_code\":0}");
    {
        std::lock_guard<std::mutex> lk(g_state.mu);
        if (g_state.file.is_open()) g_state.file.close();
        g_state.enabled.store(false, std::memory_order_release);
        g_state.enabled_kinds.store(0, std::memory_order_release);
    }
}

bool trace_is_enabled() {
    return g_state.enabled.load(std::memory_order_acquire);
}
bool trace_kind_enabled(EventKind k) noexcept {
    if (!g_state.enabled.load(std::memory_order_acquire)) return false;
    const unsigned kind = static_cast<unsigned>(k);
    if (kind >= static_cast<unsigned>(EventKind::kCount)) return false;
    return (g_state.enabled_kinds.load(std::memory_order_relaxed) & (1u << kind)) != 0;
}
const TraceConfig& trace_config() {
    return g_state.cfg;
}
uint64_t trace_sequence() { return g_state.seq.load(); }
bool trace_byte_limit_reached() {
    std::lock_guard<std::mutex> lk(g_state.mu);
    return g_state.hit_byte_limit;
}

void trace_emit(EventKind kind, uint32_t guest_pc, uint32_t task_id,
                uint32_t guest_frame, std::string_view payload_json) {
    // Cheap rejection before touching the mutex. Every selector dispatch and
    // input poll reaches this function even when no trace file is requested.
    if (!trace_kind_enabled(kind)) return;
    // Single critical section to ensure sequence order matches file order (determinism).
    // This serializes all emit paths (host + guest threads) so file lines are monotonic.
    std::unique_lock<std::mutex> lk(g_state.mu);
    if (!g_state.enabled.load(std::memory_order_relaxed)) return;
    if (g_state.hit_frame_limit) return;
    if (g_state.hit_byte_limit) return;
    const uint32_t kind_bit = 1u << static_cast<unsigned>(kind);
    if ((g_state.enabled_kinds.load(std::memory_order_relaxed) & kind_bit) == 0)
        return;
    if (kind == EventKind::FrameComplete) {
        if (g_state.cfg.max_frames != 0 && g_state.frame_complete_count >= g_state.cfg.max_frames) {
            g_state.hit_frame_limit = true;
            return;
        }
    }
    uint64_t seq = g_state.seq.fetch_add(1);
    auto now = std::chrono::steady_clock::now();
    uint64_t host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_state.start).count();
    // If guest_frame is 0 and we have a guest context, try to fill it.
    // Safe to call while holding g_state.mu — guest_frame_query_safe only reads guest memory.
    uint32_t gf = guest_frame;
    if (gf == 0) {
        uint64_t q = guest_frame_query_safe();
        if (q != 0) gf = (uint32_t)q;
    }

    std::string build_id = g_state.build_id;
    std::string run_id = g_state.run_id;

    std::ostringstream oss;
    oss << "{";
    oss << "\"schema_version\":" << kTraceSchemaVersion << ",";
    oss << "\"source_image_hash\":\"" << kSourceImageHash << "\",";
    oss << "\"build_id\":\"" << json_escape(build_id) << "\",";
    oss << "\"run_id\":\"" << json_escape(run_id) << "\",";
    oss << "\"sequence\":" << seq << ",";
    oss << "\"host_time\":" << host_ms << ",";
    oss << "\"guest_frame\":" << gf << ",";
    oss << "\"task_id\":" << task_id << ",";
    oss << "\"guest_pc\":" << guest_pc << ",";
    oss << "\"event_kind\":\"" << event_kind_name(kind) << "\",";
    oss << "\"payload\":";
    if (payload_json.empty()) oss << "{}";
    else {
        if (!payload_json.empty() && (payload_json.front() == '{' || payload_json.front() == '['))
            oss << payload_json;
        else
            oss << "{" << payload_json << "}";
    }
    oss << "}\n";
    std::string line = oss.str();
    if (g_state.cfg.max_bytes &&
        line.size() > g_state.cfg.max_bytes - g_state.bytes_written) {
        g_state.hit_byte_limit = true;
        std::fprintf(stderr, "[trace] byte limit reached; trace is incomplete evidence\n");
        return;
    }
    g_state.bytes_written += line.size();

    if (kind == EventKind::FrameComplete) g_state.frame_complete_count++;
    if (g_state.file.is_open() || g_state.cfg.mirror_stderr) {
        m2host::perf::Scope logging_scope(
            m2host::perf::Phase::DiagnosticLogging);
        if (g_state.file.is_open()) {
            g_state.file << line;
            g_state.file.flush();
        }
        if (g_state.cfg.mirror_stderr) {
            std::fwrite(line.c_str(), 1, line.size(), stderr);
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void trace_input(uint32_t buttons, uint32_t pressed, uint32_t released,
                 uint8_t stick_x, uint8_t stick_y, uint8_t device,
                 uint32_t guest_frame) {
    if (!trace_input_enabled()) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"buttons\":%u,\"pressed\":%u,\"released\":%u,\"stick_x\":%u,\"stick_y\":%u,\"device\":%u}",
        buttons, pressed, released, (unsigned)stick_x, (unsigned)stick_y, (unsigned)device);
    trace_emit(EventKind::Input, 0, 0, guest_frame, buf);
}

void trace_selector_call(uint32_t folio, uint32_t ordinal, uint32_t pc,
                         uint32_t gpr3, uint32_t gpr4, uint32_t guest_frame,
                         uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::SelectorCall)) return;
    char buf[512];
    // Include selector name when known via shim_abi? For now numeric + lookup later.
    std::snprintf(buf, sizeof(buf),
        "{\"folio\":%u,\"ordinal\":%u,\"pc\":%u,\"gpr3\":%u,\"gpr4\":%u}",
        folio, ordinal, pc, gpr3, gpr4);
    trace_emit(EventKind::SelectorCall, pc, task_id, guest_frame, buf);
}
void trace_selector_return(uint32_t folio, uint32_t ordinal, uint32_t pc,
                           uint32_t ret_gpr3, uint32_t guest_frame,
                           uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::SelectorReturn)) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"folio\":%u,\"ordinal\":%u,\"pc\":%u,\"ret\":%u}",
        folio, ordinal, pc, ret_gpr3);
    trace_emit(EventKind::SelectorReturn, pc, task_id, guest_frame, buf);
}

void trace_guest_enter(uint32_t pc, const char* sym, uint32_t guest_frame,
                       uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::GuestEnter)) return;
    std::string payload = "{\"sym\":\"";
    payload += json_escape(sym ? sym : "");
    payload += "\",\"pc\":";
    payload += std::to_string(pc);
    payload += "}";
    trace_emit(EventKind::GuestEnter, pc, task_id, guest_frame, payload);
}
void trace_guest_exit(uint32_t pc, const char* sym, uint32_t ret, uint32_t guest_frame,
                      uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::GuestExit)) return;
    std::string payload = "{\"sym\":\"";
    payload += json_escape(sym ? sym : "");
    payload += "\",\"pc\":";
    payload += std::to_string(pc);
    payload += ",\"ret\":";
    payload += std::to_string(ret);
    payload += "}";
    trace_emit(EventKind::GuestExit, pc, task_id, guest_frame, payload);
}

void trace_state_transition(uint32_t from_state, uint32_t to_state,
                            uint32_t state_ea, uint32_t pc,
                            uint32_t guest_frame, uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::StateTransition)) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"from\":%u,\"to\":%u,\"state_ea\":%u,\"pc\":%u}",
        from_state, to_state, state_ea, pc);
    trace_emit(EventKind::StateTransition, pc, task_id, guest_frame, buf);
}

void trace_asset_load(std::string_view path, uint32_t resource_id, uint32_t size,
                      uint32_t guest_pc, uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::AssetLoad)) return;
    std::string payload = "{\"path\":\"" + json_escape(path) + "\",\"resource_id\":" +
                          std::to_string(resource_id) + ",\"size\":" + std::to_string(size) + "}";
    trace_emit(EventKind::AssetLoad, guest_pc, 0, guest_frame, payload);
}

void trace_graphics_request(std::string_view op, std::string_view payload_json,
                            uint32_t guest_pc, uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::GraphicsRequest)) return;
    // op + payload_json (already JSON object)
    std::string payload = "{\"op\":\"" + json_escape(op) + "\"";
    if (!payload_json.empty()) {
        // Merge: if payload_json is object, embed its fields
        // Simplify: add as nested "args"
        payload += ",\"args\":";
        payload += payload_json;
    }
    payload += "}";
    trace_emit(EventKind::GraphicsRequest, guest_pc, 0, guest_frame, payload);
}

void trace_audio_request(std::string_view op, std::string_view payload_json,
                         uint32_t guest_pc, uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::AudioRequest)) return;
    std::string payload = "{\"op\":\"" + json_escape(op) + "\"";
    if (!payload_json.empty()) {
        payload += ",\"args\":";
        payload += payload_json;
    }
    payload += "}";
    trace_emit(EventKind::AudioRequest, guest_pc, 0, guest_frame, payload);
}

void trace_memory_fault(uint32_t fault_ea, uint32_t access_size, uint32_t pc,
                        uint32_t guest_frame, uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::MemoryFault)) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"fault_ea\":%u,\"size\":%u,\"pc\":%u}", fault_ea, access_size, pc);
    trace_emit(EventKind::MemoryFault, pc, task_id, guest_frame, buf);
}

void trace_indirect_target(uint32_t ctr, uint32_t target, uint32_t pc,
                           bool resolved, uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::IndirectTarget)) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"ctr\":%u,\"target\":%u,\"pc\":%u,\"resolved\":%s}",
        ctr, target, pc, resolved ? "true" : "false");
    trace_emit(EventKind::IndirectTarget, pc, 0, guest_frame, buf);
}

void trace_unknown_behavior(std::string_view description, uint32_t pc,
                            uint32_t guest_frame, uint32_t task_id) {
    if (!trace_kind_enabled(EventKind::UnknownBehavior)) return;
    std::string payload = "{\"description\":\"" + json_escape(description) + "\"}";
    trace_emit(EventKind::UnknownBehavior, pc, task_id, guest_frame, payload);
}

void trace_frame_complete(uint32_t seq, uint32_t pc, uint32_t state,
                          const uint32_t* snapshot_words, uint32_t snapshot_len,
                          uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::FrameComplete)) return;
    std::ostringstream oss;
    oss << "{\"seq\":" << seq << ",\"pc\":" << pc << ",\"state\":" << state;
    if (snapshot_words && snapshot_len) {
        oss << ",\"snapshot\":[";
        for (uint32_t i = 0; i < snapshot_len; ++i) {
            if (i) oss << ",";
            oss << snapshot_words[i];
        }
        oss << "]";
    }
    // Include default snapshot fields
    size_t cnt = 0;
    const SnapshotField* fields = default_snapshot_fields(&cnt);
    std::string snap_json = snapshot_collect_json(fields, cnt, guest_frame);
    if (!snap_json.empty()) {
        oss << "," << snap_json;
    }
    oss << "}";
    trace_emit(EventKind::FrameComplete, pc, 0, guest_frame, oss.str());
}

void trace_guest_tracepoint(const M2TracepointCapture* capture) {
    if (!trace_kind_enabled(EventKind::Tracepoint)) return;
    if (!capture) return;

    std::ostringstream oss;
    oss << "{\"event_schema_version\":" << M2_GUEST_EVENT_SCHEMA_VERSION
        << ",\"id\":" << capture->id
        << ",\"name\":\"" << json_escape(capture->name)
        << "\",\"pc\":" << capture->pc
        << ",\"hit\":" << capture->hit
        << ",\"registers\":{";

    bool first_register = true;
    for (uint32_t i = 0; i < 32u; ++i) {
        if ((capture->gpr_mask & (1u << i)) == 0) continue;
        if (!first_register) oss << ",";
        first_register = false;
        oss << "\"r" << i << "\":" << capture->gpr[i];
    }
    if (capture->has_lr) {
        if (!first_register) oss << ",";
        first_register = false;
        oss << "\"lr\":" << capture->lr;
    }
    if (capture->has_ctr) {
        if (!first_register) oss << ",";
        first_register = false;
        oss << "\"ctr\":" << capture->ctr;
    }
    if (capture->has_cr) {
        if (!first_register) oss << ",";
        oss << "\"cr\":" << capture->cr;
    }
    oss << "},\"memory\":[";

    const uint32_t memory_count =
        capture->memory_count < M2_TRACEPOINT_MAX_MEMORY
            ? capture->memory_count : M2_TRACEPOINT_MAX_MEMORY;
    for (uint32_t i = 0; i < memory_count; ++i) {
        const M2TracepointMemoryCapture& memory = capture->memory[i];
        const uint32_t length =
            memory.length < M2_TRACEPOINT_MAX_CAPTURE_BYTES
                ? memory.length : M2_TRACEPOINT_MAX_CAPTURE_BYTES;
        if (i) oss << ",";
        oss << "{\"label\":\"" << json_escape(memory.label)
            << "\",\"kind\":" << memory.kind
            << ",\"ea\":" << memory.ea
            << ",\"base_register\":";
        if (memory.base_register == UINT32_MAX) oss << "null";
        else oss << memory.base_register;
        oss << ",\"base_value\":" << memory.base_value
            << ",\"offset\":" << memory.offset
            << ",\"length\":" << memory.length
            << ",\"valid\":" << (memory.valid ? "true" : "false")
            << ",\"bytes\":\"";
        oss << std::hex << std::setfill('0');
        for (uint32_t j = 0; j < length; ++j)
            oss << std::setw(2) << static_cast<unsigned>(memory.bytes[j]);
        oss << std::dec << "\",\"words\":[";
        bool first_word = true;
        for (uint32_t j = 0; j + 4u <= length; j += 4u) {
            uint32_t word = (static_cast<uint32_t>(memory.bytes[j]) << 24) |
                            (static_cast<uint32_t>(memory.bytes[j + 1]) << 16) |
                            (static_cast<uint32_t>(memory.bytes[j + 2]) << 8) |
                            static_cast<uint32_t>(memory.bytes[j + 3]);
            if (!first_word) oss << ",";
            first_word = false;
            oss << word;
        }
        oss << "]";
        if (memory.provenance_requested) {
            const uint32_t provenance_count =
                memory.provenance_count < M2_TRACEPOINT_MAX_PROVENANCE
                    ? memory.provenance_count : M2_TRACEPOINT_MAX_PROVENANCE;
            oss << ",\"provenance\":[";
            for (uint32_t j = 0; j < provenance_count; ++j) {
                const M2TracepointProvenance& provenance = memory.provenance[j];
                if (j) oss << ",";
                oss << "{\"ea\":" << provenance.ea
                    << ",\"writer_pc\":" << provenance.writer_pc
                    << ",\"caller_lr\":" << provenance.caller_lr
                    << ",\"frame_seq\":" << provenance.frame_seq
                    << ",\"write_count\":" << provenance.write_count
                    << ",\"src_ea\":" << provenance.src_ea
                    << ",\"copy_pc\":" << provenance.copy_pc
                    << ",\"src_writer_pc\":" << provenance.src_writer_pc
                    << ",\"alloc_tag\":\""
                    << json_escape(provenance.alloc_tag) << "\"}";
            }
            oss << "]";
        }
        oss << "}";
    }
    oss << "]}";
    trace_emit(EventKind::Tracepoint, capture->pc, capture->task_id,
               capture->guest_frame, oss.str());
}

void trace_guest_event(const M2GuestEvent* event) {
    if (!event) return;
    if (event->kind == M2_GUEST_EVENT_TRACEPOINT) {
        trace_guest_tracepoint(event->tracepoint);
        return;
    }
    if (!trace_kind_enabled(EventKind::Provenance)) return;

    const char* operation = "unknown";
    switch (event->kind) {
        case M2_GUEST_EVENT_WRITE: operation = "write"; break;
        case M2_GUEST_EVENT_READ: operation = "read"; break;
        case M2_GUEST_EVENT_COPY: operation = "copy"; break;
        case M2_GUEST_EVENT_ALLOC: operation = "alloc"; break;
        case M2_GUEST_EVENT_FREE: operation = "free"; break;
        case M2_GUEST_EVENT_OOB: operation = "oob"; break;
        default: break;
    }

    std::ostringstream oss;
    oss << "{\"event_schema_version\":" << event->schema_version
        << ",\"operation\":\"" << operation << "\""
        << ",\"ea\":" << event->ea
        << ",\"size\":" << event->size
        << ",\"width\":" << event->width
        << ",\"value\":" << event->value
        << ",\"src_ea\":" << event->src_ea
        << ",\"lr\":" << event->lr
        << ",\"tag\":\"" << json_escape(event->tag)
        << "\",\"region\":\"" << json_escape(event->region) << "\"}";
    trace_emit(EventKind::Provenance, event->pc, 0, event->frame_seq,
               oss.str());
}

void trace_shutdown(std::string_view reason, int exit_code, uint32_t guest_frame) {
    if (!trace_kind_enabled(EventKind::Shutdown)) return;
    std::string payload = "{\"reason\":\"" + json_escape(reason) + "\",\"exit_code\":" + std::to_string(exit_code) + "}";
    trace_emit(EventKind::Shutdown, 0, 0, guest_frame, payload);
}

// ---------------------------------------------------------------------------
// Snapshot helpers
// ---------------------------------------------------------------------------
const SnapshotField* default_snapshot_fields(size_t* out_count) {
    static const SnapshotField kFields[] = {
        {"primary_state",   0x1877d8u, 4, "BSS"},
        {"secondary_state", 0x1877dcu, 4, "BSS"},
        {"byte_state",      0x17d94bu, 1, "DATA"},
        {"frame_counter",   0x003F0000u, 4, "frame_carve"},
        {"frame_state",     0x003F0004u, 4, "frame_carve"},
        {"frame_pc",        0x003F0008u, 4, "frame_carve"},
        {"frame_snap0",     0x003F0010u, 4, "frame_carve"},
        {"frame_snap1",     0x003F0014u, 4, "frame_carve"},
        {"frame_snap2",     0x003F0018u, 4, "frame_carve"},
        {"frame_snap3",     0x003F001Cu, 4, "frame_carve"},
    };
    if (out_count) *out_count = sizeof(kFields)/sizeof(kFields[0]);
    return kFields;
}

    std::string snapshot_collect_json(const SnapshotField* fields, size_t count,
                                  uint32_t guest_frame) {
    (void)guest_frame;
    if (!fields || count == 0) return "";
    std::ostringstream oss;
    oss << "\"fields\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) oss << ",";
        uint32_t val = 0;
        bool ok = false;
#ifdef M2_HAVE_RECOMP
        PPCContext* ctx = nullptr;
        // Try to get guest context
        // m2_guest_ctx_for_trace returns PPCContext* or null
        ctx = m2_guest_ctx_for_trace();
        if (ctx && ctx->mem) {
            if (fields[i].guest_ea + fields[i].size <= RECOMP_GUEST_MEM) {
                if (fields[i].size == 4) {
                    // BE read
                    uint8_t* m = ctx->mem + fields[i].guest_ea;
                    val = (uint32_t(m[0])<<24)|(uint32_t(m[1])<<16)|(uint32_t(m[2])<<8)|uint32_t(m[3]);
                    ok = true;
                } else if (fields[i].size == 1) {
                    val = ctx->mem[fields[i].guest_ea];
                    ok = true;
                } else if (fields[i].size == 2) {
                    uint8_t* m = ctx->mem + fields[i].guest_ea;
                    val = (uint32_t(m[0])<<8)|uint32_t(m[1]);
                    ok = true;
                }
            }
        }
#endif
        oss << "{\"name\":\"" << json_escape(fields[i].name) << "\","
            << "\"guest_ea\":" << fields[i].guest_ea << ","
            << "\"size\":" << fields[i].size << ","
            << "\"source\":\"" << json_escape(fields[i].source) << "\","
            << "\"value\":" << val << ","
            << "\"ok\":" << (ok ? "true" : "false") << "}";
    }
    oss << "]";
    // Add checksum of the window
    uint32_t checksum = 0;
    for (size_t i=0;i<count;++i) checksum ^= (fields[i].guest_ea * 31u + fields[i].size);
    oss << ",\"checksum\":" << checksum;
    return oss.str();
}

} // namespace m2host::trace

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
extern "C" {
void m2_trace_init_file(const char* path) {
    m2host::trace::TraceConfig cfg;
    cfg.file_path = path ? path : "";
    m2host::trace::trace_init(cfg);
}
void m2_trace_shutdown(void) { m2host::trace::trace_shutdown(); }
void m2_trace_selector_call_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                              uint32_t gpr3, uint32_t gpr4) {
    m2host::trace::trace_selector_call(folio, ordinal, pc, gpr3, gpr4, 0, 0);
}
void m2_trace_selector_return_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                                uint32_t ret) {
    m2host::trace::trace_selector_return(folio, ordinal, pc, ret, 0, 0);
}
void m2_trace_guest_enter_c(uint32_t pc, const char* sym) {
    m2host::trace::trace_guest_enter(pc, sym, 0, 0);
}
void m2_trace_guest_exit_c(uint32_t pc, const char* sym, uint32_t ret) {
    m2host::trace::trace_guest_exit(pc, sym, ret, 0, 0);
}
void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc) {
    m2host::trace::trace_memory_fault(ea, size, pc, 0, 0);
}
void m2_trace_indirect_target_c(uint32_t ctr, uint32_t target, uint32_t pc,
                                int resolved) {
    m2host::trace::trace_indirect_target(ctr, target, pc, resolved != 0, 0);
}
void m2_trace_unknown_behavior_c(const char* desc, uint32_t pc) {
    m2host::trace::trace_unknown_behavior(desc ? desc : "", pc, 0, 0);
}
void m2_trace_frame_complete_c(uint32_t seq, uint32_t pc, uint32_t state) {
    m2host::trace::trace_frame_complete(seq, pc, state, nullptr, 0, seq);
}
void m2_trace_state_transition_c(uint32_t from_state, uint32_t to_state, uint32_t state_ea, uint32_t pc) {
    m2host::trace::trace_state_transition(from_state, to_state, state_ea, pc, 0, 0);
}
void m2_guest_event_c(const M2GuestEvent* event) {
    m2host::trace::trace_guest_event(event);
}
void m2_tracepoint_event_c(const M2TracepointCapture* capture) {
    m2host::trace::trace_guest_tracepoint(capture);
}
}
