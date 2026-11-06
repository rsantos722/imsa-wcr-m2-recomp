// soft_capture.h — deterministic software framebuffer for headless capture.
//
// The GL backend needs a window/context; this backend implements a small
// slice of ICelBlitterBackend (clear + textured quads + fills) in pure CPU
// code, then dumps each presented frame as a binary PPM (P6) into --capture.
//
// Header-only by design so a capture request does not add another build
// target or renderer dependency.
//
// Raster traversal and channel conversion use a fixed order; the small amount
// of floating-point filtering follows the same deterministic call path on one
// host so frame bytes remain reproducible for diagnostics.
#pragma once

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cel_blitter.h"
#include "platform.h"

namespace m2host {

class SoftCaptureBackend : public ICelBlitterBackend {
    struct DbOwner {
        uint32_t draw_sequence = 0;
        CelId texture = kInvalidCel;
        uint32_t depth_func = 0;
        float z = 1.f;
        float one_over_w = 0.f;
        bool textured = false;
        // Screen-space signed-area sign of the triangle that last wrote this
        // pixel, and that triangle's screen bbox, so a depth-test loser can be
        // attributed to the geometry that beat it (M2_LIVERY_DRAW_DIAG).
        // Mercury cull-tests vertices in its triangle assembly stage, and a
        // triangle strip bakes the odd-triangle winding flip into the emitted
        // order, so the raw sign alternates within one strip and does not by
        // itself identify a back face.
        bool neg_area = false;
        uint32_t tex_w = 0;
        uint32_t tex_h = 0;
        float bx0 = 0.f, by0 = 0.f, bx1 = 0.f, by1 = 0.f;
    };

    enum class DepthProbeMode {
        reverse_float,
        direct_float,
        reverse_w16,
        direct_w16,
        packed_w16,
        guest_float,
        mame_float,
        mame_w16,
    };

    // M_DrawDynLit projects the homogeneous vertex W and then emits its
    // reciprocal as the CLT perspective value.  Keep the recovered guest W
    // ordering separate from conventional host depth probes below.
    static uint32_t m2_tefix(float value, int bits)
    {
        const float format = std::ldexp(1.f, bits);
        const float twice_format = format * 2.f;
        const float wrapped =
            std::fmod(std::fabs(value), twice_format);
        const float encoded = wrapped >= format ? wrapped
                                                 : wrapped + format;
        uint32_t result = std::bit_cast<uint32_t>(encoded) & 0x007fffffu;
        if (wrapped >= format || encoded == twice_format)
            result |= 0x00800000u;
        if (value < 0.f) result = 0u - result;
        return result;
    }

    static unsigned m2_wrange(const CltVertex& a, const CltVertex& b,
                              const CltVertex& c)
    {
        const auto exponent = [](float value) {
            return (std::bit_cast<uint32_t>(value) >> 23u) & 0xffu;
        };
        const unsigned ae = exponent(a.one_over_w);
        const unsigned be = exponent(b.one_over_w);
        const unsigned ce = exponent(c.one_over_w);
        if (ae >= 0x7cu || be >= 0x7cu || ce >= 0x7cu) return 0u;
        if (ae >= 0x79u || be >= 0x79u || ce >= 0x79u) return 1u;
        if (ae >= 0x76u || be >= 0x76u || ce >= 0x76u) return 2u;
        return 3u;
    }

    static uint16_t m2_w16(float value, unsigned range)
    {
        constexpr int depth_bits[] = {0, -3, -6, -9};
        const uint32_t fixed = m2_tefix(value, depth_bits[range]);
        return static_cast<uint16_t>((fixed >> (range * 3u + 7u)) &
                                     0xffffu);
    }

    // Diagnostic-only candidate from the 3DO-owned TE patent: two range bits
    // in 15:14 and a 14-bit significand in 13:0. The destination comparison
    // must use the decoded block-fix value, not the raw word's unsigned order;
    // the four ranges deliberately reverse that order. This is intentionally
    // kept separate from m2_w16(), which mirrors the active MAME 0.16 write
    // path.
    static uint16_t m2_w16_packed(float value, unsigned range)
    {
        if (!std::isfinite(value) || value <= 0.f) return 0u;
        const float scaled =
            std::ldexp(value, static_cast<int>(range * 3u + 14u));
        const float bounded = std::clamp(scaled, 0.f, 16383.f);
        const uint32_t significand = static_cast<uint32_t>(std::floor(bounded));
        return static_cast<uint16_t>((range << 14u) | significand);
    }

    static float m2_w16_packed_decode(uint16_t cell)
    {
        const unsigned range = (cell >> 14u) & 3u;
        const unsigned significand = cell & 0x3fffu;
        if (significand == 0u) return 0.f;
        const float normalized = static_cast<float>(significand) / 16384.f;
        return std::ldexp(normalized, -static_cast<int>(range * 3u));
    }

    // Integer-exact PACKED-diagnostic ordering (diagnostic-only): compare the
    // decoded block-fix values s*2^(9-3r) exactly in 32 bits (common
    // denominator 2^23, max 16383*512=8388096), never the raw u16 word order
    // (ranges deliberately reverse it) and never a float epsilon. Mirrors
    // clt_w16_packed_probe::compare; patent WO1996036011A1 block-fix.
    static int m2_w16_packed_compare(uint16_t a, uint16_t b)
    {
        const unsigned range_a = (static_cast<unsigned>(a) >> 14u) & 3u;
        const unsigned range_b = (static_cast<unsigned>(b) >> 14u) & 3u;
        const uint32_t sig_a = static_cast<unsigned>(a) & 0x3fffu;
        const uint32_t sig_b = static_cast<unsigned>(b) & 0x3fffu;
        const uint32_t value_a = sig_a * (1u << (9u - 3u * range_a));
        const uint32_t value_b = sig_b * (1u << (9u - 3u * range_b));
        if (value_a < value_b) return -1;
        if (value_a > value_b) return 1;
        return 0;
    }

    // PACKED-diagnostic depth gate (diagnostic-only): same DBZCNTL polarity as
    // guest_w_allows (larger incoming 1/w is the closer/smaller-Z case) but
    // driven by m2_w16_packed_compare, never by a float epsilon. Only the
    // packed_w16 branch calls this; guest_float/reverse_float/mame paths keep
    // their existing gates untouched.
    static bool m2_w16_packed_allows(uint32_t depth_func, uint16_t incoming,
                                     uint16_t stored, bool test_enabled,
                                     bool z_update)
    {
        if (!test_enabled || depth_func == kCltDbzAll) return true;
        const int cmp = m2_w16_packed_compare(incoming, stored);
        const uint32_t mask = cmp > 0 ? (z_update ? kCltDbzZLt : kCltDbzPixelLt)
                                      : cmp < 0 ? (z_update ? kCltDbzZGt
                                                            : kCltDbzPixelGt)
                                                : (z_update ? kCltDbzZEq
                                                            : kCltDbzPixelEq);
        return (depth_func & mask) != 0;
    }

    static DepthProbeMode depth_probe_mode()
    {
        static const DepthProbeMode mode = [] {
            const char* packed_w16 = std::getenv("M2_CLT_DEPTH_W16_PACKED");
            if (packed_w16 && *packed_w16 != '\0' && *packed_w16 != '0')
                return DepthProbeMode::packed_w16;
            const char* mame_w16 = std::getenv("M2_CLT_DEPTH_W16_MAME");
            if (mame_w16 && *mame_w16 != '\0' && *mame_w16 != '0')
                return DepthProbeMode::mame_w16;
            const char* mame_float = std::getenv("M2_CLT_DEPTH_MAME");
            if (mame_float && *mame_float != '\0' && *mame_float != '0')
                return DepthProbeMode::mame_float;
            const char* w16_raw = std::getenv("M2_CLT_DEPTH_W16_RAW");
            if (w16_raw && *w16_raw != '\0' && *w16_raw != '0')
                return DepthProbeMode::direct_w16;
            const char* w16 = std::getenv("M2_CLT_DEPTH_W16");
            if (w16 && *w16 != '\0' && *w16 != '0')
                return DepthProbeMode::reverse_w16;
            // M2_CLT_DEPTH_RAW remains an explicit selector: =1 selects the
            // recovered guest mapping and =0 preserves the old reversed-depth
            // probe.  Unset uses the recovered guest mapping.  The MAME mode
            // remains as an equivalence probe for existing run recipes.
            const char* raw = std::getenv("M2_CLT_DEPTH_RAW");
            if (raw && *raw != '\0')
                return *raw == '0' ? DepthProbeMode::reverse_float
                                   : DepthProbeMode::guest_float;
            return DepthProbeMode::guest_float;
        }();
        return mode;
    }

    // The checked-in CLT reference names DBZCNTL's relations in hardware-Z
    // terms.  For the reciprocal W emitted by Mercury, a larger incoming W
    // is the closer/smaller-Z case.  Keep that translation explicit instead
    // of silently treating reciprocal W as a conventional host Z value.
    static bool guest_w_allows(uint32_t depth_func, float incoming,
                               float stored, bool test_enabled, bool z_update)
    {
        if (!test_enabled || depth_func == kCltDbzAll) return true;
        const CltDepthRelation relation = clt_depth_relation(incoming, stored);
        const uint32_t mask =
            relation == CltDepthRelation::smaller
                ? (z_update ? kCltDbzZGt : kCltDbzPixelGt)
                : relation == CltDepthRelation::greater
                      ? (z_update ? kCltDbzZLt : kCltDbzPixelLt)
                      : (z_update ? kCltDbzZEq : kCltDbzPixelEq);
        return (depth_func & mask) != 0;
    }

    static float depth_clear_value()
    {
        static const float value = [] {
            const char* raw = std::getenv("M2_CLT_DEPTH_CLEAR");
            if (!raw || *raw == '\0') {
                const DepthProbeMode mode = depth_probe_mode();
                return mode == DepthProbeMode::reverse_float ||
                               mode == DepthProbeMode::direct_float ||
                               mode == DepthProbeMode::reverse_w16 ||
                               mode == DepthProbeMode::direct_w16
                           ? 1.f
                           : 0.f;
            }
            char* end = nullptr;
            const float parsed = std::strtof(raw, &end);
            if (end == raw || !std::isfinite(parsed)) return 0.f;
            return std::clamp(parsed, 0.f, 1.f);
        }();
        return value;
    }

    static float te_z_fill_value(uint32_t z_fill_word)
    {
        // Same guest-authored fill-word contract the GL backends use; the
        // probe modes below are software-only observation inversions.
        const float value = clt_te_z_fill_depth(z_fill_word);
        const DepthProbeMode mode = depth_probe_mode();
        return mode == DepthProbeMode::reverse_float ||
                       mode == DepthProbeMode::reverse_w16
                   ? 1.f - value
                   : value;
    }

public:
    // Writes <dir>/frame_%04u.ppm starting at frame 0000.
    // M2_CAPTURE_FROM (host-frame floor, default 0 = unchanged): frames
    // below the floor are presented but not written, so a boot leg can
    // capture only late (e.g. race-era) frames. Off by default.
    SoftCaptureBackend(int w, int h, std::filesystem::path dir)
        : m_w(w), m_h(h), m_dir(std::move(dir))
    {
        m_fb.assign(static_cast<size_t>(m_w) * m_h, 0);
        m_db.assign(static_cast<size_t>(m_w) * m_h, depth_clear_value());
        m_db_owner.assign(static_cast<size_t>(m_w) * m_h, {});
        if (depth_probe_mode() == DepthProbeMode::packed_w16) {
            const uint16_t clear_cell =
                m2_w16_packed(depth_clear_value(), 0u);
            m_db_packed.assign(static_cast<size_t>(m_w) * m_h, clear_cell);
        }
        if (const char* from = std::getenv("M2_CAPTURE_FROM")) {
            const long v = std::strtol(from, nullptr, 0);
            if (v > 0) m_from = static_cast<uint32_t>(v);
        }
    }

    bool init(SDL_Window*) override { return true; }
    void shutdown() override {}

    // First failed --capture output operation (directory creation, open,
    // write, flush or close). Empty while output remains healthy. The host
    // reads this after its frame pump so an explicit capture request cannot
    // exit successfully without the requested frames.
    bool has_output_error() const { return !m_output_error.empty(); }
    const std::string& output_error() const { return m_output_error; }

    void clt_clear_z(uint32_t z_fill_word) override
    {
        const float value = te_z_fill_value(z_fill_word);
        for (auto& z : m_db) z = value;
        for (auto& owner : m_db_owner) owner = {};
        if (depth_probe_mode() == DepthProbeMode::packed_w16) {
            const uint16_t clear_cell = m2_w16_packed(value, 0u);
            if (m_db_packed.size() != m_db.size())
                m_db_packed.assign(m_db.size(), clear_cell);
            else
                for (auto& cell : m_db_packed) cell = clear_cell;
        }
    }

    void clt_clear_color(uint32_t red_word, uint32_t green_word,
                         uint32_t blue_word, uint32_t alpha_word) override
    {
        const uint32_t color =
            f2u8(clt_te_color_component(red_word)) |
            (f2u8(clt_te_color_component(green_word)) << 8u) |
            (f2u8(clt_te_color_component(blue_word)) << 16u) |
            (f2u8(clt_te_color_component(alpha_word)) << 24u);
        // The recovered TE driver answers TE_CLEAR_FRAME_BUFFER with a clear
        // list clipped by DBFBCLIP/window state like any guest primitive
        // (driver.c:251-278), so the platform clear fills the scissor box the
        // interpreter maintains for color writes. `scissor_disable()` sets
        // m_sw/m_sh negative, which falls back to the full-target fill for a
        // request with no bound destination extent.
        auto& metadata = m_metadata_targets[m_target_base];
        if (metadata.size() != m_fb.size()) metadata.assign(m_fb.size(), 0xff00ff00u);
        const uint32_t meta = 0xff000000u | ((color >> 16u) & 0xff00u);
        if (m_sw < 0 || m_sh < 0) {
            std::fill(metadata.begin(), metadata.end(), meta);
            std::fill(m_fb.begin(), m_fb.end(), color);
            return;
        }
        const int x0 = std::max(0, m_sx);
        const int y0 = std::max(0, m_sy);
        const int x1 = std::min(m_w, m_sx + m_sw);
        const int y1 = std::min(m_h, m_sy + m_sh);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                { m_fb[static_cast<size_t>(y) * m_w + x] = color;
                  metadata[static_cast<size_t>(y) * m_w + x] = meta; }
    }

    // Diagnostic-only cel raster dump (M2_CEL_DUMP_SIZE=WxH[,WxH...], default
    // off; M2_CEL_DUMP_DIR overrides the build/scratch/cel_dump default). A
    // draw's CelId is reassigned as guest textures are re-uploaded, so the
    // stable handle on "which resource is this draw sampling" is the raster's
    // dimensions. Each distinct raster of a matching size is written once as
    // an uncompressed PPM so a draw can be attributed to its content.
    static void cel_dump_if_matched(int w, int h, const uint32_t* rgba,
                                    CelId cel)
    {
        static const std::vector<std::pair<int, int>> sizes = [] {
            std::vector<std::pair<int, int>> out;
            const char* v = std::getenv("M2_CEL_DUMP_SIZE");
            if (!v || !*v || *v == '0') return out;
            const char* p = v;
            while (*p) {
                int dw = 0, dh = 0;
                if (std::sscanf(p, "%dx%d", &dw, &dh) == 2 && dw > 0 && dh > 0)
                    out.emplace_back(dw, dh);
                const char* comma = std::strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
            return out;
        }();
        if (sizes.empty() || !rgba || w <= 0 || h <= 0) return;
        bool wanted = false;
        for (const auto& s : sizes)
            if (s.first == w && s.second == h) wanted = true;
        if (!wanted) return;
        uint32_t hash = 2166136261u;
        uint64_t rgba_hash = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const uint8_t*>(rgba);
        for (size_t i = 0; i < static_cast<size_t>(w) * h * 4u; ++i) {
            rgba_hash ^= bytes[i];
            rgba_hash *= 1099511628211ull;
        }
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            hash = (hash ^ rgba[i]) * 16777619u;
        }
        static std::vector<uint32_t> dumped;
        const uint32_t key = hash ^ (static_cast<uint32_t>(w) << 20) ^
                             static_cast<uint32_t>(h);
        if (std::find(dumped.begin(), dumped.end(), key) != dumped.end())
            return;
        dumped.push_back(key);
        static const std::string dir = [] {
            const char* d = std::getenv("M2_CEL_DUMP_DIR");
            return d && *d ? std::string(d) : std::string("build/scratch/cel_dump");
        }();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[128];
        std::snprintf(name, sizeof(name), "%s/cel_%dx%d_%08x_%016llx.ppm",
                      dir.c_str(), w, h, hash,
                      static_cast<unsigned long long>(rgba_hash));
        FILE* f = std::fopen(name, "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            rgb[i * 3] = static_cast<uint8_t>(rgba[i] & 0xffu);
            rgb[i * 3 + 1] = static_cast<uint8_t>((rgba[i] >> 8u) & 0xffu);
            rgb[i * 3 + 2] = static_cast<uint8_t>((rgba[i] >> 16u) & 0xffu);
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "[cel-dump] cel=%d %s (%dx%d)\n",
                     static_cast<int>(cel), name, w, h);
    }

    CelId cel_upload_rgba(int w, int h, const void* pixels) override
    {
        return cel_upload_rgba_ex(w, h, pixels, false, false);
    }

    CelId cel_upload_rgba_ex(int w, int h, const void* pixels,
                             bool mipmap_linear, bool wrap_repeat) override
    {
        if (!pixels || w <= 0 || h <= 0) return kInvalidCel;
        Cel c;
        c.w = w;
        c.h = h;
        c.default_wrap = wrap_repeat;
        const auto* p = static_cast<const uint32_t*>(pixels);
        c.rgba.assign(p, p + static_cast<size_t>(w) * h);
        if (mipmap_linear) generate_mip_tail(c, c.w, c.h, c.mips.size());
        const CelId id = ++m_next;
        cel_dump_if_matched(c.w, c.h, c.rgba.data(), id);
        m_cels.emplace(id, std::move(c));
        return id;
    }

    bool clt_supports_authored_lod_chain() const override { return true; }

    CelId cel_upload_rgba_lod_chain(int w, int h, const void* pixels,
                                    const CelLodLevel* levels,
                                    size_t level_count, bool mipmap_linear,
                                    bool wrap_repeat,
                                    uint64_t content_hash,
                                    uint64_t lod_chain_hash) override
    {
        (void)content_hash;
        (void)lod_chain_hash;
        if (!pixels || w <= 0 || h <= 0) return kInvalidCel;
        Cel c;
        c.w = w;
        c.h = h;
        c.default_wrap = wrap_repeat;
        const auto* p = static_cast<const uint32_t*>(pixels);
        c.rgba.assign(p, p + static_cast<size_t>(w) * h);
        // The guest's own coarser levels, in its own order. They replace the
        // host reduction the guest never asked for; the levels below the
        // authored tail are still host work, so keep reducing from the
        // coarsest level the guest did supply.
        for (size_t i = 0; i < level_count; ++i) {
            const CelLodLevel& level = levels[i];
            if (!level.rgba || level.w <= 0 || level.h <= 0) break;
            Mip mip;
            mip.w = level.w;
            mip.h = level.h;
            mip.rgba.assign(
                level.rgba,
                level.rgba + static_cast<size_t>(level.w) * level.h);
            c.mips.push_back(std::move(mip));
        }
        if (mipmap_linear && !c.mips.empty()) {
            const Mip& tail = c.mips.back();
            generate_mip_tail(c, tail.w, tail.h, c.mips.size());
        }
        const CelId id = ++m_next;
        cel_dump_if_matched(c.w, c.h, c.rgba.data(), id);
        m_cels.emplace(id, std::move(c));
        return id;
    }

    void cel_free(CelId cel) override { m_cels.erase(cel); }

    void cel_blit(CelId cel, float dx, float dy, float dw, float dh,
                  float r, float g, float b, float a) override
    {
        auto it = m_cels.find(cel);
        if (it == m_cels.end()) return;
        if (dw <= 0.f || dh <= 0.f) return;
        const unsigned ga = f2u8(a);
        if (ga == 0) return;
        const Cel& c = it->second;
        const uint8_t tr = f2u8(r), tg = f2u8(g), tb = f2u8(b);
        const int x0 = static_cast<int>(dx), y0 = static_cast<int>(dy);
        const int x1 = static_cast<int>(dx + dw), y1 = static_cast<int>(dy + dh);
        for (int y = y0; y < y1; ++y) {
            const float v = (static_cast<float>(y) + 0.5f - dy) / dh;
            for (int x = x0; x < x1; ++x) {
                const float u = (static_cast<float>(x) + 0.5f - dx) / dw;
                uint32_t px = sample(c, u, v);
                // GL path multiplies texel by tint — mirror exactly.
                const unsigned sr = (px >> 0 & 0xff) * tr / 255;
                const unsigned sg = (px >> 8 & 0xff) * tg / 255;
                const unsigned sb = (px >> 16 & 0xff) * tb / 255;
                const unsigned sa = ((px >> 24) * ga + 127) / 255;
                blend_px(x, y, sr | (sg << 8) | (sb << 16) | (sa << 24));
            }
        }
    }

    void fill_rect(float dx, float dy, float dw, float dh,
                   float r, float g, float b, float a) override
    {
        const unsigned ga = f2u8(a);
        if (ga == 0) return;
        const uint32_t px =
            f2u8(r) | (f2u8(g) << 8) | (f2u8(b) << 16) | (ga << 24);
        const int x0 = static_cast<int>(dx), y0 = static_cast<int>(dy);
        const int x1 = static_cast<int>(dx + dw), y1 = static_cast<int>(dy + dh);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend_px(x, y, px);
    }

    void scissor(float x, float y, float w, float h) override
    {
        m_sx = static_cast<int>(x);
        m_sy = static_cast<int>(y);
        m_sw = static_cast<int>(w);
        m_sh = static_cast<int>(h);
    }
    void scissor_disable() override
    {
        m_sx = m_sy = 0;
        m_sw = m_sh = -1;
    }

    void clt_draw(const CltVertex* vertices, uint32_t count, bool strip,
                  bool textured, CelId texture, float target_w,
                  float target_h, bool blend_enabled,
                  CltDiscard discard, CltDepth depth,
                  CltTextureSampling sampling,
                  CltBlendState blend,
                  bool independent_triangles = false) override
    {
        // Painter's-order dump (M2_DRAW_ORDER_FRAME=<host frame> or
        // comma-separated frames, default off): log every clt_draw of the
        // target frames with its bbox and mode so a covering draw is visible
        // in sequence. Capped with an expiry line so a hot frame cannot spam
        // the log.
        static const std::string order_frames = [] {
            const char* v = std::getenv("M2_DRAW_ORDER_FRAME");
            return v ? std::string(",") + v + "," : std::string();
        }();
        const bool order_hit =
            !order_frames.empty() &&
            order_frames.find("," + std::to_string(m_frame) + ",") !=
                std::string::npos;
        if (order_hit && vertices && count >= 3u && count <= 65536u) {
            static uint32_t order_n = 0;
            static bool order_expired = false;
            if (order_n < 8000u) {
                float mnx = vertices[0].x, mxx = vertices[0].x;
                float mny = vertices[0].y, mxy = vertices[0].y;
                for (uint32_t i = 1; i < count; ++i) {
                    mnx = std::min(mnx, vertices[i].x);
                    mxx = std::max(mxx, vertices[i].x);
                    mny = std::min(mny, vertices[i].y);
                    mxy = std::max(mxy, vertices[i].y);
                }
                const Cel* info_tex = nullptr;
                if (textured) {
                    auto it = m_cels.find(texture);
                    if (it != m_cels.end()) info_tex = &it->second;
                }
                std::fprintf(stderr,
                             "[draw-order] seq=%u tex=%d cel=%d wh=%dx%d blend=%d depth=%d,%d "
                             "bbox=(%.1f,%.1f)-(%.1f,%.1f) rgb0=(%.2f,%.2f,%.2f,%.2f) ow0=%.4g "
                             "px0=%#x n=%u\n",
                             order_n, textured ? 1 : 0, (int)texture,
                             info_tex ? info_tex->w : 0,
                             info_tex ? info_tex->h : 0,
                             blend_enabled ? 1 : 0,
                             depth.test_enabled ? 1 : 0,
                             depth.write_enabled ? 1 : 0, mnx, mny, mxx, mxy,
                             vertices[0].r, vertices[0].g, vertices[0].b,
                             vertices[0].a, vertices[0].one_over_w,
                             info_tex && !info_tex->rgba.empty()
                                 ? info_tex->rgba[0]
                                 : 0u,
                             count);
                static const uint32_t max_seq = [] {
                    const char* v = std::getenv("M2_DRAW_MAX_SEQ");
                    return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : UINT32_MAX;
                }();
                static const auto [skip_min, skip_max] = []() -> std::pair<uint32_t, uint32_t> {
                    const char* v = std::getenv("M2_DRAW_SKIP_RANGE");
                    if (!v || !*v) return {UINT32_MAX, 0};
                    uint32_t s = 0, e = 0;
                    if (std::sscanf(v, "%u-%u", &s, &e) == 2) return {s, e};
                    if (std::sscanf(v, "%u", &s) == 1) return {s, s};
                    return {UINT32_MAX, 0};
                }();
                if (order_n >= max_seq || (order_n >= skip_min && order_n <= skip_max)) {
                    ++order_n;
                    return;
                }
                ++order_n;
            } else if (!order_expired) {
                order_expired = true;
                std::fprintf(stderr,
                             "[draw-order] cap reached at frame %u — further "
                             "entries suppressed\n",
                             m_frame);
            }
        }
        if (!vertices || count < 3u || count > 65536u || target_w <= 0.f ||
            target_h <= 0.f)
            return;
        if (order_census_enabled()) ++m_order_count;

        auto& metadata = m_metadata_targets[m_target_base];
        if (metadata.size() != m_fb.size()) metadata.assign(m_fb.size(), 0xff00ff00u);
        const Cel* source_metadata = nullptr;
        if (blend.source_ssb_mask != kInvalidCel) {
            const auto found = m_cels.find(blend.source_ssb_mask);
            if (found == m_cels.end()) return;
            source_metadata = &found->second;
        }
        const Cel* tex = nullptr;
        const Cel* ssb_mask = nullptr;
        const Cel* secondary_source = nullptr;
        if (textured) {
            const auto it = m_cels.find(texture);
            if (it == m_cels.end()) return;
            tex = &it->second;
        }
        if (clt_blend_uses_secondary_source(blend.mode)) {
            const auto it = m_cels.find(blend.secondary_source);
            if (it == m_cels.end()) return;
            secondary_source = &it->second;
        }
        if (discard.ssb_zero) {
            const auto it = m_cels.find(discard.ssb_mask);
            if (it == m_cels.end()) return;
            ssb_mask = &it->second;
            if (blend.weighted_mix_ssb_mask == kInvalidCel)
                blend.weighted_mix_ssb_mask = discard.ssb_mask;
        }
        // Per-texel SSB coefficient selection: the interpreter resolves both
        // coefficient entries and reports when the draw's SSB signal selects
        // between them.  The split mask rides the blend state (the
        // interpreter uploads the texture's SSB bitmap there) and falls back
        // to the discard's own mask cel, which carries the same bitmap.
        const bool ssb_entry_split =
            blend.mode == CltBlendMode::coefficient_weighted_mix &&
            blend.weighted_mix_ssb_selects_entry1;
        if (ssb_entry_split || (blend.weighted_mix_ssb_mask != kInvalidCel && (blend.dsb.select == CltDsbSelect::object_ssb || blend.tab.enabled ||
         blend.mode == CltBlendMode::general_internal || blend.mode == CltBlendMode::general_secondary))) {
            CelId mask_cel = blend.weighted_mix_ssb_mask;
            if (mask_cel == kInvalidCel) mask_cel = discard.ssb_mask;
            const auto it = m_cels.find(mask_cel);
            if (it == m_cels.end()) return;
            ssb_mask = &it->second;
        }

        const uint32_t draw_sequence = m_draw_sequence++;

        // Narrow livery/nose depth audit. The interpreter has already proven
        // the guest submits the 192x112 State 2 livery cel and the State 3
        // nose cels (647x51, 988x79, and 198x52). At this backend boundary,
        // log only those cels and large draws overlapping the car region so
        // geometry coverage, depth rejection, and texture/discard loss stay
        // distinguishable. This is observation only.
        static const bool livery_diag = [] {
            const char* value = std::getenv("M2_LIVERY_DRAW_DIAG");
            return value && *value != '\0' && *value != '0';
        }();
        static const uint32_t livery_diag_from = [] {
            const char* value =
                std::getenv("M2_LIVERY_DRAW_DIAG_FRAME");
            if (!value || !*value) return 0u;
            const long parsed = std::strtol(value, nullptr, 0);
            return parsed > 0 ? static_cast<uint32_t>(parsed) : 0u;
        }();
        bool livery_diag_entry = false;
        bool livery_body_entry = false;
        bool livery_nose_entry = false;
        float livery_min_x = 0.f, livery_max_x = 0.f;
        float livery_min_y = 0.f, livery_max_y = 0.f;
        if (livery_diag && m_frame >= livery_diag_from && vertices &&
            count >= 3u) {
            livery_min_x = livery_max_x = vertices[0].x;
            livery_min_y = livery_max_y = vertices[0].y;
            for (uint32_t i = 1; i < count; ++i) {
                livery_min_x = std::min(livery_min_x, vertices[i].x);
                livery_max_x = std::max(livery_max_x, vertices[i].x);
                livery_min_y = std::min(livery_min_y, vertices[i].y);
                livery_max_y = std::max(livery_max_y, vertices[i].y);
            }
            const bool overlaps_car = livery_max_x >= 320.f &&
                                      livery_min_x <= 590.f &&
                                      livery_max_y >= 190.f &&
                                      livery_min_y <= 330.f;
            const float area =
                (livery_max_x - livery_min_x) *
                (livery_max_y - livery_min_y);
            float livery_max_ow = vertices[0].one_over_w;
            for (uint32_t i = 1; i < count; ++i)
                livery_max_ow = std::max(livery_max_ow,
                                         vertices[i].one_over_w);
            livery_body_entry = tex && tex->w == 192 && tex->h == 112;
            livery_nose_entry =
                tex && ((tex->w == 647 && tex->h == 51) ||
                        (tex->w == 988 && tex->h == 79) ||
                        (tex->w == 198 && tex->h == 52));
            const bool depth_audit_entry =
                livery_body_entry || livery_nose_entry;
            const bool depth_write_candidate =
                depth.write_enabled && livery_max_ow > 0.012f;
            // A "cover" entry exists to catch geometry that could hide the
            // audited cels, which requires it to write depth: the room's
            // depth-free overlay strips would otherwise consume the entry cap
            // before the car's own draws are reached.
            const bool cover_entry =
                depth.write_enabled &&
                ((textured && area >= 400.f) ||
                 (!textured && area >= 2000.f) || depth_write_candidate);
            livery_diag_entry = depth_audit_entry ||
                                (overlaps_car && cover_entry);
        }
        uint32_t livery_depth_pass = 0;
        uint32_t livery_depth_fail = 0;
        uint32_t livery_color_pixels = 0;
        float livery_pz_min = 1.f, livery_pz_max = 0.f;
        float livery_db_min = 1.f, livery_db_max = 0.f;
        float livery_first_fail_pz = 0.f, livery_first_fail_db = 0.f;
        int livery_first_fail_x = -1, livery_first_fail_y = -1;
        DbOwner livery_first_owner{};
        bool livery_first_fail_recorded = false;

        // The guest supplies reciprocal homogeneous W.  Alternate polarity
        // and block-float reductions remain explicit probes, while the
        // default path uses the recovered guest ordering.
        const DepthProbeMode depth_mode = depth_probe_mode();

        // Draw diagnostics (M2_DRAW_DIAG=1, default off): classify every
        // clt_draw call by its pre-scale vertex extent — where guest 3D
        // geometry lands relative to the 640x480 target, and which cull stage
        // (off-screen, degenerate, NaN) removes it. Capped so a hot frame
        // cannot spam the log.
        static const bool draw_diag = [] {
            const char* v = std::getenv("M2_DRAW_DIAG");
            return v && v[0] != '0';
        }();
        static const uint32_t draw_diag_from = [] {
            const char* v = std::getenv("M2_DRAW_DIAG_FROM");
            return v ? static_cast<uint32_t>(strtoul(v, nullptr, 0)) : 0u;
        }();
        if (draw_diag && m_frame >= draw_diag_from) {
            static uint32_t diag_n = 0;
            static bool diag_expired = false;
            if (diag_n >= 400u) {
                if (!diag_expired) {
                    diag_expired = true;
                    std::fprintf(stderr,
                                 "[clt-draw] cap reached at frame %u — "
                                 "further entries suppressed\n",
                                 m_frame);
                }
            } else {
                float draw_minx = vertices[0].x, draw_maxx = vertices[0].x;
                float draw_miny = vertices[0].y, draw_maxy = vertices[0].y;
                bool draw_nan = false;
                for (uint32_t draw_i = 0; draw_i < count; ++draw_i) {
                    if (std::isnan(vertices[draw_i].x) ||
                        std::isnan(vertices[draw_i].y))
                        draw_nan = true;
                    draw_minx = std::min(draw_minx, vertices[draw_i].x);
                    draw_maxx = std::max(draw_maxx, vertices[draw_i].x);
                    draw_miny = std::min(draw_miny, vertices[draw_i].y);
                    draw_maxy = std::max(draw_maxy, vertices[draw_i].y);
                }
                const char* draw_cls = "onscreen";
                if (draw_nan) draw_cls = "nan";
                else if (draw_maxx < 0.f || draw_maxy < 0.f ||
                         draw_minx > target_w || draw_miny > target_h)
                    draw_cls = "offscreen";
                else if (draw_maxx < 1e-3f && draw_maxy < 1e-3f &&
                         draw_minx > -1e-3f && draw_miny > -1e-3f)
                    draw_cls = "degenerate-origin";
                else if (draw_minx > 640.f || draw_miny > 480.f)
                    draw_cls = "offscreen-lt";
                std::fprintf(stderr,
                             "[clt-draw] n=%u frame=%u cls=%s textured=%d "
                             "count=%u minx=%.1f maxx=%.1f miny=%.1f maxy=%.1f "
                             "ow0=%.3g depth_test=%d depth_write=%d\n",
                             diag_n++, m_frame, draw_cls, textured ? 1 : 0,
                             count, draw_minx, draw_maxx, draw_miny, draw_maxy,
                             vertices[0].one_over_w,
                             depth.test_enabled ? 1 : 0,
                             depth.write_enabled ? 1 : 0);
                ++diag_n;
            }
        }

        const char* trace_env = std::getenv("M2_CLT_BRIDGE_TRACE");
        if (trace_env && trace_env[0] != '0') {
            std::fprintf(stderr, "[soft_draw] frame=%u textured=%d cel=%d count=%u target=(%.1f,%.1f) v0=(%.1f,%.1f) v1=(%.1f,%.1f) v2=(%.1f,%.1f) rgb0=(%.3f,%.3f,%.3f,%.3f) ow0=%.6g\n",
                         m_frame, textured, texture, count, target_w, target_h,
                         vertices[0].x, vertices[0].y, vertices[1].x, vertices[1].y, vertices[2].x, vertices[2].y,
                         vertices[0].r, vertices[0].g, vertices[0].b, vertices[0].a,
                         vertices[0].one_over_w);
        }

        const float sx = static_cast<float>(m_w) / target_w;
        const float sy = static_cast<float>(m_h) / target_h;
        const CltBlendMode blend_mode =
            clt_effective_blend_mode(blend_enabled, blend);
        const bool near_color_z = clt_near_color_z_fill(
            depth, vertices, count, textured, blend_enabled);
        if (clt_blend_uses_secondary_source(blend_mode) &&
            ((( !blend_enabled || !textured) && blend_mode != CltBlendMode::general_secondary) ||
         (blend.alpha_control_unsupported && blend_mode != CltBlendMode::general_secondary) ||
             discard.separate_ssb_uv))
            return;
        // Raster visibility audit: these counters are observation only.
        // Keep them local to one clt_draw so a bounded draw-diagnostic entry can separate
        // guest coverage from depth/discard loss without changing the pixel
        // path or adding a host rendering policy.
        uint32_t raster_triangles = 0;
        uint64_t raster_candidate_pixels = 0;
        uint64_t raster_depth_pass_pixels = 0;
        uint64_t raster_depth_write_pixels = 0;
        uint64_t raster_ssb_discard_pixels = 0;
        uint64_t raster_alpha_discard_pixels = 0;
        uint64_t raster_rgb_discard_pixels = 0;
        uint64_t raster_zclip_discard_pixels = 0;
        uint64_t raster_color_pixels = 0;
        // Screen-space signed-area sign census (M2_CLT_FACING_CENSUS): a guest
        // that culled back faces in Mercury emits one sign only.
        uint32_t raster_tri_pos = 0;
        uint32_t raster_tri_neg = 0;
        bool raster_uv_seen = false;
        float raster_uv_min_u = 0.f, raster_uv_max_u = 0.f;
        float raster_uv_min_v = 0.f, raster_uv_max_v = 0.f;
        bool raster_sample_seen = false;
        float raster_sample_u = 0.f, raster_sample_v = 0.f;
        float raster_sample_lod = 0.f;
        uint32_t raster_sample_rgba = 0u;
        int raster_sample_x = -1, raster_sample_y = -1;
        if (textured && tex) {
            for (uint32_t i = 0; i < count; ++i) {
                const CltVertex& vertex = vertices[i];
                if (!std::isfinite(vertex.u) || !std::isfinite(vertex.v))
                    continue;
                if (!raster_uv_seen) {
                    raster_uv_min_u = raster_uv_max_u = vertex.u;
                    raster_uv_min_v = raster_uv_max_v = vertex.v;
                    raster_uv_seen = true;
                } else {
                    raster_uv_min_u = std::min(raster_uv_min_u, vertex.u);
                    raster_uv_max_u = std::max(raster_uv_max_u, vertex.u);
                    raster_uv_min_v = std::min(raster_uv_min_v, vertex.v);
                    raster_uv_max_v = std::max(raster_uv_max_v, vertex.v);
                }
            }
        }
        const auto draw_triangle = [&](const CltVertex& a,
                                       const CltVertex& b,
                                       const CltVertex& c) {
            const float ax = a.x * sx, ay = a.y * sy;
            const float bx = b.x * sx, by = b.y * sy;
            const float cx = c.x * sx, cy = c.y * sy;
            const float area = (bx - ax) * (cy - ay) -
                               (by - ay) * (cx - ax);
            if (std::fabs(area) < 1e-6f) return;
            ++raster_triangles;
            if (area > 0.f) ++raster_tri_pos; else ++raster_tri_neg;

            // The guest stream carries projected u/w and v/w plus reciprocal
            // W.  Derive screen-space UV gradients from those same rational
            // quantities so M2 LOD selection follows the rasterized triangle,
            // including a tile boundary crossed between vertices.
            const float a_ow = a.one_over_w > 1e-7f &&
                                       std::isfinite(a.one_over_w)
                                   ? a.one_over_w
                                   : 1.f;
            const float b_ow = b.one_over_w > 1e-7f &&
                                       std::isfinite(b.one_over_w)
                                   ? b.one_over_w
                                   : 1.f;
            const float c_ow = c.one_over_w > 1e-7f &&
                                       std::isfinite(c.one_over_w)
                                   ? c.one_over_w
                                   : 1.f;
            const float dwa_dx = (by - cy) / area;
            const float dwa_dy = (cx - bx) / area;
            const float dwb_dx = (cy - ay) / area;
            const float dwb_dy = (ax - cx) / area;
            const float dwc_dx = (ay - by) / area;
            const float dwc_dy = (bx - ax) / area;
            const float dow_dx = dwa_dx * a_ow + dwb_dx * b_ow +
                                 dwc_dx * c_ow;
            const float dow_dy = dwa_dy * a_ow + dwb_dy * b_ow +
                                 dwc_dy * c_ow;
            const float dqu_dx = dwa_dx * a.u * a_ow +
                                 dwb_dx * b.u * b_ow +
                                 dwc_dx * c.u * c_ow;
            const float dqu_dy = dwa_dy * a.u * a_ow +
                                 dwb_dy * b.u * b_ow +
                                 dwc_dy * c.u * c_ow;
            const float dqv_dx = dwa_dx * a.v * a_ow +
                                 dwb_dx * b.v * b_ow +
                                 dwc_dx * c.v * c_ow;
            const float dqv_dy = dwa_dy * a.v * a_ow +
                                 dwb_dy * b.v * b_ow +
                                 dwc_dy * c.v * c_ow;

            const float min_x = std::min({ax, bx, cx});
            const float max_x = std::max({ax, bx, cx});
            const float min_y = std::min({ay, by, cy});
            const float max_y = std::max({ay, by, cy});
            if (max_x < 0.f || max_y < 0.f || min_x > m_w || min_y > m_h)
                return;
            const float clipped_min_x = std::max(0.f, min_x);
            const float clipped_min_y = std::max(0.f, min_y);
            const float clipped_max_x = std::min(static_cast<float>(m_w - 1),
                                                 max_x);
            const float clipped_max_y = std::min(static_cast<float>(m_h - 1),
                                                 max_y);
            const int x0 = static_cast<int>(std::floor(clipped_min_x));
            const int y0 = static_cast<int>(std::floor(clipped_min_y));
            const int x1 = static_cast<int>(std::ceil(clipped_max_x));
            const int y1 = static_cast<int>(std::ceil(clipped_max_y));
            if (x0 > x1 || y0 > y1) return;

            const bool w16_mode =
                depth_mode == DepthProbeMode::reverse_w16 ||
                depth_mode == DepthProbeMode::direct_w16 ||
                depth_mode == DepthProbeMode::mame_w16 ||
                depth_mode == DepthProbeMode::packed_w16;
            const unsigned w_range =
                w16_mode ? m2_wrange(a, b, c) : 0u;
            const auto guest_z = [depth_mode, w_range](float one_over_w) {
                if (depth_mode == DepthProbeMode::reverse_w16 ||
                    depth_mode == DepthProbeMode::direct_w16 ||
                    depth_mode == DepthProbeMode::mame_w16 ||
                    depth_mode == DepthProbeMode::packed_w16) {
                    const uint16_t cell =
                        depth_mode == DepthProbeMode::packed_w16
                            ? m2_w16_packed(one_over_w, w_range)
                            : m2_w16(one_over_w, w_range);
                    const float encoded =
                        depth_mode == DepthProbeMode::packed_w16
                            ? m2_w16_packed_decode(cell)
                            : static_cast<float>(cell) / 65535.f;
                    return depth_mode == DepthProbeMode::direct_w16 ||
                                   depth_mode == DepthProbeMode::mame_w16 ||
                                   depth_mode == DepthProbeMode::packed_w16
                               ? encoded
                               : 1.f - encoded;
                }
                if (depth_mode == DepthProbeMode::guest_float ||
                    depth_mode == DepthProbeMode::mame_float)
                    return std::clamp(one_over_w, 0.f, 1.f);
                if (one_over_w <= 0.f) return 1.f;
                const float value = std::clamp(one_over_w, 0.f, 1.f);
                return depth_mode == DepthProbeMode::direct_float ||
                               depth_mode == DepthProbeMode::mame_float
                           ? value
                           : 1.f - value;
            };
            const float za = guest_z(a.one_over_w);
            const float zb = guest_z(b.one_over_w);
            const float zc = guest_z(c.one_over_w);

            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const float wa = ((bx - px) * (cy - py) -
                                      (by - py) * (cx - px)) / area;
                    const float wb = ((cx - px) * (ay - py) -
                                      (cy - py) * (ax - px)) / area;
                    const float wc = 1.f - wa - wb;
                    if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f)
                        continue;
                    ++raster_candidate_pixels;

                    // TE Z region (DBZCLIP/DBZOFFSET, folded by the
                    // interpreter into the pre-scale vertex space): a pixel
                    // outside the bound region skips the Z test and Z write,
                    // and DBDISCARDCONTROL.ZCLIPPED drops it before color.
                    // The coordinate mapping only runs for Z-enabled draws;
                    // ztest=0 fills keep their original per-pixel cost.
                    bool z_clipped = false;
                    if (depth.test_enabled) {
                        const int pixel_space_x = static_cast<int>(std::floor(
                            (static_cast<float>(x) + 0.5f) / sx));
                        const int pixel_space_y = static_cast<int>(std::floor(
                            (static_cast<float>(y) + 0.5f) / sy));
                        z_clipped = clt_z_clipped(
                            depth, static_cast<float>(pixel_space_x),
                            static_cast<float>(pixel_space_y));
                        if (z_clipped && depth.z_clipped_discard) {
                            ++raster_zclip_discard_pixels;
                            continue;
                        }
                    }
                    const bool z_test_enabled =
                        depth.test_enabled && !z_clipped;
                    const bool z_write_enabled =
                        depth.write_enabled && !z_clipped;

                    if (!writable_px(x, y)) continue;
                    const size_t pidx = static_cast<size_t>(y) * m_w + x;
                    const float interpolated_z = wa * za + wb * zb + wc * zc;
                    const bool is_packed =
                        depth_mode == DepthProbeMode::packed_w16;
                    const uint16_t incoming_cell =
                        is_packed ? m2_w16_packed(wa * a.one_over_w +
                                                     wb * b.one_over_w +
                                                     wc * c.one_over_w,
                                                 w_range)
                                  : 0u;
                    const float pz =
                        is_packed ? m2_w16_packed_decode(incoming_cell)
                                  : interpolated_z;
                    if (livery_body_entry || livery_nose_entry) {
                        livery_pz_min = std::min(livery_pz_min, pz);
                        livery_pz_max = std::max(livery_pz_max, pz);
                        livery_db_min = std::min(livery_db_min, m_db[pidx]);
                        livery_db_max = std::max(livery_db_max, m_db[pidx]);
                    }
                    const float stored_z = m_db[pidx];
                    const uint16_t stored_cell =
                        is_packed && pidx < m_db_packed.size()
                            ? m_db_packed[pidx]
                            : 0u;
                    const bool guest_w_mode =
                        depth_mode == DepthProbeMode::guest_float ||
                        depth_mode == DepthProbeMode::mame_float ||
                        depth_mode == DepthProbeMode::mame_w16 ||
                        depth_mode == DepthProbeMode::packed_w16;
                    const bool pixel_pass =
                        is_packed
                            ? m2_w16_packed_allows(depth.depth_func,
                                                   incoming_cell, stored_cell,
                                                   z_test_enabled, false)
                            : guest_w_mode
                                  ? guest_w_allows(depth.depth_func, pz,
                                                   stored_z,
                                                   z_test_enabled, false)
                                  : clt_depth_pixel_pass(
                                        depth.depth_func, pz, stored_z,
                                        z_test_enabled);
                    const bool depth_write_pass =
                        is_packed
                            ? z_write_enabled &&
                                  m2_w16_packed_allows(
                                      depth.depth_func, incoming_cell,
                                      stored_cell, z_test_enabled, true)
                            : guest_w_mode
                                  ? z_write_enabled &&
                                        guest_w_allows(depth.depth_func, pz,
                                                       stored_z,
                                                       z_test_enabled,
                                                       true)
                                  : clt_depth_write_pass(
                                        depth.depth_func, pz, stored_z,
                                        z_test_enabled,
                                        z_write_enabled);
                    if (depth_write_pass) {
                        ++raster_depth_write_pixels;
                        m_db[pidx] = pz;
                        if (is_packed && pidx < m_db_packed.size())
                            m_db_packed[pidx] = incoming_cell;
                        if (livery_diag) {
                            DbOwner& owner = m_db_owner[pidx];
                            owner.draw_sequence = draw_sequence;
                            owner.texture = texture;
                            owner.depth_func = depth.depth_func;
                            owner.z = pz;
                            owner.one_over_w =
                                wa * a.one_over_w + wb * b.one_over_w +
                                wc * c.one_over_w;
                            owner.textured = textured;
                            owner.neg_area = area < 0.f;
                            owner.tex_w = tex ? tex->w : 0u;
                            owner.tex_h = tex ? tex->h : 0u;
                            owner.bx0 = std::min({a.x, b.x, c.x});
                            owner.by0 = std::min({a.y, b.y, c.y});
                            owner.bx1 = std::max({a.x, b.x, c.x});
                            owner.by1 = std::max({a.y, b.y, c.y});
                        }
                    }
                    if (!pixel_pass) {
                        if (livery_body_entry || livery_nose_entry) {
                            ++livery_depth_fail;
                            if (!livery_first_fail_recorded) {
                                livery_first_fail_recorded = true;
                                livery_first_fail_pz = pz;
                                livery_first_fail_db = stored_z;
                                livery_first_fail_x = x;
                                livery_first_fail_y = y;
                                livery_first_owner = m_db_owner[pidx];
                            }
                        }
                        continue;
                    }
                    ++raster_depth_pass_pixels;
                    if (livery_body_entry || livery_nose_entry)
                        ++livery_depth_pass;

                    // Diagnostic-only cross-path probe
                    // (M2_CLT_COLOR_PERSPECTIVE=1, default off): the software
                    // model interpolates the guest's vertex colour in screen
                    // space, while the GL backends interpolate it through the
                    // perspective divide their `w` varying performs. Setting
                    // this evaluates the GL convention in the software model so
                    // the two host paths can be compared on one frame instead
                    // of differing unexplained. Observation only: unset keeps
                    // the production model byte-for-byte.
                    static const bool color_perspective = [] {
                        const char* e = std::getenv("M2_CLT_COLOR_PERSPECTIVE");
                        return e && *e != '\0' && *e != '0';
                    }();
                    const float color_w = wa * a_ow + wb * b_ow + wc * c_ow;
                    const bool color_w_ok =
                        color_perspective && color_w > 1e-7f;
                    const auto interpolate_color = [&](float va, float vb,
                                                       float vc) {
                        return color_w_ok
                                   ? (wa * va * a_ow + wb * vb * b_ow +
                                      wc * vc * c_ow) /
                                         color_w
                                   : wa * va + wb * vb + wc * vc;
                    };
                    float r = interpolate_color(a.r, b.r, c.r);
                    float g = interpolate_color(a.g, b.g, c.g);
                    float bl = interpolate_color(a.b, b.b, c.b);
                    float al = interpolate_color(a.a, b.a, c.a);
                    uint32_t px_rgba = 0xffffffffu;
                    float u = 0.f;
                    float v = 0.f;
                    if (tex) {
                        const float inv_w = wa * a_ow + wb * b_ow + wc * c_ow;
                        // DB-source coordinates are screen addresses, not texture UVs.
                        const bool screen_source = blend.db_source_linear &&
                            !clt_blend_uses_secondary_source(blend_mode);
                        if (!screen_source && inv_w > 1e-7f) {
                            u = (wa * a.u * a_ow + wb * b.u * b_ow +
                                 wc * c.u * c_ow) /
                                inv_w;
                            v = (wa * a.v * a_ow + wb * b.v * b_ow +
                                 wc * c.v * c_ow) /
                                inv_w;
                        } else {
                            u = wa * a.u + wb * b.u + wc * c.u;
                            v = wa * a.v + wb * b.v + wc * c.v;
                        }
                        float lod = 0.f;
                        if (!tex->mips.empty() && sampling.lod_max != 0u) {
                            const float du_dx =
                                (dqu_dx - u * dow_dx) / inv_w;
                            const float dv_dx =
                                (dqv_dx - v * dow_dx) / inv_w;
                            const float du_dy =
                                (dqu_dy - u * dow_dy) / inv_w;
                            const float dv_dy =
                                (dqv_dy - v * dow_dy) / inv_w;
                            const float rho_x = std::hypot(
                                du_dx * tex->w, dv_dx * tex->h);
                            const float rho_y = std::hypot(
                                du_dy * tex->w, dv_dy * tex->h);
                            const float rho = std::max(rho_x, rho_y);
                            if (std::isfinite(rho) && rho > 1.f)
                                lod = std::log2(rho);
                        }
                        // Diagnostic-only cross-path probe
                        // (M2_CLT_LOD_FIXED=<level>, default off): the software
                        // model resolves one LOD per triangle from the
                        // triangle's own screen-space UV gradient, while the GL
                        // backends resolve it per fragment from the rasterizer's
                        // derivatives. Forcing a level here evaluates a
                        // single-level sample so a cross-backend difference can
                        // be attributed to level selection instead of the
                        // filter. Observation only: unset keeps the production
                        // model byte-for-byte.
                        static const float lod_fixed = [] {
                            const char* e = std::getenv("M2_CLT_LOD_FIXED");
                            if (!e || *e == '\0') return -1.f;
                            char* end = nullptr;
                            const float parsed = std::strtof(e, &end);
                            return (end == e || !std::isfinite(parsed) ||
                                    parsed < 0.f)
                                       ? -1.f
                                       : parsed;
                        }();
                        if (lod_fixed >= 0.f) lod = lod_fixed;
                        float texture_u = u;
                        float texture_v = v;
                        if (!clt_blend_uses_secondary_source(blend_mode) &&
                            blend.db_source_linear) {
                            const CltDbSourceSampleUv source_uv =
                                clt_db_source_sample_uv(u, v, blend);
                            texture_u = source_uv.u;
                            texture_v = source_uv.v;
                        }
                        px_rgba = sample_texture(*tex, texture_u, texture_v,
                                                 sampling, lod);
                        if (!raster_sample_seen) {
                            raster_sample_seen = true;
                            raster_sample_u = u;
                            raster_sample_v = v;
                            raster_sample_lod = lod;
                            raster_sample_rgba = px_rgba;
                            raster_sample_x = x;
                            raster_sample_y = y;
                        }
                    }
                    // DBDISCARDCONTROL.SSB0 test: only when the guest armed
                    // it.  The weighted-mix entry split also samples the SSB
                    // mask cel, but must not discard SSB-0 texels.
                    float mask_u = u;
                    float mask_v = v;
                    if (clt_blend_uses_secondary_source(blend_mode) ||
                        discard.separate_ssb_uv) {
                        const float mask_inv_w =
                            wa * a_ow + wb * b_ow + wc * c_ow;
                        const bool screen_source = blend.db_source_linear &&
                            clt_blend_uses_secondary_source(blend_mode);
                        if (!screen_source && mask_inv_w > 1e-7f) {
                            mask_u = (wa * a.mask_u * a_ow +
                                      wb * b.mask_u * b_ow +
                                      wc * c.mask_u * c_ow) /
                                     mask_inv_w;
                            mask_v = (wa * a.mask_v * a_ow +
                                      wb * b.mask_v * b_ow +
                                      wc * c.mask_v * c_ow) /
                                     mask_inv_w;
                        } else {
                            mask_u = wa * a.mask_u + wb * b.mask_u +
                                     wc * c.mask_u;
                            mask_v = wa * a.mask_v + wb * b.mask_v +
                                     wc * c.mask_v;
                        }
                    }
                    // In the secondary-source form `mask_*` carries the
                    // framebuffer-source coordinates. The SSB discard still
                    // belongs to the primary texture, whose coordinates are
                    // `u/v`; keep those two address spaces separate.
                    const float ssb_u =
                        clt_blend_uses_secondary_source(blend_mode)
                            ? u
                            : mask_u;
                    const float ssb_v =
                        clt_blend_uses_secondary_source(blend_mode)
                            ? v
                            : mask_v;
                    if (discard.ssb_zero && ssb_mask &&
                        (sample_nearest_pixels(
                             ssb_mask->rgba, ssb_mask->w, ssb_mask->h,
                             ssb_u, ssb_v, sampling.wrap_u, sampling.wrap_v,
                             static_cast<int>(sampling.u_period),
                             static_cast<int>(sampling.v_period)) & 0xffu) == 0u) {
                        ++raster_ssb_discard_pixels;
                        continue;
                    }
                    if (blend.tab.enabled) {
                        const bool ssb = ssb_mask && (sample_nearest_pixels(
                            ssb_mask->rgba, ssb_mask->w, ssb_mask->h,
                            ssb_u, ssb_v, sampling.wrap_u, sampling.wrap_v,
                            static_cast<int>(sampling.u_period),
                            static_cast<int>(sampling.v_period)) & 0xffu) != 0u;
                        const auto stage = clt_tab_evaluate(blend.tab, {r, g, bl, al},
                            {float(px_rgba & 255) / 255.f, float((px_rgba >> 8) & 255) / 255.f,
                             float((px_rgba >> 16) & 255) / 255.f, float(px_rgba >> 24) / 255.f}, ssb);
                        r = stage[0]; g = stage[1]; bl = stage[2]; al = stage[3];
                        px_rgba = 0xffffffffu;
                    }
                    unsigned stage_r = 0;
                    if (blend.texture_alpha_as_color) {
                        const uint32_t alpha = (px_rgba >> 24u) & 0xffu;
                        px_rgba = 0xff000000u | (alpha << 16u) |
                                  (alpha << 8u) | alpha;
                    }
                    unsigned stage_g = 0;
                    unsigned stage_b = 0;
                    unsigned sr = 0;
                    unsigned sg = 0;
                    unsigned sb = 0;
                    unsigned sa = 0;
                    if (blend_mode == CltBlendMode::specular_add) {
                        // M_TBLitTex produces TEXCOLOR=texel*PRIMCOLOR
                        // and TEXALPHA=PRIMALPHA. M_DBSpec then computes
                        // clamp(TEXCOLOR + DBCONSTIN*TEXALPHA).
                        const float tex_r =
                            static_cast<float>((px_rgba >> 0u) & 0xffu) /
                            255.f;
                        const float tex_g =
                            static_cast<float>((px_rgba >> 8u) & 0xffu) /
                            255.f;
                        const float tex_b =
                            static_cast<float>((px_rgba >> 16u) & 0xffu) /
                            255.f;
                        stage_r = f2u8(tex_r * r);
                        stage_g = f2u8(tex_g * g);
                        stage_b = f2u8(tex_b * bl);
                        sr = f2u8(tex_r * r + blend.constant_r * al);
                        sg = f2u8(tex_g * g + blend.constant_g * al);
                        sb = f2u8(tex_b * bl + blend.constant_b * al);
                        sa = f2u8(al);
                    } else if (blend_mode == CltBlendMode::fog_composite) {
                        // M_DBFog computes A = TEXCOLOR*TEXALPHA,
                        // B = DBCONSTIN*(1-TEXALPHA), A+BCLAMP with SRCEN
                        // clear.  TEXALPHA is the per-vertex fog factor `al`,
                        // so the texture-stage color is lerped toward the fog
                        // constant: TEXCOLOR*al + DBCONSTIN*(1 - al).
                        const float tex_r =
                            static_cast<float>((px_rgba >> 0u) & 0xffu) /
                            255.f;
                        const float tex_g =
                            static_cast<float>((px_rgba >> 8u) & 0xffu) /
                            255.f;
                        const float tex_b =
                            static_cast<float>((px_rgba >> 16u) & 0xffu) /
                            255.f;
                        stage_r = f2u8(tex_r * r);
                        stage_g = f2u8(tex_g * g);
                        stage_b = f2u8(tex_b * bl);
                        sr = f2u8(tex_r * r * al +
                                  blend.constant_r * (1.f - al));
                        sg = f2u8(tex_g * g * al +
                                  blend.constant_g * (1.f - al));
                        sb = f2u8(tex_b * bl * al +
                                  blend.constant_b * (1.f - al));
                        // M_DBFog composites opaque world geometry (the
                        // transparent fog path is M_TBFog + M_DBTrans), so the
                        // result is opaque; `al` is a lerp weight, not opacity.
                        sa = 255;
                    } else {
                        stage_r = ((px_rgba >> 0u) & 0xffu) * f2u8(r) / 255u;
                        stage_g = ((px_rgba >> 8u) & 0xffu) * f2u8(g) / 255u;
                        stage_b = ((px_rgba >> 16u) & 0xffu) * f2u8(bl) / 255u;
                        sr = stage_r;
                        sg = stage_g;
                        sb = stage_b;
                        sa = ((px_rgba >> 24u) & 0xffu) * f2u8(al) / 255u;
                        if (blend_mode == CltBlendMode::constant_alpha_over) {
                            // Guest equation A = CONSTCOLOR * TEXALPHA with
                            // B = SRCCOLOR * (1 - TEXALPHA): the texture-stage
                            // alpha weights the guest's DBCONSTIN color against
                            // the bound destination pixel.  The post-texturing
                            // color stays in `stage_*` for the RGB0 discard
                            // test, exactly as for the other forms.
                            sr = f2u8(blend.constant_r);
                            sg = f2u8(blend.constant_g);
                            sb = f2u8(blend.constant_b);
                        }
                    }
                    if (blend_mode == CltBlendMode::opaque_source_over) sa = 255u;
                    unsigned secondary_r = 0;
                    unsigned secondary_g = 0;
                    unsigned secondary_b = 0;
                    unsigned secondary_a = 0;
                    bool source_ssb = false;
                    if (source_metadata && !secondary_source) {
                        const uint32_t meta = sample(*source_metadata,u,v);
                        source_ssb = (meta & 255u) >= 128u;
                    }
                    if (secondary_source) {
                        const CltDbSourceSampleUv source_uv =
                            clt_db_source_sample_uv(mask_u, mask_v, blend);
                        const uint32_t source_rgba =
                            sample(*secondary_source, source_uv.u,
                                   source_uv.v);
                        secondary_r = source_rgba & 0xffu;
                        secondary_g = (source_rgba >> 8u) & 0xffu;
                        secondary_b = (source_rgba >> 16u) & 0xffu;
                        secondary_a = source_rgba >> 24u;
                        if (source_metadata) {
                            const uint32_t meta = sample(*source_metadata, source_uv.u, source_uv.v);
                            source_ssb = (meta & 255u) >= 128u;
                            secondary_a = (meta >> 8u) & 255u;
                        }
                    }
                    if (discard.rgb_zero && stage_r == 0u && stage_g == 0u &&
                        stage_b == 0u) {
                        ++raster_rgb_discard_pixels;
                        continue;
                    }
                    if (blend_mode == CltBlendMode::general_internal ||
                        blend_mode == CltBlendMode::general_secondary) {
                        const bool tssb = ssb_mask && (sample_nearest_pixels(
                            ssb_mask->rgba, ssb_mask->w, ssb_mask->h, ssb_u, ssb_v,
                            sampling.wrap_u, sampling.wrap_v,
                            static_cast<int>(sampling.u_period), static_cast<int>(sampling.v_period)) & 255u);
                        if (discard.alpha_zero && sa == 0u) continue;
                        const auto out = clt_dsb_evaluate(blend.general,
                            {stage_r/255.f, stage_g/255.f, stage_b/255.f, sa/255.f},
                            {secondary_r/255.f, secondary_g/255.f, secondary_b/255.f, secondary_a/255.f}, tssb, source_ssb);
                        const bool dsb = blend.dsb.select == CltDsbSelect::constant_ssb ? blend.dsb.constant :
                            blend.dsb.select == CltDsbSelect::source_ssb ? source_ssb : tssb;
                        metadata[pidx] = 0xff000000u | (dsb ? 255u : 0u) | (f2u8(out[3]) << 8u);
                        copy_px(x, y, f2u8(out[0]) | (f2u8(out[1]) << 8u) |
                            (f2u8(out[2]) << 16u) | (f2u8(out[3]) << 24u));
                        ++raster_color_pixels;
                        continue;
                    }
                    if (discard.alpha_zero && sa == 0u) {
                        ++raster_alpha_discard_pixels;
                        continue;
                    }
                    const bool object_ssb = ssb_mask && (sample_nearest_pixels(
                        ssb_mask->rgba, ssb_mask->w, ssb_mask->h, ssb_u, ssb_v,
                        sampling.wrap_u, sampling.wrap_v,
                        static_cast<int>(sampling.u_period), static_cast<int>(sampling.v_period)) & 255u);
                    const bool dsb = blend.dsb.select == CltDsbSelect::constant_ssb ? blend.dsb.constant :
                        blend.dsb.select == CltDsbSelect::source_ssb ? source_ssb : object_ssb;
                    metadata[pidx] = 0xff000000u | (dsb ? 255u : 0u) |
                        ((blend_mode == CltBlendMode::fog_composite ? f2u8(al) : sa) << 8u);
                    ++raster_color_pixels;
                    if (near_color_z) {
                        m_db[pidx] = is_packed ? kCltNearColorZ : guest_z(kCltNearColorZ);
                        if (is_packed && pidx < m_db_packed.size())
                            m_db_packed[pidx] = 0x3fffu;
                    }
                    if (clt_blend_uses_secondary_source(blend_mode)) {
                        if (blend_mode == CltBlendMode::secondary_constant_over) {
                            stage_r = f2u8(blend.constant_r);
                            stage_g = f2u8(blend.constant_g);
                            stage_b = f2u8(blend.constant_b);
                        }
                        // StreamEventFunc's equation is A=TEXCOLOR*(1-A7),
                        // B=SRCCOLOR*A7, A+B. It is a direct write of the
                        // separately sampled DB source, not a blend against
                        // the current destination. DBDESTALPHACNTL=TEXALPHA
                        // leaves the post-texturing alpha in the written
                        // pixel.
                        const unsigned source_weight =
                            blend_mode == CltBlendMode::secondary_source_alpha ? sa : 255u - sa;
                        const unsigned out_r =
                            (stage_r * (255u - source_weight) + secondary_r * source_weight + 127u) /
                            255u;
                        const unsigned out_g =
                            (stage_g * (255u - source_weight) + secondary_g * source_weight + 127u) /
                            255u;
                        const unsigned out_b =
                            (stage_b * (255u - source_weight) + secondary_b * source_weight + 127u) /
                            255u;
                        copy_px(x, y, out_r | (out_g << 8u) |
                                         (out_b << 16u) | (sa << 24u));
                        if (livery_body_entry || livery_nose_entry)
                            ++livery_color_pixels;
                        continue;
                    }
                    if (blend_mode == CltBlendMode::source_copy) {
                        // Guest B = SRCCOLOR * CONST with a white coefficient
                        // constant and the ALU `B` operation: the
                        // destination-blender output is the fragment
                        // unchanged, so the destination pixel is replaced
                        // rather than alpha-lerped.  Mirrors the GL
                        // (ONE, ZERO) draw.
                        copy_px(x, y, sr | (sg << 8u) | (sb << 16u) |
                                           (sa << 24u));
                        if (livery_body_entry || livery_nose_entry)
                            ++livery_color_pixels;
                        continue;
                    }
                    if (blend_mode ==
                        CltBlendMode::coefficient_weighted_mix) {
                        // Guest equation A = TEXCOLOR * a with B = SRCCOLOR * b
                        // combined by A_PLUS_B/A_PLUS_BCLAMP: the write is the
                        // sum of the texture-stage color scaled by the A
                        // coefficient pair and the bound destination pixel
                        // scaled by the B pair.  It is not an alpha lerp, so
                        // it cannot ride blend_px; alpha passes through, per
                        // the guest's `DESTCONSTSELECT = TEXALPHA`.
                        //
                        // When the draw's per-texel SSB selects between the
                        // two coefficient entries, each fragment reads the
                        // entry its own SSB bit names: entry 0 where the bit
                        // is 0, entry 1 where it is 1 (the toolkit's
                        // source-select-bit rule; a texture with no SSB passes
                        // 0).  The weights come from the resolved register
                        // pairs, not from a host lerp.
                        unsigned tw_r = f2u8(blend.weight_tex_r);
                        unsigned tw_g = f2u8(blend.weight_tex_g);
                        unsigned tw_b = f2u8(blend.weight_tex_b);
                        unsigned sw_r = f2u8(blend.weight_src_r);
                        unsigned sw_g = f2u8(blend.weight_src_g);
                        unsigned sw_b = f2u8(blend.weight_src_b);
                        if (ssb_entry_split && ssb_mask &&
                            (sample_nearest_pixels(
                                 ssb_mask->rgba, ssb_mask->w, ssb_mask->h,
                                 ssb_u, ssb_v, sampling.wrap_u, sampling.wrap_v,
                                 static_cast<int>(sampling.u_period),
                                 static_cast<int>(sampling.v_period)) & 0xffu) != 0u) {
                            tw_r = f2u8(blend.entry1_weight_tex_r);
                            tw_g = f2u8(blend.entry1_weight_tex_g);
                            tw_b = f2u8(blend.entry1_weight_tex_b);
                            sw_r = f2u8(blend.entry1_weight_src_r);
                            sw_g = f2u8(blend.entry1_weight_src_g);
                            sw_b = f2u8(blend.entry1_weight_src_b);
                        }
                        weighted_mix_px(x, y, sr, sg, sb, sa, tw_r, tw_g,
                                        tw_b, sw_r, sw_g, sw_b);
                        if (livery_body_entry || livery_nose_entry)
                            ++livery_color_pixels;
                        continue;
                    }
                    if (blend_mode ==
                        CltBlendMode::inverse_texture_multiply) {
                        // Guest B = SRCCOLOR * (1 - TEXCOLOR) with the ALU `B`
                        // operation: the destination pixel (the bound source
                        // frame buffer) is multiplied by the inverted
                        // texture-blender color, alpha included, so the result
                        // matches the GL (ZERO, ONE_MINUS_SRC_COLOR) draw.
                        invert_multiply_px(x, y, 255u - stage_r,
                                           255u - stage_g, 255u - stage_b,
                                           255u - sa);
                        if (livery_body_entry || livery_nose_entry)
                            ++livery_color_pixels;
                        continue;
                    }
                    blend_px(x, y, sr | (sg << 8u) | (sb << 16u) |
                                           (sa << 24u));
                    if (livery_body_entry || livery_nose_entry)
                        ++livery_color_pixels;
                }
            }
        };

        const bool saved_blend_enabled = m_blend_enabled;
        const CltDither saved_dither = m_dither;
        m_dither = blend.dither;
        m_blend_enabled = blend_mode == CltBlendMode::source_over ||
                          blend_mode == CltBlendMode::constant_alpha_over ||
                          clt_blend_uses_secondary_source(blend_mode);
        if (independent_triangles) {
            for (uint32_t i = 0; i + 2u < count; i += 3u)
                draw_triangle(vertices[i], vertices[i + 1u], vertices[i + 2u]);
        } else {
            for (uint32_t i = 2; i < count; ++i) {
                if (strip) draw_triangle(vertices[i - 2], vertices[i - 1],
                                         vertices[i]);
                else draw_triangle(vertices[0], vertices[i - 1], vertices[i]);
            }
        }
        m_blend_enabled = saved_blend_enabled;
        m_dither = saved_dither;

        // Screen-space facing census (M2_CLT_FACING_CENSUS=1, default off):
        // the Triangle Engine culls back faces by default and the guest
        // Mercury draw paths cull in software, so a draw whose triangles carry
        // both signed-area signs did not cull and its back faces reach the
        // host rasterizer.  Observation only; the pixel path is unchanged.
        static const bool facing_census = [] {
            const char* v = std::getenv("M2_CLT_FACING_CENSUS");
            return v && *v != '\0' && *v != '0';
        }();
        // M2_CLT_FACING_FRAME=<host frame> suppresses the census before a
        // settled frame so the entry cap is spent on the screen under study.
        static const uint32_t facing_from = [] {
            const char* v = std::getenv("M2_CLT_FACING_FRAME");
            return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0))
                           : 0u;
        }();
        // Narrow to opaque textured geometry (the car skin and the room's
        // depth-writing surfaces): transparent overlay draws carry no depth
        // and cannot hide one another.
        if (facing_census && m_frame >= facing_from &&
            raster_triangles >= 1u && count >= 3u) {
            static uint32_t facing_n = 0;
            static bool facing_expired = false;
            if (facing_n < 6000u) {
                float fmin_x = vertices[0].x, fmax_x = vertices[0].x;
                float fmin_y = vertices[0].y, fmax_y = vertices[0].y;
                for (uint32_t i = 1; i < count; ++i) {
                    fmin_x = std::min(fmin_x, vertices[i].x);
                    fmax_x = std::max(fmax_x, vertices[i].x);
                    fmin_y = std::min(fmin_y, vertices[i].y);
                    fmax_y = std::max(fmax_y, vertices[i].y);
                }
                std::fprintf(
                    stderr,
                    "[facing] n=%u frame=%u seq=%u cel=%d wh=%dx%d tex=%d "
                    "tris=%u pos=%u neg=%u depth=%d,%d blend=%d "
                    "bbox=(%.1f,%.1f)-(%.1f,%.1f) nverts=%u\n",
                    facing_n++, m_frame, draw_sequence,
                    static_cast<int>(texture), tex ? tex->w : 0,
                    tex ? tex->h : 0, textured ? 1 : 0, raster_triangles,
                    raster_tri_pos, raster_tri_neg,
                    depth.test_enabled ? 1 : 0, depth.write_enabled ? 1 : 0,
                    blend_enabled ? 1 : 0, fmin_x, fmin_y, fmax_x, fmax_y,
                    count);
            } else if (!facing_expired) {
                facing_expired = true;
                std::fprintf(stderr,
                             "[facing] cap reached at frame %u; further "
                             "entries suppressed\n",
                             m_frame);
            }
        }

        if (draw_diag && m_frame >= draw_diag_from) {
            float raster_min_x = vertices[0].x;
            float raster_max_x = vertices[0].x;
            float raster_min_y = vertices[0].y;
            float raster_max_y = vertices[0].y;
            for (uint32_t i = 1; i < count; ++i) {
                raster_min_x = std::min(raster_min_x, vertices[i].x);
                raster_max_x = std::max(raster_max_x, vertices[i].x);
                raster_min_y = std::min(raster_min_y, vertices[i].y);
                raster_max_y = std::max(raster_max_y, vertices[i].y);
            }
            const bool substantial = raster_max_x - raster_min_x >= 40.f &&
                                     raster_max_y - raster_min_y >= 20.f;
            static uint32_t raster_diag_n = 0;
            static bool raster_diag_expired = false;
            if (substantial && raster_diag_n < 128u) {
                std::fprintf(
                    stderr,
                    "[clt-raster] n=%u frame=%u seq=%u textured=%d "
                    "cel=%d wh=%dx%d bbox=(%.1f,%.1f)-(%.1f,%.1f) "
                    "tris=%u candidates=%llu depth_pass=%llu "
                    "depth_write=%llu ssb_drop=%llu alpha_drop=%llu "
                    "rgb_drop=%llu zclip_drop=%llu "
                    "color=%llu depth=%d,%d func=%#x "
                    "uv=[%.4g,%.4g]x[%.4g,%.4g] "
                    "sample=(%.4g,%.4g) texrgba=%#x lod=%.3g at=(%d,%d) "
                    "filter=%u/%u/%u lodmax=%u wrap=%d,%d uvmask=%#x\n",
                    raster_diag_n++, m_frame, draw_sequence,
                    textured ? 1 : 0, static_cast<int>(texture),
                    tex ? tex->w : 0, tex ? tex->h : 0, raster_min_x,
                    raster_min_y, raster_max_x, raster_max_y,
                    raster_triangles,
                    static_cast<unsigned long long>(raster_candidate_pixels),
                    static_cast<unsigned long long>(raster_depth_pass_pixels),
                    static_cast<unsigned long long>(raster_depth_write_pixels),
                    static_cast<unsigned long long>(raster_ssb_discard_pixels),
                    static_cast<unsigned long long>(raster_alpha_discard_pixels),
                    static_cast<unsigned long long>(raster_rgb_discard_pixels),
                    static_cast<unsigned long long>(
                        raster_zclip_discard_pixels),
                    static_cast<unsigned long long>(raster_color_pixels),
                    depth.test_enabled ? 1 : 0, depth.write_enabled ? 1 : 0,
                    depth.depth_func, raster_uv_min_u, raster_uv_max_u,
                    raster_uv_min_v, raster_uv_max_v, raster_sample_u,
                    raster_sample_v, raster_sample_rgba, raster_sample_lod,
                    raster_sample_x, raster_sample_y,
                    static_cast<unsigned>(sampling.min_filter),
                    static_cast<unsigned>(sampling.inter_filter),
                    static_cast<unsigned>(sampling.mag_filter),
                    static_cast<unsigned>(sampling.lod_max),
                    sampling.wrap_u ? 1 : 0, sampling.wrap_v ? 1 : 0,
                    sampling.raw_uv_mask);
            } else if (substantial && !raster_diag_expired) {
                raster_diag_expired = true;
                std::fprintf(stderr,
                             "[clt-raster] cap reached at frame %u; "
                             "further entries suppressed\n",
                             m_frame);
            }
        }

        if (livery_diag_entry) {
            static uint32_t livery_diag_n = 0;
            static bool livery_diag_expired = false;
            if (livery_diag_n < 512u) {
                const char* kind = livery_nose_entry
                                       ? "nose"
                                       : livery_body_entry ? "body" : "cover";
                std::fprintf(
                    stderr,
                    "[livery-backend] n=%u frame=%u kind=%s textured=%d "
                    "cel=%d wh=%dx%d blend=%d depth=%d,%d func=%#x "
                    "bbox=(%.1f,%.1f)-(%.1f,%.1f) ow=(%.5g,%.5g,%.5g) "
                    "depth_pass=%u depth_fail=%u color_pixels=%u\n",
                    livery_diag_n++, m_frame, kind, textured ? 1 : 0,
                    static_cast<int>(texture), tex ? tex->w : 0,
                    tex ? tex->h : 0, blend_enabled ? 1 : 0,
                    depth.test_enabled ? 1 : 0, depth.write_enabled ? 1 : 0,
                    depth.depth_func, livery_min_x, livery_min_y,
                    livery_max_x, livery_max_y, vertices[0].one_over_w,
                    vertices[1].one_over_w, vertices[2].one_over_w,
                    livery_depth_pass, livery_depth_fail,
                    livery_color_pixels);
                if ((livery_body_entry || livery_nose_entry) &&
                    livery_depth_fail != 0u) {
                    std::fprintf(stderr,
                                 "[livery-backend] depth-range pz=(%.7g,%.7g) "
                                 "db=(%.7g,%.7g) first-fail=(%.7g,%.7g) "
                                 "at=(%d,%d) owner=(seq=%u cel=%d tex=%d "
                                 "z=%.7g ow=%.7g func=%#x)\n",
                                 livery_pz_min, livery_pz_max, livery_db_min,
                                 livery_db_max, livery_first_fail_pz,
                                 livery_first_fail_db, livery_first_fail_x,
                                 livery_first_fail_y,
                                 livery_first_owner.draw_sequence,
                                 static_cast<int>(livery_first_owner.texture),
                                 livery_first_owner.textured ? 1 : 0,
                                 livery_first_owner.z,
                                 livery_first_owner.one_over_w,
                                 livery_first_owner.depth_func);
                    std::fprintf(stderr,
                                 "[livery-backend] depth-range owner-tri "
                                 "wh=%dx%d neg=%d bbox=(%.1f,%.1f)-(%.1f,%.1f) "
                                 "drawpos=%u drawneg=%u\n",
                                 livery_first_owner.tex_w,
                                 livery_first_owner.tex_h,
                                 livery_first_owner.neg_area ? 1 : 0,
                                 livery_first_owner.bx0, livery_first_owner.by0,
                                 livery_first_owner.bx1, livery_first_owner.by1,
                                 raster_tri_pos, raster_tri_neg);
                }
            } else if (!livery_diag_expired) {
                livery_diag_expired = true;
                std::fprintf(stderr,
                             "[livery-backend] cap reached at frame %u; "
                             "further entries suppressed\n",
                             m_frame);
            }
        }
    }

    void get_size(int* w, int* h) override
    {
        if (w) *w = m_w;
        if (h) *h = m_h;
    }

    void frame_begin(float clear_r, float clear_g, float clear_b) override
    {
        const uint32_t c = f2u8(clear_r) | (f2u8(clear_g) << 8) |
                           (f2u8(clear_b) << 16) | 0xFF000000u;
        m_clear_color = c;
        const CltFrameClearPlan clear_plan =
            clt_frame_clear_plan(persist_target_enabled(),
                                 m_persist_initialized);
        if (clear_plan.color) {
            for (auto& px : m_fb) px = c;
            m_metadata_targets[m_target_base].assign(m_fb.size(),0xff00ff00u);
        }
        if (clear_plan.depth) {
            for (auto& z : m_db) z = depth_clear_value();
            for (auto& owner : m_db_owner) owner = {};
            if (depth_probe_mode() == DepthProbeMode::packed_w16) {
                const uint16_t clear_cell =
                    m2_w16_packed(depth_clear_value(), 0u);
                if (m_db_packed.size() != m_db.size())
                    m_db_packed.assign(m_db.size(), clear_cell);
                else
                    for (auto& cell : m_db_packed) cell = clear_cell;
            }
        }
        m_persist_initialized = true;
    }

    bool grab_presented_rgba(uint32_t* dst, int w, int h) override
    {
        if (copy_bitmap_scanout(dst, w, h)) {
            if (source_diag_hits(m_frame))
                diag_source("grab", "scanout",
                            std::vector<uint32_t>(dst, dst + size_t(w) * h),
                            w, h);
            return true;
        }
        if (!dst || w != m_w || h != m_h) return false;
        const std::vector<uint32_t>* source = &m_fb;
        if (m_scanout_base != 0 && m_scanout_base != m_target_base) {
            const auto found = m_color_targets.find(m_scanout_base);
            if (found == m_color_targets.end()) return false;
            source = &found->second;
        }
        std::copy(source->begin(), source->end(), dst);
        const uint32_t base = m_scanout_base ? m_scanout_base : m_target_base;
        const auto rect = m_scanout_rects.find(base);
        if (rect != m_scanout_rects.end()) rect->second.apply(dst, w, h);
        diag_source("grab", "m_fb", *source, m_w, m_h);
        return true;
    }

    void clt_scanout_select(uint32_t guest_base) override
    {
        m_scanout_base = guest_base;
    }
    void clt_target_scanout_rect(uint32_t base, const CltScanoutRect& rect) override
    {
        m_scanout_rects[base] = rect;
    }

    void clt_target_persistence(bool enabled) override
    {
        m_persist_target = enabled;
    }

    void clt_target_upload_rgba(uint32_t guest_base, const uint32_t* rgba,
                                int w, int h) override
    {
        if (!guest_base || !rgba || w != m_w || h != m_h) return;
        std::vector<uint32_t> pixels(rgba, rgba + static_cast<size_t>(w) * h);
        auto& metadata = m_metadata_targets[guest_base];
        metadata.resize(pixels.size());
        for (size_t i=0;i<pixels.size();++i) metadata[i] = 0xff000000u | ((pixels[i] >> 16u) & 0xff00u);
        if (guest_base == m_target_base)
            m_fb = std::move(pixels);
        else
            m_color_targets[guest_base] = std::move(pixels);
    }

    void clt_target_copy(uint32_t source_base, uint32_t dest_base) override
    {
        const std::vector<uint32_t>* source = nullptr;
        if (source_base == m_target_base)
            source = &m_fb;
        else {
            const auto found = m_color_targets.find(source_base);
            if (found != m_color_targets.end()) source = &found->second;
        }
        if (!source) return;
        m_metadata_targets[dest_base] = m_metadata_targets[source_base];
        if (dest_base == m_target_base)
            m_fb = *source;
        else
            m_color_targets[dest_base] = *source;
    }

    void clt_target_select(uint32_t guest_base) override
    {
        if (guest_base == 0 || guest_base == m_target_base) return;
        if (source_diag_hits(m_frame)) {
            std::fprintf(stderr,
                         "[cap-target] frame=%u base=%#x prev=%#x stored=%d "
                         "persist=%d\n",
                         m_frame, guest_base, m_target_base,
                         m_color_targets.count(guest_base) != 0 ? 1 : 0,
                         persist_target_enabled() ? 1 : 0);
        }
        if (m_target_base != 0) m_color_targets[m_target_base] = m_fb;
        const auto found = m_color_targets.find(guest_base);
        if (found != m_color_targets.end()) {
            m_fb = found->second;
        } else {
            m_fb.assign(static_cast<size_t>(m_w) * m_h, m_clear_color);
        }
        auto& metadata = m_metadata_targets[guest_base];
        if (metadata.size() != m_fb.size()) metadata.assign(m_fb.size(), 0xff00ff00u);
        m_target_base = guest_base;
    }

    bool grab_clt_target_rgba(uint32_t guest_base, uint32_t* dst, int w,
                              int h) override
    {
        if (!dst || guest_base == 0 || w != m_w || h != m_h) return false;
        if (guest_base == m_target_base) {
            std::copy(m_fb.begin(), m_fb.end(), dst);
            return true;
        }
        const auto found = m_color_targets.find(guest_base);
        if (found == m_color_targets.end()) return false;
        std::copy(found->second.begin(), found->second.end(), dst);
        return true;
    }

    bool grab_clt_target_window_rgba(uint32_t guest_base, uint32_t* dst,
                                     int x, int y, int w, int h) override
    {
        if (!dst || guest_base == 0 || x < 0 || y < 0 || w <= 0 || h <= 0 ||
            x + w > m_w || y + h > m_h)
            return false;
        const std::vector<uint32_t>* source = nullptr;
        if (guest_base == m_target_base)
            source = &m_fb;
        else {
            const auto found = m_color_targets.find(guest_base);
            if (found == m_color_targets.end()) return false;
            source = &found->second;
        }
        for (int row = 0; row < h; ++row)
            std::copy_n(source->data() + static_cast<size_t>(y + row) * m_w + x,
                        w, dst + static_cast<size_t>(row) * w);
        return true;
    }

    bool grab_clt_target_window_metadata(uint32_t base, uint32_t* dst, int x, int y, int w, int h) override {
        const auto found = m_metadata_targets.find(base);
        if (!dst || found == m_metadata_targets.end() || x<0 || y<0 || w<=0 || h<=0 || x+w>m_w || y+h>m_h)
            return false;
        for (int row=0;row<h;++row)
            std::copy_n(found->second.data()+size_t(y+row)*m_w+x,w,dst+size_t(row)*w);
        return true;
    }
    CelId clt_snapshot_target_cel(uint32_t base, int x, int y, int w, int h, int cw, int ch, bool preserve_alpha = false) override {
        // Keep the reference RGB555/MSBREP CPU materializer for color-only
        // reads. Metadata consumers require the retained full RGBA plane.
        if (!preserve_alpha) return kInvalidCel;
        return snapshot_target(base, x, y, w, h, cw, ch, false);
    }
    CelId clt_snapshot_target_ssb_cel(uint32_t base, int x, int y, int w, int h, int cw, int ch) override {
        return snapshot_target(base, x, y, w, h, cw, ch, true);
    }
    CelId snapshot_target(uint32_t base, int x, int y, int w, int h, int cw, int ch, bool metadata) {
        if (cw <= 0 || ch <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x+w > m_w || y+h > m_h)
            return kInvalidCel;
        std::vector<uint32_t> window(size_t(w)*h), pixels(size_t(cw)*ch);
        if (metadata) {
            const auto found = m_metadata_targets.find(base);
            if (found == m_metadata_targets.end()) return kInvalidCel;
            for (int row=0; row<h; ++row)
                std::copy_n(found->second.data()+size_t(y+row)*m_w+x, w, window.data()+size_t(row)*w);
        } else if (!grab_clt_target_window_rgba(base, window.data(), x,y,w,h)) return kInvalidCel;
        for (int row=0; row<ch; ++row)
            for (int col=0; col<cw; ++col)
                pixels[size_t(row)*cw+col] = window[size_t((row*2+1)*h/(ch*2))*w+(col*2+1)*w/(cw*2)];
        return cel_upload_rgba(cw,ch,pixels.data());
    }

    void frame_present() override
    {
        if (source_diag_hits(m_frame)) {
            std::vector<uint32_t> presented(static_cast<size_t>(m_w) * m_h);
            const bool direct = copy_bitmap_scanout(presented.data(), m_w, m_h);
            if (!direct) presented = m_fb;
            diag_source("present", direct ? "scanout" : "m_fb", presented, m_w,
                        m_h);
        }
        // Draw-order census: with M2_DRAW_ORDER_FRAME set, one line per presented
        // host frame with its clt_draw count, so frames carrying the world
        // lists stand out before targeting one with the full dump.
        if (order_census_enabled()) {
            std::fprintf(stderr, "[draw-order] frame=%u draws=%u\n", m_frame,
                         m_order_count);
            m_order_count = 0;
        }
        const uint32_t idx = m_frame++;
        if (m_dir.empty() || idx < m_from) return;
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        if (ec) {
            note_output_error(m_dir, "create directory", ec);
            return;
        }

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04u.ppm", idx);
        const std::filesystem::path path = m_dir / name;

        // The capture destination comes from a UTF-8 CLI path; use the
        // platform opener so Windows does not reinterpret it in the ANSI code
        // page.
        errno = 0;
        FILE* f = m2host::platform::fopen(path, "wb");
        if (!f) {
            const int err = errno;
            note_output_error(path, "open",
                              err ? std::error_code(err, std::generic_category())
                                  : std::error_code());
            return;
        }
        bool ok = true;
        int write_errno = 0;
        if (std::fprintf(f, "P6\n%d %d\n255\n", m_w, m_h) < 0) {
            ok = false;
            write_errno = errno;
        }
        std::vector<uint32_t> scanout(size_t(m_w) * m_h);
        const bool direct = copy_bitmap_scanout(scanout.data(), m_w, m_h);
        std::vector<uint8_t> row(static_cast<size_t>(m_w) * 3u);
        for (int y = 0; ok && y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const size_t offset = static_cast<size_t>(y) * m_w + x;
                const uint32_t px = direct ? scanout[offset] : m_fb[offset];
                row[static_cast<size_t>(x) * 3u + 0u] =
                    static_cast<uint8_t>(px & 0xff);
                row[static_cast<size_t>(x) * 3u + 1u] =
                    static_cast<uint8_t>((px >> 8) & 0xff);
                row[static_cast<size_t>(x) * 3u + 2u] =
                    static_cast<uint8_t>((px >> 16) & 0xff);
            }
            if (std::fwrite(row.data(), 1, row.size(), f) != row.size()) {
                ok = false;
                write_errno = errno;
            }
        }
        if (ok && std::fflush(f) != 0) {
            ok = false;
            write_errno = errno;
        }
        if (std::fclose(f) != 0 && ok) {
            ok = false;
            write_errno = errno;
        }
        if (!ok) {
            // A failed write leaves a truncated PPM that looks like a valid
            // capture to anything comparing frame directories.
            std::error_code remove_ec;
            std::filesystem::remove(path, remove_ec);
            note_output_error(
                path, "write",
                write_errno ? std::error_code(write_errno,
                                              std::generic_category())
                            : std::error_code());
        }
    }

    const char* name() const override { return "soft_capture"; }

private:
    void note_output_error(const std::filesystem::path& path, const char* stage,
                           const std::error_code& ec)
    {
        if (!m_output_error.empty()) return;  // retain the first failure
        std::string detail = std::string("[capture] ") + stage +
                             " failed for " +
                             m2host::platform::path_to_utf8(path);
        if (ec) detail += ": " + ec.message();
        m_output_error = detail;
        std::fprintf(stderr, "%s\n", m_output_error.c_str());
    }

    struct Mip {
        int w = 0, h = 0;
        std::vector<uint32_t> rgba;
    };

    struct Cel {
        int w = 0, h = 0;
        std::vector<uint32_t> rgba;
        std::vector<Mip> mips;
        bool default_wrap = false;
    };

    // Host-side 2x2 box reduction of the level at (w, h) down to 1x1,
    // appending to the levels `c` already holds. (w, h) describe level 0 when
    // the chain is empty and the last resident level otherwise, so a chain the
    // guest authored is only extended, never replaced.
    static void generate_mip_tail(Cel& c, int w, int h, size_t resident_levels)
    {
        int level_w = w;
        int level_h = h;
        std::vector<uint32_t> source =
            resident_levels == 0 ? c.rgba : c.mips.back().rgba;
        while (level_w > 1 || level_h > 1) {
            const int next_w = std::max(1, level_w / 2);
            const int next_h = std::max(1, level_h / 2);
            std::vector<uint32_t> next(static_cast<size_t>(next_w) * next_h);
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
                    uint32_t out = 0;
                    for (unsigned shift = 0; shift < 32; shift += 8) {
                        const unsigned sum = ((p00 >> shift) & 0xffu) +
                                             ((p10 >> shift) & 0xffu) +
                                             ((p01 >> shift) & 0xffu) +
                                             ((p11 >> shift) & 0xffu);
                        out |= ((sum + 2u) / 4u) << shift;
                    }
                    next[static_cast<size_t>(y) * next_w + x] = out;
                }
            }
            c.mips.push_back({next_w, next_h, next});
            source = std::move(next);
            level_w = next_w;
            level_h = next_h;
        }
    }

    static uint8_t f2u8(float v)
    {
        if (v <= 0.f) return 0;
        if (v >= 1.f) return 255;
        return static_cast<uint8_t>(v * 255.f + 0.5f);
    }

    // Guest TXTUVMASK address transform.  The hardware masks the integer
    // texel coordinate with the register's per-axis bits, so a masked period
    // repeats that sub-range of the texture; a period at or above the resident
    // extent is a no-op on the coordinate and the axis clamps to its extent.
    // `period` is in base-LOD texels (0 or >= size means "no mask").
    static int address_texel(int value, int size, bool repeat, int period = 0)
    {
        if (repeat) {
            const int modulus =
                (period > 0 && period < size) ? period : size;
            value %= modulus;
            if (value < 0) value += modulus;
            return value;
        }
        return std::clamp(value, 0, size - 1);
    }

    static uint32_t sample_nearest_pixels(const std::vector<uint32_t>& pixels,
                                          int w, int h, float u, float v,
                                          bool wrap_u, bool wrap_v,
                                          int period_u = 0, int period_v = 0)
    {
        if (w <= 0 || h <= 0 || pixels.empty()) return 0u;
        if (wrap_u) u -= std::floor(u);
        else u = std::clamp(u, 0.f, 1.f);
        if (wrap_v) v -= std::floor(v);
        else v = std::clamp(v, 0.f, 1.f);
        const int x = address_texel(static_cast<int>(u * w), w, wrap_u,
                                    period_u);
        const int y = address_texel(static_cast<int>(v * h), h, wrap_v,
                                    period_v);
        return pixels[static_cast<size_t>(y) * w + x];
    }

    static uint32_t sample_bilinear_pixels(const std::vector<uint32_t>& pixels,
                                            int w, int h, float u, float v,
                                            bool wrap_u, bool wrap_v,
                                            int period_u = 0,
                                            int period_v = 0)
    {
        if (w <= 0 || h <= 0 || pixels.empty()) return 0u;
        if (wrap_u) u -= std::floor(u);
        else u = std::clamp(u, 0.f, 1.f);
        if (wrap_v) v -= std::floor(v);
        else v = std::clamp(v, 0.f, 1.f);
        const float fx = u * w - 0.5f;
        const float fy = v * h - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - std::floor(fx);
        const float ty = fy - std::floor(fy);
        const auto at = [&](int x, int y) {
            const int ax = address_texel(x, w, wrap_u, period_u);
            const int ay = address_texel(y, h, wrap_v, period_v);
            return pixels[static_cast<size_t>(ay) * w + ax];
        };
        const uint32_t p00 = at(x0, y0);
        const uint32_t p10 = at(x0 + 1, y0);
        const uint32_t p01 = at(x0, y0 + 1);
        const uint32_t p11 = at(x0 + 1, y0 + 1);
        uint32_t out = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const float top = (1.f - tx) * ((p00 >> shift) & 0xffu) +
                              tx * ((p10 >> shift) & 0xffu);
            const float bottom =
                (1.f - tx) * ((p01 >> shift) & 0xffu) +
                tx * ((p11 >> shift) & 0xffu);
            out |= static_cast<uint32_t>(
                       std::clamp((1.f - ty) * top + ty * bottom, 0.f,
                                  255.f) +
                       0.5f)
                   << shift;
        }
        return out;
    }

    static uint32_t blend_rgba(uint32_t a, uint32_t b, float t)
    {
        uint32_t out = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const float value = (1.f - t) * ((a >> shift) & 0xffu) +
                                t * ((b >> shift) & 0xffu);
            out |= static_cast<uint32_t>(std::clamp(value, 0.f, 255.f) +
                                          0.5f)
                   << shift;
        }
        return out;
    }

    uint32_t sample_texture(const Cel& c, float u, float v,
                            const CltTextureSampling& sampling,
                            float lod) const
    {
        const bool minified = lod > 0.f;
        const CltTextureFilter filter =
            minified ? sampling.min_filter : sampling.mag_filter;
        const bool bilinear = filter != CltTextureFilter::point;
        const bool repeat_u = sampling.wrap_u;
        const bool repeat_v = sampling.wrap_v;
        // The mask period is authored in base-LOD texels (the guest's UV space
        // is the largest LOD's, per the toolbox `UVScale`), so it is carried
        // unchanged into mip levels here.
        const int period_u = static_cast<int>(sampling.u_period);
        const int period_v = static_cast<int>(sampling.v_period);
        const auto sample_level = [&](const std::vector<uint32_t>& pixels,
                                      int w, int h) {
            return bilinear
                       ? sample_bilinear_pixels(pixels, w, h, u, v, repeat_u,
                                                repeat_v, period_u, period_v)
                       : sample_nearest_pixels(pixels, w, h, u, v, repeat_u,
                                               repeat_v, period_u, period_v);
        };
        if (!minified || c.mips.empty() || sampling.lod_max == 0u)
            return sample_level(c.rgba, c.w, c.h);

        const int max_level = std::min<int>(
            static_cast<int>(c.mips.size()), sampling.lod_max);
        const float clamped_lod = std::clamp(lod, 0.f,
                                             static_cast<float>(max_level));
        if (sampling.inter_filter == CltTextureFilter::point) {
            const int level = std::clamp(
                static_cast<int>(std::floor(clamped_lod + 0.5f)), 0,
                max_level);
            if (level == 0) return sample_level(c.rgba, c.w, c.h);
            const Mip& mip = c.mips[static_cast<size_t>(level - 1)];
            return sample_level(mip.rgba, mip.w, mip.h);
        }

        const int lower = std::clamp(static_cast<int>(std::floor(clamped_lod)),
                                     0, max_level);
        const int upper = std::min(max_level, lower + 1);
        const auto sample_at = [&](int level) {
            if (level == 0) return sample_level(c.rgba, c.w, c.h);
            const Mip& mip = c.mips[static_cast<size_t>(level - 1)];
            return sample_level(mip.rgba, mip.w, mip.h);
        };
        return blend_rgba(sample_at(lower), sample_at(upper),
                          clamped_lod - static_cast<float>(lower));
    }

    // nearest-neighbor, clamped: classic cel_blit and SSB masks retain their
    // pixel-art behavior unless a guest CLT draw supplies lookup state.
    uint32_t sample(const Cel& c, float u, float v) const
    {
        return sample_nearest_pixels(c.rgba, c.w, c.h, u, v,
                                     c.default_wrap, c.default_wrap);
    }

    // Final framebuffer store. Dithering (when armed) follows blending, as
    // the Triangle Engine's output stage does.
    void store_px(uint32_t* px, int x, int y, uint32_t value) const
    {
        *px = m_dither.enabled ? clt_dither_pixel(m_dither, value, x, y)
                               : value;
    }

    uint32_t* writable_px(int x, int y)
    {
        if (x < 0 || y < 0 || x >= m_w || y >= m_h) return nullptr;
        if (m_sw >= 0 &&
            (x < m_sx || y < m_sy || x >= m_sx + m_sw || y >= m_sy + m_sh))
            return nullptr;
        return &m_fb[static_cast<size_t>(y) * m_w + x];
    }

    // Destination-blender multiply form: dst = dst * factor / 255 per channel.
    // `factor_*` is the inverted texture-blender color (and inverted alpha).
    // This is not expressible as the alpha lerp in blend_px, so it writes the
    // destination directly.
    void invert_multiply_px(int x, int y, unsigned factor_r, unsigned factor_g,
                            unsigned factor_b, unsigned factor_a)
    {
        uint32_t* px = writable_px(x, y);
        if (!px) return;
        const uint32_t dst = *px;
        const unsigned r = ((dst >> 0u & 0xffu) * factor_r + 127u) / 255u;
        const unsigned g = ((dst >> 8u & 0xffu) * factor_g + 127u) / 255u;
        const unsigned b = ((dst >> 16u & 0xffu) * factor_b + 127u) / 255u;
        const unsigned a = ((dst >> 24u & 0xffu) * factor_a + 127u) / 255u;
        store_px(px, x, y, r | (g << 8u) | (b << 16u) | (a << 24u));
    }

    // Toolkit destination-blender two-input weighted mix
    // (`Blt_BlendBlitObject` / `defaultDBlend`): the write is
    // `TEXCOLOR * a + SRCCOLOR * b` with one per-channel weight from each
    // multiplier coefficient register pair.  This is a sum of two scaled
    // inputs, so it is neither the alpha lerp in blend_px nor a pure
    // destination scaling, and it never treats the fragment alpha as a blend
    // factor.  Alpha follows the guest's destination-alpha source selection
    // (`DESTALPHACNTL`), which these setups set to the texture-stage alpha.
    void weighted_mix_px(int x, int y, unsigned src_r, unsigned src_g,
                         unsigned src_b, unsigned src_a, unsigned tex_r,
                         unsigned tex_g, unsigned tex_b, unsigned srcw_r,
                         unsigned srcw_g, unsigned srcw_b)
    {
        uint32_t* px = writable_px(x, y);
        if (!px) return;
        const uint32_t dst = *px;
        const auto mix = [](unsigned frag, unsigned wfrag, unsigned dest,
                            unsigned wdest) -> unsigned {
            const unsigned v = frag * wfrag + dest * wdest;
            return v > 255u * 255u ? 255u : (v + 127u) / 255u;
        };
        store_px(px, x, y,
                 mix(src_r, tex_r, dst >> 0u & 0xffu, srcw_r) |
                     (mix(src_g, tex_g, dst >> 8u & 0xffu, srcw_g) << 8u) |
                     (mix(src_b, tex_b, dst >> 16u & 0xffu, srcw_b) << 16u) |
                     (src_a << 24u));
    }

    // Destination-blender identity-coefficient form: the fragment replaces the
    // destination pixel outright.  The recovered equation never reads the
    // destination, so this is not expressible as the alpha lerp in blend_px
    // and must not inherit the source cel's opacity.
    void copy_px(int x, int y, uint32_t src)
    {
        uint32_t* px = writable_px(x, y);
        if (!px) return;
        store_px(px, x, y, src);
    }

    void blend_px(int x, int y, uint32_t src)
    {
        uint32_t* target = writable_px(x, y);
        if (!target) return;
        uint32_t& dst = *target;
        const unsigned a = (src >> 24) & 0xff;
        if (!m_blend_enabled) {
            store_px(&dst, x, y, src);
            return;
        }
        if (a == 0) return;
        if (a == 255) {
            store_px(&dst, x, y, src | 0xFF000000u);
            return;
        }
        const unsigned ia = 255 - a;
        const unsigned r = ((src >> 0 & 0xff) * a + (dst >> 0 & 0xff) * ia + 127) / 255;
        const unsigned g = ((src >> 8 & 0xff) * a + (dst >> 8 & 0xff) * ia + 127) / 255;
        const unsigned b = ((src >> 16 & 0xff) * a + (dst >> 16 & 0xff) * ia + 127) / 255;
        store_px(&dst, x, y, r | (g << 8) | (b << 16) | 0xFF000000u);
    }

    int m_w, m_h;
    std::filesystem::path m_dir;
    // First failed frame output, retained for the host's explicit --capture
    // status. Empty in the normal case and in screenshot-only (no m_dir) use.
    std::string m_output_error;
    std::vector<uint32_t> m_fb;
    std::map<uint32_t, std::vector<uint32_t>> m_color_targets;
    std::map<uint32_t, std::vector<uint32_t>> m_metadata_targets;
    std::map<uint32_t, CltScanoutRect> m_scanout_rects;
    std::vector<float> m_db;
    // PACKED-diagnostic integer depth plane (diagnostic-only): mirrors m_db
    // cell-for-cell when M2_CLT_DEPTH_W16_PACKED is set so the destination
    // comparison can use m2_w16_packed_compare exactly. Empty in all other
    // modes so production rendering allocates nothing extra.
    std::vector<uint16_t> m_db_packed;
    std::vector<DbOwner> m_db_owner;
    std::map<CelId, Cel> m_cels;
    CelId m_next = 0;
    uint32_t m_frame = 0;
    uint32_t m_draw_sequence = 0;
    uint32_t m_from = 0;
    bool m_blend_enabled = true;
    // Opt-in RGB555 dither for the draw in flight (disabled by default).
    CltDither m_dither{};
    bool m_persist_initialized = false;
    bool m_persist_target = false;
    uint32_t m_target_base = 0;
    uint32_t m_scanout_base = 0;
    uint32_t m_clear_color = 0xFF000000u;
    // Draw-order census counter, consumed by frame_present.
    uint32_t m_order_count = 0;
    static bool order_census_enabled()
    {
        static const bool enabled = [] {
            const char* v = std::getenv("M2_DRAW_ORDER_FRAME");
            return v && v[0] != '\0';
        }();
        return enabled;
    }
    bool persist_target_enabled() const
    {
        const char* value = std::getenv("M2_CLT_PERSIST_TARGET");
        return value ? value[0] != '\0' && value[0] != '0'
                     : m_persist_target;
    }

    // Bounded software-capture source diagnostic (M2_CAPTURE_SOURCE_DIAG,
    // default off).  The value is a host-frame spec: "all", a single index, or
    // comma-separated indices/ranges ("1985", "1985-1992", "100,1985-1992").
    // On those frames the backend logs one [cap-source] line per presentation
    // decision (which source served: the guest scanout bitmap or the currently
    // selected CLT colour target), the scanout/target state, and a compact
    // content signature for the served source, the selected target and every
    // stored sibling target; clt_target_select additionally logs one
    // [cap-target] switch line.  Observation only: the pixel path, target
    // state, and capture output are unchanged.
    static bool source_diag_hits(uint32_t frame)
    {
        static const std::vector<std::pair<uint32_t, uint32_t>> ranges = [] {
            std::vector<std::pair<uint32_t, uint32_t>> parsed;
            const char* value = std::getenv("M2_CAPTURE_SOURCE_DIAG");
            if (!value || value[0] == '\0') return parsed;
            const std::string spec(value);
            if (spec == "0") return parsed;
            if (spec == "all") {
                parsed.emplace_back(0u, UINT32_MAX);
                return parsed;
            }
            size_t pos = 0;
            while (pos < spec.size()) {
                const size_t comma = spec.find(',', pos);
                const size_t end =
                    comma == std::string::npos ? spec.size() : comma;
                const std::string item = spec.substr(pos, end - pos);
                pos = end + 1;
                const size_t dash = item.find('-');
                uint32_t lo = 0;
                uint32_t hi = 0;
                if (dash == std::string::npos) {
                    lo = hi = static_cast<uint32_t>(
                        std::strtoul(item.c_str(), nullptr, 0));
                } else {
                    lo = static_cast<uint32_t>(std::strtoul(
                        item.substr(0, dash).c_str(), nullptr, 0));
                    hi = static_cast<uint32_t>(std::strtoul(
                        item.substr(dash + 1).c_str(), nullptr, 0));
                }
                if (hi >= lo) parsed.emplace_back(lo, hi);
            }
            return parsed;
        }();
        for (const auto& range : ranges)
            if (frame >= range.first && frame <= range.second) return true;
        return false;
    }

    struct PlaneSig {
        uint32_t top = 0;
        double frac = 0.0;
        size_t colors = 0;
    };

    static PlaneSig plane_sig(const std::vector<uint32_t>& pixels)
    {
        PlaneSig sig;
        if (pixels.empty()) return sig;
        std::map<uint32_t, size_t> histogram;
        for (const uint32_t px : pixels) ++histogram[px];
        sig.colors = histogram.size();
        size_t best = 0;
        for (const auto& entry : histogram)
            if (entry.second > best) {
                best = entry.second;
                sig.top = entry.first;
            }
        sig.frac = static_cast<double>(best) /
                   static_cast<double>(pixels.size());
        return sig;
    }

    void diag_source(const char* call, const char* served,
                     const std::vector<uint32_t>& served_px, int sw,
                     int sh) const
    {
        if (!source_diag_hits(m_frame)) return;
        const PlaneSig s = plane_sig(served_px);
        const PlaneSig fb = plane_sig(m_fb);
        std::fprintf(stderr,
                     "[cap-source] call=%s frame=%u served=%s scanout=%d "
                     "scanout_wh=%dx%d target_base=%#x stored=%zu persist=%d "
                     "served_top=%#x served_frac=%.3f served_colors=%zu "
                     "fb_top=%#x fb_frac=%.3f fb_colors=%zu clear=%#x\n",
                     call, m_frame, served, has_bitmap_scanout() ? 1 : 0, sw,
                     sh, m_target_base, m_color_targets.size(),
                     persist_target_enabled() ? 1 : 0, s.top, s.frac,
                     s.colors, fb.top, fb.frac, fb.colors, m_clear_color);
        size_t shown = 0;
        for (const auto& entry : m_color_targets) {
            if (shown++ >= 8u) break;
            const PlaneSig t = plane_sig(entry.second);
            std::fprintf(stderr,
                         "[cap-source] frame=%u stored_base=%#x top=%#x "
                         "frac=%.3f colors=%zu\n",
                         m_frame, entry.first, t.top, t.frac, t.colors);
        }
    }
    // scissor box (disabled = full target)
    int m_sx = 0, m_sy = 0, m_sw = -1, m_sh = -1;
};

}  // namespace m2host
