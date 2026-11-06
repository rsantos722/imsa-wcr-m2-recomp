// Presentation-only fitting of the game's 4:3 image into a larger drawable.
//
// Windowed play locks the window's content aspect to 4:3, so the renderers
// size their targets to the whole window. A fullscreen drawable has the
// display's aspect instead; the image is then fitted inside it and the rest is
// left black (pillarbox on wide displays, letterbox on tall ones). Guest
// simulation and the guest's own 640x480 display surface are not affected;
// only the host target size and where it lands on the window change.
#pragma once

#include <SDL3/SDL.h>

#include <cstdlib>

namespace m2host {

struct LetterboxRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Largest 4:3 rectangle centred in a drawable. Degenerate sizes return the
// drawable itself so callers keep their previous non-zero size.
inline LetterboxRect letterbox_4x3(int drawable_w, int drawable_h) {
    LetterboxRect rect{0, 0, drawable_w, drawable_h};
    if (drawable_w <= 0 || drawable_h <= 0) return rect;
    if (static_cast<long long>(drawable_w) * 3 > static_cast<long long>(drawable_h) * 4) {
        rect.h = drawable_h;
        rect.w = static_cast<int>(static_cast<long long>(drawable_h) * 4 / 3);
    } else {
        rect.w = drawable_w;
        rect.h = static_cast<int>(static_cast<long long>(drawable_w) * 3 / 4);
    }
    rect.x = (drawable_w - rect.w) / 2;
    rect.y = (drawable_h - rect.h) / 2;
    return rect;
}

// M2_CROP_OVERSCAN: default-on presentation setting. The game draws its
// picture in a 576x432 carve inside the 640x480 display, leaving an authored
// margin; by default the carve is scaled to fill the window (or the fullscreen
// 4:3 fit). =0 keeps the original margin. Gameplay and guest state are
// unaffected.
inline bool overscan_crop_requested() {
    const char* value = std::getenv("M2_CROP_OVERSCAN");
    return !(value && value[0] == '0' && value[1] == '\0');
}

// M2_RENDER_SCALE: presentation setting for the GL backends' internal
// resolution. Unset or 0 keeps the original behavior: the guest targets are
// sized to the window's drawable (or its fullscreen 4:3 fit). N in 1..8 sizes
// them to N times the guest's 640x480 display regardless of the window, and
// the finished image is scaled to the window at present time (box-filtered
// down, or integer-prescaled then filtered up). Guest state is unaffected:
// the interpreter already scales guest coordinates to the target size.
constexpr int kMaxRenderScale = 8;
inline int render_scale_request() {
    const char* value = std::getenv("M2_RENDER_SCALE");
    if (!value || !value[0]) return 0;
    char* end = nullptr;
    const long scale = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || scale < 1) return 0;
    return scale > kMaxRenderScale ? kMaxRenderScale : static_cast<int>(scale);
}

// Target size for a render scale: the presentation size itself when the
// scale is 0 (match the window), else the scaled guest display.
inline void internal_render_size(int scale, int present_w, int present_h,
                                 int& out_w, int& out_h) {
    if (scale <= 0) {
        out_w = present_w;
        out_h = present_h;
        return;
    }
    out_w = 640 * scale;
    out_h = 480 * scale;
}

// Largest scale whose targets fit a device renderbuffer limit (at least 1).
inline int clamp_render_scale(int scale, int max_renderbuffer_size) {
    if (scale <= 0 || max_renderbuffer_size <= 0) return scale;
    while (scale > 1 && 640 * scale > max_renderbuffer_size) --scale;
    return scale;
}

// The region of the window's drawable the image occupies: the whole drawable
// when windowed (the aspect lock, or a deliberate non-4:3 diagnostic size,
// decides its shape), the centred 4:3 fit when fullscreen.
inline LetterboxRect presentation_rect(SDL_Window* window) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) return LetterboxRect{0, 0, w, h};
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0)
        return LetterboxRect{0, 0, w, h};
    return letterbox_4x3(w, h);
}

}  // namespace m2host
