// savestate_test.cpp — round-trip ctest (save → load → hash equal).
//
// Exercises the full savestate pipeline against the real recomp guest image:
//   1. Deterministically scribble RAM (sparse + dense regions) and registers.
//   2. state_hash() -> save_to_file(); filename must embed the stamped hash.
//   3. Mutate the machine (registers + previously-zero AND previously-dirty
//      RAM) — hash must change.
//   4. load_from_file(): hash returns to the saved value; canaries restored.
//   5. Negative cases: corrupted payload rejected (state untouched), bad
//      magic, truncation, missing file.
//   6. F5/F9 hotkey handler semantics on synthetic broker events.
//
// Exit code 0 = all checks passed (ctest name: m2_savestate_roundtrip).
#include "savestate.h"
#include "event_broker.h"  // M2_EV_KEY

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include "recomp_host.h"
#include "recomp_helpers.h"
#include "guest_kernel.h"
}

using m2host::savestate::load_from_file;
using m2host::savestate::save_to_file;
using m2host::savestate::state_hash_of;

namespace {

int g_failures = 0;

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
constexpr size_t kSavestateHeaderSize = 336;

// Header field offsets (see savestate.cpp kHeaderSize composition).
constexpr size_t kOffMemSize = 280;
constexpr size_t kOffPageSize = 284;
constexpr size_t kOffEncoding = 288;
constexpr size_t kOffBitmapBytes = 304;
constexpr size_t kOffPayloadBytes = 312;

void fnv_mix(uint64_t* h, const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        *h ^= bytes[i];
        *h *= kFnvPrime;
    }
}

void put_u32_at(std::string* blob, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        (*blob)[off + i] = char(uint8_t(v >> (8 * i)));
}

void put_u64_at(std::string* blob, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        (*blob)[off + i] = char(uint8_t(v >> (8 * i)));
}

uint64_t get_u64_at(const std::string& blob, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(uint8_t(blob[off + i])) << (8 * i);
    return v;
}

// Keep the semantic-seal negative test internally consistent at the stream
// layer: changing a register and rewriting only the stream stamp must still
// be rejected by load_from_file() before it touches the live machine.
void restamp_stream(std::string* blob) {
    if (!blob || blob->size() < kSavestateHeaderSize) return;
    uint64_t h = kFnvOffset;
    fnv_mix(&h, reinterpret_cast<const uint8_t*>(blob->data()),
            kSavestateHeaderSize - 8);
    const uint64_t zero = 0;
    fnv_mix(&h, reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
    fnv_mix(&h,
            reinterpret_cast<const uint8_t*>(blob->data()) + kSavestateHeaderSize,
            blob->size() - kSavestateHeaderSize);
    for (int i = 0; i < 8; ++i)
        (*blob)[kSavestateHeaderSize - 8 + i] =
            char(uint8_t(h >> (8 * i)));
}

void check(bool cond, const char* what) {
    if (cond)
        std::printf("[savestate-test] PASS %s\n", what);
    else {
        std::printf("[savestate-test] FAIL %s\n", what);
        ++g_failures;
    }
}

// Small deterministic LCG so failures reproduce.
uint32_t lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

struct ByteWrite {
    uint32_t addr;
    uint8_t val;
};

// The sparse scatter used by every epoch; tests regenerate it to pick
// canary addresses without duplicating the stream math.
void gen_sparse_writes(uint32_t seed, std::vector<ByteWrite>* out) {
    uint32_t s = seed;
    out->clear();
    constexpr uint32_t kPages = uint32_t(RECOMP_GUEST_MEM) / 4096u;
    for (uint32_t pg = 0; pg < 4096; ++pg) {
        const uint32_t page = (lcg(s) % kPages) * 4096u;
        for (int k = 0; k < 96; ++k)
            out->push_back({page + lcg(s) % 4096u, uint8_t(lcg(s))});
    }
}

constexpr uint32_t kDenseBase = 0x176070u + 0x1000u * 3;  // past data image

void scribble(PPCContext* c, uint32_t epoch_tag,
              const std::vector<ByteWrite>& sparse) {
    for (const auto& w : sparse) c->mem[w.addr] = w.val ^ uint8_t(epoch_tag);
    // Dense region straddling several pages near the data image.
    std::memset(c->mem + kDenseBase, uint8_t(epoch_tag), 4096 * 4);
    // Registers: distinctive per-epoch values incl. FPR bit patterns that are
    // NaN/inf-adjacent (must survive as bits, not through float casts).
    for (int i = 0; i < 32; ++i) {
        c->gpr[i] = 0x10000u * epoch_tag + i;
        const uint32_t fb = 0x7f800000u | (epoch_tag << 8) | uint32_t(i);
        std::memcpy(&c->fpr[i], &fb, 4);
    }
    c->cr = 0x2468ACE0u ^ epoch_tag;
    c->lr = 0xdead0000u + epoch_tag;
    c->ctr = 0x0000bEEFu + epoch_tag;
    c->xer = 0x80000000u | (epoch_tag << 16);
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path dir =
        argc > 1 ? fs::path(argv[1])
                 : fs::temp_directory_path() / "m2_savestate_test";
    std::error_code ec;
    fs::create_directories(dir, ec);

    PPCContext* c = m2_recomp_context();
    check(c && c->mem, "guest context available");
    check(m2_guest_prepare_restore() == 0,
          "guest kernel prepared for M2EX restore");

    // Fill the original Message range and cross the extension boundary before
    // saving. The queued extension descriptor is the state that must survive
    // load; the intervening mutation proves stale/reused extension handles do
    // not alias it.
    m2k_context* kernel_context = m2_guest_kernel_context();
    const m2k_item_t extension_port =
        kernel_context ? m2k_create_msgport(kernel_context) : M2K_ITEM_NONE;
    std::vector<m2k_item_t> held_messages;
    held_messages.reserve(M2K_MAX_MESSAGES + M2K_MAX_EXT_MESSAGES);
    for (uint32_t i = 0; i < M2K_MAX_MESSAGES + M2K_MAX_EXT_MESSAGES; ++i)
        held_messages.push_back(kernel_context
                                    ? m2k_create_msg(kernel_context,
                                                     M2K_ITEM_NONE)
                                    : M2K_ITEM_NONE);
    const m2k_item_t extension_message =
        held_messages.size() == M2K_MAX_MESSAGES + M2K_MAX_EXT_MESSAGES
            ? held_messages[M2K_MAX_MESSAGES] : M2K_ITEM_NONE;
    check(extension_port != M2K_ITEM_NONE &&
              extension_message != M2K_ITEM_NONE &&
              m2k_handle_type(extension_message) == M2K_TYPE_MESSAGE_EXT,
          "M2EX test crosses the original Message pool boundary");
    if (extension_port != M2K_ITEM_NONE && extension_message != M2K_ITEM_NONE) {
        check(m2k_send_msg_item(kernel_context, extension_port,
                                extension_message, 0x7600, 4) == M2K_OK,
              "extension Message is queued before save");
    }

    std::vector<ByteWrite> sparse;
    gen_sparse_writes(12345u, &sparse);

    // --- 1) deterministic machine state -------------------------------------
    scribble(c, 1u, sparse);
    const uint64_t h1 = state_hash_of(*c);
    check(h1 != 0, "hash of live state nonzero");

    // --- 2) save ---------------------------------------------------------------
    // Same naming convention as Hotkeys::save_next_slot: slot + live hash.
    char fname[64];
    std::snprintf(fname, sizeof(fname), "slot1_%016llx.state",
                  static_cast<unsigned long long>(h1));
    const auto save = save_to_file(dir / fname);
    check(save.ok, "save_to_file succeeds");
    check(save.hash == h1, "stamped hash equals live hash");
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(h1));
    check(save.path.filename().string().find(hex) != std::string::npos,
          "filename carries the hash stamp");

    m2k_item_t extension_reuse = M2K_ITEM_NONE;
    if (extension_port != M2K_ITEM_NONE && extension_message != M2K_ITEM_NONE) {
        m2k_item_t changed = M2K_ITEM_NONE;
        check(m2k_get_msg(kernel_context, extension_port, &changed, NULL, NULL) ==
                  M2K_OK,
              "saved extension Message can be consumed before mutation");
        check(changed == extension_message,
              "consumed extension Message identity is preserved");
        check(m2k_reply_msg(kernel_context, changed, 0x21) == M2K_OK,
              "consumed extension Message is deleted by ReplyMsg");
        extension_reuse = m2k_create_msg(kernel_context, M2K_ITEM_NONE);
        check(m2k_handle_type(extension_reuse) == M2K_TYPE_MESSAGE_EXT &&
                  extension_reuse != extension_message,
              "freed extension slot is generation-safe on reuse");
    }

    // Canaries sampled from the saved epoch.
    const ByteWrite& canary = sparse[17];
    const uint8_t canary_val = canary.val ^ uint8_t(1u);

    // --- 3) mutate --------------------------------------------------------------
    std::vector<ByteWrite> sparse2;
    gen_sparse_writes(999999u, &sparse2);
    scribble(c, 2u, sparse2);  // different pattern + register epoch
    check(state_hash_of(*c) != h1, "mutation changes the hash");

    // --- 4) load round-trip ------------------------------------------------------
    const auto load = load_from_file(dir / fname);
    check(load.ok, "load_from_file succeeds");
    check(load.hash == h1, "loaded stamp matches original");
    check(state_hash_of(*c) == h1, "round-trip: save -> load -> hash equal");
    bool regs_ok = c->gpr[7] == 0x10000u * 1u + 7 &&
                   c->cr == (0x2468ACE0u ^ 1u) &&
                   c->lr == 0xdead0000u + 1u &&
                   c->ctr == 0x0000bEEFu + 1u &&
                   c->xer == (0x80000000u | (1u << 16));
    uint32_t fpr_bits;
    std::memcpy(&fpr_bits, &c->fpr[13], 4);
    regs_ok = regs_ok && fpr_bits == (0x7f800000u | (1u << 8) | 13);
    check(regs_ok, "registers restored exactly (incl. FPR bit patterns)");
    check(c->mem[kDenseBase + 5] == 1, "dense-region canary restored");
    check(c->mem[canary.addr] == canary_val, "sparse-page canary restored");
    if (extension_port != M2K_ITEM_NONE && extension_message != M2K_ITEM_NONE) {
        m2k_item_t restored = M2K_ITEM_NONE;
        check(m2k_get_msg(kernel_context, extension_port, &restored, NULL, NULL) ==
                  M2K_OK,
              "M2EX queue is available after load");
        check(restored == extension_message,
              "M2EX load restores the queued extension Message");
        check(m2k_reply_msg(kernel_context, restored, 0x22) == M2K_OK,
              "restored extension Message remains replyable");
        check(extension_reuse == M2K_ITEM_NONE ||
                  m2k_message_info(kernel_context, extension_reuse,
                                   NULL, NULL, NULL, NULL) == M2K_ERR_BAD_ITEM,
              "pre-load extension reuse does not survive restore");
    }

    // --- 5) corruption / validation ------------------------------------------------
    {
        std::ifstream in(dir / fname, std::ios::binary);
        const std::string blob((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();

        auto rewrite = [&](const char* name, const std::string& data) {
            std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
        };

        std::string corrupt = blob;
        corrupt[blob.size() - 64] ^= 0x40;  // flip a payload byte
        rewrite("corrupt.state", corrupt);
        const auto r1 = load_from_file(dir / "corrupt.state");
        check(!r1.ok && r1.error.find("hash mismatch") != std::string::npos,
              "corrupted payload rejected by hash stamp");
        check(state_hash_of(*c) == h1, "rejected load left state untouched");

        std::string foreign = blob;
        // Header byte 8 is GPR0: keep the header's saved state_hash unchanged,
        // then restamp the outer stream so this is a foreign-but-byte-valid
        // file rather than an ordinary corruption case.
        foreign[8] ^= char(0x01);
        restamp_stream(&foreign);
        rewrite("foreign.state", foreign);
        const auto rforeign = load_from_file(dir / "foreign.state");
        check(!rforeign.ok &&
                  rforeign.error.find("state hash mismatch") != std::string::npos,
              "re-stamped foreign register image rejected by semantic seal");
        check(state_hash_of(*c) == h1,
              "semantic-seal rejection left state untouched");

        std::string bmagic = blob;
        bmagic[0] = 'X';
        rewrite("bmagic.state", bmagic);
        const auto r2 = load_from_file(dir / "bmagic.state");
        check(!r2.ok && r2.error.find("bad magic") != std::string::npos,
              "wrong magic rejected");

        rewrite("trunc.state", blob.substr(0, blob.size() - 512));
        check(!load_from_file(dir / "trunc.state").ok,
              "truncated file rejected");

        // Crafted header whose size sum overflows uint64: 336-byte header,
        // 8192-byte bitmap, payload_bytes = UINT64_MAX, 8192 trailing zero
        // bytes. Before the overflow-safe framing check this sent the hasher
        // off the end of the buffer (SIGBUS/SIGSEGV) before integrity
        // rejection.
        {
            std::string overflow(kSavestateHeaderSize, '\0');
            put_u32_at(&overflow, 0, m2host::savestate::kMagic);
            put_u32_at(&overflow, 4, m2host::savestate::kVersion);
            put_u32_at(&overflow, kOffMemSize, uint32_t(RECOMP_GUEST_MEM));
            put_u32_at(&overflow, kOffPageSize, 4096);
            put_u32_at(&overflow, kOffEncoding, 1);
            put_u64_at(&overflow, kOffBitmapBytes, 8192);
            put_u64_at(&overflow, kOffPayloadBytes, ~uint64_t(0));
            overflow.append(8192, '\0');
            rewrite("overflow.state", overflow);
            const auto ro = load_from_file(dir / "overflow.state");
            check(!ro.ok, "overflowing header lengths rejected cleanly");
            check(ro.error.find("payload size mismatch") != std::string::npos,
                  "overflow rejected by length framing before hashing");
            check(state_hash_of(*c) == h1,
                  "overflow rejection left state untouched");
        }

        const uint64_t bitmap_bytes = get_u64_at(blob, kOffBitmapBytes);
        const uint64_t payload_bytes = get_u64_at(blob, kOffPayloadBytes);
        const size_t expected_bitmap = RECOMP_GUEST_MEM / 4096 / 8;
        check(bitmap_bytes == expected_bitmap && payload_bytes > 0,
              "reference save has the expected sparse framing");

        // Locate a bitmap bit in a chosen state so the population tests can
        // add or remove exactly one page.
        auto find_bitmap_bit = [&](bool set, size_t* byte_off, int* bit) {
            for (size_t i = 0; i < bitmap_bytes; ++i) {
                const size_t off = kSavestateHeaderSize + i;
                const uint8_t v = uint8_t(blob[off]);
                for (int b = 0; b < 8; ++b) {
                    if (((v >> b) & 1u) == (set ? 1u : 0u)) {
                        *byte_off = off;
                        *bit = b;
                        return true;
                    }
                }
            }
            return false;
        };

        if (bitmap_bytes == expected_bitmap) {
            // Re-stamp a bitmap that claims one extra zero page: the payload
            // does not contain its bytes, so the loader must reject on page
            // population before hashing/decoding rather than read past the
            // payload.
            size_t byte_off = 0;
            int bit = 0;
            if (find_bitmap_bit(false, &byte_off, &bit)) {
                std::string extra_page = blob;
                extra_page[byte_off] =
                    char(uint8_t(extra_page[byte_off]) | uint8_t(1u << bit));
                restamp_stream(&extra_page);
                rewrite("bitmap_extra.state", extra_page);
                const auto rb = load_from_file(dir / "bitmap_extra.state");
                check(!rb.ok && rb.error.find(
                                      "bitmap/payload page count mismatch") !=
                                  std::string::npos,
                      "re-stamped extra bitmap page rejected before decode");
                check(state_hash_of(*c) == h1,
                      "bitmap population rejection left state untouched");
            }

            // Re-stamped bitmap missing a present page (payload has bytes the
            // bitmap does not describe).
            if (find_bitmap_bit(true, &byte_off, &bit)) {
                std::string missing_page = blob;
                missing_page[byte_off] =
                    char(uint8_t(missing_page[byte_off]) & ~uint8_t(1u << bit));
                restamp_stream(&missing_page);
                rewrite("bitmap_missing.state", missing_page);
                const auto rm = load_from_file(dir / "bitmap_missing.state");
                check(!rm.ok && rm.error.find(
                                      "bitmap/payload page count mismatch") !=
                                  std::string::npos,
                      "re-stamped missing bitmap page rejected before decode");
                check(state_hash_of(*c) == h1,
                      "bitmap-missing rejection left state untouched");
            }

            // Re-stamped claim of a shorter payload with the bytes still
            // present is still a population mismatch and must be rejected.
            if (payload_bytes > 4096) {
                std::string short_claim = blob;
                put_u64_at(&short_claim, kOffPayloadBytes,
                           payload_bytes - 4096);
                restamp_stream(&short_claim);
                rewrite("payload_claim.state", short_claim);
                const auto rc = load_from_file(dir / "payload_claim.state");
                check(!rc.ok && rc.error.find(
                                      "bitmap/payload page count mismatch") !=
                                  std::string::npos,
                      "re-stamped short payload claim rejected before hashing");
                check(state_hash_of(*c) == h1,
                      "short-payload claim left state untouched");

                // Actually truncate the payload tail (and anything after it)
                // while the header still claims the full payload; re-stamping
                // must not make this loadable.
                const size_t payload_off =
                    kSavestateHeaderSize + size_t(bitmap_bytes);
                const size_t payload_end = payload_off + size_t(payload_bytes);
                if (payload_end <= blob.size()) {
                    std::string short_bytes = blob.substr(0, payload_end - 4096);
                    restamp_stream(&short_bytes);
                    rewrite("payload_cut.state", short_bytes);
                    const auto rt = load_from_file(dir / "payload_cut.state");
                    check(!rt.ok &&
                              rt.error.find("payload size mismatch") !=
                                  std::string::npos,
                          "re-stamped truncated payload rejected before hashing");
                    check(state_hash_of(*c) == h1,
                          "truncated-payload rejection left state untouched");
                }
            }
        }

        check(!load_from_file(dir / "definitely_not_here.state").ok,
              "missing file rejected");

        fs::remove(dir / "corrupt.state", ec);
        fs::remove(dir / "foreign.state", ec);
        fs::remove(dir / "bmagic.state", ec);
        fs::remove(dir / "trunc.state", ec);
        fs::remove(dir / "overflow.state", ec);
        fs::remove(dir / "bitmap_extra.state", ec);
        fs::remove(dir / "bitmap_missing.state", ec);
        fs::remove(dir / "payload_claim.state", ec);
        fs::remove(dir / "payload_cut.state", ec);
    }

    // --- 6) hotkey handler -------------------------------------------------------
    {
        using m2host::savestate::Hotkeys;
        constexpr uint32_t kF5 = 0x4000003Eu;  // SDL keycodes (USB HID usage
        constexpr uint32_t kF9 = 0x40000043u;  // | 0x40000000), per broker
        gen_sparse_writes(424242u, &sparse2);
        scribble(c, 3u, sparse2);
        const uint64_t h3 = state_hash_of(*c);

        Hotkeys hk(dir);
        check(!hk.handle_event(m2host::M2_EV_KEY, 1, 'a'),
              "hotkeys ignore non-savestate keys");
        check(!hk.handle_event(m2host::M2_EV_KEY, 0, kF5),
              "hotkeys ignore key repeats (fresh downs only)");
        check(hk.handle_event(m2host::M2_EV_KEY, 1, kF5) &&
                  hk.last_slot() == 1,
              "F5 saves into slot 1");

        gen_sparse_writes(777u, &sparse2);
        scribble(c, 9u, sparse2);
        check(state_hash_of(*c) != h3, "state diverged after epoch 9");
        check(hk.handle_event(m2host::M2_EV_KEY, 1, kF9),
              "F9 loads latest state");
        check(state_hash_of(*c) == h3, "hotkey round-trip restores epoch 3");
    }

    m2_recomp_shutdown();

    std::printf("[savestate-test] %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
