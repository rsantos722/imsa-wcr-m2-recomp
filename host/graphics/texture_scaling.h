// Presentation-only texture and bitmap sampling enhancements shared by the
// two OpenGL CLT backends.
//
// M2_TEXTURE_FILTER  guest (unset) | linear | sharp
//     Sampling of primary decoded CLT textures. `linear` is the existing
//     bilinear override; `sharp` is "sharp bilinear": texels stay crisp when
//     magnified, with a one-screen-pixel blended seam between them, and fall
//     back to (trilinear) bilinear under minification.
// M2_BITMAP_FILTER   nearest (unset) | linear | sharp
//     Sampling of the direct Projector bitmap scanout (menus, stills, FMV)
//     when it is scaled to the window.
// M2_HIRES_EFFECTS   unset | 1
//     Framebuffer-feedback (display-carve DB-source) snapshots keep the host
//     target resolution instead of being reduced to the guest's texel grid.
//
// None of these change guest state, decoded bytes, SSB masks or the order of
// guest work. Feedback snapshots and SSB masks keep guest sampling.
#pragma once

#include <cstdlib>
#include <cstring>

namespace m2host {

enum class TextureFilterMode { guest, linear, sharp };

inline TextureFilterMode texture_filter_mode_from(const char* value,
                                                  TextureFilterMode fallback) {
    if (!value || !value[0]) return fallback;
    if (std::strcmp(value, "linear") == 0) return TextureFilterMode::linear;
    if (std::strcmp(value, "sharp") == 0) return TextureFilterMode::sharp;
    if (std::strcmp(value, "nearest") == 0 || std::strcmp(value, "guest") == 0)
        return TextureFilterMode::guest;
    return fallback;
}

inline TextureFilterMode texture_filter_request() {
    return texture_filter_mode_from(std::getenv("M2_TEXTURE_FILTER"),
                                    TextureFilterMode::guest);
}

inline TextureFilterMode bitmap_filter_request() {
    return texture_filter_mode_from(std::getenv("M2_BITMAP_FILTER"),
                                    TextureFilterMode::guest);
}

inline bool hires_effects_requested() {
    const char* value = std::getenv("M2_HIRES_EFFECTS");
    return value && value[0] && !(value[0] == '0' && value[1] == '\0');
}

}  // namespace m2host

// Fragment helpers shared by both CLT programs. `u_sharp` selects sharp
// bilinear for the primary texture (the sampler must be linear); a texel's
// interior samples its own centre and only the last screen pixel before a
// texel boundary blends, so magnified art stays crisp without the uneven
// pixel widths of nearest sampling at non-integer scales. Under minification
// the blend region covers the texel and this is ordinary bilinear.
#define M2_SHARP_SAMPLE_GLSL                                                   \
    "uniform int u_sharp;\n"                                                   \
    "vec2 sharp_uv(vec2 uv, vec2 size, vec2 gx, vec2 gy) {\n"                  \
    "    vec2 texel = uv * size;\n"                                            \
    "    vec2 footprint = max((abs(gx) + abs(gy)) * size, vec2(1e-4));\n"      \
    "    vec2 scale = max(1.0 / footprint, vec2(1.0));\n"                      \
    "    vec2 region = 0.5 - 0.5 / scale;\n"                                   \
    "    vec2 center_dist = fract(texel) - 0.5;\n"                             \
    "    vec2 f = (center_dist - clamp(center_dist, -region, region)) * scale" \
    " + 0.5;\n"                                                                \
    "    return (floor(texel) + f) / size;\n"                                  \
    "}\n"
