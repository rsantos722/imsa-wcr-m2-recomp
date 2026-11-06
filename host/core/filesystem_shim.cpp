// filesystem_shim.cpp — maps folio-4 raw file ops onto stdio rooted at the
// extracted disc tree. Selector keys follow the repinned call-shape mapping
// in filesystem_folio.h, including the directory 32..34 and raw-file 35..40
// windows.
#include "filesystem_shim.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <vector>
#include "graphics/guest_screenshot.h"

extern "C" __attribute__((weak)) int32_t m2_guest_graphics_read_framebuffer_c(
    uint32_t, void*, uint32_t) { return -1; }

extern "C" {
#include "filesystem_folio.h"
}
#include "folio_loader.h"
#include "platform.h"
#include "trace.h"
#include "diagnostic_perf.h"
#include "ppc_ctx.h"
#include "recomp_host.h"
__attribute__((weak)) PPCContext* m2_recomp_context(void) { return nullptr; }
__attribute__((weak)) int m2_guest_resolve_file_path(const char* path, char* out,
                                                    uint32_t capacity) {
    if (!path || std::strlen(path) >= capacity) return -1;
    std::memcpy(out, path, std::strlen(path) + 1u);
    return 0;
}
__attribute__((weak)) void m2_oob_shadow_write(uint32_t, uint32_t) {}
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t) { return 1; }

// Guest frame counter for trace_asset_load's guest_frame field (counter seq at
// 0x3F0000, incremented at 0x2fb88). Without this the field was always 0, so a
// census could not separate boot-era loads from restored-route loads. Read-only
// diagnostic; no behavior change.
static uint32_t fs_guest_frame(void) {
    PPCContext* c = m2_recomp_context();
    if (!c || !c->mem || M2_GUEST_FRAME_COUNTER_EA > RECOMP_GUEST_MEM - 4u)
        return 0;
    return rd32(c, M2_GUEST_FRAME_COUNTER_EA);
}

using namespace m2host;

static FilesystemShim* g_fs = nullptr;


bool FilesystemShim::init(const std::filesystem::path& root) {
    m_root = root;
    std::error_code ec;
    m_root_ok = std::filesystem::is_directory(root, ec) && !ec;
    if (!m_root_ok)
        std::fprintf(stderr, "[fs] WARNING: data root missing: %s\n",
                     platform::path_to_utf8(root).c_str());
    else
        std::printf("[fs] data root ok: %s\n", platform::path_to_utf8(root).c_str());
    // Writable MicroCard storage. It is deliberately NOT under the asset root:
    // the asset root is the user's read-only game data, and a real play
    // session must be able to create files. Precedence:
    //   1. M2_MICROCARD_ROOT — tests point this at their own scratch dir.
    //   2. A per-user data directory, so a real relaunch finds the same files.
    // There is no fallback back into the asset tree: if the directory cannot
    // be made the write fails closed and says so.
    m_microcard = platform::get_env_path("M2_MICROCARD_ROOT");
    // The default is shared with the launcher's save manager (platform.cpp).
    if (m_microcard.empty()) m_microcard = platform::default_microcard_dir();
    m_screenshots = platform::get_env_path("M2_SCREENSHOT_ROOT");
    if (m_screenshots.empty()) m_screenshots = platform::default_screenshot_dir();
    std::printf("[fs] microcard volume \"/%s\" -> %s%s\n", M2_CARD_VOLUME,
                platform::path_to_utf8(m_microcard).c_str(),
                !platform::get_env("M2_MICROCARD_ROOT").empty() ? "" : " (default; set M2_MICROCARD_ROOT to relocate)");
    g_fs = this;
    return true;
}
void FilesystemShim::shutdown() { g_fs = nullptr; }

namespace {

std::FILE* to_file(m2_file_t* f) { return reinterpret_cast<std::FILE*>(f); }

// Resolve `name` under `base`, falling back to a per-component
// case-insensitive walk (the disc tree is case-sensitive on the host).
// Guest paths are absolute ("/"), so components are walked one by one from
// the base — operator/ must never see them or it would discard the base.
bool resolve_under(const std::filesystem::path& base, const char* name,
                   std::filesystem::path* out) {
    if (!g_fs || !name || !g_fs->root_ok()) return false;
    std::error_code bec;
    if (!std::filesystem::is_directory(base, bec) || bec) return false;
    std::filesystem::path lp = base;
    for (const auto& part : std::filesystem::path(name)) {
        const std::string ps = part.string();
        if (ps.empty() || ps == "/" || ps == ".") continue;
        std::string low = ps;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool found = false;
        std::error_code lec;
        for (const auto& e : std::filesystem::directory_iterator(lp, lec)) {
            std::string en = e.path().filename().string();
            std::string eln = en;
            for (auto& c : eln) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (eln == low) {
                lp /= en;
                found = true;
                break;
            }
        }
        if (!found || lec) return false;
    }
    *out = lp;
    return true;
}

bool resolve_under_root(const char* name, std::filesystem::path* out) {
    if (!g_fs) return false;
    return resolve_under(g_fs->root(), name, out);
}

// Case-insensitive leading-component match: "System.m2/..." (or bare
// "System.m2"). Guest separators are '/'.
bool has_system_prefix(const std::string& stripped) {
    static const char kPrefix[] = "system.m2";
    if (stripped.size() < sizeof(kPrefix) - 1) return false;
    for (size_t i = 0; i < sizeof(kPrefix) - 1; ++i) {
        if (std::tolower(static_cast<unsigned char>(stripped[i])) != kPrefix[i])
            return false;
    }
    return stripped.size() == sizeof(kPrefix) - 1 || stripped[sizeof(kPrefix) - 1] == '/';
}

// Resolve one guest path to a host path. Two shapes:
//   * "/card[/...]" (M2_CARD_VOLUME) — the MicroCard volume, the host's
//     stand-in for the removable card Portfolio would mount and list in "/".
//     It maps onto FilesystemShim::microcard_root(). `for_write` materializes
//     the volume on first use; a read never creates anything, so a lookup on an
//     absent card reports "no such file" as the vendor's OpenFile does.
//   * anything else — the disc volume: strip a leading "data/" segment (the
//     production data root already *is* that segment), and let a "System.m2"
//     prefix resolve from up to three ancestors so the disc-root sibling
//     trees reach their templates. This is tracked original input, so it is
//     read-only: `for_write` never resolves here.
//
// The card volume name is NOT a free choice made to satisfy a gate: it is
// constrained by the guest. FindMicroCard rejects exactly one root entry name,
// "remote" (M2_HOST_LINK_VOLUME), which is the developer host-link filesystem
// Portfolio mounts under that literal name. See shims/filesystem_folio.h for
// the full derivation from driver.c:193-270 / mount.c:514 / autodocs.c:233-237.
bool resolve_host_path(const char* guest_path, bool for_write,
                       std::filesystem::path* out) {
    if (!g_fs || !guest_path) return false;
    char resolved[256];
    // DSP templates are already rooted at the system volume by the bridge.
    if (guest_path[0] != '/' && !has_system_prefix(guest_path)) {
        if (m2_guest_resolve_file_path(guest_path, resolved, sizeof(resolved)) < 0)
            return false;
        guest_path = resolved;
    }
    std::string s = guest_path;
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    static const char kCard[] = M2_CARD_VOLUME;
    const size_t kCardLen = sizeof(kCard) - 1;
    const bool is_card =
        s.size() >= kCardLen &&
        std::strncmp(s.c_str(), kCard, kCardLen) == 0 &&
        (s.size() == kCardLen || s[kCardLen] == '/');
    if (is_card) {
        const std::string rest =
            s.size() > kCardLen ? s.substr(kCardLen + 1) : std::string();
        if (for_write) {
            std::error_code cec;
            std::filesystem::create_directories(g_fs->microcard_root(), cec);
            if (cec && !std::filesystem::is_directory(g_fs->microcard_root(), cec)) {
                // Fail closed and say so: the guest's own error path runs, and
                // a silent fallback into the tracked asset tree is exactly the
                // failure this boundary exists to prevent.
                std::fprintf(stderr,
                             "[fs] ERROR: cannot create MicroCard volume %s (%s); "
                             "writes will fail\n",
                             platform::path_to_utf8(g_fs->microcard_root()).c_str(),
                             cec.message().c_str());
                return false;
            }
        }
        const std::filesystem::path base = g_fs->microcard_root();
        std::error_code dec;
        if (!std::filesystem::is_directory(base, dec) || dec) return false;
        if (rest.empty()) { *out = base; return true; }
        if (!for_write) return resolve_under(base, rest.c_str(), out);
        // A create-new write names a file that does not exist yet, and
        // resolve_under rejects a missing component. Resolve only the parent
        // directories here and hand the caller the full path; the open itself
        // decides whether the leaf may be created (fileio.h:31-42).
        const size_t slash = rest.rfind('/');
        std::filesystem::path dir = base;
        if (slash != std::string::npos) {
            const std::string parent = rest.substr(0, slash);
            if (!resolve_under(base, parent.c_str(), &dir)) return false;
        }
        const std::string leaf = rest.substr(slash + 1);
        // The leaf is not walked by resolve_under. Reject traversal, empty
        // names, and Windows drive/absolute forms ("C:foo", "\\server") that
        // would make operator/ discard the card root.
        if (leaf.empty() || leaf == "." || leaf == ".." ||
            leaf.find('\\') != std::string::npos)
            return false;
        const std::filesystem::path candidate = dir / leaf;
        if (candidate.parent_path() != dir) return false;
        *out = candidate;
        return true;
    }
    // The disc volume is read-only for the guest. `for_write` is only passed by
    // the raw-file open path; letting it through would let the caller's *_NEW
    // handling delete or truncate a tracked asset before the open
    // (rawfile.c:220-247). The writable surfaces are the MicroCard branch above
    // and the deliberately separate screenshot.N destination in raw_open_mode,
    // which does not call this resolver. Refuse before any filesystem effect.
    if (for_write) {
        std::fprintf(stderr,
                     "[fs] write-mode open refused: \"%s\" is on the read-only "
                     "disc volume\n",
                     guest_path);
        return false;
    }
    std::string stripped = s;
    if (stripped.compare(0, 5, "data/") == 0) stripped = stripped.substr(5);
    // Explicit disc directories are relative to the disc, not driving/data.
    // Restrict ancestor resolution to the shipped disc top-level names.
    std::string lower = stripped;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool disc_path = lower == "driving" || lower.compare(0, 8, "driving/") == 0 ||
                           lower == "shooter2d" || lower.compare(0, 10, "shooter2d/") == 0;
    if (disc_path) {
        std::filesystem::path base = g_fs->root();
        for (int up = 0; up < 4; ++up) {
            if (resolve_under(base, stripped.c_str(), out)) return true;
            if (!base.has_parent_path()) break;
            base = base.parent_path();
        }
        return false;
    }
    if (has_system_prefix(stripped)) {
        std::filesystem::path base = g_fs->root();
        std::filesystem::path p;
        for (int up = 0; up < 4; ++up) {
            if (resolve_under(base, stripped.c_str(), &p)) break;
            if (!base.has_parent_path()) break;
            base = base.parent_path();
        }
        *out = p;
        return true;
    }
    return resolve_under_root(stripped.c_str(), out);
}

}  // namespace

// --- directory enumeration (vectors 32/33/34, FindMicroCard shape) -----------
//
// Handles are minted slot|generation words — never host pointers — so a value
// that crosses into a guest register window stays 32-bit-clean and stale
// handles fail closed after close/reopen.

struct m2_dir {
    uint32_t handle;
    std::filesystem::directory_iterator it;
    // Enumerating "/" does NOT walk the asset tree. On Portfolio the root
    // listing is synthesised from the mounted filesystems
    // (folios/file/driver.c:193-270), so it holds the mount names, in mount
    // order, and nothing else. `vol_cursor` walks those synthetic entries; a
    // non-root directory walks the real tree and leaves it at -1.
    int vol_cursor = -1;
};

// The mounted volumes this host presents, in mount order. The disc volume is
// named "data" because every guest asset path already uses that prefix
// ("/data/tracks/...") and the disc volume's root IS the asset tree; the card
// is inserted after the disc is mounted, so it sorts last — which is what
// makes FindMicroCard's keep-scanning loop settle on the card rather than on
// the read-only disc.
const char* const kRootVolumes[] = {"data", M2_CARD_VOLUME};

namespace {

constexpr uint32_t kMaxOpenDirs = 8;
m2_dir g_dirs[kMaxOpenDirs];
uint32_t g_dir_gen = 0;

// Next mounted-volume entry for a "/" enumeration, or false at the end. The
// negative end marker FindMicroCard's loop stops on is reported once this
// returns false; `it` stays empty because a root never walks the asset tree.
bool dir_take_volume_entry(m2_dir* d, char* name_out, uint32_t cap) {
    if (d->vol_cursor < 0) return false;
    if (static_cast<size_t>(d->vol_cursor) >=
        sizeof(kRootVolumes) / sizeof(kRootVolumes[0])) {
        d->vol_cursor = -1;
        return false;
    }
    const char* n = kRootVolumes[d->vol_cursor++];
    const size_t len = std::strlen(n) + 1;
    if (cap < len) return false;
    std::memcpy(name_out, n, len);
    return true;
}

// Shared entry writer for both directory readers. Attr bits 0 and 22 are the
// two gates FindMicroCard applies to every entry it sees (lift 0x10404
// clrlwi 0x1f and 0x10410 rlwinm 0,0x16,0x16), so a real entry and the
// a mounted volume and a real directory entry present identically to
// the guest.
void fill_dirent(uint8_t* e, const char* name, size_t len) {
    uint32_t attr = M2_DIRENT_ATTR_IS_DIRECTORY | M2_DIRENT_ATTR_USER_STORAGE;
    std::memcpy(e + M2_DIRENT_ATTR_OFF, &attr, sizeof(attr));
    std::memset(e + M2_DIRENT_ATTR_OFF + 4, 0,
                M2_DIRENT_NAME_OFF - M2_DIRENT_ATTR_OFF - 4);
    std::memcpy(e + M2_DIRENT_NAME_OFF, name, len);
}

uint32_t dir_make_handle(uint32_t slot) {
    return ((++g_dir_gen & 0x0FFFFFFFu) << 4) | (slot + 1);
}

m2_dir* dir_from_handle(uint32_t h) {
    uint32_t slot = (h & 0xFu);
    if (slot == 0 || slot > kMaxOpenDirs) return nullptr;
    m2_dir* d = &g_dirs[slot - 1];
    if (d->handle != h) return nullptr;   // closed/stale slot
    return d;
}

constexpr uint32_t kMaxRawFiles = 32;
struct RawSlot {
    uint32_t handle = 0;     // the gk_raw_slot's host_token (32-bit-clean)
    std::FILE* fp = nullptr;
    uint32_t size = 0;        // cached file size (fi_ByteCount)
    bool writable = false;  // vendor rf_Writable: which mode opened the file
    std::filesystem::path screenshot_path;
};
inline RawSlot g_raw[kMaxRawFiles];
inline uint32_t g_rawGen = 0;
// The filesystem selector body runs on the calling guest task pthread, so two
// guest threads can enter the host raw-file bridge at once. This guard covers
// slot allocation (including the capacity check that must precede a destructive
// *_NEW open) and slot release. Holding it across one open is the reservation:
// no other allocator can take or recycle the slot between check and commit.
inline std::mutex g_raw_pool_mutex;
inline uint32_t raw_make_handle(uint32_t slot) {
    return ((++g_rawGen & 0x00FFFFFFu) << 8) | (slot + 1);
}
inline int raw_find_slot(uint32_t h) {
    uint32_t slot = (h & 0xFF) - 1;
    if (slot >= kMaxRawFiles) return -1;
    if (g_raw[slot].handle != h) return -1;
    return (int)slot;
}
inline int raw_find_free_slot() {
    for (uint32_t slot = 0; slot < kMaxRawFiles; ++slot) {
        if (g_raw[slot].fp == nullptr) return (int)slot;
    }
    return -1;
}

// Portfolio SeekRawFile returns the cursor position before the move, clamps
// the resulting position to [0, file_size], and treats a positive END offset
// as an end-of-file seek. Keep this helper shared by the native File helper
// and the guest RawFile bridge so both expose the same source contract.
int32_t seek_raw_file(std::FILE* fp, uint32_t file_size, int32_t offset,
                      int32_t whence) {
    if (!fp) return -1;
    std::clearerr(fp);
    const long old_pos = std::ftell(fp);
    if (old_pos < 0 || static_cast<uint64_t>(old_pos) > file_size ||
        static_cast<uint64_t>(old_pos) >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
        return -1;

    const int64_t old = static_cast<int64_t>(old_pos);
    const int64_t size = static_cast<int64_t>(file_size);
    int64_t next = 0;
    switch (whence) {
    case M2_SEEK_SET:
        next = offset <= 0 ? 0 : static_cast<int64_t>(offset);
        if (next > size) next = size;
        break;
    case M2_SEEK_CUR:
        if (offset < 0) {
            const int64_t distance = -static_cast<int64_t>(offset);
            next = distance >= old ? 0 : old - distance;
        } else {
            next = old + static_cast<int64_t>(offset);
            if (next >= size) next = size;
        }
        break;
    case M2_SEEK_END:
        if (offset < 0) {
            const int64_t distance = -static_cast<int64_t>(offset);
            next = distance >= size ? 0 : size - distance;
        } else {
            next = size;
        }
        break;
    default:
        return -1;
    }

    if (next < 0 || next > static_cast<int64_t>(
                           std::numeric_limits<long>::max()))
        return -1;
    if (std::fseek(fp, static_cast<long>(next), SEEK_SET) != 0)
        return -1;
    return static_cast<int32_t>(old);
}

}  // namespace

extern "C" {

m2_file_t* m2_OpenFile(const char* name) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!g_fs || !name) return nullptr;
    std::fprintf(stderr, "[fs] OpenFile: %s\n", name);
    std::filesystem::path p;
    if (!g_fs->root_ok()) return nullptr;
    // resolve_host_path walks the disc tree component by component with a
    // case-insensitive fallback (the disc tree is case-sensitive on the host)
    // and routes the "/remote" MicroCard mount. The previous `root() / name`
    // form was wrong for an absolute guest path: operator/ discards the base,
    // so "/remote/x" escaped the root entirely.
    if (!resolve_host_path(name, false, &p)) {
        std::fprintf(stderr, "[fs] OpenFile miss: %s\n", name);
        return nullptr;
    }
    std::FILE* f = platform::fopen(p, "rb");
    if (!f) return nullptr;
    // Trace: asset_load event — record guest-visible path and size
    long cur = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, cur, SEEK_SET);
    m2host::trace::trace_asset_load(name, 0, (uint32_t)sz, 0, fs_guest_frame());
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_free_slot();
    if (slot < 0) { std::fclose(f); return nullptr; }
    g_raw[slot].fp = f;
    g_raw[slot].size = (uint32_t)sz;
    g_raw[slot].writable = false;   // folio 4:0 OpenFile is read-only
    g_raw[slot].handle = raw_make_handle((uint32_t)slot);
    return reinterpret_cast<m2_file_t*>(g_raw[slot].handle);
}

m2_err_t m2_CloseFile(m2_file_t* f) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!f) return -1;
    uint32_t h = (uint32_t)(uintptr_t)f;
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_slot(h);
    if (slot < 0) return -1;
    std::fclose(g_raw[slot].fp);
    g_raw[slot].fp = nullptr;
    g_raw[slot].handle = 0;
    g_raw[slot].size = 0;
    g_raw[slot].writable = false;
    return M2_OK;
}

m2_dir_t* m2_OpenDirPath(const char* path) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::fprintf(stderr, "[fs] OpenDirPath: %s\n", path ? path : "(null)");
    std::filesystem::path p;
    if (!resolve_host_path(path, false, &p)) {
        std::fprintf(stderr, "[fs] OpenDirPath miss: %s\n", path ? path : "(null)");
        return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec) || ec) {
        std::fprintf(stderr, "[fs] OpenDirPath not a directory: %s\n", path);
        return nullptr;
    }
    for (uint32_t slot = 0; slot < kMaxOpenDirs; ++slot) {
        if (g_dirs[slot].handle != 0) continue;
        std::error_code iec;
        std::filesystem::directory_iterator it(p, iec);
        if (iec) return nullptr;
        // "/" is the volume listing (Portfolio synthesises it from the mounted
        // filesystems); anything else walks the real tree under its volume.
        std::error_code pec;
        g_dirs[slot].vol_cursor =
            (std::filesystem::equivalent(p, g_fs->root(), pec) && !pec) ? 0 : -1;
        g_dirs[slot].handle = dir_make_handle(slot);
        g_dirs[slot].it = it;
        return &g_dirs[slot];
    }
    std::fprintf(stderr, "[fs] OpenDirPath: handle table full\n");
    return nullptr;
}

int32_t m2_ReadDir(m2_dir_t* d, void* entry_out) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!d || !entry_out) return -1;
    m2_dir* dir = dir_from_handle(d->handle);
    if (!dir) return -1;
    std::error_code ec;
    // A "/" enumeration is served entirely from the mounted-volume table, in
    // mount order, and never walks the asset tree (driver.c:193-270).
    if (dir->vol_cursor >= 0) {
        char vol_name[M2_DIRENT_NAME_CAP];
        if (!dir_take_volume_entry(dir, vol_name, sizeof(vol_name)))
            return -1;  // end marker FindMicroCard's loop stops on
        fill_dirent(static_cast<uint8_t*>(entry_out), vol_name,
                    std::strlen(vol_name) + 1);
        return 0;
    }
    if (dir->it == std::filesystem::directory_iterator()) return -1;  // end
    const std::filesystem::path entry = dir->it->path();
    dir->it.increment(ec);   // advance now; current entry is reported below
    if (ec) {
        // Treat iterator failure as end-of-directory (fail closed).
        return -1;
    }
    std::string name = entry.filename().string();
    if (name.size() >= M2_DIRENT_NAME_CAP) name.resize(M2_DIRENT_NAME_CAP - 1);
    fill_dirent(static_cast<uint8_t*>(entry_out), name.c_str(), name.size() + 1);
    return 0;
}

m2_err_t m2_CloseDir(m2_dir_t* d) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!d) return -1;
    m2_dir* dir = dir_from_handle(d->handle);
    if (!dir) return -1;
    dir->handle = 0;   // invalidates the minted word immediately
    dir->vol_cursor = -1;
    dir->it = std::filesystem::directory_iterator();
    return M2_OK;
}


// ---- recomp guest-dispatch bridge (folio 4:7 ChangeDirectory) ---------------
// Strong definition of the weak m2_host_path_kind declared in
// recomp_runtime.c. Returns 0 when the guest path names nothing on the
// mounted volume, 1 for a regular file, 2 for a directory — the host half of
// the vendor's FindFile + FILE_IS_DIRECTORY test (folios/file/open.c:1402-1432).
//
// The lookup is the same walk m2_host_raw_open uses: strip a leading "data/",
// try the driving/data root first, then walk up to three ancestors so
// disc-root siblings (driving/, shooter2d/, System.m2/) resolve too. The
// guest-visible current directory is always absolute at this point
// (gk_cwd_resolve folds ".."), so the first component is a real disc entry and
// no relative-to-cwd host work is needed here.
int m2_host_path_kind(const char* guest_path) {
    if (!g_fs || !guest_path || !g_fs->root_ok()) return 0;
    std::string stripped = guest_path;
    while (!stripped.empty() && stripped.front() == '/') stripped.erase(0, 1);
    if (stripped.empty()) return 2;   // the volume root is always a directory
    if (stripped.compare(0, 5, "data/") == 0) stripped = stripped.substr(5);
    std::filesystem::path p;
    // MicroCard mount and the System.m2 sibling prefix first; the disc walk
    // below stays as the fallback for a path that only resolves further up.
    if (resolve_host_path(guest_path, false, &p)) {
        std::error_code rec;
        if (std::filesystem::is_directory(p, rec) && !rec) return 2;
        if (std::filesystem::is_regular_file(p, rec) && !rec) return 1;
        return 0;
    }
    if (resolve_under(g_fs->root(), stripped.c_str(), &p)) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec) && !ec) return 2;
        if (std::filesystem::is_regular_file(p, ec) && !ec) return 1;
        return 0;
    }
    std::filesystem::path base = g_fs->root();
    for (int up = 0; up < 4; ++up) {
        if (!base.has_parent_path()) break;
        base = base.parent_path();
        if (resolve_under(base, stripped.c_str(), &p)) {
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec) && !ec) return 2;
            if (std::filesystem::is_regular_file(p, ec) && !ec) return 1;
            return 0;
        }
    }
    return 0;
}
// ---- recomp guest-dispatch bridge (directory window 32/33/34) ---------------
// Guest-facing directory enumeration. The recomp guest kernel dispatch
// (recomp_runtime.c:gk_OpenDirPath/gk_ReadDir/gk_CloseDir) calls these weak
// accessors to enumerate the real disc tree; the handle returned here is the
// 32-bit-clean g_dirs token (never a host pointer), and m2_host_dir_next fills
// only the entry *name* (the guest writes the pinned M2_DIRENT layout into its
// own RAM with its own boundary checks).
uint32_t m2_host_dir_open(const char* guest_path) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!g_fs || !guest_path || !g_fs->root_ok()) return 0;
    std::filesystem::path p;
    if (!resolve_host_path(guest_path, false, &p)) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec) || ec) return 0;
    for (uint32_t slot = 0; slot < kMaxOpenDirs; ++slot) {
        if (g_dirs[slot].handle != 0) continue;
        std::error_code iec;
        std::filesystem::directory_iterator it(p, iec);
        if (iec) return 0;
        std::error_code pec;
        g_dirs[slot].vol_cursor =
            (std::filesystem::equivalent(p, g_fs->root(), pec) && !pec) ? 0 : -1;
        g_dirs[slot].handle = dir_make_handle(slot);
        g_dirs[slot].it = it;
        return g_dirs[slot].handle;   // 32-bit token, never a host pointer
    }
    return 0;   // handle table full
}

int m2_host_dir_next(uint32_t host_token, char* name_out, uint32_t name_cap) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!name_out || name_cap == 0) return -1;
    m2_dir* dir = dir_from_handle(host_token);
    if (!dir) return -1;   // stale/closed -> caller treats as end
    std::error_code ec;
    // "/" is the mounted-volume listing, not the asset tree.
    if (dir->vol_cursor >= 0)
        return dir_take_volume_entry(dir, name_out, name_cap) ? 1 : 0;
    if (dir->it == std::filesystem::directory_iterator()) return 0;  // end
    std::string name = dir->it->path().filename().string();
    dir->it.increment(ec);   // advance; current entry reported below
    if (name.size() >= name_cap) name.resize(name_cap - 1);
    std::memcpy(name_out, name.c_str(), name.size() + 1);
    return 1;   // one name produced
}

void m2_host_dir_close(uint32_t host_token) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    m2_dir* dir = dir_from_handle(host_token);
    if (!dir) return;
    dir->handle = 0;
    dir->it = std::filesystem::directory_iterator();
}

int32_t m2_SeekRawFile(m2_file_t* f, int32_t offset, int32_t whence) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!f) return -1;
    const uint32_t handle = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(f));
    const int slot = raw_find_slot(handle);
    if (slot < 0) return -1;
    return seek_raw_file(g_raw[slot].fp, g_raw[slot].size, offset, whence);
}

int32_t m2_GetRawFileInfo(m2_file_t* f, int32_t tag) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::FILE* fp = to_file(f);
    long cur = std::ftell(fp);
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, cur, SEEK_SET);
    if (tag == 0) return static_cast<int32_t>(size);
    std::printf("[fs] GetRawFileInfo tag %d -> logging only\n", tag);
    return 0;
}

m2_err_t m2_SetRawFileAttrs(m2_file_t* f, uint32_t attrs) {
    (void)f;
    (void)attrs;
    return M2_OK;
}
m2_err_t m2_SetFileAttrs(const char* name, uint32_t attrs) {
    (void)name;
    (void)attrs;
    return M2_OK;
}
void m2_ClearRawFileError(m2_file_t* f) { std::clearerr(to_file(f)); }
m2_err_t m2_SetRawFileSize(m2_file_t* f, uint32_t size) {
    (void)size;  // no truncation on a read-only view
    return M2_OK;
}

m2_err_t m2_CreateAlias(const char* existing, const char* alias) {
    std::printf("[fs] CreateAlias('%s' -> '%s') logged\n", alias, existing);
    return M2_OK;
}

// ---- recomp guest-dispatch bridge (raw-file window 35/36/37/40) --------------
// Strong definitions of the weak m2_host_raw_* accessors declared in
// recomp_runtime.c. These provide the host-side service for the typed
// gk_OpenRawFile/gk_CloseRawFile/gk_ReadRawFile/gk_GetRawFileInfo handlers;
// the guest handle is a 32-bit-clean token (never a host pointer), and the
// host file is tracked in the shared g_raw[] table so the readinmemimage
// "alloc three carves, fill via three reads, close" sequence lands real
// disc bytes into guest RAM.

// Open one guest path under a Portfolio FileOpenModes mode, following
// rawfile.c:220-247 internalOpenRawFile: FILEOPEN_READ fails when the file is
// absent, FILEOPEN_WRITE/READWRITE create it, the *_NEW variants discard any
// old contents first, and the *_EXISTING variants refuse to create.
// `writable` mirrors rf_Writable (rawfile.c:277-284), which is set for exactly
// five of the seven modes -- note READWRITE_EXISTING is writable while
// WRITE_EXISTING is not. gk_WriteRawFile refuses a handle opened any other
// way, so this set is what decides whether a folio 4:38 write succeeds.
static bool screenshot_name(const char* path) {
    // The original sprintf literal is exactly "screenshot.%d". This narrow
    // host-link substitute does not make arbitrary disc paths writable.
    static constexpr char prefix[] = "screenshot.";
    if (std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const char* n = path + sizeof(prefix) - 1;
    if (!*n) return false;
    for (; *n; ++n) if (*n < '0' || *n > '9') return false;
    return true;
}

static bool screenshot_path(const char* name, std::filesystem::path* out) {
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(g_fs->root(), ec);
    if (ec) return false;
    const auto shots = std::filesystem::weakly_canonical(g_fs->screenshot_root(), ec);
    if (ec) return false;
    // Even an override cannot turn original assets into screenshot storage.
    if (std::mismatch(base.begin(), base.end(), shots.begin(), shots.end()).first == base.end())
        return false;
    std::filesystem::create_directories(shots, ec);
    if (ec) return false;
    *out = shots / name;
    return true;
}

static bool screenshot_ppm(const std::filesystem::path& raw_path, const uint8_t* bytes, int height) {
    auto path = raw_path;
    path += ".ppm";
    std::error_code ec;
    // Like WRITE_NEW for the raw file: replace the leaf, including a symlink.
    std::filesystem::remove(path, ec);
    if (ec) return false;
    std::FILE* f = platform::fopen(path, "wb");
    if (!f) return false;
    std::vector<uint8_t> rgb(size_t(guest_screenshot::width) * height * 3u);
    for (size_t p = 0; p < rgb.size() / 3u; ++p) {
        const uint16_t word = uint16_t(bytes[p * 2u] << 8u) | bytes[p * 2u + 1u];
        for (unsigned channel = 0; channel < 3; ++channel) {
            const unsigned v = (word >> (10u - channel * 5u)) & 31u;
            rgb[p * 3u + channel] = uint8_t((v << 3u) | (v >> 2u));
        }
    }
    const bool header = std::fprintf(f, "P6\n%d %d\n255\n", guest_screenshot::width, height) > 0;
    const bool pixels = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    const bool closed = std::fclose(f) == 0;
    return header && pixels && closed;
}

static uint32_t raw_open_mode(const char* guest_path, uint32_t mode) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!g_fs || !guest_path || !g_fs->root_ok()) return 0;
    if (mode > M2_FILEOPEN_READWRITE_EXISTING) return 0;

    // Capacity is part of the open decision, not a postscript. The *_NEW modes
    // remove an existing file and every writable mode can create one, so a full
    // 32-slot table must fail before the path is resolved, removed, or opened.
    // The lock reserves the slot from this check through commit.
    std::unique_lock<std::mutex> pool_lock(g_raw_pool_mutex);
    const int slot = raw_find_free_slot();
    if (slot < 0) {
        std::fprintf(stderr,
                     "[fs] m2_host_raw_open: raw handle table full: %s\n",
                     guest_path);
        return 0;
    }

    const bool create_ok  = mode == M2_FILEOPEN_WRITE ||
                            mode == M2_FILEOPEN_READWRITE ||
                            mode == M2_FILEOPEN_WRITE_NEW ||
                            mode == M2_FILEOPEN_READWRITE_NEW;
    const bool wipe_first = mode == M2_FILEOPEN_WRITE_NEW ||
                            mode == M2_FILEOPEN_READWRITE_NEW;
    const bool writable   = mode == M2_FILEOPEN_WRITE ||
                            mode == M2_FILEOPEN_WRITE_NEW ||
                            mode == M2_FILEOPEN_READWRITE ||
                            mode == M2_FILEOPEN_READWRITE_NEW ||
                            mode == M2_FILEOPEN_READWRITE_EXISTING;

    std::filesystem::path p;
    const bool screenshot = mode == M2_FILEOPEN_WRITE_NEW && screenshot_name(guest_path);
    // Only a write-mode open may create the MicroCard mount; a read of an
    // absent card must still miss.
    if (!(screenshot ? screenshot_path(guest_path, &p) : resolve_host_path(guest_path, writable, &p))) {
        std::fprintf(stderr, "[fs] m2_host_raw_open miss: %s\n", guest_path);
        return 0;
    }
    // resolve_host_path refuses every writable non-card path, so `writable`
    // can no longer make a disc asset reach the delete/open sequence below.
    // Only the MicroCard volume and the screenshot destination arrive here
    // writable; read handles still take "rb" (rawfile.c FILE_ERR_BADMODE is
    // reported by m2_host_raw_write, not by libc).
    std::error_code fec;
    const bool exists = std::filesystem::is_regular_file(p, fec) && !fec;
    if (!exists && !create_ok) {
        std::fprintf(stderr, "[fs] m2_host_raw_open nofile: %s\n", guest_path);
        return 0;
    }
    // rawfile.c deletes an existing file for the *_NEW modes before opening,
    // so the new handle starts at offset 0 with an empty file. Existence must
    // be re-read after the delete: reusing the pre-delete value would pick
    // "r+b" for a path that no longer exists and refuse every second save.
    bool present = exists;
    if (wipe_first && exists) {
        std::error_code dec;
        std::filesystem::remove(p, dec);
        present = std::filesystem::is_regular_file(p, dec) && !dec;
    }
    // A writable handle needs a writable FILE*; a read-mode one must stay "rb"
    // so the refusal is the boundary's, not libc's. Deriving this from the
    // same rf_Writable set keeps g_raw[slot].writable honest.
    const char* fopen_mode = !writable ? "rb"
                              : !present ? "w+b"
                                         : "r+b";
    std::FILE* f = platform::fopen(p, fopen_mode);
    if (!f) {
        std::fprintf(stderr, "[fs] m2_host_raw_open fopen(%s) fail: %s\n",
                     fopen_mode, platform::path_to_utf8(p).c_str());
        return 0;
    }
    long cur = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, cur, SEEK_SET);
    m2host::trace::trace_asset_load(guest_path, 0, (uint32_t)sz, 0, fs_guest_frame());
    // The slot was reserved before any destructive work at the top of this
    // function and the pool lock is still held here.
    g_raw[slot].fp = f;
    g_raw[slot].size = (uint32_t)sz;
    g_raw[slot].writable = writable;
    g_raw[slot].screenshot_path = screenshot ? p : std::filesystem::path{};
    g_raw[slot].handle = raw_make_handle((uint32_t)slot);
    return g_raw[slot].handle;
}

uint32_t m2_host_raw_open(const char* guest_path) {
    return raw_open_mode(guest_path, M2_FILEOPEN_READ);
}

uint32_t m2_host_raw_open_mode(const char* guest_path, uint32_t mode) {
    return raw_open_mode(guest_path, mode);
}

uint32_t m2_host_raw_size(uint32_t host_token) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_slot(host_token);
    if (slot < 0) return 0;
    return g_raw[slot].size;
}

int32_t m2_host_raw_read(uint32_t host_token, void* dst, uint32_t n) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    if (!dst) return -1;
    int slot = raw_find_slot(host_token);
    if (slot < 0) return -1;
    size_t got = std::fread(dst, 1, n, g_raw[slot].fp);
    m2host::perf::add_bytes(m2host::perf::Phase::FileIO, got);
    m2host::perf::add_counter(m2host::perf::Counter::FileBytes, got);
    return (int32_t)got;   // 0..n or short on EOF; never negative on success
}

int m2_host_raw_read_at_present = 1;

int32_t m2_host_raw_read_at(uint32_t host_token, uint64_t offset,
                            void* dst, uint32_t n) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    if (!dst || n > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return -1;
    int slot = raw_find_slot(host_token);
    if (slot < 0) return -1;
    const uint64_t file_size = g_raw[slot].size;
    if (offset > file_size || static_cast<uint64_t>(n) > file_size - offset)
        return -1;
    if (n == 0) return 0;
    if (offset > static_cast<uint64_t>(std::numeric_limits<long>::max()))
        return -1;

    std::fpos_t saved;
    if (std::fgetpos(g_raw[slot].fp, &saved) != 0 ||
        std::fseek(g_raw[slot].fp, static_cast<long>(offset), SEEK_SET) != 0)
        return -1;
    size_t got = std::fread(dst, 1, n, g_raw[slot].fp);
    int restore = std::fsetpos(g_raw[slot].fp, &saved);
    m2host::perf::add_bytes(m2host::perf::Phase::FileIO, got);
    m2host::perf::add_counter(m2host::perf::Counter::FileBytes, got);
    if (restore != 0 || got != n) return -1;
    return static_cast<int32_t>(got);
}

int32_t m2_host_raw_write(uint32_t host_token, const void* src, uint32_t n) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    if (!src) return -1;
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_slot(host_token);
    if (slot < 0) return -1;
    // rawfile.c:570-571 refuses a RawFile opened read-only. Every disc file is
    // opened "rb", so those take this arm and report a truthful error rather
    // than a silent no-op; only a MicroCard volume opened for writing passes.
    if (!g_raw[slot].writable) return -1;
    if (n == 0) return 0;
    std::vector<uint8_t> captured;
    if (!g_raw[slot].screenshot_path.empty()) {
        // Dsply_screenshot and shooter2d writerawimage each supply bm_Buffer and
        // the complete span of that Bitmap. GPU pixels are not mirrored into
        // guest RAM: read that exact addressed target on the render thread
        // instead of writing stale RAM or a host frame presented earlier. Both
        // guests treat a failed write as fatal, so only those sizes are valid.
        PPCContext* c = m2_recomp_context();
        const uintptr_t start = c ? reinterpret_cast<uintptr_t>(c->mem) : 0;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(src);
        if (guest_screenshot::height_for_bytes(n) == 0 || !start || addr < start ||
            addr - start > RECOMP_GUEST_MEM - n || std::ftell(g_raw[slot].fp) != 0) return -1;
        captured.resize(n);
        if (m2_guest_graphics_read_framebuffer_c(uint32_t(addr - start), captured.data(), n) != int32_t(n))
            return -1;
        src = captured.data();
    }
    size_t put = std::fwrite(src, 1, n, g_raw[slot].fp);
    m2host::perf::add_bytes(m2host::perf::Phase::FileIO, put);
    m2host::perf::add_counter(m2host::perf::Counter::FileBytes, put);
    if (put != n) return -1;   // short write reports negative, as WriteRawFile does
    if (std::fflush(g_raw[slot].fp) != 0) return -1;
    if (!captured.empty()) {
        const int height = guest_screenshot::height_for_bytes(n);
        const bool preview = screenshot_ppm(g_raw[slot].screenshot_path, captured.data(), height);
        std::fprintf(stderr, "[guest-screenshot] saved %s (%dx%d RGB555+SSB)%s\n",
            platform::path_to_utf8(g_raw[slot].screenshot_path).c_str(),
            guest_screenshot::width, height,
            preview ? " and .ppm preview" : "; .ppm preview failed");
    }
    return (int32_t)put;
}

int32_t m2_host_raw_seek(uint32_t host_token, int32_t offset, int32_t whence) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_slot(host_token);
    if (slot < 0) return -1;
    return seek_raw_file(g_raw[slot].fp, g_raw[slot].size, offset, whence);
}

void m2_host_raw_close(uint32_t host_token) {
    m2host::perf::Scope io_scope(m2host::perf::Phase::FileIO);
    m2host::perf::add_counter(m2host::perf::Counter::FileOperations);
    std::lock_guard<std::mutex> pool_lock(g_raw_pool_mutex);
    int slot = raw_find_slot(host_token);
    if (slot < 0) return;
    std::fclose(g_raw[slot].fp);
    g_raw[slot].fp = nullptr;
    g_raw[slot].handle = 0;
    g_raw[slot].size = 0;
    g_raw[slot].writable = false;
    g_raw[slot].screenshot_path.clear();
}

}  // extern "C"

void filesystem_shim_register() {
    using namespace m2host;
    // Folio 0/1/10/32/33/34 are pinned; the typed gk_* handlers in
    // recomp_call_fs_dispatch cover the complete repinned 35..40 raw-file
    // window and reach host bytes through the weak m2_host_raw_* accessors
    // (strong defs above). A disc file is opened read-only, so a write through
    // it reports a truthful negative (rawfile.c FILE_ERR_BADMODE); the
    // writable surface is the MicroCard volume the guest reaches as
    // "/remote", which OpenRawFile(FILEOPEN_WRITE*) opens for writing.
    // SeekRawFile is forwarded with the source C-style origin values. The C
    // symbols m2_SetFileAttrs / m2_ClearRawFileError remain for any other
    // entry point that may call them, but they are not part of the repinned
    // 35..40 table.
    folio_register(M2_FOLIO_FILESYSTEM, 0, reinterpret_cast<void*>(&m2_OpenFile));
    folio_register(M2_FOLIO_FILESYSTEM, 1, reinterpret_cast<void*>(&m2_CloseFile));
    folio_register(M2_FOLIO_FILESYSTEM, 10, reinterpret_cast<void*>(&m2_CreateAlias));
    folio_register(M2_FOLIO_FILESYSTEM, 32, reinterpret_cast<void*>(&m2_OpenDirPath));
    folio_register(M2_FOLIO_FILESYSTEM, 33, reinterpret_cast<void*>(&m2_ReadDir));
    folio_register(M2_FOLIO_FILESYSTEM, 34, reinterpret_cast<void*>(&m2_CloseDir));
}
