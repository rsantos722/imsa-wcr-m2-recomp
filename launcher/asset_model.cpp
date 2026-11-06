// See asset_model.h for the format evidence behind this decoder.
#include "asset_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace launcher_assets {
namespace {

// Mercury index half-word control bits (Mercury Guide MER-20; the same five
// constants are `#define`d by the model compiler in gmerc/geo/tmutils.c).
//
// Triangle assembly consumes NEWS (start of a new strip/fan), STXT (the next
// half-word is a texture number), the index mask, and CFAN, which selects the
// fan update described at `read_triangles`. PCLK carries the previous
// triangle's orientation for the rasteriser's back-face test and PFAN mirrors
// CFAN one token earlier; neither changes which three vertices form a triangle.
[[maybe_unused]] constexpr uint16_t kPclk = 0x8000;  // previous tri was CCW
[[maybe_unused]] constexpr uint16_t kPfan = 0x4000;  // previous tri was a fan
constexpr uint16_t kCfan = 0x2000;  // current tri is a fan
constexpr uint16_t kNews = 0x1000;  // start of a new strip/fan ("snake")
constexpr uint16_t kStxt = 0x0800;  // the next half-word is a texture number
constexpr uint16_t kIndexMask = 0x07ff;  // up to 2048 vertices per model

constexpr size_t kPodSize = 0x24;      // flags..pmaterial, nine words
constexpr size_t kGeomSize = 0x30;     // six floats + pointers + counts
constexpr size_t kVertexSize = 24;     // xyz then the case-dependent tail
constexpr size_t kPodGeomOffset = 0x10;    // pgeometry word inside the POD
constexpr size_t kPodTextureOffset = 0x0c; // ptexture word inside the POD
constexpr size_t kPodNextOffset = 0x04;    // pnext word inside the POD

uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) |
           (uint32_t(d[at + 2]) << 8) | uint32_t(d[at + 3]);
}
uint16_t be16(const std::vector<uint8_t>& d, size_t at) {
    return uint16_t((uint32_t(d[at]) << 8) | uint32_t(d[at + 1]));
}
float befloat(const std::vector<uint8_t>& d, size_t at) {
    const uint32_t bits = be32(d, at);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
bool in_bounds(size_t at, size_t span, size_t limit) {
    return at <= limit && span <= limit - at;
}

// ---------------------------------------------------------------------------
// POD geometry
// ---------------------------------------------------------------------------

struct Geometry {
    uint32_t pvertex = 0;
    uint16_t vertexcount = 0;
    uint32_t pindex = 0;
    uint32_t puv = 0;
};

enum class Reject { Ok, Layout, Bounds, Index };

// Validate one PodGeometry record. Every rule below held on every inspected
// record of the showroom and ConfigCar assets; a record that does not satisfy
// them is rejected rather than shifted by a guessed field order.
Reject read_geometry(const std::vector<uint8_t>& p, uint32_t at, Geometry& out) {
    if (!in_bounds(at, kGeomSize, p.size()) || (at & 3u)) return Reject::Layout;
    for (int i = 0; i < 6; ++i) {
        const float v = befloat(p, at + size_t(i) * 4);
        if (!std::isfinite(v) || std::fabs(v) > 1e7f) return Reject::Layout;
        if (i >= 3 && v < 0.0f) return Reject::Layout;  // extents cannot be negative
    }
    const uint32_t pvertex = be32(p, at + 24);
    const uint32_t pshared = be32(p, at + 28);
    const uint16_t vertexcount = be16(p, at + 32);
    const uint16_t sharedcount = be16(p, at + 34);
    const uint32_t pindex = be32(p, at + 36);
    const uint32_t puv = be32(p, at + 40);
    // The showroom and ConfigCar pods ship no shared-vertex table; a record that
    // has one is left undecoded rather than shifted by a guessed stride.
    if (pshared != 0 || sharedcount != 0) return Reject::Layout;
    if (vertexcount < 3 || vertexcount > 2048) return Reject::Layout;
    if (!in_bounds(pvertex, size_t(vertexcount) * kVertexSize, p.size()))
        return Reject::Bounds;
    // The index array starts exactly where the vertex array ends.
    if (pindex != pvertex + size_t(vertexcount) * kVertexSize) return Reject::Layout;
    if (puv && (puv <= pindex || ((puv - pindex) & 1u))) return Reject::Layout;
    if (!in_bounds(pindex, puv ? size_t(puv - pindex) : 2, p.size()))
        return Reject::Bounds;
    out = {pvertex, vertexcount, pindex, puv};
    return Reject::Ok;
}

// Expand one POD's index half-words into a triangle list.
//
// The stream is a Mercury "snake": a triangle lives in three slots traversed as
// (oldest, middle, newest), and every index token introduces one new vertex that
// becomes the newest one. Which vertex it displaces is chosen by CFAN on that
// token: a fan replaces the middle vertex of the previous triangle, a strip
// replaces the oldest. NEWS ends the prim, and the token that carried it starts
// the next one. STXT marks a prim whose texture number (the bundle page the
// following strips draw from) is the next half-word; a negative texture number
// - which the compiler emits as its end marker, `STXT|NEWS|CFAN|pclk|pfan`
// followed by -1 - terminates the object.
//
// `read_triangles` follows the SDK runtime walker, `walk_index_array` in
// M2 SDK src/libs/mercury/libmercury3/M_BuildAAEdgeTable.c (its prim/slot handling is what feeds `M_BuildAAEdgeTable`
// the triangle each token completes). The Mercury Programmer's Guide's worked
// cube example (MER-16..MER-20, Tables 1-3 and 1-4) reproduces the same slot
// rules, and `m2_launcher_assets` pins both: the guide's cube stream is decoded
// to its documented triangles, and the showroom car chain comes out manifold.
// Two details are easy to get wrong and are load-bearing here: the first vertex
// of a prim is the token already read (the texture number is skipped, never used
// as a vertex), and the flags that steer a new vertex come from the token *after*
// it, which is how the vendored walker reads them.
//
// Each triangle is recorded in the order of the three containers - the guide's
// A->B->C path, and the order every case of the vendored walker names its edges
// in. That order is not a winding guarantee: the guide's Tables 1-3/1-4 show the
// orientation alternating triangle by triangle, and PCLK is what tells the
// rasteriser the previous triangle's orientation so its back-face test stays
// correct. The viewer shades two-sided and does not cull, so it relies on the
// triangle's vertices, not on their order.
//
// Requiring every index to be below `vertexcount` is the fail-closed guard: a
// mis-parsed token cannot index out of the vertex array.
struct PodDecode {
    Mesh mesh;
    std::vector<Vec2> uvs;        // one per triangle corner, in mesh.indices order
    std::vector<uint32_t> pages;  // one per triangle: the STXT page in effect
    std::vector<uint8_t> clockwise;
    bool uvs_ok = false;
};

// Walk the index half-words of one POD into a triangle list.
//
// `indices` receives three entries per triangle in slot order and `pages`
// receives the STXT page in effect for each of them. The walk stops at the
// object's end marker or when the index region runs out; an index outside the
// vertex array rejects the POD instead of drawing with it.
Reject read_triangles(const std::vector<uint8_t>& p, const Geometry& g,
                      std::vector<uint32_t>& indices,
                      std::vector<uint32_t>& pages,
                      std::vector<Vec2>& uvs, bool& uvs_ok,
                      std::vector<uint8_t>& clockwise) {
    const size_t words = (g.puv ? g.puv - g.pindex : p.size() - g.pindex) / 2;
    size_t pos = 0;
    auto next = [&](uint16_t& word) {
        if (pos >= words) return false;
        word = be16(p, g.pindex + pos * 2);
        ++pos;
        return true;
    };
    auto in_range = [&](uint32_t index) { return index < g.vertexcount; };
    // M_DrawDynLitTex/M_DrawPreLitTex advance puv with lfsu as each index
    // introduces a vertex; they never address puv using that vertex number.
    // Restarting a strip consumes fresh pairs, including for reused vertices.
    size_t uv_cursor = g.puv;
    uvs_ok = g.puv != 0;
    auto next_uv = [&]() {
        Vec2 value{};
        if (!g.puv) return value;
        if (!in_bounds(uv_cursor, 8, p.size())) {
            uvs_ok = false;
        } else {
            value = {befloat(p, uv_cursor), befloat(p, uv_cursor + 4)};
            if (!std::isfinite(value.u) || !std::isfinite(value.v) ||
                std::fabs(value.u) > 64.0f || std::fabs(value.v) > 64.0f) {
                uvs_ok = false;
                value = {};
            }
        }
        uv_cursor += 8;
        return value;
    };

    uint16_t token = 0;
    if (!next(token)) return Reject::Ok;  // an empty index array draws nothing
    uint32_t page = 0;
    bool stxt = (token & kStxt) != 0;
    for (;;) {
        if (stxt) {
            uint16_t number = 0;
            if (!next(number) || (number & 0x8000)) break;  // "texture -1" ends it
            page = number;
        }
        // A prim opens with its first three vertices; the vendored walker drops
        // the flags of the second and third token.
        uint32_t slot[3] = {uint32_t(token & kIndexMask), 0, 0};
        bool complete = true;
        for (int i = 1; i < 3; ++i) {
            uint16_t word = 0;
            if (!next(word)) complete = false;  // truncated stream
            else slot[i] = word & kIndexMask;
            token = word;
            if (!complete) break;
        }
        if (!complete) break;
        Vec2 slot_uv[3] = {next_uv(), next_uv(), {}};
        int tid0 = 0, tid1 = 1, tid2 = 2;
        for (;;) {
            const uint32_t newest = uint32_t(token & kIndexMask);
            if (!in_range(newest)) return Reject::Index;
            slot[tid2] = newest;
            slot_uv[tid2] = next_uv();
            for (const uint32_t index : slot)
                if (!in_range(index)) return Reject::Index;
            indices.push_back(slot[0]);
            indices.push_back(slot[1]);
            indices.push_back(slot[2]);
            uvs.insert(uvs.end(), std::begin(slot_uv), std::end(slot_uv));
            pages.push_back(page);
            uint16_t flags = 0;
            if (!next(flags)) return Reject::Index;
            // loadcont in M_DrawDynLitTex XORs the cross-product test with
            // this next token's prevclockwiseFLAG. Keep that orientation
            // separate from the slot-order topology used by the harness.
            clockwise.push_back((flags & kPclk) ? 1 : 0);
            token = flags;
            stxt = (flags & kStxt) != 0;
            if (flags & kNews) break;
            if (flags & kCfan) {
                std::swap(tid1, tid2);  // fan: the middle vertex is replaced next
            } else {
                const int oldest = tid0;  // strip: the oldest vertex is replaced
                tid0 = tid1;
                tid1 = tid2;
                tid2 = oldest;
            }
        }
    }
    return Reject::Ok;
}

Reject decode_pod(const std::vector<uint8_t>& p, const Geometry& g,
                  PodDecode& out) {
    Mesh& mesh = out.mesh;
    mesh.positions.reserve(g.vertexcount);
    mesh.normals.reserve(g.vertexcount);
    for (uint16_t i = 0; i < g.vertexcount; ++i) {
        const size_t at = g.pvertex + size_t(i) * kVertexSize;
        mesh.positions.push_back({befloat(p, at), befloat(p, at + 4),
                                  befloat(p, at + 8)});
        mesh.normals.push_back({befloat(p, at + 12), befloat(p, at + 16),
                                befloat(p, at + 20)});
    }
    const Reject result = read_triangles(p, g, mesh.indices, out.pages,
                                         out.uvs, out.uvs_ok, out.clockwise);
    if (!out.uvs_ok) std::fill(out.uvs.begin(), out.uvs.end(), Vec2{});
    return result;
}

struct Pod {
    uint32_t at = 0;
    uint32_t geometry = 0;
    uint32_t pnext = 0;
    uint32_t pcase = 0;
    uint32_t ptexture = 0;
    uint32_t pmaterial = 0;
};

// A chain link is either the end sentinel or an in-payload word offset.
bool plausible_link(uint32_t value, size_t payload) {
    return value == 0 || ((value & 3u) == 0 && value < payload);
}

void accumulate_bounds(Mesh& mesh) {
    if (mesh.positions.empty()) return;
    Vec3 lo = mesh.positions.front(), hi = lo;
    for (const Vec3& v : mesh.positions) {
        lo.x = std::min(lo.x, v.x);
        lo.y = std::min(lo.y, v.y);
        lo.z = std::min(lo.z, v.z);
        hi.x = std::max(hi.x, v.x);
        hi.y = std::max(hi.y, v.y);
        hi.z = std::max(hi.z, v.z);
    }
    mesh.bbox_min = lo;
    mesh.bbox_max = hi;
}

}  // namespace

const AssetChoice kShowroomChassisChoices[] = {
    {"Courage", "showroom/showroom.0.memimage"},
    {"Ferrari", "showroom/showroom.1.memimage"},
    {"Kudzu", "showroom/showroom.2.memimage"},
    {"Riley & Scott", "showroom/showroom.3.memimage"},
};
const int kShowroomChassisChoiceCount = int(std::size(kShowroomChassisChoices));
const AssetChoice kShowroomTireChoices[] = {
    {"Firestone", "showroom/showroom.tire.0.memimage"},
    {"Goodyear", "showroom/showroom.tire.1.memimage"},
    {"Pirelli", "showroom/showroom.tire.2.memimage"},
    {"Yokohama", "showroom/showroom.tire.3.memimage"},
};
const int kShowroomTireChoiceCount = int(std::size(kShowroomTireChoices));
const AssetChoice kRaceCarChoices[] = {
    {"cars1", "tracks/shared/cars1.memimage"},
    {"cars2", "tracks/shared/cars2.memimage"},
    {"cars3", "tracks/shared/cars3.memimage"},
    {"cars4", "tracks/shared/cars4.memimage"},
};
const int kRaceCarChoiceCount = int(std::size(kRaceCarChoices));

bool load_model(const std::filesystem::path& path, const std::string& name,
                Model& out, std::string& error) {
    std::error_code size_ec;
    const auto file_size = std::filesystem::file_size(path, size_ec);
    // A corrupt or oversized memimage must not be read into memory whole.
    if (size_ec || file_size > (64u << 20)) {
        error = "cannot open " + path.string();
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    if (raw.size() < 4) {
        error = "memimage is shorter than its payload header";
        return false;
    }
    const uint32_t payload_size = be32(raw, 0);
    if (payload_size == 0 || size_t(payload_size) > raw.size() - 4) {
        error = "memimage payload size does not fit the file";
        return false;
    }
    // The payload begins after the size word; every pointer inside it is
    // payload-relative (see asset_model.h).
    const std::vector<uint8_t> payload(raw.begin() + 4, raw.begin() + 4 + payload_size);

    // Collect every word-aligned POD record whose pgeometry fully validates.
    std::vector<Pod> pods;
    std::unordered_map<uint32_t, size_t> pod_by_offset;
    for (size_t at = 0; in_bounds(at, kPodSize, payload.size()); at += 4) {
        const uint32_t geometry = be32(payload, at + kPodGeomOffset);
        const uint32_t pnext = be32(payload, at + kPodNextOffset);
        if (geometry == 0 || !plausible_link(pnext, payload.size())) continue;
        Geometry g;
        if (read_geometry(payload, geometry, g) != Reject::Ok) continue;
        pod_by_offset.emplace(uint32_t(at), pods.size());
        pods.push_back({uint32_t(at), geometry, pnext,
                        be32(payload, at + 0x08),
                        be32(payload, at + kPodTextureOffset),
                        be32(payload, at + 0x20)});
    }

    struct Selection {
        uint32_t head, count = 0, group = 0;
        int body = -1, variant = -1, lod = -1;
        std::string label;
        std::string component = "chain";
    };
    std::vector<Selection> selections;
    const std::string stem = path.stem().string();
    const bool race_pack = stem == "cars1" || stem == "cars2" ||
                           stem == "cars3" || stem == "cars4";
    if (race_pack) {
        // readinmemimage/getnextmemimagepointer: trailer starts with the
        // exported root count and payload-relative root offsets. trackInit
        // publishes the first root as TheCarBodies. MS_buildcar indexes it
        // as body*0x18 + variant*4 (and +0x60 for damage replacements used
        // by MS_addbody at LOD 0, per the individual damage values).
        const size_t trailer = size_t(payload_size) + 4;
        if (!in_bounds(trailer, 16, raw.size()) || be32(raw, trailer) != 3) {
            error = "race pack has no three-root memimage table";
            return false;
        }
        const uint32_t bodies = be32(raw, trailer + 4);
        if (!in_bounds(bodies, 0xc0, payload.size())) {
            error = "TheCarBodies table is outside the payload";
            return false;
        }
        auto group = [&](uint32_t at, int body, int variant,
                         const std::string& label, const std::string& component) {
            if (!at) return true;  // source-defined absent slot
            if (!in_bounds(at, 0x1c, payload.size())) return false;
            const uint32_t levels = be32(payload, at + 0x18);
            if (levels == 0 || levels > 4 ||
                !in_bounds(at + 0x1c, size_t(levels) * 12, payload.size()))
                return false;
            for (uint32_t lod = 0; lod < levels; ++lod) {
                const uint32_t count = be32(payload, at + 0x20 + lod * 12);
                const uint32_t first = be32(payload, at + 0x24 + lod * 12);
                if (!count || count > 64 ||
                    !in_bounds(first, size_t(count) * kPodSize, payload.size()))
                    return false;
                for (uint32_t pod = 0; pod < count; ++pod)
                    if (!pod_by_offset.count(first + pod * kPodSize)) return false;
                selections.push_back({first, count, at, body, variant, int(lod),
                                      label + " / LOD " + std::to_string(lod),
                                      component});
            }
            return true;
        };
        for (int body = 0; body < 4; ++body) {
            for (int variant = 0; variant < 3; ++variant) {
                const size_t slot = bodies + body * 0x18 + variant * 4;
                const std::string label = "Body " + std::to_string(body) +
                                          " / Variant " + std::to_string(variant);
                if (!group(be32(payload, slot), body, variant, label, "body") ||
                    !group(be32(payload, slot + 0x60), body, variant,
                           label + " / damage replacements", "damage")) {
                    error = "race body/variant LOD group failed validation";
                    return false;
                }
            }
        }
        const uint32_t tires = be32(raw, trailer + 8);
        if (!in_bounds(tires, 4, payload.size()) ||
            !group(be32(payload, tires), -1, -1, "Race wheel", "wheel")) {
            error = "race wheel LOD group failed validation";
            return false;
        }
    }

    // Loose assets use authored links. Race groups use contiguous POD arrays:
    // copied variants retain stale pnext links into variant 0 until MS_addgroup
    // overwrites them. Following those links would mix different variants.
    std::unordered_set<uint32_t> linked;
    for (const Pod& pod : pods)
        if (pod.pnext != 0) linked.insert(pod.pnext);
    std::vector<uint32_t> heads;
    for (const Pod& pod : pods)
        if (!linked.count(pod.at)) heads.push_back(pod.at);
    std::sort(heads.begin(), heads.end());
    if (!race_pack)
        for (const uint32_t head : heads) {
            Selection sel;
            sel.head = head;
            selections.push_back(std::move(sel));
        }

    out = Model{};
    out.name = name;
    out.race_pack = race_pack;
    if (stem.size() == 10 && stem.rfind("showroom.", 0) == 0 &&
        stem[9] >= '0' && stem[9] <= '3') {
        // showRoomInit@0x244d4 takes the first exported root (offset zero is
        // valid). MS_addcar_showroom@0x23fdc..0x24040 consumes its four local
        // matrices at +0x30..+0x3c, including translation AND axle scale.
        const size_t trailer = size_t(payload_size) + 4;
        if (!in_bounds(trailer, 28, raw.size()) || be32(raw, trailer) != 6) {
            error = "showroom has no six-root memimage table";
            return false;
        }
        const uint32_t car = be32(raw, trailer + 4);
        if (!in_bounds(car, 0x40, payload.size())) {
            error = "showroom car descriptor is outside the payload";
            return false;
        }
        for (size_t wheel = 0; wheel < 4; ++wheel) {
            const uint32_t matrix = be32(payload, car + 0x30 + wheel * 4);
            if (!matrix || !in_bounds(matrix, 48, payload.size())) {
                error = "showroom wheel matrix is outside the payload";
                return false;
            }
            for (size_t j = 0; j < 12; ++j) {
                const float value = befloat(payload, matrix + j * 4);
                if (!std::isfinite(value)) {
                    error = "showroom wheel matrix is not finite";
                    return false;
                }
                out.wheel_transforms[wheel].m[j] = value;
            }
        }
        out.wheel_count = 4;
    }
    size_t best_pods = 0;
    int rejected = 0;
    int decoded_pages = 0, failed_pages = 0, untextured = 0;
    std::string texture_error;
    for (const Selection& selection : selections) {
        Part part;
        part.chain_head = selection.head;
        part.label = selection.label;
        part.component = selection.component;
        part.body = selection.body;
        part.variant = selection.variant;
        part.lod = selection.lod;
        part.group_offset = selection.group;
        uint32_t cursor = selection.head;
        // PODs of one chain can share a texture bundle; the base index of an
        // already-decoded one is reused so each bundle is decoded once.
        struct CachedTexture {
            uint32_t base = kNoTexture;
            std::vector<uint32_t> stxt_to_page;
        };
        std::unordered_map<uint32_t, CachedTexture> bundle_cache;
        // A group cannot hold more records than the payload contains.
        for (size_t guard = 0; guard <= pods.size(); ++guard) {
            if (selection.count && guard >= selection.count) break;
            const auto found = pod_by_offset.find(cursor);
            if (found == pod_by_offset.end()) break;
            const Pod& pod = pods[found->second];
            ++part.pod_count;
            part.pod_offsets.push_back(cursor);
            part.pod_pcase.push_back(pod.pcase);
            part.pod_ptexture.push_back(pod.ptexture);
            part.pod_pmaterial.push_back(pod.pmaterial);
            Geometry g;
            PodDecode decoded;
            if (read_geometry(payload, pod.geometry, g) != Reject::Ok ||
                decode_pod(payload, g, decoded) != Reject::Ok) {
                ++rejected;
            } else {
                const uint32_t base = uint32_t(part.mesh.positions.size());
                part.vertex_count += g.vertexcount;
                part.triangle_count += uint32_t(decoded.mesh.indices.size() / 3);
                for (const uint32_t index : decoded.mesh.indices)
                    part.mesh.indices.push_back(index + base);
                part.mesh.positions.insert(part.mesh.positions.end(),
                                           decoded.mesh.positions.begin(),
                                           decoded.mesh.positions.end());
                part.mesh.normals.insert(part.mesh.normals.end(),
                                         decoded.mesh.normals.begin(),
                                         decoded.mesh.normals.end());
                part.uvs.insert(part.uvs.end(), decoded.uvs.begin(),
                                decoded.uvs.end());
                part.triangle_clockwise.insert(part.triangle_clockwise.end(),
                                               decoded.clockwise.begin(), decoded.clockwise.end());

                // The POD's `ptexture` record names the bundle whose first
                // page `ptexture` points into; `STXT` tokens name the page of
                // each strip. Anything this decoder cannot resolve leaves the
                // affected triangles flat and is counted, never guessed at.
                uint32_t texture_base = kNoTexture;
                uint32_t texture_pages = 0;
                std::vector<uint32_t> stxt_to_page;
                const uint32_t pod_texture = pod.ptexture;
                if (pod_texture != 0 && !decoded.uvs_ok) {
                    if (texture_error.empty())
                        texture_error = "a POD has no usable (u,v) array";
                } else if (pod_texture != 0) {
                    const auto cached = bundle_cache.find(pod_texture);
                    if (cached != bundle_cache.end()) {
                        texture_base = cached->second.base;
                        texture_pages =
                            uint32_t(cached->second.stxt_to_page.size());
                        stxt_to_page = cached->second.stxt_to_page;
                    } else {
                        PodTextureRef ref;
                        TextureBundle bundle;
                        std::string error;
                        if (read_pod_texture_ref(payload, pod_texture, ref) &&
                            decode_pod_texture(payload, ref, bundle, error)) {
                            texture_base = uint32_t(part.textures.size());
                            texture_pages = uint32_t(bundle.pages.size());
                            stxt_to_page = bundle.stxt_to_page;
                            for (size_t i = 0; i < bundle.pages.size(); ++i) {
                                if (bundle.pages[i].empty()) {
                                    ++failed_pages;
                                    if (texture_error.empty())
                                        texture_error = bundle.page_errors[i];
                                } else {
                                    ++decoded_pages;
                                }
                                part.textures.push_back(
                                    std::move(bundle.pages[i]));
                            }
                        } else if (texture_error.empty()) {
                            texture_error = error;
                        }
                        bundle_cache.emplace(
                            pod_texture,
                            CachedTexture{texture_base, stxt_to_page});
                    }
                }
                // A triangle only samples a page that decoded: a refused page
                // leaves the triangle flat instead of pointing at an empty
                // image the renderer would have to second-guess.
                size_t flat = 0;
                for (const uint32_t page : decoded.pages) {
                    const uint32_t mapped =
                        page < stxt_to_page.size() ? stxt_to_page[page] : kNoTexture;
                    const bool usable =
                        texture_base != kNoTexture && mapped < texture_pages &&
                        !part.textures[texture_base + mapped].empty();
                    if (!usable) ++flat;
                    part.triangle_texture.push_back(usable ? texture_base + mapped
                                                           : kNoTexture);
                }
                if (pod_texture != 0 && flat != 0) ++untextured;
            }
            if (selection.count) cursor += uint32_t(kPodSize);
            else {
                if (pod.pnext == 0) break;
                cursor = pod.pnext;
            }
        }
        if (part.pod_count == 0) continue;
        accumulate_bounds(part.mesh);
        if ((!race_pack && part.pod_count > best_pods) ||
            (race_pack && part.component == "body" && part.body == 0 &&
             part.variant == 0 && part.lod == 0)) {
            best_pods = part.pod_count;
            out.default_part = int(out.parts.size());
        }
        out.parts.push_back(std::move(part));
    }
    out.rejected_pods = rejected;
    out.texture_pages_decoded = decoded_pages;
    out.texture_pages_failed = failed_pages;
    out.untextured_pods = untextured;
    if (out.texture_error.empty()) out.texture_error = texture_error;

    if (out.parts.empty()) {
        error = "no POD chain in " + path.filename().string() + " passed validation";
        return false;
    }
    if (out.default_part < 0) out.default_part = 0;
    return true;
}

}  // namespace launcher_assets
