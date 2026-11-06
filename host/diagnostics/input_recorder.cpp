// input_recorder.cpp — input recording and replay.
#include "input_recorder.h"
#include "platform.h"
#include "trace.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace m2host::trace {

InputRecorder& input_recorder() {
    static InputRecorder inst;
    return inst;
}

bool InputRecorder::wants_capture() const noexcept {
    return recording_.load(std::memory_order_acquire) || trace_input_enabled();
}

void InputRecorder::retain_record(const InputRecord& r) {
    if (buffer_.size() < kBufferedRecords) {
        buffer_.push_back(r);
        return;
    }
    buffer_[buffer_first_] = r;
    buffer_first_ = (buffer_first_ + 1) % kBufferedRecords;
}

std::vector<InputRecord> InputRecorder::buffer() const {
    std::vector<InputRecord> out;
    out.reserve(buffer_.size());
    if (buffer_.size() < kBufferedRecords) {
        out.insert(out.end(), buffer_.begin(), buffer_.end());
        return out;
    }
    for (size_t i = 0; i < kBufferedRecords; ++i)
        out.push_back(buffer_[(buffer_first_ + i) % kBufferedRecords]);
    return out;
}

bool InputRecorder::start_recording(const std::string& path) {
    if (path.empty()) return false;
    // Replace any previous destination rather than leaking its handle when a
    // second request fails to open.
    stop_recording();
    record_path_.clear();
    // argv reaches us as UTF-8 (platform::utf8_args).  The platform fopen
    // converts that path to the native wide form on Windows, where a narrow
    // fopen would interpret it in the ANSI code page.
    rec_file_ = m2host::platform::fopen(
        m2host::platform::path_from_utf8(path), "w");
    if (!rec_file_) {
        std::fprintf(stderr, "[input_recorder] failed to open %s for writing\n",
                     path.c_str());
        return false;
    }
    record_path_ = path;
    buffer_.clear();
    buffer_first_ = 0;
    rec_seq_ = 0;
    buffer_.reserve(kBufferedRecords);
    recording_.store(true, std::memory_order_release);
    std::fprintf(stderr, "[input_recorder] recording to %s\n", path.c_str());
    return true;
}
void InputRecorder::stop_recording() {
    recording_.store(false, std::memory_order_release);
    if (rec_file_) {
        std::fclose((FILE*)rec_file_);
        rec_file_ = nullptr;
    }
}

void InputRecorder::record(uint32_t buttons, uint32_t pressed, uint32_t released,
                           uint8_t stick_x, uint8_t stick_y, uint8_t device,
                           uint32_t guest_frame) {
    if (!recording_.load(std::memory_order_acquire)) {
        // With no recording file the trace is the only possible consumer.
        // Skip sequence/timestamp bookkeeping and the verification ring.
        if (trace_input_enabled())
            trace_input(buttons, pressed, released, stick_x, stick_y, device,
                        guest_frame);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    static auto start = now;
    if (rec_seq_ == 0) start = now;
    uint64_t host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

    InputRecord r;
    r.sequence = rec_seq_++;
    r.host_time = host_ms;
    r.guest_frame = guest_frame;
    r.buttons = buttons;
    r.pressed = pressed;
    r.released = released;
    r.stick_x = stick_x;
    r.stick_y = stick_y;
    r.device = device;
    retain_record(r);

    // Also emit via the trace as an input event
    trace_input(buttons, pressed, released, stick_x, stick_y, device, guest_frame);

    // Also write to recording file as JSON line (deterministic, independent of trace file)
    if (rec_file_) {
        // NDJSON with same fields as trace input payload plus sequence/host_time/guest_frame
        std::fprintf((FILE*)rec_file_,
            "{\"sequence\":%llu,\"host_time\":%llu,\"guest_frame\":%u,"
            "\"buttons\":%u,\"pressed\":%u,\"released\":%u,"
            "\"stick_x\":%u,\"stick_y\":%u,\"device\":%u}\n",
            (unsigned long long)r.sequence, (unsigned long long)r.host_time,
            r.guest_frame, r.buttons, r.pressed, r.released,
            (unsigned)r.stick_x, (unsigned)r.stick_y, (unsigned)r.device);
        std::fflush((FILE*)rec_file_);
    }
}

bool InputRecorder::start_replay(const std::string& path) {
    if (path.empty()) return false;
    stop_replay();
    replay_path_.clear();
    replay_queue_.clear();
    replay_pos_ = 0;
    replayed_ = 0;

    // Construct the stream from the native filesystem path so the path's
    // UTF-8 bytes are not reinterpreted under the Windows ANSI code page.
    std::ifstream f(m2host::platform::path_from_utf8(path));
    if (!f) {
        std::fprintf(stderr, "[input_recorder] failed to open %s for replay\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Minimal parse: look for keys via sscanf-like search
        // We expect exact format written above or trace-filtered input lines.
        // Try to parse both: recording file vs trace file (which has nested payload)
        InputRecord r{};
        // Try recording-file format first
        int n = std::sscanf(line.c_str(),
            "{\"sequence\":%*u,\"host_time\":%*u,\"guest_frame\":%u,\"buttons\":%u,\"pressed\":%u,\"released\":%u,\"stick_x\":%hhu,\"stick_y\":%hhu,\"device\":%hhu",
            &r.guest_frame, &r.buttons, &r.pressed, &r.released, &r.stick_x, &r.stick_y, &r.device);
        if (n < 7) {
            // Try trace file format: fields inside payload
            // Look for "event_kind":"input" then parse payload
            if (line.find("\"event_kind\":\"input\"") == std::string::npos) continue;
            // Extract payload values
            auto find_u32 = [&](const char* key, uint32_t* out) -> bool {
                std::string k = std::string("\"") + key + "\":";
                auto p = line.find(k);
                if (p == std::string::npos) return false;
                p += k.size();
                char* end = nullptr;
                long v = std::strtol(line.c_str() + p, &end, 10);
                if (end == line.c_str() + p) return false;
                *out = (uint32_t)v;
                return true;
            };
            uint32_t gf = 0, btn = 0, pr = 0, rel = 0;
            uint8_t sx = 128, sy = 128, dev = 0;
            find_u32("guest_frame", &gf);
            // payload buttons are inside payload object
            // payload extraction: find "buttons" after "payload"
            auto pay = line.find("\"payload\"");
            if (pay != std::string::npos) {
                std::string sub = line.substr(pay);
                auto fu32 = [&](const char* k, uint32_t* o) {
                    std::string kk = std::string("\"") + k + "\":";
                    auto pp = sub.find(kk);
                    if (pp == std::string::npos) return false;
                    pp += kk.size();
                    char* e = nullptr;
                    long vv = std::strtol(sub.c_str()+pp, &e, 10);
                    if (e==sub.c_str()+pp) return false;
                    *o=(uint32_t)vv; return true;
                };
                auto fu8 = [&](const char* k, uint8_t* o){ uint32_t t=0; if(!fu32(k,&t)) return false; *o=(uint8_t)t; return true; };
                fu32("buttons",&btn); fu32("pressed",&pr); fu32("released",&rel);
                fu8("stick_x",&sx); fu8("stick_y",&sy); fu8("device",&dev);
                // guest_frame for input is top-level guest_frame, already parsed as gf if needed
                // but trace's guest_frame is the same.
            }
            r.guest_frame = gf;
            r.buttons = btn; r.pressed = pr; r.released = rel;
            r.stick_x = sx; r.stick_y = sy; r.device = dev;
            // sequence from top-level sequence
            {
                auto p = line.find("\"sequence\":");
                if (p != std::string::npos) {
                    p += 11;
                    char* e=nullptr; long v=std::strtol(line.c_str()+p,&e,10);
                    if (e!=line.c_str()+p) r.sequence=(uint64_t)v;
                }
            }
            {
                auto p = line.find("\"host_time\":");
                if (p != std::string::npos) {
                    p+=12; char* e=nullptr; long v=std::strtol(line.c_str()+p,&e,10);
                    if (e!=line.c_str()+p) r.host_time=(uint64_t)v;
                }
            }
        } else {
            // sscanf succeeded but we lost sequence/host_time due to %* — re-parse
            auto p = line.find("\"sequence\":");
            if (p != std::string::npos) { p+=11; char* e=nullptr; long v=std::strtol(line.c_str()+p,&e,10); if(e!=line.c_str()+p) r.sequence=(uint64_t)v; }
            auto p2 = line.find("\"host_time\":");
            if (p2 != std::string::npos) { p2+=12; char* e=nullptr; long v=std::strtol(line.c_str()+p2,&e,10); if(e!=line.c_str()+p2) r.host_time=(uint64_t)v; }
        }
        replay_queue_.push_back(r);
    }
    replaying_ = !replay_queue_.empty();
    if (replaying_) {
        replay_path_ = path;
        std::fprintf(stderr, "[input_recorder] replaying %zu records from %s\n", replay_queue_.size(), path.c_str());
    } else {
        std::fprintf(stderr, "[input_recorder] replay file empty or no input events: %s\n", path.c_str());
    }
    return replaying_;
}
void InputRecorder::stop_replay() {
    replaying_ = false;
    replay_queue_.clear();
    replay_pos_ = 0;
}

bool InputRecorder::has_next_for_frame(uint32_t guest_frame) const {
    if (!replaying_ || replay_pos_ >= replay_queue_.size()) return false;
    return replay_queue_[replay_pos_].guest_frame == guest_frame ||
           replay_queue_[replay_pos_].guest_frame <= guest_frame;
}
bool InputRecorder::pop_for_frame(uint32_t guest_frame, InputRecord* out) {
    if (!replaying_ || replay_pos_ >= replay_queue_.size()) return false;
    // Return the next record whose guest_frame <= current frame.
    // If multiple records target the same frame, return them one by one.
    if (replay_queue_[replay_pos_].guest_frame > guest_frame) return false;
    if (out) *out = replay_queue_[replay_pos_];
    ++replay_pos_;
    ++replayed_;
    return true;
}
bool InputRecorder::pop_for_host_time(uint64_t elapsed_ms, InputRecord* out) {
    if (!replaying_ || replay_pos_ >= replay_queue_.size()) return false;
    const uint64_t origin = replay_queue_.front().host_time;
    const uint64_t timestamp = replay_queue_[replay_pos_].host_time;
    if (timestamp < origin || timestamp - origin > elapsed_ms) return false;
    return pop_next(out);
}
bool InputRecorder::pop_next(InputRecord* out) {
    if (!replaying_ || replay_pos_ >= replay_queue_.size()) return false;
    if (out) *out = replay_queue_[replay_pos_];
    ++replay_pos_;
    ++replayed_;
    return true;
}

bool recorder_init(const std::string& record_path, const std::string& replay_path) {
    auto& r = input_recorder();
    bool ok = true;
    if (!record_path.empty() && !r.start_recording(record_path)) {
        std::fprintf(stderr,
                     "[input_recorder] requested recording initialization failed: %s\n",
                     record_path.c_str());
        ok = false;
    }
    if (!replay_path.empty() && !r.start_replay(replay_path)) {
        std::fprintf(stderr,
                     "[input_recorder] requested replay initialization failed: %s\n",
                     replay_path.c_str());
        ok = false;
    }
    if (!ok) {
        // Do not leave a partially established evidence stream behind for a
        // run that is about to abort before guest execution.
        r.stop_recording();
        r.stop_replay();
    }
    return ok;
}
void recorder_record(uint32_t buttons, uint32_t pressed, uint32_t released,
                     uint8_t stick_x, uint8_t stick_y, uint8_t device,
                     uint32_t guest_frame) {
    input_recorder().record(buttons, pressed, released, stick_x, stick_y, device, guest_frame);
}
bool recorder_has_replay() { return input_recorder().is_replaying(); }
bool recorder_pop_for_frame(uint32_t guest_frame, InputRecord* out) {
    return input_recorder().pop_for_frame(guest_frame, out);
}

bool InputRecorder::start_broker_recording(const std::string& path) {
    if (path.empty()) return false;
    stop_broker_recording();
    broker_record_path_.clear();
    broker_rec_file_ = m2host::platform::fopen(
        m2host::platform::path_from_utf8(path), "w");
    if (!broker_rec_file_) {
        std::fprintf(stderr, "[input_recorder] broker failed to open %s for writing\n", path.c_str());
        return false;
    }
    broker_record_path_ = path;
    broker_recording_ = true;
    broker_buffer_.clear();
    broker_rec_seq_ = 0;
    std::fprintf(stderr, "[input_recorder] broker recording to %s\n", path.c_str());
    return true;
}
void InputRecorder::stop_broker_recording() {
    if (broker_rec_file_) {
        std::fclose((FILE*)broker_rec_file_);
        broker_rec_file_ = nullptr;
    }
    broker_recording_ = false;
}
void InputRecorder::record_broker(uint16_t what, int32_t x, int32_t y, uint32_t data,
                                  uint32_t guest_frame) {
    auto now = std::chrono::steady_clock::now();
    static auto bstart = now;
    if (broker_rec_seq_ == 0) bstart = now;
    uint64_t host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - bstart).count();
    BrokerRecord r;
    r.sequence = broker_rec_seq_++;
    r.host_time = host_ms;
    r.guest_frame = guest_frame;
    r.what = what;
    r.x = x; r.y = y; r.data = data;
    broker_buffer_.push_back(r);
    // Also emit via trace as input event with broker payload extension (keeps the trace schema)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"broker_what\":%u,\"broker_x\":%d,\"broker_y\":%d,\"broker_data\":%u}",
            (unsigned)what, x, y, data);
        // Use UnknownBehavior? Use Input kind with broker payload to stay within 14 kinds.
        trace_emit(EventKind::Input, 0, 0, guest_frame, buf);
    }
    if (broker_recording_ && broker_rec_file_) {
        std::fprintf((FILE*)broker_rec_file_,
            "{\"sequence\":%llu,\"host_time\":%llu,\"guest_frame\":%u,"
            "\"what\":%u,\"x\":%d,\"y\":%d,\"data\":%u}\n",
            (unsigned long long)r.sequence, (unsigned long long)r.host_time,
            r.guest_frame, (unsigned)what, x, y, data);
        std::fflush((FILE*)broker_rec_file_);
    }
}
bool InputRecorder::start_broker_replay(const std::string& path) {
    if (path.empty()) return false;
    stop_broker_replay();
    broker_replay_path_.clear();
    broker_replay_queue_.clear();
    broker_replay_pos_ = 0;
    broker_replayed_ = 0;
    std::ifstream f(m2host::platform::path_from_utf8(path));
    if (!f) {
        std::fprintf(stderr, "[input_recorder] broker failed to open %s for replay\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.find("\"what\":") == std::string::npos && line.find("\"broker_what\":") == std::string::npos) {
            if (line.find("\"event_kind\":\"input\"") != std::string::npos && line.find("broker_what") == std::string::npos) continue;
            if (line.find("\"event_kind\":\"input\"") == std::string::npos) continue;
        }
        BrokerRecord r{};
        auto find_i32 = [&](const char* key, int32_t* out) -> bool {
            std::string k = std::string("\"") + key + "\":";
            auto p = line.find(k);
            if (p == std::string::npos) return false;
            p += k.size();
            char* end = nullptr;
            long v = std::strtol(line.c_str() + p, &end, 10);
            if (end == line.c_str() + p) return false;
            *out = (int32_t)v;
            return true;
        };
        auto find_u32 = [&](const char* key, uint32_t* out) -> bool {
            std::string k = std::string("\"") + key + "\":";
            auto p = line.find(k);
            if (p == std::string::npos) return false;
            p += k.size();
            char* end = nullptr;
            long v = std::strtol(line.c_str() + p, &end, 10);
            if (end == line.c_str() + p) return false;
            *out = (uint32_t)v;
            return true;
        };
        auto find_u16 = [&](const char* key, uint16_t* out) -> bool {
            uint32_t t=0; if(!find_u32(key,&t)) return false; *out=(uint16_t)t; return true;
        };
        uint32_t gf=0; uint16_t what=0; int32_t x=0,y=0; uint32_t data=0;
        // Try broker file format first
        bool has_what = find_u16("what", &what) || find_u16("broker_what", &what);
        if (!has_what) continue;
        find_i32("x", &x); find_i32("broker_x", &x);
        find_i32("y", &y); find_i32("broker_y", &y);
        find_u32("data", &data); find_u32("broker_data", &data);
        find_u32("guest_frame", &gf);
        // sequence/host_time
        {
            auto p = line.find("\"sequence\":");
            if (p != std::string::npos) { p+=11; char* e=nullptr; long v=std::strtol(line.c_str()+p,&e,10); if(e!=line.c_str()+p) r.sequence=(uint64_t)v; }
            auto p2 = line.find("\"host_time\":");
            if (p2 != std::string::npos) { p2+=12; char* e=nullptr; long v=std::strtol(line.c_str()+p2,&e,10); if(e!=line.c_str()+p2) r.host_time=(uint64_t)v; }
        }
        r.guest_frame = gf; r.what = what; r.x = x; r.y = y; r.data = data;
        broker_replay_queue_.push_back(r);
    }
    broker_replaying_ = !broker_replay_queue_.empty();
    if (broker_replaying_) {
        broker_replay_path_ = path;
        std::fprintf(stderr, "[input_recorder] broker replaying %zu records from %s\n", broker_replay_queue_.size(), path.c_str());
    } else {
        std::fprintf(stderr, "[input_recorder] broker replay file empty: %s\n", path.c_str());
    }
    return broker_replaying_;
}
void InputRecorder::stop_broker_replay() {
    broker_replaying_ = false;
    broker_replay_queue_.clear();
    broker_replay_pos_ = 0;
}
bool InputRecorder::has_next_broker_for_frame(uint32_t guest_frame) const {
    if (!broker_replaying_ || broker_replay_pos_ >= broker_replay_queue_.size()) return false;
    return broker_replay_queue_[broker_replay_pos_].guest_frame <= guest_frame;
}
bool InputRecorder::pop_broker_for_frame(uint32_t guest_frame, BrokerRecord* out) {
    if (!broker_replaying_ || broker_replay_pos_ >= broker_replay_queue_.size()) return false;
    if (broker_replay_queue_[broker_replay_pos_].guest_frame > guest_frame) return false;
    if (out) *out = broker_replay_queue_[broker_replay_pos_];
    ++broker_replay_pos_;
    ++broker_replayed_;
    return true;
}
bool InputRecorder::pop_next_broker(BrokerRecord* out) {
    if (!broker_replaying_ || broker_replay_pos_ >= broker_replay_queue_.size()) return false;
    if (out) *out = broker_replay_queue_[broker_replay_pos_];
    ++broker_replay_pos_;
    ++broker_replayed_;
    return true;
}

bool broker_recorder_init(const std::string& record_path, const std::string& replay_path) {
    auto& r = input_recorder();
    bool ok = true;
    if (!record_path.empty() && !r.start_broker_recording(record_path)) {
        std::fprintf(stderr,
                     "[input_recorder] requested broker recording initialization failed: %s\n",
                     record_path.c_str());
        ok = false;
    }
    if (!replay_path.empty() && !r.start_broker_replay(replay_path)) {
        std::fprintf(stderr,
                     "[input_recorder] requested broker replay initialization failed: %s\n",
                     replay_path.c_str());
        ok = false;
    }
    if (!ok) {
        r.stop_broker_recording();
        r.stop_broker_replay();
    }
    return ok;
}
void recorder_record_broker(uint16_t what, int32_t x, int32_t y, uint32_t data,
                            uint32_t guest_frame) {
    input_recorder().record_broker(what, x, y, data, guest_frame);
}
bool recorder_has_broker_replay() { return input_recorder().is_broker_replaying(); }
bool recorder_pop_broker_for_frame(uint32_t guest_frame, BrokerRecord* out) {
    return input_recorder().pop_broker_for_frame(guest_frame, out);
}

} // namespace m2host::trace

extern "C" {
void m2_input_recorder_record(uint32_t buttons, uint32_t pressed,
                              uint32_t released, uint8_t stick_x,
                              uint8_t stick_y, uint8_t device,
                              uint32_t guest_frame) {
    m2host::trace::recorder_record(buttons, pressed, released, stick_x, stick_y, device, guest_frame);
}
int m2_input_recorder_has_replay(void) {
    return m2host::trace::recorder_has_replay() ? 1 : 0;
}
void m2_broker_recorder_record(uint16_t what, int32_t x, int32_t y, uint32_t data,
                               uint32_t guest_frame) {
    m2host::trace::recorder_record_broker(what, x, y, data, guest_frame);
}
int m2_broker_recorder_has_replay(void) {
    return m2host::trace::recorder_has_broker_replay() ? 1 : 0;
}
}
