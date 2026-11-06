// test_audio_shim.cpp — exercise the host mixer voice interface without a device.
#include "audio_shim.h"

#include <cstdint>
#include <cstdio>

extern "C" {
uint32_t m2_host_audio_add_sample(const int16_t* pcm, uint32_t frames);
int32_t m2_host_audio_start_voice_indexed(uint32_t sample_id, int looping);
int32_t m2_host_audio_stop_voice(uint32_t voice_index);
int32_t m2_host_audio_voice_tap_frame(uint32_t voice_index, int16_t* l,
                                      int16_t* r);
}

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++g_failures;
}

}  // namespace

int main() {
    m2host::AudioShim shim;
    expect(shim.init(true, true), "controlled audio init");

    const int16_t pcm[] = {
        1000, -1000,
        2000, -2000,
        3000, -3000,
        4000, -4000,
    };
    const uint32_t sample = m2_host_audio_add_sample(pcm, 4);
    expect(sample != 0, "add_sample");

    const int32_t voice = m2_host_audio_start_voice_indexed(sample, 0);
    expect(voice >= 0, "start_voice_indexed");

    int16_t left = 0, right = 0;
    expect(m2_host_audio_voice_tap_frame(static_cast<uint32_t>(voice), &left,
                                         &right) != 0,
           "first tap");
    expect(left > 0 && right < 0, "first tap uses uploaded stereo PCM");

    expect(m2_host_audio_voice_tap_frame(static_cast<uint32_t>(voice), &left,
                                         &right) != 0,
           "second tap");
    expect(left > 0 && right < 0, "second tap uses uploaded stereo PCM");

    expect(m2_host_audio_stop_voice(static_cast<uint32_t>(voice)) == 0,
           "stop_voice");
    expect(m2_host_audio_voice_tap_frame(static_cast<uint32_t>(voice), &left,
                                         &right) == 0,
           "stopped voice has no tap");

    shim.shutdown();
    if (g_failures != 0) {
        std::printf("m2_audio_shim_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_audio_shim_test: PASS\n");
    return 0;
}
