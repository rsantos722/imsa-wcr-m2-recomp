#pragma once
#include <filesystem>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace launcher_saves {
struct Summary { size_t files = 0; uint64_t bytes = 0; std::string error; };
struct Result { bool ok; std::string message; std::filesystem::path backup; };
// The card shared with `host_executable`: M2_MICROCARD_ROOT when set,
// otherwise that host's platform default. The launcher passes the host path it
// will launch (not its own directory), so save management and gameplay address
// one directory even when CMake splits them into build/launcher and build/host,
// including multi-config configuration subdirectories.
std::filesystem::path save_root(const std::filesystem::path& host_executable);
Summary inspect(const std::filesystem::path& root);
std::vector<std::filesystem::path> backups(const std::filesystem::path& directory);
Result backup(const std::filesystem::path& root, const std::filesystem::path& directory);
// Restore/reset preserve the current card in a backup before replacing it.
Result restore(const std::filesystem::path& root, const std::filesystem::path& directory,
               const std::filesystem::path& selected);
Result reset(const std::filesystem::path& root, const std::filesystem::path& directory);
}
