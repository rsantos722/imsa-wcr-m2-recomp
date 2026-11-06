// Real-context parity contract for the two shipped OpenGL CLT backends
// (host/graphics/gl_backend.cpp `GlBackend` and clean_renderer.cpp
// `CleanRenderer`).
//
// Why this cannot live in the `m2_guest_clt` gate: that target links the CPU
// `SoftCaptureBackend` and never compiles the GL renderer sources, so a
// shader, sampler, blend-state, texture-cache, resize, or FBO-rebind
// difference between the GL backends and the software reference passes every
// focused software test while still changing the shipped windowed image. The
// four paths the two GL backends expose -- target selection, the CLT program,
// the addressed target pool, and presentation -- are all GL-only.
//
// Expected values below are derived from the source contracts, never copied
// from renderer output:
//
//  * DB-source addressing. Vendor `frame2d/copyrect.c:F2_CopyRect()` writes
//    `src - dest` to `DBSRCOFFSET` and the base pointer to `DBSRCBASEADDR`
//    (guest_clt.cpp:174-194), so a DB-source address is the *screen* X/Y pair,
//    carried unbounded and normalized as `u = (x + offset_x) / width`. The
//    addresses therefore do not take the triangle's W: `noperspective`
//    interpolation in both CLT vertex shaders is the guest relation, and
//    `floor(u)` is resolved per fragment as a source-row carry
//    (cel_blitter.h `clt_db_source_sample_uv`). A width-4 `(+2, 0)` copy is the
//    rendering contract's own worked example (docs/recompilation/rendering.md).
//  * Reverse-alpha composition. `StreamEventFunc@0x3b63c` writes
//    `DBAMULTCNTL=0x2`, `DBBMULTCNTL=0x0`, `DBALUCNTL=0x10`, i.e.
//    `out = TEXCOLOR*(1 - TEXALPHA) + SRCCOLOR*TEXALPHA` with the destination
//    taking no part.
//  * Source copy. The B-only identity-coefficient form replaces the
//    destination pixel outright -- `(ONE, ZERO)` in `clt_gl_blend_plan` -- so a
//    fully transparent texel still writes its RGB.
//  * Rebind after present. `GlCltTargets::select` rebinds even when the
//    requested base is unchanged, because `present()` left the default
//    drawable bound (gl_clt_targets.h:84-91).
//  * Resize. `frame_begin` re-reads the window size and `GlCltTargets::resize`
//    invalidates storage, so a stale-size read must be refused.
//  * Clear extent. The recovered TE driver answers `TE_CLEAR_FRAME_BUFFER`
//    with a clear list -- a quad from the surface origin to the extent the
//    bound bitmap published (driver.c `UpdateClearFBCmdList`) -- which the TE
//    clips with the same DBFBCLIP/window state as every other primitive. The
//    platform fill therefore writes inside the interpreter's tracked clip, and
//    the guest's 576x432 render target is smaller than the 640x480 display.
//
// The identity predicate below is what makes this a parity test rather than a
// third software test: it only accepts an object that reports a GL backend
// name, keeps a current SDL GL context, and answers `glGetString(GL_RENDERER)`.
// Substituting `SoftCaptureBackend` for either factory fails it, and that
// substitution is executed as a negative control at the end of the run rather
// than merely asserted. Exit 3 (the CMake `SKIP_RETURN_CODE`) means this
// process could not obtain a context at all, which is a different outcome from
// a parity failure.

#include "cel_blitter.h"
#include "soft_capture.h"
#include "texture_replacements.h"

#include <bit>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <vector>
#include "posix_compat.h"

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kResizedWidth = 96;
constexpr int kResizedHeight = 72;

// One base per case: `clt_target_select` allocates on first use and clears the
// new storage to the frame clear color, so a fresh base is a fresh surface.
constexpr uint32_t kBaseA = 0x10000u;  // varying-W DB-source coordinates
constexpr uint32_t kBaseB = 0x11000u;  // signed row carry
constexpr uint32_t kBaseC = 0x12000u;  // reverse-alpha composition
constexpr uint32_t kBaseD = 0x13000u;  // source copy
constexpr uint32_t kBaseE = 0x14000u;  // target rebind after present
constexpr uint32_t kBaseF = 0x15000u;  // resize
constexpr uint32_t kBaseG = 0x16000u;  // clear extent
constexpr uint32_t kBaseH = 0x17000u;  // overlapping triangle depth order
constexpr uint32_t kBaseI = 0x18000u;  // GPU snapshot source
constexpr uint32_t kBaseJ = 0x19000u;  // GPU snapshot consumer
constexpr uint32_t kBaseK = 0x1a000u;  // explicit replacement texture
constexpr uint32_t kBaseL = 0x1b000u;  // optional linear filtering
constexpr uint32_t kBaseM = 0x1c000u;  // anisotropic sampler request
constexpr uint32_t kBaseN = 0x1d000u;  // multisample edge coverage
constexpr uint32_t kBaseO = 0x1e000u;  // multisample scaled snapshot source
constexpr uint32_t kBaseP = 0x1f000u;  // SSB mask wrap matching primary texture
constexpr uint32_t kBaseQ = 0x20000u;  // masked-UV mip seam derivative preservation
constexpr uint32_t kBaseU = 0x22000u;  // retired-texture recycle

// The clear color the frame arms before every draw. It is deliberately unlike
// any texel the fixtures author, so a draw that leaves the destination in
// place (an ordinary source-over fallback) is visibly wrong rather than
// accidentally equal.
constexpr float kClearR = 0.20f;
constexpr float kClearG = 0.35f;
constexpr float kClearB = 0.65f;

// The addressed readback word is the guest/interpreter 0xAABBGGRR form shared
// by the software backend and both GL backends.
constexpr uint32_t pack_rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

constexpr uint32_t kClearWord =
    pack_rgba(static_cast<unsigned>(kClearR * 255.f + 0.5f),
              static_cast<unsigned>(kClearG * 255.f + 0.5f),
              static_cast<unsigned>(kClearB * 255.f + 0.5f), 255u);

int g_failures = 0;

bool expect(bool condition, const char* fmt, ...)
{
    if (condition) return true;
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "m2_gl_backend_parity: FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    ++g_failures;
    return false;
}

// A per-channel tolerance, used only where the contract itself is a
// floating-point mixture (`TEXCOLOR*(1-TEXALPHA) + SRCCOLOR*TEXALPHA`) and the
// target is 8-bit. Every address, copy, and identity assertion is exact.
bool near_rgba(uint32_t got, uint32_t want, int tolerance, const char* what,
               int x, int y)
{
    const unsigned g[4] = {got & 0xffu, (got >> 8) & 0xffu,
                           (got >> 16) & 0xffu, (got >> 24) & 0xffu};
    const unsigned w[4] = {want & 0xffu, (want >> 8) & 0xffu,
                           (want >> 16) & 0xffu, (want >> 24) & 0xffu};
    for (int c = 0; c < 4; ++c) {
        const int d = static_cast<int>(g[c]) - static_cast<int>(w[c]);
        if (d < -tolerance || d > tolerance) {
            std::fprintf(stderr,
                         "m2_gl_backend_parity: FAIL: %s at (%d,%d) "
                         "channel %d: got %u want %u (0x%08x vs 0x%08x)\n",
                         what, x, y, c, g[c], w[c], got, want);
            ++g_failures;
            return false;
        }
    }
    return true;
}

std::vector<uint32_t> read_target(m2host::ICelBlitterBackend& backend,
                                  uint32_t base, const char* what)
{
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0u);
    if (!expect(backend.grab_clt_target_rgba(base, pixels.data(), kWidth,
                                             kHeight),
                "%s: addressed target %#x is not readable at %dx%d", what,
                base, kWidth, kHeight))
        return {};
    return pixels;
}

// A source raster in which every texel encodes its own (x, y), so any address
// error -- a stray perspective divide, a missing or mis-signed row carry, an
// edge clamp -- lands on a different, named texel instead of a plausible one.
std::vector<uint32_t> coordinate_pattern()
{
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            pixels[static_cast<size_t>(y) * kWidth + x] =
                pack_rgba(static_cast<unsigned>(x) * 4u,
                          static_cast<unsigned>(y) * 4u, 0u, 255u);
    return pixels;
}

uint32_t pattern_at(const std::vector<uint32_t>& pattern, int x, int y)
{
    return pattern[static_cast<size_t>(y) * kWidth + x];
}

m2host::CltVertex vertex(float x, float y, float u, float v, float w)
{
    m2host::CltVertex out{};
    out.x = x;
    out.y = y;
    out.u = u;
    out.v = v;
    out.r = out.g = out.b = out.a = 1.f;
    out.w = w;
    out.one_over_w = 1.f / w;
    out.mask_u = u;
    out.mask_v = v;
    return out;
}

// Begin a frame addressed at `base` and leave a cleared target selected.
void arm(m2host::ICelBlitterBackend& backend, uint32_t base)
{
    backend.frame_begin(kClearR, kClearG, kClearB);
    backend.clt_target_select(base);
}

// --- A: DB-source coordinates follow screen X/Y, not the triangle's W -------
//
// The draw spans the whole target with two different vertex W values, so a
// perspective-correct interpolation of the same vertex UVs is displaced from
// the screen-linear relation across the interior. The DB-source form declares
// the linear address `u = x / width` with the source as wide as the
// destination, so `floor(u)` is zero over the target and each destination
// pixel must read the source texel with the same (x, y).
bool case_varying_w_db_source(m2host::ICelBlitterBackend& backend)
{
    const std::vector<uint32_t> pattern = coordinate_pattern();
    const m2host::CelId source =
        backend.cel_upload_rgba(kWidth, kHeight, pattern.data());
    if (!expect(source != m2host::kInvalidCel, "A: DB source uploads"))
        return false;

    // A screen-covering strip whose W alternates 1 / 4 across the primitive.
    std::array<m2host::CltVertex, 4> quad{
        vertex(0.f, 0.f, 0.f, 0.f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 4.f),
        vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
        vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), 1.f, 1.f,
               4.f),
    };
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    blend.secondary_source = source;
    blend.db_source_linear = true;
    blend.db_source_height = static_cast<float>(kHeight);

    arm(backend, kBaseA);
    backend.clt_draw(quad.data(), 4, true, true, source,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     true, {}, {}, {}, blend);
    const std::vector<uint32_t> got = read_target(backend, kBaseA, "A");
    if (got.empty()) return false;

    // The contract: destination (x, y) reads source (x, y).
    int exact = 0;
    for (int y = 1; y < kHeight - 1; ++y)
        for (int x = 1; x < kWidth - 1; ++x)
            if (got[static_cast<size_t>(y) * kWidth + x] ==
                pattern_at(pattern, x, y))
                ++exact;
    const int interior = (kWidth - 2) * (kHeight - 2);
    if (!expect(exact == interior,
                "A: DB source reads the screen address at every interior "
                "pixel (%d of %d matched)",
                exact, interior))
        return false;

    // Control, executed in the same run: with the DB-source relation not
    // declared, the same geometry and the same UVs are ordinary perspective
    // texture addressing and MUST land somewhere else. If this arm ever
    // matched the screen addresses, the assertion above would be vacuous.
    m2host::CltBlendState perspective = blend;
    perspective.db_source_linear = false;
    arm(backend, kBaseA);
    backend.clt_draw(quad.data(), 4, true, true, source,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     true, {}, {}, {}, perspective);
    const std::vector<uint32_t> displaced = read_target(backend, kBaseA, "A");
    if (displaced.empty()) return false;
    int differing = 0;
    for (int y = 1; y < kHeight - 1; ++y)
        for (int x = 1; x < kWidth - 1; ++x)
            if (displaced[static_cast<size_t>(y) * kWidth + x] !=
                pattern_at(pattern, x, y))
                ++differing;
    return expect(differing > 0,
                  "A: control -- ordinary perspective addressing of the same "
                  "UVs is displaced from the screen relation (%d interior "
                  "pixels differ); a match would make the assertion vacuous",
                  differing);
}

// --- B: the row carry is signed -------------------------------------------
//
// `u` is an unbounded linear address and its integer part counts source rows
// to add to `v`, so a negative X has to address the *previous* row at the far
// column. The contract calls out underflow addressing the adjacent row
// "instead of clamping at the rightmost texel". Here `offset_x = -1` makes destination column 0 carry exactly
// `-1`: it must read source column 63 of the row above, while column 1 -- whose
// linear address is already non-negative -- must read the same row. Clamping
// instead would give column 0 the source's own column 0.
bool case_signed_row_carry(m2host::ICelBlitterBackend& backend)
{
    const std::vector<uint32_t> pattern = coordinate_pattern();
    const m2host::CelId source =
        backend.cel_upload_rgba(kWidth, kHeight, pattern.data());
    if (!expect(source != m2host::kInvalidCel, "B: DB source uploads"))
        return false;

    // Linear address u = (x - 1) / width across the target, v = y / height.
    const float first_u = -1.f / kWidth;
    const float last_u = (static_cast<float>(kWidth) - 1.f) / kWidth;
    std::array<m2host::CltVertex, 4> quad{
        vertex(0.f, 0.f, first_u, 0.f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, last_u, 0.f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), first_u, 1.f, 1.f),
        vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), last_u,
               1.f, 1.f),
    };
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    blend.secondary_source = source;
    blend.db_source_linear = true;
    blend.db_source_height = static_cast<float>(kHeight);

    arm(backend, kBaseB);
    backend.clt_draw(quad.data(), 4, true, true, source,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     true, {}, {}, {}, blend);
    const std::vector<uint32_t> got = read_target(backend, kBaseB, "B");
    if (got.empty()) return false;

    for (int y = 1; y < kHeight - 1; ++y) {
        // Column 0 has u < 0, so floor(u) = -1: the row above, last column.
        if (!expect(got[static_cast<size_t>(y) * kWidth] ==
                        pattern_at(pattern, kWidth - 1, y - 1),
                    "B: negative X carry reads source (%d,%d) at destination "
                    "(0,%d)", kWidth - 1, y - 1, y))
            return false;
        // Column 1 has u >= 0, so no carry: this row, first column.
        if (!expect(got[static_cast<size_t>(y) * kWidth + 1] ==
                        pattern_at(pattern, 0, y),
                    "B: the non-negative column keeps its own row -- source "
                    "(0,%d) at destination (1,%d)", y, y))
            return false;
    }
    return true;
}

// --- C: reverse-alpha composition takes no part from the destination -------
//
// `out = TEXCOLOR*(1 - TEXALPHA) + SRCCOLOR*TEXALPHA`. The target is armed
// with a clear color no texel uses, so alpha zero (which must write the
// primary texture alone) and alpha 255 (which must write the armed DB source
// alone) both fail if the backend falls back to source-over against the
// destination.
bool case_secondary_source_alpha(m2host::ICelBlitterBackend& backend,
    m2host::CltBlendMode mode = m2host::CltBlendMode::secondary_source_alpha)
{
    const uint32_t secondary = pack_rgba(40u, 60u, 80u, 255u);
    const m2host::CelId source = backend.cel_upload_rgba(1, 1, &secondary);
    if (!expect(source != m2host::kInvalidCel, "C: DB source uploads"))
        return false;

    struct Arm {
        unsigned r, g, b, alpha;
        const char* label;
    };
    const Arm arms[] = {
        {200u, 10u, 20u, 0u, "alpha 0 writes the primary texture"},
        {10u, 20u, 200u, 255u, "alpha 255 writes the armed DB source"},
        {0u, 255u, 0u, 128u, "alpha 128 weights both source inputs"},
    };
    for (const Arm& entry : arms) {
        const uint32_t primary =
            pack_rgba(entry.r, entry.g, entry.b, entry.alpha);
        const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &primary);
        if (!expect(texture != m2host::kInvalidCel, "C: primary uploads"))
            return false;

        // The primary texture supplies TEXCOLOR/TEXALPHA through u/v; the
        // independent DB-source coordinate space is mask_u/mask_v.
        m2host::CltVertex verts[3] = {
            vertex(0.f, 0.f, 0.f, 0.f, 1.f),
            vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 1.f),
            vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
        };
        for (m2host::CltVertex& v : verts) {
            v.mask_u = 0.f;
            v.mask_v = 0.f;
        }
        m2host::CltBlendState blend;
        blend.mode = mode;
        blend.secondary_source = source;
        blend.constant_r = 100.f / 255.f;
        blend.constant_g = 120.f / 255.f;
        blend.constant_b = 140.f / 255.f;

        arm(backend, kBaseC);
        backend.clt_draw(verts, 3, false, true, texture,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         true, {}, {}, {}, blend);
        const std::vector<uint32_t> got = read_target(backend, kBaseC, "C");
        if (got.empty()) return false;

        // Evaluate the guest equation from the two authored texels. The
        // secondary source is RGB(40,60,80); the primary is the arm's own.
        const float alpha = static_cast<float>(entry.alpha) / 255.f;
        const float t = mode == m2host::CltBlendMode::secondary_source_alpha
                            ? alpha : 1.f - alpha;
        const auto mix = [&](unsigned p, unsigned s) {
            return static_cast<unsigned>(
                std::lround(static_cast<float>(p) * (1.f - t) +
                            static_cast<float>(s) * t));
        };
        const bool constant = mode == m2host::CltBlendMode::secondary_constant_over;
        const uint32_t want = pack_rgba(mix(constant ? 100u : entry.r, 40u),
                                       mix(constant ? 120u : entry.g, 60u),
                                       mix(constant ? 140u : entry.b, 80u), entry.alpha);
        const int px = kWidth / 4, py = kHeight / 4;
        const uint32_t observed = got[static_cast<size_t>(py) * kWidth + px];
        if (!near_rgba(observed, want, 1, entry.label, px, py)) return false;
        if (!expect(observed != kClearWord,
                    "C: %s -- the destination clear color took no part",
                    entry.label))
            return false;
    }
    return true;
}

// Source-derived shadow-volume invariant: a receiver between the two faces
// darkens, empty space behind both faces does not, and foreground geometry
// stays untouched. The first pass writes RGB555 0x3fff through the Z alias
// even though DBZCNTL=1 has no ordinary depth-update bits.
bool case_shadow_color_z_alias(m2host::ICelBlitterBackend& backend)
{
    using namespace m2host;
    const uint32_t ground = pack_rgba(200, 160, 120, 255);
    const uint32_t shaded = pack_rgba(100, 80, 60, 255);
    const CelId source = backend.cel_upload_rgba(1, 1, &ground);
    const auto quad = [](float x0, float y0, float x1, float y1,
                         float z, float r, float g, float b) {
        std::array<CltVertex, 4> v = {vertex(x0,y0,0,0,1), vertex(x1,y0,0,0,1),
                                     vertex(x0,y1,0,0,1), vertex(x1,y1,0,0,1)};
        for (auto& p : v) { p.one_over_w=z; p.r=r; p.g=g; p.b=b; }
        return v;
    };
    CltDepth scene{};
    scene.test_enabled=scene.write_enabled=true;
    scene.depth_func=3;
    scene.z_region_max_x=kWidth; scene.z_region_max_y=kHeight;
    auto world=quad(0,0,kWidth,kHeight,.4f,200.f/255,160.f/255,120.f/255);
    auto blocker=quad(0,0,kWidth,8,.9f,200.f/255,160.f/255,120.f/255);
    auto back=quad(0,0,kWidth,56,.2f,31.f/64,1,1);
    back[1].one_over_w=back[3].one_over_w=.6f;
    auto front=quad(0,0,kWidth,56,.7f,1,1,1);
    CltBlendState darken;
    darken.mode=CltBlendMode::general_secondary;
    darken.secondary_source=source;
    darken.general.a=0x60; darken.general.b=0x20;
    darken.general.a_constant={0,0};
    darken.general.b_constant={0x808080,0x808080};
    darken.general.alu=0;
    const auto run = [&](bool alias, bool write_enable, bool batched, bool clipped) {
        arm(backend,kBaseD);
        backend.clt_clear_z(0);
        backend.clt_draw(world.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{},scene);
        backend.clt_draw(blocker.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{},scene);
        backend.clt_target_select(kBaseE);
        CltDepth volume=scene;
        volume.depth_func=1; volume.write_enabled=write_enable;
        volume.color_target_is_z=alias;
        if (clipped) backend.scissor(0,16,kWidth,24);
        if (batched) {
            const CltVertex triangles[] = {back[0],back[1],back[2],back[2],back[1],back[3]};
            backend.clt_draw(triangles,6,false,false,kInvalidCel,kWidth,kHeight,false,{},volume,{}, {},true);
        } else {
            backend.clt_draw(back.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{},volume);
        }
        if (clipped) backend.scissor_disable();
        backend.clt_target_select(kBaseD);
        volume.color_target_is_z=false;
        backend.clt_draw(front.data(),4,true,false,kInvalidCel,kWidth,kHeight,true,{},volume,{},darken);
        return read_target(backend,kBaseD,"shadow alias");
    };
    for (bool write_enable : {false,true})
    for (bool batched : {false,true})
    for (bool clipped : {false,true}) {
        const auto got=run(true,write_enable,batched,clipped);
        if (got.empty() ||
            !near_rgba(got[24*kWidth+12],shaded,1,"shadow: receiver stays dark",12,24) ||
            !near_rgba(got[24*kWidth+52],ground,1,"shadow: volume side excluded",52,24) ||
            !near_rgba(got[4*kWidth+52],ground,1,"shadow: foreground protected",52,4) ||
            !near_rgba(got[48*kWidth+52],clipped ? shaded : ground,1,"shadow: scissor coverage",52,48) ||
            !near_rgba(got[60*kWidth+52],ground,1,"shadow: outside volume untouched",52,60)) return false;
    }
    // Executed negative control: disconnect the alias, reproducing the old
    // filled side while leaving the receiver shadow present.
    const auto wrong=run(false,true,true,false);
    bool ok=!wrong.empty() && near_rgba(wrong[24*kWidth+52],shaded,1,
        "shadow negative control: missing alias darkens volume side",52,24);
    backend.cel_free(source);
    return ok;
}

// A framebuffer's alpha and DSB are independent of its RGB and of one
// another. Exercise real target storage, snapshots and the consuming ALU.
bool case_framebuffer_metadata(m2host::ICelBlitterBackend& backend)
{
    using namespace m2host;
    std::array<CltVertex,4> quad = {vertex(0,0,0,0,1),vertex(kWidth,0,1,0,1),
        vertex(0,kHeight,0,1,1),vertex(kWidth,kHeight,1,1,1)};
    for (auto& v:quad) { v.r=200.f/255; v.g=100.f/255; v.b=40.f/255; v.a=64.f/255;
        v.mask_u=v.u; v.mask_v=v.v; }
    for (unsigned bit : {0u,1u}) {
        arm(backend,kBaseD);
        CltBlendState write;
        write.mode=CltBlendMode::general_internal;
        write.general.a=0x20; write.general.a_constant={0xffffff,0xffffff};
        write.general.alu=28u<<3;
        write.dsb=clt_decode_dsb(1u | (bit<<2));
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},write);
        const CelId color=backend.clt_snapshot_target_cel(kBaseD,0,0,kWidth,kHeight,kWidth,kHeight,true);
        const CelId meta=backend.clt_snapshot_target_ssb_cel(kBaseD,0,0,kWidth,kHeight,kWidth,kHeight);
        if (!expect(color!=kInvalidCel && meta!=kInvalidCel,"metadata snapshots allocated")) return false;
        // Overwrite the producer after capture: snapshots must be immutable.
        write.dsb=clt_decode_dsb(1u | ((1u-bit)<<2));
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},write);
        CltBlendState read;
        read.mode=CltBlendMode::general_secondary;
        read.secondary_source=color; read.source_ssb_mask=meta;
        read.general.b=0x24; // SRCCOLOR * CONST selected by SRCSSB
        read.general.b_constant={0xffffff,0x808080};
        read.general.alu=26u<<3; read.general.dest_alpha=2;
        read.dsb=clt_decode_dsb(2);
        arm(backend,kBaseE);
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},read);
        auto got=read_target(backend,kBaseE,"framebuffer metadata");
        if (got.empty() || !near_rgba(got[17*kWidth+23],
            bit ? pack_rgba(100,50,20,64) : pack_rgba(200,100,40,64),1,"source DSB coefficient and alpha",23,17)) return false;
        uint32_t raw_metadata=0;
        if (!expect(backend.grab_clt_target_window_metadata(kBaseE,&raw_metadata,23,17,1,1) &&
            raw_metadata==pack_rgba(bit*255,64,0,255),
            "metadata CPU readback preserves alpha and DSB independently")) return false;
        // A second target copy must carry metadata too, independently of color.
        backend.clt_target_select(kBaseA);
        backend.clt_target_copy(kBaseE,kBaseA);
        const CelId copied_meta=backend.clt_snapshot_target_ssb_cel(kBaseA,0,0,kWidth,kHeight,kWidth,kHeight);
        if (!expect(copied_meta!=kInvalidCel,"copied framebuffer metadata snapshot")) return false;
        CltBlendState show; show.mode=CltBlendMode::source_copy;
        auto white=quad; for(auto& v:white) v.r=v.g=v.b=v.a=1;
        arm(backend,kBaseB);
        backend.clt_draw(white.data(),4,true,true,copied_meta,kWidth,kHeight,false,{}, {}, {},show);
        const auto copied=read_target(backend,kBaseB,"copied metadata channels");
        if (copied.empty() || !near_rgba(copied[17*kWidth+23],pack_rgba(bit*255,64,0,255),1,
            "DSB SRCSSB output survives target copy",23,17)) return false;
        backend.cel_free(copied_meta);
        backend.clt_target_select(kBaseE);
        // Alpha coefficient consumes the captured alpha rather than opaque
        // fallback or DSB. Force fractional alpha to one in a second draw.
        read.general.b=0x10; read.general.source_alpha=0;
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},read);
        got=read_target(backend,kBaseE,"framebuffer alpha coefficient");
        if (got.empty() || !near_rgba(got[17*kWidth+23],pack_rgba(50,25,10,64),1,"source alpha coefficient",23,17)) return false;
        read.general.source_alpha=4;
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},read);
        got=read_target(backend,kBaseE,"framebuffer alpha clamp");
        if (got.empty() || !near_rgba(got[17*kWidth+23],pack_rgba(200,100,40,255),1,"source alpha clamp",23,17)) return false;
        // Actual wrong-result control: substitute SSB zero. The bit-one case
        // must lose its half-intensity coefficient.
        read.source_ssb_mask=kInvalidCel; read.general.b=0x24; read.general.source_alpha=0;
        backend.clt_draw(quad.data(),4,true,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},read);
        got=read_target(backend,kBaseE,"metadata negative control");
        if (got.empty() || !near_rgba(got[17*kWidth+23],pack_rgba(200,100,40,64),1,"missing metadata control",23,17)) return false;
        backend.cel_free(color); backend.cel_free(meta);
    }
    return true;
}

bool case_general_dsb(m2host::ICelBlitterBackend& backend)
{
    using namespace m2host;
    CltVertex vertices[3] = {vertex(0,0,0,0,1), vertex(kWidth,0,0,0,1), vertex(0,kHeight,0,0,1)};
    for (auto& v : vertices) { v.r=204.f/255; v.g=102.f/255; v.b=51.f/255; v.a=128.f/255; }
    CltBlendState blend;
    blend.mode = CltBlendMode::general_internal;
    blend.general.a = 0x20; // texture * white coefficient
    blend.general.b = 0x60; // constant * white coefficient
    blend.general.a_constant = blend.general.b_constant = {0xffffff,0xffffff};
    blend.general.constant = 0x3366cc;
    const unsigned ops[] = {0,2,8,10,12,14,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
    const auto expected = [](unsigned op, int a, int b) -> unsigned {
        switch(op) {
        case 0: return std::min(a+b,255); case 2: return (a+b)&255;
        case 8: return std::max(a-b,0); case 10: return (a-b)&255;
        case 12: return std::max(b-a,0); case 14: return (b-a)&255;
        case 16: return 0; case 17: return (~(a|b))&255;
        case 18: return (~a&b)&255; case 19: return (~a)&255;
        case 20: return (a&~b)&255; case 21: return (~b)&255;
        case 22: return a^b; case 23: return (~(a&b))&255;
        case 24: return a&b; case 25: return (~(a^b))&255;
        case 26: return b; case 27: return (~a|b)&255;
        case 28: return a; case 29: return (a|~b)&255;
        case 30: return a|b; default: return 255;
        }
    };
    for (unsigned op : ops) for (unsigned shift=0; shift<3; ++shift) {
        blend.general.alu = (op<<3)|shift;
        arm(backend,kBaseD);
        backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
        const auto got = read_target(backend,kBaseD,"general DSB ALU");
        const unsigned divisor=1u<<shift;
        const auto divided = [divisor](unsigned n) { return unsigned(std::lround(float(n)/divisor)); };
        if (got.empty() || !near_rgba(got[kWidth+1],pack_rgba(divided(expected(op,204,51)),
            divided(expected(op,102,102)),divided(expected(op,51,204)),128),1,"general DSB ALU",1,1)) return false;
    }
    // Real secondary texture, deliberately different from the destination.
    const uint32_t source = pack_rgba(20,80,160,64);
    blend.secondary_source = backend.cel_upload_rgba(1,1,&source);
    blend.mode = CltBlendMode::general_secondary;
    blend.general.b = 0x20; // source * white
    blend.general.alu = 0x50; // wrapping texture - source
    for (unsigned alpha : {0u,1u,2u,3u,9u}) {
        blend.general.dest_alpha = alpha;
        blend.general.alpha_constant = 0x003f00bf;
        blend.general.source_alpha = 0;
        arm(backend,kBaseD);
        backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
        const auto got = read_target(backend,kBaseD,"general DSB alpha");
        const unsigned expected_alpha = alpha==0 ? 128 : alpha==2 ? 64 : alpha==3 ? 184 : 63;
        if (got.empty() || !near_rgba(got[kWidth+1],pack_rgba(184,22,147,expected_alpha),1,"general DSB alpha",1,1)) return false;
    }
    // Each multiplier input and coefficient, independently exposed by A/B.
    const float t[3]={204.f/255,102.f/255,51.f/255};
    const float source_rgb[3]={20.f/255,80.f/255,160.f/255};
    const float c[3]={51.f/255,102.f/255,204.f/255};
    const float k[3]={128.f/255,64.f/255,192.f/255};
    for (bool aside : {true,false}) for (unsigned input=0; input<4; ++input)
    for (unsigned coef : {0u,1u,8u,9u,16u,17u,24u,25u}) {
        const unsigned control=(input<<6)|(coef<<1);
        blend.general.a=aside ? control : 0;
        blend.general.b=aside ? 0 : control;
        blend.general.a_constant=blend.general.b_constant={0x8040c0,0x8040c0};
        blend.general.alu=(aside ? 28u : 26u)<<3;
        blend.general.dest_alpha=0;
        unsigned rgb[3];
        for (unsigned ch=0; ch<3; ++ch) {
            const float value=input==0 ? (aside ? t[ch] : source_rgb[ch]) : input==1 ? c[ch] :
                input==2 ? 1.f-(aside ? source_rgb[ch] : t[ch]) : 128.f/255;
            float weight=(coef&24)==0 ? 128.f/255 : (coef&24)==8 ? 64.f/255 :
                (coef&24)==16 ? k[ch] : aside ? source_rgb[ch] : t[ch];
            if (coef&1) weight=1.f-weight;
            rgb[ch]=unsigned(std::lround(value*weight*255));
        }
        arm(backend,kBaseD);
        backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
        const auto got=read_target(backend,kBaseD,"DSB multiplier selector");
        if (got.empty() || !near_rgba(got[kWidth+1],pack_rgba(rgb[0],rgb[1],rgb[2],128),1,
            "DSB multiplier selector",1,1)) return false;
    }
    // Texture SSB independently selects coefficient and destination-alpha entries.
    blend.general.a=0x20; blend.general.b=0; blend.general.alu=28u<<3;
    blend.general.a_constant={0xffffff,0x000000}; blend.general.dest_alpha=1;
    for (unsigned entry : {0u,1u}) {
        const uint32_t mask=entry ? 0xffffffffu : 0xff000000u;
        blend.weighted_mix_ssb_mask=backend.cel_upload_rgba(1,1,&mask);
        arm(backend,kBaseD);
        backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
        const auto got=read_target(backend,kBaseD,"DSB texture SSB");
        if (got.empty() || !near_rgba(got[kWidth+1],entry ? pack_rgba(0,0,0,191) : pack_rgba(204,102,51,63),1,
            "DSB texture SSB",1,1)) return false;
        backend.cel_free(blend.weighted_mix_ssb_mask);
    }
    blend.weighted_mix_ssb_mask=kInvalidCel;
    blend.general.a=0x20; blend.general.b=0x20; blend.general.alu=0x50;
    blend.general.a_constant=blend.general.b_constant={0xffffff,0xffffff};
    // Fractional framebuffer alpha is forced to one, independently of RGB.
    blend.general.dest_alpha=2;
    blend.general.source_alpha=4;
    arm(backend,kBaseD);
    backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
    const auto got=read_target(backend,kBaseD,"source alpha clamp");
    if (got.empty() || !near_rgba(got[kWidth+1],pack_rgba(184,22,147,255),1,"source alpha clamp",1,1)) return false;
    for (unsigned alpha : {0u,128u,255u}) for (unsigned action=0; action<3; ++action) {
        const uint32_t pixel=pack_rgba(20,80,160,alpha);
        backend.cel_free(blend.secondary_source);
        blend.secondary_source=backend.cel_upload_rgba(1,1,&pixel);
        blend.general.source_alpha=action<<(alpha==0 ? 0 : alpha==255 ? 4 : 2);
        arm(backend,kBaseD);
        backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
        const auto result=read_target(backend,kBaseD,"source alpha ranges");
        if (result.empty() || !near_rgba(result[kWidth+1],pack_rgba(184,22,147,action==1 ? 255 : action==2 ? 0 : alpha),1,
            "source alpha ranges",1,1)) return false;
    }
    blend.mode=CltBlendMode::general_internal;
    blend.general.a=0x21; blend.general.alu=28u<<3; blend.general.dest_alpha=0;
    arm(backend,kBaseD);
    backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,false,{}, {}, {},blend);
    const auto shifted=read_target(backend,kBaseD,"right justified input");
    if (shifted.empty() || !near_rgba(shifted[kWidth+1],pack_rgba(25,12,6,128),1,"right justified input",1,1)) return false;
    // Execute the previous approximation and prove it cannot replace this equation.
    blend.mode=CltBlendMode::source_over;
    arm(backend,kBaseD);
    backend.clt_draw(vertices,3,false,false,kInvalidCel,kWidth,kHeight,true,{}, {}, {},blend);
    const auto wrong=read_target(backend,kBaseD,"source-over negative control");
    backend.cel_free(blend.secondary_source);
    return !wrong.empty() && expect(wrong[kWidth+1]!=shifted[kWidth+1],
        "source-over negative control differs from general DSB");
}

bool case_general_tab(m2host::ICelBlitterBackend& backend)
{
    m2host::CltVertex vertices[3] = {
        vertex(0, 0, 0, 0, 1), vertex(kWidth, 0, 0, 0, 1),
        vertex(0, kHeight, 0, 0, 1),
    };
    for (auto& v : vertices) { v.r = 1; v.g = 0; v.b = 0; v.a = 0.5f; }
    const uint32_t pixel = pack_rgba(0, 0, 255, 128);
    const auto texture = backend.cel_upload_rgba(1, 1, &pixel);
    m2host::CltBlendState blend;
    // C = lerp(primitive red, texture blue, texture alpha),
    // A = primitive alpha * texture alpha. No destination blending.
    blend.tab = {true, 0x12499u, {0, 0}};
    arm(backend, kBaseD);
    backend.clt_draw(vertices, 3, false, true, texture, kWidth, kHeight,
                     false, {}, {}, {}, blend);
    auto got = read_target(backend, kBaseD, "general TAB");
    if (got.empty() || !near_rgba(got[kWidth + 1], pack_rgba(127, 0, 128, 64),
                                  1, "TAB lerp and independent alpha", 1, 1)) return false;
    blend.tab.enabled = false;
    arm(backend, kBaseD);
    backend.clt_draw(vertices, 3, false, true, texture, kWidth, kHeight,
                     false, {}, {}, {}, blend);
    const auto old = read_target(backend, kBaseD, "plain multiply control");
    if (old.empty() || !expect(old[kWidth + 1] != got[kWidth + 1],
                              "plain multiply cannot substitute for TAB lerp")) return false;
    // A different SSB constant at each texel must select the requested color,
    // including alpha, rather than collapsing both entries to constant 0.
    for (unsigned entry : {0u, 1u}) {
        const uint32_t mask = entry ? 0xffffffffu : 0xff000000u;
        blend.weighted_mix_ssb_mask = backend.cel_upload_rgba(1, 1, &mask);
        blend.tab = {true, 0u, {0x40204080u, 0x60402010u}};
        // MULT const color * const color, output primitive alpha.
        blend.tab.control = 5u | (5u << 3) | (2u << 9) | (1u << 17);
        arm(backend, kBaseD);
        backend.clt_draw(vertices, 3, false, true, texture, kWidth, kHeight,
                         false, {}, {}, {}, blend);
        got = read_target(backend, kBaseD, "TAB SSB constant");
        const uint32_t expected = entry ? pack_rgba(16, 4, 1, 128) : pack_rgba(4, 16, 64, 128);
        if (got.empty() || !near_rgba(got[kWidth + 1], expected, 1,
                                      "TAB SSB constant", 1, 1)) return false;
    }
    blend.weighted_mix_ssb_mask = m2host::kInvalidCel;
    blend.tab.constants = {0x40204080u, 0x40204080u};
    for (auto& vertex : vertices) vertex.a = 1.f;
    // FIRSTCOLOR selects each documented input; SECOND=PRIMALPHA=1
    // makes the product expose that input unchanged.
    const uint32_t selected[] = {
        pack_rgba(255,255,255,255), pack_rgba(255,0,0,255),
        pack_rgba(128,128,128,255), pack_rgba(0,0,255,255),
        pack_rgba(64,64,64,255), pack_rgba(32,64,128,255),
    };
    for (unsigned selector = 0; selector < 6; ++selector) {
        blend.tab.control = selector | (2u << 9) | (1u << 17);
        arm(backend, kBaseD);
        backend.clt_draw(vertices, 3, false, true, texture, kWidth, kHeight,
                         false, {}, {}, {}, blend);
        got = read_target(backend, kBaseD, "TAB color selector");
        if (got.empty() || !near_rgba(got[kWidth + 1], selected[selector], 1,
                                      "TAB color selector", 1, 1)) return false;
    }
    for (auto& vertex : vertices) vertex.a = .5f;
    const float alpha[] = {.5f, 128.f/255, 64.f/255};
    for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) {
        blend.tab.control = (2u << 15) | (a << 11) | (b << 13);
        arm(backend, kBaseD);
        backend.clt_draw(vertices, 3, false, true, texture, kWidth, kHeight,
                         false, {}, {}, {}, blend);
        got = read_target(backend, kBaseD, "TAB alpha selector");
        const unsigned expected_alpha = static_cast<unsigned>(std::lround(alpha[a]*alpha[b]*255));
        if (got.empty() || !near_rgba(got[kWidth + 1], pack_rgba(255,0,0,expected_alpha),
                                      1, "TAB alpha selector", 1, 1)) return false;
    }
    return true;
}

bool case_opaque_source_over(m2host::ICelBlitterBackend& backend)
{
    m2host::CltVertex verts[3] = {
        vertex(0, 0, 0, 0, 1), vertex(kWidth, 0, 1, 0, 1),
        vertex(0, kHeight, 0, 1, 1),
    };
    for (auto& v : verts) { v.r = v.g = v.b = v.a = 1.f; }
    for (unsigned alpha : {0u, 128u, 255u}) {
        const uint32_t texel = pack_rgba(200, 100, 50, alpha);
        const auto texture = backend.cel_upload_rgba(1, 1, &texel);
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::opaque_source_over;
        m2host::CltDiscard discard;
        discard.alpha_zero = true;
        arm(backend, kBaseD);
        backend.clt_draw(verts, 3, false, true, texture, kWidth, kHeight,
                         false, discard, {}, {}, blend);
        const auto got = read_target(backend, kBaseD, "opaque source-over");
        const int px = kWidth / 4, py = kHeight / 4;
        // TAB chooses PRIMALPHA, so texel alpha affects neither output nor
        // alpha-zero discard. B has zero weight, regardless of destination.
        if (got.empty() || !near_rgba(got[py * kWidth + px],
                pack_rgba(200, 100, 50, 255), 0, "opaque source-over", px, py))
            return false;
    }
    return true;
}

bool case_texture_alpha_color(m2host::ICelBlitterBackend& backend)
{
    m2host::CltVertex verts[3] = {
        vertex(0, 0, 0, 0, 1),
        vertex(kWidth, 0, 1, 0, 1),
        vertex(0, kHeight, 0, 1, 1),
    };
    for (auto& v : verts) {
        v.r = 128.f / 255.f; v.g = 64.f / 255.f; v.b = 32.f / 255.f;
        v.a = .25f;
    }
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    for (unsigned alpha : {0u, 128u, 255u}) {
        const uint32_t texel = pack_rgba(250, 10, 200, alpha);
        const auto texture = backend.cel_upload_rgba(1, 1, &texel);
        blend.texture_alpha_as_color = true;
        arm(backend, kBaseD);
        backend.clt_draw(verts, 3, false, true, texture, kWidth, kHeight,
                         true, {}, {}, {}, blend);
        const auto got = read_target(backend, kBaseD, "TAB alpha mask");
        if (got.empty()) return false;
        const uint32_t expected = pack_rgba(
            (alpha * 128u + 127u) / 255u,
            (alpha * 64u + 127u) / 255u,
            (alpha * 32u + 127u) / 255u, 64u);
        const int px = kWidth / 4, py = kHeight / 4;
        if (!near_rgba(got[py * kWidth + px], expected, 1, "TAB alpha mask", px, py))
            return false;
        blend.texture_alpha_as_color = false;
        backend.clt_draw(verts, 3, false, true, texture, kWidth, kHeight,
                         true, {}, {}, {}, blend);
        const auto wrong = read_target(backend, kBaseD, "TAB control");
        if (wrong.empty() || !expect(wrong[py * kWidth + px] != expected,
                                    "TEXCOLOR substitution differs from alpha mask"))
            return false;
    }
    return true;
}

// --- D: source copy replaces the pixel regardless of alpha ----------------
//
// `source_copy` is the B-only identity-coefficient form: `(ONE, ZERO)`. A
// fully transparent texel with non-zero RGB must still overwrite the target; a
// source-over implementation would leave the clear color in place.
bool case_source_copy(m2host::ICelBlitterBackend& backend)
{
    const uint32_t transparent = pack_rgba(200u, 100u, 50u, 0u);
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &transparent);
    if (!expect(texture != m2host::kInvalidCel, "D: texture uploads"))
        return false;

    m2host::CltVertex verts[3] = {
        vertex(0.f, 0.f, 0.f, 0.f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
    };
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;

    arm(backend, kBaseD);
    backend.clt_draw(verts, 3, false, true, texture,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     true, {}, {}, {}, blend);
    const std::vector<uint32_t> got = read_target(backend, kBaseD, "D");
    if (got.empty()) return false;

    const int px = kWidth / 4, py = kHeight / 4;
    const uint32_t observed = got[static_cast<size_t>(py) * kWidth + px];
    return expect(observed == transparent,
                  "D: a transparent texel still replaces the pixel "
                  "(got 0x%08x want 0x%08x)",
                  observed, transparent);
}

// An explicit high-resolution mapping is keyed by the original decoded
// image. It must replace both ordinary and authored-mip uploads, while an
// unmatched source must keep its original texels.
bool case_texture_replacement(m2host::ICelBlitterBackend& backend)
{
    const uint32_t original[4] = {0, 0, 0, 0};
    const uint32_t unmatched[4] = {
        pack_rgba(0, 200, 0, 255), pack_rgba(0, 200, 0, 255),
        pack_rgba(0, 200, 0, 255), pack_rgba(0, 200, 0, 255)};
    const uint32_t mip = pack_rgba(0, 0, 200, 255);
    const m2host::CelLodLevel level{1, 1, &mip};
    const m2host::CelId ordinary = backend.cel_upload_rgba(2, 2, original);
    const m2host::CelId authored = backend.cel_upload_rgba_lod_chain(
        2, 2, original, &level, 1, true, false);
    const m2host::CelId fallback = backend.cel_upload_rgba(2, 2, unmatched);
    if (!expect(ordinary != m2host::kInvalidCel &&
                    authored != m2host::kInvalidCel &&
                    fallback != m2host::kInvalidCel,
                "K: all texture uploads succeed"))
        return false;
    const m2host::CltVertex verts[3] = {
        vertex(0.f, 0.f, .5f, .5f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, .5f, .5f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), .5f, .5f, 1.f)};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    const auto sample = [&](m2host::CelId texture) {
        arm(backend, kBaseK);
        backend.clt_draw(verts, 3, false, true, texture,
                         static_cast<float>(kWidth),
                         static_cast<float>(kHeight), true, {}, {}, {}, blend);
        const auto pixels = read_target(backend, kBaseK, "K");
        return pixels.empty() ? 0u : pixels[16u * kWidth + 16u];
    };
    const uint32_t red = pack_rgba(220, 0, 0, 255);
    const bool ok = expect(sample(ordinary) == red,
                           "K: ordinary upload uses mapped image") &&
                    expect(sample(authored) == red,
                           "K: authored mip upload uses mapped image") &&
                    expect(sample(fallback) == unmatched[0],
                           "K: unmatched source retains original image");
    backend.cel_free(ordinary);
    backend.cel_free(authored);
    backend.cel_free(fallback);
    return ok;
}

bool case_filter_choice(m2host::ICelBlitterBackend& backend, bool linear)
{
    const uint32_t texels[2] = {pack_rgba(255, 0, 0, 255),
                                pack_rgba(0, 0, 255, 255)};
    const auto texture = backend.cel_upload_rgba(2, 1, texels);
    if (!expect(texture != m2host::kInvalidCel, "L: source uploads"))
        return false;
    const m2host::CltVertex verts[3] = {
        vertex(0.f, 0.f, .5f, .5f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, .5f, .5f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), .5f, .5f, 1.f)};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    // The secondary-source path forces a point sampler. A later primary draw
    // must restore this cel's selected filter, including the opt-in override.
    m2host::CltBlendState secondary;
    secondary.mode = m2host::CltBlendMode::secondary_source_alpha;
    secondary.secondary_source = texture;
    arm(backend, kBaseL);
    backend.clt_draw(verts, 3, false, true, texture,
                     static_cast<float>(kWidth),
                     static_cast<float>(kHeight), true, {}, {}, {}, secondary);
    arm(backend, kBaseL);
    backend.clt_draw(verts, 3, false, true, texture,
                     static_cast<float>(kWidth),
                     static_cast<float>(kHeight), true, {}, {}, {}, blend);
    const auto got = read_target(backend, kBaseL, "L");
    backend.cel_free(texture);
    if (got.empty()) return false;
    const uint32_t pixel = got[16u * kWidth + 16u];
    return linear
        ? near_rgba(pixel, pack_rgba(128, 0, 128, 255), 1,
                    "L: linear setting blends adjacent texels", 16, 16)
        : expect(pixel == texels[1],
                 "L: default point setting keeps the source texel");
}

// --- M: anisotropic filtering request (M2_ANISOTROPY) ----------------------
//
// Anisotropic filtering is a per-texture GL sampler parameter that only has
// an effect under linear minification. A granted request therefore has two
// observable consequences on the primary texture: its
// GL_TEXTURE_MAX_ANISOTROPY_EXT equals min(requested, device maximum), and
// minification is linear (the same fixture as case L: a two-texel source
// stretched across the target blends at the center). A request the device
// cannot serve must leave the sampler at 1x and keep the guest point sample
// rather than reporting success. `requested` is what the harness set in
// M2_ANISOTROPY before the backend initialised (1 = unset).
bool case_anisotropy_sampler(m2host::ICelBlitterBackend& backend,
                             int requested)
{
    constexpr GLenum kTextureMaxAnisotropy = 0x84FE;
    constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;
    const uint32_t texels[2] = {pack_rgba(255, 0, 0, 255),
                                pack_rgba(0, 0, 255, 255)};
    const auto texture = backend.cel_upload_rgba(2, 1, texels);
    if (!expect(texture != m2host::kInvalidCel, "M: source uploads"))
        return false;
    const m2host::CltVertex verts[3] = {
        vertex(0.f, 0.f, .5f, .5f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, .5f, .5f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), .5f, .5f, 1.f)};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    arm(backend, kBaseM);
    backend.clt_draw(verts, 3, false, true, texture,
                     static_cast<float>(kWidth),
                     static_cast<float>(kHeight), true, {}, {}, {}, blend);
    const auto got = read_target(backend, kBaseM, "M");

    // The draw leaves the mask unit active; the primary texture of interest
    // sits on unit 0. glGetTexParameterfv addresses the bound target, not a
    // texture name. opengl32.dll exports only GL 1.1, so the entry point is
    // loaded the way the backend loads it.
    using PGActiveTexture = void(GLAPIENTRY*)(GLenum);
    const auto active_texture = reinterpret_cast<PGActiveTexture>(
        SDL_GL_GetProcAddress("glActiveTexture"));
    if (!expect(active_texture != nullptr, "M: glActiveTexture loads")) {
        backend.cel_free(texture);
        return false;
    }
    active_texture(GL_TEXTURE0);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLfloat anisotropy = -1.f;
    if (bound != 0)
        glGetTexParameterfv(GL_TEXTURE_2D, kTextureMaxAnisotropy,
                            &anisotropy);

    const bool supported =
        SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") ||
        SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic");
    GLfloat device_max = 1.f;
    if (supported) glGetFloatv(kMaxTextureMaxAnisotropy, &device_max);

    bool ok = expect(bound != 0,
                     "M: the primary texture is bound on unit 0 after the "
                     "draw (bound=%d)", bound);
    if (ok && requested > 1 && supported && device_max >= 2.f) {
        const GLfloat want =
            std::min(static_cast<GLfloat>(requested), device_max);
        ok = expect(anisotropy == want,
                    "M: M2_ANISOTROPY=%d installs %gx on the primary texture "
                    "(got %g, device max %g)", requested,
                    static_cast<double>(want),
                    static_cast<double>(anisotropy),
                    static_cast<double>(device_max)) && ok;
        if (!got.empty())
            ok = near_rgba(got[16u * kWidth + 16u],
                           pack_rgba(128, 0, 128, 255), 1,
                           "M: a granted request engages linear minification",
                           16, 16) && ok;
    } else {
        ok = expect(anisotropy == 1.f,
                    "M: without a usable anisotropic extension the sampler "
                    "stays at 1x (got %g)", static_cast<double>(anisotropy)) &&
             ok;
        if (!got.empty())
            ok = expect(got[16u * kWidth + 16u] == texels[1],
                        "M: a refused request keeps the guest point sample") &&
                 ok;
    }
    backend.cel_free(texture);
    return ok;
}

// --- N: multisample anti-aliasing request (M2_ANTIALIASING) ----------------
//
// MSAA applies to the addressed CLT color targets, so a triangle edge that
// cuts a pixel resolves to a coverage blend single-sample rasterization cannot
// produce; a single-sample target only ever writes the full source or the
// background. The same case forces the resolve paths the option adds — the
// full and windowed readbacks, a scaled snapshot (the high-resolution
// display-carve case, where a multisample blit may not change extents), and
// the present-to-drawable resolve — and requires the GL error state to stay
// clean across all of them. `requested` is what the harness set in
// M2_ANTIALIASING before the backend initialised (1 = unset).
bool case_multisample_antialiasing(m2host::ICelBlitterBackend& backend,
                                   int requested)
{
    constexpr GLenum kMaxSamples = 0x8D57;
    GLint max_samples = 1;
    glGetIntegerv(kMaxSamples, &max_samples);
    const bool granted = requested > 1 && max_samples >= 2;
    // Errors inherited from an earlier case must not be attributed here.
    while (glGetError() != GL_NO_ERROR) {}

    std::vector<uint32_t> black(static_cast<size_t>(kWidth) * kHeight,
                                pack_rgba(0, 0, 0, 255));
    arm(backend, kBaseN);
    backend.clt_target_upload_rgba(kBaseN, black.data(), kWidth, kHeight);

    const uint32_t red = pack_rgba(255, 0, 0, 255);
    const m2host::CelId red_cel = backend.cel_upload_rgba(1, 1, &red);
    if (!expect(red_cel != m2host::kInvalidCel, "N: red source uploads"))
        return false;
    // A right triangle whose hypotenuse slants across the target: every pixel
    // is either background or full red under single-sample rasterization, and
    // fractional coverage under multisampling.
    const m2host::CltVertex tri[3] = {
        vertex(32.f, 0.f, .5f, .5f, 1.f),
        vertex(64.f, 0.f, .5f, .5f, 1.f),
        vertex(32.f, 64.f, .5f, .5f, 1.f)};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    backend.clt_draw(tri, 3, false, true, red_cel,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     true, {}, {}, {}, blend);
    // Query the draw target before readback can bind a resolve framebuffer.
    GLint actual_samples = 0;
    glGetIntegerv(GL_SAMPLES, &actual_samples);
    backend.cel_free(red_cel);
    const auto drawn = read_target(backend, kBaseN, "N");
    if (drawn.empty()) return false;

    int partial = 0, solid = 0, background = 0;
    for (int y = 4; y < kHeight - 4; ++y) {
        for (int x = 4; x < kWidth - 4; ++x) {
            const unsigned r = drawn[static_cast<size_t>(y) * kWidth + x] & 0xffu;
            if (r >= 8u && r <= 247u)
                ++partial;
            else if (r > 247u)
                ++solid;
            else
                ++background;
        }
    }
    bool ok = expect((actual_samples > 1) == granted,
                     "N: %s request=%d max=%d draw target samples=%d",
                     backend.name(), requested, max_samples, actual_samples);
    if (granted) {
        ok = expect(partial > 0,
                    "N: M2_ANTIALIASING=%d resolves fractional edge coverage "
                    "(%d partial, %d solid, %d background)", requested,
                    partial, solid, background) && ok;
    } else {
        ok = expect(partial == 0,
                    "N: without multisample storage every edge pixel is "
                    "background or full source (%d partial)", partial) && ok;
        ok = expect(solid > 0 && background > 0,
                    "N: the fixture really drew an edge (%d solid, %d "
                    "background)", solid, background) && ok;
    }

    // A scaled snapshot resolves 1:1 first and scales afterwards; the source
    // here is a solid plane, so any incomplete or refused resolve shows up as
    // the wrong colour.
    std::vector<uint32_t> solid_image(static_cast<size_t>(kWidth) * kHeight,
                                      red);
    arm(backend, kBaseO);
    backend.clt_target_upload_rgba(kBaseO, solid_image.data(), kWidth, kHeight);
    const m2host::CelId snapshot =
        backend.clt_snapshot_target_cel(kBaseO, 16, 16, 16, 16, 8, 8);
    if (!expect(snapshot != m2host::kInvalidCel,
                "N: a scaled snapshot of the target is created")) {
        return ok;
    }
    const m2host::CltVertex quad[4] = {
        vertex(0.f, 0.f, 0.f, 0.f, 1.f),
        vertex(8.f, 0.f, 1.f, 0.f, 1.f),
        vertex(0.f, 8.f, 0.f, 1.f, 1.f),
        vertex(8.f, 8.f, 1.f, 1.f, 1.f)};
    m2host::CltBlendState copy;
    copy.mode = m2host::CltBlendMode::source_copy;
    arm(backend, kBaseN);
    backend.clt_draw(quad, 4, true, true, snapshot, 8.f, 8.f, true, {}, {}, {},
                     copy);
    backend.cel_free(snapshot);
    const auto snap_read = read_target(backend, kBaseN, "N snapshot");
    if (snap_read.empty()) return false;
    ok = near_rgba(snap_read[4u * kWidth + 4u], red, 1,
                   "N: the scaled snapshot keeps the source colour", 4, 4) && ok;

    // The windowed readback path (CPU DB-source materializer) resolves too.
    std::vector<uint32_t> window(8u * 8u, 0u);
    ok = expect(backend.grab_clt_target_window_rgba(kBaseN, window.data(), 0,
                                                    0, 8, 8),
                "N: the target serves a windowed readback") && ok;
    if (!window.empty())
        ok = near_rgba(window[4u * 8u + 4u], red, 1,
                       "N: windowed readback keeps the source colour", 4, 4) &&
             ok;

    // Rotate loads addressed framebuffer strips through the CPU window path.
    // A solid source cannot detect a second vertical flip after MSAA resolve.
    const auto rows = coordinate_pattern();
    backend.clt_target_upload_rgba(kBaseO, rows.data(), kWidth, kHeight);
    constexpr int wx = 7, wy = 9, ww = 12, wh = 10;
    std::vector<uint32_t> addressed_window(ww * wh);
    ok = expect(backend.grab_clt_target_window_rgba(
                    kBaseO, addressed_window.data(), wx, wy, ww, wh),
                "N: asymmetric framebuffer window is readable") && ok;
    for (int y = 0; y < wh; ++y)
        for (int x = 0; x < ww; ++x)
            ok = near_rgba(addressed_window[y * ww + x],
                           pattern_at(rows, wx + x, wy + y), 0,
                           "N: framebuffer window preserves top-left rows",
                           x, y) && ok;

    // Present resolves the multisample target onto the drawable.
    backend.frame_present();
    GLenum first_error = GL_NO_ERROR;
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
        if (first_error == GL_NO_ERROR) first_error = e;
    ok = expect(first_error == GL_NO_ERROR,
                "N: the resolve paths leave no GL error (first %#x)",
                first_error) && ok;
    return ok;
}
//
// `present()` binds the default drawable. A later guest list that keeps the
// same destination base is legitimate, so `select()` rebinds even when the
// requested base is unchanged (gl_clt_targets.h:84-91). Without the rebind the
// second draw would land on the drawable and the addressed target would still
// hold the first draw's color.
bool case_rebind_after_present(m2host::ICelBlitterBackend& backend)
{
    const auto fill = [&](uint32_t rgba) {
        const m2host::CelId cel = backend.cel_upload_rgba(1, 1, &rgba);
        m2host::CltVertex verts[3] = {
            vertex(0.f, 0.f, 0.f, 0.f, 1.f),
            vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 1.f),
            vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
        };
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::source_copy;
        backend.clt_draw(verts, 3, false, true, cel,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         true, {}, {}, {}, blend);
    };

    const uint32_t first = pack_rgba(11u, 22u, 33u, 255u);
    const uint32_t second = pack_rgba(200u, 100u, 50u, 255u);
    const int px = kWidth / 4, py = kHeight / 4;
    const size_t probe = static_cast<size_t>(py) * kWidth + px;

    arm(backend, kBaseE);
    fill(first);
    GLint bound_after_select = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_after_select);
    if (!expect(bound_after_select != 0,
                "E: the addressed target is a real FBO before the present"))
        return false;

    backend.frame_present();
    GLint bound_after_present = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_after_present);
    if (!expect(bound_after_present == 0,
                "E: present() leaves the default drawable bound"))
        return false;

    // The same base as before the present: the identity does not change.
    backend.clt_target_select(kBaseE);
    GLint bound_after_reselect = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_after_reselect);
    if (!expect(bound_after_reselect != 0,
                "E: re-selecting the unchanged base rebinds its FBO after a "
                "present (binding %d)",
                bound_after_reselect))
        return false;

    fill(second);
    const std::vector<uint32_t> got = read_target(backend, kBaseE, "E");
    if (got.empty()) return false;
    return expect(got[probe] == second,
                  "E: the post-present draw reached the addressed target "
                  "(got 0x%08x want 0x%08x)",
                  got[probe], second);
}

// --- F: resize invalidates the addressed storage --------------------------
//
// `frame_begin` re-reads the window size, so a host resize must reach the
// target pool: the stale size is refused, the new size reads, the active base
// survives, and the new surface is a usable, freshly cleared target.
bool case_resize(m2host::ICelBlitterBackend& backend, SDL_Window* window)
{
    const uint32_t fill_color = pack_rgba(9u, 9u, 200u, 255u);
    const m2host::CelId cel = backend.cel_upload_rgba(1, 1, &fill_color);
    m2host::CltVertex verts[3] = {
        vertex(0.f, 0.f, 0.f, 0.f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
    };
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    const auto fill = [&]() {
        backend.clt_draw(verts, 3, false, true, cel,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         true, {}, {}, {}, blend);
    };

    arm(backend, kBaseF);
    fill();
    std::vector<uint32_t> before(static_cast<size_t>(kWidth) * kHeight);
    if (!expect(backend.grab_clt_target_rgba(kBaseF, before.data(), kWidth,
                                             kHeight),
                "F: the pre-resize target reads back"))
        return false;

    if (!SDL_SetWindowSize(window, kResizedWidth, kResizedHeight)) {
        std::fprintf(stderr, "m2_gl_backend_parity: SKIP: no resize: %s\n",
                     SDL_GetError());
        return true;  // a window that refuses to resize cannot falsify this
    }
    SDL_SyncWindow(window);

    backend.frame_begin(kClearR, kClearG, kClearB);
    int reported_w = 0, reported_h = 0;
    backend.get_size(&reported_w, &reported_h);
    if (!expect(reported_w == kResizedWidth && reported_h == kResizedHeight,
                "F: the backend adopted the new window size (got %dx%d want "
                "%dx%d)", reported_w, reported_h, kResizedWidth,
                kResizedHeight))
        return false;

    // The stale size must no longer be the storage's size.
    std::vector<uint32_t> stale(static_cast<size_t>(kWidth) * kHeight, 0u);
    if (!expect(!backend.grab_clt_target_rgba(kBaseF, stale.data(), kWidth,
                                               kHeight),
                "F: a read at the pre-resize size is refused"))
        return false;

    // The active base survives the resize and is a usable, cleared target.
    std::vector<uint32_t> after(
        static_cast<size_t>(kResizedWidth) * kResizedHeight, 0u);
    if (!expect(backend.grab_clt_target_rgba(kBaseF, after.data(),
                                             kResizedWidth, kResizedHeight),
                "F: the active base reads back at the new size"))
        return false;
    if (!expect(after[0] == kClearWord,
                "F: the resized surface is freshly cleared (got 0x%08x want "
                "0x%08x)", after[0], kClearWord))
        return false;

    backend.clt_draw(verts, 3, false, true, cel,
                     static_cast<float>(kResizedWidth),
                     static_cast<float>(kResizedHeight), true, {}, {}, {}, blend);
    if (!expect(backend.grab_clt_target_rgba(kBaseF, after.data(),
                                             kResizedWidth, kResizedHeight),
                "F: the resized surface reads back after a draw"))
        return false;
    if (!expect(after[0] == fill_color,
                "F: a draw reaches the resized surface (got 0x%08x want "
                "0x%08x)", after[0], fill_color))
        return false;
    if (SDL_SetWindowSize(window, kWidth, kHeight)) {
        SDL_SyncWindow(window);
        backend.frame_begin(kClearR, kClearG, kClearB);
    }
    return true;
}

// --- G: the platform clear follows the tracked clip -----------------------
//
// `TE_CLEAR_FRAME_BUFFER` is served by the recovered driver's generated clear
// list, and that list is clipped by DBFBCLIP and the window clip exactly like a
// guest primitive (driver.c:251-278). Both GL backends must keep the scissor
// for this fill; dropping it clears the whole bound target and paints the
// display border the guest's smaller render target never writes. The clip is also
// tracked across a present, because `frame_present()` disables the GL scissor
// for the swap while the interpreter's rect stays current -- so the bounded arm
// below clears *after* a present, which is the sequence a guest list that keeps
// its destination across a frame boundary produces, and the only one in which
// re-asserting the clip can be observed. The final control releases the clip
// and requires the same clear to reach the pixels the bounded arm left alone,
// or that arm would not be distinguishing a bounded clear from an unbounded
// one.
bool case_clear_extent(m2host::ICelBlitterBackend& backend)
{
    int w = 0, h = 0;
    backend.get_size(&w, &h);
    if (!expect(w >= 8 && h >= 8,
                "G: the target has room for a bordered clear (got %dx%d)", w,
                h))
        return false;

    const uint32_t outside_color = pack_rgba(20u, 30u, 40u, 255u);
    const uint32_t inside_color = pack_rgba(210u, 220u, 230u, 255u);
    const uint32_t unclipped_color = pack_rgba(7u, 8u, 9u, 255u);
    const auto clear_with = [&](uint32_t rgba) {
        const auto component = [&](unsigned shift) {
            const float v = static_cast<float>((rgba >> shift) & 0xffu) / 255.f;
            return std::bit_cast<uint32_t>(v);
        };
        backend.clt_clear_color(component(0u), component(8u), component(16u),
                                component(24u));
    };
    const auto read = [&]() {
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0u);
        if (!backend.grab_clt_target_rgba(kBaseG, pixels.data(), w, h))
            pixels.clear();
        return pixels;
    };

    // Arm 1: the clip covers the target, so the clear must cover it too.
    arm(backend, kBaseG);
    backend.scissor(0.f, 0.f, static_cast<float>(w), static_cast<float>(h));
    clear_with(outside_color);
    std::vector<uint32_t> got = read();
    if (got.empty()) {
        expect(false, "G: the armed target reads back");
        return false;
    }
    if (!near_rgba(got[0], outside_color, 1, "G: whole-clip clear covers the "
                                              "origin", 0, 0))
        return false;

    // Arm 2: the clip is the lower-right quadrant and a present intervenes
    // before the clear. The clear must land in the clip and leave the rest of
    // the target holding arm 1's color -- a fill that escaped the tracked clip
    // would take the border with it.
    const int qx = w / 2, qy = h / 2;
    const int qw = w - qx, qh = h - qy;
    backend.scissor(static_cast<float>(qx), static_cast<float>(qy),
                    static_cast<float>(qw), static_cast<float>(qh));
    backend.frame_present();
    backend.clt_target_select(kBaseG);
    clear_with(inside_color);
    got = read();
    if (got.empty()) {
        expect(false, "G: the sub-clip target reads back");
        return false;
    }
    const size_t inside_index =
        static_cast<size_t>(qy + 1) * w + static_cast<size_t>(qx + 1);
    if (!near_rgba(got[inside_index], inside_color, 1,
                   "G: the clipped clear fills the clip box", qx + 1, qy + 1))
        return false;
    if (!near_rgba(got[0], outside_color, 1,
                   "G: after a present the clipped clear still leaves the "
                   "border untouched -- a whole-target fill is the wrong "
                   "result here", 0, 0))
        return false;
    if (!near_rgba(got[static_cast<size_t>(w) - 1u], outside_color, 1,
                   "G: the clipped clear leaves the opposite corner untouched",
                   w - 1, 0))
        return false;

    // Control, executed in the same run: the same clear with the clip released
    // must reach the pixels arm 2 left alone. If it did not, arm 2's border
    // assertion would not be distinguishing anything.
    backend.scissor_disable();
    clear_with(unclipped_color);
    std::vector<uint32_t> released = read();
    if (released.empty()) {
        expect(false, "G: the released-clip target reads back");
        return false;
    }
    if (!near_rgba(released[0], unclipped_color, 1,
                   "G: control -- releasing the clip lets the same clear reach "
                   "the border", 0, 0))
        return false;
    return near_rgba(released[inside_index], unclipped_color, 1,
                     "G: control -- the released clear also covers the previous "
                     "clip box", qx + 1, qy + 1);
}

// --- H: overlapping triangles preserve depth order across batching ---------
//
// Ordering hazard: two overlapping opaque
// triangles with matching nearer-only relations, submitted front-then-rear.
// If the backend runs a color pass with depth writes disabled followed by a
// depth-only pass, the rear triangle can overwrite the front triangle during
// the color pass because depth was not updated yet.
//
// Tests:
// 1. Matching nearer-only depth relation (0x3): both orders (front-then-rear,
//    rear-then-front), batched vs sequential. Front color must win in all 4.
// 2. Authored 0x7 relation (greater_equal pixel, greater Z-update): both orders,
//    batched vs sequential. Front color must win in all 4.
// 3. Differing relation (pixel=always, Z-update=greater): batched and sequential
//    must produce identical results in both orders (rear overwrites front for
//    front-then-rear; front overwrites rear for rear-then-front).
// 4. SSB coefficient split: batched and sequential must produce identical results.
// 5. Clipping: Z-region scissor splits tested (inside) vs untested (outside)
//    regions. Batched and sequential must produce identical results.
bool case_overlapping_triangle_depth_order(m2host::ICelBlitterBackend& backend)
{
    const auto make_vert = [](float x, float y, float r, float g, float b,
                              float w, float one_over_w) {
        m2host::CltVertex v{};
        v.x = x;
        v.y = y;
        v.r = r;
        v.g = g;
        v.b = b;
        v.a = 1.f;
        v.w = w;
        v.one_over_w = one_over_w;
        return v;
    };

    // Front: nearer (w=1.0, 1/w=0.8), red.
    const std::array<m2host::CltVertex, 3> front = {
        make_vert(16.f, 16.f, 0.9f, 0.1f, 0.1f, 1.f, 0.8f),
        make_vert(48.f, 16.f, 0.9f, 0.1f, 0.1f, 1.f, 0.8f),
        make_vert(32.f, 48.f, 0.9f, 0.1f, 0.1f, 1.f, 0.8f),
    };
    // Rear: farther (w=2.0, 1/w=0.4), blue.
    const std::array<m2host::CltVertex, 3> rear = {
        make_vert(16.f, 16.f, 0.1f, 0.1f, 0.9f, 2.f, 0.4f),
        make_vert(48.f, 16.f, 0.1f, 0.1f, 0.9f, 2.f, 0.4f),
        make_vert(32.f, 48.f, 0.1f, 0.1f, 0.9f, 2.f, 0.4f),
    };

    const uint32_t kFrontColor = pack_rgba(230u, 26u, 26u, 255u);
    const uint32_t kRearColor = pack_rgba(26u, 26u, 230u, 255u);
    const int px = 32, py = 28;
    const size_t probe_idx = static_cast<size_t>(py) * kWidth + px;

    const auto clear_target = [&]() {
        const auto comp = [](float v) { return std::bit_cast<uint32_t>(v); };
        backend.clt_clear_color(comp(kClearR), comp(kClearG), comp(kClearB), comp(1.f));
        backend.clt_clear_z(0);
    };

    // Helper to run a test arm and return the probed center pixel
    const auto run_case = [&](const m2host::CltDepth& depth,
                              bool front_first, bool batched,
                              const m2host::CltBlendState& blend = {},
                              m2host::CelId texture = m2host::kInvalidCel,
                              bool textured = false) -> uint32_t {
        arm(backend, kBaseH);
        clear_target();

        const auto& first = front_first ? front : rear;
        const auto& second = front_first ? rear : front;

        if (batched) {
            std::array<m2host::CltVertex, 6> batch = {
                first[0], first[1], first[2],
                second[0], second[1], second[2]
            };
            backend.clt_draw(batch.data(), 6, false, textured, texture,
                             static_cast<float>(kWidth), static_cast<float>(kHeight),
                             true, {}, depth, {}, blend, true);
        } else {
            backend.clt_draw(first.data(), 3, false, textured, texture,
                             static_cast<float>(kWidth), static_cast<float>(kHeight),
                             true, {}, depth, {}, blend, false);
            backend.clt_draw(second.data(), 3, false, textured, texture,
                             static_cast<float>(kWidth), static_cast<float>(kHeight),
                             true, {}, depth, {}, blend, false);
        }

        const std::vector<uint32_t> pix = read_target(backend, kBaseH, "H");
        if (pix.empty()) return 0;
        return pix[probe_idx];
    };

    // --- 1. Matching nearer-only relation (0x3): Front must win in all 4 combinations
    {
        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = m2host::kCltDbzPixelLt | m2host::kCltDbzZLt;  // 0x3
        depth.z_region_max_x = kWidth;
        depth.z_region_max_y = kHeight;

        const uint32_t f_batched = run_case(depth, true, true);
        if (!near_rgba(f_batched, kFrontColor, 2, "H: 0x3 front-then-rear batched front wins", px, py))
            return false;

        const uint32_t f_seq = run_case(depth, true, false);
        if (!near_rgba(f_seq, kFrontColor, 2, "H: 0x3 front-then-rear sequential front wins", px, py))
            return false;

        const uint32_t r_batched = run_case(depth, false, true);
        if (!near_rgba(r_batched, kFrontColor, 2, "H: 0x3 rear-then-front batched front wins", px, py))
            return false;

        const uint32_t r_seq = run_case(depth, false, false);
        if (!near_rgba(r_seq, kFrontColor, 2, "H: 0x3 rear-then-front sequential front wins", px, py))
            return false;
    }

    // --- 2. Authored 0x7 relation (greater_equal pixel, greater Z-update): Front must win
    {
        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq | m2host::kCltDbzZLt;  // 0x7
        depth.z_region_max_x = kWidth;
        depth.z_region_max_y = kHeight;

        const uint32_t f_batched = run_case(depth, true, true);
        if (!near_rgba(f_batched, kFrontColor, 2, "H: 0x7 front-then-rear batched front wins", px, py))
            return false;

        const uint32_t f_seq = run_case(depth, true, false);
        if (!near_rgba(f_seq, kFrontColor, 2, "H: 0x7 front-then-rear sequential front wins", px, py))
            return false;

        const uint32_t r_batched = run_case(depth, false, true);
        if (!near_rgba(r_batched, kFrontColor, 2, "H: 0x7 rear-then-front batched front wins", px, py))
            return false;

        const uint32_t r_seq = run_case(depth, false, false);
        if (!near_rgba(r_seq, kFrontColor, 2, "H: 0x7 rear-then-front sequential front wins", px, py))
            return false;
    }

    // --- 3. Differing relation (pixel=always, Z-update=greater):
    // Pixel always writes, but only greater updates Z.
    // For front-then-rear, rear is second and writes color regardless of Z, so rear wins.
    // For rear-then-front, front is second and writes color, so front wins.
    // Batched and sequential MUST produce identical results.
    {
        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq |
                           m2host::kCltDbzPixelGt | m2host::kCltDbzZLt;  // 0x17
        depth.z_region_max_x = kWidth;
        depth.z_region_max_y = kHeight;

        const uint32_t f_batched = run_case(depth, true, true);
        const uint32_t f_seq = run_case(depth, true, false);
        if (!near_rgba(f_batched, kRearColor, 2, "H: differing front-then-rear batched rear wins", px, py))
            return false;
        if (!expect(f_batched == f_seq, "H: differing front-then-rear batched matches sequential (0x%08x vs 0x%08x)",
                    f_batched, f_seq))
            return false;

        const uint32_t r_batched = run_case(depth, false, true);
        const uint32_t r_seq = run_case(depth, false, false);
        if (!near_rgba(r_batched, kFrontColor, 2, "H: differing rear-then-front batched front wins", px, py))
            return false;
        if (!expect(r_batched == r_seq, "H: differing rear-then-front batched matches sequential (0x%08x vs 0x%08x)",
                    r_batched, r_seq))
            return false;
    }

    // --- 4. SSB coefficient split: batched and sequential must match
    {
        const uint32_t ssb_mask_data = 0x00000000u;
        const m2host::CelId ssb_mask_cel = backend.cel_upload_rgba(1, 1, &ssb_mask_data);
        if (!expect(ssb_mask_cel != m2host::kInvalidCel, "H: SSB mask uploads"))
            return false;

        m2host::CltBlendState blend{};
        blend.mode = m2host::CltBlendMode::coefficient_weighted_mix;
        blend.weighted_mix_ssb_selects_entry1 = true;
        blend.weighted_mix_ssb_mask = ssb_mask_cel;
        blend.weight_tex_r = 1.f;
        blend.weight_tex_g = 1.f;
        blend.weight_tex_b = 1.f;
        blend.entry1_weight_tex_r = 0.5f;
        blend.entry1_weight_tex_g = 0.5f;
        blend.entry1_weight_tex_b = 0.5f;

        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = 0x7u;
        depth.z_region_max_x = kWidth;
        depth.z_region_max_y = kHeight;

        const uint32_t batched = run_case(depth, true, true, blend, ssb_mask_cel, true);
        const uint32_t seq = run_case(depth, true, false, blend, ssb_mask_cel, true);
        if (!expect(batched == seq && batched != 0,
                    "H: SSB split batched matches sequential (0x%08x vs 0x%08x)",
                    batched, seq))
            return false;
    }

    // --- 5. Clipping (TE Z-region scissor boundary):
    // Z-region covers left half of the target [0, 32). Right half [32, 64) is outside.
    // Inside Z-region (x=24, y=28): depth test active, Front (Red) wins.
    // Outside Z-region (x=40, y=28): depth test inactive, painter's order applies (Rear Blue wins).
    {
        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = 0x7u;
        depth.z_region_min_x = 0;
        depth.z_region_max_x = 32;
        depth.z_region_min_y = 0;
        depth.z_region_max_y = 64;
        depth.z_clipped_discard = false;

        const auto run_clip = [&](bool batched) -> std::pair<uint32_t, uint32_t> {
            arm(backend, kBaseH);
            clear_target();

            if (batched) {
                std::array<m2host::CltVertex, 6> batch = {
                    front[0], front[1], front[2],
                    rear[0], rear[1], rear[2]
                };
                backend.clt_draw(batch.data(), 6, false, false, m2host::kInvalidCel,
                                 static_cast<float>(kWidth), static_cast<float>(kHeight),
                                 true, {}, depth, {}, {}, true);
            } else {
                backend.clt_draw(front.data(), 3, false, false, m2host::kInvalidCel,
                                 static_cast<float>(kWidth), static_cast<float>(kHeight),
                                 true, {}, depth, {}, {}, false);
                backend.clt_draw(rear.data(), 3, false, false, m2host::kInvalidCel,
                                 static_cast<float>(kWidth), static_cast<float>(kHeight),
                                 true, {}, depth, {}, {}, false);
            }
            const std::vector<uint32_t> pix = read_target(backend, kBaseH, "H");
            if (pix.empty()) return {0, 0};
            return {pix[static_cast<size_t>(28) * kWidth + 24],
                    pix[static_cast<size_t>(28) * kWidth + 40]};
        };

        const auto [in_batched, out_batched] = run_clip(true);
        const auto [in_seq, out_seq] = run_clip(false);

        if (!near_rgba(in_batched, kFrontColor, 2, "H: clip inside Z-region batched front wins", 24, 28))
            return false;
        if (!near_rgba(in_seq, kFrontColor, 2, "H: clip inside Z-region sequential front wins", 24, 28))
            return false;
        if (!near_rgba(out_batched, kRearColor, 2, "H: clip outside Z-region batched rear wins", 40, 28))
            return false;
        if (!near_rgba(out_seq, kRearColor, 2, "H: clip outside Z-region sequential rear wins", 40, 28))
            return false;
    }

    // --- 6. Showroom-scale reciprocal W and coplanar output relation.
    // The measured showroom range is about 0.0063-0.0079. A 3e-7 gap at
    // 0.0063 spans at least five 24-bit depth steps over [0,1], so near
    // geometry must occlude the later far primitive. Equal depth is allowed
    // by the authored 0x7 pixel relation and can replace the color.
    {
        m2host::CltDepth depth{};
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func = 0x7u;
        depth.z_region_max_x = kWidth;
        depth.z_region_max_y = kHeight;
        const auto narrow = [&](float ow, float r, float g, float b) {
            std::array<m2host::CltVertex, 3> tri = front;
            for (auto& v : tri) {
                v.one_over_w = ow;
                v.r = r;
                v.g = g;
                v.b = b;
            }
            return tri;
        };
        const auto near_tri = narrow(0.0063003f, 0.9f, 0.1f, 0.1f);
        const auto far_tri = narrow(0.0063000f, 0.1f, 0.1f, 0.9f);
        const auto equal = narrow(0.0063003f, 0.1f, 0.9f, 0.1f);
        arm(backend, kBaseH);
        clear_target();
        backend.clt_draw(near_tri.data(), 3, false, false, m2host::kInvalidCel,
                         kWidth, kHeight, true, {}, depth);
        backend.clt_draw(far_tri.data(), 3, false, false, m2host::kInvalidCel,
                         kWidth, kHeight, true, {}, depth);
        const auto separated = read_target(backend, kBaseH, "H narrow");
        if (separated.empty() ||
            !near_rgba(separated[probe_idx], kFrontColor, 2,
                       "H: narrow W gap keeps nearer color", px, py))
            return false;
        backend.clt_draw(equal.data(), 3, false, false, m2host::kInvalidCel,
                         kWidth, kHeight, true, {}, depth);
        const auto coplanar = read_target(backend, kBaseH, "H coplanar");
        if (coplanar.empty() ||
            !near_rgba(coplanar[probe_idx], pack_rgba(26u, 230u, 26u, 255u),
                       2, "H: equal-depth color follows guest order", px, py))
            return false;
    }

    return true;
}

// A display-source read takes a copy at the command boundary. The source
// window is top-left addressed, its texture keeps that orientation, and later
// target writes cannot change an earlier sample. Non-five-bit color and a
// nonopaque source alpha distinguish the GPU presentation copy from the old
// RGB555/opaque CPU expansion.
bool case_gpu_feedback_snapshot(m2host::ICelBlitterBackend& backend)
{
    constexpr int x0 = 7, y0 = 9, w = 12, h = 10;
    std::vector<uint32_t> source(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            source[static_cast<size_t>(y) * kWidth + x] =
                pack_rgba(static_cast<unsigned>(x * 3 + 1),
                          static_cast<unsigned>(y * 3 + 2), 37u, 64u);
    arm(backend, kBaseI);
    backend.clt_target_upload_rgba(kBaseI, source.data(), kWidth, kHeight);
    const m2host::CelId first = backend.clt_snapshot_target_cel(
        kBaseI, x0, y0, w, h, w, h);
    if (!expect(first != m2host::kInvalidCel, "I: GPU snapshot created"))
        return false;
    if (!expect(backend.clt_snapshot_target_cel(0xdead0000u, x0, y0, w, h,
                                                w, h) == m2host::kInvalidCel,
                "I: an unknown addressed target cannot be sampled"))
        return false;

    const uint32_t changed = pack_rgba(5u, 211u, 103u, 128u);
    std::fill(source.begin(), source.end(), changed);
    backend.clt_target_upload_rgba(kBaseI, source.data(), kWidth, kHeight);
    const m2host::CelId second = backend.clt_snapshot_target_cel(
        kBaseI, x0, y0, w, h, w, h);
    if (!expect(second != m2host::kInvalidCel, "I: second GPU snapshot created"))
        return false;

    const auto draw = [&](m2host::CelId cel, float left) {
        std::array<m2host::CltVertex, 4> quad{
            vertex(left, 0.f, 0.f, 0.f, 1.f),
            vertex(left + w, 0.f, 1.f, 0.f, 1.f),
            vertex(left, static_cast<float>(h), 0.f, 1.f, 1.f),
            vertex(left + w, static_cast<float>(h), 1.f, 1.f, 1.f),
        };
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::source_copy;
        blend.db_source_linear = true;
        blend.db_source_height = static_cast<float>(h);
        backend.clt_draw(quad.data(), 4, true, true, cel,
                         static_cast<float>(kWidth),
                         static_cast<float>(kHeight), true, {}, {}, {}, blend);
    };
    arm(backend, kBaseJ);
    draw(first, 0.f);
    draw(second, 20.f);
    const uint32_t primary_pixel = pack_rgba(200u, 10u, 20u, 128u);
    const m2host::CelId primary =
        backend.cel_upload_rgba(1, 1, &primary_pixel);
    if (!expect(primary != m2host::kInvalidCel,
                "I: reverse-alpha primary texture uploads"))
        return false;
    std::array<m2host::CltVertex, 3> reverse{
        vertex(0.f, 16.f, 0.f, 0.f, 1.f),
        vertex(12.f, 16.f, 1.f, 0.f, 1.f),
        vertex(0.f, 28.f, 0.f, 1.f, 1.f),
    };
    for (auto& v : reverse) {
        v.mask_u = 0.5f;
        v.mask_v = 0.5f;
    }
    m2host::CltBlendState reverse_blend;
    reverse_blend.mode = m2host::CltBlendMode::secondary_source_alpha;
    reverse_blend.secondary_source = second;
    reverse_blend.db_source_linear = true;
    reverse_blend.db_source_height = static_cast<float>(h);
    backend.clt_draw(reverse.data(), 3, false, true, primary,
                     kWidth, kHeight, true, {}, {}, {}, reverse_blend);
    const auto got = read_target(backend, kBaseJ, "I");
    backend.cel_free(primary);
    backend.cel_free(first);
    backend.cel_free(second);
    if (got.empty()) return false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t original = pack_rgba(
                static_cast<unsigned>((x0 + x) * 3 + 1),
                static_cast<unsigned>((y0 + y) * 3 + 2), 37u, 255u);
            if (!near_rgba(got[static_cast<size_t>(y) * kWidth + x],
                           original, 0, "I: original window and lifetime", x, y))
                return false;
            const uint32_t current = pack_rgba(5u, 211u, 103u, 255u);
            if (!near_rgba(got[static_cast<size_t>(y) * kWidth + x + 20],
                           current, 0, "I: later target generation", x + 20, y))
                return false;
        }
    }
    const float t = 128.f / 255.f;
    const auto mix = [&](unsigned p, unsigned s) {
        return static_cast<unsigned>(std::lround(p * (1.f - t) + s * t));
    };
    const uint32_t reverse_want = pack_rgba(
        mix(200u, 5u), mix(10u, 211u), mix(20u, 103u), 128u);
    if (!near_rgba(got[static_cast<size_t>(19) * kWidth + 3],
                   reverse_want, 1,
                   "I: GPU feedback serves the reverse-alpha source", 3, 19))
        return false;
    return true;
}

// --- P: SSB mask wrapping follows the primary texture's address rules -------
//
// The SSB sidecar is part of the selected texel: when the primary texture
// wraps, the SSB mask must sample the wrapped texel rather than clamping to
// the texture edge. A repeating 4x1 texture with mask [1,0,1,0] sampled at
// U=1.125 wraps to texel 0 (SSB=1, opaque white); clamping reaches texel 3
// (SSB=0) and discards the fragment.
bool case_ssb_mask_wrap(m2host::ICelBlitterBackend& backend)
{
    constexpr uint32_t kOpaqueWhite = pack_rgba(255u, 255u, 255u, 255u);
    const uint32_t white[4] = {kOpaqueWhite, kOpaqueWhite, kOpaqueWhite, kOpaqueWhite};
    const m2host::CelId tex_4x1 = backend.cel_upload_rgba(4, 1, white);
    if (!expect(tex_4x1 != m2host::kInvalidCel, "P: 4x1 texture uploads"))
        return false;

    // Mask pattern [1, 0, 1, 0]: texels 0 and 2 have SSB=1, texels 1 and 3 have SSB=0.
    const uint32_t mask_bits[4] = {0xffffffffu, 0x00000000u, 0xffffffffu, 0x00000000u};
    const m2host::CelId mask_4x1 = backend.cel_upload_rgba(4, 1, mask_bits);
    if (!expect(mask_4x1 != m2host::kInvalidCel, "P: 4x1 SSB mask uploads"))
        return false;

    const auto draw_quad = [&](uint32_t base, m2host::CelId tex, m2host::CelId mask,
                               float u, float v, bool wrap_u, bool wrap_v,
                               uint32_t u_period, uint32_t v_period) {
        arm(backend, base);
        std::array<m2host::CltVertex, 4> quad{
            vertex(0.f, 0.f, u, v, 1.f),
            vertex(static_cast<float>(kWidth), 0.f, u, v, 1.f),
            vertex(0.f, static_cast<float>(kHeight), u, v, 1.f),
            vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), u, v, 1.f),
        };
        m2host::CltDiscard discard;
        discard.ssb_zero = true;
        discard.ssb_mask = mask;
        m2host::CltTextureSampling sampling;
        sampling.wrap_u = wrap_u;
        sampling.wrap_v = wrap_v;
        sampling.u_period = u_period;
        sampling.v_period = v_period;
        backend.clt_draw(quad.data(), 4, true, true, tex,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         true, discard, {}, sampling);
        return read_target(backend, base, "P");
    };

    const size_t center = static_cast<size_t>(kHeight / 2) * kWidth + (kWidth / 2);

    // 1. Primary failure case: U=1.125 wraps to 0.125 (texel 0, SSB=1).
    // Must NOT be discarded -> opaque white.
    // If the mask clamps to 1.0, it samples texel 3 (SSB=0) and discards -> kClearWord.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_4x1, mask_4x1, 1.125f, 0.5f, true, false, 4, 1);
        if (got.empty()) return false;
        if (!expect(got[center] == kOpaqueWhite,
                    "P: wrapped U=1.125 samples texel 0 (SSB=1, white); "
                    "got 0x%08x want 0x%08x (clear was 0x%08x)",
                    got[center], kOpaqueWhite, kClearWord))
            return false;
    }

    // 2. Negative control: U=1.375 wraps to 0.375 (texel 1, SSB=0).
    // Must be discarded -> kClearWord.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_4x1, mask_4x1, 1.375f, 0.5f, true, false, 4, 1);
        if (got.empty()) return false;
        if (!expect(got[center] == kClearWord,
                    "P: wrapped U=1.375 samples texel 1 (SSB=0, discarded); "
                    "got 0x%08x want 0x%08x",
                    got[center], kClearWord))
            return false;
    }

    // 3. Clamped control: U=1.125 with wrap_u=false clamps to 1.0 (texel 3, SSB=0).
    // Must be discarded -> kClearWord.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_4x1, mask_4x1, 1.125f, 0.5f, false, false, 4, 1);
        if (got.empty()) return false;
        if (!expect(got[center] == kClearWord,
                    "P: clamped U=1.125 with wrap_u=false samples texel 3 (SSB=0, discarded); "
                    "got 0x%08x want 0x%08x",
                    got[center], kClearWord))
            return false;
    }

    // 4. Unwrapped control: U=0.125 within [0, 1] samples texel 0 (SSB=1).
    // Must NOT be discarded -> opaque white.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_4x1, mask_4x1, 0.125f, 0.5f, true, false, 4, 1);
        if (got.empty()) return false;
        if (!expect(got[center] == kOpaqueWhite,
                    "P: unwrapped U=0.125 samples texel 0 (SSB=1, white); "
                    "got 0x%08x want 0x%08x",
                    got[center], kOpaqueWhite))
            return false;
    }

    // 5. Vertical wrap test: 1x4 texture with mask [1, 0, 1, 0] in V.
    const m2host::CelId tex_1x4 = backend.cel_upload_rgba(1, 4, white);
    const m2host::CelId mask_1x4 = backend.cel_upload_rgba(1, 4, mask_bits);
    if (!expect(tex_1x4 != m2host::kInvalidCel && mask_1x4 != m2host::kInvalidCel,
                "P: 1x4 textures upload"))
        return false;

    // V=1.125 with wrap_v=true wraps to 0.125 (texel 0, SSB=1) -> white.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_1x4, mask_1x4, 0.5f, 1.125f, false, true, 1, 4);
        if (got.empty()) return false;
        if (!expect(got[center] == kOpaqueWhite,
                    "P: wrapped V=1.125 samples texel 0 (SSB=1, white); "
                    "got 0x%08x want 0x%08x",
                    got[center], kOpaqueWhite))
            return false;
    }

    // V=1.125 with wrap_v=false clamps to 1.0 (texel 3, SSB=0) -> discarded.
    {
        const std::vector<uint32_t> got =
            draw_quad(kBaseP, tex_1x4, mask_1x4, 0.5f, 1.125f, false, false, 1, 4);
        if (got.empty()) return false;
        if (!expect(got[center] == kClearWord,
                    "P: clamped V=1.125 with wrap_v=false samples texel 3 (SSB=0, discarded); "
                    "got 0x%08x want 0x%08x",
                    got[center], kClearWord))
            return false;
    }

    // 6. Weighted mix SSB coefficient selection:
    // Mode coefficient_weighted_mix with weighted_mix_ssb_selects_entry1.
    // Entry 0 weight = 0.5 (half bright), Entry 1 weight = 1.0 (full white).
    {
        arm(backend, kBaseP);
        std::array<m2host::CltVertex, 4> quad{
            vertex(0.f, 0.f, 1.125f, 0.5f, 1.f),
            vertex(static_cast<float>(kWidth), 0.f, 1.125f, 0.5f, 1.f),
            vertex(0.f, static_cast<float>(kHeight), 1.125f, 0.5f, 1.f),
            vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), 1.125f, 0.5f, 1.f),
        };
        m2host::CltDiscard discard;
        discard.ssb_zero = false;
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::coefficient_weighted_mix;
        blend.weighted_mix_ssb_selects_entry1 = true;
        blend.weighted_mix_ssb_mask = mask_4x1;
        blend.weight_tex_r = blend.weight_tex_g = blend.weight_tex_b = 0.5f;
        blend.entry1_weight_tex_r = blend.entry1_weight_tex_g = blend.entry1_weight_tex_b = 1.0f;
        m2host::CltTextureSampling sampling;
        sampling.wrap_u = true;
        sampling.wrap_v = false;
        sampling.u_period = 4;
        sampling.v_period = 1;
        backend.clt_draw(quad.data(), 4, true, true, tex_4x1,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         true, discard, {}, sampling, blend);
        const std::vector<uint32_t> got = read_target(backend, kBaseP, "P");
        if (got.empty()) return false;
        // Wrapped U=1.125 has SSB=1 -> selects entry 1 (weight 1.0 -> white).
        // If clamped, SSB=0 -> selects entry 0 (weight 0.5 -> 128 grey).
        const uint32_t px = got[center];
        const uint8_t r = px & 0xffu;
        if (!expect(r > 200,
                    "P: weighted mix wrapped U=1.125 selects entry 1 (SSB=1, full brightness); "
                    "got r=%u (expected ~255, entry 0 gives ~128)",
                    r))
            return false;
    }

    return true;
}

// GL-MASKED-UV-LOD-SEAM: address wrapping under TXTUVMASK must not turn
// a smooth magnified footprint into minification at wrap seams.
// Solid 8x8 red LOD0, authored 4x4 green LOD1, authored 2x2 blue LOD2.
// U wrap with period 4 texels (period 0.5 in normalized space), V repeat = false.
// LODMAX = 2, point filters.
// 64x64 strip spanning X 0..64, Y 0..64 with U spanning -1/64..63/64 and V 0..1.
// Unfolded footprint is 8 texels / 64 px = 1/8 texel per pixel (magnification).
// The center pixel (32, 32) straddles the folded U=0.5 seam.
// Without gradient preservation, implicit derivatives see the folded jump
// and select blue LOD2 (0xffff0000). With gradient preservation, center
// and interior both select red LOD0 (0xff0000ff).
bool case_masked_uv_lod_seam(m2host::ICelBlitterBackend& backend)
{
    constexpr uint32_t kRedLOD0 = pack_rgba(255, 0, 0, 255);
    constexpr uint32_t kGreenLOD1 = pack_rgba(0, 255, 0, 255);
    constexpr uint32_t kBlueLOD2 = pack_rgba(0, 0, 255, 255);

    const std::vector<uint32_t> red_8x8(8 * 8, kRedLOD0);
    const std::vector<uint32_t> green_4x4(4 * 4, kGreenLOD1);
    const std::vector<uint32_t> blue_2x2(2 * 2, kBlueLOD2);

    const std::array<m2host::CelLodLevel, 2> levels{{
        {4, 4, green_4x4.data()},
        {2, 2, blue_2x2.data()},
    }};

    const m2host::CelId tex = backend.cel_upload_rgba_lod_chain(
        8, 8, red_8x8.data(), levels.data(), levels.size(), true, false);
    if (!expect(tex != m2host::kInvalidCel, "Q: 8x8 LOD chain uploads"))
        return false;

    const auto draw_strip = [&](uint32_t base, float u0, float u1) {
        arm(backend, base);
        std::array<m2host::CltVertex, 4> strip{
            vertex(0.f, 0.f, u0, 0.f, 1.f),
            vertex(static_cast<float>(kWidth), 0.f, u1, 0.f, 1.f),
            vertex(0.f, static_cast<float>(kHeight), u0, 1.f, 1.f),
            vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), u1, 1.f, 1.f),
        };
        m2host::CltTextureSampling sampling;
        sampling.min_filter = m2host::CltTextureFilter::point;
        sampling.mag_filter = m2host::CltTextureFilter::point;
        sampling.inter_filter = m2host::CltTextureFilter::point;
        sampling.lod_max = 2;
        sampling.wrap_u = true;
        sampling.wrap_v = false;
        sampling.u_period = 4;
        sampling.v_period = 8;
        backend.clt_draw(strip.data(), 4, true, true, tex,
                         static_cast<float>(kWidth), static_cast<float>(kHeight),
                         false, {}, {}, sampling);
        return read_target(backend, base, "Q");
    };

    const size_t center = static_cast<size_t>(kHeight / 2) * kWidth + (kWidth / 2);
    const size_t interior_left = static_cast<size_t>(kHeight / 2) * kWidth + (kWidth / 4);
    const size_t interior_right = static_cast<size_t>(kHeight / 2) * kWidth + (3 * kWidth / 4);

    // 1. Primary failure case: U spans -1/64..63/64. The folded U=0.5 seam falls between
    // x=31 and x=32. Center (32, 32) must be red LOD0 (0xff0000ff).
    {
        const std::vector<uint32_t> got =
            draw_strip(kBaseQ, -1.f / 64.f, 63.f / 64.f);
        if (got.empty()) { backend.cel_free(tex); return false; }
        if (!expect(got[center] == kRedLOD0,
                    "Q: seam center (32,32) under magnification selects red LOD0; "
                    "got 0x%08x want 0x%08x (0x%08x would indicate blue LOD2 seam defect)",
                    got[center], kRedLOD0, kBlueLOD2)) {
            backend.cel_free(tex);
            return false;
        }
        if (!expect(got[interior_left] == kRedLOD0,
                    "Q: interior left (16,32) selects red LOD0; got 0x%08x want 0x%08x",
                    got[interior_left], kRedLOD0)) {
            backend.cel_free(tex);
            return false;
        }
        if (!expect(got[interior_right] == kRedLOD0,
                    "Q: interior right (48,32) selects red LOD0; got 0x%08x want 0x%08x",
                    got[interior_right], kRedLOD0)) {
            backend.cel_free(tex);
            return false;
        }
    }

    // 2. Executed control: shift U by +1/64 so U spans 0..1 (seam lies between derivative pairs).
    {
        const std::vector<uint32_t> got =
            draw_strip(kBaseQ, 0.f, 1.f);
        if (got.empty()) { backend.cel_free(tex); return false; }
        if (!expect(got[center] == kRedLOD0,
                    "Q: control shifted U spans 0..1, center selects red LOD0; "
                    "got 0x%08x want 0x%08x",
                    got[center], kRedLOD0)) {
            backend.cel_free(tex);
            return false;
        }
    }

    backend.cel_free(tex);
    return true;
}

// The discriminator. A substituted CPU backend reports `soft_capture`, holds
// no GL context, and answers no renderer string, so it cannot pass.
//
// Order matters: the context must be tested before any GL entry point is
// called. `glGetString` with no current context is undefined and crashes on
// macOS, and this predicate's whole job is to describe the case where no
// context exists, so it must never issue a GL call to find that out.
bool case_projector_pixel_replication(m2host::ICelBlitterBackend& backend)
{
    constexpr uint32_t base = 0x24000u;
    const auto pattern = coordinate_pattern();
    arm(backend, base);
    backend.clt_target_upload_rgba(base, pattern.data(), kWidth, kHeight);
    backend.clt_target_scanout_rect(base, {8, 4, 32, 16, 32, 32});
    backend.clt_scanout_select(base);
    std::vector<uint32_t> got(kWidth * kHeight);
    if (!expect(backend.grab_presented_rgba(got.data(), kWidth, kHeight),
                "S: expanded target can be captured")) return false;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const bool inside = x >= 8 && x < 40 && y >= 4 && y < 36;
            const int sy = inside ? 4 + (y - 4) / 2 : y;
            if (!expect(got[size_t(y) * kWidth + x] == pattern_at(pattern, x, sy),
                        "S: pixYSize=2 repeats rows inside the physical View (%d,%d)", x, y))
                return false;
        }
    if (!expect(read_target(backend, base, "S") == pattern,
                "S: TE source stays unexpanded for feedback")) return false;
    // Execute the old result as a control: unity projection must differ from
    // doubled-row scanout, with the exact same stored target.
    backend.clt_target_scanout_rect(base, {8, 4, 32, 16, 32, 16});
    std::vector<uint32_t> unity(kWidth * kHeight);
    if (!expect(backend.grab_presented_rgba(unity.data(), kWidth, kHeight) &&
                unity == pattern && unity != got,
                "S: unity projection rejects the half-height result")) return false;
    backend.clt_target_scanout_rect(base, {8, 4, 32, 16, 32, 32});
    backend.frame_present();
    // Exercise the actual GL presentation blit as well as capture. After a
    // swap the back buffer is unspecified, so present again with swap disabled
    // is not an oracle; the addressed capture above remains deterministic.
    backend.clt_scanout_select(0);
    return true;
}

// --- T: opt-in RGB555 output dithering --------------------------------------
// A flat gray (128) opaque draw with the toolkit's appendix-A matrix must land
// on exactly the per-pixel pattern the shared CPU model computes (offset by
// the low two bits of the guest X/Y, then truncated to 555 with MSB
// replication), and the same draw with dithering off must stay uniform 128.
bool case_rgb555_dither(m2host::ICelBlitterBackend& backend)
{
    m2host::CltVertex verts[6] = {
        vertex(0, 0, 0, 0, 1), vertex(kWidth, 0, 1, 0, 1),
        vertex(0, kHeight, 0, 1, 1), vertex(kWidth, 0, 1, 0, 1),
        vertex(kWidth, kHeight, 1, 1, 1), vertex(0, kHeight, 0, 1, 1),
    };
    for (auto& v : verts) { v.r = v.g = v.b = v.a = 1.f; }
    const uint32_t gray = pack_rgba(128, 128, 128, 255);
    const auto texture = backend.cel_upload_rgba(1, 1, &gray);
    m2host::CltDither dither;
    dither.enabled = true;
    dither.matrix_a = 0xc0d12e3fu;
    dither.matrix_b = 0xe1d0302fu;
    for (const bool on : {false, true}) {
        m2host::CltBlendState blend;
        blend.dither = dither;
        blend.dither.enabled = on;
        arm(backend, kBaseD);
        backend.clt_draw(verts, 6, false, true, texture, kWidth, kHeight,
                         false, {}, {}, {}, blend, true);
        const auto got = read_target(backend, kBaseD, "T");
        if (!expect(got.size() == size_t(kWidth) * kHeight,
                    "T: dither readback has full extent")) return false;
        bool varies = false;
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                const uint32_t want = on
                    ? m2host::clt_dither_pixel(dither, gray, x, y) : gray;
                const uint32_t have = got[size_t(y) * kWidth + x];
                if (have != gray) varies = true;
                if (!expect(have == want,
                            "T: dither=%d at (%d,%d): got 0x%08x want 0x%08x",
                            on ? 1 : 0, x, y, have, want)) return false;
            }
        if (!expect(varies == on,
                    "T: dither=%d pattern %s", on ? 1 : 0,
                    on ? "must differ from flat gray (control)"
                       : "must leave the flat gray untouched")) return false;
    }
    return true;
}

// --- U: retired-texture recycle identity ------------------------------------
// `cel_free` retires a texture into the cache the next upload is served from.
// The cache holds no CPU copy of the texels, so the packed-RGBA identity in
// the cache key is the only thing separating two same-sized, same-sampler
// images: an upload whose bytes differ must never be served from a retired
// one, and re-uploading the retired image must still return that image.
bool case_texture_cache_recycle(m2host::ICelBlitterBackend& backend)
{
    constexpr int kExtent = 4;
    constexpr size_t kTexels = static_cast<size_t>(kExtent) * kExtent;
    const uint32_t base_color = pack_rgba(210, 20, 10, 255);
    const uint32_t other_color = pack_rgba(10, 190, 90, 255);
    std::vector<uint32_t> first(kTexels, base_color);
    // One texel differs, so the two images agree in every key field except
    // the content identity.
    std::vector<uint32_t> second(kTexels, base_color);
    second[1u * kExtent + 1u] = other_color;

    // A screen-covering strip with UV 0..1: destination pixel (16,16) reads
    // texel (1,1) and (48,48) reads texel (3,3) under point sampling.
    const std::array<m2host::CltVertex, 4> quad{
        vertex(0.f, 0.f, 0.f, 0.f, 1.f),
        vertex(static_cast<float>(kWidth), 0.f, 1.f, 0.f, 1.f),
        vertex(0.f, static_cast<float>(kHeight), 0.f, 1.f, 1.f),
        vertex(static_cast<float>(kWidth), static_cast<float>(kHeight), 1.f,
               1.f, 1.f)};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    const auto observed = [&](m2host::CelId texture, int x, int y,
                              const char* what) {
        arm(backend, kBaseU);
        backend.clt_draw(quad.data(), 4, true, true, texture,
                         static_cast<float>(kWidth),
                         static_cast<float>(kHeight), true, {}, {}, {}, blend);
        const auto pixels = read_target(backend, kBaseU, what);
        return pixels.empty() ? 0u
                              : pixels[static_cast<size_t>(y) * kWidth + x];
    };
    const auto sample = [&](m2host::CelId texture, const char* what) {
        return observed(texture, 16, 16, what);
    };
    const auto corner = [&](m2host::CelId texture, const char* what) {
        return observed(texture, 48, 48, what);
    };

    const m2host::CelId uploaded_first =
        backend.cel_upload_rgba(kExtent, kExtent, first.data());
    if (!expect(uploaded_first != m2host::kInvalidCel,
                "U: the first 4x4 uploads"))
        return false;
    bool ok = expect(sample(uploaded_first, "U") == base_color,
                     "U: the first image is drawn");
    backend.cel_free(uploaded_first);

    const m2host::CelId uploaded_second =
        backend.cel_upload_rgba(kExtent, kExtent, second.data());
    if (!expect(uploaded_second != m2host::kInvalidCel,
                "U: the differing 4x4 uploads")) {
        backend.cel_free(uploaded_second);
        return false;
    }
    // The retired image must not answer this upload: the differing texel is
    // what a key that ignored the content identity would lose.
    ok = expect(sample(uploaded_second, "U") == other_color,
                "U: a differing image is not served from the retired one") &&
         ok;
    ok = expect(corner(uploaded_second, "U") == base_color,
                "U: only the differing texel moved") && ok;
    backend.cel_free(uploaded_second);

    // The retired image is still recyclable: the same bytes must come back
    // with their own texels, whichever of the two textures carries them.
    const m2host::CelId uploaded_again =
        backend.cel_upload_rgba(kExtent, kExtent, first.data());
    if (!expect(uploaded_again != m2host::kInvalidCel,
                "U: the first image re-uploads"))
        return false;
    ok = expect(sample(uploaded_again, "U") == base_color,
                "U: a recycled texture serves the image it was uploaded with") &&
         ok;
    backend.cel_free(uploaded_again);
    return ok;
}

bool is_real_gl_backend(const m2host::ICelBlitterBackend& backend)
{
    const char* name = backend.name();
    if (!name) return false;
    if (std::strcmp(name, "opengl") != 0 && std::strcmp(name, "cleangl") != 0)
        return false;
    if (!backend.clt_supports_triangle_batches()) return false;
    if (SDL_GL_GetCurrentContext() == nullptr) return false;
    const GLubyte* renderer = glGetString(GL_RENDERER);
    return renderer != nullptr && renderer[0] != '\0';
}

// Same ordering rule: safe to call when the identity check has already failed
// for want of a context, which is exactly when the diagnostic is needed.
const char* current_renderer()
{
    if (SDL_GL_GetCurrentContext() == nullptr) return "(no current context)";
    const GLubyte* renderer = glGetString(GL_RENDERER);
    return renderer ? reinterpret_cast<const char*>(renderer) : "(null)";
}

bool run_backend(const char* label,
                 std::unique_ptr<m2host::ICelBlitterBackend> (*factory)(),
                 bool filter_only = false, int anisotropy = 0,
                 int antialiasing = 0)
{
    std::unique_ptr<m2host::ICelBlitterBackend> backend = factory();
    if (!expect(backend != nullptr, "%s: factory returned no backend", label))
        return false;

    SDL_Window* window =
        SDL_CreateWindow(label, kWidth, kHeight,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "m2_gl_backend_parity: SKIP: no window: %s\n",
                     SDL_GetError());
        return true;
    }
    // A backend that cannot initialise on a window this process just created
    // is a parity failure, not a skip: main() has already shown the context
    // is available.
    if (!backend->init(window)) {
        std::fprintf(stderr,
                     "m2_gl_backend_parity: FAIL: %s: init() failed on a "
                     "window this process could create: %s\n",
                     label, SDL_GetError());
        ++g_failures;
        SDL_DestroyWindow(window);
        return false;
    }

    bool ok = expect(
        is_real_gl_backend(*backend),
        "%s: this must be the real GL backend, not a software substitute "
        "(name=%s context=%p renderer=%s)",
        label, backend->name(),
        static_cast<void*>(SDL_GL_GetCurrentContext()), current_renderer());
    if (ok) {
        std::printf("m2_gl_backend_parity: %s ready (%s, %s)\n", label,
                    backend->name(), current_renderer());
        if (anisotropy > 0) {
            ok = case_anisotropy_sampler(*backend, anisotropy);
        } else if (filter_only) {
            ok = case_filter_choice(*backend, true);
        } else {
        // Each case reports independently so one failure does not hide the
        // state the next case needs.
        const bool a = case_varying_w_db_source(*backend);
        const bool b = case_signed_row_carry(*backend);
        const bool c = case_secondary_source_alpha(*backend);
        const bool c_forward = case_secondary_source_alpha(
            *backend, m2host::CltBlendMode::secondary_source_over);
        const bool c_constant = case_secondary_source_alpha(
            *backend, m2host::CltBlendMode::secondary_constant_over);
        const bool d = case_source_copy(*backend);
        const bool opaque = case_opaque_source_over(*backend);
        const bool general_tab = case_general_tab(*backend);
        const bool general_dsb = case_general_dsb(*backend) && case_framebuffer_metadata(*backend);
        const bool shadow_alias = case_shadow_color_z_alias(*backend);
        const bool tab_alpha = case_texture_alpha_color(*backend);
        const bool e = case_rebind_after_present(*backend);
        const bool s = case_projector_pixel_replication(*backend);
        const bool f = case_resize(*backend, window);
        const bool g = case_clear_extent(*backend);
        const bool h = case_overlapping_triangle_depth_order(*backend);
        const bool i = case_gpu_feedback_snapshot(*backend);
        const bool k = case_texture_replacement(*backend);
        const bool l = case_filter_choice(*backend, false);
        const bool m = case_anisotropy_sampler(*backend, 1);
        // Under M2_ANTIALIASING the same suite runs against multisample
        // targets; the requested value is what the backend actually parsed.
        const bool n = case_multisample_antialiasing(
            *backend, antialiasing > 0 ? antialiasing : 1);
        const bool p = case_ssb_mask_wrap(*backend);
        const bool q = case_masked_uv_lod_seam(*backend);
        const bool t = case_rgb555_dither(*backend);
        const bool u = case_texture_cache_recycle(*backend);
        ok = shadow_alias && general_dsb && general_tab && opaque && a && b && c && c_forward && c_constant && d && tab_alpha && e && f && g && h && i && k && l && m && n && p && q && s && t && u;
        }
    }

    backend->shutdown();
    SDL_DestroyWindow(window);
    return ok;
}

// Negative control, executed rather than asserted: the software backend the
// focused software gate uses is constructed and put through the very predicate
// that admits the GL backends. It must be rejected, otherwise the identity
// assertions in this file would not be proving anything.
bool negative_control_rejects_software_backend()
{
    m2host::SoftCaptureBackend soft(kWidth, kHeight, {});
    if (!expect(soft.init(nullptr), "control: SoftCaptureBackend initialises"))
        return false;
    if (!case_shadow_color_z_alias(soft) || !case_general_tab(soft) || !case_general_dsb(soft) || !case_framebuffer_metadata(soft) || !case_rgb555_dither(soft)) return false;
    if (!expect(!is_real_gl_backend(soft),
                "control: SoftCaptureBackend must be rejected by the GL "
                "identity predicate, but it was accepted (name=%s)",
                soft.name()))
        return false;
    std::puts("m2_gl_backend_parity: negative control OK "
              "(SoftCaptureBackend rejected)");
    return true;
}

}  // namespace

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("m2_gl_backend_parity: SKIP: no video: %s\n",
                    SDL_GetError());
        return 3;
    }
    // The same context request the shipped host makes (main.cpp:897-906): a
    // core profile with a stencil, which the clean renderer's shadow path and
    // the addressed target pool both require.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    // Same request as m2host (main.cpp): the backends need only GL 3.3 core.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

    // Probe for that context first. Without one (e.g. the GL 1.1 software
    // driver on a Windows CI runner) there is nothing to compare, so this is
    // a skip; with one, a backend init() failure below stays a failure.
    {
        SDL_Window* probe = SDL_CreateWindow(
            "m2-gl-parity-probe", kWidth, kHeight,
            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        SDL_GLContext context = probe ? SDL_GL_CreateContext(probe) : nullptr;
        // Without WGL_ARB_create_context, SDL on Windows reports "GL 3.x is
        // not supported" but still returns a legacy context, so the version
        // the context actually got decides.
        const GLubyte* version = context ? glGetString(GL_VERSION) : nullptr;
        const int major =
            version ? std::atoi(reinterpret_cast<const char*>(version)) : 0;
        if (major < 3) {
            std::printf("m2_gl_backend_parity: SKIP: no GL 3 context: %s\n",
                        version ? reinterpret_cast<const char*>(version)
                                : SDL_GetError());
            if (context) SDL_GL_DestroyContext(context);
            if (probe) SDL_DestroyWindow(probe);
            SDL_Quit();
            return 3;
        }
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(probe);
    }

    // Build a mapping with a distinctive pixel, then let both production GL
    // backends load it through the same opt-in manifest path as the host.
    const auto fixture_dir = std::filesystem::temp_directory_path() /
        ("m2-gl-replacement-" + std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directories(fixture_dir);
    const auto image_path = fixture_dir / "replacement.png";
    const auto manifest_path = fixture_dir / "manifest.txt";
    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        std::fprintf(stderr, "m2_gl_backend_parity: fixture surface: %s\n",
                     SDL_GetError());
        SDL_Quit();
        return 1;
    }
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const uint32_t red = pack_rgba(220, 0, 0, 255);
            std::memcpy(static_cast<uint8_t*>(surface->pixels) +
                            static_cast<size_t>(y) * surface->pitch + x * 4,
                        &red, 4);
        }
    const bool saved = SDL_SavePNG(surface, image_path.string().c_str());
    SDL_DestroySurface(surface);
    if (!saved) {
        std::fprintf(stderr, "m2_gl_backend_parity: fixture PNG: %s\n",
                     SDL_GetError());
        SDL_Quit();
        return 1;
    }
    const uint32_t original[4] = {0, 0, 0, 0};
    std::ofstream manifest(manifest_path);
    manifest << "2 2 0x" << std::hex
             << m2host::texture_rgba_hash(original, 4)
             << " \"replacement.png\"\n";
    manifest.close();
    setenv("M2_TEXTURE_REPLACEMENTS", manifest_path.string().c_str(), 1);
    unsetenv("M2_TEXTURE_FILTER");
    unsetenv("M2_ANISOTROPY");
    unsetenv("M2_ANTIALIASING");

    // Each backend gets its own window: `init()` creates and owns a context, so
    // two live contexts on one window would interleave their GL state.
    bool ok = run_backend("gl_backend", &m2host::make_gl_backend);
    ok = run_backend("clean_renderer", &m2host::make_clean_renderer) && ok;
    setenv("M2_TEXTURE_FILTER", "linear", 1);
    ok = run_backend("gl_backend_linear", &m2host::make_gl_backend,
                     true) && ok;
    ok = run_backend("clean_renderer_linear", &m2host::make_clean_renderer,
                     true) && ok;
    unsetenv("M2_TEXTURE_FILTER");
    // The request is read at backend init, so each anisotropy run sets the
    // variable before constructing its backend.
    setenv("M2_ANISOTROPY", "4", 1);
    ok = run_backend("gl_backend_aniso", &m2host::make_gl_backend, false,
                     4) && ok;
    ok = run_backend("clean_renderer_aniso", &m2host::make_clean_renderer,
                     false, 4) && ok;
    unsetenv("M2_ANISOTROPY");
    // The full suite also runs against multisample targets: every readback,
    // windowed read, snapshot, copy and present must resolve correctly.
    setenv("M2_ANTIALIASING", "4", 1);
    ok = run_backend("gl_backend_msaa", &m2host::make_gl_backend, false, 0,
                     4) && ok;
    ok = run_backend("clean_renderer_msaa", &m2host::make_clean_renderer, false,
                     0, 4) && ok;
    unsetenv("M2_ANTIALIASING");
    ok = negative_control_rejects_software_backend() && ok;

    unsetenv("M2_TEXTURE_REPLACEMENTS");
    std::filesystem::remove_all(fixture_dir);

    SDL_Quit();
    if (g_failures == 0 && ok) {
        std::puts("m2_gl_backend_parity: OK");
        return 0;
    }
    std::fprintf(stderr, "m2_gl_backend_parity: %d failure(s)\n", g_failures);
    return 1;
}
