// clean_renderer.cpp — clean OpenGL renderer.
//
// Replaces the recomp'd cel-blitter path as the preferred backend: real 3D
// geometry (track/car meshes), per-vertex (Gouraud) directional sun lighting,
// stencil-guarded planar projected shadows, and perspective-correct bilinear/
// mipmapped texturing — no cel artifacts. The 2D cel surface (cel_blit /
// fill_rect / scissor) remains available to the presentation boundary.
//
// gl_backend.cpp remains the fallback backend; selection lives in main.cpp
// (clean → gl blitter → null). GL entry points load through
// SDL_GL_GetProcAddress, same loader-free pattern as gl_backend.cpp.
//
// Selftest: M2_CLEAN_SELFTEST=1 ./m2host --frames N renders a procedural
// track + two cars each frame and dumps PPMs into build/scratch/clean_selftest
// (override dir: M2_CLEAN_SELFTEST_DIR) for offline pixel checks (windowed
// only — needs a real context).
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

// ---------------------------------------------------------------------------
// Column-major 4x4 math (right-handed, Y-up, matching the backend mesh
// convention).
using Mat4 = float[16];

void mat_identity(Mat4 m)
{
    std::memset(m, 0, sizeof(Mat4));
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void mat_mul(const Mat4 a, const Mat4 b, Mat4 out)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    std::memcpy(out, r, sizeof(Mat4));
}

void mat_perspective(float fov_y_rad, float aspect, float zn, float zf,
                     Mat4 out)
{
    std::memset(out, 0, sizeof(Mat4));
    const float f = 1.f / std::tan(fov_y_rad * 0.5f);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (zf + zn) / (zn - zf);
    out[11] = -1.f;
    out[14] = 2.f * zf * zn / (zn - zf);
}

void mat_look_at(float ex, float ey, float ez, float cx, float cy, float cz,
                 float ux_in, float uy_in, float uz_in, Mat4 out)
{
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-8f) { fx = 0; fy = 0; fz = -1; fl = 1; }
    fx /= fl; fy /= fl; fz /= fl;
    // s = normalize(cross(f, up)); u = cross(s, f)
    float sx = fy * uz_in - fz * uy_in;
    float sy = fz * ux_in - fx * uz_in;
    float sz = fx * uy_in - fy * ux_in;
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-8f) { sx = 1; sy = 0; sz = 0; sl = 1; }
    sx /= sl; sy /= sl; sz /= sl;
    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    out[0] = sx; out[4] = ux; out[8] = -fx;
    out[1] = sy; out[5] = uy; out[9] = -fy;
    out[2] = sz; out[6] = uz; out[10] = -fz;
    out[3] = 0; out[7] = 0; out[11] = 0;
    out[12] = -(sx * ex + sy * ey + sz * ez);
    out[13] = -(ux * ex + uy * ey + uz * ez);
    out[14] = (fx * ex + fy * ey + fz * ez);
    out[15] = 1.f;
}

void mat_rot_y(float rad, Mat4 m)
{
    std::memset(m, 0, sizeof(Mat4));
    const float c = std::cos(rad), s = std::sin(rad);
    m[0] = c; m[2] = -s;
    m[8] = s; m[10] = c;
    m[5] = m[15] = 1.f;
}

void mat_translate(float x, float y, float z, Mat4 m)
{
    mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

// ---------------------------------------------------------------------------
// geometry builders (all triangle soup → u16 indices, CCW from outside)

struct MeshData {
    std::vector<float> pos, nrm, uv, col;
    std::vector<uint16_t> idx;

    int vertex_count() const { return static_cast<int>(pos.size() / 3); }

    void vertex(float x, float y, float z, float nx, float ny, float nz,
                float u, float v, float r, float g, float b, float a)
    {
        pos.insert(pos.end(), {x, y, z});
        nrm.insert(nrm.end(), {nx, ny, nz});
        uv.insert(uv.end(), {u, v});
        col.insert(col.end(), {r, g, b, a});
    }

    void tri(uint16_t a, uint16_t b, uint16_t c) { idx.insert(idx.end(), {a, b, c}); }

    // Axis-aligned box centered at (cx,cy,cz); +X is the long axis for cars.
    void box(float cx, float cy, float cz, float sx, float sy, float sz,
             float r, float g, float b)
    {
        const float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        struct Face { float n[3]; float c[4][3]; };
        const Face faces[6] = {
            {{1, 0, 0}, {{cx + hx, cy - hy, cz - hz}, {cx + hx, cy + hy, cz - hz},
                         {cx + hx, cy + hy, cz + hz}, {cx + hx, cy - hy, cz + hz}}},
            {{-1, 0, 0}, {{cx - hx, cy - hy, cz + hz}, {cx - hx, cy + hy, cz + hz},
                          {cx - hx, cy + hy, cz - hz}, {cx - hx, cy - hy, cz - hz}}},
            {{0, 1, 0}, {{cx - hx, cy + hy, cz - hz}, {cx - hx, cy + hy, cz + hz},
                         {cx + hx, cy + hy, cz + hz}, {cx + hx, cy + hy, cz - hz}}},
            {{0, -1, 0}, {{cx - hx, cy - hy, cz + hz}, {cx - hx, cy - hy, cz - hz},
                          {cx + hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz + hz}}},
            {{0, 0, 1}, {{cx - hx, cy - hy, cz + hz}, {cx + hx, cy - hy, cz + hz},
                         {cx + hx, cy + hy, cz + hz}, {cx - hx, cy + hy, cz + hz}}},
            {{0, 0, -1}, {{cx + hx, cy - hy, cz - hz}, {cx - hx, cy - hy, cz - hz},
                          {cx - hx, cy + hy, cz - hz}, {cx + hx, cy + hy, cz - hz}}},
        };
        for (const Face& f : faces) {
            const uint16_t base = static_cast<uint16_t>(vertex_count());
            const float uvs[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
            for (int i = 0; i < 4; ++i)
                vertex(f.c[i][0], f.c[i][1], f.c[i][2], f.n[0], f.n[1], f.n[2],
                       uvs[i][0], uvs[i][1], r, g, b, 1.f);
            tri(base, base + 1, base + 2);
            tri(base, base + 2, base + 3);
        }
    }

    // Horizontal quad at height y, centered (cx,cz), size (sx,sz), uv tiles.
    void quad_xz(float cx, float y, float cz, float sx, float sz,
                 float uv_rep_u, float uv_rep_v, float r, float g, float b)
    {
        const float hx = sx * 0.5f, hz = sz * 0.5f;
        const uint16_t base = static_cast<uint16_t>(vertex_count());
        vertex(cx - hx, y, cz + hz, 0, 1, 0, 0, 0, r, g, b, 1);
        vertex(cx - hx, y, cz - hz, 0, 1, 0, 0, uv_rep_v, r, g, b, 1);
        vertex(cx + hx, y, cz - hz, 0, 1, 0, uv_rep_u, uv_rep_v, r, g, b, 1);
        vertex(cx + hx, y, cz + hz, 0, 1, 0, uv_rep_u, 0, r, g, b, 1);
        tri(base, base + 1, base + 2);
        tri(base, base + 2, base + 3);
    }

    // Flat ring ribbon (the demo track): centerline circle radius R, half
    // width hw, SEG quads; v runs around the loop in tex repeats.
    void ring(float radius, float hw, float y, int seg, float v_reps)
    {
        for (int i = 0; i <= seg; ++i) {
            const float t = static_cast<float>(i) / seg;
            const float ang = t * 6.2831853f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            const float nx = sa, nz = -ca;  // across-track direction
            vertex(radius * ca - nx * hw, y, radius * sa - nz * hw, 0, 1, 0,
                   0.f, t * v_reps, 1, 1, 1, 1);
            vertex(radius * ca + nx * hw, y, radius * sa + nz * hw, 0, 1, 0,
                   1.f, t * v_reps, 1, 1, 1, 1);
        }
        for (int i = 0; i < seg; ++i) {
            const uint16_t a = static_cast<uint16_t>(i * 2);
            tri(a, a + 1, a + 3);
            tri(a, a + 3, a + 2);
        }
    }

    // Smooth per-vertex normals from accumulated face normals (used by the
    // pos+color upload path, which carries no normals).
    void compute_smooth_normals()
    {
        const int n = vertex_count();
        nrm.assign(static_cast<size_t>(n) * 3, 0.f);
        auto add_face = [&](int a, int b, int c) {
            const float* pa = &pos[size_t(a) * 3];
            const float* pb = &pos[size_t(b) * 3];
            const float* pc = &pos[size_t(c) * 3];
            const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
            const float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
            const float nx = uy * vz - uz * vy;
            const float ny = uz * vx - ux * vz;
            const float nz = ux * vy - uy * vx;
            for (int i : {a, b, c}) {
                nrm[size_t(i) * 3 + 0] += nx;
                nrm[size_t(i) * 3 + 1] += ny;
                nrm[size_t(i) * 3 + 2] += nz;
            }
        };
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
            add_face(idx[i], idx[i + 1], idx[i + 2]);
        for (int i = 0; i < n; ++i) {
            float* v = &nrm[size_t(i) * 3];
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
            else { v[0] = 0; v[1] = 1; v[2] = 0; }
        }
    }
};

// ---------------------------------------------------------------------------
// deterministic procedural textures (RGBA8888, byte order r,g,b,a like the
// demo cel in main.cpp)

uint32_t pack_px(uint8_t r, uint8_t g, uint8_t b)
{
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

uint32_t hash_noise(int x, int y, uint32_t salt)
{
    uint32_t h = (uint32_t(x) * 73856093u) ^ (uint32_t(y) * 19349663u) ^ salt;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h & 0xFFu;
}

std::vector<uint32_t> make_asphalt(int sz)
{
    std::vector<uint32_t> px(size_t(sz) * sz);
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            const float u = float(x) / (sz - 1);
            uint8_t base = uint8_t(88 + hash_noise(x, y, 7) % 26);
            uint8_t r = base, g = base, b = uint8_t(base + 6);
            if (u < 0.035f || u > 0.965f) { r = g = b = 225; }              // edge line
            if (std::fabs(u - 0.5f) < 0.02f && ((y / 8) % 2 == 0)) {       // dashes
                r = 215; g = 180; b = 50;
            }
            px[size_t(y) * sz + x] = pack_px(r, g, b);
        }
    return px;
}

std::vector<uint32_t> make_grass(int sz)
{
    std::vector<uint32_t> px(size_t(sz) * sz);
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            const uint8_t g = uint8_t(74 + hash_noise(x, y, 31) % 30);
            px[size_t(y) * sz + x] =
                pack_px(uint8_t(g * 0.55f), g, uint8_t(g * 0.45f));
        }
    return px;
}

std::vector<uint32_t> make_car_paint(int sz)
{
    std::vector<uint32_t> px(size_t(sz) * sz);
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            const bool stripe = y >= sz * 5 / 16 && y < sz * 7 / 16;
            px[size_t(y) * sz + x] =
                stripe ? pack_px(240, 240, 240) : pack_px(196, 26, 34);
        }
    return px;
}

// ---------------------------------------------------------------------------
class CleanRenderer final : public ICelBlitterBackend {
public:
    bool init(SDL_Window* window) override;
    void shutdown() override;

    CelId cel_upload_rgba(int w, int h, const void* pixels) override;
    CelId cel_upload_rgba_ex(int w, int h, const void* pixels,
                             bool mipmap_linear, bool wrap_repeat) override;
    CelId cel_upload_rgba_ex_hashed(int w, int h, const void* pixels,
                                    bool mipmap_linear, bool wrap_repeat,
                                    uint64_t content_hash) override;
    bool clt_supports_authored_lod_chain() const override;
    CelId cel_upload_rgba_lod_chain(int w, int h, const void* pixels,
                                    const CelLodLevel* levels,
                                    size_t level_count, bool mipmap_linear,
                                    bool wrap_repeat,
                                    uint64_t content_hash,
                                    uint64_t lod_chain_hash) override;
    void cel_free(CelId cel) override;
    void cel_blit(CelId cel, float dx, float dy, float dw, float dh, float r,
                  float g, float b, float a) override;
    void fill_rect(float dx, float dy, float dw, float dh, float r, float g,
                   float b, float a) override;
    void scissor(float x, float y, float w, float h) override;
    void scissor_disable() override;
    bool clt_supports_triangle_batches() const override { return true; }
    void clt_draw(const CltVertex* vertices, uint32_t count, bool strip,
                  bool textured, CelId texture, float target_w,
                  float target_h, bool blend_enabled,
                  CltDiscard discard, CltDepth depth,
                  CltTextureSampling sampling,
                  CltBlendState blend,
                  bool independent_triangles = false) override;

    MeshId mesh_upload(int vc, const float* pos, const float* col, int ic,
                       const uint16_t* idx) override;
    MeshId mesh_upload_lit(int vc, const float* pos, const float* nrm,
                           const float* uv, const float* col, int ic,
                           const uint16_t* idx) override;
    void mesh_free(MeshId mesh) override;
    void mesh_draw(MeshId mesh, const float view_proj[16],
                   const float cam_pos[3], float fog_r, float fog_g,
                   float fog_b, float fog_far) override;
    void mesh_draw_state(MeshId mesh, const MeshDrawState& st) override;
    void mesh_draw_shadow(MeshId mesh, const float model[16],
                          const float view_proj[16], const float sun_dir[3],
                          float ground_y, float strength) override;

    void get_size(int* w, int* h) override
    {
        if (w) *w = static_cast<int>(m_w);
        if (h) *h = static_cast<int>(m_h);
    }

    void frame_begin(float clear_r, float clear_g, float clear_b) override;
    void frame_present() override;
    void render_selftest_scene(int frame) override;

    // Copy the current draw target (top-left origin RGBA8888).
    // Called before present by the retained-frame bridge and captures.
    bool grab_presented_rgba(uint32_t* dst, int w, int h) override;
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
    // The guest's TE clear-Z request (`UpdateClearZBCmdList`) fills the whole
    // bound Z surface with the fill word its `TEZBufferInfo` authored.  The
    // depth attachment is shared by every CLT color target, so the currently
    // bound target's depth is the guest Z plane.
    void clt_clear_z(uint32_t z_fill_word) override;
    void clt_clear_color(uint32_t red_word, uint32_t green_word,
                         uint32_t blue_word, uint32_t alpha_word) override;
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
    CelId snapshot_target(uint32_t, int, int, int, int, int, int, bool);
    CelId clt_snapshot_target_cel(uint32_t base, int x, int y, int w, int h,
                                  int cel_w, int cel_h, bool preserve_alpha = false) override;

    const char* name() const override { return "cleangl"; }

private:
    // --- GL typedefs (self-contained; Apple core headers lack PFNGL*) -------
    using PGGenTextures = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindTexture = void(GLAPIENTRY*)(GLenum, GLuint);
    using PGDeleteTextures = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGTexImage2D = void(GLAPIENTRY*)(GLenum, GLint, GLint, GLsizei,
                                           GLsizei, GLint, GLenum, GLenum,
                                           const void*);
    using PGTexParameteri = void(GLAPIENTRY*)(GLenum, GLenum, GLint);
    using PGTexParameterf = void(GLAPIENTRY*)(GLenum, GLenum, GLfloat);
    using PGPixelStorei = void(GLAPIENTRY*)(GLenum, GLint);
    using PGGenerateMipmap = void(GLAPIENTRY*)(GLenum);
    using PGCreateShader = GLuint(GLAPIENTRY*)(GLenum);
    using PGShaderSource = void(GLAPIENTRY*)(GLuint, GLsizei,
                                             const GLchar* const*, const GLint*);
    using PGCompileShader = void(GLAPIENTRY*)(GLuint);
    using PGGetShaderiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
    using PGGetShaderInfoLog = void(GLAPIENTRY*)(GLuint, GLsizei, GLsizei*,
                                                 GLchar*);
    using PGDeleteShader = void(GLAPIENTRY*)(GLuint);
    using PGCreateProgram = GLuint(GLAPIENTRY*)(void);
    using PGAttachShader = void(GLAPIENTRY*)(GLuint, GLuint);
    using PGLinkProgram = void(GLAPIENTRY*)(GLuint);
    using PGDeleteProgram = void(GLAPIENTRY*)(GLuint);
    using PGGetProgramiv = void(GLAPIENTRY*)(GLuint, GLenum, GLint*);
    using PGUseProgram = void(GLAPIENTRY*)(GLuint);
    using PGGetUniformLocation = GLint(GLAPIENTRY*)(GLuint, const GLchar*);
    using PGUniform1i = void(GLAPIENTRY*)(GLint, GLint);
    using PGUniform1f = void(GLAPIENTRY*)(GLint, GLfloat);
    using PGUniform2f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat);
    using PGUniform3f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
    using PGUniform4f = void(GLAPIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    using PGUniformMatrix3fv = void(GLAPIENTRY*)(GLint, GLsizei, GLboolean,
                                                 const GLfloat*);
    using PGUniformMatrix4fv = void(GLAPIENTRY*)(GLint, GLsizei, GLboolean,
                                                 const GLfloat*);
    using PGGenBuffers = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindBuffer = void(GLAPIENTRY*)(GLenum, GLuint);
    using PGBufferData = void(GLAPIENTRY*)(GLenum, GLsizeiptr, const void*,
                                           GLenum);
    using PGDeleteBuffers = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGGenVertexArrays = void(GLAPIENTRY*)(GLsizei, GLuint*);
    using PGBindVertexArray = void(GLAPIENTRY*)(GLuint);
    using PGDeleteVertexArrays = void(GLAPIENTRY*)(GLsizei, const GLuint*);
    using PGEnableVertexAttribArray = void(GLAPIENTRY*)(GLuint);
    using PGDisableVertexAttribArray = void(GLAPIENTRY*)(GLuint);
    using PGVertexAttribPointer =
        void(GLAPIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                          const void*);
    using PGEnable = void(GLAPIENTRY*)(GLenum);
    using PGDisable = void(GLAPIENTRY*)(GLenum);
    using PGBlendFunc = void(GLAPIENTRY*)(GLenum, GLenum);
    using PGBlendFuncSeparate =
        void(GLAPIENTRY*)(GLenum, GLenum, GLenum, GLenum);
    using PGBlendColor = void(GLAPIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using PGViewport = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei);
    using PGClearColor = void(GLAPIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using PGClear = void(GLAPIENTRY*)(GLbitfield);
    using PGClearDepth = void(GLAPIENTRY*)(GLdouble);
    using PGActiveTexture = void(GLAPIENTRY*)(GLenum);
    using PGDrawArrays = void(GLAPIENTRY*)(GLenum, GLint, GLsizei);
    using PGDrawElements = void(GLAPIENTRY*)(GLenum, GLsizei, GLenum,
                                             const void*);
    using PGDepthFunc = void(GLAPIENTRY*)(GLenum);
    using PGDepthMask = void(GLAPIENTRY*)(GLboolean);
    using PGColorMask = void(GLAPIENTRY*)(GLboolean, GLboolean, GLboolean,
                                          GLboolean);
    using PGScissor = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei);
    using PGPolygonOffset = void(GLAPIENTRY*)(GLfloat, GLfloat);
    using PGStencilFunc = void(GLAPIENTRY*)(GLenum, GLint, GLuint);
    using PGStencilOp = void(GLAPIENTRY*)(GLenum, GLenum, GLenum);
    using PGStencilMask = void(GLAPIENTRY*)(GLuint);
    using PGLGetIntegerv = void(GLAPIENTRY*)(GLenum, GLint*);
    using PGGetFloatv = void(GLAPIENTRY*)(GLenum, GLfloat*);
    using PGReadPixels = void(GLAPIENTRY*)(GLint, GLint, GLsizei, GLsizei,
                                           GLenum, GLenum, void*);

    PGGenTextures glGenTextures = nullptr;
    PGBindTexture glBindTexture = nullptr;
    PGDeleteTextures glDeleteTextures = nullptr;
    PGTexImage2D glTexImage2D = nullptr;
    PGTexParameteri glTexParameteri = nullptr;
    PGTexParameterf glTexParameterf = nullptr;
    PGPixelStorei glPixelStorei = nullptr;
    PGGenerateMipmap glGenerateMipmap = nullptr;
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
    PGUniform1f glUniform1f = nullptr;
    PGUniform2f glUniform2f = nullptr;
    PGUniform3f glUniform3f = nullptr;
    PGUniform4f glUniform4f = nullptr;
    PGUniformMatrix3fv glUniformMatrix3fv = nullptr;
    PGUniformMatrix4fv glUniformMatrix4fv = nullptr;
    PGGenBuffers glGenBuffers = nullptr;
    PGBindBuffer glBindBuffer = nullptr;
    PGBufferData glBufferData = nullptr;
    PGDeleteBuffers glDeleteBuffers = nullptr;
    PGGenVertexArrays glGenVertexArrays = nullptr;
    PGBindVertexArray glBindVertexArray = nullptr;
    PGDeleteVertexArrays glDeleteVertexArrays = nullptr;
    PGEnableVertexAttribArray glEnableVertexAttribArray = nullptr;
    PGDisableVertexAttribArray glDisableVertexAttribArray = nullptr;
    PGVertexAttribPointer glVertexAttribPointer = nullptr;
    PGEnable glEnable = nullptr;
    PGDisable glDisable = nullptr;
    PGBlendFunc glBlendFunc = nullptr;
    PGBlendFuncSeparate glBlendFuncSeparate = nullptr;
    PGBlendColor glBlendColor = nullptr;
    PGViewport glViewport = nullptr;
    PGClearColor glClearColor = nullptr;
    PGClear glClear = nullptr;
    PGClearDepth glClearDepth = nullptr;
    PGActiveTexture glActiveTexture = nullptr;
    PGDrawArrays glDrawArrays = nullptr;
    PGDrawElements glDrawElements = nullptr;
    PGDepthFunc glDepthFunc = nullptr;
    PGDepthMask glDepthMask = nullptr;
    PGColorMask glColorMask = nullptr;
    PGScissor glScissor = nullptr;
    PGPolygonOffset glPolygonOffset = nullptr;
    PGStencilFunc glStencilFunc = nullptr;
    PGStencilOp glStencilOp = nullptr;
    PGStencilMask glStencilMask = nullptr;
    PGLGetIntegerv glGetIntegerv = nullptr;
    PGGetFloatv glGetFloatv = nullptr;
    PGReadPixels glReadPixels = nullptr;

    template <typename T>
    T proc(const char* fn)
    {
        return reinterpret_cast<T>(SDL_GL_GetProcAddress(fn));
    }

    bool load_procs();
    GLuint compile(GLenum type, const char* src);
    bool build_programs();
    CelId try_texture_replacement(int w, int h, uint64_t source_hash,
                                  bool mipmap_linear, bool wrap_repeat);

    // --- shaders -------------------------------------------------------------
    // 2D: window-space textured quads (HUD parity with gl_backend).
    static constexpr const char* kVert2D = R"(#version 330 core
layout(location=0) in vec4 a_pos;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_col;
layout(location=3) in vec2 a_mask_uv;
uniform vec2 u_res;
noperspective out vec2 v_db_uv;
noperspective out vec2 v_db_mask_uv;
noperspective out vec2 v_guest_xy;
out vec2 v_uv; out vec4 v_col; out vec2 v_mask_uv;
void main() {
    v_guest_xy = a_pos.xy;
    vec2 ndc = vec2(a_pos.x / u_res.x * 2.0 - 1.0,
                    1.0 - a_pos.y / u_res.y * 2.0);
    gl_Position = vec4(ndc * a_pos.w, a_pos.z * a_pos.w, a_pos.w);
    v_db_uv = a_uv; v_db_mask_uv = a_mask_uv;
    v_uv = a_uv; v_col = a_col; v_mask_uv = a_mask_uv;
}
)";
    static constexpr const char* kFrag2D = R"(#version 330 core
noperspective in vec2 v_db_uv;
noperspective in vec2 v_db_mask_uv;
noperspective in vec2 v_guest_xy;
in vec2 v_uv; in vec4 v_col; in vec2 v_mask_uv;
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
        // v_mask_uv carries the independent DB-source coordinates. Resolve
        // an unbounded linear address at the fragment row boundary.
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

    // 3D: per-vertex (Gouraud) sun lighting; fragment adds distance fog.
    // Texture coordinates interpolate with perspective correction (GLSL
    // varyings are perspective-correct by construction — no affine/cel crawl).
    static constexpr const char* kVert3D = R"(#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_nrm;
layout(location=2) in vec2 a_uv;
layout(location=3) in vec4 a_col;
uniform mat4 u_mvp;
uniform mat4 u_model;
uniform mat3 u_nmat;
uniform vec3 u_cam;
uniform vec3 u_sun;
out vec2 v_uv; out vec4 v_col; out float v_ndl; out float v_dist;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    vec3 wn = normalize(u_nmat * a_nrm);
    v_ndl = dot(wn, u_sun);
    v_uv = a_uv; v_col = a_col;
    v_dist = distance((u_model * vec4(a_pos, 1.0)).xyz, u_cam);
}
)";
    static constexpr const char* kFrag3D = R"(#version 330 core
in vec2 v_uv; in vec4 v_col; in float v_ndl; in float v_dist;
uniform sampler2D u_tex;
uniform int u_has_tex;
uniform vec3 u_sunrgb;
uniform vec3 u_amb;
uniform vec3 u_fogrgb;
uniform float u_fogfar;
uniform float u_alpha;
out vec4 frag;
void main() {
    vec4 tex = (u_has_tex == 1) ? texture(u_tex, v_uv) : vec4(1.0);
    float nl = max(v_ndl, 0.0);
    vec3 lit = tex.rgb * v_col.rgb * (u_amb + u_sunrgb * nl);
    float f = 1.0;
    if (u_fogfar > 0.0)
        f = clamp((u_fogfar - v_dist) / (u_fogfar * 0.35), 0.0, 1.0);
    frag = vec4(mix(u_fogrgb, lit, f), tex.a * v_col.a * u_alpha);
}
)";

    // Shadow pass: planar-projected flat dark quads.
    static constexpr const char* kVertSH = R"(#version 330 core
layout(location=0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";
    static constexpr const char* kFragSH = R"(#version 330 core
uniform float u_alpha;
out vec4 frag;
void main() { frag = vec4(0.0, 0.0, 0.0, u_alpha); }
)";

    struct Prog {
        GLuint id = 0;
        std::unordered_map<std::string, GLint> loc;
    };

    struct CltUniforms {
        GLint res = -1;
        GLint tex = -1;
        GLint ssb_mask = -1;
        GLint secondary_source = -1;
        GLint tab_alpha_color = -1;
        GLint tab_control = -1, tab_constant0 = -1, tab_constant1 = -1;
        GLint dsb_has_ssb = -1, source_ssb = -1, has_source_ssb = -1, has_object_ssb = -1, dsb_select = -1;
        GLint dsb_a = -1, dsb_b = -1, dsb_alu = -1;
        GLint dsb_src_alpha = -1, dsb_dest_alpha = -1;
        GLint dsb_constant = -1, dsb_a0 = -1, dsb_a1 = -1;
        GLint dsb_b0 = -1, dsb_b1 = -1, dsb_alpha = -1;
        GLint separate_ssb_uv = -1;
        GLint discard_ssb = -1;
        GLint discard_alpha = -1;
        GLint discard_rgb = -1;
        GLint blend_mode = -1;
        GLint dither_enable = -1, dither_a = -1, dither_b = -1;
        GLint blend_const = -1;
        GLint blend_weight = -1;
        GLint db_source_linear = -1;
        GLint db_source_height = -1;
        GLint feedback_source = -1;
        GLint ssb_keep = -1;
        GLint uv_period = -1;
        GLint sharp = -1;
    };

    GLint u(Prog& p, const char* name)
    {
        auto it = p.loc.find(name);
        if (it != p.loc.end()) return it->second;
        const GLint l = glGetUniformLocation(p.id, name);
        p.loc[name] = l;
        return l;
    }

    struct GlMesh {
        GLuint vao = 0, vbo = 0, ibo = 0;
        int index_count = 0;
    };
    struct GlTex {
        int w = 0, h = 0;
        GLuint tex = 0;
        bool has_mips = false;
        bool wrap_repeat = false;
        bool gpu_snapshot = false;
        bool replacement = false;
        int logical_w = 0, logical_h = 0;
        uint64_t content_hash = 0;
        // Identity of the guest-authored MIP chain this texture was built
        // from; 0 for a single-level upload.  Two uploads can share level 0
        // while the guest loaded different coarser bytes for them.
        uint64_t lod_chain_hash = 0;
        // Last CLT sampler state installed on this texture.  The guest can
        // author a different filter/wrap per draw, but repeated world draws
        // usually keep the same tuple; avoid reissuing identical driver
        // parameter calls without changing the effective GL state.
        GLint sampler_min = -1;
        GLint sampler_mag = -1;
        GLint sampler_wrap_s = -1;
        GLint sampler_wrap_t = -1;
        GLint sampler_max_level = -1;
        GLfloat sampler_aniso = -1.f;

        // Identity the retired-texture cache files this texture under.
        CelCacheKey cache_key() const
        {
            return cel_cache_key(w, h, has_mips, wrap_repeat, content_hash,
                                 lod_chain_hash);
        }
    };

    struct Vert2D {
        float x, y, z, w;
        float u, v;
        float r, g, b, a;
        float mask_u, mask_v;
    };

    void draw_quad_2d(GLuint tex, float dx, float dy, float dw, float dh,
                      float r, float g, float b, float a, bool sharp = false);
    void draw_mesh(MeshId mesh, const float mvp[16], const float model[16],
                   const MeshDrawState& st);
    void apply_clip();
    void apply_scissor_rect(const CltClipRect& rect);

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
    // M2_ANISOTROPY request (1 = off). m_anisotropy stays 1 unless the
    // extension is present and the device reports a usable maximum, so the
    // draw path can treat `m_anisotropy > 1` as "granted".
    int m_anisotropy = 1;
    SDL_GLContext m_ctx = nullptr;
    float m_w = 640.f, m_h = 480.f;
    int m_view_x = 0, m_view_y = 0;  // letterbox origin (GL bottom-left)
    // Current interpreter scissor rect (top-left origin); used to restore
    // the guest window clip after the per-draw Z-region split passes.
    bool m_clip_active = false;
    CltClipRect m_clip_rect{};
    bool m_stencil = false;
    bool m_persist_initialized = false;
    bool m_persist_target = false;

    bool persist_target_enabled() const
    {
        const char* value = std::getenv("M2_CLT_PERSIST_TARGET");
        return value ? value[0] != '\0' && value[0] != '0'
                     : m_persist_target;
    }

    GLuint m_vao2d = 0, m_vbo2d = 0;
    GLuint m_white_tex = 0;
    Prog m_p2d, m_p3d, m_psh;
    CltUniforms m_clt_uniforms;
    std::unordered_map<MeshId, GlMesh> m_meshes;
    std::unordered_map<CelId, GlTex> m_textures;
    CelTextureCache<GlTex> m_texture_cache;
    // CLT packets are submitted serially on the render thread. Reuse the
    // converted vertex storage between draws so packet cadence does not pay
    // a heap allocation/free pair for every guest submission.
    std::vector<Vert2D> m_clt_vertices;
    uint64_t m_texture_cache_hits = 0;
    uint64_t m_texture_cache_misses = 0;
    uint32_t m_next_cel = 0;
    std::vector<CelId> m_free_cels;
    uint32_t m_next_mesh = 0;

    // selftest mode (M2_CLEAN_SELFTEST env): render demo scene + dump PPMs
    bool m_selftest = false;
    int m_selftest_frames = 4;
    int m_selftest_frame = 0;

    // demo resources (lazy)
    MeshId m_demo_ground = kInvalidMesh, m_demo_track = kInvalidMesh,
           m_demo_start = kInvalidMesh, m_demo_body = kInvalidMesh,
           m_demo_details = kInvalidMesh;
    CelId m_tex_grass = kInvalidCel, m_tex_road = kInvalidCel,
          m_tex_car = kInvalidCel;
};

bool CleanRenderer::init(SDL_Window* window)
{
    m_window = window;
    m_ctx = SDL_GL_CreateContext(window);
    if (!m_ctx) {
        SDL_Log("clean_renderer: context creation failed: %s", SDL_GetError());
        return false;
    }
    if (!load_procs()) {
        SDL_Log("clean_renderer: required GL functions unavailable");
        return false;
    }
    m_replacements.load(std::getenv("M2_TEXTURE_REPLACEMENTS"));
    const TextureFilterMode filter = texture_filter_request();
    m_linear_filter = filter != TextureFilterMode::guest;
    m_sharp_filter = filter == TextureFilterMode::sharp;
    m_bitmap_filter = bitmap_filter_request();
    m_hires_effects = hires_effects_requested();
    // M2_ANISOTROPY: optional presentation request. Anisotropy only has an
    // effect under linear minification, so a granted request also engages the
    // linear override above. An unavailable extension or a device maximum
    // below 2x is refused with a log line; guest sampling stays in force.
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
            std::printf("[gfx] clean_renderer anisotropic filtering %dx "
                        "(device max %gx)\n",
                        m_anisotropy, static_cast<double>(max_anisotropy));
        } else {
            std::printf("[gfx] clean_renderer anisotropic filtering "
                        "unavailable; keeping guest texture sampling\n");
            m_anisotropy = 1;
        }
    }
    if (!m_targets.init() || !build_programs()) return false;
    // M2_ANTIALIASING: MSAA on the addressed CLT color targets. The request is
    // clamped to the context's GL_MAX_SAMPLES; a device that cannot serve at
    // least 2x keeps single-sample targets and says so instead of silently
    // presenting a fake setting.
    const int aa_request = clt_antialiasing_request();
    if (aa_request > 1) {
        const int max_samples = m_targets.usable_samples();
        if (max_samples >= 2) {
            const int samples =
                aa_request < max_samples ? aa_request : max_samples;
            m_targets.set_samples(samples);
            std::printf("[gfx] clean_renderer anti-aliasing %dx MSAA "
                        "(device max %dx)\n",
                        samples, max_samples);
        } else {
            std::printf("[gfx] clean_renderer anti-aliasing unavailable; "
                        "keeping single-sample targets\n");
        }
    }
    // CLT draws are frequent enough that querying uniform locations on the
    // guest packet path is measurable. Resolve the fixed shader interface
    // once after linking; values are still set for every draw exactly as
    // before, preserving the guest state boundary.
    m_clt_uniforms.res = u(m_p2d, "u_res");
    m_clt_uniforms.tex = u(m_p2d, "u_tex");
    m_clt_uniforms.ssb_mask = u(m_p2d, "u_ssb_mask");
    m_clt_uniforms.secondary_source = u(m_p2d, "u_secondary_source");
    m_clt_uniforms.tab_alpha_color = u(m_p2d, "u_tab_alpha_color");
    m_clt_uniforms.tab_control = u(m_p2d, "u_tab_control");
    m_clt_uniforms.tab_constant0 = u(m_p2d, "u_tab_constant0");
    m_clt_uniforms.tab_constant1 = u(m_p2d, "u_tab_constant1");
    m_clt_uniforms.dsb_a = u(m_p2d, "u_dsb_a");
    m_clt_uniforms.dsb_has_ssb = u(m_p2d, "u_dsb_has_ssb");
    m_clt_uniforms.source_ssb = u(m_p2d, "u_source_ssb");
    m_clt_uniforms.has_source_ssb = u(m_p2d, "u_has_source_ssb");
    m_clt_uniforms.has_object_ssb = u(m_p2d, "u_has_object_ssb");
    m_clt_uniforms.dsb_select = u(m_p2d, "u_dsb_select");

    m_clt_uniforms.dsb_b = u(m_p2d, "u_dsb_b");
    m_clt_uniforms.dsb_alu = u(m_p2d, "u_dsb_alu");
    m_clt_uniforms.dsb_src_alpha = u(m_p2d, "u_dsb_src_alpha");
    m_clt_uniforms.dsb_dest_alpha = u(m_p2d, "u_dsb_dest_alpha");
    m_clt_uniforms.dsb_constant = u(m_p2d, "u_dsb_constant");
    m_clt_uniforms.dsb_a0 = u(m_p2d, "u_dsb_a0");
    m_clt_uniforms.dsb_a1 = u(m_p2d, "u_dsb_a1");
    m_clt_uniforms.dsb_b0 = u(m_p2d, "u_dsb_b0");
    m_clt_uniforms.dsb_b1 = u(m_p2d, "u_dsb_b1");
    m_clt_uniforms.dsb_alpha = u(m_p2d, "u_dsb_alpha");
    m_clt_uniforms.separate_ssb_uv = u(m_p2d, "u_separate_ssb_uv");
    m_clt_uniforms.discard_ssb = u(m_p2d, "u_discard_ssb");
    m_clt_uniforms.discard_alpha = u(m_p2d, "u_discard_alpha");
    m_clt_uniforms.discard_rgb = u(m_p2d, "u_discard_rgb");
    m_clt_uniforms.blend_mode = u(m_p2d, "u_blend_mode");
    m_clt_uniforms.dither_enable = u(m_p2d, "u_dither_enable");
    m_clt_uniforms.dither_a = u(m_p2d, "u_dither_a");
    m_clt_uniforms.dither_b = u(m_p2d, "u_dither_b");
    m_clt_uniforms.blend_const = u(m_p2d, "u_blend_const");
    m_clt_uniforms.blend_weight = u(m_p2d, "u_blend_weight");
    m_clt_uniforms.db_source_linear = u(m_p2d, "u_db_source_linear");
    m_clt_uniforms.db_source_height = u(m_p2d, "u_db_source_height");
    m_clt_uniforms.feedback_source = u(m_p2d, "u_feedback_source");
    m_clt_uniforms.ssb_keep = u(m_p2d, "u_ssb_keep");
    m_clt_uniforms.uv_period = u(m_p2d, "u_uv_period");
    m_clt_uniforms.sharp = u(m_p2d, "u_sharp");
    // Guest-visible Z-plane convention (see cel_blitter.h): the plane holds
    // the guest's reciprocal-W, so its far value is 0.0, and a target's depth
    // attachment must start there rather than at GL's clear-depth default
    // (1.0 = nearest, which inverts every comparing DBZCNTL mask).
    m_targets.set_depth_clear(kCltTeZFarDepth);
    glClearDepth(kCltTeZFarDepth);

    glGenVertexArrays(1, &m_vao2d);
    glBindVertexArray(m_vao2d);
    glGenBuffers(1, &m_vbo2d);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vert2D) * 4, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vert2D),
                          reinterpret_cast<void*>(offsetof(Vert2D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vert2D),
                          reinterpret_cast<void*>(offsetof(Vert2D, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vert2D),
                          reinterpret_cast<void*>(offsetof(Vert2D, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vert2D),
                          reinterpret_cast<void*>(offsetof(Vert2D, mask_u)));

    // 1x1 white source for fill_rect tints.
    const uint32_t white = pack_px(255, 255, 255);
    glGenTextures(1, &m_white_tex);
    glBindTexture(GL_TEXTURE_2D, m_white_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, &white);

    // No back-face culling: mesh winding arrives from several producers
    // (recomp path, glTF exports) whose orientation we do not police here;
    // depth testing keeps the image correct.
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLint sb = 0;
    glGetIntegerv(GL_STENCIL_BITS, &sb);
    m_stencil = sb > 0;
    if (m_stencil) {
        glStencilMask(0xFF);
        glClearStencil(0);
    }

    const char* st_env = std::getenv("M2_CLEAN_SELFTEST");
    if (st_env && *st_env) {
        m_selftest = true;
        m_selftest_frames = std::atoi(st_env);
        if (m_selftest_frames <= 0) m_selftest_frames = 4;
    }

    // M2_RENDER_SCALE: fixed internal resolution, bounded by the device's
    // renderbuffer limit.
    m_render_scale = render_scale_request();
    if (m_render_scale > 0) {
        GLint max_rb = 0;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
        const int granted = clamp_render_scale(m_render_scale, max_rb);
        if (granted != m_render_scale)
            std::printf("[gfx] clean_renderer render scale %dx exceeds the "
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
    // M2_CROP_OVERSCAN (default on): present only the guest's 576x432
    // carve, scaled to fill; =0 keeps the original margin.
    set_overscan_crop(overscan_crop_requested());
    m_targets.set_crop_overscan(overscan_crop());
    std::printf("[gfx] overscan crop %s\n", overscan_crop() ? "on" : "off (M2_CROP_OVERSCAN=0)");
    if (m_render_scale > 0)
        std::printf("[gfx] render scale %dx (%dx%d targets)\n", m_render_scale,
                    w, h);
    if (m_sharp_filter) std::printf("[gfx] texture filter sharp bilinear\n");
    if (m_bitmap_filter != TextureFilterMode::guest)
        std::printf("[gfx] bitmap filter %s\n",
                    m_bitmap_filter == TextureFilterMode::sharp ? "sharp" : "linear");
    if (m_hires_effects) std::printf("[gfx] high-resolution feedback effects\n");
    std::printf("[gfx] %s backend ready (%dx%d px, stencil=%d)\n", name(), w,
                h, m_stencil ? 1 : 0);
    return true;
}

void CleanRenderer::shutdown()
{
    if (std::getenv("M2_FRAME_TIMING"))
        std::fprintf(stderr,
                     "[texture-cache] backend=clean hits=%llu misses=%llu "
                     "resident=%zu bytes=%zu\n",
                     (unsigned long long)m_texture_cache_hits,
                     (unsigned long long)m_texture_cache_misses,
                     m_texture_cache.size(), m_texture_cache.bytes());
    m_targets.shutdown();
    for (auto& [id, mm] : m_meshes) {
        (void)id;
        if (mm.ibo) glDeleteBuffers(1, &mm.ibo);
        if (mm.vbo) glDeleteBuffers(1, &mm.vbo);
        if (mm.vao) glDeleteVertexArrays(1, &mm.vao);
    }
    m_meshes.clear();
    for (auto& [id, t] : m_textures) {
        (void)id;
        glDeleteTextures(1, &t.tex);
    }
    m_textures.clear();
    for (const auto& entry : m_texture_cache.records())
        if (entry.record.tex) glDeleteTextures(1, &entry.record.tex);
    m_texture_cache.clear();
    m_free_cels.clear();
    m_next_cel = 0;
    if (m_white_tex) glDeleteTextures(1, &m_white_tex);
    if (m_vbo2d) glDeleteBuffers(1, &m_vbo2d);
    if (m_vao2d) glDeleteVertexArrays(1, &m_vao2d);
    if (m_p2d.id) glDeleteProgram(m_p2d.id);
    if (m_p3d.id) glDeleteProgram(m_p3d.id);
    if (m_psh.id) glDeleteProgram(m_psh.id);
    m_vbo2d = m_vao2d = m_white_tex = 0;
    m_p2d = m_p3d = m_psh = Prog{};
    if (m_ctx) SDL_GL_DestroyContext(m_ctx);
    m_ctx = nullptr;
}

bool CleanRenderer::load_procs()
{
#define CP(v) \
    if (!(v = proc<decltype(v)>(#v))) return false
    CP(glGenTextures); CP(glBindTexture); CP(glDeleteTextures);
    CP(glTexImage2D); CP(glTexParameteri); CP(glPixelStorei);
    CP(glTexParameterf);
    CP(glGenerateMipmap);
    CP(glCreateShader); CP(glShaderSource); CP(glCompileShader);
    CP(glGetShaderiv); CP(glGetShaderInfoLog); CP(glDeleteShader);
    CP(glCreateProgram); CP(glAttachShader); CP(glLinkProgram);
    CP(glDeleteProgram); CP(glGetProgramiv); CP(glUseProgram);
    CP(glGetUniformLocation); CP(glUniform1i); CP(glUniform1f);
    CP(glUniform2f); CP(glUniform3f); CP(glUniform4f); CP(glUniformMatrix3fv);
    CP(glUniformMatrix4fv);
    CP(glGenBuffers); CP(glBindBuffer); CP(glBufferData); CP(glDeleteBuffers);
    CP(glGenVertexArrays); CP(glBindVertexArray); CP(glDeleteVertexArrays);
    CP(glEnableVertexAttribArray); CP(glDisableVertexAttribArray);
    CP(glVertexAttribPointer);
    CP(glEnable); CP(glDisable); CP(glBlendFunc); CP(glViewport);
    // Core GL 1.4; the destination-blender weighted mix needs a per-channel
    // destination factor and a separate alpha factor.
    CP(glBlendFuncSeparate); CP(glBlendColor);
    CP(glClearColor); CP(glClear); CP(glClearDepth); CP(glActiveTexture);
    CP(glDrawArrays); CP(glDrawElements);
    CP(glDepthFunc); CP(glDepthMask); CP(glColorMask); CP(glScissor);
    CP(glPolygonOffset);
    CP(glStencilFunc); CP(glStencilOp); CP(glStencilMask);
    CP(glGetIntegerv); CP(glReadPixels);
    CP(glGetFloatv);
#undef CP
    return true;
}

GLuint CleanRenderer::compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        SDL_Log("clean_renderer: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool CleanRenderer::build_programs()
{
    const struct { const char* vs; const char* fs; Prog* out; } specs[3] = {
        {kVert2D, kFrag2D, &m_p2d},
        {kVert3D, kFrag3D, &m_p3d},
        {kVertSH, kFragSH, &m_psh},
    };
    for (const auto& sp : specs) {
        GLuint v = compile(GL_VERTEX_SHADER, sp.vs);
        GLuint f = compile(GL_FRAGMENT_SHADER, sp.fs);
        if (!v || !f) return false;
        sp.out->id = glCreateProgram();
        glAttachShader(sp.out->id, v);
        glAttachShader(sp.out->id, f);
        glLinkProgram(sp.out->id);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(sp.out->id, GL_LINK_STATUS, &ok);
        if (!ok) {
            SDL_Log("clean_renderer: program link failed");
            return false;
        }
    }
    return true;
}

// --- textures -----------------------------------------------------------------

CelId CleanRenderer::try_texture_replacement(int w, int h,
                                              uint64_t source_hash,
                                              bool mipmap_linear,
                                              bool wrap_repeat)
{
    const TextureReplacements::Image* image =
        m_replacements.find(w, h, source_hash);
    if (!image) return kInvalidCel;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return kInvalidCel;
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint min_filter = mipmap_linear ? GL_LINEAR_MIPMAP_LINEAR
                                           : GL_LINEAR;
    const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width, image->height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image->rgba.data());
    if (mipmap_linear) glGenerateMipmap(GL_TEXTURE_2D);
    const CelId id =
        !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
    if (!m_free_cels.empty()) m_free_cels.pop_back();
    GlTex uploaded;
    uploaded.w = image->width;
    uploaded.h = image->height;
    uploaded.logical_w = w;
    uploaded.logical_h = h;
    uploaded.tex = tex;
    uploaded.has_mips = mipmap_linear;
    uploaded.wrap_repeat = wrap_repeat;
    uploaded.replacement = true;
    uploaded.sampler_min = min_filter;
    uploaded.sampler_mag = GL_LINEAR;
    uploaded.sampler_wrap_s = wrap;
    uploaded.sampler_wrap_t = wrap;
    m_textures.emplace(id, std::move(uploaded));
    return id;
}

CelId CleanRenderer::cel_upload_rgba(int w, int h, const void* pixels)
{
    return cel_upload_rgba_ex(w, h, pixels, false, false);
}

CelId CleanRenderer::cel_upload_rgba_ex(int w, int h, const void* pixels,
                                        bool mipmap_linear, bool wrap_repeat)
{
    return cel_upload_rgba_ex_hashed(w, h, pixels, mipmap_linear, wrap_repeat,
                                     0);
}

CelId CleanRenderer::cel_upload_rgba_ex_hashed(int w, int h, const void* pixels,
                                               bool mipmap_linear,
                                               bool wrap_repeat,
                                               uint64_t content_hash)
{
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
    if (std::optional<GlTex> reused = m_texture_cache.take(
            cel_cache_key(w, h, mipmap_linear, wrap_repeat, content_hash))) {
        glBindTexture(GL_TEXTURE_2D, reused->tex);
        const GLint base_min = mipmap_linear ? GL_LINEAR_MIPMAP_LINEAR
                                             : GL_NEAREST;
        const GLint base_mag = mipmap_linear ? GL_LINEAR : GL_NEAREST;
        const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, base_min);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, base_mag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        reused->sampler_min = base_min;
        reused->sampler_mag = base_mag;
        reused->sampler_wrap_s = wrap;
        reused->sampler_wrap_t = wrap;
        reused->sampler_max_level = -1;
        const CelId id =
            !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
        if (!m_free_cels.empty()) m_free_cels.pop_back();
        m_textures.emplace(id, std::move(*reused));
        ++m_texture_cache_hits;
        return id;
    }
    ++m_texture_cache_misses;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (mipmap_linear) {
        // Bilinear + trilinear mips: kills NEAREST crawl/shimmer ("cel
        // artifacts") on minified road/grass detail.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    if (mipmap_linear) glGenerateMipmap(GL_TEXTURE_2D);
    const CelId id =
        !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
    if (!m_free_cels.empty()) m_free_cels.pop_back();
    GlTex uploaded;
    uploaded.w = w;
    uploaded.h = h;
    uploaded.tex = tex;
    uploaded.has_mips = mipmap_linear;
    uploaded.wrap_repeat = wrap_repeat;
    uploaded.content_hash = content_hash;
    uploaded.sampler_min = mipmap_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST;
    uploaded.sampler_mag = mipmap_linear ? GL_LINEAR : GL_NEAREST;
    uploaded.sampler_wrap_s = wrap;
    uploaded.sampler_wrap_t = wrap;
    m_textures.emplace(id, std::move(uploaded));
    return id;
}

bool CleanRenderer::clt_supports_authored_lod_chain() const
{
    return true;
}

CelId CleanRenderer::cel_upload_rgba_lod_chain(int w, int h, const void* pixels,
                                              const CelLodLevel* levels,
                                              size_t level_count,
                                              bool mipmap_linear,
                                              bool wrap_repeat,
                                              uint64_t content_hash,
                                              uint64_t lod_chain_hash)
{
    if (!mipmap_linear || !levels || level_count == 0 || !pixels || w <= 0 ||
        h <= 0)
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
    // differently shaped level would redefine the GL chain shape instead of
    // the guest's own reduction of it.
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
    // A level list this backend truncates (authored < level_count) is rehashed
    // over the accepted prefix so a differently shaped chain cannot alias a
    // cached texture. The refresh path caches the accepted prefix's identity
    // with its expanded pixels, so a palette alternation does not rescan it.
    uint64_t chain_hash = lod_chain_hash;
    if (chain_hash == 0 || authored != level_count)
        chain_hash = cel_lod_chain_hash(levels, authored);
    if (std::optional<GlTex> reused = m_texture_cache.take(
            cel_cache_key(w, h, true, wrap_repeat, content_hash, chain_hash))) {
        glBindTexture(GL_TEXTURE_2D, reused->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const GLint wrap = wrap_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        reused->sampler_min = GL_LINEAR_MIPMAP_LINEAR;
        reused->sampler_mag = GL_LINEAR;
        reused->sampler_wrap_s = wrap;
        reused->sampler_wrap_t = wrap;
        reused->sampler_max_level = -1;
        const CelId id =
            !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
        if (!m_free_cels.empty()) m_free_cels.pop_back();
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
    // GL needs a complete chain.  Levels the guest did not load have no
    // authored bytes, so extend it below the authored tail with the host's own
    // reduction of the coarsest authored level -- the documented tail source
    // and the software backend's behavior.  Generating the chain from level 0
    // instead would serve a reduction of LOD0 there, which the guest's coarser
    // levels may not resemble.
    std::vector<CelLodTail> tail;
    const CelLodLevel& coarsest = levels[authored - 1];
    cel_generate_lod_tail(coarsest.rgba, coarsest.w, coarsest.h, tail);
    GLint level_index = static_cast<GLint>(authored) + 1;
    for (const CelLodTail& level : tail) {
        glTexImage2D(GL_TEXTURE_2D, level_index++, GL_RGBA8, level.w, level.h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, level.rgba.data());
    }
    const CelId id =
        !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
    if (!m_free_cels.empty()) m_free_cels.pop_back();
    GlTex uploaded;
    uploaded.w = w;
    uploaded.h = h;
    uploaded.tex = tex;
    uploaded.has_mips = true;
    uploaded.wrap_repeat = wrap_repeat;
    uploaded.content_hash = content_hash;
    uploaded.lod_chain_hash = chain_hash;
    uploaded.sampler_min = GL_LINEAR_MIPMAP_LINEAR;
    uploaded.sampler_mag = GL_LINEAR;
    uploaded.sampler_wrap_s = wrap;
    uploaded.sampler_wrap_t = wrap;
    m_textures.emplace(id, std::move(uploaded));
    return id;
}

CelId CleanRenderer::clt_snapshot_target_cel(uint32_t base, int x, int y,
                                               int w, int h, int cel_w,
                                               int cel_h, bool preserve_alpha)
{
    (void)preserve_alpha;
    return snapshot_target(base, x, y, w, h, cel_w, cel_h, false);
}
CelId CleanRenderer::snapshot_target(uint32_t base, int x, int y, int w, int h, int cel_w, int cel_h, bool ssb)
{
    if (cel_w <= 0 || cel_h <= 0) return kInvalidCel;
    // M2_HIRES_EFFECTS: keep the copied window at target resolution. UVs are
    // normalized, so the record's guest extent (w/h below) is unchanged.
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
    if (!m_targets.snapshot_to_texture(base, tex, x, y, w, h, tex_w, tex_h, ssb)) {
        glDeleteTextures(1, &tex);
        return kInvalidCel;
    }
    const CelId id =
        !m_free_cels.empty() ? m_free_cels.back() : ++m_next_cel;
    if (!m_free_cels.empty()) m_free_cels.pop_back();
    GlTex snapshot;
    snapshot.w = cel_w;
    snapshot.h = cel_h;
    snapshot.tex = tex;
    snapshot.gpu_snapshot = true;
    m_textures.emplace(id, std::move(snapshot));
    return id;
}

void CleanRenderer::cel_free(CelId cel)
{
    auto it = m_textures.find(cel);
    if (it == m_textures.end()) return;
    if (it->second.gpu_snapshot || it->second.replacement) {
        glDeleteTextures(1, &it->second.tex);
        m_textures.erase(it);
        m_free_cels.push_back(cel);
        return;
    }
    std::vector<GlTex> released;
    m_texture_cache.insert(it->second.cache_key(), std::move(it->second),
                           released);
    m_textures.erase(it);
    m_free_cels.push_back(cel);
    for (GlTex& record : released)
        if (record.tex) glDeleteTextures(1, &record.tex);
}

// --- 2D path --------------------------------------------------------------

void CleanRenderer::draw_quad_2d(GLuint tex, float dx, float dy, float dw,
                                 float dh, float r, float g, float b, float a,
                                 bool sharp)
{
    const Vert2D verts[4] = {
        {dx, dy,          0.f, 1.f, 0.f, 0.f, r, g, b, a, 0.f, 0.f},
        {dx + dw, dy,     0.f, 1.f, 1.f, 0.f, r, g, b, a, 1.f, 0.f},
        {dx, dy + dh,     0.f, 1.f, 0.f, 1.f, r, g, b, a, 0.f, 1.f},
        {dx + dw, dy + dh, 0.f, 1.f, 1.f, 1.f, r, g, b, a, 1.f, 1.f},
    };
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(m_p2d.id);
    glUniform2f(m_clt_uniforms.res, m_w, m_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_clt_uniforms.tex, 0);
    glUniform1i(m_clt_uniforms.discard_ssb, 0);
    glUniform1i(m_clt_uniforms.discard_alpha, 0);
    glUniform1i(m_clt_uniforms.discard_rgb, 0);
    glUniform1i(m_clt_uniforms.blend_mode, 0);
    glUniform1i(m_clt_uniforms.dither_enable, 0);
    glUniform1i(m_clt_uniforms.tab_control, -1);
    glUniform1i(m_clt_uniforms.tab_alpha_color, 0);
    glUniform3f(m_clt_uniforms.blend_const, 0.f, 0.f, 0.f);
    glUniform1i(m_clt_uniforms.db_source_linear, 0);
    glUniform1i(m_clt_uniforms.feedback_source, 0);
    glUniform1f(m_clt_uniforms.db_source_height, 0.f);
    glUniform2f(m_clt_uniforms.uv_period, 1.f, 1.f);
    glUniform1i(m_clt_uniforms.sharp, sharp ? 1 : 0);
    glBindVertexArray(m_vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void CleanRenderer::cel_blit(CelId cel, float dx, float dy, float dw, float dh,
                             float r, float g, float b, float a)
{
    auto it = m_textures.find(cel);
    if (it == m_textures.end()) return;
    // M2_BITMAP_FILTER applies to the Projector bitmap scanout only; other
    // 2D cels keep the sampler state they were uploaded with.
    bool sharp = false;
    if (cel == bitmap_scanout_cel() && m_bitmap_filter != TextureFilterMode::guest) {
        GlTex& scanout = it->second;
        glBindTexture(GL_TEXTURE_2D, scanout.tex);
        if (scanout.sampler_min != GL_LINEAR) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            scanout.sampler_min = GL_LINEAR;
        }
        if (scanout.sampler_mag != GL_LINEAR) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            scanout.sampler_mag = GL_LINEAR;
        }
        sharp = m_bitmap_filter == TextureFilterMode::sharp;
    }
    draw_quad_2d(it->second.tex, dx, dy, dw, dh, r, g, b, a, sharp);
}

void CleanRenderer::fill_rect(float dx, float dy, float dw, float dh, float r,
                              float g, float b, float a)
{
    draw_quad_2d(m_white_tex, dx, dy, dw, dh, r, g, b, a);
}

void CleanRenderer::scissor(float x, float y, float w, float h)
{
    m_clip_active = true;
    m_clip_rect = {x, y, w < 0.f ? 0.f : w, h < 0.f ? 0.f : h};
    apply_clip();
}

void CleanRenderer::scissor_disable()
{
    m_clip_active = false;
    glDisable(GL_SCISSOR_TEST);
}

// Apply the interpreter's tracked clip rect (top-left origin) to GL.
void CleanRenderer::apply_clip()
{
    if (!m_clip_active) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    apply_scissor_rect(m_clip_rect);
}

void CleanRenderer::apply_scissor_rect(const CltClipRect& rect)
{
    // interface origin is top-left; GL scissor is bottom-left
    glScissor(static_cast<GLint>(rect.x),
              static_cast<GLint>(m_h - rect.y - rect.h),
              static_cast<GLsizei>(rect.w < 0.f ? 0.f : rect.w),
              static_cast<GLsizei>(rect.h < 0.f ? 0.f : rect.h));
    glEnable(GL_SCISSOR_TEST);
}

void CleanRenderer::clt_draw(const CltVertex* vertices, uint32_t count,
                             bool strip, bool textured, CelId texture,
                             float target_w, float target_h,
                             bool blend_enabled, CltDiscard discard,
                             CltDepth depth, CltTextureSampling sampling,
                             CltBlendState blend,
                             bool independent_triangles)
{
    if (!vertices || count < 3u || count > 65536u || target_w <= 0.f ||
        target_h <= 0.f)
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
    // sampler's own wrap mode is exact when the guest's masked period reaches
    // the resident extent (the vendor page builder's tiled case); a shorter
    // period repeats only its masked sub-range and is folded per fragment.
    float uv_period_s = 1.f;
    float uv_period_t = 1.f;
    // Presentation sampling of the primary texture: sharp bilinear.
    bool sharp_sample = false;
    const auto set_sampler = [&](GLenum pname, GLint value,
                                 GLint* cached) {
        if (*cached == value) return;
        glTexParameteri(GL_TEXTURE_2D, pname, value);
        *cached = value;
    };
    if (textured) {
        const auto it = m_textures.find(texture);
        if (it == m_textures.end()) return;
        gl_texture = it->second.tex;
        gpu_feedback_source = it->second.gpu_snapshot;
        glBindTexture(GL_TEXTURE_2D, gl_texture);
        GlTex& bound_texture = it->second;
        const int logical_w = bound_texture.logical_w > 0
                                  ? bound_texture.logical_w : bound_texture.w;
        const int logical_h = bound_texture.logical_h > 0
                                  ? bound_texture.logical_h : bound_texture.h;
        if (sampling.wrap_u && logical_w > 0 &&
            sampling.u_period < static_cast<uint32_t>(logical_w))
            uv_period_s = static_cast<float>(sampling.u_period) /
                          static_cast<float>(logical_w);
        if (sampling.wrap_v && logical_h > 0 &&
            sampling.v_period < static_cast<uint32_t>(logical_h))
            uv_period_t = static_cast<float>(sampling.v_period) /
                          static_cast<float>(logical_h);
        const bool linear_override = (m_linear_filter || m_anisotropy > 1) &&
                                     !bound_texture.gpu_snapshot;
        sharp_sample = m_sharp_filter && !bound_texture.gpu_snapshot;
        const GLint base_filter =
            linear_override || sampling.min_filter != CltTextureFilter::point
                ? GL_LINEAR : GL_NEAREST;
        const GLint min_filter =
            !it->second.has_mips || sampling.lod_max == 0u
                ? base_filter
                : sampling.inter_filter == CltTextureFilter::point
                      ? (base_filter == GL_NEAREST
                             ? GL_NEAREST_MIPMAP_NEAREST
                             : GL_LINEAR_MIPMAP_NEAREST)
                      : (base_filter == GL_NEAREST
                             ? GL_NEAREST_MIPMAP_LINEAR
                             : GL_LINEAR_MIPMAP_LINEAR);
        const GLint mag_filter =
            linear_override || sampling.mag_filter != CltTextureFilter::point
                ? GL_LINEAR : GL_NEAREST;
        const GLint wrap_s = sampling.wrap_u ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        const GLint wrap_t = sampling.wrap_v ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        const GLint max_level = bound_texture.has_mips ? sampling.lod_max : 0;
        set_sampler(GL_TEXTURE_MIN_FILTER, min_filter,
                    &bound_texture.sampler_min);
        set_sampler(GL_TEXTURE_MAG_FILTER, mag_filter,
                    &bound_texture.sampler_mag);
        set_sampler(GL_TEXTURE_WRAP_S, wrap_s, &bound_texture.sampler_wrap_s);
        set_sampler(GL_TEXTURE_WRAP_T, wrap_t, &bound_texture.sampler_wrap_t);
        set_sampler(GL_TEXTURE_MAX_LEVEL, max_level,
                    &bound_texture.sampler_max_level);
        // Anisotropy is a per-texture parameter. GPU feedback sources keep
        // guest sampling (the same exclusion the linear override uses), so the
        // request is only installed on primary decoded textures.
        if (m_anisotropy > 1 && !bound_texture.gpu_snapshot) {
            const GLfloat wanted = static_cast<GLfloat>(m_anisotropy);
            if (bound_texture.sampler_aniso != wanted) {
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                wanted);
                bound_texture.sampler_aniso = wanted;
            }
        }
    }
    if (discard.ssb_zero ||
        (blend.weighted_mix_ssb_mask != kInvalidCel && (blend.dsb.select == CltDsbSelect::object_ssb || blend.tab.enabled ||
         blend.mode == CltBlendMode::general_internal || blend.mode == CltBlendMode::general_secondary)) ||
        (blend.weighted_mix_ssb_selects_entry1 &&
         blend.mode == CltBlendMode::coefficient_weighted_mix)) {
        // The SSB discard test and the per-texel coefficient-entry split
        // both sample the texture's SSB bitmap.  The interpreter carries the
        // uploaded mask cel on the blend state; the discard's own mask cel
        // is the same bitmap and serves as the fallback.
        CelId mask_cel = blend.weighted_mix_ssb_mask;
        if (mask_cel == kInvalidCel) mask_cel = discard.ssb_mask;
        const auto it = m_textures.find(mask_cel);
        if (it == m_textures.end()) return;
        gl_ssb_mask = it->second.tex;
        glBindTexture(GL_TEXTURE_2D, gl_ssb_mask);
        const GLint wrap_s = sampling.wrap_u ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        const GLint wrap_t = sampling.wrap_v ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        set_sampler(GL_TEXTURE_MIN_FILTER, GL_NEAREST, &it->second.sampler_min);
        set_sampler(GL_TEXTURE_MAG_FILTER, GL_NEAREST, &it->second.sampler_mag);
        set_sampler(GL_TEXTURE_WRAP_S, wrap_s, &it->second.sampler_wrap_s);
        set_sampler(GL_TEXTURE_WRAP_T, wrap_t, &it->second.sampler_wrap_t);
        set_sampler(GL_TEXTURE_MAX_LEVEL, 0, &it->second.sampler_max_level);
    }
    if (clt_blend_uses_secondary_source(blend_mode)) {
        const auto it = m_textures.find(blend.secondary_source);
        if (it == m_textures.end()) return;
        gl_secondary_source = it->second.tex;
        glBindTexture(GL_TEXTURE_2D, gl_secondary_source);
        set_sampler(GL_TEXTURE_MIN_FILTER, GL_NEAREST, &it->second.sampler_min);
        set_sampler(GL_TEXTURE_MAG_FILTER, GL_NEAREST, &it->second.sampler_mag);
        set_sampler(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE, &it->second.sampler_wrap_s);
        set_sampler(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE, &it->second.sampler_wrap_t);
        set_sampler(GL_TEXTURE_MAX_LEVEL, 0, &it->second.sampler_max_level);
    }

    m_clt_vertices.resize(count);
    std::vector<Vert2D>& verts = m_clt_vertices;
    const float sx = m_w / target_w;
    const float sy = m_h / target_h;
    for (uint32_t i = 0; i < count; ++i) {
        const CltVertex& src = vertices[i];
        if (!std::isfinite(src.x) || !std::isfinite(src.y) ||
            !std::isfinite(src.u) || !std::isfinite(src.v) ||
            !std::isfinite(src.mask_u) || !std::isfinite(src.mask_v) ||
            !std::isfinite(src.r) || !std::isfinite(src.g) ||
            !std::isfinite(src.b) || !std::isfinite(src.a))
            return;
        Vert2D& dst = verts[i];
        dst.x = src.x * sx;
        dst.y = src.y * sy;
        // Keep the same monotonic reciprocal-W depth mapping as the guest CLT
        // backend. DBZCNTL's smaller-Z relation is translated by the shared
        // comparison vocabulary below; the old inverse mapping plus LEQUAL
        // collapsed all non-ALL masks and made CleanGL disagree with GL.
        dst.z = -1.f + 2.f * std::clamp(src.one_over_w, 0.f, 1.f);
        dst.w = (src.w > 0.f && std::isfinite(src.w)) ? src.w : 1.f;
        dst.u = src.u;
        dst.v = src.v;
        dst.r = src.r;
        dst.g = src.g;
        dst.b = src.b;
        dst.a = src.a;
        dst.mask_u = clt_blend_uses_secondary_source(blend_mode)
                         ? src.mask_u
                         : (discard.separate_ssb_uv ? src.mask_u : src.u);
        dst.mask_v = clt_blend_uses_secondary_source(blend_mode)
                         ? src.mask_v
                         : (discard.separate_ssb_uv ? src.mask_v : src.v);
    }

    // TE Z region: outside-region pixels bypass the Z test/write, which GL
    // exposes only per draw call. Inside-region fragments keep the guest's
    // DBZCNTL relation; outside fragments draw color-only, and
    // DBDISCARDCONTROL.ZCLIPPED drops them entirely. The backend's scissor
    // contract splits the two pixel sets exactly at 1:1 scale.
    const CltClipRect clip_rect =
        m_clip_active ? m_clip_rect : CltClipRect{0.f, 0.f, m_w, m_h};
    CltClipRect inside = clip_rect;
    CltClipRect outside[4]{};
    int outside_count = 0;
    if (depth.test_enabled) {
        const CltClipRect region{
            static_cast<float>(depth.z_region_min_x) * sx,
            static_cast<float>(depth.z_region_min_y) * sy,
            static_cast<float>(depth.z_region_max_x - depth.z_region_min_x) *
                sx,
            static_cast<float>(depth.z_region_max_y - depth.z_region_min_y) *
                sy};
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
        // Pixel output and Z update are independent DBZCNTL decisions.  The
        // color pass must never update the shared Z surface; the separate
        // update pass below applies the Z relation mask.
        if (glDepthMask) glDepthMask(GL_FALSE);
    };
    const auto set_depth_update_state = [&]() {
        // GL requires the depth test to be enabled for a depth-buffer write;
        // this state is entered only for ZBUFEN-enabled draws.
        glEnable(GL_DEPTH_TEST);
        if (glDepthFunc) {
            glDepthFunc(gl_depth_func_for_dbz_z_update(depth.depth_func));
        }
        if (glDepthMask) glDepthMask(GL_TRUE);
    };
    const auto draw_once = [&](GLint first, GLsizei prim_count) {
        const GLenum mode = independent_triangles
                                ? GL_TRIANGLES
                                : strip ? GL_TRIANGLE_STRIP : GL_TRIANGLE_FAN;
        glDrawArrays(mode, first, prim_count);
    };

    glUseProgram(m_p2d.id);
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

    glUniform1i(m_clt_uniforms.discard_ssb, discard.ssb_zero ? 1 : 0);
    glUniform1i(m_clt_uniforms.separate_ssb_uv,
                discard.separate_ssb_uv ? 1 : 0);
    glUniform1i(m_clt_uniforms.discard_alpha,
                discard.alpha_zero ? 1 : 0);
    glUniform1i(m_clt_uniforms.discard_rgb, discard.rgb_zero ? 1 : 0);
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


    glUniform3f(m_clt_uniforms.blend_const, blend.constant_r, blend.constant_g,
                blend.constant_b);
    glUniform3f(m_clt_uniforms.blend_weight, blend.weight_tex_r,
                blend.weight_tex_g, blend.weight_tex_b);
    glUniform1i(m_clt_uniforms.db_source_linear,
                blend.db_source_linear ? 1 : 0);
    glUniform1i(m_clt_uniforms.feedback_source,
                gpu_feedback_source ? 1 : 0);
    glUniform1f(m_clt_uniforms.db_source_height, blend.db_source_height);
    // Per-texel SSB coefficient split: 0 = single resolved pair, 1 = keep
    // SSB-0 texels, 2 = keep SSB-1 texels (the second pass sets it).
    const bool ssb_entry_split =
        blend_mode == CltBlendMode::coefficient_weighted_mix &&
        blend.weighted_mix_ssb_selects_entry1 &&
        gl_ssb_mask != m_white_tex;
    glUniform1i(m_clt_uniforms.ssb_keep, 0);
    glUniform2f(m_clt_uniforms.uv_period, uv_period_s, uv_period_t);
    glUniform1i(m_clt_uniforms.sharp, sharp_sample ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vert2D) * verts.size(), verts.data(),
                 GL_STREAM_DRAW);
    // Fixed-function factors for the decoded equation come from the shared
    // plan so both GL backends cannot disagree; the shader-side half of the
    // equation (constant_alpha_over/specular_add/weighted-mix rewrite) rides
    // u_blend_mode above.
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
    // texels whose own SSB bit selects that entry, so the two-equation split
    // the per-texel source-select-bit produces is evaluated exactly (a single
    // resolved pair cannot express it).
    const auto draw_passes = [&](GLint first, GLsizei prim_count,
                                 int ssb_keep, float src_w_r, float src_w_g,
                                 float src_w_b) {
        if (ssb_entry_split) {
            glUniform1i(m_clt_uniforms.ssb_keep, ssb_keep);
            if (ssb_keep == 2) {
                glUniform3f(m_clt_uniforms.blend_weight,
                            blend.entry1_weight_tex_r,
                            blend.entry1_weight_tex_g,
                            blend.entry1_weight_tex_b);
                glBlendColor(src_w_r, src_w_g, src_w_b, 1.f);
            } else if (plan.uses_constant_color) {
                // The preceding SSB-1 pass temporarily installs its source
                // weight as the fixed-function blend constant. Restore entry
                // 0 before a color or depth pass evaluates its next fragment
                // set.
                glBlendColor(blend.weight_src_r, blend.weight_src_g,
                             blend.weight_src_b, 1.f);
            }
        }
        draw_once(first, prim_count);
        if (ssb_entry_split) {
            glUniform1i(m_clt_uniforms.ssb_keep, 0);
            glUniform3f(m_clt_uniforms.blend_weight, blend.weight_tex_r,
                        blend.weight_tex_g, blend.weight_tex_b);
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
        // Color writes are disabled while the independent Z-update relation
        // is evaluated. The shader still performs the same source/discard
        // work, so only surviving guest fragments reach the depth test just
        // as on the color pass.
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
}

// --- 3D path --------------------------------------------------------------

MeshId CleanRenderer::mesh_upload(int vc, const float* pos, const float* col,
                                  int ic, const uint16_t* idx)
{
    // Compatibility entry point: callers provide positions and colors only.
    // Compute smooth normals for lighting; zero uvs (untextured draws ignore
    // them).
    MeshData md;
    md.pos.assign(pos, pos + size_t(vc) * 3);
    md.col.assign(col, col + size_t(vc) * 4);
    md.uv.assign(size_t(vc) * 2, 0.f);
    md.idx.assign(idx, idx + ic);
    md.compute_smooth_normals();
    return mesh_upload_lit(vc, md.pos.data(), md.nrm.data(), md.uv.data(),
                           md.col.data(), ic, idx);
}

MeshId CleanRenderer::mesh_upload_lit(int vc, const float* pos,
                                      const float* nrm, const float* uv,
                                      const float* col, int ic,
                                      const uint16_t* idx)
{
    if (!pos || !idx || vc <= 0 || ic <= 0) return kInvalidMesh;
    std::vector<float> interleaved(size_t(vc) * 12);
    static const float kWhite[4] = {1, 1, 1, 1};
    static const float kZero3[3] = {0, 1, 0};
    for (int i = 0; i < vc; ++i) {
        float* d = &interleaved[size_t(i) * 12];
        std::memcpy(d + 0, pos + size_t(i) * 3, sizeof(float) * 3);
        std::memcpy(d + 3, nrm ? nrm + size_t(i) * 3 : kZero3,
                    sizeof(float) * 3);
        if (uv) std::memcpy(d + 6, uv + size_t(i) * 2, sizeof(float) * 2);
        else { d[6] = 0; d[7] = 0; }
        std::memcpy(d + 8, col ? col + size_t(i) * 4 : kWhite,
                    sizeof(float) * 4);
    }

    GLuint vao = 0, vbo = 0, ibo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * interleaved.size(),
                 interleaved.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16_t) * size_t(ic), idx,
                 GL_STATIC_DRAW);
    constexpr GLsizei kStride = sizeof(float) * 12;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<void*>(sizeof(float) * 3));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<void*>(sizeof(float) * 6));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<void*>(sizeof(float) * 8));
    glBindVertexArray(0);

    const MeshId id = ++m_next_mesh;
    m_meshes[id] = {vao, vbo, ibo, ic};
    return id;
}

void CleanRenderer::mesh_free(MeshId mesh)
{
    auto it = m_meshes.find(mesh);
    if (it == m_meshes.end()) return;
    if (it->second.ibo) glDeleteBuffers(1, &it->second.ibo);
    if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
    if (it->second.vao) glDeleteVertexArrays(1, &it->second.vao);
    m_meshes.erase(it);
}

void CleanRenderer::mesh_draw_state(MeshId mesh, const MeshDrawState& st)
{
    Mat4 mvp;
    mat_mul(st.view_proj, st.model, mvp);
    draw_mesh(mesh, mvp, st.model, st);
}

void CleanRenderer::mesh_draw(MeshId mesh, const float view_proj[16],
                              const float cam_pos[3], float fog_r, float fog_g,
                              float fog_b, float fog_far)
{
    // Compatibility wrapper with neutral lighting and caller-provided fog.
    MeshDrawState st{};
    mat_identity(st.model);
    std::memcpy(st.view_proj, view_proj, sizeof(Mat4));
    std::memcpy(st.cam_pos, cam_pos, sizeof(st.cam_pos));
    st.sun_dir[0] = 0.3607f; st.sun_dir[1] = 0.8964f; st.sun_dir[2] = 0.2576f;
    st.sun_rgb[0] = 0.98f; st.sun_rgb[1] = 0.95f; st.sun_rgb[2] = 0.88f;
    st.ambient_rgb[0] = 0.42f; st.ambient_rgb[1] = 0.45f;
    st.ambient_rgb[2] = 0.52f;
    st.fog_rgb[0] = fog_r; st.fog_rgb[1] = fog_g; st.fog_rgb[2] = fog_b;
    st.fog_far = fog_far;
    st.texture = kInvalidCel;
    st.alpha = 1.f;
    mesh_draw_state(mesh, st);
}

void CleanRenderer::draw_mesh(MeshId mesh, const float mvp[16],
                              const float model[16], const MeshDrawState& st)
{
    auto it = m_meshes.find(mesh);
    if (it == m_meshes.end()) return;
    const GlMesh& gm = it->second;

    glUseProgram(m_p3d.id);
    glUniformMatrix4fv(u(m_p3d, "u_mvp"), 1, GL_FALSE, mvp);
    glUniformMatrix4fv(u(m_p3d, "u_model"), 1, GL_FALSE, model);
    // Normal matrix: model rotation/scale part (models here carry no shear).
    const GLfloat nmat[9] = {
        model[0], model[1], model[2],
        model[4], model[5], model[6],
        model[8], model[9], model[10],
    };
    glUniformMatrix3fv(u(m_p3d, "u_nmat"), 1, GL_FALSE, nmat);
    glUniform3f(u(m_p3d, "u_cam"), st.cam_pos[0], st.cam_pos[1],
                st.cam_pos[2]);
    glUniform3f(u(m_p3d, "u_sun"), st.sun_dir[0], st.sun_dir[1],
                st.sun_dir[2]);
    glUniform3f(u(m_p3d, "u_sunrgb"), st.sun_rgb[0], st.sun_rgb[1],
                st.sun_rgb[2]);
    glUniform3f(u(m_p3d, "u_amb"), st.ambient_rgb[0], st.ambient_rgb[1],
                st.ambient_rgb[2]);
    glUniform3f(u(m_p3d, "u_fogrgb"), st.fog_rgb[0], st.fog_rgb[1],
                st.fog_rgb[2]);
    glUniform1f(u(m_p3d, "u_fogfar"), st.fog_far);
    glUniform1f(u(m_p3d, "u_alpha"), st.alpha);

    GLuint tex = 0;
    const bool has_tex = st.texture != kInvalidCel &&
                         m_textures.count(st.texture) > 0;
    if (has_tex) tex = m_textures[st.texture].tex;
    glUniform1i(u(m_p3d, "u_has_tex"), has_tex ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, has_tex ? tex : m_white_tex);
    glUniform1i(u(m_p3d, "u_tex"), 0);

    glBindVertexArray(gm.vao);
    glDrawElements(GL_TRIANGLES, gm.index_count, GL_UNSIGNED_SHORT, nullptr);
}

void CleanRenderer::mesh_draw_shadow(MeshId mesh, const float model[16],
                                     const float view_proj[16],
                                     const float sun_dir[3], float ground_y,
                                     float strength)
{
    auto it = m_meshes.find(mesh);
    if (it == m_meshes.end()) return;
    if (sun_dir[1] > -0.08f) return;  // sun too low: degenerate projection

    // Project along the sun ray onto plane y = ground_y:
    //   p' = p + sun * ((ground_y - p.y) / sun.y)   (matrix form below)
    const float lx = sun_dir[0], ly = sun_dir[1], lz = sun_dir[2];
    Mat4 S;
    S[0] = 1.f;      S[4] = -lx / ly; S[8] = 0.f; S[12] = ground_y * lx / ly;
    S[1] = 0.f;      S[5] = 0.f;      S[9] = 0.f; S[13] = ground_y;
    S[2] = 0.f;      S[6] = -lz / ly; S[10] = 0.f; S[14] = ground_y * lz / ly;
    S[3] = 0.f;      S[7] = 0.f;      S[11] = 0.f; S[15] = 1.f;
    Mat4 sm, mvp;
    mat_mul(S, model, sm);
    mat_mul(view_proj, sm, mvp);

    const GlMesh& gm = it->second;
    glUseProgram(m_psh.id);
    glUniformMatrix4fv(u(m_psh, "u_mvp"), 1, GL_FALSE, mvp);
    glUniform1f(u(m_psh, "u_alpha"), strength);

    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);  // lift off the road surface
    glPolygonOffset(-1.f, -1.f);
    if (m_stencil) {
        // Draw each shadowed texel once even where triangles overlap.
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    }
    glBindVertexArray(gm.vao);
    glDrawElements(GL_TRIANGLES, gm.index_count, GL_UNSIGNED_SHORT, nullptr);
    if (m_stencil) glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
}

// A new depth attachment starts at the far value; the guest's own clear-Z
// request then installs the fill word its TE surface state authored.
void CleanRenderer::clt_clear_z(uint32_t z_fill_word)
{
    if (!glClear) return;
    if (glDepthMask) glDepthMask(GL_TRUE);
    if (glClearDepth) glClearDepth(clt_te_z_fill_depth(z_fill_word));
    glClear(GL_DEPTH_BUFFER_BIT);
}

void CleanRenderer::clt_clear_color(uint32_t red_word, uint32_t green_word,
                                    uint32_t blue_word, uint32_t alpha_word)
{
    // The recovered TE driver answers TE_CLEAR_FRAME_BUFFER with its generated
    // clear list: one untextured quad over the bound surface's own extent,
    // clipped by DBFBCLIP and the active window clip like any guest primitive
    // (driver.c:251-278). Re-assert the tracked clip the interpreter maintains
    // for guest color writes, so the platform clear fills exactly the same
    // region a guest draw would reach instead of the whole target.
    apply_clip();
    m_targets.clear_color(clt_te_color_component(red_word),
                          clt_te_color_component(green_word),
                          clt_te_color_component(blue_word),
                          clt_te_color_component(alpha_word));
}

// --- frame ----------------------------------------------------------------

void CleanRenderer::frame_begin(float cr, float cg, float cb)
{
    // A fullscreen drawable is fitted to 4:3 (black bars); windowed it is the
    // whole drawable.
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
    glViewport(0, 0, static_cast<GLsizei>(m_w), static_cast<GLsizei>(m_h));
    glClearColor(cr, cg, cb, 1.f);
    const CltFrameClearPlan clear_plan =
        m_selftest ? CltFrameClearPlan{}
                   : clt_frame_clear_plan(persist_target_enabled(),
                                          m_persist_initialized);
    GLbitfield clear_bits = 0;
    if (clear_plan.color) clear_bits |= GL_COLOR_BUFFER_BIT;
    if (clear_plan.depth)
        clear_bits |= GL_DEPTH_BUFFER_BIT |
                      (m_stencil ? GL_STENCIL_BUFFER_BIT : 0);
    if (clear_bits != 0) {
        glClearDepth(kCltTeZFarDepth);
        glClear(clear_bits);
        if (clear_plan.color) m_targets.clear_metadata();
        m_persist_initialized = true;
    }
    if (m_selftest) render_selftest_scene(m_selftest_frame++);
}

bool CleanRenderer::grab_presented_rgba(uint32_t* dst, int w, int h)
{
    if (copy_bitmap_scanout(dst, w, h)) return true;
    if (m_targets.scanout_base() != 0)
        return m_targets.read_presented(m_targets.scanout_base(), dst, w, h);
    if (!dst || !glReadPixels || w <= 0 || h <= 0) return false;
    if (w != static_cast<int>(m_w) || h != static_cast<int>(m_h)) return false;
    // Read the current draw target before presentation. GL rows are bottom-up;
    // the caller expects top-left origin, so flip in place.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // Renderer work can leave a diagnostic GL error pending. Clear it before
    // the read so only an error raised by this capture decides its result.
    while (glGetError() != GL_NO_ERROR) {}
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    if (glGetError() != GL_NO_ERROR) return false;
    for (int y = 0; y < h / 2; ++y) {
        uint32_t* top = &dst[static_cast<size_t>(y) * w];
        uint32_t* bottom = &dst[static_cast<size_t>(h - 1 - y) * w];
        for (int x = 0; x < w; ++x) std::swap(top[x], bottom[x]);
    }
    return true;
}

void CleanRenderer::frame_present()
{
    if (m_selftest) {
        const int w = static_cast<int>(m_w), h = static_cast<int>(m_h);
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        for (int y = 0; y < h; ++y)  // GL rows are bottom-up
            std::memcpy(&rgb[size_t(y) * w * 3],
                        &rgba[size_t(h - 1 - y) * w * 4], size_t(w) * 3);
        // Scratch PPMs stay inside the repo (gitignored build/ tree) so
        // Scratch directory: M2_CLEAN_SELFTEST_DIR overrides.
        namespace fs = std::filesystem;
        static fs::path st_dir = [] {
            if (const char* d = std::getenv("M2_CLEAN_SELFTEST_DIR"))
                return fs::path(d);
            return fs::path("build/scratch/clean_selftest");
        }();
        std::error_code mk_ec;
        fs::create_directories(st_dir, mk_ec);
        char path[256];
        std::snprintf(path, sizeof(path), "%s/m2clean_selftest_%04d.ppm",
                      st_dir.string().c_str(), m_selftest_frame - 1);
        FILE* f = std::fopen(path, "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
            std::printf("[gfx] selftest frame -> %s\n", path);
        }
    }
    m_targets.present(!has_bitmap_scanout());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    // The scanout quad draws on the default framebuffer: confine it to the
    // letterboxed image region, then restore the target viewport.
    glViewport(m_view_x, m_view_y, static_cast<GLsizei>(m_present_w),
               static_cast<GLsizei>(m_present_h));
    draw_bitmap_scanout();
    glViewport(0, 0, static_cast<GLsizei>(m_w), static_cast<GLsizei>(m_h));
    SDL_GL_SwapWindow(m_window);
}

// --- selftest scene ---------------------------------------------------------

namespace {
constexpr float kTrackR = 60.f;       // centerline radius (world units ≈ m)
constexpr float kTrackHalfW = 7.f;
constexpr float kTrackY = 0.06f;
constexpr float kShadowPlaneY = 0.10f;
constexpr float kSpeed = 22.f;        // world units / s
}  // namespace

void CleanRenderer::render_selftest_scene(int frame)
{
    const bool fresh = m_demo_track == kInvalidMesh;
    if (fresh) {
        MeshData md;
        md.quad_xz(0.f, 0.f, 0.f, 800.f, 800.f, 200.f, 200.f, 1, 1, 1);
        m_demo_ground = mesh_upload_lit(md.vertex_count(), md.pos.data(),
                                        md.nrm.data(), md.uv.data(),
                                        md.col.data(), int(md.idx.size()),
                                        md.idx.data());

        md = MeshData{};
        md.ring(kTrackR, kTrackHalfW, kTrackY, 160, 48.f);
        m_demo_track = mesh_upload_lit(md.vertex_count(), md.pos.data(),
                                       md.nrm.data(), md.uv.data(),
                                       md.col.data(), int(md.idx.size()),
                                       md.idx.data());

        md = MeshData{};
        md.quad_xz(kTrackR, kTrackY + 0.015f, 0.f, kTrackHalfW * 2.f, 2.f, 1.f,
                   1.f, 0.95f, 0.95f, 0.95f);
        m_demo_start = mesh_upload_lit(md.vertex_count(), md.pos.data(),
                                       md.nrm.data(), md.uv.data(),
                                       md.col.data(), int(md.idx.size()),
                                       md.idx.data());

        md = MeshData{};  // car body (+X forward), painted + stripe texture
        md.box(0.f, 0.55f, 0.f, 4.4f, 0.72f, 1.9f, 1, 1, 1);
        m_demo_body = mesh_upload_lit(md.vertex_count(), md.pos.data(),
                                      md.nrm.data(), md.uv.data(),
                                      md.col.data(), int(md.idx.size()),
                                      md.idx.data());

        md = MeshData{};  // untextured details
        md.box(-0.35f, 1.05f, 0.f, 1.9f, 0.55f, 1.7f, 0.16f, 0.18f, 0.24f);
        md.box(-2.05f, 1.00f, 0.f, 0.28f, 0.10f, 1.75f, 0.75f, 0.10f, 0.12f);
        for (int s : {-1, 1})
            for (int t : {-1, 1})
                md.box(1.45f * float(t), 0.33f, 0.88f * float(s), 0.66f,
                       0.66f, 0.36f, 0.07f, 0.07f, 0.08f);
        m_demo_details = mesh_upload_lit(md.vertex_count(), md.pos.data(),
                                         md.nrm.data(), md.uv.data(),
                                         md.col.data(), int(md.idx.size()),
                                         md.idx.data());

        const auto grass = make_grass(64);
        const auto road = make_asphalt(64);
        const auto paint = make_car_paint(32);
        m_tex_grass = cel_upload_rgba_ex(64, 64, grass.data(), true, true);
        m_tex_road = cel_upload_rgba_ex(64, 64, road.data(), true, true);
        m_tex_car = cel_upload_rgba_ex(32, 32, paint.data(), true, false);
    }
    if (m_demo_track == kInvalidMesh) return;

    const float t = static_cast<float>(frame) / 60.f;
    const float theta = t * kSpeed / kTrackR;
    const float wob = 1.6f * std::sin(t * 0.9f);  // gentle lane wander

    const float cx = kTrackR * std::cos(theta) + std::sin(theta) * wob;
    const float cz = kTrackR * std::sin(theta) - std::cos(theta) * wob;
    const float heading = -theta - 1.5707963f;  // +X axis onto velocity dir

    // chase camera
    const float fx = std::sin(theta), fz = -std::cos(theta);
    const float ex = cx - fx * 13.f, ez = cz - fz * 13.f, ey = 5.4f;
    Mat4 proj, view, vp;
    mat_perspective(58.f * 3.14159265f / 180.f, m_w / (m_h > 0 ? m_h : 1.f),
                    0.5f, 3000.f, proj);
    mat_look_at(ex, ey, ez, cx + fx * 8.f, 1.1f, cz + fz * 8.f, 0.f, 1.f, 0.f,
                view);
    mat_mul(proj, view, vp);

    MeshDrawState st{};
    std::memcpy(st.view_proj, vp, sizeof(Mat4));
    st.cam_pos[0] = ex; st.cam_pos[1] = ey; st.cam_pos[2] = ez;
    st.sun_dir[0] = 0.42f; st.sun_dir[1] = -1.f; st.sun_dir[2] = 0.30f;
    { const float l = std::sqrt(0.42f * 0.42f + 1.f + 0.09f);
      for (float& v : st.sun_dir) v = -v / l; }  // flip to toward-sun
    st.sun_rgb[0] = 0.92f; st.sun_rgb[1] = 0.89f; st.sun_rgb[2] = 0.80f;
    st.ambient_rgb[0] = 0.40f; st.ambient_rgb[1] = 0.44f;
    st.ambient_rgb[2] = 0.52f;
    st.fog_rgb[0] = 0.45f; st.fog_rgb[1] = 0.65f; st.fog_rgb[2] = 0.85f;
    st.fog_far = 1400.f;
    st.alpha = 1.f;

    auto place = [&](float x, float y, float z, float rot_y, float* m16) {
        Mat4 ry, tr;
        mat_rot_y(rot_y, ry);
        mat_translate(x, y, z, tr);
        mat_mul(tr, ry, m16);
    };

    mat_identity(st.model);
    st.texture = m_tex_grass;
    mesh_draw_state(m_demo_ground, st);
    st.texture = m_tex_road;
    mesh_draw_state(m_demo_track, st);
    st.texture = kInvalidCel;
    mesh_draw_state(m_demo_start, st);

    Mat4 moving_m, parked_m;
    place(cx, kTrackY + 0.01f, cz, heading, moving_m);
    place(kTrackR - 3.2f, kTrackY + 0.01f, 14.f, 1.2f, parked_m);
    for (const Mat4* cm : {&moving_m, &parked_m}) {
        std::memcpy(st.model, cm, sizeof(Mat4));
        st.texture = m_tex_car;
        mesh_draw_state(m_demo_body, st);
        st.texture = kInvalidCel;
        mesh_draw_state(m_demo_details, st);
    }

    const float sun_sceneward[3] = {-st.sun_dir[0], -st.sun_dir[1],
                                    -st.sun_dir[2]};
    for (const Mat4* cm : {&moving_m, &parked_m}) {
        mesh_draw_shadow(m_demo_body, *cm, vp, sun_sceneward, kShadowPlaneY,
                         0.40f);
        mesh_draw_shadow(m_demo_details, *cm, vp, sun_sceneward,
                         kShadowPlaneY, 0.40f);
    }
}

}  // namespace

std::unique_ptr<ICelBlitterBackend> make_clean_renderer()
{
#if defined(M2_HAVE_GL)
    return std::make_unique<CleanRenderer>();
#else
    return nullptr;
#endif
}

}  // namespace m2host
