#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace m2host {
class GuestCltBridge;
class AudioShim;

enum class PresentWait : int {
    unpaced = 0,
    completed = 1,
    expired = 2,
    guest_ended = 3,
};

class DiagnosticControl {
public:
    using Capture = std::function<bool(const std::filesystem::path&)>;
    DiagnosticControl() = default;
    ~DiagnosticControl();
    bool open(const std::string& socket_path, GuestCltBridge* graphics,
              AudioShim* audio, Capture capture, std::string& error);
    bool enabled() const { return listener_ >= 0; }
    bool poll();
    void after_tick();
    bool quit_requested() const { return quit_; }
    bool suspend();
    void set_present_wait(PresentWait state) { present_wait_ = state; }

private:
    struct Request { std::string id, wire, verb; std::vector<std::string> args; };
    struct Cached { std::string id, wire, response; };
    int listener_ = -1;
    int client_ = -1;
    std::string socket_path_;
    uint64_t socket_inode_ = 0;
    // Fields marked [[maybe_unused]] serve guest commands, which exist only
    // in game-enabled builds.
    [[maybe_unused]] GuestCltBridge* graphics_ = nullptr;
    [[maybe_unused]] AudioShim* audio_ = nullptr;
    Capture capture_;
    Request request_;
    std::deque<Cached> cache_;
    bool active_ = false, quit_ = false;
    [[maybe_unused]] bool pause_failed_ = false;
    PresentWait present_wait_ = PresentWait::unpaced;
    uint64_t ticks_ = 0, request_ticks_ = 0;
    [[maybe_unused]] uint64_t input_sequence_ = 0;
    [[maybe_unused]] uint64_t max_ticks_ = 0;
    [[maybe_unused]] uint64_t expected_ = 0;
    std::string field_, comparison_;
    std::chrono::steady_clock::time_point deadline_;
    uint64_t value(const std::string& field) const;
    const char* present_wait_name() const;
    std::string state() const;
    void accept_command(int wait_ms);
    void execute();
    void finish(const std::string& error = {}, const std::string& result = "{}");
    std::string response(const std::string& id, const std::string& error,
                         const std::string& result) const;
    void send_response(int fd, const std::string& response) const;
};

}  // namespace m2host
