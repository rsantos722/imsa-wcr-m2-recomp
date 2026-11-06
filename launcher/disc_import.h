#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher_disc {
struct Progress {
    std::atomic<float> fraction{0};
    std::atomic<bool> cancel{false};
};
struct Result { bool ok; std::string message; };
// One installation per launcher settings directory. No caller-selected data path.
std::filesystem::path install_root(const std::filesystem::path& settings);
bool installed(const std::filesystem::path& root);
Result import_iso(const std::filesystem::path& iso, const std::filesystem::path& root,
                  Progress& progress);
// The source image import_iso verifies before extracting. The overload lets
// tests check digest rejection with a small image instead of a full-size one.
struct IsoSpec { uint64_t size; const char* sha256; };
Result import_iso(const std::filesystem::path& iso, const std::filesystem::path& root,
                  Progress& progress, const IsoSpec& expected);
}
