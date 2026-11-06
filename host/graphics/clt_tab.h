#pragma once

#include <array>
#include <cstdint>

namespace m2host {
// TXTTABCNTL: toolkit chapter 3, Texture Application Blender. Bypassed
// outputs do not consume their selectors; MULT does not consume input C.
struct CltTabState {
    bool enabled = false;
    uint32_t control = 0;
    std::array<uint32_t, 2> constants{};
};
inline bool clt_tab_documented(uint32_t word) {
    const unsigned co = (word >> 9) & 3, ao = (word >> 15) & 3;
    return !(word & ~0x3ffffu) && co < 3 && ao < 3 &&
        (co != 2 || ((word & 7) < 6 && ((word >> 3) & 7) < 6 &&
                     ((word & 0x20000) || ((word >> 6) & 7) < 6))) &&
        (ao != 2 || (((word >> 11) & 3) < 3 && ((word >> 13) & 3) < 3));
}
using CltTabColor = std::array<float, 4>;
inline CltTabColor clt_tab_constant(uint32_t word) {
    return {float((word >> 16) & 255) / 255.f,
            float((word >> 8) & 255) / 255.f,
            float(word & 255) / 255.f,
            float((word >> 24) & 127) / 255.f};
}
inline CltTabColor clt_tab_evaluate(const CltTabState& state,
                                   const CltTabColor& primitive,
                                   const CltTabColor& texture, bool ssb) {
    const auto constant = clt_tab_constant(state.constants[ssb ? 1 : 0]);
    const auto color = [&](unsigned selector, unsigned channel) {
        switch (selector) {
        case 0: return primitive[3]; case 1: return primitive[channel];
        case 2: return texture[3]; case 3: return texture[channel];
        case 4: return constant[3]; default: return constant[channel];
        }
    };
    const auto alpha = [&](unsigned selector) {
        return selector == 0 ? primitive[3] : selector == 1 ? texture[3] : constant[3];
    };
    const uint32_t w = state.control;
    CltTabColor out{};
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned co = (w >> 9) & 3;
        if (co < 2) out[i] = co ? texture[i] : primitive[i];
        else {
            const float a = color(w & 7, i), b = color((w >> 3) & 7, i);
            const float t = color((w >> 6) & 7, i);
            out[i] = (w & 0x20000) ? a * b : a * (1.f - t) + b * t;
        }
    }
    const unsigned ao = (w >> 15) & 3;
    out[3] = ao < 2 ? (ao ? texture[3] : primitive[3]) :
        alpha((w >> 11) & 3) * alpha((w >> 13) & 3);
    return out;
}
} // namespace m2host

// Shared by both GL backends; inputs and selector layout match the CPU path.
#define M2_CLT_TAB_GLSL \
"\n" \
"uniform int u_tab_control;\n" \
"uniform vec4 u_tab_constant0;\n" \
"uniform vec4 u_tab_constant1;\n" \
"vec3 tab_color(int s, vec4 p, vec4 t, vec4 c) {\n" \
"    if (s == 0) return vec3(p.a);\n" \
"    if (s == 1) return p.rgb;\n" \
"    if (s == 2) return vec3(t.a);\n" \
"    if (s == 3) return t.rgb;\n" \
"    if (s == 4) return vec3(c.a);\n" \
"    return c.rgb;\n" \
"}\n" \
"float tab_alpha(int s, vec4 p, vec4 t, vec4 c) {\n" \
"    return s == 0 ? p.a : s == 1 ? t.a : c.a;\n" \
"}\n" \
"vec4 tab_apply(vec4 p, vec4 t, float ssb) {\n" \
"    int w = u_tab_control;\n" \
"    vec4 c = (w & 0x100000) != 0 && ssb >= 0.5 ? u_tab_constant1 : u_tab_constant0;\n" \
"    int co = (w >> 9) & 3;\n" \
"    int ao = (w >> 15) & 3;\n" \
"    vec3 a = tab_color(w & 7, p, t, c);\n" \
"    vec3 b = tab_color((w >> 3) & 7, p, t, c);\n" \
"    vec3 factor = tab_color((w >> 6) & 7, p, t, c);\n" \
"    vec3 rgb = co == 0 ? p.rgb : co == 1 ? t.rgb :\n" \
"        (w & 0x20000) != 0 ? a * b : mix(a, b, factor);\n" \
"    float alpha = ao == 0 ? p.a : ao == 1 ? t.a :\n" \
"        tab_alpha((w >> 11) & 3, p, t, c) * tab_alpha((w >> 13) & 3, p, t, c);\n" \
"    return vec4(rgb, alpha);\n" \
"}\n"
