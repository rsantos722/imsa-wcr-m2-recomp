// guest_clt.h — bounded bridge for guest-owned M2 Triangle Engine lists.
//
// The recompiled guest builds CLT words in guest RAM and submits them through
// the TE IO request.  This interface copies those words at the selector
// boundary, interprets the evidence-backed register and vertex forms, and
// leaves actual drawing to the active host backend.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <utility>
#include <vector>

#include "cel_blitter.h"
#include "te_reference_vertex_state.h"
#include "te_vertex_state.h"

namespace m2host {

// Guest memory needed by a copied command list. The bridge snapshots these
// ranges on the guest callback thread so the render thread never dereferences
// guest-owned storage.
struct CltMemoryRange {
    uint32_t guest_address = 0;
    std::vector<uint8_t> bytes;
    // FNV-1a over `bytes`, computed while the guest callback snapshots the
    // range. Zero means a hand-built test range has no precomputed hash and
    // the interpreter should retain its local fallback.
    uint64_t content_hash = 0;
};

struct CltMemorySnapshot {
    std::vector<CltMemoryRange> ranges;
};

// One registered bitmap buffer span (published through
// ModifyGraphicsItemVA). The State 2 strip transition blits from the
// double-buffered display carves (identified by the framebuffer item + the
// observed 497,664+2,048 spacing); the host renders to GL but never
// rasterizes into them, so the bridge serves the retained presented frame
// instead of the zero guest carve when the DB-source base lands inside one.
struct CltFbSpan {
    uint32_t item = 0;
    uint32_t ea = 0;
    uint32_t size = 0;
};

struct CltExecutionResult {
    bool ok = true;
    size_t words_consumed = 0;
    uint64_t execute_seq = 0;
    uint32_t register_writes = 0;
    uint32_t vertex_batches = 0;
    uint32_t triangles = 0;
    uint32_t substantial_triangles = 0;
    uint32_t draws = 0;
    uint32_t unsupported = 0;
    // DCNTL flow-control accounting. A terminal PSE is a source-defined stop,
    // while a jump consumes a target in the copied list. External terminal
    // jumps are recorded as handoffs; non-terminal external targets and DINT
    // are fail-closed unsupported requests.
    uint32_t control_flow_jumps = 0;
    uint32_t control_flow_pauses = 0;
    uint32_t control_flow_external_handoffs = 0;
    uint32_t control_flow_refusals = 0;
    // Guest-authored field loops: a jump whose target instruction this
    // execution has already run. Portfolio's display lists use it to keep the
    // current field on the TE while the guest builds the next one (the
    // showroom/screensaver iterate path ends its ~34k-word list with a JA back
    // to its first word), so the pass ends there instead of redrawing
    // identical pixels. This is an accepted source shape, not a refusal.
    uint32_t control_flow_loops = 0;
    // DBDESTBASEADDR writes the executed list performed itself, and the last
    // value it wrote. The destination bound for the list's draws is the
    // guest's own write when the list has one; otherwise it is the TE
    // framebuffer the guest last set. Recording both lets a trace tell an
    // authored destination apart from the display buffer it happened to
    // inherit, which the destination address alone cannot.
    uint32_t dest_base_writes = 0;
    uint32_t dest_base_value = 0;
};

// VideoPlayer changes the Projector View attachment before the host notices
// that a streamed overlay has started. Seed the guest TE siblings from the
// last completed TE scanout, not from the now-current View: that View may
// already be an MPEG bitmap. The presented-image fallback is only for startup
// paths that have no retained TE target yet.
bool seed_video_overlay_targets(ICelBlitterBackend* backend,
                                uint32_t completed_te_scanout,
                                uint32_t overlay_view_base,
                                uint32_t overlay_draw_base,
                                uint32_t* scratch_rgba, int width, int height);

// Scalar state published by the guest TE setup path. The host keeps no guest
// pointers; a command list snapshots the state that was current when it was
// submitted.
struct GuestGraphicsState {
    uint32_t view_item = 0;
    uint32_t framebuffer_item = 0;
    uint32_t zbuffer_item = 0;
    int32_t view_left = 0;
    int32_t view_top = 0;
    int32_t view_width = 0;
    int32_t view_height = 0;
    int32_t display_width = 0;
    int32_t display_height = 0;
    bool viewport_valid = false;

    // Platform-authored Triangle Engine surface registers. InitTE starts the
    // destination blender with 16-bit framebuffer/Z writes enabled (0x6),
    // SetTEFrameBuffer sets DESTOUTEN and the framebuffer registers, and
    // SetTEZBuffer sets the Z base/clip. These are not ordinary CLT list
    // writes and therefore travel with the setup snapshot separately.
    uint32_t te_db_super_control = 0x6u;
    uint32_t te_db_fb_clip = 0u;
    uint32_t te_db_dest_control = 0u;
    uint32_t te_db_dest_base = 0u;
    uint32_t te_db_dest_x_stride = 0u;
    uint32_t te_db_z_base = 0u;
    uint32_t te_db_z_clip = 0u;
    uint32_t te_z_fill_word = 0u;
    uint32_t te_fb_clear_red_word = 0u;
    uint32_t te_fb_clear_green_word = 0u;
    uint32_t te_fb_clear_blue_word = 0u;
    uint32_t te_fb_clear_alpha_word = 0u;
    uint64_t te_surface_generation = 1u;
};

// Which materializer published a CLT texture resident. Diagnostics only:
// the [clt-accept] audit line carries it so audited draws can be split by
// publishing route; production code must not branch on it. Only the MMDMA
// materializer retains staged TRAM bytes, so per-draw re-decode applies
// exclusively to kTexturePublisherMmdma residents.
enum : uint8_t {
    kTexturePublisherMmdma = 0,
    kTexturePublisherFullLoad = 1,
    kTexturePublisherCompressed = 2,
};

class GuestCltInterpreter {
public:
    explicit GuestCltInterpreter(ICelBlitterBackend* backend);
    ~GuestCltInterpreter();

    // Execute one copied, host-endian command list. Register state persists
    // across lists because the guest TE consumes a shared stream of state.
    // `command_list_base` is the guest byte address of words[0]. It is needed
    // for the vendor DCNTL absolute/relative jump forms; direct fixtures may
    // leave it at zero when their list is addressed from zero.
    CltExecutionResult execute(const uint32_t* words, size_t word_count,
                               const CltMemorySnapshot* memory = nullptr,
                               uint32_t command_list_base = 0u);

    // Release backend texture handles while the backend is still alive.
    void release_resources();

    // Emit the bounded destination/Z census summary when its opt-in
    // diagnostic is enabled. Safe to call more than once.
    void dump_z_census();

    // Apply the source View's pixel-space origin to subsequent CLT draws.
    // The interpreter leaves coordinates unchanged until a complete View plus
    // renderable framebuffer state has been published.
    void set_viewport(int32_t left, int32_t top, int32_t width, int32_t height,
                      bool valid, int32_t display_width,
                      int32_t display_height);

    // Presentation-cadence frame number for host-keyed diagnostics. Set once
    // per pump from the bridge's host frame counter; the interpreter's
    // execute() path otherwise only sees guest-frame counters.
    void set_host_frame(uint32_t host_frame);

    // Apply platform-authored TE surface registers once per setup generation.
    void set_te_surface_state(uint32_t db_super_control, uint32_t db_fb_clip,
                              uint32_t db_dest_control,
                              uint32_t db_dest_base,
                              uint32_t db_dest_x_stride, uint32_t db_z_base,
                              uint32_t db_z_clip, uint32_t z_fill_word,
                              uint32_t fb_clear_red_word,
                              uint32_t fb_clear_green_word,
                              uint32_t fb_clear_blue_word,
                              uint32_t fb_clear_alpha_word,
                              uint64_t generation);

    // Execute the platform-authored TE_CLEAR_Z_BUFFER operation associated
    // with the current surface setup. This does not add guest CLT words.
    void clear_z_buffer();

    // M2_CLT_SELECT_DUMP: sample what the current destination and DB source
    // hold on their first selection in each guest frame, before the list's
    // draws reach the destination.
    void sample_destination_first_select();

    // Execute the platform-authored TE_CLEAR_FRAME_BUFFER operation
    // associated with the current surface setup. This does not add guest CLT
    // words.
    void clear_color_buffer();

    // Register the display-carve spans the DB-source strip blits sample.
    // Called from the bridge render thread with the latest runtime-published
    // spans before each pump batch.
    void set_fb_spans(const std::vector<CltFbSpan>& spans);

    // Retain the last presented frame as a 576x432 RGB555 sheet (big-endian
    // words, matching guest memory byte order) for DB-source blits whose
    // armed base lands inside a registered display carve. Env-gated
    // (M2_DBSRC_FB_READBACK=1); the normal display-carve path uses the
    // in-progress framebuffer, and M2_DBSRC_INPROGRESS=0 selects this older
    // retained-frame diagnostic path.
    void retain_presented_frame();

    // Exposed for deterministic unit tests and diagnostics only.
    uint32_t register_value(uint16_t address) const;
    // Render-thread observation; no state changes and no savestate claim.
    std::string diagnostic_snapshot() const;

private:
    enum class WindowClipMode {
        disabled,
        outside,
        // No guest window clip is active, but the bound destination surface
        // still bounds the write (see update_window_clip()).
        destination,
        // The guest's window clip cannot be represented with one rectangle.
        // The scissor, when one is still set, then carries the destination
        // rectangle alone rather than an inverse or compound guest clip.
        unsupported,
    };

    enum class DestinationBlendMode {
        disabled,
        source_over,
        general_internal,
        general_secondary,
        // Mercury M_DBSpec: BLENDEN is set but SRCEN is clear, so the
        // destination framebuffer is not a secondary input.
        specular_add,
        // Mercury M_DBFog: like M_DBSpec, BLENDEN is set and SRCEN is clear,
        // but the A coefficient is TEXALPHA and the B coefficient is the
        // complement, so the texture-stage color is lerped toward the DBCONSTIN
        // fog color by the per-vertex fog factor instead of added to it.
        fog_composite,
        // Source-frame-buffer read-modify-write with the guest's DBCONSTIN
        // color in the A slot (the statically authored TextDestBlendInit /
        // UI_DrawHighScores_nameEntry equation).
        constant_alpha_over,
        // Source-frame-buffer read-modify-write with the B-only equation (the
        // statically authored carshadow_initshadows form): the destination
        // pixel is multiplied by (1 - texture-blender color).
        inverse_texture_multiply,
        // Source-frame-buffer B-only equation with the identity coefficient
        // (the statically authored screen-transition / frame-buffer-copy
        // form): the bound source frame buffer pixel replaces the destination
        // pixel, independently of the fragment's alpha.
        source_copy,
        // Toolkit two-input weighted mix (`Blt_BlendBlitObject` /
        // `defaultDBlend`, and the guest's `setSsbBlendAttr`): A = TEXCOLOR,
        // B = SRCCOLOR, each scaled by its own multiplier coefficient pair,
        // combined with `A_PLUS_B`/`A_PLUS_BCLAMP`.
        coefficient_weighted_mix,
        // StreamEventFunc's reverse-alpha source-framebuffer equation:
        // A = TEXCOLOR*(1-TEXALPHA), B = SRCCOLOR*TEXALPHA, A+B.
        secondary_source_alpha,
        unsupported,
    };

    // One authored coarser MIP level of a palette-backed MMDMA resident. The
    // base level's stored index/alpha/SSB sidecars live in Texture itself;
    // this is a coarser level's own source fields, retained so a palette or
    // selector refresh re-expands the guest's authored chain under the new
    // use state instead of replacing the coarser levels with host reductions
    // of LOD0 (CLT-PALETTE-REFRESH-LOSES-MIPS).
    struct MmdmaLevelSidecars {
        int width = 0;
        int height = 0;
        // Stored TRAM index per texel; zeros for the no-color rows, whose RGB
        // has no PIP index.
        std::vector<uint8_t> indices;
        // Expanded source alpha per texel when the expansion format carries
        // alpha; empty means the materializer's constant-alpha rule applies.
        std::vector<uint8_t> alpha;
        // Raw SSB bit per texel (may be all zero for formats without one).
        std::vector<uint8_t> source_ssb;
    };

    struct Texture {
        CelId cel = kInvalidCel;
        CelId ssb_mask = kInvalidCel;
        uint32_t tram_base = 0;
        // Loaded byte span, [tram_base, tram_base + tram_size).  The SDK
        // loader writes one blob per texture (all LODs) and the draw-side
        // TxtLODBase0 selects a position inside it (lodBase[i] =
        // tramOffset + sum of earlier texelDataSizes), so a draw whose base
        // lies inside the loaded range targets this texture even when it is
        // not the blob start.
        uint32_t tram_size = 0;
        int width = 0;
        int height = 0;
        // Optional Configure Car footer provenance. These fields are filled
        // only for the bounded M2_CONFIG_TEXTURE_TRACE diagnostic so the
        // draw-side report can identify the source payload and resident that
        // served it without retaining guest pointers.
        uint32_t source_address = 0;
        uint32_t source_span = 0;
        uint32_t source_available = 0;
        uint32_t source_checksum = 0;
        std::array<uint8_t, 16> source_sample{};
        uint8_t source_sample_size = 0;
        uint32_t decoded_checksum = 0;
        bool decoded_checksum_valid = false;
        std::vector<uint8_t> ssb;
        // Cached SSB presence for weighted destination-blender draws. The
        // vector is immutable between materializations, so do not rescan a
        // large resident for every triangle that uses it.
        bool ssb_nonzero = false;
        // DB-source 2D copy blit: the guest guest-source base/stride this
        // texture was loaded from, so a list that draws the armed background
        // without re-snapshotting can reuse the cached cel.
        uint32_t db_source_base = 0;
        // Stable renderer-target identity for display-carve sources. The
        // guest may bias DBSRCBASEADDR every transition step while all those
        // addresses still sample one unchanged target; keep that storage
        // identity separate from the current coordinate origin above.
        uint32_t db_source_storage_base = 0;
        uint32_t db_source_stride = 0;
        // Signed linear-pixel displacement of DBSRCBASEADDR from the stable
        // display-carve snapshot. Transition recipes use non-row-aligned
        // displacements (for example -556 pixels); normalizing those into X/Y
        // before adding the vertex coordinate changes the TE row carry.
        int64_t db_source_origin_pixels = 0;
        bool db_source_msb_replicate = false;
        bool db_source_32bpp = false;
        // Generation of the retained presented frame this cel was built from;
        // a changed retained frame must rebuild the cel (strips re-sample the
        // previous frame every frame).
        uint32_t db_source_seq = 0;
        // Which source the cel bytes came from: 0 = guest RAM, 1 = retained
        // presented frame, 2 = in-progress host framebuffer snapshot. Part
        // of the cache key so the opt-in sources never alias each other.
        uint32_t db_source_kind = 0;
        // Guest RAM buffers are reused in place between menu screens. The
        // address/stride pair identifies storage, not immutable content. A
        // snapshot supplies its 64-bit content identity; hand-built/partial
        // views retain the 32-bit local-checksum fallback in the low word.
        uint64_t db_source_checksum = 0;
        // Palette-backed MMDMA and indexed DCI keep source indices alongside
        // the expanded cel. TXTPIPCNTL is draw/use state, not part of the
        // TRAM texel transfer; the cel may therefore need rebinding when a
        // later draw changes a PIP selector/offset or reloads the palette.
        bool mmdma_indexed = false;
        // No-color indexed rows (and the two guest-authored literal aliases)
        // have no PIP index. Their RGB comes from the SSB-selected PIP
        // constant, so retain the raw alpha/SSB sidecars for the same
        // draw-state rebinding path.
        bool mmdma_no_color = false;
        uint32_t mmdma_exp_type = 0;
        uint32_t mmdma_pip_control = 0;
        uint32_t mmdma_pip_offset = 0;
        uint64_t mmdma_pip_generation = 0;
        uint32_t mmdma_constant0 = 0;
        uint32_t mmdma_constant1 = 0;
        uint64_t mmdma_pip_hash = 0;
        uint64_t mmdma_source_hash = 0;
        // Identity of the retained coarser-level sidecars below. The material
        // cache keys its expanded coarse chain on this, so two residents with
        // the same base level but different authored MIP payloads cannot share
        // an expansion. Zero means the resident has no authored chain.
        uint64_t mmdma_coarse_hash = 0;
        std::vector<uint8_t> mmdma_indices;
        std::vector<uint8_t> mmdma_alpha;
        std::vector<uint8_t> mmdma_source_ssb;
        // The guest's authored coarser levels in lodBase[] order (level k+1 of
        // the drawn base). Empty for single-level residents and for formats
        // without a palette-indexed decode, which keep the base-only refresh
        // and host reductions below LOD0.
        std::vector<MmdmaLevelSidecars> mmdma_coarse_levels;
        // The staged TRAM bytes and the use state this resident's own cel was
        // decoded from. The M2 texture engine has no resident decode: every
        // draw samples the bytes at *its* LOD base under *its* armed
        // registers. Retaining the staged blob is what lets a draw whose
        // state or base differs from this record's be decoded faithfully
        // instead of reusing the record's own decode.
        bool staged_valid = false;
        // Unique per publication. A texture entry is destroyed on eviction
        // and a later one can reuse the same address, so decode caches key on
        // this serial rather than on the record pointer.
        uint64_t staged_serial = 0;
        // Where the staged blob was DMA'd (the byte origin of staged_bytes),
        // and which LOD base the resident's own decode starts at. The two can
        // differ: a draw-time materialization of a packed chain decodes from
        // the draw's interior LOD base, so its decode origin is that base
        // while its blob still starts at the DMA destination.
        uint32_t staged_blob_base = 0;
        uint32_t staged_decode_base = 0;
        // TXTLODBASE1..3 exactly as armed when this decode was materialized.
        // The materializer's footprint recovery and authored-chain decode
        // consume the active LOD quad (TXTADDRCNTL.LODMAX) and, for a
        // LODMAX-0 state, a coherent full quad; a later draw that changes a
        // consumed base has to decode the TRAM bytes under its own quad.
        // LODBASE0 is `staged_decode_base` above.
        std::array<uint32_t, 3> staged_lod_bases{};
        uint32_t staged_source_bit_offset = 0;
        uint64_t staged_blob_hash = 0;
        uint32_t staged_exp_type = 0;
        uint32_t staged_uv_max = 0;
        uint32_t staged_uv_mask = 0;
        uint32_t staged_addr_cntl = 0;
        uint32_t staged_pip_control = 0;
        // A mode-0 TLD that cannot describe its rows with the live TXTEXPTYPE
        // stages the exact destination bits and the TLD's own row geometry
        // (destination row pitch in bits, row count). Both are retained with
        // the bytes: they are the only geometry a raw-row decode has, and a
        // later draw re-decodes those bytes under its own format
        // (row pitch / its bits-per-texel) rather than inheriting this one's.
        bool staged_linear_rows = false;
        uint32_t staged_linear_row_bits = 0;
        uint32_t staged_linear_row_count = 0;
        std::vector<uint8_t> staged_bytes;
        // Set on an eagerly decoded mode-0 (full) load. Its raw rows are
        // retained because TRAM holds raw bits and the hardware applies
        // TXTEXPTYPE at use time: the eager decode used whatever format was
        // live at the TLD, which may be the previous texture's. Only a
        // TXTEXPTYPE that differs from that load-time format re-decodes the
        // rows; LOD-base, UV and address changes keep the eager decode, as
        // they did before these rows were retained.
        bool staged_format_only = false;
        // Which materializer published this resident. The MMDMA materializer
        // and the full-load path (`staged_format_only`) retain staged bytes,
        // so serve_for_draw()'s re-decode trigger applies to those records;
        // compressed loads always keep their own decode. Bounded
        // diagnostic accounting: the [clt-accept] audit line carries it so
        // the audited draws can be split by publishing route.
        uint8_t publisher = kTexturePublisherMmdma;
    };

    struct TeTrackedVertex {
        CltVertex value{};
        uint64_t id = 0;
    };

    struct DestStateCensusEntry {
        bool used = false;
        uint32_t db_super = 0;
        uint32_t db_user = 0;
        uint32_t discard = 0;
        uint32_t db_z = 0;
        uint64_t triangles = 0;
        uint64_t first_list = 0;
        uint32_t first_header_index = 0;
        uint32_t first_header = 0;
        uint64_t first_triangle_id = 0;
        float first_min_x = 0.f;
        float first_min_y = 0.f;
        float first_max_x = 0.f;
        float first_max_y = 0.f;
        float first_min_one_over_w = 0.f;
        float first_max_one_over_w = 0.f;
        bool first_shaded = false;
        bool first_textured = false;
        // Platform-authored Z surface binding at the first triangle of this
        // state (DBZBASEADDR/DBZCLIP as the interpreter holds them). The Z
        // region test is a *bypass* of the depth test outside [clip), so a
        // degenerate clip silently disables depth for every pixel; the census
        // reports the bound words so that case is observable rather than
        // inferred. Observation only.
        uint32_t first_z_base = 0;
        uint32_t first_z_clip = 0;
        // Destination-blender words exactly as the guest wrote them. The
        // census key includes the depth state (DBSUPERGENCONTROL,
        // DBUSERCONTROL, DBDISCARDCONTROL, DBZCNTL) and the blend words, so
        // each distinct blend form in a scene remains observable instead of
        // collapsing into the depth path alone.
        uint32_t db_ssb_dsb = 0;
        uint32_t db_a_mult = 0;
        uint32_t db_b_mult = 0;
        uint32_t db_alu = 0;
        uint32_t db_const_in = 0;
        uint32_t db_src_alpha_cntl = 0;
        uint32_t db_dest_alpha_cntl = 0;
        uint32_t db_dest_alpha_const = 0;
        // Decoded classification for this state: the pre-downgrade
        // DestinationBlendMode the registers select, the CltBlendMode the
        // backends actually receive after the alpha-control downgrade, and
        // whether that downgrade was armed. Observation only.
        uint32_t blend_mode = 0;
        uint32_t effective_blend_mode = 0;
        uint32_t alpha_control_unsupported = 0;
    };

    bool fb_readback_enabled() const;
    bool inprogress_readback_enabled() const;
    // DB-source falsification gate (M2_DBSRC_SKIP=1): drop DB-source background
    // draws instead of rasterizing them. If the race windshield fills with
    // world geometry, the covering quads are the mechanism; if it stays
    // clear, the world never reaches the framebuffer under them.
    bool db_source_skip_enabled() const;

    bool load_texture(const CltMemorySnapshot* memory);
    bool load_pip(const CltMemorySnapshot* memory);
    bool load_uncompressed_texture(const CltMemorySnapshot* memory);
    bool load_compressed_texture(const CltMemorySnapshot* memory,
                                 const char** fail_reason);
    bool load_mmdma_texture(const CltMemorySnapshot* memory,
                            const char** fail_reason);
    bool materialize_mmdma_texture();
    // Shared body. `publish` installs the decode as the current resident (the
    // materialization contract); when false the decode is returned through
    // `out` and no resident/pending state is consumed, which is how a draw
    // whose armed use state differs from the resident's staged state gets a
    // faithful per-draw decode of the retained TRAM bytes.
    struct MaterializeResult {
        CelId cel = kInvalidCel;
        std::vector<uint8_t> ssb;
        int width = 0;
        int height = 0;
        // True when this instance's geometry came from the guest's own
        // description of the texture (a bracketing LOD chain, or the window of
        // a single-LOD load) rather than from a payload-size guess. A draw at
        // an interior base may only reuse the decode when its own registers
        // determine the footprint this way: a guessed size is calibrated for
        // a blob that starts at its own base and is meaningless for a draw
        // aimed into the middle of one.
        bool sized_from_state = false;
    };
    bool materialize_mmdma_texture_impl(bool publish, MaterializeResult* out);
    // What a textured draw actually samples: normally the resident's own
    // decode; otherwise a decode of the resident's retained bytes at the
    // drawn LOD base under the draw's armed registers.
    struct ArmedDecode {
        Texture* record = nullptr;
        CelId cel = kInvalidCel;
        CelId ssb_mask = kInvalidCel;
        int width = 0;
        int height = 0;
        bool ssb_nonzero = false;
        // True when this draw was re-decoded from the resident's retained
        // bytes under the draw's own registers rather than sampled from the
        // resident's own decode.
        bool armed = false;
        // True when the draw's registers differed from the resident's staged
        // state but no faithful re-decode was possible, so the resident's own
        // decode was kept. Bounded diagnostic accounting only.
        bool declined = false;
        // Publishing route of the resident that served this draw. Diagnostics
        // only (the [clt-accept] audit line); production never branches on
        // it. kTexturePublisherMmdma is the zero value so a decode that did
        // not pass through serve_for_draw() still reports the materializer
        // that owns every staged-byte publication.
        uint8_t publisher = kTexturePublisherMmdma;
        // True when the serving resident retained staged TRAM bytes
        // (staged_valid && !staged_bytes.empty()). Diagnostics only.
        bool resident_staged = false;
    };
    // Key and result of one per-draw armed decode. The key is the resident
    // publication serial plus everything the draw's own decode depends on:
    // its LOD base, the four LOD bases that describe its footprint, and the
    // use state (format, UV window/mask, address control, PIP selector and
    // palette generation). Content- and state-based rather than
    // pointer-based, because an evicted resident's address is reused.
    struct ArmedDecodeEntry {
        uint64_t serial = 0;
        uint32_t lod_base = 0;
        uint32_t exp_type = 0;
        uint32_t uv_max = 0;
        uint32_t uv_mask = 0;
        uint32_t addr_cntl = 0;
        uint32_t lod_base1 = 0;
        uint32_t lod_base2 = 0;
        uint32_t lod_base3 = 0;
        uint32_t txt_count = 0;
        uint32_t pip_control = 0;
        uint64_t pip_hash = 0;
        uint64_t pip_generation = 0;
        CelId cel = kInvalidCel;
        CelId ssb_mask = kInvalidCel;
        std::vector<uint8_t> ssb;
        int width = 0;
        int height = 0;
        bool ssb_nonzero = false;
        bool same_key(const ArmedDecodeEntry& other) const
        {
            return serial == other.serial &&
                   lod_base == other.lod_base &&
                   exp_type == other.exp_type && uv_max == other.uv_max &&
                   uv_mask == other.uv_mask &&
                   addr_cntl == other.addr_cntl &&
                   lod_base1 == other.lod_base1 &&
                   lod_base2 == other.lod_base2 &&
                   lod_base3 == other.lod_base3 &&
                   txt_count == other.txt_count &&
                   pip_control == other.pip_control &&
                   pip_hash == other.pip_hash &&
                   pip_generation == other.pip_generation;
        }
    };
    ArmedDecode serve_for_draw(Texture& record, uint32_t lod_base,
                               bool require_ssb_mask = false);
    bool armed_decode_matches(const Texture& record, uint32_t lod_base) const;
    void release_armed_decodes();
    bool load_db_source_texture(const CltMemorySnapshot* memory,
                                const std::vector<CltVertex>& vertices);
    // Record that a guest destination address may have been written in the
    // current state generation. This is called both at list entry and after a
    // DBDESTBASEADDR register write so a later same-list DB-source read cannot
    // reuse a pre-write snapshot for the newly selected target.
    void note_destination_write(uint32_t target);
    // Record a draw's bounds on the current destination, so a later
    // same-list DB-source read of that region re-snapshots the target.
    void note_destination_draw(const CltVertex* vertices, size_t count);
    // Snapshot the selected guest target's display region into
    // m_db_source_window (big-endian RGB555, guest byte order), windowed at
    // window_rel within the addressed display carve. `copy_ssb` carries the
    // renderer target's independent DSB metadata back into bit 15
    // when the same 16-bit framebuffer cells are reloaded through
    // TXTEXPTYPE=0x1605 (dorotatetrans); DB-source reads use only RGB. Returns
    // false when the
    // caller checks the default-on display-carve gate before calling;
    // M2_DBSRC_INPROGRESS=0 selects guest-RAM/A-B paths.
    bool snapshot_db_source_from_target(uint32_t target_base,
                                        int64_t window_rel,
                                        size_t source_bytes, bool copy_ssb,
                                        int sheet_width = 576,
                                        int sheet_height = 432);
    // A draw's TxtLODBase0 selects a position inside the loaded TRAM span
    // (SDK: lodBase[i] = tramOffset + sum of earlier texelDataSizes), so a
    // base contained in [tram_base, tram_base + tram_size) targets this
    // texture even when it is not the blob start.
    bool in_texture_span(uint32_t lod_base0) const;
    // TRAM-region cache: the guest preloads several textures at TRAM
    // base 0 and draws stale regions (e.g. lod 0xff0) after a small load
    // overwrites the single resident. Retain bounded older publishes and
    // serve draws from the most-recent containing entry (real TRAM retains
    // suffix bytes beyond a small prefix overwrite).
    Texture* texture_for_draw(uint32_t lod_base0);
    const Texture* texture_for_draw(uint32_t lod_base0) const;
    bool refresh_mmdma_palette(Texture& entry);
    void free_texture_entry(Texture& entry);
    bool ensure_ssb_mask_for(Texture& entry);
    CelId upload_ssb_mask(int width, int height,
                          const std::vector<uint8_t>& ssb);
    void release_texture();
    void publish_texture(CelId cel, uint32_t tram_base, uint32_t tram_size,
                         int width, int height, std::vector<uint8_t>&& ssb,
                         uint8_t publisher = kTexturePublisherMmdma);
    bool ensure_ssb_mask();
    void update_window_clip();
    void update_destination_blend();
    CltBlendState destination_blend_state() const;
    // The destination blender's multiplier coefficient constants are selected
    // per texel by the CLT "source selection bit" (SSB): `...CONSTSSB0` for
    // texels with SSB 0 and `...CONSTSSB1` for texels with SSB 1, with absent
    // SSB passed as 0.  A weighted-mix draw whose two entries resolve to
    // different weights is therefore two different equations across the
    // primitive; when the draw's SSB signal can set that bit, the gate arms
    // the per-fragment entry selection (uploading the texture's SSB mask cel)
    // instead of downgrading the draw, and fails closed to the recovered
    // source-over default only when the mask cannot be carried.
    void gate_weighted_mix_on_ssb(CltBlendState& state, Texture* texture,
                                  const ArmedDecode* served = nullptr);
    // Content-keyed indexed-material cache. Palette-backed TRAM residents
    // retain raw indices/alpha/SSB and are rebound when TXTPIPCNTL or the PIP
    // contents change. Cache the resulting CPU pixels as well, so that a
    // palette alternation does not repeat the per-texel selector loop for
    // every older resident. The guest's authored coarser levels are expanded
    // with the base and cached in the same entry, keyed by `coarse_hash`, so a
    // refresh does not re-expand or re-hash the whole MIP chain. Entries hold
    // CPU data only; GPU cel lifetimes remain owned by the ordinary
    // publish/free path. The backends' retired-texture cache keys on the same
    // `pixel_hash`/`coarse_chain_hash` identities, which is what reuses the
    // cel when palette state alternates; a CelId stored here would alias a
    // cel another resident may have freed.
    struct MmdmaMaterialKey {
        uint64_t source_hash = 0;
        uint64_t coarse_hash = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t exp_type = 0;
        uint32_t pip_control = 0;
        uint32_t constant0 = 0;
        uint32_t constant1 = 0;
        uint64_t pip_palette_hash = 0;
        bool no_color = false;
        bool operator==(const MmdmaMaterialKey& other) const
        {
            return source_hash == other.source_hash &&
                   coarse_hash == other.coarse_hash &&
                   width == other.width && height == other.height &&
                   exp_type == other.exp_type &&
                   pip_control == other.pip_control &&
                   constant0 == other.constant0 &&
                   constant1 == other.constant1 &&
                   pip_palette_hash == other.pip_palette_hash &&
                   no_color == other.no_color;
        }
    };
    struct MmdmaMaterialValue {
        std::vector<uint32_t> pixels;
        std::vector<uint8_t> ssb_bits;
        uint64_t pixel_hash = 0;
        // Expanded guest-authored coarser levels, aligned with
        // Texture::mmdma_coarse_levels. `coarse_chain_hash` is the same
        // identity `cel_lod_chain_hash` produces for those levels. Empty/zero
        // for a single-level resident.
        std::vector<std::vector<uint32_t>> coarse_pixels;
        uint64_t coarse_chain_hash = 0;
    };
    const MmdmaMaterialValue* find_cached_mmdma_material(
        const MmdmaMaterialKey& key);
    void store_cached_mmdma_material(const MmdmaMaterialKey& key,
                                     MmdmaMaterialValue&& value);
    // Identity of the retained coarser-level sidecars, used by the material
    // cache key so a chain cannot alias a different one that shares the base.
    static uint64_t mmdma_coarse_sidecar_hash(
        const std::vector<MmdmaLevelSidecars>& levels);
    // Content-keyed MMDMA decode cache. The guest re-DMAs the same texture
    // bytes on every application frame; unpacking them again is host-only
    // work with no guest-visible effect. The key carries the staged blob hash
    // plus every decode parameter (bit offsets, expansion format, resolved
    // dimensions, PIP control/constants and a hash of the PIP palette
    // contents), so a changed blob or palette decodes fresh. The palette is
    // keyed by content rather than by the load generation because the guest
    // re-arms the same palette every frame; content equality means the
    // decoded texels are identical. Entries hold CPU texel data only and
    // never own a GPU cel, leaving publish/free lifetimes unchanged.
    struct MmdmaDecodeKey {
        uint64_t blob_hash = 0;
        uint32_t source_bit_offset = 0;
        uint64_t draw_bit_offset = 0;
        uint32_t exp_type = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bits_per_texel = 0;
        uint32_t pip_control = 0;
        uint32_t constant0 = 0;
        uint32_t constant1 = 0;
        uint64_t pip_palette_hash = 0;
        bool operator==(const MmdmaDecodeKey& other) const
        {
            return blob_hash == other.blob_hash &&
                   source_bit_offset == other.source_bit_offset &&
                   draw_bit_offset == other.draw_bit_offset &&
                   exp_type == other.exp_type && width == other.width &&
                   height == other.height &&
                   bits_per_texel == other.bits_per_texel &&
                   pip_control == other.pip_control &&
                   constant0 == other.constant0 &&
                   constant1 == other.constant1 &&
                   pip_palette_hash == other.pip_palette_hash;
        }
    };
    struct MmdmaDecodeValue {
        std::vector<uint32_t> pixels;
        std::vector<uint8_t> ssb_bits;
        std::vector<uint8_t> palette_indices;
        std::vector<uint8_t> palette_alpha;
        std::vector<uint8_t> source_alpha;
        std::vector<uint8_t> source_ssb;
        std::vector<uint8_t> palette_source_ssb;
        uint64_t pixel_hash = 0;
        // Hash of the retained indexed source sidecars.  Publish paths use
        // this cached identity when a resident is re-decoded from the host
        // cache, avoiding an O(texel) walk on every guest re-DMA.
        uint64_t sidecar_hash = 0;
    };
    const MmdmaDecodeValue* find_cached_mmdma_decode(
        const MmdmaDecodeKey& key);
    void store_cached_mmdma_decode(const MmdmaDecodeKey& key,
                                   MmdmaDecodeValue&& value);

    // One MMDMA level to decode out of the staged TRAM blob. `bit_offset` is
    // the absolute bit position of the level's first texel inside that blob;
    // a MIP chain's coarser levels start at their own lodBase[] byte, so the
    // same decode runs for every level of the residency.
    struct MmdmaLevelRequest {
        uint64_t bit_offset = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t exp_type = 0;
        uint32_t pip_control = 0;
        uint32_t constant0 = 0;
        uint32_t constant1 = 0;
    };
    // Decode one level. Returns null on success, or the materializer's
    // failure reason on a short/unsupported read so the caller keeps its
    // existing trace.
    const char* decode_mmdma_level(const MmdmaLevelRequest& request,
                                   MmdmaDecodeValue* out);
    // Authored MIP chain of an MMDMA residency: the levels a draw that
    // selected `draw_base` can still sample, in increasing-LOD order, each
    // with the byte span the guest's lodBase[] quad gave it. Empty when the
    // armed registers do not describe a chain.
    struct MmdmaLodChain {
        struct Level {
            uint32_t base = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            // TRAM bytes the quad gave this level: the next base's offset, or
            // the staged blob's end for the coarsest loaded level.
            uint32_t span_bytes = 0;
        };
        std::vector<Level> levels;
    };
    bool mmdma_authored_lod_chain(uint32_t draw_base, uint32_t level_width,
                                  uint32_t level_height,
                                  MmdmaLodChain* chain) const;
    // Content-keyed cache for the mode-0 full/compressed texture loads. These
    // re-decode the same source bytes on every application frame (the guest
    // re-issues its TRAM loads per frame; real TE hardware reloads them for
    // free). The key covers the hashed source bytes and every decode input:
    // load control/count/width, expansion and source formats, PIP
    // control/constants and a content hash of the PIP palette.
    struct LoadDecodeKey {
        uint64_t source_hash = 0;
        uint32_t source_address = 0;
        uint64_t source_span = 0;
        uint32_t load_control = 0;
        uint32_t count = 0;
        uint32_t load_width = 0;
        uint32_t exp_type = 0;
        uint32_t src_type01 = 0;
        uint32_t src_type23 = 0;
        uint32_t pip_control = 0;
        uint32_t constants[4] = {0, 0, 0, 0};
        uint64_t pip_palette_hash = 0;
        bool operator==(const LoadDecodeKey& other) const
        {
            if (source_hash != other.source_hash ||
                source_address != other.source_address ||
                source_span != other.source_span ||
                load_control != other.load_control ||
                count != other.count || load_width != other.load_width ||
                exp_type != other.exp_type ||
                src_type01 != other.src_type01 ||
                src_type23 != other.src_type23 ||
                pip_control != other.pip_control ||
                pip_palette_hash != other.pip_palette_hash)
                return false;
            for (unsigned i = 0; i < 4u; ++i) {
                if (constants[i] != other.constants[i]) return false;
            }
            return true;
        }
    };
    const MmdmaDecodeValue* find_cached_load_decode(
        const LoadDecodeKey& key);
    void store_cached_load_decode(const LoadDecodeKey& key,
                                  MmdmaDecodeValue&& value);
    // Guest Z-buffer state for the current draw: DBZCNTL/ZBUFEN/ZOUTEN plus
    // the DBZCLIP/DBZOFFSET Z region folded into target-pixel space (the
    // View origin is added to vertices at emit time, so the region bounds
    // carry the same translation) and DBDISCARDCONTROL.ZCLIPPED.
    CltDepth guest_depth_state() const;
    void dump_te_state_comparison();
    // Single-frame per-draw provenance (M2_DRAW_PROV_FRAME): one bounded
    // [draw-prov] line per emitted draw in execution (painter) order. Off by
    // default; capped. No rendering effect.
    void draw_prov_note(uint32_t header, bool textured, int serve_w,
                        int serve_h, const CltVertex* tri, size_t count,
                        const CltDepth& depth, bool blend);
    // Bounded Configure Car footer provenance (M2_CONFIG_TEXTURE_TRACE):
    // report the source/decode identity and the current-vs-cached TRAM
    // resident immediately before the backend draw. Observation only.
    void config_texture_draw_diag(const char* path, const Texture& texture,
                                  uint32_t lod_base0, uint32_t header,
                                  const CltVertex* tri, size_t count);

    ICelBlitterBackend* m_backend = nullptr;
    std::vector<uint32_t> m_registers;
    std::vector<uint32_t> m_pip;
    Texture m_texture;
    // Bounded older TRAM publishes (oldest at front, most-recent before
    // m_texture at back). Capacity 16 keeps GPU cel lifetimes bounded while
    // covering the observed preload-then-draw-stale-region pattern.
    std::vector<Texture> m_tram_cache;
    // Bounded cache of per-draw armed decodes. Keyed by the resident's
    // identity plus the draw's own LOD base and use state, so a race's
    // repeated stale-region draws reuse one decode instead of re-uploading a
    // cel per triangle.
    std::vector<ArmedDecodeEntry> m_armed_decodes;
    uint64_t m_texture_serial = 0;
    Texture m_db_source;
    bool m_pending_mmdma_texture = false;
    uint32_t m_pending_mmdma_source_bit_offset = 0;
    uint32_t m_pending_mmdma_destination_base = 0;
    std::vector<uint8_t> m_pending_mmdma_bytes;
    uint64_t m_pending_mmdma_hash = 0;
    // A mode-0 load can observe the preceding texture's TXTEXPTYPE. Retain
    // its raw row geometry until the following use-state supplies the format.
    bool m_pending_linear_rows = false;
    uint32_t m_pending_linear_row_bits = 0;
    uint32_t m_pending_linear_row_count = 0;
    // Bounded decode cache (oldest first); see MmdmaDecodeKey above.
    std::deque<std::pair<MmdmaDecodeKey, MmdmaDecodeValue>>
        m_mmdma_decode_cache;
    std::deque<std::pair<LoadDecodeKey, MmdmaDecodeValue>>
        m_load_decode_cache;
    size_t m_mmdma_decode_cache_bytes = 0;
    size_t m_load_decode_cache_bytes = 0;
    std::deque<std::pair<MmdmaMaterialKey, MmdmaMaterialValue>>
        m_mmdma_material_cache;
    size_t m_mmdma_material_cache_bytes = 0;
    int32_t m_viewport_left = 0;
    int32_t m_viewport_top = 0;
    int32_t m_display_width = 0;
    int32_t m_display_height = 0;
    bool m_viewport_valid = false;
    uint64_t m_te_surface_generation = 0;
    uint32_t m_te_z_fill_word = 0;
    uint32_t m_te_fb_clear_red_word = 0;
    uint32_t m_te_fb_clear_green_word = 0;
    uint32_t m_te_fb_clear_blue_word = 0;
    uint32_t m_te_fb_clear_alpha_word = 0;
    WindowClipMode m_window_clip_mode = WindowClipMode::disabled;
    DestinationBlendMode m_destination_blend_mode =
        DestinationBlendMode::disabled;
    // Set by update_destination_blend(): an armed RGB equation consumes
    // source alpha while the source is 16bpp (no stored alpha). Diagnostic
    // tripwire input only (M2_CLT_SRCALPHA16_TRIPWIRE).
    bool m_rgb_needs_16bit_source_alpha = false;
    // M2_RGB555_DITHER (opt-in), sampled at construction.
    bool m_rgb555_dither = false;

    // A destination-blend mode that writes the bound destination frame buffer
    // from the guest's equation has to reach the backends with blending
    // enabled; `specular_add` and `disabled`/`unsupported` do not touch the
    // destination.
    static constexpr bool destination_blend_reaches_backend(
        DestinationBlendMode mode)
    {
        return mode == DestinationBlendMode::general_internal ||
               mode == DestinationBlendMode::general_secondary ||
               mode == DestinationBlendMode::source_over ||
               mode == DestinationBlendMode::constant_alpha_over ||
               mode == DestinationBlendMode::inverse_texture_multiply ||
               mode == DestinationBlendMode::source_copy ||
               mode == DestinationBlendMode::coefficient_weighted_mix ||
               mode == DestinationBlendMode::secondary_source_alpha;
    }
    std::vector<CltFbSpan> m_fb_spans;
    // Retained previous frame: 576*432*2 bytes of big-endian RGB555 covering
    // the display carve sheet, plus the generation of the present it came
    // from. Empty until the first retain_presented_frame() with a readable
    // backend framebuffer.
    std::vector<uint8_t> m_retained_frame;
    // Render-thread scratch buffers. They retain capacity between frames so
    // the optional DB-source/readback path does not allocate once per frame.
    std::vector<uint32_t> m_retained_rgba;
    // Windowed display-carve readback scratch. GL backends fill only the
    // authored 576x432 region when the addressed target supports it; the full
    // retained buffer above remains the fallback contract.
    std::vector<uint32_t> m_retained_rgba_window;
    std::vector<uint8_t> m_db_source_window;
    std::vector<uint32_t> m_db_source_pixels;
    // Rotate-wipe strip cache: the 36 MMDMA strip loads in one frame all read
    // the same display sheet. The full sheet is read and converted once per
    // source generation/target, then each strip is copied out of it. Keying
    // on the generation (which advances at list entry and on destination
    // writes) keeps a strip read after a same-list draw to the source fresh.
    std::vector<uint8_t> m_db_sheet_bytes;
    uint64_t m_db_sheet_generation = ~0ull;
    uint32_t m_db_sheet_target = 0;
    uint32_t m_retained_seq = 0;
    // Diagnostic list sequence. It advances once per execute() only while a
    // list-gated diagnostic is armed (see clt_exec_seq_live) so that a FROM
    // floor can select a list; in production it stays frozen at zero.
    uint64_t m_exec_seq = 0;
    // Always-advancing production state generation. It advances at list entry
    // and at each nonzero mid-list DBDESTBASEADDR write. Addressed destination
    // targets retain their last possible-write generation below; the map is
    // updated at both boundaries. A DB-source snapshot is therefore rebuilt
    // when its actual renderer target may have changed, while a transition
    // that merely biases DBSRCBASEADDR across an untouched sibling target can
    // reuse the same snapshot.
    uint64_t m_db_source_generation = 0;
    std::vector<std::pair<uint32_t, uint32_t>>
        m_target_write_generations;
    // Per target, the union of draw bounds (destination pixels) not yet
    // observed by its in-progress DB-source snapshot. Bounded like the map
    // above.
    struct DirtyBounds {
        uint32_t target;
        float min_x, min_y, max_x, max_y;
    };
    std::vector<DirtyBounds> m_target_dirty_bounds;
    // Last host presentation frame seen via set_host_frame. Only consumed by
    // host-keyed diagnostics (M2_DRAW_PROV_FRAME); never affects rendering.
    uint32_t m_host_frame = 0;
    // Last guest PIP publication, retained only to correlate the bounded
    // materializer audit with the CLT list that supplied its palette.
    uint32_t m_last_pip_source = 0;
    uint32_t m_last_pip_bytes = 0;
    uint64_t m_last_pip_exec_seq = 0;
    uint64_t m_last_pip_frame = 0;
    uint64_t m_pip_generation = 0;
    // Content hash of the resident PIP RAM.  PIP is a 256-word table and its
    // content is unchanged for most draw calls; retain the hash at the DMA
    // publication boundary instead of walking the table for every indexed
    // triangle/material lookup.
    uint64_t m_pip_hash = 0;
    // Non-clear-texel fraction (percent) of the last in-progress DB
    // snapshot, for the capped [dbsrc-fb] content diagnostic.
    uint32_t m_db_inprog_nonclear_pct = 0;
    // Mean 5-bit green of the same snapshot. Distinguishes a destination
    // whose composition advances from one that is served a frozen copy.
    uint64_t m_db_inprog_green_mean = 0;
    // The setup engine retains three vertices and its replacement ordering
    // across vertex headers and command-list submissions. This is deliberately
    // shared for RM/fan and non-RM/strip continuations; separate fan/strip
    // histories do not reproduce the M2 hardware state machine.
    // These buffers are reused across vertex headers. Their contents remain
    // scoped to the current header; retaining capacity avoids a host
    // allocation/free pair for every guest packet without changing the
    // guest-visible setup-engine state.
    std::vector<CltVertex> m_vertex_scratch;
    std::vector<TeVertexState<TeTrackedVertex>::Triangle>
        m_te_triangles_scratch;
    std::vector<std::array<uint64_t, 3>> m_new_triangle_refs_scratch;
    std::vector<CltVertex> m_new_vertices_scratch;
    // Consecutive vertex packets with an unchanged complete draw state can
    // share one GL_TRIANGLES submission.  This is host-only presentation
    // scratch; the TE retained slots and guest packet boundaries remain
    // independent.
    std::vector<CltVertex> m_pending_vertices_scratch;
    TeVertexState<TeTrackedVertex> m_te_vertex_state;
    // Independent table-driven model used only when the bounded TE trace is
    // enabled. Keeping it separate from the production switch makes a
    // production/reference comparison meaningful.
    TeReferenceVertexState<TeTrackedVertex> m_te_reference_state;
    uint64_t m_te_next_vertex_id = 0;
    uint64_t m_te_reference_triangles = 0;
    uint32_t m_te_reference_divergences = 0;
    bool m_te_reference_divergence_reported = false;
    bool m_te_reference_summary_dumped = false;
    uint32_t m_te_trace_emitted = 0;
    bool m_te_trace_cap_reported = false;
    uint32_t m_te_flat_trace_emitted = 0;
    bool m_te_flat_trace_cap_reported = false;
    uint64_t m_te_flat_triangle_sequence = 0;
    uint32_t m_dest_trace_emitted = 0;
    bool m_dest_trace_cap_reported = false;
    std::array<DestStateCensusEntry, 128> m_dest_state_census{};
    uint32_t m_dest_state_census_emitted = 0;
    bool m_dest_state_census_cap_reported = false;
    bool m_dest_state_census_dumped = false;
};

// --- M2_CLT_DEPTH_W16_PACKED probe (diagnostic-only) -------------------------
// Patent-backed candidate 16-bit W cell ("Raw 16-bit W-cell evidence",
// WO1996036011A1 block-fix):
// bits 15:14 hold the range e, bits 13:0 hold the 14-bit significand s, with
// 1/w = (s / 2^14) * 2^(-3e). Production stays on reciprocal-W; these helpers
// mirror SoftCaptureBackend's m2_w16_packed probe path and are only consumed
// when M2_CLT_DEPTH_W16_PACKED is set. Cross-range ordering MUST use the
// decoded (range, significand) value, never the raw u16 word order.
namespace clt_w16_packed_probe {
bool enabled();
uint16_t encode(float one_over_w, unsigned range);
float decode(uint16_t cell);
// Integer-exact -1/0/+1 ordering by decoded block-fix value (common
// denominator 2^23); never the raw u16 order, which ranges reverse.
int compare(uint16_t a, uint16_t b);
}  // namespace clt_w16_packed_probe

// --- M2_CLT_Z_CELL_DUMP probe (diagnostic-only) -------------------------------
// Bounded raw-guest-Z readout: the race-window trace proves the guest never
// writes DBZBASEADDR on route (bound default 0x56a000 stands), so byte order
// is unobservable until the host reads raw guest Z cells. These pure helpers
// cover the offset math (byte address z_base + 2*(x+y*stride_words) for a
// small origin window) and the line formats, which print the stored bytes
// plus both BE and LE u16 readings without asserting which is correct.
// Production rendering never consumes them; the single live dump site reads
// the submit-time snapshot through find_memory and fails closed with one
// line when the base is unmapped.
namespace clt_z_cell_probe {
// Decimal word count carried by M2_CLT_Z_CELL_DUMP itself: 0 = off (unset,
// empty, or 0); truthy non-numeric tokens take the default 16; numeric
// values clamp to 256.
uint32_t dump_word_count();
// Byte offset of cell (x, y) in a stride_words-wide 16-bit plane.
uint32_t cell_offset_bytes(uint32_t x, uint32_t y, uint32_t stride_words);
// Absolute guest address of cell (x, y) for the bound Z base.
uint32_t cell_address(uint32_t z_base, uint32_t x, uint32_t y,
                      uint32_t stride_words);
// Total snapshot bytes covering the first word_count cells of the window.
size_t dump_span_bytes(uint32_t word_count, uint32_t stride_words);
uint16_t decode_be(uint8_t hi, uint8_t lo);
uint16_t decode_le(uint8_t hi, uint8_t lo);
// PPC-natural BE cell write (diagnostic-only): stores
// [hi,lo]=[(cell>>8)&0xFF, cell&0xFF] so the PPC-natural BE word lands
// [hi,lo] at zaddr via the WORD2_XOR_BE bus swizzle.
// The LE host MUST use this helper for any future guest-Z
// write; mirrors decode_be/decode_le.
void encode_be(uint16_t cell, uint8_t* hi, uint8_t* lo);
// Row width of the small origin window (bounded 8-wide tiling).
uint32_t window_width(uint32_t stride_words);
void coords_for_index(uint32_t index, uint32_t stride_words, uint32_t* x,
                       uint32_t* y);
int format_cell_line(char* out, size_t out_size, uint64_t list, uint32_t x,
                      uint32_t y, uint32_t address, uint8_t byte0,
                      uint8_t byte1);
int format_header_line(char* out, size_t out_size, uint64_t list,
                        uint32_t base, uint32_t stride_words,
                        uint32_t word_count);
int format_unmapped_line(char* out, size_t out_size, uint64_t list,
                          uint32_t base, uint32_t stride_words,
                          uint32_t word_count);
}  // namespace clt_z_cell_probe
// Executed-list sequence liveness (diagnostic-only): true while a list-gated
// diagnostic (M2_CLT_Z_CELL_DUMP, M2_CLT_Z_SURFACE_TRACE, M2_CLT_DRAW_LIST
// filter) is armed. GuestCltInterpreter::execute() advances its per-list
// sequence only then; otherwise the counter stays frozen at zero so the
// default-on in-progress DB-source snapshot key behaves bit-identically in
// production runs. Consumed by the interpreter and by unit tests pinning the
// calibration; production rendering never calls it.
bool clt_exec_seq_live();

// --- texture channel widening probe (contract-test surface) ------------------
// Every stored or source texture channel field that reaches the host is
// widened to 8 bits by exactly one documented rule: the CLT toolkit's TXTLOAD
// expansion section, "replication of MSBs" (a 555 literal color to 888, and a
// 4-bit source alpha expanded as abcd -> abcdabc). Exposing it lets
// m2_guest_clt_test compare production against an independent transcription of
// that rule across the whole depth/value domain, and against the dissenting
// toolkit *utility* rule (M2TXcompress.c's qmem.h oneExtend, which fills the
// low bits from the source LSB). The two rules disagree on most odd values,
// which is exactly why the same TRAM field must not be widened two ways
// depending on the load path that produced the resident.
namespace clt_tex_expand_probe {
uint32_t widen(unsigned depth, uint32_t value);
}  // namespace clt_tex_expand_probe

// --- TXTADDRCNTL filter-selector contract ----------------------------------
// The vendor header defines point/bilinear for the minification and
// magnification fields and point/linear/bilinear/trilinear for the inter-level
// field; every other encoding is reserved. A reserved selector has no
// source-backed mapping, so production refuses the draw instead of decoding it
// to a plausible filter. `reserved` reports that refusal; it is not a
// deliverable sampling state.
struct CltFilterSelector {
    CltTextureFilter filter = CltTextureFilter::point;
    bool reserved = false;
};

struct CltTextureSamplingDecode {
    CltTextureSampling sampling;
    bool filter_reserved = false;
};

// --- TXTUVMASK address-transform probe (contract-test surface) ---------------
// The guest TXTADDRCNTL/TXTUVMASK tuple decodes to one draw-time address
// state. Exposing the decode lets m2_guest_clt_test pin the vendor contract
// independently of any renderer: TXTUVMASK is an address mask, so the period
// (mask + 1) is applied to the integer texel coordinate, and a period at or
// below the resident extent repeats while a larger one leaves the axis
// clamped. Production uses the same function on the interpreter's draw path.
namespace clt_uv_address_probe {
CltTextureSamplingDecode decode(uint32_t addr_cntl, uint32_t uv_mask,
                                float texture_width, float texture_height);
}  // namespace clt_uv_address_probe

struct GuestCltStats {
    uint64_t submitted = 0;
    uint64_t dropped = 0;
    uint64_t queue_full_drops = 0;
    uint64_t malformed = 0;
    uint64_t unsupported = 0;
    uint64_t draws = 0;
    uint64_t peak_queued_lists = 0;
    uint64_t peak_queued_bytes = 0;
    uint64_t peak_pump_batch = 0;
    uint64_t control_flow_jumps = 0;
    uint64_t control_flow_pauses = 0;
    uint64_t control_flow_external_handoffs = 0;
    uint64_t control_flow_refusals = 0;
    uint64_t control_flow_loops = 0;
    // Bitmap display-span registrations released to make room for a newer
    // guest item. Every one is an explicit, counted capacity decision; the
    // required display chain is never the victim, so a nonzero value means the
    // guest published more distinct Bitmap items than the bounded span table
    // holds, not that a display plane was dropped.
    uint64_t span_evictions = 0;
    // Pump batches that found no registered span for the guest's framebuffer
    // item, so the interpreter's DB-source reads fall back to guest RAM. Counted
    // once per pump batch (the pump evaluates the condition once per batch, not
    // per draw). The fallback is correct for an ordinary guest-RAM framebuffer
    // and is reported rather than refused, which is exactly why it needs a
    // counter: a *composed* target that loses its span reads stale guest bytes
    // silently, and the per-run log sample for it is capped at 16 lines.
    uint64_t fb_span_missing = 0;
};

class GuestCltBridge {
public:
    explicit GuestCltBridge(ICelBlitterBackend* backend);
    ~GuestCltBridge();

    GuestCltBridge(const GuestCltBridge&) = delete;
    GuestCltBridge& operator=(const GuestCltBridge&) = delete;

    // Original screenshot's CPU framebuffer read. Only the render thread
    // touches the backend; detach releases a waiting guest with an error.
    int32_t read_framebuffer(uint32_t base, void* dst, uint32_t count);
    void service_framebuffer_reads();

    // Copy a guest-owned big-endian payload immediately.  No guest pointer is
    // retained after this call returns.
    void enqueue(uint32_t io_item, uint32_t command, uint32_t command_options,
                 uint32_t guest_buffer, uint32_t byte_count,
                 const uint8_t* guest_bytes,
                 const uint8_t* guest_memory, uint32_t guest_memory_size);

    // Accept a source-shaped TE setup update. The state is copied into later
    // list submissions under the same lock as the queue.
    void update_graphics_state(
        uint32_t io_item, uint32_t command, uint32_t item,
        uint32_t view_item, uint32_t framebuffer_item, uint32_t zbuffer_item,
        int32_t view_left, int32_t view_top, int32_t view_width,
        int32_t view_height, int32_t display_width, int32_t display_height);

    // Accept the platform-authored TE surface setup that is not present in
    // ordinary CLT list payloads. Values are source register values, not
    // host pointers; the generation preserves later guest register writes
    // until the next actual surface setup.
    void update_te_surface_state(
        uint32_t db_super_control, uint32_t db_fb_clip,
        uint32_t db_dest_control, uint32_t db_dest_base,
        uint32_t db_dest_x_stride, uint32_t db_z_base, uint32_t db_z_clip,
        uint32_t z_fill_word, uint32_t fb_clear_red_word,
        uint32_t fb_clear_green_word, uint32_t fb_clear_blue_word,
        uint32_t fb_clear_alpha_word);

    // Record the item's current display-carve allocation, published by the
    // guest (care=3 bitmap buffer via ModifyGraphicsItemVA). Called from the
    // runtime's graphics dispatch on the guest thread; stored under the queue
    // lock. A later nonzero registration for the same item rebinds the span to
    // the new allocation; a zero buffer or size unpublishes it.
    //
    // The table holds one span per distinct guest item and is bounded by the
    // recovered item pool, not by a fixed count of display planes: the runtime
    // publishes every nonzero Bitmap buffer, so a cap low enough to be reached
    // by unrelated bitmaps would drop the display chain itself. When the bound
    // is reached the release victim is a span outside the current display
    // chain, and the decision is counted in stats().span_evictions.
    void register_bitmap_buffer(uint32_t item, uint32_t buffer, uint32_t size);

    // Retain the just-presented frame for the next pump's DB-source strip
    // blits. Called from the host render thread right after frame_present().
    void retain_presented_frame();

    // Execute queued lists on the host/render thread.  The guest callback only
    // copies data and never calls a graphics API.  max_frame bounds the pump
    // to submissions tagged at or below a completed guest application frame so
    // a look-ahead guest frame is never partially interpreted; the default
    // pumps every queued submission (capture/diagnostic semantics).  `phase`
    // only labels the bounded transaction trace (M2_CLT_TRANSACTION_TRACE).
    void pump(uint32_t max_frame = 0xffffffffu, const char* phase = "all");

    // True when at least one queued submission is tagged at or below
    // max_frame. The interactive look-ahead path uses this to keep the target
    // presented on the prior frame when a completed application frame submits
    // no CLT work (hardware would also leave the display unchanged).
    bool has_pending_upto(uint32_t max_frame) const;
    // Optionally return the oldest ready unit's publication tag. The guest's
    // current frame may already be newer; it is not a safe draw boundary.
    bool has_display_target_upto(uint32_t max_frame,
                                uint32_t* out_frame = nullptr) const;
    // Pump only through the oldest ready display unit, leaving later guest
    // frames queued for their own renderer/depth boundary.
    void pump_display_target_upto(uint32_t max_frame);

    // Publish one guest DISPLAYFRAMEBUFFER unit with the application frame that
    // issued it. A frame may publish a sequence of them (the authored state
    // transitions are 30-, 48- and 16-frame animations submitted from inside
    // one `StateDriver` iteration), so every command is queued in order rather
    // than folded into one choice per frame.
    void publish_display_target(uint32_t guest_frame, uint32_t target);
    // Present the oldest pending display unit whose guest frame is at or below
    // max_frame, and return its target (0 when none is eligible, in which case
    // the caller keeps the target presented on the prior frame). Only one unit is
    // consumed per call so a multi-frame authored animation is presented frame
    // by frame, as the TE drains one display command per video frame. Callers
    // pass the frame the guest has reached, not the last completed one: a guest
    // frame posts its DISPLAYFRAMEBUFFER from its own synchronous GS_EndFrame,
    // after GS_SendList/GS_WaitTEPass completed that frame's lists, so "in
    // flight" does not describe the publisher; bounding on the completed frame
    // instead still presents the sequence correctly but leaves a standing
    // backlog equal to its length. When out_frame is non-null it receives that
    // unit's guest frame tag, so a caller can prove the presented target was
    // published by a frame the guest had reached.
    uint32_t consume_display_target_upto(uint32_t max_frame,
                                         uint32_t* out_frame = nullptr);
    void set_target_persistence(bool enabled) {
        m_target_persistence = enabled;
    }

    // Diagnostic policy seam for the interactive look-ahead pump. An in-flight
    // guest frame may only be interpreted while its owning application frame
    // can never complete -- the synchronous video-overlay (movie) lifecycle.
    // Off (default) preserves the general look-ahead pump over every scene.
    void set_inflight_overlay_only(bool enabled) {
        m_inflight_overlay_only = enabled;
    }

    // True when this iteration may interpret the still-running guest frame.
    // `completed` is the last application frame that reached its loop head and
    // `current` the frame still being built. Interpreting `current` is only
    // safe where its owning frame will never complete, because its partial
    // lists are executed into the target presented in this same iteration.
    bool inflight_lookahead_allowed(bool interactive_pacing, bool video_overlay,
                                    uint32_t completed,
                                    uint32_t current) const;

    // Bound the optional CLT/frame correlation diagnostic to the host's
    // presentation cadence. These calls do not affect normal rendering.
    void host_frame_begin(uint32_t host_frame, bool clears_target);
    void host_frame_present(bool presented);

    // Stop accepting new callback submissions.  Existing queued copies remain
    // inspectable but are not required during shutdown.
    void detach();

    // Release any backend resources owned by the interpreter. Call before the
    // backend is shut down.
    void release_resources();

    // Emit the interpreter's bounded destination/Z census summary before the
    // launcher enters its process-level shutdown path.
    void dump_z_census();

    GuestCltStats stats() const;
    std::string diagnostic_snapshot() const;

    // Exposed for deterministic unit tests and diagnostics only.
    GuestGraphicsState graphics_state_for_testing() const;

private:
    struct Submission {
        uint32_t io_item = 0;
        uint32_t command = 0;
        uint32_t command_options = 0;
        uint32_t guest_buffer = 0;
        uint32_t guest_frame = 0;
        uint32_t guest_checksum = 0;
        uint64_t sequence = 0;
        std::vector<uint32_t> words;
        CltMemorySnapshot memory;
        GuestGraphicsState graphics_state;
    };

    struct DisplaySelection {
        uint32_t guest_frame = 0;
        uint32_t target = 0;
        uint64_t submission_sequence = 0;
    };

    struct FramebufferRead {
        uint32_t base = 0, frame = 0;
        uint64_t sequence = 0;
        GuestGraphicsState state;
        std::vector<uint8_t> bytes;
        bool done = false, ok = false;
    };
    ICelBlitterBackend* m_backend;
    std::shared_ptr<FramebufferRead> m_framebuffer_read;

    mutable std::mutex m_mutex;
    std::condition_variable m_queue_cv;
    std::deque<Submission> m_queue;
    uint64_t m_queued_bytes = 0;
    uint64_t m_submission_sequence = 0;
    GuestCltStats m_stats;
    GuestCltInterpreter m_interpreter;
    GuestGraphicsState m_graphics_state;
    // Pending display units in guest order. One entry per DISPLAYFRAMEBUFFER
    // command: the device's display queue, not a per-frame "current choice".
    std::deque<DisplaySelection> m_display_selections;
    std::vector<CltFbSpan> m_fb_spans;
    std::atomic<bool> m_accepting{true};
    bool m_attached = true;
    bool m_target_persistence = false;
    bool m_inflight_overlay_only = false;
    const char* m_pump_phase = "all";
    uint32_t m_host_frame_current = 0;
    bool m_frame_trace_active = false;
    uint32_t m_frame_trace_host = 0;
    bool m_frame_trace_clears = false;
    uint64_t m_frame_trace_first_list = 0;
    uint64_t m_frame_trace_last_list = 0;
    uint32_t m_frame_trace_lists = 0;
    uint64_t m_frame_trace_triangles = 0;
    uint64_t m_frame_trace_substantial = 0;
    uint32_t m_ring_audit_consumed = 0;
    uint32_t m_ring_audit_logged = 0;
    bool m_ring_audit_cap_reported = false;
};

}  // namespace m2host

// Strong definition lives in the host executable.  The recomp runtime uses a
// weak declaration so standalone guest probes remain linkable.
extern "C" void m2_guest_graphics_submit_c(
    uint32_t io_item, uint32_t command, uint32_t command_options,
    uint32_t guest_buffer, uint32_t byte_count,
    const uint8_t* guest_bytes,
    const uint8_t* guest_memory, uint32_t guest_memory_size);

extern "C" void m2_guest_graphics_state_c(
    uint32_t io_item, uint32_t command, uint32_t item,
    uint32_t view_item, uint32_t framebuffer_item, uint32_t zbuffer_item,
    int32_t view_left, int32_t view_top, int32_t view_width,
    int32_t view_height, int32_t display_width, int32_t display_height);

extern "C" void m2_guest_graphics_te_surface_c(
    uint32_t db_super_control, uint32_t db_fb_clip,
    uint32_t db_dest_control, uint32_t db_dest_base,
    uint32_t db_dest_x_stride, uint32_t db_z_base, uint32_t db_z_clip,
    uint32_t z_fill_word, uint32_t fb_clear_red_word,
    uint32_t fb_clear_green_word, uint32_t fb_clear_blue_word,
    uint32_t fb_clear_alpha_word);

extern "C" void m2_guest_graphics_bitmap_buffer_c(
    uint32_t item, uint32_t buffer, uint32_t size);

extern "C" void m2_guest_graphics_display_c(
    uint32_t guest_frame, uint32_t target);

// Optional runtime accessor: the guest StateDriver application-frame counter
// at the moment a submission is copied.  The recomp runtime supplies the
// strong definition; guest_clt.cpp carries a weak zero fallback so standalone
// parser tests submit untagged (frame 0) and pump everything.
extern "C" uint32_t m2_guest_current_frame_c(void);

// Optional runtime accessor: the last guest application frame that reached a
// StateDriver loop head, i.e. the boundary below which a submission belongs to
// a completed frame. Only the transaction trace reads it, to stamp each
// executed list with the boundary in force *at execution time*. Without that
// stamp the boundary can only be approximated from the presenter iteration,
// which is wrong while a diagnostic drain keeps running the guest (the
// completed counter advances inside the drain). The weak fallback reports
// unbounded, matching the pump-everything behavior of standalone parser tests.
extern "C" uint32_t m2_guest_frame_pacing_completed(void);
