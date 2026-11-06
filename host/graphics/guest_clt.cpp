#include "guest_clt.h"
#include "guest_screenshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include "diagnostic_perf.h"

namespace m2host {

namespace {

constexpr uint32_t kRegisterType = 0x10000000u;
constexpr uint32_t kVertexType = 0x20000000u;
constexpr uint32_t kPointType = 0x40000000u;
constexpr uint32_t kTypeMask = 0xf0000000u;
constexpr uint32_t kTeExecuteListCommand = 0x000d0003u;
constexpr uint32_t kTeClearFrameBuffer = 0x00000001u;
constexpr uint32_t kTeClearZBuffer = 0x00000002u;

constexpr uint32_t kRegisterAddressMask = 0x0000e7ffu;
constexpr uint32_t kRegisterCountMask = 0x00ff0000u;

constexpr uint32_t kVertexShading = 0x00010000u;
constexpr uint32_t kVertexTexture = 0x00020000u;
constexpr uint32_t kVertexPerspective = 0x00040000u;
constexpr uint32_t kVertexNew = 0x00080000u;
constexpr uint32_t kVertexFan = 0x00100000u;

bool bridge_trace_enabled();
bool bridge_trace_full();
bool clt_unsupported_diag_enabled();
uint32_t clt_unsupported_diag_max();

constexpr uint16_t kDcCntlData = 0x0010u;
constexpr uint16_t kDcCntl = 0x0014u;
constexpr uint16_t kTxtLdCntl = 0x6404u;
constexpr uint16_t kTxtAddrCntl = 0x6408u;
constexpr uint16_t kTxtPipCntl = 0x640cu;
constexpr uint16_t kTxtTabCntl = 0x6410u;
constexpr uint16_t kTxtLodBase0 = 0x6414u;
constexpr uint16_t kTxtLdSrcAddr = 0x6424u;
constexpr uint16_t kTxtCount = 0x6428u;
constexpr uint16_t kTxtLdWidth = 0x642cu;
constexpr uint16_t kTxtSrcType01 = 0x6434u;
constexpr uint16_t kTxtSrcType23 = 0x6438u;
constexpr uint16_t kTxtExpType = 0x643cu;
constexpr uint16_t kTxtConst0 = 0x6440u;
constexpr uint16_t kTxtConst1 = 0x6444u;
constexpr uint16_t kTxtConst2 = 0x6448u;
constexpr uint16_t kTxtConst3 = 0x644cu;
constexpr uint16_t kDbSuperGenControl = 0x8004u;
constexpr uint16_t kDbUserControl = 0x8008u;
constexpr uint16_t kDbDiscardControl = 0x800cu;
constexpr uint16_t kDbFbClip = 0x8018u;
constexpr uint16_t kDbXWinClip = 0x801cu;
constexpr uint16_t kDbYWinClip = 0x8020u;
constexpr uint16_t kDbDestControl = 0x8024u;
constexpr uint16_t kDbDestBaseAddr = 0x8028u;
constexpr uint16_t kDbDestXStride = 0x802cu;
constexpr uint16_t kDbSrcCntl = 0x8030u;
constexpr uint16_t kDbSrcBaseAddr = 0x8034u;
constexpr uint16_t kDbSrcXStride = 0x8038u;
constexpr uint16_t kDbSrcOffset = 0x803cu;
constexpr uint16_t kDbZCntl = 0x8040u;
constexpr uint16_t kDbZBaseAddr = 0x8044u;
constexpr uint16_t kDbZOffset = 0x8048u;
constexpr uint16_t kDbZClip = 0x804cu;
constexpr uint16_t kDbSsbDsbCntl = 0x8050u;
constexpr uint16_t kDbConstIn = 0x8054u;
constexpr uint16_t kDbAMultCntl = 0x8058u;
constexpr uint16_t kDbBMultCntl = 0x8064u;
// The multiplier coefficient constants (`CONST`/`CONSTCOMPLEMENT` in the
// control words) live in their own register pairs; the checked-in Portfolio
// `DBlendSnippet` binds them as `dbl_txtCoefConst[2]` (DBAMULTCONSTSSB0/1,
// 0x805c/0x8060) and `dbl_multCoefConst[2]` (DBBMULTCONSTSSB0/1,
// 0x8068/0x806c).  Both pairs are read: the B pair by the recovered
// identity-coefficient copy form, and both pairs by the toolkit's two-input
// weighted mix.
constexpr uint16_t kDbAMultConstSsb0 = 0x805cu;
constexpr uint16_t kDbAMultConstSsb1 = 0x8060u;
constexpr uint16_t kDbBMultConstSsb0 = 0x8068u;
constexpr uint16_t kDbBMultConstSsb1 = 0x806cu;
constexpr uint16_t kDbAluCntl = 0x8070u;
// Destination-alpha controls (cltmacros.h 0x8074/0x8078/0x807c).  The guest
// authors all three statically (`FN_DestBlendInit__2e870`, the seven screen
// transitions, `StreamEventFunc`, and `TEInit`), so they are decoded exactly
// like the neighbouring DB registers instead of staying unmodeled.
constexpr uint16_t kDbSrcAlphaCntl = 0x8074u;
constexpr uint16_t kDbDestAlphaCntl = 0x8078u;
constexpr uint16_t kDbDestAlphaConst = 0x807cu;
// Dither matrix pair (cltmacros.h CLT_DBDITHERMATRIX 0x8080/0x8084) and the
// DBUSERCONTROL.DITHEREN bit (PPCBIT(27), normalized to bit 4).
constexpr uint16_t kDbDitherMatrixA = 0x8080u;
constexpr uint16_t kDbDitherMatrixB = 0x8084u;
constexpr uint32_t kDbDitherEnable = 0x00000010u;

// The CLT register words are PPC big-endian values after the bridge's
// big-endian-to-host decode. These are the DCNTL field bits from
// M2 SDK `cltmacros.h` (PPC bit numbers are reversed by the CLT encoding).
constexpr uint32_t kDcTld = 0x00000001u;
constexpr uint32_t kDcJa = 0x00000002u;
constexpr uint32_t kDcJr = 0x00000004u;
constexpr uint32_t kDcDint = 0x00000008u;
constexpr uint32_t kDcPse = 0x00000010u;
constexpr uint32_t kDcSync = 0x00000020u;
constexpr uint32_t kDcFlowMask =
    kDcTld | kDcJa | kDcJr | kDcDint | kDcPse | kDcSync;
constexpr uint32_t kTxtLdCompressed = 0x00000400u;
constexpr uint32_t kTxtLdModeShift = 8u;
constexpr uint32_t kTxtLdModeMask = 3u;
constexpr uint32_t kTxtAddrTextureEnable = 0x00002000u;
// The TXTADDRCNTL bits that describe the texture's addressing: the low nibble
// is the LOD shift (zero for a single-LOD load, see materialize_mmdma_texture).
// The remaining bits, including the texture-enable gate, do not change how a
// texel is decoded.
constexpr uint32_t kTxtAddrLodMask = 0x0000000fu;
constexpr uint32_t kPipRamBase = 0x00046000u;
constexpr uint32_t kPipRamEnd = 0x000463ffu;
constexpr size_t kPipRamEntries = 256u;
constexpr uint32_t kDbWinClipInEnable = 0x00000100u;
constexpr uint32_t kDbWinClipOutEnable = 0x00000080u;
constexpr uint32_t kDbBlendEnable = 0x00000040u;
constexpr uint32_t kDbSourceEnable = 0x00000020u;
constexpr uint32_t kDbZBufferEnable = 0x00000400u;
constexpr uint32_t kDbZOutputEnable = 0x00000200u;
constexpr uint32_t kDbWinClipCoordinateMask = 0x000007ffu;
// Mercury cltmacros.h: DBSRCCNTL.SRC32BPP is PPCBIT(31), normalized by the
// CLT register word to bit 0. Selects two-byte DSB/RGB555 versus
// four-byte DSB/A7/RGB888 source addressing and materialization.
constexpr uint32_t kDbSrc32Bpp = 0x00000001u;
// Mercury cltmacros.h: DBSRCXSTRIDE.XSTRIDE is PPCMASK(0x07FF, 31),
// an 11-bit pixel stride.  Keep the field mask at the guest-hardware
// boundary; the raw register state itself remains preserved unchanged.
constexpr uint32_t kDbSrcXStrideMask = 0x000007ffu;
// DBDISCARDCONTROL PPCMASK fields (cltmacros.h): ALPHA0 bit 31 -> 0x1,
// RGB0 bit 30 -> 0x2, SSB0 bit 29 -> 0x4, ZCLIPPED bit 28 -> 0x8.
constexpr uint32_t kDbDiscardZClipped = 0x00000008u;
constexpr uint32_t kDbZClipCoordinateMask = 0x000007ffu;

// DBDISCARDCONTROL is evaluated after texturing, before the destination
// blender reads its secondary DB source. The host represents that secondary
// source with a texture, but the ALPHA0 test still belongs to the primitive's
// post-TAB alpha. Preserve that alpha through the implementation texture.
// ZCLIPPED is carried by CltDepth. RGB0 would currently observe the host's
// implementation texture instead of the primitive RGB, and SSB has no
// source-backed untextured supply, so those two forms remain fail-closed.
bool decode_db_source_discard(uint32_t raw, CltDiscard* discard)
{
    if (!discard || (raw & ~0x9u) != 0u) return false;
    *discard = {};
    discard->alpha_zero = (raw & 0x1u) != 0u;
    return true;
}

int32_t sign_extend_12(uint32_t value)
{
    value &= 0xfffu;
    return (value & 0x800u) != 0 ? static_cast<int32_t>(value) - 0x1000
                                 : static_cast<int32_t>(value);
}

struct CltDbSourceCoordinates {
    float u = 0.f;
    float v = 0.f;
};

// F2_CopyRect and the CLT register contract describe one linear source
// address: base + 2 * ((y + offset_y) * width + x + offset_x). Normalize that
// address for every DB-source storage kind while retaining unbounded X. In
// particular, guest-RAM sources are not allowed to clamp an X overflow to the
// last texel; X must carry into the source row just as it does for a
// display-carve source.
CltDbSourceCoordinates clt_db_source_coordinates(
    float dest_x, float dest_y, float source_origin_pixels,
    uint32_t source_offset, int source_width, int source_height)
{
    const float width = static_cast<float>(source_width);
    const float height = static_cast<float>(source_height);
    const float offset_x = static_cast<float>(
        sign_extend_12(source_offset >> 16u));
    const float offset_y = static_cast<float>(sign_extend_12(source_offset));
    // Keep the X address unbounded. The backend applies floor(X / width) per
    // fragment, which preserves the half-open rectangle edge while carrying
    // non-row-aligned offsets into the adjacent source row.
    return {(dest_x + offset_x + source_origin_pixels) / width,
            (dest_y + offset_y) / height};
}

constexpr uint32_t kFormatColorDepthMask = 0x0fu;
constexpr uint32_t kFormatAlphaDepthMask = 0x0fu;
constexpr uint32_t kFormatIsTransparent = 1u << 8u;
constexpr uint32_t kFormatHasSsb = 1u << 9u;
constexpr uint32_t kFormatHasColor = 1u << 10u;
constexpr uint32_t kFormatHasAlpha = 1u << 11u;
constexpr uint32_t kFormatIsLiteral = 1u << 12u;

constexpr uint32_t kPipSelectConstant = 0u;
constexpr uint32_t kPipSelectTexture = 1u;
constexpr uint32_t kPipSelectPip = 2u;

constexpr size_t kRegisterWords = 0x10000u / sizeof(uint32_t);
constexpr uint32_t kMaxVertexCount = 65536u;
constexpr uint32_t kMaxListBytes = 4u * 1024u * 1024u;
// Keep snapshot memory bounded, but apply backpressure when the render thread
// has not retired a batch. Dropping an authentic list can invalidate later
// stateful CLT lists, so queue capacity is a scheduling boundary rather than
// a lossy overflow policy.
constexpr size_t kMaxQueuedLists = 64u;
constexpr size_t kMaxTextureSnapshotBytes = 4u * 1024u * 1024u;
constexpr uint64_t kMaxTexturePixels = 4u * 1024u * 1024u;

// Configure Car footer provenance (M2_CONFIG_TEXTURE_TRACE=1). The steady
// footer quad is source 0x6eb468 / count 0x398c (508x29, 8-bit, PIP palette;
// draw bbox (57,428)-(565,457)) and is re-drawn every list in the selection
// screen. The screen-entry transition also stages a wider 520x29 quad from
// source 0x6bf3f0 / count 0x3ae8 / ld_width 0x10401040 (bbox
// (57,428)-(577,457)); that quad is not the per-frame footer. Keep this
// diagnostic keyed to the steady footer's source+count identity so it cannot
// become a general texture census or affect rendering. ld_width is logged but
// not matched: pinning a row-control value would silently suppress the trace
// if the register variant changed.
constexpr uint32_t kConfigFooterSource = 0x006eb468u;
constexpr uint32_t kConfigFooterCount = 0x0000398cu;
constexpr size_t kConfigFooterSampleBytes = 16u;
constexpr uint32_t kConfigFooterTraceDefaultMax = 256u;

bool config_texture_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CONFIG_TEXTURE_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t config_texture_trace_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CONFIG_TEXTURE_TRACE_MAX");
        if (!value || *value == '\0') return kConfigFooterTraceDefaultMax;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(
            std::min<unsigned long>(parsed, 1024ul));
    }();
    return max_entries;
}

bool config_footer_texture_load(uint32_t source_address, uint32_t count)
{
    return source_address == kConfigFooterSource &&
           count == kConfigFooterCount;
}

uint32_t config_texture_hash_bytes(const uint8_t* bytes, size_t byte_count)
{
    uint32_t hash = 2166136261u;
    if (!bytes) return hash;
    for (size_t i = 0; i < byte_count; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

uint32_t config_texture_hash_rgba(const std::vector<uint32_t>& pixels)
{
    uint32_t hash = 2166136261u;
    for (const uint32_t rgba : pixels) {
        // Pixels are stored as packed R|G<<8|B<<16|A<<24. Hash the logical
        // RGBA byte order explicitly instead of relying on host endianness.
        hash = (hash ^ (rgba & 0xffu)) * 16777619u;
        hash = (hash ^ ((rgba >> 8u) & 0xffu)) * 16777619u;
        hash = (hash ^ ((rgba >> 16u) & 0xffu)) * 16777619u;
        hash = (hash ^ ((rgba >> 24u) & 0xffu)) * 16777619u;
    }
    return hash;
}

void config_texture_copy_sample(std::array<uint8_t, 16>* destination,
                                uint8_t* sample_size, const uint8_t* source,
                                size_t source_size)
{
    if (!destination || !sample_size) return;
    const size_t count = std::min(source_size, kConfigFooterSampleBytes);
    destination->fill(0u);
    if (source && count != 0u)
        std::copy_n(source, count, destination->begin());
    *sample_size = static_cast<uint8_t>(count);
}

bool config_texture_trace_admit(const char* stage, uint32_t* emitted,
                                bool* cap_reported)
{
    if (!config_texture_trace_enabled() || !emitted || !cap_reported)
        return false;
    const uint32_t cap = config_texture_trace_max();
    if (*emitted >= cap) {
        if (!*cap_reported) {
            *cap_reported = true;
            std::fprintf(stderr,
                         "[config-texture] stage=%s cap reached at %u — "
                         "further entries suppressed\n",
                         stage ? stage : "unknown", cap);
        }
        return false;
    }
    ++*emitted;
    return true;
}

// Silent-unsupported attribution (M2_CLT_UNSUPPORTED_DIAG=1, default off).
// Deep-race live-mixer runs report CLT unsupported=397-725 with zero
// [clt-reject] lines and malformed=0: the increments come from sites that
// never log (texture load/materialize register-path failures, kPointType),
// not from the reject_vertex paths. These helpers gate a bounded per-site
// [clt-reject] sample. Observation only: no raster/draw/discard change.
bool clt_unsupported_diag_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_UNSUPPORTED_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// 16bpp source-alpha tripwire (M2_CLT_SRCALPHA16_TRIPWIRE, default off).
// BMTYPE_16 stores no alpha and the hardware's synthesized value is unproven,
// so draws that need it are refused or approximated. Two sites report:
//   rgb-refused   -- the RGB equation reads source alpha and the draw is
//                    dropped. This is the case worth pursuing.
//   written-alpha -- RGB renders; only the written alpha is approximated (the
//                    known intro source tint hits this every run).
// Values: "1" exits (code 86) on the first rgb-refused hit and logs each
// distinct written-alpha register signature once; "log" never exits; "all"
// also exits on written-alpha. Observation only: nothing in rendering changes
// when it is unset.
void clt_srcalpha16_tripwire(const char* site, uint64_t list,
                             uint32_t dbuser, uint32_t dbsrc, uint32_t a,
                             uint32_t b, uint32_t alu, uint32_t srcalpha,
                             uint32_t destalpha)
{
    static const char* const mode = std::getenv("M2_CLT_SRCALPHA16_TRIPWIRE");
    if (!mode || mode[0] == '\0' || mode[0] == '0') return;
    const bool rgb_refused = std::strcmp(site, "rgb-refused") == 0;
    const bool exit_mode = std::strcmp(mode, "log") != 0 &&
                           (rgb_refused || std::strcmp(mode, "all") == 0);
    static std::mutex lock;
    static std::vector<std::array<uint32_t, 8>> seen;
    static uint32_t hits = 0;
    const std::array<uint32_t, 8> signature = {
        rgb_refused ? 1u : 0u, dbuser, dbsrc, a, b, alu, srcalpha, destalpha};
    std::lock_guard<std::mutex> guard(lock);
    ++hits;
    const bool first_of_kind =
        std::find(seen.begin(), seen.end(), signature) == seen.end();
    if (first_of_kind && seen.size() < 64u) seen.push_back(signature);
    if (first_of_kind || exit_mode) {
        std::fprintf(stderr,
                     "[clt-srcalpha16] hit=%u site=%s list=%llu dbuser=%#x "
                     "dbsrc=%#x a=%#x b=%#x alu=%#x srcalpha=%#x "
                     "destalpha=%#x\n",
                     hits, site, (unsigned long long)list, dbuser, dbsrc, a,
                     b, alu, srcalpha, destalpha);
        std::fflush(stderr);
    }
    if (exit_mode) std::_Exit(86);
}

// Opt-in RGB555 output dithering (M2_RGB555_DITHER, default off; exact "1" is
// on, any other value reads as off, matching the other launcher booleans).
// Off leaves CltBlendState::dither disabled, so the default image is
// byte-identical.
bool clt_rgb555_dither_enabled()
{
    const char* value = std::getenv("M2_RGB555_DITHER");
    return value && value[0] == '1' && value[1] == '\0';
}

uint32_t clt_unsupported_diag_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CLT_UNSUPPORTED_DIAG_MAX");
        if (!value || *value == '\0') return 64u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 1024ul));
    }();
    return max_entries;
}

// MMDMA load/use-state order trace (under M2_CLT_UNSUPPORTED_DIAG=1,
// default off; own counter against the same MAX ceiling).
// Deep-race packs stage several MMDMA blobs per list while TXTEXPTYPE /
// TXTUVMAX still hold earlier textures' values, so a failing load's
// load-time exptype alone cannot tell stale from real. This trace records
// every TLD (load-time registers + staged-blob result) and every
// TXTEXPTYPE write over a pending blob (written use-state + outcome) in
// list order. A 32-entry ring preserves the intra-list context before the
// first 0xe78 or stale-label sighting; that list streams live to the cap.
// Bounded by M2_CLT_UNSUPPORTED_DIAG_MAX (ceiling 1024); observation only:
// no raster/draw/discard change.
struct MmdmaOrderEntry {
    uint64_t list = 0;
    bool is_use = false;  // false = TLD load, true = TXTEXPTYPE use-state
    uint32_t ld_cntl = 0;
    uint32_t src = 0;
    uint32_t count = 0;
    uint32_t dst = 0;
    uint32_t exptype = 0;  // load: load-time register; use: written value
    uint32_t uvmax = 0;
    uint32_t blob_base = 0;
    size_t blob_bytes = 0;
    bool ok = false;
    // Load only: the TLD-time TXTEXPTYPE label fails every expansion
    // predicate, so the staged blob is deferred to the following use-state.
    bool load_label_unknown = false;
};

constexpr size_t kMmdmaOrderRing = 32u;

struct MmdmaOrderState {
    MmdmaOrderEntry ring[kMmdmaOrderRing];
    size_t pos = 0;
    uint64_t latched_list = UINT64_MAX;
    uint32_t emitted = 0;
    bool expired = false;
};

MmdmaOrderState& mmdma_order_state()
{
    static MmdmaOrderState state;
    return state;
}

void emit_mmdma_order_entry(const MmdmaOrderEntry& entry)
{
    std::fprintf(stderr,
                 "[clt-mmdma-order] list=%llu %s ld_cntl=%#x src=%#x "
                 "count=%#x dst=%#x exptype=%#x uvmax=%#x blob_base=%#x "
                 "blob_bytes=%zu ok=%u stale=%u\n",
                 (unsigned long long)entry.list,
                 entry.is_use ? "use" : "load", entry.ld_cntl, entry.src,
                 entry.count, entry.dst, entry.exptype, entry.uvmax,
                 entry.blob_base, entry.blob_bytes,
                 entry.ok ? 1u : 0u, entry.load_label_unknown ? 1u : 0u);
}

void log_mmdma_order_entry(const MmdmaOrderEntry& entry)
{
    MmdmaOrderState& state = mmdma_order_state();
    const uint32_t cap = clt_unsupported_diag_max();
    const bool stale_load = !entry.is_use && entry.load_label_unknown;
    if (state.latched_list == UINT64_MAX && entry.exptype != 0x0e78u &&
        !stale_load) {
        state.ring[state.pos % kMmdmaOrderRing] = entry;
        ++state.pos;
        return;
    }
    if (state.latched_list == UINT64_MAX) {
        state.latched_list = entry.list;
        const size_t held = std::min(state.pos, kMmdmaOrderRing);
        for (size_t i = 0; i < held; ++i) {
            const MmdmaOrderEntry& prior =
                state.ring[(state.pos - held + i) % kMmdmaOrderRing];
            if (prior.list != entry.list) continue;
            if (state.emitted >= cap) break;
            emit_mmdma_order_entry(prior);
            ++state.emitted;
        }
    }
    if (entry.list != state.latched_list) return;  // first latched list only
    if (state.emitted < cap) {
        emit_mmdma_order_entry(entry);
        ++state.emitted;
    } else if (!state.expired) {
        state.expired = true;
        std::fprintf(stderr, "[clt-mmdma-order] cap reached — further entries "
                             "suppressed\n");
    }
}

struct TextureFormat {
    uint32_t color_depth = 0;
    uint32_t alpha_depth = 0;
    bool transparent = false;
    bool has_ssb = false;
    bool has_color = false;
    bool has_alpha = false;
    bool literal = false;
};

struct PipControl {
    uint32_t ssb_select = kPipSelectConstant;
    uint32_t alpha_select = kPipSelectConstant;
    uint32_t color_select = kPipSelectConstant;
    uint32_t index_offset = 0;
    // False when the register word sits outside the toolkit's documented field
    // set.  Every consumer fails closed on it rather than reading masked
    // fields out of an unprogrammed word.
    bool documented = false;
};

PipControl decode_pip_control(uint32_t value)
{
    // Portfolio's PPC field macros place these fields at PPC bit positions
    // 17, 20, 23, and 31 respectively (the packed values are 14, 11, 8,
    // and 0 in the host integer).
    return {(value >> 14u) & 7u, (value >> 11u) & 7u,
            (value >> 8u) & 7u, value & 0xffu,
            clt_pip_control_documented(value)};
}

uint32_t pip_lookup_index(uint32_t input_index, uint32_t index_offset)
{
    // TXTPIPCNTL.PIPINDEXOFFSET is an 8-bit draw-stage segment offset. The
    // CLT toolkit says the generated index wraps from 255 to 0; it is not
    // masked to the destination TXTEXPTYPE color depth.
    return (input_index + index_offset) & 0xffu;
}

bool select_pip_component(uint32_t selector, uint32_t texture_value,
                          uint32_t pip_value, uint32_t constant_value,
                          uint32_t* selected)
{
    if (!selected) return false;
    switch (selector) {
    case kPipSelectConstant:
        *selected = constant_value;
        return true;
    case kPipSelectTexture:
        *selected = texture_value;
        return true;
    case kPipSelectPip:
        *selected = pip_value;
        return true;
    default:
        // The CLT field is seven bits wide, but the SDK defines only the
        // three source selectors above. Do not silently invent behavior for
        // an unclassified selector.
        return false;
    }
}

TextureFormat decode_format(uint32_t packed, unsigned slot)
{
    const unsigned shift = (slot & 1u) != 0 ? 16u : 0u;
    const uint32_t value = packed >> shift;
    TextureFormat format;
    format.color_depth = value & kFormatColorDepthMask;
    format.alpha_depth = (value >> 4u) & kFormatAlphaDepthMask;
    format.transparent = (value & kFormatIsTransparent) != 0;
    format.has_ssb = (value & kFormatHasSsb) != 0;
    format.has_color = (value & kFormatHasColor) != 0;
    format.has_alpha = (value & kFormatHasAlpha) != 0;
    format.literal = (value & kFormatIsLiteral) != 0;
    return format;
}

// M2TXLibrary.c:M2TX_IsLegal is the source-backed indexed TXTEXPTYPE
// denominator. The table is not the full cross-product of Idepth/Adepth/SSB:
// each listed combination is a legal format and the other combinations stay
// fail-closed. The zero-bit (0,0,0) library default is not a drawable texel
// and is intentionally excluded here.
bool is_indexed_tram_format(const TextureFormat& format)
{
    if (format.literal || format.transparent ||
        (format.has_color &&
         (format.color_depth == 0u || format.color_depth > 8u)) ||
        (!format.has_color && format.color_depth != 0u) ||
        (format.has_alpha && format.alpha_depth != 4u &&
         format.alpha_depth != 7u) ||
        (!format.has_alpha && format.alpha_depth != 0u))
        return false;

    const uint32_t format_key =
        format.color_depth * 100u +
        (format.has_alpha ? format.alpha_depth : 0u) * 10u +
        (format.has_ssb ? 1u : 0u);
    switch (format_key) {
    case 871u:
    case 840u:
    case 800u:
    case 741u:
    case 701u:
    case 600u:
    case 501u:
    case 471u:
    case 440u:
    case 400u:
    case 341u:
    case 301u:
    case 240u:
    case 200u:
    case 141u:
    case 101u:
    case 100u:
    case 71u:
    case 40u:
    case 1u:
        return true;
    default:
        return false;
    }
}

bool is_indexed_no_color_tram_format(const TextureFormat& format)
{
    return is_indexed_tram_format(format) && !format.has_color;
}

bool is_color_alpha_ssb_format(const TextureFormat& format)
{
    // Named support predicate for the source-authored 0xe74/0xe78 indexed
    // rows. Their storage decode is deliberately not specialized: like every
    // other indexed TRAM row it is SSB, alpha, then color index.
    return !format.literal && format.has_color && format.has_alpha &&
           format.has_ssb && !format.transparent &&
           format.alpha_depth == 7u &&
           (format.color_depth == 4u || format.color_depth == 8u);
}

bool is_rgb555_ssb_format(const TextureFormat& format)
{
    return format.literal && format.has_color && format.has_ssb &&
           !format.has_alpha && !format.transparent &&
           format.color_depth == 5u;
}

// Literal TRAM aliases with no color channel at all: the lift statically
// authors TXTEXPTYPE 0x1200 (SSB-only in dosquare1trans/dosquare2trans) and
// 0x1a70 (A7+SSB in initskids/UI_DrawHighScores_nameEntry). The vendor legal
// table classifies the same channel layouts as indexed rows; the literal bit
// is a guest encoding alias, not a new Table 3-4 row.
bool is_no_color_literal_format(const TextureFormat& format)
{
    if (!format.literal || format.has_color || !format.has_ssb ||
        format.transparent)
        return false;
    // M2TX_IsLegal classifies the no-color rows as indexed. The lift uses
    // literal-bit aliases 0x1200 and 0x1a70 for the two source-authored
    // no-color residents, so keep only those aliases here; an arbitrary
    // literal no-color/alpha combination is not promoted from the table.
    return !format.has_alpha || format.alpha_depth == 7u;
}

// Vendor M2TX_IsLegal's literal rows are RGB555+SSB, RGB888 without alpha or
// SSB, and RGB888+A7+SSB. M_SetupPreLitFogTrans/M_SetupDynLitFogTrans
// statically author the last of these (TXTEXPTYPE 0x1e78).
bool is_literal_rgb888_format(const TextureFormat& format)
{
    return format.literal && format.has_color &&
           format.color_depth == 8u && !format.transparent &&
           ((!format.has_alpha && format.alpha_depth == 0u) ||
            (format.has_alpha && format.alpha_depth == 7u &&
             format.has_ssb));
}

uint32_t register_index(uint16_t address)
{
    return static_cast<uint32_t>(address) / sizeof(uint32_t);
}

uint32_t register_value(const std::vector<uint32_t>& registers,
                        uint16_t address)
{
    if ((address & 3u) != 0) return 0;
    const uint32_t index = register_index(address);
    return index < registers.size() ? registers[index] : 0;
}

void apply_register(uint32_t* destination, uint32_t value, uint32_t operation)
{
    if (!destination) return;
    if (operation == 0x1000u)
        *destination |= value;
    else if (operation == 0x1800u)
        *destination &= ~value;
    else
        *destination = value;
}

bool decode_register_header(uint32_t header, uint32_t* count,
                            uint32_t* operation, uint32_t* address)
{
    if ((header & kTypeMask) != kRegisterType || !count || !operation ||
        !address)
        return false;

    *count = ((header & kRegisterCountMask) >> 16u) + 1u;
    // RC_WRITE_REGISTER | ((cnt-1)<<16) | (addr & 0xffff) | op.  The SET
    // (0x1000) and CLEAR (0x1800 = 0x1000|0x800) operations overlap the low
    // address field, so the address mask must clear the op bits while
    // retaining the DB-register high bits (0xe7ff keeps 0x8000/0x4000/0x2000).
    *operation = header & 0x1800u;
    if (*operation != 0u && *operation != 0x1000u && *operation != 0x1800u)
        return false;
    *address = header & kRegisterAddressMask;
    return (*address & 3u) == 0 && *address < 0x10000u &&
           *count <= (0x10000u - *address) / sizeof(uint32_t);
}

struct CltFlowTarget {
    bool valid = false;
    bool in_list = false;
    uint64_t address = 0;
    size_t word_index = 0;
};

// Resolve the address carried by DCNTLDATA for a JR/JA instruction. The
// vendor describes JR as a signed displacement from the jump instruction and
// JA as an absolute byte address. The command words are byte-addressed, so the
// displacement is not silently reinterpreted as a word count. The toolkit does
// not spell out the displacement origin; the host uses the DCNTL payload word
// (the instruction word it is executing) as the bounded fetch-PC mapping.
CltFlowTarget resolve_clt_flow_target(uint32_t command_list_base,
                                      size_t word_count,
                                      size_t instruction_word_index,
                                      uint32_t data, bool relative)
{
    CltFlowTarget result;
    const uint64_t begin = command_list_base;
    const uint64_t byte_count = static_cast<uint64_t>(word_count) * 4u;
    const uint64_t end = begin + byte_count;
    if (end < begin) return result;

    uint64_t target = 0;
    if (relative) {
        const int64_t offset = (data & 0x80000000u) != 0u
            ? static_cast<int64_t>(data) - 0x100000000LL
            : static_cast<int64_t>(data);
        const uint64_t instruction_address =
            begin + static_cast<uint64_t>(instruction_word_index) * 4u;
        if (instruction_address > 0x7fffffffffffffffULL) return result;
        const int64_t target_signed =
            static_cast<int64_t>(instruction_address) + offset;
        if (target_signed < 0) return result;
        target = static_cast<uint64_t>(target_signed);
    } else {
        target = data;
    }

    result.valid = (target & 3u) == 0;
    if (!result.valid) return result;
    result.address = target;
    if (target < begin || target >= end) return result;
    result.in_list = true;
    result.word_index = static_cast<size_t>((target - begin) / 4u);
    return result;
}

struct CltMemoryView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t content_hash = 0;
};

// 64-bit content identity for a snapshot range. The hash is per-process only
// (decode keys and renderer content caches); it is never persisted or compared
// with another process's value, so the exact mixing function is free as long
// as it is deterministic and well distributed. A paced-race sample put ~4 ms
// of serial StateDriver CPU per frame here: the per-byte FNV chain costs ~4-5 cycles per byte through its serial 64-bit multiply, and a
// race frame copies ~2.3 MB. Fold 8-byte words through two independent lanes
// so the multiply latency overlaps, then mix the lanes and the tail bytes.
uint64_t snapshot_content_hash(const uint8_t* data, size_t size)
{
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    constexpr uint64_t kLaneSeed = 0x9e3779b97f4a7c15ull;
    if (!data || size == 0) return kOffset;
    uint64_t lane0 = kOffset;
    uint64_t lane1 = kOffset ^ kLaneSeed;
    size_t i = 0;
    // The copied vector storage is normally 8-byte aligned; the byte loop
    // keeps the hash defined for any pointer without relying on that.
    while (i < size && (reinterpret_cast<uintptr_t>(data + i) & 7u) != 0u) {
        lane0 ^= data[i++];
        lane0 *= kPrime;
    }
    const size_t words = (size - i) / sizeof(uint64_t);
    for (size_t w = 0; w < words; ++w) {
        uint64_t value = 0;
        std::memcpy(&value, data + i + w * sizeof(uint64_t), sizeof(value));
        if ((w & 1u) == 0u) {
            lane0 ^= value;
            lane0 *= kPrime;
        } else {
            lane1 ^= value;
            lane1 *= kPrime;
        }
    }
    i += words * sizeof(uint64_t);
    while (i < size) {
        lane1 ^= data[i++];
        lane1 *= kPrime;
    }
    // Fold the lanes (boost-style mix) and keep zero reserved for "absent".
    uint64_t hash = lane0 ^ (lane1 + kLaneSeed + (lane0 << 6) + (lane0 >> 2));
    return hash != 0 ? hash : 1u;
}

CltMemoryView find_memory(const CltMemorySnapshot* memory,
                          uint32_t address, size_t length)
{
    if (!memory) return {};
    const uint64_t end = static_cast<uint64_t>(address) + length;
    CltMemoryView containing;
    for (const CltMemoryRange& range : memory->ranges) {
        const uint64_t range_begin = range.guest_address;
        const uint64_t range_end = range_begin + range.bytes.size();
        if (static_cast<uint64_t>(address) < range_begin ||
            end > range_end)
            continue;

        const size_t offset = static_cast<size_t>(address - range_begin);
        const size_t available = range.bytes.size() - offset;
        if (range_begin == address)
            return {range.bytes.data(), available,
                    range.bytes.size() == length ? range.content_hash : 0};
        if (available > containing.size)
            containing = {range.bytes.data() + offset, available, 0};
    }
    return containing;
}

void snapshot_range(CltMemorySnapshot* snapshot, const uint8_t* guest_memory,
                    uint32_t guest_memory_size, uint32_t address,
                    size_t length)
{
    if (!snapshot || !guest_memory || length == 0 ||
        address >= guest_memory_size)
        return;

    const size_t available = guest_memory_size - address;
    const size_t copy_length =
        std::min({length, available, kMaxTextureSnapshotBytes});
    if (copy_length == 0) return;

    const uint64_t request_end =
        static_cast<uint64_t>(address) + copy_length;
    for (const CltMemoryRange& existing : snapshot->ranges) {
        const uint64_t range_begin = existing.guest_address;
        const uint64_t range_end = range_begin + existing.bytes.size();
        if (static_cast<uint64_t>(address) >= range_begin &&
            request_end <= range_end) {
            perf::add_counter(perf::Counter::SnapshotRangesDeduplicated);
            return;
        }
    }

    CltMemoryRange range;
    range.guest_address = address;
    range.bytes.assign(guest_memory + address,
                       guest_memory + address + copy_length);
    range.content_hash = snapshot_content_hash(range.bytes.data(),
                                               range.bytes.size());
    if (snapshot->ranges.capacity() == snapshot->ranges.size())
        perf::add_counter(perf::Counter::SnapshotRangeVectorGrowths);
    snapshot->ranges.push_back(std::move(range));
    perf::add_bytes(perf::Phase::TextureSnapshotCopy, copy_length);
    perf::add_counter(perf::Counter::TextureSnapshotBytes, copy_length);
    perf::add_counter(perf::Counter::TextureSnapshotRanges);
}

// Bounded texture-byte dump: write the first few decoded RGBA rasters
// as PPMs under the existing draw-diagnostic floor so sky-vs-road can be told apart
// from authenticated guest bytes. No new env var; capped with an expiry line.
bool write_tex_ppm(const char* path, int width, int height,
                   const uint32_t* pixels)
{
    if (!path || !pixels || width <= 0 || height <= 0 || width > 2048 ||
        height > 2048)
        return false;
    // The default dump location is build/texdump, which does not exist in a
    // fresh tree; without this the opt-in diagnostic silently captures nothing.
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    FILE* out = std::fopen(path, "wb");
    if (!out) return false;
    std::fprintf(out, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        const uint32_t p = pixels[i];
        const uint8_t rgb[3] = {
            static_cast<uint8_t>(p & 0xffu),
            static_cast<uint8_t>((p >> 8u) & 0xffu),
            static_cast<uint8_t>((p >> 16u) & 0xffu),
        };
        std::fwrite(rgb, 1, 3, out);
    }
    std::fclose(out);
    return true;
}

void dump_tex_pixels(int width, int height, const uint32_t* pixels,
                     const char* tag, uint32_t exp_type)
{
    // Process-lifetime gates, cached: the materialization paths call this for
    // every decoded texture, and an uncached pair of environment lookups per
    // call showed up as measurable pump time in the race profile.
    static const bool dump_diag = [] {
        const char* e = std::getenv("M2_DRAW_DIAG");
        return e && *e && *e != '0';
    }();
    if (!dump_diag) return;
    static const uint32_t from = [] {
        const char* f = std::getenv("M2_DRAW_DIAG_FROM");
        return f ? static_cast<uint32_t>(strtoul(f, nullptr, 0)) : 0u;
    }();
    const uint64_t frame =
        perf::counter_value(perf::Counter::GuestFramesStarted);
    if (frame < from) return;
    // Bounded 0xe78 capture: the deep-race 0xe78 resident submits in a
    // run-dependent window that the generic first-N cap demonstrably
    // starves (a 256-entry budget exhausted by unrelated textures before
    // the window opens). Exempt it with its own budget of 4 so one bounded
    // run always retains the live 21x10 decode for the bit-order sanity
    // check. Same diagnostic floor, same directory, distinct prefix.
    if (exp_type == 0x0e78u) {
        static uint32_t e78_dumped = 0;
        static bool e78_expired = false;
        constexpr uint32_t kE78DumpMax = 4u;
        if (e78_dumped < kE78DumpMax) {
            char e78_path[256];
            std::snprintf(e78_path, sizeof(e78_path),
                          "build/texdump/tex_e78_%02u_f%llu_%s_%dx%d.ppm",
                          e78_dumped, (unsigned long long)frame,
                          tag ? tag : "unk", width, height);
            if (write_tex_ppm(e78_path, width, height, pixels)) {
                std::fprintf(stderr, "[texdump] n=e78_%u %s frame=%llu\n",
                             e78_dumped, e78_path,
                             (unsigned long long)frame);
                ++e78_dumped;
            }
        }
        if (e78_dumped == kE78DumpMax && !e78_expired) {
            e78_expired = true;
            std::fprintf(stderr,
                         "[texdump] 0xe78 cap reached at frame %llu — "
                         "further entries suppressed\n",
                         (unsigned long long)frame);
        }
        return;
    }
    static uint32_t dumped = 0;
    static bool expired = false;
    static const uint32_t dump_max = [] {
        const char* m = std::getenv("M2_TEXDUMP_MAX");
        if (!m || !*m) return 16u;
        const long v = std::strtol(m, nullptr, 0);
        if (v < 16L) return 16u;
        if (v > 256L) return 256u;
        return static_cast<uint32_t>(v);
    }();
    if (dumped >= dump_max) {
        if (!expired) {
            expired = true;
            std::fprintf(stderr,
                         "[texdump] cap reached at frame %llu — further "
                         "entries suppressed\n",
                         (unsigned long long)frame);
        }
        return;
    }
    char path[256];
    std::snprintf(path, sizeof(path),
                  "build/texdump/tex_%02u_f%llu_%s_%dx%d_exp%#x.ppm",
                  dumped, (unsigned long long)frame, tag ? tag : "unk",
                  width, height, exp_type);
    if (write_tex_ppm(path, width, height, pixels)) {
        std::fprintf(stderr, "[texdump] n=%u %s frame=%llu\n", dumped, path,
                     (unsigned long long)frame);
        ++dumped;
    }
    if (dumped == dump_max && !expired) {
        expired = true;
        std::fprintf(stderr,
                     "[texdump] cap reached at frame %llu — further entries "
                     "suppressed\n",
                     (unsigned long long)frame);
    }
}

uint32_t read_be32(const uint8_t* bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24u) |
           (static_cast<uint32_t>(bytes[1]) << 16u) |
           (static_cast<uint32_t>(bytes[2]) << 8u) |
           static_cast<uint32_t>(bytes[3]);
}

// A cross-list jump target is a command list in guest RAM, and the
// interpreter deliberately keeps no guest pointer, so the target's words
// have to ride along in the same snapshot the texture and DB-source spans
// use. The window is sized from the vendor's own TextState sizing sum
// (font/telists.c:210-217): 21 words per character plus the fixed
// init/dblend/ret/sync snippets and one texture load per range, so
// kCrossListSnapshotWords covers several hundred characters of text with
// room to spare. A list longer than the window stops at the snapshot edge
// and the walk refuses there, so an over-long list degrades to a
// fail-closed refusal rather than a silently truncated draw.
constexpr size_t kCrossListSnapshotWords = 8192u;
// Bound the extra copies a malformed stream can ask for: a list full of
// jump bits must not be able to grow the snapshot without limit. The
// vendor's own call/return pairs (DrawText -> TE list, M_Draw -> primitive
// list, clip.c -> clip vertex list) need one target each.
constexpr uint32_t kMaxCrossListSnapshots = 4u;
// A malformed list must not turn cross-list flow into an unbounded
// interpreter loop. Every call/return pair the checked-in Portfolio
// sources build nests one list deep, so a few levels of headroom covers
// the vendor's own shapes; a deeper or repeating chain is refused rather
// than walked. The walk and the submit-time scan share these bounds so
// both cover the same lists.
constexpr size_t kMaxCltListDepth = 8u;
constexpr size_t kMaxCrossListMoves = 64u;

// The scan follows the same cross-list call the walk does. Both the walk and
// this pass read command lists out of guest RAM, so a list the walk will
// enter must also be a list this pass scans: the font folio's TE list, for
// instance, carries the glyph atlas's texture load, and a scan that stopped
// at the submitted list would leave that source unmapped and the walker's
// load fail closed as src-short.
void collect_texture_memory(const std::vector<uint32_t>& submitted_words,
                            const uint8_t* guest_memory,
                            uint32_t guest_memory_size,
                            uint32_t command_list_base,
                            size_t command_list_word_count,
                            std::vector<uint32_t>* registers,
                            CltMemorySnapshot* snapshot)
{
    if (!snapshot || !guest_memory || guest_memory_size == 0 || !registers)
        return;
    if (registers->size() != kRegisterWords)
        registers->assign(kRegisterWords, 0u);
    size_t cross_list_moves = 0;

    // The list currently being scanned. `words`/`word_count` follow the top
    // frame, exactly as the walk's own view does.
    struct ScanFrame {
        const uint32_t* words = nullptr;
        size_t word_count = 0;
        uint32_t base = 0;
        size_t resume_cursor = 0;
        // The snapshot is a raw, never-byteswapped copy of guest RAM and the
        // scan decodes big-endian words, so an entered list keeps its own
        // swapped copy for as long as the frame is on the stack.
        std::vector<uint32_t> owned;
    };
    std::vector<ScanFrame> frames;
    frames.reserve(kMaxCltListDepth + 1u);
    frames.emplace_back();
    frames.back().words = submitted_words.data();
    frames.back().word_count = submitted_words.size();
    frames.back().base = command_list_base;
    const uint32_t* words = frames.back().words;
    size_t word_count = frames.back().word_count;
    size_t cursor = 0;
    // A cross-list entry is deferred to the top of the loop: the register
    // packet that carried the jump is still being read, and `cursor` has to
    // finish that packet before it can mean anything in another list.
    bool scan_enter = false;
    for (;;) {
        if (scan_enter) {
            scan_enter = false;
            words = frames.back().words;
            word_count = frames.back().word_count;
            cursor = 0;
            continue;
        }
        if (cursor >= word_count) {
            // This list is scanned. Resume the caller after the jump that
            // entered it; the submitted list owns no caller and ends the
            // scan.
            if (frames.size() <= 1u) return;
            frames.pop_back();
            words = frames.back().words;
            word_count = frames.back().word_count;
            cursor = frames.back().resume_cursor;
            continue;
        }
        const uint32_t header = words[cursor++];
        const uint32_t type = header & kTypeMask;
        if (type == kRegisterType) {
            uint32_t count = 0, operation = 0, address = 0;
            if (!decode_register_header(header, &count, &operation, &address) ||
                count > word_count - cursor)
                return;

            const uint32_t first = address / sizeof(uint32_t);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t register_address =
                    address + i * sizeof(uint32_t);
                apply_register(&(*registers)[first + i], words[cursor++],
                               operation);
                const uint32_t dccntl = (*registers)[first + i];
                if (register_address == kDcCntl &&
                    (dccntl & kDcTld) != 0) {
                    const uint32_t load_control =
                        register_value(*registers, kTxtLdCntl);
                    const uint32_t source_address =
                        register_value(*registers, kTxtLdSrcAddr);
                    const uint32_t count_value =
                        register_value(*registers, kTxtCount) & 0x00ffffffu;
                    const uint32_t ld_width =
                        register_value(*registers, kTxtLdWidth);
                    const uint32_t mode =
                        (load_control >> kTxtLdModeShift) & kTxtLdModeMask;

                    size_t source_bytes = 0;
                    if (mode == 2u || mode == 1u) {
                        source_bytes = count_value;
                    } else if (mode == 0u &&
                               (load_control & kTxtLdCompressed) != 0) {
                        const uint64_t estimated =
                            static_cast<uint64_t>(count_value) * 2u + 4096u;
                        source_bytes = static_cast<size_t>(std::min<uint64_t>(
                            estimated, kMaxTextureSnapshotBytes));
                    } else if (mode == 0u) {
                        const uint32_t source_bit_offset = load_control & 7u;
                        uint32_t source_row_bits = ld_width & 0xffffu;
                        uint32_t destination_row_bits =
                            (ld_width >> 16u) & 0xffffu;
                        if (source_row_bits == 0 && count_value > 0) {
                            source_bytes =
                                static_cast<size_t>(count_value) * 432u * 2u;
                        } else {
                            const uint64_t required_bits =
                                source_bit_offset +
                                (count_value == 0
                                     ? 0
                                     : static_cast<uint64_t>(count_value - 1u) *
                                           source_row_bits +
                                       destination_row_bits);
                            source_bytes = static_cast<size_t>(std::min<uint64_t>(
                                (required_bits + 7u) / 8u,
                                kMaxTextureSnapshotBytes));
                        }
                    }
                    if (config_footer_texture_load(source_address,
                                                    count_value)) {
                        static uint32_t source_emitted = 0;
                        static bool source_cap_reported = false;
                        const bool emit = config_texture_trace_admit(
                            "submit", &source_emitted,
                            &source_cap_reported);
                        if (emit) {
                            const size_t available =
                                source_address < guest_memory_size
                                    ? guest_memory_size - source_address
                                    : 0u;
                            const size_t hash_bytes =
                                std::min(source_bytes, available);
                            const TextureFormat expanded = decode_format(
                                register_value(*registers, kTxtExpType), 0);
                            std::fprintf(
                                stderr,
                                "[config-texture] stage=submit source=%#x "
                                "source_span=%#zx hash_bytes=%#zx "
                                "count=%#x ld_width=%#x ld_cntl=%#x mode=%u "
                                "exp=%#x fmt=(c%u,a%u,ssb%d,color%d,alpha%d,lit%d) "
                                "source_hash=%08x sample=",
                                source_address, source_bytes, hash_bytes,
                                count_value, ld_width, load_control, mode,
                                register_value(*registers, kTxtExpType),
                                expanded.color_depth, expanded.alpha_depth,
                                expanded.has_ssb ? 1 : 0,
                                expanded.has_color ? 1 : 0,
                                expanded.has_alpha ? 1 : 0,
                                expanded.literal ? 1 : 0,
                                config_texture_hash_bytes(
                                    source_address < guest_memory_size
                                        ? guest_memory + source_address
                                        : nullptr,
                                    hash_bytes));
                            const size_t sample =
                                std::min(hash_bytes, kConfigFooterSampleBytes);
                            for (size_t j = 0; j < sample; ++j)
                                std::fprintf(stderr, "%02x",
                                             guest_memory[source_address + j]);
                            std::fprintf(stderr, "\n");
                        }
                    }
                    snapshot_range(snapshot, guest_memory, guest_memory_size,
                                   source_address, source_bytes);
                }
                // Cross-list call: a jump whose target is a different
                // command list in guest RAM. The vendor's own folios build
                // exactly this (font/telists.c DrawText jumps from the app's
                // GState list into the TextState's TE list and back through
                // ts_TERet; M_Draw.c and font/clip.c use the same
                // call/return pair). The target list is copied in and then
                // scanned, because the spans the walk will need from it --
                // the font folio's glyph atlas load above all -- are
                // addressed from that list, not from this one.
                if (register_address == kDcCntl &&
                    (dccntl & (kDcJa | kDcJr)) != 0u &&
                    cross_list_moves < kMaxCrossListMoves &&
                    frames.size() < kMaxCltListDepth) {
                    const CltFlowTarget flow = resolve_clt_flow_target(
                        command_list_base, word_count, cursor - 1u,
                        (*registers)[register_index(kDcCntlData)],
                        (dccntl & kDcJr) != 0u);
                    if (flow.valid && !flow.in_list) {
                        // A target already on the stack is a return into a
                        // list this pass has scanned; there is nothing left
                        // to collect from it.
                        bool on_stack = false;
                        for (size_t i = frames.size(); i-- > 0;) {
                            const uint64_t begin = frames[i].base;
                            if (flow.address >= begin &&
                                flow.address <
                                    begin + frames[i].word_count * 4ull) {
                                on_stack = true;
                                break;
                            }
                        }
                        if (!on_stack &&
                            cross_list_moves < kMaxCrossListSnapshots) {
                            snapshot_range(snapshot, guest_memory,
                                           guest_memory_size, flow.address,
                                           kCrossListSnapshotWords * 4u);
                            const CltMemoryView view =
                                find_memory(snapshot, flow.address, 4u);
                            if (view.data != nullptr && view.size >= 4u) {
                                frames.emplace_back();
                                ScanFrame& entered = frames.back();
                                entered.base = flow.address;
                                entered.word_count = view.size / 4u;
                                entered.owned.resize(entered.word_count);
                                for (size_t w = 0; w < entered.word_count; ++w) {
                                    entered.owned[w] = read_be32(
                                        view.data + w * sizeof(uint32_t));
                                }
                                entered.words = entered.owned.data();
                                frames[frames.size() - 2u].resume_cursor =
                                    cursor;
                                ++cross_list_moves;
                                scan_enter = true;
                            }
                        }
                    }
                }
                // DB-source 2D copy blit: snapshot the armed source buffer
                // (rgb555, 432 rows) so the render thread can sample it on the
                // untextured background quad.  The guest may arm the base
                // (0x8034) in a different list from the stride (0x8038) — the
                // State 2 showroom lists write only the base per band — so
                // both writes snapshot using the other half of the pair.
                if (register_address == kDbSrcXStride ||
                    register_address == kDbSrcBaseAddr ||
                    register_address == kDbSrcCntl) {
                    const uint32_t db_base = register_value(*registers, kDbSrcBaseAddr);
                    const uint32_t db_stride = register_value(*registers, kDbSrcXStride) & kDbSrcXStrideMask;
                    const size_t pixel_bytes = (register_value(*registers, kDbSrcCntl) & kDbSrc32Bpp) ? 4u : 2u;
                    if (db_base && db_stride)
                        snapshot_range(snapshot, guest_memory, guest_memory_size,
                            db_base, static_cast<size_t>(db_stride) * 432u * pixel_bytes);
                }
            }
            continue;
        }

        if (type == kVertexType) {
            const uint32_t count = (header & 0xffffu) + 1u;
            const bool shaded = (header & kVertexShading) != 0;
            const bool textured = (header & kVertexTexture) != 0;
            const bool perspective = (header & kVertexPerspective) != 0;
            const size_t words_per_vertex =
                2u + (shaded ? 4u : 0u) + (perspective ? 1u : 0u) +
                (textured ? 2u : 0u);
            const uint64_t vertex_words =
                static_cast<uint64_t>(count) * words_per_vertex;
            if (vertex_words > word_count - cursor) return;
            cursor += static_cast<size_t>(vertex_words);
            continue;
        }

        return;
    }
}

class BitReader {
public:
    BitReader(const uint8_t* bytes, size_t byte_count)
        : m_bytes(bytes), m_byte_count(byte_count) {}

    bool read(unsigned bit_count, uint32_t* value)
    {
        if (!value || bit_count > 32u) return false;
        *value = 0;
        const uint64_t total_bits = static_cast<uint64_t>(m_byte_count) * 8u;
        if (m_bit_position > total_bits ||
            bit_count > total_bits - m_bit_position)
            return false;

        for (unsigned i = 0; i < bit_count; ++i) {
            const uint64_t bit = m_bit_position + i;
            const uint8_t source = m_bytes[bit / 8u];
            *value = (*value << 1u) | ((source >> (7u - (bit % 8u))) & 1u);
        }
        m_bit_position += bit_count;
        return true;
    }

    bool skip(uint64_t bit_count)
    {
        const uint64_t total_bits = static_cast<uint64_t>(m_byte_count) * 8u;
        if (m_bit_position > total_bits ||
            bit_count > total_bits - m_bit_position)
            return false;
        m_bit_position += bit_count;
        return true;
    }

private:
    const uint8_t* m_bytes = nullptr;
    size_t m_byte_count = 0;
    uint64_t m_bit_position = 0;
};


struct PipColor {
    uint32_t rgba = 0;
    uint8_t ssb = 0;
};

PipColor decode_pip(uint32_t value)
{
    const uint32_t red = (value >> 16u) & 0xffu;
    const uint32_t green = (value >> 8u) & 0xffu;
    const uint32_t blue = value & 0xffu;
    const uint32_t alpha_byte = (value >> 24u) & 0xffu;
    const uint32_t alpha = ((alpha_byte & 0x7fu) << 1u) |
                           (alpha_byte & 1u);
    return {red | (green << 8u) | (blue << 16u) | (alpha << 24u),
            static_cast<uint8_t>((value >> 31u) & 1u)};
}

const PipColor& pip_constant_for_ssb(uint32_t texture_ssb,
                                     const PipColor& constant0,
                                     const PipColor& constant1)
{
    // TXTPIPCNTL's CONSTANT source is TxtPIPConst0/1. The toolkit specifies
    // that the raw SSB coming from the TRAM lookup chooses between them; the
    // selected SSB/alpha/color outputs may then independently use that
    // constant. TXTCONST2/3 are TAB constants, not PIP constants.
    return texture_ssb != 0u ? constant1 : constant0;
}

// Literal 5/8-bit color and 4/7-bit alpha expansion to the host's 8-bit
// channels per the CLT toolkit's TXTLOAD expansion section:
//   "Expansion from 555 to 888 is done by replication of MSBs" and
//   "the top three MSBs of the 4-bit source alpha are replicated into the
//    bottom three LSBs of the expanded alpha".
// That one documented rule covers every place a stored or source channel
// field is widened to the host representation: the full-load resident decode,
// the lazy MMDMA/compressed resident materialization, and the compressed
// source-channel expansion (the same section describes source-vs-expanded
// channel depths). Applying it per load path instead would decode the same
// TRAM field two different ways.
//
// The toolkit's host-side M2TXcompress.c decompressor instead fills the low
// bits from the source LSB (qmem.h oneExtend). That utility produces the
// Mercury preview's raw planes, not TE output, and is not used for any
// production channel widening here; the instrumentation probe below keeps it
// only so the differential matrix in m2_guest_clt_test can show the two rules
// apart instead of letting a path silently drift onto the other one.
uint32_t replicate_msb(unsigned depth, uint32_t value)
{
    if (depth == 0u) return 0u;
    if (depth >= 8u) return value & 0xffu;
    const uint32_t masked = value & ((1u << depth) - 1u);
    if (depth >= 4u)
        return (masked << (8u - depth)) | (masked >> (2u * depth - 8u));
    // Widths 1..3 are index widths, not channel widths: M2TX_IsLegal's literal
    // rows are 555/888 and the documented alpha widths are 4/7, so no legal
    // format reaches this widening. The doc's "replication of MSBs" is
    // applied by repeating the whole field left to fill the byte; this is an
    // inferred extension for widths the toolkit does not define, recorded as
    // such in the graphics recovery contracts.
    uint32_t out = masked;
    for (unsigned filled = depth; filled < 8u;) {
        const unsigned take = std::min(8u - filled, depth);
        out = (out << take) | (masked >> (depth - take));
        filled += take;
    }
    return out & 0xffu;
}

// Shared indexed-resident materialization: the stored TRAM index selects a
// PIP entry (plus the TXTPIPCNTL index offset), and the three TXTPIPCNTL
// selectors independently choose color, alpha, and SSB from the texture
// channel, the PIP entry, or the SSB-selected constant. Selector values the
// SDK does not define, and a non-literal texture-color request, stay
// fail-closed.
bool materialize_indexed_texel(const std::vector<uint32_t>& pip,
                               uint32_t pip_control_value,
                               const PipColor& constant0,
                               const PipColor& constant1,
                               uint32_t source_index, uint32_t texture_alpha,
                               uint32_t texture_ssb, uint32_t* rgba,
                               uint8_t* ssb)
{
    if (!rgba || !ssb) return false;
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return false;
    const uint32_t pip_index =
        pip_lookup_index(source_index, pip_control.index_offset);
    if (pip_index >= pip.size()) return false;
    const PipColor pip_entry = decode_pip(pip[pip_index]);
    const PipColor& constant =
        pip_constant_for_ssb(texture_ssb, constant0, constant1);
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    if (pip_control.color_select == kPipSelectPip) {
        red = pip_entry.rgba & 0xffu;
        green = (pip_entry.rgba >> 8u) & 0xffu;
        blue = (pip_entry.rgba >> 16u) & 0xffu;
    } else if (pip_control.color_select == kPipSelectConstant) {
        red = constant.rgba & 0xffu;
        green = (constant.rgba >> 8u) & 0xffu;
        blue = (constant.rgba >> 16u) & 0xffu;
    } else {
        // An indexed destination has no independent RGB stream for the
        // TEXTURE selector.
        return false;
    }
    uint32_t alpha = 0;
    if (!select_pip_component(pip_control.alpha_select, texture_alpha,
                              (pip_entry.rgba >> 24u) & 0xffu,
                              (constant.rgba >> 24u) & 0xffu, &alpha))
        return false;
    uint32_t selected_ssb = 0;
    if (!select_pip_component(pip_control.ssb_select, texture_ssb,
                              pip_entry.ssb, constant.ssb, &selected_ssb))
        return false;
    *rgba = red | (green << 8u) | (blue << 16u) | (alpha << 24u);
    *ssb = static_cast<uint8_t>(selected_ssb != 0u);
    return true;
}

// A no-color TRAM format has no PIP index. Its missing RGB channel is supplied
// by the SSB-selected PIP constant; the raw alpha/SSB fields, when present,
// remain independently selectable. The toolkit's literal no-color aliases
// and its indexed no-color rows therefore share this use-state operation.
bool materialize_no_color_texel(uint32_t pip_control_value,
                                const PipColor& constant0,
                                const PipColor& constant1,
                                uint32_t texture_alpha,
                                uint32_t texture_ssb, uint32_t* rgba,
                                uint8_t* ssb)
{
    if (!rgba || !ssb) return false;
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return false;
    const PipColor& constant =
        pip_constant_for_ssb(texture_ssb, constant0, constant1);
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    switch (pip_control.color_select) {
    case kPipSelectConstant:
        red = constant.rgba & 0xffu;
        green = (constant.rgba >> 8u) & 0xffu;
        blue = (constant.rgba >> 16u) & 0xffu;
        break;
    case kPipSelectTexture:
        // No color bits are present in the TRAM word.
        break;
    default:
        // There is no PIP index to address the PIP RAM for a no-color texel.
        return false;
    }

    uint32_t alpha = 0;
    switch (pip_control.alpha_select) {
    case kPipSelectConstant:
        alpha = (constant.rgba >> 24u) & 0xffu;
        break;
    case kPipSelectTexture:
        alpha = texture_alpha;
        break;
    default:
        return false;
    }

    uint32_t selected_ssb = 0;
    switch (pip_control.ssb_select) {
    case kPipSelectConstant:
        selected_ssb = constant.ssb;
        break;
    case kPipSelectTexture:
        selected_ssb = texture_ssb;
        break;
    default:
        return false;
    }
    *rgba = red | (green << 8u) | (blue << 16u) | (alpha << 24u);
    *ssb = static_cast<uint8_t>(selected_ssb != 0u);
    return true;
}

bool bridge_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_BRIDGE_TRACE");
        return value && (value[0] == '1' || value[0] == 'y' ||
                         value[0] == 'Y' || std::string(value) == "full");
    }();
    return enabled;
}

bool bridge_trace_full()
{
    const char* value = std::getenv("M2_CLT_BRIDGE_TRACE");
    return value && std::string(value) == "full";
}

bool clt_super_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_SUPER_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t clt_super_trace_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CLT_SUPER_TRACE_MAX");
        if (!value || *value == '\0') return 128u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 512ul));
    }();
    return max_entries;
}

bool clt_z_surface_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_Z_SURFACE_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t clt_z_surface_trace_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CLT_Z_SURFACE_TRACE_MAX");
        if (!value || *value == '\0') return 256u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 1024ul));
    }();
    return max_entries;
}

uint64_t clt_z_surface_trace_from()
{
    static const uint64_t from_frame = [] {
        const char* value = std::getenv("M2_CLT_Z_SURFACE_TRACE_FROM");
        if (!value || *value == '\0') return uint64_t(0);
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return from_frame;
}

// M2_CLT_SELECT_DUMP gates a diagnostic-only sample of what a destination and
// its armed DB source hold at their first selection in each guest frame, before
// any of that list's draws reach the destination. M2_CLT_SELECT_DUMP_FROM
// lower-bounds the host frame. This is the datum needed at a transition seed:
// it separates a retained destination from a wrongly resolved source, and the
// later consumption sample then shows what the seed actually established.
bool clt_select_dump_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_SELECT_DUMP");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t clt_select_dump_from()
{
    static const uint32_t from = [] {
        const char* value = std::getenv("M2_CLT_SELECT_DUMP_FROM");
        return value && value[0] != '\0'
                   ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
                   : 0u;
    }();
    return from;
}

// M2_CLT_Z_CELL_DUMP live site (diagnostic-only): read the first K raw guest
// Z cells at z_base + 2*(x+y*stride) from the submit-time snapshot through
// the same find_memory accessor the payload/sheet reads use. The enqueue
// path snapshots exactly this window when DUMP is set (production depth
// never addresses guest Z cells, so no other span covers it). Fires once,
// for the first executed list at/after M2_CLT_Z_SURFACE_TRACE_FROM; the DUMP
// gate is independent of M2_CLT_Z_SURFACE_TRACE. Read-only: never alters
// rendering and never byteswaps production state. Unmapped/OOB bases fail
// closed with one line, never a crash.
// Calibration root cause: m_exec_seq never advanced (zero
// mutation sites), so any FROM floor above zero suppressed every list and a
// floored dump printed no lines; the trace shares the same frozen counter,
// so its window never selected either. The counter now advances once per
// execute() while a list-gated diagnostic is armed (clt_exec_seq_live) and
// stays frozen otherwise, preserving production behavior.
void maybe_dump_z_cells_once(uint64_t exec_seq, const CltMemorySnapshot* memory,
                              uint32_t z_base, uint32_t stride_words)
{
    const uint32_t word_count = clt_z_cell_probe::dump_word_count();
    if (word_count == 0) return;
    if (exec_seq < clt_z_surface_trace_from()) return;
    static bool fired = false;
    if (fired) return;
    fired = true;
    char line[192];
    if (z_base == 0 || stride_words == 0 || stride_words > 2048u) {
        clt_z_cell_probe::format_unmapped_line(line, sizeof(line), exec_seq,
                                                z_base, stride_words,
                                                word_count);
        std::fprintf(stderr, "%s\n", line);
        return;
    }
    const uint64_t span = clt_z_cell_probe::dump_span_bytes(word_count,
                                                             stride_words);
    const uint64_t end =
        static_cast<uint64_t>(z_base) + static_cast<uint64_t>(span);
    CltMemoryView view;
    if (span == 0 || span > kMaxTextureSnapshotBytes ||
        end > 0x100000000ull) {
        view = CltMemoryView{};
    } else {
        view = find_memory(memory, z_base, static_cast<size_t>(span));
    }
    if (!view.data || view.size < span) {
        clt_z_cell_probe::format_unmapped_line(line, sizeof(line), exec_seq,
                                                z_base, stride_words,
                                                word_count);
        std::fprintf(stderr, "%s\n", line);
        return;
    }
    clt_z_cell_probe::format_header_line(line, sizeof(line), exec_seq, z_base,
                                          stride_words, word_count);
    std::fprintf(stderr, "%s\n", line);
    for (uint32_t i = 0; i < word_count; ++i) {
        uint32_t x = 0, y = 0;
        clt_z_cell_probe::coords_for_index(i, stride_words, &x, &y);
        const uint32_t offset =
            clt_z_cell_probe::cell_offset_bytes(x, y, stride_words);
        clt_z_cell_probe::format_cell_line(
            line, sizeof(line), exec_seq, x, y, z_base + offset,
            view.data[offset], view.data[offset + 1u]);
        std::fprintf(stderr, "%s\n", line);
    }
}


const char* clt_z_surface_register_name(uint16_t address)
{
    switch (address) {
    case kDbSuperGenControl: return "DBSUPERGENCONTROL";
    case kDbUserControl: return "DBUSERCONTROL";
    case kDbDiscardControl: return "DBDISCARDCONTROL";
    case kDbFbClip: return "DBFBCLIP";
    case kDbXWinClip: return "DBXWINCLIP";
    case kDbYWinClip: return "DBYWINCLIP";
    case kDbDestControl: return "DBDESTCONTROL";
    case kDbDestBaseAddr: return "DBDESTBASEADDR";
    case kDbDestXStride: return "DBDESTXSTRIDE";
    case kDbSrcCntl: return "DBSRCCNTL";
    case kDbSrcBaseAddr: return "DBSRCBASEADDR";
    case kDbSrcXStride: return "DBSRCXSTRIDE";
    case kDbSrcOffset: return "DBSRCOFFSET";
    case kDbZCntl: return "DBZCNTL";
    case kDbZBaseAddr: return "DBZBASEADDR";
    case kDbZOffset: return "DBZOFFSET";
    case kDbZClip: return "DBZCLIP";
    case kDbSsbDsbCntl: return "DBSSBDSBCNTL";
    default: return nullptr;
    }
}

bool clt_frame_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_FRAME_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool clt_persist_target_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_PERSIST_TARGET");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// Bounded CLT transaction trace (M2_CLT_TRANSACTION_TRACE). Correlates one
// presenter iteration with the guest frames, pump phases, target/clear state,
// display selection, and scanout that produced it. Observation only.
bool clt_transaction_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_TRANSACTION_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// Optional host-frame floor for the transaction trace, so a bounded race
// window can be captured without spending the line budget on boot. Earlier
// frames emit nothing and do not consume the cap.
uint32_t clt_transaction_trace_from()
{
    static const uint32_t from = [] {
        const char* value = std::getenv("M2_CLT_TRANSACTION_TRACE_FROM");
        if (!value || value[0] == '\0') return 0u;
        return static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
    }();
    return from;
}

bool clt_ring_audit_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_TE_RING_AUDIT");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool livery_list_diag_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_LIVERY_LIST_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

struct LiveryListDiagState {
    uint64_t submissions = 0;
    uint64_t marker_lists = 0;
    uint32_t emitted = 0;
    bool cap_reported = false;
};

LiveryListDiagState& livery_list_diag_state()
{
    static LiveryListDiagState state;
    return state;
}

uint32_t livery_list_diag_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_LIVERY_LIST_DIAG_MAX");
        if (!value || *value == '\0') return 64u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 256ul));
    }();
    return max_entries;
}
bool livery_draw_diag_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_LIVERY_DRAW_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// Documented start-frame gate for the interpreter-side livery draw detail.
// Previously only the soft-capture backend consumed
// M2_LIVERY_DRAW_DIAG_FRAME, so the interpreter detail always started at
// the first matching draw and could exhaust its cap before a late pop.
// Keyed on the HOST frame (set per pump): GuestFramesStarted does not
// advance on the m2host fast-forward path, so a guest-frame gate would
// never open there. Shot cadences (M2_SHOT_FRAMES) are host frames too.
uint64_t livery_draw_diag_frame_from()
{
    static const uint64_t from = [] {
        const char* value = std::getenv("M2_LIVERY_DRAW_DIAG_FRAME");
        if (!value || *value == '\0') return uint64_t(0);
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return from;
}
// pump while M2_LIVERY_DRAW_DIAG=1, so a present/missing transition is
// visible without dumping every draw. Bounded: at most 16 distinct
// cel/lod pairs retained per frame; counters only otherwise. One line per
// HOST frame: pump runs many times per host frame, so the line for frame N
// emits on rollover to N+1 (the run tail's final frame is not emitted).
struct LiveryFrameTally {
    bool active = false;
    uint32_t host_frame = 0;
    uint32_t markers = 0;
    uint32_t mmdma = 0;
    uint32_t draws = 0;
    uint32_t onscreen = 0;
    uint32_t texrejects = 0;
    uint32_t dbdraws = 0;
    // Generic textured-draw census: the 192x112 fields above never fired in
    // m2host State 2 while decals visibly render, so the summary must show
    // what textured draws DO exist per frame (transition detection for any
    // resident, not just the assumed one). Bounded like pairs.
    uint32_t texdraws = 0;
    uint32_t texscreen = 0;
    uint64_t texdims[16] = {};
    uint32_t ntexdims = 0;
    uint64_t pairs[16] = {};
    uint32_t npairs = 0;
};

LiveryFrameTally& livery_tally()
{
    static LiveryFrameTally tally;
    return tally;
}


uint32_t livery_tally_host_frame()
{
    return livery_tally().host_frame;
}
void livery_tally_emit(const LiveryFrameTally& tally);

void livery_tally_begin_frame(uint32_t host_frame)
{
    if (!livery_draw_diag_enabled()) return;
    LiveryFrameTally& tally = livery_tally();
    if (tally.active && tally.host_frame == host_frame) return;
    if (tally.active) livery_tally_emit(tally);
    tally = LiveryFrameTally{};
    tally.active = true;
    tally.host_frame = host_frame;
}

void livery_tally_note_marker()
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active) return;
    ++tally.markers;
}

void livery_tally_note_mmdma()
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active) return;
    ++tally.mmdma;
}

void livery_tally_note_texreject()
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active) return;
    ++tally.texrejects;
}
void livery_tally_note_db()
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active) return;
    ++tally.dbdraws;
}

void livery_tally_note_draw(int cel, uint32_t lod0, const CltVertex* tri)
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active || !tri) return;
    ++tally.draws;
    float min_x = tri[0].x, max_x = tri[0].x;
    float min_y = tri[0].y, max_y = tri[0].y;
    for (size_t i = 1; i < 3; ++i) {
        min_x = std::min(min_x, tri[i].x);
        max_x = std::max(max_x, tri[i].x);
        min_y = std::min(min_y, tri[i].y);
        max_y = std::max(max_y, tri[i].y);
    }
    // Fixed 640x480 onscreen box: interpreter vertices are target pixels
    // before backend scaling, so this only separates fully-offscreen
    // geometry from candidate-visible draws.
    if (max_x >= 0.f && min_x <= 640.f && max_y >= 0.f && min_y <= 480.f)
        ++tally.onscreen;
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(cel)) << 32u) | lod0;
    for (uint32_t i = 0; i < tally.npairs; ++i)
        if (tally.pairs[i] == key) return;
    if (tally.npairs < 16u) tally.pairs[tally.npairs++] = key;
}
// Generic census of every successfully-textured draw (any resident). Called
// at the cont/new sites where the texture lookup already succeeded, so a
// livery panel using a non-192x112 resident still shows per-frame presence.
void livery_tally_note_textured(int cel, int w, int h, const CltVertex* tri,
                                size_t count)
{
    LiveryFrameTally& tally = livery_tally();
    if (!tally.active || !tri || count < 3u) return;
    ++tally.texdraws;
    float min_x = tri[0].x, max_x = tri[0].x;
    float min_y = tri[0].y, max_y = tri[0].y;
    for (size_t i = 1; i < count; ++i) {
        min_x = std::min(min_x, tri[i].x);
        max_x = std::max(max_x, tri[i].x);
        min_y = std::min(min_y, tri[i].y);
        max_y = std::max(max_y, tri[i].y);
    }
    if (max_x >= 0.f && min_x <= 640.f && max_y >= 0.f && min_y <= 480.f)
        ++tally.texscreen;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(w))
                          << 48u) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(h))
                          << 32u) |
                         static_cast<uint32_t>(cel);
    for (uint32_t i = 0; i < tally.ntexdims; ++i)
        if (tally.texdims[i] == key) return;
    if (tally.ntexdims < 16u) tally.texdims[tally.ntexdims++] = key;
}
void livery_tally_emit(const LiveryFrameTally& tally)
{
    std::fprintf(stderr,
                 "[livery-frame] host=%u markers=%u mmdma192=%u "
                 "draws=%u onscreen=%u texreject=%u pairs=%u "
                 "texdraws=%u texscreen=%u texdims=%u dbdraws=%u",
                 tally.host_frame,
                 tally.markers, tally.mmdma, tally.draws, tally.onscreen,
                 tally.texrejects, tally.npairs, tally.texdraws,
                 tally.texscreen, tally.ntexdims, tally.dbdraws);
    for (uint32_t i = 0; i < tally.npairs; ++i)
        std::fprintf(stderr, " cel=%d/lod=%#x",
                     static_cast<int>(tally.pairs[i] >> 32u),
                     static_cast<uint32_t>(tally.pairs[i]));
    for (uint32_t i = 0; i < tally.ntexdims; ++i)
        std::fprintf(stderr, " dim=%dx%d/cel=%d",
                     static_cast<int>(tally.texdims[i] >> 48u),
                     static_cast<int>((tally.texdims[i] >> 32u) & 0xffffu),
                     static_cast<int>(tally.texdims[i]));
    std::fputc('\n', stderr);
}

uint32_t clt_ring_audit_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_TE_RING_AUDIT_MAX");
        if (!value || *value == '\0') return 128u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 512ul));
    }();
    return max_entries;
}

uint32_t clt_ring_audit_from()
{
    static const uint32_t first_frame = [] {
        const char* value = std::getenv("M2_TE_RING_AUDIT_FROM");
        if (!value || *value == '\0') return 0u;
        return static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
    }();
    return first_frame;
}

// State 2 room-provenance focus: when M2_CLT_DRAW_LIST selects one
// executed list, per-draw provenance logs (publish, mat-dims, accept) bypass
// their frame floors and use focused caps so a single steady-state list tells
// its complete upload->draw story. Off by default (no filter = old behavior).
uint64_t clt_diag_list_filter()
{
    static const uint64_t filter = [] {
        const char* e = std::getenv("M2_CLT_DRAW_LIST");
        return e && *e ? strtoull(e, nullptr, 0) : UINT64_MAX;
    }();
    return filter;
}

bool clt_diag_list_selected(uint64_t exec_seq)
{
    const uint64_t filter = clt_diag_list_filter();
    return filter == UINT64_MAX || filter == exec_seq;
}

uint32_t clt_diag_focused_cap(uint32_t normal, uint32_t focused)
{
    return clt_diag_list_filter() == UINT64_MAX ? normal : focused;
}

// TXTADDRCNTL's three filter fields share one selector domain. The vendor
// header enumerates only point = 0 and bilinear = 2 for minification and
// magnification, and point/linear/bilinear/trilinear = 0..3 for inter-level
// filtering
// (the M2 Toolkit CLT header, lines 71-84),
// but that list is the subset the toolkit's own defaults use, not a domain
// restriction: the toolkit programs all three fields from one shared value
// table. `CLT_SetTxAttribute()` points TXA_MinFilter / TXA_MagFilter /
// TXA_InterFilter at TXTADDRCNTL.MINFILTER / .MAGFILTER / .INTERFILTER and
// applies the caller's value unchanged
// (M2 SDK src/libs/clt/clttxattr.c:59-75); the Mercury page builder does the
// same through `TAB_AddrCntl()`
// (M2 SDK src/tools/mercury/txtlib/src/page.c:400-425); and one parser,
// `Texture_Filter()`, reads the value for all three attributes
// (M2 SDK src/tools/mercury/txtlib/src/SDFTexBlend.c:1029-1035). The toolkit's
// own names are those same four values for every attribute
// (M2 SDK src/includes/graphics/clt/clttxdblend.h:74-77: TX_Nearest = 0,
// TX_Linear = 1, TX_Bilinear = 2, TX_QuasiTrilinear = 3).
//
// A measured State 3 race draw authors TXTADDRCNTL = 0x2493, i.e. linear in all
// three fields, so selector 1 really is written to minification and
// magnification. Treating it as reserved there silently refused every such
// draw (5,735 of 58,221 draws in a 120-tick race window, 100% of the run's
// `unsupported`). `CLT_CreateTxLoadCommandList*()` only rewrites LODMAX on top
// of the guest's choice
// (M2 SDK src/libs/clt/clttxload.c:186-189,402-405).
//
// 4..7 have no source-backed meaning in any field, so decoding one to a
// plausible filter would fabricate sampling the guest never selected. Only that
// case fails closed and refuses the draw.
CltFilterSelector decode_filter(uint32_t raw)
{
    CltFilterSelector selector;
    switch (raw & 7u) {
    case 0u:
        selector.filter = CltTextureFilter::point;
        return selector;
    case 1u:
        selector.filter = CltTextureFilter::linear;
        return selector;
    case 2u:
        selector.filter = CltTextureFilter::bilinear;
        return selector;
    case 3u:
        selector.filter = CltTextureFilter::trilinear;
        return selector;
    default:
        break;  // 4..7 are reserved in every filter field.
    }
    selector.reserved = true;
    return selector;
}

// MAME addr_calc (3dom2_te.cpp) applies TXTUVMASK to the integer texel
// coordinate: u0 = (u>>4) & u_mask, then clamps to the masked maximum. Keep
// the guest's projected UV values unwrapped at the backend boundary: the
// hardware applies the address mode after interpolation, so wrapping vertices
// before rasterization creates an artificial seam whenever one edge crosses a
// tile boundary.
//
// The toolkit's own description of the register is the same operation -- "Bits
// set to '0' in this register will be masked off during address calculation"
// -- and the vendor page builder makes the period meaningful rather than
// incidental: `PageCLT_SetUV()` in the checked-in Mercury texlib writes mask =
// largest-LOD dimension - 1 for a tiled axis (`M2PG_FLAGS_XWrapMode` /
// `M2PG_FLAGS_YWrapMode`) and the 0x3ff full scale for a clamped one. A period
// at or below the resident extent therefore wraps modulo that period; a period
// beyond the extent is a no-op on the coordinate and the axis clamps to its
// extent, which is exactly the long-standing exact-resident-period case.
//
// Returns false when TXTADDRCNTL selects a reserved filter, in which case
// *state is left untouched: the address state still decodes, but the sampling
// state is not a deliverable one and the caller must refuse the draw rather
// than render a filter the guest did not select.
bool texture_sampling_state(uint32_t addr_cntl, uint32_t uv_mask, float tw,
                            float th, CltTextureSampling* state)
{
    if (!state) return false;
    const CltFilterSelector min_selector = decode_filter(addr_cntl >> 10u);
    const CltFilterSelector inter_selector = decode_filter(addr_cntl >> 7u);
    const CltFilterSelector mag_selector = decode_filter(addr_cntl >> 4u);
    // TXTUVMASK follows the PPC-numbered CLT fields: U occupies the high
    // half (bits 16..25) and V occupies the low half (bits 0..9).  Keep this
    // consistent with TXTUVMAX and the vendor CLA_TXTUVMASK macro.
    const uint32_t umask = (uv_mask >> 16u) & 0x3ffu;
    const uint32_t vmask = uv_mask & 0x3ffu;

    CltTextureSampling decoded;
    decoded.min_filter = min_selector.filter;
    decoded.inter_filter = inter_selector.filter;
    decoded.mag_filter = mag_selector.filter;
    decoded.lod_max = static_cast<uint8_t>(addr_cntl & 0xfu);
    decoded.u_period = umask + 1u;
    decoded.v_period = vmask + 1u;
    decoded.wrap_u = tw > 0.f && static_cast<float>(decoded.u_period) <= tw;
    decoded.wrap_v = th > 0.f && static_cast<float>(decoded.v_period) <= th;
    decoded.raw_uv_mask = uv_mask;
    if (min_selector.reserved || inter_selector.reserved ||
        mag_selector.reserved)
        return false;
    *state = decoded;
    return true;
}

uint32_t clt_guest_checksum(const uint8_t* bytes, size_t byte_count)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < byte_count; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

bool clt_dest_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_DEST_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t clt_dest_trace_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CLT_DEST_MAX");
        if (!value || *value == '\0') return 128u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 512ul));
    }();
    return max_entries;
}

uint64_t clt_dest_trace_list_from()
{
    static const uint64_t first_list = [] {
        const char* value = std::getenv("M2_CLT_DEST_LIST_FROM");
        if (!value || *value == '\0') return uint64_t(0);
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return first_list;
}

uint64_t clt_dest_trace_list_to()
{
    static const uint64_t last_list = [] {
        const char* value = std::getenv("M2_CLT_DEST_LIST_TO");
        if (!value || *value == '\0') return UINT64_MAX;
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return last_list;
}

bool clt_dest_trace_giant_only()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_DEST_GIANT_ONLY");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool clt_z_census_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_CLT_Z_CENSUS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t clt_z_census_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_CLT_Z_CENSUS_MAX");
        if (!value || *value == '\0') return 64u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 128ul));
    }();
    return max_entries;
}

uint64_t clt_z_census_list_from()
{
    static const uint64_t first_list = [] {
        const char* value = std::getenv("M2_CLT_Z_CENSUS_LIST_FROM");
        if (!value || *value == '\0') return uint64_t(0);
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return first_list;
}

bool te_vertex_state_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint32_t te_vertex_state_trace_max()
{
    static const uint32_t max_entries = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_MAX");
        if (!value || *value == '\0') return 256u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return 1u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 4096ul));
    }();
    return max_entries;
}

uint32_t te_vertex_state_trace_header()
{
    static const uint32_t selected_header = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_HEADER");
        if (!value || *value == '\0') return 0u;
        return static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
    }();
    return selected_header;
}

uint64_t te_vertex_state_list_from()
{
    static const uint64_t first_list = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_LIST_FROM");
        if (!value || *value == '\0') return uint64_t(0);
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 0));
    }();
    return first_list;
}

bool te_vertex_state_flat_debug_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_FLAT");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool te_vertex_state_flat_depth_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_FLAT_DEPTH");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool te_vertex_state_flat_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_TE_VERTEX_STATE_FLAT_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

float color_component(uint32_t value, unsigned shift)
{
    return static_cast<float>((value >> shift) & 0xffu) / 255.f;
}

bool read_float(const uint32_t* words, size_t word_count, size_t* cursor,
                float* out)
{
    if (!words || !cursor || !out || *cursor >= word_count) return false;
    const float value = std::bit_cast<float>(words[*cursor]);
    ++*cursor;
    if (!std::isfinite(value)) return false;
    *out = value;
    return true;
}

std::pair<float, float> target_size(int32_t display_width,
                                    int32_t display_height,
                                    ICelBlitterBackend* backend)
{
    float width = static_cast<float>(display_width);
    float height = static_cast<float>(display_height);
    if (width <= 0.f || height <= 0.f) {
        int host_width = 0, host_height = 0;
        if (backend) backend->get_size(&host_width, &host_height);
        if (width <= 0.f) width = static_cast<float>(host_width);
        if (height <= 0.f) height = static_cast<float>(host_height);
    }
    if (width <= 0.f) width = 640.f;
    if (height <= 0.f) height = 480.f;
    return {width, height};
}

// Intentionally leaked process-lifetime synchronization. The recovered guest
// may remain live while the launcher is unwinding; a callback racing process
// teardown must never lock a destroyed static mutex.
std::mutex& active_bridge_mutex()
{
    static std::mutex* mutex = new std::mutex;
    return *mutex;
}

GuestCltBridge*& active_bridge()
{
    static GuestCltBridge* bridge = nullptr;
    return bridge;
}

}  // namespace

uint64_t rgba_content_hash(const std::vector<uint32_t>& pixels);
uint64_t pip_palette_hash(const std::vector<uint32_t>& pip);
uint64_t mmdma_sidecar_hash(const std::vector<uint8_t>& indices,
                            const std::vector<uint8_t>& alpha,
                            const std::vector<uint8_t>& source_ssb);

bool seed_video_overlay_targets(ICelBlitterBackend* backend,
                                uint32_t completed_te_scanout,
                                uint32_t overlay_view_base,
                                uint32_t overlay_draw_base,
                                uint32_t* scratch_rgba, int width, int height)
{
    if (!backend || !overlay_view_base || !overlay_draw_base ||
        !scratch_rgba || width <= 0 || height <= 0)
        return false;

    // SuperSetViewBitmap changes only the View attachment. It does not copy
    // the MPEG bitmap into the old TE framebuffer. At overlay discovery time
    // grab_presented_rgba() can therefore name a movie frame rather than the
    // game surface that was visible before the switch.
    bool captured = false;
    if (completed_te_scanout != 0u) {
        // A named TE predecessor that cannot be read is an unavailable source,
        // not permission to substitute the current (possibly MPEG) View.
        captured = backend->grab_clt_target_rgba(
            completed_te_scanout, scratch_rgba, width, height);
    } else {
        captured = backend->grab_presented_rgba(scratch_rgba, width, height);
    }
    if (!captured) return false;

    backend->clt_target_upload_rgba(overlay_view_base, scratch_rgba, width,
                                    height);
    if (overlay_draw_base != overlay_view_base)
        backend->clt_target_upload_rgba(overlay_draw_base, scratch_rgba,
                                        width, height);
    return true;
}

GuestCltInterpreter::GuestCltInterpreter(ICelBlitterBackend* backend)
    : m_backend(backend), m_registers(kRegisterWords, 0u), m_pip(kPipRamEntries, 0u)
{
    m_rgb555_dither = clt_rgb555_dither_enabled();
    m_pip_hash = pip_palette_hash(m_pip);
}

GuestCltInterpreter::~GuestCltInterpreter()
{
    dump_te_state_comparison();
    dump_z_census();
}

void GuestCltInterpreter::dump_te_state_comparison()
{
    if (!te_vertex_state_trace_enabled() || m_te_reference_summary_dumped)
        return;
    m_te_reference_summary_dumped = true;
    std::fprintf(
        stderr,
        "[te-state-summary] vertices=%llu triangles=%llu "
        "reference-divergences=%u first-divergence=%s\n",
        (unsigned long long)m_te_next_vertex_id,
        (unsigned long long)m_te_reference_triangles,
        m_te_reference_divergences,
        m_te_reference_divergence_reported ? "reported" : "none");
}

void GuestCltInterpreter::dump_z_census()
{
    dump_te_state_comparison();
    if (!clt_z_census_enabled()) return;
    if (m_dest_state_census_dumped) return;
    m_dest_state_census_dumped = true;
    for (size_t i = 0; i < m_dest_state_census_emitted; ++i) {
        const DestStateCensusEntry& entry = m_dest_state_census[i];
        const uint32_t db_z = entry.db_z;
        std::fprintf(
            stderr,
            "[clt-z-census-summary] state=%zu draws=%llu first_list=%llu "
            "hdr_index=%u hdr=%#x tri=%llu dbsuper=%#x dbuser=%#x "
            "discard=%#x dbz=%#x destmask=%#x zbufen=%d zout=%d "
            "blend=%d src=%d shaded=%d textured=%d "
            "bbox=(%.1f,%.1f)-(%.1f,%.1f) ow=[%.5g,%.5g] "
            "color=(lt%d eq%d gt%d) z=(lt%d eq%d gt%d) "
            "zbase=%#x zclip=(%ux%u) "
            "blendword=%#x am=%#x bm=%#x alu=%#x dbin=%#x "
            "sac=%#x dac=%#x daconst=%#x mode=%u eff=%u aunsup=%u\n",
            i + 1, (unsigned long long)entry.triangles,
            (unsigned long long)entry.first_list, entry.first_header_index,
            entry.first_header, (unsigned long long)entry.first_triangle_id,
            entry.db_super, entry.db_user, entry.discard, entry.db_z,
            entry.db_user & 0xfu,
            (entry.db_user & kDbZBufferEnable) != 0,
            (entry.db_user & kDbZOutputEnable) != 0,
            (entry.db_user & kDbBlendEnable) != 0,
            (entry.db_user & kDbSourceEnable) != 0,
            entry.first_shaded ? 1 : 0, entry.first_textured ? 1 : 0,
            entry.first_min_x, entry.first_min_y, entry.first_max_x,
            entry.first_max_y, entry.first_min_one_over_w,
            entry.first_max_one_over_w,
            (db_z & (1u << 0u)) != 0, (db_z & (1u << 2u)) != 0,
            (db_z & (1u << 4u)) != 0, (db_z & (1u << 1u)) != 0,
            (db_z & (1u << 3u)) != 0, (db_z & (1u << 5u)) != 0,
            entry.first_z_base,
            (entry.first_z_clip >> 16u) & kDbZClipCoordinateMask,
            entry.first_z_clip & kDbZClipCoordinateMask, entry.db_ssb_dsb,
            entry.db_a_mult, entry.db_b_mult, entry.db_alu, entry.db_const_in,
            entry.db_src_alpha_cntl, entry.db_dest_alpha_cntl,
            entry.db_dest_alpha_const, entry.blend_mode,
            entry.effective_blend_mode, entry.alpha_control_unsupported);
    }
}

uint32_t GuestCltInterpreter::register_value(uint16_t address) const
{
    return m2host::register_value(m_registers, address);
}

void GuestCltInterpreter::release_resources()
{
    release_armed_decodes();
    release_texture();
    free_texture_entry(m_db_source);
}

bool GuestCltInterpreter::in_texture_span(uint32_t lod_base0) const
{
    if (lod_base0 >= m_texture.tram_base &&
        static_cast<uint64_t>(lod_base0) <
            static_cast<uint64_t>(m_texture.tram_base) + m_texture.tram_size)
        return true;
    for (auto it = m_tram_cache.rbegin(); it != m_tram_cache.rend(); ++it) {
        if (lod_base0 >= it->tram_base &&
            static_cast<uint64_t>(lod_base0) <
                static_cast<uint64_t>(it->tram_base) + it->tram_size)
            return true;
    }
    return false;
}

GuestCltInterpreter::Texture* GuestCltInterpreter::texture_for_draw(
    uint32_t lod_base0)
{
    Texture* selected = nullptr;
    if (lod_base0 >= m_texture.tram_base &&
        static_cast<uint64_t>(lod_base0) <
            static_cast<uint64_t>(m_texture.tram_base) + m_texture.tram_size)
        selected = &m_texture;
    else {
        for (auto it = m_tram_cache.rbegin(); it != m_tram_cache.rend(); ++it) {
            if (lod_base0 >= it->tram_base &&
                static_cast<uint64_t>(lod_base0) <
                    static_cast<uint64_t>(it->tram_base) + it->tram_size) {
                selected = &(*it);
                break;
            }
        }
    }
    if (selected && !refresh_mmdma_palette(*selected)) return nullptr;
    return selected;
}

const GuestCltInterpreter::Texture* GuestCltInterpreter::texture_for_draw(
    uint32_t lod_base0) const
{
    if (lod_base0 >= m_texture.tram_base &&
        static_cast<uint64_t>(lod_base0) <
            static_cast<uint64_t>(m_texture.tram_base) + m_texture.tram_size)
        return &m_texture;
    for (auto it = m_tram_cache.rbegin(); it != m_tram_cache.rend(); ++it) {
        if (lod_base0 >= it->tram_base &&
            static_cast<uint64_t>(lod_base0) <
                static_cast<uint64_t>(it->tram_base) + it->tram_size)
            return &(*it);
    }
    return nullptr;
}

bool GuestCltInterpreter::refresh_mmdma_palette(Texture& entry)
{
    if (!entry.mmdma_indexed) return true;
    // A full-load resident drawn under a different TXTEXPTYPE is re-decoded
    // from its raw rows by serve_for_draw(); its stored indices were split
    // with the load-time format and must not be re-expanded (or fail the
    // draw) here.
    if (entry.staged_format_only && entry.staged_valid &&
        entry.staged_exp_type != register_value(kTxtExpType))
        return true;

    const uint32_t pip_control_value = register_value(kTxtPipCntl);
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return false;
    const uint32_t pip_offset = pip_control.index_offset;
    const uint32_t constant0_raw = register_value(kTxtConst0);
    const uint32_t constant1_raw = register_value(kTxtConst1);
    const uint64_t pip_hash = m_pip_hash;
    static const bool draw_diag = [] {
        const char* e = std::getenv("M2_DRAW_DIAG");
        return e && *e && *e != '0';
    }();
    const bool nose_central = entry.tram_size == 0x2630u &&
                              entry.width == 988 && entry.height == 79;
    if (draw_diag && nose_central) {
        static uint32_t state_logged = 0;
        if (state_logged < 16u) {
            std::fprintf(stderr,
                         "[mmdma-pip-state] base=%#x size=%#x w=%d h=%d "
                         "indexed=%d old_ctl=%#x current_ctl=%#x "
                         "old_pip=%#x current_pip=%#x "
                         "old_generation=%llu current_generation=%llu "
                         "indices=%zu cel=%d\n",
                         entry.tram_base, entry.tram_size, entry.width,
                         entry.height, entry.mmdma_indexed ? 1 : 0,
                         entry.mmdma_pip_control, pip_control_value,
                         entry.mmdma_pip_offset, pip_offset,
                         (unsigned long long)entry.mmdma_pip_generation,
                         (unsigned long long)m_pip_generation,
                         entry.mmdma_indices.size(), entry.cel);
            ++state_logged;
        }
    }
    if (entry.mmdma_pip_control == pip_control_value &&
        entry.mmdma_pip_offset == pip_offset &&
        entry.mmdma_pip_generation == m_pip_generation &&
        entry.mmdma_constant0 == constant0_raw &&
        entry.mmdma_constant1 == constant1_raw &&
        entry.mmdma_pip_hash == pip_hash)
        return true;
    if (entry.width <= 0 || entry.height <= 0 ||
        entry.mmdma_indices.size() !=
            static_cast<size_t>(entry.width) * entry.height)
        return false;

    const TextureFormat format = decode_format(entry.mmdma_exp_type, 0);
    if ((!is_indexed_tram_format(format) &&
         !is_no_color_literal_format(format)) ||
        entry.mmdma_no_color != !format.has_color)
        return false;
    // Every indexed resident keeps the source SSB stream, including the
    // all-zero stream for formats without an encoded SSB bit. That makes a
    // later TEXTURE/PIP/CONSTANT selector change deterministic and keeps a
    // malformed cache entry fail-closed rather than reading past the sidecar.
    if (entry.mmdma_source_ssb.size() != entry.mmdma_indices.size())
        return false;
    if (!entry.mmdma_alpha.empty() &&
        entry.mmdma_alpha.size() != entry.mmdma_indices.size())
        return false;
    if (entry.mmdma_source_hash == 0)
        entry.mmdma_source_hash = mmdma_sidecar_hash(
            entry.mmdma_indices, entry.mmdma_alpha,
            entry.mmdma_source_ssb);
    if (entry.mmdma_coarse_hash == 0 && !entry.mmdma_coarse_levels.empty())
        entry.mmdma_coarse_hash =
            mmdma_coarse_sidecar_hash(entry.mmdma_coarse_levels);

    std::vector<uint32_t> pixels;
    std::vector<uint8_t> ssb_bits;
    std::vector<std::vector<uint32_t>> coarse_pixels;
    std::vector<CelLodLevel> coarse_levels;
    uint64_t coarse_chain_hash = 0;
    MmdmaMaterialKey material_key;
    material_key.source_hash = entry.mmdma_source_hash;
    material_key.coarse_hash = entry.mmdma_coarse_hash;
    material_key.width = static_cast<uint32_t>(entry.width);
    material_key.height = static_cast<uint32_t>(entry.height);
    material_key.exp_type = entry.mmdma_exp_type;
    material_key.pip_control = pip_control_value;
    material_key.constant0 = constant0_raw;
    material_key.constant1 = constant1_raw;
    material_key.pip_palette_hash = pip_hash;
    material_key.no_color = entry.mmdma_no_color;
    const MmdmaMaterialValue* cached_material =
        find_cached_mmdma_material(material_key);
    // Validity is structural, not hashed: the cache entry must describe the
    // same level count and extents this resident retained. Size checks also
    // keep a malformed record fail-closed even if its key collided with a
    // valid entry.
    const auto cached_chain_matches =
        [&entry](const MmdmaMaterialValue& value) {
            if (value.coarse_pixels.size() != entry.mmdma_coarse_levels.size())
                return false;
            if (entry.mmdma_coarse_levels.empty())
                return value.coarse_chain_hash == 0;
            if (value.coarse_chain_hash == 0) return false;
            for (size_t i = 0; i < entry.mmdma_coarse_levels.size(); ++i) {
                const MmdmaLevelSidecars& level = entry.mmdma_coarse_levels[i];
                if (level.width <= 0 || level.height <= 0) return false;
                if (level.indices.size() !=
                        static_cast<size_t>(level.width) * level.height ||
                    level.source_ssb.size() != level.indices.size() ||
                    (!level.alpha.empty() &&
                     level.alpha.size() != level.indices.size()))
                    return false;
                if (value.coarse_pixels[i].size() != level.indices.size())
                    return false;
            }
            return true;
        };
    if (cached_material && !cached_chain_matches(*cached_material))
        cached_material = nullptr;
    const PipColor constant0 = decode_pip(constant0_raw);
    const PipColor constant1 = decode_pip(constant1_raw);
    // One expansion rule for the base level and every authored coarser level:
    // the stored index/alpha/SSB sidecars are re-selected under the armed
    // TXTPIPCNTL/constants, so a palette change rebinds the whole chain.
    const auto expand_level_sidecars =
        [&](const std::vector<uint8_t>& indices,
            const std::vector<uint8_t>& alpha,
            const std::vector<uint8_t>& source_ssb,
            std::vector<uint32_t>* out_pixels,
            std::vector<uint8_t>* out_ssb) {
            if (source_ssb.size() != indices.size()) return false;
            if (!alpha.empty() && alpha.size() != indices.size())
                return false;
            out_pixels->clear();
            out_ssb->clear();
            out_pixels->reserve(indices.size());
            out_ssb->reserve(indices.size());
            for (size_t i = 0; i < indices.size(); ++i) {
                const uint32_t texture_ssb = source_ssb[i];
                const PipColor& constant =
                    pip_constant_for_ssb(texture_ssb, constant0, constant1);
                const uint32_t texture_alpha =
                    alpha.empty()
                        ? (entry.mmdma_no_color
                               ? 0u
                               : (constant.rgba >> 24u) & 0xffu)
                        : static_cast<uint32_t>(alpha[i]);
                uint32_t rgba = 0;
                uint8_t ssb = 0;
                const bool materialized =
                    entry.mmdma_no_color
                        ? materialize_no_color_texel(
                              pip_control_value, constant0, constant1,
                              texture_alpha, texture_ssb, &rgba, &ssb)
                        : materialize_indexed_texel(
                              m_pip, pip_control_value, constant0,
                              constant1, indices[i], texture_alpha,
                              texture_ssb, &rgba, &ssb);
                if (!materialized) return false;
                out_pixels->push_back(rgba);
                out_ssb->push_back(ssb);
            }
            return true;
        };
    if (cached_material) {
        // Keep the cached CPU product borrowed through this refresh. Copying
        // a resident's full RGBA vector on every draw defeats the material
        // cache (the nose/footer residents are tens of thousands of texels).
        // The cache is not mutated again until after the upload, so the
        // reference remains stable for the entire publication below.
        coarse_chain_hash = cached_material->coarse_chain_hash;
        coarse_levels.reserve(cached_material->coarse_pixels.size());
        for (size_t i = 0; i < cached_material->coarse_pixels.size(); ++i) {
            const MmdmaLevelSidecars& level = entry.mmdma_coarse_levels[i];
            coarse_levels.push_back({level.width, level.height,
                                     cached_material->coarse_pixels[i].data()});
        }
    } else {
        if (!expand_level_sidecars(entry.mmdma_indices, entry.mmdma_alpha,
                                   entry.mmdma_source_ssb, &pixels,
                                   &ssb_bits))
            return false;
        // Re-expand the guest's authored coarser levels under the same palette
        // and selectors: a base-only upload would replace them with host
        // reductions of LOD0 and lose the guest's own payloads. The expanded
        // levels are cached with the base, so an unchanged palette state
        // reuses them instead of repeating this loop.
        if (!entry.mmdma_coarse_levels.empty()) {
            coarse_pixels.reserve(entry.mmdma_coarse_levels.size());
            coarse_levels.reserve(entry.mmdma_coarse_levels.size());
            std::vector<uint8_t> level_ssb;
            for (const MmdmaLevelSidecars& level : entry.mmdma_coarse_levels) {
                if (level.width <= 0 || level.height <= 0 ||
                    level.indices.size() !=
                        static_cast<size_t>(level.width) * level.height)
                    return false;
                std::vector<uint32_t> level_pixels;
                if (!expand_level_sidecars(level.indices, level.alpha,
                                           level.source_ssb, &level_pixels,
                                           &level_ssb))
                    return false;
                coarse_pixels.push_back(std::move(level_pixels));
                coarse_levels.push_back({level.width, level.height,
                                         coarse_pixels.back().data()});
            }
            // The same identity the backend would compute by rescanning the
            // chain; caching it here lets a refresh skip that scan too.
            coarse_chain_hash =
                cel_lod_chain_hash(coarse_levels.data(), coarse_levels.size());
        }
        MmdmaMaterialValue material_value;
        material_value.pixels = pixels;
        material_value.ssb_bits = ssb_bits;
        material_value.pixel_hash = rgba_content_hash(pixels);
        material_value.coarse_pixels = coarse_pixels;
        material_value.coarse_chain_hash = coarse_chain_hash;
        store_cached_mmdma_material(material_key, std::move(material_value));
    }
    const std::vector<uint32_t>& material_pixels =
        cached_material ? cached_material->pixels : pixels;
    const std::vector<uint8_t>& material_ssb =
        cached_material ? cached_material->ssb_bits : ssb_bits;
    const uint64_t material_hash =
        cached_material && cached_material->pixel_hash != 0
            ? cached_material->pixel_hash
            : rgba_content_hash(material_pixels);

    CelId new_cel = kInvalidCel;
    {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        if (m_backend) {
            // The retired-texture cache is keyed by these two identities, so
            // alternating palette states reuse the prior GL texture while the
            // material cache supplies the matching CPU pixels. Keeping a CelId
            // in the material entry itself would be unsafe: the same entry can
            // serve several residents, and freeing one would invalidate it.
            if (!coarse_levels.empty())
                new_cel = m_backend->cel_upload_rgba_lod_chain(
                    entry.width, entry.height, material_pixels.data(),
                    coarse_levels.data(), coarse_levels.size(), true, false,
                    material_hash, coarse_chain_hash);
            else
                new_cel = m_backend->cel_upload_rgba_ex_hashed(
                    entry.width, entry.height, material_pixels.data(), true,
                    false, material_hash);
        }
    }
    if (m_backend && new_cel == kInvalidCel) return false;

    if (config_texture_trace_enabled() &&
        entry.source_address == kConfigFooterSource) {
        entry.decoded_checksum = config_texture_hash_rgba(material_pixels);
        entry.decoded_checksum_valid = true;
    }

    if (draw_diag) {
        static uint32_t refresh_logged = 0;
        static uint32_t nose_refresh_logged = 0;
        const bool log_refresh =
            nose_central ? nose_refresh_logged < 16u : refresh_logged < 64u;
        if (log_refresh) {
            uint32_t min_rgb = 0x00ffffffu;
            uint32_t max_rgb = 0u;
            uint32_t nonblack = 0u;
            for (const uint32_t pixel : material_pixels) {
                const uint32_t rgb = pixel & 0x00ffffffu;
                min_rgb = std::min(min_rgb, rgb);
                max_rgb = std::max(max_rgb, rgb);
                if (rgb != 0u) ++nonblack;
            }
            std::fprintf(stderr,
                         "[mmdma-pip-refresh] base=%#x size=%#x w=%d h=%d "
                         "old_ctl=%#x new_ctl=%#x old_pip=%#x new_pip=%#x "
                         "generation=%llu\n",
                         entry.tram_base, entry.tram_size, entry.width,
                         entry.height, entry.mmdma_pip_control,
                         pip_control_value, entry.mmdma_pip_offset, pip_offset,
                         (unsigned long long)m_pip_generation);
            std::fprintf(stderr,
                         "[mmdma-pip-refresh-color] base=%#x size=%#x "
                         "rgb=[%06x,%06x] nonblack=%u/%zu\n",
                         entry.tram_base, entry.tram_size, min_rgb, max_rgb,
                         nonblack, material_pixels.size());
            if (nose_central)
                ++nose_refresh_logged;
            else
                ++refresh_logged;
        }
    }
    if (m_backend) {
        if (entry.ssb_mask != kInvalidCel)
            m_backend->cel_free(entry.ssb_mask);
        if (entry.cel != kInvalidCel) m_backend->cel_free(entry.cel);
    }
    entry.cel = new_cel;
    entry.ssb_mask = kInvalidCel;
    entry.ssb = material_ssb;
    entry.ssb_nonzero = std::any_of(
        entry.ssb.begin(), entry.ssb.end(),
        [](uint8_t value) { return value != 0u; });
    entry.mmdma_pip_control = pip_control_value;
    entry.mmdma_pip_offset = pip_offset;
    entry.mmdma_pip_generation = m_pip_generation;
    entry.mmdma_constant0 = constant0_raw;
    entry.mmdma_constant1 = constant1_raw;
    entry.mmdma_pip_hash = pip_hash;
    return true;
}

void GuestCltInterpreter::free_texture_entry(Texture& entry)
{
    if (!m_backend) {
        entry = {};
        return;
    }
    if (entry.ssb_mask != kInvalidCel)
        m_backend->cel_free(entry.ssb_mask);
    if (entry.cel != kInvalidCel)
        m_backend->cel_free(entry.cel);
    entry = {};
}

void GuestCltInterpreter::release_texture()
{
    if (!m_backend) {
        m_texture = {};
        m_tram_cache.clear();
        return;
    }
    if (m_texture.ssb_mask != kInvalidCel)
        m_backend->cel_free(m_texture.ssb_mask);
    if (m_texture.cel != kInvalidCel)
        m_backend->cel_free(m_texture.cel);
    m_texture = {};
    for (Texture& entry : m_tram_cache) free_texture_entry(entry);
    m_tram_cache.clear();
}

void GuestCltInterpreter::publish_texture(CelId cel, uint32_t tram_base,
                                          uint32_t tram_size, int width,
                                          int height,
                                          std::vector<uint8_t>&& ssb,
                                          uint8_t publisher)
{
    if (bridge_trace_enabled())
        std::fprintf(stderr,
                     "[publish] cel=%d base=%#x size=%#x w=%d h=%d ld_cntl=%#x exp=%#x pip_src=%#x pip_bytes=%u\n",
                     (int)cel, tram_base, tram_size, width, height,
                     register_value(kTxtLdCntl), register_value(kTxtExpType),
                     m_last_pip_source, m_last_pip_bytes);
    // A texture load writes TRAM in place. A staged MMDMA blob whose
    // destination bytes overlap this write has been overwritten and can no
    // longer be materialized as the bytes the guest DMA'd; drop it before a
    // later draw can materialize it against foreign registers and ghost the
    // freshly published texture (Gears footer 0x6bf3f0 shadowed by a stale
    // mesh blob at the same base).
    if (m_pending_mmdma_texture) {
        const uint64_t pending_begin = m_pending_mmdma_destination_base;
        const uint64_t pending_end =
            pending_begin + m_pending_mmdma_bytes.size();
        const uint64_t load_begin = tram_base;
        const uint64_t load_end = load_begin + tram_size;
        if (pending_begin < load_end && load_begin < pending_end) {
            m_pending_mmdma_texture = false;
            m_pending_mmdma_bytes.clear();
            m_pending_mmdma_hash = 0;
            m_pending_linear_rows = false;
            m_pending_linear_row_bits = 0u;
            m_pending_linear_row_count = 0u;
        }
    }
    // Livery census: count 192x112 materializations for the per-frame
    // summary regardless of which detail diagnostics are enabled.
    if (width == 192 && height == 112) livery_tally_note_mmdma();
    // Bounded residency audit: log successful TRAM publishes under
    // the existing diagnostic floor so the single-texture overwrite hypothesis
    // (draw base != resident base) starts from authenticated evidence.
    {
        static const bool pub_diag = [] {
            const char* e = std::getenv("M2_DRAW_DIAG");
            return e && *e && *e != '0';
        }();
        static const uint32_t pub_from = [] {
            const char* e = std::getenv("M2_DRAW_DIAG_FROM");
            return e ? static_cast<uint32_t>(strtoul(e, nullptr, 0)) : 0u;
        }();
        const uint64_t pub_frame =
            perf::counter_value(perf::Counter::GuestFramesStarted);
        static uint32_t pub_emitted = 0;
        static bool pub_expired = false;
        const uint32_t pub_cap = clt_diag_focused_cap(32u, 4096u);
        const bool pub_list_hit = clt_diag_list_selected(m_exec_seq);
        if (pub_emitted >= pub_cap && !pub_expired) {
            pub_expired = true;
            std::fprintf(stderr,
                         "[publish] cap reached at frame %llu — further "
                         "entries suppressed\n",
                         (unsigned long long)pub_frame);
        }
        if ((pub_diag &&
             (pub_frame >= pub_from ||
              clt_diag_list_filter() != UINT64_MAX) &&
             pub_list_hit) &&
            pub_emitted < pub_cap) {
            ++pub_emitted;
            // publisher is the argument, not m_texture: the caller assigns
            // the new record's staged fields only after publish_texture
            // returns, and retention is a property of the publishing
            // materializer (the MMDMA and full-load paths retain bytes).
            std::fprintf(stderr,
                         "[publish] cel=%d base=%#x size=%#x w=%d h=%d "
                         "exp=%#x lod0=%#x lod1=%#x lod2=%#x lod3=%#x "
                         "pub=%u "
                         "frame=%llu exec=%llu\n",
                         (int)cel, tram_base, tram_size, width, height,
                         register_value(kTxtExpType),
                         register_value(kTxtLodBase0),
                         register_value(0x6418u), register_value(0x641cu),
                         register_value(0x6420u),
                         publisher,
                         (unsigned long long)pub_frame,
                         (unsigned long long)m_exec_seq);
            if (pub_emitted == pub_cap && !pub_expired) {
                pub_expired = true;
                std::fprintf(stderr,
                             "[publish] cap reached at frame %llu — "
                             "further entries suppressed\n",
                             (unsigned long long)pub_frame);
            }
        }
    }
    // Retain the outgoing resident so stale-region draws (lod 0xff0
    // vs base-0 small overwrite) can still hit the suffix bytes that real
    // TRAM preserves. Bounded to 16 entries; oldest evicted with cel free.
    // No overlap eviction: a small prefix overwrite must not discard the
    // larger entry that still owns the suffix region.
    if (m_texture.cel != kInvalidCel) {
        if (m_tram_cache.size() >= 16u) {
            free_texture_entry(m_tram_cache.front());
            m_tram_cache.erase(m_tram_cache.begin());
        }
        m_tram_cache.push_back(std::move(m_texture));
        m_texture = {};
    } else if (m_texture.ssb_mask != kInvalidCel) {
        if (m_backend) m_backend->cel_free(m_texture.ssb_mask);
        m_texture = {};
    }
    m_texture.cel = cel;
    m_texture.tram_base = tram_base;
    m_texture.tram_size = tram_size;
    m_texture.width = width;
    m_texture.height = height;
    m_texture.publisher = publisher;
    m_texture.ssb = std::move(ssb);
    m_texture.ssb_nonzero = std::any_of(
        m_texture.ssb.begin(), m_texture.ssb.end(),
        [](uint8_t value) { return value != 0u; });
}

bool GuestCltInterpreter::ensure_ssb_mask_for(Texture& entry)
{
    if (entry.ssb_mask != kInvalidCel) return true;
    if (entry.cel == kInvalidCel) return false;
    entry.ssb_mask = upload_ssb_mask(entry.width, entry.height, entry.ssb);
    return entry.ssb_mask != kInvalidCel;
}

CelId GuestCltInterpreter::upload_ssb_mask(
    int width, int height, const std::vector<uint8_t>& ssb)
{
    if (!m_backend || width <= 0 || height <= 0 ||
        ssb.size() != static_cast<size_t>(width) * height)
        return kInvalidCel;

    std::vector<uint32_t> mask;
    mask.reserve(ssb.size());
    for (const uint8_t value : ssb)
        mask.push_back(value != 0 ? 0xffffffffu : 0u);
    CelId cel = kInvalidCel;
    {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        cel = m_backend->cel_upload_rgba_ex_hashed(
            width, height, mask.data(), false, false,
            rgba_content_hash(mask));
    }
    return cel;
}

bool GuestCltInterpreter::ensure_ssb_mask()
{
    return ensure_ssb_mask_for(m_texture);
}

void GuestCltInterpreter::set_viewport(int32_t left, int32_t top,
                                       int32_t width, int32_t height,
                                       bool valid, int32_t display_width,
                                       int32_t display_height)
{
    m_viewport_left = left;
    m_viewport_top = top;
    m_display_width = display_width;
    m_display_height = display_height;
    m_viewport_valid = valid && width > 0 && height > 0;
    if (m_backend) {
        int host_width = 0, host_height = 0;
        m_backend->get_size(&host_width, &host_height);
        CltScanoutRect rect;
        if (m_viewport_valid && display_width > 0 && display_height > 0) {
            const uint32_t clip = register_value(kDbFbClip);
            const auto scale_x = [&](int value) {
                return int(std::lround(double(value) * host_width / display_width));
            };
            const auto scale_y = [&](int value) {
                return int(std::lround(double(value) * host_height / display_height));
            };
            rect = {scale_x(left), scale_y(top),
                    scale_x(int((clip >> 16) & 0x7ffu)),
                    scale_y(int(clip & 0x7ffu)), scale_x(width), scale_y(height)};
        }
        m_backend->clt_target_scanout_rect(register_value(kDbDestBaseAddr), rect);
    }
    update_window_clip();
    update_destination_blend();
}

void GuestCltInterpreter::set_te_surface_state(
    uint32_t db_super_control, uint32_t db_fb_clip,
    uint32_t db_dest_control, uint32_t db_dest_base,
    uint32_t db_dest_x_stride, uint32_t db_z_base, uint32_t db_z_clip,
    uint32_t z_fill_word, uint32_t fb_clear_red_word,
    uint32_t fb_clear_green_word, uint32_t fb_clear_blue_word,
    uint32_t fb_clear_alpha_word, uint64_t generation)
{
    m_te_z_fill_word = z_fill_word;
    m_te_fb_clear_red_word = fb_clear_red_word;
    m_te_fb_clear_green_word = fb_clear_green_word;
    m_te_fb_clear_blue_word = fb_clear_blue_word;
    m_te_fb_clear_alpha_word = fb_clear_alpha_word;
    if (generation != m_te_surface_generation) {
        m_registers[register_index(kDbSuperGenControl)] = db_super_control;
        m_registers[register_index(kDbFbClip)] = db_fb_clip;
        m_registers[register_index(kDbDestControl)] = db_dest_control;
        m_registers[register_index(kDbDestBaseAddr)] = db_dest_base;
        m_registers[register_index(kDbDestXStride)] = db_dest_x_stride;
        m_registers[register_index(kDbZBaseAddr)] = db_z_base;
        m_registers[register_index(kDbZClip)] = db_z_clip;
        m_te_surface_generation = generation;
        // The bound surface also supplies the destination containment
        // rectangle (see update_window_clip()), so refresh the clip when it
        // changes.
        update_window_clip();
    }
    // Bind the destination for every submitted list, not only when the
    // surface registers changed. The presenter leaves the default drawable
    // bound at every host frame boundary (`present()` binds framebuffer 0 so
    // SDL can swap), so a later list that inherits the same generation must
    // still re-establish its target; skipping that sent the list's CLT draws
    // to the drawable and left the addressed guest target frozen at its
    // previous content while its sibling kept advancing.
    //
    // Bind the destination register the draws will actually use, not the
    // submission's snapshot: a guest `DBDESTBASEADDR` write that did not bump
    // the generation must win over a stale snapshot.
    if (m_backend && clt_select_dump_enabled()) sample_destination_first_select();
    if (m_backend)
        m_backend->clt_target_select(register_value(kDbDestBaseAddr));
}

void GuestCltInterpreter::sample_destination_first_select()
{
    // One line per (guest frame, destination), capped. All authored transition
    // frames share one guest-frame tag, so this selects the seed list for each
    // destination without suppressing the later showroom-to-race transition
    // merely because the same address appeared in an earlier transition.
    struct Seen {
        uint64_t frame;
        uint32_t base;
    };
    static constexpr uint32_t kMaxEntries = 4096u;
    static Seen seen[kMaxEntries] = {};
    static uint32_t seen_count = 0;
    if (m_host_frame < clt_select_dump_from()) return;
    const uint64_t frame =
        perf::counter_value(perf::Counter::GuestFramesStarted);
    const uint32_t base = register_value(kDbDestBaseAddr);
    if (base == 0u) return;
    for (uint32_t i = 0; i < seen_count; ++i)
        if (seen[i].frame == frame && seen[i].base == base) return;
    if (seen_count >= kMaxEntries) return;
    seen[seen_count++] = {frame, base};

    int w = 0, h = 0;
    m_backend->get_size(&w, &h);
    const bool valid_size = w > 0 && h > 0 && w <= 4096 && h <= 4096;
    const size_t count = valid_size
                             ? static_cast<size_t>(w) * static_cast<size_t>(h)
                             : 0u;
    std::vector<uint32_t> dest(count), source(count);
    const uint32_t source_base = register_value(kDbSrcBaseAddr);
    const bool got_dest = count != 0u &&
                          m_backend->grab_clt_target_rgba(
                              base, dest.data(), w, h);
    const bool got_source = count != 0u && source_base != 0u &&
                            m_backend->grab_clt_target_rgba(
                                source_base, source.data(), w, h);
    const auto mean = [](const std::vector<uint32_t>& px, bool got,
                         unsigned shift) {
        if (!got || px.empty()) return uint64_t{0};
        uint64_t sum = 0;
        for (const uint32_t p : px) sum += (p >> shift) & 0xffu;
        return sum / px.size();
    };
    uint64_t diff_sum = 0;
    if (got_dest && got_source) {
        for (size_t i = 0; i < count; ++i) {
            for (unsigned shift = 0; shift < 24; shift += 8) {
                diff_sum += static_cast<uint64_t>(std::abs(
                    static_cast<int>((dest[i] >> shift) & 0xffu) -
                    static_cast<int>((source[i] >> shift) & 0xffu)));
            }
        }
    }
    const unsigned mad_x100 = count != 0u && got_dest && got_source
                                  ? static_cast<unsigned>(
                                        (diff_sum / count) * 100u / 3u)
                                  : 0u;
    std::fprintf(stderr,
                 "[sel-dump] host=%u frame=%llu dest=%#x got_dest=%d "
                 "dest_mean=%llu,%llu,%llu src=%#x got_src=%d "
                 "src_mean=%llu,%llu,%llu mad=%u\n",
                 m_host_frame, static_cast<unsigned long long>(frame), base,
                 got_dest ? 1 : 0,
                 static_cast<unsigned long long>(mean(dest, got_dest, 0)),
                 static_cast<unsigned long long>(mean(dest, got_dest, 8)),
                 static_cast<unsigned long long>(mean(dest, got_dest, 16)),
                 source_base, got_source ? 1 : 0,
                 static_cast<unsigned long long>(mean(source, got_source, 0)),
                 static_cast<unsigned long long>(mean(source, got_source, 8)),
                 static_cast<unsigned long long>(mean(source, got_source, 16)),
                 mad_x100);
}

void GuestCltInterpreter::clear_z_buffer()
{
    if (m_backend) m_backend->clt_clear_z(m_te_z_fill_word);
}

void GuestCltInterpreter::clear_color_buffer()
{
    // The recovered driver's TE_CLEAR_FRAME_BUFFER arm runs a generated clear
    // list before the request's own list: an untextured quad from the surface
    // origin to the extent the bound bitmap published when it was bound
    // (driver.c:206-220 `ds_ClearFBCmdList`, :251-278 `UpdateClearFBCmdList`).
    // The TE clips that quad with the same DBFBCLIP and window clip as every
    // other primitive, so the backends run this platform fill inside the clip
    // the interpreter already maintains for guest color writes
    // (`update_window_clip()`), not over the whole target.
    if (m_backend) {
        m_backend->clt_clear_color(m_te_fb_clear_red_word,
                                   m_te_fb_clear_green_word,
                                   m_te_fb_clear_blue_word,
                                   m_te_fb_clear_alpha_word);
    }
}

bool GuestCltInterpreter::fb_readback_enabled() const
{
    static const bool enabled = [] {
        const char* e = std::getenv("M2_DBSRC_FB_READBACK");
        return e && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

bool GuestCltInterpreter::inprogress_readback_enabled() const
{
    static const bool enabled = [] {
        const char* e = std::getenv("M2_DBSRC_INPROGRESS");
        // A display-carve DB-source read is a same-frame copy on the real TE:
        // earlier guest geometry is already in the destination surface when
        // the CAR/576 window samples it. Keep the old guest-RAM path available
        // as an explicit A/B diagnostic, but make the recovered platform
        // behavior normal when no override is supplied.
        return !e || (e[0] != '\0' && e[0] != '0');
    }();
    return enabled;
}

bool GuestCltInterpreter::db_source_skip_enabled() const
{
    static const bool enabled = [] {
        const char* e = std::getenv("M2_DBSRC_SKIP");
        return e && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

// Sample the addressed in-progress guest target at DB-draw time. World lists
// submitted earlier in this pump are already rasterized into it; the
// covering DB quads of the current state generation are not (the caller
// snapshots once per execute, unless a nonzero mid-list DBDESTBASEADDR write
// advances the generation). Mapping mirrors retain_presented_frame: the
// 576x432 display region at the view origin,
// converted to big-endian RGB555 and windowed at window_rel. Reads through
// grab_clt_target_rgba: software and GL retain independent sibling targets.
bool GuestCltInterpreter::snapshot_db_source_from_target(
    uint32_t target_base, int64_t window_rel, size_t source_bytes,
    bool copy_ssb, int sheet_width, int sheet_height)
{
    if (!m_backend) return false;
    int fb_w = 0, fb_h = 0;
    m_backend->get_size(&fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return false;
    const int32_t disp_w = m_display_width > 0 ? m_display_width : 640;
    const int32_t disp_h = m_display_height > 0 ? m_display_height : 480;
    const float org_x =
        m_viewport_valid ? static_cast<float>(m_viewport_left) : 32.f;
    const float org_y =
        m_viewport_valid ? static_cast<float>(m_viewport_top) : 24.f;
    const float sx = static_cast<float>(fb_w) / static_cast<float>(disp_w);
    const float sy = static_cast<float>(fb_h) / static_cast<float>(disp_h);

    const int kSheetW = sheet_width;
    const int kSheetH = sheet_height;
    if (kSheetW <= 0 || kSheetH <= 0) return false;
    // The display carve occupies a fixed 576x432 window in the guest target.
    // Read just that window when the backend can address it; keep the full
    // target read as a compatibility fallback for non-GL/older backends.
    int window_x = std::clamp(static_cast<int>(std::floor(org_x * sx)),
                              0, fb_w);
    int window_y = std::clamp(static_cast<int>(std::floor(org_y * sy)),
                              0, fb_h);
    int requested_window_w = static_cast<int>(std::ceil(kSheetW * sx));
    int requested_window_h = static_cast<int>(std::ceil(kSheetH * sy));
    // Rotate's strip loads are full-width row spans of the display sheet.
    // Read the sheet once per source generation and copy each strip out of
    // it: the 36 loads in one frame all address the same sheet, so one full
    // read replaces 36 strip-window calls at the same total pixel cost and
    // every strip sees the full-sheet content the transition was authored
    // against. A request that is not a clean full-width row span keeps the
    // ordinary full-sheet read.
    const size_t bytes_per_row = static_cast<size_t>(kSheetW) * 2u;
    const bool full_width_rows =
        source_bytes != 0u && source_bytes % bytes_per_row == 0u &&
        static_cast<int64_t>(source_bytes / bytes_per_row) <= kSheetH;
    const bool row_span =
        full_width_rows && window_rel >= 0 &&
        static_cast<uint64_t>(window_rel) % bytes_per_row == 0u;
    int64_t source_row = 0;
    int strip_rows = 0;
    if (row_span) {
        source_row = window_rel / static_cast<int64_t>(bytes_per_row);
        strip_rows = static_cast<int>(source_bytes / bytes_per_row);
    }
    if (row_span && copy_ssb && strip_rows < kSheetH &&
        source_row + strip_rows <= kSheetH) {
        const size_t sheet_bytes =
            static_cast<size_t>(kSheetW) * kSheetH * 2u;
        if (m_db_sheet_generation != m_db_source_generation ||
            m_db_sheet_target != target_base ||
            m_db_sheet_bytes.size() != sheet_bytes) {
            if (!snapshot_db_source_from_target(target_base, 0, sheet_bytes,
                                                true, kSheetW, kSheetH))
                return false;
            m_db_sheet_bytes = m_db_source_window;
            m_db_sheet_generation = m_db_source_generation;
            m_db_sheet_target = target_base;
        }
        if (m_db_source_window.capacity() < source_bytes)
            perf::add_counter(perf::Counter::DbSourceBufferGrowths);
        m_db_source_window.assign(source_bytes, 0);
        std::memcpy(m_db_source_window.data(),
                    m_db_sheet_bytes.data() +
                        static_cast<size_t>(source_row) * bytes_per_row,
                    source_bytes);
        return true;
    }
    if (row_span && source_row + strip_rows <= kSheetH) {
        window_y = std::clamp(
            static_cast<int>(std::floor(
                (org_y + static_cast<float>(source_row)) * sy)),
            0, fb_h);
        requested_window_h = static_cast<int>(std::ceil(
            static_cast<float>(strip_rows) * sy));
    }
    const int window_w = std::min(requested_window_w, fb_w - window_x);
    const int window_h = std::min(requested_window_h, fb_h - window_y);
    bool window_read = false;
    {
        perf::Scope readback_scope(perf::Phase::FramebufferReadback);
        if (window_w > 0 && window_h > 0) {
            const size_t window_pixels =
                static_cast<size_t>(window_w) * window_h;
            if (m_retained_rgba_window.capacity() < window_pixels)
                perf::add_counter(perf::Counter::ReadbackBufferGrowths);
            m_retained_rgba_window.resize(window_pixels);
            window_read = m_backend->grab_clt_target_window_rgba(
                target_base, m_retained_rgba_window.data(), window_x, window_y,
                window_w, window_h);
        }
        if (!window_read) {
            const size_t pixel_count = static_cast<size_t>(fb_w) * fb_h;
            if (m_retained_rgba.capacity() < pixel_count)
                perf::add_counter(perf::Counter::ReadbackBufferGrowths);
            m_retained_rgba.resize(pixel_count);
            if (!m_backend->grab_clt_target_rgba(
                    target_base, m_retained_rgba.data(), fb_w, fb_h))
                return false;
        }
    }

    std::vector<uint32_t> metadata;
    if (copy_ssb) {
        metadata.resize(size_t(window_w)*window_h);
        if (!window_read || !m_backend->grab_clt_target_window_metadata(
                target_base,metadata.data(),window_x,window_y,window_w,window_h)) return false;
    }
    const size_t sheet_bytes = static_cast<size_t>(kSheetW) * kSheetH * 2;
    if (m_db_source_window.capacity() < source_bytes)
        perf::add_counter(perf::Counter::DbSourceBufferGrowths);
    m_db_source_window.assign(source_bytes, 0);
    const size_t pixel_bytes = source_bytes / 2u;
    // Content diagnostic: texels that are NOT the host clear color. The
    // framebuffer clear (0.05,0.07,0.10) converts to RGB555 word 0x0443, so
    // a snapshot of an undrawn frame scores ~0% while world geometry scores
    // high. (A plain nonzero count is vacuous: the clear color is nonzero.)
    constexpr uint16_t kClearWord = 0x0443u;
    uint32_t inprog_nonclear = 0;
    // Content signature for the matching [dbsrc-fb] inprogress line. A
    // destination whose snapshot never changes is indistinguishable from one
    // whose draw was rejected unless the sampled bytes are printed; the mean
    // 5-bit green channel is enough to separate "frozen" from "advancing".
    uint64_t inprog_green_sum = 0;
    for (size_t p = 0; p < pixel_bytes; ++p) {
        const int64_t src = window_rel + static_cast<int64_t>(p) * 2;
        if (src < 0 || src >= static_cast<int64_t>(sheet_bytes)) continue;
        const int64_t idx = src / 2;
        const int shx = static_cast<int>(idx % kSheetW);
        const int shy = static_cast<int>(idx / kSheetW);
        const int fx = static_cast<int>((org_x + static_cast<float>(shx)) * sx);
        const int fy = static_cast<int>((org_y + static_cast<float>(shy)) * sy);
        if (fx < 0 || fx >= fb_w || fy < 0 || fy >= fb_h) continue;
        uint32_t px = 0;
        if (window_read) {
            if (fx < window_x || fx >= window_x + window_w ||
                fy < window_y || fy >= window_y + window_h)
                continue;
            px = m_retained_rgba_window[
                static_cast<size_t>(fy - window_y) * window_w +
                static_cast<size_t>(fx - window_x)];
        } else {
            px = m_retained_rgba[static_cast<size_t>(fy) * fb_w + fx];
        }
        const uint32_t r5 = (px & 0xffu) >> 3;
        const uint32_t g5 = ((px >> 8u) & 0xffu) >> 3;
        const uint32_t b5 = ((px >> 16u) & 0xffu) >> 3;
        const uint16_t word = static_cast<uint16_t>(
            (copy_ssb && (metadata[size_t(fy-window_y)*window_w+fx-window_x] & 255u) >= 128u ? 0x8000u : 0u) |
            (r5 << 10u) | (g5 << 5u) | b5);
        m_db_source_window[p * 2u] = static_cast<uint8_t>(word >> 8u);
        m_db_source_window[p * 2u + 1u] =
            static_cast<uint8_t>(word & 0xffu);
        if (word != kClearWord) ++inprog_nonclear;
        inprog_green_sum += (word >> 5u) & 0x1fu;
    }
    m_db_inprog_nonclear_pct =
        pixel_bytes != 0 ? (100u * inprog_nonclear) /
                               static_cast<uint32_t>(pixel_bytes)
                         : 0u;
    m_db_inprog_green_mean = pixel_bytes != 0
                                 ? inprog_green_sum /
                                       static_cast<uint64_t>(pixel_bytes)
                                 : 0u;
    return true;
}

void GuestCltInterpreter::note_destination_write(uint32_t target)
{
    if (target == 0u) return;
    if (target == m_db_sheet_target) m_db_sheet_generation = ~0ull;
    const uint32_t generation = static_cast<uint32_t>(m_db_source_generation);
    for (auto& [known_target, known_generation] : m_target_write_generations) {
        if (known_target != target) continue;
        known_generation = generation;
        return;
    }
    // Keep the map bounded as before. A target absent from a full map falls
    // back to the current state generation in load_db_source_texture(), which
    // is conservative and still cannot reuse an older list's snapshot.
    if (m_target_write_generations.size() < 32u)
        m_target_write_generations.emplace_back(target, generation);
}

void GuestCltInterpreter::note_destination_draw(const CltVertex* vertices,
                                                size_t count)
{
    // A draw changes its target's contents, so a later same-list DB-source
    // read of that region must not reuse a snapshot taken before it. The
    // Main Menu's checkered flag blends against the target beneath it after
    // the IMSA banner is drawn; a list-scoped snapshot from the footer's
    // earlier read erased the banner's middle rows. Record bounds instead of
    // advancing the generation, so reads elsewhere keep their snapshot.
    const uint32_t target = register_value(kDbDestBaseAddr);
    if (target == 0u || !vertices || count == 0u) return;
    if (target == m_db_sheet_target) m_db_sheet_generation = ~0ull;
    float min_x = vertices[0].x, max_x = min_x;
    float min_y = vertices[0].y, max_y = min_y;
    for (size_t i = 1; i < count; ++i) {
        min_x = std::min(min_x, vertices[i].x);
        max_x = std::max(max_x, vertices[i].x);
        min_y = std::min(min_y, vertices[i].y);
        max_y = std::max(max_y, vertices[i].y);
    }
    for (DirtyBounds& dirty : m_target_dirty_bounds) {
        if (dirty.target != target) continue;
        dirty.min_x = std::min(dirty.min_x, min_x);
        dirty.min_y = std::min(dirty.min_y, min_y);
        dirty.max_x = std::max(dirty.max_x, max_x);
        dirty.max_y = std::max(dirty.max_y, max_y);
        return;
    }
    if (m_target_dirty_bounds.size() < 32u) {
        m_target_dirty_bounds.push_back({target, min_x, min_y, max_x, max_y});
        return;
    }
    // Bounded like the generation map: an untracked target invalidates.
    ++m_db_source_generation;
    note_destination_write(target);
}

void GuestCltInterpreter::set_fb_spans(const std::vector<CltFbSpan>& spans)
{
    m_fb_spans = spans;
}

void GuestCltInterpreter::set_host_frame(uint32_t host_frame)
{
    m_host_frame = host_frame;
}

// State 3 settled-world provenance: M2_DRAW_PROV_FRAME selects exactly one
// HOST frame (ran_frames numbering, same frames the soft backend numbers in
// M2_DRAW_ORDER_FRAME). For that frame only, every emitted draw logs one
// [draw-prov] line in execution (painter) order carrying the executed-list
// sequence, CLT header, destination base/stride, bbox, reciprocal-W range,
// depth/blend state, and resident serve dimensions — sufficient to separate
// distant-detail packets
// absent from the EXECUTELIST from packets guest-ordered beneath the
// authentic cyan 0x201d0003 sky quads. Unset/empty = off. Capped.
uint32_t draw_prov_frame()
{
    static const uint32_t frame = [] {
        const char* v = std::getenv("M2_DRAW_PROV_FRAME");
        if (!v || !*v) return UINT32_MAX;
        return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    }();
    return frame;
}

// Optional serve-dimension filter for the same [draw-prov] line:
// M2_DRAW_PROV_SERVE=<W>x<H> logs every emitted draw served by a texture of
// those dimensions (capped), independent of M2_DRAW_PROV_FRAME. Diagnostic
// only; used to locate the draw that owns a screen region for a cel whose
// dimensions are known from the texture dump.
uint32_t draw_prov_serve_packed()
{
    static const uint32_t packed = [] {
        const char* v = std::getenv("M2_DRAW_PROV_SERVE");
        if (!v || !*v) return 0u;
        if (v[0] == '*' && v[1] == '\0') return 1u;  // wildcard: any textured
        unsigned sw = 0, sh = 0;
        if (std::sscanf(v, "%ux%u", &sw, &sh) != 2) return 0u;
        if (sw == 0u || sh == 0u || sw > 4096u || sh > 4096u) return 0u;
        return (sw << 16u) | sh;
    }();
    return packed;
}

bool draw_prov_serve_enabled()
{
    return draw_prov_serve_packed() != 0u;
}

bool draw_prov_serve_matches(int w, int h)
{
    const uint32_t packed = draw_prov_serve_packed();
    if (packed == 0u || w <= 0 || h <= 0) return false;
    if (packed == 1u) return true;
    return (static_cast<uint32_t>(w) << 16u | static_cast<uint32_t>(h)) ==
           packed;
}

void GuestCltInterpreter::draw_prov_note(uint32_t header, bool textured,
                                         int serve_w, int serve_h,
                                         const CltVertex* tri, size_t count,
                                         const CltDepth& depth, bool blend)
{
    const uint32_t want = draw_prov_frame();
    const bool frame_hit = want != UINT32_MAX && m_host_frame == want;
    const bool serve_hit = draw_prov_serve_matches(serve_w, serve_h);
    if (!frame_hit && !serve_hit) return;
    if (!tri || count < 3u) return;
    static uint32_t order = 0;
    static bool expired = false;
    static constexpr uint32_t kMax = 4096u;
    static constexpr uint32_t kMaxServe = 20000u;
    const uint32_t cap = serve_hit && !frame_hit ? kMaxServe : kMax;
    if (order >= cap) {
        if (!expired) {
            expired = true;
            std::fprintf(stderr,
                         "[draw-prov] cap reached at frame %u — further "
                         "entries suppressed\n",
                         m_host_frame);
        }
        return;
    }
    float mnx = tri[0].x, mxx = tri[0].x;
    float mny = tri[0].y, mxy = tri[0].y;
    float ow_min = tri[0].one_over_w, ow_max = tri[0].one_over_w;
    for (size_t i = 1; i < count; ++i) {
        mnx = std::min(mnx, tri[i].x);
        mxx = std::max(mxx, tri[i].x);
        mny = std::min(mny, tri[i].y);
        mxy = std::max(mxy, tri[i].y);
        ow_min = std::min(ow_min, tri[i].one_over_w);
        ow_max = std::max(ow_max, tri[i].one_over_w);
    }
    std::fprintf(stderr,
                 "[draw-prov] frame=%u order=%u exec=%llu header=%#x tex=%d "
                 "serve=%dx%d bbox=(%.1f,%.1f)-(%.1f,%.1f) ow=[%.5g,%.5g] "
                 "ztest=%d zwrite=%d zfunc=%#x blend=%d pip=%#x exp=%#x "
                 "lod0=%#x dbuser=%#x dest=%#x stride=%u src=%#x zbase=%#x\n",
                 m_host_frame, order, (unsigned long long)m_exec_seq, header,
                 textured ? 1 : 0, serve_w, serve_h, mnx, mny, mxx, mxy,
                 ow_min, ow_max, depth.test_enabled ? 1 : 0,
                 depth.write_enabled ? 1 : 0, depth.depth_func,
                 blend ? 1 : 0, register_value(kTxtPipCntl),
                 register_value(kTxtExpType), register_value(kTxtLodBase0),
                 register_value(kDbUserControl),
                 register_value(kDbDestBaseAddr),
                 register_value(kDbDestXStride),
                 register_value(kDbSrcBaseAddr),
                 register_value(kDbZBaseAddr));
    ++order;
}

void GuestCltInterpreter::config_texture_draw_diag(
    const char* path, const Texture& texture, uint32_t lod_base0,
    uint32_t header, const CltVertex* tri, size_t count)
{
    if (!config_texture_trace_enabled() ||
        texture.source_address != kConfigFooterSource)
        return;
    static uint32_t draw_emitted = 0;
    static bool draw_cap_reported = false;
    if (!config_texture_trace_admit("draw", &draw_emitted,
                                   &draw_cap_reported))
        return;

    float min_x = 0.f, max_x = 0.f, min_y = 0.f, max_y = 0.f;
    if (tri && count != 0u) {
        min_x = max_x = tri[0].x;
        min_y = max_y = tri[0].y;
        for (size_t i = 1; i < count; ++i) {
            min_x = std::min(min_x, tri[i].x);
            max_x = std::max(max_x, tri[i].x);
            min_y = std::min(min_y, tri[i].y);
            max_y = std::max(max_y, tri[i].y);
        }
    }
    int cache_index = -1;
    for (size_t i = 0; i < m_tram_cache.size(); ++i) {
        if (&m_tram_cache[i] == &texture) {
            cache_index = static_cast<int>(i);
            break;
        }
    }
    const char* resident = &texture == &m_texture
                               ? "current"
                               : (cache_index >= 0 ? "cache" : "unknown");
    std::fprintf(
        stderr,
        "[config-texture] stage=draw path=%s exec=%llu host_frame=%u "
        "header=%#x resident=%s cache_index=%d cel=%d tram=%#x+%#x "
        "lod0=%#x wh=%dx%d source=%#x source_span=%#x available=%#x "
        "source_hash=%08x decoded_rgba_hash=%08x decoded_valid=%d "
        "bbox=(%.1f,%.1f)-(%.1f,%.1f) sample=",
        path ? path : "unknown", (unsigned long long)m_exec_seq, m_host_frame,
        header, resident, cache_index, static_cast<int>(texture.cel),
        texture.tram_base, texture.tram_size, lod_base0, texture.width,
        texture.height, texture.source_address, texture.source_span,
        texture.source_available, texture.source_checksum,
        texture.decoded_checksum, texture.decoded_checksum_valid ? 1 : 0,
        min_x, min_y, max_x, max_y);
    for (size_t i = 0; i < texture.source_sample_size; ++i)
        std::fprintf(stderr, "%02x", texture.source_sample[i]);
    std::fprintf(stderr, "\n");
}

void GuestCltInterpreter::retain_presented_frame()
{
    m_retained_frame.clear();
    if (!fb_readback_enabled() || !m_backend) return;

    int fb_w = 0, fb_h = 0;
    m_backend->get_size(&fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;
    const size_t pixel_count = static_cast<size_t>(fb_w) * fb_h;
    if (m_retained_rgba.capacity() < pixel_count)
        perf::add_counter(perf::Counter::ReadbackBufferGrowths);
    m_retained_rgba.resize(pixel_count);
    {
        perf::Scope readback_scope(perf::Phase::FramebufferReadback);
        if (!m_backend->grab_presented_rgba(m_retained_rgba.data(), fb_w,
                                            fb_h))
            return;
    }

    // The display carve is the recovered 576x432 Bitmap sitting at
    // view (32,24) inside the 640x480 View. Convert that region of the
    // presented frame to a big-endian RGB555 sheet (guest byte order).
    const int32_t disp_w = m_display_width > 0 ? m_display_width : 640;
    const int32_t disp_h = m_display_height > 0 ? m_display_height : 480;
    const float org_x =
        m_viewport_valid ? static_cast<float>(m_viewport_left) : 32.f;
    const float org_y =
        m_viewport_valid ? static_cast<float>(m_viewport_top) : 24.f;
    const float sx = static_cast<float>(fb_w) / static_cast<float>(disp_w);
    const float sy = static_cast<float>(fb_h) / static_cast<float>(disp_h);

    constexpr int kSheetW = 576;
    constexpr int kSheetH = 432;
    const size_t sheet_bytes = static_cast<size_t>(kSheetW) * kSheetH * 2;
    if (m_retained_frame.capacity() < sheet_bytes)
        perf::add_counter(perf::Counter::RetainedFrameBufferGrowths);
    m_retained_frame.resize(sheet_bytes);
    std::fill(m_retained_frame.begin(), m_retained_frame.end(), 0);
    for (int y = 0; y < kSheetH; ++y) {
        const int fy = static_cast<int>((org_y + static_cast<float>(y)) * sy);
        if (fy < 0 || fy >= fb_h) continue;
        for (int x = 0; x < kSheetW; ++x) {
            const int fx =
                static_cast<int>((org_x + static_cast<float>(x)) * sx);
            if (fx < 0 || fx >= fb_w) continue;
            const uint32_t px =
                m_retained_rgba[static_cast<size_t>(fy) * fb_w + fx];
            const uint32_t r5 = (px & 0xffu) >> 3;
            const uint32_t g5 = ((px >> 8u) & 0xffu) >> 3;
            const uint32_t b5 = ((px >> 16u) & 0xffu) >> 3;
            const uint16_t word =
                static_cast<uint16_t>((r5 << 10u) | (g5 << 5u) | b5);
            const size_t offset =
                (static_cast<size_t>(y) * kSheetW + x) * 2u;
            m_retained_frame[offset] = static_cast<uint8_t>(word >> 8u);
            m_retained_frame[offset + 1u] =
                static_cast<uint8_t>(word & 0xffu);
        }
    }
    ++m_retained_seq;
}

// Two independent rectangles bound a TE color write, and both are scissored
// here together:
//
//  - the destination surface the guest bound with `SetTEFrameBuffer`. Its
//    extent is the Bitmap's own clip (`DBFBCLIP`, published by the runtime as
//    `(clip_width << 16) | clip_height`), placed by the View at `view_left/top`
//    in the display space every emitted vertex already lives in. The recovered
//    Portfolio TE driver programs that clip whenever it binds a framebuffer,
//    so a draw whose vertices run past the surface edge is dropped there.
//    Without it the host paints the display border around the surface - the
//    640x480 display is larger than the guest's 576x432 render target - with
//    guest geometry the original never shows there.
//  - the guest's own `DBXWINCLIP`/`DBYWINCLIP` window clip, which every
//    authored draw leaves disabled (see the register audit's accepted set).
//
// The backend exposes one rectangular scissor, so the two are combined by
// intersection. An inside clip (or both window-clip modes together) still
// needs an inverse/compound clip that is not part of that contract and keeps
// the documented no-scissor approximation for that guest clip, while the
// destination rect continues to contain the write.
void GuestCltInterpreter::update_window_clip()
{
    if (!m_backend) return;

    const auto [display_width, display_height] =
        target_size(m_display_width, m_display_height, m_backend);
    int host_width = 0;
    int host_height = 0;
    m_backend->get_size(&host_width, &host_height);
    if (host_width <= 0) host_width = static_cast<int>(display_width);
    if (host_height <= 0) host_height = static_cast<int>(display_height);

    const float scale_x = static_cast<float>(host_width) / display_width;
    const float scale_y = static_cast<float>(host_height) / display_height;
    const float view_left = m_viewport_valid
                                ? static_cast<float>(m_viewport_left)
                                : 0.f;
    const float view_top = m_viewport_valid
                               ? static_cast<float>(m_viewport_top)
                               : 0.f;

    const uint32_t fb_clip = register_value(kDbFbClip);
    CltClipRect destination;
    destination.x = view_left;
    destination.y = view_top;
    destination.w = static_cast<float>(
        (fb_clip >> 16u) & kDbWinClipCoordinateMask);
    destination.h = static_cast<float>(fb_clip & kDbWinClipCoordinateMask);

    const uint32_t control = register_value(kDbUserControl);
    const bool clip_inside = (control & kDbWinClipInEnable) != 0;
    const bool clip_outside = (control & kDbWinClipOutEnable) != 0;

    CltClipRect window;
    float x_min = 0.f, x_max = 0.f, y_min = 0.f, y_max = 0.f;
    bool window_usable = false;
    if (clip_outside && !clip_inside) {
        const uint32_t x_clip = register_value(kDbXWinClip);
        const uint32_t y_clip = register_value(kDbYWinClip);
        x_min = static_cast<float>((x_clip >> 16u) & kDbWinClipCoordinateMask);
        x_max = static_cast<float>(x_clip & kDbWinClipCoordinateMask);
        y_min = static_cast<float>((y_clip >> 16u) & kDbWinClipCoordinateMask);
        y_max = static_cast<float>(y_clip & kDbWinClipCoordinateMask);
        if (x_max >= x_min && y_max >= y_min) {
            window.x = view_left + x_min;
            window.y = view_top + y_min;
            window.w = x_max - x_min;
            window.h = y_max - y_min;
            window_usable = !clt_clip_rect_empty(window);
        }
    }

    CltClipRect combined;
    if (window_usable && !clt_clip_rect_empty(destination))
        combined = clt_clip_rect_intersection(destination, window);
    else if (window_usable)
        combined = window;
    else
        combined = destination;

    static const bool s_trace = (getenv("M2_WINCLIP_TRACE") != nullptr);
    if (s_trace) {
        static int s_count = 0;
        if (s_count < 50) {
            fprintf(stderr, "[CLT-WINCLIP] ctrl=%#x inside=%d outside=%d x=(%g..%g) y=(%g..%g) view=(%g,%g) scale=(%g,%g) dest=(%g,%g,%g,%g) -> scissor(%g,%g,%g,%g)\n",
                    control, clip_inside, clip_outside, x_min, x_max, y_min, y_max,
                    view_left, view_top, scale_x, scale_y,
                    destination.x, destination.y, destination.w, destination.h,
                    combined.x * scale_x, combined.y * scale_y,
                    combined.w * scale_x, combined.h * scale_y);
            s_count++;
        }
    }
    static const bool s_disable_scissor = (getenv("M2_DISABLE_SCISSOR") != nullptr);
    if (s_disable_scissor) {
        m_window_clip_mode = WindowClipMode::disabled;
        m_backend->scissor_disable();
        return;
    }

    // The inside-clip form has no backend representation, so that guest clip
    // keeps its documented no-scissor approximation rather than an invented
    // inverse. The bound destination surface is platform state, independent of
    // the guest clip, so it still contains the write when there is one.
    const bool guest_clip_unsupported =
        clip_inside || (clip_outside && !window_usable);
    if (guest_clip_unsupported && clt_clip_rect_empty(destination)) {
        m_window_clip_mode = WindowClipMode::unsupported;
        m_backend->scissor_disable();
        return;
    }

    if (clt_clip_rect_empty(combined)) {
        m_window_clip_mode = WindowClipMode::disabled;
        m_backend->scissor_disable();
        return;
    }

    m_window_clip_mode =
        window_usable ? WindowClipMode::outside
                      : (guest_clip_unsupported ? WindowClipMode::unsupported
                                                : WindowClipMode::destination);
    m_backend->scissor(combined.x * scale_x, combined.y * scale_y,
                       combined.w * scale_x, combined.h * scale_y);
}

void GuestCltInterpreter::update_destination_blend()
{
    const uint32_t control = register_value(kDbUserControl);
    const bool blend_enabled = (control & kDbBlendEnable) != 0;
    const bool source_enabled = (control & kDbSourceEnable) != 0;
    if (!blend_enabled) {
        m_destination_blend_mode = DestinationBlendMode::disabled;
    } else if (source_enabled) {
        // BLENDEN + SRCEN means the destination blender reads the bound source
        // frame buffer (`M_DBTrans` and the `Blt_BlendBlobObject` form).  The
        // recovered State 3 blend setup is the source-over form; the exact
        // statically authored equations are selected from the decoded
        // registers instead of being collapsed into it.
        const CltDsbAMultiplier a =
            clt_decode_dsb_a_multiplier(register_value(kDbAMultCntl));
        const CltDsbBMultiplier b =
            clt_decode_dsb_b_multiplier(register_value(kDbBMultCntl));
        const CltDsbAlu alu = clt_decode_dsb_alu(register_value(kDbAluCntl));
        if (clt_dsb_is_source_copy(a, b, alu,
                                   register_value(kDbBMultConstSsb0),
                                   register_value(kDbBMultConstSsb1)))
            m_destination_blend_mode = DestinationBlendMode::source_copy;
        else if (clt_dsb_is_coefficient_weighted_mix(a, b, alu))
            m_destination_blend_mode =
                DestinationBlendMode::coefficient_weighted_mix;
        else if (clt_dsb_is_constant_alpha_blend(a, b, alu))
            m_destination_blend_mode = DestinationBlendMode::constant_alpha_over;
        else if (clt_dsb_is_inverse_texture_multiply(a, b, alu))
            m_destination_blend_mode =
                DestinationBlendMode::inverse_texture_multiply;
        else if (clt_dsb_is_secondary_source_alpha(a, b, alu))
            m_destination_blend_mode =
                DestinationBlendMode::secondary_source_alpha;
        else if (clt_dsb_is_source_over(a, b, alu))
            m_destination_blend_mode = DestinationBlendMode::source_over;
        else
            m_destination_blend_mode =
                DestinationBlendMode::unsupported;
    } else if (clt_dsb_is_specular_composite(
                   clt_decode_dsb_a_multiplier(register_value(kDbAMultCntl)),
                   clt_decode_dsb_b_multiplier(register_value(kDbBMultCntl)),
                   clt_decode_dsb_alu(register_value(kDbAluCntl)))) {
        // The reference M_DBSpec macro uses A=TEXCOLOR*CONST and
        // B=CONSTCOLOR*TEXALPHA with A+BCLAMP, while leaving SRCEN clear.
        // This is an internal color composite, not framebuffer blending.
        // The predicate is the decoded equation, so any other selector or
        // ALU form (including every statically authored non-specular form)
        // falls through to the fail-closed branch below.
        m_destination_blend_mode = DestinationBlendMode::specular_add;
    } else if (clt_dsb_is_fog_composite(
                   clt_decode_dsb_a_multiplier(register_value(kDbAMultCntl)),
                   clt_decode_dsb_b_multiplier(register_value(kDbBMultCntl)),
                   clt_decode_dsb_alu(register_value(kDbAluCntl)))) {
        // The reference M_DBFog macro uses A=TEXCOLOR*TEXALPHA and
        // B=CONSTCOLOR*(1-TEXALPHA) with A+BCLAMP, while leaving SRCEN clear.
        // Like M_DBSpec this is an internal composite (BLENDEN set, no frame
        // buffer source), but the per-vertex fog factor in the texture-stage
        // alpha lerps the color toward the DBCONSTIN fog constant rather than
        // adding to it. Without this recognition the equation fell through to
        // the fail-closed branch and every fogged world draw was dropped,
        // leaving only the clear color (the gray-screen fog defect).
        m_destination_blend_mode = DestinationBlendMode::fog_composite;
    } else {
        // Keep the register-only result unsupported when SRCEN is clear.
        // execute() can still prove an opaque source-over primitive has a
        // zero source coefficient without assigning the disabled input a value.
        m_destination_blend_mode = DestinationBlendMode::unsupported;
    }
    // General equations consume independently addressed RGB, alpha and DSB.
    // BMTYPE_16 contains no stored alpha: preserve that unresolved synthesis
    // boundary instead of treating its DSB as alpha. BMTYPE_32 supplies A7.
    const auto a = clt_decode_dsb_a_multiplier(register_value(kDbAMultCntl));
    const auto b = clt_decode_dsb_b_multiplier(register_value(kDbBMultCntl));
    const auto alu = clt_decode_dsb_alu(register_value(kDbAluCntl));
    const auto sa = clt_decode_dsb_src_alpha_control(register_value(kDbSrcAlphaCntl));
    const auto da = clt_decode_dsb_dest_alpha_control(register_value(kDbDestAlphaCntl));
    const bool source32 = (register_value(kDbSrcCntl) & kDbSrc32Bpp) != 0;
    const unsigned op = alu.op_raw;
    const bool uses_a = op < 16u || ((op & 3u) != ((op >> 2u) & 3u));
    const bool uses_b = op < 16u || ((op & 5u) != ((op >> 1u) & 5u));
    const auto needs_rgb = [](uint32_t raw, bool aside) {
        const unsigned input = (raw >> 6u) & 3u, coef = (raw >> 1u) & 24u;
        return aside ? input == 2u || coef == 24u : input == 0u;
    };
    const auto needs_alpha = [](uint32_t raw) { return ((raw >> 1u) & 24u) == 8u; };
    const auto needs_ssb = [](uint32_t raw) { return (raw & 0x34u) == 0x24u; };
    const bool rgb_uses_source_alpha = (uses_a && needs_alpha(a.raw)) ||
                                       (uses_b && needs_alpha(b.raw));
    m_rgb_needs_16bit_source_alpha =
        blend_enabled && rgb_uses_source_alpha && !source32;
    const bool rgb_uses_source_ssb = (uses_a && needs_ssb(a.raw)) ||
                                     (uses_b && needs_ssb(b.raw));
    const bool output_source_ssb = source_enabled &&
        (register_value(kDbSsbDsbCntl) & 3u) == 2u;
    const bool output_source_alpha = source32 && da.source == CltDestAlphaSource::source_alpha;
    const bool output_source_constant = da.source == CltDestAlphaSource::source_ssb_const;
    const bool needs_source = rgb_uses_source_alpha || rgb_uses_source_ssb ||
        output_source_ssb || output_source_alpha || output_source_constant ||
        (uses_a && needs_rgb(a.raw, true)) || (uses_b && needs_rgb(b.raw, false));
    // UI_DrawHighScores_nameEntry draws the record row with TextDestBlendInit's
    // constant-color text equation but sets DBLA_EnableAttrs=0x5f, clearing
    // SRCEN. No reference defines SRCCOLOR with the source input disabled; the
    // general internal path evaluates it as zero (no fetch), so anti-aliased
    // edges darken like the LCD font's opaque rows. Admit only this equation
    // whose single source dependency is that RGB B term.
    const bool unarmed_constant_text = blend_enabled && !source_enabled &&
        clt_dsb_is_constant_alpha_blend(a, b, alu) &&
        !rgb_uses_source_alpha && !rgb_uses_source_ssb && !output_source_ssb &&
        !output_source_alpha && !output_source_constant;
    const bool general_valid = blend_enabled && a.valid && b.valid &&
        alu.op_documented && alu.final_divide_documented && alu.reserved_clear &&
        sa.documented && da.documented && (!rgb_uses_source_alpha || source32) &&
        (!needs_source || source_enabled || unarmed_constant_text);
    const bool metadata_changed = output_source_ssb ||
        da.source == CltDestAlphaSource::texture_ssb_const ||
        da.source == CltDestAlphaSource::rblend ||
        (source_enabled && (output_source_alpha || output_source_constant));
    if (general_valid && (m_destination_blend_mode == DestinationBlendMode::unsupported ||
                          metadata_changed))
        m_destination_blend_mode = needs_source && source_enabled ?
            DestinationBlendMode::general_secondary :
            DestinationBlendMode::general_internal;

}

CltBlendState GuestCltInterpreter::destination_blend_state() const
{
    CltBlendState state;
    state.dsb = clt_decode_dsb(register_value(kDbSsbDsbCntl));
    if (m_rgb555_dither &&
        (register_value(kDbUserControl) & kDbDitherEnable) != 0u) {
        state.dither.enabled = true;
        state.dither.matrix_a = register_value(kDbDitherMatrixA);
        state.dither.matrix_b = register_value(kDbDitherMatrixB);
    }
    state.equation.a = clt_decode_dsb_a_multiplier(register_value(kDbAMultCntl));
    state.equation.b = clt_decode_dsb_b_multiplier(register_value(kDbBMultCntl));
    state.equation.alu = clt_decode_dsb_alu(register_value(kDbAluCntl));
    state.general = {register_value(kDbAMultCntl), register_value(kDbBMultCntl),
        register_value(kDbAluCntl), register_value(kDbConstIn),
        {register_value(kDbAMultConstSsb0), register_value(kDbAMultConstSsb1)},
        {register_value(kDbBMultConstSsb0), register_value(kDbBMultConstSsb1)},
        register_value(kDbSrcAlphaCntl), register_value(kDbDestAlphaCntl),
        register_value(kDbDestAlphaConst)};
    switch (m_destination_blend_mode) {
    case DestinationBlendMode::general_internal:
        state.mode = CltBlendMode::general_internal;
        break;
    case DestinationBlendMode::general_secondary:
        state.mode = CltBlendMode::general_secondary;
        break;
    case DestinationBlendMode::source_over:
        state.mode = CltBlendMode::source_over;
        break;
    case DestinationBlendMode::specular_add:
    case DestinationBlendMode::fog_composite:
    case DestinationBlendMode::constant_alpha_over: {
        // All three forms take their color from the guest's DBCONSTIN register.
        const uint32_t constant = register_value(kDbConstIn);
        state.constant_r = color_component(constant, 16u);
        state.constant_g = color_component(constant, 8u);
        state.constant_b = color_component(constant, 0u);
        switch (m_destination_blend_mode) {
        case DestinationBlendMode::fog_composite:
            state.mode = CltBlendMode::fog_composite;
            break;
        case DestinationBlendMode::constant_alpha_over:
            state.mode = CltBlendMode::constant_alpha_over;
            break;
        default:
            state.mode = CltBlendMode::specular_add;
            break;
        }
        break;
    }
    case DestinationBlendMode::inverse_texture_multiply:
        state.mode = CltBlendMode::inverse_texture_multiply;
        break;
    case DestinationBlendMode::source_copy:
        // B = SRCCOLOR * CONST with the coefficient constant white and the ALU
        // `B` operation: the equation carries no color of its own beyond the
        // bound source frame buffer pixel, and it does not read the
        // destination, so the mode needs no constant or alpha input.
        state.mode = CltBlendMode::source_copy;
        break;
    case DestinationBlendMode::secondary_source_alpha:
        state.mode = CltBlendMode::secondary_source_alpha;
        break;
    case DestinationBlendMode::coefficient_weighted_mix: {
        // Toolkit `Blt_BlendBlitObject` / `defaultDBlend` weighted sum: the
        // A-side coefficient pair scales the texture-blender output and the
        // B-side pair scales the bound source frame buffer pixel.  The B
        // weight is the coefficient itself for the `CONST` selector and its
        // complement for `CONSTCOMPLEMENT`, which is how the guest's
        // `setSsbBlendAttr` spells `1 - w`.  Both entries are resolved and
        // carried; the per-texel SSB selects between them at draw time.
        //
        // `...CONSTSSB0` is coefficient entry 0 and `...CONSTSSB1` entry 1; the
        // CLT toolkit's "source selection bit" (the per-texel SSB, 0 for a
        // texture with no SSB) selects between them, so entry 0 is the value
        // this draw reads unless the SSB picks entry 1.
        const auto weights = [](uint32_t raw, bool complement, float* r,
                                float* g, float* b) {
            *r = clt_dsb_const_weight(raw, 16u);
            *g = clt_dsb_const_weight(raw, 8u);
            *b = clt_dsb_const_weight(raw, 0u);
            if (complement) {
                *r = 1.f - *r;
                *g = 1.f - *g;
                *b = 1.f - *b;
            }
        };
        const bool b_complement =
            state.equation.b.coef == CltDbBCoef::constant_complement;
        const uint32_t a_const0 = register_value(kDbAMultConstSsb0);
        const uint32_t b_const0 = register_value(kDbBMultConstSsb0);
        weights(a_const0, false, &state.weight_tex_r, &state.weight_tex_g,
                &state.weight_tex_b);
        weights(b_const0, b_complement, &state.weight_src_r,
                &state.weight_src_g, &state.weight_src_b);
        const uint32_t a_const1 = register_value(kDbAMultConstSsb1);
        const uint32_t b_const1 = register_value(kDbBMultConstSsb1);
        float entry1_tex_r = 0.f, entry1_tex_g = 0.f, entry1_tex_b = 0.f;
        float entry1_src_r = 0.f, entry1_src_g = 0.f, entry1_src_b = 0.f;
        weights(a_const1, false, &entry1_tex_r, &entry1_tex_g, &entry1_tex_b);
        weights(b_const1, b_complement, &entry1_src_r, &entry1_src_g,
                &entry1_src_b);
        state.weighted_mix_entry_dependent =
            entry1_tex_r != state.weight_tex_r ||
            entry1_tex_g != state.weight_tex_g ||
            entry1_tex_b != state.weight_tex_b ||
            entry1_src_r != state.weight_src_r ||
            entry1_src_g != state.weight_src_g ||
            entry1_src_b != state.weight_src_b;
        state.entry1_weight_tex_r = entry1_tex_r;
        state.entry1_weight_tex_g = entry1_tex_g;
        state.entry1_weight_tex_b = entry1_tex_b;
        state.entry1_weight_src_r = entry1_src_r;
        state.entry1_weight_src_g = entry1_src_g;
        state.entry1_weight_src_b = entry1_src_b;
        state.mode = CltBlendMode::coefficient_weighted_mix;
        break;
    }
    case DestinationBlendMode::disabled:
    case DestinationBlendMode::unsupported:
        break;
    }
    // The destination-alpha control registers are decoded for every blend
    // form, exactly as the guest wrote them.  `DBDESTALPHACNTL` selects the
    // *written* destination alpha independently of the RGB coefficients.
    // First retain the established fast-path dispositions; general modes
    // below evaluate the additional selections and explicitly mark missing
    // framebuffer metadata. M_DBSpec/SRCALPHA preserves its RGB equation.
    state.src_alpha =
        clt_decode_dsb_src_alpha_control(register_value(kDbSrcAlphaCntl));
    state.dest_alpha =
        clt_decode_dsb_dest_alpha_control(register_value(kDbDestAlphaCntl));
    state.dest_alpha_const =
        clt_decode_dsb_dest_alpha_const(register_value(kDbDestAlphaConst));
    const bool aclamp_leave_alone =
        state.src_alpha.documented &&
        state.src_alpha.alpha0 == 0u && state.src_alpha.alpha_frac == 0u &&
        state.src_alpha.alpha1 == 0u;
    const bool dest_alpha_tex = state.dest_alpha.documented &&
                                state.dest_alpha.source ==
                                    CltDestAlphaSource::texture_alpha;
    // All recognized fast-path RGB equations ignore framebuffer alpha. Its
    // documented clamp is therefore a no-op when destination alpha is TEXALPHA.
    state.alpha_control_unsupported = !state.src_alpha.documented || !dest_alpha_tex;
    // Configure Car carries the documented SRCALPHA destination-alpha
    // selection into M_DBSpec draws.  That selector changes the alpha written
    // by the destination blender; it does not replace M_DBSpec's independently
    // recovered RGB equation.  Downgrading the whole draw to source-over made
    // the model's zero-alpha vertices erase its color.  Preserve the exact
    // specular RGB path while keeping the unmodelled alpha result explicit.
    const bool preserve_specular_rgb =
        aclamp_leave_alone &&
        state.mode == CltBlendMode::specular_add &&
        state.dest_alpha.documented &&
        state.dest_alpha.source == CltDestAlphaSource::source_alpha;
    // The M_DBFog distance-fog lerp is an RGB equation the destination-alpha
    // controls do not touch: TEXALPHA is the per-vertex fog factor in the
    // texture-stage alpha, not the written destination alpha.  Keep the fog
    // RGB composite even when the written-alpha result is unmodelled, exactly
    // as the specular and secondary-source RGB paths are preserved; only the
    // written alpha stays marked unsupported below.
    const bool preserve_fog_rgb = state.mode == CltBlendMode::fog_composite;
    // The reverse-alpha secondary-source form is not allowed to fall back to
    // source-over when its written-alpha controls change: doing so would
    // silently substitute the current destination for the separately armed DB
    // source and would also make the alpha-control result unobservable. Keep
    // the recovered RGB mode so the draw boundary can reject that unmodelled
    // combination explicitly.
    const bool preserve_secondary_rgb =
        state.mode == CltBlendMode::secondary_source_alpha;
    if (state.alpha_control_unsupported &&
        !preserve_specular_rgb && !preserve_secondary_rgb &&
        !preserve_fog_rgb &&
        state.mode != CltBlendMode::disabled &&
        state.mode != CltBlendMode::source_over)
        state.mode = CltBlendMode::source_over;
    if (m_destination_blend_mode == DestinationBlendMode::general_internal ||
        m_destination_blend_mode == DestinationBlendMode::general_secondary) {
        state.mode = m_destination_blend_mode == DestinationBlendMode::general_internal ?
            CltBlendMode::general_internal : CltBlendMode::general_secondary;
        // Preserve independently defined RGB when only written alpha needs
        // unavailable framebuffer metadata, as the established specular path
        // does. The approximation remains explicit and is reported by the
        // backend; it must never feed back into the RGB coefficients.
        state.alpha_control_unsupported = !(register_value(kDbSrcCntl) & kDbSrc32Bpp) &&
            state.dest_alpha.source == CltDestAlphaSource::source_alpha;
        if (state.alpha_control_unsupported) {
            state.general.dest_alpha = 0;
            clt_srcalpha16_tripwire(
                "written-alpha", m_exec_seq, register_value(kDbUserControl),
                register_value(kDbSrcCntl), register_value(kDbAMultCntl),
                register_value(kDbBMultCntl), register_value(kDbAluCntl),
                register_value(kDbSrcAlphaCntl),
                register_value(kDbDestAlphaCntl));
        }
    }
    return state;
}

void GuestCltInterpreter::gate_weighted_mix_on_ssb(CltBlendState& state,
                                                   Texture* texture,
                                                   const ArmedDecode* served)
{
    if (state.mode != CltBlendMode::coefficient_weighted_mix ||
        !state.weighted_mix_entry_dependent)
        return;
    const bool ssb_signal_nonzero =
        served != nullptr
            ? served->ssb_nonzero
            : texture != nullptr && texture->ssb_nonzero;
    if (!clt_dsb_weighted_mix_ssb_selects_entry1(true, ssb_signal_nonzero))
        return;
    // The per-texel SSB selects between the two coefficient entries, so the
    // draw is two equations across the primitive.  That is now evaluated
    // rather than downgraded: the software renderer samples the texture's SSB
    // bitmap per fragment and the GL backends split the primitive into an
    // SSB-0 pass (entry 0) and an SSB-1 pass masked by the uploaded `ssb_mask`
    // cel, each uniform in its own entry.  The backend must be able to carry
    // the second entry; when it cannot, fail closed to the recovered
    // source-over default.
    state.weighted_mix_ssb_selects_entry1 = true;
    if (m_backend == nullptr || texture == nullptr)
        state.mode = CltBlendMode::source_over;
    // The split samples the texture's SSB bitmap per fragment.  The discard
    // path may already have uploaded the mask cel for this texture; reuse it
    // when present, otherwise upload it now (or fail closed when the cel
    // store is unavailable).
    else if (served != nullptr && served->ssb_mask != kInvalidCel)
        state.weighted_mix_ssb_mask = served->ssb_mask;
    else if (served != nullptr)
        state.mode = CltBlendMode::source_over;
    else if (texture->ssb_mask != kInvalidCel)
        state.weighted_mix_ssb_mask = texture->ssb_mask;
    else if (!ensure_ssb_mask_for(*texture))
        state.mode = CltBlendMode::source_over;
    else
        state.weighted_mix_ssb_mask = texture->ssb_mask;
}

CltDepth GuestCltInterpreter::guest_depth_state() const
{
    CltDepth depth;
    const uint32_t db_user = register_value(kDbUserControl);
    depth.test_enabled = (db_user & kDbZBufferEnable) != 0;
    depth.write_enabled = (db_user & kDbZOutputEnable) != 0;
    depth.depth_func = register_value(kDbZCntl);
    const uint32_t z_base = register_value(kDbZBaseAddr);
    const uint32_t dsb = register_value(kDbSsbDsbCntl);
    // Object SSB is zero for the untextured primitive-color fill. A constant
    // zero DSB is equivalent. Nonzero/source DSB and offset aliasing remain
    // separate contracts; do not reinterpret those as a near-cell write.
    depth.color_target_is_z = z_base != 0 &&
        register_value(kDbDestBaseAddr) == z_base &&
        (register_value(kDbDestControl) & 1u) == 0 &&
        register_value(kTxtTabCntl) == 0 &&
        (db_user & 0x1fu) == 0x0fu &&
        ((dsb & 3u) == 0u || (dsb & 7u) == 1u) &&
        register_value(kDbZOffset) == 0 &&
        (register_value(kDbDestXStride) & 0x7ffu) ==
            ((register_value(kDbZClip) >> 16u) & 0x7ffu);
    // DBZOFFSET (0x8048): signed 12-bit Z X/Y offsets. The vendor driver
    // leaves them zero when it binds the Z bitmap; CLT lists author them.
    const uint32_t z_offset = register_value(kDbZOffset);
    const int32_t z_offset_x = sign_extend_12(z_offset >> 16u);
    const int32_t z_offset_y = sign_extend_12(z_offset);
    // DBZCLIP (0x804c): 11-bit X/Y region bounds, written by the Portfolio
    // TE driver from the bound Z bitmap's Width/Height (hw.c:SetTEZBuffer).
    const uint32_t z_clip = register_value(kDbZClip);
    const int32_t z_clip_x =
        static_cast<int32_t>((z_clip >> 16u) & kDbZClipCoordinateMask);
    const int32_t z_clip_y =
        static_cast<int32_t>(z_clip & kDbZClipCoordinateMask);
    // Guest CLT vertices are bound-bitmap coordinates and the interpreter
    // adds the View origin when it emits, so fold the origin and the signed
    // Z offsets into the bounds the backends test against emitted vertices:
    // host = guest + view, region test is guest + zoffset in [0, clip).
    const int32_t view_left = m_viewport_valid ? m_viewport_left : 0;
    const int32_t view_top = m_viewport_valid ? m_viewport_top : 0;
    depth.z_region_min_x = view_left - z_offset_x;
    depth.z_region_max_x = depth.z_region_min_x + z_clip_x;
    depth.z_region_min_y = view_top - z_offset_y;
    depth.z_region_max_y = depth.z_region_min_y + z_clip_y;
    depth.z_clipped_discard =
        (register_value(kDbDiscardControl) & kDbDiscardZClipped) != 0u;
    return depth;
}

// FNV-1a over a raw byte range (declared here for the texture decoders below;
// defined with its vector wrapper next to the materializer caches).
uint64_t blob_hash_bytes(const uint8_t* data, size_t size);
uint64_t pip_palette_hash(const std::vector<uint32_t>& pip);
uint64_t mmdma_sidecar_hash(const std::vector<uint8_t>& indices,
                            const std::vector<uint8_t>& alpha,
                            const std::vector<uint8_t>& source_ssb);
uint64_t clt_now_ns();

bool GuestCltInterpreter::load_pip(const CltMemorySnapshot* memory)
{
    // The vendor PIP builder writes TEPIPRAM + 4*pipIndex. PIP RAM is the
    // 256-word window 0x46000..0x463ff, so a valid nonzero destination is a
    // word-aligned window entry rather than another texture/TRAM base.
    const uint32_t pip_base = register_value(kTxtLodBase0);
    if (pip_base < kPipRamBase || pip_base > kPipRamEnd ||
        ((pip_base - kPipRamBase) & (sizeof(uint32_t) - 1u)) != 0u)
        return false;
    const size_t pip_index =
        (pip_base - kPipRamBase) / sizeof(uint32_t);
    if (pip_index >= m_pip.size()) return false;

    const uint32_t byte_count = register_value(kTxtCount) & 0x00ffffffu;
    const uint32_t source_address = register_value(kTxtLdSrcAddr);
    if (byte_count == 0 || (byte_count & 3u) != 0 || byte_count > 1024u)
        return false;

    const CltMemoryView source =
        find_memory(memory, source_address, byte_count);
    if (!source.data || source.size < byte_count) return false;
    const size_t entry_count = byte_count / sizeof(uint32_t);
    if (entry_count > m_pip.size() - pip_index) return false;
    bool changed = false;
    for (size_t i = 0; i < entry_count; ++i) {
        const uint32_t value =
            read_be32(source.data + i * sizeof(uint32_t));
        changed = changed || m_pip[pip_index + i] != value;
        m_pip[pip_index + i] = value;
    }
    // A PIP DMA that rewrites identical words has no guest-visible hardware
    // effect. Keep the content generation stable so indexed TRAM residents do
    // not get decoded, uploaded, mipmapped, and deleted again solely because
    // the guest repeated the same palette publication.
    if (changed) {
        ++m_pip_generation;
        m_pip_hash = pip_palette_hash(m_pip);
    }
    m_last_pip_source = source_address;
    m_last_pip_bytes = byte_count;
    m_last_pip_exec_seq = m_exec_seq;
    m_last_pip_frame =
        perf::counter_value(perf::Counter::GuestFramesStarted);
    // Bounded PIP audit: log resident palette entries under the
    // existing diagnostic floor so cold-PIP (black world) vs loaded-PIP can be told
    // apart in offline replay, where GuestFramesStarted stays 0.
    {
        const char* e = std::getenv("M2_DRAW_DIAG");
        if (e && *e && *e != '0') {
            static uint32_t pip_logged = 0;
            static bool pip_expired = false;
            if (pip_logged >= 64u && !pip_expired) {
                pip_expired = true;
                std::fprintf(stderr,
                             "[pip-load] cap reached — further entries "
                             "suppressed\n");
            }
            if (pip_logged < 64u) {
                std::fprintf(stderr,
                             "[pip-load] n=%u exec=%llu frame=%llu "
                             "bytes=%u src=%#x "
                             "pip[0]=%08x pip[1]=%08x pip[2]=%08x pip[3]=%08x\n",
                             pip_logged,
                             (unsigned long long)m_exec_seq,
                             (unsigned long long)m_last_pip_frame,
                             byte_count, source_address,
                             entry_count > 0 ? m_pip[0] : 0u,
                             entry_count > 1 ? m_pip[1] : 0u,
                             entry_count > 2 ? m_pip[2] : 0u,
                             entry_count > 3 ? m_pip[3] : 0u);
                ++pip_logged;
                if (pip_logged == 64u && !pip_expired) {
                    pip_expired = true;
                    std::fprintf(stderr,
                                 "[pip-load] cap reached — further entries "
                                 "suppressed\n");
                }
            }
        }
    }
    return true;
}

bool GuestCltInterpreter::load_uncompressed_texture(
    const CltMemorySnapshot* memory)
{
    const uint32_t load_control = register_value(kTxtLdCntl);
    const uint32_t source_bit_offset = load_control & 7u;
    const uint32_t count_val = register_value(kTxtCount) & 0x00ffffffu;
    const uint32_t source_address = register_value(kTxtLdSrcAddr);
    const uint32_t destination_base = register_value(kTxtLodBase0);
    const uint32_t row_control = register_value(kTxtLdWidth);
    uint32_t destination_row_bits = (row_control >> 16u) & 0xffffu;
    uint32_t source_row_bits = row_control & 0xffffu;
    uint32_t row_count = count_val;
    uint32_t width = 0;
    const TextureFormat format = decode_format(register_value(kTxtExpType), 0);

    // A mode-0 load copies raw rows into TRAM; TXTEXPTYPE is applied when the
    // resident is used, not when it is loaded. The value readable here is
    // whatever the preceding use state left behind. The race's car-shadow
    // snippet (carshadow_initshadows) loads its 128x256 4-bit resident with
    // the previous draw's format still live and writes EXPTYPE=0x404 only
    // afterwards, so the load-time format is frequently not this texture's.
    //
    // The CLT toolkit's texel-expansion tables and M2TX_UncomprToM2TXRaw
    // define the legal space as:
    //   color: absent (constant) or literal 5/8 bits or indexed 1..8 bits
    //   alpha: absent (constant) or 4/7 bits
    //   SSB:   present or absent
    // Reserved encodings stay fail-closed instead of being guessed.
    bool format_legal = !format.transparent;
    if (format.has_alpha && format.alpha_depth != 0u &&
        format.alpha_depth != 4u && format.alpha_depth != 7u)
        format_legal = false;
    if (format.has_color) {
        if (format.literal) {
            if (format.color_depth != 0u && format.color_depth != 5u &&
                format.color_depth != 8u)
                format_legal = false;
        } else if (format.color_depth < 1u || format.color_depth > 8u) {
            format_legal = false;
        }
    }
    const bool has_alpha_bits = format.has_alpha && format.alpha_depth > 0u;
    const bool has_color_bits = format.has_color && format.color_depth > 0u;
    const bool indexed = !format.literal && has_color_bits;
    const uint32_t color_bits =
        has_color_bits
            ? (format.literal ? format.color_depth * 3u
                              : format.color_depth)
            : 0u;
    const uint32_t texel_bits =
        color_bits + (has_alpha_bits ? format.alpha_depth : 0u) +
        (format.has_ssb ? 1u : 0u);
    if (texel_bits == 0u || texel_bits > 32u) format_legal = false;

    // Copy the exact destination bits of each row, format-agnostically, the
    // way the TLD writes them into TRAM.
    const auto stage_raw_rows = [&](std::vector<uint8_t>* staged) {
        const uint64_t required_bits =
            source_bit_offset +
            static_cast<uint64_t>(row_count - 1u) * source_row_bits +
            destination_row_bits;
        const size_t required_bytes =
            static_cast<size_t>((required_bits + 7u) / 8u);
        const CltMemoryView source =
            find_memory(memory, source_address, required_bytes);
        if (!source.data || source.size < required_bytes) return false;

        const uint64_t staged_bits =
            static_cast<uint64_t>(destination_row_bits) * row_count;
        if (staged_bits == 0u ||
            staged_bits > static_cast<uint64_t>(kMaxTextureSnapshotBytes) * 8u)
            return false;
        staged->assign(static_cast<size_t>((staged_bits + 7u) / 8u), 0u);
        if (source_bit_offset == 0u && (destination_row_bits & 7u) == 0u &&
            (source_row_bits & 7u) == 0u) {
            const size_t copy_bytes = destination_row_bits / 8u;
            const size_t source_stride = source_row_bits / 8u;
            for (uint32_t row = 0; row < row_count; ++row)
                std::memcpy(staged->data() + row * copy_bytes,
                            source.data + row * source_stride, copy_bytes);
        } else {
            for (uint32_t row = 0; row < row_count; ++row) {
                const uint64_t source_row =
                    source_bit_offset +
                    static_cast<uint64_t>(row) * source_row_bits;
                const uint64_t destination_row =
                    static_cast<uint64_t>(row) * destination_row_bits;
                for (uint32_t bit = 0; bit < destination_row_bits; ++bit) {
                    const uint64_t src = source_row + bit;
                    const uint64_t dst = destination_row + bit;
                    const uint8_t value = static_cast<uint8_t>(
                        (source.data[src / 8u] >> (7u - (src & 7u))) & 1u);
                    (*staged)[dst / 8u] |=
                        static_cast<uint8_t>(value << (7u - (dst & 7u)));
                }
            }
        }
        return true;
    };

    const bool is_fullscreen_bitmap = (row_control == 0 && count_val > 0);
    if (is_fullscreen_bitmap) {
        if (!format_legal) return false;
        width = count_val;
        row_count = 432u;
        destination_row_bits = width * texel_bits;
        source_row_bits = width * texel_bits;
    } else {
        if (row_count == 0 || destination_row_bits == 0 ||
            source_row_bits == 0 ||
            source_row_bits < destination_row_bits)
            return false;
        if (!format_legal || destination_row_bits % texel_bits != 0) {
            // The live format cannot describe these rows at all, so it is
            // certainly a stale use state. Stage the raw rows until the
            // following use state supplies the resident's format.
            std::vector<uint8_t> staged;
            if (!stage_raw_rows(&staged)) return false;
            m_pending_mmdma_texture = true;
            m_pending_mmdma_source_bit_offset = 0u;
            m_pending_mmdma_destination_base = destination_base;
            m_pending_mmdma_bytes = std::move(staged);
            m_pending_mmdma_hash = blob_hash_bytes(
                m_pending_mmdma_bytes.data(), m_pending_mmdma_bytes.size());
            m_pending_linear_rows = true;
            m_pending_linear_row_bits = destination_row_bits;
            m_pending_linear_row_count = row_count;
            return true;
        }
        width = destination_row_bits / texel_bits;
    }

    if (width == 0 || width > 2048u || row_count > 2048u ||
        static_cast<uint64_t>(width) * row_count > kMaxTexturePixels)
        return false;

    const uint64_t required_bits =
        source_bit_offset +
        static_cast<uint64_t>(row_count - 1u) * source_row_bits +
        destination_row_bits;
    const size_t required_bytes = static_cast<size_t>((required_bits + 7u) / 8u);
    const CltMemoryView source =
        find_memory(memory, source_address, required_bytes);
    if (!source.data || source.size < required_bytes) return false;

    const bool config_trace_target =
        config_texture_trace_enabled() &&
        config_footer_texture_load(source_address, count_val);
    uint32_t config_source_hash = 0;
    size_t config_source_hash_bytes = 0;
    std::array<uint8_t, 16> config_source_sample{};
    uint8_t config_source_sample_size = 0;
    if (config_trace_target) {
        config_source_hash_bytes = std::min(required_bytes, source.size);
        config_source_hash = config_texture_hash_bytes(
            source.data, config_source_hash_bytes);
        config_texture_copy_sample(&config_source_sample,
                                   &config_source_sample_size, source.data,
                                   config_source_hash_bytes);
        static uint32_t decode_emitted = 0;
        static bool decode_cap_reported = false;
        if (config_texture_trace_admit("decode", &decode_emitted,
                                       &decode_cap_reported)) {
            std::fprintf(
                stderr,
                "[config-texture] stage=decode source=%#x source_span=%#zx "
                "available=%#zx hash_bytes=%#zx source_hash=%08x "
                "count=%#x ld_width=%#x ld_cntl=%#x exp=%#x "
                "fmt=(c%u,a%u,ssb%d,color%d,alpha%d,lit%d) width=%u "
                "height=%u output_bits=%u\n",
                source_address, required_bytes, source.size,
                config_source_hash_bytes, config_source_hash, count_val,
                row_control, load_control, register_value(kTxtExpType),
                format.color_depth, format.alpha_depth,
                format.has_ssb ? 1 : 0, format.has_color ? 1 : 0,
                format.has_alpha ? 1 : 0, format.literal ? 1 : 0, width,
                row_count, texel_bits);
        }
    }

    const uint32_t pip_control_value = register_value(kTxtPipCntl);
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return false;
    const uint32_t pip_offset = pip_control.index_offset;
    const PipColor constant0 = decode_pip(register_value(kTxtConst0));
    const PipColor constant1 = decode_pip(register_value(kTxtConst1));

    LoadDecodeKey decode_key;
    decode_key.source_hash = source.content_hash != 0
                                 ? source.content_hash
                                 : blob_hash_bytes(source.data, required_bytes);
    decode_key.source_address = source_address;
    decode_key.source_span = required_bytes;
    decode_key.load_control = load_control;
    decode_key.count = count_val;
    decode_key.load_width = row_control;
    decode_key.exp_type = register_value(kTxtExpType);
    decode_key.pip_control = pip_control_value;
    for (unsigned i = 0; i < 4u; ++i)
        decode_key.constants[i] =
            register_value(static_cast<uint16_t>(kTxtConst0 + i * 4u));
    decode_key.pip_palette_hash = m_pip_hash;

    std::vector<uint32_t> pixels;
    std::vector<uint8_t> ssb_bits;
    std::vector<uint8_t> palette_indices;
    std::vector<uint8_t> palette_alpha;
    std::vector<uint8_t> palette_source_ssb;
    const MmdmaDecodeValue* cached_decode =
        find_cached_load_decode(decode_key);
    const std::vector<uint32_t>* cached_pixels = nullptr;
    uint64_t decoded_pixels_hash = 0;
    uint64_t source_sidecar_hash = 0;
    if (cached_decode) {
        cached_pixels = &cached_decode->pixels;
        decoded_pixels_hash = cached_decode->pixel_hash;
        ssb_bits = cached_decode->ssb_bits;
        palette_indices = cached_decode->palette_indices;
        palette_alpha = cached_decode->palette_alpha;
        palette_source_ssb = cached_decode->palette_source_ssb;
        source_sidecar_hash = cached_decode->sidecar_hash;
    } else {
    pixels.resize(static_cast<size_t>(width) * row_count);
    ssb_bits.resize(static_cast<size_t>(width) * row_count);
    if (indexed) {
        palette_indices.reserve(pixels.size());
        palette_source_ssb.reserve(pixels.size());
        if (format.has_alpha) palette_alpha.reserve(pixels.size());
    }

    // The exercised RGB555+SSB layout stays on the word fast path; every
    // other legal full-load format decodes MSB-first per texel, in the
    // toolkit's SSB -> alpha -> color channel order.
    const bool fast_rgb555_ssb =
        format.literal && format.has_color && format.color_depth == 5u &&
        format.has_ssb && !has_alpha_bits;
    if (fast_rgb555_ssb && source_bit_offset == 0 &&
        (source_row_bits % 16u) == 0 &&
        destination_row_bits == width * 16u) {
        const size_t src_stride_bytes = source_row_bits / 8u;
        size_t dst_idx = 0;
        for (uint32_t y = 0; y < row_count; ++y) {
            const uint8_t* row_ptr = source.data + y * src_stride_bytes;
            for (uint32_t x = 0; x < width; ++x) {
                const uint16_t word = (static_cast<uint16_t>(row_ptr[x * 2]) << 8) |
                                      row_ptr[x * 2 + 1];
                const uint32_t ssb = (word >> 15u) & 1u;
                const uint32_t red = (word >> 10u) & 0x1fu;
                const uint32_t green = (word >> 5u) & 0x1fu;
                const uint32_t blue = word & 0x1fu;
                const uint32_t r = (red << 3u) | (red >> 2u);
                const uint32_t g = (green << 3u) | (green >> 2u);
                const uint32_t b = (blue << 3u) | (blue >> 2u);
                const uint32_t a = (is_fullscreen_bitmap || !format.has_ssb)
                                       ? 0xffu
                                       : (ssb != 0 ? 0xffu : 0u);
                pixels[dst_idx] = r | (g << 8u) | (b << 16u) | (a << 24u);
                ssb_bits[dst_idx] = static_cast<uint8_t>(ssb != 0);
                dst_idx++;
            }
        }
    } else {
        BitReader reader(source.data, source.size);
        if (!reader.skip(source_bit_offset)) return false;
        size_t dst_idx = 0;
        for (uint32_t y = 0; y < row_count; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t ssb = 0;
                if (format.has_ssb && !reader.read(1u, &ssb)) return false;
                uint32_t alpha = 0;
                if (has_alpha_bits) {
                    uint32_t encoded = 0;
                    if (!reader.read(format.alpha_depth, &encoded))
                        return false;
                    alpha = replicate_msb(format.alpha_depth, encoded);
                }
                uint32_t red = 0;
                uint32_t green = 0;
                uint32_t blue = 0;
                uint32_t source_index = 0;
                if (has_color_bits) {
                    if (format.literal) {
                        uint32_t encoded = 0;
                        if (!reader.read(format.color_depth, &encoded))
                            return false;
                        red = replicate_msb(format.color_depth, encoded);
                        if (!reader.read(format.color_depth, &encoded))
                            return false;
                        green = replicate_msb(format.color_depth, encoded);
                        if (!reader.read(format.color_depth, &encoded))
                            return false;
                        blue = replicate_msb(format.color_depth, encoded);
                    } else if (!reader.read(format.color_depth,
                                            &source_index)) {
                        return false;
                    }
                }
                if (indexed) {
                    const PipColor& constant =
                        pip_constant_for_ssb(ssb, constant0, constant1);
                    const uint32_t texture_alpha =
                        has_alpha_bits ? alpha
                                       : (constant.rgba >> 24u) & 0xffu;
                    uint32_t rgba = 0;
                    uint8_t selected_ssb = 0;
                    if (!materialize_indexed_texel(
                            m_pip, pip_control_value, constant0, constant1,
                            source_index, texture_alpha, ssb, &rgba,
                            &selected_ssb))
                        return false;
                    pixels[dst_idx] = rgba;
                    ssb_bits[dst_idx] = selected_ssb;
                    palette_indices.push_back(
                        static_cast<uint8_t>(source_index));
                    palette_source_ssb.push_back(
                        static_cast<uint8_t>(ssb != 0u));
                    if (has_alpha_bits)
                        palette_alpha.push_back(
                            static_cast<uint8_t>(alpha));
                } else {
                    if (!has_color_bits) {
                        const PipColor& constant =
                            pip_constant_for_ssb(ssb, constant0, constant1);
                        red = constant.rgba & 0xffu;
                        green = (constant.rgba >> 8u) & 0xffu;
                        blue = (constant.rgba >> 16u) & 0xffu;
                    }
                    if (!has_alpha_bits) {
                        // A literal RGB555 TRAM format without alpha keeps
                        // the established SSB-as-transparency behavior; a
                        // fullscreen backdrop stays opaque.
                        alpha = (is_fullscreen_bitmap || !format.has_ssb)
                                    ? 0xffu
                                    : (ssb != 0u ? 0xffu : 0u);
                    }
                    pixels[dst_idx] = red | (green << 8u) | (blue << 16u) |
                                      (alpha << 24u);
                    ssb_bits[dst_idx] = static_cast<uint8_t>(ssb != 0u);
                }
                ++dst_idx;
            }
            if (y + 1u < row_count &&
                !reader.skip(static_cast<uint64_t>(source_row_bits) -
                             destination_row_bits))
                return false;
        }
    }

    MmdmaDecodeValue decode_value;
    decode_value.pixels = pixels;
    decode_value.pixel_hash = rgba_content_hash(pixels);
    decoded_pixels_hash = decode_value.pixel_hash;
    decode_value.ssb_bits = ssb_bits;
    decode_value.palette_indices = palette_indices;
    decode_value.palette_alpha = palette_alpha;
    decode_value.palette_source_ssb = palette_source_ssb;
    if (indexed) {
        decode_value.sidecar_hash = mmdma_sidecar_hash(
            decode_value.palette_indices, decode_value.palette_alpha,
            decode_value.palette_source_ssb);
        source_sidecar_hash = decode_value.sidecar_hash;
    }
    store_cached_load_decode(decode_key, std::move(decode_value));
    }

    const std::vector<uint32_t>& decoded_pixels =
        cached_pixels ? *cached_pixels : pixels;
    if (decoded_pixels_hash == 0)
        decoded_pixels_hash = rgba_content_hash(decoded_pixels);

    CelId cel = kInvalidCel;
    dump_tex_pixels(static_cast<int>(width), static_cast<int>(row_count),
                    decoded_pixels.data(), "uncomp",
                    register_value(kTxtExpType));
    {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        if (m_backend)
            cel = m_backend->cel_upload_rgba_ex_hashed(
                static_cast<int>(width), static_cast<int>(row_count),
                decoded_pixels.data(), false, false, decoded_pixels_hash);
    }
    if (m_backend && cel == kInvalidCel) return false;
    if (bridge_trace_enabled()) {
        std::fprintf(stderr,
                     "[clt-dbg] load_uncompressed_texture uploaded %u pixels, cel=%d: "
                     "p[0]=%08x p[1]=%08x p[2]=%08x p[3]=%08x p[4]=%08x\n",
                     (unsigned)decoded_pixels.size(), (int)cel,
                     decoded_pixels.size() > 0 ? decoded_pixels[0] : 0,
                     decoded_pixels.size() > 1 ? decoded_pixels[1] : 0,
                     decoded_pixels.size() > 2 ? decoded_pixels[2] : 0,
                     decoded_pixels.size() > 3 ? decoded_pixels[3] : 0,
                     decoded_pixels.size() > 4 ? decoded_pixels[4] : 0);
    }
    const uint32_t tram_size = static_cast<uint32_t>(
        (static_cast<uint64_t>(width) * row_count * texel_bits + 7u) / 8u);
    const uint32_t config_decoded_hash =
        config_trace_target ? config_texture_hash_rgba(decoded_pixels) : 0u;
    if (config_trace_target) {
        static uint32_t decoded_emitted = 0;
        static bool decoded_cap_reported = false;
        if (config_texture_trace_admit("decoded", &decoded_emitted,
                                       &decoded_cap_reported)) {
            std::fprintf(stderr,
                         "[config-texture] stage=decoded source=%#x "
                         "decoded_rgba_hash=%08x pixels=%zu width=%u "
                         "height=%u materialized=1\n",
                         source_address, config_decoded_hash,
                         decoded_pixels.size(),
                         width, row_count);
        }
    }
    publish_texture(cel, destination_base, tram_size, static_cast<int>(width),
                    static_cast<int>(row_count), std::move(ssb_bits),
                    kTexturePublisherFullLoad);
    // The eager decode assumes the load-time TXTEXPTYPE is this resident's
    // use format. Retain the raw TRAM rows so a draw under a different
    // format re-decodes them (serve_for_draw) instead of sampling the stale
    // decode.
    if (!is_fullscreen_bitmap) {
        std::vector<uint8_t> raw_rows;
        if (stage_raw_rows(&raw_rows)) {
            m_texture.staged_valid = true;
            m_texture.staged_format_only = true;
            m_texture.staged_serial = ++m_texture_serial;
            m_texture.staged_blob_base = destination_base;
            m_texture.staged_decode_base = destination_base;
            m_texture.staged_source_bit_offset = 0u;
            m_texture.staged_blob_hash =
                blob_hash_bytes(raw_rows.data(), raw_rows.size());
            m_texture.staged_exp_type = register_value(kTxtExpType);
            m_texture.staged_linear_rows = true;
            m_texture.staged_linear_row_bits = destination_row_bits;
            m_texture.staged_linear_row_count = row_count;
            m_texture.staged_bytes = std::move(raw_rows);
        }
    }
    if (config_trace_target) {
        m_texture.source_address = source_address;
        m_texture.source_span = static_cast<uint32_t>(std::min<size_t>(
            required_bytes, static_cast<size_t>(UINT32_MAX)));
        m_texture.source_available = static_cast<uint32_t>(std::min<size_t>(
            source.size, static_cast<size_t>(UINT32_MAX)));
        m_texture.source_checksum = config_source_hash;
        m_texture.source_sample = config_source_sample;
        m_texture.source_sample_size = config_source_sample_size;
        m_texture.decoded_checksum = config_decoded_hash;
        m_texture.decoded_checksum_valid = true;
    }
    if (indexed) {
        // The TRAM destination is indexed; keep the source vectors so a
        // later TXTPIPCNTL selector/offset or PIP generation change rebinds
        // the presented cel instead of freezing the load-time palette.
        m_texture.mmdma_indexed = true;
        m_texture.mmdma_exp_type = register_value(kTxtExpType);
        m_texture.mmdma_pip_control = pip_control_value;
        m_texture.mmdma_pip_offset = pip_offset;
        m_texture.mmdma_pip_generation = m_pip_generation;
        m_texture.mmdma_indices = std::move(palette_indices);
        m_texture.mmdma_alpha = std::move(palette_alpha);
        m_texture.mmdma_source_ssb = std::move(palette_source_ssb);
        m_texture.mmdma_constant0 = register_value(kTxtConst0);
        m_texture.mmdma_constant1 = register_value(kTxtConst1);
        m_texture.mmdma_pip_hash = m_pip_hash;
        m_texture.mmdma_source_hash = source_sidecar_hash != 0
                                          ? source_sidecar_hash
                                          : mmdma_sidecar_hash(
                                                m_texture.mmdma_indices,
                                                m_texture.mmdma_alpha,
                                                m_texture.mmdma_source_ssb);
    }
    return true;
}

bool GuestCltInterpreter::load_compressed_texture(
    const CltMemorySnapshot* memory, const char** fail_reason)
{
    const auto fail = [&](const char* reason) {
        if (fail_reason) *fail_reason = reason;
        return false;
    };
    const uint32_t texel_count = register_value(kTxtCount) & 0x00ffffffu;
    const uint32_t source_address = register_value(kTxtLdSrcAddr);
    const uint32_t destination_base = register_value(kTxtLodBase0);
    const uint32_t row_control = register_value(kTxtLdWidth);
    const uint32_t destination_row_bits = (row_control >> 16u) & 0xffffu;
    const TextureFormat expanded =
        decode_format(register_value(kTxtExpType), 0);
    const unsigned output_color_bits =
        expanded.literal ? expanded.color_depth * 3u : expanded.color_depth;
    const unsigned output_bits =
        output_color_bits +
        expanded.alpha_depth +
        (expanded.has_ssb ? 1u : 0u);

    // CLT_CreateTxLoadCommandListTexLoad() sizes the expanded destination as
    // CDEPTH*3 for literal color and CDEPTH for indexed color, followed by
    // ADEPTH and the optional SSB bit.  The disc's driving assets currently
    // exercise indexed output, but the source-format decoder is still part of
    // the guest-visible CLT contract and must not narrow valid DCI forms just
    // because an authentic route has not selected them yet.
    if (texel_count == 0 || destination_row_bits == 0 || output_bits == 0)
        return fail("shape");
    if (expanded.transparent || !expanded.has_color ||
        expanded.color_depth > 8u ||
        (!expanded.literal && expanded.color_depth < 1u) ||
        expanded.alpha_depth > 8u ||
        destination_row_bits % output_bits != 0)
        return fail("format");

    const uint32_t width = destination_row_bits / output_bits;
    if (width == 0 || texel_count % width != 0) return fail("width-mismatch");
    const uint32_t height = texel_count / width;
    if (width > 2048u || height > 2048u ||
        static_cast<uint64_t>(width) * height > kMaxTexturePixels)
        return fail("dims");

    const uint64_t estimated_source =
        std::min<uint64_t>(static_cast<uint64_t>(texel_count) * 2u + 4096u,
                           kMaxTextureSnapshotBytes);
    CltMemoryView source =
        find_memory(memory, source_address,
                    static_cast<size_t>(estimated_source));
    if (!source.data) {
        // A shorter captured range can still be complete for a highly
        // compressible texture, so use it when it starts at the right address.
        source = find_memory(memory, source_address, 1u);
    }
    if (!source.data) return fail("src-missing");

    const bool config_trace_target =
        config_texture_trace_enabled() &&
        config_footer_texture_load(source_address, texel_count);
    uint32_t config_source_hash = 0;
    size_t config_source_hash_bytes = 0;
    std::array<uint8_t, 16> config_source_sample{};
    uint8_t config_source_sample_size = 0;
    if (config_trace_target) {
        config_source_hash_bytes = std::min(
            static_cast<size_t>(estimated_source), source.size);
        config_source_hash = config_texture_hash_bytes(
            source.data, config_source_hash_bytes);
        config_texture_copy_sample(&config_source_sample,
                                   &config_source_sample_size, source.data,
                                   config_source_hash_bytes);
        static uint32_t decode_emitted = 0;
        static bool decode_cap_reported = false;
        if (config_texture_trace_admit("decode", &decode_emitted,
                                       &decode_cap_reported)) {
            std::fprintf(
                stderr,
                "[config-texture] stage=decode source=%#x source_span=%#llx "
                "available=%#zx hash_bytes=%#zx source_hash=%08x "
                "count=%#x ld_width=%#x ld_cntl=%#x exp=%#x "
                "fmt=(c%u,a%u,ssb%d,color%d,alpha%d,lit%d) width=%u "
                "height=%u output_bits=%u\n",
                source_address,
                static_cast<unsigned long long>(estimated_source),
                source.size, config_source_hash_bytes, config_source_hash,
                texel_count, row_control, register_value(kTxtLdCntl),
                register_value(kTxtExpType), expanded.color_depth,
                expanded.alpha_depth, expanded.has_ssb ? 1 : 0,
                expanded.has_color ? 1 : 0, expanded.has_alpha ? 1 : 0,
                expanded.literal ? 1 : 0, width, height, output_bits);
        }
    }

    std::array<TextureFormat, 4> source_formats;
    const uint32_t source01 = register_value(kTxtSrcType01);
    const uint32_t source23 = register_value(kTxtSrcType23);
    for (unsigned i = 0; i < 4u; ++i)
        source_formats[i] = decode_format(i < 2u ? source01 : source23,
                                          i & 1u);

    for (const TextureFormat& source : source_formats) {
        // M2TX_ComprToM2TXRaw ignores the color/literal/depth fields for a
        // transparent control and uses the source constant instead.  The
        // compressor can therefore retain alpha/SSB metadata when it turns
        // a format into a transparent one.  Likewise, a non-transparent
        // source with no stored color uses its constant color; do not reject
        // either source form merely because a field is unused by the decoder.
        if (source.color_depth > 8u || source.alpha_depth > 8u)
            return fail("src-format");
    }

    const uint32_t pip_control_value = register_value(kTxtPipCntl);
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return fail("pip-control");
    const uint32_t pip_offset = pip_control.index_offset;
    // A non-literal compressed destination is an indexed TRAM resident. The
    // DCI loader stores the source index (M2TX_ComprToM2TXIndex), while the
    // later use state applies TXTPIPCNTL against the resident PIP. Keep the
    // same source vectors as the MMDMA path so a changed use-state selector or
    // palette is rebound at draw time instead of being frozen at TLD time.
    const bool palette_backed_output = is_indexed_tram_format(expanded);
    // TXTPIPCNTL is use/draw state, not part of a compressed texture load.
    // A load can therefore arrive while the preceding draw's selector state
    // is still live.  Retain the indexed TRAM data even when that stale state
    // cannot materialize it; texture_for_draw() will bind it once the guest
    // supplies the texture's actual use state.  Reserved selectors and the
    // toolkit-forbidden indexed TEXTURE-color combination still fail closed
    // if they remain active at draw time.
    const bool indexed_use_state_valid =
        !palette_backed_output ||
        (pip_control.ssb_select <= kPipSelectPip &&
         pip_control.alpha_select <= kPipSelectPip &&
         pip_control.color_select <= kPipSelectPip &&
         pip_control.color_select != kPipSelectTexture);
    std::vector<uint32_t> pixels;
    std::vector<uint8_t> ssb_bits;
    std::vector<uint8_t> palette_indices;
    std::vector<uint8_t> palette_alpha;
    std::vector<uint8_t> palette_source_ssb;
    pixels.reserve(static_cast<size_t>(texel_count));
    ssb_bits.reserve(static_cast<size_t>(texel_count));
    if (palette_backed_output) {
        palette_indices.reserve(static_cast<size_t>(texel_count));
        palette_source_ssb.reserve(static_cast<size_t>(texel_count));
        if (expanded.has_alpha)
            palette_alpha.reserve(static_cast<size_t>(texel_count));
    }
    const PipColor pipeline_constant0 =
        decode_pip(register_value(kTxtConst0));
    const PipColor pipeline_constant1 =
        decode_pip(register_value(kTxtConst1));
    LoadDecodeKey decode_key;
    const size_t decode_hash_bytes = std::min<size_t>(
        source.size, static_cast<size_t>(estimated_source));
    decode_key.source_hash = source.content_hash != 0 &&
                                     source.size == decode_hash_bytes
                                 ? source.content_hash
                                 : blob_hash_bytes(source.data,
                                                   decode_hash_bytes);
    decode_key.source_address = source_address;
    decode_key.source_span = decode_hash_bytes;
    decode_key.load_control = register_value(kTxtLdCntl);
    decode_key.count = texel_count;
    decode_key.load_width = row_control;
    decode_key.exp_type = register_value(kTxtExpType);
    decode_key.src_type01 = source01;
    decode_key.src_type23 = source23;
    decode_key.pip_control = pip_control_value;
    for (unsigned i = 0; i < 4u; ++i)
        decode_key.constants[i] =
            register_value(static_cast<uint16_t>(kTxtConst0 + i * 4u));
    decode_key.pip_palette_hash = m_pip_hash;
    const MmdmaDecodeValue* cached_decode =
        find_cached_load_decode(decode_key);
    const std::vector<uint32_t>* cached_pixels = nullptr;
    uint64_t decoded_pixels_hash = 0;
    uint64_t source_sidecar_hash = 0;
    if (cached_decode) {
        cached_pixels = &cached_decode->pixels;
        decoded_pixels_hash = cached_decode->pixel_hash;
        ssb_bits = cached_decode->ssb_bits;
        palette_indices = cached_decode->palette_indices;
        palette_alpha = cached_decode->palette_alpha;
        palette_source_ssb = cached_decode->palette_source_ssb;
        source_sidecar_hash = cached_decode->sidecar_hash;
    } else {
    BitReader reader(source.data, source.size);
    while (pixels.size() < texel_count) {
        uint32_t control = 0;
        if (!reader.read(8u, &control)) return fail("bitstream");
        const unsigned type = (control >> 6u) & 3u;
        const TextureFormat& source = source_formats[type];
        const PipColor constant = decode_pip(
            register_value(static_cast<uint16_t>(kTxtConst0 + type * 4u)));
        if (source.transparent) {
            uint32_t count = (control & 0x3fu) + 1u;
            count = std::min<uint32_t>(count,
                                       texel_count - pixels.size());
            if (palette_backed_output) {
                if (source.literal) return fail("indexed-src-literal");
                const uint32_t source_index =
                    (constant.rgba >> 16u) & 0xffu;
                const uint32_t source_alpha =
                    (constant.rgba >> 24u) & 0xffu;
                const uint32_t source_ssb =
                    expanded.has_ssb ? constant.ssb : 0u;
                uint32_t rgba = 0;
                uint8_t ssb = 0;
                if (indexed_use_state_valid &&
                    !materialize_indexed_texel(
                        m_pip, pip_control_value, pipeline_constant0,
                        pipeline_constant1, source_index, source_alpha,
                        source_ssb, &rgba, &ssb))
                    return fail("pip-select");
                pixels.insert(pixels.end(), count, rgba);
                ssb_bits.insert(ssb_bits.end(), count, ssb);
                palette_indices.insert(
                    palette_indices.end(), count,
                    static_cast<uint8_t>(source_index));
                palette_source_ssb.insert(
                    palette_source_ssb.end(), count,
                    static_cast<uint8_t>(source_ssb != 0u));
                if (expanded.has_alpha)
                    palette_alpha.insert(palette_alpha.end(), count,
                                         static_cast<uint8_t>(source_alpha));
                continue;
            }
            // A transparent DCI control names its PIP entry through the blue
            // byte of the source constant.  The source SDK's decompressor
            // uses the PIP color for channels not stored by the expansion
            // format; stored alpha/SSB remain the source constant.  This
            // preserves the observed indexed-output path while also keeping
            // literal/alpha/SSB destinations source-faithful.
            const PipColor pip =
                decode_pip(m_pip[(constant.rgba >> 16u) & 0xffu]);
            const uint32_t red = pip.rgba & 0xffu;
            const uint32_t green = (pip.rgba >> 8u) & 0xffu;
            const uint32_t blue = (pip.rgba >> 16u) & 0xffu;
            const uint32_t alpha = expanded.has_alpha
                                       ? (constant.rgba >> 24u) & 0xffu
                                       : (pip.rgba >> 24u) & 0xffu;
            const uint8_t ssb = expanded.has_ssb ? constant.ssb : pip.ssb;
            const uint32_t rgba = red | (green << 8u) | (blue << 16u) |
                                  (alpha << 24u);
            pixels.insert(pixels.end(), count, rgba);
            ssb_bits.insert(ssb_bits.end(), count, ssb);
            continue;
        }
        uint32_t count = (control & 0x1fu) + 1u;
        const bool string_storage = (control & 0x20u) != 0;
        // TXTCOUNT is the destination limit. The final compressed run may
        // describe more texels than remain in that destination, so consume
        // only the bounded suffix that the loader can store.
        count = std::min<uint32_t>(count,
                                   texel_count - pixels.size());

        for (uint32_t i = 0; i < (string_storage ? count : 1u); ++i) {
            uint32_t ssb = constant.ssb;
            uint32_t alpha = (constant.rgba >> 24u) & 0xffu;
            uint32_t red = constant.rgba & 0xffu;
            uint32_t green = (constant.rgba >> 8u) & 0xffu;
            uint32_t blue = (constant.rgba >> 16u) & 0xffu;
            if (source.has_ssb && !reader.read(1u, &ssb))
                return fail("bitstream");
            if (source.has_alpha && source.alpha_depth > 0u) {
                uint32_t encoded_alpha = 0;
                if (!reader.read(source.alpha_depth, &encoded_alpha))
                    return fail("bitstream");
                alpha = replicate_msb(source.alpha_depth, encoded_alpha);
            }
            uint32_t source_index = 0;
            if (source.has_color && source.color_depth > 0u) {
                if (source.literal) {
                    if (palette_backed_output)
                        return fail("indexed-src-literal");
                    uint32_t encoded = 0;
                    if (!reader.read(source.color_depth, &encoded))
                        return fail("bitstream");
                    red = replicate_msb(source.color_depth, encoded);
                    if (!reader.read(source.color_depth, &encoded))
                        return fail("bitstream");
                    green = replicate_msb(source.color_depth, encoded);
                    if (!reader.read(source.color_depth, &encoded))
                        return fail("bitstream");
                    blue = replicate_msb(source.color_depth, encoded);
                } else {
                    if (!reader.read(source.color_depth, &source_index))
                        return fail("bitstream");
                    if (!palette_backed_output) {
                        const PipColor pip = decode_pip(
                            m_pip[pip_lookup_index(source_index, pip_offset)]);
                        red = pip.rgba & 0xffu;
                        green = (pip.rgba >> 8u) & 0xffu;
                        blue = (pip.rgba >> 16u) & 0xffu;
                        if (!expanded.has_alpha && !source.has_alpha)
                            alpha = (pip.rgba >> 24u) & 0xffu;
                        if (!expanded.has_ssb && !source.has_ssb)
                            ssb = pip.ssb;
                    }
                }
            }
            uint32_t texture_alpha = 0;
            uint32_t texture_ssb = 0;
            if (palette_backed_output) {
                if (source.literal)
                    return fail("indexed-src-literal");
                // M2TX_ComprToM2TXIndex uses opaque/zero defaults when an
                // indexed source omits alpha/SSB, while transparent controls
                // retain their source constants. The expanded destination is
                // the authority for which sidecars survive in TRAM.
                texture_alpha =
                    (expanded.has_alpha && source.has_color &&
                     !source.has_alpha)
                        ? 0xffu
                        : ((expanded.has_alpha || !source.has_color)
                               ? alpha
                               : (constant.rgba >> 24u) & 0xffu);
                texture_ssb =
                    expanded.has_ssb
                        ? ((source.has_color && !source.has_ssb) ? 0u : ssb)
                        : 0u;
                uint32_t selected_rgba = 0;
                uint8_t selected_ssb = 0;
                if (indexed_use_state_valid &&
                    !materialize_indexed_texel(
                        m_pip, pip_control_value, pipeline_constant0,
                        pipeline_constant1, source_index, texture_alpha,
                        texture_ssb, &selected_rgba, &selected_ssb))
                    return fail("pip-select");
                red = selected_rgba & 0xffu;
                green = (selected_rgba >> 8u) & 0xffu;
                blue = (selected_rgba >> 16u) & 0xffu;
                alpha = (selected_rgba >> 24u) & 0xffu;
                ssb = selected_ssb;
            }
            const uint32_t rgba = red | (green << 8u) | (blue << 16u) |
                                  (alpha << 24u);
            const uint32_t repeat = string_storage ? 1u : count;
            for (uint32_t j = 0; j < repeat; ++j) {
                pixels.push_back(rgba);
                ssb_bits.push_back(static_cast<uint8_t>(ssb != 0u));
                if (palette_backed_output) {
                    palette_indices.push_back(
                        static_cast<uint8_t>(source_index));
                    palette_source_ssb.push_back(
                        static_cast<uint8_t>(
                            (expanded.has_ssb ?
                                 ((source.has_color && !source.has_ssb)
                                      ? 0u
                                      : ssb)
                                 : 0u) != 0u));
                    if (expanded.has_alpha)
                        palette_alpha.push_back(
                            static_cast<uint8_t>(texture_alpha));
                }
            }
        }
    }

    MmdmaDecodeValue decode_value;
    decode_value.pixels = pixels;
    decode_value.pixel_hash = rgba_content_hash(pixels);
    decoded_pixels_hash = decode_value.pixel_hash;
    decode_value.ssb_bits = ssb_bits;
    decode_value.palette_indices = palette_indices;
    decode_value.palette_alpha = palette_alpha;
    decode_value.palette_source_ssb = palette_source_ssb;
    if (palette_backed_output) {
        decode_value.sidecar_hash = mmdma_sidecar_hash(
            decode_value.palette_indices, decode_value.palette_alpha,
            decode_value.palette_source_ssb);
        source_sidecar_hash = decode_value.sidecar_hash;
    }
    store_cached_load_decode(decode_key, std::move(decode_value));
    }

    const std::vector<uint32_t>& decoded_pixels =
        cached_pixels ? *cached_pixels : pixels;
    if (decoded_pixels_hash == 0)
        decoded_pixels_hash = rgba_content_hash(decoded_pixels);

    CelId cel = kInvalidCel;
    dump_tex_pixels(static_cast<int>(width), static_cast<int>(height),
                    decoded_pixels.data(), "comp",
                    register_value(kTxtExpType));
    if (!palette_backed_output || indexed_use_state_valid) {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        if (m_backend)
            cel = m_backend->cel_upload_rgba_ex_hashed(
                static_cast<int>(width), static_cast<int>(height),
                decoded_pixels.data(), false, false, decoded_pixels_hash);
    }
    if (m_backend && (!palette_backed_output || indexed_use_state_valid) &&
        cel == kInvalidCel)
        return fail("cel");
    const uint32_t config_decoded_hash =
        config_trace_target ? config_texture_hash_rgba(decoded_pixels) : 0u;
    if (config_trace_target) {
        static uint32_t decoded_emitted = 0;
        static bool decoded_cap_reported = false;
        if (config_texture_trace_admit("decoded", &decoded_emitted,
                                       &decoded_cap_reported)) {
            std::fprintf(stderr,
                         "[config-texture] stage=decoded source=%#x "
                         "decoded_rgba_hash=%08x pixels=%zu width=%u "
                         "height=%u materialized=%d\n",
                         source_address, config_decoded_hash,
                         decoded_pixels.size(),
                         width, height,
                         (!palette_backed_output || indexed_use_state_valid)
                             ? 1
                             : 0);
        }
    }
    // The expanded destination occupies output_bits per texel in TRAM,
    // matching the guest's own texelDataSize accounting for compressed loads.
    const uint32_t tram_size =
        static_cast<uint32_t>((texel_count * output_bits + 7u) / 8u);
    publish_texture(cel, destination_base, tram_size, static_cast<int>(width),
                    static_cast<int>(height), std::move(ssb_bits),
                    kTexturePublisherCompressed);
    if (config_trace_target) {
        m_texture.source_address = source_address;
        m_texture.source_span = static_cast<uint32_t>(std::min<uint64_t>(
            estimated_source, static_cast<uint64_t>(UINT32_MAX)));
        m_texture.source_available = static_cast<uint32_t>(std::min<size_t>(
            source.size, static_cast<size_t>(UINT32_MAX)));
        m_texture.source_checksum = config_source_hash;
        m_texture.source_sample = config_source_sample;
        m_texture.source_sample_size = config_source_sample_size;
        m_texture.decoded_checksum = config_decoded_hash;
        m_texture.decoded_checksum_valid =
            !palette_backed_output || indexed_use_state_valid;
    }
    if (palette_backed_output) {
        m_texture.mmdma_indexed = true;
        m_texture.mmdma_exp_type = register_value(kTxtExpType);
        m_texture.mmdma_pip_control =
            indexed_use_state_valid ? pip_control_value : UINT32_MAX;
        m_texture.mmdma_pip_offset = pip_offset;
        m_texture.mmdma_pip_generation = m_pip_generation;
        m_texture.mmdma_indices = std::move(palette_indices);
        m_texture.mmdma_alpha = std::move(palette_alpha);
        m_texture.mmdma_source_ssb = std::move(palette_source_ssb);
        m_texture.mmdma_constant0 = register_value(kTxtConst0);
        m_texture.mmdma_constant1 = register_value(kTxtConst1);
        m_texture.mmdma_pip_hash = m_pip_hash;
        m_texture.mmdma_source_hash = source_sidecar_hash != 0
                                          ? source_sidecar_hash
                                          : mmdma_sidecar_hash(
                                                m_texture.mmdma_indices,
                                                m_texture.mmdma_alpha,
                                                m_texture.mmdma_source_ssb);
    }
    return true;
}

bool GuestCltInterpreter::load_mmdma_texture(
    const CltMemorySnapshot* memory, const char** fail_reason)
{
    const auto fail = [&](const char* reason) {
        if (fail_reason) *fail_reason = reason;
        return false;
    };
    const uint32_t byte_count = register_value(kTxtCount) & 0x00ffffffu;
    const uint32_t source_address = register_value(kTxtLdSrcAddr);
    const uint32_t source_bit_offset = register_value(kTxtLdCntl) & 7u;
    static const bool room_diag = [] {
        const char* e = std::getenv("M2_CLT_ROOM_MMDMA_DIAG");
        return e && *e && *e != '0';
    }();

    // Staging is format-agnostic (raw memcpy): the TXTEXPTYPE value readable
    // at TLD time is the previous texture's use-state, not this load's
    // format. World-pack lists write only SRC/COUNT/LOD0 fresh per load and
    // never write 0x402, yet their TLDs observe 0x402; the real use-state
    // (0x404/0x406-class) arrives at the following TXTEXPTYPE write, where
    // materialize_mmdma_texture validates it before any draw can observe the
    // staged bytes (pending bytes are consumed only by materialization; the
    // draw path publishes via texture_for_draw). Genuinely unknown use-state
    // labels still fail there, so the fail-closed contract is unchanged —
    // only the attribution site moves from load time to use time.
    if (byte_count == 0 || source_bit_offset > 7u) return fail("shape");

    // MMDMA reads VRAM, not a second stale system-memory copy. Rotate is the
    // one transition that relies on this distinction: it reloads the prior
    // display as thirty-six 576x12 RGB555+SSB strips, while the host's live
    // pixels reside in the addressed renderer target. The enqueue snapshot
    // can still contain an old MPEG frame at these guest addresses.
    //
    // `m_fb_spans` contains only the bridge-authenticated 576x432 display
    // carves. A load must be wholly contained, so MPEG bitmaps and ordinary
    // guest textures retain the existing immutable guest-memory path.
    CltMemoryView source;
    constexpr uint32_t kDisplayBytes = 576u * 432u * 2u;
    const bool display_rgb555_strip =
        byte_count == 576u * 12u * 2u && source_bit_offset == 0u &&
        is_rgb555_ssb_format(
            decode_format(register_value(kTxtExpType), 0u));
    for (const CltFbSpan& span : m_fb_spans) {
        if (!display_rgb555_strip) break;
        const uint64_t source_end =
            static_cast<uint64_t>(source_address) + byte_count;
        const uint64_t span_end = static_cast<uint64_t>(span.ea) + span.size;
        if (span.size != kDisplayBytes || source_address < span.ea ||
            source_end > span_end)
            continue;
        const int64_t window_rel = static_cast<int64_t>(source_address) -
                                   static_cast<int64_t>(span.ea);
        if (!snapshot_db_source_from_target(span.ea, window_rel, byte_count,
                                            true)) {
            break;
        }
        source.data = m_db_source_window.data();
        source.size = m_db_source_window.size();
        source.content_hash = snapshot_content_hash(source.data, source.size);
        if (bridge_trace_enabled()) {
            static uint32_t logged = 0;
            if (logged < 128u) {
                ++logged;
                std::fprintf(stderr,
                             "[mmdma-fb] target=%#x source=%#x rel=%lld bytes=%u hash=%#llx\n",
                             span.ea, source_address,
                             static_cast<long long>(window_rel), byte_count,
                             static_cast<unsigned long long>(
                                 source.content_hash));
            }
        }
        break;
    }
    if (!source.data) source = find_memory(memory, source_address, byte_count);
    if (!source.data || source.size < byte_count) return fail("src-short");

    m_pending_mmdma_texture = true;
    m_pending_mmdma_source_bit_offset = source_bit_offset;
    m_pending_mmdma_destination_base = register_value(kTxtLodBase0);
    m_pending_mmdma_bytes.assign(source.data, source.data + byte_count);
    m_pending_mmdma_hash =
        source.content_hash != 0 && source.size == byte_count
            ? source.content_hash
            : blob_hash_bytes(m_pending_mmdma_bytes.data(),
                              m_pending_mmdma_bytes.size());
    m_pending_linear_rows = false;
    m_pending_linear_row_bits = 0u;
    m_pending_linear_row_count = 0u;
    if (room_diag && source_address >= 0x955000u &&
        source_address < 0x99c000u) {
        std::fprintf(stderr,
                     "[room-mmdma-load] list=%llu frame=%llu src=%#x "
                     "count=%#x dst=%#x ld_cntl=%#x exp=%#x pip=%#x "
                     "pending=%zu\n",
                     (unsigned long long)m_exec_seq,
                     (unsigned long long)perf::counter_value(
                         perf::Counter::GuestFramesStarted),
                     source_address, byte_count,
                     m_pending_mmdma_destination_base,
                     register_value(kTxtLdCntl), register_value(kTxtExpType),
                     register_value(kTxtPipCntl),
                     m_pending_mmdma_bytes.size());
    }
    return true;
}

// Content identity over a staged MMDMA blob: identifies identical re-DMAs so
// the content-keyed decode cache can skip unpacking the same bits every frame.
// Delegates to the single snapshot content-hash algorithm so an exact-range
// hash (copied on the guest thread) and this fallback hash agree on the same
// bytes; see snapshot_content_hash for the mixing contract.
uint64_t blob_hash_bytes(const uint8_t* data, size_t size)
{
    return snapshot_content_hash(data, size);
}

uint64_t mmdma_blob_hash(const std::vector<uint8_t>& bytes)
{
    return blob_hash_bytes(bytes.data(), bytes.size());
}

// Match the GL cel-cache identity exactly: FNV-1a over the packed RGBA byte
// representation. A key match is what the backends treat as the same image,
// so this is the identity itself, not merely a lookup hint.
uint64_t rgba_content_hash(const std::vector<uint32_t>& pixels)
{
    return blob_hash_bytes(reinterpret_cast<const uint8_t*>(pixels.data()),
                           pixels.size() * sizeof(uint32_t));
}

uint64_t mmdma_sidecar_hash(const std::vector<uint8_t>& indices,
                            const std::vector<uint8_t>& alpha,
                            const std::vector<uint8_t>& source_ssb)
{
    // Include lengths and a field separator so differently shaped sidecars
    // cannot alias merely because their concatenated bytes happen to match.
    uint64_t hash = 1469598103934665603ull;
    const auto add = [&hash](const std::vector<uint8_t>& bytes) {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            hash ^= static_cast<uint8_t>(bytes.size() >> shift);
            hash *= 1099511628211ull;
        }
        for (const uint8_t byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    };
    add(indices);
    add(alpha);
    add(source_ssb);
    return hash;
}

uint64_t GuestCltInterpreter::mmdma_coarse_sidecar_hash(
    const std::vector<MmdmaLevelSidecars>& levels)
{
    // Include the level count and each level's dimensions so a chain cannot
    // alias another that concatenates the same sidecar bytes in a different
    // shape. Zero is reserved for "no authored chain".
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            hash ^= static_cast<uint8_t>(value >> shift);
            hash *= 1099511628211ull;
        }
    };
    mix(levels.size());
    for (const MmdmaLevelSidecars& level : levels) {
        mix(static_cast<uint32_t>(level.width));
        mix(static_cast<uint32_t>(level.height));
        mix(mmdma_sidecar_hash(level.indices, level.alpha, level.source_ssb));
    }
    return hash != 0u ? hash : 1u;
}

uint64_t g_mmdma_cache_hits = 0;
uint64_t g_mmdma_cache_misses = 0;

// Palette-content hash for the decode cache key. The guest re-arms the same
// palette every frame, so content equality (not the load generation) is the
// decode input that matters.
uint64_t pip_palette_hash(const std::vector<uint32_t>& pip)
{
    uint64_t hash = 1469598103934665603ull;
    for (const uint32_t word : pip) {
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            hash ^= (word >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool GuestCltInterpreter::materialize_mmdma_texture()
{
    return materialize_mmdma_texture_impl(true, nullptr);
}

// Bits one decoded texel of this TRAM expansion format occupies. 0 means the
// format stores no texel data of its own. Shared by the materializer's sizing
// and by every per-level decode so both agree on a level's byte footprint.
uint32_t mmdma_expansion_bits_per_texel(const TextureFormat& format,
                                        bool rgb555_ssb, bool literal_rgb888,
                                        bool no_color)
{
    if (rgb555_ssb) return 16u;
    const uint32_t alpha_bits =
        (format.has_alpha && format.alpha_depth > 0u) ? format.alpha_depth : 0u;
    const uint32_t ssb_bits = format.has_ssb ? 1u : 0u;
    if (literal_rgb888) return 24u + alpha_bits + ssb_bits;
    if (no_color) return alpha_bits + ssb_bits;
    return format.color_depth + alpha_bits + ssb_bits;
}

// The authored MIP chain of an MMDMA residency. CLT_CreateTxLoadCommandListMMDMA
// (vendor .../clt/clttxload.c:119-129) accumulates one texelDataSize per loaded
// LOD into lodBase[], and CLT_TxLoad4LOD (:180) publishes that quad, so each
// coarser level is a distinct span of the same TRAM blob rather than a reduction
// of the level the draw selected. Levels are listed coarsest-last in the guest's
// own order; a level the armed quad does not describe, or one whose span is not
// inside this blob, ends the chain and leaves the host to reduce the rest.
bool GuestCltInterpreter::mmdma_authored_lod_chain(uint32_t draw_base,
                                                   uint32_t level_width,
                                                   uint32_t level_height,
                                                   MmdmaLodChain* chain) const
{
    chain->levels.clear();
    if (m_pending_linear_rows || level_width == 0u || level_height == 0u)
        return false;
    const uint32_t lod_bases[4] = {register_value(0x6414),
                                   register_value(0x6418),
                                   register_value(0x641c),
                                   register_value(0x6420)};
    const uint32_t active_lods =
        std::min(4u, (register_value(kTxtAddrCntl) & 0xfu) + 1u);
    if (active_lods < 2u)
        return false;
    constexpr uint32_t kMaxTramBytes = 4u * 1024u * 1024u;
    // A real SDK MIP chain has strictly increasing active LOD bases (each
    // active base is the previous plus its texelDataSize, clttxload.c:119-129).
    // Inactive slots beyond LODMAX (numLOD-1) are left uninitialized by the
    // SDK and must not invalidate the active chain.
    if (lod_bases[0] < m_pending_mmdma_destination_base ||
        lod_bases[active_lods - 1u] >= kMaxTramBytes)
        return false;
    for (unsigned k = 0; k + 1u < active_lods; ++k) {
        if (lod_bases[k] >= lod_bases[k + 1u]) return false;
    }
    unsigned drawn = 0;
    while (drawn < active_lods && lod_bases[drawn] != draw_base) ++drawn;
    if (drawn >= active_lods) return false;
    const uint64_t resident_end =
        static_cast<uint64_t>(m_pending_mmdma_destination_base) +
        m_pending_mmdma_bytes.size();
    for (unsigned i = drawn + 1u; i < active_lods; ++i) {
        const uint32_t shift = i - drawn;
        const uint32_t w = std::max(1u, level_width >> shift);
        const uint32_t h = std::max(1u, level_height >> shift);
        if (w == 1u && h == 1u) break;
        const uint32_t next_base = (i + 1u < active_lods) ? lod_bases[i + 1u] : 0u;
        const uint64_t span =
            (next_base != 0u && next_base > lod_bases[i] &&
             next_base <= resident_end)
                ? static_cast<uint64_t>(next_base - lod_bases[i])
                : (resident_end > lod_bases[i]
                       ? resident_end - lod_bases[i]
                       : 0u);
        if (span == 0u) break;
        MmdmaLodChain::Level level;
        level.base = lod_bases[i];
        level.width = w;
        level.height = h;
        level.span_bytes = static_cast<uint32_t>(
            std::min<uint64_t>(span, 0xffffffffu));
        chain->levels.push_back(level);
    }
    return !chain->levels.empty();
}

const char* GuestCltInterpreter::decode_mmdma_level(
    const MmdmaLevelRequest& request, MmdmaDecodeValue* out)
{
    const TextureFormat format = decode_format(request.exp_type, 0);
    const bool rgb555_ssb = is_rgb555_ssb_format(format);
    const bool literal_rgb888 = !rgb555_ssb && is_literal_rgb888_format(format);
    const bool no_color_literal =
        !rgb555_ssb && !literal_rgb888 && is_no_color_literal_format(format);
    const bool indexed = is_indexed_tram_format(format);
    const bool no_color =
        no_color_literal || is_indexed_no_color_tram_format(format);
    if (!indexed && !rgb555_ssb && !literal_rgb888 && !no_color)
        return "format";
    const uint32_t bits_per_texel = mmdma_expansion_bits_per_texel(
        format, rgb555_ssb, literal_rgb888, no_color);
    if (bits_per_texel == 0u || request.width == 0u || request.height == 0u)
        return "shape";
    if (request.bit_offset < m_pending_mmdma_source_bit_offset)
        return "reader-skip";
    const uint64_t texel_count =
        static_cast<uint64_t>(request.width) * request.height;
    const PipControl pip_control = decode_pip_control(request.pip_control);
    if (!pip_control.documented) return "pip-control";
    const PipColor constant0 = decode_pip(request.constant0);
    const PipColor constant1 = decode_pip(request.constant1);

    MmdmaDecodeKey cache_key;
    cache_key.blob_hash = m_pending_mmdma_hash != 0
                              ? m_pending_mmdma_hash
                              : mmdma_blob_hash(m_pending_mmdma_bytes);
    cache_key.source_bit_offset = m_pending_mmdma_source_bit_offset;
    // Each level of a MIP chain has its own start byte inside the same blob, so
    // the key's level offset is what keeps the levels from sharing a cache entry.
    cache_key.draw_bit_offset =
        request.bit_offset - m_pending_mmdma_source_bit_offset;
    cache_key.exp_type = request.exp_type;
    cache_key.width = request.width;
    cache_key.height = request.height;
    cache_key.bits_per_texel = bits_per_texel;
    cache_key.pip_control = request.pip_control;
    cache_key.constant0 = request.constant0;
    cache_key.constant1 = request.constant1;
    cache_key.pip_palette_hash = m_pip_hash;
    if (const MmdmaDecodeValue* cached_decode =
            find_cached_mmdma_decode(cache_key)) {
        ++g_mmdma_cache_hits;
        // Same staged blob, same expansion/palette state: reuse the decoded
        // texels instead of re-unpacking identical bits.
        *out = *cached_decode;
        return nullptr;
    }
    ++g_mmdma_cache_misses;
    MmdmaDecodeValue value;
    std::vector<uint32_t>& pixels = value.pixels;
    std::vector<uint8_t>& ssb_bits = value.ssb_bits;
    std::vector<uint8_t>& palette_indices = value.palette_indices;
    std::vector<uint8_t>& source_alpha = value.source_alpha;
    std::vector<uint8_t>& source_ssb = value.source_ssb;
    BitReader reader(m_pending_mmdma_bytes.data(),
                     m_pending_mmdma_bytes.size());
    if (!reader.skip(request.bit_offset)) return "reader-skip";

    pixels.reserve(static_cast<size_t>(texel_count));
    ssb_bits.reserve(static_cast<size_t>(texel_count));
    if (indexed || no_color) {
        palette_indices.reserve(static_cast<size_t>(texel_count));
        source_ssb.reserve(static_cast<size_t>(texel_count));
    }
    if ((indexed || no_color) && format.has_alpha)
        source_alpha.reserve(static_cast<size_t>(texel_count));
    if (rgb555_ssb) {
        for (uint64_t i = 0; i < texel_count; ++i) {
            uint32_t ssb = 0, red = 0, green = 0, blue = 0;
            if (!reader.read(1u, &ssb) || !reader.read(5u, &red) ||
                !reader.read(5u, &green) || !reader.read(5u, &blue))
                return "rgb555-read";
            const uint32_t r = (red << 3u) | (red >> 2u);
            const uint32_t g = (green << 3u) | (green >> 2u);
            const uint32_t b = (blue << 3u) | (blue >> 2u);
            const uint32_t a = ssb != 0 ? 0xffu : 0u;
            pixels.push_back(r | (g << 8u) | (b << 16u) | (a << 24u));
            ssb_bits.push_back(static_cast<uint8_t>(ssb != 0));
        }
    } else if (literal_rgb888 || no_color) {
        // Literal TRAM words decode MSB-first in the toolkit's SSB -> alpha
        // -> color channel order.  With no color channel (0x1200/0x1a70
        // forms) or with 8-bit components (0x1e78), TXTPIPCNTL still owns
        // each output channel selection; a literal TRAM word carries no PIP
        // index, so a PIP selection has no defined entry and stays
        // fail-closed.  The texture side of a channel the TRAM word does not
        // store reads as zero (e.g. the 0x1200 SSB-only skid-mask form is
        // drawn with TXTPIPCNTL=0x4900, i.e. every channel from the texture
        // side: TRAM SSB with black color and zero alpha).
        for (uint64_t i = 0; i < texel_count; ++i) {
            uint32_t ssb = 0;
            if (format.has_ssb && !reader.read(1u, &ssb)) return "literal-read";
            uint32_t tex_alpha = 0;
            if (format.has_alpha && format.alpha_depth > 0u) {
                if (!reader.read(format.alpha_depth, &tex_alpha))
                    return "literal-alpha-read";
            }
            uint32_t red = 0, green = 0, blue = 0;
            if (literal_rgb888) {
                if (!reader.read(8u, &red) || !reader.read(8u, &green) ||
                    !reader.read(8u, &blue))
                    return "literal-color-read";
            }
            const PipColor& constant =
                pip_constant_for_ssb(ssb, constant0, constant1);
            // Texture-side channels: literal 8-bit components, or zero for a
            // channel absent from the TRAM word.  A 7-bit texel alpha
            // expands by MSB replication (4-bit likewise).
            const uint32_t texture_alpha =
                format.has_alpha && format.alpha_depth > 0u
                    ? replicate_msb(format.alpha_depth, tex_alpha)
                    : 0u;
            const uint32_t texture_rgb =
                literal_rgb888
                    ? (red | (green << 8u) | (blue << 16u))
                    : 0u;
            uint32_t rgb = 0;
            uint32_t alpha = 0;
            uint32_t selected_ssb = 0;
            bool ok = true;
            switch (pip_control.color_select) {
            case kPipSelectTexture:
                rgb = texture_rgb;
                break;
            case kPipSelectConstant:
                rgb = constant.rgba & 0xffffffu;
                break;
            default:
                ok = false;
                break;
            }
            if (ok) {
                switch (pip_control.alpha_select) {
                case kPipSelectTexture:
                    alpha = texture_alpha;
                    break;
                case kPipSelectConstant:
                    alpha = (constant.rgba >> 24u) & 0xffu;
                    break;
                default:
                    ok = false;
                    break;
                }
            }
            if (ok) {
                switch (pip_control.ssb_select) {
                case kPipSelectTexture:
                    selected_ssb = ssb != 0u;
                    break;
                case kPipSelectConstant:
                    selected_ssb = constant.ssb;
                    break;
                default:
                    ok = false;
                    break;
                }
            }
            if (!ok) return "literal-selector";
            pixels.push_back(rgb | (alpha << 24u));
            ssb_bits.push_back(static_cast<uint8_t>(selected_ssb != 0u));
            if (no_color) {
                palette_indices.push_back(0u);
                source_ssb.push_back(static_cast<uint8_t>(ssb != 0u));
                if (format.has_alpha)
                    source_alpha.push_back(static_cast<uint8_t>(texture_alpha));
            }
        }
    } else {
        // Every indexed TRAM row, including the 0xe74/0xe78 forms, uses the
        // vendor encoder's one storage order below. Keeping those two rows on
        // this shared path also keeps their 7-bit alpha on replicate_msb()
        // instead of silently introducing a second widening rule.
        for (uint64_t i = 0; i < texel_count; ++i) {
            uint32_t ssb = 0;
            uint32_t texture_alpha = 0;
            uint32_t source_index = 0;
            // Indexed TRAM layouts store SSB, alpha, then the 1..8-bit
            // index, matching M2TX_UncomprToM2TXRaw's source order. Alpha is
            // retained as an expanded sidecar when present so a later
            // TXTPIPCNTL change rebinds the same raw resident.
            if (format.has_ssb && !reader.read(1u, &ssb))
                return "indexed-ssb-read";
            if (format.has_alpha && format.alpha_depth > 0u) {
                uint32_t encoded_alpha = 0;
                if (!reader.read(format.alpha_depth, &encoded_alpha))
                    return "indexed-alpha-read";
                texture_alpha = replicate_msb(format.alpha_depth, encoded_alpha);
            }
            if (!reader.read(format.color_depth, &source_index))
                return "indexed-read";
            const PipColor& constant =
                pip_constant_for_ssb(ssb, constant0, constant1);
            uint32_t rgba = 0;
            uint8_t selected_ssb = 0;
            if (!materialize_indexed_texel(
                    m_pip, request.pip_control, constant0, constant1,
                    source_index,
                    format.has_alpha ? texture_alpha
                                     : (constant.rgba >> 24u) & 0xffu,
                    ssb,
                    &rgba, &selected_ssb))
                return "indexed-selector";
            pixels.push_back(rgba);
            palette_indices.push_back(static_cast<uint8_t>(source_index));
            source_ssb.push_back(static_cast<uint8_t>(ssb != 0u));
            if (format.has_alpha)
                source_alpha.push_back(static_cast<uint8_t>(texture_alpha));
            ssb_bits.push_back(selected_ssb);
        }
    }
    value.pixel_hash = rgba_content_hash(pixels);
    if (indexed || no_color)
        value.sidecar_hash = mmdma_sidecar_hash(
            value.palette_indices, value.source_alpha, value.source_ssb);
    MmdmaDecodeValue retained = value;
    *out = std::move(retained);
    store_cached_mmdma_decode(cache_key, std::move(value));
    return nullptr;
}

bool GuestCltInterpreter::materialize_mmdma_texture_impl(bool publish,
                                                        MaterializeResult* out)
{
    if (!m_pending_mmdma_texture) return true;

    const auto trace_materialize_failure = [&](const char* reason) {
        if (!bridge_trace_enabled() && !clt_unsupported_diag_enabled()) return;
        static uint32_t emitted = 0;
        static bool expired = false;
        if (emitted >= clt_unsupported_diag_max()) {
            if (!expired) {
                expired = true;
                std::fprintf(stderr,
                             "[mat-fail] cap reached — further entries "
                             "suppressed\n");
            }
            return;
        }
        ++emitted;
        std::fprintf(stderr,
                     "[mat-fail] reason=%s exp=%#x uv=%#x base=%#x "
                     "blob_base=%#x blob_bytes=%zu pip=%#x\n",
                     reason, register_value(kTxtExpType),
                     register_value(kTxtLdWidth),
                     register_value(kTxtLodBase0),
                     m_pending_mmdma_destination_base,
                     m_pending_mmdma_bytes.size(),
                     register_value(kTxtPipCntl));
    };

    static const bool room_diag = [] {
        const char* e = std::getenv("M2_CLT_ROOM_MMDMA_DIAG");
        return e && *e && *e != '0';
    }();
    if (room_diag && m_last_pip_source >= 0x955000u &&
        m_last_pip_source < 0x99c000u) {
        std::fprintf(stderr,
                     "[room-mmdma-materialize] list=%llu frame=%llu "
                     "pip_src=%#x pip_bytes=%u pending=%zu dst=%#x "
                     "lod0=%#x exp=%#x\n",
                     (unsigned long long)m_exec_seq,
                     (unsigned long long)perf::counter_value(
                         perf::Counter::GuestFramesStarted),
                     m_last_pip_source, m_last_pip_bytes,
                     m_pending_mmdma_bytes.size(),
                     m_pending_mmdma_destination_base,
                     register_value(kTxtLodBase0), register_value(kTxtExpType));
    }

    const uint32_t uv_max = register_value(kTxtLdWidth);
    uint32_t width = ((uv_max >> 16u) & 0x3ffu) + 1u;
    uint32_t height = (uv_max & 0x3ffu) + 1u;
    uint64_t texel_count = static_cast<uint64_t>(width) * height;
    const uint32_t uv_mask = register_value(0x6430u);
    const uint32_t umax = (uv_max >> 16u) & 0x3ffu;
    const uint32_t vmax = uv_max & 0x3ffu;
    const uint32_t umask = (uv_mask >> 16u) & 0x3ffu;
    const uint32_t vmask = uv_mask & 0x3ffu;
    // MAME's 3D0 address calculation masks and then clamps the integerized
    // U/V coordinates with the raw TXTUVMAX/TXTUVMASK pair. For a flat
    // single-LOD load, that effective window is also the geometry of the
    // MMDMA texture slice. Do not apply this to MIP chains: their packed
    // bases describe a larger residency with separate LOD addressing.
    const uint32_t address_width = (umax & umask) + 1u;
    const uint32_t address_height = (vmax & vmask) + 1u;
    const bool single_lod_addressing =
        (register_value(kTxtAddrCntl) & 0xfu) == 0u;
    // The load/use command list is self-describing about the residency's
    // geometry. CLT_CreateTxLoadCommandListMMDMA (the checked-in Mercury
    // texlib) writes, for each axis:
    //   tiled axis  : TXTUVMASK = (finest loaded dim - 1),
    //                 TXTUVMAX = 0x3FF >> (numLOD-1);
    //   clamped axis: TXTUVMASK = 0x3FF,
    //                 TXTUVMAX = (coarsest loaded dim - 1);
    // and TXTADDRCNTL.LODMAX = numLOD-1. Each mask period is therefore a
    // *dimension* and TXTUVMAX is a wrap ceiling, so the pair recovers both
    // finest loaded dims -- including their orientation, which the UVMAX
    // window cannot express (a wrap-axis ceiling says nothing about size).
    // State 3 world draw: UVMAX
    // 0x000f007f, UVMASK 0x03ff003f, LODMAX 3 -> clamped U = (15+1)<<3 = 128,
    // tiled V = 63+1 = 64, which matches the LOD0 footprint of
    // lodBase[1]-lodBase[0] = 0x1000 bytes of 4-bit texels (8192 = 128*64).
    const uint32_t lod_shift = register_value(kTxtAddrCntl) & 0xfu;
    const uint32_t derived_u =
        lod_shift <= 10u
            ? (umask == 0x3ffu ? ((umax + 1u) << lod_shift) : (umask + 1u))
            : 0u;
    const uint32_t derived_v =
        lod_shift <= 10u
            ? (vmask == 0x3ffu ? ((vmax + 1u) << lod_shift) : (vmask + 1u))
            : 0u;
    // A square or oversized derivation carries no usable orientation signal;
    // fall back to the UVMAX window aspect in that case.
    const bool derived_dims_usable =
        derived_u >= 1u && derived_v >= 1u && derived_u <= 2048u &&
        derived_v <= 2048u && derived_u != derived_v;
    const bool derived_u_longer = derived_u > derived_v;
    // The guest's use list supplies the dimensions and expansion format for
    // the resident texture. Materialization is normally forced at the
    // following TXTEXPTYPE write, after TXTUVMAX has established the window
    // and before a later use list can overwrite these registers. Keep the
    // current draw/use format here because the load command's expansion field
    // can be reused by the guest before the corresponding use state is emitted.
    const TextureFormat format =
        decode_format(register_value(kTxtExpType), 0);
    const bool rgb555_ssb = is_rgb555_ssb_format(format);
    const bool literal_rgb888 = !rgb555_ssb && is_literal_rgb888_format(format);
    const bool no_color_literal =
        !rgb555_ssb && !literal_rgb888 && is_no_color_literal_format(format);
    const bool indexed = is_indexed_tram_format(format);
    const bool no_color = no_color_literal ||
                          is_indexed_no_color_tram_format(format);
    if (!indexed && !rgb555_ssb && !literal_rgb888 && !no_color) {
        if (bridge_trace_enabled()) std::fprintf(stderr, "[mat-fail] not indexed nor rgb555 exp=%#x\n", register_value(kTxtExpType));
        return false;
    }
    // One derivation, shared with every per-level decode: the level's byte
    // footprint has to be measured with the same texel width the decoder used.
    const uint32_t bits_per_texel = mmdma_expansion_bits_per_texel(
        format, rgb555_ssb, literal_rgb888, no_color);
    // Set when the sizing below follows the guest's own description of the
    // texture rather than a payload-size guess. See MaterializeResult.
    bool sized_from_state = false;
    if (m_pending_linear_rows) {
        if (bits_per_texel == 0u || m_pending_linear_row_bits == 0u ||
            m_pending_linear_row_count == 0u ||
            (m_pending_linear_row_bits % bits_per_texel) != 0u) {
            trace_materialize_failure("linear-row-format");
            return false;
        }
        width = m_pending_linear_row_bits / bits_per_texel;
        height = m_pending_linear_row_count;
        texel_count = static_cast<uint64_t>(width) * height;
        // The row pitch and row count are the guest's own TLD registers, so
        // this geometry is described by state, not inferred from the payload
        // size. Report it as such so a later draw of the same raw rows under
        // a different TXTEXPTYPE may re-decode them (serve_for_draw()).
        sized_from_state = true;
    }
    // A draw selects a MIP chain position inside the DMA'd blob (SDK:
    // lodBase[i] = tramOffset + sum of earlier texelDataSizes), so the
    // materializer must satisfy the draw window starting from that position,
    // not from the blob start.
    const uint32_t draw_base = register_value(kTxtLodBase0);
    uint64_t draw_bit_offset = 0;
    if (draw_base >= m_pending_mmdma_destination_base) {
        draw_bit_offset = static_cast<uint64_t>(draw_base -
                                                m_pending_mmdma_destination_base) *
                          8u;
    } else {
        // The draw references a base before this blob (an earlier texture in
        // the TRAM chain that was not part of the last DMA). No evidence yet
        // maps that case to a host texture.
        if (bridge_trace_enabled())
            std::fprintf(stderr,
                         "[mat-fail] draw base %#x precedes blob base %#x\n",
                         draw_base, m_pending_mmdma_destination_base);
        return false;
    }
    // The guest writes four monotonically increasing LOD bases (SDK
    // CLT_TxLoad4LOD). The spacing between consecutive bases is the byte
    // footprint of that LOD, which is the authoritative texture geometry for
    // the draw: the SDKS TXTUVMAX WRAP-mode encoding (umax = 0x3FF >>
    // (numLOD-1)) does not describe the texture window for MIP chains, so
    // the uvmax-derived size is only a fallback for flat (single-base)
    // loads.  Bases that read as code/data pointers (>= the observed TRAM
    // bounds) mark an absent or cross-register quad and are ignored.
    const uint32_t lod_bases[4] = {
        register_value(0x6414), register_value(0x6418),
        register_value(0x641c), register_value(0x6420)};
    constexpr uint32_t kMaxTramBytes = 4u * 1024u * 1024u;
    const uint64_t resident_end =
        static_cast<uint64_t>(m_pending_mmdma_destination_base) +
        m_pending_mmdma_bytes.size();
    uint32_t footprint_texels = 0;
    const uint32_t active_lods =
        std::min(4u, (register_value(kTxtAddrCntl) & 0xfu) + 1u);
    uint32_t cand_lods = 0u;
    if (active_lods >= 2u &&
        lod_bases[0] >= m_pending_mmdma_destination_base &&
        lod_bases[active_lods - 1u] < kMaxTramBytes) {
        bool sane = true;
        for (unsigned k = 0; sane && k + 1u < active_lods; ++k) {
            if (lod_bases[k] >= lod_bases[k + 1u]) sane = false;
        }
        if (sane) cand_lods = active_lods;
    } else if (active_lods < 2u &&
               lod_bases[0] < lod_bases[1] &&
               lod_bases[1] < lod_bases[2] &&
               lod_bases[2] < lod_bases[3] &&
               lod_bases[3] < kMaxTramBytes &&
               lod_bases[0] >= m_pending_mmdma_destination_base) {
        // Fallback for fixtures or stale state where TXTADDRCNTL was omitted
        // or unupdated but all four LOD bases form a strictly increasing quad.
        cand_lods = 4u;
    }
    for (unsigned i = 0; !m_pending_linear_rows && i < cand_lods; ++i) {
        if (lod_bases[i] != draw_base) continue;
        const uint32_t next_base =
            (i + 1u < cand_lods && lod_bases[i + 1u] > draw_base)
                ? lod_bases[i + 1u]
                : (i + 2u < cand_lods && lod_bases[i + 2u] > draw_base)
                      ? lod_bases[i + 2u]
                      : 0;
        // The final LOD extends to the end of the DMA'd chain? The guest
        // packs several textures per DMA, so the last base's size is bounded
        // by the blob end instead of by a next base.
        const uint32_t blobs_remaining =
            static_cast<uint32_t>(m_pending_mmdma_bytes.size()) -
            static_cast<uint32_t>(draw_bit_offset / 8u);
        const uint32_t footprint_bytes =
            (next_base != 0 && next_base > draw_base &&
             static_cast<uint64_t>(next_base) <= resident_end)
                ? (next_base - draw_base)
                : blobs_remaining;
        footprint_texels = footprint_bytes * 8u / bits_per_texel;
        sized_from_state = footprint_texels != 0;
        break;
    }
    const uint64_t available_bits =
        static_cast<uint64_t>(m_pending_mmdma_bytes.size()) * 8u;
    const uint64_t remaining_bits =
        (available_bits > draw_bit_offset + m_pending_mmdma_source_bit_offset)
            ? (available_bits - draw_bit_offset -
               m_pending_mmdma_source_bit_offset)
            : 0;
    const uint64_t max_texels = remaining_bits / bits_per_texel;
    uint32_t target_texels = footprint_texels;
    const uint64_t address_texels =
        static_cast<uint64_t>(address_width) * address_height;
    if (!m_pending_linear_rows && single_lod_addressing && uv_mask != 0u &&
        address_texels > 0u &&
        address_width <= 2048u && address_height <= 2048u &&
        address_texels <= max_texels) {
        width = address_width;
        height = address_height;
        texel_count = address_texels;
        target_texels = static_cast<uint32_t>(address_texels);
        // The single-LOD window is the texture's geometry by construction.
        sized_from_state = true;
    }
    const uint64_t uv_window_bits = texel_count * bits_per_texel;
    const uint64_t payload_uv_bit_delta =
        remaining_bits >= uv_window_bits
            ? remaining_bits - uv_window_bits
            : uv_window_bits - remaining_bits;
    const bool payload_size_differs = payload_uv_bit_delta > 7u;
    const bool nontrivial_wrap_mask = register_value(0x6430) != 0u;
    if (!m_pending_linear_rows && target_texels == 0 && max_texels > 0 &&
        max_texels != texel_count && payload_size_differs &&
        nontrivial_wrap_mask) {
        // When LOD quads are flat or uninitialized (e.g. pointer values in
        // secondary registers), a nontrivial TXTUVMASK proves that the
        // TXTUVMAX register reflects the hardware wrap ceiling rather than
        // the actual texture dimensions. Sizing to the loaded DMA payload
        // recovers the true texture resolution (e.g. 128x256 from 16 KiB
        // 4-bit). With the default zero mask, retain the explicit UVMAX
        // window; any surplus bits there are alignment padding, as in the
        // 3x1 indexed unit fixtures.
        target_texels = static_cast<uint32_t>(max_texels);
    }
    // A valid MIP use list gives the complete dimensions, not just the long
    // axis. Trust them when their texel count agrees with the selected LOD's
    // byte footprint. Guessing a near-square shape from that footprint alone
    // splits wide banner rows and joins tall scenery rows; using the UVMAX
    // wrap ceiling as an aspect ratio also shears non-power-of-two textures.
    // Keep flat/linear loads and stale or incomplete use states on their
    // existing paths: a mask by itself does not prove the resident's shape.
    const uint64_t derived_texels = static_cast<uint64_t>(derived_u) * derived_v;
    if (!m_pending_linear_rows && !single_lod_addressing &&
        derived_u >= 1u && derived_v >= 1u &&
        derived_u <= 2048u && derived_v <= 2048u &&
        footprint_texels != 0u && derived_texels == footprint_texels &&
        derived_texels <= max_texels) {
        width = derived_u;
        height = derived_v;
        texel_count = derived_texels;
    }
    if (target_texels != 0 &&
        target_texels != static_cast<uint64_t>(width) * height) {
        // The uvmax value is the chain's WRAP range, not the drawn LOD dims;
        // rasterize the target footprint instead of the stale window.
        const double window_ratio =
            width > 0 ? static_cast<double>(width) / height : 1.0;
        if ((target_texels & (target_texels - 1)) == 0) {
            // The payload is an exact power-of-two footprint. The M2 texture
            // cache is power-of-two and UVMAX is a WRAP ceiling (e.g. 1024x128
            // while the DMA'd payload is 128x128 = 0x4000 bytes of 8bpp), so
            // reconstruct exact pow2 geometry (128x128, 256x128, ...) rather
            // than inheriting the meaningless window aspect.
            uint32_t w = 1u, h = 1u;
            while (static_cast<uint64_t>(w) * w < target_texels) w <<= 1;
            h = static_cast<uint32_t>(target_texels / w);
            // Which axis is the long one: the mask-derived dims are real
            // dimensions, the UVMAX window aspect is only a ceiling. Using
            // the window here is what transposed the accepted 128x64
            // residency into 64x128.
            const bool keep_w_long =
                derived_dims_usable ? derived_u_longer : window_ratio >= 1.0;
            if (!keep_w_long) std::swap(w, h);
            width = std::max(1u, w);
            height = std::max(1u, h);
        } else {
            // The target footprint is smaller than the uvmax window (or
            // non-power-of-two): preserve the window aspect.
            //
            // State 2 showroom refinement: before guessing from
            // the window aspect, consult the guest's texture wrap mask
            // (TXTUVMASK 0x6430). Mask+1 is the addressing period; when it
            // exactly explains the footprint as a base level plus a standard
            // MIP tail, it is the authentic base dims. The showroom floor is
            // DMA'd as 16320 bytes of 6-bit texels (21760 texels) with mask
            // (127,127): 128x128 base plus 64/32/16 MIPs, not the 147x147
            // square the aspect heuristic guesses (that shear turns the
            // marble checker into gray stripes). Only exact MIP-chain matches
            // (k>=1, so a MIP tail must be present) override the heuristic;
            // stale masks fail either the fit or the exact-ratio test.
            const uint32_t mask_w = ((uv_mask >> 16u) & 0x3ffu) + 1u;
            const uint32_t mask_h = (uv_mask & 0x3ffu) + 1u;
            bool mask_sized = false;
            if (mask_w <= 2048u && mask_h <= 2048u) {
                const uint64_t mask_base =
                    static_cast<uint64_t>(mask_w) * mask_h;
                if (mask_base < target_texels &&
                    target_texels <= kMaxTexturePixels) {
                    for (unsigned k = 1u; k <= 8u; ++k) {
                        // footprint == base * (1 + 1/4 + ... + 1/4^k)?
                        const uint64_t scale = 1ULL << (2u * k);
                        if (target_texels * 3u * scale ==
                            mask_base * ((scale << 2u) - 1u)) {
                            width = mask_w;
                            height = mask_h;
                            mask_sized = true;
                            break;
                        }
                    }
                }
            }
            if (!mask_sized) {
                uint32_t w =
                    static_cast<uint32_t>(std::max(1.0, std::sqrt(
                        static_cast<double>(target_texels) * window_ratio)));
                uint32_t h =
                    static_cast<uint32_t>(std::max(1.0, std::sqrt(
                        static_cast<double>(target_texels) / window_ratio)));
                while (static_cast<uint64_t>(w) * h > target_texels &&
                       (w > 1u || h > 1u)) {
                    if (w >= h && w > 1u)
                        --w;
                    else if (h > 1u)
                        --h;
                    else
                        break;
                }
                // Same orientation preference as the power-of-two path above.
                // The swap preserves the texel product, so the aspect guess
                // keeps its footprint either way.
                if (derived_dims_usable && derived_u_longer != (w >= h))
                    std::swap(w, h);
                width = std::max(1u, w);
                height = std::max(1u, h);
            }
        }
        texel_count = static_cast<uint64_t>(width) * height;
    }
    const uint64_t required_bits =
        draw_bit_offset + m_pending_mmdma_source_bit_offset +
        texel_count * bits_per_texel;
    if (width == 0 || height == 0 || width > 2048u || height > 2048u ||
        texel_count > kMaxTexturePixels ||
        required_bits > available_bits) {
        trace_materialize_failure("shape");
        if (bridge_trace_enabled() || clt_unsupported_diag_enabled())
            std::fprintf(stderr,
                         "[mat-fail] w=%u h=%u texels=%llu req=%llu avail=%llu srcbits=%u exp=%#x uv=%#x base=%#x blob_base=%#x lod0=%#x lod1=%#x lod2=%#x lod3=%#x blob_bytes=%zu count=%#x\n",
                         width, height, (unsigned long long)texel_count,
                         (unsigned long long)required_bits,
                         (unsigned long long)available_bits,
                         m_pending_mmdma_source_bit_offset,
                         register_value(kTxtExpType),
                         register_value(kTxtLdWidth),
                         draw_base, m_pending_mmdma_destination_base,
                         register_value(0x6414), register_value(0x6418),
                         register_value(0x641c), register_value(0x6420),
                         m_pending_mmdma_bytes.size(),
                         register_value(kTxtCount));
        return false;
    }

    const uint32_t pip_control_value = register_value(kTxtPipCntl);
    const PipControl pip_control = decode_pip_control(pip_control_value);
    if (!pip_control.documented) return false;
    if (!publish && !sized_from_state) {
        // Draw-time decode whose footprint the armed registers do not
        // determine. Bail before decoding or uploading anything: the caller
        // keeps the resident's own decode, and a guessed geometry here would
        // be calibrated for a blob starting at its own base. See
        // MaterializeResult::sized_from_state.
        return false;
    }
    const uint32_t pip_offset = pip_control.index_offset;
    const uint32_t constant0_raw = register_value(kTxtConst0);
    const uint32_t constant1_raw = register_value(kTxtConst1);
    std::vector<uint32_t> pixels;
    std::vector<uint8_t> ssb_bits;
    std::vector<uint8_t> palette_indices;
    std::vector<uint8_t> source_alpha;
    std::vector<uint8_t> source_ssb;
    uint64_t decoded_pixels_hash = 0;
    uint64_t source_sidecar_hash = 0;
    {
        MmdmaLevelRequest level;
        level.bit_offset = m_pending_mmdma_source_bit_offset + draw_bit_offset;
        level.width = width;
        level.height = height;
        level.exp_type = register_value(kTxtExpType);
        level.pip_control = pip_control_value;
        level.constant0 = constant0_raw;
        level.constant1 = constant1_raw;
        MmdmaDecodeValue decoded;
        if (const char* failure = decode_mmdma_level(level, &decoded)) {
            trace_materialize_failure(failure);
            return false;
        }
        pixels = std::move(decoded.pixels);
        ssb_bits = std::move(decoded.ssb_bits);
        palette_indices = std::move(decoded.palette_indices);
        source_alpha = std::move(decoded.source_alpha);
        source_ssb = std::move(decoded.source_ssb);
        decoded_pixels_hash = decoded.pixel_hash;
        source_sidecar_hash = decoded.sidecar_hash;
    }

    const std::vector<uint32_t>& decoded_pixels = pixels;
    if (decoded_pixels_hash == 0)
        decoded_pixels_hash = rgba_content_hash(decoded_pixels);

    CelId cel = kInvalidCel;
    {
        const char* e = std::getenv("M2_DRAW_DIAG");
        if (e && *e && *e != '0') {
            uint32_t min_rgb = 0x00ffffffu;
            uint32_t max_rgb = 0u;
            uint32_t nonblack = 0u;
            for (const uint32_t pixel : decoded_pixels) {
                const uint32_t rgb = pixel & 0x00ffffffu;
                min_rgb = std::min(min_rgb, rgb);
                max_rgb = std::max(max_rgb, rgb);
                if (rgb != 0u) ++nonblack;
            }
            static uint32_t mat_logged = 0;
            static bool mat_expired = false;
            const uint32_t mat_cap = clt_diag_focused_cap(16u, 1024u);
            const bool mat_list_hit = clt_diag_list_selected(m_exec_seq);
            if (mat_logged >= mat_cap && !mat_expired) {
                mat_expired = true;
                std::fprintf(stderr,
                             "[mat-dims] cap reached — further entries "
                             "suppressed\n");
            }
            if (mat_logged < mat_cap && mat_list_hit) {
                std::fprintf(stderr,
                             "[mat-dims] n=%u exec=%llu frame=%llu "
                             "pip_exec=%llu pip_frame=%llu pip_src=%#x "
                             "pip_bytes=%u w=%u h=%u uv=%#x maxtex=%llu "
                             "foot=%u target=%u bpt=%u exp=%#x base=%#x "
                             "dblob=%zu pipcntl=%#x pipoff=%#x "
                             "pip0=%08x pip1=%08x pip2=%08x "
                             "pip3=%08x rgb=[%06x,%06x] nonblack=%u/%zu\n",
                             mat_logged, (unsigned long long)m_exec_seq,
                             (unsigned long long)perf::counter_value(
                                 perf::Counter::GuestFramesStarted),
                             (unsigned long long)m_last_pip_exec_seq,
                             (unsigned long long)m_last_pip_frame,
                             m_last_pip_source, m_last_pip_bytes, width,
                             height,
                             register_value(kTxtLdWidth),
                             (unsigned long long)max_texels, footprint_texels,
                             target_texels, bits_per_texel,
                             register_value(kTxtExpType), draw_base,
                             m_pending_mmdma_bytes.size(),
                             register_value(kTxtPipCntl), pip_offset,
                             m_pip[pip_lookup_index(pip_offset, 0u)],
                             m_pip[pip_lookup_index(pip_offset, 1u)],
                             m_pip[pip_lookup_index(pip_offset, 2u)],
                             m_pip[pip_lookup_index(pip_offset, 3u)], min_rgb,
                             max_rgb, nonblack, decoded_pixels.size());
                ++mat_logged;
                if (mat_logged == mat_cap && !mat_expired) {
                    mat_expired = true;
                    std::fprintf(stderr,
                                 "[mat-dims] cap reached — further entries "
                                 "suppressed\n");
                }
            }
        }
    }
    dump_tex_pixels(static_cast<int>(width), static_cast<int>(height),
                    decoded_pixels.data(), "mmdma",
                    register_value(kTxtExpType));
    // The guest's own coarser levels. CLT_CreateTxLoadCommandListMMDMA gives
    // every loaded LOD its own texelDataSize span and CLT_TxLoad4LOD publishes
    // those lodBase[] offsets, so the residency the draw selects is only the
    // first level of data the guest loaded: a host reduction of it would
    // replace bytes the guest authored (a different LOD may hold different
    // content or a different orientation). Decode the coarser spans and hand
    // them to the backend with the selected level; a level the quad does not
    // describe, whose span is not inside this blob, or that the backend cannot
    // sample, ends the chain and leaves the host to reduce the rest.
    std::vector<CelLodLevel> authored_levels;
    std::vector<std::vector<uint32_t>> authored_pixels;
    // The coarser levels' retained source sidecars. The base level's are kept
    // in the published record below; retaining every level's lets a later
    // palette or selector change re-expand the whole authored chain under the
    // new use state (refresh_mmdma_palette).
    std::vector<MmdmaLevelSidecars> authored_sidecars;
    MmdmaLodChain lod_chain;
    if (m_backend && !m_pending_linear_rows &&
        m_backend->clt_supports_authored_lod_chain() &&
        mmdma_authored_lod_chain(draw_base, width, height, &lod_chain)) {
        authored_pixels.reserve(lod_chain.levels.size());
        MmdmaLevelRequest level;
        level.exp_type = register_value(kTxtExpType);
        level.pip_control = pip_control_value;
        level.constant0 = constant0_raw;
        level.constant1 = constant1_raw;
        for (const auto& authored : lod_chain.levels) {
            if (static_cast<uint64_t>(authored.span_bytes) * 8u <
                static_cast<uint64_t>(authored.width) * authored.height *
                    bits_per_texel)
                break;
            level.bit_offset =
                (static_cast<uint64_t>(authored.base) -
                 m_pending_mmdma_destination_base) *
                    8u +
                m_pending_mmdma_source_bit_offset;
            level.width = authored.width;
            level.height = authored.height;
            MmdmaDecodeValue decoded;
            if (decode_mmdma_level(level, &decoded)) break;
            if (publish && (indexed || no_color)) {
                MmdmaLevelSidecars sidecars;
                sidecars.width = static_cast<int>(authored.width);
                sidecars.height = static_cast<int>(authored.height);
                sidecars.indices = std::move(decoded.palette_indices);
                sidecars.alpha = std::move(decoded.source_alpha);
                sidecars.source_ssb = std::move(decoded.source_ssb);
                authored_sidecars.push_back(std::move(sidecars));
            }
            CelLodLevel upload;
            upload.w = static_cast<int>(authored.width);
            upload.h = static_cast<int>(authored.height);
            authored_pixels.push_back(std::move(decoded.pixels));
            authored_levels.push_back(upload);
        }
        for (size_t i = 0; i < authored_levels.size(); ++i)
            authored_levels[i].rgba = authored_pixels[i].data();
    }
    {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        // World MMDMA textures carry SDK MIP chains (TxLoad4LOD, trilinear
        // interfilter) and are minified up to ~50x on the horizon band, so
        // sample them with bilinear and the guest's own LOD bytes. HUD/pixel-art
        // paths keep NEAREST via cel_upload_rgba; the guest draw carries the
        // authentic per-material filter and wrap state to the backend.
        if (m_backend) {
            if (m_pending_linear_rows)
                cel = m_backend->cel_upload_rgba_ex_hashed(
                    static_cast<int>(width), static_cast<int>(height),
                    decoded_pixels.data(), false, false, decoded_pixels_hash);
            else if (!authored_levels.empty())
                cel = m_backend->cel_upload_rgba_lod_chain(
                    static_cast<int>(width), static_cast<int>(height),
                    decoded_pixels.data(), authored_levels.data(),
                    authored_levels.size(), true, false, decoded_pixels_hash);
            else
                cel = m_backend->cel_upload_rgba_ex_hashed(
                    static_cast<int>(width), static_cast<int>(height),
                    decoded_pixels.data(), true, false, decoded_pixels_hash);
        }
    }
    if (m_backend && cel == kInvalidCel) {
        trace_materialize_failure("backend-upload");
        return false;
    }
    // The DMA'd blob is texelDataSize bytes at the destination base; a draw's
    // LOD base lands at an interior MIP chain position (lodBase[i] =
    // tramOffset + sum of earlier texelDataSizes), so the loaded span is the
    // whole blob, not just its start.
    if (!publish) {
        // Draw-time decode: hand the result back and leave both the staged
        // record and the resident state untouched. The caller owns the
        // pending slots (it swapped the resident's retained bytes in for the
        // duration of this call) and restores them when it is done.
        if (out) {
            out->cel = cel;
            out->ssb = std::move(ssb_bits);
            out->width = static_cast<int>(width);
            out->height = static_cast<int>(height);
            out->sized_from_state = sized_from_state;
        }
        return true;
    }
    const uint32_t stream_base = m_pending_mmdma_destination_base;
    const uint32_t stream_size =
        static_cast<uint32_t>(m_pending_mmdma_bytes.size());
    // Retain the staged bytes and the use state that produced this decode. A
    // later draw whose armed registers or LOD base differ from these has to
    // decode the TRAM bytes itself; see serve_for_draw().
    std::vector<uint8_t> staged_bytes = std::move(m_pending_mmdma_bytes);
    const uint32_t staged_source_bit_offset = m_pending_mmdma_source_bit_offset;
    const uint64_t staged_blob_hash = m_pending_mmdma_hash;
    const bool staged_linear_rows = m_pending_linear_rows;
    const uint32_t staged_linear_row_bits = m_pending_linear_row_bits;
    const uint32_t staged_linear_row_count = m_pending_linear_row_count;
    const uint32_t staged_exp_type = register_value(kTxtExpType);
    const uint32_t staged_uv_max = uv_max;
    const uint32_t staged_uv_mask = uv_mask;
    const uint32_t staged_addr_cntl = register_value(kTxtAddrCntl);
    const uint32_t staged_pip_control = register_value(kTxtPipCntl);
    // The staged bytes are resolved into `cel`; consume the pending record
    // before publishing so this materialization's own TRAM write is not
    // mistaken for a foreign overwrite by publish_texture.
    m_pending_mmdma_texture = false;
    m_pending_mmdma_bytes.clear();
    m_pending_mmdma_hash = 0;
    m_pending_linear_rows = false;
    m_pending_linear_row_bits = 0u;
    m_pending_linear_row_count = 0u;
    publish_texture(cel, stream_base, stream_size, static_cast<int>(width),
                    static_cast<int>(height), std::move(ssb_bits));
    m_texture.staged_valid = true;
    m_texture.staged_serial = ++m_texture_serial;
    m_texture.staged_blob_base = stream_base;
    m_texture.staged_decode_base = draw_base;
    // Record the coarser bases the sizing and authored-chain recovery above
    // consumed, so a later draw that changes one cannot sample this decode.
    m_texture.staged_lod_bases = {lod_bases[1], lod_bases[2], lod_bases[3]};
    m_texture.staged_source_bit_offset = staged_source_bit_offset;
    m_texture.staged_blob_hash = staged_blob_hash;
    m_texture.staged_exp_type = staged_exp_type;
    m_texture.staged_uv_max = staged_uv_max;
    m_texture.staged_uv_mask = staged_uv_mask;
    m_texture.staged_addr_cntl = staged_addr_cntl;
    m_texture.staged_pip_control = staged_pip_control;
    m_texture.staged_linear_rows = staged_linear_rows;
    m_texture.staged_linear_row_bits = staged_linear_row_bits;
    m_texture.staged_linear_row_count = staged_linear_row_count;
    m_texture.staged_bytes = std::move(staged_bytes);
    if (indexed || no_color) {
        m_texture.mmdma_indexed = true;
        m_texture.mmdma_no_color = no_color;
        m_texture.mmdma_exp_type = register_value(kTxtExpType);
        m_texture.mmdma_pip_control = pip_control_value;
        m_texture.mmdma_pip_offset = pip_offset;
        m_texture.mmdma_pip_generation = m_pip_generation;
        m_texture.mmdma_indices = std::move(palette_indices);
        m_texture.mmdma_alpha = std::move(source_alpha);
        m_texture.mmdma_source_ssb = std::move(source_ssb);
        m_texture.mmdma_constant0 = constant0_raw;
        m_texture.mmdma_constant1 = constant1_raw;
        m_texture.mmdma_pip_hash = m_pip_hash;
        m_texture.mmdma_source_hash = source_sidecar_hash != 0
                                          ? source_sidecar_hash
                                          : mmdma_sidecar_hash(
                                                m_texture.mmdma_indices,
                                                m_texture.mmdma_alpha,
                                                m_texture.mmdma_source_ssb);
        // Retain the coarser levels' own source fields so the next palette or
        // selector refresh re-expands them with the base instead of reducing
        // LOD0 over them.
        m_texture.mmdma_coarse_levels = std::move(authored_sidecars);
        // A republished record may reuse this Texture slot; the identity is
        // derived from the new sidecars on the next refresh.
        m_texture.mmdma_coarse_hash = 0;
    }
    return true;
}

// True when the record's own decode is already the draw's own decode: the
// byte range it was materialized from starts at the drawn LOD base and the
// use state that produced it is still the armed state.
bool GuestCltInterpreter::armed_decode_matches(const Texture& record,
                                               uint32_t lod_base) const
{
    // Without retained bytes there is nothing to re-decode; keep the record's
    // own decode (the pre-existing behavior) rather than failing the draw.
    if (!record.staged_valid || record.staged_bytes.empty()) return true;
    // An eager full-load decode is only stale when the draw names a different
    // texel format than the one live at its TLD.
    if (record.staged_format_only)
        return record.staged_exp_type == register_value(kTxtExpType);
    if (record.staged_linear_rows) {
        // A raw-row resident's decode is a function of exactly two draw-time
        // inputs: the TLD's staged row pitch divided by the *draw's* bits per
        // texel, and the bit offset of the draw's own LOD base into those
        // rows. TXTUVMAX, TXTUVMASK and the TXTADDRCNTL LOD field take no
        // part in it (the raw-row sizing branch replaces the UV window and
        // skips the LOD-quad and address-window derivations), so a change to
        // them cannot make this decode stale and must not cost a re-decode.
        // TXTEXPTYPE and the drawn base can, and the hardware decodes TRAM
        // afresh for each of them.
        if (lod_base != record.staged_decode_base) return false;
        return record.staged_exp_type == register_value(kTxtExpType);
    }
    if (lod_base != record.staged_decode_base) return false;
    if (record.staged_exp_type != register_value(kTxtExpType)) return false;
    if (record.staged_uv_max != register_value(kTxtLdWidth)) return false;
    if (record.staged_uv_mask != register_value(0x6430u)) return false;
    if ((record.staged_addr_cntl & kTxtAddrLodMask) !=
        (register_value(kTxtAddrCntl) & kTxtAddrLodMask))
        return false;
    // TXTLODBASE1..3 are material identity too: the footprint recovery and
    // authored-chain decode that produced this record consumed the active LOD
    // quad, so a draw whose coarser bases differ cannot sample this decode.
    // Only the bases the consumed prefix actually spans count; an inactive
    // slot (TXTADDRCNTL LODMAX) must not cost a re-decode, matching the
    // recovery's own inactive-slot rule.
    const uint32_t live_lod_bases[3] = {
        register_value(0x6418u), register_value(0x641cu),
        register_value(0x6420u)};
    const uint32_t active_lods =
        std::min(4u, (register_value(kTxtAddrCntl) & kTxtAddrLodMask) + 1u);
    // The materializer's sanity gate for the active prefix and, for a
    // LODMAX-0 state, the coherent full-quad fallback: strictly increasing
    // in-TRAM bases starting at or after the blob origin. Nothing outside
    // that prefix determines the decoded shape or its authored chain.
    const auto consumed_lod_prefix = [](uint32_t base0, uint32_t base1,
                                        uint32_t base2, uint32_t base3,
                                        uint32_t active, uint32_t blob_base) {
        constexpr uint64_t kMaxTramBytes = 4u * 1024u * 1024u;
        const uint32_t bases[4] = {base0, base1, base2, base3};
        if (bases[0] < blob_base) return 0u;
        const uint32_t limit = active >= 2u ? active : 4u;
        for (uint32_t k = 0; k + 1u < limit; ++k) {
            if (bases[k] >= bases[k + 1u]) return 0u;
        }
        if (bases[limit - 1u] >= kMaxTramBytes) return 0u;
        return limit;
    };
    const uint32_t live_prefix = consumed_lod_prefix(
        lod_base, live_lod_bases[0], live_lod_bases[1], live_lod_bases[2],
        active_lods, record.staged_blob_base);
    const uint32_t staged_prefix = consumed_lod_prefix(
        record.staged_decode_base, record.staged_lod_bases[0],
        record.staged_lod_bases[1], record.staged_lod_bases[2], active_lods,
        record.staged_blob_base);
    for (uint32_t i = 1u; i < 4u; ++i) {
        if (i >= live_prefix && i >= staged_prefix) continue;
        if (live_lod_bases[i - 1u] != record.staged_lod_bases[i - 1u])
            return false;
    }
    // A PIP selection change is *not* a re-decode trigger: the resident keeps
    // its stored indices, and refresh_mmdma_palette() has already re-expanded
    // them under the draw's TXTPIPCNTL before this call. TXTPIPCNTL is
    // draw/use state, not part of the TRAM texel transfer.
    return true;
}

GuestCltInterpreter::ArmedDecode
GuestCltInterpreter::serve_for_draw(Texture& record, uint32_t lod_base,
                                    bool require_ssb_mask)
{
    ArmedDecode result;
    result.record = &record;
    result.cel = record.cel;
    result.width = record.width;
    result.height = record.height;
    result.ssb_nonzero = record.ssb_nonzero;
    result.publisher = record.publisher;
    result.resident_staged = record.staged_valid && !record.staged_bytes.empty();
    if (armed_decode_matches(record, lod_base)) {
        if (require_ssb_mask && ensure_ssb_mask_for(record))
            result.ssb_mask = record.ssb_mask;
        return result;
    }

    ArmedDecodeEntry key;
    key.serial = record.staged_serial;
    key.lod_base = lod_base;
    key.exp_type = register_value(kTxtExpType);
    key.uv_max = register_value(kTxtLdWidth);
    key.uv_mask = register_value(0x6430u);
    key.addr_cntl = register_value(kTxtAddrCntl) & kTxtAddrLodMask;
    key.lod_base1 = register_value(0x6418u);
    key.lod_base2 = register_value(0x641cu);
    key.lod_base3 = register_value(0x6420u);
    key.txt_count = register_value(kTxtCount);
    key.pip_control = register_value(kTxtPipCntl);
    key.pip_hash = m_pip_hash;
    key.pip_generation = m_pip_generation;
    for (ArmedDecodeEntry& entry : m_armed_decodes) {
        if (!entry.same_key(key)) continue;
        if (require_ssb_mask && entry.ssb_mask == kInvalidCel)
            entry.ssb_mask = upload_ssb_mask(entry.width, entry.height,
                                             entry.ssb);
        result.cel = entry.cel;
        result.ssb_mask = entry.ssb_mask;
        result.width = entry.width;
        result.height = entry.height;
        result.ssb_nonzero = entry.ssb_nonzero;
        return result;
    }

    // Stage the resident's retained TRAM bytes as if they had just been DMA'd
    // at the base they were loaded to, and let the materializer decode them
    // with the *armed* registers: its sizing, format and PIP handling are the
    // contract this draw has to honor. The pending slots are borrowed and
    // restored, because a draw can legitimately happen between a TLD and its
    // TXTEXPTYPE write with a live staged blob of its own. A raw-row resident
    // borrows its TLD's row geometry too: those bits are its only geometry,
    // and the materializer reads them with this draw's format.
    const bool saved_pending = m_pending_mmdma_texture;
    const uint32_t saved_source_bit_offset = m_pending_mmdma_source_bit_offset;
    const uint32_t saved_base = m_pending_mmdma_destination_base;
    const uint64_t saved_hash = m_pending_mmdma_hash;
    const bool saved_linear = m_pending_linear_rows;
    const uint32_t saved_row_bits = m_pending_linear_row_bits;
    const uint32_t saved_row_count = m_pending_linear_row_count;
    std::swap(m_pending_mmdma_bytes, record.staged_bytes);
    m_pending_mmdma_texture = true;
    m_pending_mmdma_source_bit_offset = record.staged_source_bit_offset;
    m_pending_mmdma_destination_base = record.staged_blob_base;
    m_pending_mmdma_hash = record.staged_blob_hash;
    m_pending_linear_rows = record.staged_linear_rows;
    m_pending_linear_row_bits = record.staged_linear_row_bits;
    m_pending_linear_row_count = record.staged_linear_row_count;
    MaterializeResult decoded;
    const bool ok = materialize_mmdma_texture_impl(false, &decoded);
    std::swap(m_pending_mmdma_bytes, record.staged_bytes);
    m_pending_mmdma_texture = saved_pending;
    m_pending_mmdma_source_bit_offset = saved_source_bit_offset;
    m_pending_mmdma_destination_base = saved_base;
    m_pending_mmdma_hash = saved_hash;
    m_pending_linear_rows = saved_linear;
    m_pending_linear_row_bits = saved_row_bits;
    m_pending_linear_row_count = saved_row_count;
    // A draw aimed into the middle of a resident may only be re-decoded when
    // its own registers say what the footprint is. When they do not (a stale
    // use state whose LOD quad does not bracket the drawn base), the only
    // sizes available are guesses calibrated for a blob that starts at its own
    // base -- keep the resident's own decode instead of inventing one.
    if (!ok || decoded.cel == kInvalidCel || decoded.width <= 0 ||
        decoded.height <= 0 || !decoded.sized_from_state) {
        result.declined = true;
        return result;
    }

    result.cel = decoded.cel;
    result.width = decoded.width;
    result.height = decoded.height;
    result.ssb_nonzero = std::any_of(
        decoded.ssb.begin(), decoded.ssb.end(),
        [](uint8_t value) { return value != 0u; });
    result.armed = true;
    result.publisher = record.publisher;
    if (m_armed_decodes.size() >= 32u) {
        if (m_backend && m_armed_decodes.front().cel != kInvalidCel)
            m_backend->cel_free(m_armed_decodes.front().cel);
        if (m_backend && m_armed_decodes.front().ssb_mask != kInvalidCel)
            m_backend->cel_free(m_armed_decodes.front().ssb_mask);
        m_armed_decodes.erase(m_armed_decodes.begin());
    }
    key.cel = decoded.cel;
    key.ssb = std::move(decoded.ssb);
    key.width = decoded.width;
    key.height = decoded.height;
    key.ssb_nonzero = result.ssb_nonzero;
    if (require_ssb_mask)
        key.ssb_mask = upload_ssb_mask(key.width, key.height, key.ssb);
    result.ssb_mask = key.ssb_mask;
    m_armed_decodes.push_back(std::move(key));
    return result;
}

void GuestCltInterpreter::release_armed_decodes()
{
    for (const ArmedDecodeEntry& entry : m_armed_decodes) {
        if (m_backend && entry.cel != kInvalidCel)
            m_backend->cel_free(entry.cel);
        if (m_backend && entry.ssb_mask != kInvalidCel)
            m_backend->cel_free(entry.ssb_mask);
    }
    m_armed_decodes.clear();
}

const GuestCltInterpreter::MmdmaDecodeValue*
GuestCltInterpreter::find_cached_mmdma_decode(
    const MmdmaDecodeKey& key)
{
    for (auto it = m_mmdma_decode_cache.begin();
         it != m_mmdma_decode_cache.end(); ++it) {
        if (it->first != key) continue;
        // Keep hot material near the back. Moving a cache entry only moves
        // its vectors (not the decoded bytes), and callers copy the value
        // immediately before any later cache mutation can occur.
        if (it + 1 != m_mmdma_decode_cache.end()) {
            auto entry = std::move(*it);
            m_mmdma_decode_cache.erase(it);
            m_mmdma_decode_cache.emplace_back(std::move(entry));
            return &m_mmdma_decode_cache.back().second;
        }
        return &it->second;
    }
    return nullptr;
}

void GuestCltInterpreter::store_cached_mmdma_decode(
    const MmdmaDecodeKey& key, MmdmaDecodeValue&& value)
{
    // Bound both entry count and total resident CPU storage so the cache can
    // never grow into a meaningful fraction of guest memory. A race frame
    // materializes ~50 residents and decodes each MIP level separately, so the
    // distinct-decode working set is ~130 entries; the former 64-entry window
    // evicted every resident before the guest's next-frame re-DMA could reuse
    // it (measured: 58,991 misses / 3,899 hits, with only 37 near-palette and
    // 0 near-blob candidates in the cache). The window now covers several
    // frames; oversized or unbounded repeats simply keep decoding as before.
    constexpr size_t kMaxCachedDecodes = 512u;
    constexpr size_t kMaxCachedBytes = 128u * 1024u * 1024u;
    constexpr size_t kMaxCachedTexels = 256u * 1024u;
    if (value.pixels.size() > kMaxCachedTexels) return;
    const auto value_bytes = [](const MmdmaDecodeValue& cached) {
        return cached.pixels.capacity() * sizeof(uint32_t) +
               cached.ssb_bits.capacity() * sizeof(uint8_t) +
               cached.palette_indices.capacity() * sizeof(uint8_t) +
               cached.palette_alpha.capacity() * sizeof(uint8_t) +
               cached.source_alpha.capacity() * sizeof(uint8_t) +
               cached.source_ssb.capacity() * sizeof(uint8_t) +
               cached.palette_source_ssb.capacity() * sizeof(uint8_t);
    };
    const size_t bytes = value_bytes(value);
    if (bytes > kMaxCachedBytes) return;
    for (auto it = m_mmdma_decode_cache.begin();
         it != m_mmdma_decode_cache.end(); ++it) {
        if (it->first == key) {
            m_mmdma_decode_cache_bytes -= value_bytes(it->second);
            m_mmdma_decode_cache.erase(it);
            break;
        }
    }
    while (m_mmdma_decode_cache.size() >= kMaxCachedDecodes ||
           m_mmdma_decode_cache_bytes + bytes > kMaxCachedBytes) {
        m_mmdma_decode_cache_bytes -= value_bytes(
            m_mmdma_decode_cache.front().second);
        m_mmdma_decode_cache.pop_front();
    }
    m_mmdma_decode_cache.emplace_back(key, std::move(value));
    m_mmdma_decode_cache_bytes += bytes;
}

const GuestCltInterpreter::MmdmaDecodeValue*
GuestCltInterpreter::find_cached_load_decode(const LoadDecodeKey& key)
{
    for (auto it = m_load_decode_cache.begin();
         it != m_load_decode_cache.end(); ++it) {
        if (it->first != key) continue;
        if (it + 1 != m_load_decode_cache.end()) {
            auto entry = std::move(*it);
            m_load_decode_cache.erase(it);
            m_load_decode_cache.emplace_back(std::move(entry));
            return &m_load_decode_cache.back().second;
        }
        return &it->second;
    }
    return nullptr;
}

void GuestCltInterpreter::store_cached_load_decode(const LoadDecodeKey& key,
                                                   MmdmaDecodeValue&& value)
{
    // Heavy-track pass: the 64-entry cap bound before the byte cap
    // for race working sets (hundreds of distinct texture loads per lap), so
    // entries were evicted and re-decoded as soon as useful again. 256 entries
    // stay inside the same 64 MiB byte cap, so memory is unchanged; only the
    // FIFO window widens. See the MMDMA decode-cache window fix for the same
    // streaming-working-set reasoning.
    constexpr size_t kMaxCachedDecodes = 256u;
    constexpr size_t kMaxCachedBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxCachedTexels = 256u * 1024u;
    if (value.pixels.size() > kMaxCachedTexels) return;
    const auto value_bytes = [](const MmdmaDecodeValue& cached) {
        return cached.pixels.capacity() * sizeof(uint32_t) +
               cached.ssb_bits.capacity() * sizeof(uint8_t) +
               cached.palette_indices.capacity() * sizeof(uint8_t) +
               cached.palette_alpha.capacity() * sizeof(uint8_t) +
               cached.source_alpha.capacity() * sizeof(uint8_t) +
               cached.source_ssb.capacity() * sizeof(uint8_t) +
               cached.palette_source_ssb.capacity() * sizeof(uint8_t);
    };
    const size_t bytes = value_bytes(value);
    if (bytes > kMaxCachedBytes) return;
    for (auto it = m_load_decode_cache.begin();
         it != m_load_decode_cache.end(); ++it) {
        if (it->first == key) {
            m_load_decode_cache_bytes -= value_bytes(it->second);
            m_load_decode_cache.erase(it);
            break;
        }
    }
    while (m_load_decode_cache.size() >= kMaxCachedDecodes ||
           m_load_decode_cache_bytes + bytes > kMaxCachedBytes) {
        m_load_decode_cache_bytes -=
            value_bytes(m_load_decode_cache.front().second);
        m_load_decode_cache.pop_front();
    }
    m_load_decode_cache.emplace_back(key, std::move(value));
    m_load_decode_cache_bytes += bytes;
}

const GuestCltInterpreter::MmdmaMaterialValue*
GuestCltInterpreter::find_cached_mmdma_material(
    const MmdmaMaterialKey& key)
{
    for (auto it = m_mmdma_material_cache.begin();
         it != m_mmdma_material_cache.end(); ++it) {
        if (it->first != key) continue;
        if (it + 1 != m_mmdma_material_cache.end()) {
            auto entry = std::move(*it);
            m_mmdma_material_cache.erase(it);
            m_mmdma_material_cache.emplace_back(std::move(entry));
            return &m_mmdma_material_cache.back().second;
        }
        return &it->second;
    }
    return nullptr;
}

void GuestCltInterpreter::store_cached_mmdma_material(
    const MmdmaMaterialKey& key, MmdmaMaterialValue&& value)
{
    // Same widening as the load-decode cache: 256 entries inside
    // the unchanged 64 MiB byte cap.
    constexpr size_t kMaxCachedMaterials = 256u;
    constexpr size_t kMaxCachedBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxCachedTexels = 256u * 1024u;
    if (value.pixels.size() > kMaxCachedTexels) return;
    const auto value_bytes = [](const MmdmaMaterialValue& cached) {
        size_t bytes = cached.pixels.capacity() * sizeof(uint32_t) +
                       cached.ssb_bits.capacity() * sizeof(uint8_t) +
                       cached.coarse_pixels.capacity() *
                           sizeof(std::vector<uint32_t>);
        for (const std::vector<uint32_t>& level : cached.coarse_pixels)
            bytes += level.capacity() * sizeof(uint32_t);
        return bytes;
    };
    const size_t bytes = value_bytes(value);
    if (bytes > kMaxCachedBytes) return;
    for (auto it = m_mmdma_material_cache.begin();
         it != m_mmdma_material_cache.end(); ++it) {
        if (it->first == key) {
            m_mmdma_material_cache_bytes -= value_bytes(it->second);
            m_mmdma_material_cache.erase(it);
            break;
        }
    }
    while (m_mmdma_material_cache.size() >= kMaxCachedMaterials ||
           m_mmdma_material_cache_bytes + bytes > kMaxCachedBytes) {
        m_mmdma_material_cache_bytes -=
            value_bytes(m_mmdma_material_cache.front().second);
        m_mmdma_material_cache.pop_front();
    }
    m_mmdma_material_cache.emplace_back(key, std::move(value));
    m_mmdma_material_cache_bytes += bytes;
}

// DB-source 2D copy blit. The guest arms DBSRCCNTL / DBSRCBASEADDR /
// DBSRCXSTRIDE and draws an untextured fullscreen quad whose texels are
// copied from the named source buffer (a rgb555 bitmap, e.g. the recovered
// 576x432 main-menu background). The source bytes are snapshotted by
// collect_texture_memory so the render thread never reads guest-owned memory.
bool GuestCltInterpreter::load_db_source_texture(
    const CltMemorySnapshot* memory,
    const std::vector<CltVertex>& vertices)
{
    const bool source32 = (register_value(kDbSrcCntl) & kDbSrc32Bpp) != 0;
    const auto coefficient_uses_ssb = [](uint32_t word) {
        return (word & 0x34u) == 0x24u;
    };
    const bool need_metadata = source32 || (register_value(kDbSsbDsbCntl) & 3u) == 2u ||
        register_value(kDbDestAlphaCntl) == 9u ||
        coefficient_uses_ssb(register_value(kDbAMultCntl)) ||
        coefficient_uses_ssb(register_value(kDbBMultCntl));
    const uint32_t bytes_per_pixel = source32 ? 4u : 2u;
    if (m_db_source.cel != kInvalidCel && m_db_source.db_source_32bpp != source32)
        free_texture_entry(m_db_source);
    const bool source_msb_replicate =
        (register_value(kDbSrcCntl) & 0x2u) != 0u;
    const uint32_t width =
        register_value(kDbSrcXStride) & kDbSrcXStrideMask;
    constexpr uint32_t kDbSourceRows = 432u;
    const uint32_t base = register_value(kDbSrcBaseAddr);
    if (base == 0 || width == 0 || width > kDbSrcXStrideMask ||
        static_cast<uint64_t>(width) * kDbSourceRows > kMaxTexturePixels)
        return false;

    // The guest arms the DB-source in one list and draws the background quad
    // over many lists without re-snapshotting, so the current list's memory
    // snapshot may not hold the source buffer. Keep the cel resident and
    // refresh it when the underlying storage target changes, not merely when
    // a transition biases DBSRCBASEADDR within that same target.
    //
    // The State 2 strip transition blits from the double-buffered display
    // carves, which the host renders to GL but never fills in guest RAM. When
    // readback is enabled and the armed base lands inside a registered display
    // carve (tolerating the strip walk around the anchor), the cel is keyed to
    // the retained presented frame's generation so each present rebuilds it.
    //
    // The recovered display-carve path (default-on; set
    // M2_DBSRC_INPROGRESS=0 for the guest-RAM A/B diagnostic) takes precedence
    // over retained-frame readback: the race-era CAR/576 covering quads sample
    // the display carve after earlier guest world lists rasterize into the
    // host framebuffer, so the cel is built at DB-draw time and keyed to the
    // state generation.
    bool carve_hit = false;
    bool offscreen_hit = false;
    uint32_t source_rows = kDbSourceRows;
    uint32_t carve_target = 0;
    int64_t carve_rel = 0;
    constexpr int64_t kSamplePad = 0x800;
    // How far a non-row-aligned armed base may sit from the allocation it
    // names. A strip/scroll walk stays within a row (measured 1,074-1,114 B);
    // a whole-row bias is recognized separately below, and an origin
    // compensated through DBSRCOFFSET resolves onto its own origin.
    constexpr int64_t kOriginPad = 0x800;
    const uint32_t source_offset = register_value(kDbSrcOffset);
    const int64_t source_offset_pixels_y = sign_extend_12(source_offset);
    const int64_t source_offset_pixels_x =
        sign_extend_12(source_offset >> 16u);
    const float source_offset_x =
        static_cast<float>(source_offset_pixels_x);
    const float source_offset_y =
        static_cast<float>(source_offset_pixels_y);
    // DBSRCBASEADDR, the signed DBSRCOFFSET and the vertex coordinates form
    // one linear TE address: base + 2 * ((y + off_y) * width + x + off_x).
    // Resolve the address the destination origin samples before classifying,
    // so a recipe that writes a genuine offset still names the allocation it
    // reads. The transitions' whole-row bias is not one of those: they leave
    // DBSRCOFFSET at zero and compensate in the quad instead (see below).
    const int64_t resolved_base =
        static_cast<int64_t>(base) +
        bytes_per_pixel * (source_offset_pixels_y * static_cast<int64_t>(width) +
             source_offset_pixels_x);
    // M2_DBSRC_CLASSIFY_TRACE: bounded, opt-in record of the origin decision
    // for every DB-source materialization. It prints the armed base, the signed
    // offset, the resolved origin, each candidate span's window test and
    // vertex-sample test, and the outcome, so a draw that falls through to
    // guest memory can be attributed instead of inferred.
    static const bool classify_trace = [] {
        const char* value = std::getenv("M2_DBSRC_CLASSIFY_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    bool trace_span_window[8] = {false, false, false, false,
                                 false, false, false, false};
    bool trace_span_samples[8] = {false, false, false, false,
                                  false, false, false, false};
    size_t trace_span_count = 0;
    for (const CltFbSpan& span : m_fb_spans) {
        if (span.size != kDbSourceRows * 576u * bytes_per_pixel) continue;
        if (trace_span_count < 8u) ++trace_span_count;
        const int64_t candidate = resolved_base -
                                  static_cast<int64_t>(span.ea);
        // An allocation is named by the armed base when the drawn samples land
        // in it and that base is its origin, sits inside the strip-walk guard
        // around its edges, or is biased from it by a whole number of source
        // rows. The whole-row form is the transition recipes' own shape: the
        // lifted effects arm `base +/- 48 rows` and compensate with the draw's
        // source coordinates, leaving DBSRCOFFSET at the zero value they write
        // once at setup (`FN_dogridtrans__35d7c` 0x35e9c,
        // `FN_dosquare1trans__32010` 0x32140). Requiring the resolved origin to
        // fall inside the tight window rejected the negated half of that pair,
        // which then read guest memory instead of the addressed plane -- the
        // stale frame the racer sees in a transition. A non-row-aligned origin
        // still needs the guard, which is what keeps the Configure Race MPEG
        // overlay on its guest-memory path: its live arm (DBSRCBASEADDR
        // 0x526e94, DBSRCOFFSET 0, stride 576) sits 238 rows below the display
        // plane, and the addresses its 204x128 quad samples fall inside the
        // *next* 576x432 allocation (the Z/movie plane at 0x56a000, which the
        // display chain walk adds as a sibling). That displacement is 238 rows
        // plus 620 bytes, i.e. deliberately not a whole-row bias of the
        // sampled plane. Sampling that span's renderer target would show its
        // contents (menu or seeded image) instead of the decoded movie frame.
        const int64_t armed_bias =
            static_cast<int64_t>(base) - static_cast<int64_t>(span.ea);
        const int64_t source_pitch = static_cast<int64_t>(width) * bytes_per_pixel;
        const bool row_biased_origin =
            source_pitch > 0 && armed_bias % source_pitch == 0;
        const bool window_span =
            row_biased_origin ||
            (candidate >= -kOriginPad &&
             candidate < static_cast<int64_t>(span.size) + kOriginPad);
        // Biased sources need the sample guard; clipped sprite corners cannot
        // change an explicitly named display plane's identity.
        bool samples_span = !vertices.empty();
        for (const CltVertex& vertex : vertices) {
            const int64_t sx = static_cast<int64_t>(std::floor(
                vertex.x + source_offset_x));
            const int64_t sy = static_cast<int64_t>(std::floor(
                vertex.y + source_offset_y));
            const int64_t sampled = static_cast<int64_t>(base) +
                                    bytes_per_pixel * (sy * static_cast<int64_t>(width) + sx);
            const int64_t sampled_rel =
                sampled - static_cast<int64_t>(span.ea);
            if (sampled_rel < -kSamplePad ||
                sampled_rel >=
                    static_cast<int64_t>(span.size) + kSamplePad) {
                samples_span = base == span.ea && width == 576u &&
                               source_offset == 0u;
                break;
            }
        }
        if (trace_span_count >= 1u && trace_span_count <= 8u) {
            trace_span_window[trace_span_count - 1u] = window_span;
            trace_span_samples[trace_span_count - 1u] = samples_span;
        }
        if (!window_span || !samples_span) continue;
        carve_hit = true;
        carve_target = span.ea;
        // The stored displacement is the armed base's, not the resolved one:
        // draw coordinates add the signed offset separately (see the linear
        // source-coordinate build in the vertex path).
        carve_rel = static_cast<int64_t>(base) -
                    static_cast<int64_t>(span.ea);
        break;
    }
    // Report only the arm the origin window rejects while every vertex sample
    // still lands inside that span: that is the shape where the compensating
    // walk lives in the draw's source coordinates rather than in the signed
    // DBSRCOFFSET, and it is the only case where the origin window alone
    // decides a read that the sampled addresses would have placed in a carve.
    bool window_rejected_samples_inside = false;
    if (classify_trace && !carve_hit && !vertices.empty()) {
        size_t index = 0;
        for (const CltFbSpan& span : m_fb_spans) {
            if (span.size != kDbSourceRows * 576u * bytes_per_pixel) continue;
            if (index >= 8u) break;
            if (!trace_span_window[index] && trace_span_samples[index]) {
                window_rejected_samples_inside = true;
                break;
            }
            ++index;
        }
    }
    if (window_rejected_samples_inside) {
        static std::atomic<uint32_t> classify_logged{0};
        const uint32_t n = classify_logged.fetch_add(1u);
        if (n < 512u) {
            std::fprintf(stderr,
                         "[dbsrc-classify] host=%u base=%#x offxy=(%lld,%lld) "
                         "resolved=%#llx spans=%zu dest=%#x stride=%u "
                         "windows=",
                         m_host_frame, base,
                         (long long)source_offset_pixels_x,
                         (long long)source_offset_pixels_y,
                         (unsigned long long)resolved_base, trace_span_count,
                         register_value(kDbDestBaseAddr), width);
            size_t index = 0;
            for (const CltFbSpan& span : m_fb_spans) {
                if (span.size != kDbSourceRows * 576u * bytes_per_pixel) continue;
                if (index >= 8u) break;
                std::fprintf(
                    stderr, " %#x:%s%s", span.ea,
                    trace_span_window[index] ? "w" : "-",
                    trace_span_samples[index] ? "s" : "-");
                ++index;
            }
            std::fprintf(stderr, "\n");
        } else if (n == 512u) {
            std::fprintf(stderr,
                         "[dbsrc-classify] cap reached — further entries "
                         "suppressed\n");
        }
    }
    // A non-display Bitmap can also be a renderer-owned DB source. The
    // rear-view pass renders a 128x40 Bitmap, then TwoD_DrawBitmap copies it
    // with a destination-compensating base bias. Its registered byte span
    // and source stride define the rows; it is never the retained scanout.
    // Only a draw whose source origin names a previously drawn target qualifies,
    // so ordinary guest-RAM bitmaps keep their immutable snapshot path.
    if (!carve_hit) {
        for (const CltFbSpan& span : m_fb_spans) {
            if (span.size == kDbSourceRows * 576u * bytes_per_pixel ||
                span.size == 0u || span.size % (width * bytes_per_pixel) != 0u)
                continue;
            const uint32_t rows = span.size / (width * bytes_per_pixel);
            if (rows > 2048u ||
                static_cast<uint64_t>(width) * rows > kMaxTexturePixels)
                continue;
            if (vertices.empty()) continue;
            float min_x = vertices.front().x, max_x = min_x;
            float min_y = vertices.front().y, max_y = min_y;
            for (const CltVertex& vertex : vertices) {
                min_x = std::min(min_x, vertex.x);
                max_x = std::max(max_x, vertex.x);
                min_y = std::min(min_y, vertex.y);
                max_y = std::max(max_y, vertex.y);
            }
            // The original bitmap blitter subtracts the destination's
            // linear byte offset from DBSRCBASEADDR, leaving DBSRCOFFSET
            // zero. Resolve that origin before testing allocation identity.
            const int64_t sampled_origin = resolved_base +
                bytes_per_pixel * (static_cast<int64_t>(std::floor(min_y)) * width +
                     static_cast<int64_t>(std::floor(min_x)));
            if (sampled_origin != span.ea || max_x - min_x > width ||
                max_y - min_y > rows)
                continue;
            const bool rendered = std::any_of(
                m_target_write_generations.begin(),
                m_target_write_generations.end(),
                [&span](const auto& entry) { return entry.first == span.ea; });
            if (!rendered) continue;
            carve_hit = offscreen_hit = true;
            carve_target = span.ea;
            carve_rel = static_cast<int64_t>(base) - span.ea;
            source_rows = rows;
            break;
        }
    }
    bool served_from_retained = false;
    if (carve_hit && !offscreen_hit && !inprogress_readback_enabled() &&
        fb_readback_enabled() && !m_retained_frame.empty()) {
        served_from_retained = true;
    }
    uint32_t want_kind = served_from_retained ? 1u : 0u;
    uint32_t want_seq = served_from_retained ? m_retained_seq : 0u;
    if (carve_hit && inprogress_readback_enabled()) {
        want_kind = 2u;
        // A same-list draw into the sampled region since the target's last
        // snapshot invalidates it. Only the unbiased same-position read is
        // bounded by vertex extent; any biased read is conservative.
        for (const DirtyBounds& dirty : m_target_dirty_bounds) {
            if (dirty.target != carve_target) continue;
            bool overlaps = offscreen_hit || carve_rel != 0 ||
                            source_offset != 0u || vertices.empty();
            if (!overlaps) {
                float min_x = vertices.front().x, max_x = min_x;
                float min_y = vertices.front().y, max_y = min_y;
                for (const CltVertex& vertex : vertices) {
                    min_x = std::min(min_x, vertex.x);
                    max_x = std::max(max_x, vertex.x);
                    min_y = std::min(min_y, vertex.y);
                    max_y = std::max(max_y, vertex.y);
                }
                // Dirty bounds are recorded from the submitted vertices,
                // after the viewport origin is applied; these are still
                // viewport-relative. Comparing them unshifted missed the
                // Hall of Fame stripe's overlap with the header drawn just
                // before it (raw y 67-98 against 114-141), so the stripe
                // reused an earlier snapshot and erased the header's top.
                if (m_viewport_valid) {
                    const float left = static_cast<float>(m_viewport_left);
                    const float top = static_cast<float>(m_viewport_top);
                    min_x += left;
                    max_x += left;
                    min_y += top;
                    max_y += top;
                }
                overlaps = min_x < dirty.max_x && dirty.min_x < max_x &&
                           min_y < dirty.max_y && dirty.min_y < max_y;
            }
            if (overlaps) {
                ++m_db_source_generation;
                note_destination_write(carve_target);
            }
            break;
        }
        // Conservatively use the current state generation when this target
        // has never been an interpreter destination. Once observed, its last
        // possible-write generation is the exact source identity.
        want_seq = static_cast<uint32_t>(m_db_source_generation);
        for (const auto& [target, generation] :
             m_target_write_generations) {
            if (target == carve_target) {
                want_seq = generation;
                break;
            }
        }
    }
    const uint32_t storage_base =
        want_kind != 0u && carve_hit ? carve_target : base;
    if (want_kind != 0u && m_db_source.cel != kInvalidCel && (!need_metadata || m_db_source.ssb_mask != kInvalidCel) &&
        m_db_source.db_source_storage_base == storage_base &&
        m_db_source.db_source_stride == width &&
        m_db_source.height == static_cast<int>(source_rows) &&
        m_db_source.db_source_msb_replicate == source_msb_replicate &&
        m_db_source.db_source_kind == want_kind &&
        m_db_source.db_source_seq == want_seq) {
        m_db_source.db_source_base = base;
        m_db_source.db_source_origin_pixels = carve_rel / bytes_per_pixel;
        return true;
    }
    // The snapshot taken below observes every draw recorded so far.
    if (want_kind == 2u)
        m_target_dirty_bounds.erase(
            std::remove_if(m_target_dirty_bounds.begin(),
                           m_target_dirty_bounds.end(),
                           [carve_target](const DirtyBounds& dirty) {
                               return dirty.target == carve_target;
                           }),
            m_target_dirty_bounds.end());

    const size_t source_bytes =
        static_cast<size_t>(width) * source_rows * bytes_per_pixel;
    int64_t source_origin_pixels = 0;
    // The GL target is a renderbuffer. Snapshot its addressed display window
    // into a separate cel before this draw, then let the existing source
    // coordinates apply stride, signed offsets and row carry in the shader.
    // Registered offscreen rows use the same viewport-to-host mapping as
    // their earlier draw, with their own dimensions instead of the scanout.
    if (want_kind == 2u && (width == 576u || offscreen_hit) && m_backend) {
        int fb_w = 0, fb_h = 0;
        m_backend->get_size(&fb_w, &fb_h);
        const int disp_w = m_display_width > 0 ? m_display_width : 640;
        const int disp_h = m_display_height > 0 ? m_display_height : 480;
        if (fb_w > 0 && fb_h > 0) {
            const float sx = static_cast<float>(fb_w) / disp_w;
            const float sy = static_cast<float>(fb_h) / disp_h;
            const float org_x = m_viewport_valid
                                    ? static_cast<float>(m_viewport_left)
                                    : 32.f;
            const float org_y = m_viewport_valid
                                    ? static_cast<float>(m_viewport_top)
                                    : 24.f;
            const int x = static_cast<int>(std::floor(org_x * sx));
            const int y = static_cast<int>(std::floor(org_y * sy));
            const int w = static_cast<int>(std::ceil(width * sx));
            const int h = static_cast<int>(std::ceil(source_rows * sy));
            if (x >= 0 && y >= 0 && x + w <= fb_w && y + h <= fb_h) {
                CelId snapshot = kInvalidCel;
                {
                    perf::Scope renderer_scope(perf::Phase::RendererWork);
                    perf::Scope copy_scope(perf::Phase::GpuFeedbackCopy);
                    snapshot = m_backend->clt_snapshot_target_cel(
                        carve_target, x, y, w, h,
                        static_cast<int>(width), static_cast<int>(source_rows), need_metadata);
                }
                CelId metadata = kInvalidCel;
                if (snapshot != kInvalidCel && need_metadata)
                    metadata = m_backend->clt_snapshot_target_ssb_cel(carve_target, x, y, w, h,
                        static_cast<int>(width), static_cast<int>(source_rows));
                if (snapshot != kInvalidCel && need_metadata && metadata == kInvalidCel) {
                    m_backend->cel_free(snapshot); snapshot = kInvalidCel;
                }
                if (snapshot != kInvalidCel) {
                    perf::add_counter(perf::Counter::GpuFeedbackSnapshots);
                    free_texture_entry(m_db_source);
                    m_db_source.ssb_mask = metadata;
                    m_db_source.db_source_32bpp = source32;
                    m_db_source.cel = snapshot;
                    m_db_source.width = static_cast<int>(width);
                    m_db_source.height = static_cast<int>(source_rows);
                    m_db_source.db_source_base = base;
                    m_db_source.db_source_kind = 2u;
                    m_db_source.db_source_storage_base = carve_target;
                    m_db_source.db_source_stride = width;
                    m_db_source.db_source_origin_pixels = carve_rel / bytes_per_pixel;
                    m_db_source.db_source_msb_replicate =
                        source_msb_replicate;
                    m_db_source.db_source_seq = want_seq;
                    m_db_source.db_source_checksum = 0u;
                    return true;
                }
            }
        }
    }
    if (need_metadata && carve_hit) return false;
    // Display-carve textures retain the allocation's stable 576x432 origin.
    // DBSRCBASEADDR's bias is carried into draw coordinates below rather than
    // rebasing the uploaded texture and clamping compensating negative source
    // coordinates at its edge.
    const uint8_t* source_data = nullptr;
    uint64_t source_content_hash = 0;
    bool served_from_inprogress = false;
    // A carve read in in-progress mode that cannot be served silently falls
    // through to the retained or guest-RAM path, and the host never fills a
    // display carve in guest RAM, so that draw renders a frozen copy of the
    // carve. Keep the fallback observable instead of indistinguishable from a
    // served snapshot.
    const bool inprogress_requested = (want_kind == 2u);
    if (want_kind == 2u &&
        snapshot_db_source_from_target(
            carve_target, 0, source_bytes, false,
            offscreen_hit ? static_cast<int>(width) : 576,
            offscreen_hit ? static_cast<int>(source_rows) : 432)) {
        served_from_inprogress = true;
        source_origin_pixels = carve_rel / bytes_per_pixel;
        source_data = m_db_source_window.data();
    } else {
        if (want_kind == 2u) {
            // No readable in-progress framebuffer (e.g. null backend):
            // fail over to the retained/guest-RAM path for this draw.
            served_from_retained = carve_hit && !offscreen_hit && fb_readback_enabled() &&
                                   !m_retained_frame.empty();
            want_kind = served_from_retained ? 1u : 0u;
            want_seq = served_from_retained ? m_retained_seq : 0u;
            const uint32_t fallback_storage_base =
                want_kind != 0u && carve_hit ? carve_target : base;
            if (want_kind != 0u && m_db_source.cel != kInvalidCel && (!need_metadata || m_db_source.ssb_mask != kInvalidCel) &&
                m_db_source.db_source_storage_base ==
                    fallback_storage_base &&
                m_db_source.db_source_stride == width &&
                m_db_source.db_source_msb_replicate == source_msb_replicate &&
                m_db_source.db_source_kind == want_kind &&
                m_db_source.db_source_seq == want_seq) {
                m_db_source.db_source_base = base;
                m_db_source.db_source_origin_pixels = carve_rel / bytes_per_pixel;
                return true;
            }
        }
        if (served_from_retained) {
            source_origin_pixels = carve_rel / bytes_per_pixel;
            if (m_db_source_window.capacity() < source_bytes)
                perf::add_counter(perf::Counter::DbSourceBufferGrowths);
            m_db_source_window.assign(source_bytes, 0);
            for (size_t i = 0; i < source_bytes; ++i) {
                if (i < m_retained_frame.size())
                    m_db_source_window[i] = m_retained_frame[i];
            }
            source_data = m_db_source_window.data();
        } else {
            const CltMemoryView source =
                find_memory(memory, base, source_bytes);
            if (!source.data || source.size < source_bytes) {
                if (bridge_trace_enabled())
                    std::fprintf(stderr,
                                 "[dbsrc-fb] source-miss base=%#x carve-hit=%d target=%#x rel=%lld want=%u\n",
                                 base, carve_hit ? 1 : 0, carve_target,
                                 static_cast<long long>(carve_rel), want_kind);
                // The arm list carries the source bytes; later draw-only
                // lists may not. Reuse only the cel for this exact guest-RAM
                // storage identity when no fresh snapshot is available.
                return m_db_source.cel != kInvalidCel && (!need_metadata || m_db_source.ssb_mask != kInvalidCel) &&
                       m_db_source.db_source_base == base &&
                       m_db_source.db_source_stride == width &&
                       m_db_source.db_source_msb_replicate ==
                           source_msb_replicate &&
                       m_db_source.db_source_kind == 0u;
            }
            source_data = source.data;
            if (source.size == source_bytes)
                source_content_hash = source.content_hash;
        }
    }  // else: in-progress snapshot path did not serve this draw

    // Enqueue-time snapshots already hash an exact guest range while copying
    // it. Reuse that identity here instead of rescanning a 576x432 (or wider)
    // source for every draw-only list. Hand-built tests and containing/partial
    // views do not carry an exact snapshot hash, so retain the old checksum
    // fallback for those callers.
    const uint64_t source_checksum =
        want_kind == 0u
            ? (source_content_hash != 0
                   ? source_content_hash
                   : static_cast<uint64_t>(
                         clt_guest_checksum(source_data, source_bytes)))
            : 0u;
    if (want_kind == 0u && m_db_source.cel != kInvalidCel && (!need_metadata || m_db_source.ssb_mask != kInvalidCel) &&
        m_db_source.db_source_base == base &&
        m_db_source.db_source_stride == width &&
        m_db_source.db_source_msb_replicate == source_msb_replicate &&
        m_db_source.db_source_kind == 0u &&
        m_db_source.db_source_checksum == source_checksum) {
        return true;
    }

    const size_t row_pitch = static_cast<size_t>(width) * bytes_per_pixel;
    const size_t pixel_count = static_cast<size_t>(width) * source_rows;
    if (m_db_source_pixels.capacity() < pixel_count)
        perf::add_counter(perf::Counter::DbSourceBufferGrowths);
    m_db_source_pixels.resize(pixel_count);
    std::vector<uint32_t> source_metadata(pixel_count);
    for (uint32_t y = 0; y < source_rows; ++y) {
        const uint8_t* row = source_data + static_cast<size_t>(y) * row_pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = size_t(y) * width + x;
            if (source32) {
                const uint32_t word = read_be32(row + x * 4u);
                // DSB/A7/R8/G8/B8 literal word; widen the stored alpha
                // with the existing host MSB-replication rule, independently of DSB.
                const uint32_t alpha = replicate_msb(7u, word >> 24u);
                m_db_source_pixels[index] = ((word >> 16u) & 255u) | (word & 0xff00u) |
                    ((word & 255u) << 16u) | (alpha << 24u);
                source_metadata[index] = 0xff000000u | (alpha << 8u) |
                    ((word & 0x80000000u) ? 255u : 0u);
                continue;
            }
            const uint16_t w =
                (static_cast<uint16_t>(row[x * 2]) << 8) | row[x * 2 + 1];
            source_metadata[index] = 0xff00ff00u | ((w & 0x8000u) ? 255u : 0u);
            const uint32_t red = (w >> 10u) & 0x1fu;
            const uint32_t green = (w >> 5u) & 0x1fu;
            const uint32_t blue = w & 0x1fu;
            // Toolkit DBSRCCNTL.SRCMSBREP selects replicated low bits versus
            // zero fill for the 16-bpp source expansion. Preserve the guest's
            // selection even though both forms agree after 5-bit destination
            // quantization.
            const uint32_t r = clt_db_source_expand_5_to_8(
                red, source_msb_replicate);
            const uint32_t g = clt_db_source_expand_5_to_8(
                green, source_msb_replicate);
            const uint32_t b = clt_db_source_expand_5_to_8(
                blue, source_msb_replicate);
            // RGB555 has a DSB bit but no stored alpha. Its selection
            // bit travels separately; opaque color alpha is not evidence
            // for the hardware's source-alpha synthesis on this format.
            m_db_source_pixels[static_cast<size_t>(y) * width + x] =
                r | (g << 8u) | (b << 16u) | (0xffu << 24u);
        }
    }

    CelId cel = kInvalidCel;
    {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        if (m_backend)
            cel = m_backend->cel_upload_rgba_ex_hashed(
                static_cast<int>(width), static_cast<int>(source_rows),
                m_db_source_pixels.data(), false, false,
                rgba_content_hash(m_db_source_pixels));
    }
    if (m_backend && cel == kInvalidCel) return false;
    CelId metadata = kInvalidCel;
    if (m_backend && need_metadata) {
        metadata = m_backend->cel_upload_rgba_ex_hashed(width, source_rows,
            source_metadata.data(), false, false, rgba_content_hash(source_metadata));
        if (metadata == kInvalidCel) { m_backend->cel_free(cel); return false; }
    }
    if (m_backend && m_db_source.ssb_mask != kInvalidCel)
        m_backend->cel_free(m_db_source.ssb_mask);
    m_db_source.ssb_mask = metadata;
    m_db_source.db_source_32bpp = source32;

    if (m_db_source.cel != kInvalidCel && (!need_metadata || m_db_source.ssb_mask != kInvalidCel) && m_backend) {
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        m_backend->cel_free(m_db_source.cel);
    }
    m_db_source.cel = cel;
    m_db_source.width = static_cast<int>(width);
    m_db_source.height = static_cast<int>(source_rows);
    m_db_source.db_source_base = base;
    m_db_source.db_source_kind =
        served_from_inprogress ? 2u : want_kind;
    m_db_source.db_source_storage_base =
        m_db_source.db_source_kind != 0u && carve_hit ? carve_target : base;
    m_db_source.db_source_stride = width;
    m_db_source.db_source_origin_pixels = source_origin_pixels;
    m_db_source.db_source_msb_replicate = source_msb_replicate;
    m_db_source.db_source_seq = want_seq;
    m_db_source.db_source_checksum =
        m_db_source.db_source_kind == 0u ? source_checksum : 0u;
    if (bridge_trace_enabled() && served_from_retained) {
        std::fprintf(stderr,
                     "[dbsrc-fb] retained frame seq=%u base=%#x w=%u "
                     "bytes=%zu\n",
                     m_retained_seq, base, width, source_bytes);
    }
    if (bridge_trace_enabled() && served_from_inprogress) {
        static std::atomic<uint32_t> inprog_logged{0};
        static bool inprog_expired = false;
        static const uint32_t inprog_from = [] {
            const char* v = std::getenv("M2_DRAW_DIAG_FROM");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
        }();
        const uint64_t frame_now =
            perf::counter_value(perf::Counter::GuestFramesStarted);
        if (frame_now < inprog_from) return true;
        const uint32_t n = inprog_logged.fetch_add(1);
        // 256 covers a full authored transition (30 display units, one DB draw
        // each) so a whole sequence can be attributed; still bounded.
        if (n < 256u) {
            std::fprintf(stderr,
                         "[dbsrc-fb] inprogress exec=%llu base=%#x w=%u "
                         "bytes=%zu nonclear=%u%% host=%u dest=%#x "
                         "g5=%llu dbuser=%#x blend=%d\n",
                         static_cast<unsigned long long>(m_exec_seq), base,
                         width, source_bytes, m_db_inprog_nonclear_pct,
                         m_host_frame, register_value(kDbDestBaseAddr),
                         static_cast<unsigned long long>(
                             m_db_inprog_green_mean),
                         register_value(kDbUserControl),
                         static_cast<int>(m_destination_blend_mode));
        } else if (!inprog_expired) {
            inprog_expired = true;
            std::fprintf(stderr,
                         "[dbsrc-fb] inprogress cap reached — further entries "
                         "suppressed\n");
        }
    }
    if (bridge_trace_enabled() && inprogress_requested &&
        !served_from_inprogress) {
        static std::atomic<uint32_t> fallback_logged{0};
        const uint32_t n = fallback_logged.fetch_add(1);
        if (n < 256u) {
            std::fprintf(stderr,
                         "[dbsrc-fb] inprogress-fallback host=%u base=%#x "
                         "carve=%#x kind=%u\n",
                         m_host_frame, base, carve_target, want_kind);
        }
    }
    return true;
}

bool GuestCltInterpreter::load_texture(const CltMemorySnapshot* memory)
{
    const uint32_t load_control = register_value(kTxtLdCntl);
    const uint32_t mode =
        (load_control >> kTxtLdModeShift) & kTxtLdModeMask;
    bool ok = false;
    const char* fail_reason = nullptr;
    if (!clt_tex_load_control_documented(load_control)) {
        // TXTLDCNTL carries the documented field set only (bits 0..2 and
        // 8..10).  All three vendor authoring forms compose their payload out
        // of those masked field bits, so a load control with reserved bits
        // set or with the compression bit armed for a non-texture load mode is
        // not a programmed state: reject the load instead of dispatching it.
        fail_reason = "txtldcntl-undocumented-form";
    } else if (mode == 2u) ok = load_pip(memory);
    else if (mode == 1u)
        ok = load_mmdma_texture(memory, &fail_reason);
    else if (mode == 0u && (load_control & kTxtLdCompressed) != 0)
        ok = load_compressed_texture(memory, &fail_reason);
    else if (mode == 0u) ok = load_uncompressed_texture(memory);
    static const bool texfail_diag = [] {
        const char* e = std::getenv("M2_DRAW_DIAG");
        return e && *e && *e != '0';
    }();
    static const uint32_t texfail_from = [] {
        const char* e = std::getenv("M2_DRAW_DIAG_FROM");
        return e ? static_cast<uint32_t>(strtoul(e, nullptr, 0)) : 0u;
    }();
    const uint64_t texfail_frame =
        perf::counter_value(perf::Counter::GuestFramesStarted);
    static uint32_t texfail_emitted = 0;
    static bool texfail_expired = false;
    if (texfail_emitted >= 32u && !texfail_expired) {
        texfail_expired = true;
        std::fprintf(stderr,
                     "[clt-tex-fail] cap reached at frame %llu — further "
                     "entries suppressed\n",
                     (unsigned long long)texfail_frame);
    }
    if (!ok && (bridge_trace_enabled() ||
                (texfail_diag && texfail_frame >= texfail_from)) &&
        texfail_emitted < 32u) {
        ++texfail_emitted;
        std::fprintf(stderr,
                     "[clt-tex-fail] mode=%u ld_cntl=%#x src=%#x dst=%#x count=%u ld_width=%#x exp_type=%#x src01=%#x src23=%#x pip=%#x reason=%s\n",
                     mode, load_control, register_value(kTxtLdSrcAddr),
                     register_value(kTxtLodBase0), register_value(kTxtCount),
                     register_value(kTxtLdWidth), register_value(kTxtExpType),
                     register_value(kTxtSrcType01),
                     register_value(kTxtSrcType23),
                     register_value(kTxtPipCntl),
                     fail_reason ? fail_reason : "-");
        if (texfail_emitted == 32u && !texfail_expired) {
            texfail_expired = true;
            std::fprintf(stderr,
                         "[clt-tex-fail] cap reached at frame %llu — "
                         "further entries suppressed\n",
                         (unsigned long long)texfail_frame);
        }
    }
    return ok;
}

// Executed-list sequence liveness (diagnostic-only): the sequence advances
// once per execute() call only while a list-gated diagnostic is armed
// (M2_CLT_Z_CELL_DUMP, M2_CLT_Z_SURFACE_TRACE, or the M2_CLT_DRAW_LIST
// filter). Otherwise m_exec_seq stays frozen at zero, so a diagnostic FROM
// floor keeps meaning the same thing in every run and only armed runs observe
// advancing list numbers.
//
// This counter is not the in-progress DB-source cel key. Keying the cel to a
// frozen sequence made the cache reuse a snapshot across executes whenever
// the armed source base and stride were unchanged, and a display-carve read
// re-arms the same base while the carve advances underneath it — so production
// served the first snapshot indefinitely. load_db_source_texture keys the cel
// to m_db_source_generation, which advances at execute entry and at a nonzero
// mid-list DBDESTBASEADDR write.
bool clt_exec_seq_live()
{
    return clt_z_cell_probe::dump_word_count() != 0 ||
           clt_z_surface_trace_enabled() || clt_diag_list_filter() != UINT64_MAX;
}

// Exactly one sequence bump per execute() call on every return path,
// including the malformed/unsupported early returns, so each executed list
// owns one sequence number. The dump gate (entry) and the register trace
// (in-body) therefore observe the same number for the same list, and
// numbering stays 0-based at entry.
struct CltExecSeqBump {
    explicit CltExecSeqBump(uint64_t& seq, bool live) : m_seq(seq), m_live(live)
    {
    }
    ~CltExecSeqBump()
    {
        if (m_live) ++m_seq;
    }
    uint64_t& m_seq;
    bool m_live;
};

// Lightweight monotonic clock and scoped accumulator for the bounded
// per-list section attribution above.
uint64_t clt_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct CltSectionTimer {
    bool active = false;
    uint64_t* accumulator = nullptr;
    uint64_t start = 0;
    CltSectionTimer(bool enabled, uint64_t* target)
        : active(enabled), accumulator(target),
          start(enabled ? clt_now_ns() : 0)
    {
    }
    ~CltSectionTimer()
    {
        if (active && accumulator) *accumulator += clt_now_ns() - start;
    }
};

CltExecutionResult GuestCltInterpreter::execute(
    const uint32_t* words, size_t word_count,
    const CltMemorySnapshot* memory, uint32_t command_list_base)
{
    CltExecutionResult result;
    result.execute_seq = m_exec_seq;
    const CltExecSeqBump exec_seq_bump(m_exec_seq, clt_exec_seq_live());
    // Stamp the addressed destination at list entry before any draw. This is
    // deliberately conservative (a no-draw list may invalidate), but unlike
    // a global per-list key it does not force a readback of an untouched
    // sibling source target on every frame of a square transition.
    ++m_db_source_generation;
    note_destination_write(register_value(kDbDestBaseAddr));
    // Bounded per-list section attribution (M2_FRAME_TIMING, diagnostic only):
    // separates register-write processing from vertex/draw emission so a heavy
    // list is attributed instead of guessed.
    static const bool exec_section_timing = [] {
        return std::getenv("M2_FRAME_TIMING") != nullptr;
    }();
    // These gates are static environment lookups, but register-heavy State 3
    // lists visit them thousands of times. Snapshot the immutable diagnostic
    // state once per list so the production register loop only does the
    // relevant name lookup when a trace is actually armed.
    const bool exec_z_surface_trace = clt_z_surface_trace_enabled();
    const bool exec_super_trace = clt_super_trace_enabled();
    const bool livery_diag_enabled_for_execute = livery_draw_diag_enabled();
    const uint32_t draw_prov_wanted = draw_prov_frame();
    const bool draw_prov_for_execute =
        (draw_prov_wanted != UINT32_MAX && m_host_frame == draw_prov_wanted) ||
        draw_prov_serve_enabled();
    const bool config_texture_trace_for_execute =
        config_texture_trace_enabled();
    // Draw diagnostics are process-lifetime environment gates. Snapshot
    // them once per list so the production path does not repeatedly enter
    // static getenv guards (or read the guest-frame counter) for every
    // emitted triangle when diagnostics are off.
    static const bool draw_diag_for_execute = [] {
        const char* e = std::getenv("M2_DRAW_DIAG");
        return e && *e && *e != '0';
    }();
    static const uint32_t draw_diag_from_for_execute = [] {
        const char* e = std::getenv("M2_DRAW_DIAG_FROM");
        return e ? static_cast<uint32_t>(strtoul(e, nullptr, 0)) : 0u;
    }();
    const uint64_t exec_section_start =
        exec_section_timing ? clt_now_ns() : 0;
    uint64_t exec_reg_ns = 0;
    uint64_t exec_vtx_ns = 0;
    uint64_t exec_load_ns = 0;
    uint64_t exec_mat_ns = 0;
    uint32_t exec_load_calls = 0;
    uint32_t exec_mat_calls = 0;
    uint64_t exec_load_ns_by_mode[4] = {0, 0, 0, 0};
    uint32_t exec_load_calls_by_mode[4] = {0, 0, 0, 0};
    uint64_t exec_flush_ns = 0;
    uint32_t exec_flush_calls = 0;
    // Bounded per-list section attribution emission. Production race lists end
    // in a DCCNTL PSE register that returns through the stop_after_register
    // path, so the emission must run on that return too; otherwise
    // M2_FRAME_TIMING silently omits the heavy lists it exists to attribute.
    const auto emit_exec_section_timing = [&]() {
        if (!exec_section_timing) return;
        static uint32_t exec_timing_lines = 0;
        if (exec_timing_lines >= 2400u) return;
        const uint64_t total_ns = clt_now_ns() - exec_section_start;
        std::fprintf(stderr,
                     "[exec-timing] n=%u words=%zu reg_ms=%.2f vtx_ms=%.2f "
                     "load_ms=%.2f load_calls=%u "
                     "m0_ms=%.2f m0_n=%u m1_ms=%.2f m1_n=%u "
                     "m2_ms=%.2f m2_n=%u m3_ms=%.2f m3_n=%u "
                     "mat_ms=%.2f mat_n=%u flush_ms=%.2f flush_n=%u "
                     "vb=%u tris=%u draws=%u "
                     "total_ms=%.2f\n",
                     exec_timing_lines, word_count, exec_reg_ns / 1e6,
                     exec_vtx_ns / 1e6, exec_load_ns / 1e6,
                     exec_load_calls,
                     exec_load_ns_by_mode[0] / 1e6,
                     exec_load_calls_by_mode[0],
                     exec_load_ns_by_mode[1] / 1e6,
                     exec_load_calls_by_mode[1],
                     exec_load_ns_by_mode[2] / 1e6,
                     exec_load_calls_by_mode[2],
                     exec_load_ns_by_mode[3] / 1e6,
                     exec_load_calls_by_mode[3],
                     exec_mat_ns / 1e6, exec_mat_calls,
                     exec_flush_ns / 1e6, exec_flush_calls,
                     result.vertex_batches, result.triangles, result.draws,
                     total_ns / 1e6);
        ++exec_timing_lines;
    };
    const auto time_load_texture = [&]() {
        const uint32_t load_control = register_value(kTxtLdCntl);
        const uint32_t mode =
            (load_control >> kTxtLdModeShift) & kTxtLdModeMask;
        const size_t mode_slot = mode < 4u ? mode : 3u;
        const uint64_t start = exec_section_timing ? clt_now_ns() : 0;
        const bool ok = load_texture(memory);
        if (exec_section_timing) {
            const uint64_t elapsed = clt_now_ns() - start;
            exec_load_ns += elapsed;
            ++exec_load_calls;
            exec_load_ns_by_mode[mode_slot] += elapsed;
            ++exec_load_calls_by_mode[mode_slot];
        }
        return ok;
    };
    const auto time_materialize = [&]() {
        const uint64_t start = exec_section_timing ? clt_now_ns() : 0;
        const bool ok = materialize_mmdma_texture();
        if (exec_section_timing) {
            exec_mat_ns += clt_now_ns() - start;
            ++exec_mat_calls;
        }
        return ok;
    };
    const auto mmdma_register_state_ready = [&]() {
        if (!m_pending_mmdma_texture || m_pending_mmdma_bytes.empty())
            return false;

        // The SDK's MMDMA builder writes TXTEXPTYPE before the use-list's
        // TXTPIPCNTL in the common indexed path.  A PIP load also temporarily
        // uses TXTLODBASE0=TEPIPRAM.  Neither intermediate register state is
        // a drawable MMDMA use state; defer materialization to the draw
        // boundary, where the guest has supplied all selectors and the LOD
        // base points into this blob.
        const TextureFormat format =
            decode_format(register_value(kTxtExpType), 0);
        const bool indexed_or_color_alpha =
            is_indexed_tram_format(format) ||
            is_color_alpha_ssb_format(format);
        const bool literal_format = is_no_color_literal_format(format) ||
                                    is_literal_rgb888_format(format);
        if (m_pending_linear_rows) {
            uint32_t bits_per_texel = 0u;
            if (is_rgb555_ssb_format(format)) {
                bits_per_texel = 16u;
            } else if (is_literal_rgb888_format(format)) {
                bits_per_texel =
                    24u +
                    (format.has_alpha && format.alpha_depth > 0u
                         ? format.alpha_depth
                         : 0u) +
                    (format.has_ssb ? 1u : 0u);
            } else if (is_no_color_literal_format(format) ||
                       is_indexed_no_color_tram_format(format)) {
                bits_per_texel =
                    (format.has_alpha && format.alpha_depth > 0u
                         ? format.alpha_depth
                         : 0u) +
                    (format.has_ssb ? 1u : 0u);
            } else if (is_indexed_tram_format(format)) {
                bits_per_texel =
                    format.color_depth +
                    (format.has_alpha && format.alpha_depth > 0u
                         ? format.alpha_depth
                         : 0u) +
                    (format.has_ssb ? 1u : 0u);
            }
            // A row staged because the TLD-time format could not describe
            // it remains incomplete until a compatible use format arrives.
            // Do not turn that stale register snapshot into an unsupported
            // packet merely because the next load supersedes an unused
            // resident; a real draw still materializes and fails closed.
            if (bits_per_texel == 0u ||
                m_pending_linear_row_bits == 0u ||
                (m_pending_linear_row_bits % bits_per_texel) != 0u)
                return false;
        }
        if (indexed_or_color_alpha || literal_format) {
            const PipControl pip =
                decode_pip_control(register_value(kTxtPipCntl));
            if (!pip.documented || pip.ssb_select > kPipSelectPip ||
                pip.alpha_select > kPipSelectPip ||
                pip.color_select > kPipSelectPip)
                return false;
            // A non-literal indexed resident has no independent RGB stream;
            // its color must wait for the guest's use-state PIP/constant
            // selector. Literal RGB888 can supply texture color, and a
            // colorless literal has no RGB bits (the materializer handles
            // those two cases separately), so neither literal form is
            // rejected merely because the prior selector says TEXTURE.
            if (indexed_or_color_alpha &&
                pip.color_select == kPipSelectTexture)
                return false;
        }

        const uint64_t blob_begin = m_pending_mmdma_destination_base;
        const uint64_t blob_end = blob_begin + m_pending_mmdma_bytes.size();
        const uint64_t draw_base = register_value(kTxtLodBase0);
        return draw_base >= blob_begin && draw_base < blob_end;
    };

    // OpenGL can consume all independent TE triangles in a contiguous
    // register-state run with one GL_TRIANGLES submission. The guest still
    // emits and accounts for each vertex packet independently; this queue is
    // only a host-side presentation optimization. A register header is a
    // hard boundary (flushed below), so the generation key represents the
    // complete guest-derived draw state rather than a guessed material key.
    uint64_t draw_state_generation = 0;
    bool cached_depth_valid = false;
    uint64_t cached_depth_generation = 0;
    CltDepth cached_depth_state{};
    bool cached_blend_valid = false;
    uint64_t cached_blend_generation = 0;
    CltBlendState cached_blend_state{};
    const auto depth_state_for_generation = [&]() -> const CltDepth& {
        if (!cached_depth_valid ||
            cached_depth_generation != draw_state_generation) {
            cached_depth_state = guest_depth_state();
            cached_depth_generation = draw_state_generation;
            cached_depth_valid = true;
        }
        return cached_depth_state;
    };
    const auto blend_state_for_generation = [&]() -> const CltBlendState& {
        if (!cached_blend_valid ||
            cached_blend_generation != draw_state_generation) {
            cached_blend_state = destination_blend_state();
            cached_blend_generation = draw_state_generation;
            cached_blend_valid = true;
        }
        return cached_blend_state;
    };
    bool pending_batch_active = false;
    uint64_t pending_batch_generation = 0;
    bool pending_batch_textured = false;
    CelId pending_batch_cel = kInvalidCel;
    bool pending_batch_blend_enabled = false;
    CltDiscard pending_batch_discard{};
    CltDepth pending_batch_depth{};
    CltTextureSampling pending_batch_sampling{};
    CltBlendState pending_batch_blend_state{};
    const auto flush_pending_batch = [&]() {
        if (!pending_batch_active || m_pending_vertices_scratch.empty() ||
            !m_backend)
            return;
        const uint64_t flush_start = exec_section_timing ? clt_now_ns() : 0;
        const auto [width, height] =
            target_size(m_display_width, m_display_height, m_backend);
        perf::Scope renderer_scope(perf::Phase::RendererWork);
        // Keep every submission triangle-aligned. The existing per-header
        // path used a 16-bit vertex bound; retain that bound while allowing a
        // whole compatible run to share the upload/state setup.
        constexpr size_t kMaxBatchVertices = 65532u;
        for (size_t first = 0; first < m_pending_vertices_scratch.size();
             first += kMaxBatchVertices) {
            const uint32_t batch_count = static_cast<uint32_t>(std::min(
                kMaxBatchVertices, m_pending_vertices_scratch.size() - first));
            m_backend->clt_draw(
                m_pending_vertices_scratch.data() + first, batch_count, false,
                pending_batch_textured, pending_batch_cel, width, height,
                pending_batch_blend_enabled, pending_batch_discard,
                pending_batch_depth, pending_batch_sampling,
                pending_batch_blend_state, true);
        }
        m_pending_vertices_scratch.clear();
        pending_batch_active = false;
        if (exec_section_timing) {
            exec_flush_ns += clt_now_ns() - flush_start;
            ++exec_flush_calls;
        }
    };
    const auto append_pending_vertices =
        [&](const CltVertex* vertices, size_t vertex_count, bool textured_draw,
            CelId cel, bool blend, const CltDiscard& discard,
            const CltDepth& depth, CltTextureSampling sampling,
            const CltBlendState& blend_state) {
            if (!m_backend || vertex_count == 0) return;
            note_destination_draw(vertices, vertex_count);
            const bool batch_supported =
                m_backend->clt_supports_triangle_batches();
            if (!batch_supported) {
                const auto [width, height] =
                    target_size(m_display_width, m_display_height, m_backend);
                perf::Scope renderer_scope(perf::Phase::RendererWork);
                for (size_t first = 0; first < vertex_count; first += 3u) {
                    m_backend->clt_draw(
                        vertices + first, 3u, false, textured_draw, cel,
                        width, height, blend, discard, depth, sampling,
                        blend_state);
                }
                return;
            }

            // Every register packet flushed the queue and advanced this key.
            // The remaining fields distinguish draw variants that can be
            // selected without a register write (for example a DB-source
            // texture versus a regular cel). All other backend state is
            // decoded from the same register generation and is retained in
            // the pending state for the eventual submission.
            const bool compatible =
                pending_batch_active &&
                pending_batch_generation == draw_state_generation &&
                pending_batch_textured == textured_draw &&
                pending_batch_cel == cel &&
                pending_batch_blend_enabled == blend &&
                pending_batch_blend_state.tab.enabled == blend_state.tab.enabled &&
                pending_batch_blend_state.texture_alpha_as_color ==
                    blend_state.texture_alpha_as_color &&
                pending_batch_blend_state.secondary_source ==
                    blend_state.secondary_source &&
                pending_batch_blend_state.source_ssb_mask == blend_state.source_ssb_mask &&
                pending_batch_blend_state.db_source_linear ==
                    blend_state.db_source_linear &&
                pending_batch_blend_state.db_source_height ==
                    blend_state.db_source_height;
            if (!compatible) {
                flush_pending_batch();
                pending_batch_active = true;
                pending_batch_generation = draw_state_generation;
                pending_batch_textured = textured_draw;
                pending_batch_cel = cel;
                pending_batch_blend_enabled = blend;
                pending_batch_discard = discard;
                pending_batch_depth = depth;
                pending_batch_sampling = sampling;
                pending_batch_blend_state = blend_state;
            }

            constexpr size_t kMaxBatchVertices = 65532u;
            // NEW packets are assembled into the reusable member scratch
            // before reaching this helper.  When a compatible run starts
            // with an empty queue, transfer that buffer instead of copying
            // every vertex a second time.  The software/diagnostic path
            // returned above still receives its original per-triangle calls.
            if (vertices == m_new_vertices_scratch.data() &&
                vertex_count == m_new_vertices_scratch.size() &&
                m_pending_vertices_scratch.empty() &&
                vertex_count <= kMaxBatchVertices) {
                m_pending_vertices_scratch.swap(m_new_vertices_scratch);
                m_new_vertices_scratch.clear();
                return;
            }
            // A rollover flush clears pending_batch_active but keeps the
            // batch state, so rearm it before the rest of this packet is
            // appended; otherwise the final flush would drop the tail.
            const auto flush_and_rearm = [&]() {
                flush_pending_batch();
                pending_batch_active = true;
            };
            size_t first = 0;
            while (first < vertex_count) {
                const size_t room =
                    kMaxBatchVertices - m_pending_vertices_scratch.size();
                size_t take = std::min(room, vertex_count - first);
                take -= take % 3u;
                if (take == 0u) {
                    flush_and_rearm();
                    continue;
                }
                m_pending_vertices_scratch.insert(
                    m_pending_vertices_scratch.end(), vertices + first,
                    vertices + first + take);
                first += take;
                if (m_pending_vertices_scratch.size() ==
                    kMaxBatchVertices)
                    flush_and_rearm();
            }
        };
    if (!words && word_count != 0) {
        result.ok = false;
        return result;
    }
    // State generation for the in-progress DB snapshot cache: draws in
    // one list share an at-draw-time framebuffer sample until a nonzero
    // DBDESTBASEADDR write creates a new destination-state boundary.
    // Guest-RAM Z-cell dump (M2_CLT_Z_CELL_DUMP): single read-only read of
    // the bound Z plane at entry from the submit-time snapshot (the enqueue
    // path snapshots the dump window when DUMP is set); persistent registers
    // already hold the armed base/stride from earlier setup lists.
    maybe_dump_z_cells_once(m_exec_seq, memory,
                             register_value(kDbZBaseAddr),
                             register_value(kDbDestXStride) & 0xffffu);

    // Single-list word dump (P2 walls): M2_CLT_LIST_DUMP=<path> writes the
    // full word stream of the M2_CLT_DRAW_LIST-selected executed list for
    // offline dissection (analyze_clt_dump.py workflow). Exactly one list,
    // bounded by the word-count guard.
    if (clt_diag_list_filter() != UINT64_MAX &&
        m_exec_seq == clt_diag_list_filter()) {
        static const char* list_dump_path = [] {
            const char* e = std::getenv("M2_CLT_LIST_DUMP");
            return (e && *e) ? e : "";
        }();
        if (list_dump_path[0] != '\0' && words != nullptr && word_count > 0 &&
            word_count <= 4u * 1024u * 1024u) {
            if (FILE* dump_file = std::fopen(list_dump_path, "w")) {
                std::fprintf(dump_file,
                             "# m2-clt-list-dump v1 exec=%llu words=%zu\n",
                             (unsigned long long)m_exec_seq, word_count);
                for (size_t i = 0; i < word_count; ++i) {
                    std::fprintf(dump_file, "%08x%c", words[i],
                                 ((i + 1u) % 8u == 0u || i + 1u == word_count)
                                     ? '\n'
                                     : ' ');
                }
                std::fclose(dump_file);
            }
        }
    }

    size_t cursor = 0;
    size_t flow_jump_count = 0;
    // A malformed/hostile list must not turn a copied command stream into an
    // unbounded interpreter loop. Keep the guard proportional to the copied
    // list rather than allowing a tiny cyclic list to burn a million passes.
    constexpr size_t kFlowJumpSlack = 1024u;
    const size_t max_flow_jumps =
        word_count > (std::numeric_limits<size_t>::max() - kFlowJumpSlack) / 4u
            ? std::numeric_limits<size_t>::max()
            : word_count * 4u + kFlowJumpSlack;
    // ---- Cross-list control flow -------------------------------------
    // The vendor links command lists with an absolute jump in both
    // directions and carries no length prefix, so a list the walk
    // executes can be a different buffer from the submitted one:
    // font/telists.c DrawText() writes CLT_JumpAbsolute into the app's
    // GState list pointing at the TextState's TE list ("We will then add
    // a JMP instruction to the App's GState to execute these
    // instructions", CreateTELists), and the TE list's reserved ts_TERet
    // slot jumps back to the GState continuation. The Mercury primitive
    // path (M_Draw.c: "ltf += JUMP_WORDS; /* Now point to the return
    // address */") and the clipped-text path (font/clip.c: "Jump in to
    // these instructions" / "And jump out of the clip instructions")
    // build the same call/return pair. gstate.c:511 goes further and
    // re-aims a GState list at another list outright. The walk therefore
    // keeps a bounded stack of list views instead of one word stream;
    // frame 0 is the submitted list and owns no caller.
    size_t cross_list_moves = 0;
    struct CltListFrame {
        const uint32_t* words = nullptr;
        size_t word_count = 0;
        uint32_t base = 0;
        // Word in this frame to resume at once the frame above it returns.
        size_t resume_cursor = 0;
        // Set when the walk has left this frame for good. Jumping back
        // into a finished frame is an unterminated loop, not a call.
        bool exited = false;
        // Storage for a list entered from guest RAM. The snapshot holds
        // guest bytes in guest order and never byteswaps them, while the
        // walk consumes big-endian words, exactly as enqueue's own copy of
        // the submitted list does. The frame owns the swapped copy so the
        // pointer stays valid for as long as the frame is on the stack.
        std::vector<uint32_t> owned;
    };
    std::vector<CltListFrame> clt_frames;
    clt_frames.reserve(kMaxCltListDepth + 1u);
    clt_frames.emplace_back();
    clt_frames.back().words = words;
    clt_frames.back().word_count = word_count;
    clt_frames.back().base = command_list_base;
    // Executed-instruction record for the guest's own field loops. While the
    // list walks forward this costs one comparison per packet: every word
    // below `linear_end` was reached by the straight-line walk, so nothing has
    // to be stored. A jump is recorded in `visited_bits` only when it lands
    // above that prefix, which keeps a straight-line list allocation-free.
    size_t linear_end = 0;
    bool took_jump = false;
    std::vector<uint64_t> visited_bits;
    const auto visited_mark = [&](size_t index) {
        if (index < linear_end) return;
        const size_t words = (index >> 6) + 1;
        if (visited_bits.size() < words) visited_bits.resize(words, 0ull);
        visited_bits[index >> 6] |= 1ull << (index & 63u);
    };
    const auto visited_test = [&](size_t index) {
        if (index < linear_end) return true;
        return visited_bits.size() > (index >> 6) &&
               (visited_bits[index >> 6] & (1ull << (index & 63u))) != 0ull;
    };
    // Rebind the walk to another list. The per-view loop bookkeeping
    // (the linear prefix and the visited bitmap) describes the list being
    // left, not this one, so both reset: a word index means nothing until
    // it is qualified by the list it belongs to.
    const auto enter_view = [&](const CltListFrame& frame,
                                size_t word_index) {
        words = frame.words;
        word_count = frame.word_count;
        command_list_base = frame.base;
        cursor = word_index;
        linear_end = 0;
        took_jump = false;
        visited_bits.clear();
    };
    // A list the walk entered from another list ends either at one of its
    // own control-flow instructions, which the loop body handles, or by
    // running out of words. The vendor reserves the return jump in the
    // callee, so a callee that simply runs out hands control back the same
    // way: mark the frame finished, drop it and resume the caller at the
    // word after the jump that entered it. The submitted list, which owns
    // no caller, ends the walk.
    const auto clt_should_keep_walking = [&]() {
        if (cursor < word_count) return true;
        if (clt_frames.size() <= 1u) return false;
        clt_frames.back().exited = true;
        clt_frames.pop_back();
        enter_view(clt_frames.back(), clt_frames.back().resume_cursor);
        return true;
    };
    while (clt_should_keep_walking()) {
        const size_t header_index = cursor;
        const uint32_t header = words[cursor++];
        const uint32_t type = header & kTypeMask;
        if (!took_jump && header_index >= linear_end)
            linear_end = header_index + 1u;
        visited_mark(header_index);

        if (type == kRegisterType) {
            flush_pending_batch();
            ++draw_state_generation;
            CltSectionTimer section_timer(exec_section_timing, &exec_reg_ns);
            uint32_t count = 0, operation = 0, address = 0;
            if (!decode_register_header(header, &count, &operation, &address) ||
                count > word_count - cursor) {
                result.ok = false;
                result.words_consumed = header_index;
                flush_pending_batch();
                return result;
            }

            const uint32_t first = address / sizeof(uint32_t);
            bool window_clip_changed = false;
            bool destination_blend_changed = false;
            size_t flow_target = 0;
            bool flow_target_set = false;
            // A cross-list jump rebinds the walk to another list view; the
            // switch itself happens once this register packet is complete,
            // alongside the in-list jump target.
            size_t cross_enter = 0;
            size_t cross_word_index = 0;
            bool cross_taken = false;
            bool stop_after_register = false;
            for (uint32_t i = 0; i < count; ++i) {
                const uint16_t register_address = static_cast<uint16_t>(
                    address + i * sizeof(uint32_t));
                const uint32_t previous_value = m_registers[first + i];
                const size_t payload_index = cursor;
                const uint32_t written_value = words[cursor];
                // A new load-control write starts the next CLT load snippet.
                // Materialize a preceding MMDMA resident here only when the
                // current register snapshot is a complete drawable use state.
                // CLT_CreatePipCommandList writes TXTLDCNTL first and later
                // points TXTLODBASE0 at TEPIPRAM (0x46000); stale selectors or
                // that intermediate PIP base must defer to the draw boundary.
                if (register_address == kTxtLdCntl &&
                    m_pending_mmdma_texture &&
                    mmdma_register_state_ready() &&
                    !time_materialize()) {
                    if (clt_unsupported_diag_enabled()) {
                        static uint32_t emitted = 0;
                        static bool expired = false;
                        if (emitted < clt_unsupported_diag_max()) {
                            std::fprintf(
                                stderr,
                                "[clt-reject] texture-materialize-before-load "
                                "old_ld=%#x new_ld=%#x src=%#x count=%#x "
                                "uvmax=%#x exptype=%#x pip=%#x lod0=%#x "
                                "blob_base=%#x blob_bytes=%zu linear=%d "
                                "row_bits=%u rows=%u list=%llu frame=%llu\n",
                                register_value(kTxtLdCntl), written_value,
                                register_value(kTxtLdSrcAddr),
                                register_value(kTxtCount),
                                register_value(kTxtLdWidth),
                                register_value(kTxtExpType),
                                register_value(kTxtPipCntl),
                                register_value(kTxtLodBase0),
                                m_pending_mmdma_destination_base,
                                m_pending_mmdma_bytes.size(),
                                m_pending_linear_rows ? 1 : 0,
                                m_pending_linear_row_bits,
                                m_pending_linear_row_count,
                                (unsigned long long)m_exec_seq,
                                (unsigned long long)perf::counter_value(
                                    perf::Counter::GuestFramesStarted));
                            ++emitted;
                        } else if (!expired) {
                            expired = true;
                            std::fprintf(stderr,
                                         "[clt-reject] cap reached — further "
                                         "entries suppressed\n");
                        }
                    }
                    ++result.unsupported;
                }
                apply_register(&m_registers[first + i], words[cursor++],
                               operation);
                if (register_address == kDbDestBaseAddr) {
                    result.dest_base_value = m_registers[first + i];
                    ++result.dest_base_writes;
                    // A destination selected in the middle of a list is a
                    // possible write for the in-progress DB-source identity
                    // just as the list-entry destination is. Advance the
                    // state generation before recording the new target so a
                    // source snapshot taken earlier in this same list cannot
                    // be reused after the write.
                    if (m_registers[first + i] != 0u)
                        ++m_db_source_generation;
                    note_destination_write(m_registers[first + i]);
                    if (m_backend)
                        m_backend->clt_target_select(
                            m_registers[first + i]);
                }
                ++result.register_writes;
                const char* z_surface_name =
                    exec_z_surface_trace
                        ? clt_z_surface_register_name(register_address)
                        : nullptr;
                if (z_surface_name) {
                    static uint32_t emitted = 0;
                    static bool expired = false;
                    // FROM keys on the executed-list sequence so the window is
                    // independent of the guest-frame timebase; m_exec_seq is
                    // live on every path including fast-forward. The printed
                    // frame= field is the GuestFramesStarted guest-frame count,
                    // which advances whether or not M2_PERF is enabled.
                    const uint64_t z_frame = perf::counter_value(
                        perf::Counter::GuestFramesStarted);
                    const bool z_in_window =
                        static_cast<uint64_t>(m_exec_seq) >=
                        clt_z_surface_trace_from();
                    if (z_in_window &&
                        emitted < clt_z_surface_trace_max()) {
                        std::fprintf(
                            stderr,
                            "[clt-z-surface] frame=%llu list=%llu "
                            "hdr_index=%zu hdr=%#x reg=%s addr=%#x "
                            "op=%#x write=%#x previous=%#x value=%#x\n",
                            static_cast<unsigned long long>(z_frame),
                            static_cast<unsigned long long>(m_exec_seq),
                            header_index, header, z_surface_name,
                            register_address,
                            operation, written_value, previous_value,
                            m_registers[first + i]);
                        ++emitted;
                    } else if (!expired && z_in_window &&
                               emitted >= clt_z_surface_trace_max()) {
                        expired = true;
                        std::fprintf(
                            stderr,
                            "[clt-z-surface] cap reached at list %llu "
                            "— further entries suppressed\n",
                            static_cast<unsigned long long>(m_exec_seq));
                    }
                }
                if (register_address == kDbSuperGenControl &&
                    exec_super_trace) {
                    static uint32_t emitted = 0;
                    static bool expired = false;
                    if (emitted < clt_super_trace_max()) {
                        std::fprintf(
                            stderr,
                            "[clt-super] frame=%llu list=%llu "
                            "hdr_index=%zu hdr=%#x op=%#x "
                            "write=%#x previous=%#x value=%#x\n",
                            static_cast<unsigned long long>(
                                perf::counter_value(
                                    perf::Counter::GuestFramesStarted)),
                            static_cast<unsigned long long>(m_exec_seq),
                            header_index, header, operation, written_value,
                            previous_value, m_registers[first + i]);
                        ++emitted;
                    } else if (!expired) {
                        expired = true;
                        std::fprintf(stderr,
                                     "[clt-super] cap reached at list %llu "
                                     "— further entries suppressed\n",
                                     static_cast<unsigned long long>(
                                         m_exec_seq));
                    }
                }
                // Headers decode to canonical addresses here, so a SET/CLEAR
                // that targets DBUSERCONTROL / DBXWINCLIP / DBYWINCLIP (via
                // e.g. 0x9008/0x9808) arrives as kDbUserControl etc. and the
                // OR/AND is already applied by apply_register above.
                if (register_address == kDbUserControl) {
                    window_clip_changed = true;
                    destination_blend_changed = true;
                } else if (register_address == kDbAMultCntl ||
                           register_address == kDbBMultCntl ||
                           register_address == kDbAluCntl || register_address == kDbSrcCntl ||
                           register_address == kDbSrcAlphaCntl || register_address == kDbDestAlphaCntl ||
                           register_address == kDbSsbDsbCntl) {
                    // M_DBSpec and the other vendor blend snippets write
                    // their multiplier/ALU registers separately from
                    // DBUSERCONTROL. Reclassify after those stateful writes;
                    // otherwise a later list can retain an obsolete
                    // source-over/unsupported classification.
                    destination_blend_changed = true;
                } else if (register_address == kDbXWinClip ||
                           register_address == kDbYWinClip) {
                    window_clip_changed = true;
                }
                if (register_address == kDcCntl) {
                    const uint32_t dccntl = m_registers[first + i];
                    const uint32_t instruction_bits = dccntl & kDcFlowMask;
                    const bool reserved_flow_bits =
                        (dccntl & ~kDcFlowMask) != 0u;
                    const bool multiple_instructions =
                        (instruction_bits & (instruction_bits - 1u)) != 0u;
                    const bool has_pause = (dccntl & kDcPse) != 0u;
                    const bool has_interrupt = (dccntl & kDcDint) != 0u;
                    const bool has_jump =
                        (dccntl & (kDcJa | kDcJr)) != 0u;
                    if (reserved_flow_bits || multiple_instructions) {
                        ++result.unsupported;
                        ++result.control_flow_refusals;
                        stop_after_register = true;
                    } else if (has_pause) {
                        // Portfolio's high-latency lists end in PSE. It is a
                        // real source-defined stop, so retain that terminal
                        // form; a mid-list pause would require the device's
                        // restart/IRP continuation boundary and is refused.
                        if (cursor == word_count) {
                            ++result.control_flow_pauses;
                        } else {
                            ++result.unsupported;
                            ++result.control_flow_refusals;
                        }
                        stop_after_register = true;
                    } else if (has_interrupt) {
                        // DINT has an external CPU-interrupt side effect. The
                        // current synchronous host boundary cannot publish it;
                        // never continue linearly through the list as if it
                        // were an ordinary register write.
                        ++result.unsupported;
                        ++result.control_flow_refusals;
                        stop_after_register = true;
                    } else if (has_jump) {
                        const bool relative = (dccntl & kDcJr) != 0u;
                        const CltFlowTarget target = resolve_clt_flow_target(
                            command_list_base, word_count, payload_index,
                            register_value(kDcCntlData), relative);
                        // Attribution for a jump target the walk cannot use
                        // (M2_CLT_UNSUPPORTED_DIAG, capped, no raster effect).
                        // Names the list, the target and the words on both
                        // sides of the jump, which is what distinguishes a
                        // vendor cross-list call from a malformed stream.
                        if ((!target.valid || !target.in_list) &&
                            clt_unsupported_diag_enabled()) {
                            static uint32_t emitted = 0;
                            static bool expired = false;
                            if (emitted < clt_unsupported_diag_max()) {
                                std::fprintf(
                                    stderr,
                                    "[clt-reject] clt-flow-external list=%llu "
                                    "frame=%llu base=%#x words=%zu cursor=%zu "
                                    "data=%#x valid=%d in_list=%d frames=%zu "
                                    "prev=%08x %08x %08x "
                                    "next=%08x %08x %08x\n",
                                    static_cast<unsigned long long>(m_exec_seq),
                                    static_cast<unsigned long long>(
                                        perf::counter_value(
                                            perf::Counter::GuestFramesStarted)),
                                    command_list_base, word_count, cursor,
                                    register_value(kDcCntlData),
                                    target.valid ? 1 : 0,
                                    target.in_list ? 1 : 0,
                                    clt_frames.size(),
                                    cursor >= 3u ? words[cursor - 3u] : 0u,
                                    cursor >= 2u ? words[cursor - 2u] : 0u,
                                    cursor >= 1u ? words[cursor - 1u] : 0u,
                                    cursor < word_count ? words[cursor] : 0u,
                                    cursor + 1u < word_count ? words[cursor + 1u] : 0u,
                                    cursor + 2u < word_count ? words[cursor + 2u] : 0u);
                                ++emitted;
                            } else if (!expired) {
                                expired = true;
                                std::fprintf(stderr,
                                             "[clt-reject] cap reached at list "
                                             "%llu — further entries suppressed\n",
                                             static_cast<unsigned long long>(m_exec_seq));
                            }
                        }
                        if (!target.valid) {
                            ++result.unsupported;
                            ++result.control_flow_refusals;
                            stop_after_register = true;
                        } else if (!target.in_list) {
                            // A jump to another list. Two shapes stay
                            // refused-by-contract, and everything else is
                            // the vendor's call/return pair.
                            //
                            //  * A terminal jump out of the submitted list
                            //    may hand off to a separately submitted
                            //    Portfolio command buffer this boundary
                            //    never sees, so it stays a handoff.
                            //  * A target that is in no list on the stack and
                            //    in none of the guest RAM the submit path
                            //    snapshotted is not a command list this walk
                            //    can read, so it stays unsupported.
                            //
                            // Otherwise the target is either a list already
                            // on the stack -- which is how the reserved
                            // return slot gets back to the caller's
                            // continuation -- or another guest-RAM list, and
                            // the walk enters it. Both are bounded by the
                            // depth and move budgets above, and re-entering a
                            // frame that already finished is refused as the
                            // unterminated loop it is.
                            if (cursor == word_count &&
                                command_list_base != 0u &&
                                clt_frames.size() == 1u) {
                                ++result.control_flow_external_handoffs;
                                stop_after_register = true;
                            } else if (cross_list_moves >= kMaxCrossListMoves ||
                                       clt_frames.size() >= kMaxCltListDepth) {
                                ++result.unsupported;
                                ++result.control_flow_refusals;
                                stop_after_register = true;
                            } else {
                                size_t frame_index = clt_frames.size();
                                for (size_t i = clt_frames.size(); i-- > 0;) {
                                    const uint64_t frame_begin = clt_frames[i].base;
                                    const uint64_t frame_end =
                                        frame_begin +
                                        clt_frames[i].word_count * 4ull;
                                    if (target.address >= frame_begin &&
                                        target.address < frame_end) {
                                        frame_index = i;
                                        break;
                                    }
                                }
                                size_t word_index = 0;
                                if (frame_index < clt_frames.size()) {
                                    // Returning into a list the walk is
                                    // already inside. `target.in_list` is
                                    // false for the current frame, so this
                                    // always discards at least one frame.
                                    if (clt_frames[frame_index].exited) {
                                        ++result.unsupported;
                                        ++result.control_flow_refusals;
                                        stop_after_register = true;
                                    } else {
                                        clt_frames.back().exited = true;
                                        while (clt_frames.size() > frame_index + 1u) {
                                            clt_frames.pop_back();
                                        }
                                        word_index = static_cast<size_t>(
                                            (target.address -
                                             clt_frames[frame_index].base) /
                                            4u);
                                    }
                                } else {
                                    // A list in guest RAM, addressed outside
                                    // every frame on the stack. The submit
                                    // path snapshotted it at the target, so
                                    // the span's own length is the walk's
                                    // bound: a target past the snapshot edge
                                    // stops the walk there rather than
                                    // reading beyond what was copied.
                                    const CltMemoryView view =
                                        find_memory(memory, target.address, 4u);
                                    if (view.data == nullptr) {
                                        ++result.unsupported;
                                        ++result.control_flow_refusals;
                                        stop_after_register = true;
                                    } else {
                                        clt_frames.emplace_back();
                                        CltListFrame& entered = clt_frames.back();
                                        entered.base = target.address;
                                        entered.word_count = view.size / 4u;
                                        // The snapshot is a raw, never-
                                        // byteswapped copy of guest RAM, so
                                        // swap here exactly as enqueue does
                                        // for the submitted list. A target
                                        // with fewer than four bytes left in
                                        // the span is not an addressable
                                        // instruction and stays refused.
                                        if (entered.word_count == 0) {
                                            clt_frames.pop_back();
                                            ++result.unsupported;
                                            ++result.control_flow_refusals;
                                            stop_after_register = true;
                                        } else {
                                            entered.owned.resize(
                                                entered.word_count);
                                            for (size_t w = 0;
                                                 w < entered.word_count; ++w) {
                                                entered.owned[w] = read_be32(
                                                    view.data +
                                                    w * sizeof(uint32_t));
                                            }
                                            entered.words =
                                                entered.owned.data();
                                            clt_frames[clt_frames.size() - 2u]
                                                .resume_cursor = cursor;
                                            word_index = 0;
                                        }
                                    }
                                }
                                if (!stop_after_register) {
                                    ++cross_list_moves;
                                    ++result.control_flow_jumps;
                                    cross_taken = true;
                                    cross_enter = clt_frames.size() - 1u;
                                    cross_word_index = word_index;
                                }
                            }
                        } else if (visited_test(target.word_index)) {
                            // The target instruction already ran in this
                            // submission: the guest authored a field loop. The
                            // showroom's large list draws its scene and then
                            // jumps back to its own first word so the TE keeps
                            // producing that field while the guest builds the
                            // next one; one pass is the field's whole
                            // rasterization and a second pass would only emit
                            // identical pixels. Ending the pass here keeps the
                            // list bounded without refusing a source shape the
                            // guest really authors, and it guarantees the walk
                            // terminates because no instruction runs twice.
                            ++result.control_flow_loops;
                            stop_after_register = true;
                        } else if (flow_jump_count >= max_flow_jumps) {
                            ++result.unsupported;
                            ++result.control_flow_refusals;
                            stop_after_register = true;
                        } else {
                            ++flow_jump_count;
                            ++result.control_flow_jumps;
                            took_jump = true;
                            flow_target = target.word_index;
                            flow_target_set = true;
                        }
                    }
                    // Only TLD (0x1) triggers a texture load; the other
                    // single-bit forms above are handled as control flow.
                    if (!stop_after_register && !flow_target_set &&
                        (dccntl & kDcTld) != 0) {
                        const bool load_ok = time_load_texture();
                        // MMDMA load/use-state order trace (diag only): the
                        // staged-blob result is read after the call, so the
                        // entry shows the pending state going forward.
                        if (clt_unsupported_diag_enabled()) {
                            MmdmaOrderEntry entry;
                            entry.list = m_exec_seq;
                            entry.is_use = false;
                            entry.ld_cntl = register_value(kTxtLdCntl);
                            entry.src = register_value(kTxtLdSrcAddr);
                            entry.count = register_value(kTxtCount);
                            entry.dst = register_value(kTxtLodBase0);
                            entry.exptype = register_value(kTxtExpType);
                            entry.uvmax = register_value(kTxtLdWidth);
                            entry.blob_base =
                                m_pending_mmdma_destination_base;
                            entry.blob_bytes = m_pending_mmdma_bytes.size();
                            entry.ok = load_ok;
                            // Stale-label latch input: an MMDMA TLD staged
                            // under a label that fails every expansion
                            // predicate defers to the following use-state
                            // (world-pack TLDs observe the previous
                            // texture's 0x402 residue). Mode-0 raw-row loads
                            // have their separate linear-row pending marker.
                            const uint32_t trace_mode =
                                (entry.ld_cntl >> kTxtLdModeShift) &
                                kTxtLdModeMask;
                            const TextureFormat trace_format =
                                decode_format(entry.exptype, 0);
                            entry.load_label_unknown =
                                trace_mode == 1u &&
                                !is_indexed_tram_format(trace_format) &&
                                !is_rgb555_ssb_format(trace_format) &&
                                !is_color_alpha_ssb_format(trace_format) &&
                                !is_no_color_literal_format(trace_format) &&
                                !is_literal_rgb888_format(trace_format);
                            log_mmdma_order_entry(entry);
                        }
                        if (!load_ok) {
                        // Silent-unsupported site 1/3 (diag only): TLD-load
                        // failure on the register path never logs (the
                        // [clt-tex-fail] sample inside load_texture needs
                        // bridge/draw-diagnostic gates). Attribute under the dedicated
                        // gate so deep-race unsupported censuses resolve.
                        if (clt_unsupported_diag_enabled()) {
                            static uint32_t emitted = 0;
                            static bool expired = false;
                            if (emitted < clt_unsupported_diag_max()) {
                                std::fprintf(stderr,
                                             "[clt-reject] texture-load "
                                             "dccntl=%#x ld_cntl=%#x src=%#x "
                                             "dst=%#x count=%#x uvmax=%#x "
                                             "exptype=%#x list=%llu frame=%llu\n",
                                             dccntl,
                                             register_value(kTxtLdCntl),
                                             register_value(kTxtLdSrcAddr),
                                             register_value(kTxtLodBase0),
                                             register_value(kTxtCount),
                                             register_value(kTxtLdWidth),
                                             register_value(kTxtExpType),
                                             (unsigned long long)m_exec_seq,
                                             (unsigned long long)perf::counter_value(
                                                 perf::Counter::GuestFramesStarted));
                                ++emitted;
                            } else if (!expired) {
                                expired = true;
                                std::fprintf(stderr,
                                             "[clt-reject] cap reached at frame "
                                             "%llu — further entries suppressed\n",
                                             (unsigned long long)perf::counter_value(
                                                 perf::Counter::GuestFramesStarted));
                            }
                        }
                        ++result.unsupported;
                    }
                    }
                    // A cross-list jump transfers execution to another
                    // list exactly as an in-list jump does, so the rest of
                    // this packet belongs to neither list. Every vendor
                    // JA/JR packet writes DCNTL last (cltmacros.h:642-646
                    // emits DCNTLDATA then DCNTL), so this is inert for
                    // every authored shape and only closes the case where
                    // DCNTL is not the packet's last register.
                    if (stop_after_register || flow_target_set ||
                        cross_taken)
                        break;
                }
                // MMDMA transfers are complete at the TLD write, but the
                // resident texture's dimensions and expansion format come
                // from the following use-state registers. Materialize when
                // the guest finishes that state at TXTEXPTYPE, before the
                // vertex packet can be preceded by another texture's setup.
                if (register_address == kTxtExpType &&
                    m_pending_mmdma_texture) {
                    // Pre-state for the order trace (diag only): the call
                    // below consumes the blob on success, so snapshot first.
                    const uint32_t order_blob_base =
                        m_pending_mmdma_destination_base;
                    const size_t order_blob_bytes =
                        m_pending_mmdma_bytes.size();
                    const uint32_t order_uvmax =
                        register_value(kTxtLdWidth);
                    const uint32_t order_exptype =
                        register_value(kTxtExpType);
                    const bool mat_ready = mmdma_register_state_ready();
                    const bool mat_ok = !mat_ready || time_materialize();
                    if (clt_unsupported_diag_enabled()) {
                        MmdmaOrderEntry entry;
                        entry.list = m_exec_seq;
                        entry.is_use = true;
                        entry.exptype = order_exptype;
                        entry.uvmax = order_uvmax;
                        entry.blob_base = order_blob_base;
                        entry.blob_bytes = order_blob_bytes;
                        entry.ok = mat_ok;
                        log_mmdma_order_entry(entry);
                    }
                    if (!mat_ok) {
                    // Silent-unsupported site 2/3 (diag only): register-path
                    // materialize failure never logs ([mat-fail] needs the
                    // bridge gate). Key = use-state dims/format + residency.
                    if (clt_unsupported_diag_enabled()) {
                        static uint32_t emitted = 0;
                        static bool expired = false;
                        if (emitted < clt_unsupported_diag_max()) {
                            const uint32_t uvmax =
                                register_value(kTxtLdWidth);
                            std::fprintf(stderr,
                                         "[clt-reject] texture-materialize-reg "
                                         "uvmax=%#x w=%u h=%u exptype=%#x "
                                         "lod0=%#x lod1=%#x lod2=%#x lod3=%#x "
                                         "blob_base=%#x blob_bytes=%zu "
                                         "list=%llu frame=%llu\n",
                                         uvmax,
                                         ((uvmax >> 16u) & 0x3ffu) + 1u,
                                         (uvmax & 0x3ffu) + 1u,
                                         register_value(kTxtExpType),
                                         register_value(kTxtLodBase0),
                                         register_value(0x6418u),
                                         register_value(0x641cu),
                                         register_value(0x6420u),
                                         m_pending_mmdma_destination_base,
                                         m_pending_mmdma_bytes.size(),
                                         (unsigned long long)m_exec_seq,
                                         (unsigned long long)perf::counter_value(
                                             perf::Counter::GuestFramesStarted));
                            ++emitted;
                        } else if (!expired) {
                            expired = true;
                            std::fprintf(stderr,
                                         "[clt-reject] cap reached at frame "
                                         "%llu — further entries suppressed\n",
                                         (unsigned long long)perf::counter_value(
                                             perf::Counter::GuestFramesStarted));
                        }
                    }
                    ++result.unsupported;
                }
                }
            }
            if (window_clip_changed) update_window_clip();
            if (destination_blend_changed) update_destination_blend();
            result.words_consumed = cursor;
            if (stop_after_register) {
                flush_pending_batch();
                emit_exec_section_timing();
                return result;
            }
            if (cross_taken) {
                cross_taken = false;
                // command_list_base is still the list being left here;
                // enter_view rebinds it.
                const uint32_t from_base = command_list_base;
                enter_view(clt_frames[cross_enter], cross_word_index);
                if (clt_unsupported_diag_enabled()) {
                    static uint32_t entered_emitted = 0;
                    static bool entered_expired = false;
                    if (entered_emitted < clt_unsupported_diag_max()) {
                        const CltListFrame& view = clt_frames[cross_enter];
                        std::fprintf(
                            stderr,
                            "[clt-flow-enter] list=%llu frame=%llu "
                            "from=%#x into=%#x words=%zu at=%zu "
                            "head=%08x %08x %08x %08x\n",
                            static_cast<unsigned long long>(m_exec_seq),
                            static_cast<unsigned long long>(
                                perf::counter_value(
                                    perf::Counter::GuestFramesStarted)),
                            from_base,
                            view.base, view.word_count, cross_word_index,
                            cross_word_index < view.word_count
                                ? view.words[cross_word_index] : 0u,
                            cross_word_index + 1u < view.word_count
                                ? view.words[cross_word_index + 1u] : 0u,
                            cross_word_index + 2u < view.word_count
                                ? view.words[cross_word_index + 2u] : 0u,
                            cross_word_index + 3u < view.word_count
                                ? view.words[cross_word_index + 3u] : 0u);
                        ++entered_emitted;
                    } else if (!entered_expired) {
                        entered_expired = true;
                        std::fprintf(stderr,
                                     "[clt-flow-enter] cap reached at list "
                                     "%llu — further entries suppressed\n",
                                     static_cast<unsigned long long>(m_exec_seq));
                    }
                }
                result.words_consumed = cursor;
                continue;
            }
            if (flow_target_set) {
                cursor = flow_target;
                result.words_consumed = cursor;
                continue;
            }
            continue;
        }

        if (type == kVertexType) {
            CltSectionTimer section_timer(exec_section_timing, &exec_vtx_ns);
            const uint32_t count = (header & 0xffffu) + 1u;
            const bool shaded = (header & kVertexShading) != 0;
            const bool textured = (header & kVertexTexture) != 0;
            const bool perspective = (header & kVertexPerspective) != 0;
            const bool fan = (header & kVertexFan) != 0;
            const bool new_primitive = (header & kVertexNew) != 0;
            const size_t words_per_vertex =
                2u + (shaded ? 4u : 0u) + (perspective ? 1u : 0u) +
                (textured ? 2u : 0u);
            // The setup engine accepts short NEW headers and retains their
            // incomplete state for later continuation vertices. The count
            // field is count-1, so zero is one valid vertex for either form.
            if (count == 0u || count > kMaxVertexCount ||
                count > (word_count - cursor) / words_per_vertex) {
                result.ok = false;
                result.words_consumed = header_index;
                flush_pending_batch();
                return result;
            }

            m_vertex_scratch.resize(count);
            std::vector<CltVertex>& vertices = m_vertex_scratch;
            const uint32_t constant = register_value(kDbConstIn);
            const uint32_t escntl = register_value(0x4000u);
            for (CltVertex& vertex : vertices) {
                if (!read_float(words, word_count, &cursor, &vertex.x) ||
                    !read_float(words, word_count, &cursor, &vertex.y)) {
                    result.ok = false;
                    result.words_consumed = header_index;
                    flush_pending_batch();
                    return result;
                }
                if (shaded) {
                    if (!read_float(words, word_count, &cursor, &vertex.r) ||
                        !read_float(words, word_count, &cursor, &vertex.g) ||
                        !read_float(words, word_count, &cursor, &vertex.b) ||
                        !read_float(words, word_count, &cursor, &vertex.a)) {
                        result.ok = false;
                        result.words_consumed = header_index;
                        flush_pending_batch();
                        return result;
                    }
                } else {
                    vertex.r = color_component(constant, 16u);
                    vertex.g = color_component(constant, 8u);
                    vertex.b = color_component(constant, 0u);
                    // DBCONSTIN contains RGB only; primitive alpha is opaque
                    // for the unshaded guest sprite form observed here.
                    vertex.a = 1.f;
                }

                // CLT_VertexUvW emits 1/w before u/v. The recovered sprite
                // lists explicitly disable perspective correction, so the
                // guest pixel-space u/v values can be normalized below.
                if (perspective) {
                    float one_over_w = 0.f;
                    if (!read_float(words, word_count, &cursor, &one_over_w)) {
                        result.ok = false;
                        result.words_consumed = header_index;
                        flush_pending_batch();
                        return result;
                    }
                    vertex.one_over_w = one_over_w;
                    vertex.w = (one_over_w > 1e-7f) ? (1.f / one_over_w) : 1.f;
                } else {
                    vertex.one_over_w = 0.f;
                    vertex.w = 1.f;
                }
                vertex.u = 0.f;
                vertex.v = 0.f;
                if (textured &&
                    (!read_float(words, word_count, &cursor, &vertex.u) ||
                     !read_float(words, word_count, &cursor, &vertex.v))) {
                    result.ok = false;
                    result.words_consumed = header_index;
                    flush_pending_batch();
                    return result;
                }
                // ESCNTL PERSPECTIVEOFF (bit 2, 0x4): when clear, the TE
                // applies perspective correction and the stream carries u/w
                // and v/w (CLT_VertexRgbaUvW). Recover the true texture-unit
                // coordinates before the texture-width divide; the backend's
                // perspective-correct interpolation then cancels correctly.
                if (textured && perspective &&
                    (escntl & 0x4u) == 0u &&
                    vertex.one_over_w > 1e-7f) {
                    vertex.u /= vertex.one_over_w;
                    vertex.v /= vertex.one_over_w;
                }
            }

            ++result.vertex_batches;
            result.words_consumed = cursor;
            m_te_triangles_scratch.clear();
            m_te_triangles_scratch.reserve(count);
            std::vector<TeVertexState<TeTrackedVertex>::Triangle>&
                te_triangles = m_te_triangles_scratch;
            const bool te_trace = te_vertex_state_trace_enabled();
            const uint32_t te_trace_header = te_vertex_state_trace_header();
            const uint64_t te_trace_list_from = te_vertex_state_list_from();
            const bool te_flat_debug = te_vertex_state_flat_debug_enabled();
            const bool te_flat_depth = te_vertex_state_flat_depth_enabled();
            const bool te_flat_trace =
                te_vertex_state_flat_trace_enabled() &&
                m_exec_seq >= te_vertex_state_list_from();
            const bool lightweight_new_triangles =
                !te_trace && new_primitive && !te_flat_debug;
            m_new_triangle_refs_scratch.clear();
            if (lightweight_new_triangles)
                m_new_triangle_refs_scratch.reserve(count);
            const auto same_te_slots = [](const auto& lhs, const auto& rhs) {
                for (size_t slot = 0; slot < 3u; ++slot) {
                    if (lhs[slot].id != rhs[slot].id) return false;
                }
                return true;
            };
            const auto same_te_triangle = [](const auto& lhs,
                                             const auto& rhs) {
                for (size_t vertex = 0; vertex < 3u; ++vertex) {
                    if (lhs[vertex].id != rhs[vertex].id) return false;
                }
                return true;
            };
            for (uint32_t i = 0; i < count; ++i) {
                // The independent production/reference TE comparison is
                // diagnostic-only.  Keep the normal path to the lifted
                // setup-engine transition and triangle append; taking all
                // state snapshots below for every vertex was measurable on
                // the 34k-word State 3 world list even with tracing off.
                if (!te_trace) {
                    const TeTrackedVertex tracked{vertices[i],
                                                  ++m_te_next_vertex_id};
                    // The production path only needs the retained slots when
                    // a complete triangle is ready. Avoid zeroing and then
                    // copying a temporary triangle for every incoming vertex;
                    // add_vertex already owns the authoritative slot state.
                    if (m_te_vertex_state.add_vertex(
                            tracked, new_primitive && i == 0u, fan)) {
                        if (lightweight_new_triangles) {
                            const auto& slots = m_te_vertex_state.slots();
                            m_new_triangle_refs_scratch.push_back(
                                {slots[0].id, slots[1].id, slots[2].id});
                        } else {
                            te_triangles.push_back(m_te_vertex_state.slots());
                        }
                    }
                    continue;
                }
                const uint32_t before_count = m_te_vertex_state.vertex_count();
                const TeVertexSort before_sort = m_te_vertex_state.sort();
                const auto before_slots = m_te_vertex_state.slots();
                const bool compare_te_state = te_trace;
                const uint32_t reference_before_count =
                    compare_te_state ? m_te_reference_state.vertex_count()
                                     : 0u;
                const TeVertexSort reference_before_sort =
                    compare_te_state ? m_te_reference_state.sort()
                                     : TeVertexSort::invalid;
                const auto reference_before_slots =
                    compare_te_state ? m_te_reference_state.slots()
                                     : std::array<TeTrackedVertex, 3>{};
                TeVertexState<TeTrackedVertex>::Triangle triangle{};
                TeReferenceVertexState<TeTrackedVertex>::Triangle
                    reference_triangle{};
                const TeTrackedVertex tracked{vertices[i],
                                              ++m_te_next_vertex_id};
                const bool triangle_ready = m_te_vertex_state.add_vertex(
                        tracked, new_primitive && i == 0u, fan,
                        &triangle);
                if (triangle_ready) {
                    te_triangles.push_back(triangle);
                }
                bool reference_ready = false;
                if (compare_te_state) {
                    reference_ready = m_te_reference_state.add_vertex(
                        tracked, new_primitive && i == 0u, fan,
                        &reference_triangle);
                    if (reference_ready) ++m_te_reference_triangles;
                }
                const bool state_before_matches =
                    !compare_te_state ||
                    (before_count == reference_before_count &&
                     before_sort == reference_before_sort &&
                     same_te_slots(before_slots, reference_before_slots));
                const auto after_slots = m_te_vertex_state.slots();
                const auto reference_after_slots =
                    compare_te_state ? m_te_reference_state.slots()
                                     : std::array<TeTrackedVertex, 3>{};
                const bool state_after_matches =
                    !compare_te_state ||
                    (m_te_vertex_state.vertex_count() ==
                         m_te_reference_state.vertex_count() &&
                     m_te_vertex_state.sort() == m_te_reference_state.sort() &&
                     same_te_slots(after_slots, reference_after_slots));
                const bool triangles_match =
                    !compare_te_state ||
                    (triangle_ready == reference_ready &&
                     (!triangle_ready ||
                      same_te_triangle(triangle, reference_triangle)));
                const bool state_divergence =
                    compare_te_state &&
                    (!state_before_matches || !state_after_matches ||
                     !triangles_match);
                if (state_divergence) {
                    ++m_te_reference_divergences;
                    if (!m_te_reference_divergence_reported) {
                        m_te_reference_divergence_reported = true;
                        auto slot_id = [](const auto& slots, size_t index) {
                            return slots[index].id;
                        };
                        std::fprintf(
                            stderr,
                            "[te-state] FIRST PRODUCTION-REFERENCE "
                            "DIVERGENCE frame=%llu list=%llu "
                            "hdr_index=%zu hdr=%#x vertex=%u id=%llu "
                            "new=%d rm=%d count=%u\n"
                            "  reference-before=(vcnt=%u tsort=%u "
                            "slots=%llu,%llu,%llu)\n"
                            "  host-before=(vcnt=%u tsort=%u slots=%llu,%llu,%llu)\n"
                            "  reference-after=(vcnt=%u tsort=%u "
                            "slots=%llu,%llu,%llu) ready=%d\n"
                            "  host-after=(vcnt=%u tsort=%u slots=%llu,%llu,%llu) ready=%d\n"
                            "  incoming=(%.3f,%.3f)\n",
                            (unsigned long long)perf::counter_value(
                                perf::Counter::GuestFramesStarted),
                            (unsigned long long)m_exec_seq, header_index,
                            header, i, (unsigned long long)tracked.id,
                            (new_primitive && i == 0u) ? 1 : 0, fan ? 1 : 0,
                            count, reference_before_count,
                            static_cast<unsigned>(reference_before_sort),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         0),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         1),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         2),
                            before_count, static_cast<unsigned>(before_sort),
                            (unsigned long long)slot_id(before_slots, 0),
                            (unsigned long long)slot_id(before_slots, 1),
                            (unsigned long long)slot_id(before_slots, 2),
                            m_te_reference_state.vertex_count(),
                            static_cast<unsigned>(m_te_reference_state.sort()),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         0),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         1),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         2),
                            reference_ready ? 1 : 0,
                            m_te_vertex_state.vertex_count(),
                            static_cast<unsigned>(m_te_vertex_state.sort()),
                            (unsigned long long)slot_id(after_slots, 0),
                            (unsigned long long)slot_id(after_slots, 1),
                            (unsigned long long)slot_id(after_slots, 2),
                            triangle_ready ? 1 : 0, tracked.value.x,
                            tracked.value.y);
                        if (triangle_ready) {
                            std::fprintf(stderr,
                                         "  host-tri=(%llu,%llu,%llu)\n",
                                         (unsigned long long)triangle[0].id,
                                         (unsigned long long)triangle[1].id,
                                         (unsigned long long)triangle[2].id);
                        }
                        if (reference_ready) {
                            std::fprintf(
                                stderr, "  reference-tri=(%llu,%llu,%llu)\n",
                                (unsigned long long)reference_triangle[0].id,
                                (unsigned long long)reference_triangle[1].id,
                                (unsigned long long)reference_triangle[2].id);
                        }
                    }
                }
                if (te_trace &&
                    m_exec_seq >= te_trace_list_from &&
                    (te_trace_header == 0u || te_trace_header == header)) {
                    if (m_te_trace_emitted < te_vertex_state_trace_max()) {
                        ++m_te_trace_emitted;
                        auto slot_id = [](const auto& slots, size_t index) {
                            return slots[index].id;
                        };
                        std::fprintf(
                            stderr,
                            "[te-state] frame=%llu list=%llu hdr_index=%zu "
                            "hdr=%#x vertex=%u id=%llu new=%d rm=%d "
                            "count=%u host-before=(vcnt=%u tsort=%u "
                            "slots=%llu,%llu,%llu) host-after=(vcnt=%u "
                            "tsort=%u slots=%llu,%llu,%llu) "
                            "reference-before=(vcnt=%u tsort=%u "
                            "slots=%llu,%llu,%llu) reference-after=(vcnt=%u "
                            "tsort=%u slots=%llu,%llu,%llu) ref-ready=%d "
                            "incoming=(%.3f,%.3f)%s",
                            (unsigned long long)perf::counter_value(
                                perf::Counter::GuestFramesStarted),
                            (unsigned long long)m_exec_seq, header_index, header,
                            i, (unsigned long long)tracked.id,
                            (new_primitive && i == 0u) ? 1 : 0, fan ? 1 : 0,
                            count, before_count,
                            static_cast<unsigned>(before_sort),
                            (unsigned long long)slot_id(before_slots, 0),
                            (unsigned long long)slot_id(before_slots, 1),
                            (unsigned long long)slot_id(before_slots, 2),
                            m_te_vertex_state.vertex_count(),
                            static_cast<unsigned>(m_te_vertex_state.sort()),
                            (unsigned long long)slot_id(after_slots, 0),
                            (unsigned long long)slot_id(after_slots, 1),
                            (unsigned long long)slot_id(after_slots, 2),
                            reference_before_count,
                            static_cast<unsigned>(reference_before_sort),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         0),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         1),
                            (unsigned long long)slot_id(reference_before_slots,
                                                         2),
                            m_te_reference_state.vertex_count(),
                            static_cast<unsigned>(m_te_reference_state.sort()),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         0),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         1),
                            (unsigned long long)slot_id(reference_after_slots,
                                                         2),
                            reference_ready ? 1 : 0,
                            tracked.value.x, tracked.value.y,
                            state_divergence ? " DIVERGENCE" : "");
                        if (triangle_ready) {
                            const TeTrackedVertex& a = triangle[0];
                            const TeTrackedVertex& b = triangle[1];
                            const TeTrackedVertex& c = triangle[2];
                            const float area =
                                (b.value.x - a.value.x) *
                                    (c.value.y - a.value.y) -
                                (b.value.y - a.value.y) *
                                    (c.value.x - a.value.x);
                            const float min_x = std::min(
                                {a.value.x, b.value.x, c.value.x});
                            const float max_x = std::max(
                                {a.value.x, b.value.x, c.value.x});
                            const float min_y = std::min(
                                {a.value.y, b.value.y, c.value.y});
                            const float max_y = std::max(
                                {a.value.y, b.value.y, c.value.y});
                            std::fprintf(
                                stderr,
                                " tri=(%llu,%llu,%llu) xy=(%.3f,%.3f)"
                                ",(% .3f,%.3f),(% .3f,%.3f) area=%.3f "
                                "bbox=(%.3f,%.3f)-(%.3f,%.3f)",
                                (unsigned long long)a.id,
                                (unsigned long long)b.id,
                                (unsigned long long)c.id, a.value.x, a.value.y,
                                b.value.x, b.value.y, c.value.x, c.value.y,
                                area, min_x, min_y, max_x, max_y);
                            if (reference_ready) {
                                std::fprintf(
                                    stderr,
                                    " ref-tri=(%llu,%llu,%llu)",
                                    (unsigned long long)reference_triangle[0].id,
                                    (unsigned long long)reference_triangle[1].id,
                                    (unsigned long long)reference_triangle[2].id);
                            }
                        }
                        std::fputc('\n', stderr);
                    } else if (!m_te_trace_cap_reported) {
                        m_te_trace_cap_reported = true;
                        std::fprintf(
                            stderr,
                            "[te-state] cap reached at %u vertices; further "
                            "entries suppressed\n",
                            te_vertex_state_trace_max());
                    }
                }
            }
            if (clt_frame_trace_enabled()) {
                for (const auto& source : te_triangles) {
                    const float min_x = std::min(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    const float max_x = std::max(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    const float min_y = std::min(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    const float max_y = std::max(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    const float area =
                        (source[1].value.x - source[0].value.x) *
                            (source[2].value.y - source[0].value.y) -
                        (source[1].value.y - source[0].value.y) *
                            (source[2].value.x - source[0].value.x);
                    // Broad diagnostic classifier only. It is deliberately
                    // not a rendering gate and just separates substantial
                    // scene triangles from the one-pixel tail/UI chatter.
                    if (max_x - min_x >= 20.f && max_y - min_y >= 10.f &&
                        std::fabs(area) >= 20.f)
                        ++result.substantial_triangles;
                }
            }
            if (clt_z_census_enabled() &&
                m_exec_seq >= clt_z_census_list_from()) {
                const uint32_t db_super = register_value(kDbSuperGenControl);
                const uint32_t db_user = register_value(kDbUserControl);
                const uint32_t discard = register_value(kDbDiscardControl);
                const uint32_t db_z = register_value(kDbZCntl);
                const uint32_t db_ssb_dsb =
                    register_value(kDbSsbDsbCntl);
                const uint32_t db_a_mult = register_value(kDbAMultCntl);
                const uint32_t db_b_mult = register_value(kDbBMultCntl);
                const uint32_t db_alu = register_value(kDbAluCntl);
                const uint32_t db_const_in = register_value(kDbConstIn);
                const uint32_t db_src_alpha_cntl =
                    register_value(kDbSrcAlphaCntl);
                const uint32_t db_dest_alpha_cntl =
                    register_value(kDbDestAlphaCntl);
                const uint32_t db_dest_alpha_const =
                    register_value(kDbDestAlphaConst);
                // The blend classification is decoded once per submitted
                // state here rather than read from a render-path cache, so the
                // census reports what these registers select for this exact
                // draw boundary.
                const CltBlendState census_blend =
                    destination_blend_state();
                const uint32_t blend_mode =
                    static_cast<uint32_t>(m_destination_blend_mode);
                const uint32_t effective_blend_mode =
                    static_cast<uint32_t>(census_blend.mode);
                const uint32_t alpha_control_unsupported =
                    census_blend.alpha_control_unsupported ? 1u : 0u;
                for (const auto& source : te_triangles) {
                    DestStateCensusEntry* entry = nullptr;
                    for (auto& candidate : m_dest_state_census) {
                        if (candidate.used && candidate.db_super == db_super &&
                            candidate.db_user == db_user &&
                            candidate.discard == discard &&
                            candidate.db_z == db_z &&
                            candidate.db_ssb_dsb == db_ssb_dsb &&
                            candidate.db_a_mult == db_a_mult &&
                            candidate.db_b_mult == db_b_mult &&
                            candidate.db_alu == db_alu &&
                            candidate.db_const_in == db_const_in &&
                            candidate.db_src_alpha_cntl == db_src_alpha_cntl &&
                            candidate.db_dest_alpha_cntl == db_dest_alpha_cntl &&
                            candidate.db_dest_alpha_const ==
                                db_dest_alpha_const) {
                            entry = &candidate;
                            break;
                        }
                    }
                    if (entry) {
                        ++entry->triangles;
                        continue;
                    }
                    if (m_dest_state_census_emitted >= clt_z_census_max()) {
                        if (!m_dest_state_census_cap_reported) {
                            m_dest_state_census_cap_reported = true;
                            std::fprintf(
                                stderr,
                                "[clt-z-census] cap reached at %u states; "
                                "further states suppressed\n",
                                clt_z_census_max());
                        }
                        continue;
                    }
                    entry = &m_dest_state_census[m_dest_state_census_emitted++];
                    entry->used = true;
                    entry->db_super = db_super;
                    entry->db_user = db_user;
                    entry->discard = discard;
                    entry->db_z = db_z;
                    entry->db_ssb_dsb = db_ssb_dsb;
                    entry->db_a_mult = db_a_mult;
                    entry->db_b_mult = db_b_mult;
                    entry->db_alu = db_alu;
                    entry->db_const_in = db_const_in;
                    entry->db_src_alpha_cntl = db_src_alpha_cntl;
                    entry->db_dest_alpha_cntl = db_dest_alpha_cntl;
                    entry->db_dest_alpha_const = db_dest_alpha_const;
                    entry->blend_mode = blend_mode;
                    entry->effective_blend_mode = effective_blend_mode;
                    entry->alpha_control_unsupported = alpha_control_unsupported;
                    entry->triangles = 1;
                    entry->first_list = m_exec_seq;
                    entry->first_header_index =
                        static_cast<uint32_t>(header_index);
                    entry->first_header = header;
                    entry->first_triangle_id = source[0].id;
                    entry->first_min_x = std::min(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    entry->first_min_y = std::min(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    entry->first_max_x = std::max(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    entry->first_max_y = std::max(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    entry->first_min_one_over_w = std::min(
                        {source[0].value.one_over_w,
                         source[1].value.one_over_w,
                         source[2].value.one_over_w});
                    entry->first_max_one_over_w = std::max(
                        {source[0].value.one_over_w,
                         source[1].value.one_over_w,
                         source[2].value.one_over_w});
                    entry->first_shaded = shaded;
                    entry->first_textured = textured;
                    entry->first_z_base = register_value(kDbZBaseAddr);
                    entry->first_z_clip = register_value(kDbZClip);
                    std::fprintf(
                        stderr,
                        "[clt-z-census] state=%u first_list=%llu "
                        "hdr_index=%u hdr=%#x tri=%llu dbsuper=%#x "
                        "dbuser=%#x discard=%#x dbz=%#x destmask=%#x "
                        "zbufen=%d zout=%d blend=%d src=%d "
                        "shaded=%d textured=%d bbox=(%.1f,%.1f)-"
                        "(%.1f,%.1f) ow=[%.5g,%.5g] "
                        "color=(lt%d eq%d gt%d) z=(lt%d eq%d gt%d) "
                        "zbase=%#x zclip=(%ux%u) "
                        "blendword=%#x am=%#x bm=%#x alu=%#x dbin=%#x "
                        "sac=%#x dac=%#x daconst=%#x mode=%u eff=%u "
                        "aunsup=%u\n",
                        m_dest_state_census_emitted,
                        (unsigned long long)entry->first_list,
                        entry->first_header_index, entry->first_header,
                        (unsigned long long)entry->first_triangle_id, db_super,
                        db_user, discard, db_z, db_user & 0xfu,
                        (db_user & kDbZBufferEnable) != 0,
                        (db_user & kDbZOutputEnable) != 0,
                        (db_user & kDbBlendEnable) != 0,
                        (db_user & kDbSourceEnable) != 0,
                        shaded ? 1 : 0, textured ? 1 : 0,
                        entry->first_min_x, entry->first_min_y,
                        entry->first_max_x, entry->first_max_y,
                        entry->first_min_one_over_w,
                        entry->first_max_one_over_w,
                        (db_z & (1u << 0u)) != 0,
                        (db_z & (1u << 2u)) != 0,
                        (db_z & (1u << 4u)) != 0,
                        (db_z & (1u << 1u)) != 0,
                        (db_z & (1u << 3u)) != 0,
                        (db_z & (1u << 5u)) != 0,
                        entry->first_z_base,
                        (entry->first_z_clip >> 16u) &
                            kDbZClipCoordinateMask,
                        entry->first_z_clip & kDbZClipCoordinateMask,
                        db_ssb_dsb, db_a_mult, db_b_mult, db_alu, db_const_in,
                        db_src_alpha_cntl, db_dest_alpha_cntl,
                        db_dest_alpha_const, blend_mode, effective_blend_mode,
                        alpha_control_unsupported);


                }
            }
            if (clt_dest_trace_enabled() &&
                m_exec_seq >= clt_dest_trace_list_from() &&
                m_exec_seq <= clt_dest_trace_list_to()) {
                for (const auto& source : te_triangles) {
                    const float min_x = std::min(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    const float max_x = std::max(
                        {source[0].value.x, source[1].value.x,
                         source[2].value.x});
                    const float min_y = std::min(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    const float max_y = std::max(
                        {source[0].value.y, source[1].value.y,
                         source[2].value.y});
                    const bool giant = max_x - min_x >= 1000.f ||
                                       max_y - min_y >= 1000.f;
                    // Only retain large candidates; the purpose is to
                    // classify the giant/surface operation, not dump every
                    // mesh micro-triangle.
                    const uint32_t db_user = register_value(kDbUserControl);
                    if (clt_dest_trace_giant_only() && !giant)
                        continue;
                    if (!clt_dest_trace_giant_only() &&
                        max_x - min_x < 40.f && max_y - min_y < 30.f &&
                        db_user != 0x65fu)
                        continue;
                    if (m_dest_trace_emitted < clt_dest_trace_max()) {
                        ++m_dest_trace_emitted;
                        const float area =
                            (source[1].value.x - source[0].value.x) *
                                (source[2].value.y - source[0].value.y) -
                            (source[1].value.y - source[0].value.y) *
                                (source[2].value.x - source[0].value.x);
                        const float min_ow = std::min(
                            {source[0].value.one_over_w,
                             source[1].value.one_over_w,
                             source[2].value.one_over_w});
                        const float max_ow = std::max(
                            {source[0].value.one_over_w,
                             source[1].value.one_over_w,
                             source[2].value.one_over_w});
                        std::fprintf(
                            stderr,
                            "[clt-dest] list=%llu hdr_index=%zu hdr=%#x "
                            "new=%d rm=%d shaded=%d textured=%d "
                            "tri=(%llu,%llu,%llu) bbox=(%.1f,%.1f)-"
                            "(%.1f,%.1f) area=%.1f ow=[%.5g,%.5g] "
                            "dbsuper=%#x dbuser=%#x discard=%#x dbz=%#x "
                            "destmask=%#x zbufen=%d zout=%d blend=%d src=%d "
                            "dbmode=%d dbc=%#x dma=%#x dmb=%#x dalu=%#x\n",
                            (unsigned long long)m_exec_seq, header_index,
                            header, new_primitive ? 1 : 0, fan ? 1 : 0,
                            shaded ? 1 : 0, textured ? 1 : 0,
                            (unsigned long long)source[0].id,
                            (unsigned long long)source[1].id,
                            (unsigned long long)source[2].id, min_x, min_y,
                            max_x, max_y, area, min_ow, max_ow,
                            register_value(kDbSuperGenControl), db_user,
                            register_value(kDbDiscardControl),
                            register_value(kDbZCntl), db_user & 0xfu,
                            (db_user & kDbZBufferEnable) != 0,
                            (db_user & kDbZOutputEnable) != 0,
                            (db_user & kDbBlendEnable) != 0,
                            (db_user & kDbSourceEnable) != 0,
                            static_cast<int>(m_destination_blend_mode),
                            register_value(kDbConstIn),
                            register_value(kDbAMultCntl),
                            register_value(kDbBMultCntl),
                            register_value(kDbAluCntl));
                        if (clt_dest_trace_giant_only()) {
                            for (unsigned vertex = 0; vertex < 3u; ++vertex) {
                                const TeTrackedVertex& tracked = source[vertex];
                                const CltVertex& value = tracked.value;
                                std::fprintf(
                                    stderr,
                                    "[clt-dest-vertex] list=%llu tri=%u "
                                    "id=%llu xy=(%.5g,%.5g) ow=%.8g "
                                    "uv=(%.5g,%.5g) rgba=(%.5g,%.5g,%.5g,%.5g)\n",
                                    (unsigned long long)m_exec_seq, vertex,
                                    (unsigned long long)tracked.id, value.x,
                                    value.y, value.one_over_w, value.u,
                                    value.v, value.r, value.g, value.b,
                                    value.a);
                            }
                        }
                    } else if (!m_dest_trace_cap_reported) {
                        m_dest_trace_cap_reported = true;
                        std::fprintf(
                            stderr,
                            "[clt-dest] cap reached at %u entries; further "
                            "candidates suppressed\n",
                            clt_dest_trace_max());
                    }
                }
            }
            // A=TEXCOLOR*a, B=SRCCOLOR*(1-a): with PRIMALPHA=1 the
            // unavailable source input contributes nothing. Prove opacity
            // before TAB rewriting, including retained continuation slots.
            const bool opaque_unarmed_source_over = [&] {
                if (m_destination_blend_mode != DestinationBlendMode::unsupported ||
                    !textured ||
                    (register_value(kDbUserControl) & kDbSourceEnable) != 0 ||
                    ((register_value(kTxtTabCntl) >> 15u) & 3u) != 0u)
                    return false;
                const auto& state = blend_state_for_generation();
                if (state.alpha_control_unsupported ||
                    !clt_dsb_is_source_over(state.equation.a, state.equation.b,
                                           state.equation.alu))
                    return false;
                for (const auto& vertex : vertices)
                    if (vertex.a != 1.f) return false;
                for (const auto& triangle : te_triangles)
                    for (const auto& vertex : triangle)
                        if (vertex.value.a != 1.f) return false;
                return true;
            }();
            const auto reject_vertex = [&](const char* reason) {
                if (m_rgb_needs_16bit_source_alpha &&
                    std::strcmp(reason, "destination-blend") == 0)
                    clt_srcalpha16_tripwire(
                        "rgb-refused", m_exec_seq,
                        register_value(kDbUserControl),
                        register_value(kDbSrcCntl),
                        register_value(kDbAMultCntl),
                        register_value(kDbBMultCntl),
                        register_value(kDbAluCntl),
                        register_value(kDbSrcAlphaCntl),
                        register_value(kDbDestAlphaCntl));
                static const bool diagnosis = [] {
                    const char* e = std::getenv("M2_DRAW_DIAG");
                    return e && *e && *e != '0';
                }();
                static const uint32_t from = [] {
                    const char* e = std::getenv("M2_DRAW_DIAG_FROM");
                    return e ? static_cast<uint32_t>(strtoul(e, nullptr, 0)) : 0u;
                }();
                static uint32_t emitted = 0;
                // Route vertex rejections through the silent-unsupported gate
                // too (own counter/cap so the diagnostic sample budget is untouched).
                // Diag only: the fprintf condition widens, nothing else moves.
                static uint32_t unsup_emitted = 0;
                static bool unsup_expired = false;
                const uint64_t frame = perf::counter_value(perf::Counter::GuestFramesStarted);
                const bool sample = diagnosis && frame >= from && emitted < 64u;
                const bool unsup_sample = clt_unsupported_diag_enabled() &&
                    unsup_emitted < clt_unsupported_diag_max();
                if (bridge_trace_enabled() || sample || unsup_sample) {
                    float min_alpha = vertices.empty() ? 0.f : vertices[0].a;
                    float max_alpha = min_alpha;
                    for (const CltVertex& vertex : vertices) {
                        min_alpha = std::min(min_alpha, vertex.a);
                        max_alpha = std::max(max_alpha, vertex.a);
                    }
                    std::fprintf(stderr,
                                 "[clt-reject] %s list=%llu word=%zu "
                                 "header=%#x dbuser=%#x discard=%#x tab=%#x "
                                 "exptype=%#x dbsrc=%#x stride=%#x "
                                 "alpha=(%.6g,%.6g) a=%#x b=%#x alu=%#x srcalpha=%#x destalpha=%#x\n",
                                 reason, (unsigned long long)m_exec_seq,
                                 header_index, header,
                                 register_value(kDbUserControl),
                                 register_value(kDbDiscardControl),
                                 register_value(kTxtTabCntl),
                                 register_value(kTxtExpType),
                                 register_value(kDbSrcBaseAddr),
                                 register_value(kDbSrcXStride), min_alpha,
                                 max_alpha, register_value(kDbAMultCntl),
                                 register_value(kDbBMultCntl), register_value(kDbAluCntl),
                                 register_value(kDbSrcAlphaCntl), register_value(kDbDestAlphaCntl));
                }
                if (sample && ++emitted == 64u)
                    std::fprintf(stderr, "[clt-reject] cap reached at frame %llu — "
                                 "further entries suppressed\n", (unsigned long long)frame);
                if (unsup_sample && ++unsup_emitted == clt_unsupported_diag_max() &&
                    !unsup_expired) {
                    unsup_expired = true;
                    std::fprintf(stderr, "[clt-reject] cap reached at frame %llu — "
                                 "further entries suppressed\n", (unsigned long long)frame);
                }
                ++result.unsupported;
            };

            // Narrow showroom livery draw audit.  The 192x112 MMDMA cel is
            // the guest-authored red body panel; logging only that resident
            // keeps the diagnostic useful without paying the cost of the
            // broad accept census.  This is observation only: it does not
            // alter texture, depth, or destination-blend handling.
            const auto livery_draw_diag = [&](const char* path,
                                               const Texture* texture,
                                               const CltVertex* triangle,
                                               bool blend,
                                               const CltDiscard& discard,
                                               const CltDepth& depth) {
                if (!livery_diag_enabled_for_execute) return;
                if (!texture || !triangle || texture->width != 192 ||
                    texture->height != 112)
                    return;
                // Per-frame census first: the detail below is frame-gated
                // and capped, but the summary must see every match.
                livery_tally_note_draw(texture->cel,
                                       register_value(kTxtLodBase0), triangle);
                if (livery_tally_host_frame() < livery_draw_diag_frame_from())
                    return;
                static uint32_t max_entries = 64u;
                static bool max_checked = false;
                if (!max_checked) {
                    max_checked = true;
                    const char* value =
                        std::getenv("M2_LIVERY_DRAW_DIAG_MAX");
                    if (value && *value) {
                        const long parsed = std::strtol(value, nullptr, 0);
                        if (parsed > 0)
                            max_entries = static_cast<uint32_t>(
                                parsed > 256L ? 256L : parsed);
                    }
                }
                static uint32_t emitted = 0;
                static bool cap_reported = false;
                if (emitted >= max_entries) {
                    if (!cap_reported) {
                        cap_reported = true;
                        std::fprintf(
                            stderr,
                            "[livery-draw] cap reached at %u entries; "
                            "further entries suppressed\n",
                            max_entries);
                    }
                    return;
                }
                ++emitted;
                float min_x = triangle[0].x;
                float max_x = triangle[0].x;
                float min_y = triangle[0].y;
                float max_y = triangle[0].y;
                float mr = 0.f, mg = 0.f, mb = 0.f, ma = 0.f;
                for (size_t i = 0; i < 3; ++i) {
                    min_x = std::min(min_x, triangle[i].x);
                    max_x = std::max(max_x, triangle[i].x);
                    min_y = std::min(min_y, triangle[i].y);
                    max_y = std::max(max_y, triangle[i].y);
                    mr += triangle[i].r;
                    mg += triangle[i].g;
                    mb += triangle[i].b;
                    ma += triangle[i].a;
                }
                std::fprintf(
                    stderr,
                    "[livery-draw] frame=%llu list=%llu path=%s cel=%d "
                    "tram=%#x+%#x lod0=%#x wh=%dx%d "
                    "dbuser=%#x dbc=%#x dma=%#x dmb=%#x dalu=%#x "
                    "discard=%#x tab=%#x pip=%#x exp=%#x "
                    "rgb=(%.2f,%.2f,%.2f) a=%.2f "
                    "uv=((%.1f,%.1f),(%.1f,%.1f),(%.1f,%.1f)) "
                    "bbox=(%.1f,%.1f)-(%.1f,%.1f) blend=%d ztest=%d "
                    "zwrite=%d ow=(%.5g,%.5g,%.5g) hdr=%#x\n",
                    (unsigned long long)livery_tally_host_frame(),
                    (unsigned long long)m_exec_seq, path, texture->cel,
                    texture->tram_base, texture->tram_size,
                    register_value(kTxtLodBase0), texture->width,
                    texture->height, register_value(kDbUserControl),
                    register_value(kDbConstIn),
                    register_value(kDbAMultCntl),
                    register_value(kDbBMultCntl), register_value(kDbAluCntl),
                    discard.alpha_zero ? 1u : 0u,
                    register_value(kTxtTabCntl), register_value(kTxtPipCntl),
                    register_value(kTxtExpType), mr / 3.f, mg / 3.f,
                    mb / 3.f, ma / 3.f, triangle[0].u, triangle[0].v,
                    triangle[1].u, triangle[1].v, triangle[2].u,
                    triangle[2].v, min_x, min_y, max_x, max_y,
                    blend ? 1 : 0, depth.test_enabled ? 1 : 0,
                    depth.write_enabled ? 1 : 0, triangle[0].one_over_w,
                    triangle[1].one_over_w, triangle[2].one_over_w, header);
            };

            // The setup engine owns triangle assembly for NEW packets as
            // well as continuations.  Do not hand the raw packet to a
            // backend strip/fan assembler: its parity/order is not the
            // M2 retained-slot result (notably for the second triangle
            // of a strip).  All NEW triangles contain vertices from this
            // packet, so map the retained IDs back to the transformed
            // packet vertices immediately before each draw.
            const uint64_t first_packet_vertex_id =
                m_te_next_vertex_id - count + 1u;
            const auto draw_new_triangles =
                [&](bool textured_draw, CelId cel, bool blend,
                    const CltDiscard& discard, const CltDepth& depth,
                    CltTextureSampling sampling = {}, int serve_w = 0,
                    int serve_h = 0, CltBlendState blend_state = {},
                    Texture* config_texture = nullptr) {
                    if (!m_backend) return;
                    gate_weighted_mix_on_ssb(blend_state, config_texture);
                    // Opt-in per-draw cursor diagnostic: correlate the guest
                    // word offset with the material that reaches the backend
                    // so the room's compact wall blocks can be separated from
                    // the large car mesh. Capped, and off unless requested.
                    static const bool cursor_diag = [] {
                        const char* e = std::getenv("M2_CLT_DRAW_CURSOR");
                        return e && *e && *e != '0';
                    }();
                    static uint32_t cursor_emitted = 0;
                    if (cursor_diag && cursor_emitted < 512u &&
                        header_index >= 700u && header_index <= 19000u) {
                        ++cursor_emitted;
                        std::fprintf(
                            stderr,
                            "[clt-cursor] list=%llu hdr_index=%zu hdr=%#x "
                            "textured=%d cel=%d tri=%zu pending=%d "
                            "texture=%d tram=%#x+%#x\n",
                            (unsigned long long)m_exec_seq, header_index,
                            header, textured_draw ? 1 : 0, (int)cel,
                            te_triangles.size(),
                            m_pending_mmdma_texture ? 1 : 0,
                            (int)m_texture.cel, m_texture.tram_base,
                            m_texture.tram_size);
                    }
                    const size_t triangle_count =
                        lightweight_new_triangles
                            ? m_new_triangle_refs_scratch.size()
                            : te_triangles.size();
                    m_new_vertices_scratch.clear();
                    m_new_vertices_scratch.reserve(triangle_count * 3u);
                    std::vector<CltVertex>& batched_vertices =
                        m_new_vertices_scratch;
                    for (size_t triangle_index = 0;
                         triangle_index < triangle_count; ++triangle_index) {
                        std::array<uint64_t, 3> source_ids{};
                        if (lightweight_new_triangles) {
                            source_ids =
                                m_new_triangle_refs_scratch[triangle_index];
                        } else {
                            const auto& source = te_triangles[triangle_index];
                            for (size_t vertex_index = 0; vertex_index < 3u;
                                 ++vertex_index)
                                source_ids[vertex_index] =
                                    source[vertex_index].id;
                        }
                        const size_t triangle_begin = batched_vertices.size();
                        for (size_t vertex_index = 0; vertex_index < 3u;
                             ++vertex_index) {
                            const uint64_t id = source_ids[vertex_index];
                            const size_t packet_index = static_cast<size_t>(
                                id - first_packet_vertex_id);
                            // reserve() above makes these append-only copies
                            // allocation-free while avoiding the default
                            // initialization that resize() would perform
                            // before every three-vertex triangle.
                            batched_vertices.push_back(vertices[packet_index]);
                        }
                        CltVertex* triangle =
                            batched_vertices.data() + triangle_begin;
                        if (!textured_draw && draw_diag_for_execute) {
                            const uint64_t frame = perf::counter_value(
                                perf::Counter::GuestFramesStarted);
                            float min_x = triangle[0].x;
                            float max_x = triangle[0].x;
                            float min_y = triangle[0].y;
                            float max_y = triangle[0].y;
                            float mr = 0.f, mg = 0.f, mb = 0.f, ma = 0.f;
                            for (size_t vertex_index = 0; vertex_index < 3u;
                                 ++vertex_index) {
                                const CltVertex& vertex =
                                    triangle[vertex_index];
                                min_x = std::min(min_x, vertex.x);
                                max_x = std::max(max_x, vertex.x);
                                min_y = std::min(min_y, vertex.y);
                                max_y = std::max(max_y, vertex.y);
                                mr += vertex.r;
                                mg += vertex.g;
                                mb += vertex.b;
                                ma += vertex.a;
                            }
                            static uint32_t logged = 0;
                            static bool expired = false;
                            const bool substantial = max_x - min_x >= 40.f &&
                                                     max_y - min_y >= 20.f;
                            if (frame >= draw_diag_from_for_execute && substantial &&
                                logged < 64u) {
                                ++logged;
                                std::fprintf(
                                    stderr,
                                    "[clt-untex] cont=0 dbuser=%#x "
                                    "dbc=%#x dma=%#x dmb=%#x dalu=%#x "
                                    "discard=%#x dbz=%#x tab=%#x "
                                    "rgb=(%.2f,%.2f,%.2f) a=%.2f "
                                    "bbox=(%.1f,%.1f)-(%.1f,%.1f) "
                                    "blend=%d ztest=%d zwrite=%d header=%#x\n",
                                    register_value(kDbUserControl),
                                    register_value(kDbConstIn),
                                    register_value(kDbAMultCntl),
                                    register_value(kDbBMultCntl),
                                    register_value(kDbAluCntl),
                                    register_value(kDbDiscardControl),
                                    register_value(kDbZCntl),
                                    register_value(kTxtTabCntl), mr / 3.f,
                                    mg / 3.f, mb / 3.f, ma / 3.f, min_x,
                                    min_y, max_x, max_y, blend ? 1 : 0,
                                    depth.test_enabled ? 1 : 0,
                                    depth.write_enabled ? 1 : 0, header);
                                if (logged == 64u && !expired) {
                                    expired = true;
                                    std::fprintf(
                                        stderr,
                                        "[clt-untex] cap reached at frame "
                                        "%llu — further entries suppressed\n",
                                        (unsigned long long)frame);
                                }
                            } else if (frame >= draw_diag_from_for_execute &&
                                       substantial && !expired) {
                                expired = true;
                                std::fprintf(
                                    stderr,
                                    "[clt-untex] cap reached at frame %llu "
                                    "— further entries suppressed\n",
                                    (unsigned long long)frame);
                            }
                        }
                        if (draw_prov_for_execute)
                            draw_prov_note(header, textured_draw, serve_w,
                                           serve_h, triangle, 3, depth,
                                           blend);
                        if (config_texture &&
                            config_texture_trace_for_execute)
                            config_texture_draw_diag(
                                "new", *config_texture,
                                register_value(kTxtLodBase0), header,
                                triangle, 3u);
                        ++result.draws;
                    }
                    append_pending_vertices(
                        batched_vertices.data(), batched_vertices.size(),
                        textured_draw, cel, blend, discard, depth, sampling,
                        blend_state);
                };
            const auto draw_flat_te_triangles = [&](const CltDepth& depth) {
                // Diagnostic only: preserve the guest-derived triangle
                // coordinates and View origin, but bypass texture/TAB/color
                // interpretation so topology can be judged independently.
                // The palette is deterministic and keyed only by emitted
                // triangle order; it is not a rendering behavior override.
                if (!m_backend) return;
                flush_pending_batch();
                static constexpr std::array<std::array<float, 3>, 8> kColors = {{
                    {{0.95f, 0.18f, 0.18f}}, {{0.18f, 0.95f, 0.35f}},
                    {{0.20f, 0.45f, 0.98f}}, {{0.95f, 0.78f, 0.16f}},
                    {{0.86f, 0.22f, 0.91f}}, {{0.12f, 0.86f, 0.86f}},
                    {{0.98f, 0.48f, 0.14f}}, {{0.72f, 0.78f, 0.88f}},
                }};
                const float left = m_viewport_valid
                                       ? static_cast<float>(m_viewport_left)
                                       : 0.f;
                const float top = m_viewport_valid
                                      ? static_cast<float>(m_viewport_top)
                                      : 0.f;
                for (const auto& source : te_triangles) {
                    const auto& color =
                        kColors[m_te_flat_triangle_sequence++ % kColors.size()];
                    std::array<CltVertex, 3> triangle{};
                    for (size_t vertex_index = 0; vertex_index < 3;
                         ++vertex_index) {
                        triangle[vertex_index] = source[vertex_index].value;
                        triangle[vertex_index].x += left;
                        triangle[vertex_index].y += top;
                        triangle[vertex_index].u = 0.f;
                        triangle[vertex_index].v = 0.f;
                        triangle[vertex_index].r = color[0];
                        triangle[vertex_index].g = color[1];
                        triangle[vertex_index].b = color[2];
                        triangle[vertex_index].a = 1.f;
                    }
                    if (te_flat_trace) {
                        if (m_te_flat_trace_emitted <
                            te_vertex_state_trace_max()) {
                            ++m_te_flat_trace_emitted;
                            const float min_x = std::min(
                                {triangle[0].x, triangle[1].x, triangle[2].x});
                            const float max_x = std::max(
                                {triangle[0].x, triangle[1].x, triangle[2].x});
                            const float min_y = std::min(
                                {triangle[0].y, triangle[1].y, triangle[2].y});
                            const float max_y = std::max(
                                {triangle[0].y, triangle[1].y, triangle[2].y});
                            std::fprintf(
                                stderr,
                                "[te-flat] list=%llu hdr_index=%zu hdr=%#x "
                                "count=%zu ids=(%llu,%llu,%llu) "
                                "bbox=(%.3f,%.3f)-(%.3f,%.3f)\n",
                                (unsigned long long)m_exec_seq, header_index,
                                header, te_triangles.size(),
                                (unsigned long long)source[0].id,
                                (unsigned long long)source[1].id,
                                (unsigned long long)source[2].id, min_x, min_y,
                                max_x, max_y);
                        } else if (!m_te_flat_trace_cap_reported) {
                            m_te_flat_trace_cap_reported = true;
                            std::fprintf(
                                stderr,
                                "[te-flat] cap reached at %u triangles; "
                                "further entries suppressed\n",
                                te_vertex_state_trace_max());
                        }
                    }
                    const auto [width, height] =
                        target_size(m_display_width, m_display_height,
                                    m_backend);
                    perf::Scope renderer_scope(perf::Phase::RendererWork);
                    note_destination_draw(triangle.data(), 3u);
                    m_backend->clt_draw(triangle.data(), 3, false, false,
                                        kInvalidCel, width, height, false, {},
                                        te_flat_depth ? depth : CltDepth{}, {});
                    ++result.draws;
                }
            };
            if (new_primitive) {
                // A short NEW header only seeds the setup engine. It must not
                // reach a backend as an incomplete strip/fan.
                const size_t new_triangle_count =
                    lightweight_new_triangles
                        ? m_new_triangle_refs_scratch.size()
                        : te_triangles.size();
                if (new_triangle_count == 0u) continue;
                result.triangles += static_cast<uint32_t>(new_triangle_count);
                if (te_flat_debug) {
                    draw_flat_te_triangles(depth_state_for_generation());
                    continue;
                }
            } else {
                // A continuation may still be completing a short NEW header;
                // no triangle is emitted until the retained state reaches 3.
                if (!te_triangles.empty()) {
                    if (bridge_trace_enabled()) {
                        if (fan) {
                            std::fprintf(
                                stderr,
                                "[fan-cont] header=%#x count=%u fan=%d "
                                "triangles=%zu new=(%f,%f)\n",
                                header, count, fan, te_triangles.size(),
                                vertices.front().x, vertices.front().y);
                        } else {
                            std::fprintf(
                                stderr,
                                "[strip-cont] header=%#x count=%u fan=%d "
                                "triangles=%zu new=(%f,%f)\n",
                                header, count, fan, te_triangles.size(),
                                vertices.front().x, vertices.front().y);
                        }
                    }
                    result.triangles += static_cast<uint32_t>(te_triangles.size());
                    if (te_flat_debug) {
                        draw_flat_te_triangles(depth_state_for_generation());
                        continue;
                    }
                    if (m_window_clip_mode == WindowClipMode::unsupported) {
                        reject_vertex("window-clip");
                        continue;
                    }
                    if (m_destination_blend_mode ==
                        DestinationBlendMode::unsupported &&
                        !opaque_unarmed_source_over) {
                        reject_vertex("destination-blend");
                        continue;
                    }
                    const CltDepth cont_depth = depth_state_for_generation();
                    if (textured && !materialize_mmdma_texture()) {
                        reject_vertex("texture-materialize");
                        continue;
                    }
                    const bool cont_blend_enabled =
                        destination_blend_reaches_backend(
                            m_destination_blend_mode);
                    CltBlendState cont_blend_state = blend_state_for_generation();
                    if (opaque_unarmed_source_over)
                        cont_blend_state.mode = CltBlendMode::opaque_source_over;
                    const uint32_t cont_discard =
                        register_value(kDbDiscardControl);
                    // Source-over retains the primitive as A; SRCEN supplies B,
                    // not a replacement for the primitive color.
                    const bool cont_db_armed =
                        m_destination_blend_mode != DestinationBlendMode::general_internal &&
                        register_value(kDbSrcBaseAddr) != 0 &&
                        (register_value(kDbSrcXStride) &
                         kDbSrcXStrideMask) != 0 &&
                        (register_value(kDbUserControl) & kDbSourceEnable) !=
                            0;
                    const bool cont_db_secondary =
                        (textured || m_destination_blend_mode == DestinationBlendMode::general_secondary) && cont_db_armed &&
                        (m_destination_blend_mode == DestinationBlendMode::general_secondary || m_destination_blend_mode ==
                            DestinationBlendMode::secondary_source_alpha ||
                         ((m_destination_blend_mode == DestinationBlendMode::source_over ||
                           m_destination_blend_mode == DestinationBlendMode::constant_alpha_over) &&
                          (register_value(kDbSrcBaseAddr) != register_value(kDbDestBaseAddr) ||
                           register_value(kDbSrcOffset) != 0u)));
                    // This equation has two distinct texture inputs. Do not
                    // let an untextured packet, a missing arm, or an alpha
                    // control that the host does not model reach a flat or
                    // source-over fallback; the draw is unmodelled in that
                    // state and must remain fail-closed.
                    if (m_destination_blend_mode ==
                            DestinationBlendMode::secondary_source_alpha &&
                        !cont_db_secondary) {
                        reject_vertex("db-source-secondary-unarmed");
                        continue;
                    }
                    if (m_destination_blend_mode == DestinationBlendMode::general_secondary && !cont_db_secondary) {
                        reject_vertex("db-source-secondary-unarmed");
                        continue;
                    }
                    if (cont_db_secondary &&
                        cont_blend_state.alpha_control_unsupported &&
                        cont_blend_state.mode != CltBlendMode::general_secondary) {
                        reject_vertex("db-source-secondary-alpha-control");
                        continue;
                    }
                    const bool cont_has_texture =
                        textured ||
                        ((register_value(kTxtAddrCntl) &
                          kTxtAddrTextureEnable) != 0 &&
                         m_texture.cel != kInvalidCel);
                    // A DB-source draw samples the already-rasterized target.
                    // Cross-packet batching may still hold earlier triangles;
                    // submit them before taking the framebuffer snapshot so
                    // this host optimization cannot change copy ordering.
                    if ((!textured || cont_db_secondary) && cont_db_armed)
                        flush_pending_batch();
                    std::vector<CltVertex> cont_db_vertices;
                    if ((!textured || cont_db_secondary) && cont_db_armed) {
                        cont_db_vertices.reserve(te_triangles.size() * 3u);
                        for (const auto& triangle : te_triangles)
                            for (const TeTrackedVertex& vertex : triangle)
                                cont_db_vertices.push_back(vertex.value);
                    }
                    const bool cont_db_background =
                        ((!textured && m_destination_blend_mode != DestinationBlendMode::source_over) ||
                         cont_db_secondary) && cont_db_armed &&
                        load_db_source_texture(memory, cont_db_vertices);
                    if (cont_db_armed && !cont_db_background && (register_value(kDbSrcCntl) & kDbSrc32Bpp)) {
                        reject_vertex("db-source-32bpp-missing"); continue;
                    }
                    if (cont_db_background) {
                        cont_blend_state.db_source_linear = true;
                        cont_blend_state.source_ssb_mask = m_db_source.ssb_mask;
                        cont_blend_state.db_source_height =
                            static_cast<float>(m_db_source.height);
                    }
                    if (cont_db_secondary) {
                        if (!cont_db_background) {
                            reject_vertex("db-source-secondary");
                            continue;
                        }
                        cont_blend_state.secondary_source = m_db_source.cel;
                        cont_blend_state.source_ssb_mask = m_db_source.ssb_mask;
                        if (m_destination_blend_mode == DestinationBlendMode::source_over)
                            cont_blend_state.mode = CltBlendMode::secondary_source_over;
                        else if (m_destination_blend_mode == DestinationBlendMode::constant_alpha_over)
                            cont_blend_state.mode = CltBlendMode::secondary_constant_over;
                    }
                    if (cont_db_background && db_source_skip_enabled()) {
                        reject_vertex("db-source-skip");
                        continue;
                    }
                    // M_TBNoTex (cltmacromacros.h) sets COLOROUT=PRIMCOLOR
                    // (0) and ALPHAOUT=PRIMALPHA (0): untextured prim color,
                    // even when a stale texture enable/cel remains armed.
                    // Treat co==0 && ao==0 as untextured to avoid TAB-form
                    // rejects on header 0x20090003/tab 0xb draws.
                    const uint32_t cont_tab_co =
                        (register_value(kTxtTabCntl) >> 9u) & 3u;
                    const uint32_t cont_tab_ao =
                        (register_value(kTxtTabCntl) >> 15u) & 3u;
                    const bool cont_tab_no_tex =
                        cont_tab_co == 0u && cont_tab_ao == 0u;
                    const bool cont_tex_enabled =
                        (register_value(kTxtAddrCntl) &
                         kTxtAddrTextureEnable) != 0;
                    if (cont_db_secondary && m_destination_blend_mode != DestinationBlendMode::general_secondary &&
                        (!cont_tex_enabled || cont_tab_no_tex)) {
                        reject_vertex("db-source-secondary-texture-state");
                        continue;
                    }
                    const bool cont_effective_textured =
                        (textured ? cont_tex_enabled : cont_has_texture) &&
                        (!cont_db_background || cont_db_secondary) &&
                        !cont_tab_no_tex &&
                        !(!textured && cont_blend_state.mode == CltBlendMode::general_secondary &&
                          cont_blend_state.alpha_control_unsupported &&
                          !clt_dsb_rgb_uses_texture(cont_blend_state.general));
                    bool cont_batch_ok = true;
                    std::string cont_reject_reason;
                    float cont_tint_r = 1.f, cont_tint_g = 1.f,
                          cont_tint_b = 1.f;
                    bool cont_preserve_prim_alpha = false;
                    // PRIMCOLOR * TEXCOLOR (M_TBLitTex): the per-vertex
                    // primitive color is an input to the texture blend, so
                    // the backend must multiply texel * vertex color.
                    bool cont_preserve_prim_color = false;
                    CltDiscard cont_discard_state;
                    Texture* cont_draw_tex = nullptr;
                    ArmedDecode cont_serve{};
                    CltTextureSampling cont_sampling{};
                    if (cont_effective_textured) {
                        const uint32_t tab = register_value(kTxtTabCntl);
                        const uint32_t fc = tab & 7u;
                        const uint32_t sc = (tab >> 3u) & 7u;
                        const uint32_t tc = (tab >> 6u) & 7u;
                        const uint32_t fa = (tab >> 11u) & 3u;
                        const uint32_t sa = (tab >> 13u) & 3u;
                        const uint32_t co = (tab >> 9u) & 3u;
                        const uint32_t ao = (tab >> 15u) & 3u;
                        const uint32_t bop = (tab >> 17u) & 1u;
                        Texture* cont_tex =
                            texture_for_draw(register_value(kTxtLodBase0));
                        cont_draw_tex = cont_tex;
                        const bool cont_weighted_ssb =
                            cont_blend_state.mode ==
                                CltBlendMode::coefficient_weighted_mix &&
                            cont_blend_state.weighted_mix_entry_dependent;
                        // The resident supplies the bytes, this draw's own
                        // registers supply the format and geometry.
                        if (cont_tex)
                            cont_serve = serve_for_draw(
                                *cont_tex, register_value(kTxtLodBase0),
                                (cont_discard & 0x4u) != 0u ||
                                    cont_weighted_ssb ||
                                    (register_value(kDbDestBaseAddr) != 0u &&
                                     cont_blend_state.dsb.select == CltDsbSelect::object_ssb));
                        if (cont_blend_state.dsb.select == CltDsbSelect::object_ssb)
                            cont_blend_state.weighted_mix_ssb_mask = cont_serve.ssb_mask;
                        gate_weighted_mix_on_ssb(cont_blend_state, cont_tex,
                                                 &cont_serve);
                        if (!cont_tex || cont_serve.cel == kInvalidCel ||
                            (register_value(kTxtAddrCntl) &
                             kTxtAddrTextureEnable) == 0 ||
                            cont_serve.width <= 0 || cont_serve.height <= 0) {
                            // Bounded predicate breakdown for the boot-warm
                            // falsification: the generic [clt-reject]
                            // sample cannot distinguish a missing cel from a
                            // cleared enable bit or a stale LOD base. Emit
                            // under the existing draw-diagnostic floor (or the broad
                            // bridge trace) with an explicit cap line.
                            static const bool tex_diag = [] {
                                const char* e =
                                    std::getenv("M2_DRAW_DIAG");
                                return e && *e && *e != '0';
                            }();
                            static const uint32_t tex_from = [] {
                                const char* e = std::getenv("M2_DRAW_DIAG_FROM");
                                return e ? static_cast<uint32_t>(
                                               strtoul(e, nullptr, 0))
                                         : 0u;
                            }();
                            const uint64_t tex_frame = perf::counter_value(
                                perf::Counter::GuestFramesStarted);
                            static uint32_t tex_emitted = 0;
                            static bool tex_expired = false;
                            if (tex_emitted >= 64u && !tex_expired) {
                                tex_expired = true;
                                std::fprintf(
                                    stderr,
                                    "[clt-reject-texstate] cap reached at "
                                    "frame %llu — further entries "
                                    "suppressed\n",
                                    (unsigned long long)tex_frame);
                            }
                            if ((bridge_trace_enabled() ||
                                 (tex_diag && tex_frame >= tex_from)) &&
                                tex_emitted < 64u) {
                                ++tex_emitted;
                                std::fprintf(
                                    stderr,
                                    "[clt-reject-texstate] cont=1 cel=%d "
                                    "enable=%d w=%d h=%d in_span=%d "
                                    "pending=%d lod_base0=%#x lod1=%#x "
                                    "lod2=%#x lod3=%#x tram_base=%#x "
                                    "tram_size=%#x ld_cntl=%#x exp=%#x "
                                    "header=%#x tab=%#x\n",
                                    m_texture.cel,
                                    (register_value(kTxtAddrCntl) &
                                     kTxtAddrTextureEnable) != 0,
                                    m_texture.width, m_texture.height,
                                    in_texture_span(register_value(
                                        kTxtLodBase0)),
                                    m_pending_mmdma_texture ? 1 : 0,
                                    register_value(kTxtLodBase0),
                                    register_value(0x6418u),
                                    register_value(0x641cu),
                                    register_value(0x6420u),
                                    m_texture.tram_base, m_texture.tram_size,
                                    register_value(kTxtLdCntl),
                                    register_value(kTxtExpType), header,
                                    register_value(kTxtTabCntl));
                                if (tex_emitted == 64u && !tex_expired) {
                                    tex_expired = true;
                                    std::fprintf(
                                        stderr,
                                        "[clt-reject-texstate] cap reached "
                                        "at frame %llu — further entries "
                                        "suppressed\n",
                                        (unsigned long long)tex_frame);
                                }
                            }
                            cont_batch_ok = false;
                            cont_reject_reason = "texture-state";
                            if (livery_diag_enabled_for_execute)
                                livery_tally_note_texreject();
                        } else {
                            const bool mult = fc == 3u && sc == 5u &&
                                              tc == 0u && fa == 0u &&
                                              sa == 0u && co == 2u &&
                                              ao == 1u && bop == 1u;
                            const bool alpha_color = fc == 2u && sc == 5u &&
                                tc == 0u && fa == 0u && sa == 0u &&
                                co == 2u && ao == 0u && bop == 1u;
                            // Mirror the new-primitive direct-texture bypass:
                            // when both TAB outputs bypass the blender its
                            // input selectors and blend op are don't-care
                            // (M_TBTex in cltmacromacros.h). The continuation
                            // path applies the same TAB-form handling.
                            const bool cont_direct =
                                co == 1u && ao == 1u;
                            if (mult || alpha_color) {
                                const uint32_t c0 = register_value(kTxtConst2);
                                const uint32_t c1 = register_value(kTxtConst3);
                                if ((c0 & 0x00ffffffu) != (c1 & 0x00ffffffu)) {
                                    cont_batch_ok = false;
                                    cont_reject_reason = "TAB-constant";
                                } else {
                                    cont_tint_r = color_component(c0, 16u);
                                    cont_tint_g = color_component(c0, 8u);
                                    cont_tint_b = color_component(c0, 0u);
                                    cont_blend_state.texture_alpha_as_color =
                                        alpha_color;
                                    cont_preserve_prim_alpha = alpha_color;
                                }
                            } else if (cont_direct) {
                                // Bypass outputs: keep the white tint so the
                                // texel passes through unchanged.
                            } else if (fc == 0u && sc == 0u && tc == 0u &&
                                       fa == 0u && sa == 1u && co == 1u &&
                                       ao == 2u && bop == 1u) {
                                // Authentic 0x32200 sample:
                                // color bypass (TEXCOLOR), alpha BLEND MULT of
                                // PRIMALPHA*TEXTALPHA. Final = tex_rgb,
                                // prim_a * tex_a. Preserve per-vertex prim
                                // alpha; backend multiplies tex*vertex.
                                cont_preserve_prim_alpha = true;
                            } else {
                                const bool observed_prim_multiply =
                                    fc == 1u && sc == 3u &&
                                    (tc == 0u || tc == 2u) && fa == 0u &&
                                    sa == 0u && co == 2u && ao == 0u &&
                                    bop == 1u;
                                if (observed_prim_multiply) {
                                    // CLT MULT is A*B with the t input
                                    // don't-care. fc==1 selects Citer
                                    // (per-vertex primitive color), sc==3
                                    // selects Ct (texture color), so the
                                    // blend output is PRIMCOLOR * TEXCOLOR
                                    // (M_TBLitTex semantics). The TAB
                                    // constants are not inputs to this form.
                                    cont_preserve_prim_color = true;
                                    if (m_destination_blend_mode ==
                                            DestinationBlendMode::specular_add ||
                                        m_destination_blend_mode ==
                                            DestinationBlendMode::fog_composite) {
                                        // M_TBLitTex publishes PRIMALPHA as
                                        // TEXALPHA. M_DBSpec consumes that
                                        // value for its B multiplier, and the
                                        // M_DBFog distance lerp composites
                                        // the same stage alpha as the fog
                                        // weight M_LightFog wrote into the
                                        // per-vertex alpha. Both internal
                                        // composites need the primitive
                                        // alpha; no other destination
                                        // equation reads it.
                                        cont_preserve_prim_alpha = true;
                                    }
                                } else {
                                    cont_batch_ok = false;
                                    cont_reject_reason = "TAB-form";
                                }
                            }
                            // The recognized fast paths above keep their exact
                            // transport. All other documented TAB equations use the
                            // general per-fragment evaluator, including SSB constants.
                            if (((!cont_batch_ok && cont_reject_reason.rfind("TAB-", 0) == 0) ||
                                 m_destination_blend_mode == DestinationBlendMode::general_internal ||
                                 m_destination_blend_mode == DestinationBlendMode::general_secondary) &&
                                clt_tab_documented(tab)) {
                                cont_serve = serve_for_draw(*cont_tex, register_value(kTxtLodBase0), true);
                                if (cont_serve.cel != kInvalidCel) {
                                    cont_blend_state.tab = {true, tab,
                                        {register_value(kTxtConst2), register_value(kTxtConst3)}};
                                    cont_blend_state.weighted_mix_ssb_mask = cont_serve.ssb_mask;
                                    cont_preserve_prim_color = cont_preserve_prim_alpha = true;
                                    cont_blend_state.texture_alpha_as_color = false;
                                    cont_batch_ok = true;
                                }
                            }
                            // ZCLIPPED (0x8) is carried by CltDepth, not
                            // CltDiscard; it must not reject the draw.
                            if (cont_batch_ok && (cont_discard & ~0xfu) != 0u) {
                                cont_batch_ok = false;
                                cont_reject_reason = "discard-bits";
                            }
                            if (cont_batch_ok)
                                cont_discard_state.rgb_zero =
                                    (cont_discard & 0x2u) != 0u;
                            if (cont_batch_ok && (cont_discard & 0x4u) != 0u) {
                                if (!cont_tex ||
                                    cont_serve.ssb_mask == kInvalidCel) {
                                    cont_batch_ok = false;
                                    cont_reject_reason = "discard-SSB-mask";
                                } else {
                                    cont_discard_state.ssb_zero = true;
                                    cont_discard_state.alpha_zero =
                                        (cont_discard & 0x1u) != 0u;
                                    cont_discard_state.ssb_mask =
                                        cont_serve.ssb_mask;
                                }
                            } else if (cont_batch_ok) {
                                cont_discard_state.ssb_zero =
                                    (cont_discard & 0x4u) != 0u;
                                cont_discard_state.alpha_zero =
                                    (cont_discard & 0x1u) != 0u;
                                if (cont_discard_state.ssb_zero &&
                                    (!cont_tex ||
                                     cont_serve.ssb_mask == kInvalidCel)) {
                                    cont_batch_ok = false;
                                    cont_reject_reason = "discard-SSB-mask";
                                } else if (cont_discard_state.ssb_zero) {
                                    cont_discard_state.ssb_mask =
                                        cont_serve.ssb_mask;
                                }
                            }
                        }
                    } else if (cont_db_background && !cont_db_secondary) {
                        if (!decode_db_source_discard(
                                cont_discard, &cont_discard_state)) {
                            cont_batch_ok = false;
                            cont_reject_reason = "discard-db-source";
                        }
                    } else if (cont_discard != 0u) {
                        // Untextured ALPHA0/RGB0: the white texture keeps the
                        // primitive color as the post-texturing value, so
                        // the GL/soft backends can apply both tests directly.
                        // ZCLIPPED (0x8) is carried by CltDepth instead of
                        // CltDiscard and stays valid on this path; SSB still
                        // needs a resident source and remains fail-closed.
                        if ((cont_discard & ~0xbu) == 0u) {
                            cont_discard_state.ssb_zero = false;
                            cont_discard_state.alpha_zero =
                                (cont_discard & 0x1u) != 0u;
                            cont_discard_state.rgb_zero =
                                (cont_discard & 0x2u) != 0u;
                        } else {
                            cont_batch_ok = false;
                            cont_reject_reason = "discard-untextured";
                        }
                    }
                    if (!cont_batch_ok) {
                        reject_vertex(cont_reject_reason.c_str());
                        continue;
                    }
                    if (cont_effective_textured && cont_draw_tex) {
                        if (!texture_sampling_state(
                                register_value(kTxtAddrCntl),
                                register_value(0x6430),
                                static_cast<float>(cont_serve.width),
                                static_cast<float>(cont_serve.height),
                                &cont_sampling)) {
                            // Reserved min/mag/inter selector: the vendor
                            // header defines no hardware meaning for it, so
                            // refuse the draw instead of sampling the resident
                            // with a filter the guest never selected.
                            reject_vertex("texture-filter-reserved");
                            continue;
                        }
                    }
                    // Opt-in continuation diagnostic: room geometry is
                    // predominantly emitted by retained strip/fan draws, so
                    // the new-packet cursor diagnostic above does not show
                    // which texture reaches the backend. List-filtered and
                    // observation only, never draw behaviour.
                    const auto cont_cursor_diag = [&](bool effective,
                                                       CelId draw_cel) {
                        static const bool enabled = [] {
                            const char* e = std::getenv("M2_CLT_DRAW_CURSOR");
                            return e && *e && *e != '0';
                        }();
                        static const uint64_t list_filter = [] {
                            const char* e = std::getenv("M2_CLT_DRAW_LIST");
                            return e && *e ? strtoull(e, nullptr, 0)
                                           : UINT64_MAX;
                        }();
                        static uint32_t max_entries = 4096u;
                        static bool max_checked = false;
                        if (!max_checked) {
                            max_checked = true;
                            const char* e =
                                std::getenv("M2_CLT_DRAW_CURSOR_MAX");
                            if (e && *e) {
                                const unsigned long parsed =
                                    strtoul(e, nullptr, 0);
                                if (parsed > 0)
                                    max_entries = static_cast<uint32_t>(
                                        parsed > 20000ul ? 20000ul : parsed);
                            }
                        }
                        static uint32_t emitted = 0;
                        if (!enabled ||
                            (list_filter != UINT64_MAX &&
                             m_exec_seq != list_filter) ||
                            header_index < 700u || header_index > 19000u ||
                            emitted >= max_entries)
                            return;
                        ++emitted;
                        std::fprintf(
                            stderr,
                            "[clt-cursor-cont] list=%llu hdr_index=%zu "
                            "hdr=%#x textured=%d effective=%d cel=%d "
                            "pending=%d current=%d tram=%#x+%#x "
                            "enable=%d tab=%#x lod0=%#x\n",
                            (unsigned long long)m_exec_seq, header_index,
                            header, textured ? 1 : 0, effective ? 1 : 0,
                            (int)draw_cel,
                            m_pending_mmdma_texture ? 1 : 0,
                            (int)m_texture.cel, m_texture.tram_base,
                            m_texture.tram_size,
                            (register_value(kTxtAddrCntl) &
                             kTxtAddrTextureEnable) != 0,
                            register_value(kTxtTabCntl),
                            register_value(kTxtLodBase0));
                    };
                    const auto append_cont_triangle =
                        [&](const std::array<CltVertex, 3>& triangle,
                            bool textured_draw, CelId draw_cel,
                            CltTextureSampling sampling = {}) {
                            append_pending_vertices(
                                triangle.data(), triangle.size(),
                                textured_draw, draw_cel, cont_blend_enabled,
                                cont_discard_state, cont_depth, sampling,
                                cont_blend_state);
                        };
                    for (const auto& te_triangle : te_triangles) {
                        std::array<CltVertex, 3> tri{};
                        for (size_t vertex_index = 0; vertex_index < 3;
                             ++vertex_index)
                            tri[vertex_index] =
                                te_triangle[vertex_index].value;
                        if (!textured && cont_effective_textured) {
                            for (CltVertex& v : tri) {
                                v.u = v.x;
                                v.v = v.y;
                            }
                        }
                        if (m_viewport_valid) {
                            const float left =
                                static_cast<float>(m_viewport_left);
                            const float top =
                                static_cast<float>(m_viewport_top);
                            for (CltVertex& v : tri) {
                                v.x += left;
                                v.y += top;
                            }
                        }
                        if (cont_db_secondary && cont_db_background) {
                            const float org_x = m_viewport_valid
                                                    ? static_cast<float>(
                                                          m_viewport_left)
                                                    : 0.f;
                            const float org_y = m_viewport_valid
                                                    ? static_cast<float>(
                                                          m_viewport_top)
                                                    : 0.f;
                            const uint32_t source_offset =
                                register_value(kDbSrcOffset);
                            for (CltVertex& v : tri) {
                                const CltDbSourceCoordinates source_coord =
                                    clt_db_source_coordinates(
                                        v.x - org_x, v.y - org_y,
                                        static_cast<float>(
                                            m_db_source.db_source_origin_pixels),
                                        source_offset, m_db_source.width,
                                        m_db_source.height);
                                v.mask_u = source_coord.u;
                                v.mask_v = source_coord.v;
                            }
                        }
                        if (cont_effective_textured) {
                            if (!cont_draw_tex) {
                                if (livery_diag_enabled_for_execute)
                                    livery_tally_note_texreject();
                                reject_vertex("texture-state");
                                continue;
                            }
                            cont_cursor_diag(true, cont_serve.cel);
                            const float tw =
                                static_cast<float>(cont_serve.width);
                            const float th =
                                static_cast<float>(cont_serve.height);
                            // Bounded accept audit for continuation textured
                            // draws (same gates/cap env as the new-path
                            // audit): the showroom body arrives mostly as
                            // continuations, invisible to the new-path log.
                            // Reads guest colors/alpha/pre-divide UVs before
                            // the tint/divide below overwrites them.
                            if (draw_diag_for_execute) {
                                const bool cacc_diag = true;
                                const uint32_t cacc_from =
                                    draw_diag_from_for_execute;
                                static uint32_t cacc_max = 0;
                                static bool cacc_max_checked = false;
                                if (!cacc_max_checked) {
                                    cacc_max_checked = true;
                                    if (clt_diag_list_filter() != UINT64_MAX) {
                                        cacc_max = 20000u;
                                    } else {
                                        const char* m =
                                            std::getenv("M2_ACCEPT_MAX");
                                        cacc_max = 32u;
                                        if (m && *m) {
                                            const long v = std::strtol(
                                                m, nullptr, 0);
                                            if (v > 32L) cacc_max =
                                                static_cast<uint32_t>(
                                                    v > 512L ? 512L : v);
                                        }
                                    }
                                }
                                const uint64_t cacc_frame =
                                    perf::counter_value(
                                        perf::Counter::GuestFramesStarted);
                                static uint32_t cacc_emitted = 0;
                                static bool cacc_expired = false;
                                if (cacc_emitted >= cacc_max &&
                                    !cacc_expired) {
                                    cacc_expired = true;
                                    std::fprintf(
                                        stderr,
                                        "[clt-accept] cap reached at frame "
                                        "%llu — further entries suppressed\n",
                                        (unsigned long long)cacc_frame);
                                }
                                if ((cacc_diag &&
                                     (cacc_frame >= cacc_from ||
                                      clt_diag_list_filter() != UINT64_MAX) &&
                                     clt_diag_list_selected(m_exec_seq)) &&
                                    cacc_emitted < cacc_max) {
                                    ++cacc_emitted;
                                    float cmr = 0.f, cmg = 0.f, cmb = 0.f;
                                    float cma = 0.f, cmu = 0.f, cmv = 0.f;
                                    float cmnx = tri[0].u, cmxx = tri[0].u;
                                    float cmny = tri[0].v, cmxy = tri[0].v;
                                    for (const CltVertex& cv : tri) {
                                        cmr += cv.r;
                                        cmg += cv.g;
                                        cmb += cv.b;
                                        cma += cv.a;
                                        cmu += cv.u;
                                        cmv += cv.v;
                                        cmnx = std::min(cmnx, cv.u);
                                        cmxx = std::max(cmxx, cv.u);
                                        cmny = std::min(cmny, cv.v);
                                        cmxy = std::max(cmxy, cv.v);
                                    }
                                    std::fprintf(
                                        stderr,
                                        "[clt-accept] cont=1 dbuser=%#x "
                                        "lod0=%#x lod1=%#x "
                                        "serve_base=%#x serve_size=%#x "
                                        "serve_wh=%dx%d armed=%d sdecl=%d "
                                        "dec=%dx%d pub=%u staged=%d "
                                        "tab=%#x pip=%#x "
                                        "uvmax=%#x uvmask=%#x addr=%#x "
                                        "ldctl=%#x count=%#x "
                                        "c2=%#x c3=%#x exp=%#x "
                                        "escnl=%#x ow=%.4g "
                                        "tint=(%.2f,%.2f,%.2f) "
                                        "vrgba=(%.2f,%.2f,%.2f,%.2f) "
                                        "uvmean=(%.1f,%.1f) "
                                        "uvx=[%.1f,%.1f] uvy=[%.1f,%.1f] "
                                        "blend=%d header=%#x v0=(%.1f,%.1f)\n",
                                        register_value(kDbUserControl),
                                        register_value(kTxtLodBase0),
                                        register_value(0x6418u),
                                        cont_draw_tex->tram_base,
                                        cont_draw_tex->tram_size,
                                        cont_draw_tex->width,
                                        cont_draw_tex->height,
                                        cont_serve.armed ? 1 : 0,
                                        cont_serve.declined ? 1 : 0,
                                        cont_serve.width, cont_serve.height,
                                        cont_serve.publisher,
                                        cont_serve.resident_staged ? 1 : 0,
                                        register_value(kTxtTabCntl),
                                        register_value(kTxtPipCntl),
                                        register_value(kTxtLdWidth),
                                        register_value(0x6430u),
                                        register_value(kTxtAddrCntl),
                                        register_value(kTxtLdCntl),
                                        register_value(kTxtCount),
                                        register_value(kTxtConst2),
                                        register_value(kTxtConst3),
                                        register_value(kTxtExpType),
                                        register_value(0x4000u),
                                        tri[0].one_over_w,
                                        cont_tint_r, cont_tint_g,
                                        cont_tint_b, cmr / 3.f, cmg / 3.f,
                                        cmb / 3.f, cma / 3.f, cmu / 3.f,
                                        cmv / 3.f, cmnx, cmxx, cmny, cmxy,
                                        cont_blend_enabled ? 1 : 0, header,
                                        tri[0].x, tri[0].y);
                                    if (cacc_emitted == cacc_max &&
                                        !cacc_expired) {
                                        cacc_expired = true;
                                        std::fprintf(
                                            stderr,
                                            "[clt-accept] cap reached at "
                                            "frame %llu — further entries "
                                            "suppressed\n",
                                            (unsigned long long)cacc_frame);
                                    }
                                }
                            }
                            livery_draw_diag("cont", cont_draw_tex, tri.data(),
                                             cont_blend_enabled,
                                             cont_discard_state, cont_depth);
                            if (livery_diag_enabled_for_execute)
                                livery_tally_note_textured(
                                    cont_draw_tex->cel, cont_draw_tex->width,
                                    cont_draw_tex->height, tri.data(), 3u);
                            for (CltVertex& v : tri) {
                                v.u /= tw;
                                v.v /= th;
                                if (!cont_preserve_prim_color) {
                                    v.r = cont_tint_r;
                                    v.g = cont_tint_g;
                                    v.b = cont_tint_b;
                                }
                                if (!cont_preserve_prim_alpha) v.a = 1.f;
                            }
                            if (m_backend) {
                                if (draw_prov_for_execute)
                                    draw_prov_note(
                                        header, true, cont_draw_tex->width,
                                        cont_draw_tex->height, tri.data(),
                                        tri.size(), cont_depth,
                                        cont_blend_enabled);
                                if (config_texture_trace_for_execute)
                                    config_texture_draw_diag(
                                        "cont", *cont_draw_tex,
                                        register_value(kTxtLodBase0), header,
                                        tri.data(), tri.size());
                                append_cont_triangle(
                                    tri, true, cont_serve.cel,
                                    cont_sampling);
                                ++result.draws;
                            }
                        } else if (cont_db_background && !cont_db_secondary) {
                            cont_cursor_diag(true, m_db_source.cel);
                            const float org_x = m_viewport_valid
                                                    ? static_cast<float>(
                                                          m_viewport_left)
                                                    : 0.f;
                            const float org_y = m_viewport_valid
                                                    ? static_cast<float>(
                                                          m_viewport_top)
                                                    : 0.f;
                            const uint32_t source_offset =
                                register_value(kDbSrcOffset);
                            for (CltVertex& v : tri) {
                                const CltDbSourceCoordinates source_coord =
                                    clt_db_source_coordinates(
                                        v.x - org_x, v.y - org_y,
                                        static_cast<float>(
                                            m_db_source.db_source_origin_pixels),
                                        source_offset, m_db_source.width,
                                        m_db_source.height);
                                v.u = source_coord.u;
                                v.v = source_coord.v;
                                v.r = 1.f;
                                v.g = 1.f;
                                v.b = 1.f;
                            }
                            if (m_backend) {
                                if (livery_diag_enabled_for_execute)
                                    livery_tally_note_db();
                                if (draw_prov_for_execute)
                                    draw_prov_note(
                                        header, true, m_db_source.width,
                                        m_db_source.height, tri.data(),
                                        tri.size(), cont_depth,
                                        cont_blend_enabled);
                                append_cont_triangle(tri, true,
                                                     m_db_source.cel);
                                ++result.draws;
                            }
                        } else {
                            cont_cursor_diag(false, kInvalidCel);
                            if (draw_diag_for_execute) {
                                const uint64_t frame = perf::counter_value(
                                    perf::Counter::GuestFramesStarted);
                                float min_x = tri[0].x;
                                float max_x = tri[0].x;
                                float min_y = tri[0].y;
                                float max_y = tri[0].y;
                                float mr = 0.f, mg = 0.f, mb = 0.f, ma = 0.f;
                                for (const CltVertex& vertex : tri) {
                                    min_x = std::min(min_x, vertex.x);
                                    max_x = std::max(max_x, vertex.x);
                                    min_y = std::min(min_y, vertex.y);
                                    max_y = std::max(max_y, vertex.y);
                                    mr += vertex.r;
                                    mg += vertex.g;
                                    mb += vertex.b;
                                    ma += vertex.a;
                                }
                                static uint32_t logged = 0;
                                static bool expired = false;
                                const bool substantial = max_x - min_x >= 40.f &&
                                                         max_y - min_y >= 20.f;
                                if (frame >= draw_diag_from_for_execute && substantial &&
                                    logged < 64u) {
                                    ++logged;
                                    std::fprintf(
                                        stderr,
                                        "[clt-untex] cont=1 dbuser=%#x "
                                        "dbc=%#x dma=%#x dmb=%#x dalu=%#x "
                                        "discard=%#x dbz=%#x tab=%#x "
                                        "rgb=(%.2f,%.2f,%.2f) a=%.2f "
                                        "bbox=(%.1f,%.1f)-(%.1f,%.1f) "
                                        "blend=%d ztest=%d zwrite=%d header=%#x\n",
                                        register_value(kDbUserControl),
                                        register_value(kDbConstIn),
                                        register_value(kDbAMultCntl),
                                        register_value(kDbBMultCntl),
                                        register_value(kDbAluCntl),
                                        register_value(kDbDiscardControl),
                                        register_value(kDbZCntl),
                                        register_value(kTxtTabCntl), mr / 3.f,
                                        mg / 3.f, mb / 3.f, ma / 3.f, min_x,
                                        min_y, max_x, max_y,
                                        cont_blend_enabled ? 1 : 0,
                                        cont_depth.test_enabled ? 1 : 0,
                                        cont_depth.write_enabled ? 1 : 0, header);
                                    if (logged == 64u && !expired) {
                                        expired = true;
                                        std::fprintf(
                                            stderr,
                                            "[clt-untex] cap reached at frame "
                                            "%llu — further entries suppressed\n",
                                            (unsigned long long)frame);
                                    }
                                } else if (frame >= draw_diag_from_for_execute &&
                                           substantial && !expired) {
                                    expired = true;
                                    std::fprintf(
                                        stderr,
                                        "[clt-untex] cap reached at frame %llu "
                                        "— further entries suppressed\n",
                                        (unsigned long long)frame);
                                }
                            }
                            if (m_backend) {
                                if (draw_prov_for_execute)
                                    draw_prov_note(header, false, 0, 0,
                                                   tri.data(), tri.size(),
                                                   cont_depth,
                                                   cont_blend_enabled);
                                append_cont_triangle(tri, false,
                                                     kInvalidCel);
                                ++result.draws;
                            }
                        }
                    }
                    continue;
                }
                reject_vertex("primitive-continuation");
                continue;
            }
            if (m_window_clip_mode == WindowClipMode::unsupported) {
                reject_vertex("window-clip");
                continue;
            }
            if (m_destination_blend_mode == DestinationBlendMode::unsupported &&
                !opaque_unarmed_source_over) {
                reject_vertex("destination-blend");
                continue;
            }

            const CltDepth depth = depth_state_for_generation();

            // A pending MMDMA load becomes guest texture state at the draw
            // boundary. Materialize it before checking draw-only state so a
            // rejected draw cannot leave an older load pending for a later
            // list.
            if (textured && !materialize_mmdma_texture()) {
                reject_vertex("texture-materialize");
                continue;
            }
            const bool blend_enabled =
                destination_blend_reaches_backend(m_destination_blend_mode);
            CltBlendState blend_state = blend_state_for_generation();
            if (opaque_unarmed_source_over)
                blend_state.mode = CltBlendMode::opaque_source_over;
            const uint32_t discard_control =
                register_value(kDbDiscardControl);

            // Source-over retains the primitive as A; the backend blends
            // it over the current destination. Do not turn SRCEN into a
            // primary framebuffer-copy texture (RenderRadarFrame).
            const bool db_source_armed =
                m_destination_blend_mode != DestinationBlendMode::general_internal &&
                register_value(kDbSrcBaseAddr) != 0 &&
                (register_value(kDbSrcXStride) & kDbSrcXStrideMask) != 0 &&
                (register_value(kDbUserControl) & kDbSourceEnable) != 0;
            const bool has_texture =
                textured ||
                ((register_value(kTxtAddrCntl) & kTxtAddrTextureEnable) != 0 &&
                 m_texture.cel != kInvalidCel);

            // The square transitions author an SSB-only texture while the
            // B-only destination equation copies colour from DB source.  The
            // texture and source framebuffer are simultaneous TE inputs.
            const bool db_source_ssb_mask =
                textured && db_source_armed &&
                m_destination_blend_mode == DestinationBlendMode::source_copy &&
                register_value(kTxtExpType) == 0x1200u;
            // StreamEventFunc's Configure Race border uses the distinct
            // reverse-alpha equation (2/0/0x10). It needs the separately
            // armed DB source even though the primary packet is textured.
            const bool db_source_secondary =
                (textured || m_destination_blend_mode == DestinationBlendMode::general_secondary) && db_source_armed &&
                (m_destination_blend_mode == DestinationBlendMode::general_secondary || m_destination_blend_mode == DestinationBlendMode::secondary_source_alpha ||
                 ((m_destination_blend_mode == DestinationBlendMode::source_over ||
                   m_destination_blend_mode == DestinationBlendMode::constant_alpha_over) &&
                  (register_value(kDbSrcBaseAddr) != register_value(kDbDestBaseAddr) ||
                   register_value(kDbSrcOffset) != 0u)));
            // The reverse-alpha equation requires both the primary texture
            // and an independently armed DB source. An untextured packet or a
            // missing/32-bit source is not an ordinary untextured blend and
            // must not fall through to the plain destination-blender path.
            if (m_destination_blend_mode ==
                    DestinationBlendMode::secondary_source_alpha &&
                !db_source_secondary) {
                reject_vertex("db-source-secondary-unarmed");
                continue;
            }
            if (m_destination_blend_mode == DestinationBlendMode::general_secondary && !db_source_secondary) {
                reject_vertex("db-source-secondary-unarmed");
                continue;
            }
            if (db_source_secondary &&
                blend_state.alpha_control_unsupported &&
                blend_state.mode != CltBlendMode::general_secondary) {
                reject_vertex("db-source-secondary-alpha-control");
                continue;
            }

            // A DB-source draw samples the already-rasterized target. Flush
            // queued compatible triangles before the readback; otherwise the
            // batching optimization could make the copy observe stale pixels.
            if (((!textured && m_destination_blend_mode != DestinationBlendMode::source_over) ||
                 db_source_ssb_mask || db_source_secondary) &&
                db_source_armed)
                flush_pending_batch();
            const bool db_source_attempted =
                ((!textured && m_destination_blend_mode != DestinationBlendMode::source_over) ||
                 db_source_ssb_mask || db_source_secondary) &&
                db_source_armed;
            const bool db_source_background =
                db_source_attempted && load_db_source_texture(memory, vertices);
            if (db_source_attempted && !db_source_background && (register_value(kDbSrcCntl) & kDbSrc32Bpp)) {
                reject_vertex("db-source-32bpp-missing"); continue;
            }
            if (db_source_background) {
                blend_state.db_source_linear = true;
                blend_state.source_ssb_mask = m_db_source.ssb_mask;
                blend_state.db_source_height =
                    static_cast<float>(m_db_source.height);
            }
            if (db_source_secondary) {
                if (!db_source_background) {
                    reject_vertex("db-source-secondary");
                    continue;
                }
                blend_state.secondary_source = m_db_source.cel;
                blend_state.source_ssb_mask = m_db_source.ssb_mask;
                if (m_destination_blend_mode == DestinationBlendMode::source_over)
                    blend_state.mode = CltBlendMode::secondary_source_over;
                else if (m_destination_blend_mode == DestinationBlendMode::constant_alpha_over)
                    blend_state.mode = CltBlendMode::secondary_constant_over;
            }
            if (db_source_background && db_source_skip_enabled()) {
                reject_vertex("db-source-skip");
                continue;
            }
            const uint32_t new_tab_co =
                (register_value(kTxtTabCntl) >> 9u) & 3u;
            const uint32_t new_tab_ao =
                (register_value(kTxtTabCntl) >> 15u) & 3u;
            const bool new_tab_no_tex =
                new_tab_co == 0u && new_tab_ao == 0u;
            const bool new_tex_enabled =
                (register_value(kTxtAddrCntl) & kTxtAddrTextureEnable) != 0;
            if (db_source_secondary && m_destination_blend_mode != DestinationBlendMode::general_secondary &&
                (!new_tex_enabled || new_tab_no_tex)) {
                reject_vertex("db-source-secondary-texture-state");
                continue;
            }
            // Textured vertex packets with TEXTUREENABLE cleared are
            // untextured prim-color draws (SDK: enable gates mapping); do
            // not reject them as texture-state. Untextured packets reuse a
            // stale texture only when enable is still set.
            const bool effective_textured =
                (textured ? new_tex_enabled : has_texture) &&
                (!db_source_background || db_source_ssb_mask ||
                 db_source_secondary) &&
                !new_tab_no_tex &&
                !(!textured && blend_state.mode == CltBlendMode::general_secondary &&
                  blend_state.alpha_control_unsupported &&
                  !clt_dsb_rgb_uses_texture(blend_state.general));
            {
                static const bool dbsrc_diag = [] {
                    const char* e = std::getenv("M2_DBSRC_DIAG");
                    return e && e[0] != '0' && e[0] != '\0';
                }();
                if (dbsrc_diag) {
                    static uint32_t diag_n;
                    static bool diag_expired;
                    static const uint32_t draw_diag_from = [] {
                        const char* v = std::getenv("M2_DRAW_DIAG_FROM");
                        return v ? (uint32_t)strtoul(v, nullptr, 0) : 0u;
                    }();
                    const uint64_t diag_frame =
                        perf::counter_value(perf::Counter::GuestFramesStarted);
                    if (diag_n >= 48u && !diag_expired) {
                        diag_expired = true;
                        std::fprintf(stderr,
                                     "[clt-dbsrc] cap reached at n=%u — further "
                                     "entries suppressed\n",
                                     diag_n);
                    }
                    if (diag_frame >= draw_diag_from && diag_n < 48u) {
                        const uint32_t base = register_value(kDbSrcBaseAddr);
                        const uint32_t stride =
                            register_value(kDbSrcXStride) &
                            kDbSrcXStrideMask;
                        // Sample the source rows the live guest has at this
                        // moment (bounded: 3 rows of 16 rgb555 words).
                        if (memory) {
                            const size_t want =
                                (size_t)stride * 432u * 2u;
                            const CltMemoryView src =
                                find_memory(memory, base, want);
                            uint32_t row0[8] = {0}, row216[8] = {0};
                            if (src.data && src.size >= want) {
                                for (int k = 0; k < 8; ++k) {
                                    const size_t o0 =
                                        (size_t)(40 + k * 40) * 2u;
                                    const size_t o216 =
                                        ((size_t)216 * stride + 40 + k * 40) *
                                        2u;
                                    row0[k] = src.data && o0 + 1 < src.size
                                                  ? ((uint32_t)src.data[o0]
                                                     << 8) |
                                                        src.data[o0 + 1]
                                                  : 0;
                                    row216[k] =
                                        src.data && o216 + 1 < src.size
                                            ? ((uint32_t)src.data[o216] << 8) |
                                                  src.data[o216 + 1]
                                            : 0;
                                }
                            }
                            std::fprintf(stderr,
                                         "[clt-dbsrc] n=%u base=%#x stride=%u "
                                         "snap=%u bytes=%zu quad=(%.1f,%.1f)-"
                                         "(%.1f,%.1f) row40=%04x.%04x.%04x.%04x "
                                         "row216=%04x.%04x.%04x.%04x\n",
                                         diag_n, base, stride,
                                         src.data ? 1u : 0u,
                                         src.data ? src.size : 0u,
                                         vertices[0].x, vertices[0].y,
                                         vertices[2].x, vertices[2].y,
                                         row0[0], row0[1], row0[2], row0[3],
                                         row216[0], row216[1], row216[2],
                                         row216[3]);
                        }
                        ++diag_n;
                    }
                }
            }

            if (!textured && has_texture && !db_source_background) {
                // A fullscreen untextured quad armed with DB-source is the
                // 2D copy blit; do not divert it through the stale textured
                // path (which would stretch the previous option texture over
                // the whole screen and hide the background art).  The u/v
                // mapping for the genuine untextured-sprite case follows.
                for (CltVertex& vertex : vertices) {
                    vertex.u = vertex.x;
                    vertex.v = vertex.y;
                }
            }

            if (m_viewport_valid) {
                const float left = static_cast<float>(m_viewport_left);
                const float top = static_cast<float>(m_viewport_top);
                for (CltVertex& vertex : vertices) {
                    vertex.x += left;
                    vertex.y += top;
                }
            }

            if (db_source_secondary && db_source_background) {
                // The DB source uses the same linear TE address as the
                // untextured copy path. Keep its normalized coordinates in
                // the independent mask slots; the primary texture continues
                // to use vertex u/v.
                const float org_x = m_viewport_valid
                                        ? static_cast<float>(m_viewport_left)
                                        : 0.f;
                const float org_y = m_viewport_valid
                                        ? static_cast<float>(m_viewport_top)
                                        : 0.f;
                const uint32_t source_offset = register_value(kDbSrcOffset);
                for (CltVertex& vertex : vertices) {
                    const CltDbSourceCoordinates source_coord =
                        clt_db_source_coordinates(
                            vertex.x - org_x, vertex.y - org_y,
                            static_cast<float>(
                                m_db_source.db_source_origin_pixels),
                            source_offset, m_db_source.width,
                            m_db_source.height);
                    vertex.mask_u = source_coord.u;
                    vertex.mask_v = source_coord.v;
                }
            }

            if (effective_textured) {
                const uint32_t tab = register_value(kTxtTabCntl);
                const uint32_t first_color = tab & 7u;
                const uint32_t second_color = (tab >> 3u) & 7u;
                const uint32_t third_color = (tab >> 6u) & 7u;
                const uint32_t first_alpha = (tab >> 11u) & 3u;
                const uint32_t second_alpha = (tab >> 13u) & 3u;
                const uint32_t color_out = (tab >> 9u) & 3u;
                const uint32_t alpha_out = (tab >> 15u) & 3u;
                const uint32_t blend_op = (tab >> 17u) & 1u;
                Texture* new_tex =
                    texture_for_draw(register_value(kTxtLodBase0));
                ArmedDecode new_serve =
                    new_tex ? serve_for_draw(*new_tex,
                                             register_value(kTxtLodBase0),
                                             (discard_control & 0x4u) != 0u ||
                                             (register_value(kDbDestBaseAddr) != 0u &&
                                              blend_state.dsb.select == CltDsbSelect::object_ssb))
                            : ArmedDecode{};
                if (blend_state.dsb.select == CltDsbSelect::object_ssb)
                    blend_state.weighted_mix_ssb_mask = new_serve.ssb_mask;
                if (!new_tex || new_serve.cel == kInvalidCel ||
                    (register_value(kTxtAddrCntl) & kTxtAddrTextureEnable) == 0 ||
                    new_serve.width <= 0 || new_serve.height <= 0) {
                    static const bool new_tex_diag = [] {
                        const char* e = std::getenv("M2_DRAW_DIAG");
                        return e && *e && *e != '0';
                    }();
                    static const uint32_t new_tex_from = [] {
                        const char* e = std::getenv("M2_DRAW_DIAG_FROM");
                        return e ? static_cast<uint32_t>(
                                       strtoul(e, nullptr, 0))
                                 : 0u;
                    }();
                    const uint64_t new_tex_frame = perf::counter_value(
                        perf::Counter::GuestFramesStarted);
                    static uint32_t new_tex_emitted = 0;
                    static bool new_tex_expired = false;
                    if (new_tex_emitted >= 64u && !new_tex_expired) {
                        new_tex_expired = true;
                        std::fprintf(stderr,
                                     "[clt-reject-texstate] cap reached at "
                                     "frame %llu — further entries "
                                     "suppressed\n",
                                     (unsigned long long)new_tex_frame);
                    }
                    if ((bridge_trace_enabled() ||
                         (new_tex_diag && new_tex_frame >= new_tex_from)) &&
                        new_tex_emitted < 64u) {
                        ++new_tex_emitted;
                        std::fprintf(stderr,
                                     "[clt-reject-texstate] cont=0 cel=%d "
                                     "enable=%d w=%d h=%d in_span=%d "
                                     "pending=%d lod_base0=%#x lod1=%#x "
                                     "lod2=%#x lod3=%#x tram_base=%#x "
                                     "tram_size=%#x ld_cntl=%#x exp=%#x "
                                     "header=%#x tab=%#x\n",
                                     m_texture.cel,
                                     (register_value(kTxtAddrCntl) &
                                      kTxtAddrTextureEnable) != 0,
                                     m_texture.width, m_texture.height,
                                     in_texture_span(register_value(
                                         kTxtLodBase0)),
                                     m_pending_mmdma_texture ? 1 : 0,
                                     register_value(kTxtLodBase0),
                                     register_value(0x6418u),
                                     register_value(0x641cu),
                                     register_value(0x6420u),
                                     m_texture.tram_base, m_texture.tram_size,
                                     register_value(kTxtLdCntl),
                                     register_value(kTxtExpType), header,
                                     register_value(kTxtTabCntl));
                        if (new_tex_emitted == 64u && !new_tex_expired) {
                            new_tex_expired = true;
                            std::fprintf(stderr,
                                         "[clt-reject-texstate] cap reached "
                                         "at frame %llu — further entries "
                                         "suppressed\n",
                                         (unsigned long long)new_tex_frame);
                        }
                    }
                    reject_vertex("texture-state");
                    if (livery_diag_enabled_for_execute)
                        livery_tally_note_texreject();
                    continue;
                }

                float texture_tint_r = 1.f;
                float texture_tint_g = 1.f;
                float texture_tint_b = 1.f;
                bool new_preserve_prim_alpha = false;
                // PRIMCOLOR * TEXCOLOR (M_TBLitTex): the per-vertex primitive
                // color is an input to the texture blend, so the backend must
                // multiply texel * vertex color.
                bool new_preserve_prim_color = false;
                // TAB CONSTALPHA form: the stage alpha is a product of the
                // texel alpha and the TAB constant, so it replaces the
                // per-vertex alpha factor instead of preserving it.
                bool new_tab_const_alpha_applies = false;
                float new_tab_const_alpha = 1.f;
                // When both outputs bypass the TAB blender, its input
                // selectors and blend operation are explicitly don't-care.
                const bool direct_texture = color_out == 1u && alpha_out == 1u;
                const bool observed_multiply =
                    first_color == 3u && second_color == 5u &&
                    third_color == 0u && first_alpha == 0u &&
                    second_alpha == 0u && color_out == 2u && alpha_out == 1u &&
                    blend_op == 1u;
                const bool alpha_color = first_color == 2u &&
                    second_color == 5u && third_color == 0u &&
                    first_alpha == 0u && second_alpha == 0u &&
                    color_out == 2u && alpha_out == 0u && blend_op == 1u;
                const bool new_tex_bypass_alpha =
                    first_color == 0u && second_color == 0u &&
                    third_color == 0u && first_alpha == 0u &&
                    second_alpha == 1u && color_out == 1u && alpha_out == 2u &&
                    blend_op == 1u;
                // spaceship.c `drawspaceship` (guest .text+0xf2e8, the arm its
                // caller selects with the second argument) authors
                // TXTTABCNTL = 0x34a2a with TXTCONST2/3 = 0x40000000:
                // FIRSTALPHA=TEXALPHA, SECONDALPHA=CONSTALPHA, ALPHAOUT=BLEND,
                // BLENDOP=MULT, COLOROUT=TEXCOLOR. COLOROUT takes the texture
                // color straight out of the blender, so the color inputs and
                // their result are discarded -- the same don't-care reading
                // `direct_texture` already applies to a fully bypassed TAB --
                // while the alpha path is the authored product
                // TEXALPHA * CONSTALPHA and therefore not bypassed.
                const bool bypass_color_const_alpha =
                    color_out == 1u && alpha_out == 2u &&
                    first_alpha == 1u && second_alpha == 2u && blend_op == 1u;
                const bool primitive_multiply = first_color == 1u && second_color == 3u &&
                    (third_color == 0u || third_color == 2u) && first_alpha == 0u &&
                    second_alpha == 0u && color_out == 2u && alpha_out == 0u && blend_op == 1u;
                const uint32_t tab_const0 = register_value(kTxtConst2);
                const uint32_t tab_const1 = register_value(kTxtConst3);
                const bool general_tab = clt_tab_documented(tab) &&
                    (m_destination_blend_mode == DestinationBlendMode::general_internal ||
                     m_destination_blend_mode == DestinationBlendMode::general_secondary ||
                     !(direct_texture || observed_multiply || alpha_color || new_tex_bypass_alpha ||
                       bypass_color_const_alpha || primitive_multiply) ||
                     ((observed_multiply || alpha_color) &&
                      ((tab_const0 ^ tab_const1) & 0xffffffu)) ||
                     (bypass_color_const_alpha && ((tab_const0 ^ tab_const1) & 0x7f000000u)));
                if (general_tab) {
                    new_serve = serve_for_draw(*new_tex, register_value(kTxtLodBase0), true);
                    if (new_serve.cel == kInvalidCel) {
                        reject_vertex("texture-state");
                        continue;
                    }
                    blend_state.tab = {true, tab, {tab_const0, tab_const1}};
                    blend_state.weighted_mix_ssb_mask = new_serve.ssb_mask;
                    new_preserve_prim_color = new_preserve_prim_alpha = true;
                } else if (observed_multiply || alpha_color) {
                    // CLT MULT is A*B. The recovered list selects texture
                    // color for A and the TAB constant for B. SSB selection
                    // is safe to discard here because both published
                    // constants are the same color.
                    const uint32_t constant0 = register_value(kTxtConst2);
                    const uint32_t constant1 = register_value(kTxtConst3);
                    if ((constant0 & 0x00ffffffu) !=
                        (constant1 & 0x00ffffffu)) {
                        reject_vertex("TAB-constant");
                        continue;
                    }
                    texture_tint_r = color_component(constant0, 16u);
                    texture_tint_g = color_component(constant0, 8u);
                    texture_tint_b = color_component(constant0, 0u);
                    blend_state.texture_alpha_as_color = alpha_color;
                    new_preserve_prim_alpha = alpha_color;
                } else if (!direct_texture && !new_tex_bypass_alpha) {
                    // CLT MULT is A*B with the t input don't-care. fc==1
                    // selects Citer (per-vertex primitive color), sc==3
                    // selects Ct (texture color), so the blend output is
                    // PRIMCOLOR * TEXCOLOR (M_TBLitTex semantics); the TAB
                    // constants are not inputs to this form.
                    const bool observed_prim_multiply =
                        first_color == 1u && second_color == 3u &&
                        (third_color == 0u || third_color == 2u) &&
                        first_alpha == 0u && second_alpha == 0u &&
                        color_out == 2u && alpha_out == 0u && blend_op == 1u;
                    if (bypass_color_const_alpha) {
                        // The spaceship form's stage alpha is the TAB's own
                        // product, so the primitive alpha is not an input: the
                        // constant alpha takes the place of the per-vertex
                        // factor the backend multiplies the texel alpha by.
                        // TXTCONST2 (SSB 0) and TXTCONST3 (SSB 1) are selected
                        // by the filtered texel's SSB, which this path does not
                        // resolve, so a draw whose two constants disagree on
                        // alpha stays fail-closed.
                        const uint32_t const_alpha2 =
                            (register_value(kTxtConst2) >> 24) & 0x7fu;
                        const uint32_t const_alpha3 =
                            (register_value(kTxtConst3) >> 24) & 0x7fu;
                        if (const_alpha2 != const_alpha3) {
                            reject_vertex("TAB-constant-alpha");
                            continue;
                        }
                        // The constant register's alpha field is the
                        // documented 7 bits (FV_TXTCONST*_ALPHA_MASK =
                        // PPCMASK(0x7F, 7)); the pipeline alpha the DSB
                        // coefficients multiply is 8-bit (DBSRCALPHACNTL tests
                        // `alpha == 0xff`), and no scaling of the constant field
                        // is documented, so the field is zero-extended into
                        // that range.
                        new_tab_const_alpha =
                            static_cast<float>(const_alpha2) / 255.f;
                        new_tab_const_alpha_applies = true;
                    } else if (!observed_prim_multiply) {
                        reject_vertex("TAB-form");
                        continue;
                    } else {
                        if (m_destination_blend_mode ==
                                DestinationBlendMode::specular_add ||
                            m_destination_blend_mode ==
                                DestinationBlendMode::fog_composite) {
                            // M_TBLitTex's ALPHAOUT=PRIMALPHA feeds the
                            // TEXALPHA coefficient selected by M_DBSpec, and
                            // the M_DBFog distance lerp uses that same stage
                            // alpha as its fog weight (M_LightFog wrote the
                            // per-vertex factor into PRIMALPHA). Keep the
                            // primitive alpha for both internal composites; no
                            // other destination equation consumes it.
                            new_preserve_prim_alpha = true;
                        }
                        new_preserve_prim_color = true;
                    }
                } else if (new_tex_bypass_alpha) {
                    // Authentic 0x32200: color bypass (TEXCOLOR), alpha BLEND
                    // MULT of PRIMALPHA*TEXTALPHA. Preserve per-vertex prim
                    // alpha; backend multiplies tex*vertex.
                    new_preserve_prim_alpha = true;
                }

                // DBDISCARDCONTROL tests occur after all texture/TAB work.
                // PIP/source SSB is retained independently from RGBA alpha;
                // RGB0 is the SDK-defined all-zero post-texturing RGB test;
                // ZCLIPPED is carried through CltDepth's Z region contract.
                // Any other bit stays fail-closed.
                if ((discard_control & ~0xfu) != 0u) {
                    reject_vertex("discard-bits");
                    continue;
                }
                CltDiscard discard;
                discard.ssb_zero = (discard_control & 0x4u) != 0u;
                discard.alpha_zero = (discard_control & 0x1u) != 0u;
                discard.rgb_zero = (discard_control & 0x2u) != 0u;
                if (discard.ssb_zero) {
                    if (!new_tex || new_serve.ssb_mask == kInvalidCel) {
                        reject_vertex("discard-SSB-mask");
                        continue;
                    }
                    discard.ssb_mask = new_serve.ssb_mask;
                }

                const float texture_width = static_cast<float>(new_serve.width);
                const float texture_height =
                    static_cast<float>(new_serve.height);
                // Bounded accept audit: for textured draws that pass
                // all gates, log the serving TRAM entry vs requested LOD base
                // plus blend/depth/TAB, under the existing diagnostic floor. This
                // distinguishes cache-aliasing (road served sky) from guest
                // culling (road never submitted) without a new env var.
                if (draw_diag_for_execute) {
                    const bool acc_diag = true;
                    const uint32_t acc_from = draw_diag_from_for_execute;
                    static uint32_t acc_max = 0;
                    static bool acc_max_checked = false;
                    if (!acc_max_checked) {
                        acc_max_checked = true;
                        if (clt_diag_list_filter() != UINT64_MAX) {
                            acc_max = 20000u;
                        } else {
                            const char* m = std::getenv("M2_ACCEPT_MAX");
                            acc_max = 32u;
                            if (m && *m) {
                                const long v = std::strtol(m, nullptr, 0);
                                if (v > 32L) acc_max = static_cast<uint32_t>(
                                    v > 512L ? 512L : v);
                            }
                        }
                    }
                    const uint64_t acc_frame = perf::counter_value(
                        perf::Counter::GuestFramesStarted);
                    static uint32_t acc_emitted = 0;
                    static bool acc_expired = false;
                    if (acc_emitted >= acc_max && !acc_expired) {
                        acc_expired = true;
                        std::fprintf(stderr,
                                     "[clt-accept] cap reached at frame %llu — "
                                     "further entries suppressed\n",
                                     (unsigned long long)acc_frame);
                    }
                    if ((acc_diag &&
                         (acc_frame >= acc_from ||
                          clt_diag_list_filter() != UINT64_MAX) &&
                         clt_diag_list_selected(m_exec_seq)) &&
                        acc_emitted < acc_max) {
                        ++acc_emitted;
                        float acc_mr = 0.f, acc_mg = 0.f, acc_mb = 0.f;
                        float acc_mu = 0.f, acc_mv = 0.f;
                        for (const CltVertex& acc_v : vertices) {
                            acc_mr += acc_v.r;
                            acc_mg += acc_v.g;
                            acc_mb += acc_v.b;
                            acc_mu += acc_v.u;
                            acc_mv += acc_v.v;
                        }
                        const float acc_n = static_cast<float>(
                            vertices.empty() ? 1u : vertices.size());
                        std::fprintf(stderr,
                                     "[clt-accept] cont=0 dbuser=%#x "
                                     "lod0=%#x lod1=%#x serve_base=%#x serve_size=%#x "
                                     "serve_wh=%dx%d armed=%d sdecl=%d "
                                     "dec=%dx%d pub=%u staged=%d "
                                     "tab=%#x pip=%#x "
                                     "uvmax=%#x uvmask=%#x addr=%#x "
                                     "ldctl=%#x count=%#x "
                                     "c2=%#x c3=%#x exp=%#x "
                                     "escnl=%#x ow=%.4g "
                                     "tint=(%.2f,%.2f,%.2f) "
                                     "vrgb=(%.2f,%.2f,%.2f) "
                                     "uvmean=(%.1f,%.1f) "
                                     "blend=%d ztest=%d zwrite=%d header=%#x "
                                     "v0=(%.1f,%.1f)\n",
                                     register_value(kDbUserControl),
                                     register_value(kTxtLodBase0),
                                     register_value(0x6418u),
                                     new_tex->tram_base, new_tex->tram_size,
                                     new_tex->width, new_tex->height,
                                     new_serve.armed ? 1 : 0,
                                     new_serve.declined ? 1 : 0,
                                     new_serve.width, new_serve.height,
                                     new_serve.publisher,
                                     new_serve.resident_staged ? 1 : 0,
                                     register_value(kTxtTabCntl),
                                     register_value(kTxtPipCntl),
                                     register_value(kTxtLdWidth),
                                     register_value(0x6430u),
                                     register_value(kTxtAddrCntl),
                                     register_value(kTxtLdCntl),
                                     register_value(kTxtCount),
                                     register_value(kTxtConst2),
                                     register_value(kTxtConst3),
                                     register_value(kTxtExpType),
                                     register_value(0x4000u),
                                     vertices[0].one_over_w,
                                     texture_tint_r, texture_tint_g,
                                     texture_tint_b, acc_mr / acc_n,
                                     acc_mg / acc_n, acc_mb / acc_n,
                                     acc_mu / acc_n, acc_mv / acc_n,
                                     blend_enabled ? 1 : 0,
                                     depth.test_enabled ? 1 : 0,
                                     depth.write_enabled ? 1 : 0, header,
                                     vertices[0].x, vertices[0].y);
                        if (acc_emitted == acc_max && !acc_expired) {
                            acc_expired = true;
                            std::fprintf(stderr,
                                         "[clt-accept] cap reached at frame "
                                         "%llu — further entries suppressed\n",
                                         (unsigned long long)acc_frame);
                        }
                    }
                }
                livery_draw_diag("new", new_tex, vertices.data(),
                                 blend_enabled, discard, depth);
                if (livery_diag_enabled_for_execute)
                    livery_tally_note_textured(
                        new_tex->cel, new_tex->width, new_tex->height,
                        vertices.data(), vertices.size());
                for (CltVertex& vertex : vertices) {
                    vertex.u /= texture_width;
                    vertex.v /= texture_height;
                    if (!new_preserve_prim_color) {
                        vertex.r = texture_tint_r;
                        vertex.g = texture_tint_g;
                        vertex.b = texture_tint_b;
                    }
                    // Both supported multiply forms select texture alpha as
                    // output; the 0x32200 bypass-alpha form preserves prim.
                    // The spaceship form's stage alpha is the TAB constant
                    // alpha times the texel alpha, so the constant takes the
                    // per-vertex alpha slot the backend scales the texel by.
                    if (new_tab_const_alpha_applies)
                        vertex.a = new_tab_const_alpha;
                    else if (!new_preserve_prim_alpha)
                        vertex.a = 1.f;
                }

                if (db_source_ssb_mask) {
                    if (!db_source_background || !discard.ssb_zero ||
                        discard.ssb_mask == kInvalidCel) {
                        reject_vertex("db-source-SSB-mask");
                        continue;
                    }
                    const float org_x =
                        m_viewport_valid ? m_viewport_left : 0.f;
                    const float org_y =
                        m_viewport_valid ? m_viewport_top : 0.f;
                    const uint32_t source_offset =
                        register_value(kDbSrcOffset);
                    for (CltVertex& vertex : vertices) {
                        vertex.mask_u = vertex.u;
                        vertex.mask_v = vertex.v;
                        const CltDbSourceCoordinates source_coord =
                            clt_db_source_coordinates(
                                vertex.x - org_x, vertex.y - org_y,
                                static_cast<float>(
                                    m_db_source.db_source_origin_pixels),
                                source_offset, m_db_source.width,
                                m_db_source.height);
                        vertex.u = source_coord.u;
                        vertex.v = source_coord.v;
                        vertex.r = 1.f;
                        vertex.g = 1.f;
                        vertex.b = 1.f;
                    }
                    discard.separate_ssb_uv = true;
                    draw_new_triangles(true, m_db_source.cel, blend_enabled,
                                       discard, depth, {}, m_db_source.width,
                                       m_db_source.height, blend_state,
                                       new_tex);
                    continue;
                }
                CltTextureSampling sampling;
                if (!texture_sampling_state(register_value(kTxtAddrCntl),
                                            register_value(0x6430),
                                            texture_width, texture_height,
                                            &sampling)) {
                    // Reserved min/mag/inter selector: fail closed rather
                    // than deliver a plausible point/bilinear filter for a
                    // value the vendor header leaves undefined.
                    reject_vertex("texture-filter-reserved");
                    continue;
                }
                draw_new_triangles(true, new_serve.cel, blend_enabled, discard,
                                   depth, sampling, new_serve.width,
                                   new_serve.height, blend_state, new_tex);
                continue;
            }

            if ((!db_source_background || db_source_secondary) &&
                (discard_control & ~0xbu) == 0u && discard_control != 0u) {
                // Untextured ALPHA0/RGB0: pass through; the white-texture
                // backend path applies the post-texturing tests to the
                // primitive color. ZCLIPPED (0x8) is carried by CltDepth and
                // must pass through here with no CltDiscard bits set.
                CltDiscard untex_discard;
                untex_discard.alpha_zero = (discard_control & 0x1u) != 0u;
                untex_discard.rgb_zero = (discard_control & 0x2u) != 0u;
                draw_new_triangles(false, kInvalidCel, blend_enabled,
                                   untex_discard, depth, {}, 0, 0,
                                   blend_state);
                continue;
            }
            if ((!db_source_background || db_source_secondary) && discard_control != 0u) {
                reject_vertex("discard-untextured");
                continue;
            }

            // DB-source 2D copy blit: when the guest arms DBSRCCNTL /
            // DBSRCBASEADDR / DBSRCXSTRIDE and draws an untextured quad, the
            // quad samples the named source buffer instead of the flat
            // DBCONSTIN colour (the recovered 576x432 background bitmap).
            if (db_source_background && !db_source_secondary) {
                CltDiscard db_source_discard;
                if (!decode_db_source_discard(discard_control,
                                              &db_source_discard)) {
                    reject_vertex("discard-db-source");
                    continue;
                }

                // The source buffer is a 2D copy blit: the untextured quad's
                // own pixel coordinates select texels in the armed source
                // buffer (uv = xy), exactly as the has_texture routing above
                // maps untextured u/v to vertex coordinates.
                const float org_x = m_viewport_valid ? m_viewport_left : 0.f;
                const float org_y = m_viewport_valid ? m_viewport_top : 0.f;
                // DBSRCOFFSET is a signed 12-bit source-minus-destination
                // coordinate pair (Portfolio F2_CopyRect), not a byte offset
                // to DBSRCBASEADDR.
                const uint32_t source_offset = register_value(kDbSrcOffset);
                for (CltVertex& vertex : vertices) {
                    const CltDbSourceCoordinates source_coord =
                        clt_db_source_coordinates(
                            vertex.x - org_x, vertex.y - org_y,
                            static_cast<float>(
                                m_db_source.db_source_origin_pixels),
                            source_offset, m_db_source.width,
                            m_db_source.height);
                    vertex.u = source_coord.u;
                    vertex.v = source_coord.v;
                    vertex.r = 1.f;
                    vertex.g = 1.f;
                    vertex.b = 1.f;
                }
                if (livery_diag_enabled_for_execute)
                    livery_tally_note_db();
                draw_new_triangles(true, m_db_source.cel, blend_enabled,
                                   db_source_discard, depth, {},
                                   m_db_source.width,
                                   m_db_source.height, blend_state);
                continue;
            }

            draw_new_triangles(false, kInvalidCel, blend_enabled, {}, depth, {},
                               0, 0, blend_state);
            continue;
        }

        if (type == kPointType) {
            // Silent-unsupported site 3/3 (diag only): point packets never
            // log. Key = header + position + following parameter words.
            if (clt_unsupported_diag_enabled()) {
                static uint32_t emitted = 0;
                static bool expired = false;
                if (emitted < clt_unsupported_diag_max()) {
                    const uint32_t w1 =
                        header_index + 1u < word_count ? words[header_index + 1u] : 0u;
                    const uint32_t w2 =
                        header_index + 2u < word_count ? words[header_index + 2u] : 0u;
                    const uint32_t w3 =
                        header_index + 3u < word_count ? words[header_index + 3u] : 0u;
                    std::fprintf(stderr,
                                 "[clt-reject] point-type header=%#x "
                                 "hdr_index=%zu cursor=%zu words=%zu w1=%#x "
                                 "w2=%#x w3=%#x list=%llu frame=%llu\n",
                                 header, header_index, cursor, word_count, w1,
                                 w2, w3, (unsigned long long)m_exec_seq,
                                 (unsigned long long)perf::counter_value(
                                     perf::Counter::GuestFramesStarted));
                    ++emitted;
                } else if (!expired) {
                    expired = true;
                    std::fprintf(stderr,
                                 "[clt-reject] cap reached at frame "
                                 "%llu — further entries suppressed\n",
                                 (unsigned long long)perf::counter_value(
                                     perf::Counter::GuestFramesStarted));
                }
            }
            // A point packet is a recognized CLT packet type that this
            // interpreter does not implement, not malformed guest data. Leave
            // result.ok set so the bridge's "!ok -> malformed" mapping does not
            // charge one refused point packet twice: once as unsupported here
            // and again as malformed there. This is the same accounting the
            // DINT/flow-control refusals above already use, and it keeps the
            // contrast with the unknown-type return below, which genuinely is
            // malformed and must keep clearing ok. The list still stops at the
            // packet and nothing is rendered for it.
            ++result.unsupported;
            result.words_consumed = header_index;
            flush_pending_batch();
            return result;
        }

        // Unknown packet type: feeds malformed (not unsupported), but log
        // under the same gate so "any other" unsupported-adjacent traffic is
        // attributable with a register snapshot instead of silent.
        if (clt_unsupported_diag_enabled()) {
            static uint32_t emitted = 0;
            static bool expired = false;
            if (emitted < clt_unsupported_diag_max()) {
                std::fprintf(stderr,
                             "[clt-reject] unknown-type header=%#x "
                             "hdr_index=%zu words=%zu dbuser=%#x discard=%#x "
                             "tab=%#x exptype=%#x list=%llu frame=%llu\n",
                             header, header_index, word_count,
                             register_value(kDbUserControl),
                             register_value(kDbDiscardControl),
                             register_value(kTxtTabCntl),
                             register_value(kTxtExpType),
                             (unsigned long long)m_exec_seq,
                             (unsigned long long)perf::counter_value(
                                 perf::Counter::GuestFramesStarted));
                ++emitted;
            } else if (!expired) {
                expired = true;
                std::fprintf(stderr,
                             "[clt-reject] cap reached at frame "
                             "%llu — further entries suppressed\n",
                             (unsigned long long)perf::counter_value(
                                 perf::Counter::GuestFramesStarted));
            }
        }
        result.ok = false;
        result.words_consumed = header_index;
        flush_pending_batch();
        return result;
    }

    result.words_consumed = cursor;
    flush_pending_batch();
    emit_exec_section_timing();
    return result;
}

GuestCltBridge::GuestCltBridge(ICelBlitterBackend* backend)
    : m_backend(backend), m_interpreter(backend)
{
    perf::TimedMutexLock active_lock(active_bridge_mutex());
    active_bridge() = this;
}

GuestCltBridge::~GuestCltBridge()
{
    detach();
}

int32_t GuestCltBridge::read_framebuffer(uint32_t base, void* dst, uint32_t count)
{
    if (!dst || guest_screenshot::height_for_bytes(count) == 0) return -1;
    perf::TimedMutexLock lock(m_mutex);
    if (!m_accepting.load() || m_framebuffer_read) return -1;
    const auto span = std::find_if(m_fb_spans.begin(), m_fb_spans.end(),
        [base, count](const CltFbSpan& s) { return s.ea == base && s.size == count; });
    if (span == m_fb_spans.end()) return -1;
    auto read = std::make_shared<FramebufferRead>();
    read->base = base;
    read->frame = m2_guest_current_frame_c();
    read->sequence = m_submission_sequence;
    read->state = m_graphics_state;
    read->bytes.resize(count);
    m_framebuffer_read = read;
    m_queue_cv.wait(lock.native_lock(), [&] { return read->done || !m_accepting.load(); });
    if (m_framebuffer_read == read) m_framebuffer_read.reset();
    if (!read->done || !read->ok) return -1;
    std::memcpy(dst, read->bytes.data(), count);
    return static_cast<int32_t>(count);
}

void GuestCltBridge::service_framebuffer_reads()
{
    std::shared_ptr<FramebufferRead> read;
    {
        perf::TimedMutexLock lock(m_mutex);
        read = m_framebuffer_read;
        if (!read || read->done || !m_accepting.load()) return;
    }
    // A CPU framebuffer read is a fence. Drain earlier TE submissions even
    // while this application frame waits for the read; keep display fences
    // and all later submissions in order. Never wait for the frame to finish.
    pump(read->frame, "framebuffer-read");
    {
        perf::TimedMutexLock lock(m_mutex);
        if (!m_queue.empty() && m_queue.front().sequence <= read->sequence) return;
    }
    bool ok = false;
    int w = 0, h = 0;
    if (m_backend) m_backend->get_size(&w, &h);
    const auto& gs = read->state;
    const int dw = gs.display_width, dh = gs.display_height;
    // A target holds Bitmap rows 1:1 from the View origin. Line doubling happens
    // later at scanout (CltScanoutRect), so a 216-row Bitmap reads the same way.
    const int rows = guest_screenshot::height_for_bytes(uint32_t(read->bytes.size()));
    if (w > 0 && h > 0 && dw > 0 && dh > 0 &&
        gs.view_left >= 0 && gs.view_top >= 0 &&
        gs.view_left + guest_screenshot::width <= dw &&
        gs.view_top + rows <= dh) {
        perf::Scope readback_scope(perf::Phase::FramebufferReadback);
        std::vector<uint32_t> rgba(size_t(w) * h), metadata(size_t(w) * h);
        ok = m_backend->grab_clt_target_rgba(read->base, rgba.data(), w, h) &&
             m_backend->grab_clt_target_window_metadata(read->base, metadata.data(), 0, 0, w, h);
        if (ok) {
            for (int y = 0; y < rows; ++y) {
                const int fy = int(int64_t(gs.view_top + y) * h / dh);
                for (int x = 0; x < guest_screenshot::width; ++x) {
                    const int fx = int(int64_t(gs.view_left + x) * w / dw);
                    const size_t p = size_t(fy) * w + fx;
                    const uint32_t rgb = rgba[p];
                    const uint16_t word = uint16_t(
                        ((metadata[p] & 255u) >= 128u ? 0x8000u : 0u) |
                        (((rgb & 255u) >> 3u) << 10u) |
                        ((((rgb >> 8u) & 255u) >> 3u) << 5u) |
                        (((rgb >> 16u) & 255u) >> 3u));
                    const size_t out = (size_t(y) * guest_screenshot::width + x) * 2;
                    read->bytes[out] = uint8_t(word >> 8u);
                    read->bytes[out + 1] = uint8_t(word);
                }
            }
        }
    }
    {
        perf::TimedMutexLock lock(m_mutex);
        read->ok = ok;
        read->done = true;
    }
    m_queue_cv.notify_all();
}

void GuestCltBridge::enqueue(uint32_t io_item, uint32_t command,
                             uint32_t command_options, uint32_t guest_buffer,
                             uint32_t byte_count,
                             const uint8_t* guest_bytes,
                             const uint8_t* guest_memory,
                             uint32_t guest_memory_size)
{
    const bool empty_clear =
        command == kTeExecuteListCommand && byte_count == 0u &&
        (command_options & (kTeClearFrameBuffer | kTeClearZBuffer)) != 0u;
    if ((!guest_bytes && byte_count != 0u) ||
        (byte_count == 0u && !empty_clear) || (byte_count & 3u) != 0 ||
        (command == kTeExecuteListCommand && byte_count != 0u &&
         (guest_buffer & 3u) != 0u) ||
        byte_count > kMaxListBytes) {
        perf::TimedMutexLock lock(m_mutex);
        ++m_stats.dropped;
        return;
    }

    const size_t word_count = byte_count / sizeof(uint32_t);
    Submission submission;
    submission.io_item = io_item;
    submission.command = command;
    submission.command_options = command_options;
    submission.guest_buffer = guest_buffer;
    // Frame provenance for the interactive look-ahead pump: the guest counter
    // increments at the StateDriver loop head before the frame body runs, so
    // every list copied during body K carries K. The bridge can then execute
    // exactly the completed frames while the guest computes the next one.
    submission.guest_frame = m2_guest_current_frame_c();
    if (clt_ring_audit_enabled() && io_item == 0u &&
        command == kTeExecuteListCommand)
        submission.guest_checksum = clt_guest_checksum(guest_bytes, byte_count);
    {
        perf::Scope payload_scope(perf::Phase::GuestPayloadCopy);
        submission.words.resize(word_count);
        for (size_t i = 0; i < word_count; ++i) {
            const size_t byte = i * sizeof(uint32_t);
            submission.words[i] =
                (static_cast<uint32_t>(guest_bytes[byte]) << 24u) |
                (static_cast<uint32_t>(guest_bytes[byte + 1]) << 16u) |
                (static_cast<uint32_t>(guest_bytes[byte + 2]) << 8u) |
                static_cast<uint32_t>(guest_bytes[byte + 3]);
        }
    }
    perf::add_bytes(perf::Phase::GuestPayloadCopy, byte_count);
    perf::add_counter(perf::Counter::CltPayloadBytes, byte_count);
    // Texture snapshotting addresses only this submission's copied words and
    // live guest RAM, so it runs before the bridge lock. The interactive pump
    // can then interpret the previous frame while the guest thread copies the
    // next frame's source ranges instead of serializing on m_mutex. The
    // collection scratch is thread-local because two guest tasks may submit
    // concurrently.
    {
        perf::Scope snapshot_scope(perf::Phase::TextureSnapshotCopy);
        static thread_local std::vector<uint32_t> collection_registers;
        if (!submission.words.empty()) {
            collect_texture_memory(
                submission.words, guest_memory, guest_memory_size,
                submission.guest_buffer, submission.words.size(),
                &collection_registers, &submission.memory);
        }
    }
    perf::TimedMutexLock lock(m_mutex);
    if (!m_attached || !m_accepting.load(std::memory_order_acquire)) {
        ++m_stats.dropped;
        return;
    }

    {
        perf::Scope snapshot_scope(perf::Phase::TextureSnapshotCopy);
        if (!submission.words.empty()) {
            // M2_CLT_Z_CELL_DUMP cell readout (diagnostic-only): production
            // depth never addresses guest Z cells, so no payload/sheet span
            // covers the dump window and the execute-time find_memory misses.
            // Snapshot exactly the dump window here on the guest thread —
            // the only layer with live guest RAM (the interpreter keeps no
            // guest pointer by design, so a live read at execute() time is
            // unavailable). Opt-in only via DUMP; bounded by dump_span_bytes
            // (~1 KiB for the 16-word/576 live window, ~124 KiB at the
            // 256-word/2048-stride corner) under the existing snapshot cap.
            // The bound plane comes from the TE setup path (SetTEZBuffer;
            // the guest never writes DBZBASEADDR on route), i.e. the same
            // setup values this submission carries into set_te_surface_state.
            // Fail-closed: any invalid base/stride/span snapshots nothing
            // and the dump keeps its one unmapped line. Raw copy, never
            // byteswapped; never alters rendering.
            const uint32_t z_dump_words =
                clt_z_cell_probe::dump_word_count();
            const uint32_t z_base = m_graphics_state.te_db_z_base;
            const uint32_t z_stride =
                m_graphics_state.te_db_dest_x_stride & 0xffffu;
            const size_t z_span = (z_dump_words == 0 || z_base == 0 ||
                                   z_stride == 0 || z_stride > 2048u)
                                      ? 0
                                      : clt_z_cell_probe::dump_span_bytes(
                                            z_dump_words, z_stride);
            const uint64_t z_end = static_cast<uint64_t>(z_base) + z_span;
            if (z_span != 0 && z_span <= kMaxTextureSnapshotBytes &&
                z_end <= 0x100000000ull) {
                snapshot_range(&submission.memory, guest_memory,
                               guest_memory_size, z_base, z_span);
            }
        }
    }
    submission.graphics_state = m_graphics_state;

    if (m_queue.size() >= kMaxQueuedLists) {
        perf::Scope queue_wait_scope(perf::Phase::CltQueueWait);
        m_queue_cv.wait(lock.native_lock(), [this] {
            return !m_attached ||
                   !m_accepting.load(std::memory_order_acquire) ||
                   m_queue.size() < kMaxQueuedLists;
        });
    }
    if (!m_attached || !m_accepting.load(std::memory_order_acquire)) {
        ++m_stats.dropped;
        return;
    }

    if (bridge_trace_enabled()) {
        static std::atomic<uint32_t> trace_count{0};
        const uint32_t trace_index = trace_count.fetch_add(1);
        if (trace_index < 32u || !submission.memory.ranges.empty()) {
            std::fprintf(stderr,
                         "[clt-bridge] submit[%u] io=%#x command=%#x "
                         "buf=%#x bytes=%u words=%zu memory_ranges=%zu\n",
                         trace_index, io_item, command, guest_buffer, byte_count,
                         word_count, submission.memory.ranges.size());
            const size_t preview_words = std::min<size_t>(
                word_count, bridge_trace_full() ? 256u : 24u);
            std::fprintf(stderr, "[clt-bridge] words[%u]:", trace_index);
            for (size_t i = 0; i < preview_words; ++i)
                std::fprintf(stderr, " %08x", submission.words[i]);
            if (preview_words < word_count) std::fprintf(stderr, " ...");
            std::fputc('\n', stderr);
            if (bridge_trace_full() && word_count > 512u) {
                const size_t middle = word_count / 2u;
                const size_t begin = middle > 64u ? middle - 64u : 0u;
                const size_t end = std::min(word_count, begin + 128u);
                std::fprintf(stderr,
                             "[clt-bridge] middle[%u] offset=%zu guest=%#x:",
                             trace_index, begin,
                             guest_buffer + static_cast<uint32_t>(begin * 4u));
                for (size_t i = begin; i < end; ++i)
                    std::fprintf(stderr, " %08x", submission.words[i]);
                if (end < word_count) std::fprintf(stderr, " ...");
                std::fputc('\n', stderr);
                if (word_count > 2048u) {
                    const size_t near_middle = middle > 512u
                                                   ? middle - 512u
                                                   : 0u;
                    const size_t near_end = std::min(
                        word_count, near_middle + 256u);
                    std::fprintf(stderr,
                                 "[clt-bridge] near-middle[%u] offset=%zu "
                                 "guest=%#x:",
                                 trace_index, near_middle,
                                 guest_buffer + static_cast<uint32_t>(
                                                     near_middle * 4u));
                    for (size_t i = near_middle; i < near_end; ++i)
                        std::fprintf(stderr, " %08x", submission.words[i]);
                    if (near_end < word_count) std::fprintf(stderr, " ...");
                    std::fputc('\n', stderr);
                    const size_t early_begin = 768u;
                    const size_t early_end = std::min(word_count, size_t(1152));
                    std::fprintf(stderr,
                                 "[clt-bridge] early-large[%u] offset=%zu "
                                 "guest=%#x:",
                                 trace_index, early_begin,
                                 guest_buffer + static_cast<uint32_t>(
                                                     early_begin * 4u));
                    for (size_t i = early_begin; i < early_end; ++i)
                        std::fprintf(stderr, " %08x", submission.words[i]);
                    if (early_end < word_count) std::fprintf(stderr, " ...");
                    std::fputc('\n', stderr);
                    if (word_count > 18048u) {
                        const size_t room_begin = 9240u;
                        const size_t room_end = std::min(word_count, size_t(9360));
                        std::fprintf(stderr,
                                     "[clt-bridge] room-floor[%u] offset=%zu "
                                     "guest=%#x:",
                                     trace_index, room_begin,
                                     guest_buffer + static_cast<uint32_t>(
                                                         room_begin * 4u));
                        for (size_t i = room_begin; i < room_end; ++i)
                            std::fprintf(stderr, " %08x", submission.words[i]);
                        if (room_end < word_count) std::fprintf(stderr, " ...");
                        std::fputc('\n', stderr);
                        const size_t wall_begin = 12920u;
                        const size_t wall_end = std::min(word_count, size_t(13200));
                        std::fprintf(stderr,
                                     "[clt-bridge] room-walls[%u] offset=%zu "
                                     "guest=%#x:",
                                     trace_index, wall_begin,
                                     guest_buffer + static_cast<uint32_t>(
                                                         wall_begin * 4u));
                        for (size_t i = wall_begin; i < wall_end; ++i)
                            std::fprintf(stderr, " %08x", submission.words[i]);
                        if (wall_end < word_count) std::fprintf(stderr, " ...");
                        std::fputc('\n', stderr);
                    }
                }
            }
        }
    }

    submission.sequence = ++m_submission_sequence;
    m_queue.push_back(std::move(submission));
    m_queued_bytes += byte_count;
    ++m_stats.submitted;
    m_stats.peak_queued_lists =
        std::max<uint64_t>(m_stats.peak_queued_lists, m_queue.size());
    m_stats.peak_queued_bytes =
        std::max<uint64_t>(m_stats.peak_queued_bytes, m_queued_bytes);
    perf::add_counter(perf::Counter::SnapshotObjectsCreated);
}

void GuestCltBridge::update_graphics_state(
    uint32_t io_item, uint32_t command, uint32_t item, uint32_t view_item,
    uint32_t framebuffer_item, uint32_t zbuffer_item, int32_t view_left,
    int32_t view_top, int32_t view_width, int32_t view_height,
    int32_t display_width, int32_t display_height)
{
    (void)io_item;
    (void)command;
    (void)item;
    perf::TimedMutexLock lock(m_mutex);
    m_graphics_state.view_item = view_item;
    m_graphics_state.framebuffer_item = framebuffer_item;
    m_graphics_state.zbuffer_item = zbuffer_item;
    m_graphics_state.view_left = view_left;
    m_graphics_state.view_top = view_top;
    m_graphics_state.view_width = view_width;
    m_graphics_state.view_height = view_height;
    m_graphics_state.display_width = display_width;
    m_graphics_state.display_height = display_height;
    m_graphics_state.viewport_valid = view_item != 0 &&
                                      framebuffer_item != 0 &&
                                      view_width > 0 && view_height > 0 &&
                                      display_width > 0 && display_height > 0;
}

void GuestCltBridge::update_te_surface_state(
    uint32_t db_super_control, uint32_t db_fb_clip,
    uint32_t db_dest_control, uint32_t db_dest_base,
    uint32_t db_dest_x_stride, uint32_t db_z_base, uint32_t db_z_clip,
    uint32_t z_fill_word, uint32_t fb_clear_red_word,
    uint32_t fb_clear_green_word, uint32_t fb_clear_blue_word,
    uint32_t fb_clear_alpha_word)
{
    perf::TimedMutexLock lock(m_mutex);
    m_graphics_state.te_db_super_control = db_super_control;
    m_graphics_state.te_db_fb_clip = db_fb_clip;
    m_graphics_state.te_db_dest_control = db_dest_control;
    m_graphics_state.te_db_dest_base = db_dest_base;
    m_graphics_state.te_db_dest_x_stride = db_dest_x_stride;
    m_graphics_state.te_db_z_base = db_z_base;
    m_graphics_state.te_db_z_clip = db_z_clip;
    m_graphics_state.te_z_fill_word = z_fill_word;
    m_graphics_state.te_fb_clear_red_word = fb_clear_red_word;
    m_graphics_state.te_fb_clear_green_word = fb_clear_green_word;
    m_graphics_state.te_fb_clear_blue_word = fb_clear_blue_word;
    m_graphics_state.te_fb_clear_alpha_word = fb_clear_alpha_word;
    ++m_graphics_state.te_surface_generation;
}

// Upper bound on recorded bitmap display spans. One span is held per distinct
// guest item, and the recovered item table is GK_MAX_ITEMS = 256 slots with an
// 8-bit slot index in the handle layout
// (runtime/src/recomp_runtime.c:571), so this covers every distinct Bitmap
// object the guest can own. It is a memory bound, not a display-plane budget:
// the display chain is resolved from the recorded set at pump time.
static constexpr size_t kMaxFbSpans = 256u;

// The display carve chain: the framebuffer item's own span, then every
// same-size span exactly one guard past an already-linked plane. Dsply
// allocates the two color buffers and the transition scratch as one run with a
// 2048-byte guard after every 576x432 plane, so looking only one hop from the
// seed omitted the old color buffer whenever the scratch plane was bound and
// the transition seed then sampled its stale guest-RAM backing instead of the
// renderer-owned image. Membership only makes a draw a *candidate*;
// load_db_source_texture still decides by the resolved origin address.
//
// Shared by the pump, which hands the chain to the interpreter, and
// register_bitmap_buffer(), whose overflow victim has to be exactly a span this
// function would not select. One definition, so the two cannot disagree about
// which planes are required.
static void collect_display_span_chain(const std::vector<CltFbSpan>& spans,
                                       uint32_t framebuffer_item,
                                       std::vector<CltFbSpan>* out)
{
    if (out == nullptr) return;
    out->clear();
    for (const CltFbSpan& span : spans) {
        if (span.item == framebuffer_item) {
            out->push_back(span);
            break;
        }
    }
    if (out->empty()) return;
    bool added = true;
    while (added) {
        added = false;
        for (const CltFbSpan& candidate : spans) {
            const bool already =
                std::any_of(out->begin(), out->end(),
                            [&](const CltFbSpan& chosen) {
                                return chosen.item == candidate.item;
                            });
            if (already) continue;
            for (const CltFbSpan& chosen : *out) {
                if (candidate.size != chosen.size) continue;
                const uint64_t spacing =
                    candidate.ea > chosen.ea
                        ? static_cast<uint64_t>(candidate.ea) - chosen.ea
                        : static_cast<uint64_t>(chosen.ea) - candidate.ea;
                if (spacing ==
                    static_cast<uint64_t>(chosen.size) + 2048u) {
                    out->push_back(candidate);
                    added = true;
                    break;
                }
            }
            if (added) break;
        }
    }
}

void GuestCltBridge::register_bitmap_buffer(uint32_t item, uint32_t buffer,
                                            uint32_t size)
{
    perf::TimedMutexLock lock(m_mutex);
    const auto trace_registration = [item, buffer, size]() {
        if (!bridge_trace_enabled()) return;
        static std::atomic<uint32_t> span_diag{0};
        if (span_diag.fetch_add(1) < 16u) {
            std::fprintf(stderr,
                         "[dbsrc-fb] register span item=%#x ea=%#x size=%#x "
                         "(%u)\n",
                         item, buffer, size, size);
        }
    };
    for (auto it = m_fb_spans.begin(); it != m_fb_spans.end(); ++it) {
        if (it->item != item) continue;
        // A span is the guest object's *current* backing allocation, not the
        // first one it ever published. ModifyGraphicsItemVA rebinds a live
        // Bitmap to a new buffer, and the DB-source classification, framebuffer
        // snapshot, and address-adjacent chain walk all read the span as the
        // item's present target, so a second registration has to replace the
        // recorded one. Keeping the first left the interpreter addressing a
        // released renderer target while the guest object named another.
        if (buffer != 0 && size != 0) {
            *it = {item, buffer, size};
            trace_registration();
            return;
        }
        // A zero buffer or size is the guest releasing the allocation. Drop
        // the span so the item stops naming it at all; the interpreter then
        // falls back to its ordinary guest-memory read for that address
        // instead of sampling a target the guest has let go of.
        m_fb_spans.erase(it);
        if (bridge_trace_enabled()) {
            static std::atomic<uint32_t> unreg_diag{0};
            if (unreg_diag.fetch_add(1) < 16u) {
                std::fprintf(stderr,
                             "[dbsrc-fb] unregister span item=%#x ea=%#x "
                             "size=%#x\n",
                             item, buffer, size);
            }
        }
        return;
    }
    if (buffer == 0 || size == 0) return;  // nothing published to remove
    if (m_fb_spans.size() >= kMaxFbSpans) {
        // The runtime publishes *every* nonzero Bitmap buffer, not only the
        // three display-chain planes the observed showroom routes happen to
        // register, and it does so in the guest's own allocation order. The
        // previous `size >= 8` return dropped the ninth registration outright,
        // so eight unrelated bitmaps published before the display allocation
        // left the interpreter with no framebuffer span: its DB-source read
        // then fell back to guest RAM or an unrelated target with no graphics
        // reject anywhere.
        //
        // The bound is the recovered item pool instead. Item handles expose an
        // 8-bit slot index and the item table is GK_MAX_ITEMS = 256 slots
        // (runtime/src/recomp_runtime.c:571), so 256 recorded spans already
        // covers every distinct Bitmap object the guest can own. Past that the
        // release victim is chosen to be a span the current display chain
        // cannot use: the chain is the framebuffer item's span plus its
        // same-size, one-guard address neighbours, exactly the set
        // collect_display_span_chain() links, so an unrelated span is dead
        // weight by construction. Every release is counted and logged; nothing
        // is dropped silently.
        //
        // "Current chain" is the chain the pump would select from
        // m_graphics_state.framebuffer_item right now, read under this same
        // lock. That covers both guest orders: a chain registered before the
        // TE surface names its framebuffer item is protected from the moment
        // the item is named, and a chain registered afterwards is the newest
        // entry, so the oldest span released is an older unrelated bitmap. A
        // span that no current chain selects is not a display plane the guest
        // is presenting, which is the whole sense in which releasing it is
        // safe.
        std::vector<CltFbSpan> chain;
        collect_display_span_chain(m_fb_spans,
                                   m_graphics_state.framebuffer_item, &chain);
        size_t victim = static_cast<size_t>(-1);
        for (size_t i = 0; i < m_fb_spans.size(); ++i) {
            const bool required = std::any_of(
                chain.begin(), chain.end(), [&](const CltFbSpan& kept) {
                    return kept.item == m_fb_spans[i].item;
                });
            if (required) continue;
            victim = i;
            break;
        }
        // No span outside the chain exists only when the chain is longer than
        // the table, which the pool bound rules out. The capacity decision
        // still has to be explicit rather than a silent drop, so the oldest
        // entry goes and the counter records it.
        if (victim == static_cast<size_t>(-1)) victim = 0;
        const CltFbSpan released = m_fb_spans[victim];
        m_fb_spans.erase(m_fb_spans.begin() +
                         static_cast<std::ptrdiff_t>(victim));
        ++m_stats.span_evictions;
        {
            static std::atomic<uint32_t> evict_diag{0};
            if (evict_diag.fetch_add(1) < 16u) {
                std::fprintf(stderr,
                             "[dbsrc-fb] span table full (%zu): released "
                             "item=%#x ea=%#x size=%#x for item=%#x "
                             "(evictions=%llu)\n",
                             kMaxFbSpans, released.item, released.ea,
                             released.size, item,
                             (unsigned long long)m_stats.span_evictions);
            }
        }
    }
    m_fb_spans.push_back({item, buffer, size});
    trace_registration();
}

void GuestCltBridge::retain_presented_frame()
{
    m_interpreter.retain_presented_frame();
}

void GuestCltBridge::dump_z_census()
{
    m_interpreter.dump_z_census();
}

void GuestCltBridge::host_frame_begin(uint32_t host_frame,
                                      bool clears_target)
{
    m_host_frame_current = host_frame;
    if (!clt_frame_trace_enabled()) return;
    m_frame_trace_active = true;
    m_frame_trace_host = host_frame;
    // In persistent-target mode frame_begin still runs, but only the first
    // call clears the backend surface. Keep the correlation output honest so
    // the falsification run cannot be mistaken for the normal clear cadence.
    m_frame_trace_clears =
        clears_target &&
        (!(m_target_persistence || clt_persist_target_enabled()) ||
         host_frame == 0);
    m_frame_trace_first_list = 0;
    m_frame_trace_last_list = 0;
    m_frame_trace_lists = 0;
    m_frame_trace_triangles = 0;
    m_frame_trace_substantial = 0;
}

void GuestCltBridge::host_frame_present(bool presented)
{
    if (!m_frame_trace_active) return;
    std::fprintf(
        stderr,
        "[clt-frame] host=%u first_list=%llu last_list=%llu lists=%u "
        "triangles=%llu substantial=%llu clear=%d present=%d\n",
        m_frame_trace_host, (unsigned long long)m_frame_trace_first_list,
        (unsigned long long)m_frame_trace_last_list, m_frame_trace_lists,
        (unsigned long long)m_frame_trace_triangles,
        (unsigned long long)m_frame_trace_substantial,
        m_frame_trace_clears ? 1 : 0, presented ? 1 : 0);
    m_frame_trace_active = false;
}

bool GuestCltBridge::has_pending_upto(uint32_t max_frame) const
{
    perf::TimedMutexLock lock(m_mutex);
    for (const Submission& submission : m_queue) {
        if (submission.guest_frame <= max_frame) return true;
    }
    return false;
}

bool GuestCltBridge::has_display_target_upto(uint32_t max_frame,
                                            uint32_t* out_frame) const
{
    perf::TimedMutexLock lock(m_mutex);
    if (out_frame) *out_frame = 0u;
    if (m_display_selections.empty() ||
        m_display_selections.front().guest_frame > max_frame)
        return false;
    if (out_frame) *out_frame = m_display_selections.front().guest_frame;
    return true;
}

void GuestCltBridge::pump_display_target_upto(uint32_t max_frame)
{
    uint32_t published_frame = 0;
    if (has_display_target_upto(max_frame, &published_frame))
        pump(published_frame, "display");
}

void GuestCltBridge::publish_display_target(uint32_t guest_frame,
                                            uint32_t target)
{
    if (target == 0) return;
    perf::TimedMutexLock lock(m_mutex);
    if (clt_transaction_trace_enabled() &&
        m_host_frame_current >= clt_transaction_trace_from()) {
        static std::atomic<uint32_t> display_logged{0};
        if (display_logged.fetch_add(1) < 4000u) {
            std::fprintf(stderr,
                         "[xact-display] host=%u frame=%u target=%#x\n",
                         m_host_frame_current, guest_frame, target);
        }
    }
    // Every DISPLAYFRAMEBUFFER is one display unit, and a single guest
    // application frame may publish a whole sequence of them. The authored
    // state transitions are exactly that: `dotrans` runs an effect body whose
    // frame loop submits one `GS_BeginFrame`/`GS_SetDestBuffer`/`GS_SendList`
    // /`GS_EndFrame` cycle per frame (30 frames for five of the seven effects,
    // 48 and 16 for the two square effects), all from inside one `StateDriver`
    // iteration, and `GS_EndFrame` posts the DISPLAYFRAMEBUFFER that reaches
    // here. Folding same-tag choices into one entry collapses the entire
    // authored animation to its last frame, so each one is kept in order. The
    // guest frame tag stays for the trace and for the completed-frame gate.
    m_display_selections.push_back(
        {guest_frame, target, m_submission_sequence});
}

uint32_t GuestCltBridge::consume_display_target_upto(uint32_t max_frame,
                                                     uint32_t* out_frame)
{
    perf::TimedMutexLock lock(m_mutex);
    if (out_frame != nullptr) *out_frame = 0;
    if (m_display_selections.empty() ||
        m_display_selections.front().guest_frame > max_frame) {
        return 0;
    }
    // One display unit per presentation, oldest first: the queue is the
    // device's pending display sequence, not a single "current choice". On
    // original hardware the TE drains one display command per video frame, so
    // a 30-frame transition is visible over 30 frames. Returning the last
    // eligible choice instead would present the animation's final frame only.
    const DisplaySelection next = m_display_selections.front();
    m_display_selections.pop_front();
    if (out_frame != nullptr) *out_frame = next.guest_frame;
    return next.target;
}

bool GuestCltBridge::inflight_lookahead_allowed(bool interactive_pacing,
                                               bool video_overlay,
                                               uint32_t completed,
                                               uint32_t current) const
{
    if (!interactive_pacing || current <= completed) return false;
    // General look-ahead service writes a partially built frame into the target
    // presented in the same iteration. Only the video-overlay lifecycle, whose
    // owning application frame never completes, requires it.
    if (m_inflight_overlay_only && !video_overlay) return false;
    return has_pending_upto(current);
}

void GuestCltBridge::pump(uint32_t max_frame, const char* phase)
{
    m_pump_phase = phase ? phase : "all";
    static const bool xact_trace_enabled = clt_transaction_trace_enabled();
    static const bool pump_timing_enabled = [] {
        return std::getenv("M2_FRAME_TIMING") != nullptr;
    }();
    const uint64_t pump_entry_start =
        pump_timing_enabled ? clt_now_ns() : 0;
    // Track B1 census window: one [livery-frame] summary per host frame.
    livery_tally_begin_frame(m_host_frame_current);
    // Host-keyed interpreter diagnostics (M2_DRAW_PROV_FRAME) read this.
    m_interpreter.set_host_frame(m_host_frame_current);
    std::deque<Submission> pending;
    size_t pump_skipped = 0;
    const uint64_t pump_lock_start =
        pump_timing_enabled ? clt_now_ns() : 0;
    uint64_t pump_lock_wait_ns = 0;
    {
        perf::TimedMutexLock lock(m_mutex);
        if (pump_timing_enabled) pump_lock_wait_ns = clt_now_ns() - pump_lock_start;
        // A bounded pump moves only submissions whose guest application frame
        // has completed, leaving a look-ahead frame's lists queued in order.
        // Untagged submissions (frame 0) are always eligible.
        // Effects publish several display units from one StateDriver frame.
        // The next unit's lists can arrive before the previous display fence
        // releases: a frame-tag bound alone would execute them early. Keep
        // every pump (including diagnostic drains) behind the oldest pending
        // display's submit-time boundary until that display is consumed.
        uint64_t published_sequence = m_display_selections.empty()
            ? UINT64_MAX : m_display_selections.front().submission_sequence;
        if (m_framebuffer_read && !m_framebuffer_read->done)
            published_sequence = std::min(published_sequence, m_framebuffer_read->sequence);
        while (!m_queue.empty() &&
               m_queue.front().sequence <= published_sequence &&
               (max_frame == 0xffffffffu ||
                m_queue.front().guest_frame <= max_frame)) {
            Submission& front = m_queue.front();
            m_queued_bytes -= front.words.size() * sizeof(uint32_t);
            pending.push_back(std::move(front));
            m_queue.pop_front();
        }
        if (max_frame != 0xffffffffu) {
            for (const Submission& queued : m_queue) {
                if (queued.guest_frame <= max_frame) break;
                ++pump_skipped;
            }
        }
        m_stats.peak_pump_batch =
            std::max<uint64_t>(m_stats.peak_pump_batch, pending.size());
        // The display carve spans are published by the runtime on the guest
        // thread; hand the latest set to the render thread before executing.
        // Only the framebuffer item's span and its guarded same-size
        // allocation chain are strip targets — the menu background bitmap is
        // also 497,664 bytes but must keep reading guest RAM. The chain also
        // reaches the Z/transition-scratch plane at 0x56a000, which doubles as
        // the MPEG movie bitmap: membership here only makes a draw a
        // *candidate*, and load_db_source_texture decides by the resolved
        // origin address (see the Configure Race overlay note there).
        //
        // A nonzero framebuffer item with no recorded span is the one case the
        // guest can reach without the span table being at fault: nothing has
        // published that item's buffer yet, so the interpreter's DB-source read
        // falls back to guest RAM. That fallback is correct for an ordinary
        // guest-RAM framebuffer, so it is reported rather than refused.
        std::vector<CltFbSpan> display_spans;
        const uint32_t fb_item = m_graphics_state.framebuffer_item;
        collect_display_span_chain(m_fb_spans, fb_item, &display_spans);
        if (display_spans.empty() && fb_item != 0u) {
            ++m_stats.fb_span_missing;
            static std::atomic<uint32_t> missing_span_diag{0};
            if (missing_span_diag.fetch_add(1) < 16u) {
                std::fprintf(stderr,
                             "[dbsrc-fb] framebuffer item=%#x has no recorded "
                             "span; DB-source reads fall back to guest RAM\n",
                             fb_item);
            }
        }
        // Smaller offscreen Bitmaps (including the rear-view target) are
        // independent of the guarded display allocation chain. The
        // interpreter additionally requires an observed destination write
        // before treating one as renderer-owned feedback.
        for (const CltFbSpan& span : m_fb_spans) {
            if (span.size != 576u * 432u * 2u &&
                std::none_of(display_spans.begin(), display_spans.end(),
                             [&span](const CltFbSpan& selected) {
                                 return selected.item == span.item;
                             }))
                display_spans.push_back(span);
        }
        m_interpreter.set_fb_spans(display_spans);
    }
    m_queue_cv.notify_all();

    // Bounded pump attribution (`M2_FRAME_TIMING`, same env as the host frame
    // cadence log): separates CLT interpretation from the rest of the pump so
    // a heavy menu frame is attributed instead of guessed.
    const uint64_t pump_timing_start =
        pump_timing_enabled
            ? static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count())
            : 0;
    uint64_t pump_execute_ns = 0;
    uint64_t pump_execute_max_ns = 0;
    uint64_t pump_post_ns = 0;
    uint32_t pump_words = 0;
    const uint64_t pump_renderer_before =
        pump_timing_enabled
            ? perf::phase_total_ns(perf::Phase::RendererWork)
            : 0;

    for (const Submission& submission : pending) {
        m_interpreter.set_te_surface_state(
            submission.graphics_state.te_db_super_control,
            submission.graphics_state.te_db_fb_clip,
            submission.graphics_state.te_db_dest_control,
            submission.graphics_state.te_db_dest_base,
            submission.graphics_state.te_db_dest_x_stride,
            submission.graphics_state.te_db_z_base,
            submission.graphics_state.te_db_z_clip,
            submission.graphics_state.te_z_fill_word,
            submission.graphics_state.te_fb_clear_red_word,
            submission.graphics_state.te_fb_clear_green_word,
            submission.graphics_state.te_fb_clear_blue_word,
            submission.graphics_state.te_fb_clear_alpha_word,
            submission.graphics_state.te_surface_generation);
        m_interpreter.set_viewport(
            submission.graphics_state.view_left,
            submission.graphics_state.view_top,
            submission.graphics_state.view_width,
            submission.graphics_state.view_height,
            submission.graphics_state.viewport_valid,
            submission.graphics_state.display_width,
            submission.graphics_state.display_height);
        if (submission.command == kTeExecuteListCommand &&
            (submission.command_options & kTeClearZBuffer) != 0u &&
            submission.graphics_state.te_db_z_base != 0u) {
            // The Portfolio TE driver runs its generated clear list before
            // the user's list. Keep this platform operation outside the CLT
            // word stream; an empty clear request must not become a fake
            // guest draw/list in provenance or frame counters.
            m_interpreter.clear_z_buffer();
        } else if (submission.command == kTeExecuteListCommand &&
                   (submission.command_options & kTeClearFrameBuffer) != 0u &&
                   submission.graphics_state.te_db_dest_base != 0u) {
            // The recovered TE driver selects the framebuffer clear only when
            // the Z clear branch did not run. Keep that source ordering
            // outside the guest CLT word stream as a platform operation.
            m_interpreter.clear_color_buffer();
        }
        if (submission.words.empty()) continue;

        CltExecutionResult result;
        {
            perf::Scope interpretation_scope(perf::Phase::CltInterpretation);
            uint64_t execute_start_ns = 0;
            if (pump_timing_enabled) {
                execute_start_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            }
            result = m_interpreter.execute(
                submission.words.data(), submission.words.size(),
                &submission.memory, submission.guest_buffer);
            if (pump_timing_enabled) {
                const uint64_t elapsed =
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()) -
                    execute_start_ns;
                pump_execute_ns += elapsed;
                pump_execute_max_ns = std::max(pump_execute_max_ns, elapsed);
            }
            pump_words += static_cast<uint32_t>(submission.words.size());
        }
        const uint64_t pump_post_start =
            pump_timing_enabled ? clt_now_ns() : 0;
        // Bounded livery publication diagnostic.  M_LoadPodTexture builds
        // the authentic MMDMA/PIP register snippets for the red showroom
        // texture; this scan answers whether those exact words reached a
        // submitted CLT list before the interpreter consumed it. The word
        // scan also feeds the per-frame census, so it runs when either the
        // list detail or the draw summary is enabled; per-list detail lines
        // stay under the list gate.
        const bool livery_scan =
            livery_list_diag_enabled() || livery_draw_diag_enabled();
        if (livery_scan) {
            LiveryListDiagState& diag = livery_list_diag_state();
            ++diag.submissions;
            bool mmdma_source = false;
            bool mmdma_count = false;
            bool pip_source = false;
            bool pip_dest = false;
            bool pip_load = false;
            size_t first_marker = submission.words.size();
            for (size_t i = 0; i + 1 < submission.words.size(); ++i) {
                const uint32_t reg = submission.words[i];
                const uint32_t value = submission.words[i + 1];
                const bool is_marker =
                    reg == 0x10006424u &&
                    (value == 0x0097299cu || value == 0x00972768u);
                if (!is_marker) continue;
                if (first_marker == submission.words.size()) first_marker = i;
                if (reg == 0x10006424u && value == 0x0097299cu)
                    mmdma_source = true;
                if (reg == 0x10006428u && value == 0x00003f00u)
                    mmdma_count = true;
                if (reg == 0x10006424u && value == 0x00972768u)
                    pip_source = true;
                if (reg == 0x10006414u && value == 0x00046000u)
                    pip_dest = true;
                if (reg == 0x10006404u && value == 0x00000200u)
                    pip_load = true;
            }
            if (first_marker != submission.words.size()) {
                ++diag.marker_lists;
                livery_tally_note_marker();
                if (livery_list_diag_enabled() &&
                    diag.emitted < livery_list_diag_max()) {
                    ++diag.emitted;
                    const size_t begin = first_marker > 8u
                                              ? first_marker - 8u
                                              : 0u;
                    const size_t end = std::min(
                        submission.words.size(), first_marker + 18u);
                    std::fprintf(
                        stderr,
                        "[livery-list] frame=%u exec=%llu io=%#x command=%#x "
                        "buf=%#x words=%zu marker=%zu "
                        "mmdma_src=%d mmdma_count=%d pip_src=%d pip_dest=%d "
                        "pip_load=%d result_ok=%d unsupported=%u\n",
                        m_host_frame_current,
                        (unsigned long long)result.execute_seq,
                        submission.io_item, submission.command,
                        submission.guest_buffer, submission.words.size(),
                        first_marker, mmdma_source ? 1 : 0,
                        mmdma_count ? 1 : 0, pip_source ? 1 : 0,
                        pip_dest ? 1 : 0, pip_load ? 1 : 0,
                        result.ok ? 1 : 0, result.unsupported);
                    std::fprintf(stderr, "[livery-list] words:");
                    for (size_t i = begin; i < end; ++i)
                        std::fprintf(stderr, " %08x", submission.words[i]);
                    if (end < submission.words.size())
                        std::fprintf(stderr, " ...");
                    std::fputc('\n', stderr);
                } else if (livery_list_diag_enabled() && !diag.cap_reported) {
                    diag.cap_reported = true;
                    std::fprintf(
                        stderr,
                        "[livery-list] cap reached at %u marker lists; "
                        "further entries suppressed\n",
                        livery_list_diag_max());
                }
            }
            if (livery_list_diag_enabled() && (diag.submissions % 256u) == 0u) {
                std::fprintf(stderr,
                             "[livery-list-scan] frame=%u submissions=%llu "
                             "marker_lists=%llu emitted=%u\n",
                             m_host_frame_current,
                             (unsigned long long)diag.submissions,
                             (unsigned long long)diag.marker_lists,
                             diag.emitted);
            }
        }
        if (clt_ring_audit_enabled() && submission.io_item == 0u &&
            submission.command == kTeExecuteListCommand) {
            const uint32_t consumer_sequence = ++m_ring_audit_consumed;
            const uint32_t audit_from = clt_ring_audit_from();
            if (m_host_frame_current >= audit_from) {
                if (m_ring_audit_logged < clt_ring_audit_max()) {
                    ++m_ring_audit_logged;
                    std::fprintf(
                        stderr,
                        "[te-ring-consumer] seq=%u host=%u exec=%llu "
                        "addr=%#x len=%zu checksum=%08x\n",
                        consumer_sequence, m_host_frame_current,
                        (unsigned long long)result.execute_seq,
                        submission.guest_buffer,
                        submission.words.size() * sizeof(uint32_t),
                        submission.guest_checksum);
                } else if (!m_ring_audit_cap_reported) {
                    m_ring_audit_cap_reported = true;
                    std::fprintf(
                        stderr,
                        "[te-ring-consumer] cap reached at %u entries; "
                        "further entries suppressed\n",
                        clt_ring_audit_max());
                }
            }
        }
        if (xact_trace_enabled &&
            m_host_frame_current >= clt_transaction_trace_from()) {
            static std::atomic<uint32_t> xact_logged{0};
            if (xact_logged.fetch_add(1) < 4000u) {
                const GuestGraphicsState& gs = submission.graphics_state;
                const bool clear_z =
                    submission.command == kTeExecuteListCommand &&
                    (submission.command_options & kTeClearZBuffer) != 0u &&
                    gs.te_db_z_base != 0u;
                const bool clear_fb =
                    submission.command == kTeExecuteListCommand &&
                    (submission.command_options & kTeClearFrameBuffer) != 0u &&
                    gs.te_db_dest_base != 0u;
                // bound_e/cur_e are sampled here, at execution time. The
                // presenter iteration's boundary is not a substitute: a
                // diagnostic drain runs the guest, so the completed counter
                // can advance between the pump and the presenter's own trace
                // line, which made "above the completed boundary" ambiguous.
                std::fprintf(
                    stderr,
                    // dbsrc/dbstride/dbuser are the register state the
                    // interpreter held when this list's draws ran. A 2D
                    // background copy is only attempted when the DB source is
                    // armed (nonzero base, nonzero stride, DBUSERCONTROL SRC
                    // enabled), so a list that renders as a flat primitive
                    // colour instead of a DB-source blit is visible here: the
                    // destination address alone cannot tell the two apart.
                    "[xact-clt] host=%u phase=%s seq=%llu frame=%u cmd=%#x "
                    "dest=%#x z=%#x dest_w=%u dest_wv=%#x dbsrc=%#x "
                    "dbstride=%u dbuser=%#x clear_z=%d "
                    "clear_fb=%d words=%zu tri=%llu ok=%d bound_e=%u "
                    "cur_e=%u\n",
                    m_host_frame_current, m_pump_phase,
                    (unsigned long long)result.execute_seq,
                    submission.guest_frame, submission.command,
                    gs.te_db_dest_base, gs.te_db_z_base,
                    result.dest_base_writes, result.dest_base_value,
                    m_interpreter.register_value(kDbSrcBaseAddr),
                    m_interpreter.register_value(kDbSrcXStride) &
                        kDbSrcXStrideMask,
                    m_interpreter.register_value(kDbUserControl),
                    clear_z ? 1 : 0, clear_fb ? 1 : 0,
                    submission.words.size(),
                    (unsigned long long)result.triangles, result.ok ? 1 : 0,
                    m2_guest_frame_pacing_completed(),
                    m2_guest_current_frame_c());
            }
        }
        if (m_frame_trace_active) {
            if (m_frame_trace_lists == 0)
                m_frame_trace_first_list = result.execute_seq;
            m_frame_trace_last_list = result.execute_seq;
            ++m_frame_trace_lists;
            m_frame_trace_triangles += result.triangles;
            m_frame_trace_substantial += result.substantial_triangles;
        }
        if (!result.ok && bridge_trace_enabled()) {
            static std::atomic<uint32_t> malformed_count{0};
            const uint32_t malformed_index = malformed_count.fetch_add(1);
            if (malformed_index < 8u) {
                const size_t preview_words = std::min<size_t>(
                    submission.words.size(), bridge_trace_full() ? 256u : 32u);
                std::fprintf(stderr,
                             "[clt-bridge] malformed[%u] io=%#x command=%#x "
                             "buf=%#x consumed=%zu words=%zu:",
                             malformed_index, submission.io_item,
                             submission.command, submission.guest_buffer,
                             result.words_consumed, submission.words.size());
                for (size_t i = 0; i < preview_words; ++i)
                    std::fprintf(stderr, " %08x", submission.words[i]);
                if (preview_words < submission.words.size())
                    std::fprintf(stderr, " ...");
                std::fputc('\n', stderr);
            }
        }
        if (result.unsupported != 0 && bridge_trace_enabled()) {
            static std::atomic<uint32_t> reject_count{0};
            const uint32_t reject_index = reject_count.fetch_add(1);
            if (reject_index < 8u) {
                const size_t preview_words = std::min<size_t>(
                    submission.words.size(), bridge_trace_full() ? 256u : 32u);
                std::fprintf(stderr,
                             "[clt-bridge] rejected[%u] io=%#x command=%#x "
                             "buf=%#x unsupported=%u words=%zu:",
                             reject_index, submission.io_item,
                             submission.command, submission.guest_buffer,
                             result.unsupported, submission.words.size());
                for (size_t i = 0; i < preview_words; ++i)
                    std::fprintf(stderr, " %08x", submission.words[i]);
                if (preview_words < submission.words.size())
                    std::fprintf(stderr, " ...");
                std::fputc('\n', stderr);
            }
        }
        perf::TimedMutexLock lock(m_mutex);
        // "!ok -> malformed" is the whole-list rejection signal, so it must
        // only be cleared for the terminal rejection sites. Every ok=false
        // return in execute() is a truncated or undecodable packet; a
        // recognized-but-unimplemented packet (point packets, DINT) leaves ok
        // set and is counted only by the unsupported total below. Do not
        // narrow this to "!ok && unsupported == 0": unsupported is cumulative
        // over the list, so a list that first hits a per-batch unsupported
        // site and later a truncated header would lose its malformed count.
        if (!result.ok) ++m_stats.malformed;
        m_stats.unsupported += result.unsupported;
        m_stats.draws += result.draws;
        m_stats.control_flow_jumps += result.control_flow_jumps;
        m_stats.control_flow_pauses += result.control_flow_pauses;
        m_stats.control_flow_external_handoffs +=
            result.control_flow_external_handoffs;
        m_stats.control_flow_refusals += result.control_flow_refusals;
        m_stats.control_flow_loops += result.control_flow_loops;

        // Draw diagnostics (M2_DRAW_DIAG=1, default off): per-list
        // verdict — size class, vertex-batch shape, draw and reject counts —
        // so the world/car/HUD lists can be told apart in the submit stream.
        {
            static const bool draw_diag = [] {
                const char* v = std::getenv("M2_DRAW_DIAG");
                return v && v[0] != '0';
            }();
            static const uint32_t draw_diag_from = [] {
                const char* v = std::getenv("M2_DRAW_DIAG_FROM");
                return v ? static_cast<uint32_t>(strtoul(v, nullptr, 0)) : 0u;
            }();
            static uint32_t diag_lists = 0;
            static bool diag_expired = false;
            if (draw_diag && diag_lists >= 120u && !diag_expired) {
                diag_expired = true;
                std::fprintf(stderr,
                             "[clt-list] cap reached at submit n=%u — further "
                             "entries suppressed\n",
                             diag_lists);
            }
            // Frame proxy: GuestFramesStarted counts completed guest frames
            // (one per VBL). Floor the sample window on it so the 120-list
            // cap cannot expire before the race era begins.
            const uint64_t diag_frame =
                perf::counter_value(perf::Counter::GuestFramesStarted);
            const bool diag_emit = draw_diag &&
                                  diag_frame >= draw_diag_from &&
                                  diag_lists < 120u;
            if (diag_emit) {
                const uint32_t first = submission.words.empty()
                                           ? 0u
                                           : submission.words.front();
                const char* kind = "small";
                const size_t bytes = submission.words.size() * 4u;
                if (bytes >= 4096u)
                    kind = "LARGE";
                else if (bytes >= 256u)
                    kind = "mid";
                std::fprintf(stderr,
                             "[clt-list] n=%u frame=%llu exec=%llu bytes=%zu kind=%s hdr=%08x "
                             "vtx_batches=%u draws=%u unsupported=%u ok=%d\n",
                             diag_lists++, (unsigned long long)diag_frame,
                             (unsigned long long)result.execute_seq, bytes,
                             kind, first,
                             result.vertex_batches, result.draws,
                             result.unsupported, result.ok ? 1 : 0);
            }
        }
        if (pump_timing_enabled)
            pump_post_ns += clt_now_ns() - pump_post_start;
    }
    perf::add_counter(perf::Counter::SnapshotObjectsDestroyed, pending.size());
    if (pump_timing_enabled &&
        (!pending.empty() || max_frame != 0xffffffffu)) {
        // Bounded at 12000 lines (~2.4 MB): the 2000 cap was exhausted by the
        // menu phase, so a race-length M2_FRAME_TIMING run could not attribute
        // the heavy post-State-3 pumps that drive the field slips.
        static uint32_t pump_timing_lines = 0;
        if (pump_timing_lines < 12000u) {
        ++pump_timing_lines;
        const uint64_t total_ns =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()) -
            pump_timing_start;
        std::fprintf(stderr,
                     "[pump-timing] host=%u lists=%zu words=%u "
                     "execute_ms=%.2f execute_max_ms=%.2f render_ms=%.2f "
                     "mmdma_hits=%llu mmdma_misses=%llu "
                     "post_ms=%.2f lock_ms=%.2f "
                     "pre_ms=%.2f skipped=%zu total_ms=%.2f\n",
                     m_host_frame_current, pending.size(), pump_words,
                     pump_execute_ns / 1e6, pump_execute_max_ns / 1e6,
                     (perf::phase_total_ns(perf::Phase::RendererWork) -
                      pump_renderer_before) /
                         1e6,
                     (unsigned long long)g_mmdma_cache_hits,
                     (unsigned long long)g_mmdma_cache_misses,
                     pump_post_ns / 1e6, pump_lock_wait_ns / 1e6,
                     pump_timing_start > pump_entry_start
                         ? static_cast<double>(pump_timing_start -
                                               pump_entry_start) /
                               1e6
                         : 0.0,
                     pump_skipped,
                     total_ns / 1e6);
        }
    }
}

void GuestCltBridge::detach()
{
    // A guest callback holds active_bridge_mutex() across enqueue(). Wake a
    // producer before taking that lock, otherwise a full queue could make
    // shutdown wait forever for the callback that shutdown must release.
    m_accepting.store(false, std::memory_order_release);
    m_queue_cv.notify_all();
    {
        perf::TimedMutexLock active_lock(active_bridge_mutex());
        if (active_bridge() == this) active_bridge() = nullptr;
    }
    perf::TimedMutexLock lock(m_mutex);
    m_attached = false;
    m_queue_cv.notify_all();
}

void GuestCltBridge::release_resources()
{
    m_interpreter.release_resources();
    // Shutdown follows detach(), so no guest callback can append here. Release
    // queued snapshots before the profiler report is finalized; this makes the
    // logical created/destroyed counters account for the complete run without
    // changing the live bounded queue or frame ordering.
    perf::TimedMutexLock lock(m_mutex);
    if (m_attached) return;
    perf::add_counter(perf::Counter::SnapshotObjectsDestroyed,
                      m_queue.size());
    m_queue.clear();
    m_queued_bytes = 0;
}

GuestCltStats GuestCltBridge::stats() const
{
    perf::TimedMutexLock lock(m_mutex);
    return m_stats;
}

std::string GuestCltBridge::diagnostic_snapshot() const
{
    return m_interpreter.diagnostic_snapshot();
}

std::string GuestCltInterpreter::diagnostic_snapshot() const
{
    std::ostringstream out;
    out << "{\"registers\":[";
    bool first = true;
    for (size_t i = 0; i < m_registers.size(); ++i) {
        if (!m_registers[i]) continue;
        if (!first) out << ',';
        first = false;
        out << '[' << i * 4 << ',' << m_registers[i] << ']';
    }
    out << "],\"textures\":[";
    first = true;
    const auto texture = [&](const Texture& t, const char* role) {
        if (!first) out << ',';
        first = false;
        out << "{\"role\":\"" << role << "\",\"tram_base\":" << t.tram_base
            << ",\"tram_size\":" << t.tram_size << ",\"width\":" << t.width
            << ",\"height\":" << t.height << ",\"source_address\":" << t.source_address
            << ",\"source_span\":" << t.source_span
            << ",\"source_checksum\":" << t.source_checksum
            << ",\"decoded_checksum_valid\":" << (t.decoded_checksum_valid ? "true" : "false")
            << ",\"decoded_checksum\":" << t.decoded_checksum << '}';
    };
    texture(m_texture, "current");
    for (const auto& entry : m_tram_cache) texture(entry, "cached");
    texture(m_db_source, "db_source");
    out << "],\"pending_texture_bytes\":" << m_pending_mmdma_bytes.size() << '}';
    return out.str();
}

GuestGraphicsState GuestCltBridge::graphics_state_for_testing() const
{
    perf::TimedMutexLock lock(m_mutex);
    return m_graphics_state;
}

namespace clt_tex_expand_probe {

// Contract-test surface only: production sites call replicate_msb() directly.
// The differential matrix in m2_guest_clt_test needs the same function the
// production paths use, so that a path-specific second rule cannot reappear
// unnoticed, and so the documented rule can be compared against the toolkit
// utility's LSB fill across the full depth/value domain.
uint32_t widen(unsigned depth, uint32_t value)
{
    return replicate_msb(depth, value);
}

}  // namespace clt_tex_expand_probe

namespace clt_uv_address_probe {

// Contract-test surface only: production draws call texture_sampling_state()
// directly on the interpreter's draw paths.  The focused address oracle needs
// the same decode, so that a second address rule cannot reappear unnoticed.
// The reserved-selector verdict travels with the address state because a
// refused draw still decodes its periods, and a test must be able to tell a
// refused register from a decoded one.
CltTextureSamplingDecode decode(uint32_t addr_cntl, uint32_t uv_mask,
                                float texture_width, float texture_height)
{
    CltTextureSamplingDecode decoded;
    decoded.filter_reserved =
        !texture_sampling_state(addr_cntl, uv_mask, texture_width,
                                texture_height, &decoded.sampling);
    return decoded;
}

}  // namespace clt_uv_address_probe

namespace clt_z_cell_probe {

uint32_t dump_word_count()
{
    static const uint32_t words = [] {
        const char* value = std::getenv("M2_CLT_Z_CELL_DUMP");
        if (!value || value[0] == '\0') return 0u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed == 0) return value[0] == '0' ? 0u : 16u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 256ul));
    }();
    return words;
}

uint32_t cell_offset_bytes(uint32_t x, uint32_t y, uint32_t stride_words)
{
    return 2u * (x + y * stride_words);
}

uint32_t cell_address(uint32_t z_base, uint32_t x, uint32_t y,
                      uint32_t stride_words)
{
    return z_base + cell_offset_bytes(x, y, stride_words);
}

size_t dump_span_bytes(uint32_t word_count, uint32_t stride_words)
{
    if (word_count == 0) return 0;
    const uint32_t width = window_width(stride_words);
    const uint32_t last = word_count - 1u;
    uint32_t max_cell;
    if (last < width) {
        max_cell = last;
    } else {
        max_cell = (width - 1u) + (last / width) * stride_words;
    }
    return static_cast<size_t>(max_cell) * 2u + 2u;
}

uint16_t decode_be(uint8_t hi, uint8_t lo)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8u) | lo);
}

uint16_t decode_le(uint8_t hi, uint8_t lo)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(hi) |
                                 (static_cast<uint16_t>(lo) << 8u));
}

void encode_be(uint16_t cell, uint8_t* hi, uint8_t* lo)
{
    if (hi) *hi = static_cast<uint8_t>((cell >> 8u) & 0xffu);
    if (lo) *lo = static_cast<uint8_t>(cell & 0xffu);
}

uint32_t window_width(uint32_t stride_words)
{
    if (stride_words == 0) return 1u;
    return std::min(stride_words, 8u);
}

void coords_for_index(uint32_t index, uint32_t stride_words, uint32_t* x,
                       uint32_t* y)
{
    const uint32_t width = window_width(stride_words);
    if (x) *x = index % width;
    if (y) *y = index / width;
}

int format_cell_line(char* out, size_t out_size, uint64_t list, uint32_t x,
                      uint32_t y, uint32_t address, uint8_t byte0,
                      uint8_t byte1)
{
    if (!out || out_size == 0) return -1;
    return std::snprintf(out, out_size,
                          "[clt-z-cell] list=%llu x=%u y=%u addr=%#x "
                          "raw=%02x.%02x be=%#x le=%#x",
                          static_cast<unsigned long long>(list), x, y, address,
                          byte0, byte1, decode_be(byte0, byte1),
                          decode_le(byte0, byte1));
}

int format_header_line(char* out, size_t out_size, uint64_t list,
                        uint32_t base, uint32_t stride_words,
                        uint32_t word_count)
{
    if (!out || out_size == 0) return -1;
    return std::snprintf(out, out_size,
                          "[clt-z-cell] list=%llu base=%#x stride=%u words=%u",
                          static_cast<unsigned long long>(list), base,
                          stride_words, word_count);
}

int format_unmapped_line(char* out, size_t out_size, uint64_t list,
                          uint32_t base, uint32_t stride_words,
                          uint32_t word_count)
{
    if (!out || out_size == 0) return -1;
    return std::snprintf(out, out_size,
                          "[clt-z-cell] list=%llu base=%#x stride=%u words=%u "
                          "unmapped",
                          static_cast<unsigned long long>(list), base,
                          stride_words, word_count);
}

}  // namespace clt_z_cell_probe

// --- M2_CLT_DEPTH_W16_PACKED probe definitions (diagnostic-only) ------------
// Quantization/tie rule narrowed from the patent candidate encoding
// (range << 14) | floor((one_over_w * 2^(3 * range)) * 2^14)
// (WO1996036011A1 block-fix 16-bit example; contracts "Raw 16-bit W-cell
// evidence"): truncation toward zero (floor of the non-negative scaled value)
// with saturation at 0x3FFF, so exact .5-step ties encode down, never
// round-to-nearest. Mirrors SoftCaptureBackend::m2_w16_packed /
// m2_w16_packed_decode; the production reciprocal-W path never calls these.
bool clt_w16_packed_probe::enabled()
{
    const char* value = std::getenv("M2_CLT_DEPTH_W16_PACKED");
    return value && value[0] != '\0' && value[0] != '0';
}

uint16_t clt_w16_packed_probe::encode(float one_over_w, unsigned range)
{
    if (!std::isfinite(one_over_w) || one_over_w <= 0.f || range > 3u)
        return 0u;
    const float scaled =
        std::ldexp(one_over_w, static_cast<int>(range * 3u + 14u));
    const float bounded = std::clamp(scaled, 0.f, 16383.f);
    const uint32_t significand =
        static_cast<uint32_t>(std::floor(bounded));
    return static_cast<uint16_t>((range << 14u) | significand);
}

float clt_w16_packed_probe::decode(uint16_t cell)
{
    const unsigned range = (static_cast<unsigned>(cell) >> 14u) & 3u;
    const unsigned significand = static_cast<unsigned>(cell) & 0x3fffu;
    if (significand == 0u) return 0.f;
    const float normalized = static_cast<float>(significand) / 16384.f;
    return std::ldexp(normalized, -static_cast<int>(range * 3u));
}

int clt_w16_packed_probe::compare(uint16_t a, uint16_t b)
{
    // Patent destination comparison uses both stored significand/range
    // fields; the four ranges deliberately reverse raw u16 order, so compare
    // decoded block-fix values. Value is s * 2^(-3r) / 2^14; scale by the
    // common denominator 2^23 to compare s * 2^(9 - 3r) exactly in 32 bits
    // (max 16383 * 512 = 8388096).
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

}  // namespace m2host

extern "C" void m2_guest_graphics_submit_c(
    uint32_t io_item, uint32_t command, uint32_t command_options,
    uint32_t guest_buffer, uint32_t byte_count,
    const uint8_t* guest_bytes,
    const uint8_t* guest_memory, uint32_t guest_memory_size)
{
    m2host::perf::TimedMutexLock active_lock(
        m2host::active_bridge_mutex());
    if (m2host::active_bridge()) {
        m2host::active_bridge()->enqueue(
            io_item, command, command_options, guest_buffer, byte_count,
            guest_bytes, guest_memory, guest_memory_size);
    }
}

extern "C" void m2_guest_graphics_state_c(
    uint32_t io_item, uint32_t command, uint32_t item,
    uint32_t view_item, uint32_t framebuffer_item, uint32_t zbuffer_item,
    int32_t view_left, int32_t view_top, int32_t view_width,
    int32_t view_height, int32_t display_width, int32_t display_height)
{
    m2host::perf::TimedMutexLock active_lock(
        m2host::active_bridge_mutex());
    if (m2host::active_bridge()) {
        m2host::active_bridge()->update_graphics_state(
            io_item, command, item, view_item, framebuffer_item, zbuffer_item,
            view_left, view_top, view_width, view_height, display_width,
            display_height);
    }
}

extern "C" void m2_guest_graphics_display_c(uint32_t guest_frame,
                                              uint32_t target)
{
    m2host::perf::TimedMutexLock active_lock(
        m2host::active_bridge_mutex());
    if (m2host::active_bridge())
        m2host::active_bridge()->publish_display_target(guest_frame, target);
}

extern "C" void m2_guest_graphics_te_surface_c(
    uint32_t db_super_control, uint32_t db_fb_clip,
    uint32_t db_dest_control, uint32_t db_dest_base,
    uint32_t db_dest_x_stride, uint32_t db_z_base, uint32_t db_z_clip,
    uint32_t z_fill_word, uint32_t fb_clear_red_word,
    uint32_t fb_clear_green_word, uint32_t fb_clear_blue_word,
    uint32_t fb_clear_alpha_word)
{
    m2host::perf::TimedMutexLock active_lock(
        m2host::active_bridge_mutex());
    if (m2host::active_bridge()) {
        m2host::active_bridge()->update_te_surface_state(
            db_super_control, db_fb_clip, db_dest_control, db_dest_base,
            db_dest_x_stride, db_z_base, db_z_clip, z_fill_word,
            fb_clear_red_word, fb_clear_green_word, fb_clear_blue_word,
            fb_clear_alpha_word);
    }
}

extern "C" void m2_guest_graphics_bitmap_buffer_c(
    uint32_t item, uint32_t buffer, uint32_t size)
{
    m2host::perf::TimedMutexLock active_lock(
        m2host::active_bridge_mutex());
    if (m2host::active_bridge()) {
        m2host::active_bridge()->register_bitmap_buffer(item, buffer, size);
    }
}

extern "C" int32_t m2_guest_graphics_read_framebuffer_c(
    uint32_t base, void* dst, uint32_t count)
{
    m2host::perf::TimedMutexLock active_lock(m2host::active_bridge_mutex());
    auto* bridge = m2host::active_bridge();
    return bridge ? bridge->read_framebuffer(base, dst, count) : -1;
}

// Weak zero fallback for the optional runtime frame accessor: standalone
// parser tests link this file without the recomp runtime and submit untagged.
extern "C" __attribute__((weak)) uint32_t m2_guest_current_frame_c(void)
{
    return 0;
}

// Weak fallback for the optional runtime pacing-boundary accessor. Unbounded
// matches the pump-everything contract of the standalone parser tests;
// only the transaction trace reads it.
extern "C" __attribute__((weak)) uint32_t m2_guest_frame_pacing_completed(void)
{
    return 0xffffffffu;
}
