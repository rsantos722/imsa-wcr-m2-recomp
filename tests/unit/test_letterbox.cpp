// Fullscreen presentation fit: the game's 4:3 image keeps its aspect inside a
// display of any shape, centred, with the remainder left for black bars.
#include "letterbox.h"

#include <cstdio>
#include <cstdlib>

using m2host::LetterboxRect;
using m2host::letterbox_4x3;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
bool same(const LetterboxRect& r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}
}  // namespace

int main() {
    // 16:9 display -> pillarbox: height-limited, equal bars left and right.
    expect(same(letterbox_4x3(1920, 1080), 240, 0, 1440, 1080), "1920x1080 pillarbox");
    expect(same(letterbox_4x3(3840, 2160), 480, 0, 2880, 2160), "3840x2160 pillarbox");
    // 16:10 display (MacBook-class) -> still pillarbox.
    expect(same(letterbox_4x3(2560, 1600), 213, 0, 2133, 1600), "2560x1600 pillarbox");
    // Taller than 4:3 (portrait or 5:4) -> letterbox: width-limited.
    expect(same(letterbox_4x3(1280, 1024), 0, 32, 1280, 960), "1280x1024 letterbox");
    expect(same(letterbox_4x3(1080, 1920), 0, 555, 1080, 810), "portrait letterbox");
    // Exactly 4:3 fills the drawable; no bars.
    expect(same(letterbox_4x3(1024, 768), 0, 0, 1024, 768), "4:3 fills the display");
    // The fitted rectangle never exceeds the drawable and stays 4:3 within
    // integer rounding for a spread of shapes.
    const int sizes[][2] = {{1366, 768}, {1440, 900}, {2560, 1080}, {3440, 1440},
                            {800, 600}, {641, 479}, {7680, 4320}};
    for (const auto& s : sizes) {
        const LetterboxRect r = letterbox_4x3(s[0], s[1]);
        expect(r.x >= 0 && r.y >= 0 && r.x + r.w <= s[0] && r.y + r.h <= s[1],
               "fit stays inside the drawable");
        expect(r.w * 3 - r.h * 4 <= 3 && r.w * 3 - r.h * 4 >= -3, "fit keeps 4:3");
        expect(r.x == s[0] - r.w - r.x || r.x == s[0] - r.w - r.x - 1 ||
                   r.x == s[0] - r.w - r.x + 1, "fit is horizontally centred");
        expect(r.y == s[1] - r.h - r.y || r.y == s[1] - r.h - r.y - 1 ||
                   r.y == s[1] - r.h - r.y + 1, "fit is vertically centred");
    }
    // Degenerate sizes are returned unchanged rather than dividing to zero.
    expect(same(letterbox_4x3(0, 0), 0, 0, 0, 0), "zero drawable");
    expect(same(letterbox_4x3(100, 0), 0, 0, 100, 0), "zero height");

    // M2_RENDER_SCALE: 0 follows the presentation size; N fixes the targets
    // to N times the guest's 640x480 display whatever the window is.
    int tw = 0, th = 0;
    m2host::internal_render_size(0, 1440, 1080, tw, th);
    expect(tw == 1440 && th == 1080, "scale 0 matches the presentation");
    m2host::internal_render_size(1, 1440, 1080, tw, th);
    expect(tw == 640 && th == 480, "scale 1 is the guest display");
    m2host::internal_render_size(3, 800, 600, tw, th);
    expect(tw == 1920 && th == 1440, "scale 3 ignores the window size");
    // A device renderbuffer limit lowers the scale but never below 1x.
    expect(m2host::clamp_render_scale(8, 16384) == 8, "8x fits a 16k limit");
    expect(m2host::clamp_render_scale(8, 4096) == 6, "8x clamps to 6x at 4k");
    expect(m2host::clamp_render_scale(4, 512) == 1, "clamp floors at 1x");
    expect(m2host::clamp_render_scale(0, 4096) == 0, "window mode is unclamped");
    const struct { const char* value; int scale; } requests[] = {
        {"", 0}, {"0", 0}, {"1", 1}, {"4", 4}, {"8", 8}, {"12", 8},
        {"-2", 0}, {"2x", 0}, {"abc", 0}};
    for (const auto& request : requests) {
        set_env("M2_RENDER_SCALE", request.value);
        expect(m2host::render_scale_request() == request.scale,
               "M2_RENDER_SCALE parse");
    }
    set_env("M2_RENDER_SCALE", "");
    expect(m2host::render_scale_request() == 0, "empty M2_RENDER_SCALE");
    if (failures == 0) std::puts("letterbox_test: ok");
    return failures == 0 ? 0 : 1;
}
