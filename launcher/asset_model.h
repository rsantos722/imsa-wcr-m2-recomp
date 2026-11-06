// Host-only car-model reader for the launcher's Asset Viewer tab.
//
// ImGui-free and SDL-free so the memimage -> mesh path can be exercised
// headlessly by `m2_launcher_assets`.
//
// Ground truth for this decoder ("M2 SDK" paths are relative to ws_root in the
// archived Portfolio OS reference; see docs/README.md):
//   * `.memimage` container: docs/formats/memimage.md (u32 payload size, then
//     the dumped payload, then an unused trailer).
//   * POD record and `PodGeometry`: the Mercury SDK declares both
//     (`M2 SDK src/includes/mercury/mercury.h`); `M_Sort`'s five-word key
//     independently confirms the game's POD field order.
//   * The index half-word flags and the strip/fan slot rules: the Mercury
//     Programmer's Guide (MER-16..MER-20), the model compiler that writes the
//     stream, `M2 SDK src/tools/mercury/gmerc/geo/tmutils.c`
//     (`#define PCLK/PFAN/CFAN/NEWS/STXT`), and the runtime walker that consumes
//     it, `M2 SDK src/libs/mercury/libmercury3/M_BuildAAEdgeTable.c`
//     (`walk_index_array`). The walker and the guide's worked cube example
//     agree; `asset_model.cpp` follows the walker.
//   * `PodGeometry::puv`/`PodTexture` fields: `includes/mercury/mercury.h`.
//     The draw routines consume `puv` sequentially for each index occurrence,
//     and that the `ptexture`/`ppip` pointers land in an `M2TX` bundle's
//     `M2TD` texel data and `M2PI` palette (wrapped in a `FORM TXTR` or loose in
//     the payload), which `asset_texture.h` decodes. An `STXT` number selects
//     a `TpageSnippets` entry; its CLT data pointer is joined to `PCLT`'s
//     `TexCLTOff` table before it is converted to an M2PG page index.
#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "asset_texture.h"

namespace launcher_assets {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Vec2 {
    float u = 0, v = 0;
};

// Mercury Matrix: three basis rows followed by translation (row vectors).
struct Transform {
    std::array<float, 12> m{1,0,0, 0,1,0, 0,0,1, 0,0,0};
    Vec3 point(const Vec3& p) const {
        return {p.x*m[0] + p.y*m[3] + p.z*m[6] + m[9],
                p.x*m[1] + p.y*m[4] + p.z*m[7] + m[10],
                p.x*m[2] + p.y*m[5] + p.z*m[8] + m[11]};
    }
};

// One POD's geometry expanded into a triangle list. `indices` holds three
// entries per triangle, and `positions`/`normals` are the authored per-vertex
// arrays (24 bytes per vertex: three position floats then the vendor's
// case-dependent lighting triplet; see `vertex_field_role` below).
struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<uint32_t> indices;
    Vec3 bbox_min{}, bbox_max{};
};

// Mercury's 24-byte vertex stride is shared by two shipped draw cases. The
// dynamic-lit routines consume the second triplet as nx/ny/nz; the pre-lit
// routines consume it as RGB. The POD record's relocated pcase callback is
// not enough to classify a dumped asset without executing the guest dispatch,
// so the loader exposes the bounded role instead of claiming a universal
// normal interpretation.
enum class VertexFieldRole { DynamicLitNormalOrPreLitColor };
constexpr VertexFieldRole kVertexFieldRole =
    VertexFieldRole::DynamicLitNormalOrPreLitColor;

// One POD chain (the game's render list for one model group). The chain head is
// the POD no other POD points at through `pnext`, and `pod_count` counts the
// records reached by following `pnext`.
// `Part::triangle_texture` holds one entry per triangle: an index into
// `Part::textures`, or `kNoTexture` when that triangle has no decoded texture
// (no texture on the POD, unusable UVs, or a page this decoder refused).
constexpr uint32_t kNoTexture = 0xffffffffu;

struct Part {
    std::string label;
    std::string component = "chain";  // body, damage, wheel, or loose chain
    int body = -1, variant = -1, lod = -1;  // race pack selection; -1 = other
    uint32_t group_offset = 0;
    uint32_t chain_head = 0;
    uint32_t pod_count = 0;
    uint32_t vertex_count = 0;
    uint32_t triangle_count = 0;
    Mesh mesh;
    std::vector<Vec2> uvs;                   // per corner, parallel to mesh.indices
    std::vector<TextureImage> textures;      // decoded pages this part draws
    std::vector<uint32_t> triangle_texture;  // one per triangle
    std::vector<uint8_t> triangle_clockwise; // next token's PCLK, before RH export
    // POD callback/material pointers are retained for provenance. `ptexture`
    // is resolved through PCLT/M2PG; the guest-specific `pmaterial` object is
    // intentionally not reinterpreted by this host-only exporter.
    std::vector<uint32_t> pod_pcase;
    std::vector<uint32_t> pod_offsets;
    std::vector<uint32_t> pod_ptexture;
    std::vector<uint32_t> pod_pmaterial;
};

struct Model {
    std::string name;         // literal in-code asset name (the file stem)
    bool race_pack = false;
    std::array<Transform, 4> wheel_transforms{}; // authored showroom mounts
    int wheel_count = 0;
    std::vector<Part> parts;  // POD chains, ordered by chain-head offset
    int default_part = -1;    // chain with the most PODs
    int rejected_pods = 0;    // records whose geometry failed validation
    // Texture accounting for the viewer's status line. A POD whose UVs or
    // texture bundle did not decode is counted, not silently guessed at.
    int texture_pages_decoded = 0;
    int texture_pages_failed = 0;
    int untextured_pods = 0;
    std::string texture_error;  // first texture-layer reason, for the UI
};

// A car asset the Asset Viewer offers. Showroom entries carry friendly
// chassis names; the file mapping stays in `relative_path`.
struct AssetChoice {
    const char* label;          // e.g. "showroom.1"
    const char* relative_path;  // relative to the extracted data root
};

// Four separate showroom car assets. Race body variants are the three paint
// selections within each car pack's TheCarBodies table, exposed on Part.
extern const AssetChoice kShowroomChassisChoices[];
extern const int kShowroomChassisChoiceCount;
// The four authored tire variants (data/showroom).
extern const AssetChoice kShowroomTireChoices[];
extern const int kShowroomTireChoiceCount;
extern const AssetChoice kRaceCarChoices[];
extern const int kRaceCarChoiceCount;

// Decode `path` into a Model. Returns false and sets `error` on a container or
// bounds violation; a POD whose geometry does not satisfy the documented
// layout is rejected and counted in `Model::rejected_pods` rather than
// guessed at. `name` is stored verbatim (the viewer passes the asset label).
bool load_model(const std::filesystem::path& path, const std::string& name,
                Model& out, std::string& error);

}  // namespace launcher_assets
