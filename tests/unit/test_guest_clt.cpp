#include "guest_clt.h"
#include "soft_capture.h"
#include "te_vertex_state.h"

#include <atomic>
#include <bit>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "posix_compat.h"

// The standalone fixture links guest_clt.cpp, whose application-frame counter
// is a weak zero fallback (no live guest context). A strong definition here
// lets the boundary fixtures tag submissions with real guest frames, which is
// exactly what the look-ahead pump contract is about. It defaults to 0 so
// every other fixture keeps its untagged behavior.
extern "C" uint32_t m2_guest_test_current_frame = 0;

extern "C" uint32_t m2_guest_current_frame_c(void)
{
    return m2_guest_test_current_frame;
}

namespace {

// Scoped application-frame override for the pump-boundary fixtures.
struct GuestFrameOverride {
    explicit GuestFrameOverride(uint32_t frame)
    {
        m2_guest_test_current_frame = frame;
    }
    ~GuestFrameOverride() { m2_guest_test_current_frame = 0u; }
};

class RecordingBackend final : public m2host::ICelBlitterBackend {
public:
    struct Draw {
        std::vector<m2host::CltVertex> vertices;
        bool strip = false;
        bool independent_triangles = false;
        bool textured = false;
        bool blend_enabled = false;
        m2host::CltBlendState blend;
        m2host::CltDiscard discard;
        m2host::CltDepth depth;
        m2host::CltTextureSampling sampling;
        // The cel the interpreter resolved for this draw's TxtLODBase0. A
        // packed blast publishes several residents with the same span, so the
        // resolved cel -- not the sampling state -- is what identifies which
        // texture served the draw.
        m2host::CelId cel = 0;
        float target_w = 0.f;
        float target_h = 0.f;
    };

    struct Scissor {
        bool enabled = false;
        float x = 0.f;
        float y = 0.f;
        float w = 0.f;
        float h = 0.f;
    };

    // One level of a guest-authored MIP chain as the backend received it.
    struct LodLevel {
        int w = 0;
        int h = 0;
        std::vector<uint32_t> pixels;
    };

    bool init(SDL_Window*) override { return true; }
    void shutdown() override {}
    m2host::CelId cel_upload_rgba(int w, int h, const void* pixels) override
    {
        Upload upload;
        upload.w = w;
        upload.h = h;
        const auto* rgba = static_cast<const uint32_t*>(pixels);
        upload.pixels.assign(rgba, rgba + static_cast<size_t>(w) * h);
        uploads.push_back(std::move(upload));
        return static_cast<m2host::CelId>(uploads.size());
    }
    // Records the guest-authored MIP chain the interpreter handed over, so a
    // test can compare the supplied levels against the guest's own bytes.
    m2host::CelId cel_upload_rgba_ex(int w, int h, const void* pixels,
                                     bool mipmap_linear, bool wrap_repeat) override
    {
        const m2host::CelId id = cel_upload_rgba(w, h, pixels);
        if (id != m2host::kInvalidCel &&
            static_cast<size_t>(id) <= uploads.size())
            uploads[id - 1u].mipmap_linear = mipmap_linear;
        return id;
    }
    bool clt_supports_authored_lod_chain() const override { return true; }
    m2host::CelId cel_upload_rgba_lod_chain(int w, int h, const void* pixels,
                                            const m2host::CelLodLevel* levels,
                                            size_t level_count,
                                            bool mipmap_linear,
                                            bool wrap_repeat,
                                            uint64_t content_hash,
                                            uint64_t lod_chain_hash) override
    {
        Upload upload;
        upload.w = w;
        upload.h = h;
        const auto* rgba = static_cast<const uint32_t*>(pixels);
        upload.pixels.assign(rgba, rgba + static_cast<size_t>(w) * h);
        upload.mipmap_linear = mipmap_linear;
        upload.content_hash = content_hash;
        upload.lod_chain_hash = lod_chain_hash;
        for (size_t i = 0; i < level_count; ++i) {
            LodLevel level;
            level.w = levels[i].w;
            level.h = levels[i].h;
            if (levels[i].rgba) {
                const auto* p = levels[i].rgba;
                level.pixels.assign(
                    p, p + static_cast<size_t>(level.w) * level.h);
            }
            upload.lod_chain.push_back(std::move(level));
        }
        uploads.push_back(std::move(upload));
        return static_cast<m2host::CelId>(uploads.size());
    }
    void cel_free(m2host::CelId cel) override { freed_cels.push_back(cel); }
    void cel_blit(m2host::CelId, float, float, float, float, float, float,
                  float, float) override {}
    bool clt_supports_triangle_batches() const override { return true; }
    void clt_draw(const m2host::CltVertex* vertices, uint32_t count,
                  bool strip, bool textured, m2host::CelId cel,
                  float target_w, float target_h,
                  bool blend_enabled, m2host::CltDiscard discard,
                  m2host::CltDepth depth,
                  m2host::CltTextureSampling sampling,
                  m2host::CltBlendState blend,
                  bool independent_triangles = false) override
    {
        if (independent_triangles) ++triangle_batch_calls;
        const uint32_t stride = independent_triangles ? 3u : count;
        for (uint32_t first = 0; first < count; first += stride) {
            Draw draw;
            draw.vertices.assign(vertices + first,
                                 vertices + std::min(count, first + stride));
            draw.strip = strip;
            draw.independent_triangles = independent_triangles;
            draw.textured = textured;
            draw.blend_enabled = blend_enabled;
            draw.blend = blend;
            draw.discard = discard;
            draw.depth = depth;
            draw.sampling = sampling;
            draw.cel = cel;
            draw.target_w = target_w;
            draw.target_h = target_h;
            draws.push_back(std::move(draw));
        }
    }
    uint32_t triangle_batch_calls = 0;
    void scissor(float x, float y, float w, float h) override
    {
        scissors.push_back({true, x, y, w, h});
    }
    void scissor_disable() override { scissors.push_back({}); }
    void get_size(int* w, int* h) override
    {
        if (w) *w = host_width;
        if (h) *h = host_height;
    }
    void frame_begin(float, float, float) override {}
    // Model the backend's documented presentation contract: `present()` binds
    // the default drawable so SDL can swap, so no guest target is bound until
    // the next list selects one (see GlCltTargets::select's comment on this
    // exact interaction).
    void frame_present() override { bound_target = 0u; }
    void clt_target_select(uint32_t guest_base) override
    {
        target_selects.push_back(guest_base);
        if (guest_base != 0u) bound_target = guest_base;
    }
    // Guest-addressed target receiving subsequent CLT color writes, 0 = the
    // drawable. Recorded so a test can assert which target a submitted list
    // actually bound rather than which address it named.
    uint32_t bound_target = 0u;
    std::vector<uint32_t> target_selects;
    bool grab_clt_target_rgba(uint32_t base, uint32_t* dst, int w,
                              int h) override
    {
        if (!target_readback_enabled || !dst || w != host_width ||
            h != host_height)
            return false;
        uint32_t color = 0u;
        if (!lookup_target_color(base, &color)) return false;
        std::fill(dst, dst + static_cast<size_t>(w) * h, color);
        return true;
    }
    struct WindowRead {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool metadata = false;
    };
    bool grab_clt_target_window_rgba(uint32_t base, uint32_t* dst, int x,
                                     int y, int w, int h) override
    {
        if (!target_readback_enabled || !dst) return false;
        uint32_t color = 0u;
        if (!lookup_target_color(base, &color)) return false;
        window_reads.push_back({x, y, w, h, false});
        if (target_readback_rows) {
            // Row-keyed pattern so a strip copied out of the cached full
            // sheet proves it came from its own source rows.
            for (int row = 0; row < h; ++row) {
                const uint32_t g =
                    static_cast<uint32_t>((y + row) * 4) & 0xffu;
                std::fill(dst + static_cast<size_t>(row) * w,
                          dst + static_cast<size_t>(row + 1) * w,
                          0xff000000u | (g << 8u));
            }
        } else {
            std::fill(dst, dst + static_cast<size_t>(w) * h, color);
        }
        return true;
    }
    bool grab_clt_target_window_metadata(uint32_t base, uint32_t* dst, int x, int y, int w, int h) override {
        uint32_t ignored = 0;
        if (!target_readback_enabled || !dst || !lookup_target_color(base,&ignored)) return false;
        window_reads.push_back({x, y, w, h, true});
        std::fill(dst,dst+size_t(w)*h,target_readback_metadata);
        return true;
    }
    std::vector<WindowRead> window_reads;
    bool target_readback_rows = false;
    uint32_t target_readback_metadata = 0xff00ffffu;
    // Additional addressed targets this backend can serve. The GL backends
    // keep a target for every base a guest list has selected, so the live
    // production case can have a readable target at an address the current
    // draw only samples accidentally (a 576x432 neighbour in the allocation
    // chain). A test that models that case must be able to read it.
    std::map<uint32_t, uint32_t> extra_target_readback;
    bool lookup_target_color(uint32_t base, uint32_t* color) const
    {
        if (base == target_readback_base) {
            *color = target_readback_color;
            return true;
        }
        const auto found = extra_target_readback.find(base);
        if (found == extra_target_readback.end()) return false;
        *color = found->second;
        return true;
    }
    void clt_clear_z(uint32_t z_fill_word) override
    {
        ++z_clears;
        last_z_fill_word = z_fill_word;
    }
    void clt_clear_color(uint32_t red_word, uint32_t green_word,
                         uint32_t blue_word, uint32_t alpha_word) override
    {
        ++color_clears;
        last_color_words = {red_word, green_word, blue_word, alpha_word};
    }
    const char* name() const override { return "recording"; }

    struct Upload {
        int w = 0;
        int h = 0;
        std::vector<uint32_t> pixels;
        // Guest-authored coarser levels, in the order the interpreter supplied
        // them; empty when the upload carried only the drawn level.
        std::vector<LodLevel> lod_chain;
        bool mipmap_linear = false;
        // Identities the interpreter supplied. A refresh that cached the
        // expanded chain must hand over the chain's own identity instead of
        // making the backend rescan the levels.
        uint64_t content_hash = 0;
        uint64_t lod_chain_hash = 0;
    };

    std::vector<Draw> draws;
    std::vector<Scissor> scissors;
    std::vector<Upload> uploads;
    std::vector<m2host::CelId> freed_cels;
    uint32_t z_clears = 0;
    uint32_t last_z_fill_word = 0;
    uint32_t color_clears = 0;
    std::array<uint32_t, 4> last_color_words{};
    int host_width = 640;
    int host_height = 480;
    bool target_readback_enabled = false;
    uint32_t target_readback_base = 0;
    uint32_t target_readback_color = 0;
};

bool expect(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "m2_guest_clt_test: FAIL: %s\n", message);
    return false;
}

uint64_t snapshot_hash_for_test(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Documented MSB-replication widening, defined below with the widening
// oracle. Declared here for oracles that need the documented alpha rule
// before that section in file order.
uint32_t doc_widen_msb(unsigned depth, uint32_t value);

bool test_dbz_masks()
{
    // State 2 uses DBZCNTL=0x7: pixel output on smaller/equal Z, but Z
    // updates only on smaller Z.  The two decisions must not be collapsed.
    if (!expect(m2host::clt_depth_pixel_pass(0x7u, 1.f, 2.f, true),
                "DBZ 0x7 emits a smaller-Z pixel")) return false;
    if (!expect(m2host::clt_depth_pixel_pass(0x7u, 2.f, 1.f, true) == false,
                "DBZ 0x7 rejects a greater-Z pixel")) return false;
    if (!expect(m2host::clt_depth_pixel_pass(0x7u, 1.f, 1.f, true),
                "DBZ 0x7 emits an equal-Z pixel")) return false;
    if (!expect(m2host::clt_depth_write_pass(0x7u, 1.f, 2.f, true, true),
                "DBZ 0x7 updates on smaller Z")) return false;
    if (!expect(m2host::clt_depth_write_pass(0x7u, 1.f, 1.f, true, true) ==
                    false,
                "DBZ 0x7 does not update on equal Z")) return false;
    if (!expect(m2host::clt_depth_write_pass(0x7u, 2.f, 1.f, true, true) ==
                    false,
                "DBZ 0x7 does not update on greater Z")) return false;
    if (!expect(m2host::clt_depth_pixel_pass(m2host::kCltDbzAll, 2.f, 1.f,
                                             true),
                "DBZ 0x3f permits every pixel relation"))
        return false;
    // State 3 Z-as-destination alias (host frame 879, draw-prov orders
    // 1002-1009): the 0x56a000 fill (exec 2266, dbuser 0x60f) and the
    // 0x4f0000 composite (exec 2267, dbuser 0x66f) both carry DBZCNTL=0x1
    // (PIXOUTONSMALLERZ only, no ZUPDATE bits) with ztest=zwrite=1.
    // Trace: M2_PREVIEW_MODE=advance, M2_DRAW_PROV_FRAME=879, 880 host
    // frames, exit 0, State 3/17, zero drops. The pixel gate passes only
    // the strict smaller-Z relation and the Z-write gate matches nothing,
    // so neither draw updates depth through the ordinary Z-output path.
    // The first draw ALSO aliases color to Z; the shadow regression pins
    // that independent write (it must not be gated by these Z-update bits).
    if (!expect(m2host::clt_depth_pixel_pass(0x1u, 1.f, 2.f, true),
                "DBZ 0x1 emits a smaller-Z pixel"))
        return false;
    if (!expect(m2host::clt_depth_pixel_pass(0x1u, 2.f, 1.f, true) == false,
                "DBZ 0x1 rejects a greater-Z pixel"))
        return false;
    if (!expect(m2host::clt_depth_pixel_pass(0x1u, 1.f, 1.f, true) == false,
                "DBZ 0x1 rejects an equal-Z pixel"))
        return false;
    if (!expect(m2host::clt_depth_write_pass(0x1u, 1.f, 2.f, true, true) ==
                    false,
                "DBZ 0x1 never updates depth on smaller Z"))
        return false;
    if (!expect(m2host::clt_depth_write_pass(0x1u, 2.f, 1.f, true, true) ==
                    false,
                "DBZ 0x1 never updates depth on greater Z"))
        return false;
    return expect(m2host::clt_depth_write_pass(0x1u, 1.f, 1.f, true, true) ==
                      false,
                  "DBZ 0x1 never updates depth on equal Z");
}

bool test_dbz_backend_split()
{
    // The GL backends keep the pixel-output compare in the color pass and
    // translate the independent Z-update mask for a color-masked depth pass.
    // These are the authored words: 0x3 and 0x7 arm the smaller-Z update
    // relation, which maps to host GREATER on the reciprocal-W plane.
    const std::array<std::pair<uint32_t, m2host::CltBackendDepthCompare>, 6>
        authored = {{
            {0x0u, m2host::CltBackendDepthCompare::never},
            {0x1u, m2host::CltBackendDepthCompare::never},
            {0x3u, m2host::CltBackendDepthCompare::greater},
            {0x5u, m2host::CltBackendDepthCompare::never},
            {0x7u, m2host::CltBackendDepthCompare::greater},
            {m2host::kCltDbzAll, m2host::CltBackendDepthCompare::always},
        }};
    for (const auto& [raw, expected] : authored) {
        if (!expect(m2host::clt_backend_depth_compare_for_dbz_z_update(raw) ==
                        expected,
                    "DBZ update mask maps to a depth-only GL pass"))
            return false;
    }
    if (!expect(m2host::clt_dbz_z_update_enabled(0x7u, true, true) &&
                    !m2host::clt_dbz_z_update_enabled(0x7u, true, false) &&
                    !m2host::clt_dbz_z_update_enabled(0x5u, true, true) &&
                    !m2host::clt_dbz_z_update_enabled(0x5u, false, true),
                "DBZ update pass follows ZBUFEN, ZOUTEN, and the independent mask"))
        return false;
    if (!expect(m2host::clt_backend_depth_compare_for_dbz(0x7u) ==
                    m2host::CltBackendDepthCompare::greater_equal,
                "DBZ 0x7 keeps the independent pixel compare in the color pass"))
        return false;
    // The software backend evaluates the same two decisions per fragment; the
    // GL passes therefore need no alternate relation or polarity.
    if (!expect(m2host::clt_depth_pixel_pass(0x7u, 1.f, 1.f, true) &&
                    !m2host::clt_depth_write_pass(0x7u, 1.f, 1.f, true,
                                                   true),
                "DBZ 0x7 preserves equal-Z color without a Z update"))
        return false;

    // Combined pass criteria (clt_can_combine_depth_passes):
    // 0x3 (LT / LT) and 0x7 (GE / GT) are combinable when test & write are enabled and no SSB split.
    m2host::CltBackendDepthCompare combined_cmp = m2host::CltBackendDepthCompare::never;
    if (!expect(m2host::clt_can_combine_depth_passes(0x3u, true, true, false, &combined_cmp) &&
                    combined_cmp == m2host::CltBackendDepthCompare::greater,
                "DBZ 0x3 combines into a single pass with GREATER comparison"))
        return false;
    if (!expect(m2host::clt_can_combine_depth_passes(0x7u, true, true, false, &combined_cmp) &&
                    combined_cmp == m2host::CltBackendDepthCompare::greater_equal,
                "DBZ 0x7 combines into a single pass with GREATER_EQUAL comparison"))
        return false;
    // Differing relation (0x17 = always pixel, greater Z) cannot combine.
    if (!expect(!m2host::clt_can_combine_depth_passes(0x17u, true, true, false),
                "DBZ 0x17 requires split passes due to differing update masks"))
        return false;
    // SSB split cannot combine.
    if (!expect(!m2host::clt_can_combine_depth_passes(0x7u, true, true, true),
                "SSB entry split requires separate passes"))
        return false;
    // Disabled Z-update cannot combine with depth write.
    if (!expect(!m2host::clt_can_combine_depth_passes(0x7u, true, false, false),
                "Disabled Z-update cannot combine depth write"))
        return false;
    // Disabled depth test cannot combine.
    if (!expect(!m2host::clt_can_combine_depth_passes(0x7u, false, true, false),
                "Disabled depth test cannot combine"))
        return false;
    return true;
}

bool test_depth_compare_and_dsb_contract()
{
    using m2host::CltBackendDepthCompare;
    const std::array<std::pair<uint32_t, CltBackendDepthCompare>, 8> cases = {{
        {0u, CltBackendDepthCompare::never},
        {m2host::kCltDbzPixelLt, CltBackendDepthCompare::greater},
        {m2host::kCltDbzPixelEq, CltBackendDepthCompare::equal},
        {m2host::kCltDbzPixelGt, CltBackendDepthCompare::less},
        {m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq,
         CltBackendDepthCompare::greater_equal},
        {m2host::kCltDbzPixelEq | m2host::kCltDbzPixelGt,
         CltBackendDepthCompare::less_equal},
        {m2host::kCltDbzPixelLt | m2host::kCltDbzPixelGt,
         CltBackendDepthCompare::not_equal},
        {m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq |
             m2host::kCltDbzPixelGt,
         CltBackendDepthCompare::always},
    }};
    for (const auto& [raw, expected] : cases) {
        if (!expect(m2host::clt_backend_depth_compare_for_dbz(raw) == expected,
                    "DBZ pixel mask has one shared backend comparison"))
            return false;
    }
    const float adjacent = std::nextafter(1.0f, 2.0f);
    if (!expect(m2host::clt_depth_relation(1.0f, adjacent) ==
                    m2host::CltDepthRelation::smaller &&
                    m2host::clt_depth_relation(adjacent, 1.0f) ==
                        m2host::CltDepthRelation::greater,
                "adjacent reciprocal-W values are not widened into equality"))
        return false;

    // cltmacros.h raw host-word layout: DSBCONST = PPCBIT(29) -> 0x4 and
    // DSBSELECT = PPCMASK(3,31) -> 0x3 (0=object SSB, 1=constant, 2=source).
    // The vendor `M_DBInit` macro writes DSBCONST=1 with DSBSELECT=CONST,
    // i.e. raw 0x5; the lifted `DestBlendInit@0x2e870` emits that same word
    // (generated game_text_04.c, guest 0x2ebac).
    const m2host::CltDsbState init = m2host::clt_decode_dsb(0x5u);
    if (!expect(init.valid && init.constant &&
                    init.select == m2host::CltDsbSelect::constant_ssb,
                "DBSSBDSBCNTL decodes the M_DBInit constant selector"))
        return false;
    const m2host::CltDsbState object = m2host::clt_decode_dsb(0x0u);
    if (!expect(object.valid && !object.constant &&
                    object.select == m2host::CltDsbSelect::object_ssb,
                "DBSSBDSBCNTL decodes the object selector"))
        return false;
    const m2host::CltDsbState constant_bit = m2host::clt_decode_dsb(0x4u);
    if (!expect(constant_bit.valid && constant_bit.constant &&
                    constant_bit.select == m2host::CltDsbSelect::object_ssb,
                "DBSSBDSBCNTL decodes DSBCONST independently of the selector"))
        return false;
    // 0x2 is the source-SSB selection recorded by the authenticated
    // M2_CLT_Z_SURFACE_TRACE capture; the SDK layout decodes it as source SSB.
    const m2host::CltDsbState source = m2host::clt_decode_dsb(0x2u);
    if (!expect(source.valid && !source.constant &&
                    source.select == m2host::CltDsbSelect::source_ssb,
                "DBSSBDSBCNTL decodes the source selector"))
        return false;
    return expect(!m2host::clt_decode_dsb(0x3u).valid,
                  "reserved DBSSBDSBCNTL selector is not guessed");
}

// The DSB equation registers (DBAMULTCNTL/DBBMULTCNTL/DBALUCNTL) are decoded
// from the checked-in Mercury field declarations.  The words below are the
// vendor `M_DB*` macro expansions plus the literal words the lifted guest
// text statically authors, so a field-position drift fails this test.
bool test_dsb_equation_decode()
{
    using m2host::CltDbACoef;
    using m2host::CltDbAInput;
    using m2host::CltDbAluOp;
    using m2host::CltDbBCoef;
    using m2host::CltDbBInput;
    using m2host::CltDbConstControl;

    // M_DBSpec: A = TEXCOLOR * CONST, B = CONSTCOLOR * TEXALPHA, A+BCLAMP.
    const m2host::CltDsbAMultiplier spec_a =
        m2host::clt_decode_dsb_a_multiplier(0x20u);
    const m2host::CltDsbBMultiplier spec_b =
        m2host::clt_decode_dsb_b_multiplier(0x40u);
    const m2host::CltDsbAlu spec_alu = m2host::clt_decode_dsb_alu(0x0u);
    if (!expect(spec_a.valid && !spec_a.justify &&
                    spec_a.input == CltDbAInput::texture_color &&
                    spec_a.coef == CltDbACoef::constant &&
                    spec_a.const_control == CltDbConstControl::texture_ssb,
                "DBAMULTCNTL decodes the M_DBSpec A term"))
        return false;
    if (!expect(spec_b.valid && !spec_b.justify &&
                    spec_b.input == CltDbBInput::constant_color &&
                    spec_b.coef == CltDbBCoef::texture_alpha &&
                    spec_b.const_control == CltDbConstControl::texture_ssb,
                "DBBMULTCNTL decodes the M_DBSpec B term"))
        return false;
    if (!expect(spec_alu.op_documented &&
                    spec_alu.op == CltDbAluOp::a_plus_bclamp &&
                    spec_alu.final_divide == 0u &&
                    m2host::clt_dsb_is_specular_composite(spec_a, spec_b,
                                                          spec_alu),
                "M_DBSpec words select the source-backed specular composite"))
        return false;

    // M_DBTrans: A = TEXCOLOR * TEXALPHA, B = SRCCOLOR * (1 - TEXALPHA),
    // A+BCLAMP.  That is the blend that SRCEN selects, not the specular form.
    const m2host::CltDsbAMultiplier trans_a =
        m2host::clt_decode_dsb_a_multiplier(0x0u);
    const m2host::CltDsbBMultiplier trans_b =
        m2host::clt_decode_dsb_b_multiplier(0x2u);
    if (!expect(trans_a.input == CltDbAInput::texture_color &&
                    trans_a.coef == CltDbACoef::texture_alpha &&
                    trans_b.input == CltDbBInput::source_color &&
                    trans_b.coef == CltDbBCoef::texture_alpha_complement &&
                    !m2host::clt_dsb_is_specular_composite(trans_a, trans_b,
                                                           spec_alu),
                "M_DBTrans words decode to the source-over equation"))
        return false;

    // M_DBFog: A = TEXCOLOR * TEXALPHA, B = CONSTCOLOR * (1 - TEXALPHA),
    // A+BCLAMP.  Const-color B input plus complemented texture alpha.
    const m2host::CltDsbBMultiplier fog_b =
        m2host::clt_decode_dsb_b_multiplier(0x42u);
    if (!expect(fog_b.valid &&
                    fog_b.input == CltDbBInput::constant_color &&
                    fog_b.coef == CltDbBCoef::texture_alpha_complement,
                "DBBMULTCNTL decodes the M_DBFog B term"))
        return false;

    // Statically authored words recovered from the lifted image.  Each one
    // must decode to a documented form and must not be mistaken for the
    // specular composite.
    struct AMultCase {
        uint32_t raw;
        CltDbAInput input;
        CltDbACoef coef;
    };
    const AMultCase a_cases[] = {
        {0x2u, CltDbAInput::texture_color, CltDbACoef::texture_alpha_complement},
        {0x40u, CltDbAInput::constant_color, CltDbACoef::texture_alpha},
    };
    for (const AMultCase& c : a_cases) {
        const m2host::CltDsbAMultiplier a =
            m2host::clt_decode_dsb_a_multiplier(c.raw);
        if (!expect(a.valid && a.input == c.input && a.coef == c.coef &&
                        !a.justify,
                    "statically authored DBAMULTCNTL word decodes"))
            return false;
    }
    struct BMultCase {
        uint32_t raw;
        CltDbBInput input;
        CltDbBCoef coef;
    };
    const BMultCase b_cases[] = {
        {0x2u, CltDbBInput::source_color, CltDbBCoef::texture_alpha_complement},
        {0x20u, CltDbBInput::source_color, CltDbBCoef::constant},
        {0x32u, CltDbBInput::source_color, CltDbBCoef::texture_color_complement},
        {0x40u, CltDbBInput::constant_color, CltDbBCoef::texture_alpha},
    };
    for (const BMultCase& c : b_cases) {
        const m2host::CltDsbBMultiplier b =
            m2host::clt_decode_dsb_b_multiplier(c.raw);
        if (!expect(b.valid && b.input == c.input && b.coef == c.coef &&
                        !b.justify,
                    "statically authored DBBMULTCNTL word decodes"))
            return false;
    }
    struct AluCase {
        uint32_t raw;
        CltDbAluOp op;
        uint32_t final_divide;
    };
    const AluCase alu_cases[] = {
        {0x10u, CltDbAluOp::a_plus_b, 0u},
        {0xd0u, CltDbAluOp::b, 0u},
        {0x0u, CltDbAluOp::a_plus_bclamp, 0u},
    };
    for (const AluCase& c : alu_cases) {
        const m2host::CltDsbAlu alu = m2host::clt_decode_dsb_alu(c.raw);
        if (!expect(alu.op_documented && alu.op == c.op &&
                        alu.final_divide == c.final_divide,
                    "statically authored DBALUCNTL word decodes"))
            return false;
    }

    const m2host::CltDsbAlu undocumented_final_divide =
        m2host::clt_decode_dsb_alu(0x03u);
    if (!expect(undocumented_final_divide.final_divide == 3u &&
                    !undocumented_final_divide.final_divide_documented,
                "undocumented DBALUCNTL final-divide selector stays undocumented"))
        return false;

    // Undocumented encodings stay recognisable as such so no caller silently
    // treats them as a documented form.
    if (!expect(!m2host::clt_dsb_alu_op_documented(0x01u) &&
                    !m2host::clt_decode_dsb_alu(0x09u).op_documented &&
                    !m2host::clt_decode_dsb_alu(0x03u).final_divide_documented &&
                    !m2host::clt_decode_dsb_a_multiplier(0x8u).valid &&
                    !m2host::clt_decode_dsb_a_multiplier(0x100020u).valid &&
                    !m2host::clt_decode_dsb_alu(0x100u).reserved_clear,
                "undocumented DSB encodings are not promoted to documented ones"))
        return false;
    return true;
}

// The statically authored non-specular DSB equations recovered from the
// lifted image must not be silently reinterpreted as the source-over or
// specular composite.  `FN_TextDestBlendInit__2ebd4`'s DBAMULTCNTL=0x40,
// DBBMULTCNTL=0x2, DBALUCNTL=0x10 with SRCEN clear (the name-entry record row)
// is admitted as a general internal equation whose disabled source reads zero;
// M_DBTrans's words with SRCEN clear still have no defined source term.
bool test_dsb_equation_classification()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    const auto run = [&](uint32_t a_mult, uint32_t b_mult, uint32_t alu) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, 0x40u,  // DBUSERCONTROL: BLENDEN, SRCEN clear
            0x10008058u, a_mult,
            0x10008064u, b_mult,
            0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        return interpreter.execute(words.data(), words.size());
    };
    const m2host::CltExecutionResult spec = run(0x20u, 0x40u, 0x0u);
    if (!expect(spec.ok && spec.unsupported == 0 && spec.draws == 1,
                "M_DBSpec equation reaches the production draw boundary"))
        return false;
    const m2host::CltExecutionResult text = run(0x40u, 0x2u, 0x10u);
    if (!expect(text.ok && text.unsupported == 0 && text.draws == 1,
                "SRCEN-clear constant text reaches the production draw "
                "boundary"))
        return false;
    const m2host::CltExecutionResult other = run(0x0u, 0x2u, 0x10u);
    if (!expect(other.unsupported == 1 && other.draws == 0,
                "SRCEN-clear M_DBTrans DSB equation stays fail-closed"))
        return false;
    // Reserved bits above the documented multiplier field must not be
    // stripped into the specular form.
    const m2host::CltExecutionResult reserved = run(0x100020u, 0x40u, 0x0u);
    if (!expect(reserved.unsupported == 1 && reserved.draws == 0,
                "reserved multiplier bits stay fail-closed"))
        return false;

    // `FN_StreamEventFunc__3b63c` authors the Configure Race border blend
    // after its source-copy setup: DBUSERCONTROL=0x6f (BLENDEN|SRCEN),
    // DBAMULTCNTL=0x2 (TEXCOLOR*(1-TEXALPHA)), DBBMULTCNTL=0x0
    // (SRCCOLOR*TEXALPHA), and DBALUCNTL=0x10 (A_PLUS_B). This is the
    // source-frame-buffer read-modify-write used for the four 0xe78 strips.
    // Keep the equation predicate separate from the draw boundary: an
    // untextured packet with no armed source must fail closed rather than
    // proving the predicate by reaching a backend without its second input.
    if (!expect(m2host::clt_dsb_is_secondary_source_alpha(
                    m2host::clt_decode_dsb_a_multiplier(0x2u),
                    m2host::clt_decode_dsb_b_multiplier(0x0u),
                    m2host::clt_decode_dsb_alu(0x10u)),
                "Configure Race border equation decodes to texture/source "
                "alpha composition"))
        return false;
    // The same equation with the inherited A_PLUS_BCLAMP word (the vendor
    // M_DBTrans value, and what a list that never writes DBALUCNTL runs)
    // decodes identically; a subtract word is a different equation and must
    // not classify.
    if (!expect(m2host::clt_dsb_is_secondary_source_alpha(
                    m2host::clt_decode_dsb_a_multiplier(0x2u),
                    m2host::clt_decode_dsb_b_multiplier(0x0u),
                    m2host::clt_decode_dsb_alu(0x0u)) &&
                    !m2host::clt_dsb_is_secondary_source_alpha(
                        m2host::clt_decode_dsb_a_multiplier(0x2u),
                        m2host::clt_decode_dsb_b_multiplier(0x0u),
                        m2host::clt_decode_dsb_alu(0x40u)),
                "reverse-alpha equation accepts the inherited add word, not "
                "a subtract"))
        return false;
    RecordingBackend border_backend;
    m2host::GuestCltInterpreter border_interpreter(&border_backend);
    std::vector<uint32_t> border_words = {
        0x10008008u, 0x6fu,
        0x10008058u, 0x2u,
        0x10008064u, 0x0u,
        0x10008070u, 0x10u,
        0x20090002u,
    };
    for (const auto& vertex : vertex_data)
        for (const float value : vertex)
            border_words.push_back(std::bit_cast<uint32_t>(value));
    const m2host::CltExecutionResult border =
        border_interpreter.execute(border_words.data(), border_words.size());
    if (!expect(border.ok && border.unsupported == 1 && border.draws == 0 &&
                    border_backend.draws.empty(),
                "secondary-source equation without an armed DB source stays "
                "fail-closed"))
        return false;
    return true;
}

// RenderRadarFrame copies Corner1Snippet at 0x17d1a4: 164 words at
// 0x17cf14. Original yellow RGB=(31/32,63/64,95/256), alpha=1/4.
// The four six-vertex fans must retain these colors even with SRCEN armed.
bool test_radar_corner_source_over()
{
    const uint32_t original[] = {
        0x10008008u, 0x0000006fu, 0x10008058u, 0x00000000u, 0x10008064u, 0x00000002u,
        0x10008070u, 0x00000010u, 0x1000800cu, 0x00000000u, 0x10006408u, 0x00000000u,
        0x10006410u, 0x0000000bu, 0x20190005u, 0x41900000u, 0x41900000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42740000u, 0x41900000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42680000u, 0x41a80000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x41a80000u, 0x41a80000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x41a80000u, 0x42680000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x41900000u, 0x42740000u, 0x3f780000u,
        0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x20190005u, 0x43220000u, 0x41900000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42ee0000u, 0x41900000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42f40000u, 0x41a80000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x431f0000u, 0x41a80000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x431f0000u, 0x42680000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x43220000u, 0x42740000u,
        0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x20190005u, 0x43220000u,
        0x43720000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42ee0000u,
        0x43720000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x42f40000u,
        0x436f0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x431f0000u,
        0x436f0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x431f0000u,
        0x434a0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x43220000u,
        0x43470000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u, 0x20190005u,
        0x41900000u, 0x43720000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x42740000u, 0x43720000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x42680000u, 0x436f0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x41a80000u, 0x436f0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x41a80000u, 0x434a0000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x41900000u, 0x43470000u, 0x3f780000u, 0x3f7c0000u, 0x3ebe0000u, 0x3e800000u,
        0x10000014u, 0x00000020u,
    };
    for (bool continuation : {false, true}) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> commands = {
            0x10008034u, 0x1000u,  // source framebuffer, as in the race
            0x10008038u, 576u,
        };
        commands.insert(commands.end(), std::begin(original), std::end(original));
        if (continuation) {
            // Split each fan after three vertices: the same geometry must
            // survive the separate continuation routing branch.
            for (size_t i = commands.size(); i-- > 4;) {
                if (commands[i] != 0x20190005u) continue;
                commands[i] = 0x20190002u;
                commands.insert(commands.begin() + i + 19, 0x20110002u);
            }
        }
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange source;
        source.guest_address = 0x1000u;
        source.bytes.assign(576u * 432u * 2u, 0u);
        memory.ranges.push_back(std::move(source));
        const auto result = interpreter.execute(commands.data(), commands.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 16,
                    "original radar snippet renders all four corner strips")) return false;
        for (const auto& draw : backend.draws) {
            if (!expect(!draw.textured && draw.blend_enabled &&
                            draw.blend.mode == m2host::CltBlendMode::source_over,
                        "radar retains primitive color as source-over input")) return false;
            for (const auto& vertex : draw.vertices)
                if (!expect(vertex.r == 31.f/32.f && vertex.g == 63.f/64.f &&
                                vertex.b == 95.f/256.f && vertex.a == .25f,
                            "authored yellow and quarter alpha survive SRCEN")) return false;
        }
        m2host::SoftCaptureBackend soft(576, 432, {});
        soft.frame_begin(0.f, 0.f, 0.f);
        m2host::GuestCltInterpreter raster(&soft);
        raster.set_viewport(0, 0, 576, 432, false, 576, 432);
        const auto pixels_result = raster.execute(commands.data(), commands.size(), &memory);
        std::vector<uint32_t> pixels(576u * 432u);
        if (!expect(pixels_result.ok && soft.grab_presented_rgba(pixels.data(), 576, 432),
                    "original radar corners reach raster output")) return false;
        // Black background: C = yellow * 1/4, byte rounding within one.
        // Interior pixels on each horizontal arm, plus untouched radar center.
        for (auto xy : {std::pair{30,19}, {150,19}, {150,240}, {30,240}}) {
            const uint32_t pixel = pixels[xy.second * 576u + xy.first];
            if (!expect(std::abs(int(pixel & 255u) - 62) <= 1 &&
                            std::abs(int((pixel >> 8u) & 255u) - 63) <= 1 &&
                            std::abs(int((pixel >> 16u) & 255u) - 24) <= 1,
                        "each corner blends authored yellow over black")) return false;
        }
        if (!expect((pixels[130u * 576u + 90u] & 0xffffffu) == 0,
                    "radar center remains untouched")) return false;
    }
    return true;
}

// The statically authored constant-color equation.  `FN_TextDestBlendInit__2ebd4`
// and `FN_UI_DrawHighScores_nameEntry__3a458` write DBUSERCONTROL=0x6f
// (BLENDEN + SRCEN + DESTOUTMASK), DBDISCARDCONTROL=0x1 (alpha0 discard),
// DBSRCXSTRIDE=0x240 (the source frame buffer is the bound screen),
// DBAMULTCNTL=0x40 (A = CONSTCOLOR * TEXALPHA), DBBMULTCNTL=0x2
// (B = SRCCOLOR * (1 - TEXALPHA)) and DBALUCNTL=0x10 (A_PLUS_B, no final
// divide).  With SRCEN armed the equation is fully defined, so the draw must
// reach the backend as the guest's DBCONSTIN color over the destination
// instead of being collapsed into the recovered source-over default.
bool test_dsb_constant_alpha_blend()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    struct Probe {
        bool drew = false;
        bool blend_enabled = false;
        m2host::CltBlendState blend{};
    };
    const auto run = [&](uint32_t user_control, uint32_t a_mult, uint32_t b_mult,
                         uint32_t alu, uint32_t constant) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, user_control,
            0x10008054u, constant,
            0x10008058u, a_mult,
            0x10008064u, b_mult,
            0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) {
            probe.blend_enabled = backend.draws.back().blend_enabled;
            probe.blend = backend.draws.back().blend;
        }
        return probe;
    };

    const Probe text = run(0x6fu, 0x40u, 0x2u, 0x10u, 0x00112233u);
    if (!expect(text.drew,
                "guest text/UI blend setup reaches the production draw "
                "boundary"))
        return false;
    if (!expect(text.blend.mode == m2host::CltBlendMode::constant_alpha_over,
                "constant-color DSB equation selects the constant blend"))
        return false;
    if (!expect(text.blend_enabled && text.blend.constant_r == 17.f / 255.f &&
                    text.blend.constant_g == 34.f / 255.f &&
                    text.blend.constant_b == 51.f / 255.f,
                "decoded constant equation carries guest DBCONSTIN to the "
                "backend"))
        return false;

    // M_DBTrans (cltmacromacros.h) uses the identical B word and the same
    // BLENDEN|SRCEN gate with the texture-blender output in the A slot; the
    // recovered State 3 source-over result must not change.
    const Probe trans = run(0x6fu, 0x0u, 0x2u, 0x0u, 0x00112233u);
    if (!expect(trans.drew && trans.blend_enabled &&
                    trans.blend.mode == m2host::CltBlendMode::source_over,
                "M_DBTrans equation keeps the recovered source-over result"))
        return false;

    // The shooter2d background list (`drawsourceandtexture`) authors these
    // same A/B words but never writes DBALUCNTL: its cloud bands run with the
    // font folio's authored 0x0 (A_PLUS_BCLAMP), the value the vendor
    // M_DBTrans macro itself selects.  The convex weights w and 1 - w make
    // the clamp a no-op, so the inherited word must evaluate the guest's
    // equation exactly instead of falling to the disclosed source-over
    // approximation (which blends against the current destination and left
    // the shooter2d background black).
    const Probe inherited = run(0x7fu, 0x40u, 0x2u, 0x0u, 0x008080b4u);
    if (!expect(inherited.drew && !inherited.blend.equation_approximate &&
                    inherited.blend.mode ==
                        m2host::CltBlendMode::constant_alpha_over &&
                    inherited.blend.constant_r == 128.f / 255.f &&
                    inherited.blend.constant_b == 180.f / 255.f,
                "inherited A_PLUS_BCLAMP word evaluates the constant-color "
                "equation"))
        return false;

    // UI_DrawHighScores_nameEntry's record row keeps these words but sets
    // DBLA_EnableAttrs=0x5f (BLENDEN|DITHEREN, SRCEN clear). It must draw
    // through the general internal path, whose disabled source reads zero,
    // instead of being refused (the blank record row and typed name).
    const Probe unarmed = run(0x5fu, 0x40u, 0x2u, 0x10u, 0x001b8200u);
    if (!expect(unarmed.drew && unarmed.blend_enabled &&
                    unarmed.blend.mode ==
                        m2host::CltBlendMode::general_internal &&
                    unarmed.blend.general.constant == 0x001b8200u,
                "SRCEN-clear constant text draws as a general internal "
                "equation with the guest DBCONSTIN"))
        return false;
    // Half-coverage edge texel with the zero source: CONSTCOLOR * 0.5.
    const auto edge = m2host::clt_dsb_evaluate(
        unarmed.blend.general, {0.f, 0.f, 0.f, 0.5f}, {0.f, 0.f, 0.f, 0.f},
        false, false);
    if (!expect(std::abs(edge[0] - 0x1bu / 255.f * 0.5f) < 0.01f &&
                    std::abs(edge[1] - 0x82u / 255.f * 0.5f) < 0.01f &&
                    edge[2] < 0.01f,
                "SRCEN-clear constant text scales DBCONSTIN by coverage"))
        return false;
    // The admission is limited to the constant form: M_DBTrans with SRCEN
    // clear and fractional alpha still has an undefined source term.
    const Probe unarmed_trans = run(0x5fu, 0x0u, 0x2u, 0x10u, 0x001b8200u);
    if (!expect(!unarmed_trans.drew,
                "SRCEN-clear M_DBTrans stays fail-closed"))
        return false;

    // Wrong-result controls: a subtract ALU or a non-zero final divide is a
    // different equation on the device, so the same A/B words must not
    // classify as the constant blend.
    const Probe subtract = run(0x7fu, 0x40u, 0x2u, 0x40u, 0x008080b4u);
    if (!expect(!subtract.drew,
                "A_MINUS_BCLAMP requires the separately armed source"))
        return false;
    if (!expect(!m2host::clt_dsb_is_constant_alpha_blend(
                    m2host::clt_decode_dsb_a_multiplier(0x40u),
                    m2host::clt_decode_dsb_b_multiplier(0x2u),
                    m2host::clt_decode_dsb_alu(0x1u)),
                "a final divide does not evaluate the constant blend"))
        return false;
    return true;
}

// M_DBFog distance-fog composite. cltmacromacros.h `M_DBFog` writes
// DBCONSTIN=fogcolor, DBAMULTCNTL=0x0 (A = TEXCOLOR*TEXALPHA),
// DBBMULTCNTL=0x42 (B = CONSTCOLOR*(1-TEXALPHA)) and DBALUCNTL=0x0
// (A_PLUS_BCLAMP) with BLENDEN set and SRCEN clear. The texture-stage alpha
// carries the per-vertex fog factor from `M_LightFog`, so the write lerps the
// texture-stage color toward the DBCONSTIN fog color. Before this equation was
// recognized it fell through to the fail-closed branch and every fogged world
// draw was dropped (the gray-screen fog defect).
bool test_dsb_fog_composite()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 0.f, 1.f},
    };
    // Decode level: the M_DBFog words select the fog composite and are not
    // mistaken for the specular or source-over equations.
    const m2host::CltDsbAMultiplier fog_a =
        m2host::clt_decode_dsb_a_multiplier(0x0u);
    const m2host::CltDsbBMultiplier fog_b =
        m2host::clt_decode_dsb_b_multiplier(0x42u);
    const m2host::CltDsbAlu fog_alu = m2host::clt_decode_dsb_alu(0x0u);
    if (!expect(m2host::clt_dsb_is_fog_composite(fog_a, fog_b, fog_alu),
                "M_DBFog words decode to the fog composite"))
        return false;
    if (!expect(!m2host::clt_dsb_is_specular_composite(fog_a, fog_b, fog_alu),
                "M_DBFog is not the M_DBSpec composite"))
        return false;
    if (!expect(!m2host::clt_dsb_is_source_over(fog_a, fog_b, fog_alu),
                "M_DBFog is not the source-over equation"))
        return false;
    // Negative control: the specular and source-over words must not classify
    // as fog either, so the classifier is not merely matching every form.
    if (!expect(!m2host::clt_dsb_is_fog_composite(
                    m2host::clt_decode_dsb_a_multiplier(0x20u),
                    m2host::clt_decode_dsb_b_multiplier(0x40u),
                    m2host::clt_decode_dsb_alu(0x0u)),
                "M_DBSpec words are not the fog composite"))
        return false;
    if (!expect(!m2host::clt_dsb_is_fog_composite(
                    m2host::clt_decode_dsb_a_multiplier(0x0u),
                    m2host::clt_decode_dsb_b_multiplier(0x2u),
                    m2host::clt_decode_dsb_alu(0x0u)),
                "M_DBTrans words are not the fog composite"))
        return false;

    struct Probe {
        bool drew = false;
        bool blend_enabled = false;
        uint32_t unsupported = 0;
        m2host::CltBlendState blend{};
    };
    const auto run = [&](uint32_t user_control, uint32_t a_mult, uint32_t b_mult,
                         uint32_t alu, uint32_t constant) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, user_control,
            0x10008054u, constant,
            0x10008058u, a_mult,
            0x10008064u, b_mult,
            0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.unsupported = result.unsupported;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) {
            probe.blend_enabled = backend.draws.back().blend_enabled;
            probe.blend = backend.draws.back().blend;
        }
        return probe;
    };

    // BLENDEN set (0x40) with SRCEN clear is M_DBFog's gate.
    const Probe fog = run(0x40u, 0x0u, 0x42u, 0x0u, 0x00112233u);
    if (!expect(fog.drew && fog.unsupported == 0,
                "M_DBFog equation reaches the production draw boundary"))
        return false;
    if (!expect(fog.blend.mode == m2host::CltBlendMode::fog_composite,
                "M_DBFog selects the fog composite blend"))
        return false;
    if (!expect(!fog.blend_enabled,
                "fog composite is an internal composite without framebuffer "
                "blending"))
        return false;
    if (!expect(fog.blend.constant_r == 17.f / 255.f &&
                    fog.blend.constant_g == 34.f / 255.f &&
                    fog.blend.constant_b == 51.f / 255.f,
                "fog composite carries the guest DBCONSTIN fog color"))
        return false;
    return true;
}

// The statically authored B-only equation.
// `FN_carshadow_initshadows__0c860` writes DBBMULTCNTL=0x32
// (B = SRCCOLOR * (1 - TEXCOLOR)) and DBALUCNTL=0xd0 (ALU operation B, no
// final divide) without touching DBUSERCONTROL, so the draw runs under
// whatever destination-blend gate the preceding list programmed.  With the
// source input armed the equation is fully defined and must reach the backend
// as the multiplied destination instead of the blanket source-over default;
// an unarmed source leaves SRCCOLOR undefined and must stay fail-closed.
bool test_dsb_inverse_texture_multiply()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    struct Probe {
        bool drew = false;
        bool blend_enabled = false;
        uint32_t unsupported = 0;
        m2host::CltBlendState blend{};
    };
    const auto run = [&](uint32_t user_control, uint32_t a_mult, uint32_t b_mult,
                         uint32_t alu) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, user_control,
            0x10008058u, a_mult,
            0x10008064u, b_mult,
            0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.unsupported = result.unsupported;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) {
            probe.blend_enabled = backend.draws.back().blend_enabled;
            probe.blend = backend.draws.back().blend;
        }
        return probe;
    };

    // DBUSERCONTROL 0x6f = BLENDEN + SRCEN armed; the A word is register
    // carried in the guest, so only its documented layout is required (the
    // ALU `B` operation discards the A product regardless of its selector).
    const Probe shadow = run(0x6fu, 0x20u, 0x32u, 0xd0u);
    if (!expect(shadow.drew && shadow.blend_enabled &&
                    shadow.blend.mode ==
                        m2host::CltBlendMode::inverse_texture_multiply,
                "car-shadow B-only DSB equation selects the multiply form"))
        return false;

    // An undocumented multiplier layout is not a known programmed state and
    // must not be promoted to the multiply form.
    const Probe reserved = run(0x6fu, 0x100020u, 0x32u, 0xd0u);
    if (!expect(!reserved.drew && reserved.unsupported == 1,
                "reserved A multiplier bits do not select the multiply form"))
        return false;

    // Same words with SRCEN clear: the guest's B input is the source frame
    // buffer, so without the source input the equation is undefined and the
    // draw must stay rejected rather than guessed.
    const Probe unarmed = run(0x40u, 0x20u, 0x32u, 0xd0u);
    if (!expect(unarmed.unsupported == 1 && !unarmed.drew,
                "B-only equation without the source input stays fail-closed"))
        return false;
    return true;
}

// The statically authored identity-coefficient B-only equation.
//
// The seven screen transitions (`FN_doscrolltrans__342f4`,
// `FN_dowipetrans__34cb8`, `FN_dostripestrans__35650`,
// `FN_dogridtrans__35d7c`, `FN_dosquare1trans__32010`,
// `FN_dosquare2trans__32f88`, `FN_dorotatetrans__33880`) and
// `FN_StreamEventFunc__3b63c` write DBUSERCONTROL=0x6f (BLENDEN + SRCEN),
// DBBMULTCNTL=0x20 (B = SRCCOLOR * CONST), DBBMULTCONSTSSB0/1=0xffffff (the
// coefficient constant is white, so the product is SRCCOLOR unchanged) and
// DBALUCNTL=0xd0 (ALU operation B, no final divide).
//
// The ALU `B` operation discards the A product, so the destination-blender
// output is the bound source frame buffer pixel and the write replaces the
// destination pixel instead of blending against it.  That is the reference
// blitter's own `Blt_BlendBlitObject(bo, src = 0.0, dest = 1.0)` case, which
// emits the identical B word from the same helper the transitions use.
bool test_dsb_source_copy()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    struct Probe {
        bool drew = false;
        bool blend_enabled = false;
        uint32_t unsupported = 0;
        m2host::CltBlendState blend{};
    };
    const auto run = [&](uint32_t user_control, uint32_t a_mult, uint32_t b_mult,
                         uint32_t alu, uint32_t b_const0, uint32_t b_const1) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, user_control,
            0x10008058u, a_mult,
            0x10008064u, b_mult,
            0x10008068u, b_const0,
            0x1000806cu, b_const1,
            0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.unsupported = result.unsupported;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) {
            probe.blend_enabled = backend.draws.back().blend_enabled;
            probe.blend = backend.draws.back().blend;
        }
        return probe;
    };

    // Recovered transition words.  The transitions do not re-author the A
    // word in this block, so the register is persistent state; the result must
    // not depend on it because the ALU `B` operation discards the A product.
    const Probe wipe = run(0x6fu, 0x0u, 0x20u, 0xd0u, 0xffffffffu, 0xffffffffu);
    if (!expect(wipe.drew,
                "screen-transition DSB setup reaches the production draw "
                "boundary"))
        return false;
    if (!expect(wipe.blend.mode == m2host::CltBlendMode::source_copy,
                "identity-coefficient B-only DSB equation selects the copy "
                "form"))
        return false;
    if (!expect(wipe.blend_enabled,
                "the recovered copy equation carries the destination write to "
                "the backend"))
        return false;

    const Probe stale_a = run(0x6fu, 0x40u, 0x20u, 0xd0u, 0xffffffffu,
                              0xffffffffu);
    if (!expect(stale_a.drew &&
                    stale_a.blend.mode == m2host::CltBlendMode::source_copy,
                "a register-carried A word cannot change the copy result"))
        return false;

    // A non-white coefficient constant is a scaled copy, not the identity
    // form; it needs an armed source and must not become an identity copy.
    const Probe scaled = run(0x6fu, 0x0u, 0x20u, 0xd0u, 0x00808080u,
                             0x00808080u);
    if (!expect(!scaled.drew && scaled.unsupported == 1,
                "a non-white B coefficient is not the identity copy form"))
        return false;

    // The toolkit's two-input weighted mix authors the same B word with
    // A = TEXCOLOR * CONST and A_PLUS_B, so the A product participates and
    // the form is not a copy of the source input.
    const Probe weighted = run(0x6fu, 0x20u, 0x20u, 0x10u, 0xffffffffu,
                               0xffffffffu);
    if (!expect(weighted.drew &&
                    weighted.blend.mode != m2host::CltBlendMode::source_copy,
                "A_PLUS_B with the same B word is not the copy form"))
        return false;

    // An undocumented A multiplier layout is not a known programmed state and
    // must not be promoted to the copy form either.
    const Probe reserved = run(0x6fu, 0x100020u, 0x20u, 0xd0u, 0xffffffffu,
                               0xffffffffu);
    if (!expect(!reserved.drew && reserved.unsupported == 1,
                "reserved A multiplier bits do not select the copy form"))
        return false;

    // Same words with SRCEN clear: the B input is the source frame buffer, so
    // without the source input the equation is undefined and the draw must
    // stay rejected rather than guessed.
    const Probe unarmed = run(0x40u, 0x0u, 0x20u, 0xd0u, 0xffffffffu,
                              0xffffffffu);
    if (!expect(unarmed.unsupported == 1 && !unarmed.drew,
                "copy equation without the source input stays fail-closed"))
        return false;
    return true;
}

// The toolkit's two-input weighted mix: `Blt_BlendBlitObject(bo, src, dest)`
// writes the two weights into the multiplier coefficient pairs and combines
// A = TEXCOLOR with B = SRCCOLOR through A_PLUS_B/A_PLUS_BCLAMP.  The guest's
// `FN_setSsbBlendAttr__371a0` authors the same A word with the weight carried
// in its parameter and the B side spelled with the complement coefficient.
bool test_dsb_coefficient_weighted_mix()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    struct Probe {
        bool drew = false;
        bool blend_enabled = false;
        uint32_t unsupported = 0;
        m2host::CltBlendState blend{};
    };
    const auto run = [&](uint32_t user_control, uint32_t a_mult, uint32_t b_mult,
                         uint32_t alu, uint32_t a_const0, uint32_t a_const1,
                         uint32_t b_const0, uint32_t b_const1) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, user_control, 0x10008058u, a_mult,
            0x1000805cu, a_const0,     0x10008060u, a_const1,
            0x10008064u, b_mult,       0x10008068u, b_const0,
            0x1000806cu, b_const1,     0x10008070u, alu,
            0x20090002u,
        };
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.unsupported = result.unsupported;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) {
            probe.blend_enabled = backend.draws.back().blend_enabled;
            probe.blend = backend.draws.back().blend;
        }
        return probe;
    };

    // `Blt_BlendBlitObject(bo, src = 0.5, dest = 0.5)`: both coefficient pairs
    // carry 0x80 per channel and the ALU is A_PLUS_BCLAMP.  Both entries agree,
    // so the per-texel SSB cannot change the result (the toolkit's own
    // constant-transparency example writes both entry registers alike).
    const Probe balanced = run(0x6fu, 0x20u, 0x20u, 0x00u, 0x00808080u,
                               0x00808080u, 0x00808080u, 0x00808080u);
    if (!expect(balanced.drew,
                "weighted-mix DSB setup reaches the production draw boundary"))
        return false;
    if (!expect(balanced.blend.mode ==
                    m2host::CltBlendMode::coefficient_weighted_mix,
                "the toolkit weighted-mix equation selects its own result"))
        return false;
    if (!expect(balanced.blend_enabled,
                "the weighted mix carries the destination write to the "
                "backend"))
        return false;
    if (!expect(std::abs(balanced.blend.weight_tex_r - 128.f / 255.f) <
                        1e-4f &&
                    std::abs(balanced.blend.weight_src_r - 128.f / 255.f) <
                        1e-4f,
                "both coefficient pairs reach the backend as their own "
                "weights"))
        return false;
    if (!expect(!balanced.blend.weighted_mix_entry_dependent,
                "agreeing coefficient entries are texel-independent"))
        return false;

    // The guest's `setSsbBlendAttr` spelling with both entries alike: the B
    // side uses the complement coefficient, so
    // `Blt_BlendBlitObject(bo, w, 1 - w)`.
    const Probe complement = run(0x6fu, 0x20u, 0x22u, 0x00u, 0x00808080u,
                                 0x00808080u, 0x00808080u, 0x00808080u);
    if (!expect(complement.drew &&
                    complement.blend.mode ==
                        m2host::CltBlendMode::coefficient_weighted_mix,
                "the complement B coefficient is the same weighted mix"))
        return false;
    if (!expect(std::abs(complement.blend.weight_tex_r - 128.f / 255.f) <
                        1e-4f &&
                    std::abs(complement.blend.weight_src_r - 127.f / 255.f) <
                        1e-4f,
                "the complement coefficient yields the 1 - w destination "
                "weight"))
        return false;

    // The guest's own `setSsbBlendAttr` publishes its weight in the entry-0
    // words and 0xffffff in both entry-1 words.  The toolkit makes that the
    // per-texel SSB choice (entry 0 for SSB 0, entry 1 for SSB 1), so the two
    // entries really are two different equations and the state has to say so;
    // the draw-time gate is what keeps an SSB-carrying texture off this path.
    const Probe guest_site =
        run(0x6fu, 0x20u, 0x22u, 0x00u, 0x00404040u, 0x00ffffffu, 0x00404040u,
            0x00ffffffu);
    if (!expect(guest_site.drew &&
                    guest_site.blend.mode ==
                        m2host::CltBlendMode::coefficient_weighted_mix,
                "the guest's setSsbBlendAttr words select the weighted mix"))
        return false;
    if (!expect(std::abs(guest_site.blend.weight_tex_r - 64.f / 255.f) <
                        1e-4f &&
                    std::abs(guest_site.blend.weight_src_r - 191.f / 255.f) <
                        1e-4f,
                "the guest's cross-fade resolves entry 0 to w and 1 - w"))
        return false;
    if (!expect(guest_site.blend.weighted_mix_entry_dependent,
                "entry 0 and entry 1 differ for the guest's cross-fade"))
        return false;

    // Only the B entries differ: same rule.
    const Probe half_pair = run(0x6fu, 0x20u, 0x20u, 0x00u, 0x00808080u,
                                0x00808080u, 0x00808080u, 0x00ffffffu);
    if (!expect(half_pair.drew &&
                    half_pair.blend.mode ==
                        m2host::CltBlendMode::coefficient_weighted_mix &&
                    half_pair.blend.weighted_mix_entry_dependent,
                "a differing B entry alone makes the equation entry-"
                "dependent"))
        return false;

    // The entry rule itself, in both directions.  `false` for either input
    // means the draw reads entry 0 only (the toolkit passes SSB 0 for a
    // texture with no SSB), so the equation stays texel-independent.
    if (!expect(!m2host::clt_dsb_weighted_mix_ssb_selects_entry1(false,
                                                                true) &&
                    !m2host::clt_dsb_weighted_mix_ssb_selects_entry1(true,
                                                                    false),
                "a zero SSB signal or agreeing entries leaves entry 0 in "
                "force"))
        return false;
    if (!expect(m2host::clt_dsb_weighted_mix_ssb_selects_entry1(true, true),
                "a set SSB bit with differing entries selects entry 1"))
        return false;

    // The A product only participates through a documented A input/coef pair.
    // An A side that selects the texture alpha instead of the constant
    // coefficient is the M_DBTrans equation, not this one.  (No case here uses
    // the "both entries agree" shortcut to reach a different mode, so the
    // negatives below are all structure checks.)
    const Probe alpha_slot = run(0x6fu, 0x00u, 0x20u, 0x00u, 0x00808080u,
                                 0x00808080u, 0x00808080u, 0x00808080u);
    if (!expect(!alpha_slot.drew && alpha_slot.unsupported == 1,
                "a non-constant A coefficient is not the weighted mix"))
        return false;

    // `a_minus_b` is a different ALU operation on the same products.
    const Probe subtracted = run(0x6fu, 0x20u, 0x20u, 0x50u, 0x00808080u,
                                 0x00808080u, 0x00808080u, 0x00808080u);
    if (!expect(!subtracted.drew && subtracted.unsupported == 1,
                "a non-additive ALU operation is not the weighted mix"))
        return false;

    // The B input is the source frame buffer, so the equation is undefined
    // without the armed source input.
    const Probe unarmed = run(0x40u, 0x20u, 0x20u, 0x00u, 0x00808080u,
                              0x00808080u, 0x00808080u, 0x00808080u);
    if (!expect(unarmed.unsupported == 1 && !unarmed.drew,
                "the weighted mix without the source input stays "
                "fail-closed"))
        return false;
    return true;
}

bool test_dsb_backend_transport_and_shared_depth()
{
    // The DSB selector is a guest-visible register state.  It must reach the
    // backend unchanged even though the source evidence does not establish a
    // production DSB output formula for every destination format.
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 0.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 0.f, 0.f, 1.f, 1.f},
    };
    // SDK raw words: 0x0 object, 0x4 object + DSBCONST, 0x5 constant +
    // DSBCONST (M_DBInit / DestBlendInit), 0x2 source SSB (authenticated
    // M2_CLT_Z_SURFACE_TRACE capture value).
    struct DsbCase {
        uint32_t raw;
        m2host::CltDsbSelect select;
        bool constant;
    };
    const std::array<DsbCase, 4> dsb_words = {{
        {0x0u, m2host::CltDsbSelect::object_ssb, false},
        {0x4u, m2host::CltDsbSelect::object_ssb, true},
        {0x5u, m2host::CltDsbSelect::constant_ssb, true},
        {0x2u, m2host::CltDsbSelect::source_ssb, false},
    }};
    for (const DsbCase& dsb_case : dsb_words) {
        const uint32_t raw = dsb_case.raw;
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {0x10008050u, raw,
                                       0x20090002u};
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.draws.size() == 1,
                    "DBSSBDSBCNTL reaches the production draw boundary"))
            return false;
        const m2host::CltDsbState& state = backend.draws[0].blend.dsb;
        if (!expect(state.valid && state.raw == raw,
                    "backend preserves the raw DSB register word"))
            return false;
        if (!expect(state.select == dsb_case.select &&
                        state.constant == dsb_case.constant,
                    "backend preserves the source-backed DSB selector"))
            return false;
        // The multiplier/ALU registers were never written on this path, so
        // the decoded equation must be the documented all-zero default.
        const m2host::CltDsbEquation& equation = backend.draws[0].blend.equation;
        if (!expect(equation.a.raw == 0u && equation.b.raw == 0u &&
                        equation.alu.raw == 0u && equation.a.valid &&
                        equation.b.valid && equation.alu.op_documented,
                    "backend carries the decoded DSB equation state"))
            return false;
    }

    // Color targets are independent, but the TE Z surface is shared.  A
    // depth write on one sibling carve must reject a farther draw after the
    // color target switches; resetting depth per target would violate the
    // production setup contract.
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.25f, 0.f, 0.f);
    backend.clt_clear_z(0u);  // 0.0f; reciprocal-W values are in [0, 1].
    backend.clt_target_select(0x1000u);
    std::array<m2host::CltVertex, 3> near_triangle{};
    near_triangle[0] = {0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
    near_triangle[1] = {4.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
    near_triangle[2] = {0.f, 4.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
    m2host::CltDepth depth;
    depth.test_enabled = true;
    depth.write_enabled = true;
    depth.depth_func = m2host::kCltDbzPixelLt | m2host::kCltDbzZLt;
    depth.z_region_max_x = 4;
    depth.z_region_max_y = 4;
    backend.clt_draw(near_triangle.data(), 3, false, false,
                     m2host::kInvalidCel, 4.f, 4.f, false, {}, depth, {}, {});

    backend.clt_target_select(0x2000u);
    std::array<m2host::CltVertex, 3> far_triangle = near_triangle;
    for (auto& vertex : far_triangle) {
        vertex.r = 1.f;
        vertex.g = 0.f;
        vertex.b = 0.f;
        vertex.one_over_w = 0.75f;
    }
    backend.clt_draw(far_triangle.data(), 3, false, false,
                     m2host::kInvalidCel, 4.f, 4.f, false, {}, depth, {}, {});

    std::array<uint32_t, 16> target{};
    if (!expect(backend.grab_clt_target_rgba(0x2000u, target.data(), 4, 4),
                "shared-depth test reads the second color target"))
        return false;
    if (!expect(target[0] == (64u | 0xff000000u),
                "far draw is rejected against depth from the first target"))
        return false;
    if (!expect(backend.grab_clt_target_rgba(0x1000u, target.data(), 4, 4),
                "shared-depth test reads the first color target"))
        return false;
    return expect(target[0] == 0xff00ff00u,
                  "first target retains its depth-qualified color");
}

// Presentation-source contract for the headless software capture backend
// (bounded regression for the software-capture pin): while the
// guest has published no Portfolio scanout bitmap, the presented pixels are
// the *selected* CLT colour target -- the same surface both GL backends read --
// and a stored sibling carve (the clear plane the other buffer held) is never
// substituted for it. A published scanout bitmap still takes precedence over
// every colour target. This pins the audited mechanism: the capture that
// showed a flat upper viewport was not served from a sibling carve.
bool test_presentation_source_selection()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    const uint32_t clear_rgba = 64u | 0xff000000u;  // frame_begin(0.25, 0, 0)
    const uint32_t green_rgba = 0xff00ff00u;
    const uint32_t scanout_rgba = 0xff020304u;  // r=4 g=3 b=2

    backend.frame_begin(0.25f, 0.f, 0.f);
    backend.clt_target_select(0x1000u);
    std::array<m2host::CltVertex, 4> quad{};
    // Vertex order is x, y, u, v, r, g, b, a, w, one_over_w: opaque green.
    quad[0] = {0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    quad[1] = {4.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    quad[2] = {0.f, 4.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    quad[3] = {4.f, 4.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    backend.clt_draw(quad.data(), 4, true, false, m2host::kInvalidCel, 4.f,
                     4.f, false, {}, {}, {}, {});

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "presentation-source test reads the drawn target"))
        return false;
    if (!expect(frame[0] == green_rgba,
                "presentation serves the selected CLT colour target"))
        return false;

    // Selecting a sibling carve stores the drawn frame and loads the sibling's
    // own clear plane. The presented pixels must follow the selection: a
    // stored sibling is a colour target, not a presentation source.
    backend.clt_target_select(0x2000u);
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "presentation-source test reads the sibling target"))
        return false;
    if (!expect(frame[0] == clear_rgba,
                "presentation follows the selection, not a stored sibling"))
        return false;

    backend.clt_target_select(0x1000u);
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "presentation-source test reselects the drawn target"))
        return false;
    if (!expect(frame[0] == green_rgba,
                "stored target content survives a sibling excursion"))
        return false;

    std::array<uint32_t, 16> scanout;
    scanout.fill(scanout_rgba);
    backend.set_bitmap_scanout(scanout.data(), 4, 4);
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "presentation-source test reads the published scanout"))
        return false;
    if (!expect(frame[0] == scanout_rgba,
                "published scanout outranks the CLT colour target"))
        return false;
    backend.clear_bitmap_scanout();
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "presentation-source test reads after scanout withdrawal"))
        return false;
    return expect(frame[0] == green_rgba,
                  "withdrawing the scanout falls back to the selected target");
}

bool test_rgb_discard_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.25f, 0.f, 0.f);
    const uint32_t black = 0xff000000u;
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &black);

    std::array<m2host::CltVertex, 3> vertices{};
    vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[1] = {4.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[2] = {0.f, 4.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    m2host::CltDiscard discard;
    discard.rgb_zero = true;
    m2host::CltBlendState specular;
    specular.mode = m2host::CltBlendMode::specular_add;
    specular.constant_r = 1.f;
    backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f,
                     false, discard, {}, {}, specular);

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes RGB0 discard frame"))
        return false;
    if (!expect(frame[0] == (64u | 0xff000000u),
                "RGB0 discard occurs before destination-blender add"))
        return false;

    backend.frame_begin(0.f, 0.f, 0.f);
    const uint32_t green = 0xff000100u;
    const m2host::CelId nonblack = backend.cel_upload_rgba(1, 1, &green);
    backend.clt_draw(vertices.data(), 3, false, true, nonblack, 4.f, 4.f,
                     false, discard, {}, {}, {});
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes nonzero RGB frame"))
        return false;
    return expect(frame[0] == green,
                  "RGB0 preserves a nonblack post-texturing color");
}

bool test_alpha_discard_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    const std::array<m2host::CltVertex, 3> vertices = {{
        {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f},
        {4.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f},
        {0.f, 4.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f},
    }};
    m2host::CltDiscard discard;
    discard.alpha_zero = true;

    backend.frame_begin(0.25f, 0.f, 0.f);
    const uint32_t transparent = 0x00010203u;
    const m2host::CelId transparent_texture =
        backend.cel_upload_rgba(1, 1, &transparent);
    backend.clt_draw(vertices.data(), 3, false, true, transparent_texture,
                     4.f, 4.f, false, discard, {}, {}, {});
    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes ALPHA0 discard frame"))
        return false;
    if (!expect(frame[0] == (64u | 0xff000000u),
                "ALPHA0 discards a zero-alpha texel before the write"))
        return false;

    backend.frame_begin(0.25f, 0.f, 0.f);
    const uint32_t opaque = 0xff102030u;
    const m2host::CelId opaque_texture = backend.cel_upload_rgba(1, 1, &opaque);
    backend.clt_draw(vertices.data(), 3, false, true, opaque_texture, 4.f, 4.f,
                     false, discard, {}, {}, {});
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes the nonzero-alpha frame"))
        return false;
    return expect(frame[0] == opaque,
                  "ALPHA0 preserves a nonzero-alpha texel");
}

bool test_specular_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.f, 0.f, 0.f);
    const uint32_t texel = 0xff804020u;  // RGBA: 0x20, 0x40, 0x80, 0xff
    const m2host::CelId texture =
        backend.cel_upload_rgba(1, 1, &texel);

    std::array<m2host::CltVertex, 3> vertices{};
    vertices[0] = {0.f, 0.f, 0.f, 0.f, 0.5f, 0.25f, 0.75f, 0.5f, 1.f,
                   0.f};
    vertices[1] = {4.f, 0.f, 0.f, 0.f, 0.5f, 0.25f, 0.75f, 0.5f, 1.f,
                   0.f};
    vertices[2] = {0.f, 4.f, 0.f, 0.f, 0.5f, 0.25f, 0.75f, 0.5f, 1.f,
                   0.f};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::specular_add;
    blend.constant_r = 0.5f;
    blend.constant_g = 0.25f;
    blend.constant_b = 0.1f;
    backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f,
                     false, {}, {}, {}, blend);

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes specular test frame"))
        return false;
    return expect(frame[0] == (80u | (48u << 8u) | (109u << 16u) |
                               (128u << 24u)),
                  "software backend evaluates saturated M_DBSpec A+B");
}

// The M_DBFog composite evaluated by the software backend: write =
// TEXCOLOR*TEXALPHA + DBCONSTIN*(1 - TEXALPHA), where TEXALPHA is the
// per-vertex fog factor (the texture-stage alpha) and DBCONSTIN is the fog
// color. The factor drives a real lerp between the texture color and the fog
// color, so a half-fogged pixel is distinct from both endpoints -- a factor
// that was ignored (or clamped) would collapse onto one of them.
bool test_fog_composite_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    const uint32_t texel = 0xff804020u;  // RGBA: 0x20, 0x40, 0x80, 0xff
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);

    const auto draw_with_factor = [&](float factor, uint32_t& out) -> bool {
        backend.frame_begin(0.f, 0.f, 0.f);
        std::array<m2host::CltVertex, 3> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, factor, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, factor, 1.f, 0.f};
        vertices[2] = {0.f, 4.f, 0.f, 0.f, 1.f, 1.f, 1.f, factor, 1.f, 0.f};
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::fog_composite;
        blend.constant_r = 1.f;  // pure red fog color
        blend.constant_g = 0.f;
        blend.constant_b = 0.f;
        backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f,
                         false, {}, {}, {}, blend);
        std::array<uint32_t, 16> frame{};
        if (!backend.grab_presented_rgba(frame.data(), 4, 4)) return false;
        out = frame[0];
        return true;
    };

    uint32_t fogged = 0, mid = 0, clear = 0;
    if (!draw_with_factor(0.f, fogged)) return false;
    if (!draw_with_factor(0.5f, mid)) return false;
    if (!draw_with_factor(1.f, clear)) return false;

    // Unfogged (factor 1) is the texture color, opaque.
    if (!expect(clear == (32u | (64u << 8u) | (128u << 16u) | 0xff000000u),
                "fog factor 1 renders the unfogged texture color"))
        return false;
    // Fully fogged (factor 0) is the pure fog color, opaque.
    if (!expect(fogged == (255u | 0xff000000u),
                "fog factor 0 renders the pure fog color"))
        return false;
    // Half fogged (factor 0.5) is the exact lerp: (32,64,128)*0.5 +
    // (255,0,0)*0.5 = (143.5, 32, 64) -> (144, 32, 64).
    if (!expect(mid == (144u | (32u << 8u) | (64u << 16u) | 0xff000000u),
                "fog factor 0.5 renders the midpoint of the fog lerp"))
        return false;
    // Negative control: the fog factor must actually interpolate, not clamp to
    // a fully-fogged or unfogged endpoint.
    if (!expect(mid != fogged && mid != clear,
                "fog factor drives a real interpolation (negative control)"))
        return false;
    return true;
}

bool test_opaque_source_over_pixels()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    m2host::CltVertex vertices[3] = {
        {0, 0, 0, 0, 1, 1, 1, 1, 1, 0},
        {4, 0, 0, 0, 1, 1, 1, 1, 1, 0},
        {0, 4, 0, 0, 1, 1, 1, 1, 1, 0},
    };
    for (unsigned alpha : {0u, 128u, 255u}) {
        const uint32_t texel = (alpha << 24u) | 0x003264c8u;
        const auto texture = backend.cel_upload_rgba(1, 1, &texel);
        backend.frame_begin(0, 1, 0);
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::opaque_source_over;
        m2host::CltDiscard discard;
        discard.alpha_zero = true;
        backend.clt_draw(vertices, 3, false, true, texture, 4, 4,
                         false, discard, {}, {}, blend);
        std::array<uint32_t, 16> pixels{};
        if (!expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                    pixels[0] == 0xff3264c8u,
                    "opaque PRIMALPHA replaces destination regardless of texel alpha"))
            return false;
    }
    return true;
}

bool test_general_tab_equations()
{
    const m2host::CltTabColor primitive{.2f, .4f, .8f, .6f};
    const m2host::CltTabColor texture{.9f, .7f, .3f, .5f};
    const m2host::CltTabColor constant{32.f/255, 64.f/255, 128.f/255, 96.f/255};
    const m2host::CltTabColor colors[] = {
        {.6f,.6f,.6f,0}, primitive, {.5f,.5f,.5f,0}, texture,
        {96.f/255,96.f/255,96.f/255,0}, constant,
    };
    for (unsigned op : {0u, 1u}) for (unsigned a = 0; a < 6; ++a)
        for (unsigned b = 0; b < 6; ++b) for (unsigned t = 0; t < 6; ++t) {
            const uint32_t word = a | (b << 3) | (t << 6) | (2u << 9) | (op << 17);
            m2host::CltTabState state{true, word, {0x60204080u, 0x60204080u}};
            if (!expect(m2host::clt_tab_documented(word), "documented TAB inputs accepted")) return false;
            const auto got = m2host::clt_tab_evaluate(state, primitive, texture, false);
            for (unsigned c = 0; c < 3; ++c) {
                const float expected = op ? colors[a][c] * colors[b][c] :
                    colors[a][c] * (1-colors[t][c]) + colors[b][c] * colors[t][c];
                if (!expect(std::abs(got[c]-expected) < 1e-6f, "all TAB color selectors and operations")) return false;
            }
        }
    const float alpha[] = {.6f, .5f, 96.f/255};
    for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) {
        m2host::CltTabState state{true, (a << 11) | (b << 13) | (2u << 15),
                                  {0x60204080u, 0x60204080u}};
        const auto got = m2host::clt_tab_evaluate(state, primitive, texture, false);
        if (!expect(std::abs(got[3]-alpha[a]*alpha[b]) < 1e-6f, "all TAB alpha inputs")) return false;
    }
    return expect(!m2host::clt_tab_documented(6u | (2u << 9)), "reserved consumed selector rejected") &&
           expect(m2host::clt_tab_documented(6u | (7u << 3) | (7u << 6)), "bypassed selectors ignored");
}

bool test_general_dsb_interpreter()
{
    using namespace m2host;
    {
        SoftCaptureBackend backend(4,432,{});
        GuestCltInterpreter interpreter(&backend);
        interpreter.set_viewport(0,0,4,432,true,4,432);
        interpreter.set_fb_spans({{1u,0x10000u,4u*432u*4u}});
        auto triangle=[](std::vector<uint32_t>& words) {
            words.push_back(0x20090002u);
            for(auto xy:{std::array<float,2>{0,0},std::array<float,2>{4,0},std::array<float,2>{0,4}})
                for(float f:{xy[0],xy[1],200.f/255,100.f/255,40.f/255,64.f/255})
                    words.push_back(std::bit_cast<uint32_t>(f));
        };
        std::vector<uint32_t> write={0x10008008u,0x4fu,0x10008028u,0x10000u,
            0x10008050u,5u,0x10008058u,0x20u,0x1000805cu,0xffffffu,
            0x10008060u,0xffffffu,0x10008064u,0x60u,0x10008070u,0xe0u};
        triangle(write);
        auto written=interpreter.execute(write.data(),write.size());
        std::vector<uint32_t> read={0x10008008u,0x6fu,0x10008028u,0x20000u,
            0x10008030u,1u,0x10008034u,0x10000u,0x10008038u,4u,
            0x10008050u,2u,0x10008064u,0x24u,0x10008068u,0xffffffu,
            0x1000806cu,0x808080u,0x10008070u,0xd0u,0x10008078u,2u};
        triangle(read);
        auto result=interpreter.execute(read.data(),read.size());
        std::vector<uint32_t> pixels(4*432);
        if(!expect(written.ok && written.unsupported==0 && result.ok && result.unsupported==0 && result.draws==1 &&
            backend.grab_clt_target_rgba(0x20000u,pixels.data(),4,432) &&
            pixels[0]==0x40143264u,"rendered 32-bit Bitmap feeds alpha/DSB to addressed source consumer")) return false;
        interpreter.release_resources();
    }

    {
        RecordingBackend backend;
        GuestCltInterpreter interpreter(&backend);
        CltMemorySnapshot memory;
        CltMemoryRange range; range.guest_address=0x10000;
        range.bytes.resize(4u*432u*4u);
        for (size_t i=0;i<range.bytes.size();i+=4) {
            range.bytes[i]=0xa5; range.bytes[i+1]=0x12;
            range.bytes[i+2]=0x34; range.bytes[i+3]=0x56;
        }
        range.bytes[4]=0x00; // A7 zero, DSB zero
        range.bytes[8]=0x7f; // A7 maximum, DSB zero
        range.bytes[12]=0xff; // same alpha, DSB one
        memory.ranges.push_back(std::move(range));
        std::vector<uint32_t> words={0x10008008u,0x6fu,0x10008030u,1u,
            0x10008034u,0x10000u,0x10008038u,4u,
            0x10008058u,0x20u,0x10008064u,0x24u,
            0x10008068u,0xffffffu,0x1000806cu,0x808080u,
            0x10008070u,0xd0u,0x10008078u,2u,0x20090002u};
        const auto add_vertex=[&](float x,float y) {
            for(float f:{x,y,1.f,1.f,1.f,.5f}) words.push_back(std::bit_cast<uint32_t>(f));
        };
        add_vertex(0,0);add_vertex(4,0);add_vertex(0,4);
        words.push_back(0x20010000u);add_vertex(4,4);
        const auto result=interpreter.execute(words.data(),words.size(),&memory);
        if (!expect(result.ok && result.unsupported==0 && backend.draws.size()==2 && backend.uploads.size()==2,
            "32-bit source NEW/continuation reads four-byte pixels and reuses storage")) return false;
        if (!expect(backend.uploads[0].pixels[0]==0x4a563412u && backend.uploads[1].pixels[0]==0xff004affu,
            "BMTYPE_32 RGB, expanded A7 and DSB decode independently")) return false;
        if (!expect(backend.uploads[0].pixels[1]==0x00563412u &&
            backend.uploads[0].pixels[2]==0xff563412u && backend.uploads[0].pixels[3]==0xff563412u &&
            backend.uploads[1].pixels[2]==0xff00ff00u && backend.uploads[1].pixels[3]==0xff00ffffu,
            "32-bit alpha endpoints do not depend on DSB")) return false;
        for(const auto& draw:backend.draws)
            if(!expect(draw.blend.mode==CltBlendMode::general_secondary &&
                draw.blend.source_ssb_mask!=kInvalidCel && !draw.blend.alpha_control_unsupported &&
                draw.blend.general.dest_alpha==2,"32-bit source metadata reaches production blend")) return false;
        std::vector<uint32_t> format_change={0x10008030u,0u,0x10008078u,0u};
        const auto packet=std::find(words.begin(),words.end(),0x20090002u);
        format_change.insert(format_change.end(),packet,words.end());
        const auto changed=interpreter.execute(format_change.data(),format_change.size(),&memory);
        if(!expect(changed.ok && changed.unsupported==0 && backend.uploads.size()==4 &&
            backend.uploads[2].pixels[0]==0xff904048u,
            "source-format-only change invalidates the four-byte cached cel")) return false;
        interpreter.release_resources();
    }

    for (bool source : {false,true}) for (uint32_t op : {0x40u,0xb0u,0xe0u}) {
        RecordingBackend backend;
        GuestCltInterpreter interpreter(&backend);
        CltMemorySnapshot memory;
        CltMemoryRange range;
        range.guest_address=0x10000;
        range.bytes.resize(4*432*2,0x55);
        memory.ranges.push_back(std::move(range));
        std::vector<uint32_t> words = {
            0x10008008u, source ? 0x6fu : 0x40u,
            0x10008030u, 2u, 0x10008034u, 0x10000u, 0x10008038u, 4u,
            0x10008054u, 0x3366ccu,
            0x10008058u, 0x20u, 0x10008064u, source ? 0x20u : 0x60u,
            0x1000805cu, 0xffffffu, 0x10008060u, 0xffffffu,
            0x10008068u, 0xffffffu, 0x1000806cu, 0xffffffu,
            0x10008070u, op, 0x10008078u, 1u, 0x1000807cu, 0x003f00bfu,
            0x20090002u,
        };
        const auto vertex = [&](float x,float y) {
            for(float f : {x,y,.8f,.4f,.2f,.5f}) words.push_back(std::bit_cast<uint32_t>(f));
        };
        vertex(0,0);vertex(4,0);vertex(0,4);
        words.push_back(0x20010000u); vertex(4,4);
        const auto result=interpreter.execute(words.data(),words.size(),&memory);
        if (!expect(result.ok && result.unsupported==0 && backend.draws.size()==2,
            "general DSB NEW and continuation reach backend")) return false;
        for (const auto& draw : backend.draws) {
            if (!expect(draw.blend.mode==((source && op!=0xe0u) ? CltBlendMode::general_secondary : CltBlendMode::general_internal) &&
                !draw.blend.alpha_control_unsupported && !draw.blend.equation_approximate &&
                draw.blend.general.alu==op && draw.blend.general.alpha_constant==0x003f00bfu &&
                draw.vertices[0].r==.8f && draw.vertices[0].a==.5f && !draw.textured,
                "general DSB preserves primitive, raw equation, and independent alpha")) return false;
            if (source && op!=0xe0u && !expect(draw.blend.secondary_source!=kInvalidCel,
                "general DSB carries separate source")) return false;
        }
    }
    {
        RecordingBackend backend;
        GuestCltInterpreter interpreter(&backend);
        CltMemorySnapshot memory;
        CltMemoryRange range; range.guest_address=0x10000; range.bytes.resize(4*432*2,0x55);
        memory.ranges.push_back(std::move(range));
        std::vector<uint32_t> words={0x10008008u,0x66fu,0x10008030u,2u,
            0x10008034u,0x10000u,0x10008038u,4u,0x10008054u,0xffffffu,
            0x10008058u,0x70u,0x10008064u,2u,0x10008070u,0xe0u,
            0x10008078u,2u,0x20090002u};
        for (const auto& v : {std::array<float,6>{0,0,1,1,1,1},
            std::array<float,6>{4,0,1,1,1,1},std::array<float,6>{0,4,1,1,1,1}})
            for(float f:v) words.push_back(std::bit_cast<uint32_t>(f));
        const auto result=interpreter.execute(words.data(),words.size(),&memory);
        if (!expect(result.ok && result.unsupported==0 && backend.draws.size()==1 &&
            backend.draws[0].blend.mode==CltBlendMode::general_secondary &&
            backend.draws[0].blend.alpha_control_unsupported &&
            backend.draws[0].blend.general.dest_alpha==0 &&
            backend.draws[0].blend.dest_alpha.raw==2 &&
            backend.draws[0].blend.general.a==0x70,
            "live intro source tint preserves RGB with explicit written-alpha limitation")) return false;
    }
    return true;
}

bool test_clipped_menu_footer_feedback()
{
    constexpr uint32_t base = 0x4f0000u;
    constexpr uint32_t bytes = 576u * 432u * 2u;
    // Both footer widths slide across the bottom edge; guest RAM is black
    // while the renderer owns the green background the blend must preserve.
    for (const auto& [left, width, bias] : {
             std::array<int, 3>{24, 362, 0}, {162, 362, 0},
             {530, 362, 0}, {300, 210, 0}, {162, 362, 2}}) {
        m2host::SoftCaptureBackend backend(640, 480, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        std::vector<uint32_t> pixels(640u * 480u, 0xff00ff00u);
        backend.clt_target_upload_rgba(base, pixels.data(), 640, 480);
        m2host::GuestCltInterpreter interpreter(&backend);
        interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
        interpreter.set_fb_spans({{1u, base, bytes}});
        interpreter.set_te_surface_state(
            7u, 0u, 0u, base, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);

        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange range;
        range.guest_address = base;
        range.bytes.assign(bytes + 2u, 0u);
        memory.ranges.push_back(std::move(range));
        std::vector<uint32_t> words = {
            0x10006408u, 0u, 0x10006410u, 0u, // primitive color/alpha
            0x10008008u, 0x6fu, 0x1000800cu, 1u,
            0x10048030u, 2u, base + static_cast<uint32_t>(bias), 576u, 0u, 0u,
            0x10008050u, 2u, 0x10008058u, 0u, // source DSB, A = color * alpha
            0x10008064u, 2u, 0x10008070u, 0u, // B = source * (1 - alpha), A+B
            0x10008078u, 2u, // preserve source alpha, as the menu does
            0x20090003u,
        };
        for (const auto& xy : {
                 std::array<int, 2>{left, 404}, {left, 433},
                 {left + width, 404}, {left + width, 433}})
            for (float value : {float(xy[0]), float(xy[1]),
                                0.f, 0.f, 0.f, 64.f / 255.f})
                words.push_back(std::bit_cast<uint32_t>(value));

        const auto result = interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0u && result.draws == 2u &&
                        backend.grab_clt_target_rgba(base, pixels.data(), 640, 480),
                    "clipped menu footer reaches the addressed framebuffer"))
            return false;
        // A biased source still needs the sample bounds check; it must not
        // acquire the exact-origin exception and read the rendered target.
        const uint32_t expected = bias == 0 ? 0x00bf00u : 0u;
        if (!expect((pixels[452u * 640u + 33u + left] & 0xffffffu) == expected,
                    "moving footer blends over rendered pixels; biased sources stay guarded"))
            return false;
        interpreter.release_resources();
    }
    return true;
}

bool test_general_tab_interpreter()
{
    // PIP + MMDMA staging copied from the authenticated 3x1 resident fixture:
    // a 6-bit indexed texture published at base 0.
    const std::vector<uint32_t> staging_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x10006430u, 0x00020000u,
        0x10006408u, 0x00002000u,  // texture lookup enabled
        0x1000640cu, 0x00009200u,  // PIP supplies color/alpha/SSB
    };
    // Lerp primitive/texture color; multiply their independent alpha inputs.
    const std::vector<uint32_t> tab_words = {
        0x10006410u, 0x00012499u,
    };
    const std::vector<uint32_t> dsb_words = {
        0x10008008u, 0x00000000u,  // DBUSERCONTROL: destination blending disabled
        0x10008054u, 0x00808080u,  // DBCONSTIN: gray fog color
        0x10008058u, 0x00000000u,  // A = TEXCOLOR * TEXALPHA
        0x10008064u, 0x00000002u,  // B = SRCCOLOR * (1 - TEXALPHA)
        0x10008070u, 0x00000000u,  // A_PLUS_BCLAMP without a final divide
    };

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    pip_range.bytes = {0xff, 0x11, 0x22, 0x33, 0x3f, 0x44,
                       0x55, 0x66, 0x1f, 0x77, 0x88, 0x99};
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange texel_range;
    texel_range.guest_address = 0x200u;
    texel_range.bytes = {0x00u, 0x10u, 0x80u};
    memory.ranges.push_back(std::move(texel_range));

    const auto append_vertex = [](std::vector<uint32_t>& words, float x,
                                  float y, float a) {
        // Shaded textured perspective vertex: x, y, r, g, b, a, 1/w, u, v.
        for (float value : {x, y, 1.f, 1.f, 1.f, a, 1.f, 0.f, 0.f})
            words.push_back(std::bit_cast<uint32_t>(value));
    };


    for (bool general_dsb : {false, true})
    for (const uint32_t tab : {0x12499u, 0x2842bu, 0x34a2au}) {
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    auto words = staging_words;
    words.insert(words.end(), tab_words.begin(), tab_words.end());
    words.insert(words.end(), dsb_words.begin(), dsb_words.end());
    words.insert(words.end(), {0x10006410u, tab, 0x10006448u, 0x40204080u,
                              0x1000644cu, 0x60402010u});
    if (general_dsb) words.insert(words.end(), {
        0x10008008u, 0x40u, 0x10008058u, 0x20u, 0x10008064u, 0x60u,
        0x1000805cu, 0xffffffu, 0x10008060u, 0xffffffu,
        0x10008068u, 0xffffffu, 0x1000806cu, 0xffffffu,
        0x10008070u, 0xb0u}); // XOR internal texture/constant RGB
    words.push_back(0x200f0002u);
    append_vertex(words, 0, 0, .5f);
    append_vertex(words, 3, 0, .5f);
    append_vertex(words, 0, 3, .5f);
    words.push_back(0x20070000u);
    append_vertex(words, 3, 3, .25f);
    auto result = interpreter.execute(words.data(), words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && backend.draws.size() == 2,
                "general TAB NEW and continuation reach the backend")) return false;
    for (const auto& draw : backend.draws)
        if (!expect(draw.blend.tab.enabled && draw.blend.tab.control == tab &&
                    draw.vertices[0].a < 1.f &&
                    (!general_dsb || draw.blend.mode == m2host::CltBlendMode::general_internal),
                    "general TAB preserves primitive alpha and equation")) return false;
    // A later untextured source-tint packet inherits this resident texture,
    // but its RGB equation consumes only DBCONSTIN and the framebuffer.
    m2host::CltMemoryRange framebuffer;
    framebuffer.guest_address=0x10000; framebuffer.bytes.resize(4*432*2,0x55);
    memory.ranges.push_back(std::move(framebuffer));
    std::vector<uint32_t> tint={0x10008008u,0x66fu,0x10008030u,2u,
        0x10008034u,0x10000u,0x10008038u,4u,0x10008054u,0xffffffu,
        0x10008058u,0x70u,0x10008064u,2u,0x10008070u,0xe0u,
        0x10008078u,2u,0x10006410u,0xaadbu,0x1000643cu,0x408u,
        0x20090002u};
    for (const auto& v : {std::array<float,6>{0,0,1,1,1,1},
        std::array<float,6>{4,0,1,1,1,1},std::array<float,6>{0,4,1,1,1,1}})
        for(float f:v) tint.push_back(std::bit_cast<uint32_t>(f));
    const auto tinted=interpreter.execute(tint.data(),tint.size(),&memory);
    if (!expect(tinted.ok && tinted.unsupported==0 && tinted.draws==1 &&
        backend.draws.back().blend.mode==m2host::CltBlendMode::general_secondary &&
        !backend.draws.back().textured,
        "source tint does not consume incompatible inherited texture")) return false;
    memory.ranges.pop_back();
    }
    return true;
}

bool test_opaque_unarmed_source_over()
{
    // PIP + MMDMA staging copied from the authenticated 3x1 resident fixture:
    // a 6-bit indexed texture published at base 0.
    const std::vector<uint32_t> staging_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x10006430u, 0x00020000u,
        0x10006408u, 0x00002000u,  // texture lookup enabled
        0x1000640cu, 0x00009200u,  // PIP supplies color/alpha/SSB
    };
    // The accepted race TAB word: PRIMCOLOR * TEXCOLOR, ALPHAOUT=PRIMALPHA.
    const std::vector<uint32_t> tab_words = {
        0x10006410u, 0x00020419u,
    };
    const std::vector<uint32_t> dsb_words = {
        0x10008008u, 0x00000040u,  // DBUSERCONTROL: BLENDEN, SRCEN clear
        0x10008054u, 0x00808080u,  // DBCONSTIN: gray fog color
        0x10008058u, 0x00000000u,  // A = TEXCOLOR * TEXALPHA
        0x10008064u, 0x00000002u,  // B = SRCCOLOR * (1 - TEXALPHA)
        0x10008070u, 0x00000000u,  // A_PLUS_BCLAMP without a final divide
    };

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    pip_range.bytes = {0xff, 0x11, 0x22, 0x33, 0x3f, 0x44,
                       0x55, 0x66, 0x1f, 0x77, 0x88, 0x99};
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange texel_range;
    texel_range.guest_address = 0x200u;
    texel_range.bytes = {0x00u, 0x10u, 0x80u};
    memory.ranges.push_back(std::move(texel_range));

    const auto append_vertex = [](std::vector<uint32_t>& words, float x,
                                  float y, float a) {
        // Shaded textured perspective vertex: x, y, r, g, b, a, 1/w, u, v.
        for (float value : {x, y, 1.f, 1.f, 1.f, a, 1.f, 0.f, 0.f})
            words.push_back(std::bit_cast<uint32_t>(value));
    };


    for (int scenario = 0; scenario < 7; ++scenario) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        auto words = staging_words;
        words.insert(words.end(), tab_words.begin(), tab_words.end());
        words.insert(words.end(), dsb_words.begin(), dsb_words.end());
        // Fractional primitive alpha, texture-selected alpha, and a different
        // ALU must not gain permission from the opaque source-over reduction.
        if (scenario == 2) words.insert(words.end(), {0x10006410u, 0x00028419u});
        if (scenario == 3) words.insert(words.end(), {0x10008070u, 0x00000001u});
        if (scenario == 4) words.insert(words.end(), {0x10008078u, 1u});
        const float alpha = scenario == 1 ? 0.5f : 1.f;
        words.push_back(0x200f0002u);
        append_vertex(words, 0.f, 0.f, alpha);
        append_vertex(words, 3.f, 0.f, alpha);
        append_vertex(words, 0.f, 3.f, alpha);
        // Scenario 1 has an opaque new vertex but fractional retained slots.
        words.push_back(scenario == 6 ? 0x20170000u : 0x20070000u);
        append_vertex(words, 3.f, 3.f, scenario == 5 ? 0.5f : 1.f);
        auto result = interpreter.execute(words.data(), words.size(), &memory);
        const unsigned refused = scenario == 0 || scenario == 6 ? 0u :
                                 scenario == 5 ? 1u : 2u;
        if (!expect(result.ok && result.unsupported == refused &&
                    result.draws == 2u - refused,
                    "only wholly opaque PRIMALPHA source-over without SRCEN is accepted"))
            return false;
        for (const auto& draw : backend.draws)
            if (!expect(!draw.blend_enabled && draw.blend.mode ==
                        m2host::CltBlendMode::opaque_source_over,
                        "opaque reduction reaches backend with blending disabled"))
                return false;
    }
    return true;
}

// The per-vertex fog factor M_LightFog computes rides PRIMALPHA, and the
// world's accepted M_TBLitTex-style TAB form publishes it as the texture-stage
// alpha (`ALPHAOUT=PRIMALPHA`, raw 0x00020499). The M_DBFog distance lerp
// composes TEXCOLOR with that stage alpha, so the interpreter must hand the
// primitive alpha to the backend for the fog composite exactly as it already
// does for M_DBSpec. This fixture runs the production staging sequence (PIP +
// MMDMA publish + the accepted TAB word + the M_DBFog registers) through the
// interpreter, so classification, the TAB gate, and the vertex boundary are
// exercised together instead of calling `clt_draw` with a prebuilt blend
// state. Without the preservation the lerp weight collapses to 1 and enabling
// fog renders no distance fade.
bool test_fog_vertex_alpha_reaches_backend()
{
    // PIP + MMDMA staging copied from the authenticated 3x1 resident fixture:
    // a 6-bit indexed texture published at base 0.
    const std::vector<uint32_t> staging_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x10006430u, 0x00020000u,
        0x10006408u, 0x00002000u,  // texture lookup enabled
        0x1000640cu, 0x00009200u,  // PIP supplies color/alpha/SSB
    };
    // The accepted race TAB word: PRIMCOLOR * TEXCOLOR, ALPHAOUT=PRIMALPHA.
    const std::vector<uint32_t> tab_words = {
        0x10006410u, 0x00020499u,
    };
    const std::vector<uint32_t> fog_dsb_words = {
        0x10008008u, 0x00000040u,  // DBUSERCONTROL: BLENDEN, SRCEN clear
        0x10008054u, 0x00808080u,  // DBCONSTIN: gray fog color
        0x10008058u, 0x00000000u,  // A = TEXCOLOR * TEXALPHA
        0x10008064u, 0x00000042u,  // B = CONSTCOLOR * (1 - TEXALPHA)
        0x10008070u, 0x00000000u,  // A_PLUS_BCLAMP without a final divide
    };

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    pip_range.bytes = {0xff, 0x11, 0x22, 0x33, 0x3f, 0x44,
                       0x55, 0x66, 0x1f, 0x77, 0x88, 0x99};
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange texel_range;
    texel_range.guest_address = 0x200u;
    texel_range.bytes = {0x00u, 0x10u, 0x80u};
    memory.ranges.push_back(std::move(texel_range));

    const auto append_vertex = [](std::vector<uint32_t>& words, float x,
                                  float y, float a) {
        // Shaded textured perspective vertex: x, y, r, g, b, a, 1/w, u, v.
        for (float value : {x, y, 1.f, 1.f, 1.f, a, 1.f, 0.f, 0.f})
            words.push_back(std::bit_cast<uint32_t>(value));
    };

    // New-primitive arm: the shaded strip's per-vertex alphas are the fog
    // weights and must survive to the backend.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = staging_words;
        words.insert(words.end(), tab_words.begin(), tab_words.end());
        words.insert(words.end(), fog_dsb_words.begin(), fog_dsb_words.end());
        words.push_back(0x200f0002u);  // new shaded textured perspective strip
        append_vertex(words, 0.f, 0.f, 0.25f);
        append_vertex(words, 3.f, 0.f, 0.5f);
        append_vertex(words, 0.f, 3.f, 0.75f);
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0u &&
                        result.draws == 1u && backend.draws.size() == 1u,
                    "M_DBFog shaded texture reaches the production draw "
                    "boundary"))
            return false;
        if (!expect(backend.draws[0].blend.mode ==
                            m2host::CltBlendMode::fog_composite &&
                        !backend.draws[0].blend_enabled,
                    "the fixture draw selects the fog composite"))
            return false;
        if (!expect(backend.draws[0].vertices.size() == 3u &&
                        backend.draws[0].vertices[0].a == 0.25f &&
                        backend.draws[0].vertices[1].a == 0.5f &&
                        backend.draws[0].vertices[2].a == 0.75f,
                    "the per-vertex fog factor reaches the backend as the "
                    "fog lerp weight"))
            return false;
    }

    // Continuation arm: the continuation packet runs its own TAB decode and
    // must preserve the primitive alpha the same way.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = staging_words;
        words.insert(words.end(), tab_words.begin(), tab_words.end());
        words.insert(words.end(), fog_dsb_words.begin(), fog_dsb_words.end());
        words.push_back(0x200f0002u);
        append_vertex(words, 0.f, 0.f, 0.25f);
        append_vertex(words, 3.f, 0.f, 0.25f);
        append_vertex(words, 0.f, 3.f, 0.25f);
        words.push_back(0x20170000u);  // fan continuation, one vertex
        append_vertex(words, 3.f, 3.f, 0.6f);
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0u &&
                        result.draws == 2u && backend.draws.size() == 2u,
                    "M_DBFog fan continuation reaches the draw boundary"))
            return false;
        bool carried = false;
        for (const m2host::CltVertex& vertex : backend.draws[1].vertices) {
            if (vertex.a == 0.6f) carried = true;
            if (vertex.a >= 1.f) {
                expect(false,
                       "continuation fog alpha was replaced by 1.0");
                return false;
            }
        }
        if (!expect(carried,
                    "the continuation vertex fog weight reaches the backend"))
            return false;
    }

    // Negative control: the same TAB and vertex alphas with the destination
    // blend disabled keep the historical written alpha of 1.0, so the
    // preservation is gated on the recognized fog composite rather than a
    // blanket pass-through of whatever alpha the stream carried.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = staging_words;
        words.insert(words.end(), tab_words.begin(), tab_words.end());
        words.push_back(0x10008008u);
        words.push_back(0x00000000u);  // BLENDEN clear
        words.push_back(0x200f0002u);
        append_vertex(words, 0.f, 0.f, 0.25f);
        append_vertex(words, 3.f, 0.f, 0.5f);
        append_vertex(words, 0.f, 3.f, 0.75f);
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0u &&
                        result.draws == 1u && backend.draws.size() == 1u,
                    "the disabled-blend control draw reaches the backend"))
            return false;
        if (!expect(backend.draws[0].blend.mode ==
                            m2host::CltBlendMode::disabled &&
                        backend.draws[0].vertices.size() == 3u &&
                        backend.draws[0].vertices[0].a == 1.f &&
                        backend.draws[0].vertices[1].a == 1.f &&
                        backend.draws[0].vertices[2].a == 1.f,
                    "a non-fog destination equation still writes alpha 1.0 "
                    "(negative control)"))
            return false;
    }
    return true;
}

// The constant-color destination-blender equation evaluated by the software
// backend: write = DBCONSTIN * TEXALPHA + destination * (1 - TEXALPHA), where
// TEXALPHA is the texture-stage alpha (`TEXALPHA` in the guest's A/B
// multiplier words) and the destination is the bound source frame buffer.
bool test_constant_alpha_over_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(1.f, 1.f, 1.f);  // white destination pixel
    const uint32_t texel = 0x80402010u;  // r=0x10 g=0x20 b=0x40 alpha=0x80
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);

    std::array<m2host::CltVertex, 3> vertices{};
    vertices[0] = {0.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[1] = {4.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[2] = {0.f, 4.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::constant_alpha_over;
    blend.constant_r = 0.5f;
    blend.constant_g = 0.25f;
    blend.constant_b = 0.1f;
    backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f, true,
                     {}, {}, {}, blend);

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes the constant-blend frame"))
        return false;
    // alpha = texel alpha 0x80 = 128, destination = 255, constants 128/64/26:
    //   r = (128*128 + 255*127 + 127)/255 = 191
    //   g = ( 64*128 + 255*127 + 127)/255 = 159
    //   b = ( 26*128 + 255*127 + 127)/255 = 140
    const uint32_t expected = 191u | (159u << 8u) | (140u << 16u) |
                              0xff000000u;
    return expect(
        frame[0] == expected,
        "software backend evaluates the constant-color DSB equation");
}

// The B-only destination-blender equation evaluated by the software backend:
// write = destination * (1 - TEXCOLOR) with the texture-blender color (and
// alpha) in the fragment, matching the GL (ZERO, ONE_MINUS_SRC_COLOR) draw.
bool test_inverse_texture_multiply_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.5f, 0.5f, 0.5f);  // destination 0x80 per channel
    const uint32_t texel = 0x80402010u;  // r=0x10 g=0x20 b=0x40 alpha=0x80
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);

    std::array<m2host::CltVertex, 3> vertices{};
    vertices[0] = {0.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[1] = {4.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[2] = {0.f, 4.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::inverse_texture_multiply;
    backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f, true,
                     {}, {}, {}, blend);

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes the multiply-blend frame"))
        return false;
    // The texture color 16/32/64 and alpha 128 invert to 239/223/191/127.
    // The destination is 128 per color channel and 255 in alpha, so
    // (128*239+127)/255 = 120, (128*223+127)/255 = 112,
    // (128*191+127)/255 = 96 and (255*127+127)/255 = 127.
    const uint32_t expected = 120u | (112u << 8u) | (96u << 16u) |
                              (127u << 24u);
    return expect(frame[0] == expected,
                  "software backend multiplies the destination by the "
                  "inverted texture color");
}

// The identity-coefficient destination-blender equation evaluated by the
// software backend: write = SRCCOLOR, the bound source frame buffer pixel
// unchanged.  The write replaces the destination pixel, so the result must
// not depend on the destination or on the fragment's alpha the way the
// source-over lerp does.
bool test_dsb_source_copy_backend()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.5f, 0.5f, 0.5f);  // destination 0x80 per channel
    const uint32_t texel = 0x80402010u;  // r=0x10 g=0x20 b=0x40 alpha=0x80
    const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);

    std::array<m2host::CltVertex, 3> vertices{};
    vertices[0] = {0.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[1] = {4.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[2] = {0.f, 4.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f, true,
                     {}, {}, {}, blend);

    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "software backend exposes the copy-blend frame"))
        return false;
    // The fragment is the source texel verbatim; an alpha lerp against the
    // 0x80 destination would instead land on 0x48/0x50/0x60.
    if (!expect(frame[0] == texel,
                "software backend replaces the destination with the source "
                "frame buffer pixel"))
        return false;

    // Square transitions address framebuffer colour and their SSB-only mask
    // independently. Keep colour fixed on the red DB-source texel while the
    // mask crosses SSB0 -> SSB1. Reusing colour UVs for the mask would discard
    // the entire primitive, which is the production failure this guards.
    m2host::SoftCaptureBackend masked_backend(4, 2, {});
    masked_backend.frame_begin(0.f, 0.f, 0.f);
    const uint32_t source_texels[2] = {0xff0000ffu, 0xff00ff00u};
    const m2host::CelId source =
        masked_backend.cel_upload_rgba(2, 1, source_texels);
    const uint32_t mask_texels[2] = {0x00000000u, 0xffffffffu};
    const m2host::CelId mask =
        masked_backend.cel_upload_rgba(2, 1, mask_texels);
    std::array<m2host::CltVertex, 4> masked_vertices{};
    masked_vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f};
    masked_vertices[1] = {4.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f};
    masked_vertices[2] = {4.f, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f};
    masked_vertices[3] = {0.f, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f};
    masked_vertices[0].mask_u = 0.f;
    masked_vertices[1].mask_u = 1.f;
    masked_vertices[2].mask_u = 1.f;
    masked_vertices[3].mask_u = 0.f;
    m2host::CltDiscard discard;
    discard.ssb_zero = true;
    discard.ssb_mask = mask;
    discard.separate_ssb_uv = true;
    masked_backend.clt_draw(masked_vertices.data(), 4, false, true, source,
                            4.f, 2.f, true, discard, {}, {}, blend);
    std::array<uint32_t, 8> masked_frame{};
    if (!expect(masked_backend.grab_presented_rgba(masked_frame.data(), 4, 2),
                "software backend exposes separate-mask copy frame"))
        return false;
    for (int x = 0; x < 2; ++x) {
        if (!expect((masked_frame[x] & 0xffffffu) == 0u,
                    "SSB0 half preserves the destination"))
            return false;
    }
    for (int x = 2; x < 4; ++x) {
        if (!expect(masked_frame[x] == source_texels[0],
                    "SSB1 half copies DB-source colour, not mask RGB"))
            return false;
    }
    return true;
}

bool test_dsb_coefficient_weighted_mix_backend()
{
    // The weighted mix is a sum of two scaled inputs, not an alpha lerp, so
    // the test uses a fully transparent fragment: an alpha lerp would leave
    // the destination untouched, while the guest's equation still writes
    // `TEXCOLOR * a + SRCCOLOR * b`.
    const uint32_t texel = 0x00402010u;  // r=0x10 g=0x20 b=0x40 alpha=0
    const auto capture = [&](float tex_weight, float src_weight) {
        m2host::SoftCaptureBackend backend(4, 4, {});
        backend.frame_begin(0.5f, 0.5f, 0.5f);  // destination 0x80 per channel
        const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);
        std::array<m2host::CltVertex, 3> vertices{};
        vertices[0] = {0.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {0.f, 4.f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::coefficient_weighted_mix;
        blend.weight_tex_r = blend.weight_tex_g = blend.weight_tex_b =
            tex_weight;
        blend.weight_src_r = blend.weight_src_g = blend.weight_src_b =
            src_weight;
        backend.clt_draw(vertices.data(), 3, false, true, texture, 4.f, 4.f,
                         true, {}, {}, {}, blend);
        std::array<uint32_t, 16> frame{};
        if (!backend.grab_presented_rgba(frame.data(), 4, 4)) return 0xffffffffu;
        return frame[0];
    };

    // 0x80 weights on both sides: (0x10*128 + 0x80*128 + 127)/255 = 0x48 and
    // the same arithmetic for green (0x50) and blue (0x60), with the
    // texture-stage alpha written through.
    if (!expect(capture(128.f / 255.f, 128.f / 255.f) == 0x00605048u,
                "software backend sums the two weighted destination-blender "
                "inputs"))
        return false;

    // Only the source-side weight is set: the destination pixel carries the
    // whole result and the texture contributes nothing.
    if (!expect(capture(0.f, 128.f / 255.f) == 0x00404040u,
                "software backend scales the bound source input by its own "
                "weight"))
        return false;

    // Only the texture-side weight is set: the result is the scaled fragment
    // and the destination contributes nothing.
    if (!expect(capture(128.f / 255.f, 0.f) == 0x00201008u,
                "software backend scales the texture-blender output by its own "
                "weight"))
        return false;
    return true;
}

// Per-texel SSB coefficient selection: the guest's `setSsbBlendAttr` writes
// `arg2` into the entry-0 words and 0xffffff into the entry-1 words, so an
// SSB-carrying texture is two equations across the primitive -- entry 0 where
// the texel's SSB bit is 0, entry 1 where it is 1.  The software backend
// samples the texture's own SSB bitmap per fragment (the same bitmap the SSB
// discard test uses).  The 4x2 target maps pixel centers x=0..1 to texel 0
// (SSB 0) and x=2..3 to texel 1 (SSB 1) at u = 0, 1/3, 2/3, 1.
bool test_dsb_weighted_mix_ssb_entry_split()
{
    // The guest's own word set, resolved by destination_blend_state: A =
    // TEXCOLOR*CONST with entry 0 = 0x808080 and entry 1 = 0xffffff, B =
    // SRCCOLOR*CONSTCOMPLEMENT (so entry 0 is 1 - 0x80 and entry 1 is 0).
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::coefficient_weighted_mix;
    blend.weight_tex_r = blend.weight_tex_g = blend.weight_tex_b =
        128.f / 255.f;
    blend.weight_src_r = blend.weight_src_g = blend.weight_src_b =
        127.f / 255.f;
    blend.entry1_weight_tex_r = blend.entry1_weight_tex_g =
        blend.entry1_weight_tex_b = 1.f;
    blend.entry1_weight_src_r = blend.entry1_weight_src_g =
        blend.entry1_weight_src_b = 0.f;
    blend.weighted_mix_entry_dependent = true;
    blend.weighted_mix_ssb_selects_entry1 = true;

    // A 4-vertex fan covers the whole 4x2 target with u = x/4 and v = y/2,
    // so pixel centers x=0,1 sample texel 0 and x=2,3 sample texel 1 (the
    // texture is 2x1, nearest sampling, no wrap).
    const auto draw = [&](m2host::SoftCaptureBackend& backend,
                          const m2host::CelId texture,
                          const m2host::CelId ssb_mask, bool ssb_discard,
                          bool split) {
        std::array<m2host::CltVertex, 4> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {4.f, 2.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[3] = {0.f, 2.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        m2host::CltDiscard discard;
        if (ssb_discard) discard.ssb_zero = true;
        discard.ssb_mask = ssb_mask;
        m2host::CltBlendState state = blend;
        state.weighted_mix_ssb_selects_entry1 = split;
        backend.clt_draw(vertices.data(), 4, false, true, texture, 4.f, 2.f,
                         true, discard, {}, {}, state);
        std::array<uint32_t, 8> frame{};
        if (!backend.grab_presented_rgba(frame.data(), 4, 2))
            return false;
        // SSB-0 texels (x=0,1): entry 0 -> (0xff*128 + 0*127 + 127)/255 = 0x80
        // red.  SSB-1 texels (x=2,3): entry 1 -> the plain texture 0xff red.
        for (int x = 0; x < 2; ++x) {
            if (!expect((frame[x] & 0xffu) == 0x80u,
                        "SSB-0 texel reads coefficient entry 0 (the w "
                        "cross-fade)"))
                return false;
        }
        for (int x = 2; x < 4; ++x) {
            if (!expect((frame[x] & 0xffu) == 0xffu,
                        "SSB-1 texel reads coefficient entry 1 (the plain "
                        "texture)"))
                return false;
        }
        return true;
    };

    // Entry split alone: both entries evaluate per fragment.
    {
        m2host::SoftCaptureBackend backend(4, 2, {});
        backend.frame_begin(0.f, 0.f, 0.f);  // destination 0x00000000
        const uint32_t texels[2] = {0xff0000ffu, 0xff0000ffu};
        const m2host::CelId texture = backend.cel_upload_rgba(2, 1, texels);
        const uint32_t ssb_bits[2] = {0x00000000u, 0xffffffffu};
        const m2host::CelId ssb_mask = backend.cel_upload_rgba(2, 1, ssb_bits);
        if (!draw(backend, texture, ssb_mask, false, true)) return false;
    }

    // The SSB discard test still discards SSB-0 texels when armed alongside
    // the entry split; the two uses of the mask must not conflate.
    {
        m2host::SoftCaptureBackend backend(4, 2, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        const uint32_t texels[2] = {0xff0000ffu, 0xff0000ffu};
        const m2host::CelId texture = backend.cel_upload_rgba(2, 1, texels);
        const uint32_t ssb_bits[2] = {0x00000000u, 0xffffffffu};
        const m2host::CelId ssb_mask = backend.cel_upload_rgba(2, 1, ssb_bits);
        std::array<m2host::CltVertex, 4> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {4.f, 2.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[3] = {0.f, 2.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        m2host::CltDiscard discard;
        discard.ssb_zero = true;
        discard.ssb_mask = ssb_mask;
        backend.clt_draw(vertices.data(), 4, false, true, texture, 4.f, 2.f,
                         true, discard, {}, {}, blend);
        std::array<uint32_t, 8> split_frame{};
        if (!backend.grab_presented_rgba(split_frame.data(), 4, 2))
            return false;
        for (int x = 0; x < 2; ++x) {
            if (!expect((split_frame[x] & 0xffffffu) == 0u,
                        "the armed SSB0 discard still drops SSB-0 texels"))
                return false;
        }
        for (int x = 2; x < 4; ++x) {
            if (!expect((split_frame[x] & 0xffu) == 0xffu,
                        "the armed SSB0 discard leaves SSB-1 texels at entry 1"))
                return false;
        }
    }

    // A texel-independent draw (no set SSB bit anywhere) keeps the single
    // entry-0 evaluation; the split flag is the draw-time gate.
    {
        m2host::SoftCaptureBackend backend(4, 2, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        const uint32_t texels[2] = {0xff0000ffu, 0xff0000ffu};
        const m2host::CelId texture = backend.cel_upload_rgba(2, 1, texels);
        const uint32_t ssb_bits[2] = {0x00000000u, 0x00000000u};
        const m2host::CelId ssb_mask = backend.cel_upload_rgba(2, 1, ssb_bits);
        std::array<m2host::CltVertex, 4> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {4.f, 2.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[3] = {0.f, 2.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        m2host::CltDiscard discard;
        discard.ssb_mask = ssb_mask;
        backend.clt_draw(vertices.data(), 4, false, true, texture, 4.f, 2.f,
                         true, discard, {}, {}, blend);
        std::array<uint32_t, 8> single_frame{};
        if (!backend.grab_presented_rgba(single_frame.data(), 4, 2))
            return false;
        for (int x = 0; x < 4; ++x) {
            if (!expect((single_frame[x] & 0xffu) == 0x80u,
                        "a texel-independent weighted mix reads entry 0 "
                        "everywhere"))
                return false;
        }
    }
    return true;
}

bool test_ssb_mask_wrap_and_discard()
{
    // The SSB mask is part of the selected texel: when the primary texture
    // wraps, the SSB mask must sample the wrapped texel rather than clamping
    // to the texture edge. A repeating 4x1 texture with mask [1,0,1,0] sampled
    // at U=1.125 wraps to texel 0 (SSB=1, opaque white); clamping reaches
    // texel 3 (SSB=0) and discards the fragment.
    const uint32_t white[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    const uint32_t mask_bits[4] = {0xffffffffu, 0x00000000u, 0xffffffffu, 0x00000000u};

    const auto draw = [&](int target_w, int target_h,
                          int tex_w, int tex_h, const uint32_t* tex_px,
                          const uint32_t* mask_px,
                          float u, float v, bool wrap_u, bool wrap_v,
                          uint32_t u_period, uint32_t v_period,
                          bool ssb_discard, const m2host::CltBlendState* blend_state = nullptr) -> uint32_t {
        m2host::SoftCaptureBackend backend(target_w, target_h, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        const m2host::CelId tex = backend.cel_upload_rgba(tex_w, tex_h, tex_px);
        const m2host::CelId mask = backend.cel_upload_rgba(tex_w, tex_h, mask_px);
        std::array<m2host::CltVertex, 4> vertices{};
        vertices[0] = {0.f, 0.f, u, v, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {static_cast<float>(target_w), 0.f, u, v, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {static_cast<float>(target_w), static_cast<float>(target_h), u, v, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[3] = {0.f, static_cast<float>(target_h), u, v, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        m2host::CltDiscard discard;
        if (ssb_discard) discard.ssb_zero = true;
        discard.ssb_mask = mask;
        m2host::CltTextureSampling sampling;
        sampling.wrap_u = wrap_u;
        sampling.wrap_v = wrap_v;
        sampling.u_period = u_period;
        sampling.v_period = v_period;
        m2host::CltBlendState blend = blend_state ? *blend_state : m2host::CltBlendState{};
        if (blend_state && blend_state->weighted_mix_ssb_selects_entry1)
            blend.weighted_mix_ssb_mask = mask;
        backend.clt_draw(vertices.data(), 4, false, true, tex,
                         static_cast<float>(target_w), static_cast<float>(target_h),
                         true, discard, {}, sampling, blend);
        std::vector<uint32_t> frame(static_cast<size_t>(target_w) * target_h, 0u);
        if (!backend.grab_presented_rgba(frame.data(), target_w, target_h))
            return 0xdeadbeefu;
        return frame[0];
    };

    // 1. Wrapped U=1.125 on 4x1 texture: wraps to 0.125 (texel 0, SSB=1) -> opaque white.
    const uint32_t got_wrap_u = draw(4, 2, 4, 1, white, mask_bits, 1.125f, 0.5f, true, false, 4, 1, true);
    if (!expect(got_wrap_u == 0xffffffffu,
                "wrapped U=1.125 samples texel 0 (SSB=1, white) in soft backend"))
        return false;

    // 2. Negative control: wrapped U=1.375 wraps to 0.375 (texel 1, SSB=0) -> discarded (0).
    const uint32_t got_neg_u = draw(4, 2, 4, 1, white, mask_bits, 1.375f, 0.5f, true, false, 4, 1, true);
    if (!expect((got_neg_u & 0xffffffu) == 0u,
                "wrapped U=1.375 samples texel 1 (SSB=0, discarded) in soft backend"))
        return false;

    // 3. Clamped control: U=1.125 with wrap_u=false clamps to 1.0 (texel 3, SSB=0) -> discarded (0).
    const uint32_t got_clamp_u = draw(4, 2, 4, 1, white, mask_bits, 1.125f, 0.5f, false, false, 4, 1, true);
    if (!expect((got_clamp_u & 0xffffffu) == 0u,
                "clamped U=1.125 with wrap_u=false samples texel 3 (SSB=0, discarded) in soft backend"))
        return false;

    // 4. Unwrapped control: U=0.125 within [0, 1] samples texel 0 (SSB=1) -> white.
    const uint32_t got_direct_u = draw(4, 2, 4, 1, white, mask_bits, 0.125f, 0.5f, true, false, 4, 1, true);
    if (!expect(got_direct_u == 0xffffffffu,
                "unwrapped U=0.125 samples texel 0 (SSB=1, white) in soft backend"))
        return false;

    // 5. Vertical wrap: 1x4 texture with mask [1, 0, 1, 0] in V.
    const uint32_t got_wrap_v = draw(2, 4, 1, 4, white, mask_bits, 0.5f, 1.125f, false, true, 1, 4, true);
    if (!expect(got_wrap_v == 0xffffffffu,
                "wrapped V=1.125 samples texel 0 (SSB=1, white) in soft backend"))
        return false;

    const uint32_t got_clamp_v = draw(2, 4, 1, 4, white, mask_bits, 0.5f, 1.125f, false, false, 1, 4, true);
    if (!expect((got_clamp_v & 0xffffffu) == 0u,
                "clamped V=1.125 with wrap_v=false samples texel 3 (SSB=0, discarded) in soft backend"))
        return false;

    // 6. Weighted mix SSB coefficient selection:
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::coefficient_weighted_mix;
    blend.weight_tex_r = blend.weight_tex_g = blend.weight_tex_b = 128.f / 255.f;
    blend.weight_src_r = blend.weight_src_g = blend.weight_src_b = 0.f;
    blend.entry1_weight_tex_r = blend.entry1_weight_tex_g = blend.entry1_weight_tex_b = 1.f;
    blend.entry1_weight_src_r = blend.entry1_weight_src_g = blend.entry1_weight_src_b = 0.f;
    blend.weighted_mix_entry_dependent = true;
    blend.weighted_mix_ssb_selects_entry1 = true;

    // Wrapped U=1.125 has SSB=1 -> selects entry 1 -> 0xff.
    const uint32_t got_split_wrap = draw(4, 2, 4, 1, white, mask_bits, 1.125f, 0.5f, true, false, 4, 1, false, &blend);
    if (!expect((got_split_wrap & 0xffu) == 0xffu,
                "weighted mix wrapped U=1.125 selects entry 1 (SSB=1) in soft backend"))
        return false;

    // Clamped U=1.125 with wrap_u=false has SSB=0 -> selects entry 0 -> 0x80.
    const uint32_t got_split_clamp = draw(4, 2, 4, 1, white, mask_bits, 1.125f, 0.5f, false, false, 4, 1, false, &blend);
    if (!expect((got_split_clamp & 0xffu) == 0x80u,
                "weighted mix clamped U=1.125 selects entry 0 (SSB=0) in soft backend"))
        return false;

    return true;
}

bool test_masked_uv_lod_seam()
{
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr uint32_t kRedLOD0 = 0xff0000ffu;
    constexpr uint32_t kGreenLOD1 = 0xff00ff00u;
    constexpr uint32_t kBlueLOD2 = 0xffff0000u;

    const std::vector<uint32_t> red_8x8(8 * 8, kRedLOD0);
    const std::vector<uint32_t> green_4x4(4 * 4, kGreenLOD1);
    const std::vector<uint32_t> blue_2x2(2 * 2, kBlueLOD2);

    const std::array<m2host::CelLodLevel, 2> levels{{
        {4, 4, green_4x4.data()},
        {2, 2, blue_2x2.data()},
    }};

    m2host::SoftCaptureBackend backend(kWidth, kHeight, {});
    backend.frame_begin(0.f, 0.f, 0.f);
    const m2host::CelId tex = backend.cel_upload_rgba_lod_chain(
        8, 8, red_8x8.data(), levels.data(), levels.size(), true, false, 0, 0);
    if (!expect(tex != m2host::kInvalidCel, "8x8 LOD chain uploads in soft backend"))
        return false;

    // Draw strip with U spanning -1/64..63/64, V 0..1
    std::array<m2host::CltVertex, 4> vertices{};
    vertices[0] = {0.f, 0.f, -1.f / 64.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[1] = {static_cast<float>(kWidth), 0.f, 63.f / 64.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[2] = {0.f, static_cast<float>(kHeight), -1.f / 64.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    vertices[3] = {static_cast<float>(kWidth), static_cast<float>(kHeight), 63.f / 64.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};

    m2host::CltTextureSampling sampling;
    sampling.min_filter = m2host::CltTextureFilter::point;
    sampling.mag_filter = m2host::CltTextureFilter::point;
    sampling.inter_filter = m2host::CltTextureFilter::point;
    sampling.lod_max = 2;
    sampling.wrap_u = true;
    sampling.wrap_v = false;
    sampling.u_period = 4;
    sampling.v_period = 8;

    backend.clt_draw(vertices.data(), 4, true, true, tex,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     false, {}, {}, sampling, {});

    std::vector<uint32_t> frame(static_cast<size_t>(kWidth) * kHeight, 0u);
    if (!backend.grab_presented_rgba(frame.data(), kWidth, kHeight))
        return false;

    const size_t center = static_cast<size_t>(kHeight / 2) * kWidth + (kWidth / 2);
    const size_t interior_left = static_cast<size_t>(kHeight / 2) * kWidth + (kWidth / 4);
    const size_t interior_right = static_cast<size_t>(kHeight / 2) * kWidth + (3 * kWidth / 4);

    if (!expect(frame[center] == kRedLOD0,
                "seam center (32,32) under magnification selects red LOD0 in soft backend"))
        return false;
    if (!expect(frame[interior_left] == kRedLOD0,
                "interior left (16,32) selects red LOD0 in soft backend"))
        return false;
    if (!expect(frame[interior_right] == kRedLOD0,
                "interior right (48,32) selects red LOD0 in soft backend"))
        return false;

    return true;
}

bool test_dsb_secondary_source_alpha_backend()
{
    // Alpha zero must write the primary texture, while alpha one must write
    // the separately armed DB source. A generic source-over implementation
    // would leave the first case at the destination and write the texture on
    // the second.
    const auto run = [](uint32_t texture_rgba, uint32_t expected,
                        const char* label) {
        m2host::SoftCaptureBackend backend(1, 1, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        const m2host::CelId texture =
            backend.cel_upload_rgba(1, 1, &texture_rgba);
        const uint32_t source_rgba = 0xff503c28u;  // RGB(40,60,80)
        const m2host::CelId source =
            backend.cel_upload_rgba(1, 1, &source_rgba);
        std::array<m2host::CltVertex, 3> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {1.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        for (m2host::CltVertex& vertex : vertices) {
            vertex.mask_u = 0.f;
            vertex.mask_v = 0.f;
        }
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::secondary_source_alpha;
        blend.secondary_source = source;
        backend.clt_draw(vertices.data(), 3, false, true, texture, 1.f, 1.f,
                         true, {}, {}, {}, blend);
        std::array<uint32_t, 1> frame{};
        if (!expect(backend.grab_presented_rgba(frame.data(), 1, 1),
                    "secondary-source software draw presents"))
            return false;
        return expect(frame[0] == expected, label);
    };
    if (!run(0x003264c8u, 0x003264c8u,
             "reverse-alpha zero writes the primary texture") ||
        !run(0xff1e140au, 0xff503c28u,
             "reverse-alpha one writes the secondary DB source") ||
        !run(0x803264c8u, 0x80415078u,
             "reverse-alpha midpoint weights both source inputs"))
        return false;

    // The primary SSB discard and the secondary DB source have independent
    // coordinate spaces. Put the primary UV at the zero-side mask texel and
    // the DB UV at the one-side texel; a correct implementation discards,
    // while reusing the DB coordinates for SSB would keep the pixel.
    {
        m2host::SoftCaptureBackend backend(1, 1, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        const uint32_t primary_pixels[2] = {
            0xff102030u, 0xff102030u};
        const uint32_t source_pixels[2] = {
            0xff405060u, 0xff708090u};
        const uint32_t ssb_pixels[2] = {
            0x00000000u, 0xffffffffu};
        const m2host::CelId primary =
            backend.cel_upload_rgba(2, 1, primary_pixels);
        const m2host::CelId source =
            backend.cel_upload_rgba(2, 1, source_pixels);
        const m2host::CelId ssb = backend.cel_upload_rgba(2, 1, ssb_pixels);
        std::array<m2host::CltVertex, 3> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        for (m2host::CltVertex& vertex : vertices) {
            vertex.mask_u = 1.f;
            vertex.mask_v = 0.f;
        }
        m2host::CltBlendState blend;
        blend.mode = m2host::CltBlendMode::secondary_source_alpha;
        blend.secondary_source = source;
        m2host::CltDiscard discard;
        discard.ssb_zero = true;
        discard.ssb_mask = ssb;
        backend.clt_draw(vertices.data(), 3, false, true, primary, 1.f, 1.f,
                         true, discard, {}, {}, blend);
        std::array<uint32_t, 1> frame{};
        if (!expect(backend.grab_presented_rgba(frame.data(), 1, 1) &&
                        frame[0] == 0xff000000u,
                    "secondary-source SSB discard keeps primary coordinates"))
            return false;
    }
    return true;
}

bool test_dsb_secondary_source_interpreter()
{
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const uint32_t source_base = 0x1000u;
    const size_t source_bytes = 576u * 432u * 2u;
    const std::vector<uint32_t> words = {
        // A small 4x2 indexed primary texture.
        0x10006404u, 0x00000200u,
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000020u,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000408u,
        0x10006404u, 0x00000000u,
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,
        0x1000642cu, 0x00200020u,
        0x10000014u, 0x00000001u,
        0x1000640cu, 0x00009200u,
        0x1000643cu, 0x00000408u,
        0x10006408u, 0x00002000u,
        0x10006410u, 0x00009200u,
        // StreamEventFunc's reverse-alpha DB equation and armed source.
        0x10008008u, 0x0000006fu,
        0x1000800cu, 0x00000000u,
        0x10008030u, 0x00000000u,
        0x10008034u, source_base,
        0x10008038u, 0x00000240u,
        0x1000803cu, 0x00000000u,
        0x10008068u, 0x00ffffffu,
        0x1000806cu, 0x00ffffffu,
        0x10008058u, 0x00000002u,
        0x10008064u, 0x00000000u,
        0x10008070u, 0x00000010u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_words = words;
    const float vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    for (const auto& vertex : vertices)
        for (float value : vertex)
            complete_words.push_back(std::bit_cast<uint32_t>(value));

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip;
    pip.guest_address = 0x100u;
    pip.bytes.resize(0x20u);
    for (size_t i = 0; i < pip.bytes.size(); i += 4u) {
        pip.bytes[i + 0u] = 0xffu;
        pip.bytes[i + 1u] = 0x11u;
        pip.bytes[i + 2u] = 0x22u;
        pip.bytes[i + 3u] = 0x33u;
    }
    memory.ranges.push_back(std::move(pip));
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes = {0u, 1u, 2u, 3u, 3u, 2u, 1u, 0u};
    memory.ranges.push_back(std::move(texels));
    m2host::CltMemoryRange source;
    source.guest_address = source_base;
    source.bytes.assign(source_bytes, 0u);
    memory.ranges.push_back(std::move(source));

    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.draws.size() == 1,
                "StreamEventFunc secondary-source draw reaches backend"))
        return false;
    const auto& draw = backend.draws.front();
    if (!expect(draw.blend.mode ==
                    m2host::CltBlendMode::secondary_source_alpha &&
                    draw.blend.secondary_source != m2host::kInvalidCel,
                "secondary-source mode carries the armed DB cel"))
        return false;
    if (!expect(draw.vertices.size() == 3 &&
                    std::abs(draw.vertices[1].mask_u - 4.f / 576.f) < 1e-6f &&
                    std::abs(draw.vertices[2].mask_v - 1.f / 432.f) < 1e-6f,
                "secondary-source vertices carry DB linear coordinates"))
        return false;

    // Shooter's forward-alpha form takes B from a separate raw background,
    // not the current destination. Swap the two coefficient selector words.
    auto forward_words = complete_words;
    for (size_t i = 0; i < words.size() - 1; i += 2) {
        if (forward_words[i] == 0x10008058u) forward_words[i + 1] = 0u;
        if (forward_words[i] == 0x10008064u) forward_words[i + 1] = 2u;
    }
    RecordingBackend forward_backend;
    m2host::GuestCltInterpreter forward_interpreter(&forward_backend);
    const auto forward_result = forward_interpreter.execute(
        forward_words.data(), forward_words.size(), &memory);
    if (!expect(forward_result.ok && forward_result.unsupported == 0 &&
                forward_backend.draws.size() == 1 &&
                forward_backend.draws[0].blend.mode ==
                    m2host::CltBlendMode::secondary_source_over &&
                forward_backend.draws[0].blend.secondary_source != 0,
                "forward-alpha material uses the guest's separate DB source"))
        return false;
    auto constant_words = forward_words;
    for (size_t i = 0; i < words.size() - 1; i += 2)
        if (constant_words[i] == 0x10008058u) constant_words[i + 1] = 0x40u;
    RecordingBackend constant_backend;
    m2host::GuestCltInterpreter constant_interpreter(&constant_backend);
    const auto constant_result = constant_interpreter.execute(
        constant_words.data(), constant_words.size(), &memory);
    if (!expect(constant_result.ok && constant_result.unsupported == 0 &&
                constant_backend.draws.size() == 1 &&
                constant_backend.draws[0].blend.mode ==
                    m2host::CltBlendMode::secondary_constant_over,
                "cloud constant-alpha form uses the separate background")) return false;
    // The live shooter2d background list authors the same cloud words but
    // never writes DBALUCNTL, so the draw runs with the font folio's
    // inherited A_PLUS_BCLAMP (0x0) -- the vendor M_DBTrans word.  It must
    // reach the same secondary_constant_over evaluation with the separate
    // background, not the disclosed source-over approximation.
    auto inherited_cloud_words = constant_words;
    for (size_t i = 0; i < words.size() - 1; i += 2)
        if (inherited_cloud_words[i] == 0x10008070u)
            inherited_cloud_words[i + 1] = 0x0u;
    RecordingBackend inherited_cloud_backend;
    m2host::GuestCltInterpreter inherited_cloud_interpreter(
        &inherited_cloud_backend);
    const auto inherited_cloud_result = inherited_cloud_interpreter.execute(
        inherited_cloud_words.data(), inherited_cloud_words.size(), &memory);
    if (!expect(inherited_cloud_result.ok &&
                    inherited_cloud_result.unsupported == 0 &&
                    inherited_cloud_backend.draws.size() == 1 &&
                    inherited_cloud_backend.draws[0].blend.mode ==
                        m2host::CltBlendMode::secondary_constant_over &&
                    inherited_cloud_backend.draws[0].blend.secondary_source !=
                        m2host::kInvalidCel &&
                    !inherited_cloud_backend.draws[0]
                         .blend.equation_approximate,
                "inherited A_PLUS_BCLAMP cloud form uses the separate "
                "background"))
        return false;

    // The guest may continue the same TE state with a fan continuation. It
    // must take the secondary-source materialization and independent DB
    // coordinates through the continuation path as well as the NEW path.
    std::vector<uint32_t> continuation_words = complete_words;
    continuation_words.push_back(0x20170000u);  // fan continuation, textured
    const float continuation_vertex[9] = {
        4.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 4.f, 0.f};
    for (float value : continuation_vertex)
        continuation_words.push_back(std::bit_cast<uint32_t>(value));
    auto forward_continuation = forward_words;
    forward_continuation.insert(forward_continuation.end(),
        continuation_words.begin() + complete_words.size(), continuation_words.end());
    RecordingBackend forward_cont_backend;
    m2host::GuestCltInterpreter forward_cont_interpreter(&forward_cont_backend);
    const auto forward_cont_result = forward_cont_interpreter.execute(
        forward_continuation.data(), forward_continuation.size(), &memory);
    if (!expect(forward_cont_result.ok && forward_cont_result.unsupported == 0 &&
                forward_cont_backend.draws.size() == 2 &&
                forward_cont_backend.draws[1].blend.mode ==
                    m2host::CltBlendMode::secondary_source_over,
                "forward-alpha material survives fan continuation")) return false;
    RecordingBackend continuation_backend;
    m2host::GuestCltInterpreter continuation_interpreter(&continuation_backend);
    const m2host::CltExecutionResult continuation_result =
        continuation_interpreter.execute(
            continuation_words.data(), continuation_words.size(), &memory);
    if (!expect(continuation_result.ok &&
                    continuation_result.unsupported == 0 &&
                    continuation_result.draws == 2 &&
                    continuation_backend.draws.size() == 2,
                "secondary-source state reaches textured fan continuations"))
        return false;
    if (!expect(continuation_backend.draws[1].blend.mode ==
                        m2host::CltBlendMode::secondary_source_alpha &&
                    continuation_backend.draws[1].blend.secondary_source ==
                        draw.blend.secondary_source &&
                    std::abs(continuation_backend.draws[1]
                                 .vertices[1]
                                 .mask_u -
                             4.f / 576.f) < 1e-6f,
                "secondary-source continuation carries DB coordinates and "
                "source identity"))
        return false;

    // Guest-RAM DB-source X overflow must follow the linear source address,
    // not clamp at the rightmost texel. Use a 4-pixel source, X offset +2,
    // and a [0,5)x[0,1) quad: the last three destination pixels must come
    // from source row 1. The alpha-one primary makes the secondary RGB
    // directly observable in the software backend.
    {
        const uint32_t overflow_base = 0x3000u;
        const size_t overflow_header = words.size() - 1u;
        std::vector<uint32_t> overflow_words = words;
        for (size_t i = 0; i < overflow_header; i += 2u) {
            if (overflow_words[i] == 0x10008034u)
                overflow_words[i + 1u] = overflow_base;
            else if (overflow_words[i] == 0x10008038u)
                overflow_words[i + 1u] = 4u;
            else if (overflow_words[i] == 0x1000803cu)
                overflow_words[i + 1u] = 0x00020000u;  // (+2, 0)
        }
        overflow_words.resize(overflow_header);
        overflow_words.push_back(0x201a0003u);  // four-vertex textured fan
        const float overflow_vertices[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {5.f, 0.f, 5.f, 0.f},
            {5.f, 1.f, 5.f, 1.f},
            {0.f, 1.f, 0.f, 1.f},
        };
        for (const auto& vertex : overflow_vertices)
            for (float value : vertex)
                overflow_words.push_back(std::bit_cast<uint32_t>(value));

        m2host::CltMemorySnapshot overflow_memory = memory;
        bool replaced_source = false;
        for (auto& range : overflow_memory.ranges) {
            if (range.guest_address != source_base) continue;
            range.guest_address = overflow_base;
            range.bytes.assign(4u * 432u * 2u, 0u);
            const uint8_t red5[2][4] = {
                {0u, 4u, 8u, 12u},
                {16u, 20u, 24u, 28u},
            };
            for (uint32_t y = 0; y < 432u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint16_t word = static_cast<uint16_t>(
                        (y < 2u ? red5[y][x] : 0u) << 10u);
                    const size_t at = (static_cast<size_t>(y) * 4u + x) * 2u;
                    range.bytes[at] = static_cast<uint8_t>(word >> 8u);
                    range.bytes[at + 1u] = static_cast<uint8_t>(word);
                }
            }
            range.content_hash = 0;
            replaced_source = true;
            break;
        }
        if (!expect(replaced_source,
                    "overflow fixture replaces the guest DB-source window"))
            return false;

        RecordingBackend overflow_backend;
        m2host::GuestCltInterpreter overflow_interpreter(&overflow_backend);
        const m2host::CltExecutionResult overflow_result =
            overflow_interpreter.execute(overflow_words.data(),
                                         overflow_words.size(),
                                         &overflow_memory);
        if (!expect(overflow_result.ok && overflow_result.unsupported == 0 &&
                        overflow_result.draws == 2 &&
                        overflow_backend.draws.size() == 2,
                    "guest-RAM secondary source overflow reaches the backend"))
            return false;

        bool found_carried_vertex = false;
        for (const auto& recorded : overflow_backend.draws) {
            for (const auto& vertex : recorded.vertices) {
                if (std::abs(vertex.x - 5.f) < 1e-6f &&
                    std::abs(vertex.y) < 1e-6f) {
                    found_carried_vertex = true;
                    if (!expect(std::abs(vertex.mask_u - 7.f / 4.f) <
                                        1e-6f &&
                                    std::abs(vertex.mask_v) < 1e-6f,
                                "guest-RAM X overflow retains the unbounded "
                                "linear source coordinate"))
                        return false;
                }
            }
        }
        if (!expect(found_carried_vertex,
                    "overflow fixture retains its right-edge vertex"))
            return false;

        const uint32_t red5[2][4] = {
            {0u, 4u, 8u, 12u},
            {16u, 20u, 24u, 28u},
        };
        std::vector<uint32_t> source_pixels(4u * 432u, 0xff000000u);
        for (uint32_t y = 0; y < 2u; ++y) {
            for (uint32_t x = 0; x < 4u; ++x) {
                const uint32_t red = m2host::clt_db_source_expand_5_to_8(
                    red5[y][x], false);
                source_pixels[y * 4u + x] = red | 0xff000000u;
            }
        }
        m2host::SoftCaptureBackend soft(5, 1, {});
        soft.frame_begin(0.f, 0.f, 0.f);
        const uint32_t primary_pixel = 0xffffffffu;
        const m2host::CelId primary = soft.cel_upload_rgba(
            1, 1, &primary_pixel);
        const m2host::CelId source = soft.cel_upload_rgba(
            4, 432, source_pixels.data());
        std::vector<m2host::CltVertex> all_vertices;
        for (const auto& recorded : overflow_backend.draws)
            all_vertices.insert(all_vertices.end(), recorded.vertices.begin(),
                                recorded.vertices.end());
        for (auto& vertex : all_vertices) {
            vertex.u = 0.f;
            vertex.v = 0.f;
            vertex.r = vertex.g = vertex.b = vertex.a = 1.f;
            vertex.w = 1.f;
            vertex.one_over_w = 0.f;
        }
        m2host::CltBlendState overflow_blend =
            overflow_backend.draws.front().blend;
        if (!expect(overflow_blend.mode ==
                        m2host::CltBlendMode::secondary_source_alpha &&
                    overflow_blend.db_source_linear &&
                    std::abs(overflow_blend.db_source_height - 432.f) < 1e-6f,
                    "overflow fixture carries linear DB-source state to the "
                    "backend"))
            return false;
        const m2host::CltDbSourceSampleUv underflow_sample =
            m2host::clt_db_source_sample_uv(-0.25f, 0.f, overflow_blend);
        if (!expect(std::abs(underflow_sample.u - 0.75f) < 1e-6f &&
                        std::abs(underflow_sample.v + 1.f / 432.f) < 1e-6f,
                    "DB-source negative X underflow carries to the previous "
                    "row"))
            return false;
        overflow_blend.secondary_source = source;
        soft.clt_draw(all_vertices.data(), all_vertices.size(), false, true,
                      primary, 5.f, 1.f, true, {}, {}, {}, overflow_blend,
                      true);
        std::array<uint32_t, 5> overflow_frame{};
        if (!expect(soft.grab_presented_rgba(overflow_frame.data(), 5, 1),
                    "overflow fixture presents secondary pixels"))
            return false;
        const std::array<uint32_t, 5> expected_overflow = {
            source_pixels[2], source_pixels[3], source_pixels[4],
            source_pixels[5], source_pixels[6]};
        if (!expect(overflow_frame == expected_overflow,
                    "guest-RAM X overflow samples the next source row"))
            return false;
    }

    // The reverse-alpha RGB equation is not sufficient to authorize an
    // unmodelled written-alpha selector. Preserve the source-backed mode long
    // enough for the draw boundary to reject it; silently turning it into
    // source-over would reintroduce the same destination substitution.
    std::vector<uint32_t> alpha_control_words = words;
    alpha_control_words.insert(alpha_control_words.end() - 1,
                               {0x10008078u, 0x2u});
    std::vector<uint32_t> alpha_control_complete = alpha_control_words;
    for (const auto& vertex : vertices)
        for (float value : vertex)
            alpha_control_complete.push_back(std::bit_cast<uint32_t>(value));
    RecordingBackend alpha_backend;
    m2host::GuestCltInterpreter alpha_interpreter(&alpha_backend);
    const m2host::CltExecutionResult alpha_result =
        alpha_interpreter.execute(alpha_control_complete.data(),
                                  alpha_control_complete.size(), &memory);
    if (!expect(alpha_result.ok && alpha_result.unsupported == 1 &&
                    alpha_result.draws == 0 && alpha_backend.draws.empty(),
                "secondary-source blend with an unsupported alpha control "
                "stays fail-closed"))
        return false;
    return true;
}

// The destination-alpha control registers the guest statically authors
// (`DBSRCALPHACNTL` 0x8074, `DBDESTALPHACNTL` 0x8078, `DBDESTALPHACONST`
// 0x807c) are decoded with the checked-in SDK field positions.  The guest's
// values are leave-alone clamp plus TEXALPHA destination source, which the
// production weighted-mix evaluation already honours; any other documented
// selection stays fail-closed (downgraded to the recovered source-over
// default) rather than guessed.
bool test_dsb_blend_plan_parity()
{
    // Cross-backend parity at the guest-visible boundary: for every decoded
    // blend mode the software backend's written pixel must equal the shared
    // GL plan's arithmetic (clt_gl_blend_plan in cel_blitter.h).  The GL
    // backends consume that same plan structurally, so this pins all three
    // paths to one equation evaluation with no duplicated mapping.
    const auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return r | (g << 8u) | (b << 16u) | (a << 24u);
    };
    struct Case {
        m2host::CltBlendMode mode;
        uint32_t dst;
        uint32_t texel;
        m2host::CltBlendState blend;
        uint32_t expected;
    };
    m2host::CltBlendState over;
    over.mode = m2host::CltBlendMode::source_over;
    m2host::CltBlendState inverse;
    inverse.mode = m2host::CltBlendMode::inverse_texture_multiply;
    m2host::CltBlendState copy;
    copy.mode = m2host::CltBlendMode::source_copy;
    m2host::CltBlendState weighted;
    weighted.mode = m2host::CltBlendMode::coefficient_weighted_mix;
    weighted.weight_src_r = weighted.weight_src_g =
        weighted.weight_src_b = 0.5f;
    weighted.weight_tex_r = weighted.weight_tex_g = weighted.weight_tex_b =
        1.f;
    m2host::CltBlendState constant_over;
    constant_over.mode = m2host::CltBlendMode::constant_alpha_over;
    constant_over.constant_r = 0.75f;
    constant_over.constant_g = 0.5f;
    constant_over.constant_b = 0.25f;

    const std::array<Case, 5> cases = {{
        // source_over: dst*(1-a) + src*a with a = 0.5:
        // (40,60,80)/2 + (200,100,50)/2 = (120,80,65).
        {m2host::CltBlendMode::source_over, rgba(40, 60, 80, 255),
         rgba(200, 100, 50, 128), over, rgba(120, 80, 65, 255)},
        // inverse_texture_multiply: RGB dst*(1-src color); alpha follows the
        // separate texture-alpha factor (the GL plan uses ZERO,
        // ONE_MINUS_SRC_ALPHA for the alpha slots).
        {m2host::CltBlendMode::inverse_texture_multiply,
         rgba(200, 100, 50, 255), rgba(64, 128, 192, 255), inverse,
         rgba(150, 50, 12, 0)},
        // source_copy: the fragment replaces the pixel, post-texturing
        // alpha included.
        {m2host::CltBlendMode::source_copy, rgba(10, 20, 30, 255),
         rgba(90, 140, 200, 60), copy, rgba(90, 140, 200, 60)},
        // coefficient_weighted_mix: src*1 + dst*0.5 = (130,160,40).
        {m2host::CltBlendMode::coefficient_weighted_mix,
         rgba(100, 200, 40, 255), rgba(80, 60, 20, 255), weighted,
         rgba(130, 160, 40, 255)},
        // constant_alpha_over: the fragment color is the DBCONSTIN constant
        // (0.75,0.5,0.25); the software backend's byte-rounded 0.5 lerp
        // yields (106,84,62).
        {m2host::CltBlendMode::constant_alpha_over, rgba(20, 40, 60, 255),
         rgba(0, 0, 0, 128), constant_over, rgba(106, 84, 62, 255)},
    }};
    for (const Case& test_case : cases) {
        m2host::SoftCaptureBackend backend(1, 1, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        // Seed the 1x1 destination with the case's color via an unblended
        // flat (untextured) triangle whose vertex colors are the dst value.
        const float dr =
            static_cast<float>(test_case.dst & 0xffu) / 255.f;
        const float dg =
            static_cast<float>((test_case.dst >> 8u) & 0xffu) / 255.f;
        const float db =
            static_cast<float>((test_case.dst >> 16u) & 0xffu) / 255.f;
        std::array<m2host::CltVertex, 3> seed{};
        seed[0] = {0.f, 0.f, 0.f, 0.f, dr, dg, db, 1.f, 1.f, 0.f};
        seed[1] = {1.f, 0.f, 1.f, 0.f, dr, dg, db, 1.f, 1.f, 0.f};
        seed[2] = {0.f, 1.f, 0.f, 1.f, dr, dg, db, 1.f, 1.f, 0.f};
        backend.clt_draw(seed.data(), 3, false, false, m2host::kInvalidCel,
                         1.f, 1.f, false, {}, {}, {}, {});
        const uint32_t texel = test_case.texel;
        const m2host::CelId texture = backend.cel_upload_rgba(1, 1, &texel);
        std::array<m2host::CltVertex, 3> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {1.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        backend.clt_draw(vertices.data(), 3, false, true, texture, 1.f, 1.f,
                         true, {}, {}, {}, test_case.blend);
        std::array<uint32_t, 1> frame{};
        if (!expect(backend.grab_presented_rgba(frame.data(), 1, 1),
                    "software backend presents the parity frame"))
            return false;
        if (!expect(frame[0] == test_case.expected,
                    "software pixel equals the shared blend-plan arithmetic "
                    "for its decoded mode")) {
            std::fprintf(stderr, "  [parity] mode=%d frame=%08x expected=%08x\n",
                         int(test_case.mode), frame[0], test_case.expected);
            return false;
        }
    }

    // The shared plan itself: every decoded mode maps to the documented
    // fixed-function factors, so an edit cannot silently desync the two GL
    // backends' contracts from the equation the software backend evaluates.
    const auto check_plan = [&](m2host::CltBlendMode mode,
                                m2host::CltGlBlendFactor src,
                                m2host::CltGlBlendFactor dst,
                                m2host::CltGlBlendFactor src_a,
                                m2host::CltGlBlendFactor dst_a,
                                bool enabled, bool uses_constant,
                                bool restores) {
        const m2host::CltGlBlendPlan plan = m2host::clt_gl_blend_plan(mode);
        return expect(plan.src_factor == src && plan.dst_factor == dst &&
                          plan.src_alpha_factor == src_a &&
                          plan.dst_alpha_factor == dst_a &&
                          plan.blend_enabled == enabled &&
                          plan.uses_constant_color == uses_constant &&
                          plan.restores == restores,
                      "shared GL blend plan matches its documented factors");
    };
    if (!check_plan(m2host::CltBlendMode::disabled,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    false, false, false))
        return false;
    if (!check_plan(m2host::CltBlendMode::source_over,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    true, false, false))
        return false;
    if (!check_plan(m2host::CltBlendMode::constant_alpha_over,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    true, false, false))
        return false;
    if (!check_plan(m2host::CltBlendMode::specular_add,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    false, false, false))
        return false;
    if (!check_plan(m2host::CltBlendMode::inverse_texture_multiply,
                    m2host::CltGlBlendFactor::zero,
                    m2host::CltGlBlendFactor::one_minus_src_color,
                    m2host::CltGlBlendFactor::zero,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    true, false, true))
        return false;
    if (!check_plan(m2host::CltBlendMode::source_copy,
                    m2host::CltGlBlendFactor::one,
                    m2host::CltGlBlendFactor::zero,
                    m2host::CltGlBlendFactor::one,
                    m2host::CltGlBlendFactor::zero,
                    true, false, true))
        return false;
    if (!check_plan(m2host::CltBlendMode::coefficient_weighted_mix,
                    m2host::CltGlBlendFactor::one,
                    m2host::CltGlBlendFactor::constant_color,
                    m2host::CltGlBlendFactor::one,
                    m2host::CltGlBlendFactor::zero,
                    true, true, true))
        return false;
    if (!check_plan(m2host::CltBlendMode::secondary_source_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    m2host::CltGlBlendFactor::src_alpha,
                    m2host::CltGlBlendFactor::one_minus_src_alpha,
                    false, false, false))
        return false;
    return true;
}

bool test_dsb_alpha_control_decode()
{
    const float vertex_data[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {4.f, 0.f, 1.f, 1.f, 0.f, 1.f},
        {0.f, 4.f, 1.f, 0.f, 1.f, 1.f},
    };
    struct Probe {
        bool drew = false;
        m2host::CltBlendState blend{};
    };
    // One authored equation per evaluated blend form.  The DSB words are the
    // recovered statically authored words the per-equation tests above pin;
    // only the cases that exercise the destination-alpha control registers
    // write them, so every other run sees their reset value (leave-alone clamp
    // and TEXALPHA, the state every statically authored list establishes).
    struct ModeCase {
        const char* name;
        uint32_t user_control;
        uint32_t a_mult;
        uint32_t b_mult;
        uint32_t alu;
        uint32_t b_const0;
        uint32_t b_const1;
        m2host::CltBlendMode mode;
    };
    const ModeCase cases[] = {
        {"the source-over equation", 0x6fu, 0x0u, 0x2u, 0x00u, 0x0u, 0x0u,
         m2host::CltBlendMode::source_over},
        {"the specular composite", 0x40u, 0x20u, 0x40u, 0x00u, 0x0u, 0x0u,
         m2host::CltBlendMode::specular_add},
        {"the constant-alpha equation", 0x6fu, 0x40u, 0x2u, 0x10u, 0x0u, 0x0u,
         m2host::CltBlendMode::constant_alpha_over},
        {"the inverse-multiply equation", 0x6fu, 0x20u, 0x32u, 0xd0u, 0x0u,
         0x0u, m2host::CltBlendMode::inverse_texture_multiply},
        {"the source-copy equation", 0x6fu, 0x0u, 0x20u, 0xd0u, 0xffffffu,
         0xffffffu, m2host::CltBlendMode::source_copy},
        {"the weighted mix", 0x6fu, 0x20u, 0x22u, 0x00u, 0x00808080u,
         0x00808080u, m2host::CltBlendMode::coefficient_weighted_mix},
        // The reverse-alpha form needs a separately armed DB source and is
        // covered by the source-backed interpreter fixture below rather than
        // this deliberately untextured alpha-control table.
        // The shooter2d cloud words with the inherited A_PLUS_BCLAMP word
        // (that list never writes DBALUCNTL) classify as the same constant
        // equation, not as a disclosed approximation.
        {"the inherited constant-alpha equation", 0x6fu, 0x40u, 0x2u, 0x00u,
         0x0u, 0x0u, m2host::CltBlendMode::constant_alpha_over},
    };
    const auto run = [&](const ModeCase& mode_case,
                         std::initializer_list<std::pair<uint32_t, uint32_t>>
                             alpha_words) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, mode_case.user_control,
            0x10008058u, mode_case.a_mult,
            0x1000805cu, 0x00808080u,
            0x10008060u, 0x00808080u,
            0x10008064u, mode_case.b_mult,
            0x10008068u, mode_case.b_const0,
            0x1000806cu, mode_case.b_const1,
            0x10008070u, mode_case.alu,
        };
        for (const auto& [address, value] : alpha_words) {
            words.push_back(0x10000000u | address);
            words.push_back(value);
        }
        words.push_back(0x20090002u);
        for (const auto& vertex : vertex_data)
            for (const float value : vertex)
                words.push_back(std::bit_cast<uint32_t>(value));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        Probe probe;
        probe.drew = result.ok && result.draws == 1 && !backend.draws.empty();
        if (probe.drew) probe.blend = backend.draws.back().blend;
        return probe;
    };

    for (const ModeCase& mode_case : cases) {
        // The guest's own words (`FN_DestBlendInit__2e870` writes 0x0/0x0/0x3f,
        // the transitions write DBSRCALPHACNTL=0): leave-alone + TEXALPHA is
        // fully supported, so each equation is evaluated as its DSB registers
        // encode it.
        const Probe authored = run(mode_case, {});
        const std::string authored_message =
            std::string("leave-alone/TEXALPHA alpha controls support ") +
            mode_case.name;
        if (!expect(authored.drew && authored.blend.mode == mode_case.mode,
                    authored_message.c_str()))
            return false;

        // A destination-alpha source other than TEXALPHA changes the alpha
        // written by the destination blender.  The Configure Car material
        // carries SRCALPHA into M_DBSpec; preserve that independently
        // recovered RGB equation instead of replacing it with source-over.
        // Other evaluated forms remain on the existing fail-closed path.
        const Probe srcalpha = run(mode_case, {{0x8078u, 0x00000002u}});
        const std::string srcalpha_message =
            std::string("SRCALPHA keeps the expected RGB disposition for ") +
            mode_case.name;
        const m2host::CltBlendMode expected_srcalpha_mode =
            mode_case.mode == m2host::CltBlendMode::specular_add
                ? m2host::CltBlendMode::specular_add
                : m2host::CltBlendMode::source_over;
        if (!expect(srcalpha.drew && srcalpha.blend.alpha_control_unsupported &&
                        srcalpha.blend.mode == expected_srcalpha_mode,
                    srcalpha_message.c_str()))
            return false;

        // A source-side alpha clamp selection the SDK documents but no
        // recovered source ties to these equations stays fail-closed too.
        const Probe clamped = run(mode_case, {{0x8074u, 0x00000001u}});
        const std::string clamped_message =
            std::string("unused source-alpha clamp preserves the RGB equation for ") +
            mode_case.name;
        if (!expect(clamped.drew && !clamped.blend.alpha_control_unsupported &&
                        clamped.blend.mode == mode_case.mode,
                    clamped_message.c_str()))
            return false;
    }

    // An otherwise valid but undecoded BLENDEN|SRCEN equation reaches the
    // draw consumer with its raw words and an explicit approximation marker.
    // The guest's deliberate source-over equation must not carry that marker.
    const Probe deliberate = run(cases[0], {});
    const ModeCase undecoded{"undecoded A multiplier", 0x6fu, 0x08u,
                             0x02u, 0x00u, 0x0u, 0x0u,
                             m2host::CltBlendMode::source_over};
    const Probe approximation = run(undecoded, {});
    if (!expect(deliberate.drew && !deliberate.blend.equation_approximate &&
                    !approximation.drew,
                "reserved multiplier control is rejected instead of approximated"))
        return false;

    const Probe authored = run(cases[5], {{0x8074u, 0x0u}, {0x8078u, 0x0u},
                                          {0x807cu, 0x3fu}});
    if (!expect(authored.drew &&
                    authored.blend.mode ==
                        m2host::CltBlendMode::coefficient_weighted_mix &&
                    authored.blend.src_alpha.valid &&
                    authored.blend.src_alpha.alpha0 == 0u &&
                    authored.blend.dest_alpha.valid &&
                    authored.blend.dest_alpha.source ==
                        m2host::CltDestAlphaSource::texture_alpha &&
                    authored.blend.dest_alpha_const.valid &&
                    authored.blend.dest_alpha_const.ssb1 == 0x3f,
                "the alpha control registers decode with the SDK fields"))
        return false;

    // A reserved ACLAMP field value (3) is not a documented selection either.
    const Probe reserved = run(cases[5], {{0x8074u, 0x00000003u}});
    if (!expect(reserved.drew && reserved.blend.alpha_control_unsupported &&
                    reserved.blend.mode == m2host::CltBlendMode::source_over,
                "a reserved ACLAMP field value is not evaluated"))
        return false;
    return true;
}

void append_float(std::vector<uint32_t>& words, float value)
{
    words.push_back(std::bit_cast<uint32_t>(value));
}

struct TaggedVertex {
    int id = -1;
};

bool expect_tagged_triangle(const m2host::TeVertexState<TaggedVertex>::Triangle& triangle,
                            std::array<int, 3> expected, const char* message)
{
    return expect(std::array<int, 3>{triangle[0].id, triangle[1].id,
                                    triangle[2].id} == expected,
                  message);
}

bool test_te_vertex_state()
{
    using State = m2host::TeVertexState<TaggedVertex>;
    using Triangle = State::Triangle;

    // NEW with one and two vertices is an incomplete setup state, not a
    // malformed command. NEW applies only to the first vertex in the header.
    {
        State state;
        Triangle triangle{};
        if (!expect(!state.add_vertex({1}, true, false, &triangle) &&
                        state.vertex_count() == 1u &&
                        state.sort() == m2host::TeVertexSort::invalid,
                    "TE NEW with one vertex retains an incomplete state"))
            return false;
        if (!expect(!state.add_vertex({2}, false, false, &triangle) &&
                        state.vertex_count() == 2u,
                    "TE NEW with two vertices remains incomplete"))
            return false;
    }

    // A later one-vertex continuation completes a short NEW header.
    {
        State state;
        Triangle triangle{};
        state.add_vertex({10}, true, false, &triangle);
        state.add_vertex({11}, false, false, &triangle);
        if (!expect(state.add_vertex({12}, false, false, &triangle) &&
                        state.vertex_count() == 3u &&
                        state.sort() == m2host::TeVertexSort::omn,
                    "short NEW plus continuation completes the triangle"))
            return false;
        if (!expect_tagged_triangle(triangle, {10, 11, 12},
                                    "short NEW emits retained source vertices"))
            return false;
    }

    // Exact retained slots for ordinary strip replacement: A B C D E.
    {
        State state;
        Triangle triangle{};
        state.add_vertex({0}, true, false, &triangle);
        state.add_vertex({1}, false, false, &triangle);
        if (!expect(state.add_vertex({2}, false, false, &triangle) &&
                        state.slots()[0].id == 0 && state.slots()[1].id == 1 &&
                        state.slots()[2].id == 2,
                    "strip establishes A B C retained slots"))
            return false;
        if (!expect_tagged_triangle(triangle, {0, 1, 2},
                                    "strip emits the initial triangle"))
            return false;
        if (!expect(state.add_vertex({3}, false, false, &triangle) &&
                        state.sort() == m2host::TeVertexSort::nom &&
                        state.slots()[0].id == 3 && state.slots()[1].id == 1 &&
                        state.slots()[2].id == 2,
                    "strip D replaces the MAME-selected slot"))
            return false;
        if (!expect_tagged_triangle(triangle, {3, 1, 2},
                                    "strip emits D B C"))
            return false;
        if (!expect(state.add_vertex({4}, false, false, &triangle) &&
                        state.sort() == m2host::TeVertexSort::mno &&
                        state.slots()[0].id == 3 && state.slots()[1].id == 4 &&
                        state.slots()[2].id == 2,
                    "strip E replaces the next retained slot"))
            return false;
        if (!expect_tagged_triangle(triangle, {3, 4, 2},
                                    "strip emits D E C"))
            return false;
    }

    // Exact retained slots for RM/fan replacement: A B C D E.
    {
        State state;
        Triangle triangle{};
        state.add_vertex({0}, true, true, &triangle);
        state.add_vertex({1}, false, true, &triangle);
        state.add_vertex({2}, false, true, &triangle);
        if (!expect_tagged_triangle(triangle, {0, 1, 2},
                                    "fan emits the initial triangle"))
            return false;
        if (!expect(state.add_vertex({3}, false, true, &triangle) &&
                        state.sort() == m2host::TeVertexSort::onm &&
                        state.slots()[0].id == 0 && state.slots()[1].id == 3 &&
                        state.slots()[2].id == 2,
                    "fan D replaces the middle retained slot"))
            return false;
        if (!expect_tagged_triangle(triangle, {0, 3, 2},
                                    "fan emits A D C"))
            return false;
        if (!expect(state.add_vertex({4}, false, true, &triangle) &&
                        state.sort() == m2host::TeVertexSort::omn &&
                        state.slots()[0].id == 0 && state.slots()[1].id == 3 &&
                        state.slots()[2].id == 4,
                    "fan E rotates back to the OMN state"))
            return false;
        if (!expect_tagged_triangle(triangle, {0, 3, 4},
                                    "fan emits A D E"))
            return false;
    }

    // Changing RM mid-stream must use the shared setup state, not separate
    // fan/strip histories.
    {
        State state;
        Triangle triangle{};
        state.add_vertex({0}, true, false, &triangle);
        state.add_vertex({1}, false, false, &triangle);
        state.add_vertex({2}, false, false, &triangle);
        state.add_vertex({3}, false, false, &triangle);
        if (!expect(state.add_vertex({4}, false, true, &triangle) &&
                        state.sort() == m2host::TeVertexSort::mon &&
                        state.slots()[0].id == 3 && state.slots()[1].id == 1 &&
                        state.slots()[2].id == 4,
                    "RM transition consumes the shared strip state"))
            return false;
        if (!expect_tagged_triangle(triangle, {3, 1, 4},
                                    "RM transition emits D B E"))
            return false;
        if (!expect(state.add_vertex({5}, false, false, &triangle) &&
                        state.sort() == m2host::TeVertexSort::onm &&
                        state.slots()[0].id == 3 && state.slots()[1].id == 5 &&
                        state.slots()[2].id == 4,
                    "strip resumes from the RM-updated state"))
            return false;
        if (!expect_tagged_triangle(triangle, {3, 5, 4},
                                    "strip after RM emits D F E"))
            return false;
    }

    // State is intentionally independent of command-list boundaries.
    {
        State state;
        Triangle triangle{};
        state.add_vertex({0}, true, false, &triangle);
        state.add_vertex({1}, false, false, &triangle);
        state.add_vertex({2}, false, false, &triangle);
        if (!expect(state.add_vertex({3}, false, false, &triangle) &&
                        expect_tagged_triangle(triangle, {3, 1, 2},
                                               "list boundary preserves strip state"),
                    "continuation list emits from retained state"))
            return false;
        if (!expect(state.add_vertex({4}, false, false, &triangle) &&
                        expect_tagged_triangle(triangle, {3, 4, 2},
                                               "second list boundary preserves tsort"),
                    "second continuation list emits from retained state"))
            return false;
    }

    // Compare every transition in a mixed strip/RM stream against the
    // independent table-driven reference, including continuation list
    // boundaries represented by adjacent steps here.
    {
        using Reference = m2host::TeReferenceVertexState<TaggedVertex>;
        State production;
        Reference reference;
        struct Step {
            int id;
            bool new_triangle;
            bool rm;
        };
        const Step steps[] = {
            {0, true, false},  {1, false, false}, {2, false, false},
            {3, false, false}, {4, false, true},  {5, false, false},
            {6, true, true},   {7, false, true},  {8, false, true},
            {9, false, false},
        };
        for (const Step& step : steps) {
            Triangle production_triangle{};
            Reference::Triangle reference_triangle{};
            const bool production_ready = production.add_vertex(
                {step.id}, step.new_triangle, step.rm, &production_triangle);
            const bool reference_ready = reference.add_vertex(
                {step.id}, step.new_triangle, step.rm, &reference_triangle);
            if (!expect(production_ready == reference_ready,
                        "production/reference readiness agrees"))
                return false;
            if (!expect(production.vertex_count() == reference.vertex_count() &&
                            production.sort() == reference.sort() &&
                            production.slots()[0].id == reference.slots()[0].id &&
                            production.slots()[1].id == reference.slots()[1].id &&
                            production.slots()[2].id == reference.slots()[2].id,
                        "production/reference retained slots agree"))
                return false;
            if (production_ready &&
                !expect_tagged_triangle(
                    production_triangle,
                    {reference_triangle[0].id, reference_triangle[1].id,
                     reference_triangle[2].id},
                    "production/reference triangle identities agree"))
                return false;
        }
    }

    return true;
}

std::vector<uint32_t> shaded_triangle()
{
    std::vector<uint32_t> words = {
        0x1000801cu, 0x001200a2u,  // clip min=18, max=162
        0x10008020u, 0x001200f2u,  // clip min=18, max=242
        0x20090002u,  // new, shaded, strip, three vertices
    };
    const float vertices[][6] = {
        {10.f, 20.f, 1.f, 0.f, 0.f, 1.f},
        {30.f, 20.f, 0.f, 1.f, 0.f, 1.f},
        {10.f, 40.f, 0.f, 0.f, 1.f, 1.f},
    };
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(words, value);
    return words;
}

std::vector<uint32_t> shaded_strip_quad()
{
    std::vector<uint32_t> words = {
        0x1000801cu, 0x001200a2u,  // clip min=18, max=162
        0x10008020u, 0x001200f2u,  // clip min=18, max=242
        0x20090003u,  // new, shaded, strip, four vertices
    };
    const float vertices[][6] = {
        {0.f, 0.f, 1.f, 0.f, 0.f, 1.f},
        {2.f, 0.f, 0.f, 1.f, 0.f, 1.f},
        {0.f, 2.f, 0.f, 0.f, 1.f, 1.f},
        {2.f, 2.f, 1.f, 1.f, 1.f, 1.f},
    };
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(words, value);
    return words;
}

std::vector<uint8_t> big_endian_bytes(const std::vector<uint32_t>& words)
{
    std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        bytes[i * 4 + 0] = static_cast<uint8_t>(word >> 24);
        bytes[i * 4 + 1] = static_cast<uint8_t>(word >> 16);
        bytes[i * 4 + 2] = static_cast<uint8_t>(word >> 8);
        bytes[i * 4 + 3] = static_cast<uint8_t>(word);
    }
    return bytes;
}

bool test_room_texture_dims()
{
    // State 2 showroom floor: DMA'd as 16320 bytes of 6-bit texels
    // (21760 texels) with TXTUVMAX reading the 1024x1024 wrap ceiling and
    // TXTUVMASK (0x6430) holding (127,127). The footprint is exactly a
    // 128x128 base plus 64/32/16 MIPs, so the resident must be 128x128 --
    // not the 147x147 square the window-aspect heuristic guesses (that shear
    // turns the marble checker into gray stripes on screen).
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x10006404u, 0x00000100u,  // MMDMA load
        0x10006424u, 0x00000200u,  // source address
        0x10006414u, 0x00000000u,  // TRAM destination base 0
        0x10006428u, 0x00003fc0u,  // 16320 byte payload
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10000014u, 0x00000001u,  // TLD executes the load
        0x1001642cu, 0x03ff03ffu, 0x007f007fu,  // UVMAX ceiling + UVMASK 127
        0x10006408u, 0x000029a0u,  // texture lookup enabled
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00020499u,  // PRIM*TEX multiply form
        0x200a0002u,               // new, texture-only, three-vertex strip
    };
    const float verts[][4] = {
        {0.f, 0.f, 202.f, 10.f},
        {8.f, 0.f, 210.f, 10.f},
        {0.f, 8.f, 202.f, 18.f},
    };
    std::vector<uint32_t> complete_words = words;
    for (const auto& vertex : verts)
        for (float value : vertex) append_float(complete_words, value);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes.assign(0x3fc0u, 0u);
    memory.ranges.push_back(std::move(texels));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1,
                "masked MIP-chain texture reaches backend"))
        return false;
    if (!expect(backend.uploads[0].w == 128 &&
                    backend.uploads[0].h == 128 &&
                    backend.uploads[0].pixels.size() == 128u * 128u,
                "wrap mask plus MIP footprint recovers 128x128 base"))
        return false;
    // The stream UV remains in texel space through the backend boundary;
    // rasterization applies the tiled address mode after interpolation. This
    // keeps a triangle that crosses a tile boundary continuous in hardware
    // space: u=202 -> 202/128 = 1.578125 and v=10 -> 10/128 = 0.078125.
    if (!expect(backend.draws.size() == 1 && backend.draws[0].textured,
                "masked texture draw reaches backend"))
        return false;
    const float got_u = backend.draws[0].vertices[0].u;
    const float got_v = backend.draws[0].vertices[0].v;
    if (!expect(got_u > 1.578f && got_u < 1.5783f && got_v > 0.078f &&
                    got_v < 0.0783f,
                "stream UVs stay unwrapped until raster address calculation"))
        return false;
    if (!expect(backend.draws[0].sampling.min_filter ==
                    m2host::CltTextureFilter::bilinear &&
                    backend.draws[0].sampling.inter_filter ==
                        m2host::CltTextureFilter::trilinear &&
                    backend.draws[0].sampling.mag_filter ==
                        m2host::CltTextureFilter::bilinear &&
                    backend.draws[0].sampling.lod_max == 0u,
                "guest texture filter state reaches the draw backend"))
        return false;
    if (!expect(backend.draws[0].sampling.wrap_u &&
                    backend.draws[0].sampling.wrap_v,
                "resident-period UV mask reaches the draw backend as repeat"))
        return false;
    return true;
}

bool test_mmdma_address_window()
{
    // MAME's 3D0 address calculation uses (UMAX & UMASK)+1 and
    // (VMAX & VMASK)+1 for a flat texture window. This is the State 3 world
    // tuple observed in the boot-warm list: 1024x120 raw maxima masked to an
    // 8x120 resident slice, with the remaining MMDMA bytes belonging to the
    // next texture in the packed TRAM blob.
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x000002d0u,  // 8*120 six-bit texels
        0x10000014u, 0x00000001u,  // TLD
        0x1000642cu, 0x03ff0077u,  // UMAX=1023, VMAX=119
        0x10006430u, 0x000703ffu,  // UMASK=7, V MASK=1023
        0x10006408u, 0x00002000u,  // texture enable, LOD max 0
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    const float vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {8.f, 0.f, 8.f, 0.f},
        {0.f, 120.f, 0.f, 120.f},
    };
    std::vector<uint32_t> complete_words = words;
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(complete_words, value);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange source;
    source.guest_address = 0x200u;
    source.bytes.assign(0x2d0u, 0u);
    memory.ranges.push_back(std::move(source));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1,
                "State 3 MMDMA address-window texture reaches backend"))
        return false;
    if (!expect(backend.uploads[0].w == 8 && backend.uploads[0].h == 120 &&
                    backend.uploads[0].pixels.size() == 8u * 120u,
                "raw TXTUVMAX/TXTUVMASK publish the effective 8x120 window"))
        return false;
    return expect(backend.draws.size() == 1 &&
                      backend.draws[0].sampling.wrap_u &&
                      !backend.draws[0].sampling.wrap_v,
                  "raw address-window periods reach matching draw axes");
}

// Resident selection across packed TXTCOUNT blasts.  The guest
// packs several textures into one TRAM window: TXTLODBASE0 carries the window
// base at TLD time, the use list's CLT_TxLoad4LOD then writes the texture's
// own four LOD bases, and TXTCOUNT covers the whole blast.  The MMDMA
// materializer publishes each resident with that blast's byte range, so a
// resident's span *is* the TRAM the blast overwrote.  A draw resolves to the
// newest publication whose span covers its TxtLODBase0, which is the byte
// model: whichever blast last wrote that address owns the texels there.  Both
// directions are pinned here because they are easy to invert by accident --
// preferring the resident whose own LOD quad names the base serves textures
// whose bytes a later blast already overwrote (measured on a bounded five-A
// audit: 775 -> 473 dim-exact of 1024 audited draws and 249 -> 551 unrelated
// when that rule was tried, so it was reverted).
struct PackedResidentProbe {
    m2host::CltExecutionResult result{};
    std::vector<std::pair<int, int>> upload_dims;
    std::vector<std::pair<int, int>> served_dims;
    std::vector<bool> textured;
};

PackedResidentProbe run_packed_resident_stream(
    const std::vector<uint32_t>& words, uint32_t blast_bytes)
{
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    for (uint32_t i = 0; i < 64u; ++i) {
        const uint32_t pip_word = 0x00112233u | (i * 0x01010101u);
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 24u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 16u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 8u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word));
    }
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange blast;
    blast.guest_address = 0x200u;
    blast.bytes.assign(blast_bytes, 0u);
    memory.ranges.push_back(std::move(blast));
    PackedResidentProbe probe;
    probe.result = interpreter.execute(words.data(), words.size(), &memory);
    for (const auto& upload : backend.uploads)
        probe.upload_dims.emplace_back(upload.w, upload.h);
    for (const auto& draw : backend.draws) {
        probe.textured.push_back(draw.textured);
        // A cel id indexes the upload that served the draw (an indexed
        // resident re-decodes into a fresh cel when its palette generation
        // moved), so the served residency's dims read back from that upload.
        const m2host::CelId cel = draw.cel;
        probe.served_dims.emplace_back(
            cel != 0 && static_cast<size_t>(cel) <= backend.uploads.size()
                ? std::pair<int, int>(backend.uploads[cel - 1].w,
                                      backend.uploads[cel - 1].h)
                : std::pair<int, int>(0, 0));
    }
    return probe;
}

// Appends a texture-only three-vertex strip to `words`.
void append_strip(std::vector<uint32_t>& words,
                  const float (&vertices)[3][4], float offset)
{
    words.push_back(0x200a0002u);
    for (const auto& vertex : vertices) {
        for (int i = 0; i < 4; ++i)
            append_float(words, vertex[i] + offset);
    }
}

bool test_resident_selection_by_blast_span()
{
    // Case 1: a second blast overwrites the whole window.  The bytes at every
    // base inside it now belong to the newer resident, so both draws must be
    // served the newer resident's decode -- 4-bit 128x64 is gone.
    std::vector<uint32_t> overwrite = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000100u,  // 64 entries: 4- and 6-bit tables
        0x10000014u, 0x00000001u,
        0x10006440u, 0x00112233u,  // PIP constant 0
        // Resident A: 4-bit 128x64 at LOD bases 0x17e8/0x27e8/0x2be8/0x2ce8.
        0x1000643cu, 0x00000404u,  // A use format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // packed blast source
        0x10006414u, 0x00000000u,  // TLD-time TXTLODBASE0 = window base
        0x10006428u, 0x00002d28u,  // TXTCOUNT: the whole packed blast
        0x10000014u, 0x00000001u,  // TLD stages the blob
        0x10006414u, 0x000017e8u,  // use list CLT_TxLoad4LOD
        0x10006418u, 0x000027e8u,
        0x1000641cu, 0x00002be8u,
        0x10006420u, 0x00002ce8u,
        0x1000642cu, 0x000f007fu,  // UMAX=15, VMAX=127
        0x10006430u, 0x03ff003fu,  // UMASK full scale, VMASK=63
        0x10006408u, 0x000029a3u,
        0x1000643cu, 0x00000404u,  // use-state write -> materialize A
        // Resident B: 6-bit 32x64, same window and blast size.
        0x10006404u, 0x00000100u,
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00002d28u,
        0x10000014u, 0x00000001u,
        0x10006414u, 0x00000ff0u,
        0x10006418u, 0x000015f0u,
        0x1000641cu, 0x000018f0u,
        0x10006420u, 0x000019f0u,
        0x1000642cu, 0x007f007fu,
        0x10006430u, 0x001f003fu,
        0x10006408u, 0x000029a3u,
        0x1000643cu, 0x00000406u,  // use-state write -> materialize B
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00020499u,
        0x10006414u, 0x000017e8u,  // draw at A's LOD0
    };
    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 2.f, 0.f, 2.f},
    };
    append_strip(overwrite, quad, 0.f);
    overwrite.push_back(0x10006414u);
    overwrite.push_back(0x00000ff0u);  // draw at B's LOD0
    append_strip(overwrite, quad, 4.f);

    const PackedResidentProbe overwritten =
        run_packed_resident_stream(overwrite, 0x2d28u);
    if (!expect(overwritten.result.ok && overwritten.result.unsupported == 0 &&
                    overwritten.result.draws == 2 &&
                    overwritten.textured.size() == 2u &&
                    overwritten.textured[0] && overwritten.textured[1],
                "both packed-window draws stay textured"))
        return false;
    if (!expect(overwritten.upload_dims.size() >= 2u &&
                    overwritten.upload_dims[0] == std::pair<int, int>(128, 64) &&
                    overwritten.upload_dims[1] == std::pair<int, int>(32, 64),
                "packed residents materialize their own dims"))
        return false;
    if (!expect(overwritten.served_dims[0] == std::pair<int, int>(32, 64),
                "a fully overwritten window serves its newest blast even at "
                "the older resident's LOD0"))
        return false;
    if (!expect(overwritten.served_dims[1] == std::pair<int, int>(32, 64),
                "a draw at the newest resident's LOD0 serves that resident"))
        return false;

    // Case 2: the second blast is a small prefix.  Real TRAM retains the
    // suffix bytes, so a draw at 0x17e8 (outside the small blast's range)
    // still resolves to the larger resident whose span covers it.
    std::vector<uint32_t> prefix = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000100u,
        0x10000014u, 0x00000001u,
        0x10006440u, 0x00112233u,
        0x1000643cu, 0x00000404u,  // A use format
        0x10006404u, 0x00000100u,
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00002d28u,
        0x10000014u, 0x00000001u,
        0x10006414u, 0x000017e8u,
        0x10006418u, 0x000027e8u,
        0x1000641cu, 0x00002be8u,
        0x10006420u, 0x00002ce8u,
        0x1000642cu, 0x000f007fu,
        0x10006430u, 0x03ff003fu,
        0x10006408u, 0x000029a3u,
        0x1000643cu, 0x00000404u,  // use-state write -> materialize A
        // Resident C: 8x8, a 0x100-byte blast at the same base.
        0x10006404u, 0x00000100u,
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000100u,
        0x10000014u, 0x00000001u,
        0x10006414u, 0x00000000u,
        0x1000642cu, 0x00070007u,  // UMAX=VMAX=7 -> 8x8 window
        0x10006430u, 0x03ff03ffu,
        0x10006408u, 0x00002000u,  // single-LOD addressing, texture enable
        0x1000643cu, 0x00000404u,  // use-state write -> materialize C
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00020499u,
        // Re-arm A's use state before drawing its interior base.  This keeps
        // the scenario about *selection*: a coherent MIP chain bracketing the
        // drawn base (see test_stale_region_draw_uses_armed_decode for the
        // stale-state case, where the armed registers decide the geometry).
        0x1000642cu, 0x000f007fu,
        0x10006430u, 0x03ff003fu,
        0x10006408u, 0x000029a3u,
        0x10006414u, 0x000017e8u,  // draw at A's LOD0, outside C's blast
    };
    append_strip(prefix, quad, 0.f);

    const PackedResidentProbe retained =
        run_packed_resident_stream(prefix, 0x2d28u);
    if (!expect(retained.result.ok && retained.result.unsupported == 0 &&
                    retained.result.draws == 1 && retained.textured.size() == 1u &&
                    retained.textured[0],
                "stale-region draw after a prefix overwrite stays textured"))
        return false;
    if (!expect(retained.upload_dims.size() >= 2u &&
                    retained.upload_dims[0] == std::pair<int, int>(128, 64),
                "the larger resident materializes before the prefix "
                "overwrite"))
        return false;
    return expect(retained.served_dims[0] == std::pair<int, int>(128, 64),
                  "a base outside the newer prefix blast still serves the "
                  "larger resident that owns those TRAM bytes");
}

// Residency orientation from the load/use state triple.  The
// accepted State 3 world draw published `serve_wh=64x128` while its own
// registers describe a 128x64 residency: TXTUVMAX 0x000f007f (UMAX 15, VMAX
// 127), TXTUVMASK 0x03ff003f (U at the full-scale 0x3FF ceiling -> a clamped
// axis; V tiled with period 63+1 = 64) and TXTADDRCNTL 0x29a3 (LODMAX 3).  A
// tiled axis' mask period is its own dimension, so V is 64 texels and the
// long axis is U; the LOD0 footprint agrees (lodBase[1] - lodBase[0] = 0x1000
// bytes of 4-bit texels = 8192 = 128*64).  TXTUVMAX alone cannot say this: as
// a wrap ceiling it carries no size, and the 16/128 window aspect the old
// sizing trusted is exactly what transposed the residency.
bool test_mmdma_resident_orientation()
{
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000040u,  // sixteen PIP entries
        0x10000014u, 0x00000001u,
        0x10006440u, 0x00112233u,  // PIP constant 0
        0x1000643cu, 0x00000404u,  // 4-bit indexed expansion format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // packed chain source address
        0x10006414u, 0x00000000u,  // LOD base 0 / TRAM destination base
        0x10006418u, 0x00001000u,  // LOD base 1 = base 0 + LOD0's 8192 texels
        0x1000641cu, 0x00001800u,  // LOD base 2 = base 1 + 4096 texels
        0x10006420u, 0x00001c00u,  // LOD base 3
        0x10006428u, 0x00002d28u,  // 11560-byte chain
        0x10000014u, 0x00000001u,  // TLD executes the load
        0x1000642cu, 0x000f007fu,  // UMAX=15, VMAX=127
        0x10006430u, 0x03ff003fu,  // UMASK full scale, VMASK=63
        0x10006408u, 0x000029a3u,  // accepted draw's TXTADDRCNTL (LODMAX 3)
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00020499u,  // PRIM*TEX multiply form
        0x200a0002u,               // new, texture-only, three-vertex strip
    };
    const float verts[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 4.f, 0.f, 4.f},
    };
    std::vector<uint32_t> complete_words = words;
    for (const auto& vertex : verts)
        for (float value : vertex) append_float(complete_words, value);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t pip_word = 0x00112233u | (i * 0x01010101u);
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 24u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 16u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word >> 8u));
        pip_range.bytes.push_back(static_cast<uint8_t>(pip_word));
    }
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes.assign(0x2d28u, 0u);
    memory.ranges.push_back(std::move(texels));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1,
                "accepted State 3 residency reaches backend"))
        return false;
    // The footprint (8192 texels) was never in doubt; only the split was.  A
    // 64x128 publish is the pre-fix transpose and fails here.
    if (!expect(backend.uploads[0].w == 128 && backend.uploads[0].h == 64 &&
                    backend.uploads[0].pixels.size() == 128u * 64u,
                "tiled V mask period 64 fixes the 128x64 residency "
                "orientation"))
        return false;
    if (!expect(backend.draws.size() == 1 &&
                    !backend.draws[0].sampling.wrap_u &&
                    backend.draws[0].sampling.wrap_v,
                "accepted draw's clamped U and tiled V reach the backend"))
        return false;
    return expect(backend.draws[0].sampling.v_period == 64u,
                  "accepted draw's V period reaches the raster address "
                  "calculation");
}

bool test_mmdma_register_dimensions()
{
    // SDK use lists describe each axis independently. The LOD footprint
    // cannot distinguish a 256x32 banner from 128x64, or a 32x128 wall from
    // 64x64. Use synthetic row patterns to verify the actual decode and its
    // authored MIPs, including tiled non-power-of-two scenery textures.
    struct Shape {
        uint32_t w, h;
        bool wrap_u, wrap_v;
    };
    const Shape shapes[] = {
        {256, 32, false, false},
        {512, 32, false, false},
        {32, 128, false, false},
        {8, 64, false, false},
        {256, 64, true, false},
        {176, 64, true, false},
        {128, 80, true, false},
        {32, 168, true, false},
        {80, 128, false, true},
        {32, 128, true, true},
        {128, 128, false, false},
    };
    for (const Shape& shape : shapes) {
        const auto index = [](uint32_t x, uint32_t y, uint32_t lod) {
            return (x + 3u * y + 5u * lod) & 15u;
        };
        std::vector<uint32_t> palette;
        std::vector<uint32_t> colors;
        for (uint32_t i = 0; i < 16u; ++i) {
            palette.push_back(0x7f000000u | (13u * i << 16u) |
                              (7u * i << 8u) | (3u * i));
            colors.push_back(0xff000000u | (3u * i << 16u) |
                             (7u * i << 8u) | (13u * i));
        }
        // The selected texture sits inside a larger packed DMA residency.
        std::vector<uint8_t> bytes(64u, 0x99u);
        std::array<uint32_t, 4> bases;
        std::array<std::vector<uint32_t>, 4> expected;
        for (uint32_t lod = 0; lod < 4u; ++lod) {
            bases[lod] = 0x400u + static_cast<uint32_t>(bytes.size());
            const uint32_t w = shape.w >> lod, h = shape.h >> lod;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t value = index(x, y, lod);
                    if (((y * w + x) & 1u) == 0u)
                        bytes.push_back(static_cast<uint8_t>(value << 4u));
                    else
                        bytes.back() |= static_cast<uint8_t>(value);
                    expected[lod].push_back(colors[value]);
                }
        }
        bytes.insert(bytes.end(), 64u, 0x66u);
        const uint32_t umax = shape.wrap_u ? 0x7fu : (shape.w >> 3u) - 1u;
        const uint32_t vmax = shape.wrap_v ? 0x7fu : (shape.h >> 3u) - 1u;
        const uint32_t umask = shape.wrap_u ? shape.w - 1u : 0x3ffu;
        const uint32_t vmask = shape.wrap_v ? shape.h - 1u : 0x3ffu;
        std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000040u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000404u,
            0x10006404u, 0x00000100u,
            0x10006424u, 0x00002000u,
            0x10006414u, 0x00000400u,
            0x10006428u, static_cast<uint32_t>(bytes.size()),
            0x10000014u, 0x00000001u,
            0x1001642cu, (umax << 16u) | vmax, (umask << 16u) | vmask,
            0x10006408u, 0x00002923u,
            0x10036414u, bases[0], bases[1], bases[2], bases[3],
            0x1000643cu, 0x00000404u,
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00020419u,
            0x200a0002u,
        };
        const float vertices[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {4.f, 0.f, float(shape.w), 0.f},
            {0.f, 4.f, 0.f, float(shape.h)},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
        m2host::CltMemorySnapshot memory;
        memory.ranges.push_back({0x100u, big_endian_bytes(palette)});
        memory.ranges.push_back({0x2000u, std::move(bytes)});
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const auto result = interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                        backend.draws.size() == 1 && backend.draws[0].cel > 0 &&
                        size_t(backend.draws[0].cel) <= backend.uploads.size(),
                    "register-described MIP texture reaches backend"))
            return false;
        const auto& draw = backend.draws[0];
        const auto& upload = backend.uploads[size_t(draw.cel) - 1u];
        if (!expect(upload.w == int(shape.w) && upload.h == int(shape.h) &&
                        upload.pixels == expected[0] && upload.lod_chain.size() == 3u,
                    "register dimensions preserve every base-level row"))
            return false;
        for (uint32_t lod = 1; lod < 4u; ++lod) {
            const auto& level = upload.lod_chain[lod - 1u];
            if (!expect(level.w == int(shape.w >> lod) &&
                            level.h == int(shape.h >> lod) &&
                            level.pixels == expected[lod],
                        "authored MIP rows retain the same aspect ratio"))
                return false;
        }
        if (!expect(draw.vertices[1].u == 1.f && draw.vertices[2].v == 1.f &&
                        draw.sampling.wrap_u == shape.wrap_u &&
                        draw.sampling.wrap_v == shape.wrap_v,
                    "UV normalization and addressing use the authored dimensions"))
            return false;
    }
    return true;
}

// Authored MIP chain residency (CLT-GENERATED-MIPMAP-REPLACES-AUTHORED-LOD).
// CLT_CreateTxLoadCommandListMMDMA (vendor
// .../libs/clt/clttxload.c:119-129) accumulates one texelDataSize per loaded LOD
// into lodBase[] and CLT_TxLoad4LOD (:180) publishes that quad, so a residency's
// coarser levels are distinct guest payloads and not a reduction of the level a
// draw selects. This fixture authors a two-level chain with deliberately
// different colors (LOD0 red, LOD1 green) and samples it at a fractional
// minification LOD, where the two levels carry equal weight. The requirement is
// the guest's own bytes: a host-generated downsample of a flat red level can
// only produce red, so the authored mix and the generated result cannot both
// be right.
bool test_mmdma_authored_lod_chain()
{
    // 0x1e78 literal RGB888+A7+SSB: 32-bit texels stored SSB/A7 -> R,G,B, so a
    // level's payload states its color directly with no palette indirection.
    // LOD0's eight texels are red; LOD1's two texels are green.
    constexpr uint32_t kLevel0Rgba = 0xff1e2cd8u;  // R=d8 G=2c B=1e A=ff
    constexpr uint32_t kLevel1Rgba = 0xff1eff00u;  // R=00 G=ff B=1e A=ff
    const auto texel_bytes = [](uint32_t rgba, std::vector<uint8_t>* bytes) {
        // SSB=1 in the top bit, then the 7-bit alpha, then the channels.
        bytes->push_back(0x80u | ((rgba >> 24u) & 0x7fu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 0u) & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 8u) & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 16u) & 0xffu));
    };
    // 4x2 LOD0 (32 bytes) at lodBase[0] and 2x1 LOD1 (8 bytes) at lodBase[1],
    // with the quad's LOD2/LOD3 bases continuing past the level-1 payload.
    // The register order is the builder's own: TXTEXPTYPE opens the load list,
    // CLT_TxLoad commits it, and the use list publishes the quad, the UV
    // window and TXTADDRCNTL.LODMAX (= numLOD-1, so 1 for this two-level
    // residency).
    constexpr uint32_t kLod0Base = 0x100u;
    constexpr uint32_t kLod1Base = 0x120u;
    constexpr uint32_t kBlobBytes = 0x28u;
    const std::vector<uint32_t> words = {
        0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB expansion format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // packed chain source address
        0x10006414u, kLod0Base,    // TXTLODBASE0 = tramOffset = lodBase[0]
        0x10006428u, kBlobBytes,   // TXTCOUNT: the whole packed chain
        0x10000014u, 0x00000001u,  // CLT_TxLoad stages the blob
        0x10006414u, kLod0Base,    // use list CLT_TxLoad4LOD
        0x10006418u, kLod1Base,
        0x1000641cu, 0x00000128u,
        0x10006420u, 0x00000130u,
        0x1000642cu, 0x00030001u,  // UMAX=3, VMAX=1 -> the 4x2 drawn level
        0x10006430u, 0x00030001u,  // UMASK=3, VMASK=1
        0x10006408u, 0x000029a1u,  // TXTADDRCNTL: LODMAX 1, MIP addressing
        0x1000640cu, 0x00004900u,  // color, alpha, and SSB from the texture
        0x10006410u, 0x00009200u,
        0x200a0002u,               // new, texture-only, three-vertex strip
    };
    const float verts[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 4.f, 0.f, 4.f},
    };
    std::vector<uint32_t> complete_words = words;
    for (const auto& vertex : verts)
        for (float value : vertex) append_float(complete_words, value);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes.assign(kBlobBytes, 0u);
    {
        // Each level's payload starts at its own lodBase[] byte.
        std::vector<uint8_t> level0;
        for (unsigned i = 0; i < 8u; ++i) texel_bytes(kLevel0Rgba, &level0);
        std::vector<uint8_t> level1;
        for (unsigned i = 0; i < 2u; ++i) texel_bytes(kLevel1Rgba, &level1);
        std::copy(level0.begin(), level0.end(), texels.bytes.begin());
        std::copy(level1.begin(), level1.end(),
                  texels.bytes.begin() + (kLod1Base - kLod0Base));
    }
    memory.ranges.push_back(std::move(texels));

    {
        RecordingBackend metadata_backend;
        m2host::GuestCltInterpreter metadata_interpreter(&metadata_backend);
        std::vector<uint32_t> draw={0x10008028u,0x10000u,0x10008050u,0u};
        draw.insert(draw.end(),complete_words.begin(),complete_words.end());
        draw.push_back(0x20020000u); // one retained-vertex texture continuation
        for(float f:{4.f,4.f,4.f,4.f}) append_float(draw,f);
        const auto r=metadata_interpreter.execute(draw.data(),draw.size(),&memory);
        if(!expect(r.ok && r.unsupported==0 && metadata_backend.draws.size()==2 &&
            metadata_backend.draws[0].blend.weighted_mix_ssb_mask!=m2host::kInvalidCel &&
            metadata_backend.draws[1].blend.weighted_mix_ssb_mask!=m2host::kInvalidCel,
            "object SSB output retains its texture mask on NEW and continuation")) return false;
        metadata_interpreter.release_resources();
    }
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1,
                "the two-level MMDMA residency reaches the backend"))
        return false;
    const RecordingBackend::Upload& upload = backend.uploads.front();
    if (!expect(upload.w == 4 && upload.h == 2 &&
                    upload.pixels.size() == 8u &&
                    upload.pixels.front() == kLevel0Rgba &&
                    upload.pixels.back() == kLevel0Rgba,
                "the drawn level is the 4x2 LOD0 payload the guest loaded"))
        return false;
    if (!expect(upload.mipmap_linear,
                "the MIP-sampled residency still asks for mip filtering"))
        return false;
    if (!expect(upload.lod_chain.size() == 1u &&
                    upload.lod_chain[0].w == 2 &&
                    upload.lod_chain[0].h == 1 &&
                    upload.lod_chain[0].pixels.size() == 2u &&
                    upload.lod_chain[0].pixels[0] == kLevel1Rgba &&
                    upload.lod_chain[0].pixels[1] == kLevel1Rgba,
                "the coarser lodBase[] payload is uploaded as its own level"))
        return false;

    // Sample the residency at LOD 0.5, where the drawn level and the authored
    // coarser level carry equal weight. rho = du/dx * width = sqrt(2) selects
    // that LOD exactly; the flat levels make the bilinear filter an identity.
    const auto sample_at_lod_half = [](const std::vector<uint32_t>& base,
                                       const m2host::CelLodLevel* level,
                                       uint32_t* out) {
        m2host::SoftCaptureBackend soft(4, 4, {});
        soft.frame_begin(0.f, 0.f, 0.f);
        m2host::CelId texture = m2host::kInvalidCel;
        if (level) {
            texture = soft.cel_upload_rgba_lod_chain(4, 2, base.data(), level,
                                                    1u, true, false, 0, 0);
        } else {
            // Wrong control: the pre-fix single-level upload, where the host
            // reduces level 0 itself instead of using the guest's own bytes.
            texture = soft.cel_upload_rgba_ex(4, 2, base.data(), true, false);
        }
        if (texture == m2host::kInvalidCel) return false;
        m2host::CltTextureSampling sampling;
        sampling.min_filter = m2host::CltTextureFilter::bilinear;
        sampling.inter_filter = m2host::CltTextureFilter::trilinear;
        sampling.mag_filter = m2host::CltTextureFilter::bilinear;
        sampling.lod_max = 1u;  // the residency's own TXTADDRCNTL.LODMAX
        const float u_span = std::sqrt(2.f);
        m2host::CltVertex vertices[3]{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
        vertices[1] = {4.f, 0.f, u_span, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
        vertices[2] = {0.f, 4.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
        soft.clt_draw(vertices, 3u, false, true, texture, 4.f, 4.f, false,
                      {}, {}, sampling, {});
        std::array<uint32_t, 16> frame{};
        if (!soft.grab_presented_rgba(frame.data(), 4, 4)) return false;
        *out = frame[0];
        return true;
    };
    std::vector<uint32_t> level_pixels = upload.lod_chain[0].pixels;
    m2host::CelLodLevel level;
    level.w = upload.lod_chain[0].w;
    level.h = upload.lod_chain[0].h;
    level.rgba = level_pixels.data();
    uint32_t authored = 0u;
    if (!sample_at_lod_half(upload.pixels, &level, &authored)) return false;
    // The expected value is derived from the guest's two authored colors at
    // equal weight, not from any recorded host output.
    const auto mix = [](uint32_t a, uint32_t b) {
        return static_cast<uint32_t>(((a >> 0u & 0xffu) +
                                      (b >> 0u & 0xffu) + 1u) /
                                     2u) |
               (static_cast<uint32_t>(((a >> 8u & 0xffu) +
                                       (b >> 8u & 0xffu) + 1u) /
                                      2u)
                << 8u) |
               (static_cast<uint32_t>(((a >> 16u & 0xffu) +
                                       (b >> 16u & 0xffu) + 1u) /
                                      2u)
                << 16u);
    };
    const uint32_t expected_mix = mix(kLevel0Rgba, kLevel1Rgba);
    const auto channel_matches = [](uint32_t got, uint32_t want) {
        for (unsigned shift = 0; shift < 24u; shift += 8u)
            if (std::abs(static_cast<int>((got >> shift & 0xffu)) -
                         static_cast<int>((want >> shift & 0xffu))) > 1)
                return false;
        return true;
    };
    if (!expect(channel_matches(authored, expected_mix),
                "a fractional minification LOD follows the authored level "
                "bytes"))
        return false;

    // Plausible-wrong control, actually run: the same residency sampled
    // through the generated-mipmap path. A reduction of a flat red level is
    // still flat red, so this control produces the drawn level's own color and
    // is what the authored assertion above would fail against.
    uint32_t generated = 0u;
    if (!sample_at_lod_half(upload.pixels, nullptr, &generated)) return false;
    if (!expect(generated == kLevel0Rgba,
                "the generated-mipmap control yields only the drawn level's "
                "color"))
        return false;
    if (!expect(generated != authored,
                "the authored chain and a generated downsample are "
                "distinguishable at a fractional LOD"))
        return false;
    // Same residency, but the guest published a flat quad instead of a LOD
    // chain. Without a coherent, increasing lodBase[] quad there is no authored
    // level to hand over, so the upload keeps the single-level path and the
    // host reduces level 0 itself.
    {
        std::vector<uint32_t> flat_words = complete_words;
        // Overwrite the two quad writes after CLT_TxLoad with a chain that is
        // not coherent: equal bases describe a residency with no distinct LODs.
        for (size_t i = 0; i + 1u < flat_words.size(); ++i) {
            if (flat_words[i] == 0x10006418u) flat_words[i + 1u] = kLod0Base;
        }
        RecordingBackend flat_backend;
        m2host::GuestCltInterpreter flat_interpreter(&flat_backend);
        const m2host::CltExecutionResult flat = flat_interpreter.execute(
            flat_words.data(), flat_words.size(), &memory);
        if (!expect(flat.ok && flat.draws == 1u &&
                        flat_backend.uploads.size() == 1u &&
                        flat_backend.uploads[0].lod_chain.empty(),
                    "a residency without distinct authored LOD bases keeps the "
                    "single-level upload"))
            return false;
    }
    return true;
}

// CLT-INACTIVE-LOD-SLOTS: MMDMA shape recovery and authored-chain decode
// must validate only the active LOD bases defined by TXTADDRCNTL.LODMAX.
// Inactive bases (which the SDK leaves uninitialized or zero) must not
// invalidate an authored LOD chain.
bool test_mmdma_inactive_lod_slots()
{
    constexpr uint32_t kLevel0Rgba = 0xff1e2cd8u;
    constexpr uint32_t kLevel1Rgba = 0xff1eff00u;
    constexpr uint32_t kLevel2Rgba = 0xffff0000u;
    const auto texel_bytes = [](uint32_t rgba, std::vector<uint8_t>* bytes) {
        bytes->push_back(0x80u | ((rgba >> 24u) & 0x7fu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 0u) & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 8u) & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 16u) & 0xffu));
    };

    // 1. Two-level fixture (LODMAX=1): 4x2 red LOD0 at 0x100, 2x1 green LOD1 at 0x120.
    // Inactive slots LODBASE2 and LODBASE3 set to 0.
    {
        constexpr uint32_t kLod0Base = 0x100u;
        constexpr uint32_t kLod1Base = 0x120u;
        constexpr uint32_t kBlobBytes = 0x28u;
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB expansion format
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
            0x10006424u, 0x00000200u,  // packed chain source address
            0x10006414u, kLod0Base,    // TXTLODBASE0 = tramOffset = lodBase[0]
            0x10006428u, kBlobBytes,   // TXTCOUNT: 40 bytes
            0x10000014u, 0x00000001u,  // CLT_TxLoad stages the blob
            0x10006414u, kLod0Base,    // use list CLT_TxLoad4LOD
            0x10006418u, kLod1Base,    // active lodBase[1]
            0x1000641cu, 0x00000000u,  // inactive lodBase[2] = 0
            0x10006420u, 0x00000000u,  // inactive lodBase[3] = 0
            0x1000642cu, 0x00030001u,  // UMAX=3, VMAX=1 -> 4x2 drawn level
            0x10006430u, 0x00030001u,  // UMASK=3, VMASK=1
            0x10006408u, 0x000029a1u,  // TXTADDRCNTL: LODMAX 1 (2 active levels)
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00009200u,
            0x200a0002u,               // new, texture-only, three-vertex strip
        };
        const float verts[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {4.f, 0.f, 4.f, 0.f},
            {0.f, 4.f, 0.f, 4.f},
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : verts)
            for (float value : vertex) append_float(complete_words, value);

        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes.assign(kBlobBytes, 0u);
        std::vector<uint8_t> level0;
        for (unsigned i = 0; i < 8u; ++i) texel_bytes(kLevel0Rgba, &level0);
        std::vector<uint8_t> level1;
        for (unsigned i = 0; i < 2u; ++i) texel_bytes(kLevel1Rgba, &level1);
        std::copy(level0.begin(), level0.end(), texels.bytes.begin());
        std::copy(level1.begin(), level1.end(),
                  texels.bytes.begin() + (kLod1Base - kLod0Base));
        memory.ranges.push_back(std::move(texels));

        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                        backend.uploads.size() == 1,
                    "two-level MMDMA draw with zero inactive bases succeeds"))
            return false;
        const RecordingBackend::Upload& upload = backend.uploads.front();
        if (!expect(upload.w == 4 && upload.h == 2 &&
                        upload.pixels.size() == 8u &&
                        upload.pixels.front() == kLevel0Rgba,
                    "LODMAX=1 drawn level is 4x2 LOD0"))
            return false;
        if (!expect(upload.lod_chain.size() == 1u &&
                        upload.lod_chain[0].w == 2 &&
                        upload.lod_chain[0].h == 1 &&
                        upload.lod_chain[0].pixels.size() == 2u &&
                        upload.lod_chain[0].pixels[0] == kLevel1Rgba,
                    "LODMAX=1 authored chain preserves green LOD1 despite zero inactive bases"))
            return false;
    }

    // 2. Three-level fixture (LODMAX=2): 8x4 LOD0 at 0x100 (128B), 4x2 LOD1 at 0x180 (32B),
    // 2x1 LOD2 at 0x1a0 (8B), total 168B blob.
    // Inactive slot LODBASE3 set to arbitrary non-increasing value (e.g. 0x50).
    {
        constexpr uint32_t kLod0Base = 0x100u;
        constexpr uint32_t kLod1Base = 0x180u;
        constexpr uint32_t kLod2Base = 0x1a0u;
        constexpr uint32_t kBlobBytes = 0xa8u;  // 168 bytes
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB
            0x10006404u, 0x00000100u,  // mode 1 MMDMA
            0x10006424u, 0x00000200u,  // source address
            0x10006414u, kLod0Base,
            0x10006428u, kBlobBytes,
            0x10000014u, 0x00000001u,
            0x10006414u, kLod0Base,
            0x10006418u, kLod1Base,
            0x1000641cu, kLod2Base,
            0x10006420u, 0x00000050u,  // inactive lodBase[3] = 0x50 (< lodBase[2])
            0x1000642cu, 0x00070003u,  // UMAX=7, VMAX=3 -> 8x4 drawn level
            0x10006430u, 0x00070003u,  // UMASK=7, VMASK=3
            0x10006408u, 0x000029a2u,  // TXTADDRCNTL: LODMAX 2 (3 active levels)
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        const float verts[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {8.f, 0.f, 8.f, 0.f},
            {0.f, 4.f, 0.f, 4.f},
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : verts)
            for (float value : vertex) append_float(complete_words, value);

        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes.assign(kBlobBytes, 0u);
        std::vector<uint8_t> level0;
        for (unsigned i = 0; i < 32u; ++i) texel_bytes(kLevel0Rgba, &level0);
        std::vector<uint8_t> level1;
        for (unsigned i = 0; i < 8u; ++i) texel_bytes(kLevel1Rgba, &level1);
        std::vector<uint8_t> level2;
        for (unsigned i = 0; i < 2u; ++i) texel_bytes(kLevel2Rgba, &level2);
        std::copy(level0.begin(), level0.end(), texels.bytes.begin());
        std::copy(level1.begin(), level1.end(),
                  texels.bytes.begin() + (kLod1Base - kLod0Base));
        std::copy(level2.begin(), level2.end(),
                  texels.bytes.begin() + (kLod2Base - kLod0Base));
        memory.ranges.push_back(std::move(texels));

        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                        backend.uploads.size() == 1,
                    "three-level MMDMA draw with non-increasing inactive base succeeds"))
            return false;
        const RecordingBackend::Upload& upload = backend.uploads.front();
        if (!expect(upload.w == 8 && upload.h == 4 &&
                        upload.pixels.size() == 32u,
                    "LODMAX=2 drawn level is 8x4 LOD0"))
            return false;
        if (!expect(upload.lod_chain.size() == 2u &&
                        upload.lod_chain[0].w == 4 && upload.lod_chain[0].h == 2 &&
                        upload.lod_chain[0].pixels[0] == kLevel1Rgba &&
                        upload.lod_chain[1].w == 2 && upload.lod_chain[1].h == 1 &&
                        upload.lod_chain[1].pixels[0] == kLevel2Rgba,
                    "LODMAX=2 authored chain preserves both LOD1 and LOD2 despite arbitrary inactive LOD3"))
            return false;
    }

    // 3. Single-level fixture (LODMAX=0): 4x2 LOD0 at 0x100 (32B).
    // Inactive slots LODBASE1..3 all zero.
    {
        constexpr uint32_t kLod0Base = 0x100u;
        constexpr uint32_t kBlobBytes = 0x20u;
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB
            0x10006404u, 0x00000100u,  // mode 1 MMDMA
            0x10006424u, 0x00000200u,  // source address
            0x10006414u, kLod0Base,
            0x10006428u, kBlobBytes,
            0x10000014u, 0x00000001u,
            0x10006414u, kLod0Base,
            0x10006418u, 0x00000000u,  // inactive lodBase[1] = 0
            0x1000641cu, 0x00000000u,  // inactive lodBase[2] = 0
            0x10006420u, 0x00000000u,  // inactive lodBase[3] = 0
            0x1000642cu, 0x00030001u,  // UMAX=3, VMAX=1 -> 4x2 drawn level
            0x10006430u, 0x00030001u,  // UMASK=3, VMASK=1
            0x10006408u, 0x000029a0u,  // TXTADDRCNTL: LODMAX 0 (1 active level)
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        const float verts[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {4.f, 0.f, 4.f, 0.f},
            {0.f, 4.f, 0.f, 4.f},
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : verts)
            for (float value : vertex) append_float(complete_words, value);

        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes.assign(kBlobBytes, 0u);
        std::vector<uint8_t> level0;
        for (unsigned i = 0; i < 8u; ++i) texel_bytes(kLevel0Rgba, &level0);
        std::copy(level0.begin(), level0.end(), texels.bytes.begin());
        memory.ranges.push_back(std::move(texels));

        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                        backend.uploads.size() == 1,
                    "single-level MMDMA draw with zero inactive bases succeeds"))
            return false;
        const RecordingBackend::Upload& upload = backend.uploads.front();
        if (!expect(upload.w == 4 && upload.h == 2 &&
                        upload.pixels.size() == 8u &&
                        upload.lod_chain.empty(),
                    "LODMAX=0 single-level upload has empty lod_chain"))
            return false;
    }

    return true;
}

// TXTUVMASK address transform.  The accepted State 3 world draw carries
// TXTUVMAX=0x000f007f,
// TXTUVMASK=0x03ff003f and TXTADDRCNTL=0x29a3 with V coordinates far beyond
// the base image (uvy=[762.7, 832.0], mean V 808.9) against a resident the
// host publishes 64x128.  TXTUVMASK is an address mask, not a repeat-enable:
// the checked-in Mercury page builder (`PageCLT_SetUV`) writes mask =
// largest-LOD dimension - 1 on a tiled axis and the 0x3ff full scale on a
// clamped one, and the hardware masks the integer texel coordinate before
// forming the address (MAME `addr_calc`).  A mask period shorter than the
// resident therefore repeats its masked sub-range; it must not clamp to the
// edge row, which is what the host did and why a mean V of 808 sampled the
// last row of a 128-high resident.
bool test_uvmask_address_transform()
{
    // 1. Decode of the accepted tuple.  UMASK 0x3ff -> period 1024 > the
    //    resident width (clamped axis, unchanged); VMASK 0x3f -> period 64,
    //    which the 128-high resident does not exceed (masked axis, repeats).
    //    The same register carries the filter/LOD selection: min 2 (bilinear),
    //    inter 3 (trilinear), mag 2 (bilinear), LOD max 3.
    const m2host::CltTextureSampling accepted =
        m2host::clt_uv_address_probe::decode(0x29a3u, 0x03ff003fu, 64.f, 128.f)
            .sampling;
    if (!expect(accepted.u_period == m2host::kCltUvFullPeriod &&
                    !accepted.wrap_u,
                "accepted draw's full-scale U mask leaves the axis clamped"))
        return false;
    if (!expect(accepted.v_period == 64u && accepted.wrap_v,
                "accepted draw's V mask period 63 repeats inside the "
                "resident instead of clamping"))
        return false;
    if (!expect(accepted.min_filter == m2host::CltTextureFilter::bilinear &&
                    accepted.inter_filter ==
                        m2host::CltTextureFilter::trilinear &&
                    accepted.mag_filter == m2host::CltTextureFilter::bilinear &&
                    accepted.lod_max == 3u,
                "accepted draw's MIP/filter selection decodes from TXTADDRCNTL"))
        return false;

    // 2. Pixel oracle.  A one-texel-wide, four-row resident encodes its own row
    //    in its red channel; a fan covers a 4x4 target with v spanning texels
    //    0..4.  With a V period of 2 the mask repeats rows 0,1,0,1.  The pre-fix
    //    result was the clamp 0,1,2,3 (and a single edge row for guest
    //    coordinates beyond the resident), so this oracle fails on the old
    //    address transform.
    const auto sampled_rows = [](uint32_t v_period, bool wrap_v,
                                 uint32_t* rows) {
        m2host::SoftCaptureBackend backend(4, 4, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        std::array<uint32_t, 4> texels{};
        for (uint32_t y = 0; y < 4u; ++y)
            texels[y] = 0xff000000u | (y + 1u);
        const m2host::CelId texture =
            backend.cel_upload_rgba(1, 4, texels.data());
        m2host::CltTextureSampling sampling;
        sampling.wrap_v = wrap_v;
        sampling.v_period = v_period;
        std::array<m2host::CltVertex, 4> vertices{};
        vertices[0] = {0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[1] = {4.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[2] = {4.f, 4.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        vertices[3] = {0.f, 4.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        backend.clt_draw(vertices.data(), 4, false, true, texture, 4.f, 4.f,
                         false, {}, {}, sampling, {});
        std::array<uint32_t, 16> frame{};
        if (!backend.grab_presented_rgba(frame.data(), 4, 4)) return false;
        for (int y = 0; y < 4; ++y)
            rows[y] = frame[static_cast<size_t>(y) * 4u] & 0xffu;
        return true;
    };
    uint32_t rows[4] = {0u, 0u, 0u, 0u};
    if (!sampled_rows(2u, true, rows)) return false;
    if (!expect(rows[0] == 1u && rows[1] == 2u && rows[2] == 1u &&
                    rows[3] == 2u,
                "a mask period below the resident repeats its sub-range"))
        return false;
    if (!sampled_rows(m2host::kCltUvFullPeriod, false, rows)) return false;
    if (!expect(rows[0] == 1u && rows[1] == 2u && rows[2] == 3u &&
                    rows[3] == 4u,
                "a full-scale mask period still clamps to the resident edge"))
        return false;

    // 3. The same transform on the interpreter's draw path.  The accepted
    //    register tuple is a draw-time property carried to the backend with the
    //    resident's own extent, so the backend sees the period rather than a
    //    bare repeat flag that only holds for period == extent.
    RecordingBackend end_to_end;
    m2host::GuestCltInterpreter interpreter(&end_to_end);
    const std::vector<uint32_t> accepted_words = {
        0x1000643cu, 0x00000404u,  // 4-bit indexed expansion (exp=0x404)
        0x10006404u, 0x00000100u,  // MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // source address
        0x10006414u, 0x00000000u,  // TRAM destination base 0
        0x10006428u, 0x00004000u,  // 16384-byte payload, 4-bit texels
        0x10000014u, 0x00000001u,  // TLD executes the load
        0x1000642cu, 0x000f007fu,  // accepted TXTUVMAX: UMAX 15, VMAX 127
        0x10006430u, 0x03ff003fu,  // accepted TXTUVMASK: UMASK 1023, VMASK 63
        0x10006408u, 0x000029a3u,  // accepted TXTADDRCNTL (LOD max 3)
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00020499u,  // PRIM*TEX multiply form
        0x200a0002u,               // new, texture-only, three-vertex strip
    };
    const float accepted_verts[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {8.f, 0.f, 8.f, 0.f},
        {0.f, 8.f, 0.f, 8.f},
    };
    std::vector<uint32_t> complete_words = accepted_words;
    for (const auto& vertex : accepted_verts)
        for (float value : vertex) append_float(complete_words, value);
    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange source;
    source.guest_address = 0x200u;
    source.bytes.assign(0x4000u, 0u);
    memory.ranges.push_back(std::move(source));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1,
                "accepted TXTUVMASK tuple reaches the backend"))
        return false;
    const m2host::CltTextureSampling drawn = end_to_end.draws[0].sampling;
    return expect(drawn.u_period == m2host::kCltUvFullPeriod &&
                      !drawn.wrap_u && drawn.v_period == 64u &&
                      drawn.wrap_v,
                  "accepted draw carries its mask periods to the backend");
}

// TXTADDRCNTL filter selectors. All three filter fields share one 0..3 selector
// domain. The vendor header lists only point = 0 and bilinear = 2 for
// minification and magnification and all four for inter-level filtering
// (the M2 Toolkit CLT header, lines 71-84),
// but that list is the toolkit's default subset, not a field domain:
// clttxdblend.h aliases its four shared attribute names onto the INTERFILTER
// constants (TX_Nearest = RC_TXTADDRCNTL_INTERFILTER_POINT ... TX_QuasiTrilinear
// = RC_TXTADDRCNTL_INTERFILTER_TRILINEAR), and clttxattr.c programs all three
// TXTADDRCNTL fields from one caller value. Selectors 4..7 have no source-backed
// meaning in any field, so the host fails closed on those and refuses the draw
// instead of inventing a filter. Statically authored literals are
// 0x0/0xf/0x2000/0x2920/0x29a4 plus the LODMAX-only writes in
// CLT_CreateTxLoadCommandList* (M2 SDK src/libs/clt/clttxload.c:186-189).
bool test_reserved_filter_selector_fails_closed()
{
    // Host-word field positions, matching the decode: LODMAX bits 0..3,
    // MAGFILTER bits 4..6, INTERFILTER bits 7..9, MINFILTER bits 10..12, and
    // the texture-enable bit just above them.
    constexpr uint32_t kTextureEnable = 0x00002000u;
    const auto addr_cntl = [](uint32_t min, uint32_t inter, uint32_t mag,
                              uint32_t lod_max) {
        return kTextureEnable | ((min & 7u) << 10) | ((inter & 7u) << 7) |
               ((mag & 7u) << 4) | (lod_max & 0xfu);
    };

    // The accepted State 3 tuple from the neighboring address fixture, with the
    // filter field under test as the only difference. Nothing else in the list
    // can account for a refusal or a delivery.
    struct Outcome {
        m2host::CltExecutionResult result;
        m2host::CltTextureSamplingDecode decode;
        bool delivered = false;
        bool uploaded = false;
        bool textured = false;
        m2host::CltTextureSampling sampling;
    };
    const auto run = [&](uint32_t value) {
        Outcome outcome;
        outcome.decode = m2host::clt_uv_address_probe::decode(
            value, 0x03ff003fu, 128.f, 64.f);
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x1000643cu, 0x00000404u,  // 4-bit indexed expansion (exp=0x404)
            0x10006404u, 0x00000100u,  // MMDMA load, mode 1
            0x10006424u, 0x00000200u,  // source address
            0x10006414u, 0x00000000u,  // TRAM destination base 0
            0x10006428u, 0x00004000u,  // 16384-byte payload, 4-bit texels
            0x10000014u, 0x00000001u,  // TLD executes the load
            0x1000642cu, 0x000f007fu,  // accepted TXTUVMAX: UMAX 15, VMAX 127
            0x10006430u, 0x03ff003fu,  // accepted TXTUVMASK: UMASK 1023, VMASK 63
            0x10006408u, value,        // TXTADDRCNTL under test
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00020499u,  // PRIM*TEX multiply form
            0x200a0002u,               // new, texture-only, three-vertex strip
        };
        const float verts[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {8.f, 0.f, 8.f, 0.f},
            {0.f, 8.f, 0.f, 8.f},
        };
        for (const auto& vertex : verts)
            for (float coordinate : vertex) append_float(words, coordinate);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange source;
        source.guest_address = 0x200u;
        source.bytes.assign(0x4000u, 0u);
        memory.ranges.push_back(std::move(source));
        outcome.result =
            interpreter.execute(words.data(), words.size(), &memory);
        outcome.uploaded = !backend.uploads.empty();
        outcome.delivered = !backend.draws.empty();
        if (outcome.delivered) {
            outcome.sampling = backend.draws[0].sampling;
            outcome.textured = backend.draws[0].textured;
        }
        return outcome;
    };
    const auto message = [](const char* what, const char* field,
                            uint32_t selector) {
        static char text[160];
        std::snprintf(text, sizeof(text), "%s: TXTADDRCNTL %s selector %u",
                      what, field, selector);
        return text;
    };

    // 1. The defined selectors are untouched. This is the accepted register
    //    0x29a3 (min bilinear, inter trilinear, mag bilinear, LODMAX 3) and it
    //    must keep decoding and delivering exactly as before.
    const Outcome accepted = run(addr_cntl(2u, 3u, 2u, 3u));
    if (!expect(accepted.decode.filter_reserved == false &&
                    accepted.result.ok && accepted.result.unsupported == 0 &&
                    accepted.result.draws == 1 && accepted.delivered,
                "defined min/inter/mag selectors still reach the backend"))
        return false;
    if (!expect(accepted.sampling.min_filter ==
                        m2host::CltTextureFilter::bilinear &&
                    accepted.sampling.inter_filter ==
                        m2host::CltTextureFilter::trilinear &&
                    accepted.sampling.mag_filter ==
                        m2host::CltTextureFilter::bilinear &&
                    accepted.decode.sampling.min_filter ==
                        accepted.sampling.min_filter &&
                    accepted.decode.sampling.inter_filter ==
                        accepted.sampling.inter_filter &&
                    accepted.decode.sampling.mag_filter ==
                        accepted.sampling.mag_filter,
                "the delivered draw and the contract probe decode the defined "
                "selectors identically"))
        return false;

    // 2. Wrong-result control. Point is a real deliverable filter: the same
    //    list with min/inter/mag = 0 (TXTADDRCNTL 0x2003) must reach the
    //    backend with a point sampling state. Without this, the empty draw set
    //    asserted for every reserved selector below would prove nothing -- a
    //    decode that fell back to point for a reserved value would deliver
    //    exactly this draw and has to fail `draws == 0`.
    const Outcome point = run(addr_cntl(0u, 0u, 0u, 3u));
    if (!expect(!point.decode.filter_reserved && point.result.ok &&
                    point.result.unsupported == 0 && point.result.draws == 1 &&
                    point.delivered &&
                    point.sampling.min_filter == m2host::CltTextureFilter::point &&
                    point.sampling.inter_filter ==
                        m2host::CltTextureFilter::point &&
                    point.sampling.mag_filter == m2host::CltTextureFilter::point,
                "point delivery is reachable, so a refused draw is a refusal "
                "and not a dead fixture"))
        return false;

    // 3. Every reserved selector fails closed. 4..7 are reserved in all three
    //    fields (the toolkit's four shared names stop at 3), and each case
    //    keeps the other two fields on defined selectors so the refusal is
    //    attributable to the one under test. The resident still materializes --
    //    only the draw is refused -- and the refusal is charged to unsupported
    //    without making the list malformed, matching the other fail-closed draw
    //    paths.
    enum class Field { min_filter, inter_filter, mag_filter };
    const struct {
        Field field;
        const char* name;
        uint32_t reserved[4];
        size_t reserved_count;
    } cases[] = {
        {Field::min_filter, "min", {4u, 5u, 6u, 7u}, 4u},
        {Field::mag_filter, "mag", {4u, 5u, 6u, 7u}, 4u},
        {Field::inter_filter, "inter", {4u, 5u, 6u, 7u}, 4u},
    };
    for (const auto& entry : cases) {
        for (size_t i = 0; i < entry.reserved_count; ++i) {
            const uint32_t selector = entry.reserved[i];
            const uint32_t value =
                entry.field == Field::min_filter
                    ? addr_cntl(selector, 3u, 2u, 3u)
                    : (entry.field == Field::inter_filter
                           ? addr_cntl(2u, selector, 2u, 3u)
                           : addr_cntl(2u, 3u, selector, 3u));
            const Outcome refused = run(value);
            if (!expect(refused.decode.filter_reserved,
                        message("reserved selector is reported as reserved",
                                entry.name, selector)))
                return false;
            if (!expect(refused.result.ok &&
                            refused.result.unsupported == 1 &&
                            refused.result.draws == 0 && !refused.delivered,
                        message("reserved selector refuses the draw instead "
                                "of delivering a filter",
                                entry.name, selector)))
                return false;
            if (!expect(refused.uploaded,
                        message("the refused draw still materializes its "
                                "resident",
                                entry.name, selector)))
                return false;
        }
    }

    // 4. Selectors 0..3 deliver in every field. The three fields share one
    //    selector domain (the toolkit programs all three from one value table),
    //    so linear and trilinear are legitimate minification and magnification
    //    choices and must reach the backend rather than be refused.
    //
    //    This is the measured State 3 regression oracle: an in-race textured
    //    draw authored TXTADDRCNTL = 0x2493 (linear in all three fields) and
    //    every such draw was refused, which was 100% of `unsupported` over a
    //    120-tick race window (5,735 of 58,221 draws). A decode that treats 1
    //    as reserved in min/mag fails the delivered/linear assertions here.
    const Outcome race_linear = run(0x2493u);
    if (!expect(!race_linear.decode.filter_reserved &&
                    race_linear.result.ok &&
                    race_linear.result.unsupported == 0 &&
                    race_linear.result.draws == 1 && race_linear.delivered &&
                    race_linear.sampling.min_filter ==
                        m2host::CltTextureFilter::linear &&
                    race_linear.sampling.inter_filter ==
                        m2host::CltTextureFilter::linear &&
                    race_linear.sampling.mag_filter ==
                        m2host::CltTextureFilter::linear,
                "the race draw's TXTADDRCNTL 0x2493 (linear in all three "
                "fields) is delivered, not refused"))
        return false;
    const Outcome min_trilinear = run(addr_cntl(3u, 3u, 2u, 3u));
    if (!expect(!min_trilinear.decode.filter_reserved &&
                    min_trilinear.delivered &&
                    min_trilinear.sampling.min_filter ==
                        m2host::CltTextureFilter::trilinear,
                "trilinear is a valid minification selector"))
        return false;
    const Outcome mag_linear = run(addr_cntl(2u, 3u, 1u, 3u));
    if (!expect(!mag_linear.decode.filter_reserved && mag_linear.delivered &&
                    mag_linear.sampling.mag_filter ==
                        m2host::CltTextureFilter::linear,
                "linear is a valid magnification selector"))
        return false;
    const Outcome inter_linear = run(addr_cntl(2u, 1u, 2u, 3u));
    if (!expect(!inter_linear.decode.filter_reserved &&
                    inter_linear.result.draws == 1 &&
                    inter_linear.delivered &&
                    inter_linear.sampling.inter_filter ==
                        m2host::CltTextureFilter::linear &&
                    inter_linear.sampling.min_filter ==
                        m2host::CltTextureFilter::bilinear,
                "linear remains a valid inter-level selector"))
        return false;
    const Outcome inter_trilinear = run(addr_cntl(2u, 3u, 2u, 3u));
    if (!expect(!inter_trilinear.decode.filter_reserved &&
                    inter_trilinear.delivered &&
                    inter_trilinear.sampling.inter_filter ==
                        m2host::CltTextureFilter::trilinear,
                "trilinear remains a valid inter-level selector"))
        return false;

    // 5. TEXTUREENABLE gates mapping rather than rejecting the packet: the same
    //    texture-only strip samples its resident with the bit set and is an
    //    untextured prim-colour draw with the bit cleared. Both directions are
    //    asserted, because only one of them would still pass for a host that
    //    ignored the field (cleared still sampling) or over-applied it (cleared
    //    refused as a texture-state failure).
    const Outcome enabled = run(addr_cntl(2u, 3u, 2u, 3u));
    if (!expect(enabled.result.ok && enabled.result.unsupported == 0 &&
                    enabled.result.draws == 1 && enabled.delivered &&
                    enabled.textured,
                "set TEXTUREENABLE samples the resident"))
        return false;
    const Outcome disabled = run(addr_cntl(2u, 3u, 2u, 3u) & ~kTextureEnable);
    if (!expect(disabled.result.ok && disabled.result.unsupported == 0 &&
                    disabled.result.draws == 1 && disabled.delivered &&
                    !disabled.textured,
                "cleared TEXTUREENABLE makes a texture-only packet an untextured draw"))
        return false;
    return true;
}

// A draw aimed into the middle of a resident samples the TRAM bytes at *its*
// LOD base with *its* armed registers: the M2 texture engine has no resident
// decode to reuse. The resident still decides which bytes those are (a later
// blast that overwrote the drawn address owns them), but the format, window
// and PIP selection come from the draw.
//
// Oracle: a literal A7+SSB resident (8 bits per texel, so the byte at a texel
// offset is its alpha, MSB-first) with a distinctive alpha at byte 0 and a
// different one at byte 0x400. A 2x2 single-LOD load materializes the
// resident's own decode from byte 0; the interior draw at 0x400 must then
// publish a *second* decode whose first texel reads byte 0x400 instead. The
// pre-fix host served the resident's own decode for both draws, so the second
// assertion fails there.
bool test_stale_region_draw_uses_armed_decode()
{
    const uint32_t first_alpha = 0x01u;   // byte 0    -> widened 0x02
    const uint32_t interior_alpha = 0x40u;  // byte 0x400 -> widened 0x81
    // Alpha is the high byte of the host's RGBA word; a black texel keeps only
    // its widened alpha.
    const uint32_t first_pixel = doc_widen_msb(7u, first_alpha) << 24u;
    const uint32_t interior_pixel = doc_widen_msb(7u, interior_alpha) << 24u;
    if (!expect(doc_widen_msb(7u, first_alpha) == 0x02u &&
                    doc_widen_msb(7u, interior_alpha) == 0x81u,
                "A7 oracle alphas widen away from each other"))
        return false;

    std::vector<uint8_t> blob(0x2000u, 0u);
    for (uint8_t& byte : blob) byte = static_cast<uint8_t>(0x80u | 0x02u);
    blob[0] = static_cast<uint8_t>(0x80u | first_alpha);
    blob[0x400] = static_cast<uint8_t>(interior_alpha);  // SSB 0

    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 2.f, 0.f, 2.f},
    };
    const auto run = [&](std::vector<uint32_t> words) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> complete = words;
        for (const auto& vertex : quad)
            for (float value : vertex) append_float(complete, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange range;
        range.guest_address = 0x200u;
        range.bytes = blob;
        memory.ranges.push_back(std::move(range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete.data(), complete.size(), &memory);
        return std::make_pair(result, std::move(backend));
    };

    // Case 1: the resident is 2x2 A7+SSB at base 0 and the interior draw arms
    // a 2x4 use state at LOD base 0x400. The armed registers determine both
    // the re-decode geometry and the UV divisor; retaining the resident's
    // height would clamp the quad's V=2 edge to 1 instead of mapping it to
    // 0.5 in the 4-row draw-time decode.
    std::vector<uint32_t> interior = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // source at 0x200
        0x10006414u, 0x00000000u,  // DMA destination base 0
        0x10006428u, 0x00002000u,  // 0x2000-byte payload
        0x1000643cu, 0x00001a70u,  // literal A7+SSB format at load time
        0x10000014u, 0x00000001u,  // TLD stages the blob
        0x1000642cu, 0x00010001u,  // UMAX=VMAX=1 -> 2x2 resident
        0x10006430u, 0x03ff03ffu,  // full-scale masks: both axes clamped
        0x10006408u, 0x00002000u,  // single-LOD addressing, texture enable
        0x1000640cu, 0x00004900u,  // color, alpha, and SSB from texture
        0x10006410u, 0x00009200u,
        0x1000643cu, 0x00001a70u,  // use-state write -> materialize
        0x1000642cu, 0x00010003u,  // draw arms a 2x4 window
        0x10006414u, 0x00000400u,  // the draw names an interior LOD base
        0x1000800cu, 0x00000004u,  // discard armed-decode SSB 0
        0x200a0002u,
    };
    {
        auto run_result = run(interior);
        const m2host::CltExecutionResult& result = run_result.first;
        RecordingBackend& backend = run_result.second;
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.uploads.size() >= 3u,
                    "an interior draw publishes its own decode"))
            return false;
        if (!expect(backend.uploads[0].w == 2 && backend.uploads[0].h == 2 &&
                        backend.uploads[0].pixels[0] == first_pixel,
                    "the resident's own decode starts at its DMA base"))
            return false;
        if (!expect(backend.draws[0].cel == 2, "the interior draw is served "
                                               "the re-decoded cel"))
            return false;
        if (!expect(backend.uploads[1].w == 2 && backend.uploads[1].h == 4,
                    "the interior draw publishes its armed geometry"))
            return false;
        if (!expect(backend.uploads[1].pixels[0] == interior_pixel &&
                        backend.uploads[1].pixels[0] != first_pixel,
                    "the interior draw samples the bytes at its own LOD base"))
            return false;
        if (!expect(backend.draws[0].vertices.size() == 3u &&
                        backend.draws[0].vertices[2].v == 0.5f,
                    "the interior draw normalizes V by its armed height"))
            return false;
        if (!expect(backend.draws[0].discard.ssb_zero &&
                        backend.draws[0].discard.ssb_mask == 3 &&
                        backend.uploads[2].w == 2 &&
                        backend.uploads[2].h == 4 &&
                        backend.uploads[2].pixels[0] == 0u &&
                        backend.uploads[2].pixels[1] == 0xffffffffu,
                    "the interior draw uses its armed SSB sidecar"))
            return false;
    }

    // Case 2: a stale use state whose registers do not determine a footprint
    // (a MIP-chain address mode whose LOD quad does not bracket the drawn base,
    // a mask-free window larger than the payload) keeps the resident's own
    // decode -- the only remaining sizes would be payload guesses calibrated
    // for a blob that starts at its own base.
    std::vector<uint32_t> undetermined = {
        0x10006404u, 0x00000100u,
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00002000u,
        0x1000643cu, 0x00001a70u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010001u,
        0x10006430u, 0x03ff03ffu,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00004900u,
        0x10006410u, 0x00009200u,
        0x1000643cu, 0x00001a70u,  // materialize the 2x2 resident
        0x1000642cu, 0x007f007fu,  // stale: a 128x128 wrap window
        0x10006430u, 0x00000000u,  // and no addressing mask
        0x10006408u, 0x00000003u,  // and MIP addressing with a flat LOD quad
        0x10006414u, 0x00000400u,
        0x200a0002u,
    };
    {
        auto run_result = run(undetermined);
        const m2host::CltExecutionResult& result = run_result.first;
        RecordingBackend& backend = run_result.second;
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1,
                    "an undetermined interior draw is still drawn"))
            return false;
        if (!expect(backend.uploads.size() == 1u, "an undetermined interior "
                                                  "draw invents no decode"))
            return false;
        if (!expect(backend.uploads[0].pixels[0] == first_pixel,
                    "an undetermined interior draw keeps the resident's own "
                    "decode"))
            return false;
    }
    return true;
}

// Only the MMDMA materializer retains staged TRAM bytes, so the per-draw
// armed re-decode contract above has always been an MMDMA-resident property.
// The publishers are still distinguished structurally: a full-load (mode 0)
// resident at the same base with the same bytes is served by its own decode
// (uploads.size() == 1 -- no second decode is invented, which is the
// fail-closed behavior recorded for non-MMDMA publications), and the
// draw-time publish stamps stay MMDMA throughout. The draw-diagnostic publisher audit
// fields carry the production-side accounting (resolved:
// non-MMDMA count 0/1024 audited race draws, "The serving residents are
// MMDMA-published").
bool test_publisher_discrimination_full_load_vs_mmdma()
{
    const uint32_t first_alpha = 0x01u;  // byte 0    -> widened 0x02
    const uint32_t interior_alpha = 0x40u;  // byte 0x400 -> widened 0x81
    const uint32_t first_pixel =
        m2host::clt_tex_expand_probe::widen(7u, first_alpha) << 24u;
    const uint32_t interior_pixel =
        m2host::clt_tex_expand_probe::widen(7u, interior_alpha) << 24u;
    if (!expect(m2host::clt_tex_expand_probe::widen(7u, first_alpha) == 0x02u &&
                    m2host::clt_tex_expand_probe::widen(7u, interior_alpha) ==
                        0x81u,
                "the widening probe distinguishes the two fixture alphas"))
        return false;

    std::vector<uint8_t> blob(0x2000u, 0u);
    for (uint8_t& byte : blob) byte = static_cast<uint8_t>(0x80u | 0x02u);
    blob[0] = static_cast<uint8_t>(0x80u | first_alpha);
    blob[0x400] = static_cast<uint8_t>(0x80u | interior_alpha);

    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 2.f, 0.f, 2.f},
    };
    const auto run = [&](std::vector<uint32_t> words) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> complete = words;
        for (const auto& vertex : quad)
            for (float value : vertex) append_float(complete, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange range;
        range.guest_address = 0x200u;
        range.bytes = blob;
        memory.ranges.push_back(std::move(range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete.data(), complete.size(), &memory);
        return std::make_pair(result, std::move(backend));
    };

    // A full-load (mode 0) publication of the identical bytes, drawn at the
    // same interior base under the same determined armed state, invents no
    // second decode: non-MMDMA residents keep their publication decode. The
    // load is 4 texels/row x 258 rows of 8-bit A7+SSB texels, so its span
    // [0, 0x810) contains the drawn base 0x400.
    std::vector<uint32_t> full_load = {
        0x10006404u, 0x00000000u,  // mode 0 raw texture load
        0x10006424u, 0x00000200u,  // source at 0x200
        0x10006414u, 0x00000000u,  // destination base 0
        0x10006428u, 0x00000102u,  // 258 rows
        0x1000642cu, 0x00200020u,  // 32 raw bits per row -> 4 texels wide
        0x1000643cu, 0x00001a70u,  // literal A7+SSB load format
        0x10000014u, 0x00000001u,  // TLD decodes and publishes the resident
        0x1000642cu, 0x00010001u,  // 2x2 use-state window
        0x10006430u, 0x03ff03ffu,  // full-scale masks: both axes clamped
        0x10006408u, 0x00002000u,  // single-LOD addressing, texture enable
        0x1000640cu, 0x00004900u,
        0x10006410u, 0x00009200u,
        0x10006414u, 0x00000400u,  // the draw names an interior LOD base
        0x200a0002u,
    };
    {
        auto run_result = run(full_load);
        const m2host::CltExecutionResult& result = run_result.first;
        RecordingBackend& backend = run_result.second;
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1,
                    "a full-load resident serves its interior draw"))
            return false;
        if (!expect(backend.uploads.size() == 1u,
                    "a non-MMDMA resident invents no second decode"))
            return false;
        if (!expect(backend.uploads[0].w == 4 && backend.uploads[0].h == 258 &&
                        backend.uploads[0].pixels[0] == first_pixel,
                    "the full-load resident's own decode starts at its base"))
            return false;
        if (!expect(backend.draws[0].cel == 1,
                    "the interior draw samples the full-load publication"))
            return false;
    }

    // The same bytes through the MMDMA path produce exactly one more decode:
    // the per-draw armed re-decode that only the retained-byte publisher
    // supports.
    std::vector<uint32_t> mmdma = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00002000u,  // 0x2000-byte payload
        0x1000643cu, 0x00001a70u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010001u,
        0x10006430u, 0x03ff03ffu,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00004900u,
        0x10006410u, 0x00009200u,
        0x1000643cu, 0x00001a70u,  // materialize the 2x2 resident
        0x10006414u, 0x00000400u,  // interior LOD base
        0x200a0002u,
    };
    {
        auto run_result = run(mmdma);
        const m2host::CltExecutionResult& result = run_result.first;
        RecordingBackend& backend = run_result.second;
        if (!expect(result.ok && result.unsupported == 0 && result.draws == 1,
                    "the MMDMA twin serves its interior draw"))
            return false;
        if (!expect(backend.uploads.size() == 2u,
                    "the retained-byte resident re-decodes for the interior "
                    "draw"))
            return false;
        if (!expect(backend.uploads[1].pixels[0] == interior_pixel &&
                        backend.uploads[1].pixels[0] != first_pixel,
                    "the MMDMA re-decode samples the bytes at the drawn base"))
            return false;
    }
    return true;
}

// The guest Z plane is reciprocal-W: the emitted NDC z maps onto
// clamp(reciprocal-W) in [0,1], so the far end of the plane is 0.0 and the
// guest's authored clear-Z fill word is an IEEE float.  The `clean` backend
// cleared to GL's default 1.0 -- the *nearest* value -- and never implemented
// `clt_clear_z`, so every DBZCNTL mask that actually compares rejected its
// geometry; the race world authors such a mask (0x5) while the showroom and
// menus author the all-relations mask (0x7) and were unaffected.
bool test_te_z_plane_clear_contract()
{
    using m2host::kCltTeZFarDepth;
    if (!expect(kCltTeZFarDepth == 0.f,
                "the guest Z plane's far value is reciprocal-W zero"))
        return false;
    const std::array<std::pair<uint32_t, float>, 7> fills = {{
        {0x00000000u, 0.f},   // 0.0f
        {0x3f000000u, 0.5f},  // 0.5f
        {0x3f800000u, 1.f},   // 1.0f
        {0xbf800000u, 0.f},   // -1.0f clamps to far
        {0x40000000u, 1.f},   // 2.0f clamps to near
        {0x7fc00000u, 0.f},   // NaN falls back to far
        {0xff800000u, 0.f},   // -inf falls back to far
    }};
    for (const auto& [word, expected] : fills) {
        if (!expect(m2host::clt_te_z_fill_depth(word) == expected,
                    "the authored Z fill word maps onto the plane's [0,1] range"))
            return false;
    }

    // The race world's mask is PIXOUTONSMALLERZ | PIXOUTONEQUALZ (cltmacros.h
    // PPCBIT(31)/PPCBIT(29)), i.e. the host's greater-equal relation on a plane
    // whose nearer values are larger.  Nearer-than-far geometry passes against
    // the far clear and fails against a nearest-valued one.
    const auto race_compare = m2host::clt_backend_depth_compare_for_dbz(
        m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq);
    if (!expect(m2host::clt_backend_depth_passes(race_compare, 0.4f,
                                                 kCltTeZFarDepth),
                "race geometry passes against a far-cleared Z plane"))
        return false;
    if (!expect(!m2host::clt_backend_depth_passes(race_compare, 0.4f, 1.f),
                "a nearest-valued clear rejects the same race geometry"))
        return false;
    if (!expect(!m2host::clt_backend_depth_passes(race_compare, 0.f, 1.f),
                "a nearest-valued clear rejects even the farthest plane value"))
        return false;

    // The showroom and menu mask carries all three pixel relations, so it is
    // indifferent to the clear value -- the reason only the race lost geometry.
    const auto menu_compare = m2host::clt_backend_depth_compare_for_dbz(
        m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq |
        m2host::kCltDbzPixelGt);
    if (!expect(m2host::clt_backend_depth_passes(menu_compare, 0.4f, 1.f) &&
                    m2host::clt_backend_depth_passes(menu_compare, 0.4f,
                                                     kCltTeZFarDepth),
                "the all-relations mask ignores the clear value"))
        return false;
    // ...and the Z *write* side of the same mask correctly keeps rejecting the
    // plane values it should, so the far clear does not disable occlusion.
    if (!expect(m2host::clt_depth_write_pass(
                    m2host::kCltDbzPixelLt | m2host::kCltDbzPixelEq, 0.4f, 0.9f,
                    true, true) == false,
                "the race mask writes no Z over nearer stored geometry"))
        return false;
    return true;
}

bool test_te_framebuffer_clear_contract()
{
    constexpr uint32_t red = 0x3f000000u;    // 0.5f
    constexpr uint32_t green = 0x3e800000u;  // 0.25f
    constexpr uint32_t blue = 0x3e000000u;   // 0.125f
    constexpr uint32_t alpha = 0x3f400000u;  // 0.75f

    // The vendor clear list is an untextured full-surface strip whose
    // primary color/alpha comes from the four TEFrameBufferInfo words. The
    // software backend must therefore clear the active color target without
    // adding a guest CLT draw. This arm publishes no destination extent
    // (`DBFBCLIP` = 0), which is the documented no-bound-surface form: the
    // platform fill keeps the whole target, as the next arm contrasts.
    m2host::SoftCaptureBackend backend(4, 4, {});
    m2host::GuestCltInterpreter interpreter(&backend);
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, 0x1000u, 4u, 0u, 0u, 0u, red, green, blue, alpha, 1u);
    interpreter.clear_color_buffer();
    std::array<uint32_t, 16> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 4, 4),
                "TE framebuffer clear exposes the active color target"))
        return false;
    if (!expect(frame[0] == 0xbf204080u,
                "TE framebuffer clear expands the four authored float words"))
        return false;
    for (uint32_t pixel : frame) {
        if (!expect(pixel == frame[0],
                    "TE framebuffer clear covers the entire active target"))
            return false;
    }

    // A bound destination surface bounds the clear too. The recovered driver
    // answers TE_CLEAR_FRAME_BUFFER with a quad from the surface origin to the
    // extent the bound bitmap published at SetTEFrameBuffer
    // (driver.c:251-278 `UpdateClearFBCmdList`), and the TE clips that list
    // with the same DBFBCLIP/window state as any other primitive. So a clear on
    // a surface smaller than the display must leave the host's surrounding
    // border holding its previous content instead of painting the whole
    // target. The three arms below are: the whole-target extent, a 2x2 extent
    // inside it, and an executed control that clears the same target with no
    // published extent -- the control must reach the pixels the bounded arm
    // left alone, or the bounded assertion would be vacuous.
    m2host::SoftCaptureBackend bounded(4, 4, {});
    m2host::GuestCltInterpreter bounded_interpreter(&bounded);
    bounded.frame_begin(0.f, 0.f, 0.f);
    // Arm 1: the same surface publishes the whole 4x4 target.
    bounded_interpreter.set_te_surface_state(
        0x7u, (4u << 16) | 4u, 0u, 0x3000u, 4u, 0u, 0u, 0u, red, green, blue,
        alpha, 1u);
    bounded_interpreter.clear_color_buffer();
    // Arm 2: the destination surface is 2x2 at the display origin.
    bounded_interpreter.set_te_surface_state(
        0x7u, (2u << 16) | 2u, 0u, 0x3000u, 4u, 0u, 0u, 0u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 2u);
    bounded_interpreter.clear_color_buffer();
    std::array<uint32_t, 16> bounded_frame{};
    if (!expect(bounded.grab_presented_rgba(bounded_frame.data(), 4, 4),
                "the bounded clear exposes the active color target"))
        return false;
    if (!expect(bounded_frame[0] == 0xffffffffu &&
                    bounded_frame[5] == 0xffffffffu,
                "the bounded clear fills the published 2x2 extent"))
        return false;
    if (!expect(bounded_frame[2] == 0xbf204080u &&
                    bounded_frame[3] == 0xbf204080u &&
                    bounded_frame[8] == 0xbf204080u &&
                    bounded_frame[15] == 0xbf204080u,
                "the bounded clear leaves the display border untouched -- a "
                "whole-target fill is the wrong result here"))
        return false;

    // Control, executed in the same fixture and target: with no published
    // extent the same platform fill covers every pixel, including the border
    // the bounded arm preserved. If it did not, the arm above would not be
    // distinguishing a bounded clear from an unbounded one.
    bounded_interpreter.set_te_surface_state(
        0x7u, 0u, 0u, 0x3000u, 4u, 0u, 0u, 0u, 0x3f800000u, 0u, 0u,
        0x3f800000u, 3u);
    bounded_interpreter.clear_color_buffer();
    if (!bounded.grab_presented_rgba(bounded_frame.data(), 4, 4)) {
        expect(false, "the unbounded control reads the active color target");
        return false;
    }
    if (!expect(bounded_frame[2] == 0xff0000ffu &&
                    bounded_frame[15] == 0xff0000ffu,
                "control -- with no published extent the same clear reaches "
                "the pixels the bounded arm preserved")) {
        std::fprintf(stderr, "  control pixels: %08x %08x %08x %08x\n",
                     bounded_frame[0], bounded_frame[2], bounded_frame[5],
                     bounded_frame[15]);
        return false;
    }

    // The bridge accepts the same zero-length live-buffer form as the device
    // driver and preserves the vendor's Z-before-framebuffer if/else when
    // both clear bits are requested.
    RecordingBackend recording;
    m2host::GuestCltBridge bridge(&recording);
    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x1000u, 4u, 0u, 0u, 0u,
                                   red, green, blue, alpha);
    bridge.enqueue(0u, 0x000d0003u, 0x1u, 0u, 0u, nullptr, nullptr, 0u);
    bridge.pump();
    if (!expect(recording.color_clears == 1 &&
                    recording.last_color_words[0] == red &&
                    recording.last_color_words[1] == green &&
                    recording.last_color_words[2] == blue &&
                    recording.last_color_words[3] == alpha,
                "TE_CLEAR_FRAME_BUFFER clears the active color target"))
        return false;

    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x1000u, 4u, 0x2000u, 0u,
                                   0u, red, green, blue, alpha);
    bridge.enqueue(0u, 0x000d0003u, 0x3u, 0u, 0u, nullptr, nullptr, 0u);
    bridge.pump();
    return expect(recording.z_clears == 1 && recording.color_clears == 1,
                  "Z clear takes precedence over framebuffer clear");
}

// A submitted list must bind its own destination even when it inherits an
// unchanged TE surface generation. The presenter binds the default drawable at
// every host frame boundary, so "the surface registers did not change" is not a
// statement about what the backend has bound. The generation guard exists to
// skip redundant register/clip work, and using it to skip the rebind sent the
// list's CLT draws to the drawable and left the addressed guest target frozen
// at its previous content -- which is the alternating live/stale display carve
// reported on the showroom-to-race transition (two display buffers composed
// from the same source and the same resolved destination blend, one advancing
// and one constant).
bool test_te_surface_rebind_after_present()
{
    constexpr float red = 0x3f000000u;    // 0.5f
    constexpr float green = 0x3e800000u;  // 0.25f
    constexpr float blue = 0x3f400000u;   // 0.75f
    constexpr float alpha = 0x3f800000u;  // 1.0f
    constexpr uint32_t dest = 0x4f0000u;
    constexpr uint32_t z_base = 0x56a000u;

    RecordingBackend recording;
    m2host::GuestCltBridge bridge(&recording);

    // One surface-generation bump, then two lists submitted under it. This is
    // the transition shape: the guest arms the destination once and submits a
    // whole sequence of lists against it.
    bridge.update_te_surface_state(0x7u, 0u, 0u, dest, 576u, z_base, 0u, 0u,
                                   red, green, blue, alpha);

    recording.target_selects.clear();
    bridge.enqueue(0u, 0x000d0003u, 0x1u, 0u, 0u, nullptr, nullptr, 0u);
    bridge.pump();
    if (!expect(recording.bound_target == dest,
                "the first submitted list binds its guest destination"))
        return false;

    // The presenter runs between guest frames and unbinds the target.
    recording.frame_present();
    if (!expect(recording.bound_target == 0u,
                "present() leaves the default drawable bound"))
        return false;

    // The second list carries the *same* surface generation. It must still
    // re-establish its destination; the stale-generation shortcut used to
    // return before the rebind, so these draws went to the drawable and the
    // guest target never advanced.
    bridge.enqueue(0u, 0x000d0003u, 0x1u, 0u, 0u, nullptr, nullptr, 0u);
    bridge.pump();
    if (!expect(recording.bound_target == dest,
                "an unchanged-generation list rebinds its destination after "
                "present"))
        return false;
    if (!expect(recording.target_selects.size() == 2u &&
                    recording.target_selects[0] == dest &&
                    recording.target_selects[1] == dest,
                "every submitted list selects its destination exactly once"))
        return false;

    // Wrong-result control: a sibling destination under its own generation
    // must bind its own target, not the one the previous list bound.
    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x476000u, 576u, z_base, 0u,
                                   0u, red, green, blue, alpha);
    bridge.enqueue(0u, 0x000d0003u, 0x1u, 0u, 0u, nullptr, nullptr, 0u);
    bridge.pump();
    return expect(recording.bound_target == 0x476000u,
                  "a new surface generation binds the sibling destination");
}

// A DB-source snapshot taken from a renderer-owned target must be invalidated
// when the same copied list writes that target before a later source read. The
// fixture exercises both a pre-list cache and an earlier same-list cache.
bool test_mid_list_destination_generation()
{
    constexpr uint32_t source_target = 0x4f0000u;
    constexpr uint32_t other_target = 0x476000u;
    constexpr uint32_t carve_bytes = 576u * 432u * 2u;
    constexpr uint32_t old_pixel = 0xff0000ffu;  // opaque red
    constexpr uint32_t new_pixel = 0xffff0000u;  // opaque blue

    // Seed the in-progress DB-source cache with red, then make the next list
    // begin on a different destination.  That list writes the source target,
    // draws blue into it, and only then samples it.  A pre-list-only stamp
    // leaves the cached red cel valid and the final copy is observably wrong.
    m2host::SoftCaptureBackend backend(640, 480, {});
    backend.frame_begin(0.05f, 0.07f, 0.10f);
    std::vector<uint32_t> initial(640u * 480u, old_pixel);
    backend.clt_target_select(source_target);
    backend.clt_target_upload_rgba(source_target, initial.data(), 640, 480);
    backend.clt_target_select(other_target);

    m2host::GuestCltInterpreter interpreter(&backend);
    interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
    interpreter.set_fb_spans({{0x2018u, source_target, carve_bytes}});
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, source_target, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);

    const auto append_fullscreen_quad = [](std::vector<uint32_t>& words) {
        words.push_back(0x200c0003u);
        const float vertices[][3] = {
            {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
    };

    std::vector<uint32_t> seed_words = {
        0x10008008u, 0x00000020u,
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
    };
    append_fullscreen_quad(seed_words);
    const m2host::CltExecutionResult seed_result = interpreter.execute(
        seed_words.data(), seed_words.size());
    if (!expect(seed_result.ok && seed_result.unsupported == 0 &&
                    seed_result.draws == 2,
                "mid-list destination fixture seeds the DB-source cache"))
        return false;

    // Start the next list on the sibling.  The explicit destination write is
    // the state transition under test; the source read follows the write in
    // the same copied command list.
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, other_target, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 2u);
    std::vector<uint32_t> mid_list = {
        0x10008008u, 0x00000000u,  // disable the old source arm
        0x10008028u, source_target,  // DBDESTBASEADDR
        0x10008054u, 0x000000ffu,  // DBCONSTIN = blue
    };
    append_fullscreen_quad(mid_list);
    mid_list.insert(mid_list.end(), {
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
        0x10008008u, 0x00000020u,
    });
    append_fullscreen_quad(mid_list);

    const m2host::CltExecutionResult mid_result = interpreter.execute(
        mid_list.data(), mid_list.size());
    if (!expect(mid_result.ok && mid_result.unsupported == 0 &&
                    mid_result.dest_base_writes == 1 &&
                    mid_result.dest_base_value == source_target &&
                    mid_result.draws == 4,
                "mid-list destination write reaches the production path"))
        return false;

    std::vector<uint32_t> final_target(640u * 480u);
    if (!expect(backend.grab_clt_target_rgba(
                    source_target, final_target.data(), 640, 480),
                "mid-list destination fixture reads its target"))
        return false;
    const uint32_t sampled = final_target[24u * 640u + 32u];
    if (!expect(sampled == new_pixel,
                "a DB-source read after a mid-list destination write uses "
                "the new target snapshot"))
        return false;
    if (!expect(sampled != old_pixel,
                "the stale pre-list DB-source snapshot is rejected"))
        return false;

    // Also exercise a source read before the mid-list write. The first read
    // seeds the cel for this list; the later write must advance the state
    // generation, not merely replace the map entry with the list-entry value.
    backend.clt_target_upload_rgba(source_target, initial.data(), 640, 480);
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, source_target, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 3u);
    std::vector<uint32_t> same_list = {
        0x10008008u, 0x00000020u,
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
    };
    append_fullscreen_quad(same_list);
    same_list.insert(same_list.end(), {
        0x10008008u, 0x00000000u,
        0x10008028u, source_target,
        0x10008054u, 0x000000ffu,
    });
    append_fullscreen_quad(same_list);
    same_list.insert(same_list.end(), {
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
        0x10008008u, 0x00000020u,
    });
    append_fullscreen_quad(same_list);
    const m2host::CltExecutionResult same_result = interpreter.execute(
        same_list.data(), same_list.size());
    if (!expect(same_result.ok && same_result.unsupported == 0 &&
                    same_result.dest_base_writes == 1 &&
                    same_result.draws == 6,
                "a source read before a mid-list destination write is "
                "re-snapshotted after the write"))
        return false;
    if (!expect(backend.grab_clt_target_rgba(
                    source_target, final_target.data(), 640, 480),
                "same-list destination fixture reads its target"))
        return false;
    return expect(final_target[24u * 640u + 32u] == new_pixel,
                  "a mid-list write invalidates an earlier same-list source "
                  "snapshot");
}

// A draw into the source target must invalidate an earlier same-list
// DB-source snapshot even without a DBDESTBASEADDR write. The Main Menu draws
// its footer (the list's first source read), then the IMSA banner, then the
// checkered flag, whose general secondary blend reads the target beneath it:
// a list-scoped snapshot showed the background instead of the banner.
bool test_same_list_draw_invalidates_db_source()
{
    constexpr uint32_t source_target = 0x4f0000u;
    constexpr uint32_t carve_bytes = 576u * 432u * 2u;
    constexpr uint32_t old_pixel = 0xff0000ffu;  // opaque red
    constexpr uint32_t new_pixel = 0xffff0000u;  // opaque blue

    m2host::SoftCaptureBackend backend(640, 480, {});
    backend.frame_begin(0.05f, 0.07f, 0.10f);
    std::vector<uint32_t> initial(640u * 480u, old_pixel);
    backend.clt_target_select(source_target);
    backend.clt_target_upload_rgba(source_target, initial.data(), 640, 480);

    m2host::GuestCltInterpreter interpreter(&backend);
    interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
    interpreter.set_fb_spans({{0x2018u, source_target, carve_bytes}});
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, source_target, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);

    const auto append_fullscreen_quad = [](std::vector<uint32_t>& words) {
        words.push_back(0x200c0003u);
        const float vertices[][3] = {
            {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
    };

    // Read red (seeding the cel), draw blue with the source disabled, then
    // read again. The destination register is never written.
    std::vector<uint32_t> words = {
        0x10008008u, 0x00000020u,
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
    };
    append_fullscreen_quad(words);
    words.insert(words.end(), {
        0x10008008u, 0x00000000u,
        0x10008054u, 0x000000ffu,  // DBCONSTIN = blue
    });
    append_fullscreen_quad(words);
    words.insert(words.end(), {0x10008008u, 0x00000020u});
    append_fullscreen_quad(words);
    const m2host::CltExecutionResult result =
        interpreter.execute(words.data(), words.size());
    if (!expect(result.ok && result.unsupported == 0 &&
                    result.dest_base_writes == 0 && result.draws == 6,
                "same-list draw fixture reaches the production path"))
        return false;

    std::vector<uint32_t> final_target(640u * 480u);
    if (!expect(backend.grab_clt_target_rgba(
                    source_target, final_target.data(), 640, 480),
                "same-list draw fixture reads its target"))
        return false;
    const uint32_t sampled = final_target[24u * 640u + 32u];
    if (!expect(sampled != old_pixel,
                "a same-list draw rejects the earlier DB-source snapshot"))
        return false;
    return expect(sampled == new_pixel,
                  "a DB-source read after a same-list draw observes it");
}

// The same-list overlap test must compare the read and the earlier draw in one
// coordinate space. Hall of Fame draws the "Best Average Speed" header at
// viewport (278,90)-(409,117), then the checkered stripe at (0,67)-(459,98)
// reads the target beneath it. Recorded dirty bounds carry the 32,24 viewport
// origin; testing the stripe's unshifted bounds against them missed the
// overlap, and the stale snapshot erased the header's top rows.
bool test_viewport_draw_invalidates_db_source()
{
    constexpr uint32_t source_target = 0x4f0000u;
    constexpr uint32_t carve_bytes = 576u * 432u * 2u;
    constexpr uint32_t old_pixel = 0xff0000ffu;  // opaque red
    constexpr uint32_t new_pixel = 0xffff0000u;  // opaque blue

    m2host::SoftCaptureBackend backend(640, 480, {});
    backend.frame_begin(0.05f, 0.07f, 0.10f);
    std::vector<uint32_t> initial(640u * 480u, old_pixel);
    backend.clt_target_select(source_target);
    backend.clt_target_upload_rgba(source_target, initial.data(), 640, 480);

    m2host::GuestCltInterpreter interpreter(&backend);
    interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
    interpreter.set_fb_spans({{0x2018u, source_target, carve_bytes}});
    interpreter.set_te_surface_state(
        0x7u, 0u, 0u, source_target, 576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);

    const auto append_quad = [](std::vector<uint32_t>& words, float x0,
                                float y0, float x1, float y1) {
        words.push_back(0x200c0003u);
        const float vertices[][3] = {
            {x0, y0, 1.f}, {x0, y1, 1.f}, {x1, y0, 1.f}, {x1, y1, 1.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
    };

    // Seed the snapshot with red. A second, distant read re-snapshots after
    // the full-screen seed draw, leaving only its own small bounds dirty, as
    // the header's own source read does in the game. Then draw the blue
    // "header" and read through the "stripe". Without the viewport shift the
    // read's rows 67-98 do not reach the header's recorded rows 114-141.
    std::vector<uint32_t> words = {
        0x10008008u, 0x00000020u,
        0x10049030u, 0x00000002u, source_target, 0x00000240u,
        0x00000000u, 0x00000000u,
    };
    append_quad(words, 0.f, 0.f, 576.f, 432.f);
    append_quad(words, 500.f, 400.f, 510.f, 410.f);
    words.insert(words.end(), {
        0x10008008u, 0x00000000u,
        0x10008054u, 0x000000ffu,  // DBCONSTIN = blue
    });
    append_quad(words, 278.f, 90.f, 409.f, 117.f);
    words.insert(words.end(), {0x10008008u, 0x00000020u});
    append_quad(words, 0.f, 67.f, 459.f, 98.f);
    const m2host::CltExecutionResult result =
        interpreter.execute(words.data(), words.size());
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 8,
                "viewport draw fixture reaches the production path"))
        return false;

    std::vector<uint32_t> final_target(640u * 480u);
    if (!expect(backend.grab_clt_target_rgba(
                    source_target, final_target.data(), 640, 480),
                "viewport draw fixture reads its target"))
        return false;
    // Display (320,118) is viewport (288,94): inside the header and the read.
    const uint32_t sampled = final_target[118u * 640u + 320u];
    if (!expect(sampled != old_pixel,
                "a viewport-offset draw rejects the earlier DB-source "
                "snapshot"))
        return false;
    return expect(sampled == new_pixel,
                  "a DB-source read over a viewport-offset draw observes it");
}

// The TE color write is bounded by the destination surface the guest bound
// with `SetTEFrameBuffer`, not by the display it is scanned into. The accepted
// guest View is a 640x480 display whose attached Bitmap is a 576x432 clip, so
// the two rectangles differ: a draw running past the bitmap edge must be
// scissored inside the bitmap instead of painting the display border with
// geometry the original never shows there (the recovered Portfolio driver
// programs the frame-buffer clip whenever it binds a framebuffer). The guest's
// own window clip is a second, independent rectangle and every authored draw
// leaves it disabled, so the destination rect is what bounds those draws.
bool test_destination_window_containment()
{
    constexpr uint32_t kDestinationClip = (576u << 16u) | 432u;
    const auto vertex_words = [] {
        std::vector<uint32_t> words = {0x20090002u};
        const float vertex[][6] = {
            {10.f, 20.f, 1.f, 0.f, 0.f, 1.f},
            {30.f, 20.f, 0.f, 1.f, 0.f, 1.f},
            {10.f, 40.f, 0.f, 0.f, 1.f, 1.f},
        };
        for (const auto& v : vertex)
            for (float value : v) append_float(words, value);
        return words;
    };

    // A draw with the guest window clip disabled (the authored state on every
    // captured route) is still scissored to the destination surface.
    {
        RecordingBackend backend;
        backend.host_width = 320;
        backend.host_height = 240;
        m2host::GuestCltInterpreter interpreter(&backend);
        interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
        interpreter.set_te_surface_state(0x7u, kDestinationClip, 0u, 0x476000u,
                                         576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);
        const std::vector<uint32_t> words = vertex_words();
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        if (!expect(result.ok && result.draws == 1,
                    "destination-clip list draws")) return false;
        if (!expect(!backend.scissors.empty() &&
                        backend.scissors.back().enabled &&
                        backend.scissors.back().x == 16.f &&
                        backend.scissors.back().y == 12.f &&
                        backend.scissors.back().w == 288.f &&
                        backend.scissors.back().h == 216.f,
                    "destination surface bounds a draw with no guest clip"))
            return false;
    }

    // An authored outside clip is a second bound: the host scissor is their
    // intersection, so the window clip cannot re-expose the display border.
    {
        RecordingBackend backend;
        backend.host_width = 320;
        backend.host_height = 240;
        m2host::GuestCltInterpreter interpreter(&backend);
        interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
        interpreter.set_te_surface_state(0x7u, kDestinationClip, 0u, 0x476000u,
                                         576u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u);
        std::vector<uint32_t> words = {
            0x1001801cu, 0x00000280u, 0x000001e0u,  // DBXYWINCLIP 0..640/0..480
            0x10009008u, 0x00000080u,                // outside-only window clip
        };
        const std::vector<uint32_t> geometry = vertex_words();
        words.insert(words.end(), geometry.begin(), geometry.end());
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        if (!expect(result.ok && result.draws == 1,
                    "full-display window clip list draws")) return false;
        if (!expect(backend.scissors.back().enabled &&
                        backend.scissors.back().x == 16.f &&
                        backend.scissors.back().y == 12.f &&
                        backend.scissors.back().w == 288.f &&
                        backend.scissors.back().h == 216.f,
                    "display-sized guest clip intersects the destination"))
            return false;

        // Narrower guest clip: the intersection is the guest rect.
        words = {
            0x1001801cu, 0x00000064u, 0x00000032u,  // DBXYWINCLIP 0..100/0..50
        };
        words.insert(words.end(), geometry.begin(), geometry.end());
        const m2host::CltExecutionResult narrow =
            interpreter.execute(words.data(), words.size());
        if (!expect(narrow.ok && narrow.draws == 1,
                    "narrow window clip list draws")) return false;
        if (!expect(backend.scissors.back().enabled &&
                        backend.scissors.back().x == 16.f &&
                        backend.scissors.back().y == 12.f &&
                        backend.scissors.back().w == 50.f &&
                        backend.scissors.back().h == 25.f,
                    "narrow guest clip wins the intersection"))
            return false;
    }

    // Plausible wrong result: with no bound destination surface the host must
    // not invent a containment rectangle (a full-target draw).
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
        const std::vector<uint32_t> words = vertex_words();
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        if (!expect(result.ok && result.draws == 1,
                    "unbound-destination list draws")) return false;
        return expect(!backend.scissors.empty() &&
                          !backend.scissors.back().enabled,
                      "no destination clip means no invented containment");
    }
}

// DBSRCCNTL.SRCMSBREP pins the host's 16-bpp DB-source expansion.  The two
// documented forms differ only in the low 3 bits, which a 5-bit destination
// discards, so the host's fixed replicate form preserves the authored value
// rather than guessing: a guest-authored 16-bit white (0x1f) must present as
// 0xff, not 0xf8.
bool test_db_source_msb_replicate_expansion()
{
    for (uint32_t v = 0; v <= 0x1fu; ++v) {
        if (!expect(m2host::clt_db_source_5bit_forms_agree(v),
                    "both SRCMSBREP forms quantize to the same 5-bit value"))
            return false;
        if (!expect((m2host::clt_db_source_expand_5_to_8(v, true) >> 3u) == v,
                    "the replicate form round-trips the authored 5-bit value"))
            return false;
        if (!expect((m2host::clt_db_source_expand_5_to_8(v, true) & 0x7u) ==
                        (v >> 2u),
                    "the replicate form fills the low 3 bits from the 3 MSBs"))
            return false;
        if (!expect(m2host::clt_db_source_expand_5_to_8(v, false) == (v << 3u),
                    "the zero form leaves the low 3 bits clear"))
            return false;
    }
    if (!expect(m2host::clt_db_source_expand_5_to_8(0x1fu, true) == 0xffu &&
                    m2host::clt_db_source_expand_5_to_8(0x1fu, false) == 0xf8u &&
                    m2host::clt_db_source_expand_5_to_8(0x0u, true) == 0x0u,
                "authored 16-bit white presents as 0xff only with replication"))
        return false;
    return true;
}

// TXTLDCNTL (0x6404) and TXTPIPCNTL (0x640c) documented field sets are the
// host's fail-closed boundary for the two texture-load control registers.  A
// word carrying bits outside the vendor masks, a reserved load mode, or the
// compression bit armed for a non-texture load mode is not a programmed state
// and must be rejected instead of being dispatched as if it were.
// --- texture channel widening oracle ----------------------------------------
// Independent transcription of the CLT toolkit's TXTLOAD expansion rules
// (the M2 Toolkit command-list documentation, "CLT + 38/39"):
//
//   Literal color-A literal color can be either 555 or 888. Expansion from 555
//   to 888 is done by replication of MSBs.
//   Alpha-...the top three MSBs of the 4-bit source alpha are replicated into
//   the bottom three LSBs of the expanded alpha.
//
// One documented rule covers both widths, and the host widens every stored or
// source channel field with it. The second function below is the toolkit
// *utility* rule used on some load paths (M2TXcompress.c's qmem.h oneExtend:
// the source LSB is repeated into the widened low bits); it is reproduced here
// so this matrix can show the two rules apart and require production to follow
// the documented one, without letting a load path drift onto the other
// silently.
uint32_t doc_widen_msb(unsigned depth, uint32_t value)
{
    if (depth == 0u) return 0u;
    if (depth >= 8u) return value & 0xffu;
    const uint32_t masked = value & ((1u << depth) - 1u);
    uint32_t out = masked;
    for (unsigned filled = depth; filled < 8u;) {
        const unsigned take =
            std::min(8u - filled, depth);
        out = (out << take) | (masked >> (depth - take));
        filled += take;
    }
    return out & 0xffu;
}

uint32_t toolkit_utility_widen(unsigned depth, uint32_t value)
{
    if (depth == 0u) return 0u;
    if (depth >= 8u) return value & 0xffu;
    const uint32_t masked = value & ((1u << depth) - 1u);
    const uint32_t scaled = masked << (8u - depth);
    return scaled | ((masked & 1u) ? (0xffu >> depth) : 0u);
}

// Load a literal A7+SSB TRAM resident through one load mode and return the
// decoded pixels. Mode 0 stages raw rows (32 bits per row) that materialize
// under the following use state; mode 1 (MMDMA) stages raw bytes the same way.
// Both must widen the same stored 7-bit alpha field identically.
std::vector<uint32_t> decode_raw_a7_resident(const std::vector<uint8_t>& bytes)
{
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x10006404u, 0x00000000u,  // raw texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,  // two rows
        0x1000642cu, 0x00200020u,  // 32 raw bits per row
        0x1000643cu, 0x00001a70u,  // literal A7+SSB load format
        0x10000014u, 0x00000001u,  // TLD stages the raw rows
        0x1000642cu, 0x00070001u,  // use-state window
        0x1000640cu, 0x00009200u,
        0x10006414u, 0x00000000u,
        0x1000643cu, 0x00001a70u,  // literal A7+SSB use format
        0x10006408u, 0x00002000u,
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_words = words;
    const float quad_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    for (const auto& vertex : quad_vertices)
        for (float value : vertex) append_float(complete_words, value);
    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes = bytes;
    memory.ranges.push_back(std::move(texels));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!result.ok || result.unsupported != 0 || result.draws != 1 ||
        backend.uploads.size() != 1)
        return {};
    return backend.uploads[0].pixels;
}

std::vector<uint32_t> decode_mmdma_a7_resident(const std::vector<uint8_t>& bytes)
{
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u,
        static_cast<uint32_t>(bytes.size()),  // payload bytes
        0x1000643cu, 0x00001a70u,             // literal A7+SSB format
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010000u,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00004900u,  // color, alpha, and SSB from texture
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_words = words;
    const float quad_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    for (const auto& vertex : quad_vertices)
        for (float value : vertex) append_float(complete_words, value);
    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes = bytes;
    memory.ranges.push_back(std::move(texels));
    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!result.ok || result.unsupported != 0 || result.draws != 1 ||
        backend.uploads.size() != 1)
        return {};
    return backend.uploads[0].pixels;
}

// Differential widening matrix. The unit sweep pins the production widening
// helper against the documented rule across every depth and field value; the
// end-to-end sweep drives the same stored A7 field through both load paths and
// requires the documented widening on each. The toolkit utility rule is
// reported rather than asserted: it is the artifact the older expectations
// were authored from, and the two rules disagree on most odd values.
bool test_texture_channel_widening_matrix()
{
    unsigned production_sites = 0;
    unsigned utility_differs = 0;
    for (unsigned depth = 1; depth <= 8; ++depth) {
        const uint32_t values = 1u << depth;
        for (uint32_t value = 0; value < values; ++value) {
            const uint32_t documented = doc_widen_msb(depth, value);
            const uint32_t production =
                m2host::clt_tex_expand_probe::widen(depth, value);
            const uint32_t utility = toolkit_utility_widen(depth, value);
            if (production != documented) {
                std::printf("m2_guest_clt_test: widening depth=%u value=0x%02x "
                            "production=0x%02x documented=0x%02x\n",
                            depth, value, production, documented);
                return expect(false,
                              "production widening follows the documented "
                              "MSB-replication rule at every depth");
            }
            ++production_sites;
            if (utility != documented) ++utility_differs;
        }
    }
    if (!expect(production_sites == 510u && utility_differs > 0u,
                "widening matrix covers the full 1..8-bit domain with the "
                "two vendor artifacts distinguishable"))
        return false;
    // The documented 4->7 alpha pattern is abcd -> abcdabc, which as the
    // host's 8-bit alpha is 0xaa for a 4-bit 0xa; the toolkit utility would
    // produce 0xa0. A7 stays untouched by either rule when the field is
    // already the documented destination width.
    if (!expect(doc_widen_msb(4u, 0xau) == 0xaau &&
                    toolkit_utility_widen(4u, 0xau) == 0xa0u &&
                    doc_widen_msb(5u, 0x15u) == 0xadu &&
                    toolkit_utility_widen(5u, 0x15u) == 0xafu,
                "documented 555->888 and 4-bit alpha widening patterns"))
        return false;

    // End-to-end, staged raw rows: eight stored A7+SSB texels materialize
    // under the use format, and each widened alpha must match the documented
    // rule for its stored field. Two of the values (bit patterns 0x40 and
    // 0x01) are exactly where the toolkit utility would differ.
    {
        std::vector<uint8_t> bytes;
        for (uint32_t i = 0; i < 8u; ++i) bytes.push_back(
            static_cast<uint8_t>(0x80u | ((i * 19u) & 0x7fu)));
        const std::vector<uint32_t> raw = decode_raw_a7_resident(bytes);
        if (!expect(raw.size() == 8u,
                    "staged raw A7+SSB rows materialize under their use "
                    "format"))
            return false;
        for (size_t i = 0; i < raw.size(); ++i) {
            const uint32_t documented = doc_widen_msb(7u, bytes[i] & 0x7fu);
            if (!expect((raw[i] >> 24u) == documented,
                        "staged raw A7 row widens its stored alpha per the "
                        "documented rule"))
                return false;
        }
    }

    // End-to-end: every stored 7-bit alpha through the lazy MMDMA path. 0x40
    // is the value the documented rule maps to 0x81 and the toolkit utility
    // maps to 0x80; the same field reaching a raw resident must decode to the
    // same byte.
    for (uint32_t alpha = 0; alpha < 128u; ++alpha) {
        const std::vector<uint8_t> bytes = {
            static_cast<uint8_t>(alpha),          // SSB=0, alpha=alpha
            static_cast<uint8_t>(0x80u | alpha),  // SSB=1, alpha=alpha
        };
        const std::vector<uint32_t> mmdma = decode_mmdma_a7_resident(bytes);
        if (!expect(mmdma.size() == 2u,
                    "literal A7 MMDMA resident decodes"))
            return false;
        const uint32_t documented = doc_widen_msb(7u, alpha);
        // The stored SSB bit sits above the alpha field; it must not shift the
        // widened alpha on either texel.
        if (!expect((mmdma[0] >> 24u) == documented &&
                        (mmdma[1] >> 24u) == documented,
                    "MMDMA residents widen a stored 7-bit alpha per the "
                    "documented rule"))
            return false;
    }
    return true;
}

bool test_tex_load_control_gate()
{
    // The three vendor authoring forms: `CLA_TXTLDCNTL` for the uncompressed
    // texture (0x0) and the PIP load (0x200 | offset), the TexLoad
    // LOADMODE/COMPRESSED/SRCBITOFF form (0x400, bits 0..2 add the start
    // offset), and `CLT_SetConst(TXTLDCNTL, LOADMODE, MMDMA)` (0x100).
    if (!expect(m2host::clt_tex_load_control_documented(0x0u) &&
                    m2host::clt_tex_load_control_documented(0x100u) &&
                    m2host::clt_tex_load_control_documented(0x200u) &&
                    m2host::clt_tex_load_control_documented(0x400u) &&
                    m2host::clt_tex_load_control_documented(0x405u),
                "the vendor TXTLDCNTL authoring forms stay documented"))
        return false;
    if (!expect(!m2host::clt_tex_load_control_documented(0x300u) &&
                    !m2host::clt_tex_load_control_documented(0x500u) &&
                    !m2host::clt_tex_load_control_documented(0x600u) &&
                    !m2host::clt_tex_load_control_documented(0x708u),
                "reserved TXTLDCNTL bits and load-mode combinations are not "
                "accepted"))
        return false;

    // The statically authored TXTPIPCNTL words (0x4900 constant/constant/
    // texture, 0x8a00, 0x9200) are inside the documented field set; a selector
    // value the toolkit does not define is not.
    if (!expect(m2host::clt_pip_control_documented(0x4900u) &&
                    m2host::clt_pip_control_documented(0x8a00u) &&
                    m2host::clt_pip_control_documented(0x9200u),
                "the statically authored TXTPIPCNTL words stay documented"))
        return false;
    if (!expect(!m2host::clt_pip_control_documented(0x20000u) &&
                    !m2host::clt_pip_control_documented(0x1c040u) &&
                    !m2host::clt_pip_control_documented(0xc000u),
                "reserved TXTPIPCNTL bits and selector values are not accepted"))
        return false;

    // Integration: the authentic State 3 address-window MMDMA list reaches the
    // backend, and the same list with one reserved control bit is rejected.
    const auto run_load = [](uint32_t ld_cntl, uint32_t pip_cntl) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
            0x10006404u, ld_cntl,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x000002d0u,
            0x10000014u, 0x00000001u,  // TLD
            0x1000642cu, 0x03ff0077u,
            0x10006430u, 0x000703ffu,
            0x10006408u, 0x00002000u,
            0x1000640cu, pip_cntl,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        const float vertices[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {8.f, 0.f, 8.f, 0.f},
            {0.f, 120.f, 0.f, 120.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange source;
        source.guest_address = 0x200u;
        source.bytes.assign(0x2d0u, 0u);
        memory.ranges.push_back(std::move(source));
        return interpreter.execute(words.data(), words.size(), &memory);
    };
    // The documented start-offset field (TXTLDCNTL bits 0..2) stays inside the
    // field set for every one of its eight values, and a nonzero offset is
    // still a loadable word rather than an undocumented form.
    for (uint32_t offset = 0; offset <= 7u; ++offset) {
        if (!expect(m2host::clt_tex_load_control_documented(0x400u | offset),
                    "every documented source bit offset stays in the field set"))
            return false;
    }
    const m2host::CltExecutionResult offset_load =
        run_load(0x100u | 3u, 0x9200u);
    if (!expect(offset_load.ok && offset_load.unsupported == 0 &&
                    offset_load.draws == 1,
                "a documented nonzero source bit offset loads and draws"))
        return false;
    const m2host::CltExecutionResult baseline = run_load(0x100u, 0x9200u);
    if (!expect(baseline.ok && baseline.unsupported == 0 &&
                    baseline.draws == 1,
                "the baseline MMDMA control words still reach one draw"))
        return false;
    const m2host::CltExecutionResult bad_load = run_load(0x708u, 0x9200u);
    if (!expect(bad_load.draws == 0 && bad_load.unsupported > 0,
                "a TXTLDCNTL word outside the documented field set is "
                "rejected instead of loaded"))
        return false;
    const m2host::CltExecutionResult bad_pip = run_load(0x100u, 0x20000u);
    if (!expect(bad_pip.draws == 0 && bad_pip.unsupported > 0,
                "a TXTPIPCNTL word outside the documented field set is "
                "rejected instead of materialized"))
        return false;
    return true;
}

bool test_pip_load_nonzero_base()
{
    // CLT_CreatePipCommandList writes TEPIPRAM + 4*pipIndex. Seed entry zero,
    // then publish entries one and two at 0x46004 and decode all three indices
    // through a production indexed MMDMA draw. This catches both rejection of
    // the nonzero destination and an implementation that writes the payload
    // back at m_pip[0].
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    std::vector<uint32_t> words = {
        // Initial four-entry palette at PIP[0].
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000010u,
        0x10000014u, 0x00000001u,
        // Replace only PIP[1] and PIP[2].
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000180u,
        0x10006414u, 0x00046004u,
        0x10006428u, 0x00000008u,
        0x10000014u, 0x00000001u,
        // Three 4-bit indices 0,1,2 in one MMDMA resident.
        0x1000643cu, 0x00000404u,
        0x10006404u, 0x00000100u,
        0x10006424u, 0x000002c0u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020002u,  // 3x1 use window
        0x10006430u, 0x00020000u,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,  // select PIP color/alpha/SSB
        0x10006410u, 0x00009200u,
        0x1000643cu, 0x00000404u,  // complete indexed use state
        0x200a0002u,
    };
    const float vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {3.f, 0.f, 3.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(words, value);

    m2host::CltMemorySnapshot memory;
    const auto append_be_words = [](m2host::CltMemoryRange& range,
                                    const uint32_t* values, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            range.bytes.push_back(static_cast<uint8_t>(values[i] >> 24u));
            range.bytes.push_back(static_cast<uint8_t>(values[i] >> 16u));
            range.bytes.push_back(static_cast<uint8_t>(values[i] >> 8u));
            range.bytes.push_back(static_cast<uint8_t>(values[i]));
        }
    };
    const uint32_t initial_pip[4] = {
        0x00112233u, 0x00445566u, 0x00778899u, 0x00aabbccu,
    };
    m2host::CltMemoryRange initial_range;
    initial_range.guest_address = 0x100u;
    append_be_words(initial_range, initial_pip, 4);
    memory.ranges.push_back(std::move(initial_range));
    const uint32_t replacement_pip[2] = {0x00de1020u, 0x00ef5060u};
    m2host::CltMemoryRange replacement_range;
    replacement_range.guest_address = 0x180u;
    append_be_words(replacement_range, replacement_pip, 2);
    memory.ranges.push_back(std::move(replacement_range));
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x2c0u;
    texels.bytes = {0x01u, 0x20u};  // 0000, 0001, 0010
    memory.ranges.push_back(std::move(texels));

    const m2host::CltExecutionResult result = interpreter.execute(
        words.data(), words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1u,
                "nonzero PIP base loads and reaches the indexed draw"))
        return false;
    if (!expect(backend.uploads[0].w == 3 && backend.uploads[0].h == 1,
                "nonzero PIP publication preserves the indexed texture window"))
        return false;
    if (!expect(backend.uploads[0].pixels[0] == 0x00332211u &&
                    backend.uploads[0].pixels[1] == 0x002010deu &&
                    backend.uploads[0].pixels[2] == 0x006050efu,
                "nonzero PIP base updates only its addressed entries"))
        return false;

    // Keep the newly accepted address range bounded: a full 256-word load
    // starting one word into PIP RAM would cross the hardware window.
    const auto run_pip_only = [](uint32_t base, uint32_t count) {
        RecordingBackend overflow_backend;
        m2host::GuestCltInterpreter overflow_interpreter(&overflow_backend);
        const std::vector<uint32_t> overflow_words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, base,
            0x10006428u, count,
            0x10000014u, 0x00000001u,
        };
        m2host::CltMemorySnapshot overflow_memory;
        m2host::CltMemoryRange overflow_source;
        overflow_source.guest_address = 0x100u;
        overflow_source.bytes.assign(count, 0u);
        overflow_memory.ranges.push_back(std::move(overflow_source));
        return overflow_interpreter.execute(overflow_words.data(),
                                            overflow_words.size(),
                                            &overflow_memory);
    };
    const m2host::CltExecutionResult overflow = run_pip_only(0x46004u, 0x400u);
    if (!expect(overflow.unsupported > 0 && overflow.draws == 0,
                "a PIP load crossing the RAM window stays fail-closed"))
        return false;
    return expect(run_pip_only(0x46001u, 4u).unsupported > 0,
                  "an unaligned PIP base stays fail-closed");
}

bool test_mmdma_pip_order_deferral()
{
    // Reduced from the authentic showroom exec=1210 list: the MMDMA TLD is
    // followed by a PIP TLD, then TXTEXPTYPE/TXTPIPCNTL use-state writes.
    // The source list writes TXTEXPTYPE while TXTPIPCNTL still contains the
    // prior 0x4900 state, and the PIP TLD temporarily uses TXTLODBASE0=0x46000.
    // Neither snapshot is a drawable state for the pending MMDMA blob.
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const std::vector<uint32_t> words = {
        0x1000643cu, 0x00000404u,  // prior indexed use-state
        0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
        0x10006430u, 0x00010000u,  // 2x1 address period
        0x1000640cu, 0x00004900u,  // stale invalid indexed color selector

        0x10006424u, 0x00000200u,  // MMDMA source
        0x10006428u, 0x00000001u,  // one byte = two 4-bit texels
        0x10006414u, 0x00000000u,  // MMDMA TRAM base
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10000014u, 0x00000001u,  // TLD

        0x10006424u, 0x00000100u,  // PIP source
        0x10006428u, 0x00000008u,  // two PIP entries
        0x10006414u, 0x00046000u,  // PIP temporarily owns LODBASE0
        0x10006404u, 0x00000200u,  // PIP load
        0x10000014u, 0x00000001u,  // TLD

        0x1000642cu, 0x00010000u,  // MMDMA use-state UMAX/VMAX
        0x10006430u, 0x00010000u,  // MMDMA use-state UVMASK
        0x10006408u, 0x00002000u,  // texture enable
        0x10006414u, 0x00000000u,  // MMDMA draw base
        0x1000643cu, 0x00000404u,  // use-state format; PIP selector stale
        0x1000640cu, 0x00009200u,  // valid PIP color/alpha/SSB selectors
        0x10006410u, 0x00009200u,
        0x200a0002u,                // new, texture-only, three-vertex strip
    };
    const float vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    std::vector<uint32_t> complete_words = words;
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(complete_words, value);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip;
    pip.guest_address = 0x100u;
    pip.bytes = {
        0xff, 0x11, 0x22, 0x33,
        0x7f, 0x44, 0x55, 0x66,
    };
    memory.ranges.push_back(std::move(pip));
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes = {0x01u};
    memory.ranges.push_back(std::move(texels));

    const m2host::CltExecutionResult result = interpreter.execute(
        complete_words.data(), complete_words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1,
                "MMDMA use-state defers across stale PIP setup"))
        return false;
    return expect(backend.uploads[0].w == 2 && backend.uploads[0].h == 1,
                  "deferred showroom-order MMDMA publishes its 2x1 window");
}

bool test_mmdma_no_color_and_rgb888_forms()
{
    // The lift statically authors three MMDMA use-state expansion words the
    // earlier indexed/RGB555/8+7+SSB families do not cover (writes to
    // register 0x643c in the generated code):
    //   0x1200 literal SSB-only 1-bit  - dosquare1trans/dosquare2trans
    //   0x1a70 literal A7+SSB 8-bit    - initskids/UI_DrawHighScores_nameEntry
    //   0x1e78 literal RGB888+A7+SSB   - M_SetupPreLitFogTrans/M_SetupDynLitFogTrans
    // The toolkit's texel table lists Color 0/SSB 1, Color 0/A7/SSB 1, and
    // Cdepth 8 formats as supported; every channel still flows through
    // TXTPIPCNTL selection, and a TRAM word without an index has no PIP
    // entry, so a PIP selection stays fail-closed.
    const float quad_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };

    // 0x1200 SSB-only, drawn with TXTPIPCNTL=0x4900 (every channel from the
    // texture side): TRAM SSB becomes the SSB output, color reads as black,
    // alpha as zero.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,  // one byte = two 1-bit texels
            0x1000643cu, 0x00001200u,  // literal SSB-only expanded format
            0x10000014u, 0x00000001u,  // TLD
            0x1000642cu, 0x00010000u,  // UMAX=1 -> 2x1
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes = {0x80u};  // texel0 SSB=1, texel1 SSB=0
        memory.ranges.push_back(std::move(texels));
        const m2host::CltExecutionResult r = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(r.ok && r.unsupported == 0 && r.draws == 1 &&
                        backend.uploads.size() == 1,
                    "literal SSB-only MMDMA form reaches backend"))
            return false;
        if (!expect(backend.uploads[0].w == 2 &&
                        backend.uploads[0].h == 1 &&
                        backend.uploads[0].pixels[0] == 0x00000000u &&
                        backend.uploads[0].pixels[1] == 0x00000000u,
                    "SSB-only texels carry no color or alpha from the texture"))
            return false;
    }

    // The square-transition form uses that SSB-only texture as a discard
    // mask while its source-copy destination equation reads colour from the
    // armed framebuffer. Pin the two coordinate domains independently: the
    // mask spans 0..1 over the tile, while DB-source u follows destination
    // pixels over the 576-wide framebuffer. Routing the black SSB texture as
    // colour (the previous implementation) fails both the cel and UV checks.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        constexpr uint32_t db_base = 0x1000u;
        std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x1000643cu, 0x00001200u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006430u, 0x00010000u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00009200u,
            0x1000800cu, 0x00000004u,
            0x10008008u, 0x0000006fu,
            0x10038064u, 0x00000020u, 0x00ffffffu, 0x00ffffffu,
            0x000000d0u,
            0x10049030u, 0x00000002u, db_base, 0x00000240u,
            0x00000000u, 0x00000000u,
            0x200a0002u,
        };
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(words, value);

        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange mask;
        mask.guest_address = 0x200u;
        mask.bytes = {0x80u};
        memory.ranges.push_back(std::move(mask));
        m2host::CltMemoryRange source;
        source.guest_address = db_base;
        source.bytes.assign(static_cast<size_t>(576u * 432u * 2u), 0u);
        for (size_t i = 0; i + 1 < source.bytes.size(); i += 2) {
            source.bytes[i] = 0x7cu;
            source.bytes[i + 1] = 0x00u;
        }
        source.content_hash = snapshot_hash_for_test(source.bytes);
        memory.ranges.push_back(std::move(source));

        const auto result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0u &&
                        result.draws == 1u && backend.draws.size() == 1u &&
                        backend.uploads.size() == 3u,
                    "square transition resolves mask and DB source"))
            return false;
        const RecordingBackend::Draw& draw = backend.draws.front();
        if (!expect(draw.cel == 2u && draw.discard.ssb_mask == 3u &&
                        draw.discard.ssb_zero &&
                        draw.discard.separate_ssb_uv &&
                        draw.blend.mode == m2host::CltBlendMode::source_copy,
                    "square transition copies DB colour through its SSB mask"))
            return false;
        if (!expect(
                std::abs(draw.vertices[1].u - (2.f / 576.f)) < 0.0001f &&
                    std::abs(draw.vertices[1].mask_u - 1.f) < 0.0001f,
                "square transition keeps framebuffer and mask UVs independent"))
            return false;
    }

    // 0x1a70 A7+SSB with the statically authored all-texture selectors:
    // the 7-bit alpha comes from the TRAM word while its absent color stays
    // zero.  The literal branch still rejects PIP selectors because there is
    // no index field from which to select an entry.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,  // two 8-bit texels
            0x1000643cu, 0x00001a70u,  // literal A7+SSB expanded format
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00004900u,  // color, alpha, and SSB from texture
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        // texel0: SSB=0 alpha=0x7f; texel1: SSB=1 alpha=0
        texels.bytes = {0x7fu, 0x80u};
        memory.ranges.push_back(std::move(texels));
        const m2host::CltExecutionResult r = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(r.ok && r.unsupported == 0 && r.draws == 1 &&
                        backend.uploads.size() == 1,
                    "literal A7+SSB MMDMA form reaches backend"))
            return false;
        if (!expect(backend.uploads[0].pixels[0] == 0xff000000u &&
                        backend.uploads[0].pixels[1] == 0x00000000u,
                    "A7+SSB texels expand alpha with absent color zero"))
            return false;
    }

    // 0x1e78 literal RGB888+A7+SSB (the M_Setup* fog-trans word): 32-bit
    // texels in the SSB -> alpha -> RGB888 order.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000008u,  // two 32-bit texels
            0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00004900u,  // color, alpha, and SSB from texture
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        // texel0: SSB=1 A=0x7f R=0x12 G=0x34 B=0x56; texel1: SSB=0 A=0x40
        texels.bytes = {0xffu, 0x12u, 0x34u, 0x56u, 0x40u, 0xa0u, 0xb0u, 0xc0u};
        memory.ranges.push_back(std::move(texels));
        const m2host::CltExecutionResult r = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(r.ok && r.unsupported == 0 && r.draws == 1 &&
                        backend.uploads.size() == 1,
                    "literal RGB888+A7+SSB MMDMA form reaches backend"))
            return false;
        // texel1 carries A7=0x40, the value the documented MSB-replication rule
    // maps to 0x81; the toolkit utility's LSB fill would give 0x80. The
    // expectation is derived from the documented rule rather than transcribed.
    const uint32_t a7_0x40 = doc_widen_msb(7u, 0x40u);
    if (!expect(a7_0x40 == 0x81u,
                "documented 7-bit alpha widening pattern (0x40 -> 0x81)"))
        return false;
    if (!expect(backend.uploads[0].pixels[0] == 0xff563412u &&
                        backend.uploads[0].pixels[1] ==
                            ((a7_0x40 << 24u) | 0x00c0b0a0u),
                    "RGB888+A7+SSB texels decode MSB-first in channel order"))
            return false;
    }

    // Selector fallbacks are format-shaped, not permissive defaults. An
    // indexed texel has no literal RGB for TEXTURE color, and a literal/no-
    // color texel has no index with which to address PIP. Both combinations
    // are explicitly forbidden by the toolkit and must reach the draw-time
    // fail-closed boundary rather than manufacture a pixel.
    const auto rejected_selector = [&](uint32_t exp_type,
                                       uint32_t pip_control,
                                       uint8_t source_byte) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x1000643cu, exp_type,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006408u, 0x00002000u,
            0x1000640cu, pip_control,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes = {source_byte};
        memory.ranges.push_back(std::move(texels));
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        return result.ok && result.unsupported == 1 && result.draws == 0 &&
               backend.uploads.empty();
    };
    if (!expect(rejected_selector(0x0404u, 0x4900u, 0x10u),
                "indexed TEXTURE-color selector stays fail-closed"))
        return false;
    if (!expect(rejected_selector(0x1200u, 0x9200u, 0x80u),
                "literal PIP selectors stay fail-closed without an index"))
        return false;

    // The fail-closed boundary holds: a reserved literal alpha depth
    // (Adepth 5, outside the toolkit's 4/7 set) must not materialize.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000100u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x1000643cu, 0x00001a50u,  // literal A5+SSB: reserved Adepth
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texels;
        texels.guest_address = 0x200u;
        texels.bytes = {0x00u};
        memory.ranges.push_back(std::move(texels));
        const m2host::CltExecutionResult r = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        return expect(r.ok && r.unsupported != 0 && backend.uploads.empty(),
                      "reserved literal Adepth stays fail-closed");
    }
}

bool test_compressed_dci_contract()
{
    // The Portfolio/Mercury source decoder treats TXTCNST's blue byte as the
    // PIP index only for a transparent control.  An ordinary indexed source
    // value selects PIPData[index], with TXTPIPCNTL providing the separate
    // 8-bit offset. Keep a nonzero constant blue byte here so an accidental
    // TXTCONST index addition cannot pass this fixture.
    RecordingBackend indexed_backend;
    m2host::GuestCltInterpreter indexed_interpreter(&indexed_backend);
    const std::vector<uint32_t> indexed_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000048u,  // 18 PIP entries; PIP[0x11] is a sentinel
        0x10000014u, 0x00000001u,
        0x10006434u, 0x04010401u,  // 1-bit indexed DCI sources
        0x10006438u, 0x04010401u,
        0x1000643cu, 0x00000404u,  // 4-bit indexed expanded destination
        0x1000640cu, 0x00009210u,  // PIP selectors, index offset 0x10
        0x10006440u, 0x00000001u,  // nontransparent constant blue, not offset
        0x10006404u, 0x00000400u,  // compressed texture load
        0x10006414u, 0x00000000u,
        0x10006424u, 0x00000200u,
        0x10006428u, 0x00000001u,
        0x1000642cu, 0x00040004u,  // one 4-bit destination texel
        0x10000014u, 0x00000001u,
    };
    m2host::CltMemorySnapshot indexed_memory;
    m2host::CltMemoryRange indexed_pip;
    indexed_pip.guest_address = 0x100u;
    indexed_pip.bytes.resize(18u * 4u, 0u);
    const auto set_indexed_pip_word = [&](size_t index, uint32_t word) {
        indexed_pip.bytes[index * 4u] = static_cast<uint8_t>(word >> 24u);
        indexed_pip.bytes[index * 4u + 1u] =
            static_cast<uint8_t>(word >> 16u);
        indexed_pip.bytes[index * 4u + 2u] = static_cast<uint8_t>(word >> 8u);
        indexed_pip.bytes[index * 4u + 3u] = static_cast<uint8_t>(word);
    };
    set_indexed_pip_word(0u, 0xff102030u);
    set_indexed_pip_word(1u, 0x7f405060u);
    set_indexed_pip_word(0x11u, 0x7faabbccu);
    indexed_memory.ranges.push_back(std::move(indexed_pip));
    m2host::CltMemoryRange indexed_source;
    indexed_source.guest_address = 0x200u;
    indexed_source.bytes = {0x00u, 0x80u};  // control 0, one-bit index 1
    indexed_memory.ranges.push_back(std::move(indexed_source));
    const m2host::CltExecutionResult indexed_result = indexed_interpreter.execute(
        indexed_words.data(), indexed_words.size(), &indexed_memory);
    // The 1-bit source index is 1 and the destination is only 4 bits, but
    // TXTPIPCNTL adds the independent 0x10 segment offset at lookup time:
    // 1 + 0x10 selects PIP[0x11], not PIP[0x01].
    if (!expect(indexed_result.ok && indexed_result.unsupported == 0 &&
                    indexed_backend.uploads.size() == 1,
                "indexed DCI source uses its direct PIP index"))
        return false;
    if (!expect(indexed_backend.uploads[0].w == 1 &&
                    indexed_backend.uploads[0].h == 1 &&
                    indexed_backend.uploads[0].pixels[0] == 0xffccbbaau,
                "indexed DCI source preserves the selected PIP color"))
        return false;
    if (!expect((indexed_backend.uploads[0].pixels[0] & 0x00ffffffu) !=
                    0x00605040u,
                "PIP index offset avoids the unoffset palette entry"))
        return false;

    // A non-transparent source may omit color entirely.  The SDK then uses
    // that source's constant RGB instead of consuming a zero-width index.
    RecordingBackend constant_backend;
    m2host::GuestCltInterpreter constant_interpreter(&constant_backend);
    const std::vector<uint32_t> constant_words = {
        0x10006434u, 0x00000000u,  // no-color DCI sources
        0x10006438u, 0x00000000u,
        0x1000643cu, 0x00000404u,  // 4-bit indexed expanded destination
        0x10006440u, 0xff112233u,  // source constant RGB
        0x10006404u, 0x00000400u,  // compressed texture load
        0x10006414u, 0x00000000u,
        0x10006424u, 0x00000400u,
        0x10006428u, 0x00000001u,
        0x1000642cu, 0x00040004u,  // one 4-bit destination texel
        0x10000014u, 0x00000001u,
    };
    m2host::CltMemorySnapshot constant_memory;
    m2host::CltMemoryRange constant_source;
    constant_source.guest_address = 0x400u;
    constant_source.bytes = {0x00u};  // control 0, one constant texel
    constant_memory.ranges.push_back(std::move(constant_source));
    const m2host::CltExecutionResult constant_result =
        constant_interpreter.execute(constant_words.data(),
                                      constant_words.size(),
                                      &constant_memory);
    if (!expect(constant_result.ok && constant_result.unsupported == 0 &&
                    constant_backend.uploads.size() == 1,
                "no-color DCI source reaches the backend from its constant"))
        return false;
    if (!expect(constant_backend.uploads[0].pixels[0] == 0xff332211u,
                "no-color DCI source preserves constant RGB"))
        return false;

    // Source DCI fields are consumed MSB-first as SSB, alpha, then color.
    // This literal fixture also exercises the target size rule
    // (3*CDEPTH + ADEPTH + SSB) and oneExtend() for odd 2-/4-bit values.
    RecordingBackend literal_backend;
    m2host::GuestCltInterpreter literal_interpreter(&literal_backend);
    const std::vector<uint32_t> literal_words = {
        0x10006434u, 0x1e241e24u,  // literal RGB4 + A2 + SSB sources
        0x10006438u, 0x1e241e24u,
        0x1000643cu, 0x00001e74u,  // literal RGB4 + A7 + SSB destination
        0x10006404u, 0x00000400u,  // compressed texture load
        0x10006414u, 0x00000000u,
        0x10006424u, 0x00000300u,
        0x10006428u, 0x00000001u,
        0x1000642cu, 0x00140014u,  // one 20-bit destination texel
        0x10000014u, 0x00000001u,
    };
    m2host::CltMemorySnapshot literal_memory;
    m2host::CltMemoryRange literal_source;
    literal_source.guest_address = 0x300u;
    // Control byte 0, then SSB=1, alpha=3, R=0xa, G=5, B=0xf.
    literal_source.bytes = {0x00u, 0xf4u, 0xbeu};
    literal_memory.ranges.push_back(std::move(literal_source));
    const m2host::CltExecutionResult literal_result = literal_interpreter.execute(
        literal_words.data(), literal_words.size(), &literal_memory);
    if (!expect(literal_result.ok && literal_result.unsupported == 0 &&
                    literal_backend.uploads.size() == 1,
                "literal alpha and SSB DCI source reaches the backend"))
        return false;
    // Source SSB=1, A2=3, R=0xa, G=5, B=0xf. The expanded destination is a
    // 4-bit-per-channel literal, and the host widens each stored 4-bit field
    // with the documented MSB-replication rule (the toolkit utility's LSB fill
    // would give 0xa0/0x5f). Derived here from that rule, not transcribed.
    const uint32_t literal_rgb =
        doc_widen_msb(4u, 0xau) | (doc_widen_msb(4u, 0x5u) << 8u) |
        (doc_widen_msb(4u, 0xfu) << 16u);
    if (!expect((literal_rgb & 0xffffffu) == 0x00ff55aau,
                "documented 4-bit literal color widening pattern"))
        return false;
    if (!expect(literal_backend.uploads[0].pixels[0] ==
                    (0xff000000u | literal_rgb),
                "literal DCI fields use the SDK bit order and the documented "
                "channel widening"))
        return false;

    // clttxload.c includes ADEPTH in the expanded row size independently of
    // the HASALPHA bit.  The decoder still consumes source alpha fields, but
    // a target with no published alpha channel retains those bits in its
    // guest-visible destination layout.
    RecordingBackend depth_only_backend;
    m2host::GuestCltInterpreter depth_only_interpreter(&depth_only_backend);
    std::vector<uint32_t> depth_only_words = literal_words;
    depth_only_words[5] = 0x00001674u;  // literal RGB4 + A7 + SSB, no alpha
    const m2host::CltExecutionResult depth_only_result =
        depth_only_interpreter.execute(depth_only_words.data(),
                                       depth_only_words.size(),
                                       &literal_memory);
    if (!expect(depth_only_result.ok && depth_only_result.unsupported == 0 &&
                    depth_only_backend.uploads.size() == 1,
                "expanded DCI sizing retains ADEPTH without HASALPHA"))
        return false;
    return expect((depth_only_backend.uploads[0].pixels[0] & 0x00ffffffu) ==
                      (literal_rgb & 0x00ffffffu),
                  "expanded DCI no-alpha target preserves literal RGB");
}

bool test_content_format_completion()
{
    // Data-format completeness beyond the exercised corpus.
    // The toolkit's TXTEXPTYPE table makes every 1..8-bit indexed depth a
    // legal TRAM format, so 2/3/5/7 must not stay fail-closed; a full
    // (uncompressed) load may carry literal 5/8-bit color, 4/7-bit alpha, or
    // an index stream with independent source/destination bit strides. These
    // fixtures pin the newly typed combinations and the reserved encodings
    // that remain fail-closed.
    const uint32_t pip_words[8] = {
        0xff112233u, 0x3f445566u, 0x1f778899u, 0x0faabbccu,
        0x80556677u, 0x40667788u, 0x20678899u, 0x106899aau,
    };
    std::vector<uint8_t> pip_bytes;
    for (uint32_t word : pip_words) {
        pip_bytes.push_back(static_cast<uint8_t>(word >> 24u));
        pip_bytes.push_back(static_cast<uint8_t>(word >> 16u));
        pip_bytes.push_back(static_cast<uint8_t>(word >> 8u));
        pip_bytes.push_back(static_cast<uint8_t>(word));
    }
    const float quad_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };

    // The toolkit defines SRCBITOFF for uncompressed mode-0 loads as the
    // starting bit within the source byte. A legal nonzero offset must change
    // the decoded texel sequence; otherwise a loader that silently ignores
    // the field would pass the ordinary zero-offset fixture.
    const auto run_uncompressed_bit_offset =
        [&](uint32_t source_bit_offset) {
            RecordingBackend backend;
            m2host::GuestCltInterpreter interpreter(&backend);
            const std::vector<uint32_t> words = {
                0x10006404u, 0x00000200u,  // PIP load
                0x10006424u, 0x00000100u,
                0x10006414u, 0x00046000u,
                0x10006428u, 0x00000020u,  // eight PIP entries
                0x10000014u, 0x00000001u,
                0x1000643cu, 0x00000401u,  // 1-bit indexed format
                0x10006404u, source_bit_offset,  // mode-0, uncompressed
                0x10006424u, 0x00000200u,
                0x10006414u, 0x00000000u,
                0x10006428u, 0x00000001u,  // one row
                0x1000642cu, 0x00040008u,  // four destination bits, eight source
                0x10000014u, 0x00000001u,
                0x1000642cu, 0x00030000u,  // 4x1 use window
                0x10006430u, 0x00030000u,
                0x10006408u, 0x00002000u,
                0x1000640cu, 0x00009200u,
                0x10006410u, 0x00009200u,
                0x200a0002u,
            };
            std::vector<uint32_t> complete_words = words;
            for (const auto& vertex : quad_vertices)
                for (float value : vertex) append_float(complete_words, value);

            m2host::CltMemorySnapshot memory;
            m2host::CltMemoryRange pip_range;
            pip_range.guest_address = 0x100u;
            for (const uint32_t pip_word : {0x00112233u, 0x7f445566u,
                                             0xffaabbccu, 0x3f778899u,
                                             0x00101010u, 0x7f202020u,
                                             0xff303030u, 0x3f404040u}) {
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 24u));
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 16u));
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 8u));
                pip_range.bytes.push_back(static_cast<uint8_t>(pip_word));
            }
            memory.ranges.push_back(std::move(pip_range));
            m2host::CltMemoryRange source_range;
            source_range.guest_address = 0x200u;
            source_range.bytes = {0x60u, 0x00u};  // 0110..., then padding
            memory.ranges.push_back(std::move(source_range));
            const m2host::CltExecutionResult result = interpreter.execute(
                complete_words.data(), complete_words.size(), &memory);
            if (!result.ok || result.unsupported != 0 || result.draws != 1 ||
                backend.uploads.size() < 2u ||
                backend.uploads.back().pixels.size() != 4u)
                return std::vector<uint32_t>{};
            return backend.uploads.back().pixels;
        };

    const std::vector<uint32_t> source_aligned_pixels =
        run_uncompressed_bit_offset(0u);
    if (!expect(source_aligned_pixels.size() == 4u &&
                    source_aligned_pixels[0] == 0x00332211u &&
                    source_aligned_pixels[1] == 0xff665544u &&
                    source_aligned_pixels[2] == 0xff665544u &&
                    source_aligned_pixels[3] == 0x00332211u,
                "uncompressed source bit offset zero decodes source-aligned indices"))
        return false;
    const std::vector<uint32_t> shifted_pixels =
        run_uncompressed_bit_offset(1u);
    if (!expect(shifted_pixels.size() == 4u &&
                    shifted_pixels[0] == 0xff665544u &&
                    shifted_pixels[1] == 0xff665544u &&
                    shifted_pixels[2] == 0x00332211u &&
                    shifted_pixels[3] == 0x00332211u,
                "uncompressed source bit offset shifts the first texel"))
        return false;
    if (!expect(source_aligned_pixels != shifted_pixels,
                "uncompressed source bit offset is not ignored"))
        return false;

    // Exercise the seven documented indexed rows that are not represented by
    // the six direct guest TXTEXPTYPE writes. The vendor M2TX decoder reads
    // each texel MSB-first as SSB -> alpha -> index; no-color rows omit the
    // index and obtain RGB from the SSB-selected PIP constant.
    const auto run_indexed_mmdma_format =
        [&](uint32_t exp_type, unsigned color_depth, unsigned alpha_depth,
            bool has_ssb, uint32_t pip_control,
            const std::vector<std::array<uint32_t, 3>>& samples,
            uint32_t expected0, uint32_t expected1, const char* label) {
            std::vector<uint8_t> texel_bytes(
                (samples.size() *
                     (color_depth + alpha_depth + (has_ssb ? 1u : 0u)) +
                 7u) /
                    8u,
                0u);
            size_t bit_offset = 0;
            const auto append_bits = [&](uint32_t value, unsigned count) {
                for (unsigned bit = count; bit > 0; --bit) {
                    if (((value >> (bit - 1u)) & 1u) != 0u)
                        texel_bytes[bit_offset / 8u] |=
                            static_cast<uint8_t>(
                                1u << (7u - (bit_offset % 8u)));
                    ++bit_offset;
                }
            };
            for (const auto& sample : samples) {
                // sample = {SSB, encoded alpha, encoded index}.
                if (has_ssb) append_bits(sample[0], 1u);
                if (alpha_depth != 0u) append_bits(sample[1], alpha_depth);
                if (color_depth != 0u) append_bits(sample[2], color_depth);
            }

            RecordingBackend backend;
            m2host::GuestCltInterpreter interpreter(&backend);
            std::vector<uint32_t> words = {
                0x10006404u, 0x00000200u,  // PIP load
                0x10006424u, 0x00000100u,
                0x10006414u, 0x00046000u,
                0x10006428u, 0x00000010u,  // four PIP entries
                0x10000014u, 0x00000001u,
                0x10006440u, 0x00112233u,  // PIP constant 0
                0x1000643cu, exp_type,     // MMDMA expansion format
                0x10006404u, 0x00000100u, // uncompressed MMDMA load
                0x10006424u, 0x00000200u,
                0x10006414u, 0x00000000u,
                0x10006428u,
                static_cast<uint32_t>(texel_bytes.size()),
                0x10000014u, 0x00000001u,
                0x1000642cu, 0x00010000u, // UMAX=1 -> 2x1
                0x10006430u, 0x00010000u,
                0x10006408u, 0x00002000u,
                0x1000640cu, pip_control,
                0x10006410u, 0x00009200u,
                0x1000643cu, exp_type,     // use-state materialization
                0x200a0002u,
            };
            for (const auto& vertex : quad_vertices)
                for (float value : vertex) append_float(words, value);

            m2host::CltMemorySnapshot memory;
            m2host::CltMemoryRange pip_range;
            pip_range.guest_address = 0x100u;
            for (const uint32_t pip_word : {0x00112233u, 0x7f112233u,
                                             0xffaabbccu, 0x3f445566u}) {
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 24u));
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 16u));
                pip_range.bytes.push_back(
                    static_cast<uint8_t>(pip_word >> 8u));
                pip_range.bytes.push_back(static_cast<uint8_t>(pip_word));
            }
            memory.ranges.push_back(std::move(pip_range));
            m2host::CltMemoryRange texel_range;
            texel_range.guest_address = 0x200u;
            texel_range.bytes = std::move(texel_bytes);
            memory.ranges.push_back(std::move(texel_range));
            const m2host::CltExecutionResult result = interpreter.execute(
                words.data(), words.size(), &memory);
            return expect(result.ok && result.unsupported == 0 &&
                              result.draws == 1 && backend.uploads.size() == 1 &&
                              backend.uploads[0].pixels.size() == 2u &&
                              backend.uploads[0].pixels[0] == expected0 &&
                              backend.uploads[0].pixels[1] == expected1,
                          label);
        };

    // Each row's pixel alpha is its stored 4-bit source alpha widened by the
    // documented MSB-replication rule (0xa -> 0xaa, 0x1 -> 0x11). Deriving
    // the expectation from that rule keeps the row pins honest: the previous
    // literals (0xa0/0x1f) were transcribed from the toolkit utility's LSB
    // fill, which is not what the toolkit's own TXTLOAD expansion section
    // specifies.
    const auto alpha4 = [](uint32_t stored) {
        return doc_widen_msb(4u, stored) << 24u;
    };

    if (!run_indexed_mmdma_format(
            0x00000840u, 0u, 4u, false, 0x00004800u,
            {{{0u, 0xau, 0u}, {0u, 0x1u, 0u}}},
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00332211u,
            "indexed colorless A4 MMDMA row reaches backend"))
        return false;
    const std::vector<std::array<uint32_t, 3>> indexed_alpha_samples = {
        {{0u, 0xau, 0x1u}},
        {{1u, 0x1u, 0x2u}},
    };
    if (!run_indexed_mmdma_format(
            0x00000e41u, 1u, 4u, true, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00332211u,
            "indexed 1-bit+A4+SSB MMDMA row reaches backend"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000e41u, 1u, 4u, true, 0x00008200u,
            indexed_alpha_samples, 0x00332211u, 0x00332211u,
            "PIP alpha selector negative control changes alpha source"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000c42u, 2u, 4u, false, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00ccbbaau,
            "indexed 2-bit+A4 MMDMA row reaches backend"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000e43u, 3u, 4u, true, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00ccbbaau,
            "indexed 3-bit+A4+SSB MMDMA row reaches backend"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000c44u, 4u, 4u, false, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00ccbbaau,
            "indexed 4-bit+A4 MMDMA row reaches backend"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000e47u, 7u, 4u, true, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00ccbbaau,
            "indexed 7-bit+A4+SSB MMDMA row reaches backend"))
        return false;
    if (!run_indexed_mmdma_format(
            0x00000c48u, 8u, 4u, false, 0x00008a00u,
            indexed_alpha_samples,
            alpha4(0xau) | 0x00332211u, alpha4(0x1u) | 0x00ccbbaau,
            "indexed 8-bit+A4 MMDMA row reaches backend"))
        return false;

    // 2-bit indexed MMDMA use-state (0x402). The vendor table lists IDEPTH
    // 1..8; four 2-bit indices decode MSB-first into four PIP entries.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,  // 8 PIP entries
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000402u,  // 2-bit indexed expansion
            0x10006404u, 0x00000100u,  // MMDMA load, mode 1
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,  // 1 byte = four 2-bit texels
            0x10000014u, 0x00000001u,  // TLD stages the raw bytes
            0x1000642cu, 0x00030000u,  // UMAX=3 -> 4x1
            0x10006430u, 0x00030000u,  // UVMASK U=3
            0x1000640cu, 0x00009200u,  // PIP selectors for materialization
            0x1000643cu, 0x00000402u,  // use-state write -> materialize
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x1bu};  // 00 01 10 11
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.uploads.size() == 1,
                    "2-bit indexed MMDMA use-state reaches backend"))
            return false;
        if (!expect(backend.uploads[0].w == 4 && backend.uploads[0].h == 1 &&
                        backend.uploads[0].pixels[0] == 0xff332211u &&
                        backend.uploads[0].pixels[1] == 0x7f665544u &&
                        backend.uploads[0].pixels[2] == 0x3f998877u &&
                        backend.uploads[0].pixels[3] == 0x1fccbbaau,
                    "2-bit indices are MSB-first and PIP-expanded"))
            return false;
    }

    // 3-bit indexed + SSB use-state: SSB precedes the index field.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000603u,  // 3-bit indexed + SSB
            0x10006404u, 0x00000100u,  // MMDMA load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00010000u,
            0x10006430u, 0x00010000u,
            0x1000640cu, 0x00009200u,  // PIP selectors for materialization
            0x1000643cu, 0x00000603u,  // use-state -> materialize
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0xd2u};  // SSB=1 idx=5, SSB=0 idx=2
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.uploads.size() == 1,
                    "3-bit indexed + SSB use-state reaches backend"))
            return false;
        if (!expect(backend.uploads[0].pixels[0] == 0x80887766u &&
                        backend.uploads[0].pixels[1] == 0x3f998877u,
                    "SSB-first index decode selects the PIP entry"))
            return false;
    }

    // The State 3 race loads raw 512-bit rows while the preceding texture's
    // 6-bit TXTEXPTYPE is still live, then supplies a 4-bit, 128x256 use
    // state. This reduced source-shaped fixture pins the same ordering: TLD
    // must stage raw rows and the following use state must decode them.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000406u,  // preceding 6-bit use state
            0x10006404u, 0x00000000u,  // raw texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,  // two rows
            0x1000642cu, 0x00200020u,  // 32 raw bits per row
            0x10000014u, 0x00000001u,  // TLD while 0x406 is stale
            0x1000642cu, 0x00070001u,  // use state: 8x2
            0x1000640cu, 0x00009200u,
            0x10006414u, 0x00000000u,
            0x1000643cu, 0x00000404u,  // 4-bit use format materializes
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                             0x76u, 0x54u, 0x32u, 0x10u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.uploads.size() == 1,
                    "stale-format raw rows reach their following use state"))
            return false;
        if (!expect(backend.uploads[0].w == 8 &&
                        backend.uploads[0].h == 2 &&
                        backend.uploads[0].pixels[0] == 0xff332211u &&
                        backend.uploads[0].pixels[7] == 0x20aa9968u &&
                        backend.uploads[0].pixels[15] == 0xff332211u,
                    "raw rows decode with the later 4-bit format"))
            return false;
    }

    // Same ordering, but the stale use state is an 8-bit indexed format that
    // divides the 32-bit rows evenly, so the load decodes eagerly as a 4x2
    // texture. TRAM holds the raw bits and the hardware applies TXTEXPTYPE at
    // use time: the later 0x404 write must re-expand the rows as 8x2 4-bit
    // texels and the draw must sample that cel, not the stale 8-bit decode.
    // The race's car-shadow quad hits this whenever the preceding draw left
    // a row-dividing format live.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000408u,  // preceding 8-bit use state
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,  // raw texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,  // two rows
            0x1000642cu, 0x00200020u,  // 32 raw bits per row
            0x10000014u, 0x00000001u,  // TLD while 0x408 is stale
            0x1000642cu, 0x00070001u,  // use state: 8x2
            0x10006414u, 0x00000000u,
            0x1000643cu, 0x00000404u,  // 4-bit use format
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                             0x76u, 0x54u, 0x32u, 0x10u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && !backend.uploads.empty() &&
                        backend.draws.size() == 1,
                    "row-dividing stale format reaches the draw"))
            return false;
        const auto& drawn =
            backend.uploads[static_cast<size_t>(backend.draws[0].cel) - 1u];
        if (!expect(drawn.w == 8 && drawn.h == 2 &&
                        drawn.pixels[0] == 0xff332211u &&
                        drawn.pixels[7] == 0x20aa9968u &&
                        drawn.pixels[15] == 0xff332211u,
                    "eager stale decode is re-expanded with the 4-bit use "
                    "format"))
            return false;
    }

    // A use state that repeats the load-time format keeps the eager decode:
    // no re-expansion and no second upload.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000404u,
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x1000642cu, 0x00200020u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00070001u,
            0x10006414u, 0x00000000u,
            0x1000643cu, 0x00000404u,  // same format rewritten
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                             0x76u, 0x54u, 0x32u, 0x10u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.uploads.size() == 1 &&
                        backend.uploads[0].w == 8 &&
                        backend.uploads[0].pixels[0] == 0xff332211u,
                    "matching use format keeps the eager full-load decode"))
            return false;
    }

    // Shared memory for the full-load reinterpretation cases below: the PIP
    // at 0x100 and two 32-bit raw rows at 0x200.
    const auto full_load_memory = [&]() {
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                             0x76u, 0x54u, 0x32u, 0x10u};
        memory.ranges.push_back(std::move(texel_range));
        return memory;
    };
    const auto with_quad = [&](std::vector<uint32_t> words) {
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(words, value);
        return words;
    };

    // The SDK order texel load -> PIP load -> use state. A PIP load writes
    // PIP RAM, not TRAM, so it must not drop the full load's raw rows: the
    // 4-bit use format still re-expands the stale 8-bit decode at the draw.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = with_quad({
            0x1000643cu, 0x00000408u,  // stale 8-bit use state
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,  // raw texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x1000642cu, 0x00200020u,
            0x10000014u, 0x00000001u,
            0x10006404u, 0x00000200u,  // PIP load after the texels
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00070001u,  // use state: 8x2, 4-bit
            0x10006414u, 0x00000000u,
            0x1000643cu, 0x00000404u,
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        });
        const m2host::CltMemorySnapshot memory = full_load_memory();
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.draws.size() == 1,
                    "PIP load between texels and use state reaches the draw"))
            return false;
        const auto& drawn =
            backend.uploads[static_cast<size_t>(backend.draws[0].cel) - 1u];
        if (!expect(drawn.w == 8 && drawn.h == 2 &&
                        drawn.pixels[0] == 0xff332211u &&
                        drawn.pixels[7] == 0x20aa9968u,
                    "PIP load does not discard the full load's raw rows"))
            return false;
    }

    // TRAM keeps raw bits for the whole frame. A full-load resident that has
    // since been pushed into the TRAM cache by another load is drawn again
    // under a different format (8-bit instead of its load-time 4-bit): the
    // hardware reads the same bits as 4x2 8-bit texels.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = with_quad({
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000404u,  // 4-bit format live at both loads
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,  // resident A at TRAM 0
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x1000642cu, 0x00200020u,
            0x10000014u, 0x00000001u,
            0x10006414u, 0x00001000u,  // resident B at TRAM 0x1000
            0x10000014u, 0x00000001u,
            0x10006414u, 0x00000000u,  // draw A under an 8-bit format
            0x1000643cu, 0x00000408u,
            0x1000642cu, 0x00030001u,  // use state: 4x2
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        });
        const m2host::CltMemorySnapshot memory = full_load_memory();
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.draws.size() == 1,
                    "cached full-load resident reaches a later draw"))
            return false;
        const auto& drawn =
            backend.uploads[static_cast<size_t>(backend.draws[0].cel) - 1u];
        // Reference: the same bytes loaded while 8-bit is the live format.
        RecordingBackend reference_backend;
        m2host::GuestCltInterpreter reference(&reference_backend);
        const std::vector<uint32_t> reference_words = with_quad({
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000408u,
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x1000642cu, 0x00200020u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00030001u,
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        });
        const m2host::CltExecutionResult reference_result = reference.execute(
            reference_words.data(), reference_words.size(), &memory);
        if (!expect(reference_result.ok && reference_backend.draws.size() == 1,
                    "8-bit reference full load draws"))
            return false;
        const auto& expected = reference_backend.uploads[
            static_cast<size_t>(reference_backend.draws[0].cel) - 1u];
        if (!expect(drawn.w == 4 && drawn.h == 2 && expected.w == 4 &&
                        expected.h == 2 && drawn.pixels == expected.pixels,
                    "cached full-load resident re-decodes under the draw's "
                    "format"))
            return false;
    }

    // An offset draw into a full-load resident under its own load-time format
    // keeps the eager decode: only a format change re-decodes the raw rows.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = with_quad({
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000404u,
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000000u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x1000642cu, 0x00200020u,
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00070001u,
            0x10006414u, 0x00000004u,  // second row of the resident
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        });
        const m2host::CltMemorySnapshot memory = full_load_memory();
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && backend.draws.size() == 1 &&
                        backend.uploads.size() == 1 &&
                        backend.draws[0].cel == 1,
                    "offset draw under the load-time format keeps the eager "
                    "decode"))
            return false;
    }

    // A staged raw-row resident may be superseded before any compatible use
    // format or draw arrives. The still-live 6-bit format cannot describe
    // these 32-bit rows, so the next load boundary must treat the register
    // snapshot as incomplete rather than reporting an unsupported draw.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00000406u,  // stale 6-bit use state
            0x10006404u, 0x00000000u,  // raw texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,  // two rows
            0x1000642cu, 0x00200020u,  // 32 raw bits per row
            0x10000014u, 0x00000001u,
            0x1000642cu, 0x00070001u,  // completed geometry, no new format
            0x1000640cu, 0x00009200u,
            0x10006414u, 0x00000000u,
            0x10006404u, 0x00000100u,  // next load supersedes it unused
        };
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                             0x76u, 0x54u, 0x32u, 0x10u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 0 && backend.uploads.empty(),
                    "superseded stale-format raw rows are not unsupported"))
            return false;
    }

    // Full (uncompressed) 8-bit indexed load. The old path assumed 16 bits
    // per texel, so the width and every texel were wrong; the format's
    // IDEPTH now drives the row geometry and the PIP materialization.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000408u,  // 8-bit indexed full-load format
            0x10006404u, 0x00000000u,  // full texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,  // two rows
            0x1000642cu, 0x00200020u,  // destination 32 bits, source 32 bits
            0x1000640cu, 0x00009200u,
            0x10000014u, 0x00000001u,
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0u, 1u, 2u, 3u, 3u, 2u, 1u, 0u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        backend.uploads.size() == 1,
                    "8-bit indexed full load reaches backend"))
            return false;
        if (!expect(backend.uploads[0].w == 4 && backend.uploads[0].h == 2 &&
                        backend.uploads[0].pixels[0] == 0xff332211u &&
                        backend.uploads[0].pixels[3] == 0x1fccbbaau &&
                        backend.uploads[0].pixels[4] == 0x1fccbbaau &&
                        backend.uploads[0].pixels[7] == 0xff332211u,
                    "full-load IDEPTH drives width and PIP expansion"))
            return false;

        // The full-load indexed resident keeps its source vectors, so a later
        // use-state selector change rebinds it like the MMDMA/compressed paths.
        std::vector<uint32_t> rebind_words = {
            0x10006440u, 0x00aabbccu,  // TxtPIPConst0
            0x1000640cu, 0x00009000u,  // constant color, PIP alpha/SSB
            0x200a0002u,
        };
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(rebind_words, value);
        const m2host::CltExecutionResult rebind_result = interpreter.execute(
            rebind_words.data(), rebind_words.size(), nullptr);
        if (!expect(rebind_result.ok && rebind_result.unsupported == 0 &&
                        backend.uploads.size() == 2,
                    "full-load indexed resident rebinds"))
            return false;
        if (!expect(backend.uploads[1].pixels[0] == 0xffccbbaau &&
                        backend.uploads[1].pixels[1] == 0x7fccbbaau &&
                        backend.uploads[1].pixels[2] == 0x3fccbbaau &&
                        backend.uploads[1].pixels[3] == 0x1fccbbaau,
                    "full-load resident rebuilds from the new selectors"))
            return false;
    }

    // Full (uncompressed) literal RGB888: 24 bits per texel, identity
    // replication for 8-bit channels, opaque when no alpha channel exists.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00001408u,  // literal RGB888
            0x10006404u, 0x00000000u,
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x1000642cu, 0x00300030u,  // destination 48 bits, source 48 bits
            0x10000014u, 0x00000001u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0x12u, 0x34u, 0x56u, 0xabu, 0xcdu, 0xefu};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                        backend.uploads.size() == 1,
                    "literal RGB888 full load reaches backend"))
            return false;
        if (!expect(backend.uploads[0].w == 2 && backend.uploads[0].h == 1 &&
                        backend.uploads[0].pixels[0] == 0xff563412u &&
                        backend.uploads[0].pixels[1] == 0xffefcdabu,
                    "RGB888 channels decode MSB-first and stay opaque"))
            return false;
    }

    // Compressed DCI with a 2-bit indexed expanded destination is a palette
    // resident now: a later TXTPIPCNTL change must rebind the presented cel
    // instead of freezing the load-time palette.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> load_words = {
            0x10006404u, 0x00000200u,
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000020u,
            0x10000014u, 0x00000001u,
            0x10006434u, 0x04010401u,  // 1-bit indexed DCI sources
            0x10006438u, 0x04010401u,
            0x1000643cu, 0x00000402u,  // 2-bit indexed expanded destination
            0x1000640cu, 0x00009200u,
            0x10006404u, 0x00000400u,  // compressed texture load
            0x10006414u, 0x00000000u,
            0x10006424u, 0x00000200u,
            0x10006428u, 0x00000004u,
            0x1000642cu, 0x00080008u,
            0x10000014u, 0x00000001u,
            0x10006408u, 0x00002000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(load_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange pip_range;
        pip_range.guest_address = 0x100u;
        pip_range.bytes = pip_bytes;
        memory.ranges.push_back(std::move(pip_range));
        m2host::CltMemoryRange source_range;
        source_range.guest_address = 0x200u;
        // Control 0x23: type 0, string store, four texels; index bits 0101.
        source_range.bytes = {0x23u, 0x50u};
        memory.ranges.push_back(std::move(source_range));
        const m2host::CltExecutionResult load_result = interpreter.execute(
            load_words.data(), load_words.size(), &memory);
        if (!expect(load_result.ok && load_result.unsupported == 0 &&
                        backend.uploads.size() == 1,
                    "2-bit indexed compressed DCI reaches backend"))
            return false;
        if (!expect(backend.uploads[0].pixels[0] == 0xff332211u &&
                        backend.uploads[0].pixels[1] == 0x7f665544u &&
                        backend.uploads[0].pixels[2] == 0xff332211u &&
                        backend.uploads[0].pixels[3] == 0x7f665544u,
                    "2-bit compressed indices select their direct PIP entries"))
            return false;

        // Deliberate plausible-wrong control: clearing COMPRESSED on the same
        // source bytes must use the uncompressed 2-bit row layout, not the
        // compressed DCI run decoder. The resulting palette sequence differs
        // from the compressed fixture, so an implementation that ignores the
        // control bit cannot satisfy both paths.
        RecordingBackend uncompressed_backend;
        m2host::GuestCltInterpreter uncompressed_interpreter(
            &uncompressed_backend);
        std::vector<uint32_t> uncompressed_words = load_words;
        uncompressed_words[19] = 0x00000000u;  // mode 0, uncompressed
        uncompressed_words[25] = 0x00000001u;  // one raw source row
        const m2host::CltExecutionResult uncompressed_result =
            uncompressed_interpreter.execute(uncompressed_words.data(),
                                              uncompressed_words.size(),
                                              &memory);
        if (!expect(uncompressed_result.ok &&
                        uncompressed_result.unsupported == 0 &&
                        uncompressed_backend.uploads.size() == 1 &&
                        uncompressed_backend.uploads[0].pixels.size() == 4u,
                    "uncompressed control bit selects the raw indexed path"))
            return false;
        if (!expect(uncompressed_backend.uploads[0].pixels !=
                        backend.uploads[0].pixels,
                    "compressed control bit is not ignored by the loader"))
            return false;

        std::vector<uint32_t> rebind_words = {
            0x10006440u, 0x00aabbccu,  // TxtPIPConst0
            0x1000640cu, 0x00009000u,  // constant color, PIP alpha/SSB
            0x200a0002u,
        };
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(rebind_words, value);
        const m2host::CltExecutionResult rebind_result = interpreter.execute(
            rebind_words.data(), rebind_words.size(), nullptr);
        if (!expect(rebind_result.ok && rebind_result.unsupported == 0 &&
                        backend.uploads.size() == 2,
                    "2-bit indexed compressed resident rebinds"))
            return false;
        if (!expect(backend.uploads[1].pixels[0] == 0xffccbbaau &&
                        backend.uploads[1].pixels[1] == 0x7fccbbaau,
                    "changed TXTPIPCNTL selectors rebuild the resident cel"))
            return false;
    }

    // Reserved encodings stay fail-closed: IDEPTH 0 is reserved in the
    // toolkit table, and alpha depth 5 is outside the 4/7-bit set.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x1000643cu, 0x00000400u,  // indexed with IDEPTH 0
            0x10006404u, 0x00000000u,  // full texture load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000001u,
            0x1000642cu, 0x00080008u,
            0x10000014u, 0x00000001u,
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00009200u,
            0x200a0002u,
        };
        std::vector<uint32_t> complete_words = words;
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(complete_words, value);
        m2host::CltMemorySnapshot memory;
        m2host::CltMemoryRange texel_range;
        texel_range.guest_address = 0x200u;
        texel_range.bytes = {0u, 0u};
        memory.ranges.push_back(std::move(texel_range));
        const m2host::CltExecutionResult result = interpreter.execute(
            complete_words.data(), complete_words.size(), &memory);
        if (!expect(result.ok && result.unsupported != 0 &&
                        backend.uploads.empty(),
                    "reserved IDEPTH 0 full load stays fail-closed"))
            return false;

        RecordingBackend alpha_backend;
        m2host::GuestCltInterpreter alpha_interpreter(&alpha_backend);
        std::vector<uint32_t> alpha_words = words;
        alpha_words[0] = 0x1000643cu;
        alpha_words[1] = 0x00001c05u;  // literal RGB555 + reserved A5 + SSB
        for (const auto& vertex : quad_vertices)
            for (float value : vertex) append_float(alpha_words, value);
        const m2host::CltExecutionResult alpha_result = alpha_interpreter.execute(
            alpha_words.data(), alpha_words.size(), &memory);
        return expect(alpha_result.ok && alpha_result.unsupported != 0 &&
                          alpha_backend.uploads.empty(),
                      "reserved alpha depth full load stays fail-closed");
    }
}

// A mode-0 TLD that cannot describe its rows with the live TXTEXPTYPE stages
// the exact destination bits (raw rows) and publishes under the use format
// that follows. Those staged bytes are TRAM: the M2 texture engine keeps no
// resident decode, so a later draw that reuses the same rows under a
// materially different TXTEXPTYPE must decode them again through its own
// format rather than inherit the first decode. The first draw arms 4-bit
// indexed rows (32 / 4 = 8 texels wide, two rows); the second arms 2-bit
// indexed rows over the same 32 raw bits (32 / 2 = 16 texels wide, two rows).
bool test_raw_row_redecode_on_use_state_change()
{
    const uint32_t pip_words[8] = {
        0xff112233u, 0x3f445566u, 0x1f778899u, 0x0faabbccu,
        0x80556677u, 0x40667788u, 0x20678899u, 0x106899aa,
    };
    std::vector<uint8_t> pip_bytes;
    for (const uint32_t word : pip_words) {
        pip_bytes.push_back(static_cast<uint8_t>(word >> 24u));
        pip_bytes.push_back(static_cast<uint8_t>(word >> 16u));
        pip_bytes.push_back(static_cast<uint8_t>(word >> 8u));
        pip_bytes.push_back(static_cast<uint8_t>(word));
    }
    // PIP entry colours as the decoder's documented expansion composes them:
    // TXTEXPTYPE indexed rows take RGB and alpha from the PIP entry, whose
    // packed word is A:R:G:B and whose 7-bit alpha field widens to 8 bits by
    // MSB replication (decode_pip). A 4-bit index row of the staged payload
    // reads the nibbles 0..7, and a 2-bit index row reads, per byte, the four
    // 2-bit groups listed below.
    const auto pip_rgba = [](uint32_t entry) {
        const uint32_t red = (entry >> 16u) & 0xffu;
        const uint32_t green = (entry >> 8u) & 0xffu;
        const uint32_t blue = entry & 0xffu;
        const uint32_t alpha_byte = (entry >> 24u) & 0xffu;
        const uint32_t alpha = ((alpha_byte & 0x7fu) << 1u) | (alpha_byte & 1u);
        return red | (green << 8u) | (blue << 16u) | (alpha << 24u);
    };
    std::vector<uint32_t> pip_rgba_table;
    for (const uint32_t entry : pip_words)
        pip_rgba_table.push_back(pip_rgba(entry));
    if (!expect(pip_rgba_table[0] == 0xff332211u &&
                    pip_rgba_table[1] == 0x7f665544u &&
                    pip_rgba_table[7] == 0x20aa9968u,
                "the PIP oracle composes the palette the indexed rows read"))
        return false;
    // 2-bit groups, MSB-first, of row 0 (01 23 45 67) and row 1 (76 54 32 10).
    const uint32_t two_bit_indices[32] = {
        0u, 0u, 0u, 1u,  0u, 2u, 0u, 3u,
        1u, 0u, 1u, 1u,  1u, 2u, 1u, 3u,
        1u, 3u, 1u, 2u,  1u, 1u, 1u, 0u,
        0u, 3u, 0u, 2u,  0u, 1u, 0u, 0u,
    };

    // One staged raw-row resident, two draws. The second draw list changes
    // TXTEXPTYPE 0x404 -> 0x402 and TXTUVMAX 8x2 -> 16x2 while holding the LOD
    // base at 0, so both draws sample the same staged TRAM bytes. A raw-row
    // decode reads neither TXTUVMAX nor the UV mask (its width is the TLD row
    // pitch over the draw's bits per texel), so the second decode can only
    // come from the draw's own format.
    std::vector<uint32_t> words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000020u,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // stale 6-bit use state
        0x10006404u, 0x00000000u,  // mode-0 raw texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,  // two rows
        0x1000642cu, 0x00200020u,  // 32 raw bits per row
        0x10000014u, 0x00000001u,  // TLD stages the raw rows under 0x406
        0x1000642cu, 0x00070001u,  // use window 8x2
        0x1000640cu, 0x00009200u,
        0x10006414u, 0x00000000u,
        0x1000643cu, 0x00000404u,  // 4-bit indexed use format materializes
        0x10006408u, 0x00002000u,
        0x10006410u, 0x00009200u,
    };
    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {2.f, 0.f, 2.f, 0.f},
        {0.f, 2.f, 0.f, 2.f},
    };
    append_strip(words, quad, 0.f);
    words.push_back(0x1000642cu);
    words.push_back(0x00030001u);  // TXTUVMAX 16x2: not a raw-row input
    words.push_back(0x10006414u);
    words.push_back(0x00000000u);  // the same rows at the same LOD base
    words.push_back(0x1000643cu);
    words.push_back(0x00000402u);  // 2-bit indexed use format
    append_strip(words, quad, 4.f);

    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    pip_range.bytes = pip_bytes;
    memory.ranges.push_back(std::move(pip_range));
    // The stored row payload. Row 0 is 0x01 0x23 0x45 0x67 and row 1 is
    // 0x76 0x54 0x32 0x10; the two armed formats read the same 64 raw bits
    // at different texel widths.
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes = {0x01u, 0x23u, 0x45u, 0x67u,
                    0x76u, 0x54u, 0x32u, 0x10u};
    memory.ranges.push_back(std::move(texels));
    const m2host::CltExecutionResult result = interpreter.execute(
        words.data(), words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 2 &&
                    backend.uploads.size() == 2u,
                "raw rows reach both use states"))
        return false;
    if (!expect(backend.uploads[0].w == 8 && backend.uploads[0].h == 2 &&
                    backend.uploads[0].pixels[0] == pip_rgba_table[0] &&
                    backend.uploads[0].pixels[7] == pip_rgba_table[7] &&
                    backend.uploads[0].pixels[15] == pip_rgba_table[0],
                "the first draw publishes the 4-bit indexed row decode"))
        return false;
    if (!expect(backend.uploads[1].w == 16 && backend.uploads[1].h == 2 &&
                    backend.uploads[1].pixels.size() == 32u,
                "the changed TXTEXPTYPE re-decodes the staged rows at its own "
                "texel width"))
        return false;
    for (size_t i = 0; i < 32u; ++i) {
        if (!expect(backend.uploads[1].pixels[i] ==
                        pip_rgba_table[two_bit_indices[i]],
                    "the re-decode reads the staged rows through the second "
                    "format's index stream"))
            return false;
    }
    // A cel id indexes the upload that served the draw, so the second draw
    // naming cel 2 is the re-decode reaching the backend rather than the
    // resident's own 8x2 cel being reused.
    if (!expect(backend.draws[0].cel == 1 && backend.draws[1].cel == 2,
                "the second draw is served its own decode, not the resident's"))
        return false;
    return true;
}

// CLT-LOD-BASE-CACHE-IDENTITY: the resident fast path must retain the LOD
// quad its own decode was materialized from. TXTLODBASE1..3 determine the
// drawn level's footprint and its authored chain (and, for a LODMAX-0 state,
// the coherent full-quad fallback), so a draw that changes a *consumed*
// coarser base cannot sample the resident's decode. A change confined to an
// inactive slot (TXTADDRCNTL.LODMAX bounds the active prefix, matching
// CLT-INACTIVE-LOD-SLOTS) must still reuse it.
bool test_rgb555_dither_transport()
{
    // The guest's DITHEREN bit and matrix pair reach the backend only when the
    // host option M2_RGB555_DITHER is exactly "1"; every other case keeps the
    // dither state disabled so the default image is untouched.
    const auto run = [](const char* option, uint32_t dbuser,
                        m2host::CltDither* out) {
        if (option) setenv("M2_RGB555_DITHER", option, 1);
        else unsetenv("M2_RGB555_DITHER");
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10008008u, dbuser,
            0x10008080u, 0xc0d12e3fu,  // CLT_DBDITHERMATRIX, toolkit pattern
            0x10008084u, 0xe1d0302fu,
            0x20090002u,
        };
        const float vertices[][6] = {
            {4.f, 4.f, 1.f, 1.f, 1.f, 1.f},
            {20.f, 4.f, 1.f, 1.f, 1.f, 1.f},
            {4.f, 20.f, 1.f, 1.f, 1.f, 1.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        unsetenv("M2_RGB555_DITHER");
        if (!result.ok || result.unsupported != 0 || backend.draws.size() != 1)
            return false;
        *out = backend.draws[0].blend.dither;
        return true;
    };
    m2host::CltDither d;
    if (!expect(run("1", 0x1fu, &d) && d.enabled &&
                    d.matrix_a == 0xc0d12e3fu && d.matrix_b == 0xe1d0302fu,
                "option on + DITHEREN transports the authored matrix"))
        return false;
    if (!expect(run(nullptr, 0x1fu, &d) && !d.enabled,
                "option unset keeps the guest's DITHEREN a no-op"))
        return false;
    for (const char* other : {"0", "", "2", "true", "11"})
        if (!expect(run(other, 0x1fu, &d) && !d.enabled,
                    "only the exact value 1 turns dithering on"))
            return false;
    if (!expect(run("1", 0x0fu, &d) && !d.enabled,
                "option on without DITHEREN leaves dithering disabled"))
        return false;
    // The shared model against the toolkit's own pattern: row 0 is
    // -4 0 -3 1, row 1 is 2 -2 3 -1, and the pattern repeats every 4 pixels.
    m2host::CltDither model;
    model.enabled = true;
    model.matrix_a = 0xc0d12e3fu;
    model.matrix_b = 0xe1d0302fu;
    const int expected[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1},
                                {-2, 1, -3, 0}, {3, 0, 2, -1}};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (!expect(m2host::clt_dither_offset(model, x, y) ==
                            expected[y & 3][x & 3],
                        "dither matrix indexing follows the toolkit pattern"))
                return false;
    // 128 + offset truncates to 5 bits and MSB-replicates; alpha is untouched
    // and the offset clamps at both ends.
    if (!expect(m2host::clt_dither_pixel(model, 0x80808080u, 0, 0) ==
                    0x807b7b7bu,
                "128-4 -> 15<<3|3 = 0x7b, alpha kept"))
        return false;
    if (!expect(m2host::clt_dither_pixel(model, 0xff00ff00u, 0, 0) ==
                    0xff00ff00u &&
                    m2host::clt_dither_pixel(model, 0xff00ff00u, 0, 1) ==
                        0xff00ff00u,
                "offsets clamp at 0 and 255 without leaving the 555 grid"))
        return false;
    return true;
}

bool test_batch_rollover_keeps_packet_tail()
{
    // One shaded NEW strip header 0x20090000|(N-1) with N vertices defines
    // N-2 triangles. N=21847 crosses the 65,532-vertex batch bound once the
    // strip is expanded to independent triangles; the rollover flush must
    // not drop the remainder of the same packet.
    const auto delivered = [](uint32_t n, uint32_t* triangles) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {0x20090000u | (n - 1u)};
        const float cycle[3][2] = {{0.f, 0.f}, {64.f, 0.f}, {0.f, 64.f}};
        for (uint32_t i = 0; i < n; ++i) {
            const float vertex[] = {cycle[i % 3u][0], cycle[i % 3u][1],
                                    1.f, 1.f, 1.f, 1.f};
            for (float value : vertex) append_float(words, value);
        }
        const m2host::CltExecutionResult result =
            interpreter.execute(words.data(), words.size());
        uint32_t total = 0;
        for (const auto& draw : backend.draws)
            total += draw.strip
                         ? (draw.vertices.size() >= 2u
                                ? static_cast<uint32_t>(draw.vertices.size()) - 2u
                                : 0u)
                         : static_cast<uint32_t>(draw.vertices.size() / 3u);
        *triangles = total;
        return result.ok && result.unsupported == 0;
    };
    for (uint32_t n : {21845u, 21846u, 21847u, 21848u, 30000u}) {
        uint32_t triangles = 0;
        if (!expect(delivered(n, &triangles) && triangles == n - 2u,
                    "large shaded strip delivers every triangle across the "
                    "batch rollover"))
            return false;
    }
    return true;
}

bool test_lod_base_cache_identity()
{
    // 0x1e78 literal RGB888+A7+SSB: 32-bit texels stored SSB/A7 -> R,G,B, so
    // eight distinct LOD0 texels make the drawn footprint observable in the
    // decoded pixels without palette indirection.
    const uint32_t level0_rgba[8] = {
        0xff204080u, 0xff406020u, 0xff608040u, 0xff80a060u,
        0xffa0c080u, 0xffc0e0a0u, 0xffe0c0c0u, 0xff102030u,
    };
    const uint32_t level1_rgba = 0xff00ff00u;
    const auto texel_bytes = [](uint32_t rgba, std::vector<uint8_t>* bytes) {
        bytes->push_back(0x80u | ((rgba >> 24u) & 0x7fu));
        bytes->push_back(static_cast<uint8_t>(rgba & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 8u) & 0xffu));
        bytes->push_back(static_cast<uint8_t>((rgba >> 16u) & 0xffu));
    };
    constexpr uint32_t kLod0Base = 0x100u;
    constexpr uint32_t kLod1Base = 0x120u;
    constexpr uint32_t kBlobBytes = 0x28u;
    // One resident, four draws over the same retained TRAM bytes:
    //   1. the use list that published it (4x2 LOD0 + authored 2x1 LOD1),
    //   2. a changed inactive slot (LODMAX=1 consumes only LODBASE1),
    //   3. a changed LODBASE1, narrowing the level-0 footprint from 0x20 to
    //      0x10 bytes (a 2x2 power-of-two reconstruction),
    //   4. the same armed state as 3, which the per-draw cache must serve.
    std::vector<uint32_t> words = {
        0x1000643cu, 0x00001e78u,  // literal RGB888+A7+SSB expansion format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // packed chain source address
        0x10006414u, kLod0Base,    // TXTLODBASE0 = tramOffset = lodBase[0]
        0x10006428u, kBlobBytes,   // TXTCOUNT: the whole packed chain
        0x10000014u, 0x00000001u,  // CLT_TxLoad stages the blob
        0x10006414u, kLod0Base,    // use list CLT_TxLoad4LOD
        0x10006418u, kLod1Base,
        0x1000641cu, 0x00000128u,
        0x10006420u, 0x00000130u,
        0x1000642cu, 0x00030001u,  // UMAX=3, VMAX=1 -> the 4x2 drawn level
        0x10006430u, 0x00030001u,  // UMASK=3, VMASK=1
        0x10006408u, 0x000029a1u,  // TXTADDRCNTL: LODMAX 1, MIP addressing
        0x1000640cu, 0x00004900u,
        0x10006410u, 0x00009200u,
    };
    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 4.f, 0.f, 4.f},
    };
    append_strip(words, quad, 0.f);
    // The inactive tail now reads as uninitialized (non-increasing) values.
    // It is outside LODMAX's active prefix, so the decode cannot consume it.
    words.push_back(0x1000641cu);
    words.push_back(0x00000007u);
    words.push_back(0x10006420u);
    words.push_back(0x00000009u);
    append_strip(words, quad, 0.f);
    // The active coarser base changes: the next level is now 0x10 bytes after
    // the drawn base instead of 0x20, so this draw's own footprint differs.
    words.push_back(0x10006418u);
    words.push_back(0x00000110u);
    append_strip(words, quad, 0.f);
    append_strip(words, quad, 0.f);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes.assign(kBlobBytes, 0u);
    {
        std::vector<uint8_t> level0;
        for (uint32_t rgba : level0_rgba) texel_bytes(rgba, &level0);
        std::vector<uint8_t> level1;
        for (unsigned i = 0; i < 2u; ++i)
            texel_bytes(level1_rgba, &level1);
        std::copy(level0.begin(), level0.end(), texels.bytes.begin());
        std::copy(level1.begin(), level1.end(),
                  texels.bytes.begin() + (kLod1Base - kLod0Base));
    }
    memory.ranges.push_back(std::move(texels));

    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const m2host::CltExecutionResult result = interpreter.execute(
        words.data(), words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 4 &&
                    backend.draws.size() == 4u,
                "the four LOD-quad draws are accepted"))
        return false;
    if (!expect(backend.uploads.size() == 2u,
                "only the changed active coarser base forces a new decode"))
        return false;
    const RecordingBackend::Upload& resident = backend.uploads[0];
    if (!expect(resident.w == 4 && resident.h == 2 &&
                    resident.pixels.size() == 8u &&
                    resident.lod_chain.size() == 1u &&
                    resident.lod_chain[0].w == 2 &&
                    resident.lod_chain[0].h == 1,
                "the resident's own decode keeps its 4x2 + authored 2x1 chain"))
        return false;
    const RecordingBackend::Upload& redecoded = backend.uploads[1];
    if (!expect(redecoded.w == 2 && redecoded.h == 2 &&
                    redecoded.pixels.size() == 4u &&
                    redecoded.lod_chain.empty(),
                "the changed LODBASE1 re-decodes the narrower 2x2 footprint"))
        return false;
    // The re-decode starts at the same TRAM bytes in the same order; only the
    // footprint derived from the changed quad is different.
    for (size_t i = 0; i < 4u; ++i) {
        if (!expect(redecoded.pixels[i] == level0_rgba[i],
                    "the re-decode samples the retained bytes at the new base"))
            return false;
    }
    if (!expect(backend.draws[0].cel == 1 && backend.draws[1].cel == 1 &&
                    backend.draws[2].cel == 2 && backend.draws[3].cel == 2,
                "only the consumed-base draw is served the re-decode"))
        return false;
    return true;
}

bool test_mmdma_palette_refresh_keeps_authored_lods()
{
    // CLT-PALETTE-REFRESH-LOSES-MIPS. The guest's TXTPIPCNTL/PIP state is
    // draw/use state: a later draw with a changed palette or selector must
    // re-expand the resident's stored index/alpha/SSB sidecars. The authored
    // coarser lodBase[] payloads must be re-expanded with them; replacing
    // them with host reductions of LOD0 substitutes different bytes for the
    // levels the guest DMA'd (and did exactly that before the fix).
    //
    // The fixture authors an indexed 4-bit residency (0x404) with four
    // distinct levels -- 16x8 LOD0, 8x4 LOD1, 4x2 LOD2, 2x1 LOD3 -- whose
    // texels all carry their level's own PIP index. With all-PIP selectors
    // (0x9200) every expanded texel equals its PIP entry, so a palette reload
    // is observable level by level, and a generated reduction of LOD0 cannot
    // masquerade as an authored coarser level.
    constexpr uint32_t kPipWord0 = 0x00112233u;  // R 0x11 G 0x22 B 0x33
    constexpr uint32_t kPipWord1 = 0x00445566u;  // R 0x44 G 0x55 B 0x66
    constexpr uint32_t kPipWord2 = 0x00778899u;
    constexpr uint32_t kPipWord3 = 0x00aabbccu;
    // The executed reproducer changes the first entry's R byte 0x11 -> 0x55
    // (and the other three so every level's payload changes too).
    constexpr uint32_t kPipWord0New = 0x00552233u;
    constexpr uint32_t kPipWord1New = 0x00665566u;
    constexpr uint32_t kPipWord2New = 0x00887788u;
    constexpr uint32_t kPipWord3New = 0x00ccbbccu;
    // PIP word -> expanded RGBA per the documented layout: R at bits 16..23,
    // G at 8..15, B at 0..7, 7-bit alpha at 24..30, SSB at 31.
    const auto pip_pixel = [](uint32_t word) {
        const uint32_t r = (word >> 16u) & 0xffu;
        const uint32_t g = (word >> 8u) & 0xffu;
        const uint32_t b = word & 0xffu;
        const uint32_t alpha_byte = (word >> 24u) & 0xffu;
        const uint32_t a = ((alpha_byte & 0x7fu) << 1u) | (alpha_byte & 1u);
        return r | (g << 8u) | (b << 16u) | (a << 24u);
    };
    const auto all_equal = [](const std::vector<uint32_t>& pixels,
                              uint32_t value) {
        for (uint32_t pixel : pixels)
            if (pixel != value) return false;
        return true;
    };
    // The identity the interpreter caches for the expanded authored chain.
    // Built from the same level extents the fixture authors, independently of
    // the upload's recorded bytes.
    const auto expected_chain_hash = [&pip_pixel](uint32_t w1, uint32_t w2,
                                                  uint32_t w3) {
        const std::vector<uint32_t> l1(8u * 4u, pip_pixel(w1));
        const std::vector<uint32_t> l2(4u * 2u, pip_pixel(w2));
        const std::vector<uint32_t> l3(2u * 1u, pip_pixel(w3));
        const m2host::CelLodLevel levels[3] = {
            {8, 4, l1.data()}, {4, 2, l2.data()}, {2, 1, l3.data()}};
        return m2host::cel_lod_chain_hash(levels, 3);
    };
    const auto palette_upload_matches =
        [&](const RecordingBackend::Upload& upload, uint32_t w0, uint32_t w1,
            uint32_t w2, uint32_t w3) {
            return upload.w == 16 && upload.h == 8 &&
                   all_equal(upload.pixels, pip_pixel(w0)) &&
                   upload.lod_chain.size() == 3u &&
                   all_equal(upload.lod_chain[0].pixels, pip_pixel(w1)) &&
                   all_equal(upload.lod_chain[1].pixels, pip_pixel(w2)) &&
                   all_equal(upload.lod_chain[2].pixels, pip_pixel(w3)) &&
                   upload.lod_chain_hash ==
                       expected_chain_hash(w1, w2, w3);
        };

    constexpr uint32_t kLod0Base = 0x0u;
    constexpr uint32_t kLod1Base = 0x40u;
    constexpr uint32_t kLod2Base = 0x50u;
    constexpr uint32_t kLod3Base = 0x54u;
    constexpr uint32_t kBlobBytes = 0x55u;
    std::vector<uint32_t> words = {
        0x10006404u, 0x00000200u,  // TXTLDCNTL mode 2: PIP load
        0x10006424u, 0x00000100u,  // PIP source address 0x100
        0x10006414u, 0x00046000u,  // TXTLODBASE0 = TEPIPRAM destination
        0x10006428u, 0x00000040u,  // sixteen PIP entries
        0x10000014u, 0x00000001u,  // TLD executes the PIP load
        0x10006440u, 0x00112233u,  // PIP constant 0 (unused: all selectors PIP)
        0x1000643cu, 0x00000404u,  // 4-bit indexed expansion format
        0x10006404u, 0x00000100u,  // TXTLDCNTL mode 1: MMDMA load
        0x10006424u, 0x00000200u,  // packed chain source
        0x10006414u, kLod0Base,    // TXTLODBASE0 = TRAM destination base
        0x10006418u, kLod1Base,
        0x1000641cu, kLod2Base,
        0x10006420u, kLod3Base,
        0x10006428u, kBlobBytes,   // TXTCOUNT: the whole packed chain
        0x10000014u, 0x00000001u,  // TLD executes the load
        0x1000642cu, 0x00010007u,  // UMAX=1 (clamped axis), VMAX=7
        0x10006430u, 0x03ff0007u,  // UMASK=0x3ff, VMASK=7 (tiled axis)
        0x10006408u, 0x000029a3u,  // TXTADDRCNTL: LODMAX 3, texture enable
        0x1000640cu, 0x00009200u,  // TXTPIPCNTL: color/alpha/SSB from PIP
        0x10006410u, 0x00020499u,  // TXTTABCNTL: PRIM*TEX multiply form
    };
    const float quad[3][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {4.f, 0.f, 4.f, 0.f},
        {0.f, 4.f, 0.f, 4.f},
    };
    append_strip(words, quad, 0.f);

    m2host::CltMemorySnapshot memory;
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    const auto fill_pip_range = [&pip_range](uint32_t w0, uint32_t w1,
                                             uint32_t w2, uint32_t w3) {
        pip_range.bytes.clear();
        const uint32_t entries[16] = {w0, w1, w2, w3};
        for (uint32_t word : entries) {
            pip_range.bytes.push_back(static_cast<uint8_t>(word >> 24u));
            pip_range.bytes.push_back(static_cast<uint8_t>(word >> 16u));
            pip_range.bytes.push_back(static_cast<uint8_t>(word >> 8u));
            pip_range.bytes.push_back(static_cast<uint8_t>(word));
        }
    };
    fill_pip_range(kPipWord0, kPipWord1, kPipWord2, kPipWord3);
    memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange texels;
    texels.guest_address = 0x200u;
    texels.bytes.assign(kBlobBytes, 0u);
    // 4-bit indices, two per byte, MSB first: LOD0 index 0, LOD1 index 1,
    // LOD2 index 2, LOD3 index 3.
    std::fill(texels.bytes.begin() + kLod1Base,
              texels.bytes.begin() + kLod2Base, 0x11u);
    std::fill(texels.bytes.begin() + kLod2Base,
              texels.bytes.begin() + kLod3Base, 0x22u);
    std::fill(texels.bytes.begin() + kLod3Base, texels.bytes.end(), 0x33u);
    memory.ranges.push_back(std::move(texels));

    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const m2host::CltExecutionResult result = interpreter.execute(
        words.data(), words.size(), &memory);
    if (!expect(result.ok && result.unsupported == 0 && result.draws == 1 &&
                    backend.uploads.size() == 1u,
                "the indexed residency and its authored levels reach the "
                "backend"))
        return false;
    const RecordingBackend::Upload& resident = backend.uploads[0];
    if (!expect(resident.w == 16 && resident.h == 8 &&
                    all_equal(resident.pixels, pip_pixel(kPipWord0)),
                "the drawn level is the guest's 16x8 LOD0 payload"))
        return false;
    if (!expect(resident.lod_chain.size() == 3u &&
                    resident.lod_chain[0].w == 8 &&
                    resident.lod_chain[0].h == 4 &&
                    all_equal(resident.lod_chain[0].pixels,
                              pip_pixel(kPipWord1)) &&
                    resident.lod_chain[1].w == 4 &&
                    resident.lod_chain[1].h == 2 &&
                    all_equal(resident.lod_chain[1].pixels,
                              pip_pixel(kPipWord2)) &&
                    resident.lod_chain[2].w == 2 &&
                    resident.lod_chain[2].h == 1 &&
                    all_equal(resident.lod_chain[2].pixels,
                              pip_pixel(kPipWord3)),
                "all three authored coarser levels publish with the base"))
        return false;

    // Guest-visible change: the PIP DMA now carries different entries.
    const auto write_pip_word = [&memory](size_t index, uint32_t word) {
        uint8_t* dst = memory.ranges[0].bytes.data() + index * 4u;
        dst[0] = static_cast<uint8_t>(word >> 24u);
        dst[1] = static_cast<uint8_t>(word >> 16u);
        dst[2] = static_cast<uint8_t>(word >> 8u);
        dst[3] = static_cast<uint8_t>(word);
    };
    write_pip_word(0u, kPipWord0New);
    write_pip_word(1u, kPipWord1New);
    write_pip_word(2u, kPipWord2New);
    write_pip_word(3u, kPipWord3New);
    std::vector<uint32_t> refreshed_words = {
        0x10006404u, 0x00000200u,  // TXTLDCNTL mode 2: PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000040u,
        0x10000014u, 0x00000001u,  // TLD executes the PIP load
        0x10006414u, kLod0Base,    // restore TXTLODBASE0 to the resident base
    };
    append_strip(refreshed_words, quad, 0.f);
    const m2host::CltExecutionResult refreshed = interpreter.execute(
        refreshed_words.data(), refreshed_words.size(), &memory);
    if (!expect(refreshed.ok && refreshed.unsupported == 0 &&
                    refreshed.draws == 1 && backend.uploads.size() == 2u,
                "the palette change re-materializes the residency"))
        return false;
    const RecordingBackend::Upload& rebased = backend.uploads[1];
    if (!expect(rebased.w == 16 && rebased.h == 8 &&
                    all_equal(rebased.pixels, pip_pixel(kPipWord0New)),
                "the refreshed LOD0 follows the new palette entry"))
        return false;
    if (!expect(rebased.lod_chain.size() == 3u &&
                    rebased.lod_chain[0].w == 8 &&
                    rebased.lod_chain[0].h == 4 &&
                    all_equal(rebased.lod_chain[0].pixels,
                              pip_pixel(kPipWord1New)) &&
                    rebased.lod_chain[1].w == 4 &&
                    rebased.lod_chain[1].h == 2 &&
                    all_equal(rebased.lod_chain[1].pixels,
                              pip_pixel(kPipWord2New)) &&
                    rebased.lod_chain[2].w == 2 &&
                    rebased.lod_chain[2].h == 1 &&
                    all_equal(rebased.lod_chain[2].pixels,
                              pip_pixel(kPipWord3New)),
                "the refresh keeps every authored level under the new palette"))
        return false;
    if (!expect(rebased.lod_chain_hash ==
                    expected_chain_hash(kPipWord1New, kPipWord2New,
                                        kPipWord3New),
                "the refresh upload supplies the cached authored-chain identity"))
        return false;

    // Control: reloading the identical palette bytes has no guest-visible
    // effect and must reuse the resident instead of re-materializing it.
    std::vector<uint32_t> identical_words = {
        0x10006404u, 0x00000200u,
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000040u,
        0x10000014u, 0x00000001u,
        0x10006414u, kLod0Base,
    };
    append_strip(identical_words, quad, 0.f);
    const m2host::CltExecutionResult identical = interpreter.execute(
        identical_words.data(), identical_words.size(), &memory);
    if (!expect(identical.ok && identical.unsupported == 0 &&
                    identical.draws == 1 && backend.uploads.size() == 2u &&
                    backend.draws.size() == 3u && backend.draws[2].cel == 2,
                "an identical PIP reload reuses the refreshed resident"))
        return false;

    // Alternation: palette A is a different material, so it must expand its
    // own chain rather than reuse B's cached pixels.
    write_pip_word(0u, kPipWord0);
    write_pip_word(1u, kPipWord1);
    write_pip_word(2u, kPipWord2);
    write_pip_word(3u, kPipWord3);
    const m2host::CltExecutionResult restored = interpreter.execute(
        refreshed_words.data(), refreshed_words.size(), &memory);
    if (!expect(restored.ok && restored.unsupported == 0 &&
                    restored.draws == 1 && backend.uploads.size() == 3u,
                "alternating back to the original palette re-materializes"))
        return false;
    if (!expect(palette_upload_matches(backend.uploads[2], kPipWord0,
                                       kPipWord1, kPipWord2, kPipWord3),
                "the alternating palette expands its own base and chain"))
        return false;

    // Alternating through the same states again must hit the cached material
    // for both palettes and keep each chain bound to its own palette.
    write_pip_word(0u, kPipWord0New);
    write_pip_word(1u, kPipWord1New);
    write_pip_word(2u, kPipWord2New);
    write_pip_word(3u, kPipWord3New);
    const m2host::CltExecutionResult cached_b = interpreter.execute(
        refreshed_words.data(), refreshed_words.size(), &memory);
    if (!expect(cached_b.ok && cached_b.unsupported == 0 &&
                    cached_b.draws == 1 && backend.uploads.size() == 4u,
                "alternating to the cached palette re-materializes"))
        return false;
    if (!expect(palette_upload_matches(backend.uploads[3], kPipWord0New,
                                       kPipWord1New, kPipWord2New,
                                       kPipWord3New),
                "the cached palette expands its own base and chain"))
        return false;
    write_pip_word(0u, kPipWord0);
    write_pip_word(1u, kPipWord1);
    write_pip_word(2u, kPipWord2);
    write_pip_word(3u, kPipWord3);
    const m2host::CltExecutionResult cached_a = interpreter.execute(
        refreshed_words.data(), refreshed_words.size(), &memory);
    if (!expect(cached_a.ok && cached_a.unsupported == 0 &&
                    cached_a.draws == 1 && backend.uploads.size() == 5u,
                "alternating back through the cached palette re-materializes"))
        return false;
    if (!expect(palette_upload_matches(backend.uploads[4], kPipWord0,
                                       kPipWord1, kPipWord2, kPipWord3),
                "the cached original palette still expands its own chain"))
        return false;

    // A resident with the same base level but a different authored coarse
    // payload must not reuse another resident's cached chain. Rewrite LOD1
    // from index 1 to index 2 and republish: the base sidecar hash is
    // unchanged, so only the coarse-level identity can keep the two chains
    // apart in the material cache.
    std::fill(memory.ranges[1].bytes.begin() + kLod1Base,
              memory.ranges[1].bytes.begin() + kLod2Base, 0x22u);
    const m2host::CltExecutionResult republished = interpreter.execute(
        words.data(), words.size(), &memory);
    if (!expect(republished.ok && republished.unsupported == 0 &&
                    republished.draws == 1 && backend.uploads.size() == 6u,
                "a changed authored level republishes the residency"))
        return false;
    const RecordingBackend::Upload& republished_upload = backend.uploads[5];
    if (!expect(republished_upload.lod_chain.size() == 3u &&
                    all_equal(republished_upload.lod_chain[0].pixels,
                              pip_pixel(kPipWord2)),
                "the republished LOD1 carries the new guest payload"))
        return false;
    // Palette B is already resident in the material cache under the *old*
    // chain. The refresh must expand the new chain, not serve the cached one.
    write_pip_word(0u, kPipWord0New);
    write_pip_word(1u, kPipWord1New);
    write_pip_word(2u, kPipWord2New);
    write_pip_word(3u, kPipWord3New);
    const m2host::CltExecutionResult changed_chain = interpreter.execute(
        refreshed_words.data(), refreshed_words.size(), &memory);
    if (!expect(changed_chain.ok && changed_chain.unsupported == 0 &&
                    changed_chain.draws == 1 && backend.uploads.size() == 7u,
                "the palette refresh of the changed chain re-materializes"))
        return false;
    const RecordingBackend::Upload& changed_chain_upload = backend.uploads[6];
    if (!expect(changed_chain_upload.lod_chain.size() == 3u &&
                    all_equal(changed_chain_upload.lod_chain[0].pixels,
                              pip_pixel(kPipWord2New)) &&
                    changed_chain_upload.lod_chain_hash ==
                        expected_chain_hash(kPipWord2New, kPipWord2New,
                                            kPipWord3New),
                "the cached chain is keyed by its own authored payload"))
        return false;
    return true;
}

bool test_interpreter()
{
    if (!test_room_texture_dims()) return false;
    if (!test_mmdma_address_window()) return false;
    if (!test_mmdma_resident_orientation()) return false;
    if (!test_mmdma_register_dimensions()) return false;
    if (!test_resident_selection_by_blast_span()) return false;
    if (!test_tex_load_control_gate()) return false;
    if (!test_pip_load_nonzero_base()) return false;
    if (!test_te_z_plane_clear_contract()) return false;
    if (!test_te_framebuffer_clear_contract()) return false;
    if (!test_te_surface_rebind_after_present()) return false;
    if (!test_mid_list_destination_generation()) return false;
    if (!test_same_list_draw_invalidates_db_source()) return false;
    if (!test_viewport_draw_invalidates_db_source()) return false;
    if (!test_destination_window_containment()) return false;
    if (!test_db_source_msb_replicate_expansion()) return false;
    if (!test_mmdma_pip_order_deferral()) return false;
    if (!test_mmdma_no_color_and_rgb888_forms()) return false;
    if (!test_texture_channel_widening_matrix()) return false;
    if (!test_uvmask_address_transform()) return false;
    if (!test_reserved_filter_selector_fails_closed()) return false;
    if (!test_stale_region_draw_uses_armed_decode()) return false;
    if (!test_raw_row_redecode_on_use_state_change()) return false;
    if (!test_lod_base_cache_identity()) return false;
    if (!test_batch_rollover_keeps_packet_tail()) return false;
    if (!test_rgb555_dither_transport()) return false;
    if (!test_mmdma_palette_refresh_keeps_authored_lods()) return false;
    if (!test_publisher_discrimination_full_load_vs_mmdma()) return false;
    if (!test_compressed_dci_contract()) return false;
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);

    const std::vector<uint32_t> words = shaded_triangle();
    const m2host::CltExecutionResult result =
        interpreter.execute(words.data(), words.size());
    if (!expect(result.ok, "shaded triangle list parses")) return false;
    if (!expect(result.register_writes == 2, "target register writes counted")) return false;
    if (!expect(result.vertex_batches == 1 && result.triangles == 1,
                "triangle batch accounting")) return false;
    if (!expect(result.draws == 1 && backend.draws.size() == 1,
                "untextured guest triangle reaches backend")) return false;
    if (!expect(!backend.draws[0].strip && !backend.draws[0].textured,
                "setup-engine triangle and texture flags preserved"))
        return false;
    if (!expect(backend.draws[0].vertices.size() == 3 &&
                    backend.draws[0].vertices[1].g == 1.f,
                "vertex payload preserved")) return false;
    if (!expect(backend.draws[0].target_w == 640.f &&
                    backend.draws[0].target_h == 480.f,
                "guest target dimensions preserved")) return false;
    if (!expect(!backend.draws[0].blend_enabled,
                "destination blending is disabled by default")) return false;

    // A four-vertex NEW strip is emitted as the exact retained-slot triangles
    // rather than reconstructed by the backend's generic strip parity.
    {
        RecordingBackend quad_backend;
        m2host::GuestCltInterpreter quad_interpreter(&quad_backend);
        const std::vector<uint32_t> words = shaded_strip_quad();
        const m2host::CltExecutionResult result = quad_interpreter.execute(
            words.data(), words.size());
        if (!expect(result.ok && result.vertex_batches == 1 &&
                        result.triangles == 2 && result.draws == 2 &&
                        quad_backend.draws.size() == 2 &&
                        quad_backend.triangle_batch_calls == 1 &&
                        quad_backend.draws[0].independent_triangles &&
                        quad_backend.draws[1].independent_triangles,
                    "four-vertex NEW strip emits two setup-engine triangles"))
            return false;
        if (!expect(!quad_backend.draws[0].strip &&
                        !quad_backend.draws[1].strip &&
                        quad_backend.draws[1].vertices.size() == 3 &&
                        quad_backend.draws[1].vertices[0].x == 2.f &&
                        quad_backend.draws[1].vertices[0].y == 2.f &&
                        quad_backend.draws[1].vertices[1].x == 2.f &&
                        quad_backend.draws[1].vertices[1].y == 0.f &&
                        quad_backend.draws[1].vertices[2].x == 0.f &&
                        quad_backend.draws[1].vertices[2].y == 2.f,
                    "NEW strip second triangle follows retained slot order"))
            return false;
    }

    // A State 3 fan list starts a new fan (three vertices) then appends
    // vertices via continuation headers (new=0; the count field encodes
    // count-1, so a one-vertex continuation is legal).  The interpreter
    // retains the three setup-engine slots and an RM continuation replaces
    // the slot selected by tsort (the observed State 3 0x20170000 form).
    {
        RecordingBackend fan_backend;
        m2host::GuestCltInterpreter fan_interpreter(&fan_backend);
        std::vector<uint32_t> fan_words = {
            0x10000014u, 0x00000020u,  // DCNTL SYNC (no texture load)
            0x200d0002u,                // new, shaded+perspective, 3 vertices
        };
        // Three shaded+perspective vertices: x,y,r,g,b,a,w
        const float fan_vertex[][7] = {
            {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f},
            {2.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f},
            {0.f, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f},
        };
        for (const auto& v : fan_vertex)
            for (float f : v) append_float(fan_words, f);
        // Continuation: fan=1, new=0, count field 0 -> one vertex.  The
        // count field encodes count-1, so 0 means a legal one-vertex
        // continuation (the observed State 3 0x20170000 form).
        fan_words.push_back(0x20150000u);
        const float fan_next[][7] = {
            {1.f, 2.f, 1.f, 1.f, 1.f, 1.f, 1.f},
        };
        for (float f : fan_next[0]) append_float(fan_words, f);
        const m2host::CltExecutionResult fan_result = fan_interpreter.execute(
            fan_words.data(), fan_words.size());
        if (!expect(fan_result.ok,
                    "fan continuation list is structurally valid"))
            return false;
        if (!expect(fan_result.unsupported == 0 && fan_result.draws == 2,
                    "fan continuation emits its triangle instead of failing closed"))
            return false;
        if (!expect(fan_result.triangles == 2,
                    "fan continuation triangle accounting"))
            return false;
        if (!expect(fan_backend.draws.size() == 2 &&
                        fan_backend.draws[1].vertices.size() == 3 &&
                        fan_backend.draws[1].vertices[0].x == 0.f &&
                        fan_backend.draws[1].vertices[0].y == 0.f &&
                        fan_backend.draws[1].vertices[1].x == 1.f &&
                        fan_backend.draws[1].vertices[1].y == 2.f &&
                        fan_backend.draws[1].vertices[2].x == 0.f &&
                        fan_backend.draws[1].vertices[2].y == 2.f,
                    "fan continuation follows retained RM slots"))
            return false;
    }

    // Triangle strip continuation: starts a new strip (three vertices) then
    // appends vertices via continuation headers (new=0, fan=0; count field 0 -> 1 vertex).
    // The interpreter retains the setup-engine slots and emits the exact
    // triangle selected by the current strip sort state.
    {
        RecordingBackend strip_backend;
        m2host::GuestCltInterpreter strip_interpreter(&strip_backend);
        std::vector<uint32_t> strip_words = {
            0x10000014u, 0x00000020u,  // DCNTL SYNC (no texture load)
            0x200d0002u,                // new, shaded+perspective, 3 vertices (strip)
        };
        // Three shaded+perspective vertices: x,y,r,g,b,a,w
        const float strip_vertex[][7] = {
            {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f},
            {2.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f},
            {0.f, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f},
        };
        for (const auto& v : strip_vertex)
            for (float f : v) append_float(strip_words, f);
        // Continuation 1: fan=0, new=0, count field 0 -> one vertex.
        strip_words.push_back(0x20050000u);
        const float strip_next1[][7] = {
            {2.f, 2.f, 1.f, 1.f, 1.f, 1.f, 1.f},
        };
        for (float f : strip_next1[0]) append_float(strip_words, f);
        // Continuation 2: fan=0, new=0, count field 0 -> one vertex.
        strip_words.push_back(0x20050000u);
        const float strip_next2[][7] = {
            {0.f, 4.f, 1.f, 1.f, 1.f, 1.f, 1.f},
        };
        for (float f : strip_next2[0]) append_float(strip_words, f);

        const m2host::CltExecutionResult strip_result =
            strip_interpreter.execute(strip_words.data(), strip_words.size());
        if (!expect(strip_result.ok,
                    "strip continuation list is structurally valid"))
            return false;
        if (!expect(strip_result.unsupported == 0 && strip_result.draws == 3,
                    "strip continuation emits triangles cleanly"))
            return false;
        if (!expect(strip_result.triangles == 3,
                    "strip continuation triangle accounting"))
            return false;
        if (!expect(strip_backend.draws.size() == 3 &&
                        strip_backend.draws[1].vertices.size() == 3 &&
                        strip_backend.draws[1].vertices[0].x == 2.f &&
                        strip_backend.draws[1].vertices[1].x == 2.f &&
                        strip_backend.draws[1].vertices[2].x == 0.f &&
                        strip_backend.draws[2].vertices[0].x == 2.f &&
                        strip_backend.draws[2].vertices[1].x == 0.f &&
                        strip_backend.draws[2].vertices[2].x == 0.f,
                    "strip continuation follows retained setup slots"))
            return false;
    }

    // The production parser must preserve the same short-NEW semantics as
    // the reference model, including across separate execute-list calls.
    {
        RecordingBackend short_backend;
        m2host::GuestCltInterpreter short_interpreter(&short_backend);
        const auto one_vertex = [](uint32_t header, float x, float y) {
            std::vector<uint32_t> words = {header};
            const float vertex[] = {x, y, 1.f, 1.f, 1.f, 1.f};
            for (float value : vertex) append_float(words, value);
            return words;
        };
        const std::vector<uint32_t> short_new =
            one_vertex(0x20090000u, 1.f, 1.f);
        const m2host::CltExecutionResult first_short = short_interpreter.execute(
            short_new.data(), short_new.size());
        if (!expect(first_short.ok && first_short.unsupported == 0 &&
                        first_short.triangles == 0 && first_short.draws == 0,
                    "short NEW is accepted without an incomplete draw"))
            return false;
        const std::vector<uint32_t> short_cont =
            one_vertex(0x20010000u, 2.f, 1.f);
        const m2host::CltExecutionResult second_short = short_interpreter.execute(
            short_cont.data(), short_cont.size());
        if (!expect(second_short.ok && second_short.triangles == 0 &&
                        second_short.draws == 0,
                    "second short vertex remains retained across lists"))
            return false;
        const std::vector<uint32_t> completing_cont =
            one_vertex(0x20010000u, 1.f, 2.f);
        const m2host::CltExecutionResult completed_short =
            short_interpreter.execute(completing_cont.data(),
                                       completing_cont.size());
        if (!expect(completed_short.ok && completed_short.unsupported == 0 &&
                        completed_short.triangles == 1 &&
                        completed_short.draws == 1 &&
                        short_backend.draws.size() == 1 &&
                        short_backend.draws[0].vertices[0].x == 1.f &&
                        short_backend.draws[0].vertices[1].x == 2.f &&
                        short_backend.draws[0].vertices[2].y == 2.f,
                    "short NEW continuation emits the completed triangle"))
            return false;
    }

    RecordingBackend clip_backend;
    clip_backend.host_width = 320;
    clip_backend.host_height = 240;
    m2host::GuestCltInterpreter clip_interpreter(&clip_backend);
    clip_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
    std::vector<uint32_t> complete_outside_clip = {
        0x1001801cu, 0x001200a2u, 0x001200f2u,
        0x10009008u, 0x00000080u,  // outside-only window clipping
        0x20090002u,
    };
    const float clip_vertices[][6] = {
        {10.f, 20.f, 1.f, 0.f, 0.f, 1.f},
        {30.f, 20.f, 0.f, 1.f, 0.f, 1.f},
        {10.f, 40.f, 0.f, 0.f, 1.f, 1.f},
    };
    for (const auto& vertex : clip_vertices)
        for (float value : vertex) append_float(complete_outside_clip, value);
    const m2host::CltExecutionResult outside_clip_result =
        clip_interpreter.execute(complete_outside_clip.data(),
                                 complete_outside_clip.size());
    if (!expect(outside_clip_result.ok && outside_clip_result.draws == 1,
                "observed outside window clip list draws")) return false;
    if (!expect(!clip_backend.scissors.empty() &&
                    clip_backend.scissors.back().enabled &&
                    clip_backend.scissors.back().x == 25.f &&
                    clip_backend.scissors.back().y == 21.f &&
                    clip_backend.scissors.back().w == 72.f &&
                    clip_backend.scissors.back().h == 112.f,
                "window clip maps through View origin and host scale"))
        return false;
    if (!expect(!clip_backend.draws.back().blend_enabled,
                "outside clip list keeps blending disabled")) return false;

    std::vector<uint32_t> shaded_geometry = {0x20090002u};
    for (const auto& vertex : clip_vertices)
        for (float value : vertex) append_float(shaded_geometry, value);
    std::vector<uint32_t> source_over_draw = {
        0x10008008u, 0x0000006fu,  // BLENDEN + SRCEN
        0x10008058u, 0u, 0x10008064u, 2u, 0x10008070u, 0u, // M_DBTrans
    };
    source_over_draw.insert(source_over_draw.end(), shaded_geometry.begin(),
                            shaded_geometry.end());
    const m2host::CltExecutionResult source_over_result =
        clip_interpreter.execute(source_over_draw.data(),
                                 source_over_draw.size());
    if (!expect(source_over_result.ok && source_over_result.draws == 1 &&
                    clip_backend.draws.back().blend_enabled,
                "observed source-over blend state reaches backend"))
        return false;

    const std::vector<uint32_t> blend_without_source = {
        0x10008008u, 0x00000040u,  // unobserved BLENDEN-without-SRCEN mode
    };
    const m2host::CltExecutionResult blend_without_source_result =
        clip_interpreter.execute(blend_without_source.data(),
                                 blend_without_source.size());
    if (!expect(blend_without_source_result.ok &&
                    blend_without_source_result.unsupported == 0,
                "blend-without-source control list parses")) return false;
    const m2host::CltExecutionResult rejected_blend_result =
        clip_interpreter.execute(shaded_geometry.data(),
                                 shaded_geometry.size());
    if (!expect(rejected_blend_result.ok &&
                    rejected_blend_result.unsupported == 1 &&
                    rejected_blend_result.draws == 0,
                "unobserved blend-without-source fails closed")) return false;

    // The vendor M_DBSpec snippet writes DBAMULTCNTL, DBBMULTCNTL, and
    // DBALUCNTL after the user-control gate. Those writes must refresh the
    // stateful destination-blend classification rather than leaving the
    // earlier unsupported result latched.
    const std::vector<uint32_t> specular_state = {
        0x10008054u, 0x00112233u,  // DBCONSTIN RGB
        0x10008058u, 0x00000020u,  // TEXCOLOR * CONST
        0x10008064u, 0x00000040u,  // CONSTCOLOR * TEXALPHA
        0x10008070u, 0x00000000u,  // A+BCLAMP
    };
    const m2host::CltExecutionResult specular_state_result =
        clip_interpreter.execute(specular_state.data(), specular_state.size());
    if (!expect(specular_state_result.ok &&
                    specular_state_result.unsupported == 0,
                "vendor specular blend state refreshes after multiplier writes"))
        return false;
    const m2host::CltExecutionResult specular_draw_result =
        clip_interpreter.execute(shaded_geometry.data(), shaded_geometry.size());
    if (!expect(specular_draw_result.ok && specular_draw_result.draws == 1,
                "vendor specular blend state reaches the backend"))
        return false;
    if (!expect(clip_backend.draws.back().blend.mode ==
                    m2host::CltBlendMode::specular_add &&
                    clip_backend.draws.back().blend.constant_r == 17.f / 255.f &&
                    clip_backend.draws.back().blend.constant_g == 34.f / 255.f &&
                    clip_backend.draws.back().blend.constant_b == 51.f / 255.f,
                "vendor specular formula carries DBCONSTIN to backend"))
        return false;

    const std::vector<uint32_t> inside_clip = {
        0x10008008u, 0x00000100u,  // unobserved inside-only mode
    };
    const m2host::CltExecutionResult inside_clip_result =
        clip_interpreter.execute(inside_clip.data(), inside_clip.size());
    if (!expect(inside_clip_result.ok && inside_clip_result.unsupported == 0,
                "inside clip control list parses")) return false;
    const m2host::CltExecutionResult rejected_inside_result =
        clip_interpreter.execute(complete_outside_clip.data(),
                                 complete_outside_clip.size());
    if (!expect(rejected_inside_result.ok &&
                    rejected_inside_result.unsupported == 1 &&
                    rejected_inside_result.draws == 0,
                "unobserved inverse clip fails closed")) return false;

    const std::vector<uint32_t> disable_clip = {
        0x10008008u, 0x00000000u,
    };
    const m2host::CltExecutionResult disable_clip_result =
        clip_interpreter.execute(disable_clip.data(), disable_clip.size());
    if (!expect(disable_clip_result.ok && disable_clip_result.unsupported == 0,
                "window clip disable parses")) return false;
    const m2host::CltExecutionResult unclipped_result =
        clip_interpreter.execute(complete_outside_clip.data(),
                                 complete_outside_clip.size());
    if (!expect(unclipped_result.ok && unclipped_result.unsupported == 0 &&
                    unclipped_result.draws == 1,
                "disabled window clip restores drawing")) return false;

    const std::vector<uint32_t> register_words = {
        0x10008054u, 0x00112233u,  // DBCONSTIN write
        0x1000800cu, 0x00000007u,  // DBDISCARDCONTROL write
        0x10009008u, 0x00000020u,  // DBUSERCONTROL set
        0x1000980cu, 0x00000003u,  // DBDISCARDCONTROL clear
    };
    const m2host::CltExecutionResult register_result = interpreter.execute(
        register_words.data(), register_words.size());
    if (!expect(register_result.ok && register_result.register_writes == 4,
                "write/set/clear register forms parse")) return false;
    if (!expect(interpreter.register_value(0x8054u) == 0x00112233u,
                "register write state retained")) return false;
    if (!expect(interpreter.register_value(0x8008u) == 0x20u,
                "register set state retained")) return false;
    if (!expect(interpreter.register_value(0x800cu) == 0x4u,
                "register clear state retained")) return false;

    const std::vector<uint32_t> discard_clear = {
        0x1000980cu, 0x00000004u,  // clear the retained discard bit
    };
    const m2host::CltExecutionResult discard_clear_result =
        interpreter.execute(discard_clear.data(), discard_clear.size());
    if (!expect(discard_clear_result.ok &&
                    interpreter.register_value(0x800cu) == 0u,
                "discard state can be cleared before drawing")) return false;

    const std::vector<uint32_t> discard_enable = {
        0x1000800cu, 0x00000004u,  // unproven SSB0 discard on untextured geometry
    };
    const m2host::CltExecutionResult discard_enable_result =
        interpreter.execute(discard_enable.data(), discard_enable.size());
    if (!expect(discard_enable_result.ok &&
                    interpreter.register_value(0x800cu) == 0x4u,
                "discard state is retained across lists")) return false;
    const m2host::CltExecutionResult rejected_discard_result =
        interpreter.execute(words.data(), words.size());
    if (!expect(rejected_discard_result.ok &&
                    rejected_discard_result.unsupported == 1 &&
                    rejected_discard_result.draws == 0,
                "unproven discard state fails closed")) return false;
    const std::vector<uint32_t> discard_reset = {
        0x1000980cu, 0x00000004u,  // clear the unproven discard request
    };
    const m2host::CltExecutionResult discard_reset_result =
        interpreter.execute(discard_reset.data(), discard_reset.size());
    if (!expect(discard_reset_result.ok &&
                    interpreter.register_value(0x800cu) == 0u,
                "discard state reset restores drawing contract")) return false;

    // Proven untextured ALPHA0 discard is supported and draws
    const std::vector<uint32_t> alpha0_enable = {
        0x1000800cu, 0x00000001u,
    };
    interpreter.execute(alpha0_enable.data(), alpha0_enable.size());
    const m2host::CltExecutionResult alpha0_draw_result =
        interpreter.execute(words.data(), words.size());
    if (!expect(alpha0_draw_result.ok && alpha0_draw_result.draws == 1,
                "proven untextured ALPHA0 discard draws")) return false;
    const std::vector<uint32_t> alpha0_reset = {
        0x1000980cu, 0x00000001u,
    };
    interpreter.execute(alpha0_reset.data(), alpha0_reset.size());

    // The recovered lists arm the DB-source copy blit with a multi-register
    // SET header: 0x10049030 is SET (0x1000) of five registers starting at
    // 0x8030 (DBSRCCNTL, DBSRCBASEADDR, DBSRCXSTRIDE, DBSRCOFFSET, DBZCNTL).
    // The old c976b36 mask (0xffff) decoded this as a plain overwrite at
    // 0x9030, losing both the operation and the canonical address.
    {
        const std::vector<uint32_t> multi_set_words = {
            0x10049030u, 0x00000002u, 0x00004000u, 0x00000240u,
            0x00000000u, 0x00000000u,
        };
        const m2host::CltExecutionResult multi_set_result =
            interpreter.execute(multi_set_words.data(), multi_set_words.size());
        if (!expect(multi_set_result.ok &&
                        multi_set_result.register_writes == 5,
                    "multi-register SET header applies to each word")) return false;
        if (!expect(interpreter.register_value(0x8030u) == 0x00000002u &&
                        interpreter.register_value(0x8034u) == 0x00004000u &&
                        interpreter.register_value(0x8038u) == 0x00000240u,
                    "multi-register SET retains canonical DB-source addresses"))
            return false;
    }

    // DCNTL bits 0x10/0x20 are PSE/SYNC, not invented texture-load banks.
    // Writing them must not trigger a texture load (which would report
    // unsupported when no load registers are armed).
    {
        const std::vector<uint32_t> dccntl_sync = {
            0x10000014u, 0x00000020u,  // DCNTL SYNC
        };
        const m2host::CltExecutionResult dccntl_result =
            interpreter.execute(dccntl_sync.data(), dccntl_sync.size());
        if (!expect(dccntl_result.ok && dccntl_result.unsupported == 0 &&
                        dccntl_result.register_writes == 1,
                    "DCNTL PSE/SYNC write does not trigger a texture load"))
            return false;
    }

    // End-to-end DB-source background: arm the copy blit with the exact
    // multi-register SET form, then draw the fullscreen untextured quad
    // (0x200c0003).  The quad must render through the DB-source cel (textured
    // draw) rather than the flat colour path.
    {
        RecordingBackend db_backend;
        m2host::GuestCltInterpreter db_interpreter(&db_backend);
        db_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
        const uint32_t db_base = 0x1000u;
        // DBSRCXSTRIDE.XSTRIDE is the SDK's 11-bit field. Preserve one raw
        // reserved bit here so the source-window decode cannot accidentally
        // treat the register as a 16-bit width.
        const uint32_t db_raw_stride = 0x00000a40u;  // field value 0x240
        std::vector<uint32_t> db_words = {
            0x10009008u, 0x00000020u,  // DBUSERCONTROL set SRCEN (0x20)
            0x10049030u, 0x00000002u, db_base, db_raw_stride,
            0x00050ffdu, 0x00000000u,  // source-dest offset (+5, -3)
            0x200c0003u,  // new, perspective-only, four-vertex strip
        };
        const float background_vertices[][3] = {
            {0.f, 0.f, 1.f},
            {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f},
            {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : background_vertices)
            for (float value : vertex) append_float(db_words, value);

        m2host::CltMemorySnapshot db_memory;
        m2host::CltMemoryRange db_range;
        db_range.guest_address = db_base;
        // 576 x 432 rgb555 texels; every pixel opaque with a distinct colour
        // so a sampled (0,0) collapse or a flat fill would be detectable.
        db_range.bytes.assign(
            static_cast<size_t>(576u * 432u * 2u), 0u);
        for (size_t i = 0; i + 1 < db_range.bytes.size(); i += 2) {
            db_range.bytes[i] = 0x7fu;
            db_range.bytes[i + 1] = 0x00u;  // rgb555 = 0x3f80-ish, opaque red
        }
        db_range.content_hash = snapshot_hash_for_test(db_range.bytes);
        db_memory.ranges.push_back(std::move(db_range));

        const m2host::CltExecutionResult db_result = db_interpreter.execute(
            db_words.data(), db_words.size(), &db_memory);
        if (!expect(db_result.ok && db_result.unsupported == 0 &&
                        db_result.draws == 2 && db_backend.draws.size() == 2,
                    "DB-source armed background quad parses")) return false;
        if (!expect(!db_backend.draws.empty() &&
                        db_backend.draws.back().textured,
                    "DB-source fullscreen quad routes through the textured blit"))
            return false;
        if (!expect(!db_backend.uploads.empty() &&
                        db_backend.uploads.back().pixels.size() ==
                            static_cast<size_t>(576u * 432u) &&
                        db_backend.uploads.back().pixels[0] != 0u,
                    "DB-source blit uploads the 576x432 background bitmap"))
            return false;
        // The fixture arms DBSRCCNTL = 0x2, the `CLA_DBSRCCNTL(1, deptheq32)`
        // source word the M_DBTrans recipe writes, so the materialized texel
        // must be the SRCMSBREP=1 (replicate) expansion: guest word 0x7f00 is
        // red 0x1f / green 0x18 / blue 0x00, which expands to 0xff / 0xc6 /
        // 0x00 with opaque alpha.  The zero form would upload 0xf8 / 0xc0 and
        // turn the authored 16-bit white into a non-white 0xf8.
        if (!expect(db_backend.uploads.back().pixels.front() == 0xff00c6ffu,
                    "DB-source 16-bpp expansion uses the replicate form"))
            return false;

        // The same source word with SRCMSBREP clear must preserve the
        // toolkit's zero-filled low bits in production, not reuse the
        // replicate-form cel or silently force presentation-friendly white.
        RecordingBackend db_zero_backend;
        m2host::GuestCltInterpreter db_zero_interpreter(&db_zero_backend);
        std::vector<uint32_t> db_zero_words = db_words;
        db_zero_words[3] = 0x00000000u;
        const m2host::CltExecutionResult db_zero_result =
            db_zero_interpreter.execute(db_zero_words.data(),
                                        db_zero_words.size(), &db_memory);
        if (!expect(db_zero_result.ok && !db_zero_backend.uploads.empty() &&
                        db_zero_backend.uploads.back().pixels.front() ==
                            0xff00c0f8u,
                    "DB-source SRCMSBREP=0 zero-fills the low three bits"))
            return false;
        float min_u = db_backend.draws.front().vertices.front().u;
        float max_u = min_u;
        float min_v = db_backend.draws.front().vertices.front().v;
        float max_v = min_v;
        for (const RecordingBackend::Draw& draw : db_backend.draws) {
            for (const m2host::CltVertex& vertex : draw.vertices) {
                min_u = std::min(min_u, vertex.u);
                max_u = std::max(max_u, vertex.u);
                min_v = std::min(min_v, vertex.v);
                max_v = std::max(max_v, vertex.v);
            }
        }
        if (!expect(std::abs(min_u - (5.f / 576.f)) < 0.0001f &&
                        std::abs(max_u - (581.f / 576.f)) < 0.0001f &&
                        std::abs(min_v - (-3.f / 432.f)) < 0.0001f &&
                        std::abs(max_v - (429.f / 432.f)) < 0.0001f,
                    "DB-source signed offset emits linear source coordinates"))
            return false;

        // Configure Race movie overlay: the guest biases DBSRCBASEADDR back
        // from the MPEG bitmap so the destination-space quad lands on the
        // decoded pixels. That biased base happens to lie inside the prior
        // display allocation. The sampled addresses, however, lie beyond it,
        // so this must use the guest MPEG bytes rather than framebuffer
        // readback. A fullscreen draw over the same display allocation is the
        // plausible-wrong control and must still use readback.
        {
            constexpr uint32_t display_base = 0x4f0000u;
            constexpr uint32_t movie_biased_base = 0x526e94u;
            constexpr uint32_t display_bytes = 576u * 432u * 2u;
            RecordingBackend movie_backend;
            movie_backend.target_readback_enabled = true;
            movie_backend.target_readback_base = display_base;
            movie_backend.target_readback_color = 0xff00ff00u;
            m2host::GuestCltInterpreter movie_interpreter(&movie_backend);
            movie_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
            movie_interpreter.set_fb_spans(
                {{0x2018u, display_base, display_bytes}});

            std::vector<uint32_t> movie_words = {
                0x10008008u, 0x0000006fu,
                0x10049030u, 0x00000002u, movie_biased_base, 0x00000240u,
                0x00000000u, 0x00000000u,
                0x20080003u,
            };
            const float movie_vertices[][2] = {
                {335.f, 261.f}, {539.f, 261.f},
                {335.f, 389.f}, {539.f, 389.f},
            };
            for (const auto& vertex : movie_vertices)
                for (float value : vertex) append_float(movie_words, value);

            m2host::CltMemorySnapshot movie_memory;
            m2host::CltMemoryRange movie_range;
            movie_range.guest_address = movie_biased_base;
            movie_range.bytes.assign(display_bytes, 0u);
            for (size_t i = 0; i + 1 < movie_range.bytes.size(); i += 2) {
                movie_range.bytes[i] = 0x7cu;
                movie_range.bytes[i + 1] = 0x00u;
            }
            movie_range.content_hash = snapshot_hash_for_test(movie_range.bytes);
            movie_memory.ranges.push_back(std::move(movie_range));
            const auto movie_result = movie_interpreter.execute(
                movie_words.data(), movie_words.size(), &movie_memory);
            if (!expect(movie_result.ok && movie_result.unsupported == 0 &&
                            !movie_backend.uploads.empty() &&
                            movie_backend.uploads.back().pixels.front() ==
                                0xff0000ffu,
                        "biased movie DB-source uses decoded guest pixels"))
                return false;

            RecordingBackend fullscreen_backend;
            fullscreen_backend.target_readback_enabled = true;
            fullscreen_backend.target_readback_base = display_base;
            fullscreen_backend.target_readback_color = 0xff00ff00u;
            m2host::GuestCltInterpreter fullscreen_interpreter(
                &fullscreen_backend);
            fullscreen_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                                480);
            fullscreen_interpreter.set_fb_spans(
                {{0x2018u, display_base, display_bytes}});
            std::vector<uint32_t> fullscreen_words = movie_words;
            fullscreen_words[4] = display_base;
            const size_t vertex_word = 9u;
            const float fullscreen_vertices[][2] = {
                {0.f, 0.f}, {0.f, 432.f},
                {576.f, 0.f}, {576.f, 432.f},
            };
            for (size_t v = 0; v < 4; ++v)
                for (size_t component = 0; component < 2; ++component)
                    fullscreen_words[vertex_word + v * 2u + component] =
                        std::bit_cast<uint32_t>(fullscreen_vertices[v][component]);
            const auto fullscreen_result = fullscreen_interpreter.execute(
                fullscreen_words.data(), fullscreen_words.size(), &movie_memory);
            if (!expect(fullscreen_result.ok &&
                            fullscreen_result.unsupported == 0 &&
                            !fullscreen_backend.uploads.empty() &&
                            fullscreen_backend.uploads.back().pixels.front() ==
                                0xff00ff00u,
                        "fullscreen DB-source still uses addressed target"))
                return false;

            // Production span set for that same overlay. The bridge registers
            // every 576x432 bitmap buffer (live `[dbsrc-fb] register span` on
            // the accepted Configure Race route: 0x1008@0x476000,
            // 0x2018@0x4f0000, 0x3028/0x4038@0x56a000, ...) and the pump hands
            // the interpreter the bound framebuffer's allocation chain, so the
            // Z/movie plane at 0x56a000 is a span sibling. The movie's own
            // samples land inside that neighbour, while its armed base sits
            // 238 rows below it with DBSRCOFFSET 0 - the base, not the samples,
            // is what proves which storage the draw reads. Serving the
            // neighbour's renderer target here is exactly the reported defect
            // (a frozen menu/seeded image in the video rectangle).
            constexpr uint32_t z_movie_plane = 0x56a000u;
            RecordingBackend production_backend;
            production_backend.target_readback_enabled = true;
            production_backend.target_readback_base = z_movie_plane;
            production_backend.target_readback_color = 0xff00ff00u;
            // Both display planes are readable too, so a wrong classification
            // cannot pass by failing its readback and falling back to guest
            // RAM: only the decoded bytes satisfy the assertion below.
            production_backend.extra_target_readback[display_base] =
                0xff00ff00u;
            production_backend.extra_target_readback[display_base - 0x7a000u] =
                0xff00ff00u;
            m2host::GuestCltInterpreter production_interpreter(
                &production_backend);
            production_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                                480);
            production_interpreter.set_fb_spans(
                {{0x1008u, display_base - 0x7a000u, display_bytes},
                 {0x2018u, display_base, display_bytes},
                 {0x4038u, z_movie_plane, display_bytes}});
            const m2host::CltExecutionResult production_result =
                production_interpreter.execute(movie_words.data(),
                                               movie_words.size(),
                                               &movie_memory);
            if (!expect(production_result.ok &&
                            production_result.unsupported == 0 &&
                            !production_backend.uploads.empty() &&
                            production_backend.uploads.back().pixels.front() ==
                                0xff0000ffu,
                        "chained-span MPEG overlay keeps decoded guest "
                        "pixels"))
                return false;

            // Control on the same span set: the row-biased transition form
            // resolves its origin through DBSRCOFFSET and must still read the
            // addressed display plane instead of falling back to guest RAM.
            RecordingBackend biased_production_backend;
            biased_production_backend.target_readback_enabled = true;
            biased_production_backend.target_readback_base = display_base;
            biased_production_backend.target_readback_color = 0xff00ff00u;
            m2host::GuestCltInterpreter biased_production_interpreter(
                &biased_production_backend);
            biased_production_interpreter.set_viewport(32, 24, 576, 432, true,
                                                       640, 480);
            biased_production_interpreter.set_fb_spans(
                {{0x1008u, display_base - 0x7a000u, display_bytes},
                 {0x2018u, display_base, display_bytes},
                 {0x4038u, z_movie_plane, display_bytes}});
            std::vector<uint32_t> biased_production_words = movie_words;
            biased_production_words[4] = display_base + 48u * 576u * 2u;
            biased_production_words[6] = 0x00000fd0u;  // DBSRCOFFSET y = -48
            const size_t pre_bias_uploads =
                biased_production_backend.uploads.size();
            const m2host::CltExecutionResult biased_production_result =
                biased_production_interpreter.execute(
                    biased_production_words.data(),
                    biased_production_words.size(), &movie_memory);
            if (!expect(biased_production_result.ok &&
                            biased_production_result.unsupported == 0 &&
                            biased_production_backend.uploads.size() ==
                                pre_bias_uploads + 1u &&
                            biased_production_backend.uploads.back()
                                    .pixels.front() == 0xff00ff00u,
                        "row-biased transition still reads the addressed "
                        "display plane"))
                return false;
        }

        // A display-carve DB-source read is a same-frame copy: earlier guest
        // lists in the frame rasterize into the carve the current draw
        // samples. The cached cel must therefore be rebuilt for each executed
        // list. A State 2 strip walk re-arms the same source base while the
        // carve content advances underneath it, so a cel keyed only on
        // (base, stride, msb) hands every later list the first snapshot and
        // the destination keeps a frozen copy of the carve. Mirror that
        // sequence: identical words, identical armed base, carve content
        // changed between the two executes.
        {
            constexpr uint32_t carve_base = 0x4f0000u;
            constexpr uint32_t dest_base = 0x56a000u;
            constexpr uint32_t carve_bytes = 576u * 432u * 2u;
            RecordingBackend carve_backend;
            carve_backend.target_readback_enabled = true;
            carve_backend.target_readback_base = carve_base;
            carve_backend.target_readback_color = 0xff00ff00u;
            m2host::GuestCltInterpreter carve_interpreter(&carve_backend);
            carve_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
            carve_interpreter.set_fb_spans(
                {{0x2018u, carve_base, carve_bytes}});

            // Four-vertex strip, so one executed list emits two triangles and
            // the within-list snapshot sharing is observable.
            std::vector<uint32_t> carve_words = {
                0x10009008u, 0x00000020u,
                0x10049030u, 0x00000002u, carve_base, 0x00000240u,
                0x00000000u, 0x00000000u,
                0x200c0003u,
            };
            const float carve_vertices[][3] = {
                {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
                {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
            };
            for (const auto& vertex : carve_vertices)
                for (float value : vertex) append_float(carve_words, value);

            const m2host::CltExecutionResult carve_first =
                carve_interpreter.execute(carve_words.data(),
                                          carve_words.size());
            if (!expect(carve_first.ok && carve_first.draws == 2 &&
                            carve_backend.uploads.size() == 1u &&
                            carve_backend.uploads.back().pixels.front() ==
                                0xff00ff00u,
                        "carve DB-source samples the addressed in-progress "
                        "target once per list"))
                return false;
            const size_t carve_uploads = carve_backend.uploads.size();

            // Same list, same armed base, carve content advanced by an
            // earlier list in the frame.
            carve_backend.target_readback_color = 0xff0000ffu;
            const m2host::CltExecutionResult carve_second =
                carve_interpreter.execute(carve_words.data(),
                                          carve_words.size());
            if (!expect(carve_second.ok &&
                            carve_backend.uploads.size() ==
                                carve_uploads + 1u &&
                            carve_backend.uploads.back().pixels.front() ==
                                0xff0000ffu,
                        "same-base carve read refreshes for the next executed "
                        "list"))
                return false;

            // Square1 keeps the old image in one renderer target, draws into
            // a sibling, and advances DBSRCBASEADDR by one pixel per authored
            // frame. Prime the source as an observed destination, then prove
            // that changing only that address bias reuses its snapshot while
            // updating the source coordinate. The old global per-list key
            // performs a second full readback/upload and fails this control.
            RecordingBackend stable_backend;
            stable_backend.target_readback_enabled = true;
            stable_backend.target_readback_base = carve_base;
            stable_backend.target_readback_color = 0xff00ff00u;
            m2host::GuestCltInterpreter stable_interpreter(&stable_backend);
            stable_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                            480);
            stable_interpreter.set_fb_spans(
                {{0x2018u, carve_base, carve_bytes}});
            stable_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, carve_base, 576u, carve_base, 0u, 0u, 0u,
                0u, 0u, 0u, 1u);
            const auto primed = stable_interpreter.execute(nullptr, 0u);
            if (!expect(primed.ok,
                        "stable DB-source target records its write generation"))
                return false;
            stable_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u,
                0u, 0u, 0u, 2u);
            const auto stable_first = stable_interpreter.execute(
                carve_words.data(), carve_words.size());
            if (!expect(stable_first.ok && stable_backend.uploads.size() == 1u,
                        "sibling DB-source target snapshots once"))
                return false;
            std::vector<uint32_t> biased_words = carve_words;
            biased_words[4] = carve_base + 2u;
            const size_t stable_draws = stable_backend.draws.size();
            const auto stable_second = stable_interpreter.execute(
                biased_words.data(), biased_words.size());
            if (!expect(stable_second.ok &&
                            stable_backend.uploads.size() == 1u &&
                            stable_backend.draws.size() > stable_draws &&
                            stable_backend.draws[stable_draws].vertices[0].u >
                                stable_backend.draws[0].vertices[0].u,
                        "square address bias reuses colour while advancing "
                        "source coordinates"))
                return false;

            // Plausible-wrong control: once that same source target becomes a
            // destination again, its generation changes and the next read
            // must rebuild rather than serve the cached green snapshot.
            stable_backend.target_readback_color = 0xff0000ffu;
            stable_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, carve_base, 576u, carve_base, 0u, 0u, 0u,
                0u, 0u, 0u, 3u);
            const auto changed_source = stable_interpreter.execute(
                carve_words.data(), carve_words.size());
            if (!expect(changed_source.ok &&
                            stable_backend.uploads.size() == 2u &&
                            stable_backend.uploads.back().pixels.front() ==
                                0xff0000ffu,
                        "a source-target write invalidates the reused "
                        "snapshot"))
                return false;
        }

        // Exercise the complete production boundary, not only the uploaded
        // implementation texture: a transition seed is a fullscreen DB-source
        // copy from one addressed target into another. A gradient rejects a
        // wrong target, collapsed coordinate, or stale flat sample while the
        // exact RGB555 expansion supplies the independent expected pixels.
        {
            constexpr uint32_t source_base = 0x4f0000u;
            constexpr uint32_t dest_base = 0x56a000u;
            constexpr uint32_t carve_bytes = 576u * 432u * 2u;
            m2host::SoftCaptureBackend seed_backend(640, 480, {});
            seed_backend.frame_begin(0.05f, 0.07f, 0.10f);
            std::vector<uint32_t> source(640u * 480u, 0xff000000u);
            for (uint32_t y = 0; y < 480u; ++y) {
                for (uint32_t x = 0; x < 640u; ++x) {
                    const uint32_t r = (x & 31u) << 3u;
                    const uint32_t g = (y & 31u) << 3u;
                    const uint32_t b = ((x + y) & 31u) << 3u;
                    source[static_cast<size_t>(y) * 640u + x] =
                        r | (g << 8u) | (b << 16u) | 0xff000000u;
                }
            }
            seed_backend.clt_target_upload_rgba(source_base, source.data(),
                                                640, 480);
            m2host::GuestCltInterpreter seed_interpreter(&seed_backend);
            seed_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
            seed_interpreter.set_fb_spans(
                {{0x2018u, source_base, carve_bytes}});
            seed_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u, 0u,
                0u, 0u, 1u);

            std::vector<uint32_t> seed_words = {
                0x10008008u, 0x0000006fu,
                0x10038064u, 0x00000020u, 0x00ffffffu, 0x00ffffffu,
                0x000000d0u,
                0x10049030u, 0x00000002u, source_base, 0x00000240u,
                0x00000000u, 0x00000000u,
                0x200c0003u,
            };
            const float seed_vertices[][3] = {
                {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
                {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
            };
            for (const auto& vertex : seed_vertices)
                for (float value : vertex) append_float(seed_words, value);
            const m2host::CltExecutionResult seed_result =
                seed_interpreter.execute(seed_words.data(), seed_words.size());
            std::vector<uint32_t> dest(640u * 480u);
            if (!expect(seed_result.ok && seed_result.unsupported == 0u &&
                            seed_result.draws == 2u &&
                            seed_backend.grab_clt_target_rgba(
                                dest_base, dest.data(), 640, 480),
                        "transition seed reaches its addressed destination"))
                return false;
            const uint32_t probes[][2] = {
                {0u, 0u}, {111u, 73u}, {319u, 215u}, {575u, 431u},
            };
            for (const auto& probe : probes) {
                const uint32_t sx = 32u + probe[0];
                const uint32_t sy = 24u + probe[1];
                const uint32_t p = source[static_cast<size_t>(sy) * 640u + sx];
                const uint32_t expected =
                    m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 11u, true)
                     << 8u) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 19u, true)
                     << 16u) |
                    0xff000000u;
                const uint32_t got =
                    dest[static_cast<size_t>(sy) * 640u + sx];
                if (!expect(got == expected,
                            "transition seed copies the addressed source "
                            "texel with RGB555 expansion"))
                    return false;
            }

            // Lifted transitions bias the source base by whole rows and
            // compensate with a negative source coordinate. The addressed TE
            // texel is still the allocation's first row; rebasing the host
            // texture at the biased address would clamp -48 to row zero and
            // sample guest row 48 instead.
            m2host::SoftCaptureBackend biased_backend(640, 480, {});
            biased_backend.frame_begin(0.05f, 0.07f, 0.10f);
            biased_backend.clt_target_upload_rgba(source_base, source.data(),
                                                   640, 480);
            m2host::GuestCltInterpreter biased_interpreter(&biased_backend);
            biased_interpreter.set_viewport(32, 24, 576, 432, true, 640, 480);
            biased_interpreter.set_fb_spans(
                {{0x2018u, source_base, carve_bytes}});
            biased_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u, 0u,
                0u, 0u, 1u);
            std::vector<uint32_t> biased_words = seed_words;
            biased_words[9] = source_base + 48u * 576u * 2u;
            biased_words[11] = 0x00000fd0u;  // DBSRCOFFSET y = -48
            const auto biased_result = biased_interpreter.execute(
                biased_words.data(), biased_words.size());
            std::vector<uint32_t> biased_dest(640u * 480u);
            if (!expect(biased_result.ok && biased_result.unsupported == 0u &&
                            biased_backend.grab_clt_target_rgba(
                                dest_base, biased_dest.data(), 640, 480),
                        "row-biased transition reaches its destination"))
                return false;
            for (const auto& probe : probes) {
                const uint32_t sx = 32u + probe[0];
                const uint32_t sy = 24u + probe[1];
                const uint32_t p = source[static_cast<size_t>(sy) * 640u + sx];
                const uint32_t expected =
                    m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 11u, true)
                     << 8u) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 19u, true)
                     << 16u) |
                    0xff000000u;
                const uint32_t biased_got =
                    biased_dest[static_cast<size_t>(sy) * 640u + sx];
                if (biased_got != expected)
                    std::fprintf(stderr,
                                 "row-bias mismatch probe=(%u,%u) got=%#x expected=%#x\n",
                                 probe[0], probe[1], biased_got, expected);
                if (!expect(
                        biased_got == expected,
                        "row-biased base plus negative coordinate preserves "
                        "the TE source address"))
                    return false;
            }

            // The observed grid arm also walks the origin in the opposite
            // direction: 48 rows before the carve with a +48 row coordinate.
            // This is the live form that the old 0x800 base-classification
            // window rejected before it could inspect the sampled addresses.
            m2host::SoftCaptureBackend negative_backend(640, 480, {});
            negative_backend.frame_begin(0.05f, 0.07f, 0.10f);
            negative_backend.clt_target_upload_rgba(source_base, source.data(),
                                                     640, 480);
            m2host::GuestCltInterpreter negative_interpreter(
                &negative_backend);
            negative_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                              480);
            negative_interpreter.set_fb_spans(
                {{0x2018u, source_base, carve_bytes}});
            negative_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u, 0u,
                0u, 0u, 1u);
            std::vector<uint32_t> negative_words = seed_words;
            negative_words[9] = source_base - 48u * 576u * 2u;
            negative_words[11] = 0x00000030u;  // DBSRCOFFSET y = +48
            const auto negative_bias_result = negative_interpreter.execute(
                negative_words.data(), negative_words.size());
            if (!expect(negative_bias_result.ok &&
                            negative_bias_result.unsupported == 0u &&
                            negative_backend.grab_clt_target_rgba(
                                dest_base, biased_dest.data(), 640, 480),
                        "negative row-biased transition reaches its "
                        "destination"))
                return false;
            for (const auto& probe : probes) {
                const uint32_t sx = 32u + probe[0];
                const uint32_t sy = 24u + probe[1];
                const uint32_t p = source[static_cast<size_t>(sy) * 640u + sx];
                const uint32_t expected =
                    m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 11u, true)
                     << 8u) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 19u, true)
                     << 16u) |
                    0xff000000u;
                const uint32_t negative_bias_got =
                    biased_dest[static_cast<size_t>(sy) * 640u + sx];
                if (negative_bias_got != expected)
                    std::fprintf(stderr,
                                 "negative-row-bias mismatch probe=(%u,%u) got=%#x expected=%#x\n",
                                 probe[0], probe[1], negative_bias_got,
                                 expected);
                if (!expect(
                        negative_bias_got == expected,
                        "negative row-biased base plus positive coordinate "
                        "preserves the TE source address"))
                    return false;
            }

            // The lifted effect routines write DBSRCOFFSET exactly once, with
            // a zero value: `FN_dogridtrans__35d7c` authors it at 0x35e9c and
            // `FN_dosquare1trans__32010` at 0x32140, and each transition
            // function carries the same single `ori ..., 0x803c` header. The
            // whole-row origin bias is compensated by the draw's own source
            // coordinates instead (`base - 48 rows` with the quad's y shifted
            // +48), so the negated half of the pair must classify on the
            // sampled addresses, exactly like its positive twin above. Before
            // the row-aligned origin rule this arm fell through to guest
            // memory and the destination kept an unrelated (black/stale)
            // frame, which is the transition defect.
            m2host::SoftCaptureBackend live_bias_backend(640, 480, {});
            live_bias_backend.frame_begin(0.05f, 0.07f, 0.10f);
            live_bias_backend.clt_target_upload_rgba(
                source_base, source.data(), 640, 480);
            m2host::GuestCltInterpreter live_bias_interpreter(
                &live_bias_backend);
            live_bias_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                               480);
            live_bias_interpreter.set_fb_spans(
                {{0x2018u, source_base, carve_bytes}});
            live_bias_interpreter.set_te_surface_state(
                0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u, 0u,
                0u, 0u, 1u);
            std::vector<uint32_t> live_bias_words = seed_words;
            live_bias_words[9] = source_base - 48u * 576u * 2u;
            // DBSRCOFFSET stays the guest's own zero; the +48-row source
            // coordinate is written into the quad, so its destination rows
            // are the source rows shifted by the same 48.
            const float live_bias_vertices[][3] = {
                {0.f, 48.f, 1.f}, {0.f, 480.f, 1.f},
                {576.f, 48.f, 1.f}, {576.f, 480.f, 1.f},
            };
            for (size_t v = 0; v < 4; ++v)
                for (size_t component = 0; component < 3; ++component)
                    live_bias_words[14u + v * 3u + component] = std::bit_cast<
                        uint32_t>(live_bias_vertices[v][component]);
            const auto live_bias_result = live_bias_interpreter.execute(
                live_bias_words.data(), live_bias_words.size());
            std::vector<uint32_t> live_bias_dest(640u * 480u);
            if (!expect(live_bias_result.ok &&
                            live_bias_result.unsupported == 0u &&
                            live_bias_backend.grab_clt_target_rgba(
                                dest_base, live_bias_dest.data(), 640, 480),
                        "zero-offset negative row bias reaches its "
                        "destination"))
                return false;
            // Source rows 0..383 map onto destination rows 48..431; the
            // authored slide puts the remaining source rows outside the
            // viewport, so probe only the part the guest asks to be visible.
            const uint32_t live_bias_probe_y[] = {0u, 73u, 215u, 383u};
            for (uint32_t probe_y : live_bias_probe_y) {
                for (uint32_t probe_x : {0u, 111u, 319u, 575u}) {
                    const uint32_t sx = 32u + probe_x;
                    const uint32_t sy = 24u + probe_y;
                    const uint32_t p =
                        source[static_cast<size_t>(sy) * 640u + sx];
                    const uint32_t expected =
                        m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
                        (m2host::clt_db_source_expand_5_to_8(p >> 11u, true)
                         << 8u) |
                        (m2host::clt_db_source_expand_5_to_8(p >> 19u, true)
                         << 16u) |
                        0xff000000u;
                    const uint32_t live_bias_got =
                        live_bias_dest[static_cast<size_t>(sy + 48u) * 640u +
                                       sx];
                    if (live_bias_got != expected)
                        std::fprintf(stderr,
                                     "live-row-bias mismatch probe=(%u,%u) "
                                     "got=%#x expected=%#x\n",
                                     probe_x, probe_y, live_bias_got,
                                     expected);
                    if (!expect(live_bias_got == expected,
                                "live zero-offset row-biased base still "
                                "reads the addressed display plane"))
                        return false;
                }
            }

            // The same negated arm on the production allocation chain: its
            // armed base sits inside the *previous* 576x432 plane, so the
            // sampled plane is the only allocation entitled to claim the draw.
            // Every candidate target carries a distinct colour, so a wrong
            // classification cannot pass on an absent readback.
            {
                constexpr uint32_t sibling_base =
                    source_base - 0x7a000u;
                constexpr uint32_t movie_plane = 0x56a000u;
                RecordingBackend chain_backend;
                chain_backend.target_readback_enabled = true;
                chain_backend.target_readback_base = source_base;
                chain_backend.target_readback_color = 0xff00ff00u;
                chain_backend.extra_target_readback[sibling_base] =
                    0xff00ffffu;
                chain_backend.extra_target_readback[movie_plane] =
                    0xff0000ffu;
                m2host::GuestCltInterpreter chain_interpreter(
                    &chain_backend);
                chain_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                               480);
                chain_interpreter.set_fb_spans(
                    {{0x1008u, sibling_base, carve_bytes},
                     {0x2018u, source_base, carve_bytes},
                     {0x4038u, movie_plane, carve_bytes}});
                chain_interpreter.set_te_surface_state(
                    0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u, 0u, 0u,
                    0u, 0u, 1u);
                std::vector<uint32_t> chain_words = seed_words;
                chain_words[9] = source_base - 48u * 576u * 2u;
                for (size_t v = 0; v < 4; ++v)
                    for (size_t component = 0; component < 3; ++component)
                        chain_words[14u + v * 3u + component] =
                            std::bit_cast<uint32_t>(
                                live_bias_vertices[v][component]);
                const auto chain_result = chain_interpreter.execute(
                    chain_words.data(), chain_words.size());
                if (!expect(chain_result.ok &&
                                chain_result.unsupported == 0u &&
                                !chain_backend.uploads.empty() &&
                                chain_backend.uploads.back().pixels.front() ==
                                    0xff00ff00u,
                            "row-biased arm reads the sampled plane, not the "
                            "allocation holding its armed base"))
                    return false;
            }

            // Exact first-frame horizontal scroll packets emitted by the
            // lifted guest routine FN_doscrolltrans__342f4. These are the
            // non-row-aligned forms that the old row-only regression never
            // exercised: direction 0 uses base-556 pixels for destination
            // x=[556,576), while direction 1 uses base+557 pixels for
            // destination x=[0,19). Each must address the corresponding
            // source edge rather than a retained-frame row or a black clamp.
            const auto check_scroll_edge =
                [&](int32_t base_pixels, float left, float right,
                    const std::vector<std::pair<uint32_t, uint32_t>>& xmap,
                    const char* label) {
                    m2host::SoftCaptureBackend edge_backend(640, 480, {});
                    edge_backend.frame_begin(0.05f, 0.07f, 0.10f);
                    edge_backend.clt_target_upload_rgba(
                        source_base, source.data(), 640, 480);
                    m2host::GuestCltInterpreter edge_interpreter(
                        &edge_backend);
                    edge_interpreter.set_viewport(32, 24, 576, 432, true,
                                                  640, 480);
                    edge_interpreter.set_fb_spans(
                        {{0x2018u, source_base, carve_bytes}});
                    edge_interpreter.set_te_surface_state(
                        0x7u, 0u, 0u, dest_base, 576u, dest_base, 0u, 0u,
                        0u, 0u, 0u, 0u, 1u);
                    std::vector<uint32_t> edge_words(seed_words.begin(),
                                                     seed_words.begin() + 14);
                    edge_words[9] = static_cast<uint32_t>(
                        static_cast<int64_t>(source_base) +
                        static_cast<int64_t>(base_pixels) * 2);
                    const float edge_vertices[][3] = {
                        {left, 0.f, 1.f}, {left, 432.f, 1.f},
                        {right, 0.f, 1.f}, {right, 432.f, 1.f},
                    };
                    for (const auto& vertex : edge_vertices)
                        for (float value : vertex)
                            append_float(edge_words, value);
                    const auto edge_result = edge_interpreter.execute(
                        edge_words.data(), edge_words.size());
                    std::vector<uint32_t> edge_dest(640u * 480u);
                    if (!expect(
                            edge_result.ok &&
                                edge_result.unsupported == 0u &&
                                edge_backend.grab_clt_target_rgba(
                                    dest_base, edge_dest.data(), 640, 480),
                            label))
                        return false;
                    constexpr uint32_t probe_y = 73u;
                    for (const auto& [dest_x, source_x] : xmap) {
                        const uint32_t source_pixel =
                            source[static_cast<size_t>(24u + probe_y) * 640u +
                                   32u + source_x];
                        const uint32_t expected =
                            m2host::clt_db_source_expand_5_to_8(
                                source_pixel >> 3u, true) |
                            (m2host::clt_db_source_expand_5_to_8(
                                 source_pixel >> 11u, true)
                             << 8u) |
                            (m2host::clt_db_source_expand_5_to_8(
                                 source_pixel >> 19u, true)
                             << 16u) |
                            0xff000000u;
                        const uint32_t got =
                            edge_dest[static_cast<size_t>(24u + probe_y) *
                                          640u +
                                      32u + dest_x];
                        if (got != expected)
                            std::fprintf(stderr,
                                         "%s: dest-x=%u source-x=%u got=%#x expected=%#x\n",
                                         label, dest_x, source_x, got,
                                         expected);
                        if (!expect(got == expected, label)) return false;
                    }
                    return true;
                };
            if (!check_scroll_edge(-556, 556.f, 576.f,
                                   {{557u, 1u}, {565u, 9u}, {574u, 18u}},
                                   "lifted scroll direction 0 preserves the "
                                   "left source edge"))
                return false;
            if (!check_scroll_edge(557, 0.f, 19.f,
                                   {{1u, 558u}, {9u, 566u}, {17u, 574u}},
                                   "lifted scroll direction 1 preserves the "
                                   "right source edge"))
                return false;

        }

        // Production bridge regression for the authentic transition seed.
        // The guest binds the third (scratch) plane while the source is two
        // guarded allocations away. A one-neighbour framebuffer-span filter
        // omitted that old color buffer and decoded stale guest RAM, producing
        // the bright/dark two-image flash. Drive the exact 41 guest words and
        // require the three-plane allocation chain to reach the renderer-owned
        // source target.
        {
            constexpr uint32_t old_base = 0x476000u;
            constexpr uint32_t current_base = 0x4f0000u;
            constexpr uint32_t scratch_base = 0x56a000u;
            constexpr uint32_t carve_bytes = 576u * 432u * 2u;
            m2host::SoftCaptureBackend backend(640, 480, {});
            backend.frame_begin(0.05f, 0.07f, 0.10f);
            std::vector<uint32_t> source(640u * 480u, 0xff000000u);
            for (uint32_t y = 0; y < 480u; ++y) {
                for (uint32_t x = 0; x < 640u; ++x) {
                    source[static_cast<size_t>(y) * 640u + x] =
                        ((x & 31u) << 3u) | (((y + 7u) & 31u) << 11u) |
                        (((x + 2u * y) & 31u) << 19u) | 0xff000000u;
                }
            }
            backend.clt_target_upload_rgba(old_base, source.data(), 640, 480);
            m2host::GuestCltBridge bridge(&backend);
            bridge.register_bitmap_buffer(0x101u, old_base, carve_bytes);
            bridge.register_bitmap_buffer(0x102u, current_base, carve_bytes);
            bridge.register_bitmap_buffer(0x103u, scratch_base, carve_bytes);
            bridge.update_graphics_state(0u, 0x000d0006u, 0x103u, 0x201u,
                                         0x103u, 0x103u, 32, 24, 576, 432,
                                         640, 480);
            bridge.update_te_surface_state(
                0x7u, 0u, 0u, scratch_base, 576u, scratch_base, 0u, 0u,
                0u, 0u, 0u, 0u);
            const std::vector<uint32_t> words = {
                0x10008030u, 0x00000000u, 0x1000803cu, 0x00000000u,
                0x10008054u, 0x00000000u, 0x10008068u, 0x00ffffffu,
                0x1000806cu, 0x00ffffffu, 0x10008070u, 0x000000d0u,
                0x10008074u, 0x00000000u, 0x10008038u, 0x00000240u,
                0x10008064u, 0x00000020u, 0x10008008u, 0x0000006fu,
                0x10006408u, 0x00000000u, 0x1000800cu, 0x00000000u,
                0x10008034u, old_base, 0x20080003u,
                0x00000000u, 0x00000000u, 0x00000000u, 0x43d80000u,
                0x44100000u, 0x00000000u, 0x44100000u, 0x43d80000u,
                0x10000014u, 0x00000020u, 0x10008034u, current_base,
                0x10000014u, 0x00000010u,
            };
            const std::vector<uint8_t> bytes = big_endian_bytes(words);
            bridge.enqueue(0u, 0x000d0003u, 0u, 0u,
                           static_cast<uint32_t>(bytes.size()), bytes.data(),
                           nullptr, 0u);
            bridge.pump();
            std::vector<uint32_t> dest(640u * 480u);
            if (!expect(backend.grab_clt_target_rgba(
                            scratch_base, dest.data(), 640, 480),
                        "authentic transition seed reaches scratch"))
                return false;
            const uint32_t probes[][2] = {{11u, 13u}, {287u, 211u},
                                          {560u, 420u}};
            for (const auto& probe : probes) {
                const uint32_t x = 32u + probe[0];
                const uint32_t y = 24u + probe[1];
                const uint32_t p = source[static_cast<size_t>(y) * 640u + x];
                const uint32_t expected =
                    m2host::clt_db_source_expand_5_to_8(p >> 3u, false) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 11u, false)
                     << 8u) |
                    (m2host::clt_db_source_expand_5_to_8(p >> 19u, false)
                     << 16u) |
                    0xff000000u;
                if (!expect(dest[static_cast<size_t>(y) * 640u + x] == expected,
                            "authentic seed reads the two-hop old color plane"))
                    return false;
            }
        }

        // The authentic State 3 framebuffer-composite packets retain SRCEN,
        // arm ALPHA0 discard, and emit shaded perspective vertices with
        // primitive alpha 1.0. DBDISCARDCONTROL runs after TAB but before the
        // secondary DB source participates in destination blending, so the
        // host's implementation texture must preserve that primitive alpha.
        const size_t draws_before_alpha0 = db_backend.draws.size();
        std::vector<uint32_t> db_alpha0_words = {
            0x1000800cu, 0x00000001u,  // DBDISCARDCONTROL.ALPHA0
            0x201d0002u,                // new fan, shaded + perspective
        };
        const float db_alpha0_vertices[][7] = {
            {0.f, 0.f, 0.2f, 0.3f, 0.4f, 1.f, 1.f},
            {16.f, 0.f, 0.4f, 0.5f, 0.6f, 1.f, 1.f},
            {0.f, 16.f, 0.6f, 0.7f, 0.8f, 1.f, 1.f},
        };
        for (const auto& vertex : db_alpha0_vertices)
            for (float value : vertex) append_float(db_alpha0_words, value);
        const m2host::CltExecutionResult db_alpha0_result =
            db_interpreter.execute(db_alpha0_words.data(),
                                   db_alpha0_words.size(), &db_memory);
        if (!expect(db_alpha0_result.ok &&
                        db_alpha0_result.unsupported == 0 &&
                        db_alpha0_result.draws == 1 &&
                        db_backend.draws.size() == draws_before_alpha0 + 1u,
                    "State 3 DB-source ALPHA0 packet reaches the backend"))
            return false;
        const auto& db_alpha0_draw = db_backend.draws.back();
        if (!expect(db_alpha0_draw.textured &&
                        db_alpha0_draw.discard.alpha_zero &&
                        db_alpha0_draw.vertices[0].a == 1.f &&
                        db_alpha0_draw.vertices[1].a == 1.f &&
                        db_alpha0_draw.vertices[2].a == 1.f,
                    "DB-source implementation texture preserves primitive "
                    "alpha for the post-TAB discard"))
            return false;
        const std::vector<uint32_t> db_alpha0_reset = {
            0x1000980cu, 0x00000001u,
        };
        db_interpreter.execute(db_alpha0_reset.data(), db_alpha0_reset.size());

        const size_t initial_uploads = db_backend.uploads.size();
        const std::vector<uint32_t> db_draw_only(db_words.begin() + 8,
                                                 db_words.end());
        const m2host::CltExecutionResult draw_only_result =
            db_interpreter.execute(db_draw_only.data(), db_draw_only.size());
        if (!expect(draw_only_result.ok && draw_only_result.draws == 2 &&
                        db_backend.uploads.size() == initial_uploads,
                    "draw-only list reuses the armed guest DB-source cel"))
            return false;

        const m2host::CltExecutionResult unchanged_result =
            db_interpreter.execute(db_words.data(), db_words.size(), &db_memory);
        if (!expect(unchanged_result.ok && unchanged_result.draws == 2 &&
                        db_backend.uploads.size() == initial_uploads,
                    "unchanged guest DB-source pixels reuse the cached cel"))
            return false;

        auto& replacement = db_memory.ranges.front().bytes;
        for (size_t i = 0; i + 1 < replacement.size(); i += 2) {
            replacement[i] = 0x03u;
            replacement[i + 1] = 0xe0u;  // rgb555 green at the same guest EA
        }
        db_memory.ranges.front().content_hash =
            snapshot_hash_for_test(replacement);
        const m2host::CltExecutionResult replaced_result =
            db_interpreter.execute(db_words.data(), db_words.size(), &db_memory);
        if (!expect(replaced_result.ok && replaced_result.draws == 2 &&
                        db_backend.uploads.size() == initial_uploads + 1u &&
                        db_backend.uploads.back().pixels.front() !=
                            db_backend.uploads.front().pixels.front(),
                    "same-address guest DB-source replacement rebuilds the cel"))
            return false;
        if (!expect(db_backend.freed_cels.size() == 1u &&
                        db_backend.freed_cels.front() == 1u,
                    "DB-source replacement releases the previous cel"))
            return false;
        db_interpreter.release_resources();
        if (!expect(db_backend.freed_cels.size() == 2u &&
                        db_backend.freed_cels.back() == 2u,
                    "interpreter teardown releases the current DB-source cel"))
            return false;

        // Deliberate plausible-wrong control: DBSRCXSTRIDE is the source
        // bitmap's authored pixel width, not a fixed 576-wide display
        // assumption.  A legal 575-pixel source must publish that width and
        // normalize the emitted source coordinates against it.  This vector
        // is intentionally separate from the 576x432 production-shaped
        // fixture above so a hard-coded display width cannot pass both.
        {
            RecordingBackend alternate_backend;
            m2host::GuestCltInterpreter alternate_interpreter(
                &alternate_backend);
            alternate_interpreter.set_viewport(32, 24, 576, 432, true, 640,
                                               480);
            const uint32_t alternate_stride = 575u;
            std::vector<uint32_t> alternate_words = db_words;
            alternate_words[5] = 0x00000a3fu;  // field value 0x23f
            alternate_words[6] = 0u;             // zero source offset
            alternate_words[7] = 0u;

            const size_t alternate_bytes =
                static_cast<size_t>(alternate_stride) * 432u * 2u;
            m2host::CltMemorySnapshot alternate_memory;
            m2host::CltMemoryRange alternate_range;
            alternate_range.guest_address = db_base;
            alternate_range.bytes.assign(alternate_bytes, 0u);
            alternate_memory.ranges.push_back(std::move(alternate_range));

            const m2host::CltExecutionResult alternate_result =
                alternate_interpreter.execute(alternate_words.data(),
                                              alternate_words.size(),
                                              &alternate_memory);
            if (!expect(alternate_result.ok &&
                            alternate_result.unsupported == 0 &&
                            alternate_result.draws == 2 &&
                            alternate_backend.uploads.size() == 1u &&
                            alternate_backend.uploads.front().w ==
                                static_cast<int>(alternate_stride) &&
                            alternate_backend.uploads.front().h == 432,
                        "DB-source stride negative control preserves the authored source width"))
                return false;
            float alternate_min_u = 0.f;
            float alternate_max_u = 0.f;
            bool alternate_have_u = false;
            for (const RecordingBackend::Draw& draw : alternate_backend.draws) {
                for (const m2host::CltVertex& vertex : draw.vertices) {
                    if (!alternate_have_u) {
                        alternate_min_u = vertex.u;
                        alternate_max_u = vertex.u;
                        alternate_have_u = true;
                    } else {
                        alternate_min_u = std::min(alternate_min_u, vertex.u);
                        alternate_max_u = std::max(alternate_max_u, vertex.u);
                    }
                }
            }
            if (!expect(alternate_have_u &&
                            std::abs(alternate_min_u) < 0.0001f &&
                            std::abs(alternate_max_u -
                                     (576.f / static_cast<float>(alternate_stride))) <
                                0.0001f,
                        "DB-source stride negative control emits the final "
                        "linear overflow coordinate"))
                return false;
            alternate_interpreter.release_resources();
        }
    }

    // Portfolio's BMTYPE_32 DB-source selector is a four-byte color/alpha/DSB
    // source form. Missing source bytes must still reject instead of falling
    // through to a flat primitive or reading the source as two-byte RGB555.
    {
        RecordingBackend db32_backend;
        m2host::GuestCltInterpreter db32_interpreter(&db32_backend);
        const uint32_t db32_base = 0x2000u;
        std::vector<uint32_t> db32_words = {
            0x10009008u, 0x00000020u,  // DBUSERCONTROL set SRCEN
            0x10008030u, 0x00000001u,  // DBSRCCNTL.SRC32BPP
            0x10008034u, db32_base,
            0x10008038u, 0x00000240u,
            0x200c0003u,  // new, perspective-only, four-vertex strip
        };
        const float db32_vertices[][3] = {
            {0.f, 0.f, 1.f},
            {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f},
            {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : db32_vertices)
            for (float value : vertex) append_float(db32_words, value);

        const m2host::CltExecutionResult db32_result = db32_interpreter.execute(
            db32_words.data(), db32_words.size());
        if (!expect(db32_result.ok && db32_result.unsupported == 1 &&
                        db32_result.draws == 0 && db32_backend.draws.empty() &&
                        db32_backend.uploads.empty(),
                    "DB-source BMTYPE_32 with missing bytes fails closed before "
                    "materialization"))
            return false;
    }

    const std::vector<uint32_t> z_enabled = {
        0x10008008u, 0x00000400u,  // ZBUFFEN mode
    };
    const m2host::CltExecutionResult z_enabled_result =
        interpreter.execute(z_enabled.data(), z_enabled.size());
    if (!expect(z_enabled_result.ok && z_enabled_result.unsupported == 0,
                "Z-buffer control list parses")) return false;
    const m2host::CltExecutionResult accepted_z_result = interpreter.execute(
        words.data(), words.size());
    if (!expect(accepted_z_result.ok && accepted_z_result.unsupported == 0 &&
                    accepted_z_result.draws == 1 &&
                    backend.draws.back().depth.test_enabled,
                "Z-buffer enabled draw forwards depth testing state")) return false;
    // ZBUFEN and ZOUTEN together (the authored `0x600` form): the Z write is
    // forwarded from ZOUTEN independently of the Z test.
    const std::vector<uint32_t> z_write_enabled = {
        0x10009008u, 0x00000600u,  // ZBUFFEN/ZOUTEN setup
    };
    const m2host::CltExecutionResult z_write_result =
        interpreter.execute(z_write_enabled.data(), z_write_enabled.size());
    if (!expect(z_write_result.ok && z_write_result.unsupported == 0,
                "Z-buffer write-enable control list parses")) return false;
    const m2host::CltExecutionResult accepted_write_result =
        interpreter.execute(words.data(), words.size());
    if (!expect(accepted_write_result.ok && accepted_write_result.draws == 1 &&
                    backend.draws.back().depth.write_enabled,
                "Z-buffer write enable forwards the Z write state"))
        return false;

    // Deliberate plausible-wrong control: ZOUTEN is independent of ZBUFEN.
    // Clearing only the output bit must retain depth testing while suppressing
    // the color-masked depth write pass.
    const std::vector<uint32_t> z_write_disabled = {
        0x10009808u, 0x00000200u,  // clear ZOUTEN, retain ZBUFEN
    };
    const m2host::CltExecutionResult z_write_disabled_result =
        interpreter.execute(z_write_disabled.data(), z_write_disabled.size());
    if (!expect(z_write_disabled_result.ok &&
                    z_write_disabled_result.unsupported == 0,
                "Z-buffer write-disable control list parses"))
        return false;
    const m2host::CltExecutionResult rejected_write_result =
        interpreter.execute(words.data(), words.size());
    if (!expect(rejected_write_result.ok && rejected_write_result.draws == 1 &&
                    backend.draws.back().depth.test_enabled &&
                    !backend.draws.back().depth.write_enabled,
                "Z-buffer write disable suppresses the Z write state"))
        return false;

    const std::vector<uint32_t> z_disabled = {
        0x10009808u, 0x00000400u,  // clear ZBUFFEN mode
    };
    const m2host::CltExecutionResult z_disabled_result =
        interpreter.execute(z_disabled.data(), z_disabled.size());
    if (!expect(z_disabled_result.ok && z_disabled_result.unsupported == 0,
                "Z-buffer control clear parses")) return false;

    const std::vector<uint32_t> observed_z_clear = {
        0x10009008u, 0x00000600u,  // observed ZBUFFEN/ZOUTEN setup
        0x10008040u, 0x0000003fu,  // force all Z-clear results
        0x1000801cu, 640u,
        0x10008020u, 480u,
        0x200c0003u,  // new, perspective-only, four-vertex strip
        0u, 0u, 0u,
        0u, 0u, 0u,
        0u, 0u, 0u,
        0u, 0u, 0u,
    };
    const m2host::CltExecutionResult observed_result = interpreter.execute(
        observed_z_clear.data(), observed_z_clear.size());
    if (!expect(observed_result.ok && observed_result.vertex_batches == 1 &&
                    observed_result.draws == 2,
                "observed perspective-only four-vertex list parses")) return false;
    if (!expect(backend.draws.back().vertices.size() == 3 &&
                    !backend.draws.back().strip,
                "observed list uses setup-engine triangle vertices"))
        return false;

    std::vector<uint32_t> shaded_z_clear = {
        0x200d0003u,  // new, shaded perspective four-vertex strip
    };
    const float clear_vertices[][7] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
        {0.f, 480.f, 0.f, 0.f, 0.f, 0.f, 0.f},
        {640.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
        {640.f, 480.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    };
    for (const auto& vertex : clear_vertices)
        for (float value : vertex) append_float(shaded_z_clear, value);
    const m2host::CltExecutionResult shaded_clear_result = interpreter.execute(
        shaded_z_clear.data(), shaded_z_clear.size());
    if (!expect(shaded_clear_result.ok &&
                    shaded_clear_result.unsupported == 0 &&
                    shaded_clear_result.draws == 2,
                "source-shaped shaded Z-clear reaches the backend"))
        return false;

    // A varying shaded primitive is scene geometry, now supported with depth state.
    shaded_z_clear[5] = std::bit_cast<uint32_t>(1.f);
    const m2host::CltExecutionResult varying_z_result = interpreter.execute(
        shaded_z_clear.data(), shaded_z_clear.size());
    if (!expect(varying_z_result.ok && varying_z_result.unsupported == 0 &&
                    varying_z_result.draws == 2 &&
                    backend.draws.back().depth.test_enabled,
                "depth-enabled varying shaded scene geometry is forwarded"))
        return false;

    std::vector<uint32_t> textured = {
        0x10006408u, 0x00002000u,  // TxtAddrCntl = TEXTUREENABLE
        0x10006410u, 0x00009200u,  // TxtTabCntl = texture color/alpha enabled
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    for (int i = 0; i < 3; ++i) {
        append_float(textured, static_cast<float>(i));
        append_float(textured, static_cast<float>(i));
        append_float(textured, 0.f);
        append_float(textured, 0.f);
    }
    const m2host::CltExecutionResult texture_result = interpreter.execute(
        textured.data(), textured.size());
    if (!expect(texture_result.ok && texture_result.unsupported == 1 &&
                    texture_result.draws == 0,
                "texture payload is parsed but fails closed")) return false;
    const std::vector<uint32_t> reset_tex_enable = {
        0x10006408u, 0x00000000u,
        0x10006410u, 0x00000000u,
    };
    interpreter.execute(reset_tex_enable.data(), reset_tex_enable.size());

    RecordingBackend texture_backend;
    m2host::GuestCltInterpreter texture_interpreter(&texture_backend);
    const std::vector<uint32_t> texture_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x10006434u, 0x04080408u,
        0x10006438u, 0x04080408u,
        0x1000643cu, 0x00000408u,
        // Stale prior-draw state is invalid for indexed color.
        0x1000640cu, 0x00004900u,
        0x10006404u, 0x00000400u,  // compressed indexed texture load
        0x10006414u, 0x00000000u,
        0x10006424u, 0x00000200u,
        0x10006428u, 0x00000003u,
        0x1000642cu, 0x00180018u,  // 3 texels across, 8 bits each
        0x10000014u, 0x00000001u,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x0000aadb,
        0x1000642cu, 0x00020002u,  // UMAX=2, VMAX=0
        0x10006430u, 0x03ff03ffu,
        0x1000801cu, 640u,
        0x10008020u, 480u,
        0x200a0002u,
    };
    const float texture_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {3.f, 0.f, 3.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    std::vector<uint32_t> complete_texture_words = texture_words;
    for (const auto& vertex : texture_vertices)
        for (float value : vertex) append_float(complete_texture_words, value);

    m2host::CltMemorySnapshot texture_memory;
    m2host::CltMemoryRange overlapping_range;
    overlapping_range.guest_address = 0x80u;
    overlapping_range.bytes.assign(0x100u, 0u);
    texture_memory.ranges.push_back(std::move(overlapping_range));
    m2host::CltMemoryRange pip_range;
    pip_range.guest_address = 0x100u;
    pip_range.bytes = {0x7f, 0x11, 0x22, 0x33,
                       0x01, 0x44, 0x55, 0x66,
                       0x1f, 0x70, 0x80, 0x90};
    texture_memory.ranges.push_back(std::move(pip_range));
    m2host::CltMemoryRange compressed_range;
    compressed_range.guest_address = 0x200u;
    compressed_range.bytes = {0x22u, 0x00u, 0x01u, 0x00u};
    texture_memory.ranges.push_back(std::move(compressed_range));
    const m2host::CltExecutionResult decoded_texture =
        texture_interpreter.execute(complete_texture_words.data(),
                                    complete_texture_words.size(),
                                    &texture_memory);
    if (!expect(decoded_texture.ok && decoded_texture.unsupported == 0 &&
                    decoded_texture.draws == 1 &&
                    texture_backend.uploads.size() == 1,
                "indexed PIP and compressed texture load reach backend"))
        return false;
    if (!expect(texture_backend.uploads[0].w == 3 &&
                    texture_backend.uploads[0].h == 1 &&
                    texture_backend.uploads[0].pixels[0] == 0xff332211u &&
                    texture_backend.uploads[0].pixels[1] == 0x03665544u,
                "stale load-time PIP state defers until the indexed draw-state rebind"))
        return false;
    if (!expect(texture_backend.draws[0].textured &&
                    texture_backend.draws[0].vertices[1].u == 1.f,
                "guest pixel UVs are normalized after texture load")) return false;

    std::vector<uint32_t> perspective_texture = {
        0x10004000u, 0x00000000u,  // perspective-on state
        0x200e0002u,                // new, perspective textured strip
    };
    const float perspective_vertices[][5] = {
        {0.f, 0.f, 1.f, 0.f, 0.f},
        {3.f, 0.f, 1.f, 3.f, 0.f},
        {0.f, 1.f, 1.f, 0.f, 1.f},
    };
    for (const auto& vertex : perspective_vertices)
        for (float value : vertex) append_float(perspective_texture, value);
    const m2host::CltExecutionResult perspective_on_result =
        texture_interpreter.execute(perspective_texture.data(),
                                    perspective_texture.size(),
                                    &texture_memory);
    if (!expect(perspective_on_result.ok &&
                    perspective_on_result.unsupported == 0 &&
                    perspective_on_result.draws == 1 &&
                    texture_backend.draws.back().vertices[1].one_over_w == 1.f,
                "perspective-on texture draw parses and passes 1/w"))
        return false;
    perspective_texture[1] = 0x00000004u;  // observed ESCNTL off bit
    const m2host::CltExecutionResult perspective_off_result =
        texture_interpreter.execute(perspective_texture.data(),
                                    perspective_texture.size(),
                                    &texture_memory);
    if (!expect(perspective_off_result.ok &&
                    perspective_off_result.unsupported == 0 &&
                    perspective_off_result.draws == 1,
                "observed perspective-off texture draws")) return false;

    RecordingBackend transparent_backend;
    m2host::GuestCltInterpreter transparent_interpreter(&transparent_backend);
    const std::vector<uint32_t> transparent_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x10006434u, 0x04080b70u,  // transparent source 0 retains A7/SSB metadata
        0x10006438u, 0x04080408u,
        0x1000643cu, 0x00000408u,  // 8-bit indexed expanded format
        0x10006404u, 0x00000400u,  // compressed texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x1000642cu, 0x00180018u,  // 3 texels across, 8 bits each
        0x10006440u, 0x00000001u,  // transparent run uses PIP index 1
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x10000014u, 0x00000001u,
        0x200a0002u,
    };
    const float transparent_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {3.f, 0.f, 3.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    std::vector<uint32_t> complete_transparent_words = transparent_words;
    for (const auto& vertex : transparent_vertices)
        for (float value : vertex)
            append_float(complete_transparent_words, value);

    m2host::CltMemorySnapshot transparent_memory;
    m2host::CltMemoryRange transparent_pip_range;
    transparent_pip_range.guest_address = 0x100u;
    transparent_pip_range.bytes = {
        0x7f, 0x11, 0x22, 0x33,
        0x3f, 0x44, 0x55, 0x66,
        0x1f, 0x77, 0x88, 0x99,
    };
    transparent_memory.ranges.push_back(std::move(transparent_pip_range));
    m2host::CltMemoryRange transparent_source_range;
    transparent_source_range.guest_address = 0x200u;
    transparent_source_range.bytes = {0x02u, 0x00u, 0x00u, 0x00u};
    transparent_memory.ranges.push_back(std::move(transparent_source_range));
    const m2host::CltExecutionResult transparent_result =
        transparent_interpreter.execute(complete_transparent_words.data(),
                                        complete_transparent_words.size(),
                                        &transparent_memory);
    if (!expect(transparent_result.ok && transparent_result.unsupported == 0 &&
                    transparent_result.draws == 1 &&
                    transparent_backend.uploads.size() == 1,
                "compressed transparent run reaches backend"))
        return false;
    if (!expect(transparent_backend.uploads[0].w == 3 &&
                    transparent_backend.uploads[0].h == 1 &&
                    transparent_backend.uploads[0].pixels[0] == 0x7f665544u &&
                    transparent_backend.uploads[0].pixels[1] == 0x7f665544u &&
                    transparent_backend.uploads[0].pixels[2] == 0x7f665544u,
                "compressed transparent run repeats its PIP constant"))
        return false;

    RecordingBackend raw_texture_backend;
    m2host::GuestCltInterpreter raw_texture_interpreter(&raw_texture_backend);
    const std::vector<uint32_t> raw_texture_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // 6-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x10006430u, 0x00020000u,  // UVMASK U=2, V=0 -> 3x1 repeat periods
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    const float raw_texture_vertices[][4] = {
        {0.f, 0.f, 0.f, 0.f},
        {3.f, 0.f, 3.f, 0.f},
        {0.f, 1.f, 0.f, 1.f},
    };
    std::vector<uint32_t> complete_raw_texture_words = raw_texture_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_raw_texture_words, value);

    m2host::CltMemorySnapshot raw_texture_memory;
    m2host::CltMemoryRange raw_pip_range;
    raw_pip_range.guest_address = 0x100u;
    raw_pip_range.bytes = {
        0xff, 0x11, 0x22, 0x33,
        0x3f, 0x44, 0x55, 0x66,
        0x1f, 0x77, 0x88, 0x99,
    };
    raw_texture_memory.ranges.push_back(std::move(raw_pip_range));
    m2host::CltMemoryRange raw_texel_range;
    raw_texel_range.guest_address = 0x200u;
    raw_texel_range.bytes = {0x00u, 0x10u, 0x80u};
    raw_texture_memory.ranges.push_back(std::move(raw_texel_range));
    const m2host::CltExecutionResult raw_texture_result =
        raw_texture_interpreter.execute(complete_raw_texture_words.data(),
                                        complete_raw_texture_words.size(),
                                        &raw_texture_memory);
    if (!expect(raw_texture_result.ok && raw_texture_result.unsupported == 0 &&
                    raw_texture_result.draws == 1 &&
                    raw_texture_backend.uploads.size() == 1,
                "6-bit indexed MMDMA texture reaches backend"))
        return false;
    if (!expect(raw_texture_backend.uploads[0].w == 3 &&
                    raw_texture_backend.uploads[0].h == 1 &&
                    raw_texture_backend.uploads[0].pixels[0] == 0xff332211u &&
                    raw_texture_backend.uploads[0].pixels[1] == 0x7f665544u &&
                    raw_texture_backend.uploads[0].pixels[2] == 0x3f998877u,
                "MMDMA 6-bit texels are MSB-first and PIP-expanded"))
        return false;
    if (!expect(raw_texture_backend.draws.size() == 1 &&
                    raw_texture_backend.draws[0].sampling.wrap_u &&
                    raw_texture_backend.draws[0].sampling.wrap_v,
                "asymmetric UVMASK fields reach the matching repeat axes"))
        return false;

    const auto textured_discard_words = [&](uint32_t control) {
        std::vector<uint32_t> words = {
            0x1000800cu, control,
            0x200a0002u,
        };
        for (const auto& vertex : raw_texture_vertices)
            for (float value : vertex) append_float(words, value);
        return words;
    };
    const std::vector<uint32_t> ssb_discard_words =
        textured_discard_words(0x4u);
    const m2host::CltExecutionResult ssb_discard_result =
        raw_texture_interpreter.execute(ssb_discard_words.data(),
                                        ssb_discard_words.size(),
                                        &raw_texture_memory);
    if (!expect(ssb_discard_result.ok &&
                    ssb_discard_result.unsupported == 0 &&
                    ssb_discard_result.draws == 1 &&
                    raw_texture_backend.uploads.size() == 2 &&
                    raw_texture_backend.draws.back().discard.ssb_zero &&
                    raw_texture_backend.draws.back().discard.ssb_mask == 2,
                "observed SSB0 discard publishes a texture sidecar"))
        return false;
    if (!expect(raw_texture_backend.uploads[1].pixels[0] == 0xffffffffu &&
                    raw_texture_backend.uploads[1].pixels[1] == 0u &&
                    raw_texture_backend.uploads[1].pixels[2] == 0u,
                "PIP SSB remains independent from texture alpha"))
        return false;

    const std::vector<uint32_t> alpha_discard_words =
        textured_discard_words(0x1u);
    const m2host::CltExecutionResult alpha_discard_result =
        raw_texture_interpreter.execute(alpha_discard_words.data(),
                                        alpha_discard_words.size(),
                                        &raw_texture_memory);
    if (!expect(alpha_discard_result.ok &&
                    alpha_discard_result.unsupported == 0 &&
                    alpha_discard_result.draws == 1 &&
                    raw_texture_backend.draws.back().discard.alpha_zero &&
                    !raw_texture_backend.draws.back().discard.ssb_zero,
                "observed ALPHA0 discard reaches the backend"))
        return false;

    const std::vector<uint32_t> rgb_discard_words =
        textured_discard_words(0x2u);
    const m2host::CltExecutionResult rgb_discard_result =
        raw_texture_interpreter.execute(rgb_discard_words.data(),
                                        rgb_discard_words.size(),
                                        &raw_texture_memory);
    if (!expect(rgb_discard_result.ok && rgb_discard_result.unsupported == 0 &&
                    rgb_discard_result.draws == 1 &&
                    raw_texture_backend.draws.back().discard.rgb_zero &&
                    !raw_texture_backend.draws.back().discard.alpha_zero,
                "SDK-defined RGB0 discard reaches the backend"))
        return false;

    const std::vector<uint32_t> discard_reset_words = {
        0x1000800cu, 0x00000000u,
    };
    const m2host::CltExecutionResult textured_discard_reset =
        raw_texture_interpreter.execute(discard_reset_words.data(),
                                        discard_reset_words.size());
    if (!expect(textured_discard_reset.ok,
                "textured discard state resets for later tests"))
        return false;

    RecordingBackend raw_8bit_backend;
    m2host::GuestCltInterpreter raw_8bit_interpreter(&raw_8bit_backend);
    const std::vector<uint32_t> raw_8bit_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000408u,  // 8-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_raw_8bit_words = raw_8bit_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_raw_8bit_words, value);

    m2host::CltMemorySnapshot raw_8bit_memory;
    m2host::CltMemoryRange raw_8bit_pip_range;
    raw_8bit_pip_range.guest_address = 0x100u;
    raw_8bit_pip_range.bytes = {
        0x7f, 0x11, 0x22, 0x33,
        0x3f, 0x44, 0x55, 0x66,
        0x1f, 0x77, 0x88, 0x99,
    };
    raw_8bit_memory.ranges.push_back(std::move(raw_8bit_pip_range));
    m2host::CltMemoryRange raw_8bit_texel_range;
    raw_8bit_texel_range.guest_address = 0x200u;
    raw_8bit_texel_range.bytes = {0x00u, 0x01u, 0x02u};
    raw_8bit_memory.ranges.push_back(std::move(raw_8bit_texel_range));
    const m2host::CltExecutionResult raw_8bit_result =
        raw_8bit_interpreter.execute(complete_raw_8bit_words.data(),
                                     complete_raw_8bit_words.size(),
                                     &raw_8bit_memory);
    if (!expect(raw_8bit_result.ok && raw_8bit_result.unsupported == 0 &&
                    raw_8bit_result.draws == 1 &&
                    raw_8bit_backend.uploads.size() == 1,
                "8-bit indexed MMDMA texture reaches backend"))
        return false;
    if (!expect(raw_8bit_backend.uploads[0].pixels[0] == 0xff332211u &&
                    raw_8bit_backend.uploads[0].pixels[1] == 0x7f665544u &&
                    raw_8bit_backend.uploads[0].pixels[2] == 0x3f998877u,
                "MMDMA 8-bit texels are PIP-expanded"))
        return false;

    // TXTPIPCNTL is draw/use state. The interpreter still materializes the
    // MMDMA geometry at the post-load TXTEXPTYPE trigger, but an indexed cel
    // must be rebound if the draw later selects a different PIP offset.
    RecordingBackend pip_timing_backend;
    m2host::GuestCltInterpreter pip_timing_interpreter(&pip_timing_backend);
    const std::vector<uint32_t> pip_timing_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000408u,  // 8-bit indexed expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
        0x1000640cu, 0x00009200u,  // PIP color/alpha/SSB for materialization
        0x1000643cu, 0x00000408u,  // host materialization trigger
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009201u,  // draw selects PIP offset 1
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_pip_timing_words = pip_timing_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_pip_timing_words, value);

    m2host::CltMemorySnapshot pip_timing_memory;
    m2host::CltMemoryRange pip_timing_pip_range;
    pip_timing_pip_range.guest_address = 0x100u;
    pip_timing_pip_range.bytes = {
        0x7f, 0x11, 0x22, 0x33,
        0x3f, 0x44, 0x55, 0x66,
        0x1f, 0x77, 0x88, 0x99,
    };
    pip_timing_memory.ranges.push_back(std::move(pip_timing_pip_range));
    m2host::CltMemoryRange pip_timing_texel_range;
    pip_timing_texel_range.guest_address = 0x200u;
    pip_timing_texel_range.bytes = {0x00u, 0x01u};
    pip_timing_memory.ranges.push_back(std::move(pip_timing_texel_range));
    const m2host::CltExecutionResult pip_timing_result =
        pip_timing_interpreter.execute(complete_pip_timing_words.data(),
                                       complete_pip_timing_words.size(),
                                       &pip_timing_memory);
    if (!expect(pip_timing_result.ok && pip_timing_result.unsupported == 0 &&
                    pip_timing_result.draws == 1 &&
                    pip_timing_backend.uploads.size() == 2,
                "indexed MMDMA palette rebind reaches draw state"))
        return false;
    if (!expect(pip_timing_backend.uploads[0].pixels[0] == 0xff332211u &&
                    pip_timing_backend.uploads[1].pixels[0] == 0x7f665544u &&
                    pip_timing_backend.uploads[1].pixels[1] == 0x3f998877u,
                "indexed MMDMA draw uses its TXTPIPCNTL offset"))
        return false;

    // Rewriting byte-identical PIP contents changes no hardware-visible
    // palette state and must not invalidate/re-upload the resident cel.
    std::vector<uint32_t> repeated_pip_words = {
        0x10006404u, 0x00000200u,
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000408u,
        0x10006414u, 0x00000000u,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009201u,
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(repeated_pip_words, value);
    const m2host::CltExecutionResult repeated_pip_result =
        pip_timing_interpreter.execute(repeated_pip_words.data(),
                                       repeated_pip_words.size(),
                                       &pip_timing_memory);
    if (!expect(repeated_pip_result.ok &&
                    repeated_pip_result.unsupported == 0 &&
                    repeated_pip_result.draws == 1 &&
                    pip_timing_backend.uploads.size() == 2,
                "byte-identical PIP rewrite preserves resident cel"))
        return false;

    // The SDK MMDMA snippet writes TXTEXPTYPE before TLD, then publishes its
    // LOD/UVMAX use state. A following PIP snippet begins with TXTLDCNTL and
    // temporarily sets TXTLODBASE0 to TEPIPRAM (0x46000). The texture must be
    // materialized at that next-load boundary, before PIP registers can be
    // mistaken for its use state. Pre-fix, the later 0x404 expansion write
    // tries the still-pending 0x406 blob at 0x46000 and increments unsupported.
    RecordingBackend interleaved_pip_backend;
    m2host::GuestCltInterpreter interleaved_pip_interpreter(
        &interleaved_pip_backend);
    const std::vector<uint32_t> interleaved_pip_words = {
        0x10006404u, 0x00000200u,  // initial PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000406u,  // SDK texture expansion before TLD
        0x10006404u, 0x00000100u,  // MMDMA texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,
        0x10000014u, 0x00000001u,
        0x10006414u, 0x00000000u,  // texture use state
        0x1000642cu, 0x00010000u,
        0x10006430u, 0x00000000u,
        0x1000640cu, 0x00009200u,
        0x10006404u, 0x00000200u,  // next load begins: PIP
        0x10006424u, 0x00000300u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000404u,  // following texture expansion
        0x1000643cu, 0x00000406u,  // restore this texture's draw format
        0x10006414u, 0x00000000u,
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_interleaved_pip_words =
        interleaved_pip_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_interleaved_pip_words, value);
    m2host::CltMemorySnapshot interleaved_pip_memory;
    m2host::CltMemoryRange interleaved_pip_initial_range;
    interleaved_pip_initial_range.guest_address = 0x100u;
    interleaved_pip_initial_range.bytes = {
        0xff, 0x11, 0x22, 0x33,
        0xff, 0x44, 0x55, 0x66,
        0xff, 0x77, 0x88, 0x99,
    };
    interleaved_pip_memory.ranges.push_back(
        std::move(interleaved_pip_initial_range));
    m2host::CltMemoryRange interleaved_pip_texel_range;
    interleaved_pip_texel_range.guest_address = 0x200u;
    interleaved_pip_texel_range.bytes = {0x00u, 0x10u};
    interleaved_pip_memory.ranges.push_back(
        std::move(interleaved_pip_texel_range));
    m2host::CltMemoryRange interleaved_pip_reload_range;
    interleaved_pip_reload_range.guest_address = 0x300u;
    interleaved_pip_reload_range.bytes = {
        0xff, 0xaa, 0xbb, 0xcc,
        0xff, 0x10, 0x20, 0x30,
        0xff, 0x40, 0x50, 0x60,
    };
    interleaved_pip_memory.ranges.push_back(
        std::move(interleaved_pip_reload_range));
    const m2host::CltExecutionResult interleaved_pip_result =
        interleaved_pip_interpreter.execute(
            complete_interleaved_pip_words.data(),
            complete_interleaved_pip_words.size(), &interleaved_pip_memory);
    if (!expect(interleaved_pip_result.ok &&
                    interleaved_pip_result.unsupported == 0 &&
                    interleaved_pip_result.draws == 1 &&
                    interleaved_pip_backend.uploads.size() == 2,
                "pending MMDMA texture closes before an interleaved PIP load"))
        return false;
    if (!expect(interleaved_pip_backend.uploads[1].pixels[0] == 0xffccbbaau &&
                    interleaved_pip_backend.uploads[1].pixels[1] == 0xff302010u,
                "interleaved PIP reload rebinds the completed texture"))
        return false;

    // State 3 can leave a monotonic LOD quad whose later bases describe an
    // earlier, larger resident. The next load boundary must size the current
    // texture from the 512-byte DMA blob: LOD offsets outside that resident
    // cannot define its footprint. Before this boundary check, LOD1=0x3000
    // inflated the 4-bit texture to 24,576 texels and rejected the packet.
    RecordingBackend stale_lod_backend;
    m2host::GuestCltInterpreter stale_lod_interpreter(&stale_lod_backend);
    const std::vector<uint32_t> stale_lod_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000040u,
        0x10000014u, 0x00000001u,
        0x10006404u, 0x00000100u,  // 512-byte MMDMA texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000200u,
        0x10000014u, 0x00000001u,
        0x10006414u, 0x00000000u,  // current resident begins at TRAM 0
        0x10006418u, 0x00003000u,  // stale bases outside its 0x200 bytes
        0x1000641cu, 0x00003c00u,
        0x10006420u, 0x00003f00u,
        0x1000642cu, 0x007f00ffu,
        0x1000640cu, 0x00009200u,
        0x1000643cu, 0x00000404u,
        0x10006404u, 0x00000200u,  // next load closes the resident
    };
    m2host::CltMemorySnapshot stale_lod_memory;
    m2host::CltMemoryRange stale_lod_pip_range;
    stale_lod_pip_range.guest_address = 0x100u;
    stale_lod_pip_range.bytes.resize(0x40u);
    for (size_t i = 0; i < stale_lod_pip_range.bytes.size(); i += 4u) {
        stale_lod_pip_range.bytes[i + 0u] = 0xffu;
        stale_lod_pip_range.bytes[i + 1u] = static_cast<uint8_t>(i);
        stale_lod_pip_range.bytes[i + 2u] = static_cast<uint8_t>(i + 1u);
        stale_lod_pip_range.bytes[i + 3u] = static_cast<uint8_t>(i + 2u);
    }
    stale_lod_memory.ranges.push_back(std::move(stale_lod_pip_range));
    m2host::CltMemoryRange stale_lod_texel_range;
    stale_lod_texel_range.guest_address = 0x200u;
    stale_lod_texel_range.bytes.resize(0x200u);
    for (size_t i = 0; i < stale_lod_texel_range.bytes.size(); ++i)
        stale_lod_texel_range.bytes[i] = static_cast<uint8_t>(i);
    stale_lod_memory.ranges.push_back(std::move(stale_lod_texel_range));
    const m2host::CltExecutionResult stale_lod_result =
        stale_lod_interpreter.execute(stale_lod_words.data(),
                                      stale_lod_words.size(),
                                      &stale_lod_memory);
    if (!expect(stale_lod_result.ok && stale_lod_result.unsupported == 0 &&
                    stale_lod_backend.uploads.size() == 1,
                "out-of-resident stale LOD bases do not inflate MMDMA shape"))
        return false;
    if (!expect(stale_lod_backend.uploads.back().w == 32u &&
                    stale_lod_backend.uploads.back().h == 32u,
                "512-byte 4-bit resident retains its 1024-texel footprint"))
        return false;

    RecordingBackend raw_4bit_backend;
    m2host::GuestCltInterpreter raw_4bit_interpreter(&raw_4bit_backend);
    const std::vector<uint32_t> raw_4bit_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00001605u,  // stale prior x1RGB555+SSB format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00020000u,  // UMAX=2, VMAX=0 -> 3x1
        0x1000643cu, 0x00000404u,  // State 3's format follows the trigger
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_raw_4bit_words = raw_4bit_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_raw_4bit_words, value);

    m2host::CltMemorySnapshot raw_4bit_memory;
    m2host::CltMemoryRange raw_4bit_pip_range;
    raw_4bit_pip_range.guest_address = 0x100u;
    raw_4bit_pip_range.bytes = {
        0x7f, 0x11, 0x22, 0x33,
        0x3f, 0x44, 0x55, 0x66,
        0x1f, 0x77, 0x88, 0x99,
    };
    raw_4bit_memory.ranges.push_back(std::move(raw_4bit_pip_range));
    m2host::CltMemoryRange raw_4bit_texel_range;
    raw_4bit_texel_range.guest_address = 0x200u;
    raw_4bit_texel_range.bytes = {0x01u, 0x20u, 0x00u};
    raw_4bit_memory.ranges.push_back(std::move(raw_4bit_texel_range));
    const m2host::CltExecutionResult raw_4bit_result =
        raw_4bit_interpreter.execute(complete_raw_4bit_words.data(),
                                     complete_raw_4bit_words.size(),
                                     &raw_4bit_memory);
    if (!expect(raw_4bit_result.ok && raw_4bit_result.unsupported == 0 &&
                    raw_4bit_result.draws == 1 &&
                    raw_4bit_backend.uploads.size() == 2,
                "4-bit indexed MMDMA texture reaches backend"))
        return false;
    if (!expect(raw_4bit_backend.uploads[1].w == 3 &&
                    raw_4bit_backend.uploads[1].h == 1 &&
                    raw_4bit_backend.uploads[1].pixels[0] == 0xff332211u &&
                    raw_4bit_backend.uploads[1].pixels[1] == 0x7f665544u &&
                    raw_4bit_backend.uploads[1].pixels[2] == 0x3f998877u,
                "MMDMA 4-bit texels are MSB-first and PIP-expanded"))
        return false;

    RecordingBackend mmdma_555_backend;
    m2host::GuestCltInterpreter mmdma_555_interpreter(&mmdma_555_backend);
    const std::vector<uint32_t> mmdma_555_words = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000004u,  // two 16-bit texels
        0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
        0x1000643cu, 0x00001605u,  // literal RGB555 + SSB
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x10000014u, 0x00000001u,  // TLD
        0x200a0002u,                // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_mmdma_555_words = mmdma_555_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_mmdma_555_words, value);

    m2host::CltMemorySnapshot mmdma_555_memory;
    m2host::CltMemoryRange mmdma_555_range;
    mmdma_555_range.guest_address = 0x200u;
    mmdma_555_range.bytes = {0xfcu, 0x00u, 0x03u, 0xe0u};
    mmdma_555_memory.ranges.push_back(std::move(mmdma_555_range));
    const m2host::CltExecutionResult mmdma_555_result =
        mmdma_555_interpreter.execute(complete_mmdma_555_words.data(),
                                      complete_mmdma_555_words.size(),
                                      &mmdma_555_memory);
    if (!expect(mmdma_555_result.ok && mmdma_555_result.unsupported == 0 &&
                    mmdma_555_result.draws == 1 &&
                    mmdma_555_backend.uploads.size() == 1,
                "MMDMA RGB555 texture reaches backend"))
        return false;
    if (!expect(mmdma_555_backend.uploads[0].w == 2 &&
                    mmdma_555_backend.uploads[0].h == 1 &&
                    mmdma_555_backend.uploads[0].pixels[0] == 0xff0000ffu &&
                    mmdma_555_backend.uploads[0].pixels[1] == 0x0000ff00u,
                "MMDMA RGB555 texels honor SSB alpha"))
        return false;

    // dorotatetrans reloads the completed display buffer through MMDMA as
    // thirty-six 576x12 literal RGB555+SSB strips. The renderer-owned target
    // is authoritative for a registered display carve; guest RAM can still
    // contain the last MPEG frame at the same address. Prove both sides of
    // that boundary with deliberately contradictory colors.
    {
        constexpr uint32_t display_base = 0x580000u;
        constexpr uint32_t strip_bytes = 576u * 12u * 2u;
        RecordingBackend rotate_backend;
        rotate_backend.target_readback_enabled = true;
        rotate_backend.target_readback_base = display_base;
        rotate_backend.target_readback_color = 0xff00ff00u;  // live green
        m2host::GuestCltInterpreter rotate_interpreter(&rotate_backend);
        rotate_interpreter.set_fb_spans(
            {{1u, display_base, 576u * 432u * 2u}});

        std::vector<uint32_t> rotate_words = {
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load
            0x10006424u, display_base,
            0x10006414u, 0x00000000u,
            0x10006428u, strip_bytes,
            0x1000642cu, 0x023f000bu,  // UMAX=575, VMAX=11
            0x1000643cu, 0x00001605u,  // literal RGB555 + SSB
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00004900u,
            0x10006410u, 0x00028219u,
            0x10000014u, 0x00000001u,
            0x200a0002u,
        };
        for (const auto& vertex : raw_texture_vertices)
            for (float value : vertex) append_float(rotate_words, value);

        m2host::CltMemorySnapshot stale_movie_memory;
        m2host::CltMemoryRange stale_movie_range;
        stale_movie_range.guest_address = display_base;
        stale_movie_range.bytes.resize(strip_bytes);
        for (size_t i = 0; i < stale_movie_range.bytes.size(); i += 2u) {
            stale_movie_range.bytes[i] = 0xfcu;      // opaque RGB555 red
            stale_movie_range.bytes[i + 1u] = 0x00u;
        }
        stale_movie_memory.ranges.push_back(std::move(stale_movie_range));
        const m2host::CltExecutionResult rotate_result =
            rotate_interpreter.execute(rotate_words.data(), rotate_words.size(),
                                       &stale_movie_memory);
        if (!expect(rotate_result.ok && rotate_result.unsupported == 0 &&
                        rotate_result.draws == 1 &&
                        rotate_backend.uploads.size() == 1u &&
                        rotate_backend.uploads[0].w == 576 &&
                        rotate_backend.uploads[0].h == 12 &&
                        rotate_backend.uploads[0].pixels.front() ==
                            0xff00ff00u,
                    "Rotate MMDMA strip reads the live display target, not stale MPEG guest RAM"))
            return false;
        bool sheet_windows = rotate_backend.window_reads.size() == 2u;
        for (const auto& window : rotate_backend.window_reads)
            sheet_windows = sheet_windows && window.w == 576 &&
                            window.h == 432 &&
                            window.y == 24;  // default viewport top
        if (!expect(sheet_windows,
                    "Rotate strip readback reads the display sheet once"))
            return false;

        // A strip later in the sheet is copied from its own source rows out
        // of the cached sheet, not from row zero.
        rotate_backend.window_reads.clear();
        rotate_backend.uploads.clear();
        rotate_backend.target_readback_rows = true;
        rotate_words[3] = display_base + strip_bytes * 3u;
        const m2host::CltExecutionResult row3_result =
            rotate_interpreter.execute(rotate_words.data(), rotate_words.size(),
                                       &stale_movie_memory);
        rotate_words[3] = display_base;
        rotate_backend.target_readback_rows = false;
        bool row3_windows = row3_result.ok &&
                            rotate_backend.window_reads.size() == 2u;
        for (const auto& window : rotate_backend.window_reads)
            row3_windows = row3_windows && window.w == 576 &&
                           window.h == 432 && window.y == 24;
        if (!expect(row3_windows,
                    "Rotate strip readback reads the display sheet once"))
            return false;
        // Source row 3*12=36 sits at target row 24+36; the row pattern
        // stores (target_row*4) in green, and the RGB555 round trip keeps
        // the top five bits.
        if (!expect(rotate_backend.uploads[0].pixels.front() ==
                        0xff00f700u,
                    "Rotate strip copy follows the source row offset"))
            return false;

        // The 36 strip loads of one transition frame share the sheet. A
        // second strip in the same list must reuse the cached readback.
        rotate_backend.window_reads.clear();
        rotate_backend.uploads.clear();
        std::vector<uint32_t> two_strips = rotate_words;
        std::vector<uint32_t> second_strip = rotate_words;
        second_strip[3] = display_base + strip_bytes;
        two_strips.insert(two_strips.end(), second_strip.begin(),
                          second_strip.end());
        const m2host::CltExecutionResult two_strip_result =
            rotate_interpreter.execute(two_strips.data(), two_strips.size(),
                                       &stale_movie_memory);
        if (!expect(two_strip_result.ok && two_strip_result.draws == 2 &&
                        rotate_backend.window_reads.size() == 2u &&
                        rotate_backend.uploads.size() >= 2u,
                    "Rotate strips in one list share one sheet readback"))
            return false;

        rotate_backend.target_readback_metadata = 0xff00ff00u; // alpha one, DSB zero
        auto metadata_zero = rotate_interpreter.execute(rotate_words.data(),rotate_words.size(),&stale_movie_memory);
        if (!expect(metadata_zero.ok && metadata_zero.unsupported==0 &&
            rotate_backend.uploads.back().pixels.front()==0x0000ff00u,
            "Rotate feedback bit 15 comes from DSB, not opaque alpha")) return false;
        rotate_backend.target_readback_metadata = 0xff0000ffu; // alpha zero, DSB one
        rotate_backend.target_readback_color = 0x0000ff00u;
        auto metadata_one = rotate_interpreter.execute(rotate_words.data(),rotate_words.size(),&stale_movie_memory);
        if (!expect(metadata_one.ok && metadata_one.unsupported==0 &&
            rotate_backend.uploads.back().pixels.front()==0xff00ff00u,
            "Rotate feedback preserves DSB one with alpha zero")) return false;

        RecordingBackend ordinary_backend;
        ordinary_backend.target_readback_enabled = true;
        ordinary_backend.target_readback_base = display_base;
        ordinary_backend.target_readback_color = 0xff00ff00u;
        m2host::GuestCltInterpreter ordinary_interpreter(&ordinary_backend);
        ordinary_interpreter.set_fb_spans(
            {{1u, display_base, 576u * 432u * 2u}});
        constexpr uint32_t ordinary_base = 0x700000u;
        rotate_words[3] = ordinary_base;
        m2host::CltMemorySnapshot ordinary_memory;
        m2host::CltMemoryRange ordinary_range;
        ordinary_range.guest_address = ordinary_base;
        ordinary_range.bytes.resize(strip_bytes);
        for (size_t i = 0; i < ordinary_range.bytes.size(); i += 2u) {
            ordinary_range.bytes[i] = 0xfcu;
            ordinary_range.bytes[i + 1u] = 0x00u;
        }
        ordinary_memory.ranges.push_back(std::move(ordinary_range));
        const m2host::CltExecutionResult ordinary_result =
            ordinary_interpreter.execute(rotate_words.data(),
                                         rotate_words.size(), &ordinary_memory);
        if (!expect(ordinary_result.ok && ordinary_result.unsupported == 0 &&
                        ordinary_result.draws == 1 &&
                        ordinary_backend.uploads.size() == 1u &&
                        ordinary_backend.uploads[0].pixels.front() ==
                            0xff0000ffu,
                    "non-framebuffer MMDMA loads remain guest-memory backed"))
            return false;
    }

    RecordingBackend mmdma_7bit_backend;
    m2host::GuestCltInterpreter mmdma_7bit_interpreter(&mmdma_7bit_backend);
    const std::vector<uint32_t> mmdma_7bit_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000008u,  // 2 PIP entries (8 bytes)
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000607u,  // 7-bit indexed + SSB expanded format
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,  // 2 bytes (two 8-bit texels)
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_mmdma_7bit_words = mmdma_7bit_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_mmdma_7bit_words, value);

    m2host::CltMemorySnapshot mmdma_7bit_memory;
    m2host::CltMemoryRange mmdma_7bit_pip_range;
    mmdma_7bit_pip_range.guest_address = 0x100u;
    mmdma_7bit_pip_range.bytes = {
        0xff, 0x11, 0x22, 0x33,  // entry 0: alpha=0xff (ssb=1), red=0x11, green=0x22, blue=0x33
        0x7f, 0x44, 0x55, 0x66,  // entry 1: alpha=0x7f (ssb=0), red=0x44, green=0x55, blue=0x66
    };
    mmdma_7bit_memory.ranges.push_back(std::move(mmdma_7bit_pip_range));
    m2host::CltMemoryRange mmdma_7bit_texel_range;
    mmdma_7bit_texel_range.guest_address = 0x200u;
    // Texel 0: SSB=1, index=0 -> 0x80 | 0x00 = 0x80
    // Texel 1: SSB=0, index=1 -> 0x00 | 0x01 = 0x01
    mmdma_7bit_texel_range.bytes = {0x80u, 0x01u};
    mmdma_7bit_memory.ranges.push_back(std::move(mmdma_7bit_texel_range));
    const m2host::CltExecutionResult mmdma_7bit_result =
        mmdma_7bit_interpreter.execute(complete_mmdma_7bit_words.data(),
                                       complete_mmdma_7bit_words.size(),
                                       &mmdma_7bit_memory);
    if (!expect(mmdma_7bit_result.ok && mmdma_7bit_result.unsupported == 0 &&
                    mmdma_7bit_result.draws == 1 &&
                    mmdma_7bit_backend.uploads.size() == 1,
                "7-bit indexed MMDMA texture reaches backend"))
        return false;
    if (!expect(mmdma_7bit_backend.uploads[0].w == 2 &&
                    mmdma_7bit_backend.uploads[0].h == 1 &&
                    mmdma_7bit_backend.uploads[0].pixels[0] == 0xff332211u &&
                    mmdma_7bit_backend.uploads[0].pixels[1] == 0xff665544u,
                "7-bit indexed MMDMA texels are PIP-expanded with SSB"))
        return false;
    // Deep-race 8+7+SSB MMDMA resident (TXTEXPTYPE 0xe78): 21x10 window
    // (TXTUVMAX 0x140009) with a 420-byte blob = 210 texels at 16 bits
    // each, resubmitted per frame. Trace: deep-race live-mixer full-diag
    // run, texture-load + texture-materialize-reg rejects with identical
    // 21x10 keys and 420-byte blobs failing all format predicates.
    // The vendor M2TXIndex_ToUncompr encoder writes the same order as every
    // indexed TRAM row: 1-bit SSB, 7-bit alpha, then the 8-bit PIP index.
    RecordingBackend mmdma_c8a7_backend;
    m2host::GuestCltInterpreter mmdma_c8a7_interpreter(&mmdma_c8a7_backend);
    const std::vector<uint32_t> mmdma_c8a7_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x00000008u,  // 2 PIP entries (8 bytes)
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000e78u,  // 8-bit color + 7-bit alpha + SSB
        0x1000640cu, 0x00000000u,  // materialize once with PIP constants
        0x10006440u, 0x00112233u,  // PIP constant 0: SSB=0, RGB=112233
        0x10006444u, 0xffaabbccu,  // PIP constant 1: SSB=1, RGB=aabbcc
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x000001a4u,  // 420 bytes = 210 16-bit texels
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00140009u,  // UMAX=0x14, VMAX=0x09 -> 21x10
        0x1000643cu, 0x00000e78u,  // use-state format stages the resident
        0x10006408u, 0x00002000u,
        0x1000800cu, 0x00000004u,  // SSB0 uses the recovered PIP selector
        0x10006410u, 0x00009200u,
        0x1000640cu, 0x00008a00u,  // rebind color/SSB to the PIP
        0x10006410u, 0x00009200u,
        0x200a0002u,  // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_mmdma_c8a7_words = mmdma_c8a7_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_mmdma_c8a7_words, value);

    m2host::CltMemorySnapshot mmdma_c8a7_memory;
    m2host::CltMemoryRange mmdma_c8a7_pip_range;
    mmdma_c8a7_pip_range.guest_address = 0x100u;
    mmdma_c8a7_pip_range.bytes = {
        0xff, 0x11, 0x22, 0x33,  // entry 0: alpha=0xff (ssb=1)
        0x7f, 0x44, 0x55, 0x66,  // entry 1: alpha=0x7f (ssb=0)
    };
    mmdma_c8a7_memory.ranges.push_back(std::move(mmdma_c8a7_pip_range));
    m2host::CltMemoryRange mmdma_c8a7_texel_range;
    mmdma_c8a7_texel_range.guest_address = 0x200u;
    mmdma_c8a7_texel_range.bytes.assign(420u, 0u);
    // Texel 0: SSB=0, alpha=max, index=0 -> 0x7f00. TXTPIPCNTL=0x8a00
    // must source the final SSB from the PIP entry, not this source bit.
    mmdma_c8a7_texel_range.bytes[0] = 0x7fu;
    mmdma_c8a7_texel_range.bytes[1] = 0x00u;
    // Texel 1: SSB=1, alpha=0, index=1 -> 0x8001.
    mmdma_c8a7_texel_range.bytes[2] = 0x80u;
    mmdma_c8a7_texel_range.bytes[3] = 0x01u;
    // Texel 2: SSB=0, alpha=0x40, index=0 -> 0x4000. This odd value
    // distinguishes MSB replication (0x81) from the removed 0xe78-only path.
    mmdma_c8a7_texel_range.bytes[4] = 0x40u;
    mmdma_c8a7_texel_range.bytes[5] = 0x00u;
    // Put distinct source fields at the end of row 0, the first texel of
    // row 1, and the final texel.  A row-sized/row-reset mistake therefore
    // cannot hide behind the zero-filled remainder of the 21x10 resident.
    const auto set_e78_texel = [](std::vector<uint8_t>& bytes, size_t pixel,
                                  uint8_t ssb, uint8_t alpha, uint8_t index) {
        const uint16_t packed = static_cast<uint16_t>(
            (static_cast<uint16_t>(ssb) << 15) |
            (static_cast<uint16_t>(alpha) << 8) | index);
        const size_t offset = pixel * 2u;
        bytes[offset] = static_cast<uint8_t>(packed >> 8);
        bytes[offset + 1] = static_cast<uint8_t>(packed);
    };
    set_e78_texel(mmdma_c8a7_texel_range.bytes, 20, 1, 0x7f, 0);
    set_e78_texel(mmdma_c8a7_texel_range.bytes, 21, 0, 0x01, 1);
    set_e78_texel(mmdma_c8a7_texel_range.bytes, 209, 1, 0x40, 1);
    mmdma_c8a7_memory.ranges.push_back(std::move(mmdma_c8a7_texel_range));
    const m2host::CltExecutionResult mmdma_c8a7_result =
        mmdma_c8a7_interpreter.execute(complete_mmdma_c8a7_words.data(),
                                       complete_mmdma_c8a7_words.size(),
                                       &mmdma_c8a7_memory);
    if (!expect(mmdma_c8a7_result.ok && mmdma_c8a7_result.unsupported == 0 &&
                    mmdma_c8a7_result.draws == 1 &&
                    mmdma_c8a7_backend.uploads.size() == 3,
                "8+7+SSB MMDMA texture reaches backend"))
        return false;
    if (!expect(mmdma_c8a7_backend.uploads[0].w == 21 &&
                    mmdma_c8a7_backend.uploads[0].h == 10 &&
                    mmdma_c8a7_backend.uploads[0].pixels.size() == 210u &&
                    mmdma_c8a7_backend.uploads[0].pixels[0] == 0x00332211u &&
                    mmdma_c8a7_backend.uploads[0].pixels[1] == 0xffccbbaau &&
                    mmdma_c8a7_backend.uploads[0].pixels[2] == 0x00332211u,
                "TXTPIPCNTL constants follow raw TRAM SSB at materialization"))
        return false;
    if (!expect(mmdma_c8a7_backend.uploads[1].pixels[0] == 0xff332211u &&
                    mmdma_c8a7_backend.uploads[1].pixels[1] == 0x00665544u &&
                    mmdma_c8a7_backend.uploads[1].pixels[2] == 0x81332211u &&
                    mmdma_c8a7_backend.uploads[1].pixels[20] == 0xff332211u &&
                    mmdma_c8a7_backend.uploads[1].pixels[21] == 0x02665544u &&
                    mmdma_c8a7_backend.uploads[1].pixels[209] == 0x81665544u,
                "8+7+SSB texels rebind to PIP color/SSB and texel alpha "
                "across row boundaries"))
        return false;
    // Wrong-layout control: these bytes encode index=1, alpha=0, SSB=1 in
    // the tempting index-first order.  The production boundary must still
    // read the source-defined SSB/alpha/index fields as (0,1,1); a decoder
    // that reverses the layout loses the source alpha and produces
    // 0x00665544 instead of 0x02665544 here.
    RecordingBackend wrong_layout_backend;
    m2host::GuestCltInterpreter wrong_layout_interpreter(&wrong_layout_backend);
    m2host::CltMemorySnapshot wrong_layout_memory = mmdma_c8a7_memory;
    wrong_layout_memory.ranges.back().bytes[0] = 0x01u;
    wrong_layout_memory.ranges.back().bytes[1] = 0x01u;
    const m2host::CltExecutionResult wrong_layout_result =
        wrong_layout_interpreter.execute(complete_mmdma_c8a7_words.data(),
                                         complete_mmdma_c8a7_words.size(),
                                         &wrong_layout_memory);
    if (!expect(wrong_layout_result.ok && wrong_layout_result.unsupported == 0 &&
                    wrong_layout_backend.uploads.size() == 3 &&
                    wrong_layout_backend.uploads[1].pixels[0] == 0x02665544u &&
                    wrong_layout_backend.uploads[1].pixels[0] != 0x00665544u,
                "0xe78 wrong index/alpha/SSB layout control is not used"))
        return false;

    if (!expect(mmdma_c8a7_backend.draws.size() == 1 &&
                    mmdma_c8a7_backend.draws[0].discard.ssb_zero &&
                    mmdma_c8a7_backend.draws[0].discard.ssb_mask == 3 &&
                    mmdma_c8a7_backend.uploads[2].pixels[0] == 0xffffffffu &&
                    mmdma_c8a7_backend.uploads[2].pixels[1] == 0u &&
                    mmdma_c8a7_backend.uploads[2].pixels[2] == 0xffffffffu,
                "0xe78/0x8a00 publishes the PIP SSB sidecar"))
        return false;

    // Source-backed 4+7+SSB MMDMA resident (TXTEXPTYPE 0xe74): this is the
    // c4 sibling of the recovered e78 form and exercises the same
    // SSB/alpha/index layout with its 12-bit texel stride.
    RecordingBackend mmdma_c4a7_backend;
    m2host::GuestCltInterpreter mmdma_c4a7_interpreter(&mmdma_c4a7_backend);
    const std::vector<uint32_t> mmdma_c4a7_words = {
        0x10006404u, 0x00000200u,  // PIP load
        0x10006424u, 0x00000100u,
        0x10006414u, 0x00046000u,
        0x10006428u, 0x0000000cu,  // three PIP entries
        0x10000014u, 0x00000001u,
        0x1000643cu, 0x00000e74u,  // 4-bit color + 7-bit alpha + SSB
        0x1000640cu, 0x00008a00u,  // PIP color/SSB, texture alpha
        0x10006404u, 0x00000100u,  // MMDMA texture load
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000003u,  // two 12-bit texels
        0x10000014u, 0x00000001u,
        0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
        0x10006430u, 0x00010000u,
        0x10006408u, 0x00002000u,
        0x10006410u, 0x00009200u,
        0x1000643cu, 0x00000e74u,  // use-state format
        0x200a0002u,
    };
    std::vector<uint32_t> complete_mmdma_c4a7_words = mmdma_c4a7_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_mmdma_c4a7_words, value);
    m2host::CltMemorySnapshot mmdma_c4a7_memory;
    m2host::CltMemoryRange mmdma_c4a7_pip_range;
    mmdma_c4a7_pip_range.guest_address = 0x100u;
    mmdma_c4a7_pip_range.bytes = {
        0x00u, 0x11u, 0x22u, 0x33u,  // PIP[0], SSB=0
        0xffu, 0xaau, 0xbbu, 0xccu,  // PIP[1], SSB=1
        0x00u, 0x44u, 0x55u, 0x66u,  // PIP[2], SSB=0
    };
    mmdma_c4a7_memory.ranges.push_back(std::move(mmdma_c4a7_pip_range));
    m2host::CltMemoryRange mmdma_c4a7_texel_range;
    mmdma_c4a7_texel_range.guest_address = 0x200u;
    // Texel 0: SSB=0, alpha=0x7f, index=1; texel 1: SSB=1,
    // alpha=0x40, index=2. The 24-bit stream is 0x7f1c02.
    mmdma_c4a7_texel_range.bytes = {0x7fu, 0x1cu, 0x02u};
    mmdma_c4a7_memory.ranges.push_back(std::move(mmdma_c4a7_texel_range));
    const m2host::CltExecutionResult mmdma_c4a7_result =
        mmdma_c4a7_interpreter.execute(complete_mmdma_c4a7_words.data(),
                                       complete_mmdma_c4a7_words.size(),
                                       &mmdma_c4a7_memory);
    if (!expect(mmdma_c4a7_result.ok && mmdma_c4a7_result.unsupported == 0 &&
                    mmdma_c4a7_result.draws == 1 &&
                    mmdma_c4a7_backend.uploads.size() == 1,
                "4+7+SSB MMDMA texture reaches backend"))
        return false;
    if (!expect(mmdma_c4a7_backend.uploads[0].w == 2 &&
                    mmdma_c4a7_backend.uploads[0].h == 1 &&
                    mmdma_c4a7_backend.uploads[0].pixels[0] == 0xffccbbaau &&
                    mmdma_c4a7_backend.uploads[0].pixels[1] == 0x81665544u,
                "4+7+SSB texels decode SSB-alpha-index order"))
        return false;

    RecordingBackend raw_555_backend;
    m2host::GuestCltInterpreter raw_555_interpreter(&raw_555_backend);
    const std::vector<uint32_t> raw_555_words = {
        0x10006404u, 0x00000000u,  // uncompressed Texture Load, bit offset 0
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x00000002u,  // two rows
        0x1000642cu, 0x00100020u,  // destination 16b, source stride 32b
        0x1000643cu, 0x00001605u,  // literal RGB555 + SSB
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x10000014u, 0x00000001u,  // TLD
        0x200a0002u,                // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_raw_555_words = raw_555_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_raw_555_words, value);

    m2host::CltMemorySnapshot raw_555_memory;
    m2host::CltMemoryRange raw_555_range;
    raw_555_range.guest_address = 0x200u;
    raw_555_range.bytes = {
        0xfcu, 0x00u, 0x03u, 0xe0u,  // red/green source row
        0x00u, 0x1fu, 0x7fu, 0xffu,  // transparent blue/white source row
    };
    raw_555_memory.ranges.push_back(std::move(raw_555_range));
    const m2host::CltExecutionResult raw_555_result =
        raw_555_interpreter.execute(complete_raw_555_words.data(),
                                    complete_raw_555_words.size(),
                                    &raw_555_memory);
    if (!expect(raw_555_result.ok && raw_555_result.unsupported == 0 &&
                    raw_555_result.draws == 1 &&
                    raw_555_backend.uploads.size() == 1,
                "uncompressed RGB555 texture tile reaches backend"))
        return false;
    if (!expect(raw_555_backend.uploads[0].w == 1 &&
                    raw_555_backend.uploads[0].h == 2 &&
                    raw_555_backend.uploads[0].pixels[0] == 0xff0000ffu &&
                    raw_555_backend.uploads[0].pixels[1] == 0x00ff0000u,
                "RGB555 texels honor SSB and source stride")) return false;

    // SDK MIP-chain load: one DMA blob holds the whole chain, the draw's
    // TXTLODBASE0 selects an interior position (lodBase[i] = tramOffset +
    // sum of earlier texelDataSizes), and TXTUVMAX carries the WRAP range
    // (umax = 0x3FF >> (numLOD-1)), not the texture window.  The blob packs
    // a 32x32 6-bit texture (768 B) at offset 0x2a8 inside a 1700 B chain;
    // the draw at base 0x2a8 with uvmax 0x7f007f must materialize the LOD's
    // own 32x32 footprint, not a 128x128 window.
    RecordingBackend chain_backend;
    m2host::GuestCltInterpreter chain_interpreter(&chain_backend);
    const std::vector<uint32_t> chain_words = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,  // source at 0x200
        0x10006414u, 0x00000000u,  // blob base 0
        0x10006428u, 0x000006a4u,  // 1700-byte chain blob
        0x1000643cu, 0x00000406u,  // 6-bit indexed
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x10000014u, 0x00000001u,  // TLD
        0x10036414u, 0x000002a8u, 0x000005a8u, 0x00000668u,
        0x00000698u,               // LOD bases: 0x2a8, size 0x300
        0x1000642cu, 0x007f007fu,  // WRAP uvmax for a 4-LOD chain
        0x10006408u, 0x00002000u,
        0x200a0002u,               // new, texture-only, three-vertex strip
    };
    std::vector<uint32_t> complete_chain_words = chain_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_chain_words, value);

    m2host::CltMemorySnapshot chain_memory;
    m2host::CltMemoryRange chain_range;
    chain_range.guest_address = 0x200u;
    // 1700 bytes: 680 B of an earlier texture + 768 B of 32x32 6-bit data at
    // offset 0x2a8 with a matching PIP gradient.
    chain_range.bytes.assign(1700u, 0u);
    chain_memory.ranges.push_back(std::move(chain_range));
    const m2host::CltExecutionResult chain_result =
        chain_interpreter.execute(complete_chain_words.data(),
                                  complete_chain_words.size(), &chain_memory);
    if (!expect(chain_result.ok && chain_result.unsupported == 0 &&
                    chain_result.draws == 1 &&
                    chain_backend.uploads.size() == 1,
                "interior MIP-chain position reaches backend"))
        return false;
    if (!expect(chain_backend.uploads[0].w == 32 &&
                    chain_backend.uploads[0].h == 32,
                "chain draw uses the LOD footprint, not uvmax"))
        return false;

    // The SDK mask fields are directional.  A 32x16 four-LOD payload has
    // 680 texels (512+128+32+8); the asymmetric mask 0x001f000f identifies
    // that base shape.  Reversing the mask halves would publish 16x32.
    RecordingBackend rectangular_backend;
    m2host::GuestCltInterpreter rectangular_interpreter(&rectangular_backend);
    const std::vector<uint32_t> rectangular_words = {
        0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
        0x10006424u, 0x00000200u,
        0x10006414u, 0x00000000u,
        0x10006428u, 0x000001feu,  // 510 bytes = 680 six-bit texels
        0x1000643cu, 0x00000406u,
        0x10000014u, 0x00000001u,  // TLD
        0x1000642cu, 0x03ff03ffu,  // four-LOD wrap ceiling
        0x10006430u, 0x001f000fu,  // U=31, V=15 -> 32x16 base
        0x10006408u, 0x00002000u,
        0x1000640cu, 0x00009200u,
        0x10006410u, 0x00009200u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_rectangular_words = rectangular_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_rectangular_words, value);
    m2host::CltMemorySnapshot rectangular_memory;
    m2host::CltMemoryRange rectangular_range;
    rectangular_range.guest_address = 0x200u;
    rectangular_range.bytes.assign(510u, 0u);
    rectangular_memory.ranges.push_back(std::move(rectangular_range));
    const m2host::CltExecutionResult rectangular_result =
        rectangular_interpreter.execute(complete_rectangular_words.data(),
                                        complete_rectangular_words.size(),
                                        &rectangular_memory);
    if (!expect(rectangular_result.ok && rectangular_result.unsupported == 0 &&
                    rectangular_result.draws == 1 &&
                    rectangular_backend.uploads.size() == 1,
                "asymmetric mask MIP chain reaches backend"))
        return false;
    if (!expect(rectangular_backend.uploads[0].w == 32 &&
                    rectangular_backend.uploads[0].h == 16,
                "asymmetric mask preserves U-width and V-height"))
        return false;
    if (!expect(rectangular_backend.draws[0].sampling.wrap_u &&
                    rectangular_backend.draws[0].sampling.wrap_v,
                "asymmetric mask enables repeat on both resident axes"))
        return false;

    // Stale-label MMDMA deferral (0x402 family): the TLD-time TXTEXPTYPE
    // value is the previous texture's use-state, not this load's format.
    // World-pack lists write only SRC/COUNT/LOD0 fresh per load and never
    // write 0x402, yet their TLDs observe 0x402; the real use-state arrives
    // at the following TXTEXPTYPE write. Staging must not gate on the stale
    // label — validation belongs to materialize time. Fails pre-fix (the
    // load rejects the stale label, so the draw finds no resident), passes
    // post-fix.
    {
        RecordingBackend stale_backend;
        m2host::GuestCltInterpreter stale_interpreter(&stale_backend);
        const std::vector<uint32_t> stale_words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000010u,  // 4 PIP entries (16 bytes)
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000402u,  // stale 2-bit residue, not this load
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
            0x10006424u, 0x00000200u,  // source at 0x200
            0x10006414u, 0x00000000u,  // blob base 0
            0x10006428u, 0x00000002u,  // 2 bytes = two 6-bit texels
            0x10000014u, 0x00000001u,  // TLD under the stale label
            0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
            0x1000643cu, 0x00000406u,  // real use-state: 6-bit indexed
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00009200u,
            0x200a0002u,  // new, texture-only, three-vertex strip
        };
        std::vector<uint32_t> complete_stale_words = stale_words;
        for (const auto& vertex : raw_texture_vertices)
            for (float value : vertex) append_float(complete_stale_words, value);
        m2host::CltMemorySnapshot stale_memory;
        m2host::CltMemoryRange stale_pip_range;
        stale_pip_range.guest_address = 0x100u;
        stale_pip_range.bytes = {
            0xff, 0x11, 0x22, 0x33,  // entry 0
            0xff, 0x44, 0x55, 0x66,  // entry 1
            0xff, 0x77, 0x88, 0x99,  // entry 2
            0xff, 0xaa, 0xbb, 0xcc,  // entry 3
        };
        stale_memory.ranges.push_back(std::move(stale_pip_range));
        m2host::CltMemoryRange stale_texel_range;
        stale_texel_range.guest_address = 0x200u;
        // Two 6-bit indices MSB-first: 000000 000001 (+ 4 pad bits).
        stale_texel_range.bytes = {0x00u, 0x10u};
        stale_memory.ranges.push_back(std::move(stale_texel_range));
        const m2host::CltExecutionResult stale_result =
            stale_interpreter.execute(complete_stale_words.data(),
                                      complete_stale_words.size(),
                                      &stale_memory);
        if (!expect(stale_result.ok && stale_result.unsupported == 0 &&
                        stale_result.draws == 1 &&
                        stale_backend.uploads.size() == 2,
                    "stale-labelled MMDMA load defers to the real use-state"))
            return false;
        if (!expect(stale_backend.uploads[1].w == 2 &&
                        stale_backend.uploads[1].h == 1 &&
                        stale_backend.uploads[1].pixels[0] == 0xff332211u &&
                        stale_backend.uploads[1].pixels[1] == 0xff665544u,
                    "deferred 6-bit texels decode from the staged blob"))
            return false;
    }

    // Fail-closed guard: a guest-WRITTEN use-state with a reserved encoding
    // (0x400 = indexed with IDEPTH 0; the toolkit's TXTEXPTYPE table reserves
    // 0 and supports only 1..8) has no evidenced expansion and must still
    // reject — at materialize time post-fix rather than at load time. Passes
    // both pre- and post-fix; it pins the closed end of the deferral.
    {
        RecordingBackend unknown_backend;
        m2host::GuestCltInterpreter unknown_interpreter(&unknown_backend);
        const std::vector<uint32_t> unknown_words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000010u,  // 4 PIP entries (16 bytes)
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000400u,  // stale reserved residue at TLD time
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load, mode 1
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000002u,
            0x10000014u, 0x00000001u,  // TLD stages the raw bytes
            0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
            0x1000643cu, 0x00000400u,  // written use-state: still reserved
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00000000u,
            0x10006410u, 0x00009200u,
            0x200a0002u,  // new, texture-only, three-vertex strip
        };
        std::vector<uint32_t> complete_unknown_words = unknown_words;
        for (const auto& vertex : raw_texture_vertices)
            for (float value : vertex) append_float(complete_unknown_words, value);
        m2host::CltMemorySnapshot unknown_memory;
        m2host::CltMemoryRange unknown_pip_range;
        unknown_pip_range.guest_address = 0x100u;
        unknown_pip_range.bytes.assign(16u, 0u);
        unknown_memory.ranges.push_back(std::move(unknown_pip_range));
        m2host::CltMemoryRange unknown_texel_range;
        unknown_texel_range.guest_address = 0x200u;
        unknown_texel_range.bytes = {0x00u, 0x10u};
        unknown_memory.ranges.push_back(std::move(unknown_texel_range));
        const m2host::CltExecutionResult unknown_result =
            unknown_interpreter.execute(complete_unknown_words.data(),
                                        complete_unknown_words.size(),
                                        &unknown_memory);
        if (!expect(unknown_result.ok && unknown_result.unsupported != 0 &&
                        unknown_result.draws == 0 &&
                        unknown_backend.uploads.empty(),
                    "unknown use-state stays fail-closed"))
            return false;
    }

    // Load/use pairing across one list: two textures stage with a stale
    // first label; each TXTEXPTYPE write must consume exactly the blob its
    // TLD staged (no overwrite before consume, no cross-talk). Fails
    // pre-fix (first load rejects, one upload), passes post-fix.
    {
        RecordingBackend pair_backend;
        m2host::GuestCltInterpreter pair_interpreter(&pair_backend);
        const std::vector<uint32_t> pair_words = {
            0x10006404u, 0x00000200u,  // PIP load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000010u,  // 4 PIP entries (16 bytes)
            0x10000014u, 0x00000001u,
            0x1000643cu, 0x00000402u,  // stale residue
            0x10006404u, 0x00000100u,  // MMDMA load A, mode 1
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,  // blob A base 0
            0x10006428u, 0x00000002u,  // two 6-bit texels
            0x10000014u, 0x00000001u,  // TLD A under the stale label
            0x1000642cu, 0x00010000u,  // 2x1
            0x1000640cu, 0x00009200u,  // PIP color/alpha/SSB for use A
            0x1000643cu, 0x00000406u,  // use A: 6-bit indexed
            0x10006404u, 0x00000100u,  // MMDMA load B, mode 1
            0x10006424u, 0x00000300u,  // source at 0x300
            0x10006414u, 0x00000100u,  // blob B base 0x100
            0x10006428u, 0x00000001u,  // two 4-bit texels
            0x10000014u, 0x00000001u,  // TLD B
            0x1000642cu, 0x00010000u,  // 2x1
            0x1000640cu, 0x00009200u,  // PIP color/alpha/SSB for use B
            0x1000643cu, 0x00000404u,  // use B: 4-bit indexed
        };
        m2host::CltMemorySnapshot pair_memory;
        m2host::CltMemoryRange pair_pip_range;
        pair_pip_range.guest_address = 0x100u;
        pair_pip_range.bytes = {
            0xff, 0x11, 0x22, 0x33,  // entry 0
            0xff, 0x44, 0x55, 0x66,  // entry 1
            0xff, 0x77, 0x88, 0x99,  // entry 2
            0xff, 0xaa, 0xbb, 0xcc,  // entry 3
        };
        pair_memory.ranges.push_back(std::move(pair_pip_range));
        m2host::CltMemoryRange pair_a_range;
        pair_a_range.guest_address = 0x200u;
        pair_a_range.bytes = {0x00u, 0x10u};  // 6-bit indices 0, 1
        pair_memory.ranges.push_back(std::move(pair_a_range));
        m2host::CltMemoryRange pair_b_range;
        pair_b_range.guest_address = 0x300u;
        pair_b_range.bytes = {0x23u};  // 4-bit indices 2, 3 MSB-first
        pair_memory.ranges.push_back(std::move(pair_b_range));
        const m2host::CltExecutionResult pair_result =
            pair_interpreter.execute(pair_words.data(), pair_words.size(),
                                     &pair_memory);
        if (!expect(pair_result.ok && pair_result.unsupported == 0 &&
                        pair_backend.uploads.size() == 2,
                    "paired stale loads each meet their use-state"))
            return false;
        if (!expect(pair_backend.uploads[0].w == 2 &&
                        pair_backend.uploads[0].h == 1 &&
                        pair_backend.uploads[0].pixels[0] == 0xff332211u &&
                        pair_backend.uploads[0].pixels[1] == 0xff665544u &&
                        pair_backend.uploads[1].w == 2 &&
                        pair_backend.uploads[1].h == 1 &&
                        pair_backend.uploads[1].pixels[0] == 0xff998877u &&
                        pair_backend.uploads[1].pixels[1] == 0xffccbbaau,
                    "paired blobs do not cross-talk"))
            return false;
    }

    // Overwritten stale blob (Gears footer shadow): a mesh blob staged in one
    // list that never materializes there (its element draws untextured) must
    // not survive a later texture load that writes over its destination
    // bytes. Pre-fix the blob stays pending across the list boundary and
    // materializes at the next textured draw using the new snippet's
    // registers, publishing over the freshly loaded texture (live Gears:
    // footer 0x6bf3f0 at base 0 shadowed by a 3448-byte mesh blob at base 0).
    // Hardware TRAM coverage says the later write destroys the staged bytes.
    {
        RecordingBackend overwrite_backend;
        m2host::GuestCltInterpreter overwrite_interpreter(
            &overwrite_backend);
        // List 1: PIP palette, stage an 8-byte MMDMA blob at base 0, then
        // force a failed boundary materialization (LODBASE0 0x46000 written
        // before the next load control) so the blob would carry into list 2.
        const std::vector<uint32_t> overwrite_stage_words = {
            0x10006404u, 0x00000200u,  // PIP palette load
            0x10006424u, 0x00000100u,
            0x10006414u, 0x00046000u,
            0x10006428u, 0x00000010u,  // 4 PIP entries
            0x10000014u, 0x00000001u,
            0x1000640cu, 0x00009200u,  // PIP color/alpha/SSB
            0x10006404u, 0x00000100u,  // MMDMA load, mode 1
            0x10006424u, 0x00000200u,  // blob source at 0x200
            0x10006414u, 0x00000000u,  // blob base 0
            0x10006428u, 0x00000008u,  // 8 bytes
            0x10000014u, 0x00000001u,  // TLD stages the raw bytes
            0x10006414u, 0x00046000u,  // next snippet points at TEPIPRAM ...
            0x10006404u, 0x00000200u,  // ... before its load control: boundary
        };
        m2host::CltMemorySnapshot overwrite_memory;
        m2host::CltMemoryRange overwrite_pip_range;
        overwrite_pip_range.guest_address = 0x100u;
        overwrite_pip_range.bytes = {
            0xff, 0x11, 0x22, 0x33,  // entry 0
            0xff, 0x44, 0x55, 0x66,  // entry 1
            0xff, 0x77, 0x88, 0x99,  // entry 2
            0xff, 0xaa, 0xbb, 0xcc,  // entry 3
        };
        overwrite_memory.ranges.push_back(std::move(overwrite_pip_range));
        m2host::CltMemoryRange overwrite_blob_range;
        overwrite_blob_range.guest_address = 0x200u;
        overwrite_blob_range.bytes = {
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
        overwrite_memory.ranges.push_back(std::move(overwrite_blob_range));
        m2host::CltMemoryRange overwrite_texel_range;
        overwrite_texel_range.guest_address = 0x300u;
        overwrite_texel_range.bytes = {0x00u, 0x01u, 0x02u, 0x03u};
        overwrite_memory.ranges.push_back(std::move(overwrite_texel_range));
        (void)overwrite_interpreter.execute(overwrite_stage_words.data(),
                                            overwrite_stage_words.size(),
                                            &overwrite_memory);
        // List 2: publish a 2x2 8-bit indexed texture at base 0 (overwriting
        // the blob's bytes) and draw it with LOD base 0.
        const std::vector<uint32_t> overwrite_draw_words = {
            0x1000640cu, 0x00009200u,  // PIP color/alpha/SSB
            0x10006404u, 0x00000000u,  // uncompressed load, mode 0
            0x10006424u, 0x00000300u,  // source at 0x300
            0x10006414u, 0x00000000u,  // destination base 0
            0x10006428u, 0x00000002u,  // two rows
            0x1000642cu, 0x00100010u,  // 2 texels per row at 8bpt
            0x1000643cu, 0x00000408u,  // 8-bit indexed expansion
            0x10000014u, 0x00000001u,  // publish at base 0
            0x10006414u, 0x00000000u,  // draw LOD base 0
            0x1000642cu, 0x00010001u,  // TXTUVMAX window 2x2
            0x10006430u, 0x00010001u,  // TXTUVMASK
            0x10006408u, 0x00002000u,  // texture lookup enabled
            0x1000640cu, 0x00009200u,
            0x10006410u, 0x00009200u,
            0x200a0002u,  // new, texture-only, three-vertex strip
        };
        std::vector<uint32_t> complete_overwrite_words = overwrite_draw_words;
        const float overwrite_vertices[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {2.f, 0.f, 2.f, 0.f},
            {0.f, 2.f, 0.f, 2.f},
        };
        for (const auto& vertex : overwrite_vertices)
            for (float value : vertex)
                append_float(complete_overwrite_words, value);
        const m2host::CltExecutionResult overwrite_result =
            overwrite_interpreter.execute(complete_overwrite_words.data(),
                                          complete_overwrite_words.size(),
                                          &overwrite_memory);
        if (!expect(overwrite_result.ok && overwrite_result.draws == 1 &&
                        overwrite_backend.uploads.size() == 1,
                    "overwritten MMDMA blob does not shadow the later texture"))
            return false;
        if (!expect(overwrite_backend.uploads[0].w == 2 &&
                        overwrite_backend.uploads[0].h == 2 &&
                        overwrite_backend.uploads[0].pixels[0] == 0xff332211u &&
                        overwrite_backend.uploads[0].pixels[1] == 0xff665544u &&
                        overwrite_backend.uploads[0].pixels[2] == 0xff998877u &&
                        overwrite_backend.uploads[0].pixels[3] == 0xffccbbaau,
                    "later texture stays resident after the overwrite"))
            return false;
    }

    const std::vector<uint32_t> blend_words = {
        0x10006410u, 0x0002842bu,  // TEXCOLOR * CONSTCOLOR, TEXALPHA output
        0x10006448u, 0x00808080u,  // TAB blend constant SSB0
        0x1000644cu, 0x00808080u,  // TAB blend constant SSB1
        0x200a0002u,
    };
    std::vector<uint32_t> complete_blend_words = blend_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex) append_float(complete_blend_words, value);
    const m2host::CltExecutionResult blend_result =
        raw_texture_interpreter.execute(complete_blend_words.data(),
                                        complete_blend_words.size(),
                                        &raw_texture_memory);
    if (!expect(blend_result.ok && blend_result.unsupported == 0 &&
                    blend_result.draws == 1 &&
                    raw_texture_backend.draws.size() == 5,
                "observed TAB multiply reaches backend"))
        return false;
    if (!expect(raw_texture_backend.draws.back().vertices[0].r > 0.5f &&
                    raw_texture_backend.draws.back().vertices[0].r < 0.51f &&
                    raw_texture_backend.draws.back().vertices[0].g > 0.5f &&
                    raw_texture_backend.draws.back().vertices[0].b > 0.5f,
                "TAB multiply publishes its constant color as tint")) return false;

    const std::vector<uint32_t> constant_output_words = {
        0x10008054u, 0x00ffffffu,  // explicit white PRIMCOLOR for M_TBLitTex
        0x10006410u, 0x00020499u,  // TEXCOLOR * CONSTCOLOR, CONSTCOLOR output
        0x10006448u, 0x00808080u,
        0x1000644cu, 0x00808080u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_constant_output_words =
        constant_output_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_constant_output_words, value);
    const m2host::CltExecutionResult constant_output_result =
        raw_texture_interpreter.execute(complete_constant_output_words.data(),
                                        complete_constant_output_words.size(),
                                        &raw_texture_memory);
    if (!expect(constant_output_result.ok &&
                    constant_output_result.unsupported == 0 &&
                    constant_output_result.draws == 1 &&
                    raw_texture_backend.draws.size() == 6,
                "observed constant-output TAB multiply reaches backend"))
        return false;
    if (!expect(raw_texture_backend.draws.back().vertices[0].r > 0.5f &&
                    raw_texture_backend.draws.back().vertices[0].g > 0.5f &&
                    raw_texture_backend.draws.back().vertices[0].b > 0.5f,
                "constant-output TAB multiply preserves tint")) return false;

    const std::vector<uint32_t> third_input_multiply_words = {
        0x10006410u, 0x00020419u,  // alternate texture/constant multiply, constant output
        0x10006448u, 0x00808080u,
        0x1000644cu, 0x00808080u,
        0x200a0002u,
    };
    std::vector<uint32_t> complete_third_input_multiply_words =
        third_input_multiply_words;
    for (const auto& vertex : raw_texture_vertices)
        for (float value : vertex)
            append_float(complete_third_input_multiply_words, value);
    const m2host::CltExecutionResult third_input_multiply_result =
        raw_texture_interpreter.execute(
            complete_third_input_multiply_words.data(),
            complete_third_input_multiply_words.size(), &raw_texture_memory);
    if (!expect(third_input_multiply_result.ok &&
                    third_input_multiply_result.unsupported == 0 &&
                    third_input_multiply_result.draws == 1,
                "observed alternate TAB multiply reaches backend"))
        return false;

    // The observed State 3 stream interleaves a new strip with a fan
    // continuation (e.g. 200f0002 strip -> 20170000 fan cont); the
    // continuation path runs its own TAB decode, so the constant-output
    // multiply must tint the continuation triangle too.
    {
        RecordingBackend cont_backend;
        m2host::GuestCltInterpreter cont_interpreter(&cont_backend);
        std::vector<uint32_t> cont_words = raw_texture_words;
        // raw_texture_words ends at the setup vertex header; its vertex
        // floats live in raw_texture_vertices and must precede any follow-on
        // headers (the texture batch draws the first triangle).
        for (const auto& vertex : raw_texture_vertices)
            for (float value : vertex) append_float(cont_words, value);
        cont_words.push_back(0x10006410u);
        cont_words.push_back(0x00020499u);  // TEXCOLOR * CONSTCOLOR output
        cont_words.push_back(0x10006448u);
        cont_words.push_back(0x00808080u);
        cont_words.push_back(0x1000644cu);
        cont_words.push_back(0x00808080u);
        // New shaded+textured+perspective strip (3 verts, 9 floats each).
        cont_words.push_back(0x200f0002u);
        const float strip_vertices[][9] = {
            {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f},
            {3.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 3.f, 0.f},
            {0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 1.f},
        };
        for (const auto& vertex : strip_vertices)
            for (float value : vertex) append_float(cont_words, value);
        // Fan continuation: new=0, count 0 -> one shaded+textured+perspective
        // vertex (the observed 0x20170000 form).
        cont_words.push_back(0x20170000u);
        const float cont_vertex[9] = {3.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 3.f, 1.f};
        for (float value : cont_vertex) append_float(cont_words, value);
        const m2host::CltExecutionResult cont_result =
            cont_interpreter.execute(cont_words.data(), cont_words.size(),
                                     &raw_texture_memory);
        if (!expect(cont_result.ok && cont_result.unsupported == 0 &&
                        cont_result.draws == 3 &&
                        cont_backend.draws.size() == 3,
                    "constant-output TAB multiply covers fan continuations"))
            return false;
        if (!expect(cont_backend.draws.back().textured &&
                        cont_backend.draws.back().vertices[0].r > 0.99f &&
                        cont_backend.draws.back().vertices[0].g < 0.01f &&
                        cont_backend.draws.back().vertices[0].b < 0.01f,
                    "fan continuation preserves PRIMCOLOR"))
            return false;
    }

    const std::vector<uint32_t> malformed = {0x10008054u};
    // Shooter effect mask: FC=TEXALPHA(2), SC=CONSTCOLOR(5), MULT(1),
    // COLOROUT=BLEND(2), ALPHAOUT=PRIMALPHA(0). Both new and continuation
    // vertices must retain primitive alpha independently of sampled alpha.
    {
        RecordingBackend mask_backend;
        m2host::GuestCltInterpreter mask_interpreter(&mask_backend);
        std::vector<uint32_t> words = complete_raw_texture_words;
        const uint32_t registers[] = {
            0x10006410u, 0x0002042au,
            0x10006448u, 0x00804020u,
            0x1000644cu, 0x00804020u,
            0x200f0002u,
        };
        words.insert(words.end(), std::begin(registers), std::end(registers));
        const float vertices[][9] = {
            {0, 0, 1, 0, 0, .25f, 1, 0, 0},
            {3, 0, 0, 1, 0, .25f, 1, 3, 0},
            {0, 1, 0, 0, 1, .25f, 1, 0, 1},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
        words.push_back(0x20170000u);
        const float continuation[] = {3, 1, 0, 1, 1, .25f, 1, 3, 1};
        for (float value : continuation) append_float(words, value);
        const auto result = mask_interpreter.execute(words.data(), words.size(),
                                                     &raw_texture_memory);
        if (!expect(result.ok && result.unsupported == 0 &&
                    mask_backend.draws.size() == 3,
                    "alpha-color TAB reaches new and continuation draws"))
            return false;
        for (size_t d = 1; d < mask_backend.draws.size(); ++d) {
            const auto& draw = mask_backend.draws[d];
            if (!expect(draw.blend.texture_alpha_as_color,
                        "alpha-color TAB transports the selected texture input"))
                return false;
            for (const auto& v : draw.vertices)
                if (!expect(std::fabs(v.r - 128.f / 255.f) < .0001f &&
                            std::fabs(v.g - 64.f / 255.f) < .0001f &&
                            std::fabs(v.b - 32.f / 255.f) < .0001f &&
                            v.a == .25f,
                            "alpha-color TAB preserves tint and primitive alpha"))
                    return false;
        }
    }
    const m2host::CltExecutionResult malformed_result = interpreter.execute(
        malformed.data(), malformed.size());
    return expect(!malformed_result.ok, "truncated register list is rejected");
}

bool test_clt_flow_control()
{
    // CLT_RegistersHeader(DCNTLDATA, 2), DCNTLDATA, DCNTL is the vendor
    // three-word JA/JR encoding. Keep the target address calculation
    // independent of the implementation: the target starts after the first
    // unreachable 19-word triangle (header plus three six-word vertices).
    const uint32_t list_base = 0x4000c0u;
    const size_t target_word = 22u;
    const uint32_t target_address =
        list_base + static_cast<uint32_t>(target_word * 4u);
    const auto append_shaded_triangle = [](std::vector<uint32_t>& words,
                                           float x, float y,
                                           float r, float g, float b) {
        words.push_back(0x20090002u);  // NEW shaded strip, three vertices
        const float vertices[][6] = {
            {x, y, r, g, b, 1.f},
            {x + 2.f, y, 0.f, 1.f, 0.f, 1.f},
            {x, y + 2.f, 0.f, 0.f, 1.f, 1.f},
        };
        for (const auto& vertex : vertices)
            for (float value : vertex) append_float(words, value);
    };
    const auto make_absolute_jump = [&](uint32_t target) {
        std::vector<uint32_t> words = {
            0x10010010u, target, 0x00000002u,  // DCNTLDATA + JA
        };
        append_shaded_triangle(words, 10.f, 10.f, 1.f, 0.f, 0.f);
        append_shaded_triangle(words, 100.f, 100.f, 0.f, 1.f, 0.f);
        return words;
    };

    // A linear decoder would consume both triangles. The source jump changes
    // the fetch cursor, so only the target triangle is observable.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = make_absolute_jump(target_address);
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && result.triangles == 1 &&
                        result.control_flow_jumps == 1 &&
                        result.control_flow_refusals == 0 &&
                        backend.draws.size() == 1,
                    "DCNTL JA consumes only its in-list target"))
            return false;
        if (!expect(!backend.draws.empty() &&
                        backend.draws[0].vertices[0].x == 100.f,
                    "DCNTL JA does not execute the unreachable triangle"))
            return false;
    }

    // JR is a signed byte displacement from the address of the DCNTL control
    // word. The control word is word 2, hence target word 22 is +0x50 bytes.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        // Replace the absolute target in the fixture with the relative form.
        std::vector<uint32_t> relative_words = make_absolute_jump(0u);
        relative_words[1] = 0x00000050u;
        relative_words[2] = 0x00000004u;  // DCNTL JR
        const m2host::CltExecutionResult result = interpreter.execute(
            relative_words.data(), relative_words.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.draws == 1 && result.control_flow_jumps == 1 &&
                        backend.draws.size() == 1 &&
                        !backend.draws.empty() &&
                        backend.draws[0].vertices[0].x == 100.f,
                    "DCNTL JR resolves its signed in-list displacement"))
            return false;
    }

    // A non-terminal target outside the copied submission cannot be silently
    // discarded. It is a valid source instruction, so retain it as an
    // unsupported refusal rather than misclassifying it as malformed bytes.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words =
            make_absolute_jump(list_base + 0x100000u);
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 1 &&
                        result.control_flow_refusals == 1 &&
                        result.draws == 0 && backend.draws.empty(),
                    "non-terminal external JA fails closed"))
            return false;
    }

    // Misalignment and an unknown list base are safety boundaries, not
    // truncations or implicit absolute addressing.
    {
        RecordingBackend misaligned_backend;
        m2host::GuestCltInterpreter misaligned_interpreter(&misaligned_backend);
        const std::vector<uint32_t> misaligned =
            make_absolute_jump(list_base + 2u);
        const m2host::CltExecutionResult misaligned_result =
            misaligned_interpreter.execute(misaligned.data(),
                                           misaligned.size(), nullptr,
                                           list_base);
        if (!expect(misaligned_result.ok &&
                        misaligned_result.unsupported == 1 &&
                        misaligned_result.control_flow_refusals == 1 &&
                        misaligned_result.draws == 0,
                    "misaligned JA target fails closed"))
            return false;

        RecordingBackend unknown_base_backend;
        m2host::GuestCltInterpreter unknown_base_interpreter(
            &unknown_base_backend);
        const std::vector<uint32_t> unknown_base = {
            0x10010010u, list_base + 0x1000u, 0x00000002u,
        };
        const m2host::CltExecutionResult unknown_base_result =
            unknown_base_interpreter.execute(unknown_base.data(),
                                             unknown_base.size());
        if (!expect(unknown_base_result.ok &&
                        unknown_base_result.unsupported == 1 &&
                        unknown_base_result.control_flow_refusals == 1 &&
                        unknown_base_result.draws == 0,
                    "external JA without a list base fails closed"))
            return false;

        RecordingBackend reserved_backend;
        m2host::GuestCltInterpreter reserved_interpreter(&reserved_backend);
        std::vector<uint32_t> reserved_words = {
            0x10000014u, 0x00000040u,  // reserved DCNTL bit
        };
        append_shaded_triangle(reserved_words, 10.f, 10.f, 1.f, 0.f, 0.f);
        const m2host::CltExecutionResult reserved_result =
            reserved_interpreter.execute(reserved_words.data(),
                                         reserved_words.size());
        if (!expect(reserved_result.ok && reserved_result.unsupported == 1 &&
                        reserved_result.control_flow_refusals == 1 &&
                        reserved_result.draws == 0,
                    "reserved DCNTL bits fail closed"))
            return false;
    }

    // A jump back into an instruction this execution already ran is the
    // guest's own field loop, not a malformed list: the showroom scene list
    // draws its frame and then jumps to its first word so the TE keeps
    // producing that field while the guest builds the next one. The walk must
    // stop at that boundary after exactly one pass -- bounded, with no
    // graphics rejection -- instead of repeating the whole list up to the
    // malformed-list guard with identical pixels.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> loop_words = shaded_triangle();
        loop_words.push_back(0x10010010u);  // DCNTLDATA ...
        loop_words.push_back(list_base);    // ... back to the list head ...
        loop_words.push_back(0x00000002u);  // ... DCNTL JA
        const m2host::CltExecutionResult result = interpreter.execute(
            loop_words.data(), loop_words.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.control_flow_refusals == 0 &&
                        result.control_flow_loops == 1 &&
                        result.control_flow_jumps == 0 &&
                        result.draws == 1 && backend.draws.size() == 1,
                    "list-head JA runs one pass and ends as a field loop"))
            return false;
    }

    // The same fixture through the production bridge, so the aggregate counter
    // a diagnostic reads cannot silently lose the loop.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        std::vector<uint32_t> loop_words = shaded_triangle();
        loop_words.push_back(0x10010010u);
        loop_words.push_back(list_base);
        loop_words.push_back(0x00000002u);
        const std::vector<uint8_t> bytes = big_endian_bytes(loop_words);
        bridge.enqueue(0u, 0x000d0003u, 0u, list_base,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.control_flow_loops == 1 && stats.unsupported == 0 &&
                        stats.control_flow_refusals == 0 && stats.draws == 1,
                    "bridge reports the field loop instead of a refusal"))
            return false;
    }

    // A self-jump is the smallest field loop: it must stop at the boundary
    // rather than spin in the host interpreter.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> self_jump = {
            0x10010010u, 0xfffffff8u, 0x00000004u,
        };
        const m2host::CltExecutionResult result = interpreter.execute(
            self_jump.data(), self_jump.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.control_flow_refusals == 0 &&
                        result.control_flow_loops == 1 &&
                        result.draws == 0 && backend.draws.empty(),
                    "self JR ends the pass as a field loop"))
            return false;
    }

    // PSE is source-defined as stopping after the word following the control
    // instruction. Portfolio's high-latency lists use it as their terminal
    // sentinel, so that form must not add a graphics rejection. A mid-list
    // pause is retained as an explicit unsupported boundary because the host
    // has no device restart/IRP continuation to publish.
    {
        RecordingBackend terminal_backend;
        m2host::GuestCltInterpreter terminal_interpreter(&terminal_backend);
        std::vector<uint32_t> terminal_words = shaded_triangle();
        terminal_words.push_back(0x10000014u);
        terminal_words.push_back(0x00000010u);  // DCNTL PSE
        const m2host::CltExecutionResult terminal_result =
            terminal_interpreter.execute(terminal_words.data(),
                                         terminal_words.size());
        if (!expect(terminal_result.ok && terminal_result.unsupported == 0 &&
                        terminal_result.draws == 1 &&
                        terminal_result.control_flow_pauses == 1 &&
                        terminal_backend.draws.size() == 1,
                    "terminal PSE preserves the preceding list work"))
            return false;

        RecordingBackend mid_backend;
        m2host::GuestCltInterpreter mid_interpreter(&mid_backend);
        std::vector<uint32_t> mid_words = {
            0x10000014u, 0x00000010u,  // mid-list PSE
        };
        append_shaded_triangle(mid_words, 10.f, 10.f, 1.f, 0.f, 0.f);
        const m2host::CltExecutionResult mid_result = mid_interpreter.execute(
            mid_words.data(), mid_words.size());
        if (!expect(mid_result.ok && mid_result.unsupported == 1 &&
                        mid_result.control_flow_refusals == 1 &&
                        mid_result.draws == 0 && mid_backend.draws.empty(),
                    "mid-list PSE is explicitly refused"))
            return false;
    }

    // DINT is a CPU interrupt request, not a renderer state bit. It must stop
    // before the following triangle and remain visible as an unsupported
    // request without being counted as malformed packet data.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        std::vector<uint32_t> words = {
            0x10010010u, 0x00001234u, 0x00000008u,  // DCNTLDATA + DINT
        };
        append_shaded_triangle(words, 10.f, 10.f, 1.f, 0.f, 0.f);
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size());
        if (!expect(result.ok && result.unsupported == 1 &&
                        result.control_flow_refusals == 1 &&
                        result.register_writes == 2 && result.draws == 0 &&
                        backend.draws.empty(),
                    "DINT refuses its external interrupt side effect"))
            return false;
    }

    // A terminal JA to another submitted Portfolio buffer is a real list
    // handoff. It is recorded without pretending that the external words were
    // present in this copied payload; a non-terminal handoff was rejected
    // above.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = {
            0x10010010u, list_base + 0x1000u, 0x00000002u,
        };
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size(), nullptr, list_base);
        if (!expect(result.ok && result.unsupported == 0 &&
                        result.control_flow_external_handoffs == 1 &&
                        result.control_flow_jumps == 0 && result.draws == 0,
                    "terminal external JA is recorded as a list handoff"))
            return false;
    }

    // Exercise the same absolute target through the production bridge, which
    // copies big-endian guest words before passing the guest list base to the
    // interpreter. This catches a regression that only handles direct calls.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        const std::vector<uint32_t> words = make_absolute_jump(target_address);
        const std::vector<uint8_t> bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, list_base,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.submitted == 1 && stats.malformed == 0 &&
                        stats.unsupported == 0 && stats.draws == 1 &&
                        stats.control_flow_jumps == 1 &&
                        stats.control_flow_refusals == 0 &&
                        backend.draws.size() == 1 &&
                        !backend.draws.empty() &&
                        backend.draws[0].vertices[0].x == 100.f,
                    "production bridge preserves the JA list base"))
            return false;
    }

    // The bridge accounting keeps a valid-but-unsupported DINT out of the
    // malformed bucket while preserving the refusal itself.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        std::vector<uint32_t> words = {
            0x10010010u, 0x00001234u, 0x00000008u,
        };
        append_shaded_triangle(words, 10.f, 10.f, 1.f, 0.f, 0.f);
        const std::vector<uint8_t> bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, list_base,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.malformed == 0 && stats.unsupported == 1 &&
                        stats.control_flow_refusals == 1 &&
                        stats.draws == 0,
                    "bridge accounts for DINT as unsupported flow control"))
            return false;
    }

    return true;
}

// CLT-POINT-PACKET-DOUBLE-COUNT. A kPointType packet is a recognized CLT
// packet type that this interpreter refuses rather than mis-parses, so the
// bridge must charge it to unsupported exactly once and leave the malformed
// bucket alone. Point-packet reachability in the game is unproven, so these
// fixtures pin the accounting boundary only and claim no guest decode: the
// interpreter rejects at the type tag before reading the payload, so every
// word after the header is opaque on this path.
bool test_point_packet_accounting()
{
    const uint32_t list_base = 0x4000c0u;
    // Type tag plus the position and parameter words the reject key names.
    // Only the 0x40000000 tag is load-bearing here.
    const auto point_packet = [](float x, float y) {
        std::vector<uint32_t> words = {0x40000000u};
        append_float(words, x);
        append_float(words, y);
        words.push_back(0x00000000u);
        words.push_back(0x00000000u);
        return words;
    };
    const auto submit = [&](m2host::GuestCltBridge& bridge,
                            const std::vector<uint32_t>& words) {
        const std::vector<uint8_t> bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, list_base,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
    };
    const auto append_shaded_triangle = [](std::vector<uint32_t>& words) {
        words.push_back(0x20090002u);  // NEW shaded strip, three vertices
        const float vertices[][6] = {
            {10.f, 10.f, 1.f, 0.f, 0.f, 1.f},
            {12.f, 10.f, 0.f, 1.f, 0.f, 1.f},
            {10.f, 12.f, 0.f, 0.f, 1.f, 1.f},
        };
        for (const auto& vertex : vertices)
            for (const float value : vertex) append_float(words, value);
    };

    // Interpreter level: the refusal is recorded as unsupported and the list
    // stops without producing a draw.
    {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const std::vector<uint32_t> words = point_packet(64.f, 48.f);
        const m2host::CltExecutionResult result = interpreter.execute(
            words.data(), words.size());
        if (!expect(result.ok && result.unsupported == 1 &&
                        result.draws == 0 && backend.draws.empty(),
                    "point packet is an unsupported refusal, not malformed"))
            return false;
    }

    // Production bridge: unsupported == 1, malformed == 0, draws == 0. This
    // is the double count the issue reported.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        submit(bridge, point_packet(64.f, 48.f));
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.submitted == 1 && stats.dropped == 0 &&
                        stats.malformed == 0 && stats.unsupported == 1 &&
                        stats.draws == 0 && backend.draws.empty(),
                    "bridge charges a point packet to unsupported only"))
            return false;
    }

    // A point packet that follows real work still keeps the earlier draw and
    // still stays out of the malformed bucket, so the refusal is attributed
    // per packet and is not a verdict on the whole list.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        std::vector<uint32_t> words;
        append_shaded_triangle(words);
        const std::vector<uint32_t> point = point_packet(64.f, 48.f);
        words.insert(words.end(), point.begin(), point.end());
        submit(bridge, words);
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.malformed == 0 && stats.unsupported == 1 &&
                        stats.draws == 1 && backend.draws.size() == 1,
                    "mid-list point packet preserves prior draws"))
            return false;
    }

    // Negative control: a truncated register header really is malformed and
    // must not be reclassified as an unsupported request.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        submit(bridge, {0x10008054u});
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.malformed == 1 && stats.unsupported == 0 &&
                        stats.draws == 0,
                    "truncated register list is malformed, not unsupported"))
            return false;
    }

    // Negative control: an unrecognized packet type holds the other half of
    // the contract. It sits at the same place in the word stream as a point
    // packet but carries no recognized type tag, so it must land in malformed
    // and leave unsupported alone. Without this the point assertions above
    // could pass merely because every list in this test is refused somewhere.
    {
        RecordingBackend backend;
        m2host::GuestCltBridge bridge(&backend);
        submit(bridge, {0x50000000u, 0x00000000u});
        const m2host::GuestCltStats stats = bridge.stats();
        if (!expect(stats.malformed == 1 && stats.unsupported == 0 &&
                        stats.draws == 0,
                    "unrecognized packet type stays malformed"))
            return false;
    }

    return true;
}

bool test_bridge()
{
    {
        RecordingBackend split_backend;
        m2host::GuestCltBridge split_bridge(&split_backend);
        const std::vector<uint32_t> texture_setup = {
            0x10006404u, 0x00000100u,  // uncompressed MMDMA load
            0x10006424u, 0x00000200u,
            0x10006414u, 0x00000000u,
            0x10006428u, 0x00000004u,
            0x1000642cu, 0x00010000u,  // UMAX=1, VMAX=0 -> 2x1
            0x1000643cu, 0x00001605u,  // literal RGB555 + SSB
            0x10006408u, 0x00002000u,
            0x1000640cu, 0x00000000u,
            0x10006410u, 0x00009200u,
        };
        const std::vector<uint32_t> texture_execute = {
            0x10000014u, 0x00000001u,  // TLD in a later list
            0x200a0002u,                // new, texture-only, three-vertex strip
        };
        const float texture_vertices[][4] = {
            {0.f, 0.f, 0.f, 0.f},
            {2.f, 0.f, 2.f, 0.f},
            {0.f, 1.f, 0.f, 1.f},
        };
        std::vector<uint32_t> complete_texture_execute = texture_execute;
        for (const auto& vertex : texture_vertices)
            for (float value : vertex)
                append_float(complete_texture_execute, value);

        std::vector<uint8_t> split_memory(0x204u, 0u);
        split_memory[0x200u] = 0xfcu;
        split_memory[0x201u] = 0x00u;
        split_memory[0x202u] = 0x03u;
        split_memory[0x203u] = 0xe0u;
        const std::vector<uint8_t> setup_bytes = big_endian_bytes(texture_setup);
        const std::vector<uint8_t> execute_bytes =
            big_endian_bytes(complete_texture_execute);
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(setup_bytes.size()), setup_bytes.data(),
            split_memory.data(), static_cast<uint32_t>(split_memory.size()));
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(execute_bytes.size()), execute_bytes.data(),
            split_memory.data(), static_cast<uint32_t>(split_memory.size()));
        split_bridge.pump();
        const m2host::GuestCltStats split_stats = split_bridge.stats();
        if (!expect(split_stats.submitted == 2 && split_stats.unsupported == 0 &&
                        split_stats.draws == 1 &&
                        split_backend.uploads.size() == 1,
                    "cross-list texture state snapshots its source"))
            return false;
    }

    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);
    const std::vector<uint32_t> words = shaded_triangle();
    const std::vector<uint8_t> bytes = big_endian_bytes(words);
    m2_guest_graphics_submit_c(0x2003u, 0x000d0003u, 0u, 0x4000a0u,
                               static_cast<uint32_t>(bytes.size()),
                               bytes.data(), nullptr, 0u);
    bridge.pump();

    const m2host::GuestCltStats stats = bridge.stats();
    if (!expect(stats.submitted == 1 && stats.dropped == 0 &&
                    stats.queue_full_drops == 0 &&
                    stats.peak_queued_lists == 1 &&
                    stats.peak_pump_batch == 1,
                "guest callback queues a copied list")) return false;
    if (!expect(stats.draws == 1 && backend.draws.size() == 1,
                "queued big-endian list executes")) return false;
    if (!expect(backend.draws[0].vertices[0].x == 10.f,
                "big-endian vertex words decode correctly")) return false;

    bridge.update_graphics_state(0x6043u, 0x000d0004u, 0x5048u,
                                 0x5048u, 0x5033u, 0x5034u, 32, 24, 576,
                                 432, 800, 600);
    if (!expect(bridge.graphics_state_for_testing().zbuffer_item == 0x5034u,
                "published Z-buffer Item is retained"))
        return false;
    bridge.update_graphics_state(0x6043u, 0x000d0005u, 0xffffffffu,
                                 0x5048u, 0x5033u, 0u, 32, 24, 576, 432,
                                 800, 600);
    if (!expect(bridge.graphics_state_for_testing().zbuffer_item == 0u,
                "cleared Z-buffer Item is removed"))
        return false;
    m2_guest_graphics_submit_c(0x2003u, 0x000d0003u, 0u, 0x4000a0u,
                               static_cast<uint32_t>(bytes.size()),
                               bytes.data(), nullptr, 0u);
    bridge.pump();
    if (!expect(backend.draws.size() == 2,
                "second queued list executes with View state")) return false;
    if (!expect(backend.draws[1].vertices[0].x == 42.f &&
                    backend.draws[1].vertices[0].y == 44.f,
                "View origin is applied to CLT pixel coordinates")) return false;
    if (!expect(backend.draws[1].target_w == 800.f &&
                    backend.draws[1].target_h == 600.f,
                "published View display dimensions scale CLT draws"))
        return false;

    // A full queue must pause the guest callback until the render thread
    // retires a batch. Dropping one stateful list is not an acceptable
    // transport behavior because later lists depend on its register state.
    for (size_t i = 0; i < 64u; ++i) {
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(bytes.size()), bytes.data(), nullptr, 0u);
    }
    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(bytes.size()), bytes.data(), nullptr, 0u);
        producer_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 100 &&
                    !producer_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool producer_blocked =
        !producer_done.load(std::memory_order_acquire);
    if (!producer_blocked) {
        bridge.pump();
        producer.join();
        return expect(false, "full CLT queue applies backpressure");
    }
    bridge.pump();
    producer.join();
    bridge.pump();
    const m2host::GuestCltStats backpressure_stats = bridge.stats();
    if (!expect(backpressure_stats.submitted == 67 &&
                    backpressure_stats.dropped == 0 &&
                    backpressure_stats.queue_full_drops == 0 &&
                    backpressure_stats.peak_queued_lists == 64,
                "full CLT queue drains without dropping submissions"))
        return false;

    bridge.update_te_surface_state(0x7u, 0x024001b0u, 0u, 0x4f0000u,
                                   576u, 0x56a000u, 0x024001b0u, 0u,
                                   0x3f000000u, 0x3f000000u,
                                   0x3f000000u, 0x3f800000u);
    m2_guest_graphics_submit_c(0x2003u, 0x000d0003u, 0x2u, 0u, 0u,
                               nullptr, nullptr, 0u);
    bridge.pump();
    if (!expect(backend.z_clears == 1 && backend.last_z_fill_word == 0u,
                "empty TE clear-Z request reaches the active surface"))
        return false;

    bridge.enqueue(0x2003u, 0x000d0003u, 0u, 0x4000a0u, 3u,
                   bytes.data(), nullptr, 0u);
    if (!expect(bridge.stats().dropped == 1,
                "unaligned payload is dropped"))
        return false;

    // Detach must also release a callback blocked on a full queue. This
    // exercises the shutdown ordering separately from normal pump progress.
    for (size_t i = 0; i < 64u; ++i) {
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(bytes.size()), bytes.data(), nullptr, 0u);
    }
    producer_done.store(false, std::memory_order_release);
    std::thread shutdown_producer([&] {
        m2_guest_graphics_submit_c(
            0x2003u, 0x000d0003u, 0u, 0x4000a0u,
            static_cast<uint32_t>(bytes.size()), bytes.data(), nullptr, 0u);
        producer_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 100 &&
                    !producer_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool shutdown_blocked =
        !producer_done.load(std::memory_order_acquire);
    bridge.detach();
    shutdown_producer.join();
    const m2host::GuestCltStats shutdown_stats = bridge.stats();
    return expect(shutdown_blocked && shutdown_stats.queue_full_drops == 0 &&
                      shutdown_stats.dropped == 2,
                  "detach releases a blocked CLT callback");
}

bool test_clt_w16_packed_quantization()
{
    // M2_CLT_DEPTH_W16_PACKED probe only: patent candidate quantization/tie
    // rule (WO1996036011A1 block-fix 16-bit example, contracts "Raw 16-bit
    // W-cell evidence"): cell = (range << 14) | floor(one_over_w * 2^(3 *
    // range) * 2^14), saturating the 14-bit significand at 0x3FFF. Exact
    // .5-step ties encode down, never round-to-nearest. Cross-range ordering
    // uses the decoded (range, significand) value, never raw u16 (ranges
    // deliberately reverse raw order). Production reciprocal-W path untouched.
    using m2host::clt_w16_packed_probe::compare;
    using m2host::clt_w16_packed_probe::decode;
    using m2host::clt_w16_packed_probe::encode;
    if (!expect(encode(0.5f, 0u) == 0x2000u,
                "packed probe encodes 0.5/range0 as (0<<14)|8192"))
        return false;
    // Exact tie halfway between significand steps 8192 and 8193: ldexp by a
    // power of two is exact, so scaled is exactly 8192.5 and floor keeps 8192.
    const float tie = std::ldexp(8192.5f, -14);
    if (!expect(encode(tie, 0u) == 0x2000u,
                "packed probe tie encodes down (floor, not nearest)"))
        return false;
    // The next exact step encodes up, proving the tie sat on the boundary.
    const float next_step = std::ldexp(8193.f, -14);
    if (!expect(encode(next_step, 0u) == 0x2001u,
                "packed probe encodes the next significand step up"))
        return false;
    if (!expect(decode(encode(0.5f, 0u)) == 0.5f,
                "packed probe decode inverts encode at 0.5/range0"))
        return false;
    // 14-bit saturation: 1.0 needs significand 16384, which does not fit.
    if (!expect(encode(1.0f, 0u) == 0x3fffu,
                "packed probe saturates 1.0/range0 at 0x3FFF"))
        return false;
    if (!expect(encode(2.0f, 0u) == 0x3fffu,
                "packed probe saturates overflow at 0x3FFF"))
        return false;
    if (!expect(encode(0.0f, 0u) == 0u && encode(-1.0f, 0u) == 0u,
                "packed probe maps non-positive 1/w to cell 0"))
        return false;
    if (!expect(encode(INFINITY, 0u) == 0u,
                "packed probe maps +inf 1/w to cell 0"))
        return false;
    if (!expect(encode(NAN, 0u) == 0u,
                "packed probe maps NaN 1/w to cell 0"))
        return false;
    // Range-field placement without saturation: significand 131 in range 1
    // (131/2^17 is exact) lands at (1<<14)|131.
    const float range1_value = std::ldexp(131.f, -17);
    if (!expect(encode(range1_value, 1u) == 0x4083u,
                "packed probe places the range field at bits 15:14"))
        return false;
    // Cross-range ordering fixture: raw u16 says 0x3FFF < 0x4001, but decoded
    // block-fix values say the opposite (range 0 s=max ~= 0.99994 vs range 1
    // s=1 ~= 7.6e-6). The probe MUST follow the decoded order.
    if (!expect(0x3fffu < 0x4001u, "cross-range raw-u16 fixture order"))
        return false;
    if (!expect(decode(0x3fffu) > decode(0x4001u),
                "packed probe decode reverses raw order across ranges"))
        return false;
    if (!expect(compare(0x3fffu, 0x4001u) > 0,
                "packed probe compares decoded value, never raw u16"))
        return false;
    if (!expect(compare(0x4001u, 0x3fffu) < 0,
                "packed probe compare is antisymmetric across ranges"))
        return false;
    if (!expect(compare(0x2000u, 0x2000u) == 0,
                "packed probe compare reports equal cells"))
        return false;
    if (!expect(compare(0x0000u, 0x4000u) == 0 &&
                    decode(0x4000u) == 0.f,
                "packed probe treats zero significand as zero in any range"))
        return false;
    // Integer-exact near-tie (diagnostic-only): adjacent significand steps in
    // range 3 differ by 2^-23 (~1.19e-7), far below the old 1e-5 float
    // epsilon, so an epsilon compare would conflate them while the integer
    // s*2^(9-3r) ordering separates them. Range 3 base 0xC000, sig 1 vs 2.
    if (!expect(compare(0xc001u, 0xc002u) < 0,
                "packed probe orders adjacent range-3 steps"))
        return false;
    if (!expect(compare(0xc002u, 0xc001u) > 0,
                "packed probe compare is antisymmetric on near-tie steps"))
        return false;
    if (!expect(std::fabs(decode(0xc001u) - decode(0xc002u)) < 1e-5f,
                "packed near-tie float gap sits inside the old epsilon"))
        return false;
    if (!expect(compare(0xc001u, 0xc001u) == 0,
                "packed probe reports equal near-tie cells"))
        return false;
    return true;
}

bool test_z_cell_dump()
{
    // M2_CLT_Z_CELL_DUMP probe only: offset/format math for the bounded
    // raw-guest-Z readout at z_base + 2*(x+y*stride). Synthetic bound base
    // 0x56a000 (the observed race-window default); no guest run needed.
    using m2host::clt_z_cell_probe::cell_address;
    using m2host::clt_z_cell_probe::cell_offset_bytes;
    using m2host::clt_z_cell_probe::coords_for_index;
    using m2host::clt_z_cell_probe::decode_be;
    using m2host::clt_z_cell_probe::decode_le;
    using m2host::clt_z_cell_probe::dump_span_bytes;
    using m2host::clt_z_cell_probe::format_cell_line;
    using m2host::clt_z_cell_probe::format_header_line;
    using m2host::clt_z_cell_probe::format_unmapped_line;
    using m2host::clt_z_cell_probe::window_width;
    if (!expect(cell_offset_bytes(0u, 0u, 576u) == 0u,
                "z-cell origin offset is zero")) return false;
    if (!expect(cell_offset_bytes(3u, 0u, 576u) == 6u,
                "z-cell x steps by 2 bytes")) return false;
    if (!expect(cell_offset_bytes(0u, 1u, 576u) == 1152u,
                "z-cell y steps by 2*stride bytes")) return false;
    if (!expect(cell_offset_bytes(2u, 3u, 640u) == 2u * (2u + 3u * 640u),
                "z-cell offset folds x and y with the stride")) return false;
    if (!expect(cell_address(0x56a000u, 1u, 2u, 576u) ==
                    0x56a000u + 2u * (1u + 2u * 576u),
                "z-cell address folds the synthetic bound base")) return false;
    if (!expect(decode_be(0x12u, 0x34u) == 0x1234u,
                "z-cell BE reading takes stored order")) return false;
    if (!expect(decode_le(0x12u, 0x34u) == 0x3412u,
                "z-cell LE reading swaps stored order")) return false;
    if (!expect(window_width(576u) == 8u && window_width(4u) == 4u &&
                    window_width(1u) == 1u,
                "z-cell window tiles 8-wide within the stride")) return false;
    {
        uint32_t x = 99u, y = 99u;
        coords_for_index(0u, 576u, &x, &y);
        if (!expect(x == 0u && y == 0u,
                    "z-cell index 0 maps to the window origin")) return false;
        coords_for_index(7u, 576u, &x, &y);
        if (!expect(x == 7u && y == 0u,
                    "z-cell index 7 ends the first row")) return false;
        coords_for_index(8u, 576u, &x, &y);
        if (!expect(x == 0u && y == 1u,
                    "z-cell index 8 wraps to the next stride row")) return false;
    }
    if (!expect(dump_span_bytes(1u, 576u) == 2u,
                "z-cell single-word span is 2 bytes")) return false;
    if (!expect(dump_span_bytes(16u, 576u) == 2u * (576u + 8u),
                "z-cell 16-word span covers two 8-wide stride rows")) return false;
    {
        char line[192];
        const int count = format_cell_line(line, sizeof(line), 9u, 1u, 2u,
                                           0x56a902u, 0x12u, 0x34u);
        if (!expect(count > 0, "z-cell cell line formats")) return false;
        if (!expect(std::strcmp(line, "[clt-z-cell] list=9 x=1 y=2 "
                                       "addr=0x56a902 raw=12.34 be=0x1234 "
                                       "le=0x3412") == 0,
                    "z-cell cell line prints raw bytes plus BE/LE readings")) return false;
        if (!expect(format_header_line(line, sizeof(line), 9u, 0x56a000u,
                                        576u, 16u) > 0 &&
                        std::strcmp(line, "[clt-z-cell] list=9 base=0x56a000 "
                                           "stride=576 words=16") == 0,
                    "z-cell header line carries base/stride/words")) return false;
        if (!expect(format_unmapped_line(line, sizeof(line), 9u, 0x56a000u,
                                          576u, 16u) > 0 &&
                        std::strcmp(line, "[clt-z-cell] list=9 base=0x56a000 "
                                           "stride=576 words=16 unmapped") == 0,
                    "z-cell unmapped line fails closed in one line")) return false;
    }
    // Snapshot-path regression: the enqueue path must snapshot exactly the
    // dump window so the once-fire reads guest Z cells instead of reporting
    // unmapped. This is the first dump_word_count call in the process
    // (earlier tests are pure math), so seeding the variable here takes
    // effect; a forced-off DUMP (0) skips the live section. The stderr
    // capture is POSIX-only; the clamp/span CHECKs run everywhere.
    if (std::getenv("M2_CLT_Z_CELL_DUMP") == nullptr) {
#if defined(_WIN32)
        _putenv("M2_CLT_Z_CELL_DUMP=16");
#else
        ::setenv("M2_CLT_Z_CELL_DUMP", "16", 1);
#endif
    }
    const uint32_t live_words =
        m2host::clt_z_cell_probe::dump_word_count();
    if (!expect(live_words <= 256u, "z-cell live dump count is clamped"))
        return false;
    if (live_words != 0) {
        const uint32_t live_stride = 576u;
        const size_t live_span =
            m2host::clt_z_cell_probe::dump_span_bytes(live_words,
                                                      live_stride);
        if (!expect(live_span != 0 &&
                        live_span <= 4u * 1024u * 1024u,
                    "z-cell live window span is bounded and non-empty"))
            return false;
#if !defined(_WIN32)
        RecordingBackend live_backend;
        m2host::GuestCltBridge live_bridge(&live_backend);
        live_bridge.update_te_surface_state(0x7u, 0x024001b0u, 0u, 0x4f0000u,
                                            live_stride, 0x56a000u,
                                            0x024001b0u, 0u,
                                            0x3f000000u, 0x3f000000u,
                                            0x3f000000u, 0x3f800000u);
        std::vector<uint8_t> live_ram(0x56a000u + live_span + 64u, 0u);
        for (size_t i = 0; i < live_span; ++i)
            live_ram[0x56a000u + i] =
                static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
        const std::vector<uint8_t> live_bytes =
            big_endian_bytes(shaded_triangle());
        std::fflush(stderr);
        const int saved_stderr = ::dup(::fileno(stderr));
        if (!expect(saved_stderr >= 0, "z-cell capture dup works"))
            return false;
        FILE* live_capture = std::tmpfile();
        if (live_capture == nullptr) {
            ::close(saved_stderr);
            return expect(false, "z-cell capture tmpfile works");
        }
        ::dup2(::fileno(live_capture), ::fileno(stderr));
        live_bridge.enqueue(0x2003u, 0x000d0003u, 0u, 0x4000a0u,
                            static_cast<uint32_t>(live_bytes.size()),
                            live_bytes.data(), live_ram.data(),
                            static_cast<uint32_t>(live_ram.size()));
        live_bridge.pump();
        std::fflush(stderr);
        ::dup2(saved_stderr, ::fileno(stderr));
        ::close(saved_stderr);
        std::rewind(live_capture);
        std::string live_out;
        char live_chunk[512];
        size_t live_read = 0;
        while ((live_read = std::fread(live_chunk, 1, sizeof(live_chunk),
                                       live_capture)) > 0)
            live_out.append(live_chunk, live_read);
        std::fclose(live_capture);
        char live_header[192];
        std::snprintf(live_header, sizeof(live_header),
                      "[clt-z-cell] list=0 base=0x56a000 stride=%u words=%u",
                      live_stride, live_words);
        if (!expect(live_out.find(live_header) != std::string::npos,
                    "z-cell dump prints the mapped header")) return false;
        if (!expect(live_out.find("unmapped") == std::string::npos,
                    "z-cell dump does not report unmapped")) return false;
        char live_cell0[192];
        std::snprintf(live_cell0, sizeof(live_cell0),
                      "[clt-z-cell] list=0 x=0 y=0 addr=0x56a000 raw=%02x.%02x",
                      live_ram[0x56a000u], live_ram[0x56a000u + 1u]);
        if (!expect(live_out.find(live_cell0) != std::string::npos,
                    "z-cell dump carries live guest bytes")) return false;
        if (live_words > 8u) {
            const uint32_t live_row1 = 0x56a000u + 2u * live_stride;
            char live_cell8[192];
            std::snprintf(
                live_cell8, sizeof(live_cell8),
                "[clt-z-cell] list=0 x=0 y=1 addr=0x%x raw=%02x.%02x",
                live_row1, live_ram[live_row1], live_ram[live_row1 + 1u]);
            if (!expect(live_out.find(live_cell8) != std::string::npos,
                        "z-cell dump wraps to the next stride row"))
                return false;
        }
#endif
    }
    return true;
}

bool test_exec_seq_advances()
{
    // Executed-list calibration (contracts "Race-window Z-surface traffic"):
    // result.execute_seq stamps the list's own number and the number advances
    // exactly once per execute() call while a list-gated diagnostic is armed
    // (m2host::clt_exec_seq_live); otherwise the counter stays frozen at zero
    // so the default-on in-progress DB-source snapshot key behaves
    // bit-identically in production runs. Runs after test_z_cell_dump so the
    // DUMP/FROM env caches are already warm; expectations read the same live
    // predicate the interpreter uses, so they hold in every environment. No
    // stderr dependence (the one-shot dump latch belongs to the earlier live
    // section; these calls print nothing either way).
    RecordingBackend backend;
    m2host::GuestCltInterpreter interpreter(&backend);
    const bool live = m2host::clt_exec_seq_live();
    const std::vector<uint32_t> empty;
    for (uint64_t i = 0; i < 3u; ++i) {
        const m2host::CltExecutionResult result =
            interpreter.execute(empty.data(), empty.size());
        if (!expect(result.ok, "exec-seq probe list executes cleanly"))
            return false;
        const uint64_t want = live ? i : 0u;
        if (!expect(result.execute_seq == want,
                    "exec-seq stamps each executed list exactly once"))
            return false;
    }
    return true;
}

bool test_z_cell_packed_round_trip()
{
    // Patent-backed packed candidate end to end (contracts "Raw 16-bit
    // W-cell evidence", WO1996036011A1 block-fix 16-bit example): logical
    // word (range << 14) | significand -> [hi, lo] guest-order bytes at
    // z_base + 2*(x+y*stride) -> BE reading recovers the word and decodes
    // back; the LE reading is the byteswapped alias and decodes differently.
    // Candidate only: production depth never consumes these helpers.
    using m2host::clt_w16_packed_probe::compare;
    using m2host::clt_w16_packed_probe::decode;
    using m2host::clt_w16_packed_probe::encode;
    using m2host::clt_z_cell_probe::cell_address;
    using m2host::clt_z_cell_probe::cell_offset_bytes;
    using m2host::clt_z_cell_probe::decode_be;
    using m2host::clt_z_cell_probe::decode_le;
    using m2host::clt_z_cell_probe::encode_be;
    // Range 0, 1/w = 0.5: word 0x2000, stored guest bytes [0x20, 0x00].
    const uint16_t range0_word = encode(0.5f, 0u);
    if (!expect(range0_word == 0x2000u,
                "packed round-trip encodes 0.5/range0 as 0x2000"))
        return false;
    if (!expect((range0_word >> 14u) == 0u &&
                    (range0_word & 0x3fffu) == 8192u,
                "packed round-trip exposes range bits 15:14 + 14-bit significand"))
        return false;
    const uint8_t hi = static_cast<uint8_t>(range0_word >> 8u);
    const uint8_t lo = static_cast<uint8_t>(range0_word & 0xffu);
    // PPC-natural BE write helper: the LE host MUST use encode_be for any
    // future guest-Z write so [hi,lo] lands via the WORD2_XOR_BE bus
    // swizzle; mirrors decode_be/decode_le.
    uint8_t be_hi = 0u, be_lo = 0u;
    encode_be(range0_word, &be_hi, &be_lo);
    if (!expect(be_hi == 0x20u && be_lo == 0x00u,
                "packed BE write stores 0x2000 as [0x20,0x00]"))
        return false;
    if (!expect(be_hi == hi && be_lo == lo,
                "packed BE helper matches the manual hi/lo split"))
        return false;
    if (!expect(decode_be(be_hi, be_lo) == range0_word,
                "packed BE helper round-trips through decode_be"))
        return false;
    if (!expect(decode_le(be_hi, be_lo) == 0x0020u,
                "packed BE helper byteswapped alias is decode_le"))
        return false;
    // Cell (3, 2) of the synthetic bound plane (0x56a000 race-window default,
    // 576 stride): offset math locates the stored pair.
    const uint32_t cell = 3u + 2u * 576u;
    if (!expect(cell_offset_bytes(3u, 2u, 576u) == 2u * cell,
                "packed round-trip offsets by z_base + 2*(x+y*stride)"))
        return false;
    if (!expect(cell_address(0x56a000u, 3u, 2u, 576u) == 0x56a000u + 2u * cell,
                "packed round-trip addresses the cell from the bound base"))
        return false;
    // Guest-order bytes through the host byteswap pair.
    if (!expect(decode_be(hi, lo) == range0_word,
                "packed round-trip BE reading recovers the stored word"))
        return false;
    const uint16_t le_alias = decode_le(hi, lo);
    if (!expect(le_alias == 0x0020u,
                "packed round-trip LE reading is the byteswapped alias"))
        return false;
    if (!expect(decode(decode_be(hi, lo)) == 0.5f,
                "packed round-trip BE value decodes back to 0.5"))
        return false;
    if (!expect(decode(le_alias) != decode(range0_word),
                "packed round-trip byte order changes the decoded value"))
        return false;
    if (!expect(compare(range0_word, le_alias) > 0,
                "packed round-trip orders the BE value above its LE alias"))
        return false;
    // Range 1, significand 131 (131/2^17 is exact): word 0x4083. Raw u16
    // order says 0x4083 > 0x3FFF, but the decoded block-fix values say the
    // range-1 cell is the smaller depth.
    const float range1_value = std::ldexp(131.f, -17);
    const uint16_t range1_word = encode(range1_value, 1u);
    if (!expect(range1_word == 0x4083u,
                "packed round-trip encodes the range-1 fixture as 0x4083"))
        return false;
    if (!expect(decode(range1_word) == range1_value,
                "packed round-trip inverts the range-1 encoding exactly"))
        return false;
    if (!expect(compare(range1_word, 0x3fffu) < 0,
                "packed round-trip compares decoded value, never raw u16"))
        return false;
    return true;
}

bool test_z_clip_region_helpers()
{
    // Recovered TE Z region (vendor hw.c:SetTEZBuffer -> DBZCLIP, signed
    // DBZOFFSET) plus DBDISCARDCONTROL.ZCLIPPED. MAME destination_blend
    // leaves the region inactive without ZBUFEN and clips pixels outside
    // [0, clip); outside pixels skip the Z test/write and are discarded only
    // when the ZCLIPPED bit is set.
    m2host::CltDepth depth;
    depth.z_region_min_x = 4;
    depth.z_region_max_x = 8;
    depth.z_region_min_y = 0;
    depth.z_region_max_y = 8;
    if (!expect(!m2host::clt_z_region_active(depth),
                "z region inactive without ZBUFEN"))
        return false;
    if (!expect(!m2host::clt_z_clipped(depth, 0.f, 0.f),
                "z region does not clip without ZBUFEN"))
        return false;
    depth.test_enabled = true;
    depth.z_clipped_discard = true;
    if (!expect(m2host::clt_z_region_contains(depth, 4.f, 0.f),
                "z region includes its lower bound"))
        return false;
    if (!expect(!m2host::clt_z_region_contains(depth, 8.f, 0.f),
                "z region excludes its upper bound"))
        return false;
    if (!expect(m2host::clt_z_clipped(depth, 3.f, 0.f),
                "z region clips below its lower bound"))
        return false;
    if (!expect(m2host::clt_z_clipped_discard(depth, 3.f, 0.f),
                "ZCLIPPED discards an outside-region pixel"))
        return false;
    if (!expect(!m2host::clt_z_clipped_discard(depth, 5.f, 0.f),
                "ZCLIPPED leaves an inside-region pixel alone"))
        return false;
    if (!expect(!m2host::clt_z_clipped(depth, 5.f, 0.f),
                "z test still applies inside the region"))
        return false;

    // clip \ region must partition the clip into disjoint bands exactly.
    const m2host::CltClipRect clip{0.f, 0.f, 10.f, 10.f};
    const m2host::CltClipRect region{2.f, 2.f, 4.f, 4.f};
    m2host::CltClipRect out[4]{};
    const int count = m2host::clt_clip_rect_subtract(clip, region, out);
    if (!expect(count == 4, "zclip rect subtract splits four bands"))
        return false;
    float coverage[10][10] = {};
    for (int i = 0; i < count; ++i) {
        for (int y = static_cast<int>(out[i].y);
             y < static_cast<int>(out[i].y + out[i].h); ++y) {
            for (int x = static_cast<int>(out[i].x);
                 x < static_cast<int>(out[i].x + out[i].w); ++x) {
                if (x >= 0 && x < 10 && y >= 0 && y < 10) coverage[y][x] += 1.f;
            }
        }
    }
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            const bool in_region = x >= 2 && x < 6 && y >= 2 && y < 6;
            if (!expect(coverage[y][x] == (in_region ? 0.f : 1.f),
                        "zclip rect subtract covers the complement once"))
                return false;
        }
    }
    const m2host::CltClipRect full{0.f, 0.f, 20.f, 20.f};
    if (!expect(m2host::clt_clip_rect_subtract(clip, full, out) == 0,
                "covered clip yields no outside rects"))
        return false;
    const m2host::CltClipRect empty_region{5.f, 5.f, 0.f, 0.f};
    if (!expect(m2host::clt_clip_rect_subtract(clip, empty_region, out) == 2,
                "empty region decomposes into full-width bands"))
        return false;
    return true;
}

bool test_z_clip_backend()
{
    // Software-backend contract: DBZCLIP=(4,8), DBZOFFSET=0, DBZCNTL=0x1
    // (pixel output only on smaller Z, i.e. larger reciprocal W). Stored Z is
    // 1.0; a far 0.5 triangle must be rejected inside the region but drawn
    // color-only outside it, without writing the skipped Z cell.
    m2host::SoftCaptureBackend backend(8, 8, {});
    backend.frame_begin(0.25f, 0.f, 0.f);
    backend.clt_clear_z(0x3f800000u);  // 1.0f

    std::array<m2host::CltVertex, 3> far_tri{};
    far_tri[0] = {0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    far_tri[1] = {8.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    far_tri[2] = {0.f, 8.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.5f};
    m2host::CltDepth depth;
    depth.test_enabled = true;
    depth.write_enabled = false;
    depth.depth_func = m2host::kCltDbzPixelLt;
    depth.z_region_min_x = 0;
    depth.z_region_max_x = 4;
    depth.z_region_min_y = 0;
    depth.z_region_max_y = 8;
    backend.clt_draw(far_tri.data(), 3, false, false, m2host::kInvalidCel,
                     8.f, 8.f, false, {}, depth, {}, {});

    const uint32_t clear_rgba = 64u | 0xff000000u;
    const uint32_t green_rgba = 0xff00ff00u;  // r|g<<8|b<<16|a<<24
    std::array<uint32_t, 64> frame{};
    if (!expect(backend.grab_presented_rgba(frame.data(), 8, 8),
                "zclip backend exposes its frame"))
        return false;
    if (!expect(frame[0] == clear_rgba,
                "inside-region pixel keeps the DBZCNTL reject"))
        return false;
    if (!expect(frame[0 * 8 + 6] == green_rgba,
                "outside-region pixel skips the Z test and draws color"))
        return false;

    // Same-frame discriminator: a nearer 0.75 red triangle over the whole
    // target passes against a stored 0.5 (the value the outside-region pixel
    // would hold if the skipped Z write had happened) but is rejected by the
    // retained 1.0. The outside pixel must therefore stay green.
    std::array<m2host::CltVertex, 3> nearer_tri = far_tri;
    for (auto& vertex : nearer_tri) {
        vertex.one_over_w = 0.75f;
        vertex.r = 1.f;
        vertex.g = 0.f;
    }
    depth.z_region_max_x = 8;
    backend.clt_draw(nearer_tri.data(), 3, false, false, m2host::kInvalidCel,
                     8.f, 8.f, false, {}, depth, {}, {});
    if (!expect(backend.grab_presented_rgba(frame.data(), 8, 8),
                "zclip backend exposes the follow-up frame"))
        return false;
    if (!expect(frame[0 * 8 + 6] == green_rgba,
                "outside-region draw skipped the Z write"))
        return false;

    // DBDISCARDCONTROL.ZCLIPPED drops the outside-region pixel entirely while
    // the inside pixel still draws (DBZCNTL=0x3f passes against clear 0.0).
    backend.frame_begin(0.25f, 0.f, 0.f);
    backend.clt_clear_z(0u);  // 0.0f
    depth.depth_func = m2host::kCltDbzAll;
    depth.z_region_max_x = 4;
    depth.z_clipped_discard = true;
    backend.clt_draw(far_tri.data(), 3, false, false, m2host::kInvalidCel,
                     8.f, 8.f, false, {}, depth, {}, {});
    if (!expect(backend.grab_presented_rgba(frame.data(), 8, 8),
                "zclip backend exposes the discard frame"))
        return false;
    if (!expect(frame[0] == green_rgba,
                "inside-region pixel draws with ZCLIPPED armed"))
        return false;
    if (!expect(frame[0 * 8 + 6] == clear_rgba,
                "ZCLIPPED discards the outside-region pixel"))
        return false;
    return true;
}

std::vector<uint32_t> z_clip_triangle_list(bool write_offset)
{
    std::vector<uint32_t> words = {
        0x10008008u, 0x00000600u,  // DBUSERCONTROL = ZBUFEN | ZOUTEN
        0x10008040u, 0x0000000fu,  // DBZCNTL
        0x1000804cu, 0x00400040u,  // DBZCLIP = (x=64, y=64)
        0x1000800cu, 0x00000008u,  // DBDISCARDCONTROL = ZCLIPPED
    };
    if (write_offset) {
        words.push_back(0x10008048u);
        // DBZOFFSET: X=+2 (bits 31:16), Y=-1 (signed 12-bit 0xfff).
        words.push_back(0x00020fffu);
    }
    words.push_back(0x20090002u);  // new, shaded, strip, three vertices
    const float vertices[][6] = {
        {1.f, 1.f, 1.f, 0.f, 0.f, 1.f},
        {3.f, 1.f, 0.f, 1.f, 0.f, 1.f},
        {1.f, 3.f, 0.f, 0.f, 1.f, 1.f},
    };
    for (const auto& vertex : vertices)
        for (float value : vertex) append_float(words, value);
    return words;
}

bool test_shadow_color_z_alias_interpreter()
{
    // The original setup redirects the framebuffer Item to the Z bitmap;
    // no track ID, callback address or fixed VRAM address selects the fix.
    for (unsigned variant=0; variant<8; ++variant) {
        RecordingBackend backend;
        m2host::GuestCltInterpreter interpreter(&backend);
        const uint32_t base=0x23000;
        std::vector<uint32_t> words = {
            0x10008008u, variant==5 ? 0x61fu : 0x60fu,
            0x10008024u, variant==2 ? 1u : 0u,
            0x10008028u, variant==1 ? base+0x1000 : base,
            0x1000802cu, variant==4 ? 32u : 64u,
            0x10008044u, base,
            0x10008048u, variant==3 ? 1u : 0u,
            0x1000804cu, 0x00400040u,
            0x10008050u, variant==6 ? 5u : variant==7 ? 1u : 0u,
            0x10008040u, 1u,
            0x10006408u, 0u, 0x10006410u, 0u,
            0x200d0002u,
        };
        const auto add=[&](float x,float y) {
            for (float f : {x,y,31.f/64,1.f,1.f,1.f,.003f})
                words.push_back(std::bit_cast<uint32_t>(f));
        };
        add(0,0); add(32,0); add(0,32);
        words.push_back(0x20050000u); add(32,32); // continuation
        const auto result=interpreter.execute(words.data(),words.size());
        if (!expect(result.ok && result.unsupported==0 && backend.draws.size()==2,
                    "shadow alias NEW and continuation are accepted")) return false;
        for (const auto& draw : backend.draws) {
            const bool wanted=variant==0 || variant==7;
            if (!expect(draw.depth.color_target_is_z==wanted,
                        "shadow alias respects address, format, stride, offset, dither and DSB")) return false;
            if (!expect(m2host::clt_near_color_z_fill(draw.depth,draw.vertices.data(),
                        static_cast<uint32_t>(draw.vertices.size()),false,false)==wanted,
                        "authored RGB555 near color reaches the depth backend")) return false;
        }
    }
    m2host::CltDepth alias{}; alias.color_target_is_z=true;
    m2host::CltVertex v{}; v.r=31.f/64; v.g=v.b=v.a=1;
    if (!expect(!m2host::clt_near_color_z_fill(alias,&v,1,true,false) &&
                !m2host::clt_near_color_z_fill(alias,&v,1,false,true),
                "textured and blended colors cannot masquerade as a near fill")) return false;
    v.r=1;
    return expect(!m2host::clt_near_color_z_fill(alias,&v,1,false,false),
                  "other aliased colors do not invent a depth encoding");
}

bool test_z_clip_interpreter()
{
    // DBDISCARDCONTROL.ZCLIPPED must not fail closed: the interpreter
    // carries it through CltDepth together with the DBZCLIP/DBZOFFSET region
    // folded into the emitted vertex space (no View origin on this bridge).
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);
    const std::vector<uint32_t> base_words = z_clip_triangle_list(false);
    const std::vector<uint8_t> base_bytes = big_endian_bytes(base_words);
    m2_guest_graphics_submit_c(0x2003u, 0x000d0003u, 0u, 0x4000a0u,
                               static_cast<uint32_t>(base_bytes.size()),
                               base_bytes.data(), nullptr, 0u);
    bridge.pump();
    const m2host::GuestCltStats stats = bridge.stats();
    if (!expect(stats.unsupported == 0 && stats.draws == 1 &&
                    backend.draws.size() == 1,
                "ZCLIPPED draw is accepted, not rejected"))
        return false;
    const m2host::CltDepth& drawn = backend.draws[0].depth;
    if (!expect(drawn.z_clipped_discard,
                "interpreter carries DBDISCARDCONTROL.ZCLIPPED"))
        return false;
    if (!expect(drawn.z_region_min_x == 0 && drawn.z_region_max_x == 64 &&
                    drawn.z_region_min_y == 0 && drawn.z_region_max_y == 64,
                "DBZCLIP bounds reach the draw in vertex space"))
        return false;
    if (!expect(!backend.draws[0].discard.ssb_zero &&
                    !backend.draws[0].discard.alpha_zero &&
                    !backend.draws[0].discard.rgb_zero,
                "ZCLIPPED does not alias an SSB/RGB/alpha discard"))
        return false;

    // Signed DBZOFFSET: X=+2 and Y=-1 shift the region so the emitted
    // (View-translated) vertex space still satisfies guest + offset < clip.
    const std::vector<uint32_t> offset_words = z_clip_triangle_list(true);
    const std::vector<uint8_t> offset_bytes = big_endian_bytes(offset_words);
    m2_guest_graphics_submit_c(0x2003u, 0x000d0003u, 0u, 0x4000a0u,
                               static_cast<uint32_t>(offset_bytes.size()),
                               offset_bytes.data(), nullptr, 0u);
    bridge.pump();
    if (!expect(backend.draws.size() == 2,
                "offset ZCLIPPED draw reaches the backend"))
        return false;
    const m2host::CltDepth& offset = backend.draws[1].depth;
    return expect(offset.z_region_min_x == -2 &&
                      offset.z_region_max_x == 62 &&
                      offset.z_region_min_y == 1 &&
                      offset.z_region_max_y == 65,
                  "signed DBZOFFSET folds into the Z region bounds");
}

// The queue's own ordering and bound, independent of the caller's policy.
// main.cpp passes the frame the guest has reached rather than the last
// completed one, because a guest frame publishes its display units from its own
// synchronous GS_EndFrame; a unit tagged above that frame is still refused.
bool test_display_unit_selection()
{
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);

    bridge.publish_display_target(10u, 0x1000u);
    bridge.publish_display_target(11u, 0x2000u);
    bridge.publish_display_target(11u, 0x3000u);

    if (!expect(bridge.consume_display_target_upto(9u) == 0u,
                "a display unit above the caller's frame stays hidden"))
        return false;
    if (!expect(bridge.consume_display_target_upto(10u) == 0x1000u,
                "a display unit at the caller's frame is presented"))
        return false;
    if (!expect(bridge.consume_display_target_upto(10u) == 0u,
                "a consumed display unit is not replayed"))
        return false;
    // Two units published by the same guest frame are two presentations, in
    // guest order. Folding them into one "last choice wins" entry would drop
    // the earlier one, which is what collapsed the authored transitions.
    // The presented target must be attributable to a guest frame that reached a
    // loop head; without the tag a caller cannot show that a frame still being
    // built did not select the scanout.
    uint32_t consumed_tag = 0u;
    if (!expect(bridge.consume_display_target_upto(11u, &consumed_tag) ==
                    0x2000u,
                "the first same-frame display unit is presented first"))
        return false;
    if (!expect(consumed_tag == 11u,
                "the presented display unit reports the guest frame that "
                "published it"))
        return false;
    if (!expect(bridge.consume_display_target_upto(11u, &consumed_tag) ==
                    0x3000u,
                "the second same-frame display unit follows it"))
        return false;
    if (!expect(consumed_tag == 11u,
                "the second display unit reports the same publishing frame"))
        return false;
    uint32_t empty_tag = 0xffffffffu;
    if (!expect(bridge.consume_display_target_upto(12u, &empty_tag) == 0u,
                "an empty queue consumes no target"))
        return false;
    return expect(empty_tag == 0u,
                  "an empty queue reports no publishing frame");
}

// Regression for the authored state transition. `dotrans`'s effect bodies
// submit one DISPLAYFRAMEBUFFER per animation frame from inside a single guest
// application frame, so one tagged frame publishes a whole sequence. The
// presenter must surface every unit, in order, one per presentation: folding
// them to the frame's last choice collapses the authored transition to its
// final frame, which is the defect this pins.
//
// The caller passes the publishing frame itself (main.cpp uses the frame the
// guest has reached, not the last completed one). Draining at the
// completed-frame cadence still presents the whole sequence but leaves a
// standing backlog equal to its length: measured on the paced five-A route, a
// constant 67-frame presentation offset, versus a maximum of 1 with the
// publishing frame as the bound.
//
// The 30 comes from the guest, not from observed output. `doscrolltrans`
// (guest 0x342f4) ends its frame loop with `0x34c84: addi r30, r30, 1`,
// `0x34c88: cmpwi r30, 0x1e`, `0x34c90: blt 0x3475c`; five of the seven
// effects use 30, `dosquare1trans` 48 and `dosquare2trans` 16. Each iteration
// is one `GS_BeginFrame`/`GS_SetDestBuffer`/`GS_SendList`/`GS_EndFrame`
// cycle, and `GS_EndFrame` (guest 0x1bb2c) posts command 0x000d0007.
// `subfic r26, r26, 1` at 0x34c8c flips the effect's buffer selector, so
// consecutive animation frames alternate between the two display buffers.
bool test_transition_display_sequence()
{
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);

    constexpr uint32_t kTransitionFrames = 30u;
    constexpr uint32_t kTag = 52u;          // the route's showroom-exit frame
    constexpr uint32_t kBufA = 0x56a000u;
    constexpr uint32_t kBufB = 0x4f0000u;
    for (uint32_t i = 0; i < kTransitionFrames; ++i)
        bridge.publish_display_target(kTag, (i & 1u) ? kBufB : kBufA);

    if (!expect(bridge.consume_display_target_upto(kTag - 1u) == 0u,
                "a transition frame is not presented before its guest frame "
                "reaches a loop head"))
        return false;

    std::vector<uint32_t> presented;
    for (uint32_t i = 0; i <= kTransitionFrames; ++i) {
        uint32_t tag = 0u;
        const uint32_t target = bridge.consume_display_target_upto(kTag, &tag);
        if (target == 0u) break;
        if (!expect(tag == kTag,
                    "every presented transition frame reports its guest "
                    "frame"))
            return false;
        presented.push_back(target);
    }
    if (!expect(presented.size() == kTransitionFrames,
                "the authored transition is presented as 30 frames, not "
                "collapsed to one"))
        return false;
    bool alternating = true;
    for (size_t i = 0; i < presented.size(); ++i) {
        if (presented[i] != ((i & 1u) ? kBufB : kBufA)) alternating = false;
    }
    return expect(alternating,
                  "the transition's buffers are presented in guest order");
}

// Regression for the interactive look-ahead boundary. A frame that is still
// being built must not be interpreted by a pump bounded to the completed
// boundary: its lists are partial, and on the diagnostic/drain route the
// presenter already shows a choice from about two guest frames back. Fails
// when the pump ignores the completed-frame tag or when the diagnostic overlay
// gate is not applied to the caller's decision. Partial lists do not
// necessarily land in the target presented in the same host iteration; the
// stamped transaction trace is the authority for that boundary.
bool test_inflight_frame_boundary()
{
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);

    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x1000u, 4u, 0x2000u, 0u,
                                   0u, 0u, 0u, 0u, 0u);
    GuestFrameOverride frame(11u);
    bridge.enqueue(0u, 0x000d0003u, 0x2u, 0u, 0u, nullptr, nullptr, 0u);

    bridge.pump(10u, "completed");
    if (!expect(backend.z_clears == 0u,
                "an in-flight frame's lists stay queued at the completed "
                "boundary"))
        return false;
    if (!expect(bridge.has_pending_upto(11u),
                "the in-flight submission is still queued"))
        return false;
    bridge.pump(11u, "inflight");
    return expect(backend.z_clears == 1u,
                  "the in-flight frame executes only at its own boundary");
}

// DISPLAYFRAMEBUFFER follows the unit's submitted lists, but the guest may
// already be building the next application frame when the host samples it.
// A published unit must not authorize that later frame's writes/clears.
bool test_published_display_pump_boundary()
{
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);
    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x1000u, 4u, 0x2000u, 0u,
                                   0u, 0u, 0u, 0u, 0u);
    const auto enqueue_clear = [&](uint32_t tag) {
        GuestFrameOverride frame(tag);
        bridge.enqueue(0u, 0x000d0003u, 0x2u, 0u, 0u, nullptr, nullptr, 0u);
    };
    enqueue_clear(10u);
    bridge.publish_display_target(10u, 0x1000u);
    enqueue_clear(11u);  // enqueued after publication, before the host pump

    uint32_t tag = 99u;
    if (!expect(!bridge.has_display_target_upto(9u, &tag) && tag == 0u,
                "future display is not ready and clears its output tag"))
        return false;
    bridge.pump_display_target_upto(9u);
    if (!expect(backend.z_clears == 0u,
                "no eligible display means no display-pump side effects"))
        return false;
    if (!expect(bridge.has_display_target_upto(11u, &tag) && tag == 10u,
                "ready display names its published frame, not current 11"))
        return false;
    bridge.pump_display_target_upto(11u);
    if (!expect(backend.z_clears == 1u &&
                    !bridge.has_pending_upto(10u) && bridge.has_pending_upto(11u),
                "display pump executes published work and leaves newer work queued"))
        return false;
    if (!expect(bridge.consume_display_target_upto(tag) == 0x1000u,
                "published target remains consumable after its pump"))
        return false;
    bridge.pump_display_target_upto(11u);
    if (!expect(backend.z_clears == 1u,
                "an unpublished newer frame cannot be pumped as display work"))
        return false;
    bridge.publish_display_target(11u, 0x3000u);
    bridge.pump_display_target_upto(11u);
    if (!expect(backend.z_clears == 2u && !bridge.has_pending_upto(11u),
                "newer work executes once its own display is published"))
        return false;

    // Same-tag animation units remain legal. Publication order still owns
    // them; this is not a once-per-application-frame rendering restriction.
    bridge.consume_display_target_upto(11u);
    enqueue_clear(11u);
    bridge.publish_display_target(11u, 0x1000u);
    bridge.pump_display_target_upto(11u);
    if (!expect(backend.z_clears == 3u &&
                      bridge.consume_display_target_upto(11u) == 0x1000u,
                  "a later transition unit with the same tag still renders"))
        return false;

    // GS_SendList for the next effect frame precedes GS_EndFrame's wait for
    // the previous display. Both use the same application tag. Even a bulk
    // diagnostic drain must leave the next unit's depth/draws untouched.
    enqueue_clear(12u);
    bridge.publish_display_target(12u, 0x1000u);
    enqueue_clear(12u);
    bridge.publish_display_target(12u, 0x3000u);
    enqueue_clear(12u); // a third unit not published yet
    bridge.pump();
    if (!expect(backend.z_clears == 4u && bridge.has_pending_upto(12u),
                "bulk pump cannot overtake a same-tag published display"))
        return false;
    bridge.pump_display_target_upto(12u);
    if (!expect(backend.z_clears == 4u,
                "repeated pumping cannot render the next display early"))
        return false;
    bridge.consume_display_target_upto(12u);
    bridge.pump_display_target_upto(12u);
    if (!expect(backend.z_clears == 5u && bridge.has_pending_upto(12u),
                "consuming the first display admits exactly the second unit"))
        return false;
    bridge.consume_display_target_upto(12u);
    bridge.pump();
    return expect(backend.z_clears == 6u && !bridge.has_pending_upto(12u),
                  "unpublished work can drain after the display queue empties");
}

bool test_inflight_lookahead_policy()
{
    RecordingBackend backend;
    m2host::GuestCltBridge bridge(&backend);

    bridge.update_te_surface_state(0x7u, 0u, 0u, 0x1000u, 4u, 0x2000u, 0u,
                                   0u, 0u, 0u, 0u, 0u);
    GuestFrameOverride frame(11u);
    bridge.enqueue(0u, 0x000d0003u, 0x2u, 0u, 0u, nullptr, nullptr, 0u);

    if (!expect(!bridge.inflight_lookahead_allowed(false, true, 10u, 11u),
                "the look-ahead pump requires interactive pacing"))
        return false;
    if (!expect(!bridge.inflight_lookahead_allowed(true, true, 11u, 11u),
                "a frame at the completed boundary is not in flight"))
        return false;
    if (!expect(bridge.inflight_lookahead_allowed(true, true, 10u, 11u),
                "default policy services a pending look-ahead frame"))
        return false;

    bridge.set_inflight_overlay_only(true);
    if (!expect(!bridge.inflight_lookahead_allowed(true, false, 10u, 11u),
                "the overlay-only branch refuses a non-overlay in-flight "
                "frame"))
        return false;
    return expect(bridge.inflight_lookahead_allowed(true, true, 10u, 11u),
                  "the overlay-only branch still services the movie "
                  "lifecycle");
}

// The host frame boundary must not blank a guest-addressed color target the
// guest still owns, but the one depth plane every color target shares must be
// reset. Both halves come from the single plan the three backends consume.
bool test_frame_clear_plan()
{
    const m2host::CltFrameClearPlan first_use =
        m2host::clt_frame_clear_plan(true, false);
    const m2host::CltFrameClearPlan host_clear =
        m2host::clt_frame_clear_plan(false, true);
    const m2host::CltFrameClearPlan steady =
        m2host::clt_frame_clear_plan(true, true);

    if (!expect(first_use.color && first_use.depth,
                "a target's first host frame clears color and depth"))
        return false;
    if (!expect(host_clear.color && host_clear.depth,
                "non-persistent mode clears color and depth every frame"))
        return false;
    if (!expect(!steady.color,
                "a persistent color target is not blanked by the host"))
        return false;
    return expect(steady.depth,
                  "the shared depth plane is reset at every host frame");
}

bool test_interactive_display_depth_reset()
{
    using m2host::clt_interactive_frame_begin_needed;
    if (!expect(!clt_interactive_frame_begin_needed(false, false) &&
                    clt_interactive_frame_begin_needed(true, false) &&
                    clt_interactive_frame_begin_needed(false, true) &&
                    clt_interactive_frame_begin_needed(true, true),
                "queued completed or published display work begins a frame"))
        return false;

    const auto render_second_frame = [](bool display_work) {
        m2host::SoftCaptureBackend backend(4, 4, {});
        backend.clt_target_persistence(true);
        backend.frame_begin(0.f, 0.f, 0.f);
        backend.clt_target_select(0x1000u);
        backend.clt_clear_color(0u, 0u, 0x3f800000u, 0x3f800000u);

        std::array<m2host::CltVertex, 3> triangle{};
        triangle[0] = {0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
        triangle[1] = {4.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
        triangle[2] = {0.f, 4.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.9f};
        m2host::CltDepth depth;
        depth.test_enabled = true;
        depth.write_enabled = true;
        depth.depth_func =
            m2host::kCltDbzPixelLt | m2host::kCltDbzZLt;
        depth.z_region_max_x = 4;
        depth.z_region_max_y = 4;
        backend.clt_draw(triangle.data(), 3, false, false,
                         m2host::kInvalidCel, 4.f, 4.f, false, {}, depth,
                         {}, {});

        // The next guest display is ready while its application frame is
        // still running. That is the path the manual Testona trace exercised.
        if (clt_interactive_frame_begin_needed(false, display_work))
            backend.frame_begin(0.05f, 0.07f, 0.10f);
        for (auto& vertex : triangle) {
            vertex.r = 1.f;
            vertex.g = 0.f;
            vertex.one_over_w = 0.4f;
        }
        backend.clt_draw(triangle.data(), 3, false, false,
                         m2host::kInvalidCel, 4.f, 4.f, false, {}, depth,
                         {}, {});
        std::array<uint32_t, 16> pixels{};
        if (!backend.grab_clt_target_rgba(0x1000u, pixels.data(), 4, 4))
            pixels.fill(0u);
        return pixels;
    };

    const auto with_reset = render_second_frame(true);
    const auto without_reset = render_second_frame(false);
    return expect(with_reset[0] == 0xff0000ffu &&
                      with_reset[15] == 0xffff0000u,
                  "display-only work clears shared Z and retains color") &&
           expect(without_reset[0] == 0xff00ff00u,
                  "omitting the display-work boundary rejects farther world geometry");
}

// Behavioral half of the same contract: the software backend must keep a
// guest-authored color across a steady host frame_begin when targets persist,
// while a stale depth plane would corrupt the next frame's depth test.
bool test_persistent_target_color_retention()
{
    // The backends read the persistence override from the environment, so the
    // assertion is only deterministic while that override is silent.
    const char* persist_env = std::getenv("M2_CLT_PERSIST_TARGET");
    if (persist_env && persist_env[0] != '\0') return true;

    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.clt_target_persistence(true);
    backend.frame_begin(0.f, 0.f, 0.f);
    backend.clt_target_select(0x2000u);
    backend.clt_clear_color(std::bit_cast<uint32_t>(0.25f),
                            std::bit_cast<uint32_t>(0.5f),
                            std::bit_cast<uint32_t>(1.f),
                            std::bit_cast<uint32_t>(1.f));
    // A steady host frame must not blank the guest's authored color.
    backend.frame_begin(0.05f, 0.07f, 0.10f);
    std::vector<uint32_t> pixels(4u * 4u, 0u);
    if (!expect(backend.grab_clt_target_rgba(0x2000u, pixels.data(), 4, 4),
                "the persisted target is readable after frame_begin"))
        return false;
    // 0.25 -> 0x40, 0.5 -> 0x80, 1.0 -> 0xff; alpha 1.0 -> 0xff.
    constexpr uint32_t kExpected = 0xffff8040u;
    for (uint32_t pixel : pixels)
        if (!expect(pixel == kExpected,
                    "the guest's authored color survives the host frame "
                    "clear"))
            return false;
    return true;
}

// ModifyView publishes the MPEG bitmap before the host sees the overlay
// worker lifecycle. The compatibility seed must preserve the prior Projector
// TE surface, not copy that now-current movie View into both persistent game
// buffers. This fixture deliberately makes those images disagree; sampling
// grab_presented_rgba is the plausible old-result negative control.
bool test_video_overlay_seed_uses_prior_te_scanout()
{
    constexpr uint32_t prior_te = 0x1000u;
    constexpr uint32_t overlay_view = 0x2000u;
    constexpr uint32_t overlay_draw = 0x3000u;
    constexpr uint32_t menu_green = 0xff00ff00u;
    constexpr uint32_t movie_red = 0xff0000ffu;

    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0.f, 0.f, 0.f);
    std::vector<uint32_t> menu(16u, menu_green);
    std::vector<uint32_t> movie(16u, movie_red);
    std::vector<uint32_t> scratch(16u, 0u);
    backend.clt_target_upload_rgba(prior_te, menu.data(), 4, 4);
    backend.set_bitmap_scanout_view(movie.data(), 4, 4, 1u);

    if (!expect(backend.grab_presented_rgba(scratch.data(), 4, 4) &&
                    scratch.front() == movie_red,
                "wrong overlay seed source observes the current FMV View"))
        return false;
    if (!expect(m2host::seed_video_overlay_targets(
                    &backend, prior_te, overlay_view, overlay_draw,
                    scratch.data(), 4, 4),
                "overlay seed snapshots the completed TE scanout"))
        return false;

    for (uint32_t target : {overlay_view, overlay_draw}) {
        std::fill(scratch.begin(), scratch.end(), 0u);
        if (!expect(backend.grab_clt_target_rgba(target, scratch.data(), 4, 4),
                    "seeded overlay sibling is readable"))
            return false;
        for (uint32_t pixel : scratch)
            if (!expect(pixel == menu_green,
                        "FMV View pixels do not enter persistent TE siblings"))
                return false;
    }

    // If a completed TE identity exists but its surface is unavailable, fail
    // closed instead of falling back to the red current View. The caller can
    // then copy an existing guest sibling without importing movie pixels.
    if (!expect(!m2host::seed_video_overlay_targets(
                    &backend, 0x4000u, overlay_view, overlay_draw,
                    scratch.data(), 4, 4),
                "missing named TE scanout does not fall back to the FMV View"))
        return false;
    return true;
}

// Vendor frame2d/copyrect.c F2_CopyRect sets DBSRCOFFSET = src - dest.
// DB source addresses are screen X/Y plus DBSRCOFFSET, independent of W.
// A patterned source makes a projectively displaced read observably wrong.
bool test_db_source_screen_interpolation()
{
    for (int form = 0; form < 3; ++form) {
        m2host::SoftCaptureBackend backend(8, 8, {});
        backend.frame_begin(0.f, 0.f, 0.f);
        std::array<uint32_t, 64> source{};
        for (unsigned i = 0; i < source.size(); ++i)
            source[i] = 0xff000000u | (i + 1u);
        const auto texture = backend.cel_upload_rgba(8, 8, source.data());
        const uint32_t white = 0xffffffffu;
        const auto primary = backend.cel_upload_rgba(1, 1, &white);
        std::array<m2host::CltVertex, 3> vertices{};
        const float xy[3][2] = {{0.f, 0.f}, {16.f, 0.f}, {0.f, 16.f}};
        for (unsigned i = 0; i < 3; ++i) {
            auto& v = vertices[i];
            v.x = xy[i][0]; v.y = xy[i][1];
            v.u = v.mask_u = v.x / 8.f;
            v.v = v.mask_v = v.y / 8.f;
            v.r = v.g = v.b = v.a = 1.f;
            v.w = i == 1 ? 4.f : 1.f;
            v.one_over_w = 1.f / v.w;
        }
        m2host::CltBlendState blend{};
        blend.mode = form == 1
            ? m2host::CltBlendMode::secondary_source_alpha
            : m2host::CltBlendMode::source_copy;
        blend.secondary_source = texture;
        blend.db_source_linear = form != 2;
        blend.db_source_height = 8.f;
        backend.clt_draw(vertices.data(), 3, false, true,
                         form == 1 ? primary : texture, 8.f, 8.f,
                         true, {}, {}, {}, blend);
        std::array<uint32_t, 64> frame{};
        if (!expect(backend.grab_presented_rgba(frame.data(), 8, 8),
                    "screen-source fixture presents")) return false;
        // Pixel (3,2) center is (3.5,2.5). Screen addressing reads (3,2).
        // Ordinary perspective texture addressing instead reads (1,2):
        // U=(3.5/8/4)/(1-3.5/16*0.75), V=(2.5/8)/same.
        if (form != 2) {
            for (unsigned y = 0; y < 7; ++y)
                for (unsigned x = 0; x < 7; ++x)
                    if (!expect(frame[y * 8 + x] == source[y * 8 + x],
                                "DB source preserves every interior screen address"))
                        return false;
        }
        const auto expected = source[2 * 8 + (form == 2 ? 1 : 3)];
        if (!expect(frame[2 * 8 + 3] == expected,
                    "DB source is affine while ordinary texture retains W"))
            return false;
    }
    return true;
}

bool test_offscreen_bitmap_feedback()
{
    // trackIterate binds the 128x40 rear-view Bitmap, submits the rear world,
    // restores the display Bitmap, then TwoD_DrawBitmap copies the small
    // source with a destination-compensating base bias. Guest RAM remains black;
    // expected colors come from the authored shaded primitives, not a renderer capture.
    constexpr uint32_t source_base = 0x900000u;
    constexpr uint32_t display_base = 0x476000u;
    constexpr uint32_t source_item = 0x102u;
    m2host::SoftCaptureBackend backend(640, 480, {});
    backend.frame_begin(0.05f, 0.07f, 0.10f);
    m2host::GuestCltBridge bridge(&backend);
    bridge.register_bitmap_buffer(0x101u, display_base, 576u * 432u * 2u);
    bridge.register_bitmap_buffer(source_item, source_base, 128u * 40u * 2u);
    bridge.update_graphics_state(0u, 0u, 0u, 0x201u, 0x101u, 0u,
                                 32, 24, 576, 432, 640, 480);
    const auto submit = [&](uint32_t target, uint32_t width, uint32_t height,
                            const std::vector<uint32_t>& words) {
        bridge.update_te_surface_state(1u, (width << 16u) | height, 0u,
                                        target, width, 0u, 0u,
                                        0u, 0u, 0u, 0u, 0u);
        const auto bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, 0u,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
    };
    const auto draw_source = [&](bool green) {
        std::vector<uint32_t> words = {
            0x10008008u, 0x0fu, // untextured primitive, no DB source
            0x10006408u, 0u, 0x10006410u, 0u,
            0x10008060u, 0u, // no discard
            0x20090003u,
        };
        const float xy[][2] = {{0.f, 0.f}, {128.f, 0.f},
                              {0.f, 40.f}, {128.f, 40.f}};
        for (const auto& vertex : xy) {
            for (float value : {vertex[0], vertex[1], green ? 0.f : 1.f,
                                green ? 1.f : 0.f, 0.f, 1.f})
                append_float(words, value);
        }
        submit(source_base, 128u, 40u, words);
    };
    const auto copy_source = [&](bool signed_offset = false) {
        std::vector<uint32_t> words = {
            0x10008008u, 0x6fu,
            0x10038064u, 0x20u, 0x00ffffffu, 0x00ffffffu, 0xd0u,
            0x10048030u, 0u,
            signed_offset ? source_base
                          : source_base - 2u * (128u * 16u + 224u),
            128u, signed_offset ? 0x0f200ff0u : 0u, 0u,
            // Original TwoD_DrawBitmap biases the base with offsets zero;
            // the equivalent signed-offset form must share the same target.
            0x200c0003u,
        };
        const float xy[][2] = {{224.f, 16.f}, {352.f, 16.f},
                              {224.f, 56.f}, {352.f, 56.f}};
        for (const auto& vertex : xy)
            for (float value : {vertex[0], vertex[1], 1.f})
                append_float(words, value);
        submit(display_base, 576u, 432u, words);
    };
    const auto check_copy = [&](uint32_t expected) {
        std::vector<uint32_t> pixels(640u * 480u);
        if (!expect(backend.grab_clt_target_rgba(
                        display_base, pixels.data(), 640, 480),
                    "offscreen consumer has an addressed display target"))
            return false;
        for (const auto& probe : std::vector<std::pair<unsigned, unsigned>>{
                 {0u, 0u}, {63u, 19u}, {127u, 39u}}) {
            const uint32_t actual = pixels[(40u + probe.second) * 640u +
                                           256u + probe.first];
            if (!expect(actual == expected,
                        "small Bitmap feedback preserves source rows and offsets")) {
                std::fprintf(stderr, "probe=(%u,%u) expected=%#x actual=%#x\n",
                             probe.first, probe.second, expected, actual);
                return false;
            }
        }
        return true;
    };
    copy_source();
    bridge.pump();
    if (!check_copy(0xff000000u)) return false;
    draw_source(false);
    copy_source();
    bridge.pump();
    if (!check_copy(0xff0000f8u)) return false;
    // Same allocation, fresh rear scene: a retained/cached first frame fails.
    draw_source(true);
    bridge.pump();
    copy_source();
    bridge.pump();
    if (!check_copy(0xff00f800u)) return false;
    copy_source(true);
    bridge.pump();
    if (!check_copy(0xff00f800u)) return false;
    // Releasing the Bitmap must remove feedback eligibility. Without guest
    // bytes the untextured fallback is black, never the old renderer image.
    bridge.register_bitmap_buffer(source_item, 0u, 0u);
    copy_source();
    bridge.pump();
    if (!check_copy(0xff000000u)) return false;
    const auto stats = bridge.stats();
    return expect(stats.dropped == 0u && stats.malformed == 0u &&
                      stats.unsupported == 0u,
                  "offscreen draw/copy has no graphics refusals");
}

bool test_bitmap_span_rebind_retargets_stale_source()
{
    // A display-carve span is the guest Bitmap object's *present* backing
    // allocation, not the first one it ever published. ModifyGraphicsItemVA
    // rebinds a live Bitmap to a new buffer and passes a zero buffer when the
    // guest releases the allocation, while the bridge's DB-source
    // classification, framebuffer snapshot, and address-adjacent chain walk all
    // read the span as the item's current target. register_bitmap_buffer()
    // kept the first registration for an item and dropped a zero buffer before
    // the bridge ever saw it, so a rebind left the interpreter addressing a
    // released renderer target while the guest object named another.
    //
    // Each case runs the same authenticated transition-seed copy through
    // GuestCltBridge over two gradients that cannot be confused (one red-only,
    // one blue-only). The planes sit one guarded allocation apart, which is
    // Dsply's own chain layout: 0x7a000 = 0x78000 plane bytes + 0x800 guard, so
    // the chain walk still adds the scratch sibling to the framebuffer span.
    // The readback therefore observes *which* allocation the interpreter
    // addressed, not merely that it drew.
    constexpr uint32_t kCarveBytes = 576u * 432u * 2u;
    constexpr uint32_t kOldBase = 0x476000u;
    constexpr uint32_t kNewBase = 0x4f0000u;
    constexpr uint32_t kScratchBase = 0x56a000u;
    constexpr uint32_t kFramebufferItem = 0x101u;
    constexpr uint32_t kScratchItem = 0x103u;
    // ModifyGraphicsItemVA's unpublish form: a zero buffer for a live item.
    constexpr uint32_t kUnpublish = 0u;

    const auto gradient = [](uint32_t shift) {
        std::vector<uint32_t> image(640u * 480u, 0xff000000u);
        for (uint32_t y = 0; y < 480u; ++y) {
            for (uint32_t x = 0; x < 640u; ++x) {
                // The +5 keeps every texel nonzero, so no probe is a colour
                // that both allocations happen to share.
                const uint32_t v = ((x + y + 5u) & 31u) << 3u;
                image[static_cast<size_t>(y) * 640u + x] =
                    (v << shift) | 0xff000000u;
            }
        }
        return image;
    };
    const std::vector<uint32_t> old_image = gradient(0u);
    const std::vector<uint32_t> new_image = gradient(16u);

    // Publishes `history` for the framebuffer item (kUnpublish sends the zero
    // buffer), then runs one transition-seed copy from `source_base` into the
    // scratch plane. Returns the scratch readback, empty when the list did not
    // reach the destination at all.
    const auto run_seed = [&](uint32_t source_base,
                              const std::vector<uint32_t>& history) {
        m2host::SoftCaptureBackend backend(640, 480, {});
        backend.frame_begin(0.05f, 0.07f, 0.10f);
        backend.clt_target_upload_rgba(kOldBase, old_image.data(), 640, 480);
        backend.clt_target_upload_rgba(kNewBase, new_image.data(), 640, 480);
        m2host::GuestCltBridge bridge(&backend);
        for (const uint32_t base : history) {
            if (base == kUnpublish)
                bridge.register_bitmap_buffer(kFramebufferItem, 0, 0);
            else
                bridge.register_bitmap_buffer(kFramebufferItem, base,
                                              kCarveBytes);
        }
        bridge.register_bitmap_buffer(kScratchItem, kScratchBase, kCarveBytes);
        bridge.update_graphics_state(0u, 0x000d0006u, kFramebufferItem, 0x201u,
                                     kFramebufferItem, kFramebufferItem, 32, 24,
                                     576, 432, 640, 480);
        bridge.update_te_surface_state(0x7u, 0u, 0u, kScratchBase, 576u,
                                        kScratchBase, 0u, 0u, 0u, 0u, 0u, 0u);
        std::vector<uint32_t> words = {
            0x10008008u, 0x0000006fu,  // DBDESTCONTROL
            0x10038064u, 0x00000020u, 0x00ffffffu, 0x00ffffffu, 0x000000d0u,
            0x10049030u, 0x00000002u, source_base, 0x00000240u,
            0x00000000u, 0x00000000u,  // DBSRCOFFSET x/y
            0x200c0003u,                // new fan, four vertices
        };
        const float seed_vertices[][3] = {
            {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : seed_vertices)
            for (float value : vertex) append_float(words, value);
        const std::vector<uint8_t> bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, 0u,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
        std::vector<uint32_t> dest(640u * 480u);
        if (!backend.grab_clt_target_rgba(kScratchBase, dest.data(), 640, 480))
            return std::vector<uint32_t>();
        return dest;
    };

    // Independent expected value: the addressed allocation's RGB555 texel
    // through the documented 5->8 expansion, with MSB replicate because the
    // fixture's DBSRCCNTL (0x2) selects it.
    const auto expect_texel = [](const std::vector<uint32_t>& image,
                                 uint32_t probe_x, uint32_t probe_y) {
        const uint32_t p = image[static_cast<size_t>(24u + probe_y) * 640u +
                                 32u + probe_x];
        return m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
               (m2host::clt_db_source_expand_5_to_8(p >> 11u, true) << 8u) |
               (m2host::clt_db_source_expand_5_to_8(p >> 19u, true) << 16u) |
               0xff000000u;
    };
    // The viewport origin (32,24) puts the DB-source quad's texel (0,0) at
    // host (32,24); these probes span the carved 576x432 region.
    const uint32_t probes[][2] = {
        {0u, 0u}, {111u, 73u}, {319u, 215u}, {575u, 431u},
    };
    const auto probe_value = [](const std::vector<uint32_t>& dest,
                                const uint32_t (&probe)[2]) {
        return dest[static_cast<size_t>(24u + probe[1]) * 640u + 32u +
                    probe[0]];
    };

    // Positive arm for the removal probe: an unreleased span serves the
    // allocation it names, so the readback carries that allocation's gradient.
    const std::vector<uint32_t> held = run_seed(kOldBase, {kOldBase});
    if (!expect(!held.empty(),
                "unreleased span reaches its addressed destination"))
        return false;
    for (const auto& probe : probes) {
        if (!expect(probe_value(held, probe) ==
                        expect_texel(old_image, probe[0], probe[1]),
                    "unreleased span samples the allocation it names"))
            return false;
    }

    // A zero buffer is the guest releasing the allocation. Same list, same
    // armed address, same backend content: the only difference is the
    // unpublishing update, so the span has to stop naming the release. A
    // resolver that kept the first registration would keep serving the old
    // gradient here, so this probe rejects it as well as any no-span result.
    const std::vector<uint32_t> released =
        run_seed(kOldBase, {kOldBase, kUnpublish});
    if (!expect(!released.empty(),
                "released span still reaches its addressed destination"))
        return false;
    for (const auto& probe : probes) {
        if (!expect(probe_value(released, probe) !=
                        expect_texel(old_image, probe[0], probe[1]),
                    "a zero-buffer update removes the item's display span"))
            return false;
    }

    // The rebind itself: the guest object now names the new buffer, so the
    // bridge must read the new allocation for it.
    const std::vector<uint32_t> rebound =
        run_seed(kNewBase, {kOldBase, kNewBase});
    if (!expect(!rebound.empty(),
                "rebound span reaches its addressed destination"))
        return false;
    for (const auto& probe : probes) {
        if (!expect(probe_value(rebound, probe) ==
                        expect_texel(new_image, probe[0], probe[1]),
                    "re-registered item samples its new backing allocation"))
            return false;
    }

    // Plausible-wrong control, actually run: a bridge that saw only the first
    // registration keeps the span on the released allocation, so the guest's
    // new address resolves to no display carve and the copy cannot produce the
    // new allocation's pixels. This is the pre-fix behavior, and the probe
    // above rejects it.
    const std::vector<uint32_t> stale = run_seed(kNewBase, {kOldBase});
    if (!expect(!stale.empty(),
                "stale span still reaches its addressed destination"))
        return false;
    for (const auto& probe : probes) {
        const uint32_t got = probe_value(stale, probe);
        const uint32_t wanted = expect_texel(new_image, probe[0], probe[1]);
        if (got == wanted)
            std::fprintf(stderr,
                         "stale-control: probe (%u,%u) got %#x, identical to "
                         "the new allocation's texel\n",
                         probe[0], probe[1], got);
        if (!expect(got != wanted,
                    "first-registration-wins span misses the new allocation"))
            return false;
    }
    return true;
}

bool test_bitmap_span_capacity_preserves_display_chain()
{
    // register_bitmap_buffer() kept a fixed eight-span budget and returned
    // silently once it was reached, but the runtime publishes *every* nonzero
    // Bitmap buffer (recomp_runtime.c ModifyGraphicsItemVA), not only the three
    // display-chain planes the observed showroom routes happen to register,
    // and it does so in the guest's own allocation order. Eight unrelated
    // bitmaps published before the display allocation therefore lost the
    // framebuffer span outright: the bridge's chain walk found no seed, the
    // interpreter was handed an empty span set, and the DB-source read fell
    // back to guest RAM with no graphics reject anywhere.
    //
    // The bound is now the recovered item pool, and a registration that still
    // exceeds it releases a span the *current* display chain cannot use. Both
    // guest orders are exercised, because they fail differently:
    //
    //   flood-then-chain  the reported shape; the chain is the newest
    //                     registration and only needs a budget above the
    //                     flood size to survive.
    //   chain-then-flood  the chain is the *oldest* registration, so a
    //                     plain oldest-first release eats the display planes
    //                     first. Only a chain-aware victim policy keeps them.
    //
    // The readback is the observation, exactly as in the rebind fixture: the
    // planes sit one guarded allocation apart (Dsply's own chain layout,
    // 0x7a000 = 0x78000 plane bytes + 0x800 guard), so a resolved chain copies
    // the renderer-owned source plane while a lost chain cannot.
    static constexpr uint32_t kCarveBytes = 576u * 432u * 2u;
    static constexpr uint32_t kFbBase = 0x476000u;
    static constexpr uint32_t kSourceBase = 0x4f0000u;
    static constexpr uint32_t kScratchBase = 0x56a000u;
    static constexpr uint32_t kFbItem = 0x101u;
    static constexpr uint32_t kSourceItem = 0x102u;
    static constexpr uint32_t kScratchItem = 0x103u;
    // 512 KiB apart, so no unrelated span can sit exactly one guard past a
    // chain plane (that spacing is 0x7a000) and none of them is a chain member.
    static constexpr uint32_t kUnrelatedBase = 0x800000u;
    static constexpr uint32_t kUnrelatedStep = 0x80000u;
    // The production item pool: 256 slots with an 8-bit slot index in the
    // handle layout (recomp_runtime.c:571). The span table holds one entry per
    // distinct item, so this is the number of entries a guest-owned Bitmap can
    // legitimately occupy.
    constexpr uint32_t kSpanTableCapacity = 256u;

    const auto gradient = [](uint32_t shift) {
        std::vector<uint32_t> image(640u * 480u, 0xff000000u);
        for (uint32_t y = 0; y < 480u; ++y) {
            for (uint32_t x = 0; x < 640u; ++x) {
                // Never zero, so an unwritten destination cannot be mistaken
                // for the source plane's texel.
                const uint32_t v = (1u + ((x + y + 5u) & 30u)) << 3u;
                image[static_cast<size_t>(y) * 640u + x] =
                    (v << shift) | 0xff000000u;
            }
        }
        return image;
    };
    // Green-only, so no probe can match an unrelated allocation's texel.
    const std::vector<uint32_t> source_image = gradient(8u);

    // Registers `unrelated_count` same-sized bitmaps away from the display
    // chain, then the display chain itself, and copies the source plane into
    // the scratch plane. `publish_source_span` false leaves the source plane
    // unregistered, which is the chain-loss shape. Returns the scratch
    // readback plus the bridge's span-table accounting.
    struct SeedResult {
        std::vector<uint32_t> dest;
        uint64_t span_evictions = 0;
        uint64_t fb_span_missing = 0;
        uint64_t dropped = 0;
        uint64_t unsupported = 0;
    };
    const auto run_seed = [&](uint32_t unrelated_count, bool publish_source_span,
                              bool flood_after_chain,
                              bool publish_fb_span = true) {
        m2host::SoftCaptureBackend backend(640, 480, {});
        backend.frame_begin(0.05f, 0.07f, 0.10f);
        backend.clt_target_upload_rgba(kSourceBase, source_image.data(), 640,
                                       480);
        m2host::GuestCltBridge bridge(&backend);
        const auto publish_unrelated = [&bridge](uint32_t from, uint32_t count) {
            for (uint32_t i = from; i < from + count; ++i) {
                bridge.register_bitmap_buffer(0x200u + i,
                                              kUnrelatedBase + i * kUnrelatedStep,
                                              kCarveBytes);
            }
        };
        const auto publish_chain = [&bridge, publish_source_span,
                                    publish_fb_span]() {
            if (publish_fb_span)
                bridge.register_bitmap_buffer(kFbItem, kFbBase, kCarveBytes);
            if (publish_source_span)
                bridge.register_bitmap_buffer(kSourceItem, kSourceBase,
                                              kCarveBytes);
            bridge.register_bitmap_buffer(kScratchItem, kScratchBase,
                                          kCarveBytes);
        };
        // The framebuffer item has to be known before a chain-aware release can
        // protect the chain, so this arm names it first. The guest's TE surface
        // setup and its ModifyGraphicsItemVA publications run under the same
        // bridge lock, and the pump reads the same field every frame.
        bridge.update_graphics_state(0u, 0x000d0006u, kFbItem, 0x201u, kFbItem,
                                     kFbItem, 32, 24, 576, 432, 640, 480);
        if (flood_after_chain) {
            publish_chain();
            publish_unrelated(0u, unrelated_count);
        } else {
            publish_unrelated(0u, unrelated_count);
            publish_chain();
        }
        bridge.update_te_surface_state(0x7u, 0u, 0u, kScratchBase, 576u,
                                        kScratchBase, 0u, 0u, 0u, 0u, 0u, 0u);
        // The authenticated transition-seed copy: DB source at the source
        // plane, destination the scratch plane, one 576x432 fan.
        std::vector<uint32_t> words = {
            0x10008008u, 0x0000006fu,  // DBDESTCONTROL
            0x10038064u, 0x00000020u, 0x00ffffffu, 0x00ffffffu, 0x000000d0u,
            0x10049030u, 0x00000002u, kSourceBase, 0x00000240u,
            0x00000000u, 0x00000000u,  // DBSRCOFFSET x/y
            0x200c0003u,                // new fan, four vertices
        };
        const float seed_vertices[][3] = {
            {0.f, 0.f, 1.f}, {0.f, 432.f, 1.f},
            {576.f, 0.f, 1.f}, {576.f, 432.f, 1.f},
        };
        for (const auto& vertex : seed_vertices)
            for (float value : vertex) append_float(words, value);
        const std::vector<uint8_t> bytes = big_endian_bytes(words);
        bridge.enqueue(0u, 0x000d0003u, 0u, 0u,
                       static_cast<uint32_t>(bytes.size()), bytes.data(),
                       nullptr, 0u);
        bridge.pump();
        SeedResult result;
        result.dest.assign(640u * 480u, 0u);
        if (!backend.grab_clt_target_rgba(kScratchBase, result.dest.data(), 640,
                                          480)) {
            result.dest.clear();
        }
        result.span_evictions = bridge.stats().span_evictions;
        result.fb_span_missing = bridge.stats().fb_span_missing;
        result.dropped = bridge.stats().dropped;
        result.unsupported = bridge.stats().unsupported;
        return result;
    };

    // Independent expected value: the source plane's RGB555 texel through the
    // documented 5->8 expansion with MSB replicate, which this fixture's
    // DBSRCCNTL (0x2) selects. The viewport origin (32,24) puts the DB-source
    // quad's texel (0,0) at host (32,24).
    const auto expect_texel = [&](uint32_t probe_x, uint32_t probe_y) {
        const uint32_t p = source_image[static_cast<size_t>(24u + probe_y) * 640u +
                                       32u + probe_x];
        return m2host::clt_db_source_expand_5_to_8(p >> 3u, true) |
               (m2host::clt_db_source_expand_5_to_8(p >> 11u, true) << 8u) |
               (m2host::clt_db_source_expand_5_to_8(p >> 19u, true) << 16u) |
               0xff000000u;
    };
    const uint32_t probes[][2] = {
        {0u, 0u}, {111u, 73u}, {319u, 215u}, {575u, 431u},
    };
    const auto carries_source = [&](const SeedResult& result) {
        if (result.dest.empty()) return false;
        for (const auto& probe : probes) {
            if (result.dest[static_cast<size_t>(24u + probe[1]) * 640u + 32u +
                            probe[0]] != expect_texel(probe[0], probe[1]))
                return false;
        }
        return true;
    };
    const auto expect_chain_served = [&](const SeedResult& result,
                                         const char* what) {
        for (const auto& probe : probes) {
            if (!carries_source(result)) {
                std::fprintf(stderr,
                             "%s: probe (%u,%u) did not carry the source "
                             "plane (chain lost)\n",
                             what, probe[0], probe[1]);
                break;
            }
        }
        return expect(carries_source(result),
                      "the required display chain stays discoverable");
    };

    // Reported shape: eight unrelated bitmaps published before the display
    // chain. The old eight-entry budget dropped the framebuffer span here.
    const SeedResult reported = run_seed(8u, true, false);
    if (!expect(reported.span_evictions == 0,
                "an eight-bitmap flood stays inside the item-pool budget"))
        return false;
    if (!expect(reported.fb_span_missing == 0,
                "a published framebuffer span is never counted as a fallback"))
        return false;
    if (!expect_chain_served(reported,
                            "flood-then-chain with eight unrelated spans"))
        return false;

    // Chain registered first, then a flood far past the bound: 3 chain spans
    // plus 300 unrelated ones overflow a 256-entry table, so 47 registrations
    // have to release something. A release that does not consult the chain
    // takes the three display planes first, because they are the oldest.
    constexpr uint32_t kFloodAfter = 300u;
    const SeedResult flooded = run_seed(kFloodAfter, true, true);
    if (!expect(flooded.span_evictions == kFloodAfter + 3u -
                                                kSpanTableCapacity,
                "the overflow releases exactly the entries past the table bound"))
        return false;
    if (!expect_chain_served(flooded,
                            "chain-then-flood past the span-table bound"))
        return false;
    if (!expect(flooded.fb_span_missing == 0,
                "an overflow rescue never leaves the framebuffer item without "
                "its span"))
        return false;

    // Plausible-wrong control, actually run: the same chain-then-flood order
    // with the source plane's own span never published. The chain is then
    // genuinely incomplete, the DB-source address resolves to no display
    // carve, and the copy cannot produce the source plane's pixels. This is
    // what the silent cap drop produced, so the probes above are not passing
    // because any draw writes these pixels.
    const SeedResult incomplete = run_seed(kFloodAfter, false, true);
    if (!expect(!carries_source(incomplete),
                "an unregistered chain plane is not served from its target"))
        return false;

    // The DB-source fallback's own counter. A nonzero framebuffer item whose
    // buffer was never published is the one condition that reaches the
    // guest-RAM fallback without the span table being at fault, and the pump
    // evaluates it once per batch: one pump here, so exactly one count. The
    // fallback is *reported*, never refused, so the same arm must also leave
    // "dropped" and "unsupported" at zero -- converting this serve into a
    // fail-closed branch would otherwise be invisible to the suite.
    const SeedResult no_fb_span = run_seed(0u, false, false, false);
    if (!expect(no_fb_span.fb_span_missing == 1,
                "a framebuffer item with no published span is counted once per "
                "pump, not silently served"))
        return false;
    if (!expect(no_fb_span.dropped == 0 && no_fb_span.unsupported == 0,
                "the DB-source fallback stays a report rather than a refusal"))
        return false;
    return true;
}

}  // namespace

bool test_projector_pixel_replication()
{
    m2host::SoftCaptureBackend backend(8, 8, {});
    m2host::GuestCltInterpreter interpreter(&backend);
    backend.frame_begin(0, 0, 0);
    interpreter.set_te_surface_state(7, (4u << 16) | 2u, 0, 0x10000,
                                     4, 0, 0, 0, 0, 0, 0, 0, 1);
    interpreter.set_viewport(2, 1, 4, 4, true, 8, 8);
    std::vector<uint32_t> source(64);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint32_t(i + 1);
    backend.clt_target_upload_rgba(0x10000, source.data(), 8, 8);
    backend.clt_scanout_select(0x10000);
    std::vector<uint32_t> pixels(64);
    if (!backend.grab_presented_rgba(pixels.data(), 8, 8)) return false;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const int sy = x >= 2 && x < 6 && y >= 1 && y < 5
                               ? 1 + (y - 1) / 2 : y;
            if (!expect(pixels[y * 8 + x] == source[sy * 8 + x],
                        "submitted View expands two Bitmap rows to four display rows"))
                return false;
        }
    if (!expect(backend.grab_clt_target_rgba(0x10000, pixels.data(), 8, 8) &&
                pixels == source, "scanout expansion preserves TE feedback pixels"))
        return false;
    interpreter.set_viewport(2, 1, 4, 2, true, 8, 8);
    return expect(backend.grab_presented_rgba(pixels.data(), 8, 8) && pixels == source,
                  "unity View projection retains the original target");
}

bool test_separate_forward_alpha_pixels()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    backend.frame_begin(0, 1, 0); // destination green must take no part
    const uint32_t red = 0x400000ffu, blue = 0xffff0000u;
    const auto primary = backend.cel_upload_rgba(1, 1, &red);
    const auto secondary = backend.cel_upload_rgba(1, 1, &blue);
    m2host::CltVertex vertices[3] = {};
    for (auto& v : vertices) v.r = v.g = v.b = v.a = v.w = 1;
    vertices[1].x = 4; vertices[2].y = 4;
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::secondary_source_over;
    blend.secondary_source = secondary;
    backend.clt_draw(vertices, 3, false, true, primary, 4, 4, true, {}, {}, {}, blend);
    std::vector<uint32_t> pixels(16);
    if (!expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                  pixels[5] == 0x40bf0040u,
                  "A*alpha+B*(1-alpha) uses separate blue source, not green destination"))
        return false;
    blend.mode = m2host::CltBlendMode::secondary_constant_over;
    blend.constant_g = 1.f;
    backend.clt_draw(vertices, 3, false, true, primary, 4, 4, true, {}, {}, {}, blend);
    if (!expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                pixels[5] == 0x40bf4000u,
                "cloud RGB takes constant green and the separate blue source")) return false;
    backend.frame_begin(0, 1, 0);
    blend.mode = m2host::CltBlendMode::source_over;
    backend.clt_draw(vertices, 3, false, true, primary, 4, 4, true, {}, {}, {}, blend);
    return expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                  pixels[5] != 0x40bf0040u && pixels[5] != 0x40bf4000u,
                  "executed destination-source substitution differs from both equations");
}

bool test_texture_alpha_color_pixels()
{
    m2host::SoftCaptureBackend backend(4, 4, {});
    m2host::CltVertex vertices[3] = {};
    for (auto& v : vertices) {
        v.r = 128.f / 255.f; v.g = 64.f / 255.f; v.b = 32.f / 255.f;
        v.a = .25f; v.w = 1;
    }
    vertices[1].x = 4; vertices[2].y = 4;
    m2host::CltBlendState blend;
    blend.mode = m2host::CltBlendMode::source_copy;
    blend.texture_alpha_as_color = true;
    std::vector<uint32_t> pixels(16);
    for (const uint32_t alpha : {0u, 128u, 255u}) {
        const uint32_t texel = (alpha << 24u) | 0x00c80afa;
        const auto texture = backend.cel_upload_rgba(1, 1, &texel);
        backend.frame_begin(0, 1, 0);
        backend.clt_draw(vertices, 3, false, true, texture, 4, 4, true,
                         {}, {}, {}, blend);
        // Independent TAB equation: texture alpha multiplies constant RGB;
        // primitive alpha is the output regardless of the texture's alpha.
        const uint32_t expected = 0x40000000u |
            (((alpha * 32u + 127u) / 255u) << 16u) |
            (((alpha * 64u + 127u) / 255u) << 8u) |
            ((alpha * 128u + 127u) / 255u);
        if (!expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                    pixels[5] == expected, "alpha mask has independent RGB and A"))
            return false;
        blend.texture_alpha_as_color = false;
        backend.clt_draw(vertices, 3, false, true, texture, 4, 4, true,
                         {}, {}, {}, blend);
        if (!expect(backend.grab_presented_rgba(pixels.data(), 4, 4) &&
                    pixels[5] != expected,
                    "executed TEXCOLOR substitution differs from alpha mask"))
            return false;
        blend.texture_alpha_as_color = true;
    }
    return true;
}

int main()
{
    if (!test_general_dsb_interpreter()) return 1;
    if (!test_clipped_menu_footer_feedback()) return 1;
    if (!test_general_tab_equations()) return 1;
    if (!test_general_tab_interpreter()) return 1;
    if (!test_opaque_source_over_pixels()) return 1;
    if (!test_opaque_unarmed_source_over()) return 1;
    if (!test_texture_alpha_color_pixels()) return 1;
    if (!test_projector_pixel_replication()) return 1;
    if (!test_separate_forward_alpha_pixels()) return 1;
    if (!test_offscreen_bitmap_feedback()) return 1;
    if (!test_db_source_screen_interpolation()) return 1;
    if (!test_mmdma_authored_lod_chain()) return 1;
    if (!test_mmdma_inactive_lod_slots()) return 1;
    if (!test_dbz_masks()) return 1;
    if (!test_dbz_backend_split()) return 1;
    if (!test_depth_compare_and_dsb_contract()) return 1;
    if (!test_dsb_equation_decode()) return 1;
    if (!test_dsb_equation_classification()) return 1;
    if (!test_radar_corner_source_over()) return 1;
    if (!test_dsb_constant_alpha_blend()) return 1;
    if (!test_dsb_fog_composite()) return 1;
    if (!test_dsb_inverse_texture_multiply()) return 1;
    if (!test_dsb_source_copy()) return 1;
    if (!test_dsb_coefficient_weighted_mix()) return 1;
    if (!test_dsb_secondary_source_alpha_backend()) return 1;
    if (!test_dsb_secondary_source_interpreter()) return 1;
    if (!test_dsb_blend_plan_parity()) return 1;
    if (!test_dsb_backend_transport_and_shared_depth()) return 1;
    if (!test_presentation_source_selection()) return 1;
    if (!test_clt_w16_packed_quantization()) return 1;
    if (!test_z_cell_dump()) return 1;
    if (!test_exec_seq_advances()) return 1;
    if (!test_z_cell_packed_round_trip()) return 1;
    if (!test_rgb_discard_backend()) return 1;
    if (!test_alpha_discard_backend()) return 1;
    if (!test_specular_backend()) return 1;
    if (!test_fog_composite_backend()) return 1;
    if (!test_fog_vertex_alpha_reaches_backend()) return 1;
    if (!test_constant_alpha_over_backend()) return 1;
    if (!test_inverse_texture_multiply_backend()) return 1;
    if (!test_dsb_source_copy_backend()) return 1;
    if (!test_dsb_coefficient_weighted_mix_backend()) return 1;
    if (!test_dsb_weighted_mix_ssb_entry_split()) return 1;
    if (!test_ssb_mask_wrap_and_discard()) return 1;
    if (!test_masked_uv_lod_seam()) return 1;
    if (!test_dsb_alpha_control_decode()) return 1;
    if (!test_z_clip_region_helpers()) return 1;
    if (!test_z_clip_backend()) return 1;
    if (!test_z_clip_interpreter()) return 1;
    if (!test_shadow_color_z_alias_interpreter()) return 1;
    if (!test_te_vertex_state()) return 1;
    if (!test_interpreter()) return 1;
    if (!test_bitmap_span_rebind_retargets_stale_source()) return 1;
    if (!test_bitmap_span_capacity_preserves_display_chain()) return 1;
    if (!test_content_format_completion()) return 1;
    if (!test_clt_flow_control()) return 1;
    if (!test_point_packet_accounting()) return 1;
    if (!test_bridge()) return 1;
    if (!test_display_unit_selection()) return 1;
    if (!test_transition_display_sequence()) return 1;
    if (!test_inflight_frame_boundary()) return 1;
    if (!test_published_display_pump_boundary()) return 1;
    if (!test_inflight_lookahead_policy()) return 1;
    if (!test_frame_clear_plan()) return 1;
    if (!test_interactive_display_depth_reset()) return 1;
    if (!test_persistent_target_color_retention()) return 1;
    if (!test_video_overlay_seed_uses_prior_te_scanout()) return 1;
    std::puts("m2_guest_clt_test: OK");
    return 0;
}
