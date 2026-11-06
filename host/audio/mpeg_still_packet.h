#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace m2host {

// MPS_InitMPEGStill configures 576x432 at the device boundary. One original UI
// still has this non-MPEG prefix instead of the sequence header shared by the
// other 15 stills. Return an empty vector for every other input so unknown
// forms retain the decoder's normal failure behavior.
inline std::vector<uint8_t> prepare_mpeg_still_packet(
    const uint8_t* bytes, size_t size, int width, int height) {
    static constexpr std::array<uint8_t, 12> kSequenceHeader = {
        0x00, 0x00, 0x01, 0xb3, 0x24, 0x01, 0xb0, 0x12,
        0xff, 0xff, 0xe0, 0x00};
    static constexpr std::array<uint8_t, 12> kPostRacePrefix = {
        0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x06, 0x18, 0x18,
        0x18, 0x18, 0x18, 0x00};
    static constexpr std::array<uint8_t, 4> kGopStart =
        {0x00, 0x00, 0x01, 0xb8};
    static constexpr std::array<uint8_t, 4> kPictureStart =
        {0x00, 0x00, 0x01, 0x00};
    if (!bytes || size < 28 || width != 576 || height != 432 ||
        std::memcmp(bytes, kPostRacePrefix.data(), kPostRacePrefix.size()) != 0 ||
        std::memcmp(bytes + 12, kGopStart.data(), kGopStart.size()) != 0 ||
        std::memcmp(bytes + 20, kPictureStart.data(), kPictureStart.size()) != 0 ||
        ((bytes[25] >> 3) & 7u) != 1u)
        return {};

    std::vector<uint8_t> prepared(bytes, bytes + size);
    std::memcpy(prepared.data(), kSequenceHeader.data(), kSequenceHeader.size());
    return prepared;
}

}  // namespace m2host
