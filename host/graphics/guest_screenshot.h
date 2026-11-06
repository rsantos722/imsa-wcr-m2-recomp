// guest_screenshot.h — screenshot geometry and framebuffer readback hook.
#pragma once

#include <cstdint>

namespace m2host::guest_screenshot {
// Both guests dump a whole display Bitmap: 16-bit RGB555+SSB, PPC big-endian.
// driving/game Dsply_screenshot writes 576x432 (0x79800 bytes) and shooter2d
// writerawimage writes its line-doubled 576x216 Bitmap (0x3cc00 bytes).
inline constexpr int width = 576;
inline constexpr int height = 432;
inline constexpr int shooter_height = 216;
inline constexpr uint32_t bytes = width * height * 2;
inline constexpr uint32_t shooter_bytes = width * shooter_height * 2;

// Rows in a dump of `count` bytes; 0 when no guest writes that size.
inline constexpr int height_for_bytes(uint32_t count) {
    return count == bytes ? height : count == shooter_bytes ? shooter_height : 0;
}
}

// Synchronous CPU read of an addressed TE surface.
extern "C" int32_t m2_guest_graphics_read_framebuffer_c(
    uint32_t base, void* dst, uint32_t count);
