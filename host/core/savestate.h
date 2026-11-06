// savestate.h — save states: full guest machine snapshot/restore.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "ppc_ctx.h"

namespace m2host {
namespace savestate {

constexpr uint32_t kMagic = 0x4D325356u;  // 'M2SV'
constexpr uint32_t kVersion = 1u;
constexpr uint32_t kRamPageSize = 4096u;

// FNV-1a64 over the canonical live state: register block, then every guest
// RAM byte raw. Stable across runs/builds on the same host.
uint64_t state_hash();
uint64_t state_hash_of(const PPCContext& ctx);

struct SaveResult {
    bool ok = false;
    uint64_t hash = 0;  // live-state digest (state_hash_of); names the file
    std::filesystem::path path;
    std::string error;
};

// Serialize the current guest machine to `path` (written via temp+rename).
SaveResult save_to_file(const std::filesystem::path& path);
// Deserialize into the current guest machine. Verifies magic/version/
// geometry AND the stamped hash before mutating any live state.
SaveResult load_from_file(const std::filesystem::path& path);

std::filesystem::path default_save_dir();  // $M2_SAVESTATE_DIR or ./saves

// F5/F9 hotkey handler for the host frame loops.
class Hotkeys {
public:
    explicit Hotkeys(std::filesystem::path dir);

    // Feed one broker event; consumes only fresh M2_EV_KEY downs that map to
    // a savestate action. Returns true when the event was handled.
    bool handle_event(uint16_t what, int32_t fresh_down, uint32_t keycode);

    bool save_next_slot();  // F5: slot 1..8 rotating
    bool load_latest();     // F9: newest slot*.state by mtime
    bool load_slot(int slot);

    const std::filesystem::path& dir() const { return m_dir; }
    int last_slot() const { return m_last_slot; }

private:
    std::filesystem::path m_dir;
    int m_next_slot = 1;
    int m_last_slot = 0;
};

}  // namespace savestate
}  // namespace m2host
