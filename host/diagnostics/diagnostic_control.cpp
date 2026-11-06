#include "diagnostic_control.h"
#include "audio_shim.h"
#include "graphics/guest_clt.h"
#include "trace.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include "platform.h"
// The control channel is a Unix-domain socket. Windows Python (tools/m2ctl.py)
// has no AF_UNIX, so on Windows open() refuses and the transport compiles out.
#if !defined(_WIN32)
#define M2_CONTROL_SOCKET 1
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef M2_HAVE_RECOMP
#include "diagnostic_gate.h"
#include "diagnostic_input.h"
#include "guest_kernel.h"
#include "guest_provenance.h"
#include "recomp_host.h"
#endif

namespace m2host {
namespace {
#ifdef M2_HAVE_RECOMP
// Guest-memory and wait arguments are only parsed when guest commands exist.
uint64_t number(const std::string& s, uint64_t limit = UINT64_MAX) {
    if (s.empty() || s[0] == '-' || s[0] == '+' ||
        s.find_first_of(" \t\r\n") != std::string::npos)
        throw std::runtime_error("invalid_number");
    size_t end = 0;
    const auto n = std::stoull(s, &end, 0);
    if (end != s.size() || n > limit) throw std::runtime_error("number_out_of_range");
    return n;
}
#endif
std::string quote(const std::string& s) { return "\"" + trace::json_escape(s) + "\""; }
}

DiagnosticControl::~DiagnosticControl() {
#ifdef M2_CONTROL_SOCKET
    if (client_ >= 0) close(client_);
    if (listener_ >= 0) close(listener_);
    struct stat st{};
    if (socket_inode_ && lstat(socket_path_.c_str(), &st) == 0 &&
        static_cast<uint64_t>(st.st_ino) == socket_inode_ && S_ISSOCK(st.st_mode))
        unlink(socket_path_.c_str());
#endif
}

bool DiagnosticControl::open(const std::string& path, GuestCltBridge* graphics,
                             AudioShim* audio, Capture capture, std::string& error) {
#if !defined(M2_CONTROL_SOCKET)
    (void)path; (void)graphics; (void)audio; (void)capture;
    error = "diagnostic control sockets are not supported on this platform";
    return false;
#elif !defined(M2_HAVE_RECOMP)
    error = "diagnostic control requires the static guest image";
    return false;
#else
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof address.sun_path) {
        error = "control socket path is too long; use a short relative path";
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0) {
        error = std::strerror(errno);
        if (listener_ >= 0) close(listener_);
        listener_ = -1;
        return false; // Never remove an existing session's socket.
    }
    socket_path_ = path;
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) socket_inode_ = st.st_ino;
    if (chmod(path.c_str(), 0600) != 0 || listen(listener_, 4) != 0) {
        error = std::strerror(errno);
        return false;
    }
    graphics_ = graphics;
    audio_ = audio;
    capture_ = std::move(capture);
    m2_diagnostic_enable(); // before the first guest task is created
    return true;
#endif
}

bool DiagnosticControl::suspend() {
#ifdef M2_HAVE_RECOMP
    m2_diagnostic_request_pause();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m2_diagnostic_is_paused()) {
        // A guest can be blocked on a full graphics queue inside its shim.
        // Drain it so that it can return to the next suspension boundary.
        graphics_->pump();
        if (std::chrono::steady_clock::now() >= deadline) {
            pause_failed_ = true;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    graphics_->pump();
    return true;
#else
    return false;
#endif
}

const char* DiagnosticControl::present_wait_name() const {
    switch (present_wait_) {
        case PresentWait::completed: return "completed";
        case PresentWait::expired: return "expired";
        case PresentWait::guest_ended: return "guest_ended";
        case PresentWait::unpaced: break;
    }
    return "unpaced";
}

uint64_t DiagnosticControl::value(const std::string& field) const {
#ifdef M2_HAVE_RECOMP
    if (field == "ticks") return ticks_;
    if (field == "completed_frames") return m2_guest_frame_pacing_completed();
    if (field == "guest_frame") return m2_guest_current_frame_c();
    if (field == "audio_frames") return audio_->rendered_frames();
    std::string address = field;
    if (field == "primary") address = "u32:0x1877d8";
    else if (field == "secondary") address = "u32:0x1877dc";
    const auto colon = address.find(':');
    if (colon == std::string::npos) throw std::runtime_error("unknown_field");
    const std::string type = address.substr(0, colon);
    const unsigned width = type == "u8" ? 1 : type == "u16" ? 2 : type == "u32" ? 4 : 0;
    const auto ea = number(address.substr(colon + 1), RECOMP_GUEST_MEM);
    const PPCContext* cpu = m2_recomp_context();
    if (!width || !cpu || !cpu->mem || ea > RECOMP_GUEST_MEM - width)
        throw std::runtime_error("invalid_memory_range");
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) result = (result << 8) | cpu->mem[ea + i];
    return result;
#else
    throw std::runtime_error("guest_unavailable");
#endif
}

std::string DiagnosticControl::state() const {
    std::ostringstream out;
    out << "{\"platform_ticks\":" << ticks_ << ",\"command_active\":" << (active_ ? "true" : "false");
#ifdef M2_HAVE_RECOMP
    const bool paused = m2_diagnostic_is_paused();
    out << ",\"paused\":" << (paused ? "true" : "false");
    m2_diagnostic_task tasks[M2_DIAGNOSTIC_TASKS];
    m2_diagnostic_tasks(tasks);
    out << ",\"tasks\":[";
    bool first = true;
    for (unsigned i = 0; i < M2_DIAGNOSTIC_TASKS; ++i) {
        if (!tasks[i].state) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"slot\":" << i << ",\"suspension\":" << tasks[i].state
            << ",\"last_boundary_pc\":" << tasks[i].pc << '}';
    }
    out << ']';
    // Never label a racy read as a snapshot when the pause barrier failed.
    if (paused) {
        int32_t task_state = -1, task_status = -1;
        m2_guest_main_query(&task_state, &task_status);
        m2_diagnostic_input_status input{};
        m2_diagnostic_input_query(&input);
        out << ",\"main_task_state\":" << task_state << ",\"main_task_status\":" << task_status
            << ",\"primary\":" << value("primary") << ",\"secondary\":" << value("secondary")
            << ",\"guest_frame\":" << value("guest_frame")
            << ",\"completed_frames\":" << value("completed_frames")
            << ",\"audio_frames\":" << value("audio_frames")
            << ",\"input\":{\"submitted\":" << input.submitted << ",\"consumed\":" << input.consumed
            << ",\"completed\":" << input.completed << ",\"buttons\":" << input.buttons
            << ",\"phase\":" << input.phase << '}';
    }
    const auto stats = graphics_->stats();
    out << ",\"graphics\":{\"submitted\":" << stats.submitted
        << ",\"draws\":" << stats.draws << ",\"dropped\":" << stats.dropped
        << ",\"malformed\":" << stats.malformed << ",\"unsupported\":" << stats.unsupported
        << ",\"flow_jumps\":" << stats.control_flow_jumps
        << ",\"flow_pauses\":" << stats.control_flow_pauses
        << ",\"flow_external\":" << stats.control_flow_external_handoffs
        << ",\"flow_refusals\":" << stats.control_flow_refusals
        << ",\"flow_loops\":" << stats.control_flow_loops
        << ",\"span_evictions\":" << stats.span_evictions
        << ",\"fb_span_missing\":" << stats.fb_span_missing << '}';
#endif
    out << ",\"trace_byte_limit_reached\":" << (trace::trace_byte_limit_reached() ? "true" : "false") << '}';
    return out.str();
}

std::string DiagnosticControl::response(const std::string& id, const std::string& error,
                                        const std::string& result) const {
    return "{\"schema\":1,\"id\":" + quote(id) + ",\"pid\":" + std::to_string(platform::process_id()) +
           ",\"ok\":" + (error.empty() ? "true" : "false") + ",\"error\":" +
           (error.empty() ? "null" : quote(error)) + ",\"state\":" + state() + ",\"result\":" + result + "}\n";
}

void DiagnosticControl::send_response(int fd, const std::string& text) const {
#ifndef M2_CONTROL_SOCKET
    (void)fd; (void)text;
#else
    size_t offset = 0;
    while (offset < text.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = send(fd, text.data() + offset, text.size() - offset, flags);
        if (n <= 0) break;
        offset += static_cast<size_t>(n);
    }
#endif
}

void DiagnosticControl::finish(const std::string& error, const std::string& result) {
    active_ = false;
    const std::string text = response(request_.id, error, result);
    cache_.push_back({request_.id, request_.wire, text});
    if (cache_.size() > 64) cache_.pop_front();
#ifdef M2_CONTROL_SOCKET
    if (client_ >= 0) { send_response(client_, text); close(client_); }
#endif
    client_ = -1;
}

void DiagnosticControl::accept_command(int wait_ms) {
#ifndef M2_CONTROL_SOCKET
    (void)wait_ms;
}
#else
    pollfd descriptor{listener_, POLLIN, 0};
    if (::poll(&descriptor, 1, wait_ms) <= 0) return;
    const int fd = accept(listener_, nullptr, nullptr);
    if (fd < 0) return;
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
    std::string wire;
    char ch = 0;
    const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (wire.size() < 4096 && std::chrono::steady_clock::now() < read_deadline &&
           recv(fd, &ch, 1, 0) == 1 && ch != '\n') wire += ch;
    if (wire.empty() || wire.size() == 4096 || ch != '\n' ||
        wire.find('\0') != std::string::npos || wire.find('\r') != std::string::npos) {
        send_response(fd, response("", "invalid_request", "{}")); close(fd); return;
    }
    Request incoming;
    incoming.wire = wire;
    std::istringstream stream(wire);
    std::getline(stream, incoming.id, '\t');
    std::getline(stream, incoming.verb, '\t');
    std::string arg;
    while (std::getline(stream, arg, '\t')) incoming.args.push_back(arg);
    if (incoming.id.empty() || incoming.id.size() > 64 || incoming.verb.empty()) {
        send_response(fd, response(incoming.id, "invalid_request", "{}")); close(fd); return;
    }
    for (const auto& cached : cache_) {
        if (cached.id != incoming.id) continue;
        send_response(fd, cached.wire == wire ? cached.response : response(incoming.id, "request_id_reused", "{}"));
        close(fd); return;
    }
    if (active_) {
        if ((incoming.verb == "pause" || incoming.verb == "quit") && incoming.args.empty()) {
#ifdef M2_HAVE_RECOMP
            m2_diagnostic_input_cancel();
#endif
            finish("cancelled");
        } else {
            send_response(fd, response(incoming.id, incoming.verb == "status" && incoming.args.empty() ? "" : "busy", "{}"));
            close(fd); return;
        }
    }
    request_ = std::move(incoming);
    client_ = fd;
    try { execute(); }
    catch (const std::exception& e) { finish(e.what()); }
}
#endif

void DiagnosticControl::execute() {
#ifdef M2_HAVE_RECOMP
    const auto& args = request_.args;
    const auto count = [&](size_t n) { if (args.size() != n) throw std::runtime_error("invalid_arguments"); };
    const auto& verb = request_.verb;
    if (verb == "status") { count(0); finish(); return; }
    if (verb == "quit") { count(0); quit_ = true; finish(); return; }
    if (pause_failed_) { finish("pause_timeout"); return; }
    if (verb == "pause") { count(0); m2_diagnostic_input_cancel(); finish(); return; }
    if (verb == "graphics") { count(0); finish({}, graphics_->diagnostic_snapshot()); return; }
    if (verb == "capture") {
        count(1);
        if (std::filesystem::exists(args[0])) throw std::runtime_error("output_exists");
        if (!capture_(args[0])) throw std::runtime_error("capture_failed");
        // Name the cadence the presented image came from. The presented target
        // is the guest's own published display unit on every path, so an
        // expired deadline changes when the host proceeded, not which image is
        // shown; without this field a capture cannot be told apart from one
        // taken after a new completed application frame.
        finish({}, "{\"path\":" + quote(args[0]) + ",\"present_wait\":" +
                       quote(present_wait_name()) + "}"); return;
    }
    if (verb == "read") {
        count(2);
        const auto ea = number(args[0], RECOMP_GUEST_MEM);
        const auto size = number(args[1], 65536);
        if (!size || ea > RECOMP_GUEST_MEM - size) throw std::runtime_error("invalid_memory_range");
        const auto* cpu = m2_recomp_context();
        std::ostringstream bytes;
        for (size_t i = 0; i < size; ++i)
            bytes << std::hex << std::setw(2) << std::setfill('0') << unsigned(cpu->mem[ea + i]);
        finish({}, "{\"ea\":" + std::to_string(ea) + ",\"hex\":" + quote(bytes.str()) + "}"); return;
    }
    if (verb == "watch") {
        count(3);
        const auto ea = number(args[0], RECOMP_GUEST_MEM);
        const auto size = number(args[1], 65536);
        if (!size || ea > RECOMP_GUEST_MEM - size) throw std::runtime_error("invalid_memory_range");
        if (!m2_provenance_add_watched_range(ea, ea + size - 1, args[2].c_str()))
            throw std::runtime_error("watch_capacity");
        finish(); return;
    }
    if (verb == "provenance") {
        count(1);
        if (std::filesystem::exists(args[0])) throw std::runtime_error("output_exists");
        if (!m2_provenance_dump_json(args[0].c_str())) throw std::runtime_error("capture_failed");
        finish({}, "{\"path\":" + quote(args[0]) + "}"); return;
    }
    if (verb == "trace") {
        count(2);
        if (std::filesystem::exists(args[0])) throw std::runtime_error("output_exists");
        trace::TraceConfig config;
        config.file_path = args[0];
        config.enable_enter_exit = false;
        config.max_bytes = 8u * 1024u * 1024u;
        std::istringstream kinds(args[1]);
        std::string kind;
        while (std::getline(kinds, kind, ',')) {
            trace::EventKind parsed;
            if (!trace::event_kind_from_name(kind, &parsed)) throw std::runtime_error("unknown_trace_kind");
            config.filter.push_back(parsed);
        }
        if (config.filter.empty()) throw std::runtime_error("trace_filter_required");
        if (!trace::trace_init(config)) throw std::runtime_error("trace_open_failed");
        finish(); return;
    }
    input_sequence_ = 0;
    request_ticks_ = 0;
    field_.clear();
    uint64_t timeout_ms = 0;
    if (verb == "advance") {
        count(2);
        max_ticks_ = number(args[0], 12000);
        timeout_ms = number(args[1], 120000);
    } else if (verb == "wait") {
        count(5);
        field_ = args[0]; comparison_ = args[1];
        if (comparison_ != "eq" && comparison_ != "ne" && comparison_ != "ge")
            throw std::runtime_error("invalid_comparison");
        (void)value(field_);
        expected_ = number(args[2]);
        max_ticks_ = number(args[3], 12000);
        timeout_ms = number(args[4], 120000);
    } else if (verb == "input") {
        count(6);
        const auto buttons = number(args[0], UINT32_MAX);
        const auto x = number(args[1], 255), y = number(args[2], 255);
        const auto tap = number(args[3], 1);
        max_ticks_ = number(args[4], 12000);
        timeout_ms = number(args[5], 120000);
        if (!max_ticks_ || !timeout_ms) throw std::runtime_error("positive_budget_required");
        input_sequence_ = m2_diagnostic_input_submit(buttons, x, y, tap);
        if (!input_sequence_) throw std::runtime_error("input_unavailable");
        trace::trace_emit(trace::EventKind::Input, 0, 0, value("guest_frame"),
            "{\"origin\":\"diagnostic\",\"phase\":\"submitted\",\"input_sequence\":" +
            std::to_string(input_sequence_) + ",\"buttons\":" + std::to_string(buttons) +
            ",\"stick_x\":" + std::to_string(x) + ",\"stick_y\":" + std::to_string(y) +
            ",\"tap\":" + (tap ? "true}" : "false}"));
    } else throw std::runtime_error("unknown_command");
    if (!max_ticks_ || !timeout_ms) throw std::runtime_error("positive_budget_required");
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    active_ = true;
    // Satisfied predicates return without advancing guest time.
    if (!field_.empty()) {
        const auto current = value(field_);
        if ((comparison_ == "eq" && current == expected_) ||
            (comparison_ == "ne" && current != expected_) ||
            (comparison_ == "ge" && current >= expected_)) finish();
    }
#endif
}

bool DiagnosticControl::poll() {
    if (!enabled()) return true;
    accept_command(active_ ? 0 : 20);
    if (!active_ || quit_) return false;
#ifdef M2_HAVE_RECOMP
    m2_diagnostic_resume();
#endif
    return true;
}

void DiagnosticControl::after_tick() {
    if (!enabled()) return;
    ++ticks_;
    ++request_ticks_;
    if (!suspend()) { finish("pause_timeout"); return; }
#ifdef M2_HAVE_RECOMP
    int32_t task_state = -1;
    if (m2_guest_main_query(&task_state, nullptr) != 0 || task_state >= 2) {
        m2_diagnostic_input_cancel();
        finish("guest_terminated"); return;
    }
    bool done = request_.verb == "advance" && request_ticks_ >= max_ticks_;
    if (input_sequence_) {
        m2_diagnostic_input_status input{};
        m2_diagnostic_input_query(&input);
        done = input.completed >= input_sequence_;
    }
    if (!field_.empty()) {
        const auto current = value(field_);
        done = (comparison_ == "eq" && current == expected_) ||
               (comparison_ == "ne" && current != expected_) ||
               (comparison_ == "ge" && current >= expected_);
    }
    const std::string result = "{\"ticks_advanced\":" + std::to_string(request_ticks_) +
                               ",\"input_sequence\":" + std::to_string(input_sequence_) + "}";
    if (done) {
        if (input_sequence_) trace::trace_emit(trace::EventKind::Input, 0, 0, value("guest_frame"),
            "{\"origin\":\"diagnostic\",\"phase\":\"completed\",\"input_sequence\":" +
            std::to_string(input_sequence_) + "}");
        finish({}, result);
    }
    else if (request_ticks_ >= max_ticks_ || std::chrono::steady_clock::now() >= deadline_) {
        m2_diagnostic_input_cancel();
        finish(request_ticks_ >= max_ticks_ ? "tick_budget_exhausted" : "deadline_exceeded", result);
    }
#endif
}
} // namespace m2host
