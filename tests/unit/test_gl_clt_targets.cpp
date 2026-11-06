// Real-context contract for the bounded addressed CLT color-target pool used by
// both OpenGL backends (host/graphics/gl_clt_targets.h).
//
// This lives outside the `m2_guest_clt` gate on purpose: the pool's overflow
// policy deletes framebuffer objects and renderbuffers and re-checks
// framebuffer completeness, so it cannot be proven by a recording backend that
// never touches GL. The interpreter gate is also deliberately SDL-free, and a
// machine with no usable GL context must skip rather than fail — hence the
// exit code 3 that the CMake entry maps to SKIP_RETURN_CODE.
//
// The pre-fix shape raised std::runtime_error("CLT color target capacity
// exceeded") at the seventeenth distinct DBDESTBASEADDR base, so reaching the
// overflow arm at all is the regression: the process must survive it, keep the
// requested target usable, keep the active and presented targets intact, and
// account for every address it gave back.

#include "gl_clt_targets.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
// The production pool bound. Kept as a literal here on purpose: if the header's
// bound moves, the fixture's 17-base arm has to move with it deliberately
// rather than silently stop testing the overflow.
constexpr size_t kPoolCapacity = 16;
constexpr uint32_t kFirstBase = 0x10000u;
constexpr uint32_t kBaseStep = 0x1000u;
constexpr size_t kFloodBases = 40;

uint32_t base_at(size_t index) { return kFirstBase + index * kBaseStep; }

// Distinct per index, always inside [0,1] so an authored channel never clips
// to the clear color, and zero in green/blue so "re-created from the host
// clear" is unambiguous.
void authored_color(size_t index, float rgb[3])
{
    rgb[0] = static_cast<float>(16u + (index % 15u) * 15u) / 255.f;
    rgb[1] = 0.f;
    rgb[2] = 0.f;
}

bool expect(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "m2_gl_clt_targets_test: FAIL: %s\n", message);
    return false;
}

bool near_rgb(const std::vector<uint32_t>& pixels, const float rgb[3], float alpha,
          const char* what, uint32_t base)
{
    const int want[4] = {static_cast<int>(rgb[0] * 255.f + 0.5f),
                         static_cast<int>(rgb[1] * 255.f + 0.5f),
                         static_cast<int>(rgb[2] * 255.f + 0.5f),
                         static_cast<int>(alpha * 255.f + 0.5f)};
    for (size_t i = 0; i < pixels.size(); ++i) {
        // The addressed readback is the same 0xAABBGGRR word the software
        // backend and the interpreter use, so channel order follows it.
        const uint32_t word = pixels[i];
        const int got[4] = {static_cast<int>(word & 0xffu),
                            static_cast<int>((word >> 8) & 0xffu),
                            static_cast<int>((word >> 16) & 0xffu),
                            static_cast<int>((word >> 24) & 0xffu)};
        for (int c = 0; c < 4; ++c) {
            if (got[c] < want[c] - 1 || got[c] > want[c] + 1) {
                std::fprintf(stderr,
                             "m2_gl_clt_targets_test: FAIL: %s "
                             "(base=%#x pixel %zu channel %d: got %d want %d)\n",
                             what, base, i, c, got[c], want[c]);
                return false;
            }
        }
    }
    return true;
}

// Author a per-base color into whichever target select() just bound, and read
// it back. The readback is the observation: it fails if select() bound the
// drawable, another base's FBO, or a target whose storage was not created.
bool author_and_verify(m2host::GlCltTargets& targets, uint32_t base,
                       size_t index)
{
    float rgb[3];
    authored_color(index, rgb);
    targets.select(base);
    if (targets.active_base() != base) {
        std::fprintf(stderr,
                     "m2_gl_clt_targets_test: FAIL: select(%#x) left "
                     "active=%#x\n",
                     base, targets.active_base());
        return false;
    }
    targets.clear_color(rgb[0], rgb[1], rgb[2], 1.f);
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0u);
    if (!targets.read(base, pixels.data(), kWidth, kHeight)) {
        std::fprintf(stderr,
                     "m2_gl_clt_targets_test: FAIL: read(%#x) reported no "
                     "target\n",
                     base);
        return false;
    }
    return near_rgb(pixels, rgb, 1.f, "authored target color", base);
}

int run(m2host::GlCltTargets& targets)
{
    if (!expect(targets.pool_size() == 0 && targets.releases() == 0 &&
                    targets.refusals() == 0,
                "a fresh pool starts empty with no capacity event"))
        return 1;
    targets.set_depth_clear(1.f);
    targets.resize(kWidth, kHeight, 0.05f, 0.07f, 0.10f);

    // Fill the pool to its bound, one authored color per base.
    for (size_t i = 0; i < kPoolCapacity; ++i)
        if (!author_and_verify(targets, base_at(i), i)) return 1;
    if (!expect(targets.pool_size() == kPoolCapacity,
                "sixteen addressed targets coexist at the bound"))
        return 1;

    // Establish the two bases a release must never take: the active one
    // (re-selecting it also makes it the most recently used, so a plain
    // oldest-first release would have to skip it) and the presented one.
    const uint32_t kActive = base_at(0);
    const uint32_t kPresent = base_at(1);
    targets.select(kActive);
    targets.select_scanout(kPresent);

    // Overflow arm: the seventeenth distinct base.
    const uint32_t kSeventeenth = base_at(kPoolCapacity);
    targets.select(kSeventeenth);
    if (!expect(targets.pool_size() == kPoolCapacity,
                "the pool stays bounded after a seventeenth base"))
        return 1;
    if (!expect(targets.refusals() == 0,
                "a releasable victim exists, so the request is never refused"))
        return 1;
    if (!expect(targets.releases() == 1,
                "the seventeenth base accounts for exactly one release"))
        return 1;
    if (!expect(targets.active_base() == kSeventeenth,
                "the requested base becomes the active target"))
        return 1;

    // The required targets keep their content. The active base was re-selected
    // immediately before the flood, so only a least-recently-used policy that
    // excludes both protected bases can land here with the authored color
    // intact; releasing the active or the presented base leaves it unpooled,
    // and a release that dropped another base's storage would erase its color.
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0u);
    float active_rgb[3];
    authored_color(0, active_rgb);
    if (!expect(targets.read(kActive, pixels.data(), kWidth, kHeight),
                "the active target survives the release"))
        return 1;
    if (!near_rgb(pixels, active_rgb, 1.f, "active target content", kActive))
        return 1;
    float present_rgb[3];
    authored_color(1, present_rgb);
    if (!expect(targets.read(kPresent, pixels.data(), kWidth, kHeight),
                "the presented target survives the release"))
        return 1;
    if (!near_rgb(pixels, present_rgb, 1.f, "presented target content", kPresent))
        return 1;

    // The released base is exactly the least-recently selected unprotected one:
    // base 0 was re-selected (newest), base 1 is presented, base 2 is the
    // oldest remaining. Checking the whole set rejects a "lowest address",
    // "oldest entry", "presented base" or "newest" victim policy, each of which
    // would still produce a plausible-looking pool.
    const uint32_t kExpectedVictim = base_at(2);
    if (!expect(!targets.pooled(kExpectedVictim),
                "the least-recently selected unprotected base is released"))
        return 1;
    for (size_t i = 0; i < kPoolCapacity; ++i) {
        const uint32_t base = base_at(i);
        if (base == kExpectedVictim) continue;
        if (!expect(targets.pooled(base), "every non-victim base is retained"))
            return 1;
    }

    // The requested target is a real, independently writable framebuffer.
    if (!author_and_verify(targets, kSeventeenth, kPoolCapacity)) return 1;

    // Re-selecting the released base re-creates it from the host clear color.
    // That loss is the documented consequence of the release, and asserting it
    // keeps a policy that "releases" by aliasing the new base, or that keeps the
    // old storage alive under a new name, from passing. The pool was still full
    // at that point, so exactly one further address is given back.
    targets.select(kExpectedVictim);
    if (!expect(targets.pool_size() == kPoolCapacity,
                "re-selecting the released base re-fills the pool"))
        return 1;
    if (!expect(targets.releases() == 2,
                "re-selecting a released base releases one more address"))
        return 1;
    if (!expect(targets.read(kExpectedVictim, pixels.data(), kWidth, kHeight),
                "the re-created released base is readable"))
        return 1;
    const float host_clear[3] = {0.05f, 0.07f, 0.10f};
    if (!near_rgb(pixels, host_clear, 1.f,
              "re-created target starts from the host clear", kExpectedVictim))
        return 1;

    // Flood arm: a long stream of distinct bases stays bounded, never turns a
    // capacity event into a refusal, and keeps the one base whose protection
    // does not move -- the presented plane. Protection follows the *current*
    // selection, so the base that was active before the flood is an ordinary
    // resident afterwards and ages out on its own; that is asserted too, so a
    // policy that pinned the first active base forever cannot pass.
    const uint64_t releases_before = targets.releases();
    for (size_t i = kPoolCapacity + 1;
         i < kPoolCapacity + 1u + kFloodBases; ++i) {
        targets.select(base_at(i));
        if (!expect(targets.pool_size() == kPoolCapacity,
                    "the pool stays bounded through a long base stream"))
            return 1;
        if (!expect(targets.refusals() == 0,
                    "a long base stream produces no refusal"))
            return 1;
        if (!expect(targets.pooled(kPresent),
                    "the presented target is never the victim"))
            return 1;
    }
    if (!expect(targets.releases() - releases_before == kFloodBases,
                "every further distinct base accounts for one release"))
        return 1;
    if (!expect(!targets.pooled(kActive),
                "protection follows the current selection, not the first one"))
        return 1;
    if (!expect(targets.read(kPresent, pixels.data(), kWidth, kHeight),
                "the presented target survives the whole stream"))
        return 1;
    if (!near_rgb(pixels, present_rgb, 1.f, "presented target after the stream",
              kPresent))
        return 1;

    // A base first seen after the releases is created, bound, and writable.
    if (!author_and_verify(targets, base_at(kPoolCapacity + 1u + kFloodBases),
                           kPoolCapacity + 1u + kFloodBases))
        return 1;

    // present() returns without binding the drawable when the presented base is
    // no longer pooled, so the draw-framebuffer binding is the observable for
    // "the presented plane is still there to blit".
    GLint bound_before = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_before);
    if (!expect(bound_before != 0,
                "a selected target is bound before the present"))
        return 1;
    targets.present(false);
    GLint bound_after = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_after);
    if (!expect(bound_after == 0,
                "present() still finds the presented base after the releases"))
        return 1;

    targets.shutdown();
    return 0;
}

// MSAA storage arm (M2_ANTIALIASING): a separate pool so set_samples() lands
// before any addressed target exists. Creation, the resolve-backed full read,
// the resolve-backed window read and present must all keep working, and the
// pool must report the granted sample count it was asked for.
int run_msaa()
{
    m2host::GlCltTargets targets;
    if (!targets.init()) {
        std::printf("m2_gl_clt_targets_test: SKIP MSAA arm: no FBO entry "
                    "points\n");
        return 3;
    }
    const int usable = targets.usable_samples();
    if (usable < 2) {
        std::printf("m2_gl_clt_targets_test: MSAA arm skipped: "
                    "GL_MAX_SAMPLES=%d\n", usable);
        targets.shutdown();
        return 0;
    }
    const int samples = usable >= 4 ? 4 : usable;
    targets.set_samples(samples);
    if (!expect(targets.samples() == samples,
                "the granted sample count is recorded"))
        return 1;
    targets.set_depth_clear(1.f);
    targets.resize(kWidth, kHeight, 0.05f, 0.07f, 0.10f);

    const uint32_t base = 0x20000u;
    if (!author_and_verify(targets, base, 3)) return 1;

    // Windowed reads resolve the requested window before glReadPixels.
    std::vector<uint32_t> window(16u * 16u, 0u);
    if (!expect(targets.read_window(base, window.data(), 8, 8, 16, 16),
                "a multisample target serves a windowed read"))
        return 1;
    float rgb[3];
    authored_color(3, rgb);
    if (!near_rgb(window, rgb, 1.f, "multisample windowed read content", base))
        return 1;

    // The presented plane resolves to the drawable.
    targets.select_scanout(base);
    targets.select(base);
    targets.present();
    targets.shutdown();
    return 0;
}

}  // namespace

int run_scanout_projection(int samples)
{
    m2host::GlCltTargets targets;
    if (!targets.init()) return 3;
    targets.set_samples(samples);
    targets.resize(kWidth, kHeight, 0, 0, 0);
    const uint32_t base = 0x30000;
    targets.select(base);
    targets.select_scanout(base);
    glDisable(GL_SCISSOR_TEST);
    targets.clear_color(0, 0, 1, 1);
    // Raw source rows at y=4..19. Alternating red/green stripes detect both
    // row doubling and a Y-inverted source window on the actual drawable.
    glEnable(GL_SCISSOR_TEST);
    for (int y = 4; y < 20; ++y) {
        glScissor(8, kHeight - y - 1, 32, 1);
        targets.clear_color((y & 1) ? 0 : 1, (y & 1) ? 1 : 0, 0, 1);
    }
    glDisable(GL_SCISSOR_TEST);
    targets.set_scanout_rect(base, {8, 4, 32, 16, 32, 32});
    targets.present();
    std::vector<uint32_t> drawable(kWidth * kHeight);
    // Single-buffer context: read before any SDL swap, independently of the
    // CPU capture's projection helper. GL rows here run bottom to top.
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, drawable.data());
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            uint32_t expected = 0xffff0000u;
            if (x >= 8 && x < 40 && y >= 4 && y < 36)
                expected = ((y - 4) / 2) & 1 ? 0xff00ff00u : 0xff0000ffu;
            if (!expect((drawable[(kHeight - 1 - y) * kWidth + x] & 0xffffffu) ==
                        (expected & 0xffffffu),
                        "drawable preserves borders and doubles source rows"))
                return 1;
        }
    targets.shutdown();
    return 0;
}

// M2_CROP_OVERSCAN: the guest's viewport carve is presented scaled to fill the
// drawable, so the authored margin around it disappears. Independent of the
// implementation, a carve of one colour inside a margin of another must leave
// the whole drawable the carve colour; the Projector source pixels (not the
// expanded destination) are what get scaled; and with the option off the margin
// stays. Reads the drawable the same way as run_scanout_projection.
int run_overscan_crop(int samples)
{
    m2host::GlCltTargets targets;
    if (!targets.init()) return 3;
    targets.set_samples(samples);
    targets.resize(kWidth, kHeight, 0, 0, 0);
    const uint32_t base = 0x40000;
    targets.select(base);
    targets.select_scanout(base);
    std::vector<uint32_t> drawable(kWidth * kHeight);
    const auto read_drawable = [&]() {
        glReadBuffer(GL_FRONT);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                     drawable.data());
    };
    const auto rgb_at = [&](int x, int y) {
        return drawable[(kHeight - 1 - y) * kWidth + x] & 0xffffffu;
    };
    // Margin blue, carve (8,4)..(40,28) red. Rect width/height equal the
    // source size, so there is no Projector expansion.
    const auto author = [&](int x, int y, int w, int h) {
        glDisable(GL_SCISSOR_TEST);
        targets.clear_color(0, 0, 1, 1);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, kHeight - y - h, w, h);
        targets.clear_color(1, 0, 0, 1);
        glDisable(GL_SCISSOR_TEST);
    };
    // Bilinear scaling blends the outermost output pixels with the margin
    // beside the carve (up to half a source pixel, 4 output pixels at the
    // largest scale here); the interior is exact.
    constexpr int kEdge = 6;
    const uint32_t red = 0x0000ffu, blue = 0xff0000u;  // GL_RGBA read as ABGR
    author(8, 4, 32, 24);
    targets.set_scanout_rect(base, {8, 4, 32, 24, 32, 24});
    targets.set_crop_overscan(true);
    targets.present();
    read_drawable();
    for (int y = kEdge; y < kHeight - kEdge; ++y)
        for (int x = kEdge; x < kWidth - kEdge; ++x)
            if (!expect(rgb_at(x, y) == red,
                        "cropped drawable is filled by the carve colour"))
                return 1;

    // Projector expansion: only the 16x12 source at (8,4) is red; the
    // expanded 32x24 destination would also cover blue surroundings if the
    // destination (not the source) were scaled.
    author(8, 4, 16, 12);
    targets.set_scanout_rect(base, {8, 4, 16, 12, 32, 24});
    targets.present();
    read_drawable();
    for (int y = kEdge; y < kHeight - kEdge; ++y)
        for (int x = kEdge; x < kWidth - kEdge; ++x)
            if (!expect(rgb_at(x, y) == red,
                        "cropped Projector carve scales its source pixels"))
                return 1;

    // Option off: the margin stays in the drawable.
    author(8, 4, 32, 24);
    targets.set_scanout_rect(base, {8, 4, 32, 24, 32, 24});
    targets.set_crop_overscan(false);
    targets.present();
    read_drawable();
    if (!expect(rgb_at(2, 2) == blue && rgb_at(20, 14) == red,
                "M2_CROP_OVERSCAN=0 keeps the authored margin"))
        return 1;

    // A carve that already covers the target is not cropped (and must not
    // fault): the plain full-target blit applies.
    targets.set_crop_overscan(true);
    targets.set_scanout_rect(base, {0, 0, kWidth, kHeight, kWidth, kHeight});
    author(8, 4, 32, 24);
    targets.present();
    read_drawable();
    if (!expect(rgb_at(2, 2) == blue, "a full-target carve is not cropped"))
        return 1;
    targets.shutdown();
    return 0;
}

// M2_RENDER_SCALE: targets larger or smaller than the presented region. The
// expected drawable values follow from the filters named in the contract, not
// from running present(): a 4:1 reduction is a box over four source columns,
// a 1:2 enlargement repeats whole source pixels, and the overscan crop still
// fills the region with the carve. A single bilinear 4:1 blit would sample
// only the middle two columns of each group, so the box case distinguishes
// the halving chain from it.
int run_render_scale(int samples)
{
    m2host::GlCltTargets targets;
    if (!targets.init()) return 3;
    targets.set_samples(samples);
    const uint32_t base = 0x50000;
    std::vector<uint32_t> drawable(kWidth * kHeight);
    const auto read_drawable = [&]() {
        glReadBuffer(GL_FRONT);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                     drawable.data());
    };
    const auto channel = [&](int x, int y, int shift) {
        return int((drawable[(kHeight - 1 - y) * kWidth + x] >> shift) & 0xffu);
    };

    // 4x targets: columns repeat red, red, red, blue.
    const int big = kWidth * 4;
    targets.resize(big, big, 0, 0, 0);
    targets.select(base);
    targets.select_scanout(base);
    targets.set_present_size(kWidth, kHeight);
    targets.set_scanout_rect(base, {0, 0, big, big, big, big});
    glDisable(GL_SCISSOR_TEST);
    targets.clear_color(1, 0, 0, 1);
    glEnable(GL_SCISSOR_TEST);
    for (int x = 3; x < big; x += 4) {
        glScissor(x, 0, 1, big);
        targets.clear_color(0, 0, 1, 1);
    }
    glDisable(GL_SCISSOR_TEST);
    targets.present();
    read_drawable();
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const int r = channel(x, y, 0), b = channel(x, y, 16);
            if (!expect(r >= 185 && r <= 197 && b >= 58 && b <= 70,
                        "4:1 reduction box-averages four columns")) {
                std::fprintf(stderr, "  at (%d,%d) r=%d b=%d samples=%d\n", x,
                             y, r, b, samples);
                return 1;
            }
        }

    // Overscan crop at 4x: carve (32,16)..(160,112) red in a blue margin.
    // present() leaves the drawable bound; select rebinds the target.
    targets.select(base);
    glDisable(GL_SCISSOR_TEST);
    targets.clear_color(0, 0, 1, 1);
    glEnable(GL_SCISSOR_TEST);
    glScissor(32, big - 16 - 96, 128, 96);
    targets.clear_color(1, 0, 0, 1);
    glDisable(GL_SCISSOR_TEST);
    targets.set_scanout_rect(base, {32, 16, 128, 96, 128, 96});
    targets.set_crop_overscan(true);
    targets.present();
    read_drawable();
    constexpr int kEdge = 2;
    for (int y = kEdge; y < kHeight - kEdge; ++y)
        for (int x = kEdge; x < kWidth - kEdge; ++x)
            if (!expect(channel(x, y, 0) == 255 && channel(x, y, 16) == 0,
                        "scaled crop is filled by the carve colour")) {
                std::fprintf(stderr, "  at (%d,%d) r=%d b=%d samples=%d\n", x,
                             y, channel(x, y, 0), channel(x, y, 16), samples);
                return 1;
            }
    targets.set_crop_overscan(false);

    // Half-size targets: a checkerboard enlarges to exact 2x2 blocks.
    const int small = kWidth / 2;
    targets.resize(small, small, 0, 0, 0);
    targets.select(base);
    targets.set_scanout_rect(base, {0, 0, small, small, small, small});
    glDisable(GL_SCISSOR_TEST);
    targets.clear_color(1, 0, 0, 1);
    glEnable(GL_SCISSOR_TEST);
    for (int y = 0; y < small; ++y)
        for (int x = (y & 1); x < small; x += 2) {
            glScissor(x, y, 1, 1);
            targets.clear_color(0, 0, 1, 1);
        }
    glDisable(GL_SCISSOR_TEST);
    targets.present();
    read_drawable();
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            // GL row y (bottom-up) of the small target is drawable row y*2.
            const int gl_y = kHeight - 1 - y;
            const bool blue_cell = (((x / 2) + (gl_y / 2)) & 1) == 0;
            const int r = channel(x, y, 0), b = channel(x, y, 16);
            if (!expect(blue_cell ? (b == 255 && r == 0) : (r == 255 && b == 0),
                        "2x enlargement repeats whole source pixels")) {
                std::fprintf(stderr, "  at (%d,%d) r=%d b=%d samples=%d\n", x,
                             y, r, b, samples);
                return 1;
            }
        }
    targets.shutdown();
    return 0;
}

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("m2_gl_clt_targets_test: SKIP: no video: %s\n",
                    SDL_GetError());
        return 3;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    SDL_Window* window = SDL_CreateWindow("m2-gl-clt-targets", kWidth, kHeight,
                                          SDL_WINDOW_OPENGL |
                                              SDL_WINDOW_HIDDEN);
    if (!window) {
        std::printf("m2_gl_clt_targets_test: SKIP: no window: %s\n",
                    SDL_GetError());
        SDL_Quit();
        return 3;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        std::printf("m2_gl_clt_targets_test: SKIP: no GL context: %s\n",
                    SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }

    m2host::GlCltTargets targets;
    int status = 3;
    if (!targets.init()) {
        std::printf("m2_gl_clt_targets_test: SKIP: no FBO entry points: %s\n",
                    SDL_GetError());
    } else {
        status = run(targets);
        if (status == 0) status = run_msaa();
        if (status == 0) status = run_scanout_projection(1);
        if (status == 0) status = run_scanout_projection(4);
        if (status == 0) status = run_overscan_crop(1);
        if (status == 0) status = run_overscan_crop(4);
        if (status == 0) status = run_render_scale(1);
        if (status == 0) status = run_render_scale(4);
        if (status == 0) std::puts("m2_gl_clt_targets_test: OK");
    }

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
