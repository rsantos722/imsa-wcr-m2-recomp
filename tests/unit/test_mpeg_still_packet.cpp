// test_mpeg_still_packet.cpp — recovery path for one authored MPEG still.
#include "mpeg_still_packet.h"

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

int main() {
    std::vector<uint8_t> still(64, 0);
    const uint8_t prefix[12] = {0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x06,
                                0x18, 0x18, 0x18, 0x18, 0x18, 0x00};
    const uint8_t gop[4] = {0x00, 0x00, 0x01, 0xb8};
    const uint8_t picture[4] = {0x00, 0x00, 0x01, 0x00};
    for (int i = 0; i < 12; ++i) still[static_cast<size_t>(i)] = prefix[i];
    for (int i = 0; i < 4; ++i) {
        still[12 + static_cast<size_t>(i)] = gop[i];
        still[20 + static_cast<size_t>(i)] = picture[i];
    }
    still[25] = 0x08;  // picture coding type: 1
    still[40] = 0x42;

    const std::vector<uint8_t> prepared =
        m2host::prepare_mpeg_still_packet(still.data(), still.size(), 576, 432);
    expect(prepared.size() == still.size(), "prepared size");
    expect(prepared[0] == 0x00 && prepared[3] == 0xb3,
           "sequence header inserted");
    expect(prepared[12] == 0x00 && prepared[15] == 0xb8, "GOP preserved");
    expect(prepared[40] == 0x42, "payload preserved");

    expect(m2host::prepare_mpeg_still_packet(still.data(), still.size(), 640,
                                             480).empty(),
           "wrong dimensions are not rewritten");
    std::vector<uint8_t> changed = still;
    changed[0] = 0x0f;
    expect(m2host::prepare_mpeg_still_packet(changed.data(), changed.size(),
                                             576, 432).empty(),
           "different prefix is not rewritten");
    expect(m2host::prepare_mpeg_still_packet(still.data(), 20, 576, 432).empty(),
           "short input is not rewritten");

    if (g_failures != 0) {
        std::printf("m2_mpeg_still_packet_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_mpeg_still_packet_test: PASS\n");
    return 0;
}
