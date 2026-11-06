// diagnostic_perf.cpp — opt-in runtime measurements for recovery probes.
#include "diagnostic_perf.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace m2host::perf {

namespace {

struct Metric {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};
    std::atomic<uint64_t> bytes{0};
};

std::array<Metric, static_cast<size_t>(Phase::kCount)> g_metrics;
std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::kCount)>
    g_counters;
std::atomic<bool> g_enabled{false};
std::string g_output_path;
uint64_t g_started_ns = 0;

uint64_t now_ns()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
            .count());
}

bool env_truthy(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0' && value[0] != '0' && value[0] != 'n' &&
           value[0] != 'N';
}

void update_max(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(previous, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

}  // namespace

const char* phase_name(Phase phase)
{
    switch (phase) {
    case Phase::GuestExecution: return "guest_execution";
    case Phase::GuestFrameWait: return "guest_frame_wait";
    case Phase::GuestPayloadCopy: return "guest_payload_copy";
    case Phase::TextureSnapshotCopy: return "texture_snapshot_copy";
    case Phase::CltQueueWait: return "clt_queue_wait";
    case Phase::GraphicsBridgeMutexWait: return "graphics_bridge_mutex_wait";
    case Phase::CltInterpretation: return "clt_interpretation";
    case Phase::RendererWork: return "renderer_work";
    case Phase::GpuFeedbackCopy: return "gpu_feedback_copy";
    case Phase::FramebufferReadback: return "framebuffer_readback";
    case Phase::DiagnosticLogging: return "diagnostic_logging";
    case Phase::FileIO: return "file_io";
    case Phase::kCount: break;
    }
    return "unknown";
}

const char* counter_name(Counter counter)
{
    switch (counter) {
    case Counter::GuestFramesStarted: return "guest_frames_started";
    case Counter::CltPayloadBytes: return "clt_payload_bytes";
    case Counter::TextureSnapshotBytes: return "texture_snapshot_bytes";
    case Counter::TextureSnapshotRanges: return "texture_snapshot_ranges";
    case Counter::SnapshotObjectsCreated: return "snapshot_objects_created";
    case Counter::SnapshotObjectsDestroyed: return "snapshot_objects_destroyed";
    case Counter::SnapshotRangesDeduplicated:
        return "snapshot_ranges_deduplicated";
    case Counter::SnapshotRangeVectorGrowths:
        return "snapshot_range_vector_growths";
    case Counter::ReadbackBufferGrowths: return "readback_buffer_growths";
    case Counter::RetainedFrameBufferGrowths:
        return "retained_frame_buffer_growths";
    case Counter::DbSourceBufferGrowths: return "db_source_buffer_growths";
    case Counter::GpuFeedbackSnapshots: return "gpu_feedback_snapshots";
    case Counter::FileOperations: return "file_operations";
    case Counter::FileBytes: return "file_bytes";
    case Counter::kCount: break;
    }
    return "unknown";
}

bool init(std::string_view output_path)
{
    g_enabled.store(false, std::memory_order_release);
    for (Metric& metric : g_metrics) {
        metric.calls.store(0, std::memory_order_relaxed);
        metric.total_ns.store(0, std::memory_order_relaxed);
        metric.max_ns.store(0, std::memory_order_relaxed);
        metric.bytes.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<uint64_t>& counter : g_counters)
        counter.store(0, std::memory_order_relaxed);
    g_output_path = std::string(output_path);
    const bool requested = !g_output_path.empty() || env_truthy("M2_PERF");
    g_started_ns = requested ? now_ns() : 0;
    g_enabled.store(requested, std::memory_order_release);
    return requested;
}

bool enabled()
{
    return g_enabled.load(std::memory_order_acquire);
}

void record(Phase phase, uint64_t elapsed_ns)
{
    if (!enabled() || phase >= Phase::kCount) return;
    Metric& metric = g_metrics[static_cast<size_t>(phase)];
    metric.calls.fetch_add(1, std::memory_order_relaxed);
    metric.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    update_max(metric.max_ns, elapsed_ns);
}

void add_bytes(Phase phase, uint64_t bytes)
{
    if (!enabled() || phase >= Phase::kCount) return;
    g_metrics[static_cast<size_t>(phase)].bytes.fetch_add(
        bytes, std::memory_order_relaxed);
}

void add_counter(Counter counter, uint64_t value)
{
    if (!enabled() || counter >= Counter::kCount) return;
    g_counters[static_cast<size_t>(counter)].fetch_add(
        value, std::memory_order_relaxed);
}

uint64_t counter_value(Counter counter)
{
    if (counter >= Counter::kCount) return 0;
    return g_counters[static_cast<size_t>(counter)].load(
        std::memory_order_relaxed);
}

uint64_t phase_total_ns(Phase phase)
{
    if (phase >= Phase::kCount) return 0;
    return g_metrics[static_cast<size_t>(phase)].total_ns.load(
        std::memory_order_relaxed);
}

uint64_t phase_calls(Phase phase)
{
    if (phase >= Phase::kCount) return 0;
    return g_metrics[static_cast<size_t>(phase)].calls.load(
        std::memory_order_relaxed);
}

Scope::Scope(Phase phase) : m_phase(phase)
{
    if (enabled()) {
        m_active = true;
        m_started_ns = now_ns();
    }
}

Scope::~Scope()
{
    if (m_active) record(m_phase, now_ns() - m_started_ns);
}

TimedMutexLock::TimedMutexLock(std::mutex& mutex)
    : m_lock(mutex, std::defer_lock)
{
    if (enabled()) {
        m_active = true;
        m_started_ns = now_ns();
    }
    m_lock.lock();
    if (m_active)
        record(Phase::GraphicsBridgeMutexWait, now_ns() - m_started_ns);
}

struct GuestFrameTimer {
    bool active = false;
    uint32_t seq = 0;
    uint64_t started_ns = 0;
};

thread_local GuestFrameTimer g_guest_frame;

void guest_frame_begin(uint32_t seq)
{
    g_counters[static_cast<size_t>(Counter::GuestFramesStarted)].fetch_add(
        1, std::memory_order_relaxed);
    if (!enabled()) return;
    if (g_guest_frame.active)
        record(Phase::GuestExecution, now_ns() - g_guest_frame.started_ns);
    g_guest_frame.active = true;
    g_guest_frame.seq = seq;
    g_guest_frame.started_ns = now_ns();
}

void guest_frame_end(uint32_t seq)
{
    (void)seq;
    if (!enabled() || !g_guest_frame.active) return;
    record(Phase::GuestExecution, now_ns() - g_guest_frame.started_ns);
    g_guest_frame.active = false;
}

namespace {

uint64_t atomic_load(const std::atomic<uint64_t>& value)
{
    return value.load(std::memory_order_relaxed);
}

void write_json(std::ostream& out, const RunSummary& summary, uint64_t elapsed_ns)
{
    out << "{\n"
        << "  \"schema\": \"m2-runtime-perf-v1\",\n"
        << "  \"elapsed_ns\": " << elapsed_ns << ",\n"
        << "  \"run\": {\n"
        << "    \"backend\": \"" << summary.backend << "\",\n"
        << "    \"host_frames\": " << summary.host_frames << ",\n"
        << "    \"guest_frames\": " << summary.guest_frames << ",\n"
        << "    \"headless\": " << (summary.headless ? "true" : "false")
        << ",\n"
        << "    \"paced\": " << (summary.paced ? "true" : "false") << ",\n"
        << "    \"fast_forward\": " << (summary.fast_forward ? "true" : "false")
        << "\n  },\n"
        << "  \"clt\": {\n"
        << "    \"submitted\": " << summary.clt_submitted << ",\n"
        << "    \"dropped\": " << summary.clt_dropped << ",\n"
        << "    \"queue_full_drops\": " << summary.clt_queue_full_drops << ",\n"
        << "    \"malformed\": " << summary.clt_malformed << ",\n"
        << "    \"unsupported\": " << summary.clt_unsupported << ",\n"
        << "    \"draws\": " << summary.clt_draws << ",\n"
        << "    \"peak_queue\": " << summary.clt_peak_queue << ",\n"
        << "    \"peak_bytes\": " << summary.clt_peak_bytes << ",\n"
        << "    \"peak_pump\": " << summary.clt_peak_pump << "\n  },\n"
        << "  \"phases\": {\n";

    for (size_t i = 0; i < static_cast<size_t>(Phase::kCount); ++i) {
        const Metric& metric = g_metrics[i];
        const uint64_t calls = atomic_load(metric.calls);
        const uint64_t total = atomic_load(metric.total_ns);
        const uint64_t max = atomic_load(metric.max_ns);
        const uint64_t bytes = atomic_load(metric.bytes);
        out << "    \"" << phase_name(static_cast<Phase>(i)) << "\": {\n"
            << "      \"calls\": " << calls << ",\n"
            << "      \"total_ns\": " << total << ",\n"
            << "      \"max_ns\": " << max << ",\n"
            << "      \"avg_ns\": " << (calls ? total / calls : 0) << ",\n"
            << "      \"bytes\": " << bytes << "\n"
            << "    }" << (i + 1 == static_cast<size_t>(Phase::kCount) ? "\n" : ",\n");
    }

    out << "  },\n  \"counters\": {\n";
    for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
        out << "    \"" << counter_name(static_cast<Counter>(i)) << "\": "
            << atomic_load(g_counters[i])
            << (i + 1 == static_cast<size_t>(Counter::kCount) ? "\n" : ",\n");
    }
    out << "  }\n}\n";
}

}  // namespace

bool finish(const RunSummary& summary)
{
    if (!enabled()) return true;
    const uint64_t elapsed_ns = now_ns() - g_started_ns;
    g_enabled.store(false, std::memory_order_release);

    bool ok = true;
    if (!g_output_path.empty()) {
        const std::filesystem::path path(g_output_path);
        std::error_code ec;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            std::fprintf(stderr, "[perf] failed to open %s\n",
                         g_output_path.c_str());
            ok = false;
        } else {
            write_json(out, summary, elapsed_ns);
            if (!out.good()) ok = false;
            std::printf("[perf] report=%s elapsed_ns=%llu\n",
                         g_output_path.c_str(),
                         static_cast<unsigned long long>(elapsed_ns));
        }
    } else {
        std::fprintf(stderr,
                     "[perf] elapsed_ns=%llu host_frames=%llu guest_frames=%llu "
                     "clt_submitted=%llu clt_dropped=%llu\n",
                     static_cast<unsigned long long>(elapsed_ns),
                     static_cast<unsigned long long>(summary.host_frames),
                     static_cast<unsigned long long>(summary.guest_frames),
                     static_cast<unsigned long long>(summary.clt_submitted),
                     static_cast<unsigned long long>(summary.clt_dropped));
    }
    return ok;
}

}  // namespace m2host::perf

extern "C" void m2_perf_guest_frame_begin(uint32_t seq)
{
    m2host::perf::guest_frame_begin(seq);
}

extern "C" void m2_perf_guest_frame_end(uint32_t seq)
{
    m2host::perf::guest_frame_end(seq);
}
