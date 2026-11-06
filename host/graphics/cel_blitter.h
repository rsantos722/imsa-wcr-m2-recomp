// cel_blitter.h — host rendering surface for the game's cel operations.
//
// M2 gstate/cel operations map onto textured quads. The recomp keeps the
// game's own `clt*` blitter code; those routines ultimately push cel rasters
// through this interface. GL 3.3-core-compatible pipeline, loaded dynamically
// via SDL to stay dependency-free.
//
// The same backend also draws the clean-GL 3D path fed by the asset pipeline
// (`mesh_*`), plus the small set of 2D primitives the HUD/game-flow needs
// (`fill_rect`, `scissor`). See docs/recompilation/rendering.md.
#pragma once
#include "clt_tab.h"
#include "clt_dsb.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

struct SDL_Window;

namespace m2host {

using CelId = uint32_t;
constexpr CelId kInvalidCel = 0;

// The guest's 640x480 display carries its picture in a 576x432 carve at (32,24)
// (the original's authored overscan margin). M2_CROP_OVERSCAN presents only the
// carve, scaled to fill the window.
constexpr int kDisplayWidth = 640, kDisplayHeight = 480;
constexpr int kCarveX = 32, kCarveY = 24, kCarveWidth = 576, kCarveHeight = 432;

// Projector pixel replication belongs at scanout, after TE writes and feedback
// reads. Rectangles are in host pixels and retain the target's submitted View.
struct CltScanoutRect {
    int x = 0, y = 0, source_width = 0, source_height = 0;
    int width = 0, height = 0;

    bool scales(int w, int h) const {
        return x >= 0 && y >= 0 && source_width > 0 && source_height > 0 &&
               width > 0 && height > 0 && source_width <= w - x &&
               source_height <= h - y && width <= w - x && height <= h - y &&
               (source_width != width || source_height != height);
    }
    // The part of a w x h target the guest actually shows: its viewport carve
    // (the game draws 576x432 inside a 640x480 display, leaving an authored
    // margin). Reports the carve's top-left and the size of its pre-Projector
    // source pixels, so a presenter can fill the window with the carve alone.
    // False when no carve is known or it already covers the whole target.
    bool crop_region(int w, int h, int& out_x, int& out_y, int& out_w,
                     int& out_h) const {
        if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
            width > w - x || height > h - y || (width == w && height == h))
            return false;
        out_x = x;
        out_y = y;
        out_w = scales(w, h) ? source_width : width;
        out_h = scales(w, h) ? source_height : height;
        return true;
    }
    void apply(uint32_t* pixels, int w, int h) const {
        if (!pixels || !scales(w, h)) return;
        std::vector<uint32_t> source(size_t(source_width) * source_height);
        for (int row = 0; row < source_height; ++row)
            std::copy_n(pixels + size_t(y + row) * w + x, source_width,
                        source.data() + size_t(row) * source_width);
        for (int row = 0; row < height; ++row) {
            const int sy = int((int64_t(row) * 2 + 1) * source_height /
                               (int64_t(height) * 2));
            for (int col = 0; col < width; ++col) {
                const int sx = int((int64_t(col) * 2 + 1) * source_width /
                                   (int64_t(width) * 2));
                pixels[size_t(y + row) * w + x + col] =
                    source[size_t(sy) * source_width + sx];
            }
        }
    }
};

// Opaque handle for an uploaded static mesh (positions + colors + u16 indices).
using MeshId = uint32_t;
constexpr MeshId kInvalidMesh = 0;

// One vertex emitted by the M2 Triangle Engine command-list interpreter.
// Coordinates are in the guest target's pixel space (origin top-left); UVs
// One vertex emitted by the M2 Triangle Engine command-list interpreter.
// Coordinates are in the guest target's pixel space (origin top-left); UVs
// are normalized by the bridge after it decodes the guest texture dimensions.
struct CltVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
    float w = 1.f;
    float one_over_w = 0.f;
    // Independent mask coordinates for TE forms where a texture supplies
    // SSB/discard while DBSRCBASEADDR supplies the fragment colour. For a
    // DB-source-backed draw, the bridge may retain an unbounded normalized
    // linear source coordinate here; the backend resolves its row carry.
    float mask_u = 0.f;
    float mask_v = 0.f;
};

// Raw DBZCNTL relation bits from the M2 destination blender.  The register
// has independent pixel-output and Z-update bits for NEW Z <, ==, and > the
// stored Z; do not collapse it to a single API depth function.
constexpr uint32_t kCltDbzPixelLt = 0x01u;
constexpr uint32_t kCltDbzZLt = 0x02u;
constexpr uint32_t kCltDbzPixelEq = 0x04u;
constexpr uint32_t kCltDbzZEq = 0x08u;
constexpr uint32_t kCltDbzPixelGt = 0x10u;
constexpr uint32_t kCltDbzZGt = 0x20u;
constexpr uint32_t kCltDbzAll = 0x3fu;
constexpr uint32_t kCltDbzPixelMask = kCltDbzPixelLt | kCltDbzPixelEq |
                                       kCltDbzPixelGt;
constexpr uint32_t kCltDbzZUpdateMask = kCltDbzZLt | kCltDbzZEq |
                                         kCltDbzZGt;

enum class CltDepthRelation { smaller, equal, greater };

inline CltDepthRelation clt_depth_relation(float incoming, float stored)
{
    // DBZCNTL is a three-way relation on the guest Z values.  The host must
    // not widen the equality class: a later W quantizer may define a
    // coarser hardware relation, but an epsilon here makes the software
    // backend disagree with exact GL depth comparisons for adjacent floats.
    if (incoming < stored) return CltDepthRelation::smaller;
    if (incoming > stored) return CltDepthRelation::greater;
    return CltDepthRelation::equal;
}

// Backend-independent comparison vocabulary for a depth value that increases
// monotonically with the guest reciprocal-W value.  The guest's "smaller Z"
// relation therefore maps to a backend greater-than comparison.  Keeping this
// mapping here prevents CleanGL from silently collapsing DBZCNTL to LEQUAL.
enum class CltBackendDepthCompare : uint8_t {
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

inline CltBackendDepthCompare clt_backend_depth_compare_for_relations(
    uint32_t relation_mask, uint32_t smaller_bit, uint32_t equal_bit,
    uint32_t greater_bit)
{
    const uint32_t relation_bits =
        ((relation_mask & smaller_bit) ? 0x1u : 0u) |
        ((relation_mask & equal_bit) ? 0x2u : 0u) |
        ((relation_mask & greater_bit) ? 0x4u : 0u);
    switch (relation_bits) {
    case 0u: return CltBackendDepthCompare::never;
    case 0x1u: return CltBackendDepthCompare::greater;
    case 0x2u: return CltBackendDepthCompare::equal;
    case 0x4u: return CltBackendDepthCompare::less;
    case 0x3u: return CltBackendDepthCompare::greater_equal;
    case 0x6u: return CltBackendDepthCompare::less_equal;
    case 0x5u:
        return CltBackendDepthCompare::not_equal;
    case 0x7u: return CltBackendDepthCompare::always;
    }
    return CltBackendDepthCompare::never;
}

inline CltBackendDepthCompare clt_backend_depth_compare_for_dbz(
    uint32_t depth_func)
{
    return clt_backend_depth_compare_for_relations(
        depth_func & kCltDbzPixelMask, kCltDbzPixelLt, kCltDbzPixelEq,
        kCltDbzPixelGt);
}

// GL depth writes use a second pass because the fixed-function depth mask is
// draw-wide while DBZCNTL carries an independent per-relation Z-update mask.
// Keep its comparison translation beside the pixel-output translation so both
// GL backends and the software reference consume the same guest vocabulary.
inline CltBackendDepthCompare clt_backend_depth_compare_for_dbz_z_update(
    uint32_t depth_func)
{
    return clt_backend_depth_compare_for_relations(
        depth_func & kCltDbzZUpdateMask, kCltDbzZLt, kCltDbzZEq,
        kCltDbzZGt);
}

inline bool clt_dbz_z_update_enabled(uint32_t depth_func, bool test_enabled,
                                     bool write_enabled)
{
    // ZBUFEN gates the whole Z path (the M2 destination-blender contract does
    // no Z compare or write when it is clear); ZOUTEN and the independent
    // relation mask then gate the update half inside that path.
    return test_enabled && write_enabled &&
           (depth_func & kCltDbzZUpdateMask) != 0u;
}

// Returns true when a single OpenGL depth pass with depth writes enabled is
// mathematically equivalent to evaluating the M2 destination blender's
// independent pixel-output and Z-update relation masks in two separate passes.
//
// Equivalence holds when:
// 1. No per-texel SSB coefficient split is active (which requires distinct blend
//    states across two passes).
// 2. Both depth test and Z update are enabled.
// 3. Either:
//    a) The pixel-output and Z-update comparison relations are identical, or
//    b) Equal-Z invariance holds: color permits equal-Z while Z-update rejects
//       it (e.g. 0x7: pixel greater_equal, Z-update greater). In that case,
//       writing depth on equal Z writes the identical stored depth value back
//       into the depth buffer, leaving depth storage unchanged while letting a
//       single combined pass preserve primitive order across batched geometry.
inline bool clt_can_combine_depth_passes(
    uint32_t depth_func, bool test_enabled, bool z_update_enabled,
    bool ssb_entry_split, CltBackendDepthCompare* out_compare = nullptr)
{
    if (ssb_entry_split) return false;
    if (!test_enabled || !z_update_enabled) return false;
    const CltBackendDepthCompare color_cmp =
        clt_backend_depth_compare_for_dbz(depth_func);
    const CltBackendDepthCompare depth_cmp =
        clt_backend_depth_compare_for_dbz_z_update(depth_func);
    if (color_cmp == depth_cmp) {
        if (out_compare) *out_compare = color_cmp;
        return true;
    }
    if (color_cmp == CltBackendDepthCompare::greater_equal &&
        depth_cmp == CltBackendDepthCompare::greater) {
        if (out_compare) *out_compare = color_cmp;
        return true;
    }
    if (color_cmp == CltBackendDepthCompare::less_equal &&
        depth_cmp == CltBackendDepthCompare::less) {
        if (out_compare) *out_compare = color_cmp;
        return true;
    }
    return false;
}

// Backend-neutral evaluation of the vocabulary above, in the fixed-function
// sense the GL backends hand to glDepthFunc (`incoming`/`stored` are plane
// values).  The focused tests use it to pin the *direction* of the translation
// without a GL context -- the direction that let the race world disappear when
// only the clear value was wrong.
inline bool clt_backend_depth_passes(CltBackendDepthCompare compare,
                                     float incoming, float stored)
{
    switch (compare) {
    case CltBackendDepthCompare::never: return false;
    case CltBackendDepthCompare::less: return incoming < stored;
    case CltBackendDepthCompare::equal: return incoming == stored;
    case CltBackendDepthCompare::less_equal: return incoming <= stored;
    case CltBackendDepthCompare::greater: return incoming > stored;
    case CltBackendDepthCompare::not_equal: return incoming != stored;
    case CltBackendDepthCompare::greater_equal: return incoming >= stored;
    case CltBackendDepthCompare::always: return true;
    }
    return false;
}

// The guest's Triangle Engine Z surface is a 16-bit reciprocal-W plane, and the
// host depth plane holds exactly that quantity: the vertex path emits
// `dst.z = -1 + 2*clamp(one_over_w)` (nearer geometry = larger z), which the
// default depth range maps back to `clamp(one_over_w)`.  Two plane values
// therefore belong to the same contract as the comparison vocabulary above:
//
//  * the *far* value an unwritten Z cell holds is `0.0` (`one_over_w -> 0`).  A
//    backend must start its depth plane there, because the guest's own TE
//    clear-Z request (`UpdateClearZBCmdList`) may arrive after a frame's first
//    draw;
//  * the guest authors that clear as a fill word in its `TEZBufferInfo`, which
//    the Portfolio driver copies verbatim into the generated clear command
//    list, so the host reads the word as the IEEE float it is authored as.
//
// Clearing to anything else inverts the plane: `1.0` is the *nearest* value, so
// every DBZCNTL mask that actually compares would reject the geometry that
// should be closest to the viewer.
inline constexpr float kCltTeZFarDepth = 0.0f;

inline float clt_te_z_fill_depth(uint32_t z_fill_word)
{
    const float raw = std::bit_cast<float>(z_fill_word);
    if (!std::isfinite(raw)) return kCltTeZFarDepth;
    return std::clamp(raw, 0.f, 1.f);
}

// Host `frame_begin` clear plan for guest-addressed CLT storage.
//
// The two planes have different owners and must not share one policy:
//
//  * Color is guest-addressed VRAM.  The guest maintains only the part of a
//    surface its own command lists touch, so a host-side clear at every frame
//    boundary destroys pixels the guest never rewrote.  A persistent target
//    therefore survives host frames until the guest's own TE clear
//    (`clt_clear_color`) or a newly allocated surface's first-use fill.
//  * Depth is not per-target.  Every guest color target shares the one depth
//    attachment the host allocates, and the guest keeps one Z surface address
//    for both of its alternating color buffers, so a plane left alone would
//    make each frame's world depth-test against the previous frame's.  The
//    host must reset depth (and its stencil companion) at every frame
//    boundary; only `clt_clear_z` can install the guest's authored fill.
struct CltFrameClearPlan {
    bool color = true;
    bool depth = true;
};

inline CltFrameClearPlan clt_frame_clear_plan(bool persist_targets,
                                             bool initialized)
{
    CltFrameClearPlan plan;
    if (!persist_targets || !initialized) return plan;
    plan.color = false;
    return plan;
}

// A published DISPLAYFRAMEBUFFER can become ready before its enclosing guest
// application frame completes. Its queued lists still need the host depth
// boundary before they execute; a completed-frame-only check skips that clear.
inline bool clt_interactive_frame_begin_needed(bool completed_work,
                                               bool display_work)
{
    return completed_work || display_work;
}

// TEFrameBufferInfo carries four guest-authored IEEE words.  The released
// driver's clear list feeds them directly to shaded primary-color vertices;
// keep the same finite normalized color contract at every backend boundary.
inline float clt_te_color_component(uint32_t color_word)
{
    const float raw = std::bit_cast<float>(color_word);
    if (!std::isfinite(raw)) return 0.f;
    return std::clamp(raw, 0.f, 1.f);
}

inline bool clt_dbz_allows(uint32_t depth_func, float incoming, float stored,
                           bool z_update)
{
    if (depth_func == kCltDbzAll) return true;
    const uint32_t lt = z_update ? kCltDbzZLt : kCltDbzPixelLt;
    const uint32_t eq = z_update ? kCltDbzZEq : kCltDbzPixelEq;
    const uint32_t gt = z_update ? kCltDbzZGt : kCltDbzPixelGt;
    switch (clt_depth_relation(incoming, stored)) {
    case CltDepthRelation::smaller: return (depth_func & lt) != 0;
    case CltDepthRelation::equal: return (depth_func & eq) != 0;
    case CltDepthRelation::greater: return (depth_func & gt) != 0;
    }
    return false;
}

inline bool clt_depth_pixel_pass(uint32_t depth_func, float incoming,
                                 float stored, bool test_enabled)
{
    return !test_enabled ||
           clt_dbz_allows(depth_func, incoming, stored, false);
}

inline bool clt_depth_write_pass(uint32_t depth_func, float incoming,
                                 float stored, bool test_enabled,
                                 bool write_enabled)
{
    return write_enabled &&
           (!test_enabled ||
            clt_dbz_allows(depth_func, incoming, stored, true));
}

// Depth buffer / Z-buffer rasterization state for CLT draws.
struct CltDepth {
    // Aligned RGB555 destination aliases the bound Z bitmap, with zero SSB
    // and all RGB channels writable. Used for the authored near-cell fill.
    bool color_target_is_z = false;
    bool test_enabled = false;
    bool write_enabled = false;
    uint32_t depth_func = 0;  // raw DBZCNTL; 0x3f permits all relations
    // Recovered TE Z region. The Portfolio driver binds it from the Z bitmap
    // (devices/triangleengine/hw.c:SetTEZBuffer writes DBZCLIP as the bound
    // bitmap's Width/Height) and DBZOFFSET supplies signed 12-bit X/Y
    // offsets. Bounds are in the same target-pixel space as CltVertex.x/y;
    // the interpreter folds the signed DBZOFFSET and View origin into them.
    // A pixel outside the half-open region [min,max) is Z-clipped: its Z
    // test and Z write are skipped while its color still writes.
    // z_clipped_discard (DBDISCARDCONTROL.ZCLIPPED) additionally discards the
    // pixel entirely. The region only applies while test_enabled
    // (DBUSERCONTROL.ZBUFEN) is set; an empty region clips every pixel,
    // matching an unwritten DBZCLIP on the MAME/vendor reset model.
    int32_t z_region_min_x = 0;
    int32_t z_region_max_x = 0;
    int32_t z_region_min_y = 0;
    int32_t z_region_max_y = 0;
    bool z_clipped_discard = false;
};

// M_SetupShadowVolume draws untextured (31/64,1,1) into BMTYPE_16_ZBUFFER.
// RGB555 packs this as 0x3fff: range zero, maximum 14-bit significand in
// the documented TE block-fix model. Recover this bounded near-cell operation
// without quantizing ordinary scene depth or guessing arbitrary aliased cells.
inline constexpr float kCltNearColorZ = 16383.f / 16384.f;
inline bool clt_near_color_z_fill(const CltDepth& depth,
                                 const CltVertex* vertices, uint32_t count,
                                 bool textured, bool blend_enabled)
{
    if (!depth.color_target_is_z || textured || blend_enabled || !vertices || !count ||
        clt_dbz_z_update_enabled(depth.depth_func, depth.test_enabled, depth.write_enabled))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& v = vertices[i];
        if (v.r != 31.f / 64.f || v.g != 1.f || v.b != 1.f || v.a != 1.f)
            return false;
    }
    return true;
}

// True when the bound Z region governs this draw (ZBUFEN). Runs without Z
// buffering must not apply the region: MAME's destination_blend leaves
// zclip false when ZBUFEN is clear.
inline bool clt_z_region_active(const CltDepth& depth)
{
    return depth.test_enabled;
}

inline bool clt_z_region_contains(const CltDepth& depth, float x, float y)
{
    return x >= static_cast<float>(depth.z_region_min_x) &&
           x < static_cast<float>(depth.z_region_max_x) &&
           y >= static_cast<float>(depth.z_region_min_y) &&
           y < static_cast<float>(depth.z_region_max_y);
}

// Pixel (x,y) lies outside the bound Z region: skip the Z test and Z write.
inline bool clt_z_clipped(const CltDepth& depth, float x, float y)
{
    return clt_z_region_active(depth) &&
           !clt_z_region_contains(depth, x, y);
}

// Outside-region pixel that the guest's DBDISCARDCONTROL.ZCLIPPED bit drops
// before any color write.
inline bool clt_z_clipped_discard(const CltDepth& depth, float x, float y)
{
    return depth.z_clipped_discard && clt_z_clipped(depth, x, y);
}

// Axis-aligned top-left-origin rectangle in a backend's draw space.
struct CltClipRect {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

inline bool clt_clip_rect_empty(const CltClipRect& rect)
{
    return rect.w <= 0.f || rect.h <= 0.f;
}

inline CltClipRect clt_clip_rect_intersection(const CltClipRect& a,
                                              const CltClipRect& b)
{
    const float x0 = a.x > b.x ? a.x : b.x;
    const float y0 = a.y > b.y ? a.y : b.y;
    const float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    const float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return {x0, y0, x1 > x0 ? x1 - x0 : 0.f, y1 > y0 ? y1 - y0 : 0.f};
}

// Decompose clip \ region into up to four disjoint rectangles (top, bottom,
// and the middle-row left/right bands). Returns the rect count. The GL
// backends use this to keep the TE per-pixel Z rule — outside-region pixels
// bypass the Z test and Z write — within their rectangular scissor contract.
inline int clt_clip_rect_subtract(const CltClipRect& clip,
                                  const CltClipRect& region,
                                  CltClipRect out[4])
{
    if (clip.w <= 0.f || clip.h <= 0.f) return 0;
    int count = 0;
    const float cx0 = clip.x;
    const float cy0 = clip.y;
    const float cx1 = clip.x + clip.w;
    const float cy1 = clip.y + clip.h;
    const float rx0 = region.x;
    const float ry0 = region.y;
    const float rx1 = region.x + region.w;
    const float ry1 = region.y + region.h;
    // Rows above the region.
    const float top_end = ry0 < cy1 ? ry0 : cy1;
    if (cy0 < top_end) out[count++] = {cx0, cy0, clip.w, top_end - cy0};
    // Rows below the region.
    const float bottom_start = ry1 > cy0 ? ry1 : cy0;
    if (bottom_start < cy1)
        out[count++] = {cx0, bottom_start, clip.w, cy1 - bottom_start};
    // Rows crossing the region: left and right bands.
    const float my0 = cy0 > ry0 ? cy0 : ry0;
    const float my1 = cy1 < ry1 ? cy1 : ry1;
    if (my0 < my1) {
        const float left_end = rx0 < cx1 ? rx0 : cx1;
        if (cx0 < left_end)
            out[count++] = {cx0, my0, left_end - cx0, my1 - my0};
        const float right_start = rx1 > cx0 ? rx1 : cx0;
        if (right_start < cx1)
            out[count++] = {right_start, my0, cx1 - right_start, my1 - my0};
    }
    return count;
}

// Texture lookup state decoded from the guest TXTADDRCNTL/TXTUVMASK
// registers.  These are draw-time properties: a cel may be reused by several
// CLT materials, so upload-time filtering cannot represent the guest state.
// The enum values intentionally match the M2 register encodings documented by
// the vendor CLT headers (point=0, linear=1, bilinear=2, trilinear=3).
enum class CltTextureFilter : uint8_t {
    point = 0,
    linear = 1,
    bilinear = 2,
    trilinear = 3,
};

// TXTUVMASK is a 10-bit per-axis field.  Mask 0x3ff is the register's full
// scale (period 1024), not "no mask": the toolkit describes the register as
// the address mask -- "Bits set to '0' in this register will be masked off
// during address calculation" -- so the period is always mask+1 and a period
// that reaches the resident extent is indistinguishable from an unmasked
// axis.
constexpr uint32_t kCltUvMaskFullScale = 0x3ffu;
constexpr uint32_t kCltUvFullPeriod = kCltUvMaskFullScale + 1u;

struct CltTextureSampling {
    CltTextureFilter min_filter = CltTextureFilter::point;
    CltTextureFilter inter_filter = CltTextureFilter::point;
    CltTextureFilter mag_filter = CltTextureFilter::point;
    uint8_t lod_max = 0;
    bool wrap_u = false;
    bool wrap_v = false;
    uint32_t raw_uv_mask = 0;
    // TXTUVMASK address periods in base-LOD texels (period = mask + 1).  The
    // mask is applied to the integer texel coordinate before the address is
    // formed, which the vendor `PageCLT_SetUV()` reproduces exactly for a
    // tiled axis: it writes mask = largest-LOD dimension - 1, i.e. the period
    // *is* that axis's base extent, while a clamped axis keeps 0x3ff.  A
    // period below the resident extent therefore repeats a sub-range of the
    // texture; it must not clamp to the edge.  These are draw-time state, like
    // the filters, because one cel can be sampled by several CLT materials.
    uint32_t u_period = kCltUvFullPeriod;
    uint32_t v_period = kCltUvFullPeriod;
};

// M2_ANISOTROPY: optional presentation request for anisotropic filtering on
// primary decoded CLT textures. The value is the sampler level (the backends
// clamp it to the device maximum); unset, invalid, or 1 keeps the guest's own
// filter state, and the GL backends refuse and log when the extension is
// absent rather than reporting success. Anisotropy is only meaningful under
// linear minification, so a granted request also engages the same linear
// sampling override as M2_TEXTURE_FILTER=linear; SSB masks and
// framebuffer-feedback sources keep their guest sampling.
inline int clt_anisotropy_request()
{
    const char* value = std::getenv("M2_ANISOTROPY");
    if (!value || !value[0]) return 1;
    char* end = nullptr;
    const long level = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return 1;
    if (level < 2) return 1;
    return level > 16 ? 16 : static_cast<int>(level);
}

// M2_ANTIALIASING: optional presentation MSAA request for the GL backends'
// addressed CLT color targets. 2, 4, or 8 samples; the backends clamp to the
// context's GL_MAX_SAMPLES and refuse (single-sample rendering plus a log
// line) when the device cannot serve at least 2, so the request is never
// reported as granted when it is not. Unset, invalid, or 1 keeps the
// single-sample default. GL backends only: the software capture backend has no
// multisample storage.
inline int clt_antialiasing_request()
{
    const char* value = std::getenv("M2_ANTIALIASING");
    if (!value || !value[0]) return 1;
    char* end = nullptr;
    const long samples = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return 1;
    if (samples < 2) return 1;
    return samples > 8 ? 8 : static_cast<int>(samples);
}

// Pixel tests performed after the texture/TAB stage. The SSB channel is kept
// in a separate cel because RGBA alpha is independently observable by the
// destination blender. RGB0 is the post-texturing all-zero RGB test from the
// M2 DBDISCARDCONTROL register.
struct CltDiscard {
    bool ssb_zero = false;
    bool alpha_zero = false;
    bool rgb_zero = false;
    CelId ssb_mask = kInvalidCel;
    bool separate_ssb_uv = false;
};

// DBSSBDSBCNTL fields from the M2 destination blender.  The checked-in
// Mercury SDK (`cltmacros.h`) declares the register in raw host-word form:
//
//   FV_DBSSBDSBCNTL_DSBCONST_MASK  = PPCBIT(29)    -> 0x4
//   FV_DBSSBDSBCNTL_DSBSELECT_MASK = PPCMASK(3,31) -> 0x3
//
// `PPCBIT(n)`/`PPCMASK(d,n)` in cltmacros.h are the raw word bit `(31 - n)`,
// the same convention the CLT register parser (RC_WRITE_REGISTER 0x10000000,
// RC_SET_REGISTER 0x1000, RC_CLEAR_REGISTER 0x1800) and the DBZCNTL /
// DBUSERCONTROL / DBDISCARDCONTROL field constants below already use.  The
// register word is therefore the SDK word; do not re-apply PPC bit numbering
// to it.  Selector 3 is reserved and must not be silently interpreted as one
// of the defined forms.
enum class CltDsbSelect : uint8_t {
    object_ssb = 0,
    constant_ssb = 1,
    source_ssb = 2,
    reserved = 3,
};

constexpr uint32_t kCltDsbConstantMask = 0x4u;  // PPCBIT(29)
constexpr uint32_t kCltDsbSelectMask = 0x3u;    // PPCMASK(3, 31)

struct CltDsbState {
    uint32_t raw = 0;
    CltDsbSelect select = CltDsbSelect::constant_ssb;
    bool constant = false;
    bool valid = true;
};

inline CltDsbState clt_decode_dsb(uint32_t raw)
{
    const uint32_t select = raw & kCltDsbSelectMask;
    CltDsbState state;
    state.raw = raw;
    state.select = static_cast<CltDsbSelect>(select);
    state.constant = (raw & kCltDsbConstantMask) != 0;
    state.valid = select != static_cast<uint32_t>(CltDsbSelect::reserved);
    return state;
}

// DBAMULTCNTL / DBBMULTCNTL / DBALUCNTL are the register form of the
// source-level destination-blender equation documented by the M2 Command List
// Toolkit, `Cout = ((MA * A) op (MB * B)) << shift`, and instantiated by the
// checked-in Mercury `M_DBInit`/`M_DBNoBlend`/`M_DBSpec`/`M_DBTrans`/
// `M_DBFog` macros.  cltmacros.h fixes the raw host-word field positions:
//
//   DBAMULTCNTL: AINPUTSELECT     PPCSHIFT(25) -> 6   (2 bits)
//                AMULTCOEFSELECT  PPCSHIFT(30) -> 1   (sparse mask 0x19)
//                AMULTCONSTCONTROL PPCSHIFT(29) -> 2  (2 bits)
//                AMULTRJUSTIFY    PPCBIT(31)   -> 0x1
//   DBBMULTCNTL: the same positions for the B side
//   DBALUCNTL:   ALUOPERATION     PPCSHIFT(28) -> 3   (5 bits)
//                FINALDIVIDE      PPCMASK(7,31) -> 0x7
//
// The multiplier coefficient field is deliberately sparse (bits 1, 4, 5) so
// that it does not collide with the constant-control bits (2, 3) or the
// right-justify bit (0); only the eight documented coefficient words can be
// produced by that mask.
constexpr uint32_t kCltDbInputSelectShift = 6u;
constexpr uint32_t kCltDbInputSelectMask = 0x3u << kCltDbInputSelectShift;
constexpr uint32_t kCltDbCoefSelectShift = 1u;
constexpr uint32_t kCltDbCoefSelectMask = 0x19u << kCltDbCoefSelectShift;
constexpr uint32_t kCltDbConstControlShift = 2u;
constexpr uint32_t kCltDbConstControlMask = 0x3u << kCltDbConstControlShift;
constexpr uint32_t kCltDbJustifyMask = 0x1u;
constexpr uint32_t kCltDbAluOpShift = 3u;
constexpr uint32_t kCltDbAluOpMask = 0x1fu << kCltDbAluOpShift;
constexpr uint32_t kCltDbFinalDivideMask = 0x7u;
// Every documented bit of the multiplier and ALU registers lives in the low
// byte; the SDK defines nothing above it.  A word with reserved bits set is
// not a documented form and must stay fail-closed rather than being decoded
// as the documented form its low byte happens to match.
constexpr uint32_t kCltDbMultiplierFieldMask = kCltDbJustifyMask |
                                               kCltDbCoefSelectMask |
                                               kCltDbConstControlMask |
                                               kCltDbInputSelectMask;
constexpr uint32_t kCltDbAluFieldMask = kCltDbFinalDivideMask | kCltDbAluOpMask;

// A and B are separate selector vocabularies: value 2 selects
// A=1-Cs(source) on the A side and 1-Ct(texture) on the B side, and the
// 0x18 coefficient is SRCCOLOR for A but TEXCOLOR for B.
enum class CltDbAInput : uint8_t {
    texture_color = 0,
    constant_color = 1,
    source_color_complement = 2,
    source_alpha = 3,
};

enum class CltDbBInput : uint8_t {
    source_color = 0,
    constant_color = 1,
    texture_color_complement = 2,
    source_alpha = 3,
};

// Coefficient words carry the complement flag in their low SDK bit.
enum class CltDbACoef : uint8_t {
    texture_alpha = 0x00,
    source_alpha = 0x08,
    constant = 0x10,
    source_color = 0x18,
    texture_alpha_complement = 0x01,
    source_alpha_complement = 0x09,
    constant_complement = 0x11,
    source_color_complement = 0x19,
};

enum class CltDbBCoef : uint8_t {
    texture_alpha = 0x00,
    source_alpha = 0x08,
    constant = 0x10,
    texture_color = 0x18,
    texture_alpha_complement = 0x01,
    source_alpha_complement = 0x09,
    constant_complement = 0x11,
    texture_color_complement = 0x19,
};

enum class CltDbConstControl : uint8_t {
    texture_ssb = 0,
    source_ssb = 1,
    reserved = 2,
};

// The coefficient words `CONST`/`CONSTCOMPLEMENT` (0x10/0x11) have no literal
// in the control register: they select the multiplier's own coefficient
// constant, which lives in a separate register pair. The checked-in Portfolio
// blitter states the mapping directly (`graphics/blitter.h`, `DBlendSnippet`):
//
//   dbl_txtCoefConst[2]  /* DBAMULTCONSTSSB0/1  0x805c/0x8060 */
//   dbl_multCoefConst[2] /* DBBMULTCONSTSSB0/1  0x8068/0x806c */
//
// and builds the value as `(mult << 16) | (mult << 8) | mult` from an 8-bit
// per-channel strength (`Blt_BlendBlitObject` in `blitter_utils/blend.c`), so
// white (0xffffff) is the identity coefficient 1.0 and the product reduces to
// the selected input unchanged. Both words of the pair carry the same value.
constexpr uint32_t kCltDbConstWhite = 0x00ffffffu;

// The coefficient registers carry one 8-bit weight per channel in the low
// three bytes, red in bits 16..23, green in 8..15 and blue in 0..7 -- the same
// packing `DBCONSTIN` uses (see `color_component` in `guest_clt.cpp`).
constexpr uint32_t clt_dsb_const_rgb(uint32_t raw)
{
    return raw & kCltDbConstWhite;
}

// One channel of a coefficient register as a 0..1 weight.  `shift` is 16, 8 or
// 0 for red, green and blue in the register's own packing.
inline float clt_dsb_const_weight(uint32_t raw, unsigned shift)
{
    return static_cast<float>((raw >> shift) & 0xffu) / 255.f;
}

inline bool clt_dsb_const_is_white(uint32_t raw)
{
    return clt_dsb_const_rgb(raw) == kCltDbConstWhite;
}

// ALU operation encodings.  `nota_and_b` (0x12) and `not_a_and_b` (0x17) keep
// the SDK's own distinct identifiers.
enum class CltDbAluOp : uint8_t {
    a_plus_bclamp = 0x00,
    a_plus_b = 0x02,
    a_minus_bclamp = 0x08,
    a_minus_b = 0x0a,
    b_minus_aclamp = 0x0c,
    b_minus_a = 0x0e,
    outputzero = 0x10,
    neither = 0x11,
    nota_and_b = 0x12,
    nota = 0x13,
    notb_and_a = 0x14,
    notb = 0x15,
    xor_op = 0x16,
    not_a_and_b = 0x17,
    a_and_b = 0x18,
    oneonequal = 0x19,
    b = 0x1a,
    nota_or_b = 0x1b,
    a = 0x1c,
    notb_or_a = 0x1d,
    a_or_b = 0x1e,
    outputone = 0x1f,
};

inline bool clt_dsb_alu_op_documented(uint32_t op)
{
    switch (op) {
    case 0x00u: case 0x02u: case 0x08u: case 0x0au: case 0x0cu: case 0x0eu:
    case 0x10u: case 0x11u: case 0x12u: case 0x13u: case 0x14u: case 0x15u:
    case 0x16u: case 0x17u: case 0x18u: case 0x19u: case 0x1au: case 0x1bu:
    case 0x1cu: case 0x1du: case 0x1eu: case 0x1fu:
        return true;
    default:
        return false;
    }
}

struct CltDsbAMultiplier {
    uint32_t raw = 0;
    CltDbAInput input = CltDbAInput::texture_color;
    CltDbACoef coef = CltDbACoef::texture_alpha;
    CltDbConstControl const_control = CltDbConstControl::texture_ssb;
    uint32_t const_control_raw = 0;
    bool justify = false;
    bool valid = true;
};

struct CltDsbBMultiplier {
    uint32_t raw = 0;
    CltDbBInput input = CltDbBInput::source_color;
    CltDbBCoef coef = CltDbBCoef::texture_alpha;
    CltDbConstControl const_control = CltDbConstControl::texture_ssb;
    uint32_t const_control_raw = 0;
    bool justify = false;
    bool valid = true;
};

struct CltDsbAlu {
    uint32_t raw = 0;
    CltDbAluOp op = CltDbAluOp::a_plus_bclamp;
    uint32_t op_raw = 0;
    bool op_documented = true;
    uint32_t final_divide = 0;
    bool final_divide_documented = true;
    bool reserved_clear = true;
};

// Destination-blender equation state carried to the backends.  It is decoded
// from the guest registers; it is not a host-authored blend decision.
struct CltDsbEquation {
    CltDsbAMultiplier a{};
    CltDsbBMultiplier b{};
    CltDsbAlu alu{};
};

inline CltDsbAMultiplier clt_decode_dsb_a_multiplier(uint32_t raw)
{
    CltDsbAMultiplier state;
    state.raw = raw;
    state.input = static_cast<CltDbAInput>(
        (raw & kCltDbInputSelectMask) >> kCltDbInputSelectShift);
    state.coef = static_cast<CltDbACoef>(
        (raw & kCltDbCoefSelectMask) >> kCltDbCoefSelectShift);
    state.const_control_raw =
        (raw & kCltDbConstControlMask) >> kCltDbConstControlShift;
    state.const_control = static_cast<CltDbConstControl>(state.const_control_raw);
    state.justify = (raw & kCltDbJustifyMask) != 0;
    state.valid = state.const_control_raw <= 1u &&
                 (raw & ~kCltDbMultiplierFieldMask) == 0u;
    return state;
}

inline CltDsbBMultiplier clt_decode_dsb_b_multiplier(uint32_t raw)
{
    CltDsbBMultiplier state;
    state.raw = raw;
    state.input = static_cast<CltDbBInput>(
        (raw & kCltDbInputSelectMask) >> kCltDbInputSelectShift);
    state.coef = static_cast<CltDbBCoef>(
        (raw & kCltDbCoefSelectMask) >> kCltDbCoefSelectShift);
    state.const_control_raw =
        (raw & kCltDbConstControlMask) >> kCltDbConstControlShift;
    state.const_control = static_cast<CltDbConstControl>(state.const_control_raw);
    state.justify = (raw & kCltDbJustifyMask) != 0;
    state.valid = state.const_control_raw <= 1u &&
                 (raw & ~kCltDbMultiplierFieldMask) == 0u;
    return state;
}

inline CltDsbAlu clt_decode_dsb_alu(uint32_t raw)
{
    CltDsbAlu state;
    state.raw = raw;
    state.op_raw = (raw & kCltDbAluOpMask) >> kCltDbAluOpShift;
    state.op = static_cast<CltDbAluOp>(state.op_raw);
    state.op_documented = clt_dsb_alu_op_documented(state.op_raw);
    state.final_divide = raw & kCltDbFinalDivideMask;
    state.final_divide_documented = state.final_divide <= 2u;
    state.reserved_clear = (raw & ~kCltDbAluFieldMask) == 0u;
    return state;
}

// The checked-in Mercury `M_DBSpec` macro writes A = TEXCOLOR * CONST,
// B = CONSTCOLOR * TEXALPHA and A+BCLAMP with no final divide while SRCEN
// stays clear, so the destination blender composites the texture-stage result
// internally instead of blending against the framebuffer.  This predicate is
// that equation expressed in the decoded register vocabulary; it is not a
// newly invented blend mode.
inline bool clt_dsb_is_specular_composite(const CltDsbAMultiplier& a,
                                          const CltDsbBMultiplier& b,
                                          const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::texture_color &&
           a.coef == CltDbACoef::constant &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::constant_color &&
           b.coef == CltDbBCoef::texture_alpha &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented && alu.op == CltDbAluOp::a_plus_bclamp &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The checked-in Mercury `M_DBFog` macro (cltmacromacros.h) writes
// DBCONSTIN=fogcolor, A = TEXCOLOR * TEXALPHA, B = CONSTCOLOR * (1 - TEXALPHA)
// and A_PLUS_BCLAMP with no final divide while SRCEN stays clear (BLENDEN is
// set).  The destination blender composites the texture-stage result with the
// fog constant internally instead of blending against the framebuffer.  The
// per-vertex fog factor computed by `M_LightFog` rides the texture-stage alpha
// (PRIMALPHA), so the write is `TEXCOLOR * a + fogcolor * (1 - a)` -- the
// distance fog lerp.  This predicate is that equation expressed in the decoded
// register vocabulary; it is not a newly invented blend mode.  It is distinct
// from `clt_dsb_is_specular_composite` (A coefficient CONST vs TEXALPHA, B
// coefficient TEXALPHA vs TEXALPHACOMPLEMENT) and from `clt_dsb_is_source_over`
// (B input CONSTCOLOR vs SRCCOLOR, and source_over additionally requires SRCEN).
inline bool clt_dsb_is_fog_composite(const CltDsbAMultiplier& a,
                                     const CltDsbBMultiplier& b,
                                     const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::texture_color &&
           a.coef == CltDbACoef::texture_alpha &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::constant_color &&
           b.coef == CltDbBCoef::texture_alpha_complement &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented && alu.op == CltDbAluOp::a_plus_bclamp &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The guest's source-over setup writes A=TEXCOLOR*TEXALPHA,
// B=SRCCOLOR*(1-TEXALPHA), A_PLUS_BCLAMP with SRCEN set. Recognize that
// equation explicitly so an undecoded equation can be identified separately
// from the guest's deliberate transparency.
inline bool clt_dsb_is_source_over(const CltDsbAMultiplier& a,
                                   const CltDsbBMultiplier& b,
                                   const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::texture_color &&
           a.coef == CltDbACoef::texture_alpha &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::source_color &&
           b.coef == CltDbBCoef::texture_alpha_complement &&
           b.const_control == CltDbConstControl::texture_ssb &&
           // A convex source-over sum stays in range, so A_PLUS_B and
           // A_PLUS_BCLAMP agree (the radar corners use A_PLUS_B).
           alu.op_documented &&
           (alu.op == CltDbAluOp::a_plus_bclamp ||
            alu.op == CltDbAluOp::a_plus_b) &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The constant-color read-modify-write form the guest authors in
// `TextDestBlendInit` and `UI_DrawHighScores_nameEntry`: with SRCEN set (a
// source frame buffer is bound and armed) the guest writes
//
//   DBAMULTCNTL = 0x40  -> A = CONSTCOLOR * TEXALPHA
//   DBBMULTCNTL = 0x02  -> B = SRCCOLOR  * (1 - TEXALPHA)
//   DBALUCNTL   = 0x10  -> A_PLUS_B, no final divide
//
// so the destination-blender output is the guest's own DBCONSTIN color
// interpolated with the source frame buffer pixel by the texture-blender
// alpha.  It is the M_DBTrans equation (`M_DBTrans` uses the identical B word
// and the same `DBUSERCONTROL` BLENDEN|SRCEN gate) with the constant in the A
// slot instead of the texture-blender output, which is why the production
// source-over default loses the guest's text/UI color for this form.
//
// Both documented adds are accepted: `TextDestBlendInit` and
// `UI_DrawHighScores_nameEntry` write A_PLUS_B (word 0x10), while the vendor
// `M_DBTrans` macro authors A_PLUS_BCLAMP (word 0x0) and the shooter2d
// background list never writes DBALUCNTL at all, running with the font
// folio's authored 0x0.  The weights are w and 1 - w on inputs normalized to
// 0..1, so the sum is convex and the clamp is a no-op: both words evaluate
// the guest's equation exactly (the same reading as `clt_dsb_is_source_over`).
inline bool clt_dsb_is_constant_alpha_blend(const CltDsbAMultiplier& a,
                                            const CltDsbBMultiplier& b,
                                            const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::constant_color &&
           a.coef == CltDbACoef::texture_alpha &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::source_color &&
           b.coef == CltDbBCoef::texture_alpha_complement &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented &&
           (alu.op == CltDbAluOp::a_plus_bclamp ||
            alu.op == CltDbAluOp::a_plus_b) &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The B-only read-modify-write form `FN_carshadow_initshadows__0c860` authors:
//
//   DBBMULTCNTL = 0x32  -> B = SRCCOLOR * (1 - TEXCOLOR)
//   DBALUCNTL   = 0xd0  -> ALU operation B, no final divide
//
// The ALU `B` operation discards the A product, so the guest's
// register-carried A word cannot affect the result; only its documented field
// layout is required.  By the same reading as `clt_dsb_is_constant_alpha_blend`
// (Mercury guide: the destination blender's inputs are the texture-blender
// output, a source frame buffer and a constant), `SRCCOLOR` is the bound
// source frame buffer pixel and `TEXCOLOR` is the texture-blender output, so
// the written value is the frame-buffer pixel multiplied by the inverted
// texture color -- the car-shadow darkening pass.  A draw whose source input
// is not armed (`SRCEN` clear) has no defined `SRCCOLOR` and stays fail-closed.
inline bool clt_dsb_is_inverse_texture_multiply(const CltDsbAMultiplier& a,
                                                const CltDsbBMultiplier& b,
                                                const CltDsbAlu& alu)
{
    return a.valid && b.valid && !b.justify && alu.reserved_clear &&
           b.input == CltDbBInput::source_color &&
           b.coef == CltDbBCoef::texture_color_complement &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented && alu.op == CltDbAluOp::b &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The B-only destination-blender form the screen transitions and the frame
// buffer copy author, recovered from the static inventory:
//
//   DBBMULTCNTL        = 0x20      -> B = SRCCOLOR * CONST
//   DBBMULTCONSTSSB0/1 = 0xffffff  -> the B coefficient constant is white
//   DBALUCNTL          = 0xd0      -> ALU operation B, no final divide
//
// `FN_doscrolltrans__342f4`, `FN_dowipetrans__34cb8`,
// `FN_dostripestrans__35650`, `FN_dogridtrans__35d7c`,
// `FN_dosquare1trans__32010`, `FN_dosquare2trans__32f88`,
// `FN_dorotatetrans__33880` and `FN_StreamEventFunc__3b63c` author all three
// words literally; `FN_Dsply_CopyFrameBuffer__11f98` authors the B and ALU
// words but loads the coefficient pair from a register, so its coefficient is
// only known from the register file at draw time.
//
// This is the reference blitter's own `Blt_BlendBlitObject(bo, src, dest)`
// case `src == 0.0, dest == 1.0`: it picks `ALUOPERATION_B`, so the A product
// is discarded and the destination-blender output is the bound source frame
// buffer pixel unchanged.  `Blt_BlendBlitObject` reproduces the identical B
// word (`BINPUTSELECT=SRCCOLOR`, `BMULTCOEFSELECT=CONST`,
// `BMULTCONSTCONTROL=TEXSSB`) from the same helper the guest's transitions use.
// The write replaces the destination pixel; it is not an alpha lerp against
// it, so it must not ride the source-over default.
//
// Both coefficient words are required to be white because the SSB0/SSB1
// selection is not yet tied to a source contract; a non-white pair is a
// scaled copy whose strength is not recovered and stays fail-closed.
inline bool clt_dsb_is_source_copy(const CltDsbAMultiplier& a,
                                   const CltDsbBMultiplier& b,
                                   const CltDsbAlu& alu, uint32_t b_const_ssb0,
                                   uint32_t b_const_ssb1)
{
    return a.valid && b.valid && !b.justify && alu.reserved_clear &&
           b.input == CltDbBInput::source_color &&
           b.coef == CltDbBCoef::constant &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented && alu.op == CltDbAluOp::b &&
           alu.final_divide_documented && alu.final_divide == 0u &&
           clt_dsb_const_is_white(b_const_ssb0) &&
           clt_dsb_const_is_white(b_const_ssb1);
}

// The reverse-alpha source-framebuffer equation authored by
// `FN_StreamEventFunc__3b63c` for the Configure Race border:
//
//   DBAMULTCNTL = 0x02 -> A = TEXCOLOR * (1 - TEXALPHA)
//   DBBMULTCNTL = 0x00 -> B = SRCCOLOR  * TEXALPHA
//   DBALUCNTL   = 0x10 -> A_PLUS_B, no final divide
//
// This is not ordinary source-over: alpha zero selects the texture and alpha
// one selects the separately armed DB source. The interpreter must carry the
// DB source as a second texture to the backend; collapsing this form into the
// generic source-over path reverses the alpha and substitutes the current
// destination for SRCCOLOR.
//
// As with `clt_dsb_is_constant_alpha_blend`, both documented adds are
// accepted: `FN_StreamEventFunc__3b63c` writes A_PLUS_B (0x10), but the
// convex weights (1 - a and a) make A_PLUS_BCLAMP -- the vendor `M_DBTrans`
// word and the value a list that never writes DBALUCNTL inherits -- evaluate
// the identical bytes.  Rejecting the inherited word here routed the draw to
// the source-over approximation, which is exactly the destination- for
// SRCCOLOR substitution this predicate exists to prevent.
inline bool clt_dsb_is_secondary_source_alpha(
    const CltDsbAMultiplier& a, const CltDsbBMultiplier& b,
    const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::texture_color &&
           a.coef == CltDbACoef::texture_alpha_complement &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::source_color &&
           b.coef == CltDbBCoef::texture_alpha &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented &&
           (alu.op == CltDbAluOp::a_plus_bclamp ||
            alu.op == CltDbAluOp::a_plus_b) &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// The two-input weighted mix the checked-in Portfolio blitter authors
// (`blitter_utils/blend.c`, `Blt_BlendBlitObject(bo, src, dest)`): it writes
// the `src` weight into the A multiplier's coefficient pair (0x805c/0x8060)
// and the `dest` weight into the B pair (0x8068/0x806c), keeps A as
// `TEXCOLOR * CONST` and B as `SRCCOLOR * CONST` with `TEXSSB` const control
// and no justify, selects `A_PLUS_BCLAMP`, and arms `BLENDEN|SRCEN`.  The
// identical A word is the toolkit's own destination-blender default snippet
// (`folios/blitter/defaults.c`, `defaultDBlend`) and the guest's per-frame
// `DestBlendInit` initializer, so this is the toolkit's documented
// "mix foreground and background" contract rather than a host invention.
//
// The guest's `FN_setSsbBlendAttr__371a0` (ui.c) authors the same triad with
// the weight carried in its parameter and the B side spelled with the
// complement coefficient, i.e. `Blt_BlendBlitObject(bo, w, 1 - w)`.  Either B
// coefficient word is therefore in the family: `CONST` makes the B weight the
// coefficient itself and `CONSTCOMPLEMENT` makes it `1 - coefficient`.
//
// The destination-blender output is `TEXCOLOR * a + SRCCOLOR * b` with the
// per-channel weights read from the coefficient pairs, so the host evaluates
// the guest's arithmetic instead of an alpha lerp.
//
// The coefficient pair is not a destination-format variant: `...CONSTSSB0` and
// `...CONSTSSB1` are entries 0 and 1 of the multiplier's coefficient constant,
// and the CLT toolkit says the per-texel SSB -- its "source selection bit" --
// selects between them (`-CLT_C-` "Color, Alpha, and SSB Values that Can Be
// Assigned to a Texel": "the alpha of the first color constant is used for
// texels that have an SSB value of 0, and the alpha of the second color
// constant is used for texels that have an SSB value of 1"; "A source
// selection bit (SSB) that is generally used to select between two sets of
// constant registers").  `AMULTCONSTCONTROL`/`BMULTCONSTCONTROL` name which
// stage's SSB drives that choice (the texture's or the source's), and a texture
// with no SSB passes SSB 0, so entry 0 is the default.
//
// This predicate therefore only recovers the structure of the equation.  The
// entry selection is a property of the draw, so `CltBlendState` resolves entry 0
// and records whether the two entries differ; see
// `clt_dsb_weighted_mix_ssb_selects_entry1`.
inline bool clt_dsb_is_coefficient_weighted_mix(const CltDsbAMultiplier& a,
                                               const CltDsbBMultiplier& b,
                                               const CltDsbAlu& alu)
{
    return a.valid && b.valid && !a.justify && !b.justify &&
           alu.reserved_clear &&
           a.input == CltDbAInput::texture_color &&
           a.coef == CltDbACoef::constant &&
           a.const_control == CltDbConstControl::texture_ssb &&
           b.input == CltDbBInput::source_color &&
           (b.coef == CltDbBCoef::constant ||
            b.coef == CltDbBCoef::constant_complement) &&
           b.const_control == CltDbConstControl::texture_ssb &&
           alu.op_documented &&
           (alu.op == CltDbAluOp::a_plus_b ||
            alu.op == CltDbAluOp::a_plus_bclamp) &&
           alu.final_divide_documented && alu.final_divide == 0u;
}

// True when a weighted-mix draw picks the second coefficient entry for at least
// one texel, i.e. the register entries differ (`entry_dependent`) and the draw's
// SSB signal has a set bit somewhere (`ssb_signal_nonzero`).  That selects
// between two different A/B weight pairs per texel, which a single resolved pair
// cannot express, so the draw falls back to the recovered source-over default
// instead of evaluating a slot the registers do not name.  When either input is
// false the equation is texel-independent and entry 0 is the whole result.
inline bool clt_dsb_weighted_mix_ssb_selects_entry1(bool entry_dependent,
                                                    bool ssb_signal_nonzero)
{
    return entry_dependent && ssb_signal_nonzero;
}

// Destination-blender state that cannot be represented by the plain
// source-over boolean.  The vendor M_DBSpec macro composites the texture
// stage result with DBCONSTIN internally while SRCEN is clear; it does not
// blend against the framebuffer.  The constant is the guest's normalized
// RGB DBCONSTIN value.  `constant_alpha_over` is the source-frame-buffer
// read-modify-write form above: the guest's DBCONSTIN color is interpolated
// with the destination pixel by the texture-blender alpha.
// `inverse_texture_multiply` is the B-only form: the destination pixel is
// multiplied by (1 - texture-blender color).  `source_copy` is the B-only
// identity-coefficient form: the bound source frame buffer pixel replaces the
// destination pixel outright.  `coefficient_weighted_mix` is the toolkit's
// two-input weighted sum `TEXCOLOR * a + SRCCOLOR * b` whose per-channel
// weights come from the multiplier coefficient registers.
enum class CltBlendMode : uint8_t {
    disabled = 0,
    source_over = 1,
    specular_add = 2,
    constant_alpha_over = 3,
    inverse_texture_multiply = 4,
    source_copy = 5,
    coefficient_weighted_mix = 6,
    // Reverse-alpha source-framebuffer composition:
    // TEXCOLOR*(1-TEXALPHA) + SRCCOLOR*TEXALPHA.
    secondary_source_alpha = 7,
    // M_DBFog internal composite (SRCEN clear): the texture-stage color is
    // lerped toward the guest's DBCONSTIN fog color by the per-vertex fog
    // factor (the texture-stage alpha).  Blending is disabled; the shader
    // evaluates TEXCOLOR*a + fogcolor*(1 - a) directly.
    fog_composite = 8,
    // TEXCOLOR*TEXALPHA + separately addressed SRCCOLOR*(1-TEXALPHA).
    secondary_source_over = 9,
    // CONSTCOLOR*TEXALPHA + separately addressed SRCCOLOR*(1-TEXALPHA).
    secondary_constant_over = 10,
    // Source-over with SRCEN clear, proven PRIMALPHA=1 at every vertex.
    // The source coefficient is zero; write texture-stage RGB and alpha 1.
    opaque_source_over = 11,
    general_internal = 12,
    general_secondary = 13,
};

inline bool clt_blend_uses_secondary_source(CltBlendMode mode)
{
    return mode == CltBlendMode::general_secondary ||
           mode == CltBlendMode::secondary_source_alpha ||
           mode == CltBlendMode::secondary_source_over ||
           mode == CltBlendMode::secondary_constant_over;
}

// DBSRCALPHACNTL (0x8074) decoded per cltmacros.h raw-word positions:
// ACLAMP PPCMASK(0x3F,31) is the aggregate of three 2-bit fields, one per
// alpha range, with per-field values LEAVEALONE=0, FORCE1=1, FORCE0=2:
//   ALPHA0    PPCMASK(3,31) -> bits 1:0  (texels whose alpha is 0)
//   ALPHAFRAC PPCMASK(3,29) -> bits 3:2  (fractional-alpha texels)
//   ALPHA1    PPCMASK(3,27) -> bits 5:4  (texels whose alpha is 0xff)
// The register is the source-frame-buffer/texture-blender alpha clamp
// control; every statically authored guest value is 0 (leave-alone), which
// is also the vendor default.
struct CltSrcAlphaControl {
    bool valid = false;
    uint32_t raw = 0;
    uint32_t alpha0 = 0u;      // 2 bits, texels with alpha == 0
    uint32_t alpha_frac = 0u;  // 2 bits, fractional-alpha texels
    uint32_t alpha1 = 0u;      // 2 bits, texels with alpha == 0xff
    bool documented = false;
};

// Per 2-bit field decode: 0 leave-alone, 1 force to 1, 2 force to 0; 3 is
// reserved by the SDK enumeration.
inline CltSrcAlphaControl clt_decode_dsb_src_alpha_control(uint32_t raw)
{
    CltSrcAlphaControl state;
    state.raw = raw;
    state.alpha0 = raw & 0x3u;
    state.alpha_frac = (raw >> 2u) & 0x3u;
    state.alpha1 = (raw >> 4u) & 0x3u;
    state.valid = true;
    state.documented = state.alpha0 <= 2u && state.alpha_frac <= 2u &&
                       state.alpha1 <= 2u;
    return state;
}// DBDESTALPHACNTL (0x8078) DESTCONSTSELECT PPCMASK(0xf, 31) = 0xf<<0: the
// destination-alpha source occupies bits 3:0.  Every statically authored
// guest value is 0 (TEXALPHA); the other documented selectors name constants
// or the source alpha.
enum class CltDestAlphaSource : uint8_t {
    texture_alpha = 0,
    texture_ssb_const = 1,
    source_alpha = 2,
    rblend = 3,
    source_ssb_const = 9,
};

struct CltDestAlphaControl {
    bool valid = false;
    uint32_t raw = 0;
    CltDestAlphaSource source = CltDestAlphaSource::texture_alpha;
    bool documented = false;
};

inline CltDestAlphaControl clt_decode_dsb_dest_alpha_control(uint32_t raw)
{
    CltDestAlphaControl state;
    state.raw = raw;
    const uint32_t select = raw & 0xfu;
    state.documented = select == 0u || select == 1u || select == 2u ||
                       select == 3u || select == 9u;
    state.source = static_cast<CltDestAlphaSource>(select);
    state.valid = true;
    return state;
}

// DBDESTALPHACONST (0x807c): the destination alpha constants, one 8-bit SSB
// entry per byte (SSB0 PPCMASK(0xFF,15) -> bits 23:16, SSB1
// PPCMASK(0xFF,31) -> bits 7:0).
struct CltDestAlphaConst {
    bool valid = false;
    uint32_t raw = 0;
    uint8_t ssb0 = 0;
    uint8_t ssb1 = 0;
};

inline CltDestAlphaConst clt_decode_dsb_dest_alpha_const(uint32_t raw)
{
    CltDestAlphaConst state;
    state.raw = raw;
    state.ssb1 = static_cast<uint8_t>(raw & 0xffu);
    state.ssb0 = static_cast<uint8_t>((raw >> 16u) & 0xffu);
    state.valid = true;
    return state;
}

// Documented field sets of the two texture-load control registers, taken from
// the vendor masks in cltmacros.h (`PPCMASK(d, n)` is `d << (31 - n)`, so the
// declared constants are the field masks themselves):
//
//   TXTLDCNTL (0x6404):  COMPRESSED  PPCMASK(1, 21) -> bit 10
//                        LOADMODE    PPCMASK(3, 23) -> bits 9:8
//                        SRCBITOFF   PPCMASK(7, 31) -> bits 2:0   => 0x707
//   TXTPIPCNTL (0x640c): PIPSSBSELECT   PPCMASK(7, 17)   -> bits 16:14
//                        PIPALPHASELECT PPCMASK(7, 20)   -> bits 13:11
//                        PIPCOLORSELECT PPCMASK(7, 23)   -> bits 10:8
//                        PIPINDEXOFFSET PPCMASK(0xFF, 31) -> bits 7:0
//                                                                  => 0x1ffff
//
// Every vendor authoring site composes these words out of the masked field bits
// alone (`CLA_TXTLDCNTL`, `CLT_SetConst(TXTLDCNTL, LOADMODE, ...)`, and
// `CLA_TXTPIPCNTL`), so a word carrying any other bit is not a programmed state.
// These two predicates are the host's fail-closed boundary for the registers:
// the audit's `host_accepted_set` gate pins them, and each consumer rejects a
// word they declare undocumented.
inline constexpr uint32_t kCltTxLoadControlKnownMask = 0x707u;
inline constexpr uint32_t kCltTxLoadControlCompressed = 0x400u;
inline constexpr uint32_t kCltTxLoadControlModeShift = 8u;
inline constexpr uint32_t kCltTxLoadControlModeMask = 3u;
inline constexpr uint32_t kCltTxPipControlKnownMask = 0x1ffffu;

// The toolkit's own load-mode numbering (RC_TXTLDCNTL_LOADMODE_*).
inline constexpr uint32_t kCltTxLoadModeTexture = 0u;
inline constexpr uint32_t kCltTxLoadModeMmdma = 1u;
inline constexpr uint32_t kCltTxLoadModePip = 2u;

// The compression bit is meaningful only for the texture load mode: the vendor
// `clttxload.c` sites write it in the TexLoad path alone and never alongside
// MMDMA (`CLT_SetConst(TXTLDCNTL, LOADMODE, MMDMA)`) or the PIP load
// (`CLT_TXTLDCNTL(..., 0, 2, 0)`), so those combinations have no authored form.
// Load mode 3 is not defined by the toolkit at all.
inline bool clt_tex_load_control_documented(uint32_t raw)
{
    if (raw & ~kCltTxLoadControlKnownMask) return false;
    const uint32_t mode =
        (raw >> kCltTxLoadControlModeShift) & kCltTxLoadControlModeMask;
    if (mode == kCltTxLoadModeTexture) return true;
    if (raw & kCltTxLoadControlCompressed) return false;
    return mode == kCltTxLoadModeMmdma || mode == kCltTxLoadModePip;
}

// The three PIP selectors are three-valued (CONSTANT / TEXTURE / PIP); the
// toolkit defines no fourth selector, and a bit above the documented field set
// makes the whole word unprogrammed.
inline bool clt_pip_control_documented(uint32_t raw)
{
    if (raw & ~kCltTxPipControlKnownMask) return false;
    return ((raw >> 14u) & 7u) <= 2u && ((raw >> 11u) & 7u) <= 2u &&
           ((raw >> 8u) & 7u) <= 2u;
}

// DBSRCCNTL.SRCMSBREP (0x8030 bit 1, `FV_DBSRCCNTL_SRCMSBREP_MASK` =
// PPCBIT(30)) selects how the destination blender expands a 16-bpp source
// component: "For 16 Bpp, Replicate 3 MSBs into 3 LSB's if left justified
// (otherwise insert zeros)" (M2 Command List Toolkit, DBSRCCNTL).  The two
// documented forms are therefore
//
//   SRCMSBREP = 1: (v << 3) | (v >> 2)   -- the round-trip identity at 5 bits
//   SRCMSBREP = 0: (v << 3)              -- the low 3 bits stay zero
//
// Every vendor authoring site passes `srcmsbrep = 1` (`F2_CopyRect`,
// `CLT_SetSrcToCurrentDest`, and the `M_DBTrans` recipe's
// `CLA_DBSRCCNTL(1, deptheq32)`), so the bit's 1 form is the one this image
// arms on the blend, volume, and copy paths.  The 0 form is authored only by
// the framebuffer copy/transition family (`Dsply_CopyFrameBuffer`, the seven
// screen transitions, `StreamEventFunc`), whose source is the 16-bit display
// surface itself.
//
// The production DB-source materializer passes the guest's register bit to
// this helper. At a 5-bit destination the forms quantize to the same value
// (`clt_db_source_5bit_forms_agree`), but an RGBA presentation retains their
// defined low-bit difference.
inline constexpr uint32_t kCltDbSourceComponentMask = 0x1fu;

inline uint32_t clt_db_source_expand_5_to_8(uint32_t component,
                                            bool replicate_msb)
{
    const uint32_t v = component & kCltDbSourceComponentMask;
    return (v << 3u) | (replicate_msb ? (v >> 2u) : 0u);
}

// True when the two documented expansions are indistinguishable at the
// destination precision the image authors (5 bits per component), which is what
// makes the host's fixed form a documented no-op rather than a guess.
inline bool clt_db_source_5bit_forms_agree(uint32_t component)
{
    return (clt_db_source_expand_5_to_8(component, true) >> 3u) ==
           (clt_db_source_expand_5_to_8(component, false) >> 3u);
}

struct CltBlendState {
    CltTabState tab{};
    CltGeneralDsb general{};
    CltBlendMode mode = CltBlendMode::disabled;
    // TAB MULT: TEXALPHA * CONSTCOLOR, with PRIMALPHA output. The
    // interpreter carries the constant in vertex RGB and primitive alpha in
    // vertex A; backends replicate sampled alpha to RGB before that multiply.
    bool texture_alpha_as_color = false;
    float constant_r = 0.f;
    float constant_g = 0.f;
    float constant_b = 0.f;
    // Texture-side (A) and bound-source-side (B) weights for
    // `coefficient_weighted_mix`, in the same channel order as `constant_*`
    // (red first), resolved from coefficient entry 0 and the entry the draw's
    // SSB signal selects.
    float weight_tex_r = 1.f;
    float weight_tex_g = 1.f;
    float weight_tex_b = 1.f;
    float weight_src_r = 1.f;
    float weight_src_g = 1.f;
    float weight_src_b = 1.f;
    // Coefficient entry 1 weights for the same two sides, resolved whenever
    // the weighted mix is decoded.  Both entries are known register words, so
    // a per-texel SSB split evaluates `TEXCOLOR*a(ssb) + SRCCOLOR*b(ssb)`
    // instead of downgrading the draw.
    float entry1_weight_tex_r = 1.f;
    float entry1_weight_tex_g = 1.f;
    float entry1_weight_tex_b = 1.f;
    float entry1_weight_src_r = 1.f;
    float entry1_weight_src_g = 1.f;
    float entry1_weight_src_b = 1.f;
    // True when coefficient entries 0 and 1 resolve to different weights, so
    // the draw's per-texel SSB selects between two equations.
    bool weighted_mix_entry_dependent = false;
    // True when the draw's SSB signal actually sets a bit and the entries
    // differ, so the backend must apply the per-fragment entry selection (or
    // the two-pass SSB-mask split) rather than a single resolved pair.
    bool weighted_mix_ssb_selects_entry1 = false;
    // The texture's SSB bitmap as an uploaded mask cel, resolved by the
    // interpreter when the entry split is armed.  Backends sample this (or
    // the discard's own mask cel, which carries the same bitmap) per fragment
    // to pick the equation; a texture with no SSB bit selects entry 0
    // everywhere, so a missing mask is only fatal when the guest could set
    // bits.
    CelId weighted_mix_ssb_mask = kInvalidCel;
    // Secondary DB-source raster for separately addressed alpha equations. The
    // interpreter fills this only when the source-backed blend form is armed;
    // the backends must fail closed rather than substitute the destination.
    CelId secondary_source = kInvalidCel;
    CelId source_ssb_mask = kInvalidCel;
    // DB-source coordinates may be carried as an unbounded normalized linear
    // address (x / stride). Backends resolve floor(x) into a source-row carry
    // per fragment; this is necessary when a destination rectangle crosses a
    // stride boundary. These are screen-space addresses: interpolate without
    // perspective correction, including mask_u/v for secondary_source_alpha. False keeps bounded coordinates for
    // non-DB-source and direct diagnostic draws.
    bool db_source_linear = false;
    float db_source_height = 0.f;
    // Statically authored destination-alpha controls (0x8074/0x8078/0x807c).
    // The register words are decoded exactly as the guest wrote them.
    // Unsupported alpha output remains explicit even when an independently
    // recovered RGB equation can still be preserved (Configure Car's
    // M_DBSpec plus SRCALPHA case).
    CltSrcAlphaControl src_alpha{};
    CltDestAlphaControl dest_alpha{};
    CltDestAlphaConst dest_alpha_const{};
    // True when the alpha controls are armed (any ACLAMP field not
    // leave-alone, or a destination-alpha source other than TEXALPHA) and the
    // draw's blend form has no source-backed evaluation for them.
    bool alpha_control_unsupported = false;
    // True only when the original blend equation is undecoded and the host
    // displays it with a reviewed source-over approximation. The backend
    // reports the first actual draws with their raw equation words.
    bool equation_approximate = false;
    CltDsbState dsb{};
    CltDsbEquation equation{};
    // Opt-in RGB555 output dithering; default-constructed (disabled) unless
    // the host option is on and the guest armed DITHEREN.
    CltDither dither{};
};

struct CltDbSourceSampleUv {
    float u = 0.f;
    float v = 0.f;
};

// Resolve the source-row carry for a normalized linear DB-source address.
// The input u is intentionally allowed to exceed [0,1); its integer part is
// the number of source rows to add to v. The backend's normal nearest/clamp
// sampler then handles the resulting fractional texel address.
inline CltDbSourceSampleUv clt_db_source_sample_uv(
    float linear_u, float linear_v, const CltBlendState& blend)
{
    if (!blend.db_source_linear || blend.db_source_height <= 0.f)
        return {linear_u, linear_v};
    const float carry = std::floor(linear_u);
    return {linear_u - carry,
            linear_v + carry / blend.db_source_height};
}

inline CltBlendMode clt_effective_blend_mode(bool blend_enabled,
                                               const CltBlendState& state)
{
    if (state.equation_approximate) {
        static std::atomic<unsigned> reported{0};
        const unsigned index = reported.fetch_add(1u);
        if (index < 16u)
            std::fprintf(stderr,
                         "[clt-blend] source-over approximation draw "
                         "a=%#x b=%#x alu=%#x\n",
                         state.equation.a.raw, state.equation.b.raw,
                         state.equation.alu.raw);
    }
    if (state.alpha_control_unsupported &&
        (state.mode == CltBlendMode::general_internal || state.mode == CltBlendMode::general_secondary)) {
        static std::atomic<unsigned> reported_alpha{0};
        if (reported_alpha.fetch_add(1u) < 8u)
            std::fprintf(stderr, "[clt-blend] RGB equation evaluated; unavailable framebuffer alpha/SSB, written alpha uses TEXALPHA (selector=%#x)\n",
                         state.dest_alpha.raw);
    }
    if (state.mode != CltBlendMode::disabled) return state.mode;
    return blend_enabled ? CltBlendMode::source_over
                         : CltBlendMode::disabled;
}

// GL fixed-function plan for each decoded destination-blender equation.  The
// two OpenGL CLT backends must not disagree about how a mode maps to blend
// factors, shader-side color rewrites, or the alpha channel's destination
// factor, so the mapping lives here once (the same reason
// clt_backend_depth_compare_for_dbz lives here).  Factor values are the
// backend-neutral names of OpenGL's fixed-function blend factors (glBlendFunc
// / glBlendFuncSeparate); the GL backends cast them to their GLenum values.
// The software backend evaluates the same equations arithmetically; its
// parity with this plan is pinned by the focused backend tests in
// guest_clt_test.cpp.
enum class CltGlBlendFactor : uint16_t {
    zero = 0,
    one = 1,
    src_alpha = 2,
    one_minus_src_alpha = 3,
    one_minus_src_color = 4,
    constant_color = 5,
};

struct CltGlBlendPlan {
    CltGlBlendFactor src_factor = CltGlBlendFactor::src_alpha;
    CltGlBlendFactor dst_factor = CltGlBlendFactor::one_minus_src_alpha;
    CltGlBlendFactor src_alpha_factor = CltGlBlendFactor::src_alpha;
    CltGlBlendFactor dst_alpha_factor =
        CltGlBlendFactor::one_minus_src_alpha;
    bool blend_enabled = true;
    bool uses_constant_color = false;  // B-side weights ride glBlendColor.
    bool restores = false;             // post-draw src-alpha/1-minus reset.
};

inline CltGlBlendPlan clt_gl_blend_plan(CltBlendMode mode)
{
    CltGlBlendPlan plan;
    switch (mode) {
    case CltBlendMode::disabled:
        plan.blend_enabled = false;
        break;
    case CltBlendMode::source_over:
    case CltBlendMode::constant_alpha_over:
        // constant_alpha_over's fragment color is the guest's DBCONSTIN
        // (shader rewrite) and GL_BLEND supplies the (1 - TEXALPHA)
        // destination term, so the factors match plain source-over.
        break;
    case CltBlendMode::specular_add:
    case CltBlendMode::fog_composite:
    case CltBlendMode::opaque_source_over:
    case CltBlendMode::general_internal:
    case CltBlendMode::general_secondary:
        // M_DBSpec/M_DBFog evaluate their constant composite in the shader with
        // fixed-function blending off, so the fragment replaces the pixel (the
        // spec add writes the post-texturing alpha; the opaque fog lerp writes
        // an opaque alpha). The proven opaque source-over reduction likewise
        // replaces the pixel without reading the destination.
        plan.blend_enabled = false;
        break;
    case CltBlendMode::inverse_texture_multiply:
        // RGB: dst = dst * (1 - TEXCOLOR).  The separate alpha path uses the
        // texture-stage alpha equivalent; OpenGL does not admit a color
        // factor in the alpha-factor slots of glBlendFuncSeparate.
        plan.src_factor = CltGlBlendFactor::zero;
        plan.dst_factor = CltGlBlendFactor::one_minus_src_color;
        plan.src_alpha_factor = CltGlBlendFactor::zero;
        plan.dst_alpha_factor = CltGlBlendFactor::one_minus_src_alpha;
        plan.restores = true;
        break;
    case CltBlendMode::source_copy:
        // B-only identity coefficient: the fragment replaces the pixel
        // regardless of its alpha: (ONE, ZERO).
        plan.src_factor = CltGlBlendFactor::one;
        plan.dst_factor = CltGlBlendFactor::zero;
        plan.src_alpha_factor = CltGlBlendFactor::one;
        plan.dst_alpha_factor = CltGlBlendFactor::zero;
        plan.restores = true;
        break;
    case CltBlendMode::coefficient_weighted_mix:
        // TEXCOLOR * a in the fragment (shader scale), SRCCOLOR * b through
        // GL's CONSTANT_COLOR destination factor.  Separate alpha factors
        // keep the written alpha at the post-texturing alpha the guest's
        // DESTCONSTSELECT = TEXALPHA names.
        plan.src_factor = CltGlBlendFactor::one;
        plan.dst_factor = CltGlBlendFactor::constant_color;
        plan.src_alpha_factor = CltGlBlendFactor::one;
        plan.dst_alpha_factor = CltGlBlendFactor::zero;
        plan.uses_constant_color = true;
        plan.restores = true;
        break;
    case CltBlendMode::secondary_source_alpha:
    case CltBlendMode::secondary_source_over:
    case CltBlendMode::secondary_constant_over:
        // The shader samples the separately armed DB source and evaluates
        // TEXCOLOR*(1-TEXALPHA) + SRCCOLOR*TEXALPHA before the write. GL
        // fixed-function blending must stay disabled so it cannot blend that
        // result against the current destination a second time.
        plan.blend_enabled = false;
        break;
    }
    return plan;
}

// One guest-authored MIP level, already in the packed RGBA layout every upload
// entry point uses. CLT_CreateTxLoadCommandListMMDMA gives each loaded LOD its
// own texelDataSize span, so these are distinct guest payloads rather than
// reductions the host derived.
struct CelLodLevel {
    int w = 0;
    int h = 0;
    const uint32_t* rgba = nullptr;
};

// Identity of a guest-authored MIP chain in the FNV-1a form the GL backends
// key their retired-texture cache with. The interpreter computes this once
// while it expands a resident's coarser levels, so a palette refresh can hand
// the result to cel_upload_rgba_lod_chain instead of making the backend rescan
// every level's bytes. `levels` must be exactly the authored prefix the
// backend will upload, in increasing-LOD order. Zero marks a single-level
// upload in a cache record, so a non-empty chain never returns zero.
inline uint64_t cel_lod_chain_hash(const CelLodLevel* levels, size_t count)
{
    if (!levels || count == 0) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < count; ++i) {
        const CelLodLevel& level = levels[i];
        const size_t texels = static_cast<size_t>(level.w) * level.h;
        const auto* bytes = reinterpret_cast<const uint8_t*>(level.rgba);
        for (size_t b = 0; b < texels * sizeof(uint32_t); ++b) {
            hash ^= bytes[b];
            hash *= 1099511628211ull;
        }
        hash ^= static_cast<uint8_t>(level.w) + 0x9eu;
        hash *= 1099511628211ull;
        hash ^= static_cast<uint8_t>(level.h) + 0x9eu;
        hash *= 1099511628211ull;
    }
    return hash != 0u ? hash : 1u;
}

// One host-generated level below a guest-authored MIP tail.
struct CelLodTail {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> rgba;
};

// Host extension of a guest-authored MIP chain below its tail: 2x2 box
// reduction of the coarsest level the guest did supply, down to 1x1. Levels
// the guest did not load have no authored bytes, so the tail must derive from
// the authored tail itself -- never from level 0, whose authored content the
// coarser levels may not carry. The reduction is the same one the software
// backend applies (soft_capture.h `generate_mip_tail`): per channel
// (sum of up to four source texels + 2) / 4, clamping the 2x2 window at the
// level's edges, so both implementations extend a chain level for level.
inline void cel_generate_lod_tail(const uint32_t* coarsest_rgba,
                                  int coarsest_w, int coarsest_h,
                                  std::vector<CelLodTail>& out)
{
    if (!coarsest_rgba || coarsest_w <= 0 || coarsest_h <= 0) return;
    int level_w = coarsest_w;
    int level_h = coarsest_h;
    std::vector<uint32_t> source(coarsest_rgba,
                                 coarsest_rgba +
                                     static_cast<size_t>(coarsest_w) *
                                         coarsest_h);
    while (level_w > 1 || level_h > 1) {
        const int next_w = std::max(1, level_w / 2);
        const int next_h = std::max(1, level_h / 2);
        CelLodTail next;
        next.w = next_w;
        next.h = next_h;
        next.rgba.resize(static_cast<size_t>(next_w) * next_h);
        for (int y = 0; y < next_h; ++y) {
            for (int x = 0; x < next_w; ++x) {
                const int x0 = std::min(level_w - 1, x * 2);
                const int x1 = std::min(level_w - 1, x0 + 1);
                const int y0 = std::min(level_h - 1, y * 2);
                const int y1 = std::min(level_h - 1, y0 + 1);
                const uint32_t p00 =
                    source[static_cast<size_t>(y0) * level_w + x0];
                const uint32_t p10 =
                    source[static_cast<size_t>(y0) * level_w + x1];
                const uint32_t p01 =
                    source[static_cast<size_t>(y1) * level_w + x0];
                const uint32_t p11 =
                    source[static_cast<size_t>(y1) * level_w + x1];
                uint32_t value = 0;
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    const unsigned sum = ((p00 >> shift) & 0xffu) +
                                         ((p10 >> shift) & 0xffu) +
                                         ((p01 >> shift) & 0xffu) +
                                         ((p11 >> shift) & 0xffu);
                    value |= ((sum + 2u) / 4u) << shift;
                }
                next.rgba[static_cast<size_t>(y) * next_w + x] = value;
            }
        }
        source = next.rgba;
        level_w = next_w;
        level_h = next_h;
        out.push_back(std::move(next));
    }
}

class ICelBlitterBackend {
public:
    virtual ~ICelBlitterBackend() = default;

    // Projector scanout from a guest RAM Bitmap, independent of TE targets.
    // Present only the guest's viewport carve, scaled to fill the target (see
    // overscan_crop_requested). The backends set this once at init.
    void set_overscan_crop(bool enabled) { m_crop_overscan = enabled; }
    bool overscan_crop() const { return m_crop_overscan; }
    void set_bitmap_scanout(const uint32_t* rgba, int w, int h) {
        m_scanout_view = nullptr;
        m_scanout_hash = 0;
        if (!rgba || w <= 0 || h <= 0) { m_scanout.clear(); return; }
        m_scanout.assign(rgba, rgba + size_t(w) * h);
        m_scanout_w = w;
        m_scanout_h = h;
    }
    // The main loop owns a stable display snapshot buffer for the entire
    // frame. Borrowing it avoids a second 640x480 host copy; the caller must
    // keep the buffer unchanged until the next call or clear_bitmap_scanout.
    void set_bitmap_scanout_view(const uint32_t* rgba, int w, int h,
                                 uint64_t content_hash = 0) {
        m_scanout.clear();
        m_scanout_view = rgba;
        m_scanout_hash = content_hash;
        if (!rgba || w <= 0 || h <= 0) {
            m_scanout_view = nullptr;
            m_scanout_hash = 0;
            return;
        }
        m_scanout_w = w;
        m_scanout_h = h;
    }
    void clear_bitmap_scanout() {
        if (m_scanout_cel != kInvalidCel) {
            cel_free(m_scanout_cel);
            m_scanout_cel = kInvalidCel;
        }
        m_scanout.clear();
        m_scanout_view = nullptr;
        m_scanout_hash = 0;
        m_scanout_cel_hash = 0;
        m_scanout_cel_w = m_scanout_cel_h = 0;
    }
    // The cel draw_bitmap_scanout() blits, so a backend can apply the
    // M2_BITMAP_FILTER presentation sampling to it alone.
    CelId bitmap_scanout_cel() const { return m_scanout_cel; }
    bool has_bitmap_scanout() const {
        return m_scanout_view != nullptr || !m_scanout.empty();
    }
    // A borrowed view wins; otherwise the internal copy is the source.  The
    // empty check is required, not defensive: a cleared std::vector keeps its
    // capacity, so data() can stay non-null after a withdrawal and a stale
    // scanout would otherwise outrank the CLT colour target.
    const uint32_t* bitmap_scanout_source() const {
        if (m_scanout_view) return m_scanout_view;
        return m_scanout.empty() ? nullptr : m_scanout.data();
    }
    bool copy_bitmap_scanout(uint32_t* dst, int w, int h) const {
        const uint32_t* source = bitmap_scanout_source();
        if (!source || !dst || w <= 0 || h <= 0) return false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                dst[size_t(y) * w + x] = source[
                    size_t(y * m_scanout_h / h) * m_scanout_w + x * m_scanout_w / w];
        return true;
    }
    // Called with the default framebuffer bound, never a guest TE target.
    void draw_bitmap_scanout() {
        const uint32_t* source = bitmap_scanout_source();
        if (!source) return;
        int w, h;
        get_size(&w, &h);
        float dx = 0.f, dy = 0.f, dw = float(w), dh = float(h);
        if (m_crop_overscan && m_scanout_w == kDisplayWidth &&
            m_scanout_h == kDisplayHeight) {
            // Scale the 640x480 display so its 576x432 carve fills the target;
            // the quad extends past the viewport, which clips the margin.
            dw = float(w) * kDisplayWidth / kCarveWidth;
            dh = float(h) * kDisplayHeight / kCarveHeight;
            dx = -dw * kCarveX / kDisplayWidth;
            dy = -dh * kCarveY / kDisplayHeight;
        }
        if (m_scanout_cel == kInvalidCel ||
            m_scanout_hash == 0 ||
            m_scanout_cel_hash != m_scanout_hash ||
            m_scanout_cel_w != m_scanout_w || m_scanout_cel_h != m_scanout_h) {
            if (m_scanout_cel != kInvalidCel) cel_free(m_scanout_cel);
            m_scanout_cel = cel_upload_rgba_ex_hashed(
                m_scanout_w, m_scanout_h, source, false, false,
                m_scanout_hash);
            m_scanout_cel_hash = m_scanout_hash;
            m_scanout_cel_w = m_scanout_w;
            m_scanout_cel_h = m_scanout_h;
        }
        if (m_scanout_cel != kInvalidCel)
            cel_blit(m_scanout_cel, dx, dy, dw, dh);
    }

    // Create context/resources for this window. Returns false if the
    // backend can't run on this platform; main() falls back to null backend.
    virtual bool init(SDL_Window* window) = 0;
    virtual void shutdown() = 0;

    // Upload an RGBA8888 raster (pitch = width). Returns cel id.
    virtual CelId cel_upload_rgba(int w, int h, const void* pixels) = 0;
    // Destroy a previously uploaded cel.
    virtual void cel_free(CelId cel) = 0;

    // Draw one cel as a textured quad at destination rect (pixels), tinted
    // by rgba in [0,1]. Coordinates are window-space, origin top-left.
    virtual void cel_blit(CelId cel, float dx, float dy, float dw, float dh,
                          float r = 1, float g = 1, float b = 1, float a = 1) = 0;

    // Untextured window-space quad (HUD panels, bars, pillarbox).
    // Default no-op so backends can opt out of the 2D-primitive path.
    virtual void fill_rect(float dx, float dy, float dw, float dh,
                           float r = 1, float g = 1, float b = 1, float a = 1) {}

    // Window-space scissor box in pixels (applies to subsequent draws);
    // scissor_disable() restores full-target drawing.
    virtual void scissor(float x, float y, float w, float h) {}
    virtual void scissor_disable() {}

    // GL backends can preserve a TE packet's materialized triangle order in
    // one GL_TRIANGLES submission. Diagnostic backends keep per-triangle
    // callback boundaries and return false.
    virtual bool clt_supports_triangle_batches() const { return false; }

    // Draw one guest-owned Triangle Engine strip or fan, or an explicitly
    // independent sequence of materialized triangles. The interpreter
    // preserves the guest vertex data and calls this only for commands it can
    // decode safely. target_w/target_h are the guest View's display surface
    // dimensions, allowing a backend to scale to the host window.
    virtual void clt_draw(const CltVertex* vertices, uint32_t count,
                          bool strip, bool textured, CelId texture,
                          float target_w, float target_h,
                          bool blend_enabled = true,
                          CltDiscard discard = {},
                          CltDepth depth = {},
                          CltTextureSampling sampling = {},
                          CltBlendState blend = {},
                          bool independent_triangles = false) {}

    // Platform-authored TE clear of the active Z bitmap. This is separate
    // from frame_begin(): the guest can request TE_CLEAR_Z_BUFFER for an
    // individual EXECUTELIST, and the real device performs that clear before
    // the list's guest CLT words run. The argument is the raw guest float
    // word supplied through TE_CMD_SETZBUFFER.
    virtual void clt_clear_z(uint32_t z_fill_word) { (void)z_fill_word; }

    // Platform-authored TE clear of the active color bitmap. The four raw
    // guest float words come from TEFrameBufferInfo and are expanded by the
    // backend using the same normalized color contract as guest vertices.
    virtual void clt_clear_color(uint32_t red_word, uint32_t green_word,
                                 uint32_t blue_word, uint32_t alpha_word)
    {
        (void)red_word;
        (void)green_word;
        (void)blue_word;
        (void)alpha_word;
    }

    // --- clean-GL 3D path -----------------------------------------------------
    //
    // Static interleaved-free mesh upload: positions xyz (world units),
    // colors rgba, triangle indices. The backend copies everything.
    virtual MeshId mesh_upload(int vertex_count, const float* pos_xyz,
                               const float* col_rgba, int index_count,
                               const uint16_t* indices) { return kInvalidMesh; }
    virtual void mesh_free(MeshId mesh) {}

    // Draw one mesh with a column-major 4x4 view-projection matrix.
    // Fog blends geometry toward the sky color with distance (matches the
    // original draw distance feel); fog_far <= 0 disables fog.
    virtual void mesh_draw(MeshId mesh, const float view_proj[16],
                           const float cam_pos[3], float fog_r, float fog_g,
                           float fog_b, float fog_far) {}

    // Current target size in device pixels (updated per frame_begin).
    virtual void get_size(int* w, int* h)
    {
        if (w) *w = 640;
        if (h) *h = 480;
    }

    // Copy the last presented frame's pixels as RGBA8888 (top-left origin,
    // row-major, w*h*4 bytes) into dst. Returns false when the backend has
    // no readable framebuffer (e.g. pure GL swap path). Used to retain the
    // previous frame for the DB-source strip blits that sample the display
    // carves. Implemented by software-framebuffer backends (soft_capture).
    virtual bool grab_presented_rgba(uint32_t* dst, int w, int h)
    {
        (void)dst;
        (void)w;
        (void)h;
        return false;
    }

    // Select the guest framebuffer receiving subsequent CLT color writes.
    // M2 keeps sibling bitmap carves independent even though the host only
    // presents one window surface. A backend without addressed readback must
    // report failure, not return pixels from an unrelated current surface.
    virtual void clt_target_select(uint32_t guest_base)
    {
        (void)guest_base;
    }
    // Select the completed guest color target for presentation without
    // changing the target receiving subsequent TE writes.
    virtual void clt_scanout_select(uint32_t guest_base)
    {
        (void)guest_base;
    }
    virtual void clt_target_scanout_rect(uint32_t guest_base,
                                         const CltScanoutRect& rect)
    {
        (void)guest_base;
        (void)rect;
    }
    // Guest-addressed TE targets are VRAM surfaces and persist between host
    // presentation frames. Tests may leave this disabled to isolate draws.
    virtual void clt_target_persistence(bool enabled) { (void)enabled; }
    virtual void clt_target_upload_rgba(uint32_t guest_base,
                                        const uint32_t* rgba, int w, int h)
    {
        (void)guest_base;
        (void)rgba;
        (void)w;
        (void)h;
    }
    virtual void clt_target_copy(uint32_t source_base, uint32_t dest_base)
    {
        (void)source_base;
        (void)dest_base;
    }
    virtual bool grab_clt_target_rgba(uint32_t guest_base, uint32_t* dst,
                                      int w, int h)
    {
        (void)guest_base;
        (void)dst;
        (void)w;
        (void)h;
        return false;
    }
    // Optional addressed sub-rectangle readback. Coordinates use the same
    // top-left origin as the guest display surface. The default keeps the
    // older full-target contract for backends that do not expose a cheaper
    // window read; DB-source sampling uses this when available.
    virtual bool grab_clt_target_window_rgba(uint32_t guest_base, uint32_t* dst,
                                             int x, int y, int w, int h)
    {
        (void)guest_base;
        (void)dst;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        return false;
    }
    // Copy an addressed target window into an independent texture at this
    // command boundary. Coordinates are top-left device pixels; the returned
    // cel is owned by the caller and survives later writes to the target.
    // Backends without a GPU copy path return invalid and use CPU readback.
    virtual CelId clt_snapshot_target_cel(uint32_t guest_base, int x, int y,
                                          int w, int h, int cel_w, int cel_h, bool preserve_alpha = false)
    {
        (void)guest_base;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)cel_w;
        (void)cel_h;
        (void)preserve_alpha;
        return kInvalidCel;
    }

    virtual bool grab_clt_target_window_metadata(uint32_t, uint32_t*, int, int, int, int)
    { return false; }

    // Independent framebuffer metadata snapshot: red = DSB (0/1), green = alpha.
    // No color filtering or blending may be applied to this attachment.
    virtual CelId clt_snapshot_target_ssb_cel(uint32_t, int, int, int, int, int, int)
    { return kInvalidCel; }

    // Bounded addressed-target pool accounting, reported on the host's `[gfx]`
    // shutdown summary. The two OpenGL backends pool guest-addressed colour
    // targets (GlCltTargets, kMaxTargets) and give one back to make room when a
    // valid guest stream names more distinct bases than the pool holds. That is
    // a disclosed host memory bound with a pixel consequence: the released
    // target's bytes are gone, so the next pass re-creates it from the host
    // clear. `releases` counts those give-backs; `refusals` counts a selection
    // that could not be satisfied at all; `pooled` is the pool's live size, so
    // `pooled + releases` is how many distinct bases the run addressed and a
    // reader can see the headroom the run had against the bound. A backend with
    // no addressed pool (software capture, null) reports zeros, which is why
    // the caller prints `name()` beside them -- a zero from a backend that has
    // no pool is not evidence of a clean pool.
    virtual void clt_target_pool(uint64_t& releases, uint64_t& refusals,
                                 uint64_t& pooled) const
    {
        releases = 0;
        refusals = 0;
        pooled = 0;
    }

    // --- clean-renderer extensions -----------------------------------------
    // Implemented by clean_renderer.cpp; every other backend keeps the safe
    // no-op defaults here so the fallback chain stays compile-compatible.
    //
    // Upload a lit, optionally textured mesh. Arrays are tightly packed:
    // pos xyz (3*n), normals (3*n, null => smooth-computed), uv (2*n, null =>
    // zero), rgba colors (4*n, null => white), u16 triangle indices.
    virtual MeshId mesh_upload_lit(int vertex_count, const float* pos_xyz,
                                   const float* nrm_xyz, const float* uv_xy,
                                   const float* col_rgba, int index_count,
                                   const uint16_t* indices)
    {
        return kInvalidMesh;
    }

    // Full-state 3D draw: per-vertex sun lighting, perspective-correct
    // texture, distance fog. All matrices column-major.
    struct MeshDrawState {
        float model[16];       // object -> world
        float view_proj[16];   // world -> clip
        float cam_pos[3];
        float sun_dir[3];      // unit vector toward the sun
        float sun_rgb[3];
        float ambient_rgb[3];
        float fog_rgb[3];
        float fog_far;         // <= 0 disables fog
        CelId texture;         // kInvalidCel = vertex colors only
        float alpha;           // 1 = opaque
    };

    virtual void mesh_draw_state(MeshId mesh, const MeshDrawState& st) {}

    // Planar projected shadow onto the y = ground_y plane along sun_dir
    // (scene-ward, i.e. pointing down). Stencil-guarded against
    // double-darkening where the context provides stencil bits.
    virtual void mesh_draw_shadow(MeshId mesh, const float model[16],
                                  const float view_proj[16],
                                  const float sun_dir[3], float ground_y,
                                  float strength) {}

    // RGBA upload with 3D sampling: bilinear + mipmaps (no cel crawl),
    // optional repeat wrap. Classic cel_upload_rgba stays NEAREST for HUD
    // pixel art parity.
    virtual CelId cel_upload_rgba_ex(int w, int h, const void* pixels,
                                     bool mipmap_linear, bool wrap_repeat)
    {
        return cel_upload_rgba(w, h, pixels);
    }
    // Same upload contract with a caller-supplied FNV-1a identity over the
    // packed RGBA bytes. The GL backends key their retired-texture cache on
    // this identity, so a matching key reuses a resident texture without
    // retaining a CPU copy to compare against; the identity only exists to
    // avoid re-scanning a large raster when the interpreter's decode/material
    // cache already computed it.
    virtual CelId cel_upload_rgba_ex_hashed(int w, int h, const void* pixels,
                                             bool mipmap_linear,
                                             bool wrap_repeat,
                                             uint64_t content_hash)
    {
        (void)content_hash;
        return cel_upload_rgba_ex(w, h, pixels, mipmap_linear, wrap_repeat);
    }

    // True when this backend samples the guest's own MIP chain through
    // cel_upload_rgba_lod_chain. The interpreter asks before decoding coarser
    // authored levels, so a backend that cannot consume them does not pay for
    // a decode whose result it would discard.
    virtual bool clt_supports_authored_lod_chain() const { return false; }

    // RGBA upload of a guest-authored MIP chain. `base_pixels` is the level the
    // draw selected; `levels[0..level_count-1]` are the coarser levels in
    // increasing-LOD order. CLT_CreateTxLoadCommandListMMDMA accumulates one
    // texelDataSize per LOD into the lodBase[] chain and CLT_TxLoad4LOD
    // publishes those distinct payloads, so the guest's own coarser bytes --
    // not a host reduction of level 0 -- decide minified output. A backend that
    // does not override this keeps the single-level generated-mipmap behavior
    // of cel_upload_rgba_ex. `content_hash` is the level-0 identity, with the
    // same contract as cel_upload_rgba_ex_hashed; 0 means compute it here.
    // `lod_chain_hash` is the cel_lod_chain_hash identity of `levels` when the
    // caller already computed it; 0 means compute it here. A backend may use
    // the supplied chain identity only when it accepts every supplied level,
    // and must hash the accepted prefix itself otherwise.
    virtual CelId cel_upload_rgba_lod_chain(int w, int h, const void* base_pixels,
                                            const CelLodLevel* levels,
                                            size_t level_count,
                                            bool mipmap_linear,
                                            bool wrap_repeat,
                                            uint64_t content_hash = 0,
                                            uint64_t lod_chain_hash = 0)
    {
        (void)levels;
        (void)level_count;
        (void)content_hash;
        (void)lod_chain_hash;
        return cel_upload_rgba_ex(w, h, base_pixels, mipmap_linear, wrap_repeat);
    }

    // Optional backend self-test content. It is not application behavior.
    virtual void render_selftest_scene(int frame) {}

    virtual void frame_begin(float clear_r, float clear_g, float clear_b) = 0;
    virtual void frame_present() = 0;

    virtual const char* name() const = 0;
private:
    std::vector<uint32_t> m_scanout;
    bool m_crop_overscan = false;
    const uint32_t* m_scanout_view = nullptr;
    uint64_t m_scanout_hash = 0;
    CelId m_scanout_cel = kInvalidCel;
    uint64_t m_scanout_cel_hash = 0;
    int m_scanout_cel_w = 0, m_scanout_cel_h = 0;
    int m_scanout_w = 0, m_scanout_h = 0;
};

// GL backend factory (returns nullptr where GL is unavailable).
std::unique_ptr<ICelBlitterBackend> make_gl_backend();

// Clean renderer factory (nullptr where GL is unavailable); the
// preferred backend, with gl_backend as fallback.
std::unique_ptr<ICelBlitterBackend> make_clean_renderer();

// No-op backend: keeps the app logic running headless / unsupported-GL.
class NullBlitter : public ICelBlitterBackend {
public:
    bool init(SDL_Window*) override { return true; }
    void shutdown() override {}
    CelId cel_upload_rgba(int w, int h, const void*) override
    {
        return static_cast<CelId>(++m_last);
    }
    void cel_free(CelId) override {}
    void cel_blit(CelId, float, float, float, float, float, float, float,
                  float) override {}
    void frame_begin(float, float, float) override {}
    void frame_present() override {}
    const char* name() const override { return "null"; }

private:
    uint64_t m_last = 0;
};

}  // namespace m2host
