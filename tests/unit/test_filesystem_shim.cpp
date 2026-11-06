// filesystem_shim_test.cpp — focused probe for the re-pinned filesystem
// vectors 32/33/34 (directory open/read/close) backing FindMicroCard.
//
// Evidence under test (all checked-in):
//   - FN_FindMicroCard__1faa8 lift: fs:32("/") -> handle-or-0; loop
//     fs:33(handle,&entry) while >= 0; entry attr word @+0 gated on bits
//     0|22, name @+36 strcmp "remote"; finally fs:34(handle).
//   - driving/game: each selector has exactly one call site (FindMicroCard),
//     so this binding cannot affect any other caller.
//
// Headless; invokes registered shims directly through m2_selector_lookup
// with typed casts (same pattern as m2_input_selftest). Bounded iteration
// guards prove the enumerate-until-negative loop terminates.
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "filesystem_folio.h"
#include "shim_abi.h"
#include "filesystem_shim.h"
#include "folio_loader.h"
#include "graphics/guest_screenshot.h"
#include "posix_compat.h"
#include "test_scratch.h"
#include "ppc_ctx.h"
// guest_kernel.h -> recomp_helpers.h and shims/shim_types.h both define a
// struct named m2_tag_arg (uint32_t vs int32_t value). Rename the runtime
// copy for this translation unit; the test uses neither type.
#define m2_tag_arg m2_runtime_tag_arg
#include "guest_kernel.h"
#undef m2_tag_arg

// folio_install_all() calls these; audio/mpeg shims are not linked here.
void audio_shim_register() {}
void mpeg_decoder_register() {}

// The guest filesystem dispatch lives in recomp_runtime.c, which is compiled
// only when the build has game data. m2_host_core publishes M2_HAVE_RECOMP
// PUBLIC in exactly that configuration, so the same test can exercise the
// real eight-slot guest RawFile table in game builds and compile the
// host-level checks alone in the no-game CI build.
#if defined(M2_HAVE_RECOMP)
extern "C" int recomp_call_fs_dispatch(
    PPCContext* c, uint32_t ordinal, uint32_t pc);
extern "C" int m2_guest_kernel_bind(m2k_context* kctx, PPCContext* cpu);
extern "C" void m2_guest_kernel_unbind(void);
#endif

extern "C" {
uint32_t m2_host_raw_open(const char* guest_path);
uint32_t m2_host_raw_open_mode(const char* guest_path, uint32_t mode);
uint32_t m2_host_raw_size(uint32_t host_token);
int32_t m2_host_raw_read(uint32_t host_token, void* dst, uint32_t n);
int32_t m2_host_raw_read_at(uint32_t host_token, uint64_t offset,
                            void* dst, uint32_t n);
int32_t m2_host_raw_write(uint32_t host_token, const void* src, uint32_t n);
int32_t m2_host_raw_seek(uint32_t host_token, int32_t offset, int32_t whence);
void m2_host_raw_close(uint32_t host_token);
}

namespace {

namespace fs = std::filesystem;

int g_fails = 0;

void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok:" : "FAIL:", what);
    if (!ok) ++g_fails;
}

template <typename T>
T sel(uint32_t folio, uint32_t ord) {
    return reinterpret_cast<T>(m2_selector_lookup(folio, ord));
}

std::filesystem::path g_card_root;
std::filesystem::path g_shot_root;

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void write_all(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Guest-RAM helpers for the end-to-end folio-4 dispatch checks. The guest
// image is big-endian, so writes and reads go through ppc_ctx.h's wr32/rd32.
#if defined(M2_HAVE_RECOMP)
uint32_t guest_put_string(PPCContext* cpu, uint32_t ea, const char* s) {
    const size_t n = std::strlen(s) + 1;
    std::memcpy(cpu->mem + ea, s, n);
    return ea;
}

uint32_t guest_fs_call(PPCContext* cpu, uint32_t ordinal, uint32_t r3,
                       uint32_t r4, uint32_t r5) {
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    cpu->gpr[5] = r5;
    recomp_call_fs_dispatch(cpu, ordinal, 0);
    return cpu->gpr[3];
}
#endif

// The asset tree deliberately contains directories whose names are NOT volume
// names, so a root listing that walked the tree instead of the mounted-volume
// table would be caught below.
std::filesystem::path make_tree() {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("m2_fs_shim_test" + test_scratch_suffix());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "driving", ec);
    FILE* f = std::fopen((root / "save.dat").string().c_str(), "wb");
    if (f) {
        for (uint32_t i = 0; i < 8192u; ++i)
            std::fputc(static_cast<int>(i & 0xffu), f);
        std::fclose(f);
    }
    // The writable card lives OUTSIDE the asset tree (see FilesystemShim).
    g_card_root = root / "card-storage";
    std::filesystem::remove_all(g_card_root, ec);
    ::setenv("M2_MICROCARD_ROOT", g_card_root.string().c_str(), 1);
    // Screenshot redirection must also land outside the tracked asset tree.
    g_shot_root = root.parent_path() / ("m2_fs_shim_shots" + test_scratch_suffix());
    std::filesystem::remove_all(g_shot_root, ec);
    ::setenv("M2_SCREENSHOT_ROOT", g_shot_root.string().c_str(), 1);
    return root;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::printf("fs shim probe: vectors 4:32/33/34 as directory open/read/close\n");

    const fs::path root = make_tree();

    m2host::FilesystemShim shim;
    check(shim.init(root), "shim init at temp data root");
    m2host::folio_install_all();

    // Registration landed (differs from the shared no-op stub).
    void* stub = m2_selector_lookup(M2_FOLIO_FILESYSTEM, 31);  // unimplemented
    check(m2_selector_lookup(M2_FOLIO_FILESYSTEM, 32) != stub &&
              m2_selector_lookup(M2_FOLIO_FILESYSTEM, 33) != stub &&
              m2_selector_lookup(M2_FOLIO_FILESYSTEM, 34) != stub,
          "selectors 32/33/34 implemented");
    // Raw-file ordinals 35..40 moved to typed dispatch in
    // recomp_runtime.c::recomp_call_fs_dispatch; the host shim no longer
    // publishes those names. The directory dispatch is still routed via the
    // host shim, while the guest raw-file path is exercised by
    // m2_filesystem_raw_test.
    check(m2_selector_lookup(M2_FOLIO_FILESYSTEM, 35) == stub &&
              m2_selector_lookup(M2_FOLIO_FILESYSTEM, 36) == stub &&
              m2_selector_lookup(M2_FOLIO_FILESYSTEM, 38) == stub &&
              m2_selector_lookup(M2_FOLIO_FILESYSTEM, 39) == stub,
          "selectors 35/36/38/39 fall through to recomp_call_fs_dispatch (typed); host shim is the directory window only");

    auto open_dir = sel<m2_dir_t* (*)(const char*)>(M2_FOLIO_FILESYSTEM, 32);
    auto read_dir = sel<int32_t (*)(m2_dir_t*, void*)>(M2_FOLIO_FILESYSTEM, 33);
    auto close_dir = sel<m2_err_t (*)(m2_dir_t*)>(M2_FOLIO_FILESYSTEM, 34);

    // Exact FindMicroCard sequence against the root path string "/".
    m2_dir_t* d = open_dir("/");
    check(d != nullptr, "OpenDirPath(\"/\") returns handle");
    if (d) {
        uint8_t entry[128];
        bool terminated = false, bits_ok = true, name_off_ok = true,
             found_card = false, found_remote = false, saw_asset_dir = false;
        std::vector<std::string> names;
        int entries = 0;
        for (int i = 0; i < 256; ++i) {   // bound: livelock guard
            std::memset(entry, 0xEE, sizeof(entry));
            int32_t rc = read_dir(d, entry);
            if (rc < 0) {
                terminated = true;
                break;
            }
            ++entries;
            uint32_t attr = 0;
            std::memcpy(&attr, entry + M2_DIRENT_ATTR_OFF, 4);
            if ((attr & M2_DIRENT_ATTR_IS_DIRECTORY) == 0 ||
                (attr & M2_DIRENT_ATTR_USER_STORAGE) == 0)
                bits_ok = false;   // FindMicroCard gates on both bits
            const char* name =
                reinterpret_cast<const char*>(entry + M2_DIRENT_NAME_OFF);
            if (entry[M2_DIRENT_NAME_OFF + M2_DIRENT_NAME_CAP - 1] != 0xEE)
                name_off_ok = false;   // wrote past the pinned window
            names.emplace_back(name);
            if (std::strcmp(name, M2_CARD_VOLUME) == 0) found_card = true;
            if (std::strcmp(name, M2_HOST_LINK_VOLUME) == 0) found_remote = true;
            if (std::strcmp(name, "driving") == 0 ||
                std::strcmp(name, "save.dat") == 0)
                saw_asset_dir = true;
        }
        check(terminated, "ReadDir loop terminates (negative end marker)");
        check(bits_ok, "attr bits 0|22 set on every entry");
        check(name_off_ok, "name stays within +36..+83 window");
        // "/" lists MOUNTED VOLUMES (folios/file/driver.c:193-270), not the
        // asset tree, and the card is mounted last so FindMicroCard's
        // keep-scanning loop settles on it.
        check(entries == 2, "exactly the two mounted volumes are listed");
        check(names.size() == 2 && names[0] == "data" &&
                  names[1] == M2_CARD_VOLUME,
              "disc volume first, card volume last (mount order)");
        check(found_card, "card volume present and not named \"remote\"");
        check(!found_remote, "no root entry named \"remote\" (guest skips it)");
        check(!saw_asset_dir, "asset-tree entries are NOT root entries");
        check(close_dir(d) == M2_OK, "CloseDir returns OK");

        // Stale handle fails closed after close.
        check(read_dir(d, entry) < 0, "ReadDir on closed handle fails closed");
        check(close_dir(d) != M2_OK, "double CloseDir rejected");
    }

    check(open_dir("/no/such/dir") == nullptr, "missing path opens NULL");
    check(read_dir(nullptr, nullptr) < 0, "ReadDir NULL handle fails closed");
    check(close_dir(nullptr) != M2_OK, "CloseDir NULL rejected");

    // The SoundPlayer file path uses block-addressed reads. Verify the host
    // accessor can read at an offset without disturbing the raw cursor.
    const uint32_t raw = m2_host_raw_open("data/save.dat");
    check(raw != 0 && m2_host_raw_size(raw) == 8192u,
          "raw file opens with size");
    uint8_t first[4] = {}, at_offset[4] = {}, after_offset[4] = {};
    check(m2_host_raw_read(raw, first, sizeof(first)) == 4 &&
              first[0] == 0 && first[1] == 1 && first[2] == 2 && first[3] == 3,
          "raw sequential read starts at zero");
    check(m2_host_raw_read_at(raw, 4096u, at_offset, sizeof(at_offset)) == 4 &&
              at_offset[0] == 0 && at_offset[1] == 1 &&
              at_offset[2] == 2 && at_offset[3] == 3,
          "raw read-at returns requested block offset");
    check(m2_host_raw_read(raw, after_offset, sizeof(after_offset)) == 4 &&
              after_offset[0] == 4 && after_offset[1] == 5 &&
              after_offset[2] == 6 && after_offset[3] == 7,
          "raw read-at preserves sequential cursor");
    check(m2_host_raw_seek(raw, 0, M2_SEEK_END) == 8,
          "raw seek returns previous cursor at end-of-file");
    check(m2_host_raw_seek(raw, -4, M2_SEEK_END) == 8192,
          "raw seek returns previous cursor for a negative END move");
    check(m2_host_raw_seek(raw, 0, M2_SEEK_SET) == 8188,
          "raw seek returns previous cursor for SET");
    check(m2_SeekRawFile(reinterpret_cast<m2_file_t*>(
                             static_cast<uintptr_t>(raw)), 4, M2_SEEK_CUR) == 0,
          "native SeekRawFile resolves the 32-bit raw token");
    m2_host_raw_close(raw);

    // ---- findings 1/2: the disc is read-only, and capacity precedes effects --
    //
    // A write-mode open naming a disc asset used to resolve through
    // resolve_host_path's disc branches, so WRITE_NEW removed the original and
    // the handle then wrote to it. Every writable mode must now fail before any
    // filesystem effect, and the asset bytes must stay byte-identical.
    const std::string kAsset = "ORIGINAL_ASSET";
    const fs::path asset_path = root / "asset.bin";
    write_all(asset_path, kAsset);
    const uint32_t kDiscWritableModes[] = {
        M2_FILEOPEN_WRITE, M2_FILEOPEN_WRITE_NEW, M2_FILEOPEN_READWRITE,
        M2_FILEOPEN_READWRITE_NEW, M2_FILEOPEN_READWRITE_EXISTING};
    for (uint32_t mode : kDiscWritableModes) {
        const uint32_t h = m2_host_raw_open_mode("/data/asset.bin", mode);
        check(h == 0, "disc writable open is refused");
        check(read_all(asset_path) == kAsset,
              "disc asset bytes unchanged after refused writable open");
        if (h) m2_host_raw_close(h);
    }
    const fs::path absent_asset = root / "new_asset.bin";
    check(m2_host_raw_open_mode("/data/new_asset.bin",
                                M2_FILEOPEN_WRITE_NEW) == 0,
          "disc WRITE_NEW cannot create a new asset");
    check(!fs::exists(absent_asset), "refused disc write creates no file");

    // WRITE_EXISTING keeps its unusual vendor semantics: it is not rf_Writable
    // and therefore still opens an existing file read-only. It must not become
    // a write vector merely because the mode name says WRITE.
    const uint32_t existing =
        m2_host_raw_open_mode("/data/asset.bin", M2_FILEOPEN_WRITE_EXISTING);
    check(existing != 0, "WRITE_EXISTING still opens an existing disc file");
    if (existing) {
        check(m2_host_raw_write(existing, "BAD", 3) == -1,
              "WRITE_EXISTING handle refuses writes");
        m2_host_raw_close(existing);
    }
    check(read_all(asset_path) == kAsset,
          "WRITE_EXISTING leaves disc asset bytes unchanged");

    // Normal read handles are read-only at the shim boundary, not merely at
    // libc: the token carries rf_Writable false and writes fail negative.
    const uint32_t read_only =
        m2_host_raw_open_mode("/data/asset.bin", M2_FILEOPEN_READ);
    check(read_only != 0, "disc read-mode open still works");
    if (read_only) {
        check(m2_host_raw_write(read_only, "BAD", 3) == -1,
              "read-mode raw handle refuses writes");
        m2_host_raw_close(read_only);
    }
    m2_file_t* native_read = m2_OpenFile("data/asset.bin");
    check(native_read != nullptr, "native OpenFile still opens a disc asset");
    if (native_read) {
        check(m2_host_raw_write(
                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(native_read)),
                  "BAD", 3) == -1,
              "native OpenFile handle refuses writes");
        m2_CloseFile(native_read);
    }
    check(read_all(asset_path) == kAsset,
          "read handles leave disc asset bytes unchanged");

    // Pool exhaustion must be detected before an existing card save is wiped.
    const std::string kSave = "PERSISTENT_SAVE";
    std::error_code cec;
    fs::create_directories(g_card_root, cec);
    const fs::path save_path = g_card_root / "IMSAprefs";
    write_all(save_path, kSave);
    std::vector<uint32_t> fillers;
    for (int i = 0; i < 64 && fillers.size() < 32; ++i) {
        const uint32_t h =
            m2_host_raw_open_mode("/data/asset.bin", M2_FILEOPEN_READ);
        if (!h) break;
        fillers.push_back(h);
    }
    check(fillers.size() == 32, "all 32 raw slots filled with read handles");
    const uint32_t victim =
        m2_host_raw_open_mode("/card/IMSAprefs", M2_FILEOPEN_WRITE_NEW);
    check(victim == 0, "card WRITE_NEW returns zero when the pool is full");
    check(read_all(save_path) == kSave,
          "pool exhaustion leaves the existing card save intact");
    check(m2_host_raw_open_mode("/card/absent.bin", M2_FILEOPEN_WRITE_NEW) == 0,
          "pool exhaustion refuses a new card file");
    check(!fs::exists(g_card_root / "absent.bin"),
          "pool exhaustion creates no empty card file");
    for (uint32_t h : fillers) m2_host_raw_close(h);

    // Legitimate MicroCard replacement still works once capacity is available:
    // WRITE_NEW discards the old save, then the new bytes persist.
    const uint32_t save =
        m2_host_raw_open_mode("/card/IMSAprefs", M2_FILEOPEN_WRITE_NEW);
    check(save != 0, "card WRITE_NEW opens after capacity is available");
    if (save) {
        check(m2_host_raw_size(save) == 0, "card WRITE_NEW truncates on open");
        check(m2_host_raw_write(save, "NEW", 3) == 3,
              "card WRITE_NEW accepts the new save bytes");
        m2_host_raw_close(save);
    }
    check(read_all(save_path) == "NEW", "card save persists the new contents");

    // The screenshot.N exception still redirects, but only for WRITE_NEW and
    // only outside the tracked asset root.
    const uint32_t shot =
        m2_host_raw_open_mode("screenshot.7", M2_FILEOPEN_WRITE_NEW);
    check(shot != 0, "screenshot.7 WRITE_NEW opens");
    if (shot) m2_host_raw_close(shot);
    check(fs::exists(g_shot_root / "screenshot.7"),
          "screenshot.N is created under the screenshot root");
    check(!fs::exists(root / "screenshot.7"),
          "screenshot.N is not created in the asset tree");
    check(m2_host_raw_open_mode("screenshot.x", M2_FILEOPEN_WRITE_NEW) == 0,
          "non-numeric screenshot name is not redirected");
    check(!fs::exists(root / "screenshot.x"),
          "non-numeric screenshot name creates nothing in the asset tree");
    check(m2_host_raw_open_mode("screenshot.7", M2_FILEOPEN_WRITE) == 0,
          "screenshot redirection is WRITE_NEW-only");

    // Both guests exit on a failed screenshot write, so each one's dump size
    // must be accepted: driving/game 576x432 and shooter2d 576x216.
    check(m2host::guest_screenshot::height_for_bytes(0x79800u) == 432,
          "racing screenshot size is accepted");
    check(m2host::guest_screenshot::height_for_bytes(0x3cc00u) == 216,
          "shooter2d screenshot size is accepted");
    check(m2host::guest_screenshot::height_for_bytes(0x3cc02u) == 0,
          "other screenshot sizes are refused");

    // ---- guest dispatch: the 8-slot RawFile table is checked pre-open -------
    //
    // recomp_call_fs_dispatch (recomp_runtime.c) is the path the game itself
    // uses for folio 4:35/36/37/38. Its table has only eight slots, so a full
    // table used to be discovered after the host had already truncated the
    // card save for FILEOPEN_WRITE_NEW. This runs the real handler in game
    // builds; the no-game CI build compiles without the runtime symbols.
#if defined(M2_HAVE_RECOMP)
    {
        PPCContext cpu{};
        cpu.mem = static_cast<uint8_t*>(std::calloc(1, RECOMP_GUEST_MEM));
        m2k_context kctx{};
        if (!cpu.mem) {
            check(false, "guest dispatch harness allocates guest RAM");
        } else {
            m2k_init(&kctx);
            check(m2_guest_kernel_bind(&kctx, &cpu) == 0,
                  "guest kernel binds for folio-4 dispatch");

            const uint32_t kPathEa = 0x1000u;
            const uint32_t kOutEa = 0x1100u;
            const uint32_t kInfoEa = 0x1200u;
            const uint32_t kBufEa = 0x1300u;

            // A writable disc path must be refused by the real guest handler
            // before it reaches the host open, with the asset byte-identical.
            guest_put_string(&cpu, kPathEa, "/data/asset.bin");
            const uint32_t disc_rc =
                guest_fs_call(&cpu, 35, kOutEa, kPathEa, M2_FILEOPEN_WRITE_NEW);
            check(disc_rc == 0xFFFFFFFFu,
                  "guest OpenRawFile refuses a disc WRITE_NEW");
            check(read_all(asset_path) == kAsset,
                  "guest disc refusal leaves the asset unchanged");

            // Fill all eight guest slots with read handles to the card save.
            const std::string kGuestSave = "GUEST_CARD_SAVE";
            write_all(save_path, kGuestSave);
            uint32_t guest_handles[8] = {};
            bool all_open = true;
            for (uint32_t i = 0; i < 8u; ++i) {
                guest_put_string(&cpu, kPathEa, "/card/IMSAprefs");
                const uint32_t rc =
                    guest_fs_call(&cpu, 35, kOutEa, kPathEa, M2_FILEOPEN_READ);
                const uint32_t h = rd32(&cpu, kOutEa);
                guest_handles[i] = h;
                if (rc != 0 || h == 0 || h == 0xFFFFFFFFu) all_open = false;
            }
            check(all_open, "guest dispatch opens all eight RawFile slots");

            // A full guest table must fail before m2_host_raw_open_mode, so the
            // existing card save is never removed/truncated.
            wr32(&cpu, kOutEa, 0xDEADBEEFu);
            guest_put_string(&cpu, kPathEa, "/card/IMSAprefs");
            const uint32_t full_rc =
                guest_fs_call(&cpu, 35, kOutEa, kPathEa, M2_FILEOPEN_WRITE_NEW);
            check(full_rc == 0xFFFFFFFFu,
                  "guest OpenRawFile returns -1 when its table is full");
            check(rd32(&cpu, kOutEa) == 0xDEADBEEFu,
                  "guest out-handle is untouched on table-full refusal");
            check(read_all(save_path) == kGuestSave,
                  "guest table exhaustion leaves the card save intact");
            guest_put_string(&cpu, kPathEa, "/card/refused_new.bin");
            const uint32_t new_rc =
                guest_fs_call(&cpu, 35, kOutEa, kPathEa, M2_FILEOPEN_WRITE_NEW);
            check(new_rc == 0xFFFFFFFFu,
                  "guest table exhaustion refuses a new card file");
            check(!fs::exists(g_card_root / "refused_new.bin"),
                  "guest table exhaustion creates no card file");

            // Close every guest handle; each close must free its guest and
            // host slot so capacity is available again.
            bool all_closed = true;
            for (uint32_t h : guest_handles) {
                if (guest_fs_call(&cpu, 36, h, 0, 0) != 0) all_closed = false;
            }
            check(all_closed, "guest CloseRawFile releases all eight slots");

            // Legitimate replacement still works through the same dispatch:
            // WRITE_NEW truncates, WriteRawFile persists, CloseRawFile closes.
            guest_put_string(&cpu, kPathEa, "/card/IMSAprefs");
            const uint32_t open_rc =
                guest_fs_call(&cpu, 35, kOutEa, kPathEa, M2_FILEOPEN_WRITE_NEW);
            const uint32_t save_h = rd32(&cpu, kOutEa);
            check(open_rc == 0 && save_h != 0,
                  "guest WRITE_NEW opens after capacity returns");
            std::memset(cpu.mem + kInfoEa, 0, 0x28u);
            check(save_h != 0 &&
                      guest_fs_call(&cpu, 40, save_h, kInfoEa, 0x28u) == 0 &&
                      rd32(&cpu, kInfoEa + 0x08u) == 0,
                  "guest WRITE_NEW truncates the card save");
            std::memcpy(cpu.mem + kBufEa, "G", 1);
            check(save_h != 0 &&
                      guest_fs_call(&cpu, 38, save_h, kBufEa, 1) == 1,
                  "guest WriteRawFile persists a writable card byte");
            check(save_h != 0 && guest_fs_call(&cpu, 36, save_h, 0, 0) == 0,
                  "guest CloseRawFile closes the writable handle");
            check(read_all(save_path) == "G",
                  "guest card save persists the written byte");

            m2_guest_kernel_unbind();
            std::free(cpu.mem);
        }
    }
#else
    std::printf("  skip: guest folio-4 dispatch absent (no-game build)\n");
#endif
    // Empty directory: open succeeds, first read is the negative end marker.
    std::error_code ec;
    fs::create_directories(root / "empty", ec);
    m2_dir_t* de = open_dir("/empty");
    check(de != nullptr && read_dir(de, nullptr) < 0 && close_dir(de) == M2_OK,
          "empty directory enumerates zero entries");

    fs::remove_all(root, ec);
    fs::remove_all(g_shot_root, ec);

    std::printf(g_fails ? "[fs-probe] FAIL (%d)\n" : "[fs-probe] PASS\n",
                g_fails);
    return g_fails ? 1 : 0;
}
