// savestate.cpp — implementation: see savestate.h for format/design.
#include "savestate.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <fstream>
#include <iterator>
#include <vector>

#include "event_broker.h"

extern "C" {
#include "recomp_host.h"
#include "recomp_helpers.h"
#include "guest_provenance.h"
}

// Generated link bases (game_data.c) — recorded in states for diagnostics.
extern unsigned char g_recomp_text[];
extern unsigned char g_recomp_data[];
extern unsigned g_recomp_text_base;
extern unsigned g_recomp_data_base;
extern unsigned g_recomp_bss_base;

namespace m2host {
namespace savestate {

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
constexpr size_t kHeaderSize =
    8 +      // magic + version
    256 +    // gpr[32] + fpr[32]
    16 +     // cr + lr + ctr + xer
    12 +     // guest_mem_size + page_size + encoding
    12 +     // text_base + data_base + bss_base
    16 +     // bitmap_bytes + payload_bytes
    8 + 8;   // reserved + stamped hash  => 336

inline void fnv_mix(uint64_t& h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
inline void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
struct Reader {
    const uint8_t* p;
    size_t n, off = 0;
    bool take(size_t k, const uint8_t** out) {
        if (off + k > n) return false;
        *out = p + off; off += k; return true;
    }
    bool u32(uint32_t* v) {
        const uint8_t* q; if (!take(4, &q)) return false;
        *v = uint32_t(q[0]) | uint32_t(q[1]) << 8 |
             uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24;
        return true;
    }
    bool u64(uint64_t* v) {
        uint32_t lo, hi; if (!u32(&lo) || !u32(&hi)) return false;
        *v = uint64_t(lo) | uint64_t(hi) << 32; return true;
    }
};

void put_regs(std::vector<uint8_t>& b, const PPCContext& c) {
    for (int i = 0; i < 32; ++i) put_u32(b, c.gpr[i]);
    for (int i = 0; i < 32; ++i) {
        uint32_t bits; std::memcpy(&bits, &c.fpr[i], 4);
        put_u32(b, bits);
    }
    put_u32(b, c.cr); put_u32(b, c.lr);
    put_u32(b, c.ctr); put_u32(b, c.xer);
}

uint64_t regs_hash(const PPCContext& c) {
    uint64_t h = kFnvOffset;
    // Registers are hashed through a canonical little-endian byte image so
    // the digest does not depend on host struct padding or endianness.
    for (int i = 0; i < 32; ++i) { uint32_t v = c.gpr[i]; fnv_mix(h, &v, 4); }
    for (int i = 0; i < 32; ++i) {
        uint32_t v; std::memcpy(&v, &c.fpr[i], 4); fnv_mix(h, &v, 4);
    }
    fnv_mix(h, &c.cr, 4); fnv_mix(h, &c.lr, 4);
    fnv_mix(h, &c.ctr, 4); fnv_mix(h, &c.xer, 4);
    return h;
}

std::filesystem::path temp_sibling(const std::filesystem::path& final) {
    return final.parent_path() /
           ("." + final.filename().string() + ".tmp");
}

}  // namespace

uint64_t state_hash_of(const PPCContext& ctx) {
    uint64_t h = regs_hash(ctx);
    if (ctx.mem)
        fnv_mix(h, ctx.mem, RECOMP_GUEST_MEM);
    return h;
}

uint64_t state_hash() { return state_hash_of(*m2_recomp_context()); }

SaveResult save_to_file(const std::filesystem::path& path) {
    SaveResult res;
    PPCContext* c = m2_recomp_context();
    if (!c || !c->mem) {
        res.error = "no live guest context";
        return res;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Live-state digest FIRST: the stamp must describe the machine that the
    // sparse encode below captures. Digesting after the encode would let a
    // concurrent guest-RAM writer stamp a state the payload does not contain
    // and make the load-time semantic seal mismatch silently. Any mutation
    // landing inside the encode window now shows up as a loud seal mismatch.
    const uint64_t live_hash = state_hash_of(*c);

    // Sparse-encode RAM: bitmap of nonzero 4 KiB pages + present pages.
    constexpr size_t kPage = kRamPageSize;
    constexpr size_t kPages = RECOMP_GUEST_MEM / kPage;
    std::vector<uint8_t> bitmap(kPages / 8, 0);
    std::vector<uint8_t> payload;
    payload.reserve(RECOMP_GUEST_MEM / 2);
    for (size_t pg = 0; pg < kPages; ++pg) {
        const uint8_t* src = c->mem + pg * kPage;
        bool nonzero = false;
        for (size_t i = 0; i < kPage; i += 16) {
            uint64_t chunk; std::memcpy(&chunk, src + i, 8);
            uint64_t chunk2; std::memcpy(&chunk2, src + i + 8, 8);
            if ((chunk | chunk2) != 0) { nonzero = true; break; }
        }
        if (nonzero) {
            bitmap[pg >> 3] |= uint8_t(1u << (pg & 7));
            payload.insert(payload.end(), src, src + kPage);
        }
    }

    std::vector<uint8_t> file;
    file.reserve(kHeaderSize + bitmap.size() + payload.size());

    put_u32(file, kMagic);
    put_u32(file, kVersion);
    put_regs(file, *c);
    put_u32(file, RECOMP_GUEST_MEM);
    put_u32(file, kPage);
    put_u32(file, 1);  // encoding: sparse 4 KiB pages
    put_u32(file, g_recomp_text_base);
    put_u32(file, g_recomp_data_base);
    put_u32(file, g_recomp_bss_base);
    put_u64(file, bitmap.size());
    put_u64(file, payload.size());
    put_u32(file, uint32_t(live_hash));
    put_u32(file, uint32_t(live_hash >> 32));  // state_hash (was reserved)
    put_u64(file, 0);                          // stream stamp placeholder

    // Extra kernel state (versioned M2EX record) so a savestate restores the
    // same guest state in another process: heap cursor, item/small-message generations and
    // occupancy, TE mirrors, broker occupancy, kernel signal scalars, task
    // records, PPC registers, and mapped continuation PCs. Process-local
    // pointers and native stacks are not serialized. See
    // m2_guest_extra_state_{save,load} in recomp_runtime.c.
    std::vector<uint8_t> extra;
#ifdef M2_HAVE_RECOMP
    {
        size_t extra_cap = m2_guest_extra_state_size();
        extra.resize(extra_cap);
        size_t out_len=0;
        if(m2_guest_extra_state_save(extra.data(), extra_cap, &out_len)==0){
            extra.resize(out_len);
        } else {
            extra.clear();
        }
    }
#endif
    uint64_t h = kFnvOffset;
    fnv_mix(h, file.data(), file.size());
    fnv_mix(h, bitmap.data(), bitmap.size());
    fnv_mix(h, payload.data(), payload.size());
#ifdef M2_HAVE_RECOMP
    fnv_mix(h, extra.data(), extra.size());
#endif
    for (int i = 0; i < 8; ++i)
        file[file.size() - 8 + i] = uint8_t(h >> (8 * i));

    file.insert(file.end(), bitmap.begin(), bitmap.end());
    file.insert(file.end(), payload.begin(), payload.end());
#ifdef M2_HAVE_RECOMP
    file.insert(file.end(), extra.begin(), extra.end());
#endif

    const auto tmp = temp_sibling(path);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            res.error = "cannot open " + tmp.string();
            return res;
        }
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            res.error = "short write to " + tmp.string();
            return res;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        res.error = "rename failed: " + ec.message();
        return res;
    }
    res.ok = true;
    res.hash = live_hash;
    res.path = path;
    std::printf("[savestate] saved %s (%zu bytes, %zu/%zu pages dirty, hash %016llx)\n",
                path.string().c_str(), file.size(), payload.size() / kPage, kPages,
                static_cast<unsigned long long>(live_hash));
    return res;
}

SaveResult load_from_file(const std::filesystem::path& path) {
    SaveResult res;
    PPCContext* c = m2_recomp_context();
    if (!c || !c->mem) {
        res.error = "no live guest context";
        return res;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        res.error = "cannot open " + path.string();
        return res;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (file.size() < kHeaderSize) {
        res.error = "truncated header (" + path.string() + ")";
        return res;
    }

    Reader r{file.data(), file.size()};
    auto fail = [&](const char* why) {
        res.path = path;
        res.error = why;
        return res;
    };
    uint32_t magic, version, mem_size, page_size, encoding;
    uint32_t text_base, data_base, bss_base, junk, junk2;
    uint64_t bitmap_bytes, payload_bytes, stamp, state_hash_stored;
    uint32_t gpr[32], fpr[32], cr, lr, ctr, xer;
    if (!r.u32(&magic) || !r.u32(&version)) return fail("header read");
    if (magic != kMagic) return fail("bad magic");
    if (version != kVersion) return fail("unsupported version");
    for (int i = 0; i < 32; ++i)
        if (!r.u32(&gpr[i])) return fail("gpr read");
    for (int i = 0; i < 32; ++i)
        if (!r.u32(&fpr[i])) return fail("fpr read");
    if (!r.u32(&cr) || !r.u32(&lr) || !r.u32(&ctr) || !r.u32(&xer))
        return fail("spr read");
    if (!r.u32(&mem_size) || !r.u32(&page_size) || !r.u32(&encoding))
        return fail("geometry read");
    (void)text_base; (void)data_base; (void)bss_base;  // informational
    if (!r.u32(&text_base) || !r.u32(&data_base) || !r.u32(&bss_base))
        return fail("base read");
    if (!r.u64(&bitmap_bytes) || !r.u64(&payload_bytes)) return fail("sizes read");
    if (!r.u32(&junk) || !r.u32(&junk2)) return fail("state hash read");
    state_hash_stored = uint64_t(junk) | uint64_t(junk2) << 32;
    if (!r.u64(&stamp)) return fail("hash read");

    if (mem_size != RECOMP_GUEST_MEM) return fail("guest RAM size mismatch");
    if (page_size != kRamPageSize || encoding != 1) return fail("RAM encoding mismatch");
    const size_t want_bitmap = RECOMP_GUEST_MEM / kRamPageSize / 8;
    if (bitmap_bytes != want_bitmap) return fail("bitmap size mismatch");

    // Overflow-safe framing. A crafted header can make
    // `kHeaderSize + bitmap_bytes + payload_bytes` wrap uint64 (for example
    // payload_bytes = UINT64_MAX), which made a short file look large enough
    // and sent the hasher and decode off the end of the buffer. Validate each
    // length against the bytes actually present; subtract only after proving
    // the subtrahend fits.
    const size_t body_bytes = file.size() - kHeaderSize;
    if (bitmap_bytes > body_bytes) return fail("truncated bitmap");
    const size_t after_bitmap = body_bytes - static_cast<size_t>(bitmap_bytes);
    if (payload_bytes > after_bitmap) return fail("payload size mismatch");
    const size_t extra_bytes =
        after_bitmap - static_cast<size_t>(payload_bytes);

    const uint8_t* bitmap_ptr = file.data() + kHeaderSize;
    const uint8_t* payload_ptr = bitmap_ptr + bitmap_bytes;
    const uint8_t* extra_ptr = payload_ptr + payload_bytes;

    // The sparse page table must describe exactly payload_bytes of present
    // pages. Validate population before hashing (the stream stamp reads every
    // payload byte) and before the decode memcpys (which would otherwise read
    // past payload_ptr when the bitmap claims more pages than were stored).
    size_t present = 0;
    for (size_t i = 0; i < want_bitmap; ++i)
        present += static_cast<size_t>(std::popcount(bitmap_ptr[i]));
    if (present > SIZE_MAX / kRamPageSize ||
        present * kRamPageSize != payload_bytes)
        return fail("bitmap/payload page count mismatch");

    // Verify the stamp before touching live state. Byte stream must match
    // save_to_file exactly: header with the stamp field zeroed, then bitmap,
    // then payload, then extra (versioned M2EX kernel/task state).
    uint64_t h = kFnvOffset;
    const uint64_t zero = 0;
    fnv_mix(h, file.data(), kHeaderSize - 8);
    fnv_mix(h, &zero, 8);
    fnv_mix(h, bitmap_ptr, bitmap_bytes);
    fnv_mix(h, payload_ptr, payload_bytes);
    fnv_mix(h, extra_ptr, extra_bytes);
    if (h != stamp)
        return fail("hash mismatch (corrupt or foreign state)");

    // Decode into a scratch RAM image first: the live machine is not touched
    // until every integrity and semantic check has passed.
    std::vector<uint8_t> scratch(RECOMP_GUEST_MEM, 0);
    size_t decoded = 0;
    for (size_t pg = 0; pg < RECOMP_GUEST_MEM / kRamPageSize; ++pg) {
        if (bitmap_ptr[pg >> 3] & (1u << (pg & 7))) {
            std::memcpy(scratch.data() + pg * kRamPageSize,
                        payload_ptr + decoded, kRamPageSize);
            decoded += kRamPageSize;
        }
    }
    if (decoded != payload_bytes)
        return fail("page count inconsistent");  // unreachable: validated above

    // Semantic seal: the decoded machine (registers + restored RAM image)
    // must reproduce the digest stamped at save time. Catches anything the
    // stream stamp cannot see, e.g. a re-stamped foreign file whose bytes
    // are internally consistent but do not describe the claimed state.
    // Semantic-seal caveat: `state_hash` spans registers + restored RAM only;
    // the host-side kernel structs (g_gk, m2k_context) are NOT part of that
    // digest. Their portable scalar state is restored separately via the M2EX
    // extra record (m2_guest_extra_state_load), whose size is sealed by the
    // file stream FNV stamp. Because the two digests are over disjoint state,
    // a mismatch here means the RAM/register image differs from what was
    // stamped (foreign or re-stamped file); kernel scalar state is not the
    // cause. Fail closed before touching the live machine: restoring a
    // foreign RAM/register image while retaining the current host kernel
    // epoch would create a mixed, non-reproducible guest state.
    {
        PPCContext probe;
        std::memset(&probe, 0, sizeof(probe));
        for (int i = 0; i < 32; ++i) probe.gpr[i] = gpr[i];
        for (int i = 0; i < 32; ++i) std::memcpy(&probe.fpr[i], &fpr[i], 4);
        probe.cr = cr; probe.lr = lr; probe.ctr = ctr; probe.xer = xer;
        probe.mem = scratch.data();
        uint64_t computed = state_hash_of(probe);
        if (computed != state_hash_stored) {
            return fail("state hash mismatch (RAM/register seal differs)");
        }
    }

    // Validate and restore the versioned host-side record before committing
    // the RAM image. The M2EX record is part of the savestate contract; a
    // malformed extension must fail closed instead of reporting a successful
    // RAM restore with a live kernel context from another epoch.
#ifdef M2_HAVE_RECOMP
    if (extra_bytes > 0) {
        if (m2_guest_extra_state_load(extra_ptr, extra_bytes) != 0)
            return fail("extra state validation failed");
        std::printf("[savestate] extra state restored (%zu bytes, M2EX kernel/task state)\n",
                    extra_bytes);
    }
#endif

    // Commit: registers, then whole-image RAM restore.
    for (int i = 0; i < 32; ++i) c->gpr[i] = gpr[i];
    for (int i = 0; i < 32; ++i) std::memcpy(&c->fpr[i], &fpr[i], 4);
    c->cr = cr; c->lr = lr; c->ctr = ctr; c->xer = xer;
    std::memcpy(c->mem, scratch.data(), RECOMP_GUEST_MEM);
    // Savestate restore is another host-side guest-RAM mutation. Record it
    // after the copy so watched entries contain the restored values.
    m2_provenance_record_host_write(c->mem, 0, RECOMP_GUEST_MEM, 0, c->lr, 0);
    res.ok = true;
    res.hash = state_hash_stored;
    res.path = path;
    std::printf("[savestate] loaded %s (%zu pages restored, hash %016llx)\n",
                path.string().c_str(), present,
                static_cast<unsigned long long>(res.hash));
    return res;
}

std::filesystem::path default_save_dir() {
    if (const char* env = std::getenv("M2_SAVESTATE_DIR"); env && *env)
        return env;
    return std::filesystem::current_path() / "saves";
}

// --- Hotkeys -----------------------------------------------------------------

// SDL keycodes for F5/F9 (= USB HID usage | 0x40000000, matching the values
// EventBroker forwards in M2EventRecord.data). Kept literal so this module
// needs no SDL headers.
constexpr uint32_t kKeyF5 = 0x4000003Eu;
constexpr uint32_t kKeyF9 = 0x40000043u;

Hotkeys::Hotkeys(std::filesystem::path dir) : m_dir(std::move(dir)) {}

bool Hotkeys::handle_event(uint16_t what, int32_t fresh_down, uint32_t keycode) {
    if (what != M2_EV_KEY || !fresh_down) return false;
    if (keycode == kKeyF5) return save_next_slot();
    if (keycode == kKeyF9) return load_latest();
    return false;
}

bool Hotkeys::save_next_slot() {
    PPCContext* c = m2_recomp_context();
    if (!c || !c->mem) return false;
    const uint64_t h = state_hash_of(*c);
    char name[64];
    std::snprintf(name, sizeof(name), "slot%d_%016llx.state", m_next_slot,
                  static_cast<unsigned long long>(h));
    SaveResult r = save_to_file(m_dir / name);
    if (!r.ok) {
        std::printf("[savestate] SAVE FAILED slot%d: %s\n", m_next_slot,
                    r.error.c_str());
        return false;
    }
    m_last_slot = m_next_slot;
    m_next_slot = m_next_slot % 8 + 1;  // rotate 1..8
    return true;
}

bool Hotkeys::load_slot(int slot) {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    std::filesystem::path best;
    auto ft = std::filesystem::file_time_type::min();
    for (const auto& e : std::filesystem::directory_iterator(m_dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("slot" + std::to_string(slot) + "_", 0) != 0) continue;
        auto t = e.last_write_time(ec);
        if (!ec && t > ft) { ft = t; best = e.path(); }
    }
    if (best.empty()) {
        std::printf("[savestate] no state for slot%d\n", slot);
        return false;
    }
    SaveResult r = load_from_file(best);
    if (!r.ok) {
        std::printf("[savestate] LOAD FAILED: %s\n", r.error.c_str());
        return false;
    }
    m_last_slot = slot;
    return true;
}

bool Hotkeys::load_latest() {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    std::filesystem::path best;
    auto ft = std::filesystem::file_time_type::min();
    for (const auto& e : std::filesystem::directory_iterator(m_dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("slot", 0) != 0 ||
            n.find(".state", n.size() - 6) == std::string::npos)
            continue;
        auto t = e.last_write_time(ec);
        if (!ec && t > ft) { ft = t; best = e.path(); }
    }
    if (best.empty()) {
        std::printf("[savestate] no saved states in %s\n", m_dir.string().c_str());
        return false;
    }
    SaveResult r = load_from_file(best);
    if (!r.ok) {
        std::printf("[savestate] LOAD FAILED: %s\n", r.error.c_str());
        return false;
    }
    return true;
}

}  // namespace savestate
}  // namespace m2host
