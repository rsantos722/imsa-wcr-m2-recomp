// Host-only texture reader for the launcher's Asset Viewer tab.
//
// The viewer wants to look at the original showroom cars with their authored
// liveries, so this decodes the texture bundle a POD points at. It is a thin,
// fail-closed subset of the project's existing TXTR decoder
// (`tools/decode_txtr.py`, which `docs/formats/memimage.md` documents and the
// loader-format contract exercises) rather than a second interpretation of the
// format. Only what the showroom assets actually use is implemented:
//
//   * chunk sequences `M2TX + M2PI [M2TA] [M2PG] [M2TD …]`, either wrapped in a
//     `FORM TXTR` or loose in the dumped payload (the showroom files carry both;
//     `M2PG` page tables joined to the `PCLT` page command list),
//   * uncompressed texel LODs (`SSB -> alpha -> color/index`, MSB-first),
//   * the `M2PI` palette with the exact `M2TXColor_Decode` alpha expansion.
//
// A compressed (`M2CI` RLE) or otherwise unmodelled source is reported as an
// error for that page; the caller then draws that POD untextured instead of
// guessing. Ground truth: `docs/formats/memimage.md` and the vendored SDK
// (`txtlib/include/M2TXTypes.h`, `mercury.h`).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace launcher_assets {

// One decoded texture page: RGBA8, four bytes per pixel, row-major, with the
// first stored texel first. An empty image (width == 0) is a page that was
// recognised but not decoded.
struct TextureImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    bool has_alpha = false;  // some texel is not fully opaque

    bool empty() const { return width <= 0 || height <= 0; }
};

// The `PodTexture` fields the viewer consumes. Both pointers are payload
// relative, as every other pointer in a dumped memimage is.
struct PodTextureRef {
    uint32_t ptpagesnippets = 0;  // TpageSnippets used by STXT selections
    uint32_t ptexture = 0;  // first page's LOD0 texel data
    uint32_t ppip = 0;      // its M2PI color table
};

// Read the `PodTexture` record at `record` in `payload`. Fails when the record,
// TpageSnippets pointer, or texture pointer is outside the payload.
bool read_pod_texture_ref(const std::vector<uint8_t>& payload, uint32_t record,
                          PodTextureRef& out);

// Decode the texture bundle `ref.ptexture` points into. Pages that are not
// decoded stay empty in `out.pages` (with the reason in `page_errors`), so the
// caller can still index pages by their authored number. Returns false, with
// `error` set, when no bundle contains `ref.ptexture`.
struct TextureBundle {
    uint32_t m2tx_offset = 0;  // payload offset of the M2TX chunk
    std::vector<TextureImage> pages;
    std::vector<std::string> page_errors;  // parallel to `pages`; empty = ok
    // STXT selects an MSnippet, whose CLT data pointer is named by PCLT's
    // TexCLTOff table. This maps that authored selection number to the M2PG
    // page index used by `pages`; it is intentionally exposed so the model
    // decoder and its wrong-order control cannot silently fall back to the
    // M2PG storage order.
    std::vector<uint32_t> stxt_to_page;
};
bool decode_pod_texture(const std::vector<uint8_t>& payload,
                        const PodTextureRef& ref, TextureBundle& out,
                        std::string& error);

}  // namespace launcher_assets
