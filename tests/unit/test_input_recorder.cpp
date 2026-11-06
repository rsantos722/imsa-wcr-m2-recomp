// test_input_recorder.cpp — input/broker recorder initialization.
//
// Pins the idle fast path (no capture without a recording file or input
// tracing), the bounded verification ring, and the failure and success
// boundaries of recorder_init() / broker_recorder_init(): a missing or empty
// replay, or a recording destination whose parent is a regular file, must
// fail; a valid round trip through a non-ASCII directory must still record and
// replay unchanged fields.  The paths are UTF-8 like platform::utf8_args()
// supplies them.
#include "input_recorder.h"
#include "platform.h"
#include "trace.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace m2host::trace;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

std::string utf8(const fs::path& path)
{
    return m2host::platform::path_to_utf8(path);
}

}  // namespace

int main()
{
    std::error_code ec;
    const fs::path base =
        fs::temp_directory_path(ec) /
        ("m2_input_recorder_" + std::to_string(m2host::platform::process_id()));
    if (ec) {
        std::fprintf(stderr, "FAIL: temp_directory_path: %s\n",
                     ec.message().c_str());
        return 1;
    }
    fs::remove_all(base, ec);
    ec.clear();
    fs::create_directories(base, ec);
    if (ec) {
        std::fprintf(stderr, "FAIL: create temp root: %s\n",
                     ec.message().c_str());
        return 1;
    }

    // A regular file where the destination's parent directory should be.
    const fs::path blocker = base / "blocked-parent";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "not a directory\n";
    }
    const std::string blocked = utf8(blocker);
    const fs::path empty_replay = base / "empty.ndjson";
    { std::ofstream empty(empty_replay, std::ios::binary); }

    auto& recorder = input_recorder();
    recorder.stop_recording();
    recorder.stop_replay();
    recorder.stop_broker_recording();
    recorder.stop_broker_replay();

    // --- idle polling has no side effects --------------------------------------
    recorder.clear_buffer();
    check(!recorder.wants_capture(), "idle input recorder wants no capture");
    for (int i = 0; i < 32; ++i)
        recorder.record(1u, 1u, 0u, 128u, 128u, 0u, 0u);
    check(recorder.recorded_count() == 0,
          "idle input polls retain no records");
    check(recorder.buffered_count() == 0,
          "idle input polls leave the verification ring empty");

    // --- input stream failures -------------------------------------------------
    check(!recorder_init("", blocked + "/missing.ndjson"),
          "missing input replay fails initialization");
    check(!recorder.is_replaying(),
          "missing input replay leaves replay inactive");

    check(!recorder_init("", utf8(empty_replay)),
          "empty input replay fails initialization");
    check(!recorder.is_replaying(),
          "empty input replay leaves replay inactive");

    const fs::path no_input_replay = base / "no-input.ndjson";
    {
        std::ofstream out(no_input_replay, std::ios::binary);
        out << "{\"event_kind\":\"other\"}\n";
    }
    check(!recorder_init("", utf8(no_input_replay)),
          "replay without input events fails initialization");
    check(!recorder.is_replaying(),
          "input-free replay leaves replay inactive");

    check(!recorder_init(blocked + "/record.ndjson", ""),
          "unwritable input recording destination fails initialization");
    check(!recorder.is_recording(),
          "unwritable input recording leaves recording inactive");

    // --- broker stream failures ------------------------------------------------
    check(!broker_recorder_init("", blocked + "/missing-broker.ndjson"),
          "missing broker replay fails initialization");
    check(!recorder.is_broker_replaying(),
          "missing broker replay leaves replay inactive");

    check(!broker_recorder_init("", utf8(empty_replay)),
          "empty broker replay fails initialization");
    check(!recorder.is_broker_replaying(),
          "empty broker replay leaves replay inactive");

    check(!broker_recorder_init(blocked + "/broker.ndjson", ""),
          "unwritable broker recording destination fails initialization");
    check(!recorder.is_broker_recording(),
          "unwritable broker recording leaves recording inactive");

    // --- trace-only capture -----------------------------------------------------
    const fs::path trace_path = base / "input-trace.ndjson";
    {
        TraceConfig tcfg;
        tcfg.file_path = utf8(trace_path);
        tcfg.filter = {EventKind::Input};
        check(trace_init(tcfg), "input-only trace initializes");
        check(recorder.wants_capture(), "input tracing requests capture");
        recorder.record(0xa1u, 0xb2u, 0xc3u, 7u, 8u, 2u, 55u);
        check(recorder.recorded_count() == 0,
              "trace-only input poll keeps the recording ring empty");
        trace_shutdown();
        check(!recorder.wants_capture(),
              "input tracing shutdown disables capture");
    }
    {
        std::ifstream in(trace_path, std::ios::binary);
        const std::string trace_text((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        check(trace_text.find("\"event_kind\":\"input\"") != std::string::npos,
              "trace-only input poll still emits the input event");
        check(trace_text.find("\"buttons\":161") != std::string::npos,
              "trace-only input event retains the button word");
    }

    // --- valid input round trip through a UTF-8 path ---------------------------
    const fs::path unicode_dir =
        base / m2host::platform::path_from_utf8(
                   "evidence \xc3\xa4\xc3\xb6\xc3\xbc \xe6\x97\xa5\xe6\x9c\xac");
    fs::create_directories(unicode_dir, ec);
    check(!ec, "create UTF-8 evidence directory");
    const fs::path input_path = unicode_dir / "input.ndjson";

    check(recorder_init(utf8(input_path), ""),
          "valid input recording initializes");
    check(recorder.is_recording(), "valid input recording is active");
    recorder.record(0x11u, 0x22u, 0x33u, 12u, 34u, 1u, 40u);
    recorder.record(0x44u, 0x55u, 0x66u, 56u, 78u, 2u, 42u);
    recorder.stop_recording();
    check(fs::exists(input_path), "valid input recording creates its file");
    check(fs::file_size(input_path, ec) > 0 && !ec,
          "valid input recording contains NDJSON");

    check(recorder_init("", utf8(input_path)),
          "valid input replay initializes");
    check(recorder.is_replaying(), "valid input replay is active");
    InputRecord record{};
    check(recorder.pop_for_frame(40u, &record),
          "first input record schedules at its guest frame");
    check(record.buttons == 0x11u && record.pressed == 0x22u &&
              record.released == 0x33u && record.stick_x == 12u &&
              record.stick_y == 34u && record.device == 1u &&
              record.guest_frame == 40u,
          "first input record retains every field");
    check(!recorder.pop_for_frame(40u, &record),
          "future input record is held until its guest frame");
    check(recorder.pop_for_frame(42u, &record),
          "second input record schedules at its guest frame");
    check(record.buttons == 0x44u && record.device == 2u &&
              record.guest_frame == 42u,
          "second input record retains every field");
    check(!recorder.pop_for_frame(42u, &record),
          "replay ends after the last input record");
    recorder.stop_replay();

    // --- verification ring keeps only the newest events ------------------------
    const fs::path ring_path = unicode_dir / "ring.ndjson";
    check(recorder.start_recording(utf8(ring_path)), "ring recording starts");
    const uint32_t ring_events =
        static_cast<uint32_t>(InputRecorder::kBufferedRecords) + 3;
    for (uint32_t i = 0; i < ring_events; ++i)
        recorder.record(0u, 0u, 0u, 128u, 128u, 0u, i);
    check(recorder.recorded_count() == ring_events,
          "recording counts every input event");
    check(recorder.buffered_count() == InputRecorder::kBufferedRecords,
          "verification ring stays at its fixed capacity");
    const std::vector<InputRecord> buffered = recorder.buffer();
    check(buffered.size() == InputRecorder::kBufferedRecords,
          "buffer snapshot exposes the retained window");
    check(buffered.front().guest_frame ==
              ring_events - InputRecorder::kBufferedRecords,
          "verification ring drops the oldest events first");
    check(buffered.back().guest_frame == ring_events - 1,
          "verification ring ends at the newest event");
    recorder.stop_recording();

    // --- valid broker round trip ----------------------------------------------
    const fs::path broker_path = unicode_dir / "broker.ndjson";
    check(broker_recorder_init(utf8(broker_path), ""),
          "valid broker recording initializes");
    check(recorder.is_broker_recording(),
          "valid broker recording is active");
    recorder.record_broker(7u, -3, 4, 99u, 40u);
    recorder.record_broker(8u, 5, -6, 100u, 41u);
    recorder.stop_broker_recording();
    check(fs::exists(broker_path), "valid broker recording creates its file");
    check(fs::file_size(broker_path, ec) > 0 && !ec,
          "valid broker recording contains NDJSON");

    check(broker_recorder_init("", utf8(broker_path)),
          "valid broker replay initializes");
    check(recorder.is_broker_replaying(), "valid broker replay is active");
    BrokerRecord broker_record{};
    check(recorder.pop_broker_for_frame(40u, &broker_record),
          "first broker record schedules at its guest frame");
    check(broker_record.what == 7u && broker_record.x == -3 &&
              broker_record.y == 4 && broker_record.data == 99u &&
              broker_record.guest_frame == 40u,
          "first broker record retains every field");
    check(recorder.pop_broker_for_frame(41u, &broker_record),
          "second broker record schedules at its guest frame");
    check(broker_record.what == 8u && broker_record.x == 5 &&
              broker_record.y == -6 && broker_record.data == 100u &&
              broker_record.guest_frame == 41u,
          "second broker record retains every field");
    check(!recorder.pop_broker_for_frame(41u, &broker_record),
          "broker replay ends after the last record");
    recorder.stop_broker_replay();

    fs::remove_all(base, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "m2_input_recorder_test: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("m2_input_recorder_test: PASS\n");
    return 0;
}
