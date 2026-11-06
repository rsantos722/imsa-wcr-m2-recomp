// Focused checks for the Asset Viewer's memimage car-model decoder.
//
// The synthetic fixture below is a self-contained MEMIMAGE built from the
// documented layout (payload size word, POD record, PodGeometry, 24-byte
// vertices, Mercury index half-words), so the decoder is exercised without the
// original disc. When IMSA_DATA_DIR names the user's extracted game data
// (CTest sets it in a game-enabled build) the test additionally asserts
// documented guest identities:
//
//   * showroom.1's four chains, of which the 19-POD car group is the default
//     (the showroom's car/room/floor POD split);
//   * the showroom floor POD named there: 1,296 vertices with bounds
//     `(-500,-31.4,-500) + (1000,0,1000)`;
//   * the car chain's textures: UV pairs follow index occurrences and preserve
//     seams per triangle corner; STXT pages decode through the POD texture
//     bundle, and the tire's UV-less POD stays flat;
//   * cars1-4's body/paint/LOD tables, including contiguous POD groups whose
//     serialized links still point into paint variant 0.
#include "asset_model.h"
#include "asset_viewport.h"
#include "asset_raster.h"
#include "test_scratch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using launcher_assets::Model;
using launcher_assets::Vec3;

constexpr uint16_t kStxt = 0x0800;
constexpr uint16_t kNews = 0x1000;
constexpr uint16_t kCfan = 0x2000;
constexpr uint16_t kPclk = 0x8000;

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    d[at] = uint8_t(v >> 24);
    d[at + 1] = uint8_t(v >> 16);
    d[at + 2] = uint8_t(v >> 8);
    d[at + 3] = uint8_t(v);
}
void put16(std::vector<uint8_t>& d, size_t at, uint16_t v) {
    d[at] = uint8_t(v >> 8);
    d[at + 1] = uint8_t(v);
}
void putfloat(std::vector<uint8_t>& d, size_t at, float v) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "float is 32-bit here");
    std::memcpy(&bits, &v, sizeof(bits));
    put32(d, at, bits);
}

// A one-or-two POD MEMIMAGE: a unit quad in the XY plane (four vertices, normal
// +Z) plus the Mercury index stream `[STXT|0][texture][1][2][3][end][-1]`.
// `second_pod_bad_index` appends a second POD in the same chain whose index
// references a vertex outside its geometry.
//
// The stream is a plain strip, so it pins the slot walk without a fan: the two
// triangles must come out with the same winding as the authored +Z normal.
std::vector<uint8_t> build_fixture(bool second_pod_bad_index) {
    constexpr size_t kPodA = 0x10, kPodB = 0x34;
    constexpr size_t kGeomA = 0x58, kGeomB = 0x118;
    constexpr size_t kVertA = 0x88, kVertB = 0x148;
    constexpr size_t kIdxA = 0xE8, kIdxB = 0x1A8;
    constexpr uint32_t kPayload = 0x1E0;

    std::vector<uint8_t> payload(kPayload, 0);
    auto pod = [&](size_t at, uint32_t pnext, uint32_t geometry) {
        put32(payload, at + 0x00, 0);              // flags
        put32(payload, at + 0x04, pnext);          // pnext
        put32(payload, at + 0x08, 1);              // pcase
        put32(payload, at + 0x0c, 0);              // ptexture
        put32(payload, at + 0x10, geometry);       // pgeometry
        put32(payload, at + 0x14, 0);              // pmatrix
        put32(payload, at + 0x18, 0);              // plights
        put32(payload, at + 0x1c, 0);              // puserdata
        put32(payload, at + 0x20, 0);              // pmaterial
    };
    auto geometry = [&](size_t at, size_t vertex_at, size_t index_at) {
        putfloat(payload, at + 0, 0.0f);           // fxmin
        putfloat(payload, at + 4, 0.0f);           // fymin
        putfloat(payload, at + 8, 0.0f);           // fzmin
        putfloat(payload, at + 12, 1.0f);          // fxextent
        putfloat(payload, at + 16, 1.0f);          // fyextent
        putfloat(payload, at + 20, 0.0f);          // fzextent
        put32(payload, at + 24, uint32_t(vertex_at));   // pvertex
        put32(payload, at + 28, 0);                // pshared
        put16(payload, at + 32, 4);                // vertexcount
        put16(payload, at + 34, 0);                // sharedcount
        put32(payload, at + 36, uint32_t(index_at));    // pindex
        put32(payload, at + 40, uint32_t(index_at + 16));  // puv
        put32(payload, at + 44, 0);                // paaedge
    };
    auto vertices = [&](size_t at) {
        const float quad[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        for (int i = 0; i < 4; ++i) {
            putfloat(payload, at + size_t(i) * 24 + 0, quad[i][0]);
            putfloat(payload, at + size_t(i) * 24 + 4, quad[i][1]);
            putfloat(payload, at + size_t(i) * 24 + 8, quad[i][2]);
            putfloat(payload, at + size_t(i) * 24 + 12, 0.0f);
            putfloat(payload, at + size_t(i) * 24 + 16, 0.0f);
            putfloat(payload, at + size_t(i) * 24 + 20, 1.0f);
        }
    };
    auto indices = [&](size_t at, uint16_t last) {
        put16(payload, at + 0, uint16_t(kStxt | 0));   // first index + texture
        put16(payload, at + 2, 0);                     // texture number
        put16(payload, at + 4, 1);
        put16(payload, at + 6, 2);
        put16(payload, at + 8, last);
        put16(payload, at + 10, uint16_t(kStxt | kNews | kCfan));  // end marker
        put16(payload, at + 12, 0xffff);               // -1 terminates the object
        put16(payload, at + 14, 0);
    };

    pod(kPodA, second_pod_bad_index ? uint32_t(kPodB) : 0, uint32_t(kGeomA));
    geometry(kGeomA, kVertA, kIdxA);
    vertices(kVertA);
    indices(kIdxA, 3);
    if (second_pod_bad_index) {
        pod(kPodB, 0, uint32_t(kGeomB));
        geometry(kGeomB, kVertB, kIdxB);
        vertices(kVertB);
        indices(kIdxB, 0x7ff);  // out of range: this POD must be rejected
    }

    std::vector<uint8_t> file;
    file.resize(4);
    put32(file, 0, kPayload);
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

// The Mercury Programmer's Guide's worked cube example (MER-14..MER-16, the
// `facetcubeindex` array) as a one-POD MEMIMAGE. The token stream is verbatim:
//
//   [STXT|PCLK] 0, 2, 10, 12, 14                     /* -x */
//   [NEWS|CFAN|PCLK] 8, 16, 1, 19                    /* -y */
//   [NEWS|CFAN|PCLK] 9, 2, 17, 21                    /* -z */
//   [NEWS|CFAN|PCLK] 13, 3, 20, 22                   /* +y */
//   [NEWS|CFAN|PCLK] 6, 7, 4, 18                     /* +x */
//   [NEWS|CFAN|PCLK] 11, 5, 15, 23                   /* +z */
//   [STXT|NEWS|CFAN|PCLK] 0, -1
//
// (the 2 after STXT is the texture number, so it is not a vertex). Each face is
// a four-vertex strip, so every face must decode to the two triangles the
// guide's shared-vertex table assigns it - a NEWS/STXT/CFAN boundary case the
// synthetic quad above does not reach. The guide builds the cube with the
// shared-vertex mechanism, which this decoder deliberately refuses, so the
// fixture keeps those 24 slots as plain vertices 0..23 in their authored order.

// The 24 slots of the guide's cube, in index order: its eight vertices followed
// by the sixteen (vertex, color) shared entries.
const float kCubeSlot[24][3] = {
    {0, 0, 0},       {0, 0, 100},     {0, 100, 0},   {0, 100, 100},
    {100, 0, 0},     {100, 0, 100},   {100, 100, 0}, {100, 100, 100},
    {0, 0, 0},       {0, 0, 0},       {0, 0, 100},   {0, 0, 100},
    {0, 100, 0},     {0, 100, 0},     {0, 100, 100}, {0, 100, 100},
    {100, 0, 0},     {100, 0, 0},     {100, 0, 100}, {100, 0, 100},
    {100, 100, 0},   {100, 100, 0},   {100, 100, 100}, {100, 100, 100},
};

// The twelve triangles the guide's table documents, as sorted vertex triples.
const uint32_t kCubeTriangles[12][3] = {
    {0, 10, 12}, {10, 12, 14}, {1, 8, 16},  {1, 16, 19},
    {2, 9, 17},  {2, 17, 21},  {3, 13, 20}, {3, 20, 22},
    {4, 6, 7},   {4, 7, 18},   {5, 11, 15}, {5, 15, 23},
};

std::vector<uint8_t> build_cube_fixture() {
    constexpr size_t kPod = 0x10, kGeom = 0x34, kVert = 0x64;
    constexpr size_t kIdx = kVert + 24 * 24;  // 0x2A4
    constexpr size_t kUv = kIdx + 56;         // 28 index words, 4-byte aligned
    constexpr uint32_t kPayload = uint32_t(kUv + 24 * 8);

    std::vector<uint8_t> payload(kPayload, 0);
    put32(payload, kPod + 0x04, 0);            // pnext: end of chain
    put32(payload, kPod + 0x08, 1);            // pcase
    put32(payload, kPod + 0x0c, 0);            // ptexture: flat, as the guide's
    put32(payload, kPod + 0x10, uint32_t(kGeom));
    putfloat(payload, kGeom + 0, 0.0f);
    putfloat(payload, kGeom + 4, 0.0f);
    putfloat(payload, kGeom + 8, 0.0f);
    putfloat(payload, kGeom + 12, 100.0f);
    putfloat(payload, kGeom + 16, 100.0f);
    putfloat(payload, kGeom + 20, 100.0f);
    put32(payload, kGeom + 24, uint32_t(kVert));
    put16(payload, kGeom + 32, 24);            // vertexcount
    put32(payload, kGeom + 36, uint32_t(kIdx));
    put32(payload, kGeom + 40, uint32_t(kUv));
    for (int i = 0; i < 24; ++i) {
        for (int k = 0; k < 3; ++k)
            putfloat(payload, kVert + size_t(i) * 24 + size_t(k) * 4,
                     kCubeSlot[i][k]);
        putfloat(payload, kVert + size_t(i) * 24 + 12, 0.0f);
        putfloat(payload, kVert + size_t(i) * 24 + 16, 0.0f);
        putfloat(payload, kVert + size_t(i) * 24 + 20, 0.0f);
    }
    const uint16_t stream[26] = {
        uint16_t(kStxt | kPclk), 2, 10, 12, 14,
        uint16_t(8 | kNews | kCfan | kPclk), 16, 1, 19,
        uint16_t(9 | kNews | kCfan | kPclk), 2, 17, 21,
        uint16_t(13 | kNews | kCfan | kPclk), 3, 20, 22,
        uint16_t(6 | kNews | kCfan | kPclk), 7, 4, 18,
        uint16_t(11 | kNews | kCfan | kPclk), 5, 15, 23,
        uint16_t(kStxt | kNews | kCfan | kPclk),
    };
    for (size_t i = 0; i < 26; ++i) put16(payload, kIdx + i * 2, stream[i]);
    put16(payload, kIdx + 52, 0xffff);         // -1 terminates the object

    std::vector<uint8_t> file;
    file.resize(4);
    put32(file, 0, kPayload);
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

// A one-POD MEMIMAGE whose POD carries a `ptexture` record pointing into a
// small loose M2TX bundle: a 2x2 4bpp indexed page with four M2PI colors, laid
// out exactly as the showroom files are (the record's `ptexture` lands on the
// first LOD0 texel byte, `ppip` on the first M2PI color). `TextureMode::
// SmallPalette` keeps a single palette entry so the authored texel indices run
// out of range.
enum class TextureMode { Good, SmallPalette, SwappedPageOrder, UvSeams, PaletteOffset };

std::vector<uint8_t> build_textured_fixture(TextureMode mode) {
    constexpr size_t kPod = 0x10, kGeom = 0x40, kVert = 0x70, kIdx = 0xD0,
                     kTexRecord = 0x130, kM2tx = 0x150;
    const bool two_pages = mode == TextureMode::SwappedPageOrder ||
                           mode == TextureMode::UvSeams;
    const size_t kUv = kIdx + (two_pages ? 24 : 16);
    const size_t palette_entries = mode == TextureMode::SmallPalette ? 1
                                  : mode == TextureMode::PaletteOffset ? 5 : 4;
    const size_t m2pi_payload = 8 + 4 * palette_entries;
    const size_t m2ta = kM2tx + 24 + 8 + m2pi_payload;  // M2TX chunk is 24 B
    const size_t m2pg = m2ta + 12;      // M2TA payload: one reserved word
    const size_t m2pg_size = two_pages ? 48 : 28;
    const size_t m2td = m2pg + 8 + m2pg_size;
    const size_t m2td_payload = m2td + 8;
    const size_t kPclt = m2td + (two_pages ? 24 : 20);
    const size_t pclt_size = two_pages ? 0x30 : 0x28;
    const size_t kTpage = kPclt + 8 + pclt_size;
    const size_t kLoadSnippets = kTpage + 12;
    const size_t kSelectSnippets = kLoadSnippets + 16;
    const uint32_t kPayload =
        uint32_t(kSelectSnippets + (two_pages ? 32 : 16));

    std::vector<uint8_t> payload(kPayload, 0);
    auto chunk = [&](size_t at, const char* magic, uint32_t size) {
        std::memcpy(&payload[at], magic, 4);
        put32(payload, at + 4, size);
    };

    put32(payload, kPod + 0x00, 0);                  // flags
    put32(payload, kPod + 0x04, 0);                  // pnext (end of chain)
    put32(payload, kPod + 0x08, 1);                  // pcase
    put32(payload, kPod + 0x0c, uint32_t(kTexRecord));  // ptexture
    put32(payload, kPod + 0x10, uint32_t(kGeom));    // pgeometry
    put32(payload, kPod + 0x14, 0);                  // pmatrix
    put32(payload, kPod + 0x18, 0);                  // plights
    put32(payload, kPod + 0x1c, 0);                  // puserdata
    put32(payload, kPod + 0x20, 0);                  // pmaterial

    putfloat(payload, kGeom + 0, 0.0f);
    putfloat(payload, kGeom + 4, 0.0f);
    putfloat(payload, kGeom + 8, 0.0f);
    putfloat(payload, kGeom + 12, 1.0f);
    putfloat(payload, kGeom + 16, 1.0f);
    putfloat(payload, kGeom + 20, 0.0f);
    put32(payload, kGeom + 24, uint32_t(kVert));     // pvertex
    put32(payload, kGeom + 28, 0);                   // pshared
    put16(payload, kGeom + 32, 4);                   // vertexcount
    put16(payload, kGeom + 34, 0);                   // sharedcount
    put32(payload, kGeom + 36, uint32_t(kIdx));      // pindex
    put32(payload, kGeom + 40, uint32_t(kUv));       // puv
    put32(payload, kGeom + 44, 0);                   // paaedge

    const float quad[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (int i = 0; i < 4; ++i) {
        putfloat(payload, kVert + size_t(i) * 24 + 0, quad[i][0]);
        putfloat(payload, kVert + size_t(i) * 24 + 4, quad[i][1]);
        putfloat(payload, kVert + size_t(i) * 24 + 8, quad[i][2]);
        putfloat(payload, kVert + size_t(i) * 24 + 20, 1.0f);
    }
    put16(payload, kIdx + 0, uint16_t(kStxt | 0));   // strip start + texture 0
    put16(payload, kIdx + 2, 0);                     // texture number
    put16(payload, kIdx + 4, 1);
    put16(payload, kIdx + 6, 2);
    if (two_pages) {
        // The first boundary token starts the next prim and names page 1;
        // the final STXT/NEWS token carries -1, matching the vendor end
        // marker while leaving both triangles in the stream.
        put16(payload, kIdx + 8, uint16_t(kStxt | kNews | kCfan));
        put16(payload, kIdx + 10, 1);                // texture number
        put16(payload, kIdx + 12, 1);
        put16(payload, kIdx + 14, 2);
        put16(payload, kIdx + 16, uint16_t(kStxt | kNews | kCfan));
        put16(payload, kIdx + 18, 0xffff);           // -1 ends the object
    } else {
        put16(payload, kIdx + 8, 3);
        put16(payload, kIdx + 10, uint16_t(kStxt | kNews | kCfan));
        put16(payload, kIdx + 12, 0xffff);            // -1 ends the object
    }
    const float uv[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1},
                            {0.25f, 0.5f}, {0.75f, 0.25f}};
    for (int i = 0; i < (two_pages ? 6 : 4); ++i) {
        putfloat(payload, kUv + size_t(i) * 8 + 0, uv[i][0]);
        putfloat(payload, kUv + size_t(i) * 8 + 4, uv[i][1]);
    }
    if (mode == TextureMode::UvSeams) {
        put16(payload, kIdx, uint16_t(kStxt | 3));
        put16(payload, kIdx + 8, uint16_t(kStxt | kNews | kCfan | 3));
        put16(payload, kIdx + 12, 2);
        put16(payload, kIdx + 14, 0);
    }

    put32(payload, kTexRecord + 0x00, 0);            // proutine
    put32(payload, kTexRecord + 0x04, uint32_t(kTpage));  // ptpagesnippets
    put32(payload, kTexRecord + 0x08, uint32_t(m2td_payload + 8));  // ptexture
    put32(payload, kTexRecord + 0x0c, 4);            // texturecount (texels)
    put32(payload, kTexRecord + 0x10, 4);            // texturebytes
    put32(payload, kTexRecord + 0x14, uint32_t(kM2tx + 24 + 16));   // ppip
    put32(payload, kTexRecord + 0x18, uint32_t(4 * palette_entries));

    // M2TX: version, flags (uncompressed), 2x2, 4bpp indexed, one LOD.
    chunk(kM2tx, "M2TX", 16);
    put32(payload, kM2tx + 8, 0);
    put32(payload, kM2tx + 12, 0);
    put16(payload, kM2tx + 16, 2);
    put16(payload, kM2tx + 18, 2);
    put16(payload, kM2tx + 20, 0x0404);
    payload[kM2tx + 22] = 1;
    // M2PI: IndexOffset, reserved, then [SSB:1][A:7][RGB:8] colors. The last
    // entry carries a 7-bit alpha of 1 to pin the (a << 1) | (a & 1) expansion.
    const uint32_t colors[4] = {0x7fff0000u, 0x7f00ff00u, 0x7f0000ffu,
                                0x01ffffffu};
    chunk(kM2tx + 24, "M2PI", uint32_t(m2pi_payload));
    for (size_t i = 0; i < palette_entries; ++i)
        put32(payload, kM2tx + 24 + 16 + 4 * i,
              mode == TextureMode::PaletteOffset
                  ? (i ? colors[i - 1] : 0x7f000000u) : colors[i]);
    chunk(m2ta, "M2TA", 4);
    put32(payload, m2ta + 8, 0);                      // reserved
    // M2PG: one or two pages at 2x2, 4bpp, one LOD.
    chunk(m2pg, "M2PG", uint32_t(m2pg_size));
    put32(payload, m2pg + 8, two_pages ? 2 : 1);      // NumTex
    put32(payload, m2pg + 12, 0);                     // version
    put32(payload, m2pg + 16, 0);                     // page offset
    put32(payload, m2pg + 20, 0x00000004u);           // M2PG_HasTexFormat
    put16(payload, m2pg + 24, 2);
    put16(payload, m2pg + 26, 2);
    put16(payload, m2pg + 28, 0x0404);
    payload[m2pg + 30] = 1;                           // NumLOD
    payload[m2pg + 31] = mode == TextureMode::PaletteOffset ? 1 : 0;
    if (two_pages) {
        const size_t page = m2pg + 36;
        put32(payload, page + 0, 4);                  // second LOD0 offset
        put32(payload, page + 4, 0x00000004u);
        put16(payload, page + 8, 2);
        put16(payload, page + 10, 2);
        put16(payload, page + 12, 0x0404);
        payload[page + 14] = 1;
    }
    // M2TD: one LOD starting after the 8-byte header; 2x2 at 4bpp is one
    // nibble per texel, packed MSB first.
    chunk(m2td, "M2TD", uint32_t(two_pages ? 16 : 12));
    put16(payload, m2td + 8, 1);
    put32(payload, m2td + 12, 8);
    payload[m2td_payload + 8] = 0x01;
    payload[m2td_payload + 9] = 0x23;
    if (two_pages) {
        payload[m2td_payload + 12] = 0x32;
        payload[m2td_payload + 13] = 0x10;
    }

    // PCLT v0 carries the page, PIP, and per-page CLT segment offsets. The
    // select snippet below points at the last segment, exactly as the loader
    // patches TpageSnippets from PCLT's TexCLTOff table.
    chunk(kPclt, "PCLT", uint32_t(pclt_size));
    put32(payload, kPclt + 8, two_pages ? 2 : 1);    // NumTex
    put32(payload, kPclt + 12, 0);                   // Version
    put32(payload, kPclt + 16, 0x1c);                // PageCLTOff
    put32(payload, kPclt + 20, 0x20);                // PIPCLTOff
    put32(payload, kPclt + 24, two_pages ? 0x28 : 0x24);  // TexCLTOff[0]
    if (two_pages) put32(payload, kPclt + 28, 0x2c); // TexCLTOff[1]
    put32(payload, kTpage + 0, 2);                   // load snippets
    put32(payload, kTpage + 4, uint32_t(kLoadSnippets));
    put32(payload, kTpage + 8, uint32_t(kSelectSnippets));
    const size_t page0_offset = two_pages ? 0x28 : 0x24;
    const size_t page1_offset = 0x2c;
    const size_t select0 = kSelectSnippets;
    const size_t select1 = kSelectSnippets + 16;
    put32(payload, select0 + 0,
          uint32_t(kPclt + 8 +
                   (two_pages && mode == TextureMode::SwappedPageOrder
                        ? page1_offset
                        : page0_offset)));
    put16(payload, select0 + 4, 1);                  // one CLT word
    put16(payload, select0 + 6, 1);
    putfloat(payload, select0 + 8, 2.0f);            // uscale
    putfloat(payload, select0 + 12, 2.0f);           // vscale
    if (two_pages) {
        put32(payload, select1 + 0,
              uint32_t(kPclt + 8 + (mode == TextureMode::SwappedPageOrder
                                        ? page0_offset
                                        : page1_offset)));
        put16(payload, select1 + 4, 1);
        put16(payload, select1 + 6, 1);
        putfloat(payload, select1 + 8, 2.0f);
        putfloat(payload, select1 + 12, 2.0f);
    }

    std::vector<uint8_t> file;
    file.resize(4);
    put32(file, 0, kPayload);
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              std::streamsize(bytes.size()));
    return bool(out);
}

int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Bounds {
    bool ok = false;
    Vec3 lo{}, hi{};
    bool matches(Vec3 expected_lo, Vec3 expected_hi, float tolerance) const {
        if (!ok) return false;
        return std::fabs(lo.x - expected_lo.x) <= tolerance &&
               std::fabs(lo.y - expected_lo.y) <= tolerance &&
               std::fabs(lo.z - expected_lo.z) <= tolerance &&
               std::fabs(hi.x - expected_hi.x) <= tolerance &&
               std::fabs(hi.y - expected_hi.y) <= tolerance &&
               std::fabs(hi.z - expected_hi.z) <= tolerance;
    }
};

Bounds find_bounds(const Model& model, Vec3 expected_lo, Vec3 expected_hi,
                   float tolerance) {
    for (const auto& part : model.parts)
        if (Bounds{!part.mesh.positions.empty(), part.mesh.bbox_min,
                   part.mesh.bbox_max}
                .matches(expected_lo, expected_hi, tolerance))
            return {true, part.mesh.bbox_min, part.mesh.bbox_max};
    return {};
}

// The fitted camera must keep the whole box inside the panel at any orbit
// angle, or the viewer would show a clipped or empty model.
void check_framing(const launcher_assets::Mesh& mesh, const char* label) {
    constexpr float kPanelWidth = 600.0f, kPanelHeight = 300.0f;
    for (const float yaw : {0.0f, 0.62f, 1.6f, 3.1f}) {
        for (const float pitch : {-1.4f, 0.0f, 0.26f, 1.4f}) {
            const auto camera = launcher_assets::fit_camera(
                mesh.bbox_min, mesh.bbox_max, kPanelWidth, kPanelHeight, 1.0f);
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 point{corner & 1 ? mesh.bbox_max.x : mesh.bbox_min.x,
                                 corner & 2 ? mesh.bbox_max.y : mesh.bbox_min.y,
                                 corner & 4 ? mesh.bbox_max.z : mesh.bbox_min.z};
                float x = 0, y = 0, depth = 0;
                launcher_assets::project(camera, point, yaw, pitch, x, y, depth);
                if (!(depth > 0)) {
                    check(false, "a bbox corner fell behind the fitted camera");
                    return;
                }
                if (x < -2.0f || x > kPanelWidth + 2.0f || y < -2.0f ||
                    y > kPanelHeight + 2.0f) {
                    std::fprintf(stderr, "FAIL: %s is clipped at yaw %.2f pitch %.2f\n",
                                 label, double(yaw), double(pitch));
                    ++failures;
                    return;
                }
            }
        }
    }
}

// Fraction of the decoded chain's edges shared by three or more triangles. A
// correctly assembled Mercury index stream is a manifold surface, so every edge
// belongs to one or two triangles; the strip-only walk this decoder replaced
// joined unrelated vertices and left several percent of the edges non-manifold.
// This is the asset-backed property that fails if the slot/fan rule regresses.
double nonmanifold_edge_fraction(const launcher_assets::Part& part) {
    std::unordered_map<uint64_t, int> edges;
    const auto& indices = part.mesh.indices;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = indices[i + size_t(k)];
            const uint32_t b = indices[i + size_t((k + 1) % 3)];
            const uint64_t key =
                (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
            ++edges[key];
        }
    }
    if (edges.empty()) return 0.0;
    size_t nonmanifold = 0;
    for (const auto& edge : edges)
        if (edge.second > 2) ++nonmanifold;
    return double(nonmanifold) / double(edges.size());
}

bool every_triangle_is_sane(const Model& model) {
    for (const auto& part : model.parts) {
        const size_t vertices = part.mesh.positions.size();
        if (part.mesh.indices.size() != size_t(part.triangle_count) * 3) return false;
        for (size_t i = 0; i + 2 < part.mesh.indices.size(); i += 3) {
            const uint32_t a = part.mesh.indices[i], b = part.mesh.indices[i + 1],
                           c = part.mesh.indices[i + 2];
            if (a >= vertices || b >= vertices || c >= vertices) return false;
            if (a == b || b == c || a == c) return false;  // no degenerate triangle
        }
    }
    return true;
}

// Independent visibility expectations: two crossing surfaces cannot be sorted
// by one triangle depth. Both submission orders and both windings must agree.
void check_raster() {
    using namespace launcher_assets;
    Camera camera;
    camera.cx = camera.cy = 16; camera.focal = 16; camera.radius = 1;
    auto vertex = [&](float x, float y, float z, float u = 0) {
        return PreviewVertex{{(16-x)*z/16,(16-y)*z/16,z},{u,0},1};
    };
    const TextureImage red{1,1,{255,0,0,255},false};
    const TextureImage blue{1,1,{0,0,255,255},false};
    PreviewVertex sloped[3]{vertex(2,2,1),vertex(30,2,8),vertex(16,30,8)};
    PreviewVertex flat[3]{vertex(2,2,4),vertex(30,2,4),vertex(16,30,4)};
    PreviewRaster image;
    image.reset(32,32);
    image.triangle(camera,sloped,&red);
    image.triangle(camera,flat,&blue);
    const auto first = image.rgba;
    auto pixel = [&](int x, int y, int channel) { return image.rgba[(y*32+x)*4+channel]; };
    check(pixel(7,6,0) == 255 && pixel(25,6,2) == 255,
          "crossing triangles resolve depth per pixel, not by triangle average");
    image.reset(32,32);
    image.triangle(camera,flat,&blue);
    image.triangle(camera,sloped,&red);
    check(image.rgba == first,"opaque visibility is independent of submission order");
    image.reset(32,32);
    std::swap(sloped[1],sloped[2]); std::swap(flat[1],flat[2]);
    image.triangle(camera,sloped,&red); image.triangle(camera,flat,&blue);
    check(image.rgba == first,"preview keeps both face windings visible");

    // Same projected triangle: affine U at (16.5,6.5) exceeds 0.5, while
    // perspective U is below 0.5 because the red corner is eight times nearer.
    TextureImage stripe{2,1,{255,0,0,255, 0,0,255,255},false};
    PreviewVertex textured[3]{vertex(2,2,1,0),vertex(30,2,8,1),vertex(16,30,8,1)};
    image.reset(32,32); image.triangle(camera,textured,&stripe);
    check(pixel(16,6,0)==255 && pixel(16,6,2)==0,"UVs interpolate with perspective");

    PreviewVertex near[3]{vertex(2,2,2),vertex(30,2,2),vertex(16,30,2)};
    const TextureImage hole{1,1,{255,0,0,0},true};
    image.reset(32,32); image.triangle(camera,near,&hole);
    image.triangle(camera,flat,&blue);
    check(pixel(16,10,2)==255,"transparent cutouts do not write depth");
    const TextureImage glass{1,1,{255,0,0,128},true};
    image.reset(32,32); image.triangle(camera,flat,&blue);
    image.triangle(camera,near,&glass,false); image.triangle(camera,near,&glass,true);
    check(pixel(16,10,0)==128 && pixel(16,10,2)==127,
          "partial alpha blends over opaque geometry in the second pass");
    image.reset(32,32); image.triangle(camera,near,&blue);
    image.triangle(camera,flat,&glass,true);
    check(pixel(16,10,0)==0 && pixel(16,10,2)==255,"opaque depth occludes glass behind it");

    PreviewVertex crossing[3]{{{-0.8f,-0.8f,1},{},1},{{0.8f,-0.8f,1},{},1},{{0,0.8f,-0.5f},{},1}};
    image.reset(32,32); image.triangle(camera,crossing,&red);
    check(std::count_if(image.depth.begin(),image.depth.end(),[](float d){return d>0;})>100,
          "a triangle crossing the near plane is clipped and remains visible");
    for (auto& v : crossing) v.view.z = -1;
    image.reset(32,32); image.triangle(camera,crossing,&red);
    check(std::all_of(image.depth.begin(),image.depth.end(),[](float d){return d==0;}),
          "geometry entirely behind the eye is rejected");

    // The shared diagonal passes through pixel centres. Partial alpha must
    // cover each pixel exactly once, without a crack or double blend.
    PreviewVertex a[3]{vertex(4,4,2),vertex(28,4,2),vertex(28,28,2)};
    PreviewVertex b[3]{vertex(4,4,2),vertex(28,28,2),vertex(4,28,2)};
    image.reset(32,32); image.triangle(camera,a,&glass,true); image.triangle(camera,b,&glass,true);
    for (int y=4; y<28; ++y) for (int x=4; x<28; ++x)
        check(pixel(x,y,0)==135,"adjacent alpha triangles share edges without gaps or double blending");
}

}  // namespace

int main() {
    check_raster();
    const fs::path dir =
        fs::temp_directory_path() / ("m2-launcher-asset-model" + test_scratch_suffix());
    std::error_code ec;
    fs::create_directories(dir, ec);
    // The name is per-process, so remove the directory on every exit path.
    struct ScratchCleanup {
        fs::path path;
        ~ScratchCleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } scratch_cleanup{dir};
    const fs::path good = dir / "fixture.memimage";
    const fs::path partial = dir / "fixture-bad-pod.memimage";
    const fs::path truncated = dir / "fixture-truncated.memimage";

    // For a camera looking +Z with +Y up, positive world X lies to its left.
    // The +X screen basis mirrors the complete textured model.
    {
        const auto camera = launcher_assets::fit_camera({-1,-1,-1}, {1,1,1},
                                                        640, 480, 1);
        float x, y, depth;
        launcher_assets::project(camera, {1,0,0}, 0, 0, x, y, depth);
        check(x < camera.cx && y == camera.cy && depth > 0,
              "the camera basis preserves exterior livery orientation");
    }

    // --- Synthetic fixture -------------------------------------------------
    if (!write_file(good, build_fixture(false))) return 1;
    Model model;
    std::string error;
    check(launcher_assets::load_model(good, "fixture", model, error),
          "synthetic memimage should decode");
    check(model.name == "fixture", "the asset label is stored verbatim");
    check(model.parts.size() == 1, "one POD chain expected");
    check(model.default_part == 0, "the only chain is the default");
    check(model.wheel_count == 0,"loose assets do not invent wheel mounts");
    {
        // Independent MEMIMAGE layout: exported car descriptor at +0x200,
        // four matrix pointers at +0x30, including nonuniform scale and Y.
        auto bytes = build_fixture(false);
        bytes.resize(4+0x380+28,0);
        put32(bytes,0,0x380); put32(bytes,4+0x380,6); put32(bytes,8+0x380,0x200);
        for (unsigned i=0; i<4; ++i) {
            const unsigned matrix = 0x280+i*48;
            put32(bytes,4+0x230+i*4,matrix);
            const float values[12]{1,0,0, 0,0.75f,0, 0,0,0.5f, float(20+i),-4,137};
            for (unsigned j=0; j<12; ++j) putfloat(bytes,4+matrix+j*4,values[j]);
        }
        const auto path = dir/"showroom.0.memimage";
        Model fixture;
        check(write_file(path,bytes) && launcher_assets::load_model(path,"mount fixture",fixture,error),
              "showroom matrices decode from the exported descriptor");
        std::array<launcher_assets::Transform,4> placements;
        check(launcher_assets::wheel_placements(fixture,0,placements)==4,
              "decoded showroom matrices reach the viewport placement boundary");
        const auto point = placements[0].point({2,4,6});
        check(point.x==18 && point.y==-7 && point.z==140,
              "wheel placement preserves scale, height, and the guest's two-row flip");
        auto bad = bytes;
        put32(bad,4+0x230,0x37c);
        check(write_file(path,bad) && !launcher_assets::load_model(path,"bad matrix",fixture,error),
              "truncated wheel matrix is refused");
        bad = bytes; put32(bad,4+0x280,0x7fc00000);
        check(write_file(path,bad) && !launcher_assets::load_model(path,"NaN matrix",fixture,error),
              "nonfinite wheel matrix is refused");
        bad = bytes; bad.resize(4+0x380);
        check(write_file(path,bad) && !launcher_assets::load_model(path,"missing roots",fixture,error),
              "missing showroom root table is refused");
        fs::remove(path,ec);
    }
    if (model.parts.size() == 1) {
        const auto& part = model.parts[0];
        check(part.pod_count == 1, "chain holds one POD");
        check(part.vertex_count == 4, "four vertices");
        check(part.triangle_count == 2, "four strip indices give two triangles");
        check(part.mesh.positions.size() == 4 && part.mesh.normals.size() == 4,
              "position and normal arrays match the vertex count");
        check(part.mesh.indices.size() == 6, "six triangle indices");
        check(part.mesh.indices == std::vector<uint32_t>({0, 1, 2, 3, 1, 2}),
              "the strip walk replaces the oldest container and keeps the other two");
        // The authored normal at every corner is +Z, so a consistent winding
        // has to give every face normal the same sign as the quad's +Z normal.
        for (size_t t = 0; t + 2 < part.mesh.indices.size(); t += 3) {
            const Vec3& a = part.mesh.positions[part.mesh.indices[t]];
            const Vec3& b = part.mesh.positions[part.mesh.indices[t + 1]];
            const Vec3& c = part.mesh.positions[part.mesh.indices[t + 2]];
            const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z};
            const Vec3 v{c.x - a.x, c.y - a.y, c.z - a.z};
            const float nz = u.x * v.y - u.y * v.x;
            const Vec3& corner = part.mesh.normals[part.mesh.indices[t]];
            check(nz * corner.z > 0.0f,
                  "strip winding agrees with the authored vertex normal");
        }
        check(Bounds{true, part.mesh.bbox_min, part.mesh.bbox_max}
                  .matches({0, 0, 0}, {1, 1, 0}, 0.0001f),
              "unit quad bounds");
        check_framing(part.mesh, "fixture quad");
    }
    check(model.rejected_pods == 0, "no POD rejected in the clean fixture");

    // A POD with an out-of-range index is rejected and counted, and the chain
    // keeps its valid POD instead of dropping the model.
    if (!write_file(partial, build_fixture(true))) return 1;
    Model partial_model;
    check(launcher_assets::load_model(partial, "partial", partial_model, error),
          "a chain with one bad POD still decodes");
    check(partial_model.rejected_pods == 1, "the bad POD is counted, not guessed");
    if (partial_model.parts.size() == 1) {
        check(partial_model.parts[0].pod_count == 2, "both records stay in the chain");
        check(partial_model.parts[0].vertex_count == 4, "only the good POD contributes");
    } else {
        check(false, "one chain expected");
    }

    // A payload size that overruns the file must fail closed.
    {
        auto bytes = build_fixture(false);
        put32(bytes, 0, 0x7fffffff);
        if (!write_file(truncated, bytes)) return 1;
        Model ignored;
        check(!launcher_assets::load_model(truncated, "truncated", ignored, error),
              "an out-of-file payload size is refused");
    }

    // --- Guide cube example ------------------------------------------------
    const fs::path cube_path = dir / "fixture-cube.memimage";
    if (!write_file(cube_path, build_cube_fixture())) return 1;
    Model cube;
    check(launcher_assets::load_model(cube_path, "cube", cube, error),
          "the guide's cube memimage decodes");
    if (cube.parts.size() == 1) {
        const auto& part = cube.parts[0];
        check(part.vertex_count == 24 && part.triangle_count == 12,
              "the cube's six four-vertex strips give twelve triangles");
        check(part.triangle_clockwise == std::vector<uint8_t>({0,1,0,1,0,1,0,1,0,1,0,1}),
              "the Guide cube's next-token PCLK normalizes each face's alternating winding");
        check(every_triangle_is_sane(cube), "every cube triangle is in range");
        // Each documented face triangle must appear exactly once, whatever
        // vertex order the slot walk emits it in.
        std::array<bool, 12> matched{};
        for (size_t t = 0; t + 2 < part.mesh.indices.size(); t += 3) {
            std::array<uint32_t, 3> tri = {part.mesh.indices[t],
                                           part.mesh.indices[t + 1],
                                           part.mesh.indices[t + 2]};
            std::sort(tri.begin(), tri.end());
            size_t hit = 12;
            for (size_t e = 0; e < 12 && hit == 12; ++e) {
                if (matched[e]) continue;
                if (std::array<uint32_t, 3>({kCubeTriangles[e][0],
                                             kCubeTriangles[e][1],
                                             kCubeTriangles[e][2]}) == tri)
                    hit = e;
            }
            check(hit < 12, "a cube triangle matches the guide's face table");
            if (hit < 12) matched[hit] = true;
        }
        // The guide's Tables 1-3/1-4 state that consecutive triangles of a
        // prim are drawn with alternating orientation, which for the cube's
        // planar faces means the two triangles of a face have opposite face
        // normals. This is the property a strip-only walk gets wrong.
        for (size_t t = 0; t + 5 < part.mesh.indices.size(); t += 6) {
            float normals[2][3] = {};
            for (int half = 0; half < 2; ++half) {
                const size_t base = t + size_t(half) * 3;
                const Vec3& a = part.mesh.positions[part.mesh.indices[base]];
                const Vec3& b = part.mesh.positions[part.mesh.indices[base + 1]];
                const Vec3& c = part.mesh.positions[part.mesh.indices[base + 2]];
                normals[half][0] = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                normals[half][1] = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                normals[half][2] = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            }
            const float dot = normals[0][0] * normals[1][0] +
                              normals[0][1] * normals[1][1] +
                              normals[0][2] * normals[1][2];
            check(dot < 0.0f, "a cube face's two triangles alternate orientation");
        }
    }
    fs::remove(cube_path, ec);

    // --- Synthetic texture bundle ------------------------------------------
    const fs::path textured_path = dir / "fixture-textured.memimage";
    const fs::path small_palette_path = dir / "fixture-small-palette.memimage";
    const fs::path swapped_path = dir / "fixture-textured-swapped-pages.memimage";
    if (!write_file(textured_path, build_textured_fixture(TextureMode::Good)) ||
        !write_file(small_palette_path,
                    build_textured_fixture(TextureMode::SmallPalette)) ||
        !write_file(swapped_path,
                    build_textured_fixture(TextureMode::SwappedPageOrder)))
        return 1;
    Model textured;
    check(launcher_assets::load_model(textured_path, "textured", textured, error),
          "the textured fixture decodes");
    if (textured.parts.size() == 1) {
        const auto& part = textured.parts[0];
        check(part.uvs.size() == part.mesh.indices.size(),
              "the (u,v) array holds one pair per triangle corner");
        check(part.uvs.size() == 6 && part.uvs[1].u == 1.0f &&
                  part.uvs[2].v == 1.0f && part.uvs[0].u == 0.0f &&
                  part.uvs[0].v == 0.0f,
              "the authored (u,v) values survive decoding");
        check(part.textures.size() == 1 && part.textures[0].width == 2 &&
                  part.textures[0].height == 2,
              "the page decodes at its authored 2x2 size");
        if (part.textures.size() == 1) {
            const std::vector<uint8_t> expected = {
                255, 0,   0,   255,  // 0x7fff0000
                0,   255, 0,   255,  // 0x7f00ff00
                0,   0,   255, 255,  // 0x7f0000ff
                255, 255, 255, 3};   // A7 = 1 expands to (1 << 1) | 1
            check(part.textures[0].rgba == expected,
                  "4bpp MSB-first indices decode through the M2PI palette");
            check(part.textures[0].has_alpha,
                  "a decoded page reports its authored alpha");
        }
        check(part.triangle_texture == std::vector<uint32_t>({0, 0}),
              "both triangles sample the page the STXT token names");
        check(textured.texture_pages_decoded == 1 &&
                  textured.texture_pages_failed == 0 &&
                  textured.untextured_pods == 0,
              "a decoded bundle is counted once");
    } else {
        check(false, "the textured fixture should hold one chain");
    }

    // Negative control: a texel index outside the authored M2PI table must
    // refuse the page and leave the triangles flat, never invent a color.
    Model small_palette;
    check(launcher_assets::load_model(small_palette_path, "small-palette",
                                      small_palette, error),
          "a refused texture still decodes its geometry");
    if (small_palette.parts.size() == 1) {
        const auto& part = small_palette.parts[0];
        check(small_palette.texture_pages_failed == 1 &&
                  small_palette.texture_pages_decoded == 0,
              "an out-of-range palette index rejects the page");
        check(small_palette.untextured_pods == 1 &&
                  !small_palette.texture_error.empty(),
              "the refused POD is counted and its reason is reported");
        check(part.textures.size() == 1 && part.textures[0].empty(),
              "a refused page stays present but empty");
        check(part.triangle_texture ==
                  std::vector<uint32_t>({launcher_assets::kNoTexture,
                                         launcher_assets::kNoTexture}),
              "the affected triangles fall back to flat shading");
        check(part.mesh.indices.size() == 6,
              "the geometry is unaffected by the texture refusal");
    } else {
        check(false, "the small-palette fixture should hold one chain");
    }

    // Wrong-result control: this bundle stores two pages in M2PG order but
    // deliberately swaps the PCLT/TpageSnippets selection order. A decoder
    // that assumes `STXT == M2PG index` produces {0,1}; the source-backed map
    // must produce {1,0} and therefore selects the opposite authored texels.
    Model swapped;
    check(launcher_assets::load_model(swapped_path, "swapped-pages", swapped,
                                      error),
          "the PCLT-permuted texture fixture decodes");
    if (swapped.parts.size() == 1) {
        const auto& part = swapped.parts[0];
        check(part.textures.size() == 2 && part.textures[0].width == 2 &&
                  part.textures[1].width == 2,
              "the permuted fixture keeps both M2PG pages");
        check(part.triangle_texture == std::vector<uint32_t>({1, 0}),
              "STXT selections follow PCLT rather than M2PG storage order");
        check(part.triangle_texture != std::vector<uint32_t>({0, 1}),
              "the M2PG-order mapping is a failing negative control");
        if (part.textures.size() == 2 && part.textures[0].rgba.size() >= 2 &&
            part.textures[1].rgba.size() >= 2) {
            check(part.textures[0].rgba[0] == 255 &&
                      part.textures[1].rgba[0] == 255 &&
                      part.textures[0].rgba[1] == 0 &&
                      part.textures[1].rgba[1] == 255,
                  "the two authored page payloads remain distinguishable");
        }
    } else {
        check(false, "the permuted texture fixture should hold one chain");
    }

    // Both strips reuse vertex 3 but author distinct UVs. Vertex-number
    // lookup gives it the fourth pair on both strips and must fail this gate.
    const fs::path seams_path = dir / "fixture-uv-seams.memimage";
    if (!write_file(seams_path, build_textured_fixture(TextureMode::UvSeams)))
        return 1;
    Model seams;
    check(launcher_assets::load_model(seams_path, "uv-seams", seams, error),
          "nonsequential index and UV-seam fixture decodes");
    if (seams.parts.size() == 1) {
        const auto& part = seams.parts[0];
        check(part.mesh.indices == std::vector<uint32_t>({3, 1, 2, 3, 2, 0}),
              "UV expansion preserves original topology");
        check(part.uvs.size() == 6 && part.uvs[0].u == 0.0f &&
                  part.uvs[0].v == 0.0f && part.uvs[3].v == 1.0f &&
                  part.uvs[4].u == 0.25f && part.uvs[5].u == 0.75f,
              "each index occurrence consumes its sequential UV pair");
        check(part.uvs[0].v != part.uvs[3].v,
              "reused vertex carries distinct UVs across the seam");
        check(seams.untextured_pods == 0,
              "UV seam survives the production texture binding");
    } else check(false, "UV seam fixture should hold one chain");
    fs::remove(seams_path, ec);

    const fs::path offset_path = dir / "fixture-palette-offset.memimage";
    if (!write_file(offset_path, build_textured_fixture(TextureMode::PaletteOffset)))
        return 1;
    Model offset;
    check(launcher_assets::load_model(offset_path, "palette-offset", offset, error),
          "a page with a shared-PIP segment offset decodes");
    if (offset.parts.size() == 1 && textured.parts.size() == 1 &&
        offset.parts[0].textures.size() == 1 && textured.parts[0].textures.size() == 1)
        check(offset.parts[0].textures[0].rgba == textured.parts[0].textures[0].rgba,
              "page PIPIndexOffset selects the authored colors, skipping the black sentinel");
    else check(false, "palette-offset fixture retains its decoded page");
    fs::remove(offset_path, ec);

    // --- User game data (optional) -----------------------------------------
    const char* data_env = std::getenv("IMSA_DATA_DIR");
    const fs::path root = data_env && *data_env
        ? fs::path(data_env) / "driving" / "data" : fs::path{};
    if (root.empty() || !fs::is_directory(root, ec)) {
        std::printf("SKIP: IMSA_DATA_DIR/driving/data is not present\n");
    } else {
        // Independent original data: wheelpos is .data+0x5d98 in the immutable
        // big-endian ELF (.data file offset 0x763e0). This rejects substituting
        // carsimmodels' suspension mounts or assuming symmetric half-tracks.
        std::ifstream game(root.parent_path()/"game",std::ios::binary);
        std::array<uint8_t,192> original_mounts{};
        if (game) {
            game.seekg(0x763e0+0x5d98);
            game.read(reinterpret_cast<char*>(original_mounts.data()),original_mounts.size());
            check(bool(game),"original wheelpos table is readable");
            auto original_float = [&](size_t at) {
                const uint32_t bits = (uint32_t(original_mounts[at])<<24) |
                    (uint32_t(original_mounts[at+1])<<16) |
                    (uint32_t(original_mounts[at+2])<<8) | original_mounts[at+3];
                float value; std::memcpy(&value,&bits,sizeof(value)); return value;
            };
            Model race; race.race_pack = true;
            for (int body=0; body<4; ++body) {
                std::array<launcher_assets::Transform,4> placements;
                check(launcher_assets::wheel_placements(race,body,placements)==4,
                      "race cars have four neutral wheel transforms");
                for (int wheel=0; wheel<4; ++wheel) {
                    for (int axis=0; axis<3; ++axis)
                        check(placements[wheel].m[9+axis]==original_float(body*48+wheel*12+axis*4),
                              "race wheel translation matches original graphics wheelpos");
                    check(placements[wheel].m[0]==((wheel==0 || wheel==3)?-1:1),
                          "race rims follow the original wheel-index rotation");
                }
            }
        } else {
            std::printf("SKIP: original driving/game wheelpos table is not present\n");
        }
        using launcher_assets::kShowroomChassisChoiceCount;
        using launcher_assets::kShowroomChassisChoices;
        using launcher_assets::kShowroomTireChoiceCount;
        using launcher_assets::kShowroomTireChoices;
        check(kShowroomChassisChoiceCount == 4 && kShowroomTireChoiceCount == 4,
              "the viewer offers the four authored chassis and tire variants");

        for (int i = 0; i < kShowroomChassisChoiceCount; ++i) {
            Model chassis;
            const auto& choice = kShowroomChassisChoices[i];
            if (!launcher_assets::load_model(root / choice.relative_path,
                                             choice.label, chassis, error)) {
                check(false, "a showroom chassis asset failed to decode");
                std::fprintf(stderr, "  %s: %s\n", choice.label, error.c_str());
                continue;
            }
            // Verified chain split of the four showroom memimages: the car
            // body (19-21 PODs), the room (6 PODs, carrying the documented
            // 1,296-vertex 1000x1000 floor), the shadow quad (1 POD) and the
            // authored wheel (1 POD, 146 vertices).
            check(chassis.parts.size() == 4,
                  "a showroom memimage holds car, room, shadow and wheel chains");
            check(every_triangle_is_sane(chassis), "every chassis triangle is in range");
            // Original showroom root+0x30 matrices, read independently from
            // the four checked-in memimages (the scale estimates differ).
            const float authored[4][5]{{85,2,112,-172,0.856f},
                                      {86,-4,137,-125,1},
                                      {85,0,120,-156,1},
                                      {85,0,122,-135,1}};
            std::array<launcher_assets::Transform,4> mounts;
            check(launcher_assets::wheel_placements(chassis,i,mounts)==4,
                  "each showroom chassis supplies four authored mounts");
            check(mounts[0].m[9]==authored[i][0] && mounts[0].m[10]==authored[i][1] &&
                      mounts[0].m[11]==authored[i][2] && mounts[3].m[11]==authored[i][3] &&
                      std::fabs(mounts[3].m[4]+authored[i][4])<1e-6f &&
                      mounts[0].m[0]==-1 && mounts[1].m[0]==1,
                  "authored showroom spacing, height, rear scale and rim orientation are retained");
            const int car = chassis.default_part;
            check(car >= 0 && chassis.parts[size_t(car)].pod_count > 10 &&
                      chassis.parts[size_t(car)].vertex_count >= 1000,
                  "the default chain is the car body, not the room");
            if (car >= 0) {
                const auto& mesh = chassis.parts[size_t(car)].mesh;
                check(mesh.bbox_min.x <= -100.0f && mesh.bbox_max.x >= 100.0f &&
                          mesh.bbox_max.x - mesh.bbox_min.x <= 260.0f,
                      "the car chain spans its authored ~210-unit width");
                check(mesh.bbox_max.z - mesh.bbox_min.z >= 380.0f,
                      "the car chain spans its authored ~390-475-unit length");
            }
            // The car chain is the fully textured one: every POD names a
            // bundle whose first page its `ptexture` pointer lands on, and the
            // STXT page of each strip decodes from that bundle's table.
            if (car >= 0) {
                const auto& part = chassis.parts[size_t(car)];
                check(part.uvs.size() == part.mesh.indices.size(),
                      "every car triangle corner carries an (u,v) pair");
                check(part.triangle_texture.size() == part.triangle_count,
                      "one texture page per car triangle");
                size_t textured = 0;
                for (const uint32_t page : part.triangle_texture) {
                    if (page == launcher_assets::kNoTexture) continue;
                    ++textured;
                    check(page < part.textures.size() &&
                              !part.textures[page].empty(),
                          "a textured triangle points at a decoded page");
                }
                check(textured * 100 >= size_t(part.triangle_count) * 99,
                      "the car chain is textured from its authored liveries");
                for (const auto& image : part.textures) {
                    if (image.empty()) continue;
                    check(
                        image.rgba.size() == size_t(image.width) * image.height * 4,
                        "a decoded page holds width*height RGBA texels");
                }
                check(chassis.texture_pages_decoded > 0 &&
                          chassis.texture_pages_failed == 0,
                      "the showroom car textures decode without a skipped page");
                check(nonmanifold_edge_fraction(part) < 0.01,
                      "the car chain decodes to a manifold surface");
            }
            check(find_bounds(chassis, {-500, -42.2f, -500}, {500, 247.8f, 500}, 0.5f).ok,
                  "the room chain carries the documented 1000x1000 showroom floor");
            check(find_bounds(chassis, {-15.5f, -31.5f, -31.5f}, {15.3f, 31.5f, 31.5f},
                              0.5f).ok,
                  "the authored wheel chain decodes at its 63-unit diameter");
            // Every chain the dropdown can select must be framed by the
            // fitted camera, the room included.
            for (const auto& part : chassis.parts)
                if (!part.mesh.positions.empty())
                    check_framing(part.mesh, choice.label);
        }
        for (int i = 0; i < kShowroomTireChoiceCount; ++i) {
            Model tire;
            const auto& choice = kShowroomTireChoices[i];
            if (!launcher_assets::load_model(root / choice.relative_path,
                                             choice.label, tire, error)) {
                check(false, "a showroom tire asset failed to decode");
                std::fprintf(stderr, "  %s: %s\n", choice.label, error.c_str());
                continue;
            }
            check(tire.parts.size() == 1 && tire.parts[0].pod_count == 5,
                  "a tire memimage includes its untextured POD in the five-POD chain");
            check(every_triangle_is_sane(tire), "every tire triangle is in range");
            const auto& tire_part = tire.parts[0];
            check(tire_part.uvs.size() == tire_part.mesh.indices.size(),
                  "every tire triangle corner carries an (u,v) pair");
            size_t tire_textured = 0;
            for (const uint32_t page : tire_part.triangle_texture)
                if (page != launcher_assets::kNoTexture) ++tire_textured;
            check(tire.texture_pages_decoded > 0 &&
                      tire.texture_pages_failed == 0,
                  "the tire textures decode without a skipped page");
            check(tire_textured > 0 &&
                      tire_textured < tire_part.triangle_count,
                  "the tire's UV-less POD stays flat while the rest is textured");
            check(nonmanifold_edge_fraction(tire_part) < 0.01,
                  "the tire chain decodes to a manifold surface");
        }
        // A missing file must be reported, not silently substituted.
        for (int pack = 0; pack < launcher_assets::kRaceCarChoiceCount; ++pack) {
            const auto& choice = launcher_assets::kRaceCarChoices[pack];
            Model race;
            check(launcher_assets::load_model(root / choice.relative_path,
                                              choice.label, race, error),
                  "a race pack decodes through its authored LOD tables");
            if (!race.race_pack) { check(false, "race pack is identified"); continue; }
            check(race.rejected_pods == 0, "all selected race PODs pass geometry validation");
            std::array<const launcher_assets::Part*, 36> bodies{};
            for (const auto& part : race.parts) {
                if (part.component != "body") continue;
                check(part.body >= 0 && part.body < 4 && part.variant >= 0 &&
                          part.variant < 3 && part.lod >= 0 && part.lod < 3,
                      "race selection has a body, variant and LOD in the guest range");
                const size_t slot = size_t(part.body * 9 + part.variant * 3 + part.lod);
                if (slot >= bodies.size()) continue;
                check(bodies[slot] == nullptr, "race selection is unique");
                bodies[slot] = &part;
                check(part.pod_offsets.size() == part.pod_count,
                      "race POD provenance covers the selected group");
                for (size_t i = 0; i < part.pod_offsets.size(); ++i)
                    check(part.pod_offsets[i] == part.chain_head + i * 0x24,
                          "MS_addgroup selects contiguous PODs, not stale variant links");
                check(part.uvs.size() == part.mesh.indices.size(),
                      "race texture seams retain a UV for each triangle corner");
            }
            check(std::all_of(bodies.begin(), bodies.end(), [](auto p) { return p; }),
                  "all four bodies, three variants and three LODs are available");
            if (bodies[0] && bodies[3] && bodies[6]) {
                check(bodies[0]->chain_head != bodies[3]->chain_head &&
                          bodies[3]->chain_head != bodies[6]->chain_head,
                      "variants resolve to distinct authored POD groups");
                const auto& original = bodies[0]->mesh.positions;
                const auto& variant = bodies[3]->mesh.positions;
                check(original.size() == variant.size() &&
                          std::equal(original.begin(), original.end(), variant.begin(),
                              [](const Vec3& a, const Vec3& b) {
                                  return a.x == b.x && a.y == b.y && a.z == b.z;
                              }),
                      "the first variant shares its authored geometry");
                check(bodies[0]->textures.size() == bodies[3]->textures.size() &&
                          bodies[0]->textures[0].rgba == bodies[3]->textures[0].rgba,
                      "body variants preserve their shared authored livery");
                size_t changed_pages = 0;
                for (size_t i = 0; i < std::min(bodies[0]->textures.size(),
                                               bodies[3]->textures.size()); ++i)
                    if (bodies[0]->textures[i].rgba != bodies[3]->textures[i].rgba)
                        ++changed_pages;
                check(changed_pages > 0,
                      "body variant selects a different authored paint scheme");
                check(bodies[0]->pod_ptexture[1] != bodies[3]->pod_ptexture[1],
                      "later PODs use the selected variant rather than stale variant-0 links");
            }
            if (bodies[0] && bodies[1] && bodies[2])
                check(bodies[0]->triangle_count > bodies[1]->triangle_count &&
                          bodies[1]->triangle_count > bodies[2]->triangle_count,
                      "the three LODs have distinct decreasing detail");
        }
        Model missing;
        check(!launcher_assets::load_model(root / "showroom/does-not-exist.memimage",
                                           "missing", missing, error),
              "an absent asset is refused");
    }

    fs::remove(good, ec);
    fs::remove(partial, ec);
    fs::remove(truncated, ec);
    fs::remove(textured_path, ec);
    fs::remove(small_palette_path, ec);
    fs::remove(swapped_path, ec);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
