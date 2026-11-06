// trace/input_recorder.h — input recording and replay.
//
// Records every input event with its guest-visible timestamp/sequence and
// can replay the same sequence deterministically.
//
// Design:
//   * Recording writes NDJSON to --record-input <path> (same trace schema
//     filtered to event_kind=input). Also emits via trace harness.
//   * Replay reads that file and injects events at the same guest_frame
//     numbers (--replay-input <path>) by default. The optional
//     --replay-input-host-time schedules the unchanged recording by elapsed
//     host milliseconds for manual runs whose guest counter parks in movies.
//     That mode requires real-time pacing and trace comparison; it does not
//     establish deterministic guest execution.
//   * A replay run must reproduce: input records, guest state transitions,
//     selector sequence, asset identifiers, frame completion count, final state.
//     The harness verifies this by comparing trace outputs, not wall-clock.
//   * The recorder is cheap when idle: InputMap::tick() and record() return
//     before reading the guest clock, formatting JSON, or touching the trace
//     lock unless recording or input tracing is active. While recording, a
//     fixed-capacity ring retains the most recent events for in-memory
//     comparison.
//
// Integration:
//   * host/input/input_map.cpp calls wants_capture() before reading the guest
//     frame counter and calls record() on each port rebuild while capture is
//     active.
//   * host/core/main.cpp checks recorder_has_replay() before polling SDL,
//     injecting recorded events instead of live input.
//
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace m2host::trace {

struct InputRecord {
    uint64_t sequence = 0;   // recorder sequence, deterministic
    uint64_t host_time = 0;  // ms since start
    uint32_t guest_frame = 0;
    uint32_t buttons = 0;
    uint32_t pressed = 0;
    uint32_t released = 0;
    uint8_t stick_x = 128;
    uint8_t stick_y = 128;
    uint8_t device = 0;
};

struct BrokerRecord {
    uint64_t sequence = 0;
    uint64_t host_time = 0;
    uint32_t guest_frame = 0;
    uint16_t what = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t data = 0;
};

class InputRecorder {
public:
    // Fixed capacity of the verification ring that retains recent recorded
    // events. Oldest entries are overwritten first.
    static constexpr size_t kBufferedRecords = 4096;

    bool start_recording(const std::string& path);
    void stop_recording();
    bool is_recording() const { return recording_.load(std::memory_order_acquire); }

    // True when an input poll would retain an event (recording) or emit a
    // trace record (input tracing enabled). InputMap::tick() checks this
    // before reading the guest frame counter or building a record.
    bool wants_capture() const noexcept;

    // Called from InputMap::tick() — records one port-0 snapshot.
    void record(uint32_t buttons, uint32_t pressed, uint32_t released,
                uint8_t stick_x, uint8_t stick_y, uint8_t device,
                uint32_t guest_frame);

    // Replay side
    bool start_replay(const std::string& path);
    void stop_replay();
    bool is_replaying() const { return replaying_; }
    bool has_next_for_frame(uint32_t guest_frame) const;
    // Pop next record that matches guest_frame, returns true when found.
    bool pop_for_frame(uint32_t guest_frame, InputRecord* out);
    // Elapsed milliseconds since the replay's first input poll. Recorded
    // timestamps are relative to the first record, independently of frames.
    bool pop_for_host_time(uint64_t elapsed_ms, InputRecord* out);
    // Pop next record regardless of frame for hosts without a guest frame
    // clock. Guest-linked hosts use pop_for_frame so future events are held.
    bool pop_next(InputRecord* out);

    // Chronological snapshot of the fixed-size ring that retains the most
    // recent recorded events for verification without reading the file back.
    std::vector<InputRecord> buffer() const;
    void clear_buffer() {
        buffer_.clear();
        buffer_first_ = 0;
        rec_seq_ = 0;
    }

    // Stats
    size_t recorded_count() const { return static_cast<size_t>(rec_seq_); }
    size_t buffered_count() const { return buffer_.size(); }
    size_t replayed_count() const { return replayed_; }

    // Broker (discrete M2EventRecord) — discrete input surface
    bool start_broker_recording(const std::string& path);
    void stop_broker_recording();
    bool is_broker_recording() const { return broker_recording_; }
    void record_broker(uint16_t what, int32_t x, int32_t y, uint32_t data,
                       uint32_t guest_frame);
    bool start_broker_replay(const std::string& path);
    void stop_broker_replay();
    bool is_broker_replaying() const { return broker_replaying_; }
    bool has_next_broker_for_frame(uint32_t guest_frame) const;
    bool pop_broker_for_frame(uint32_t guest_frame, BrokerRecord* out);
    bool pop_next_broker(BrokerRecord* out);
    const std::vector<BrokerRecord>& broker_buffer() const { return broker_buffer_; }
    size_t broker_recorded_count() const { return broker_buffer_.size(); }
    size_t broker_replayed_count() const { return broker_replayed_; }

private:
    void retain_record(const InputRecord& r);

    std::atomic<bool> recording_{false};
    bool replaying_ = false;
    std::string record_path_;
    std::string replay_path_;
    // Ring storage for the most recent recorded events.  buffer_first_ is
    // the oldest entry once the ring is full.
    std::vector<InputRecord> buffer_;
    size_t buffer_first_ = 0;
    std::vector<InputRecord> replay_queue_;  // loaded for replay
    size_t replay_pos_ = 0;
    size_t replayed_ = 0;
    uint64_t rec_seq_ = 0;
    // File handles — std::FILE* to avoid iostream overhead in hot path
    void* rec_file_ = nullptr; // FILE*
    // Broker side
    bool broker_recording_ = false;
    bool broker_replaying_ = false;
    std::string broker_record_path_;
    std::string broker_replay_path_;
    std::vector<BrokerRecord> broker_buffer_;
    std::vector<BrokerRecord> broker_replay_queue_;
    size_t broker_replay_pos_ = 0;
    size_t broker_replayed_ = 0;
    uint64_t broker_rec_seq_ = 0;
    void* broker_rec_file_ = nullptr; // FILE*
};

// Singleton accessors (process-wide, like trace)
InputRecorder& input_recorder();

// Convenience C++ helpers called from InputMap / main.cpp.  Paths are UTF-8;
// recorder_init returns false if any requested stream cannot be established
// (missing/empty replay input or an unwritable recording destination).
bool recorder_init(const std::string& record_path, const std::string& replay_path);
void recorder_record(uint32_t buttons, uint32_t pressed, uint32_t released,
                     uint8_t stick_x, uint8_t stick_y, uint8_t device,
                     uint32_t guest_frame);
bool recorder_has_replay();
bool recorder_pop_for_frame(uint32_t guest_frame, InputRecord* out);

// Broker helpers
bool broker_recorder_init(const std::string& record_path, const std::string& replay_path);
void recorder_record_broker(uint16_t what, int32_t x, int32_t y, uint32_t data,
                            uint32_t guest_frame);
bool recorder_has_broker_replay();
bool recorder_pop_broker_for_frame(uint32_t guest_frame, BrokerRecord* out);

}  // namespace m2host::trace

// C ABI for recomp layer (if needed)
extern "C" {
void m2_input_recorder_record(uint32_t buttons, uint32_t pressed,
                              uint32_t released, uint8_t stick_x,
                              uint8_t stick_y, uint8_t device,
                              uint32_t guest_frame);
int m2_input_recorder_has_replay(void);
void m2_broker_recorder_record(uint16_t what, int32_t x, int32_t y, uint32_t data,
                               uint32_t guest_frame);
int m2_broker_recorder_has_replay(void);
}
