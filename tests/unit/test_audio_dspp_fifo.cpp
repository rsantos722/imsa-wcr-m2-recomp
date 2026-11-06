// test_audio_dspp_fifo.cpp — ordering and capacity of the DSPP output ring.
//
// The mixer drains two interleaved samples for every rendered frame, so the
// queue must preserve strict FIFO order across the wrap point, refuse to
// overrun, and keep startup priming (silence in front of the queued frames)
// in the same order the deque implementation provided.
#include "audio_shim.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++g_failures;
}

}  // namespace

// Friend of AudioShim: the ring helpers are private implementation details.
// A friend declaration names the class's innermost enclosing namespace, so the
// definition must live in m2host for access to apply.
namespace m2host {

int m2_audio_dspp_fifo_test_main(void) {
    m2host::AudioShim shim;
    expect(shim.init(true, true), "controlled audio init");
    expect(shim.dspp_queued_frames() == 0u, "fresh queue is empty");
    expect(shim.dspp_room_frames() == AudioShim::kDsppFifoCapFrames,
           "fresh queue has full capacity");

    // Frame order across pops; a lone sample is not consumed as a frame.
    const int16_t frames[] = {1, -1, 2, -2, 3, -3};
    for (int16_t sample : frames) shim.dspp_push_sample(sample);
    expect(shim.dspp_queued_frames() == 3u, "three frames queued");
    int16_t left = 0;
    int16_t right = 0;
    expect(shim.dspp_pop_pair(left, right) && left == 1 && right == -1,
           "first frame order");
    expect(shim.dspp_pop_pair(left, right) && left == 2 && right == -2,
           "second frame order");
    expect(shim.dspp_pop_pair(left, right) && left == 3 && right == -3,
           "third frame order");
    expect(shim.dspp_queued_frames() == 0u, "queue drained");
    shim.dspp_push_sample(7);
    expect(!shim.dspp_pop_pair(left, right) && left == 0 && right == 0,
           "partial frame is not consumed");
    shim.dspp_push_sample(-7);  // completes the pending frame
    expect(shim.dspp_pop_pair(left, right) && left == 7 && right == -7,
           "partial sample retained until its pair arrives");
    expect(shim.dspp_queued_frames() == 0u, "pair consumed");

    // Fill exactly to capacity; the next sample must be refused.
    // The producer always pushes whole stereo frames, so fill by pairs.
    uint32_t written = 0;
    while (shim.dspp_room_frames() > 0u) {
        shim.dspp_push_sample(static_cast<int16_t>(written & 0xFFFFu));
        shim.dspp_push_sample(static_cast<int16_t>((written + 1u) & 0xFFFFu));
        written += 2u;
    }
    expect(written == AudioShim::kDsppFifoCapSamples,
           "capacity in samples");
    expect(shim.dspp_room_frames() == 0u, "no room at capacity");
    shim.dspp_push_sample(0x1234);
    expect(shim.dspp_queued_frames() == AudioShim::kDsppFifoCapFrames,
           "overflow sample refused");

    // Drain everything and check the content survived the ring wrap in order.
    uint32_t read = 0;
    bool order_ok = true;
    while (shim.dspp_pop_pair(left, right)) {
        if (static_cast<uint16_t>(left) != (read & 0xFFFFu)) order_ok = false;
        ++read;
        if (static_cast<uint16_t>(right) != (read & 0xFFFFu)) order_ok = false;
        ++read;
    }
    expect(order_ok, "sample order across the ring wrap");
    expect(read == written, "all queued samples drained");
    expect(shim.dspp_queued_frames() == 0u, "queue empty after drain");

    // A wrapped push continues at the moved head without disturbing order.
    for (int i = 0; i < 5; ++i) {
        shim.dspp_push_sample(static_cast<int16_t>(100 + i));
        shim.dspp_push_sample(static_cast<int16_t>(-100 - i));
    }
    expect(shim.dspp_pop_pair(left, right) && left == 100 && right == -100,
           "post-wrap first frame");
    expect(shim.dspp_pop_pair(left, right) && left == 101 && right == -101,
           "post-wrap second frame");

    // Startup priming puts silence ahead of the queued frames.
    shim.dspp_prepend_silence(2u);
    expect(shim.dspp_queued_frames() == 5u, "priming adds frames");
    expect(shim.dspp_pop_pair(left, right) && left == 0 && right == 0,
           "primed silence first");
    expect(shim.dspp_pop_pair(left, right) && left == 0 && right == 0,
           "primed silence second");
    expect(shim.dspp_pop_pair(left, right) && left == 102 && right == -102,
           "queued frame follows priming");
    expect(shim.dspp_pop_pair(left, right) && left == 103 && right == -103,
           "priming keeps order");
    expect(shim.dspp_pop_pair(left, right) && left == 104 && right == -104,
           "priming keeps last frame");

    // Priming is clamped to the free space rather than overrunning the ring.
    const uint32_t free_frames = shim.dspp_room_frames();
    shim.dspp_prepend_silence(free_frames + 10u);
    expect(shim.dspp_room_frames() == 0u, "priming clamps to free space");

    shim.shutdown();

    // Device-backed behaviour: the mixer path without the controlled clock.
    m2host::AudioShim device;
    expect(device.init(true, false), "uncontrolled audio init");
    int16_t out[1024 * 2];
    const auto mix = [&](int frames) {
        std::fill_n(out, frames * 2, int16_t{0});
        device.mix_into(out, frames);
    };
    std::vector<int16_t> pcm;
    const auto push = [&](uint32_t frames, int16_t value) {
        pcm.assign(frames * 2u, value);
        device.push_dspp_frames(pcm.data(), frames);
    };

    // Before any callback the target is three startup blocks of 1024 frames.
    expect(device.dspp_fill_request() == 3072u, "startup fill target");

    // The target follows the device block, and a covered block plays exactly.
    push(800, 8000);
    mix(400);
    expect(out[0] == 8000 && out[799] == 8000, "full queue plays exact samples");
    expect(device.dspp_queued_frames() == 400u, "block drained from the queue");
    expect(device.dspp_fill_request() == 800u, "request tops up to three blocks");

    // A block the queue cannot fill fades the last frames down to the gap.
    mix(1000);
    expect(device.dspp_queued_frames() == 0u, "queue ran dry");
    expect(out[(399 - 128) * 2] == 8000, "samples before the fade are exact");
    bool falling = true;
    for (int i = 400 - 128; i < 400; ++i)
        if (out[i * 2] > out[(i - 1) * 2]) falling = false;
    expect(falling, "fade-out never rises");
    expect(out[399 * 2] < 100 && out[399 * 2 + 1] < 100, "fade-out reaches near zero");
    expect(out[400 * 2] == 0 && out[999 * 2] == 0, "gap is silent");

    // Silence already played is not owed: the request is unchanged by the gap.
    expect(device.dspp_fill_request() == 3000u, "gap adds nothing to the request");

    // Audio resumes at the normal level with a fade-in and no skipped samples.
    push(3000, 8000);
    mix(1000);
    expect(out[0] > 0 && out[0] < 100, "fade-in starts near zero");
    bool rising = true;
    for (int i = 1; i < 128; ++i)
        if (out[i * 2] < out[(i - 1) * 2]) rising = false;
    expect(rising, "fade-in never falls");
    expect(out[128 * 2] == 8000 && out[999 * 2] == 8000, "level restored after the fade");
    expect(device.dspp_queued_frames() == 2000u, "gap did not grow the queue");

    // A block that empties the queue exactly still fades, and audio refilled
    // before the next block fades back in rather than stepping up from silence.
    mix(1000);
    mix(1000);
    expect(device.dspp_queued_frames() == 0u, "queue emptied exactly");
    expect(out[871 * 2] == 8000, "exact samples before the block-end fade");
    expect(out[999 * 2] < 100, "fade-out reaches near zero at the block end");
    push(1000, 8000);
    mix(200);
    expect(out[0] > 0 && out[0] < 100, "refill after an exact drain fades in");
    expect(out[128 * 2] == 8000, "level restored after the exact-drain fade");
    mix(800);  // Drain the DSPP layer so the stream below plays alone.

    // CBD2: the predictor saturates and stays saturated, as sampler_cbd2_f2
    // stores it. A run past the rail must not delay the following descent.
    const uint8_t cbd2_bytes[] = {101, 101, 101, 101, 101, 101, 101, 101,
                                  199, 199, 199, 199};  // +16098 x4, -2893 x2
    expect(device.start_cbd2_stream(1u, 1u, cbd2_bytes, sizeof(cbd2_bytes), 1.0f),
           "cbd2 stream starts");
    mix(6);
    expect(out[0] == 16098 && out[2] == 32196, "cbd2 accumulates odd bytes");
    expect(out[4] == 32767 && out[6] == 32767, "cbd2 saturates at the rail");
    expect(out[8] == 32767 - 2893 && out[10] == 32767 - 5786,
           "cbd2 descends from the rail, not from an overshoot");

    // An Amplitude change reaches a stream that is already playing.
    const uint8_t gain_bytes[] = {101, 101, 100, 100, 100, 100};  // 16098, then 15625
    expect(device.start_cbd2_stream(2u, 2u, gain_bytes, sizeof(gain_bytes), 1.0f),
           "second stream starts");
    mix(1);
    expect(out[0] == 16098, "stream starts at its start gain");
    device.set_stream_gain(2u, 0.5f);
    device.set_stream_gain(99u, 0.0f);  // An unknown stream changes nothing.
    mix(1);
    expect(out[0] == 7812, "stream follows a later gain change");
    device.shutdown();

    if (g_failures != 0) {
        std::printf("m2_audio_dspp_fifo_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_audio_dspp_fifo_test: PASS\n");
    return 0;
}

}  // namespace m2host

int main() { return m2host::m2_audio_dspp_fifo_test_main(); }
