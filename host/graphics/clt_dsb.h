#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace m2host {
// Raw destination-blender registers. Kept independent of backend fast paths.
struct CltGeneralDsb {
    uint32_t a = 0, b = 0, alu = 0, constant = 0;
    std::array<uint32_t, 2> a_constant{}, b_constant{};
    uint32_t source_alpha = 0, dest_alpha = 0, alpha_constant = 0;
};
// Whether either consumed RGB product depends on texture-stage color/alpha.
// Written alpha is separate; callers may bypass stale texture state only when
// its written-alpha result is already an explicitly disclosed approximation.
inline bool clt_dsb_rgb_uses_texture(const CltGeneralDsb& s) {
    const unsigned op = (s.alu >> 3u) & 31u;
    const bool uses_a = op < 16u || ((op & 3u) != ((op >> 2u) & 3u));
    const bool uses_b = op < 16u || ((op & 5u) != ((op >> 1u) & 5u));
    const auto needs_texture = [](uint32_t raw, bool aside) {
        const unsigned input = (raw >> 6u) & 3u, coef = (raw >> 1u) & 24u;
        return input == 3u || (aside ? input == 0u : input == 2u) ||
               coef == 0u || (!aside && coef == 24u);
    };
    return (uses_a && needs_texture(s.a, true)) || (uses_b && needs_texture(s.b, false));
}
inline float clt_dsb_channel(uint32_t word, unsigned shift) {
    return float((word >> shift) & 255u) / 255.f;
}
inline float clt_dsb_clamp_alpha(float alpha, uint32_t control) {
    const unsigned shift = alpha <= 0.f ? 0u : alpha >= 1.f ? 4u : 2u;
    const unsigned action = (control >> shift) & 3u;
    return action == 1u ? 1.f : action == 2u ? 0.f : alpha;
}
inline float clt_dsb_product(uint32_t control, bool a_side, float texture,
                             float source, float ta, float sa, float constant,
                             float coefficient) {
    const unsigned input = (control >> 6) & 3u;
    float value = input == 0 ? (a_side ? texture : source) :
                  input == 1 ? constant : input == 2 ?
                  1.f - (a_side ? source : texture) : ta;
    if (control & 1u) value = std::floor(value * 255.f / 8.f) / 255.f;
    const unsigned select = (control >> 1) & 0x18u;
    float weight = select == 0 ? ta : select == 8 ? sa :
                   select == 16 ? coefficient : (a_side ? source : texture);
    if (control & 2u) weight = 1.f - weight;
    return value * weight;
}
inline float clt_dsb_alu_channel(float a, float b, uint32_t control) {
    const unsigned op = (control >> 3) & 31u;
    const float divisor = float(1u << (control & 7u));
    if (op < 16u) {
        float value = op < 8u ? a + b : op < 12u ? a - b : b - a;
        // Saturating and wrapping operations differ before the final divide.
        if (!(op & 2u)) value = std::clamp(value, 0.f, 1.f);
        else value = float((int(std::floor(value * 255.f + .5f)) % 256 + 256) % 256) / 255.f;
        return value / divisor;
    }
    const unsigned x = unsigned(std::lround(std::clamp(a, 0.f, 1.f) * 255.f));
    const unsigned y = unsigned(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
    // The low nibble is the two-input Boolean truth table (00,01,10,11).
    unsigned value = 0;
    if (op & 1u) value |= ~x & ~y;
    if (op & 2u) value |= ~x & y;
    if (op & 4u) value |= x & ~y;
    if (op & 8u) value |= x & y;
    return float(value & 255u) / (255.f * divisor);
}
inline std::array<float,4> clt_dsb_evaluate(const CltGeneralDsb& s,
        std::array<float,4> texture, std::array<float,4> source,
        bool texture_ssb, bool source_ssb) {
    source[3] = clt_dsb_clamp_alpha(source[3], s.source_alpha);
    const bool ai = (s.a & 4u) ? source_ssb : texture_ssb;
    const bool bi = (s.b & 4u) ? source_ssb : texture_ssb;
    std::array<float,4> out{};
    for (unsigned c = 0; c < 3; ++c) {
        const unsigned shift = 16u - c * 8u;
        const float constant = clt_dsb_channel(s.constant, shift);
        const float a = clt_dsb_product(s.a, true, texture[c], source[c],
            texture[3], source[3], constant, clt_dsb_channel(s.a_constant[ai], shift));
        const float b = clt_dsb_product(s.b, false, texture[c], source[c],
            texture[3], source[3], constant, clt_dsb_channel(s.b_constant[bi], shift));
        out[c] = clt_dsb_alu_channel(a, b, s.alu);
    }
    out[3] = s.dest_alpha == 0 ? texture[3] : s.dest_alpha == 2 ? source[3] :
             s.dest_alpha == 3 ? out[0] : clt_dsb_channel(s.alpha_constant,
                 (s.dest_alpha == 9 ? source_ssb : texture_ssb) ? 0 : 16);
    return out;
}
// Optional RGB555 output dithering (opt-in, M2_RGB555_DITHER). The guest's
// DBUSERCONTROL.DITHEREN plus the 4x4 signed 4-bit DBDITHERMATRIX pair select
// a per-pixel offset (toolkit CLT-62/CLT-82): the low two bits of the
// destination X/Y index the matrix, and the offset is added to R, G and B
// before the value is stored as 555. `matrix_a` holds rows 0-1, `matrix_b`
// rows 2-3; nibble `7 ^ ((y&1)<<2 | x&3)` of the selected word is the entry
// (checked against the toolkit's appendix pattern 0xC0D12E3F/0xE1D0302F).
// `enabled` is only ever set when the host option is on, so the default image
// is untouched.
struct CltDither {
    bool enabled = false;
    uint32_t matrix_a = 0;
    uint32_t matrix_b = 0;
};
inline int clt_dither_offset(const CltDither& d, int x, int y) {
    const uint32_t word = (y & 2) != 0 ? d.matrix_b : d.matrix_a;
    const uint32_t index = 7u ^ uint32_t((((y & 1) << 2) | (x & 3)));
    const int nibble = int((word >> (index * 4u)) & 15u);
    return nibble >= 8 ? nibble - 16 : nibble;
}
// Add the offset, clamp, and truncate to five bits with MSB replication back
// to eight (the host's RGB555 expansion).
inline uint32_t clt_dither_channel(uint32_t value, int offset) {
    const int v = std::clamp(int(value) + offset, 0, 255);
    const uint32_t q = uint32_t(v) >> 3u;
    return (q << 3u) | (q >> 2u);
}
// Dither the RGB bytes of an ABGR-packed pixel (R in bits 0-7); alpha is kept.
inline uint32_t clt_dither_pixel(const CltDither& d, uint32_t rgba, int x,
                                 int y) {
    const int o = clt_dither_offset(d, x, y);
    return clt_dither_channel(rgba & 255u, o) |
           (clt_dither_channel((rgba >> 8u) & 255u, o) << 8u) |
           (clt_dither_channel((rgba >> 16u) & 255u, o) << 16u) |
           (rgba & 0xff000000u);
}
} // namespace m2host

#define M2_CLT_DSB_GLSL \
"\n" \
"uniform sampler2D u_source_ssb;\n" \
"uniform int u_has_source_ssb, u_has_object_ssb, u_dsb_select;\n" \
"uniform int u_dsb_has_ssb, u_dsb_a, u_dsb_b, u_dsb_alu, u_dsb_src_alpha, u_dsb_dest_alpha;\n" \
"uniform vec4 u_dsb_constant, u_dsb_a0, u_dsb_a1, u_dsb_b0, u_dsb_b1;\n" \
"uniform vec4 u_dsb_alpha;\n" \
"float dsb_product(int control, bool a_side, float t, float s, float ta, float sa, float c, float k) {\n" \
"    int input_sel = (control >> 6) & 3;\n" \
"    float value = input_sel == 0 ? (a_side ? t : s) : input_sel == 1 ? c : input_sel == 2 ? 1.0-(a_side ? s : t) : ta;\n" \
"    if ((control & 1) != 0) value = floor(value*255.0/8.0)/255.0;\n" \
"    int sel = (control >> 1) & 24;\n" \
"    float weight = sel == 0 ? ta : sel == 8 ? sa : sel == 16 ? k : (a_side ? s : t);\n" \
"    if ((control & 2) != 0) weight = 1.0-weight;\n" \
"    return value*weight;\n" \
"}\n" \
"float dsb_alu(float a, float b) {\n" \
"    int op = (u_dsb_alu >> 3) & 31;\n" \
"    float divisor = float(1 << (u_dsb_alu & 7));\n" \
"    if (op < 16) {\n" \
"        float v = op < 8 ? a+b : op < 12 ? a-b : b-a;\n" \
"        if ((op & 2) == 0) v = clamp(v,0.0,1.0);\n" \
"        else v = float((int(floor(v*255.0+0.5)) % 256 + 256) % 256)/255.0;\n" \
"        return v/divisor;\n" \
"    }\n" \
"    int x = int(floor(clamp(a,0.0,1.0)*255.0+0.5));\n" \
"    int y = int(floor(clamp(b,0.0,1.0)*255.0+0.5));\n" \
"    int v = 0;\n" \
"    if ((op & 1) != 0) v |= ~x & ~y;\n" \
"    if ((op & 2) != 0) v |= ~x & y;\n" \
"    if ((op & 4) != 0) v |= x & ~y;\n" \
"    if ((op & 8) != 0) v |= x & y;\n" \
"    return float(v & 255)/(255.0*divisor);\n" \
"}\n" \
"vec4 dsb_apply(vec4 t, vec4 s, float tssb, float sssb) {\n" \
"    int shift = s.a <= 0.0 ? 0 : s.a >= 1.0 ? 4 : 2;\n" \
"    int action = (u_dsb_src_alpha >> shift) & 3;\n" \
"    if (action == 1) s.a = 1.0;\n" \
"    if (action == 2) s.a = 0.0;\n" \
"    vec4 ak = ((u_dsb_a & 4) != 0 ? sssb : tssb) >= 0.5 ? u_dsb_a1 : u_dsb_a0;\n" \
"    vec4 bk = ((u_dsb_b & 4) != 0 ? sssb : tssb) >= 0.5 ? u_dsb_b1 : u_dsb_b0;\n" \
"    vec4 result;\n" \
"    for (int i=0; i<3; ++i) result[i] = dsb_alu(\n" \
"        dsb_product(u_dsb_a,true,t[i],s[i],t.a,s.a,u_dsb_constant[i],ak[i]),\n" \
"        dsb_product(u_dsb_b,false,t[i],s[i],t.a,s.a,u_dsb_constant[i],bk[i]));\n" \
"    result.a = u_dsb_dest_alpha == 0 ? t.a : u_dsb_dest_alpha == 2 ? s.a :\n" \
"        u_dsb_dest_alpha == 3 ? result.r :\n" \
"        ((u_dsb_dest_alpha == 9 ? sssb : tssb) >= 0.5 ? u_dsb_alpha.y : u_dsb_alpha.x);\n" \
"    return result;\n" \
"}\n"

// Fragment-stage form of the same operation. `v_guest_xy` is the fragment's
// guest-resolution pixel position (the vertex position interpolated in screen
// space), so the 4x4 pattern stays locked to guest pixels at any host scale.
// Applied to the fragment color before fixed-function blending: exact for
// opaque draws, an approximation for blended ones.
#define M2_CLT_DITHER_GLSL \
"\n" \
"uniform int u_dither_enable;\n" \
"uniform int u_dither_a;\n" \
"uniform int u_dither_b;\n" \
"vec3 dither_rgb555(vec3 c) {\n" \
"    ivec2 p = ivec2(floor(v_guest_xy)) & 3;\n" \
"    uint word = uint((p.y & 2) != 0 ? u_dither_b : u_dither_a);\n" \
"    uint idx = 7u ^ uint(((p.y & 1) << 2) | p.x);\n" \
"    int n = int((word >> (idx * 4u)) & 15u);\n" \
"    if (n >= 8) n -= 16;\n" \
"    vec3 v = clamp(floor(c * 255.0 + 0.5) + float(n), 0.0, 255.0);\n" \
"    vec3 q = floor(v / 8.0);\n" \
"    return (q * 8.0 + floor(q / 4.0)) / 255.0;\n" \
"}\n"
