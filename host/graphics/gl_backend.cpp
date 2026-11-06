// gl_backend.cpp — OpenGL cel-blitter + clean-3D backend.
//
// One shader, one dynamic VBO, one draw per blit. Classic cel
// textures remain RGBA8888, NEAREST-filtered (pixel-art fidelity),
// CLAMP_TO_EDGE; guest CLT draws carry their own lookup state.
// The clean-GL mesh path is fed by the glTF track
// exports — second program, static VAO per mesh, distance fog — plus
// fill_rect/scissor primitives for the HUD. All entry points are loaded
// through SDL_GL_GetProcAddress so we ship no GL loader dependency.
// Context version is requested by main() (4.1-core on macOS, 4.5 elsewhere).
#include "cel_blitter.h"
#include "cel_texture_cache.h"
#include "gl_clt_targets.h"
#include "letterbox.h"
#include "texture_replacements.h"
#include "texture_scaling.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif

// GL_EXT_texture_filter_anisotropic (the 4.6 core ARB form shares these
// tokens). Apple's core-profile headers omit the extension tokens.
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_MAX_RENDERBUFFER_SIZE
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#endif

namespace m2host {

namespace {

// OpenGL sees reciprocal-W mapped monotonically into its depth range. Keep
// the DBZCNTL mapping in cel_blitter.h so the primary and CleanGL backends
// cannot disagree about the guest pixel relation.
GLenum gl_depth_func_for_compare(CltBackendDepthCompare compare)
{
    switch (compare) {
    case CltBackendDepthCompare::never: return GL_NEVER;
    case CltBackendDepthCompare::less: return GL_LESS;
    case CltBackendDepthCompare::equal: return GL_EQUAL;
    case CltBackendDepthCompare::less_equal: return GL_LEQUAL;
    case CltBackendDepthCompare::greater: return GL_GREATER;
    case CltBackendDepthCompare::not_equal: return GL_NOTEQUAL;
    case CltBackendDepthCompare::greater_equal: return GL_GEQUAL;
    case CltBackendDepthCompare::always: return GL_ALWAYS;
    }
    return GL_NEVER;
}

GLenum gl_depth_func_for_dbz(uint32_t depth_func)
{
    return gl_depth_func_for_compare(
        clt_backend_depth_compare_for_dbz(depth_func));
}

GLenum gl_depth_func_for_dbz_z_update(uint32_t depth_func)
{
    return gl_depth_func_for_compare(
        clt_backend_depth_compare_for_dbz_z_update(depth_func));
}

GLint gl_texture_min_filter(const CltTextureSampling& sampling,
                            bool has_mips)
{
    const GLint base = sampling.min_filter == CltTextureFilter::point
                           ? GL_NEAREST
                           : GL_LINEAR;
    if (!has_mips || sampling.lod_max == 0u) return base;
    if (sampling.inter_filter == CltTextureFilter::point)
        return base == GL_NEAREST ? GL_NEAREST_MIPMAP_NEAREST
                                  : GL_LINEAR_MIPMAP_NEAREST;
    return base == GL_NEAREST ? GL_NEAREST_MIPMAP_LINEAR
                              : GL_LINEAR_MIPMAP_LINEAR;
}

GLint gl_texture_mag_filter(const CltTextureSampling& sampling)
{
    return sampling.mag_filter == CltTextureFilter::point ? GL_NEAREST
                                                           : GL_LINEAR;
}

struct Vertex {
    float x, y, z, w;  // window px (xy), depth (z), perspective scale (w)
    float u, v;
    float r, g, b, a;
    float mask_u, mask_v;
};

const char* kVertSrc = R"(#version 330 core
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec2 a_mask_uv;
uniform vec2 u_res;
noperspective out vec2 v_db_uv;
noperspective out vec2 v_db_mask_uv;
noperspective out vec2 v_guest_xy;
out vec2 v_uv;
out vec4 v_col;
out vec2 v_mask_uv;
void main() {
    v_guest_xy = a_pos.xy;
    vec2 ndc = vec2(a_pos.x / u_res.x * 2.0 - 1.0,
                    1.0 - a_pos.y / u_res.y * 2.0);
    gl_Position = vec4(ndc * a_pos.w, a_pos.z * a_pos.w, a_pos.w);
    v_db_uv = a_uv; v_db_mask_uv = a_mask_uv;
    v_uv = a_uv;
    v_col = a_col;
    v_mask_uv = a_mask_uv;
}
)";

const char* kFragSrc = R"(#version 330 core
noperspective in vec2 v_db_uv;
noperspective in vec2 v_db_mask_uv;
noperspective in vec2 v_guest_xy;
in vec2 v_uv;
in vec4 v_col;
in vec2 v_mask_uv;
uniform sampler2D u_tex;
uniform sampler2D u_ssb_mask;
uniform sampler2D u_secondary_source;
uniform int u_separate_ssb_uv;
// TXTUVMASK address period as a fraction of the resident extent (< 1 only
// when the guest's mask period is smaller than the bound texture, where the
// masked sub-range -- not the whole texture -- repeats).
uniform vec2 u_uv_period;
uniform int u_discard_ssb;
uniform int u_discard_alpha;
uniform int u_discard_rgb;
uniform int u_blend_mode;
uniform int u_tab_alpha_color;
)" M2_CLT_TAB_GLSL M2_CLT_DSB_GLSL M2_CLT_DITHER_GLSL R"(
uniform vec3 u_blend_const;
uniform vec3 u_blend_weight;
uniform int u_db_source_linear;
uniform float u_db_source_height;
uniform int u_feedback_source;
)" M2_SHARP_SAMPLE_GLSL R"(
vec2 db_source_uv(vec2 coord) {
    if (u_db_source_linear == 1 && u_db_source_height > 0.0) {
        float carry = floor(coord.x);
        return vec2(coord.x - carry,
                    coord.y + carry / u_db_source_height);
    }
    return coord;
}
// Per-texel SSB coefficient selection: 0 off, 1 keep SSB-0 texels (bit < 0.5),
// 2 keep SSB-1 texels (bit >= 0.5).  Each pass is uniform in its own
// coefficient entry, which is exactly the two-equation split the draw's
// source-select-bit signal produces.
uniform int u_ssb_keep;
layout(location=0) out vec4 frag;
layout(location=1) out vec4 frag_ssb;
void main() {
    // DB-source addresses follow screen X/Y, regardless of triangle W.
    vec2 uv = u_db_source_linear == 1 && (u_blend_mode != 7 && u_blend_mode != 9 && u_blend_mode != 10 && u_blend_mode != 13)
        ? v_db_uv : v_uv;
    vec2 ddx = dFdx(uv);
    vec2 ddy = dFdy(uv);
    if (u_uv_period.x < 1.0) uv.x = fract(uv.x / u_uv_period.x) * u_uv_period.x;
    if (u_uv_period.y < 1.0) uv.y = fract(uv.y / u_uv_period.y) * u_uv_period.y;
    if ((u_blend_mode != 7 && u_blend_mode != 9 && u_blend_mode != 10 && u_blend_mode != 13)) uv = db_source_uv(uv);
    vec2 sample_uv = u_sharp == 1
        ? sharp_uv(uv, vec2(textureSize(u_tex, 0)), ddx, ddy) : uv;
    vec4 tex = textureGrad(u_tex, sample_uv, ddx, ddy);
    if (u_feedback_source == 1) tex.a = 1.0;
    if (u_tab_alpha_color == 1) tex = vec4(vec3(tex.a), 1.0);
    vec2 mask_uv = u_separate_ssb_uv == 1 ? v_mask_uv : uv;
    float ssb_bit = texture(u_ssb_mask, mask_uv).r;
    if (u_discard_ssb == 1 && ssb_bit < 0.5) discard;
    if (u_ssb_keep == 1 && ssb_bit >= 0.5) discard;
    if (u_ssb_keep == 2 && ssb_bit < 0.5) discard;
    vec4 output_color = u_tab_control >= 0 ? tab_apply(v_col, tex, ssb_bit) : tex * v_col;
    float pipeline_alpha = output_color.a;
    float stage_alpha = u_tab_control >= 0 ? output_color.a : v_col.a;
    // Proven opaque PRIMALPHA; texture alpha is not the TAB alpha output.
    if (u_blend_mode == 11) output_color.a = 1.0;
    if (u_discard_rgb == 1 && output_color.rgb == vec3(0.0)) discard;
    if (u_blend_mode == 2) {
        output_color.rgb =
            min(vec3(1.0), output_color.rgb + u_blend_const * stage_alpha);
        output_color.a = stage_alpha;
    }
    if (u_blend_mode == 8) {
        // M_DBFog composite: A = TEXCOLOR*TEXALPHA, B = CONSTCOLOR*(1-TEXALPHA)
        // with SRCEN clear. TEXALPHA is the per-vertex fog factor (PRIMALPHA),
        // so the texture-stage color is lerped toward the guest's DBCONSTIN fog
        // color by that factor. Blending stays disabled (clt_gl_blend_plan).
        // M_DBFog composites opaque world geometry (the transparent fog path
        // is M_TBFog + M_DBTrans), so the result is opaque; the fog factor is a
        // lerp weight, not an opacity.
        output_color.rgb = mix(u_blend_const, output_color.rgb, stage_alpha);
        output_color.a = 1.0;
    }
    if (u_blend_mode == 3) {
        // Guest A = CONSTCOLOR * TEXALPHA with B = SRCCOLOR * (1 - TEXALPHA):
        // the guest's DBCONSTIN color is blended over the destination by the
        // texture-stage alpha.  GL_BLEND supplies the (1 - alpha) destination
        // term, so the fragment color is the constant and the alpha stays the
        // post-texturing alpha used by the alpha0 discard above.
        output_color.rgb = u_blend_const;
    }
    if (u_blend_mode == 6) {
        // Toolkit two-input weighted mix: the A-side multiplier coefficient
        // pair scales the texture-blender output here and GL's
        // CONSTANT_COLOR destination factor supplies the B-side weight, so the
        // framebuffer receives `TEXCOLOR * a + SRCCOLOR * b`.  The alpha stays
        // the post-texturing alpha, which is the destination-alpha source the
        // guest's setup selects (`DESTCONSTSELECT = TEXALPHA`).
        output_color.rgb *= u_blend_weight;
    }
    if (u_blend_mode == 7 || u_blend_mode == 9 || u_blend_mode == 10) {
        // StreamEventFunc's reverse-alpha source-framebuffer equation:
        // A = TEXCOLOR*(1-TEXALPHA), B = SRCCOLOR*TEXALPHA, A+B.
        // v_mask_uv carries the independent DB-source coordinates for this
        // mode; it is not the primary texture's SSB lookup. If the interpreter
        // retained an unbounded linear address, resolve its row carry here.
        vec2 source_uv = db_source_uv(u_db_source_linear == 1
            ? v_db_mask_uv : v_mask_uv);
        vec4 secondary = texture(u_secondary_source, source_uv);
        if (u_blend_mode == 10) output_color.rgb = u_blend_const;
        float source_weight = u_blend_mode == 7 ? output_color.a : 1.0 - output_color.a;
        output_color.rgb = output_color.rgb * (1.0 - source_weight) +
                           secondary.rgb * source_weight;
    }
    if (u_blend_mode == 12 || u_blend_mode == 13) {
        if (u_discard_alpha == 1 && output_color.a == 0.0) discard;
        vec4 source = vec4(0.0);
        if (u_blend_mode == 13) source = texture(u_secondary_source,
            db_source_uv(u_db_source_linear == 1 ? v_db_mask_uv : v_mask_uv));
        if (u_has_source_ssb != 0) source.a = texture(u_source_ssb,
            (u_db_source_linear == 1 && u_blend_mode != 7 && u_blend_mode != 9 && u_blend_mode != 10 && u_blend_mode != 13 ? uv : db_source_uv(u_db_source_linear == 1 ? v_db_mask_uv : v_mask_uv))).g;
        output_color = dsb_apply(output_color, source,
            u_dsb_has_ssb != 0 ? ssb_bit : 0.0,
            u_has_source_ssb != 0 ? texture(u_source_ssb,
                (u_db_source_linear == 1 && u_blend_mode != 7 && u_blend_mode != 9 && u_blend_mode != 10 && u_blend_mode != 13 ? uv : db_source_uv(u_db_source_linear == 1 ? v_db_mask_uv : v_mask_uv))).r : 0.0);
    }
    if (u_blend_mode != 12 && u_blend_mode != 13 && u_discard_alpha == 1 && output_color.a == 0.0) discard;
    float source_ssb = u_has_source_ssb != 0 ? texture(u_source_ssb,
        (u_db_source_linear == 1 && u_blend_mode != 7 && u_blend_mode != 9 && u_blend_mode != 10 && u_blend_mode != 13 ? uv : db_source_uv(u_db_source_linear == 1 ? v_db_mask_uv : v_mask_uv))).r : 0.0;
    source_ssb = step(0.5, source_ssb);
    float object_ssb = u_has_object_ssb != 0 ? ssb_bit : 0.0;
    float written_ssb = (u_dsb_select & 3) == 1 ? float((u_dsb_select >> 2) & 1) :
        (u_dsb_select & 3) == 2 ? source_ssb : object_ssb;
    float written_alpha = (u_blend_mode == 12 || u_blend_mode == 13) ? output_color.a :
        (u_blend_mode == 2 || u_blend_mode == 8) ? stage_alpha : pipeline_alpha;
    frag_ssb = vec4(written_ssb, written_alpha, 0.0, 1.0);
    // Opt-in RGB555 dithering of the destination-blender output.
    if (u_dither_enable == 1) output_color.rgb = dither_rgb555(output_color.rgb);
    frag = output_color;
}
)";

// --- clean-3D program: world-space positions + vertex color,
// fogged toward the sky color with view distance.
const char* kMeshVertSrc = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_vp;
uniform vec3 u_cam;
out vec4 v_col;
out float v_dist;
void main() {
    gl_Position = u_vp * vec4(a_pos, 1.0);
    v_col = a_col;
    v_dist = length(a_pos - u_cam);
}
)";

const char* kMeshFragSrc = R"(#version 330 core
in vec4 v_col;
in float v_dist;
uniform vec3 u_fog_color;
uniform float u_fog_far;
out vec4 frag;
void main() {
    vec3 c = v_col.rgb;
    if (u_fog_far > 0.0) {
        float f = clamp(v_dist / u_fog_far, 0.0, 1.0);
        c = mix(c, u_fog_color, f * f);
    }
    frag = vec4(c, v_col.a);
}
)";

class GlBackend final : public ICelBlitterBackend {
public:
    bool init(SDL_Window* window) override {
        m_window = window;
        m_ctx = SDL_GL_CreateContext(window);
        if (!m_ctx) {
            SDL_Log("gl_backend: context creation failed: %s", SDL_GetError());
            return false;
        }
        if (!load_procs()) {
            SDL_Log("gl_backend: required GL functions unavailable");
            return false;
        }
        m_replacements.load(std::getenv("M2_TEXTURE_REPLACEMENTS"));
        const TextureFilterMode filter = texture_filter_request();
        m_linear_filter = filter != TextureFilterMode::guest;
        m_sharp_filter = filter == TextureFilterMode::sharp;
        m_bitmap_filter = bitmap_filter_request();
        m_hires_effects = hires_effects_requested();
        // M2_ANISOTROPY: optional presentation request. Anisotropy only has an
        // effect under linear minification, so a granted request also engages
        // the linear override above. An unavailable extension or a device
        // maximum below 2x is refused with a log line; guest sampling stays in
        // force.
        m_anisotropy = clt_anisotropy_request();
        if (m_anisotropy > 1) {
            GLfloat max_anisotropy = 1.f;
            const bool supported =
                SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") ||
                SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic");
            if (supported)
                glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
            if (supported && max_anisotropy >= 2.f) {
                if (static_cast<GLfloat>(m_anisotropy) > max_anisotropy)
                    m_anisotropy = static_cast<int>(max_anisotropy);
                std::printf("[gfx] gl_backend anisotropic filtering %dx "
                            "(device max %gx)\n",
                            m_anisotropy,
                            static_cast<double>(max_anisotropy));
            } else {
                std::printf("[gfx] gl_backend anisotropic filtering "
                            "unavailable; keeping guest texture sampling\n");
                m_anisotropy = 1;
            }
        }
        if (!m_targets.init() || !build_program()) return false;
        // M2_ANTIALIASING: MSAA on the addressed CLT color targets. The
        // request is clamped to the context's GL_MAX_SAMPLES; a device that
        // cannot serve at least 2x keeps single-sample targets and says so
        // instead of silently presenting a fake setting.
        const int aa_request = clt_antialiasing_request();
        if (aa_request > 1) {
            const int max_samples = m_targets.usable_samples();
            if (max_samples >= 2) {
                const int samples =
                    aa_request < max_samples ? aa_request : max_samples;
                m_targets.set_samples(samples);
                std::printf("[gfx] gl_backend anti-aliasing %dx MSAA "
                            "(device max %dx)\n",
                            samples, max_samples);
            } else {
                std::printf("[gfx] gl_backend anti-aliasing unavailable; "
                            "keeping single-sample targets\n");
            }
        }
        // The depth attachment starts at the guest Z plane's far value (see
        // `kCltTeZFarDepth`); the guest's own clear-Z request supplies the
        // authored fill for each frame.
        m_targets.set_depth_clear(kCltTeZFarDepth);
        if (!build_mesh_program())
            SDL_Log("gl_backend: 3D program unavailable — track rendering disabled");

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, nullptr, GL_STREAM_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, r)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, mask_u)));

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // 1x1 white texture backing fill_rect through the cel pipeline.
        const uint32_t white = 0xFFFFFFFFu;
        glGenTextures(1, &m_white_tex);
        glBindTexture(GL_TEXTURE_2D, m_white_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, &white);

        // M2_RENDER_SCALE: fixed internal resolution, bounded by the
        // device's renderbuffer limit.
        m_render_scale = render_scale_request();
        if (m_render_scale > 0) {
            GLint max_rb = 0;
            glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
            const int granted = clamp_render_scale(m_render_scale, max_rb);
            if (granted != m_render_scale)
                std::printf("[gfx] gl_backend render scale %dx exceeds the "
                            "renderbuffer limit (%d); using %dx\n",
                            m_render_scale, max_rb, granted);
            m_render_scale = granted;
        }
        const LetterboxRect view = presentation_rect(window);
        m_present_w = view.w;
        m_present_h = view.h;
        int w = 0, h = 0;
        internal_render_size(m_render_scale, view.w, view.h, w, h);
        m_w = static_cast<float>(w);
        m_h = static_cast<float>(h);
        m_view_x = view.x;
        m_view_y = view.y;
        if (m_render_scale > 0)
            std::printf("[gfx] render scale %dx (%dx%d targets)\n",
                        m_render_scale, w, h);
        if (m_sharp_filter)
            std::printf("[gfx] texture filter sharp bilinear\n");
        if (m_bitmap_filter != TextureFilterMode::guest)
            std::printf("[gfx] bitmap filter %s\n",
                        m_bitmap_filter == TextureFilterMode::sharp ? "sharp"
                                                                    : "linear");
        if (m_hires_effects)
            std::printf("[gfx] high-resolution feedback effects\n");
        // M2_CROP_OVERSCAN (default on): present only the guest's 576x432
        // carve, scaled to fill; =0 keeps the original margin.
        set_overscan_crop(overscan_crop_requested());
        m_targets.set_crop_overscan(overscan_crop());
        std::printf("[gfx] overscan crop %s\n", overscan_crop() ? "on" : "off (M2_CROP_OVERSCAN=0)");
        std::printf("[gfx] %s backend ready (%dx%d px)\n", name(), w, h);
        return true;
    }

    void shutdown() override {
        if (std::getenv("M2_FRAME_TIMING"))
            std::fprintf(stderr,
                         "[texture-cache] backend=gl hits=%llu misses=%llu "
                         "resident=%zu bytes=%zu\n",
                         (unsigned long long)m_texture_cache_hits,
                         (unsigned long long)m_texture_cache_misses,
                         m_texture_cache.size(), m_texture_cache.bytes());
        m_targets.shutdown();
        for (auto& [id, mesh] : m_meshes) {
            (void)id;
            glDeleteBuffers(1, &mesh.ebo);
            glDeleteBuffers(1, &mesh.vbo);
            glDeleteVertexArrays(1, &mesh.vao);
        }
        m_meshes.clear();
        for (auto& [id, cel] : m_textures) {
            (void)id;
            glDeleteTextures(1, &cel.tex);
        }
        m_textures.clear();
        for (const auto& entry : m_texture_cache.records())
            if (entry.record.tex) glDeleteTextures(1, &entry.record.tex);
        m_texture_cache.clear();
        if (m_white_tex) glDeleteTextures(1, &m_white_tex);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        if (m_prog) glDeleteProgram(m_prog);
        if (m_mesh_prog) glDeleteProgram(m_mesh_prog);
        if (m_ctx) SDL_GL_DestroyContext(m_ctx);
        m_vbo = m_vao = m_prog = m_mesh_prog = m_white_tex = 0;
        m_ctx = nullptr;
    }

    CelId cel_upload_rgba(int w, int h, const void* pixels) override {
        return cel_upload_rgba_ex(w, h, pixels, false, false);
    }

    CelId try_texture_replacement(int w, int h, uint64_t source_hash,
                                  bool mipmap_linear, bool wrap_repeat) {
        const TextureReplacements::Image* image =
            m_replacements.find(w, h, source_hash);
        if (!image) return kInvalidCel;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return kInvalidCel;
        glBindTexture(GL_TEXTURE_2D, tex);
        const bool has_mips = mipmap_linear && glGenerateMipmap;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        has_mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width,
                     image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     image->rgba.data());
        if (has_mips) glGenerateMipmap(GL_TEXTURE_2D);
        const CelId id =
            !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
        if (!m_free_ids.empty()) m_free_ids.pop_back();
        CelTex uploaded;
        uploaded.w = image->width;
        uploaded.h = image->height;
        uploaded.logical_w = w;
        uploaded.logical_h = h;
        uploaded.tex = tex;
        uploaded.has_mips = has_mips;
        uploaded.wrap_repeat = wrap_repeat;
        uploaded.replacement = true;
        m_textures.emplace(id, std::move(uploaded));
        return id;
    }

    CelId cel_upload_rgba_ex(int w, int h, const void* pixels,
                             bool mipmap_linear, bool wrap_repeat) override {
        return cel_upload_rgba_ex_hashed(w, h, pixels, mipmap_linear,
                                         wrap_repeat, 0);
    }

    CelId cel_upload_rgba_ex_hashed(int w, int h, const void* pixels,
                                    bool mipmap_linear, bool wrap_repeat,
                                    uint64_t content_hash) override {
        if (!pixels || w <= 0 || h <= 0) return kInvalidCel;
        const size_t pixel_count = static_cast<size_t>(w) * h;
        const auto* rgba = static_cast<const uint32_t*>(pixels);
        if (content_hash == 0) {
            content_hash = 1469598103934665603ull;
            const auto* bytes = static_cast<const uint8_t*>(pixels);
            for (size_t i = 0; i < pixel_count * sizeof(uint32_t); ++i) {
                content_hash ^= bytes[i];
                content_hash *= 1099511628211ull;
            }
        }
        if (const CelId replacement = try_texture_replacement(
                w, h, content_hash, mipmap_linear, wrap_repeat);
            replacement != kInvalidCel)
            return replacement;
        if (std::optional<CelTex> reused = m_texture_cache.take(
                cel_cache_key(w, h, mipmap_linear && glGenerateMipmap,
                              wrap_repeat, content_hash))) {
            glBindTexture(GL_TEXTURE_2D, reused->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            mipmap_linear ? GL_LINEAR_MIPMAP_LINEAR
                                          : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            mipmap_linear ? GL_LINEAR : GL_NEAREST);
            const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
            const CelId id =
                !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
            if (!m_free_ids.empty()) m_free_ids.pop_back();
            m_textures.emplace(id, std::move(*reused));
            ++m_texture_cache_hits;
            return id;
        }
        ++m_texture_cache_misses;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        mipmap_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        mipmap_linear ? GL_LINEAR : GL_NEAREST);
        const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        if (mipmap_linear && glGenerateMipmap) glGenerateMipmap(GL_TEXTURE_2D);
        // Cel ids are a bounded resource: reuse ids released by cel_free
        // instead of exhausting the lifetime upload count.
        // The authentic State 3 replay publishes thousands of distinct
        // textures across a run (scene work is re-uploaded per frame), so a
        // monotonic id counter exhausts the cap and takes down every later
        // texture load.  Reused ids overwrite the previous contents, which
        // cel_free has already deleted.
        const CelId id =
            !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
        if (!m_free_ids.empty()) m_free_ids.pop_back();
        CelTex uploaded;
        uploaded.w = w;
        uploaded.h = h;
        uploaded.tex = tex;
        uploaded.has_mips = mipmap_linear && glGenerateMipmap;
        uploaded.wrap_repeat = wrap_repeat;
        uploaded.content_hash = content_hash;
        m_textures.emplace(id, std::move(uploaded));
        return id;
    }

    bool clt_supports_authored_lod_chain() const override {
        return glGenerateMipmap != nullptr;
    }

    CelId cel_upload_rgba_lod_chain(int w, int h, const void* pixels,
                                    const CelLodLevel* levels,
                                    size_t level_count, bool mipmap_linear,
                                    bool wrap_repeat,
                                    uint64_t content_hash,
                                    uint64_t lod_chain_hash) override {
        if (!mipmap_linear || !glGenerateMipmap || !levels || level_count == 0 ||
            !pixels || w <= 0 || h <= 0)
            return cel_upload_rgba_ex_hashed(w, h, pixels, mipmap_linear,
                                             wrap_repeat, content_hash);
        const size_t pixel_count = static_cast<size_t>(w) * h;
        const auto* rgba = static_cast<const uint32_t*>(pixels);
        if (content_hash == 0)
            content_hash = texture_rgba_hash(pixels, pixel_count);
        if (!m_replacements.empty()) {
            if (const CelId replacement = try_texture_replacement(
                    w, h, content_hash, true, wrap_repeat);
                replacement != kInvalidCel)
                return replacement;
        }
        // Only levels whose extents are the base level halved are usable: a
        // differently shaped level would redefine the GL chain shape instead
        // of the guest's own reduction of it.
        size_t authored = 0;
        for (; authored < level_count; ++authored) {
            const CelLodLevel& level = levels[authored];
            const int shift = static_cast<int>(authored) + 1;
            if (!level.rgba || level.w != std::max(1, w >> shift) ||
                level.h != std::max(1, h >> shift))
                break;
        }
        if (authored == 0)
            return cel_upload_rgba_ex_hashed(w, h, pixels, mipmap_linear,
                                             wrap_repeat, content_hash);
        // Trust the caller's chain identity only for the exact chain it named.
        // A truncated level list is rehashed over the accepted prefix so a
        // differently shaped chain cannot alias a cached texture.
        uint64_t chain_hash = lod_chain_hash;
        if (chain_hash == 0 || authored != level_count)
            chain_hash = cel_lod_chain_hash(levels, authored);
        if (std::optional<CelTex> reused = m_texture_cache.take(
                cel_cache_key(w, h, true, wrap_repeat, content_hash,
                              chain_hash))) {
            glBindTexture(GL_TEXTURE_2D, reused->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
            const CelId id =
                !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
            if (!m_free_ids.empty()) m_free_ids.pop_back();
            m_textures.emplace(id, std::move(*reused));
            ++m_texture_cache_hits;
            return id;
        }
        ++m_texture_cache_misses;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // The guest's LOD choice selects its own level's art: upload the base
        // level plus each authored coarser level at its own extent.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        for (size_t i = 0; i < authored; ++i) {
            glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i) + 1, GL_RGBA8,
                         levels[i].w, levels[i].h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         levels[i].rgba);
        }
        // GL needs a complete chain. Levels the guest did not load have no
        // authored bytes, so extend it below the authored tail with the host's
        // own reduction of the coarsest authored level -- the documented tail
        // source and the software backend's behavior. Generating the chain
        // from level 0 instead would serve a reduction of LOD0 there, which
        // the guest's coarser levels may not resemble.
        std::vector<CelLodTail> tail;
        const CelLodLevel& coarsest = levels[authored - 1];
        cel_generate_lod_tail(coarsest.rgba, coarsest.w, coarsest.h, tail);
        GLint level_index = static_cast<GLint>(authored) + 1;
        for (const CelLodTail& level : tail) {
            glTexImage2D(GL_TEXTURE_2D, level_index++, GL_RGBA8, level.w,
                         level.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         level.rgba.data());
        }
        const CelId id =
            !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
        if (!m_free_ids.empty()) m_free_ids.pop_back();
        CelTex uploaded;
        uploaded.w = w;
        uploaded.h = h;
        uploaded.tex = tex;
        uploaded.has_mips = true;
        uploaded.wrap_repeat = wrap_repeat;
        uploaded.content_hash = content_hash;
        uploaded.lod_chain_hash = chain_hash;
        m_textures.emplace(id, std::move(uploaded));
        return id;
    }

    void cel_free(CelId cel) override {
        auto it = m_textures.find(cel);
        if (it == m_textures.end()) return;
        if (it->second.gpu_snapshot || it->second.replacement) {
            glDeleteTextures(1, &it->second.tex);
            m_textures.erase(it);
            m_free_ids.push_back(cel);
            return;
        }
        std::vector<CelTex> released;
        m_texture_cache.insert(it->second.cache_key(), std::move(it->second),
                               released);
        m_textures.erase(it);
        m_free_ids.push_back(cel);
        for (CelTex& record : released)
            if (record.tex) glDeleteTextures(1, &record.tex);
    }

    void cel_blit(CelId cel, float dx, float dy, float dw, float dh, float r,
                  float g, float b, float a) override {
        auto it = m_textures.find(cel);
        if (it == m_textures.end()) return;
        const auto& t = it->second;

        const Vertex verts[4] = {
            {dx, dy,          0.f, 1.f, 0.f, 0.f, r, g, b, a, 0.f, 0.f},
            {dx + dw, dy,     0.f, 1.f, 1.f, 0.f, r, g, b, a, 1.f, 0.f},
            {dx, dy + dh,     0.f, 1.f, 0.f, 1.f, r, g, b, a, 0.f, 1.f},
            {dx + dw, dy + dh, 0.f, 1.f, 1.f, 1.f, r, g, b, a, 1.f, 1.f},
        };
        glUseProgram(m_prog);
        glUniform2f(m_clt_uniforms.res, m_w, m_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        // M2_BITMAP_FILTER applies to the Projector bitmap scanout only; other
        // 2D cels keep the sampler state they were uploaded with.
        const bool scanout_filter =
            cel == bitmap_scanout_cel() &&
            m_bitmap_filter != TextureFilterMode::guest;
        if (scanout_filter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glUniform1i(m_clt_uniforms.tex, 0);
        glUniform1i(m_clt_uniforms.discard_ssb, 0);
        glUniform1i(m_clt_uniforms.discard_alpha, 0);
        glUniform1i(m_clt_uniforms.discard_rgb, 0);
        glUniform1i(m_clt_uniforms.blend_mode, 0);
        glUniform1i(m_clt_uniforms.dither_enable, 0);
        glUniform1i(m_clt_uniforms.tab_control, -1);
    glUniform1i(m_clt_uniforms.tab_alpha_color, 0);
        glUniform1i(m_clt_uniforms.feedback_source, 0);
        glUniform3f(m_clt_uniforms.blend_const,
                    0.f, 0.f, 0.f);
        glUniform2f(m_clt_uniforms.uv_period, 1.f, 1.f);
        glUniform1i(m_clt_uniforms.sharp,
                    scanout_filter &&
                            m_bitmap_filter == TextureFilterMode::sharp
                        ? 1
                        : 0);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        ++m_draws;
    }

    void frame_begin(float cr, float cg, float cb) override {
        // Track window resizes every frame (cheap; avoids resize callbacks).
        // A fullscreen drawable is fitted to 4:3 (black bars); windowed it is
        // the whole drawable.
        const LetterboxRect view = presentation_rect(m_window);
        if (view.w > 0 && view.h > 0) {
            m_present_w = view.w;
            m_present_h = view.h;
            int w = 0, h = 0;
            internal_render_size(m_render_scale, view.w, view.h, w, h);
            m_w = static_cast<float>(w);
            m_h = static_cast<float>(h);
        }
        m_view_x = view.x;
        m_view_y = view.y;
        m_targets.set_present_origin(m_view_x, m_view_y);
        m_targets.set_present_size(m_present_w, m_present_h);
        m_targets.resize(static_cast<int>(m_w), static_cast<int>(m_h), cr, cg, cb);
        glViewport(0, 0, static_cast<GLint>(m_w), static_cast<GLint>(m_h));
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glClearColor(cr, cg, cb, 1.f);
        const CltFrameClearPlan clear_plan =
            clt_frame_clear_plan(persist_target_enabled(),
                                 m_persist_initialized);
        GLbitfield clear_bits = 0;
        if (clear_plan.color) clear_bits |= GL_COLOR_BUFFER_BIT;
        if (clear_plan.depth) clear_bits |= GL_DEPTH_BUFFER_BIT;
        if (clear_bits != 0) {
            if (glClearDepth) glClearDepth(kCltTeZFarDepth);
            glClear(clear_bits);
        if (clear_plan.color) m_targets.clear_metadata();
            m_persist_initialized = true;
        }
        m_draws = 0;
    }

    void clt_clear_z(uint32_t z_fill_word) override {
        if (glDepthMask) glDepthMask(GL_TRUE);
        if (glClearDepth) glClearDepth(clt_te_z_fill_depth(z_fill_word));
        if (glClear) glClear(GL_DEPTH_BUFFER_BIT);
    }

    void clt_clear_color(uint32_t red_word, uint32_t green_word,
                         uint32_t blue_word, uint32_t alpha_word) override {
        // The recovered TE driver serves TE_CLEAR_FRAME_BUFFER with a clear
        // list that is clipped by DBFBCLIP/window state like any guest
        // primitive (driver.c:251-278). frame_present() disables the scissor
        // at every host frame boundary, so re-assert the interpreter's clip
        // before clearing; a full-target clear here would escape the bound
        // surface's extent.
        apply_clip();
        m_targets.clear_color(clt_te_color_component(red_word),
                              clt_te_color_component(green_word),
                              clt_te_color_component(blue_word),
                              clt_te_color_component(alpha_word));
    }

    void frame_present() override {
        m_targets.present(!has_bitmap_scanout());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        // The scanout quad draws on the default framebuffer: confine it to
        // the letterboxed image region, then restore the target viewport.
        glViewport(m_view_x, m_view_y, static_cast<GLint>(m_present_w),
                   static_cast<GLint>(m_present_h));
        draw_bitmap_scanout();
        glViewport(0, 0, static_cast<GLint>(m_w), static_cast<GLint>(m_h));
        SDL_GL_SwapWindow(m_window);
    }

    void clt_target_select(uint32_t base) override { m_targets.select(base); }
    void clt_scanout_select(uint32_t base) override {
        m_targets.select_scanout(base);
    }
    void clt_target_scanout_rect(uint32_t base, const CltScanoutRect& rect) override {
        m_targets.set_scanout_rect(base, rect);
    }
    void clt_target_persistence(bool enabled) override {
        m_persist_target = enabled;
    }
    void clt_target_pool(uint64_t& releases, uint64_t& refusals,
                         uint64_t& pooled) const override {
        releases = m_targets.releases();
        refusals = m_targets.refusals();
        pooled = m_targets.pool_size();
    }
    void clt_target_upload_rgba(uint32_t base, const uint32_t* rgba, int w,
                                int h) override {
        if (!base || !rgba || w != static_cast<int>(m_w) ||
            h != static_cast<int>(m_h)) return;
        const uint32_t restore = m_targets.active_base();
        m_targets.select(base);
        // A host-side target upload must land verbatim. The CLT draws of the
        // frame that is still in flight leave GL_BLEND enabled with the
        // guest's source-over function, which composites this raster through
        // its own alpha channel instead of replacing the target, and can leave
        // the guest clip rect enabled, which would clip the write.
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        const CelId cel = cel_upload_rgba(w, h, rgba);
        if (cel != kInvalidCel) {
            cel_blit(cel, 0.f, 0.f, static_cast<float>(w),
                     static_cast<float>(h), 1.f, 1.f, 1.f, 1.f);
            cel_free(cel);
        }
        apply_clip();
        std::vector<uint32_t> metadata(size_t(w)*h);
        for (size_t i=0;i<metadata.size();++i)
            metadata[i] = 0xff000000u | ((rgba[i] >> 16u) & 0xff00u);
        const CelId meta = cel_upload_rgba(w,h,metadata.data());
        if (meta != kInvalidCel) {
            glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
            m_targets.metadata_upload_output(true);
            cel_blit(meta,0,0,float(w),float(h),1,1,1,1);
            m_targets.metadata_upload_output(false);
            cel_free(meta);
            apply_clip();
        }
        m_targets.select(restore);
    }
    void clt_target_copy(uint32_t source, uint32_t dest) override {
        m_targets.copy(source, dest);
    }
    bool grab_clt_target_rgba(uint32_t base, uint32_t* dst, int w,
                              int h) override { return m_targets.read(base, dst, w, h); }
    bool grab_clt_target_window_rgba(uint32_t base, uint32_t* dst, int x,
                                     int y, int w, int h) override {
        return m_targets.read_window(base, dst, x, y, w, h);
    }
    bool grab_clt_target_window_metadata(uint32_t base, uint32_t* dst, int x, int y, int w, int h) override {
        return m_targets.read_window(base,dst,x,y,w,h,true);
    }
    CelId clt_snapshot_target_ssb_cel(uint32_t base, int x, int y, int w, int h, int cw, int ch) override {
        return snapshot_target(base, x, y, w, h, cw, ch, true);
    }
    CelId clt_snapshot_target_cel(uint32_t base, int x, int y, int w, int h,
                                  int cel_w, int cel_h, bool preserve_alpha = false) override {
        (void)preserve_alpha;
        return snapshot_target(base, x, y, w, h, cel_w, cel_h, false);
    }
    CelId snapshot_target(uint32_t base, int x, int y, int w, int h, int cel_w, int cel_h, bool ssb) {
        if (cel_w <= 0 || cel_h <= 0) return kInvalidCel;
        // M2_HIRES_EFFECTS: keep the copied window at target resolution. UVs
        // are normalized, so the record's guest extent is unchanged.
        const int tex_w = m_hires_effects && w > 0 ? w : cel_w;
        const int tex_h = m_hires_effects && h > 0 ? h : cel_h;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return kInvalidCel;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_w, tex_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        if (!m_targets.snapshot_to_texture(base, tex, x, y, w, h,
                                           tex_w, tex_h, ssb)) {
            glDeleteTextures(1, &tex);
            return kInvalidCel;
        }
        const CelId id =
            !m_free_ids.empty() ? m_free_ids.back() : ++m_next_id;
        if (!m_free_ids.empty()) m_free_ids.pop_back();
        CelTex snapshot;
        snapshot.w = cel_w;
        snapshot.h = cel_h;
        snapshot.tex = tex;
        snapshot.gpu_snapshot = true;
        m_textures.emplace(id, std::move(snapshot));
        return id;
    }

    bool grab_presented_rgba(uint32_t* dst, int w, int h) override {
        if (copy_bitmap_scanout(dst, w, h)) return true;
        if (m_targets.scanout_base() != 0)
            return m_targets.read_presented(m_targets.scanout_base(), dst, w, h);
        if (!dst || !glReadPixels || w <= 0 || h <= 0) return false;
        if (w != static_cast<int>(m_w) || h != static_cast<int>(m_h)) return false;
        // Read the current draw target before presentation. GL rows are
        // bottom-up; the caller expects top-left origin, so flip in place.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (glGetError)
            while (glGetError() != GL_NO_ERROR) {}
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        if (glGetError && glGetError() != GL_NO_ERROR) return false;
        for (int y = 0; y < h / 2; ++y) {
            uint32_t* top = &dst[static_cast<size_t>(y) * w];
            uint32_t* bottom = &dst[static_cast<size_t>(h - 1 - y) * w];
            for (int x = 0; x < w; ++x) std::swap(top[x], bottom[x]);
        }
        return true;
    }

    const char* name() const override { return "opengl"; }

    // --- clean-3D primitives -------------------------------------------------

    void fill_rect(float dx, float dy, float dw, float dh, float r, float g,
                   float b, float a) override {
        // Untextured quad through the cel pipeline (white texture bound).
        const Vertex verts[4] = {
            {dx, dy,          0.f, 1.f, 0.f, 0.f, r, g, b, a, 0.f, 0.f},
            {dx + dw, dy,     0.f, 1.f, 1.f, 0.f, r, g, b, a, 1.f, 0.f},
            {dx, dy + dh,     0.f, 1.f, 0.f, 1.f, r, g, b, a, 0.f, 1.f},
            {dx + dw, dy + dh, 0.f, 1.f, 1.f, 1.f, r, g, b, a, 1.f, 1.f},
        };
        glUseProgram(m_prog);
        glUniform2f(m_clt_uniforms.res, m_w, m_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_white_tex);
        glUniform1i(m_clt_uniforms.tex, 0);
        glUniform1i(m_clt_uniforms.discard_ssb, 0);
        glUniform1i(m_clt_uniforms.discard_alpha, 0);
        glUniform1i(m_clt_uniforms.discard_rgb, 0);
        glUniform1i(m_clt_uniforms.blend_mode, 0);
        glUniform1i(m_clt_uniforms.dither_enable, 0);
        glUniform1i(m_clt_uniforms.tab_control, -1);
    glUniform1i(m_clt_uniforms.tab_alpha_color, 0);
        glUniform1i(m_clt_uniforms.feedback_source, 0);
        glUniform3f(m_clt_uniforms.blend_const,
                    0.f, 0.f, 0.f);
        glUniform2f(m_clt_uniforms.uv_period, 1.f, 1.f);
        glUniform1i(m_clt_uniforms.sharp, 0);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        ++m_draws;
    }

    void scissor(float x, float y, float w, float h) override {
        m_clip_active = true;
        m_clip_rect = {x, y, w < 0.f ? 0.f : w, h < 0.f ? 0.f : h};
        apply_clip();
    }

    void scissor_disable() override {
        m_clip_active = false;
        glDisable(GL_SCISSOR_TEST);
    }

    bool clt_supports_triangle_batches() const override { return true; }

    void clt_draw(const CltVertex* vertices, uint32_t count, bool strip,
                  bool textured, CelId texture, float target_w,
                  float target_h, bool blend_enabled,
                  CltDiscard discard, CltDepth depth,
                  CltTextureSampling sampling,
                  CltBlendState blend,
                  bool independent_triangles = false) override {
        if (!vertices || count < 3u || count > 65536u ||
            !std::isfinite(target_w) || !std::isfinite(target_h) ||
            target_w <= 0.f || target_h <= 0.f)
            return;

        const CltBlendMode blend_mode =
            clt_effective_blend_mode(blend_enabled, blend);
        if (clt_blend_uses_secondary_source(blend_mode) &&
            (((!blend_enabled || !textured) && blend_mode != CltBlendMode::general_secondary) ||
         (blend.alpha_control_unsupported && blend_mode != CltBlendMode::general_secondary) ||
             discard.separate_ssb_uv))
            return;
        GLuint gl_texture = m_white_tex;
        bool gpu_feedback_source = false;
        GLuint gl_ssb_mask = m_white_tex;
        GLuint gl_secondary_source = m_white_tex;
        // TXTUVMASK address period as a fraction of the resident extent.  The
        // sampler's own wrap mode is exact when the guest's masked period
        // reaches the resident extent (the vendor page builder's tiled case);
        // a shorter period repeats only its masked sub-range and is folded per
        // fragment.
        float uv_period_s = 1.f;
        float uv_period_t = 1.f;
        // Presentation sampling of the primary texture: sharp bilinear.
        bool sharp_sample = false;
        if (textured) {
            const auto it = m_textures.find(texture);
            if (it == m_textures.end()) return;
            gl_texture = it->second.tex;
            gpu_feedback_source = it->second.gpu_snapshot;
            CltTextureSampling effective_sampling = sampling;
            sharp_sample = m_sharp_filter && !gpu_feedback_source;
            if ((m_linear_filter || m_anisotropy > 1) && !gpu_feedback_source) {
                effective_sampling.min_filter = CltTextureFilter::linear;
                effective_sampling.mag_filter = CltTextureFilter::linear;
            }
            glBindTexture(GL_TEXTURE_2D, gl_texture);
            const int logical_w = it->second.logical_w > 0
                                      ? it->second.logical_w : it->second.w;
            const int logical_h = it->second.logical_h > 0
                                      ? it->second.logical_h : it->second.h;
            if (sampling.wrap_u && logical_w > 0 &&
                sampling.u_period < static_cast<uint32_t>(logical_w))
                uv_period_s = static_cast<float>(sampling.u_period) /
                              static_cast<float>(logical_w);
            if (sampling.wrap_v && logical_h > 0 &&
                sampling.v_period < static_cast<uint32_t>(logical_h))
                uv_period_t = static_cast<float>(sampling.v_period) /
                              static_cast<float>(logical_h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            gl_texture_min_filter(effective_sampling, it->second.has_mips));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            gl_texture_mag_filter(effective_sampling));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            sampling.wrap_u ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            sampling.wrap_v ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                            it->second.has_mips ? sampling.lod_max : 0);
            // Anisotropy is a per-texture parameter; GPU feedback sources keep
            // guest sampling (the same exclusion the linear override uses).
            if (m_anisotropy > 1 && !gpu_feedback_source)
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                static_cast<GLfloat>(m_anisotropy));
        }
        if (discard.ssb_zero ||
        (blend.weighted_mix_ssb_mask != kInvalidCel && (blend.dsb.select == CltDsbSelect::object_ssb || blend.tab.enabled ||
         blend.mode == CltBlendMode::general_internal || blend.mode == CltBlendMode::general_secondary)) ||
            (blend.weighted_mix_ssb_selects_entry1 &&
             blend.mode == CltBlendMode::coefficient_weighted_mix)) {
            // The SSB discard test and the per-texel coefficient-entry split
            // both sample the texture's SSB bitmap.  The interpreter carries
            // the uploaded mask cel on the blend state; the discard's own
            // mask cel is the same bitmap and serves as the fallback.
            CelId mask_cel = blend.weighted_mix_ssb_mask;
            if (mask_cel == kInvalidCel) mask_cel = discard.ssb_mask;
            const auto it = m_textures.find(mask_cel);
            if (it == m_textures.end()) return;
            gl_ssb_mask = it->second.tex;
            glBindTexture(GL_TEXTURE_2D, gl_ssb_mask);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            sampling.wrap_u ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            sampling.wrap_v ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        if (clt_blend_uses_secondary_source(blend_mode)) {
            const auto it = m_textures.find(blend.secondary_source);
            if (it == m_textures.end()) return;
            gl_secondary_source = it->second.tex;
            glBindTexture(GL_TEXTURE_2D, gl_secondary_source);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }

        m_clt_vertices.resize(count);
        std::vector<Vertex>& verts = m_clt_vertices;
        const float sx = m_w / target_w;
        const float sy = m_h / target_h;
        for (uint32_t i = 0; i < count; ++i) {
            const CltVertex& src = vertices[i];
            if (!std::isfinite(src.x) || !std::isfinite(src.y) ||
                !std::isfinite(src.u) || !std::isfinite(src.v) ||
                !std::isfinite(src.mask_u) || !std::isfinite(src.mask_v) ||
                !std::isfinite(src.r) || !std::isfinite(src.g) ||
                !std::isfinite(src.b) || !std::isfinite(src.a) ||
                !std::isfinite(src.one_over_w))
                return;
            Vertex& dst = verts[i];
            dst.x = src.x * sx;
            dst.y = src.y * sy;
            dst.z = -1.f + 2.f * std::clamp(src.one_over_w, 0.f, 1.f);
            dst.w = (src.w > 0.f && std::isfinite(src.w)) ? src.w : 1.f;
            dst.u = src.u;
            dst.v = src.v;
            dst.r = src.r;
            dst.g = src.g;
            dst.b = src.b;
            dst.a = src.a;
            dst.mask_u =
                clt_blend_uses_secondary_source(blend_mode)
                    ? src.mask_u
                    : (discard.separate_ssb_uv ? src.mask_u : src.u);
            dst.mask_v =
                clt_blend_uses_secondary_source(blend_mode)
                    ? src.mask_v
                    : (discard.separate_ssb_uv ? src.mask_v : src.v);
        }

        // TE Z region: outside-region pixels bypass the Z test/write, which
        // GL exposes only per draw call. Inside-region fragments keep the
        // guest's DBZCNTL relation; outside fragments draw color-only, and
        // DBDISCARDCONTROL.ZCLIPPED drops them entirely. The backend's
        // scissor contract splits the two pixel sets exactly at 1:1 scale.
        const CltClipRect clip_rect =
            m_clip_active ? m_clip_rect
                          : CltClipRect{0.f, 0.f, m_w, m_h};
        CltClipRect inside = clip_rect;
        CltClipRect outside[4]{};
        int outside_count = 0;
        if (depth.test_enabled) {
            const CltClipRect region{
                static_cast<float>(depth.z_region_min_x) * sx,
                static_cast<float>(depth.z_region_min_y) * sy,
                static_cast<float>(depth.z_region_max_x -
                                   depth.z_region_min_x) * sx,
                static_cast<float>(depth.z_region_max_y -
                                   depth.z_region_min_y) * sy};
            inside = clt_clip_rect_intersection(clip_rect, region);
            if (!depth.z_clipped_discard)
                outside_count =
                    clt_clip_rect_subtract(clip_rect, region, outside);
        }
        const auto set_depth_state = [&](bool test) {
            if (test && glDepthFunc) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(gl_depth_func_for_dbz(depth.depth_func));
            } else {
                glDisable(GL_DEPTH_TEST);
            }
            // Pixel output and Z update are independent DBZCNTL decisions.
            // The color pass must never update the shared Z surface; the
            // separate update pass below applies the Z relation mask.
            if (glDepthMask) glDepthMask(GL_FALSE);
        };
        const auto set_depth_update_state = [&]() {
            // GL requires the depth test to be enabled for a depth-buffer
            // write; this state is entered only for ZBUFEN-enabled draws.
            glEnable(GL_DEPTH_TEST);
            if (glDepthFunc) {
                glDepthFunc(gl_depth_func_for_dbz_z_update(depth.depth_func));
            }
            if (glDepthMask) glDepthMask(GL_TRUE);
        };
        const auto draw_once = [&](GLint first, GLsizei prim_count) {
            const GLenum mode = independent_triangles
                                    ? GL_TRIANGLES
                                    : strip ? GL_TRIANGLE_STRIP
                                            : GL_TRIANGLE_FAN;
            glDrawArrays(mode, first, prim_count);
        };

        glUseProgram(m_prog);
        glUniform2f(m_clt_uniforms.res, m_w, m_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_texture);
        glUniform1i(m_clt_uniforms.tex, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl_ssb_mask);
        glUniform1i(m_clt_uniforms.ssb_mask, 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl_secondary_source);
        glUniform1i(m_clt_uniforms.secondary_source, 2);
        GLuint source_ssb_texture = m_white_tex;
        if (blend.source_ssb_mask != kInvalidCel) {
            const auto mask = m_textures.find(blend.source_ssb_mask);
            if (mask == m_textures.end()) return;
            source_ssb_texture = mask->second.tex;
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, source_ssb_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glUniform1i(m_clt_uniforms.source_ssb, 3);
        glUniform1i(m_clt_uniforms.has_source_ssb, blend.source_ssb_mask != kInvalidCel);
        glUniform1i(m_clt_uniforms.has_object_ssb, blend.weighted_mix_ssb_mask != kInvalidCel || discard.ssb_zero);
        glUniform1i(m_clt_uniforms.dsb_select, blend.dsb.raw);

        glUniform1i(m_clt_uniforms.discard_ssb,
                    discard.ssb_zero ? 1 : 0);
        glUniform1i(m_clt_uniforms.separate_ssb_uv,
                    discard.separate_ssb_uv ? 1 : 0);
        glUniform1i(m_clt_uniforms.discard_alpha,
                    discard.alpha_zero ? 1 : 0);
        glUniform1i(m_clt_uniforms.discard_rgb,
                    discard.rgb_zero ? 1 : 0);
        glUniform1i(m_clt_uniforms.blend_mode,
                    static_cast<GLint>(blend_mode));
        glUniform1i(m_clt_uniforms.dither_enable, blend.dither.enabled ? 1 : 0);
        glUniform1i(m_clt_uniforms.dither_a,
                    static_cast<GLint>(blend.dither.matrix_a));
        glUniform1i(m_clt_uniforms.dither_b,
                    static_cast<GLint>(blend.dither.matrix_b));
        glUniform1i(m_clt_uniforms.tab_alpha_color,
                    blend.texture_alpha_as_color ? 1 : 0);
    glUniform1i(m_clt_uniforms.tab_control, blend.tab.enabled ?
        static_cast<GLint>(blend.tab.control | (blend.weighted_mix_ssb_mask != kInvalidCel ? 0x100000u : 0u)) : -1);
    const auto tab0 = clt_tab_constant(blend.tab.constants[0]);
    const auto tab1 = clt_tab_constant(blend.tab.constants[1]);
    glUniform4f(m_clt_uniforms.tab_constant0, tab0[0], tab0[1], tab0[2], tab0[3]);
    glUniform4f(m_clt_uniforms.tab_constant1, tab1[0], tab1[1], tab1[2], tab1[3]);
    if (blend_mode == CltBlendMode::general_internal ||
        blend_mode == CltBlendMode::general_secondary) {
        glUniform1i(m_clt_uniforms.dsb_has_ssb, blend.weighted_mix_ssb_mask != kInvalidCel ? 1 : 0);
        glUniform1i(m_clt_uniforms.dsb_a, blend.general.a);
        glUniform1i(m_clt_uniforms.dsb_b, blend.general.b);
        glUniform1i(m_clt_uniforms.dsb_alu, blend.general.alu);
        glUniform1i(m_clt_uniforms.dsb_src_alpha, blend.general.source_alpha);
        glUniform1i(m_clt_uniforms.dsb_dest_alpha, blend.general.dest_alpha);
        const auto dsb_color = [&](GLint location, uint32_t word) {
            glUniform4f(location, clt_dsb_channel(word,16), clt_dsb_channel(word,8), clt_dsb_channel(word,0), 0.f);
        };
        dsb_color(m_clt_uniforms.dsb_constant, blend.general.constant);
        dsb_color(m_clt_uniforms.dsb_a0, blend.general.a_constant[0]);
        dsb_color(m_clt_uniforms.dsb_a1, blend.general.a_constant[1]);
        dsb_color(m_clt_uniforms.dsb_b0, blend.general.b_constant[0]);
        dsb_color(m_clt_uniforms.dsb_b1, blend.general.b_constant[1]);
        glUniform4f(m_clt_uniforms.dsb_alpha, clt_dsb_channel(blend.general.alpha_constant,16),
            clt_dsb_channel(blend.general.alpha_constant,0), 0.f, 0.f);
    }


        glUniform3f(m_clt_uniforms.blend_const,
                    blend.constant_r, blend.constant_g, blend.constant_b);
        glUniform3f(m_clt_uniforms.blend_weight,
                    blend.weight_tex_r, blend.weight_tex_g, blend.weight_tex_b);
        glUniform1i(m_clt_uniforms.db_source_linear,
                    blend.db_source_linear ? 1 : 0);
        glUniform1i(m_clt_uniforms.feedback_source,
                    gpu_feedback_source ? 1 : 0);
        glUniform1f(m_clt_uniforms.db_source_height,
                    blend.db_source_height);
        glUniform2f(m_clt_uniforms.uv_period, uv_period_s,
                    uv_period_t);
        glUniform1i(m_clt_uniforms.sharp, sharp_sample ? 1 : 0);
        // Per-texel SSB coefficient split: 0 = single resolved pair, 1 = keep
        // SSB-0 texels, 2 = keep SSB-1 texels (the second pass sets it).
        const bool ssb_entry_split =
            blend_mode == CltBlendMode::coefficient_weighted_mix &&
            blend.weighted_mix_ssb_selects_entry1 &&
            gl_ssb_mask != m_white_tex;
        glUniform1i(m_clt_uniforms.ssb_keep, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * verts.size(),
                     verts.data(), GL_STREAM_DRAW);
        // Fixed-function factors for the decoded equation come from the
        // shared plan so both GL backends cannot disagree; the shader-side
        // half of the equation (constant_alpha_over/specular_add/weighted-
        // mix rewrite) rides u_blend_mode above.
        const auto gl_factor = [](CltGlBlendFactor factor) {
            switch (factor) {
            case CltGlBlendFactor::zero: return GL_ZERO;
            case CltGlBlendFactor::one: return GL_ONE;
            case CltGlBlendFactor::src_alpha: return GL_SRC_ALPHA;
            case CltGlBlendFactor::one_minus_src_alpha:
                return GL_ONE_MINUS_SRC_ALPHA;
            case CltGlBlendFactor::one_minus_src_color:
                return GL_ONE_MINUS_SRC_COLOR;
            case CltGlBlendFactor::constant_color: return GL_CONSTANT_COLOR;
            }
            return GL_ZERO;
        };
        const CltGlBlendPlan plan = clt_gl_blend_plan(blend_mode);
        if (plan.uses_constant_color)
            glBlendColor(blend.weight_src_r, blend.weight_src_g,
                         blend.weight_src_b, 1.f);
        glBlendFuncSeparate(gl_factor(plan.src_factor),
                            gl_factor(plan.dst_factor),
                            gl_factor(plan.src_alpha_factor),
                            gl_factor(plan.dst_alpha_factor));
        if (plan.blend_enabled)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        m_targets.metadata_write_state();
        // One geometry pass per coefficient entry.  Each pass keeps only the
        // texels whose own SSB bit selects that entry, so the two-equation
        // split the per-texel source-select-bit produces is evaluated exactly
        // (a single resolved pair cannot express it).
        const auto draw_passes = [&](GLint first, GLsizei prim_count,
                                     int ssb_keep, float src_w_r,
                                     float src_w_g, float src_w_b) {
            if (ssb_entry_split) {
                glUniform1i(m_clt_uniforms.ssb_keep,
                            ssb_keep);
                if (ssb_keep == 2) {
                    glUniform3f(m_clt_uniforms.blend_weight,
                                blend.entry1_weight_tex_r,
                                blend.entry1_weight_tex_g,
                                blend.entry1_weight_tex_b);
                    glBlendColor(src_w_r, src_w_g, src_w_b, 1.f);
                } else if (plan.uses_constant_color) {
                    // The preceding SSB-1 pass temporarily installs its
                    // source weight as the fixed-function blend constant.
                    // Restore entry 0 before a color or depth pass evaluates
                    // its next fragment set.
                    glBlendColor(blend.weight_src_r, blend.weight_src_g,
                                 blend.weight_src_b, 1.f);
                }
            }
            draw_once(first, prim_count);
            if (ssb_entry_split) {
                glUniform1i(m_clt_uniforms.ssb_keep, 0);
                glUniform3f(m_clt_uniforms.blend_weight,
                            blend.weight_tex_r, blend.weight_tex_g,
                            blend.weight_tex_b);
                if (plan.uses_constant_color)
                    glBlendColor(blend.weight_src_r, blend.weight_src_g,
                                 blend.weight_src_b, 1.f);
            }
        };
        const auto draw_color_region = [&](bool test, GLint first,
                                           GLsizei prim_count) {
            set_depth_state(test);
            if (!ssb_entry_split) {
                draw_passes(first, prim_count, 0, 0.f, 0.f, 0.f);
                return;
            }
            // Both coefficient-entry passes use the pixel-output relation;
            // neither can write Z because this is the color-only phase.
            draw_passes(first, prim_count, 1, 0.f, 0.f, 0.f);
            draw_passes(first, prim_count, 2, blend.entry1_weight_src_r,
                        blend.entry1_weight_src_g,
                        blend.entry1_weight_src_b);
        };
        const bool z_update_enabled =
            clt_dbz_z_update_enabled(depth.depth_func, depth.test_enabled,
                                     depth.write_enabled);
        const auto draw_depth_region = [&](bool test, GLint first,
                                           GLsizei prim_count) {
            if (!test || !z_update_enabled) return;
            set_depth_update_state();
            // Color writes are disabled while the independent Z-update
            // relation is evaluated.  The shader still performs the same
            // source/discard work, so only surviving guest fragments reach
            // the depth test just as on the color pass.
            if (glColorMask) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            if (!ssb_entry_split) {
                draw_passes(first, prim_count, 0, 0.f, 0.f, 0.f);
            } else {
                draw_passes(first, prim_count, 1, 0.f, 0.f, 0.f);
                draw_passes(first, prim_count, 2, blend.entry1_weight_src_r,
                            blend.entry1_weight_src_g,
                            blend.entry1_weight_src_b);
            }
            if (glColorMask) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        };
        CltBackendDepthCompare combined_cmp = CltBackendDepthCompare::never;
        const bool can_combine = clt_can_combine_depth_passes(
            depth.depth_func, depth.test_enabled, z_update_enabled,
            ssb_entry_split, &combined_cmp);
        const auto set_combined_depth_state = [&](bool test) {
            if (test && glDepthFunc) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(gl_depth_func_for_compare(combined_cmp));
                if (glDepthMask) glDepthMask(GL_TRUE);
            } else {
                glDisable(GL_DEPTH_TEST);
                if (glDepthMask) glDepthMask(GL_FALSE);
            }
        };
        const bool near_color_z = clt_near_color_z_fill(
            depth, vertices, count, textured, blend_enabled);
        const auto draw_alias_primitive = [&](bool test, GLint first, GLsizei n) {
            m_targets.begin_color_z_coverage();
            draw_color_region(test, first, n);
            // The color write is independent of ZOUTEN and the Z-update mask.
            // Only samples that survived its original test may change the plane.
            m_targets.resolve_color_z_coverage();
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glDepthMask(GL_TRUE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            draw_passes(first, n, 0, 0.f, 0.f, 0.f);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_targets.end_color_z_coverage();
        };
        const auto draw_z_region = [&](bool test) {
            if (near_color_z) {
                // Every surviving sample writes the same color and near value.
                // The operation is idempotent, so overlapping triangles can share
                // one coverage/resolve pair without per-triangle stencil clears.
                draw_alias_primitive(test, 0, static_cast<GLsizei>(count));
                return;
            }
            if (test && can_combine) {
                set_combined_depth_state(true);
                draw_passes(0, static_cast<GLsizei>(count), 0, 0.f, 0.f, 0.f);
                return;
            }
            if (!test || !z_update_enabled || count <= 3u) {
                draw_color_region(test, 0, static_cast<GLsizei>(count));
                draw_depth_region(test, 0, static_cast<GLsizei>(count));
                return;
            }
            // Preserve sequential primitive semantics for exceptional relations
            // or SSB coefficient split over multi-triangle submissions.
            if (independent_triangles) {
                for (uint32_t first = 0; first + 2u < count; first += 3u) {
                    draw_color_region(true, static_cast<GLint>(first), 3);
                    draw_depth_region(true, static_cast<GLint>(first), 3);
                }
            } else if (strip) {
                for (uint32_t i = 0; i + 2u < count; ++i) {
                    draw_color_region(true, static_cast<GLint>(i), 3);
                    draw_depth_region(true, static_cast<GLint>(i), 3);
                }
            } else {
                draw_color_region(true, 0, static_cast<GLsizei>(count));
                draw_depth_region(true, 0, static_cast<GLsizei>(count));
            }
        };
        if (!depth.test_enabled) {
            apply_clip();
            draw_z_region(false);
        } else {
            if (!clt_clip_rect_empty(inside)) {
                apply_scissor_rect(inside);
                draw_z_region(true);
            }
            if (outside_count > 0) {
                for (int i = 0; i < outside_count; ++i) {
                    if (clt_clip_rect_empty(outside[i])) continue;
                    apply_scissor_rect(outside[i]);
                    draw_z_region(false);
                }
            }
        }
        if (plan.restores)
            glBlendFunc(gl_factor(CltGlBlendFactor::src_alpha),
                        gl_factor(CltGlBlendFactor::one_minus_src_alpha));
        apply_clip();
        glEnable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        if (glDepthMask) glDepthMask(GL_TRUE);
        ++m_draws;
    }

    void get_size(int* w, int* h) override {
        if (w) *w = static_cast<int>(m_w);
        if (h) *h = static_cast<int>(m_h);
    }

    MeshId mesh_upload(int vertex_count, const float* pos_xyz,
                       const float* col_rgba, int index_count,
                       const uint16_t* indices) override {
        if (!m_mesh_prog || vertex_count <= 0 || index_count <= 0 || !pos_xyz ||
            !col_rgba || !indices || m_next_mesh >= kMaxMeshes)
            return kInvalidMesh;

        // Interleave pos+col into one VBO.
        std::vector<MeshVertex> verts(static_cast<size_t>(vertex_count));
        for (int i = 0; i < vertex_count; ++i) {
            MeshVertex& v = verts[static_cast<size_t>(i)];
            v.x = pos_xyz[i * 3 + 0];
            v.y = pos_xyz[i * 3 + 1];
            v.z = pos_xyz[i * 3 + 2];
            v.r = col_rgba[i * 4 + 0];
            v.g = col_rgba[i * 4 + 1];
            v.b = col_rgba[i * 4 + 2];
            v.a = col_rgba[i * 4 + 3];
        }

        GLuint vao = 0, vbo = 0, ebo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     sizeof(MeshVertex) * verts.size(), verts.data(),
                     GL_STATIC_DRAW);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     sizeof(uint16_t) * index_count, indices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void*>(offsetof(MeshVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void*>(offsetof(MeshVertex, r)));
        glBindVertexArray(0);

        const MeshId id = ++m_next_mesh;
        m_meshes[id] = {vao, vbo, ebo, vertex_count, index_count};
        return id;
    }

    void mesh_free(MeshId mesh) override {
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end()) return;
        glDeleteBuffers(1, &it->second.ebo);
        glDeleteBuffers(1, &it->second.vbo);
        glDeleteVertexArrays(1, &it->second.vao);
        m_meshes.erase(it);
    }

    void mesh_draw(MeshId mesh, const float view_proj[16],
                   const float cam_pos[3], float fog_r, float fog_g,
                   float fog_b, float fog_far) override {
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end() || !view_proj || !cam_pos) return;

        glUseProgram(m_mesh_prog);
        glUniformMatrix4fv(m_mesh_uniforms.vp, 1,
                           GL_FALSE, view_proj);
        glUniform3f(m_mesh_uniforms.cam, cam_pos[0],
                    cam_pos[1], cam_pos[2]);
        glUniform3f(m_mesh_uniforms.fog_color, fog_r,
                    fog_g, fog_b);
        glUniform1f(m_mesh_uniforms.fog_far, fog_far);

        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindVertexArray(it->second.vao);
        glDrawElements(GL_TRIANGLES, it->second.index_count, GL_UNSIGNED_SHORT,
                       nullptr);
        glBindVertexArray(0);
        glEnable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        ++m_draws;
    }

private:
    struct CelTex {
        int w, h;
        GLuint tex;
        bool has_mips;
        bool wrap_repeat = false;
        bool gpu_snapshot = false;
        bool replacement = false;
        int logical_w = 0, logical_h = 0;
        uint64_t content_hash = 0;
        // Identity of the guest-authored MIP chain this texture was built
        // from; 0 for a single-level upload. Two uploads whose level 0 bytes
        // match but whose chains differ must not share a cached texture.
        uint64_t lod_chain_hash = 0;

        // Identity the retired-texture cache files this texture under.
        CelCacheKey cache_key() const
        {
            return cel_cache_key(w, h, has_mips, wrap_repeat, content_hash,
                                 lod_chain_hash);
        }
    };

    struct MeshVertex {
        float x, y, z;
        float r, g, b, a;
    };

    struct MeshGpu {
        GLuint vao, vbo, ebo;
        int vertex_count;
        int index_count;
    };
    struct CltUniforms {
        GLint res = -1, tex = -1, ssb_mask = -1, secondary_source = -1;
        GLint tab_alpha_color = -1;
        GLint tab_control = -1, tab_constant0 = -1, tab_constant1 = -1;
        GLint dsb_has_ssb = -1, source_ssb = -1, has_source_ssb = -1, has_object_ssb = -1, dsb_select = -1;
        GLint dsb_a = -1, dsb_b = -1, dsb_alu = -1;
        GLint dsb_src_alpha = -1, dsb_dest_alpha = -1;
        GLint dsb_constant = -1, dsb_a0 = -1, dsb_a1 = -1;
        GLint dsb_b0 = -1, dsb_b1 = -1, dsb_alpha = -1;
        GLint discard_ssb = -1, separate_ssb_uv = -1;
        GLint discard_alpha = -1, discard_rgb = -1;
        GLint blend_mode = -1, blend_const = -1, blend_weight = -1;
        GLint dither_enable = -1, dither_a = -1, dither_b = -1;
        GLint db_source_linear = -1, db_source_height = -1;
        GLint feedback_source = -1;
        GLint uv_period = -1, ssb_keep = -1;
        GLint sharp = -1;
    };
    struct MeshUniforms {
        GLint vp = -1, cam = -1, fog_color = -1, fog_far = -1;
    };
    static constexpr uint32_t kMaxMeshes = 256;

    bool build_mesh_program() {
        if (!glDrawElements) return false;  // procs not loaded yet
        GLuint vs = compile(GL_VERTEX_SHADER, kMeshVertSrc);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kMeshFragSrc);
        if (!vs || !fs) return false;
        m_mesh_prog = glCreateProgram();
        glAttachShader(m_mesh_prog, vs);
        glAttachShader(m_mesh_prog, fs);
        glLinkProgram(m_mesh_prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(m_mesh_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            SDL_Log("gl_backend: mesh program link failed");
            return false;
        }
        m_mesh_uniforms.vp = glGetUniformLocation(m_mesh_prog, "u_vp");
        m_mesh_uniforms.cam = glGetUniformLocation(m_mesh_prog, "u_cam");
        m_mesh_uniforms.fog_color =
            glGetUniformLocation(m_mesh_prog, "u_fog_color");
        m_mesh_uniforms.fog_far = glGetUniformLocation(m_mesh_prog, "u_fog_far");
        return true;
    }

    // --- GL entry points, loaded dynamically (self-contained typedefs; system
    // PFNGL*PROC typedefs are absent on Apple's core-profile headers).
    using PGGenTextures = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindTexture = void(GLAPIENTRY*)(GLenum, GLuint);
    using PGDeleteTextures = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGTexImage2D = void(GLAPIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                           GLint, GLenum, GLenum, const void*);
    using PGTexParameteri = void(GLAPIENTRY*)(GLenum, GLenum, GLint);
    using PGTexParameterf = void(GLAPIENTRY*)(GLenum, GLenum, GLfloat);
    using PGGenerateMipmap = void(GLAPIENTRY*)(GLenum);
    using PGPixelStorei = void(GLAPIENTRY*)(GLenum, GLint);
    using PGCreateShader = GLuint(GLAPIENTRY*)(GLenum);
    using PGShaderSource = void(GLAPIENTRY*)(GLuint, GLsizei,
                                             const GLchar* const*, const GLint*);
    using PGCompileShader = void(GLAPIENTRY*)(GLuint);
    using PGGetShaderiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
    using PGGetShaderInfoLog = void(GLAPIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
    using PGDeleteShader = void(GLAPIENTRY*)(GLuint);
    using PGCreateProgram = GLuint(GLAPIENTRY*)(void);
    using PGAttachShader = void(GLAPIENTRY*)(GLuint, GLuint);
    using PGLinkProgram = void(GLAPIENTRY*)(GLuint);
    using PGDeleteProgram = void(GLAPIENTRY*)(GLuint);
    using PGGetProgramiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
    using PGUseProgram = void(GLAPIENTRY*)(GLuint);
    using PGGetUniformLocation = GLint(GLAPIENTRY*)(GLuint, const GLchar*);
    using PGUniform1i = void(GLAPIENTRY*)(GLint, GLint);
    using PGUniform2f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat);
    using PGGenBuffers = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindBuffer = void(GLAPIENTRY*)(GLenum, GLuint);
    using PGBufferData = void(GLAPIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
    using PGDeleteBuffers = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGGenVertexArrays = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindVertexArray = void(GLAPIENTRY*)(GLuint);
    using PGDeleteVertexArrays = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGEnableVertexAttribArray = void(GLAPIENTRY*)(GLuint);
    using PGVertexAttribPointer =
        void(GLAPIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    using PGEnable = void(GLAPIENTRY*)(GLenum);
    using PGBlendFunc = void(GLAPIENTRY*)(GLenum, GLenum);
    using PGBlendFuncSeparate =
        void(GLAPIENTRY*)(GLenum, GLenum, GLenum, GLenum);
    using PGBlendColor = void(GLAPIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using PGViewport = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei);
    using PGClearColor = void(GLAPIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using PGClearDepth = void(GLAPIENTRY*)(GLdouble);
    using PGClear = void(GLAPIENTRY*)(GLbitfield);
    using PGActiveTexture = void(GLAPIENTRY*)(GLenum);
    using PGDrawArrays = void(GLAPIENTRY*)(GLenum, GLint, GLsizei);
    using PGDrawElements = void(GLAPIENTRY*)(GLenum, GLsizei, GLenum,
                                             const void*);
    using PGUniformMatrix4fv = void(GLAPIENTRY*)(GLint, GLsizei, GLboolean,
                                                 const GLfloat*);
    using PGUniform3f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
    using PGUniform4f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    using PGUniform1f = void(GLAPIENTRY*)(GLint, GLfloat);
    using PGScissor = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei);
    using PGDepthMask = void(GLAPIENTRY*)(GLboolean);
    using PGDepthFunc = void(GLAPIENTRY*)(GLenum);
    using PGColorMask = void(GLAPIENTRY*)(GLboolean, GLboolean, GLboolean,
                                          GLboolean);
    using PGDisable = void(GLAPIENTRY*)(GLenum);
    using PGReadPixels = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei,
                                          GLenum, GLenum, void*);
    using PGGetError = GLenum(GLAPIENTRY*)(void);
    using PGGetFloatv = void(GLAPIENTRY*)(GLenum, GLfloat*);

    PGReadPixels glReadPixels = nullptr;
    PGGetError glGetError = nullptr;
    PGGetFloatv glGetFloatv = nullptr;

    PGGenTextures glGenTextures = nullptr;
    PGBindTexture glBindTexture = nullptr;
    PGDeleteTextures glDeleteTextures = nullptr;
    PGTexImage2D glTexImage2D = nullptr;
    PGTexParameteri glTexParameteri = nullptr;
    PGTexParameterf glTexParameterf = nullptr;
    PGGenerateMipmap glGenerateMipmap = nullptr;
    PGPixelStorei glPixelStorei = nullptr;
    PGCreateShader glCreateShader = nullptr;
    PGShaderSource glShaderSource = nullptr;
    PGCompileShader glCompileShader = nullptr;
    PGGetShaderiv glGetShaderiv = nullptr;
    PGGetShaderInfoLog glGetShaderInfoLog = nullptr;
    PGDeleteShader glDeleteShader = nullptr;
    PGCreateProgram glCreateProgram = nullptr;
    PGAttachShader glAttachShader = nullptr;
    PGLinkProgram glLinkProgram = nullptr;
    PGDeleteProgram glDeleteProgram = nullptr;
    PGGetProgramiv glGetProgramiv = nullptr;
    PGUseProgram glUseProgram = nullptr;
    PGGetUniformLocation glGetUniformLocation = nullptr;
    PGUniform1i glUniform1i = nullptr;
    PGUniform2f glUniform2f = nullptr;
    PGUniform1f glUniform1f = nullptr;
    PGUniform3f glUniform3f = nullptr;
    PGUniform4f glUniform4f = nullptr;
    PGUniformMatrix4fv glUniformMatrix4fv = nullptr;
    PGGenBuffers glGenBuffers = nullptr;
    PGBindBuffer glBindBuffer = nullptr;
    PGBufferData glBufferData = nullptr;
    PGDeleteBuffers glDeleteBuffers = nullptr;
    PGGenVertexArrays glGenVertexArrays = nullptr;
    PGBindVertexArray glBindVertexArray = nullptr;
    PGDeleteVertexArrays glDeleteVertexArrays = nullptr;
    PGEnableVertexAttribArray glEnableVertexAttribArray = nullptr;
    PGVertexAttribPointer glVertexAttribPointer = nullptr;
    PGEnable glEnable = nullptr;
    PGDisable glDisable = nullptr;
    PGBlendFunc glBlendFunc = nullptr;
    PGBlendFuncSeparate glBlendFuncSeparate = nullptr;
    PGBlendColor glBlendColor = nullptr;
    PGViewport glViewport = nullptr;
    PGClearColor glClearColor = nullptr;
    PGClearDepth glClearDepth = nullptr;
    PGClear glClear = nullptr;
    PGActiveTexture glActiveTexture = nullptr;
    PGDrawArrays glDrawArrays = nullptr;
    PGDrawElements glDrawElements = nullptr;
    PGDepthMask glDepthMask = nullptr;
    PGDepthFunc glDepthFunc = nullptr;
    PGColorMask glColorMask = nullptr;
    PGScissor glScissor = nullptr;

    template <typename T>
    T proc(const char* fn) {
        return reinterpret_cast<T>(SDL_GL_GetProcAddress(fn));
    }

    bool load_procs() {
#define P(v) \
    if (!(v = proc<decltype(v)>(#v))) return false
        P(glGenTextures); P(glBindTexture); P(glDeleteTextures); P(glTexImage2D);
        P(glTexParameteri); P(glTexParameterf); P(glPixelStorei);
        // Core GL 3.0+; optional so the classic path still works on a
        // driver that cannot expose mipmap generation.
        glGenerateMipmap = proc<PGGenerateMipmap>("glGenerateMipmap");
        P(glCreateShader); P(glShaderSource); P(glCompileShader); P(glGetShaderiv);
        P(glGetShaderInfoLog); P(glDeleteShader);
        P(glCreateProgram); P(glAttachShader); P(glLinkProgram); P(glDeleteProgram);
        P(glGetProgramiv); P(glUseProgram); P(glGetUniformLocation);
        P(glUniform1i); P(glUniform2f);
        P(glGenBuffers); P(glBindBuffer); P(glBufferData); P(glDeleteBuffers);
        P(glGenVertexArrays); P(glBindVertexArray); P(glDeleteVertexArrays);
        P(glEnableVertexAttribArray); P(glVertexAttribPointer);
        P(glEnable); P(glDisable); P(glBlendFunc); P(glViewport); P(glClearColor); P(glClearDepth); P(glClear);
        // Core GL 1.4; the destination-blender weighted mix needs a
        // per-channel destination factor and a separate alpha factor.
        P(glBlendFuncSeparate); P(glBlendColor);
        P(glActiveTexture); P(glDrawArrays);
        P(glDepthMask); P(glDepthFunc); P(glColorMask);
        P(glGetFloatv);
        // Optional readback support (DB-source framebuffer bridge); the
        // backend degrades to no-readback if the driver lacks them.
        glReadPixels = proc<PGReadPixels>("glReadPixels");
        glGetError = proc<PGGetError>("glGetError");
        // Optional mesh path + scissor support; degrade to 2D-only
        glDrawElements = proc<PGDrawElements>("glDrawElements");
        glScissor = proc<PGScissor>("glScissor");
        glUniformMatrix4fv = proc<PGUniformMatrix4fv>("glUniformMatrix4fv");
        glUniform3f = proc<PGUniform3f>("glUniform3f");
        glUniform4f = proc<PGUniform4f>("glUniform4f");
        glUniform1f = proc<PGUniform1f>("glUniform1f");
#undef P
        return true;
    }

    GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            SDL_Log("gl_backend: shader compile failed: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool build_program() {
        GLuint vs = compile(GL_VERTEX_SHADER, kVertSrc);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kFragSrc);
        if (!vs || !fs) return false;
        m_prog = glCreateProgram();
        glAttachShader(m_prog, vs);
        glAttachShader(m_prog, fs);
        glLinkProgram(m_prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            SDL_Log("gl_backend: program link failed");
            return false;
        }
        m_clt_uniforms.res = glGetUniformLocation(m_prog, "u_res");
        m_clt_uniforms.tex = glGetUniformLocation(m_prog, "u_tex");
        m_clt_uniforms.ssb_mask = glGetUniformLocation(m_prog, "u_ssb_mask");
        m_clt_uniforms.secondary_source =
            glGetUniformLocation(m_prog, "u_secondary_source");
        m_clt_uniforms.discard_ssb =
            glGetUniformLocation(m_prog, "u_discard_ssb");
        m_clt_uniforms.separate_ssb_uv =
            glGetUniformLocation(m_prog, "u_separate_ssb_uv");
        m_clt_uniforms.discard_alpha =
            glGetUniformLocation(m_prog, "u_discard_alpha");
        m_clt_uniforms.discard_rgb =
            glGetUniformLocation(m_prog, "u_discard_rgb");
        m_clt_uniforms.blend_mode =
            glGetUniformLocation(m_prog, "u_blend_mode");
        m_clt_uniforms.dither_enable =
            glGetUniformLocation(m_prog, "u_dither_enable");
        m_clt_uniforms.dither_a =
            glGetUniformLocation(m_prog, "u_dither_a");
        m_clt_uniforms.dither_b =
            glGetUniformLocation(m_prog, "u_dither_b");
        m_clt_uniforms.tab_alpha_color =
            glGetUniformLocation(m_prog, "u_tab_alpha_color");
        m_clt_uniforms.tab_control = glGetUniformLocation(m_prog, "u_tab_control");
        m_clt_uniforms.tab_constant0 = glGetUniformLocation(m_prog, "u_tab_constant0");
        m_clt_uniforms.tab_constant1 = glGetUniformLocation(m_prog, "u_tab_constant1");
        m_clt_uniforms.dsb_a = glGetUniformLocation(m_prog, "u_dsb_a");
        m_clt_uniforms.dsb_has_ssb = glGetUniformLocation(m_prog, "u_dsb_has_ssb");
    m_clt_uniforms.source_ssb = glGetUniformLocation(m_prog, "u_source_ssb");
    m_clt_uniforms.has_source_ssb = glGetUniformLocation(m_prog, "u_has_source_ssb");
    m_clt_uniforms.has_object_ssb = glGetUniformLocation(m_prog, "u_has_object_ssb");
    m_clt_uniforms.dsb_select = glGetUniformLocation(m_prog, "u_dsb_select");

        m_clt_uniforms.dsb_b = glGetUniformLocation(m_prog, "u_dsb_b");
        m_clt_uniforms.dsb_alu = glGetUniformLocation(m_prog, "u_dsb_alu");
        m_clt_uniforms.dsb_src_alpha = glGetUniformLocation(m_prog, "u_dsb_src_alpha");
        m_clt_uniforms.dsb_dest_alpha = glGetUniformLocation(m_prog, "u_dsb_dest_alpha");
        m_clt_uniforms.dsb_constant = glGetUniformLocation(m_prog, "u_dsb_constant");
        m_clt_uniforms.dsb_a0 = glGetUniformLocation(m_prog, "u_dsb_a0");
        m_clt_uniforms.dsb_a1 = glGetUniformLocation(m_prog, "u_dsb_a1");
        m_clt_uniforms.dsb_b0 = glGetUniformLocation(m_prog, "u_dsb_b0");
        m_clt_uniforms.dsb_b1 = glGetUniformLocation(m_prog, "u_dsb_b1");
        m_clt_uniforms.dsb_alpha = glGetUniformLocation(m_prog, "u_dsb_alpha");
        m_clt_uniforms.blend_const =
            glGetUniformLocation(m_prog, "u_blend_const");
        m_clt_uniforms.blend_weight =
            glGetUniformLocation(m_prog, "u_blend_weight");
        m_clt_uniforms.db_source_linear =
            glGetUniformLocation(m_prog, "u_db_source_linear");
        m_clt_uniforms.db_source_height =
            glGetUniformLocation(m_prog, "u_db_source_height");
        m_clt_uniforms.feedback_source =
            glGetUniformLocation(m_prog, "u_feedback_source");
        m_clt_uniforms.uv_period =
            glGetUniformLocation(m_prog, "u_uv_period");
        m_clt_uniforms.sharp = glGetUniformLocation(m_prog, "u_sharp");
        m_clt_uniforms.ssb_keep = glGetUniformLocation(m_prog, "u_ssb_keep");
        return true;
    }

    // Apply the interpreter's tracked clip rect (top-left origin) to GL.
    void apply_clip() {
        if (!m_clip_active) {
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        apply_scissor_rect(m_clip_rect);
    }

    void apply_scissor_rect(const CltClipRect& rect) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(rect.x),
                  static_cast<GLint>(m_h - rect.y - rect.h),
                  static_cast<GLsizei>(rect.w < 0.f ? 0.f : rect.w),
                  static_cast<GLsizei>(rect.h < 0.f ? 0.f : rect.h));
    }

    SDL_Window* m_window = nullptr;
    GlCltTargets m_targets;
    TextureReplacements m_replacements;
    // M2_TEXTURE_FILTER: linear and sharp both install linear sampling on
    // primary decoded textures; sharp also reshapes the sample position.
    bool m_linear_filter = false;
    bool m_sharp_filter = false;
    // M2_BITMAP_FILTER for the Projector bitmap scanout.
    TextureFilterMode m_bitmap_filter = TextureFilterMode::guest;
    // M2_HIRES_EFFECTS: feedback snapshots keep the target resolution.
    bool m_hires_effects = false;
    // M2_RENDER_SCALE (0 = targets follow the presentation size).
    int m_render_scale = 0;
    // Image region on the drawable; equals m_w x m_h unless render-scaled.
    int m_present_w = 640, m_present_h = 480;
    // M2_ANISOTROPY request (1 = off); stays 1 unless the extension is
    // present and the device reports a usable maximum.
    int m_anisotropy = 1;
    SDL_GLContext m_ctx = nullptr;
    GLuint m_prog = 0, m_vao = 0, m_vbo = 0;
    GLuint m_mesh_prog = 0;
    CltUniforms m_clt_uniforms{};
    MeshUniforms m_mesh_uniforms{};
    GLuint m_white_tex = 0;
    float m_w = 640.f, m_h = 480.f;
    int m_view_x = 0, m_view_y = 0;  // letterbox origin (GL bottom-left)
    // Current interpreter scissor rect (top-left origin); used to restore
    // the guest window clip after the per-draw Z-region split passes.
    bool m_clip_active = false;
    CltClipRect m_clip_rect{};
    uint32_t m_next_id = 0;
    std::vector<CelId> m_free_ids;
    MeshId m_next_mesh = 0;
    int m_draws = 0;
    bool m_persist_initialized = false;
    bool m_persist_target = false;
    std::unordered_map<CelId, CelTex> m_textures;
    CelTextureCache<CelTex> m_texture_cache;
    // CLT packets are submitted serially on the render thread. Reuse the
    // converted vertex storage between draws so packet cadence does not pay
    // a heap allocation/free pair for every guest submission.
    std::vector<Vertex> m_clt_vertices;
    uint64_t m_texture_cache_hits = 0;
    uint64_t m_texture_cache_misses = 0;
    std::unordered_map<MeshId, MeshGpu> m_meshes;

    bool persist_target_enabled() const
    {
        const char* value = std::getenv("M2_CLT_PERSIST_TARGET");
        return value ? value[0] != '\0' && value[0] != '0'
                     : m_persist_target;
    }
};

}  // namespace

std::unique_ptr<ICelBlitterBackend> make_gl_backend() {
#if defined(M2_HAVE_GL)
    return std::make_unique<GlBackend>();
#else
    return nullptr;
#endif
}

}  // namespace m2host
