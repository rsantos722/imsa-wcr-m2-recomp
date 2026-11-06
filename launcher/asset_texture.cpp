// See asset_texture.h for the format evidence behind this decoder.
#include "asset_texture.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace launcher_assets {
namespace {

uint32_t be32(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) |
           (uint32_t(d[2]) << 8) | uint32_t(d[3]);
}
uint16_t be16(const uint8_t* d) {
    return uint16_t((uint32_t(d[0]) << 8) | uint32_t(d[1]));
}
bool fits(uint64_t at, uint64_t span, size_t limit) {
    return at <= limit && span <= uint64_t(limit) - at;
}

// Texel-format bits (M2TXTypes.h: M2HC_/M2CI_ defines; docs/formats/memimage.md).
constexpr uint16_t kHasSsb = 0x0200;
constexpr uint16_t kHasColor = 0x0400;
constexpr uint16_t kHasAlpha = 0x0800;
constexpr uint16_t kIsLiteral = 0x1000;
constexpr uint16_t kColorDepthMask = 0x000f;
// M2TX header flags.
constexpr uint32_t kIsCompressed = 0x0001;
constexpr uint32_t kHasColorConst = 0x0004;
// M2TXTypes.h M2PG_FLAGS_*.
constexpr uint32_t kPageHasTexFormat = 0x00000004;
constexpr uint32_t kPageIsCompressed = 0x00000080;

constexpr uint32_t kMaxDim = 1024;
constexpr uint32_t kMaxPixels = 1u << 20;
constexpr uint8_t kMaxLod = 12;
constexpr uint32_t kMaxPages = 16;

struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;
    bool ssb = false;
};

// M2TXColor_Decode: [SSB:1][A:7][R:8][G:8][B:8]; the 7-bit alpha expands as
// (a << 1) | (a & 1). The top bit is the SSB selector, never alpha.
Color decode_color(uint32_t word) {
    Color c;
    c.b = uint8_t(word & 0xff);
    c.g = uint8_t((word >> 8) & 0xff);
    c.r = uint8_t((word >> 16) & 0xff);
    const uint32_t alpha7 = (word >> 24) & 0x7f;
    c.a = uint8_t((alpha7 << 1) | (alpha7 & 1));
    c.ssb = ((word >> 31) & 1u) != 0;
    return c;
}

// qmem.h oneExtend(): widen a `depth`-bit source field to 8 bits.
uint8_t one_extend(int depth, uint32_t value) {
    if (depth <= 0) return 0;
    if (depth >= 8) return uint8_t(value & 0xff);
    const uint32_t mask = (1u << depth) - 1u;
    const uint32_t v = value & mask;
    return uint8_t((v << (8 - depth)) | ((v & 1u) ? (0xffu >> depth) : 0u));
}

// M2TXTypes.h: SSB -> alpha -> color/index, with literals carrying R,G,B.
int texel_depth(uint16_t format) {
    int color = (format & kHasColor) ? int(format & kColorDepthMask) : 0;
    const int alpha = (format & kHasAlpha) ? int((format >> 4) & 0x0f) : 0;
    const int ssb = (format & kHasSsb) ? 1 : 0;
    if (format & kIsLiteral) color *= 3;
    return color + alpha + ssb;
}

// SDK padPixels: LOD storage is padded to the next 32-bit word.
uint64_t padded_lod_bytes(uint64_t pixels, int depth) {
    return ((pixels * uint64_t(depth) + 31u) / 32u) * 4u;
}

// MSB-first bit reader bounded by one LOD's byte range; a read past the end
// fails rather than reading padding.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool get(int bits, uint32_t& out) {
        if (bits <= 0) {
            out = 0;
            return true;
        }
        if (pos_ + size_t(bits) > size_ * 8) return false;
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            const size_t bit = pos_++;
            v = (v << 1) | uint32_t((data_[bit >> 3] >> (7 - (bit & 7))) & 1);
        }
        out = v;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

bool known_chunk(const uint8_t* m) {
    static const char* const kIds[] = {"M2TX", "M2PI", "M2TA", "M2PG",
                                       "M2CI", "M2TD", "PCLT", "M2LR"};
    for (const char* id : kIds)
        if (std::memcmp(m, id, 4) == 0) return true;
    return false;
}

struct Chunk {
    bool seen = false;
    uint32_t payload = 0;
    uint32_t len = 0;
};

// A texture bundle: the chunk sequence one POD texture pointer sits in. The
// showroom memimages carry these both inside a FORM TXTR and loose in the
// dumped payload, so the walk below is wrapper-agnostic.
struct Bundle {
    uint32_t m2tx = 0;
    uint32_t flags = 0;
    uint16_t tex_format = 0;
    uint16_t min_x = 0, min_y = 0;
    uint8_t num_lod = 0;
    Chunk pi, ta, pclt;
    uint32_t data = 0;      // first byte of the shared LOD0 texel area
    uint32_t data_end = 0;
    struct Page {
        uint32_t offset = 0;  // relative to `data`
        uint16_t width = 0, height = 0;
        uint16_t format = 0;
        uint8_t pip_index_offset = 0;
        bool compressed = false;
    };
    std::vector<Page> pages;
    uint32_t pclt_payload = 0;
    uint32_t pclt_len = 0;
    uint32_t pclt_version = 0;
    std::vector<uint32_t> pclt_tex_offsets;
    std::vector<std::pair<float, float>> pclt_uv_scales;
};

Chunk* pick(Chunk& tx, Chunk& pi, Chunk& ta, Chunk& pg, Chunk& ci, Chunk& td,
            Chunk& pclt, const uint8_t* magic) {
    if (std::memcmp(magic, "M2TX", 4) == 0) return &tx;
    if (std::memcmp(magic, "M2PI", 4) == 0) return &pi;
    if (std::memcmp(magic, "M2TA", 4) == 0) return &ta;
    if (std::memcmp(magic, "M2PG", 4) == 0) return &pg;
    if (std::memcmp(magic, "M2CI", 4) == 0) return &ci;
    if (std::memcmp(magic, "M2TD", 4) == 0) return &td;
    if (std::memcmp(magic, "PCLT", 4) == 0) return &pclt;
    return nullptr;
}

bool in_dim_range(uint32_t value) { return value >= 1 && value <= kMaxDim; }

// Parse the chunk sequence that starts at `start` (an M2TX chunk header).
bool parse_bundle(const std::vector<uint8_t>& p, uint32_t start, Bundle& out) {
    Chunk tx, pi, ta, pg, ci, td, pclt;
    uint32_t cursor = start;
    for (int guard = 0; guard < 32; ++guard) {
        if (!fits(cursor, 8, p.size()) || !known_chunk(&p[cursor])) break;
        const uint32_t size = be32(&p[cursor + 4]);
        const uint32_t payload = cursor + 8;
        if (!fits(payload, size, p.size())) return false;
        if (Chunk* slot = pick(tx, pi, ta, pg, ci, td, pclt, &p[cursor]))
            if (!slot->seen) {
                slot->seen = true;
                slot->payload = payload;
                slot->len = size;
            }
        cursor = payload + size + ((-size) % 4);
    }
    if (!tx.seen || !td.seen || tx.len < 16) return false;

    out.m2tx = start;
    out.flags = be32(&p[tx.payload + 4]);
    out.min_x = be16(&p[tx.payload + 8]);
    out.min_y = be16(&p[tx.payload + 10]);
    out.tex_format = be16(&p[tx.payload + 12]);
    out.num_lod = p[tx.payload + 14];
    if (!in_dim_range(out.min_x) || !in_dim_range(out.min_y)) return false;
    if (out.num_lod == 0 || out.num_lod > kMaxLod) return false;
    if (texel_depth(out.tex_format) <= 0) return false;

    if (td.len < 4) return false;
    const uint16_t lod_count = be16(&p[td.payload]);
    if (lod_count == 0 || lod_count > kMaxLod || lod_count != out.num_lod)
        return false;
    if (td.len < 4u + 4u * lod_count) return false;
    const uint32_t lod0 = be32(&p[td.payload + 4]);
    if (lod0 < 4u + 4u * lod_count || lod0 >= td.len) return false;
    out.data = td.payload + lod0;
    out.data_end = td.payload + td.len;
    out.pi = pi;
    out.ta = ta;
    out.pclt_payload = pclt.payload;
    out.pclt_len = pclt.len;

    const auto scale = [](uint16_t dim, uint8_t lod) {
        return uint32_t(dim) << (lod - 1);  // LOD0 is the largest level
    };
    if (pg.seen) {
        if (pg.len < 8) return false;
        const uint32_t num_tex = be32(&p[pg.payload]);
        const uint32_t version = be32(&p[pg.payload + 4]);
        if (num_tex == 0 || num_tex > kMaxPages) return false;
        uint32_t off = 8;
        for (uint32_t i = 0; i < num_tex; ++i) {
            if (off + 20 > pg.len) return false;
            const uint8_t* hdr = &p[pg.payload + off];
            const uint32_t page_flags = be32(hdr + 4);
            const uint16_t px = be16(hdr + 8), py = be16(hdr + 10);
            const uint16_t fmt = be16(hdr + 12);
            const uint8_t lod = hdr[14];
            if (lod == 0 || lod > kMaxLod || !in_dim_range(px) || !in_dim_range(py))
                return false;
            Bundle::Page page;
            page.offset = be32(hdr);
            page.width = uint16_t(scale(px, lod));
            page.height = uint16_t(scale(py, lod));
            if (!in_dim_range(page.width) || !in_dim_range(page.height))
                return false;
            page.format = (page_flags & kPageHasTexFormat) ? fmt : out.tex_format;
            page.pip_index_offset = hdr[15];
            page.compressed = (page_flags & kPageIsCompressed) != 0;
            if (texel_depth(page.format) <= 0) return false;
            out.pages.push_back(page);
            // Mercury >= 3.0 appends DCI + LOD lengths after the 20-byte header.
            if (version >= 0x00030000u) {
                if (lod > 11) return false;
                off += 20 + 8 + 16 + 4 * uint32_t(lod);
            } else {
                off += 20;
            }
        }
        if (off != pg.len) return false;
        // A paged texture's CLT table is the authoritative link between an
        // STXT selection and the page table. Do not infer this from storage
        // order: the loader patches the MSnippet data pointers from PCLT.
        if (pclt.seen) {
            if (pclt.len < 16) return false;
            const uint32_t num_pclt = be32(&p[pclt.payload]);
            const uint32_t pclt_version = be32(&p[pclt.payload + 4]);
            if (num_pclt != num_tex || num_pclt == 0 || num_pclt > kMaxPages)
                return false;
            const uint32_t header = pclt_version > 0 ? 20u : 16u;
            if (pclt.len < header + 4u * num_pclt) return false;
            const uint32_t page_clt = be32(&p[pclt.payload +
                                              (pclt_version > 0 ? 12 : 8)]);
            const uint32_t pip_clt = be32(&p[pclt.payload +
                                             (pclt_version > 0 ? 16 : 12)]);
            if (page_clt > pip_clt || pip_clt > pclt.len) return false;
            out.pclt_version = pclt_version;
            out.pclt_tex_offsets.reserve(num_pclt);
            uint32_t previous = pip_clt;
            for (uint32_t i = 0; i < num_pclt; ++i) {
                const uint32_t offset = be32(&p[pclt.payload + header + 4u * i]);
                // Every reference snippet follows the PIP snippet and the
                // offsets are the segment boundaries consumed by M2TXiff.c.
                if (offset < previous || offset >= pclt.len) return false;
                out.pclt_tex_offsets.push_back(offset);
                previous = offset;
            }
            if (pclt_version > 0) {
                const uint32_t uv_start = header + 4u * num_pclt;
                if (uv_start + 8u * num_pclt > pclt.len) return false;
                out.pclt_uv_scales.reserve(num_pclt);
                for (uint32_t i = 0; i < num_pclt; ++i) {
                    const float u = [&] {
                        uint32_t bits = be32(&p[pclt.payload + uv_start + 8u * i]);
                        float value;
                        std::memcpy(&value, &bits, sizeof(value));
                        return value;
                    }();
                    const float v = [&] {
                        uint32_t bits = be32(&p[pclt.payload + uv_start + 8u * i + 4]);
                        float value;
                        std::memcpy(&value, &bits, sizeof(value));
                        return value;
                    }();
                    if (!std::isfinite(u) || !std::isfinite(v) || u <= 0.0f ||
                        v <= 0.0f)
                        return false;
                    out.pclt_uv_scales.emplace_back(u, v);
                }
                const uint64_t patch_start = uint64_t(uv_start) + 8u * num_pclt;
                if (pclt_version > 0 && patch_start +
                                            4ull * be32(&p[pclt.payload + 8]) >
                                            pclt.len)
                    return false;
            }
        } else if (num_tex > 1) {
            // A multi-page bundle without PCLT has no source-backed way to
            // identify STXT selections, so it is rejected rather than
            // silently treating M2PG order as an identity map.
            return false;
        }
        return true;
    }

    Bundle::Page page;
    page.offset = 0;
    page.width = uint16_t(scale(out.min_x, out.num_lod));
    page.height = uint16_t(scale(out.min_y, out.num_lod));
    if (!in_dim_range(page.width) || !in_dim_range(page.height)) return false;
    page.format = out.tex_format;
    page.compressed = (out.flags & kIsCompressed) != 0;
    out.pages.push_back(page);
    if (pclt.seen) {
        if (pclt.len < 20 || be32(&p[pclt.payload]) != 1) return false;
        const uint32_t version = be32(&p[pclt.payload + 4]);
        const uint32_t header = version > 0 ? 20u : 16u;
        if (pclt.len < header + 4) return false;
        const uint32_t page_clt = be32(&p[pclt.payload + (version > 0 ? 12 : 8)]);
        const uint32_t pip_clt = be32(&p[pclt.payload + (version > 0 ? 16 : 12)]);
        const uint32_t offset = be32(&p[pclt.payload + header]);
        if (page_clt > pip_clt || pip_clt > offset || offset >= pclt.len)
            return false;
        out.pclt_version = version;
        out.pclt_tex_offsets.push_back(offset);
        if (version > 0) {
            if (pclt.len < header + 4 + 8) return false;
            uint32_t bits = be32(&p[pclt.payload + header + 4]);
            float u;
            std::memcpy(&u, &bits, sizeof(u));
            bits = be32(&p[pclt.payload + header + 8]);
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            if (!std::isfinite(u) || !std::isfinite(v) || u <= 0 || v <= 0)
                return false;
            out.pclt_uv_scales.emplace_back(u, v);
        }
    } else {
        return false;
    }
    return true;
}

std::vector<Color> read_palette(const std::vector<uint8_t>& p, const Chunk& pi) {
    // M2PI payload: u32 IndexOffset, u32 reserved, then color words.
    std::vector<Color> palette;
    if (!pi.seen || pi.len < 8) return palette;
    palette.reserve((pi.len - 8) / 4);
    for (uint32_t off = 8; off + 4 <= pi.len; off += 4)
        palette.push_back(decode_color(be32(&p[pi.payload + off])));
    return palette;
}

float read_float(const std::vector<uint8_t>& p, size_t at) {
    const uint32_t bits = be32(&p[at]);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool map_tpage_snippets(const std::vector<uint8_t>& p, const PodTextureRef& ref,
                        const Bundle& bundle, TextureBundle& out,
                        std::string& error) {
    if (ref.ptpagesnippets == 0 || !fits(ref.ptpagesnippets, 12, p.size())) {
        error = "PodTexture has no in-payload TpageSnippets record";
        return false;
    }
    if (bundle.pclt_len == 0 || bundle.pclt_tex_offsets.size() != bundle.pages.size()) {
        error = "texture bundle has no complete PCLT page-selection table";
        return false;
    }
    const uint32_t loadcount = be32(&p[ref.ptpagesnippets]);
    const uint32_t pload = be32(&p[ref.ptpagesnippets + 4]);
    const uint32_t pselect = be32(&p[ref.ptpagesnippets + 8]);
    if (loadcount == 0 || loadcount > 64 ||
        !fits(pload, uint64_t(loadcount) * 8, p.size()) ||
        !fits(pselect, uint64_t(bundle.pages.size()) * 16, p.size())) {
        error = "TpageSnippets arrays fall outside the payload";
        return false;
    }

    out.stxt_to_page.assign(bundle.pages.size(), 0);
    std::vector<bool> used(bundle.pages.size(), false);
    for (size_t selection = 0; selection < bundle.pages.size(); ++selection) {
        const size_t at = size_t(pselect) + selection * 16;
        const uint32_t data = be32(&p[at]);
        const float uscale = read_float(p, at + 8);
        const float vscale = read_float(p, at + 12);
        if (!std::isfinite(uscale) || !std::isfinite(vscale) || uscale <= 0.0f ||
            vscale <= 0.0f) {
            error = "TpageSnippets has an invalid UV scale";
            return false;
        }
        size_t page = bundle.pages.size();
        for (size_t candidate = 0; candidate < bundle.pclt_tex_offsets.size();
             ++candidate) {
            const uint64_t expected = uint64_t(bundle.pclt_payload) +
                                      bundle.pclt_tex_offsets[candidate];
            if (data == expected) {
                page = candidate;
                break;
            }
        }
        if (page >= bundle.pages.size() || used[page]) {
            error = "STXT selection does not map one-to-one through PCLT";
            return false;
        }
        const auto& authored = bundle.pages[page];
        if (std::fabs(uscale - authored.width) > 0.001f ||
            std::fabs(vscale - authored.height) > 0.001f) {
            error = "TpageSnippets UV scale disagrees with M2PG dimensions";
            return false;
        }
        if (bundle.pclt_version > 0) {
            const auto& pclt_scale = bundle.pclt_uv_scales[selection];
            if (std::fabs(pclt_scale.first - uscale) > 0.001f ||
                std::fabs(pclt_scale.second - vscale) > 0.001f) {
                error = "TpageSnippets UV scale disagrees with PCLT";
                return false;
            }
        }
        used[page] = true;
        out.stxt_to_page[selection] = uint32_t(page);
    }
    return true;
}

// M2TA carries the SSB-side header constants (TXA_PipConstSSB0/1). Both start
// as the loader default, which is opaque white unless the bundle declares
// color constants.
void read_constants(const std::vector<uint8_t>& p, const Chunk& ta,
                    bool has_color_const, Color out[2]) {
    const Color fallback =
        decode_color(has_color_const ? 0x7f000000u : 0x7fffffffu);
    out[0] = out[1] = fallback;
    if (!ta.seen) return;
    for (uint32_t off = 4; off + 8 <= ta.len; off += 8) {
        const uint32_t attr = be32(&p[ta.payload + off]);
        const uint32_t value = be32(&p[ta.payload + off + 4]);
        if (attr == 8) out[0] = decode_color(value);
        else if (attr == 9) out[1] = decode_color(value);
    }
}

// Decode one page's LOD0 using the uncompressed source layout: each texel
// consumes SSB (optional), alpha (optional), then literal RGB or an indexed
// color. Row-major placement only fixes the texel order; the stream does not
// restart per row.
bool decode_page(const std::vector<uint8_t>& p, const Bundle& bundle,
                 const Bundle::Page& page, const std::vector<Color>& palette,
                 const Color constants[2], TextureImage& out,
                 std::string& error) {
    out = TextureImage{};  // a refused page stays empty
    if (page.compressed) {
        error = "compressed (M2CI) texel stream is not decoded by the viewer";
        return false;
    }
    const int depth = texel_depth(page.format);
    if (depth <= 0) {
        error = "page has no texel fields";
        return false;
    }
    const uint64_t pixels = uint64_t(page.width) * page.height;
    if (pixels == 0 || pixels > kMaxPixels) {
        error = "page dimensions are outside the decoded range";
        return false;
    }
    const uint64_t size = padded_lod_bytes(pixels, depth);
    const uint64_t data_len = bundle.data_end - bundle.data;
    if (uint64_t(page.offset) + size > data_len) {
        error = "page texel data extends past the bundle";
        return false;
    }
    const uint8_t* data = &p[bundle.data + page.offset];
    // Decode into a local image: a page that fails part way through must be
    // reported as empty, never as a half-filled texture.
    TextureImage image;
    image.width = page.width;
    image.height = page.height;
    image.rgba.assign(size_t(pixels) * 4, 0);

    const bool has_color = (page.format & kHasColor) != 0;
    const bool has_alpha = (page.format & kHasAlpha) != 0;
    const bool has_ssb = (page.format & kHasSsb) != 0;
    const bool literal = (page.format & kIsLiteral) != 0;
    const int color_depth = has_color ? int(page.format & kColorDepthMask) : 0;
    const int alpha_depth = has_alpha ? int((page.format >> 4) & 0x0f) : 0;

    BitReader reader(data, size_t(size));
    for (uint64_t i = 0; i < pixels; ++i) {
        uint32_t bits = 0;
        bool ssb = constants[0].ssb;
        if (has_ssb) {
            if (!reader.get(1, bits)) {
                error = "texel stream ended early";
                return false;
            }
            ssb = bits != 0;
        }
        uint8_t source_alpha = 0;
        bool has_alpha_field = false;
        if (alpha_depth > 0 && has_alpha) {
            if (!reader.get(alpha_depth, bits)) {
                error = "texel stream ended early";
                return false;
            }
            source_alpha = one_extend(alpha_depth, bits);
            has_alpha_field = true;
        }
        const Color& constant = constants[ssb ? 1 : 0];
        Color color = constant;
        if (color_depth > 0 && has_color) {
            if (literal) {
                uint32_t r = 0, g = 0, b = 0;
                if (!reader.get(color_depth, r) || !reader.get(color_depth, g) ||
                    !reader.get(color_depth, b)) {
                    error = "texel stream ended early";
                    return false;
                }
                color.r = one_extend(color_depth, r);
                color.g = one_extend(color_depth, g);
                color.b = one_extend(color_depth, b);
            } else {
                if (!reader.get(color_depth, bits)) {
                    error = "texel stream ended early";
                    return false;
                }
                // M2TXPgHeader.PIPIndexOffset selects a segment of the shared
                // PIP. CLT's draw-stage lookup adds it with 8-bit wrap; using
                // the unshifted palette turns the race liveries into noise.
                const uint32_t index = (bits + page.pip_index_offset) & 0xffu;
                if (index >= palette.size()) {
                    error = "palette index outside the M2PI table";
                    return false;
                }
                color = palette[index];
            }
        }
        if (has_alpha) color.a = has_alpha_field ? source_alpha : constant.a;
        else if (!has_color || color_depth == 0 || literal) color.a = constant.a;

        uint8_t* texel = &image.rgba[size_t(i) * 4];
        texel[0] = color.r;
        texel[1] = color.g;
        texel[2] = color.b;
        texel[3] = color.a;
        if (color.a != 255) image.has_alpha = true;
    }
    out = std::move(image);
    return true;
}

}  // namespace

bool read_pod_texture_ref(const std::vector<uint8_t>& payload, uint32_t record,
                          PodTextureRef& out) {
    if (!fits(record, 0x1c, payload.size()) || (record & 3u)) return false;
    out.ptpagesnippets = be32(&payload[record + 0x04]);
    out.ptexture = be32(&payload[record + 0x08]);
    out.ppip = be32(&payload[record + 0x14]);
    return out.ptpagesnippets != 0 &&
           fits(out.ptpagesnippets, 12, payload.size()) && out.ptexture != 0 &&
           fits(out.ptexture, 1, payload.size()) &&
           (out.ppip == 0 || fits(out.ppip, 1, payload.size()));
}

bool decode_pod_texture(const std::vector<uint8_t>& payload,
                        const PodTextureRef& ref, TextureBundle& out,
                        std::string& error) {
    out = TextureBundle{};
    uint32_t found = 0;
    for (size_t at = 0; fits(at, 4, payload.size()); at += 4) {
        if (std::memcmp(&payload[at], "M2TX", 4) != 0) continue;
        Bundle bundle;
        if (!parse_bundle(payload, uint32_t(at), bundle)) continue;
        if (ref.ptexture < bundle.data || ref.ptexture >= bundle.data_end) continue;
        if (bundle.pi.seen && ref.ppip != 0 &&
            (ref.ppip < bundle.pi.payload ||
             ref.ppip >= bundle.pi.payload + bundle.pi.len))
            continue;  // the palette pointer disagrees with this bundle
        found = uint32_t(at);
        break;
    }
    if (found == 0) {
        error = "no M2TX bundle contains the POD's texture pointer";
        return false;
    }

    Bundle bundle;
    if (!parse_bundle(payload, found, bundle)) {
        error = "M2TX bundle failed validation";
        return false;
    }
    const std::vector<Color> palette = read_palette(payload, bundle.pi);
    Color constants[2];
    read_constants(payload, bundle.ta, (bundle.flags & kHasColorConst) != 0,
                   constants);

    out.m2tx_offset = bundle.m2tx;
    if (!map_tpage_snippets(payload, ref, bundle, out, error)) return false;
    out.pages.resize(bundle.pages.size());
    out.page_errors.resize(bundle.pages.size());
    for (size_t i = 0; i < bundle.pages.size(); ++i) {
        decode_page(payload, bundle, bundle.pages[i], palette, constants,
                    out.pages[i], out.page_errors[i]);
    }
    return true;
}

}  // namespace launcher_assets
