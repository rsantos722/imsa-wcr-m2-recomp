// diagnostic_perf.h — opt-in runtime performance measurements.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace m2host::perf {

enum class Phase : uint8_t {
    GuestExecution = 0,
    GuestFrameWait,
    GuestPayloadCopy,
    TextureSnapshotCopy,
    CltQueueWait,
    GraphicsBridgeMutexWait,
    CltInterpretation,
    RendererWork,
    GpuFeedbackCopy,
    FramebufferReadback,
    DiagnosticLogging,
    FileIO,
    kCount,
};

enum class Counter : uint8_t {
    GuestFramesStarted = 0,
    CltPayloadBytes,
    TextureSnapshotBytes,
    TextureSnapshotRanges,
    SnapshotObjectsCreated,
    SnapshotObjectsDestroyed,
    SnapshotRangesDeduplicated,
    SnapshotRangeVectorGrowths,
    ReadbackBufferGrowths,
    RetainedFrameBufferGrowths,
    DbSourceBufferGrowths,
    GpuFeedbackSnapshots,
    FileOperations,
    FileBytes,
    kCount,
};

const char* phase_name(Phase phase);
const char* counter_name(Counter counter);

bool init(std::string_view output_path = {});
bool enabled();

void record(Phase phase, uint64_t elapsed_ns);
void add_bytes(Phase phase, uint64_t bytes);
void add_counter(Counter counter, uint64_t value = 1);
uint64_t counter_value(Counter counter);
uint64_t phase_total_ns(Phase phase);
uint64_t phase_calls(Phase phase);

class Scope {
public:
    explicit Scope(Phase phase);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Phase m_phase;
    uint64_t m_started_ns = 0;
    bool m_active = false;
};

class TimedMutexLock {
public:
    explicit TimedMutexLock(std::mutex& mutex);
    ~TimedMutexLock() = default;

    std::unique_lock<std::mutex>& native_lock() { return m_lock; }

    TimedMutexLock(const TimedMutexLock&) = delete;
    TimedMutexLock& operator=(const TimedMutexLock&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
    uint64_t m_started_ns = 0;
    bool m_active = false;
};

struct RunSummary {
    std::string backend;
    uint64_t host_frames = 0;
    uint64_t guest_frames = 0;
    uint64_t clt_submitted = 0;
    uint64_t clt_dropped = 0;
    uint64_t clt_queue_full_drops = 0;
    uint64_t clt_malformed = 0;
    uint64_t clt_unsupported = 0;
    uint64_t clt_draws = 0;
    uint64_t clt_peak_queue = 0;
    uint64_t clt_peak_bytes = 0;
    uint64_t clt_peak_pump = 0;
    bool headless = false;
    bool paced = false;
    bool fast_forward = false;
};

bool finish(const RunSummary& summary);

}  // namespace m2host::perf

extern "C" void m2_perf_guest_frame_begin(uint32_t seq);
extern "C" void m2_perf_guest_frame_end(uint32_t seq);
