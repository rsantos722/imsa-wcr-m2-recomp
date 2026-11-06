// host_path.h - locate the m2host executable from the launcher's own directory.
#pragma once

#include <algorithm>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>

namespace launcher_host {

// Name of the host executable on this platform.
inline std::filesystem::path executable_name() {
#ifdef _WIN32
    return "m2host.exe";
#else
    return "m2host";
#endif
}

// Candidate locations for the host, in preference order. `base` is the
// directory SDL reports for the running launcher. CMake writes each executable
// into its own build subdirectory (build/launcher/m2launcher and
// build/host/m2host), and multi-config generators add a configuration
// directory to both. Installed trees keep the executables together, and the
// macOS bundle carries its own copy of the host.
inline std::vector<std::filesystem::path> host_candidates(
    const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    const fs::path name = executable_name();

    fs::path dir = base.empty() ? fs::path(".") : base;
    // SDL's base path ends in a separator; drop it before walking up.
    if (dir.filename().empty()) dir = dir.parent_path();

    std::vector<fs::path> candidates;
    const auto add = [&](const fs::path& path) {
        candidates.push_back(path.lexically_normal());
    };
    const auto add_build_tree = [&](const fs::path& launcher_dir) {
        add(launcher_dir / name);  // installed beside the launcher
        add(launcher_dir / ".." / "host" / name);  // single-config build
        add(launcher_dir / ".." / ".." / "host" / launcher_dir.filename() /
            name);  // multi-config build
    };

#ifdef __APPLE__
    // Inside an app bundle SDL reports Contents/Resources (or Contents/MacOS).
    // CMake stages m2host in Contents/MacOS, and the build tree keeps the
    // bundle in build/launcher with the host in the sibling build/host.
    if (dir.filename() == "Resources" || dir.filename() == "MacOS") {
        add(dir.parent_path() / "MacOS" / name);
        add_build_tree(dir.parent_path().parent_path().parent_path());
        return candidates;
    }
#endif
    add_build_tree(dir);
    return candidates;
}

// Hosts in other CMake build directories of the same source tree, e.g.
// build-game/host/m2host next to a launcher built in build/. A launcher built
// without game data would otherwise start a stub host, so these are only
// considered when the caller can tell whether a host carries the game. Only
// directories with a CMakeCache.txt count, so an installed tree never matches.
inline std::vector<std::filesystem::path> sibling_build_hosts(
    const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    std::vector<fs::path> hosts;
    fs::path dir = base.empty() ? fs::path(".") : base;
    if (dir.filename().empty()) dir = dir.parent_path();
#ifdef __APPLE__
    if (dir.filename() == "Resources" || dir.filename() == "MacOS")
        dir = dir.parent_path().parent_path().parent_path();
#endif
    // Single-config: <root>/<build>/launcher. Multi-config adds a config dir:
    // <root>/<build>/launcher/<config>. Anything else (installed trees) has no
    // sibling builds, and the search never leaves the source tree.
    fs::path root;
    fs::path config;
    if (dir.filename() == "launcher") {
        root = dir / ".." / "..";
    } else if (dir.parent_path().filename() == "launcher") {
        root = dir / ".." / ".." / "..";
        config = dir.filename();
    } else {
        return hosts;
    }
    std::error_code ec;
    std::vector<fs::path> builds;
    for (fs::directory_iterator it(root.lexically_normal(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (fs::is_regular_file(it->path() / "CMakeCache.txt", ec))
            builds.push_back(it->path());
    }
    std::sort(builds.begin(), builds.end());
    for (const auto& build : builds) {
        hosts.push_back((build / "host" / config / executable_name()).lexically_normal());
    }
    return hosts;
}

// The host to run. Without `has_guest`, the first existing candidate, or the
// preferred location when none exists so the caller can report the path it
// expected. With `has_guest`, the first existing host that reports the
// recompiled game, searching sibling build directories after the usual
// locations; when no host has it, the first existing candidate as before.
inline std::filesystem::path find_host_executable(
    const std::filesystem::path& base,
    const std::function<bool(const std::filesystem::path&)>& has_guest = {}) {
    std::error_code ec;
    const std::vector<std::filesystem::path> candidates = host_candidates(base);
    std::filesystem::path first_existing;
    for (const auto& candidate : candidates) {
        if (!std::filesystem::is_regular_file(candidate, ec)) continue;
        if (!has_guest || has_guest(candidate)) return candidate;
        if (first_existing.empty()) first_existing = candidate;
    }
    if (has_guest) {
        for (const auto& candidate : sibling_build_hosts(base)) {
            if (std::filesystem::is_regular_file(candidate, ec) && has_guest(candidate))
                return candidate;
        }
    }
    if (!first_existing.empty()) return first_existing;
    return candidates.empty() ? executable_name() : candidates.front();
}

}  // namespace launcher_host
