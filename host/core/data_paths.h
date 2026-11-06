// data_paths.h — map a guest filesystem root to the game image root.
#pragma once

#include <filesystem>

namespace m2host {

// Returns the folder containing driving/game for a filesystem data root.
// Accepts the game-data root itself, <root>/driving/data, or <root>/shooter2d.
inline std::filesystem::path find_game_data_root(
    const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (root.empty()) return {};
    auto is_game_root = [](const fs::path& p) {
        std::error_code ec;
        return !p.empty() &&
               fs::is_regular_file(p / "driving" / "game", ec) && !ec;
    };
    if (is_game_root(root)) return root;
    const fs::path parent = root.parent_path();
    if (is_game_root(parent)) return parent;
    const fs::path grandparent = parent.parent_path();
    if (is_game_root(grandparent)) return grandparent;
    return {};
}

// Maps a game data directory to the expected guest asset filesystem root
// for the given boot application.
inline std::filesystem::path resolve_app_asset_root(
    const std::filesystem::path& game_data_dir,
    bool boot_shooter2d) {
    if (game_data_dir.empty()) return {};
    return boot_shooter2d ? (game_data_dir / "shooter2d")
                          : (game_data_dir / "driving" / "data");
}

// Resolves the guest filesystem root (cfg.root) from game_data_dir and an
// explicit root path. If explicit_root is empty or points to the game data
// directory itself, the application asset subfolder (shooter2d or driving/data)
// is selected.
inline std::filesystem::path resolve_guest_root(
    const std::filesystem::path& game_data_dir,
    const std::filesystem::path& explicit_root,
    bool boot_shooter2d) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path app_data = resolve_app_asset_root(game_data_dir, boot_shooter2d);
    if (!game_data_dir.empty() && (explicit_root.empty() || explicit_root == game_data_dir)) {
        if (fs::is_directory(app_data, ec) && !ec) {
            return app_data;
        }
        if (explicit_root.empty()) {
            return app_data;
        }
    }
    return explicit_root;
}

}  // namespace m2host
