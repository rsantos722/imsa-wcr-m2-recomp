// platform.h — host platform services (macOS, Linux, Windows MinGW-w64).
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace m2host::platform {

// UTF-8 <-> filesystem path. On Windows a narrow fs::path constructor and
// path::string() use the ANSI code page, which mangles non-ASCII user names;
// SDL, wxWidgets and our config files all speak UTF-8.
inline std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
inline std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// fopen that accepts any path (wide _wfopen on Windows).
std::FILE* fopen(const std::filesystem::path& p, const char* mode);

// Environment access. Values are UTF-8 on every platform; an unset or empty
// variable reads back as "".
std::string get_env(const char* name);
std::filesystem::path get_env_path(const char* name);
void set_env(const char* name, const char* value);
void unset_env(const char* name);

int process_id();

// argv as UTF-8. Windows hands main() an ANSI-code-page argv, which cannot
// carry every path the launcher passes; this re-reads the wide command line.
// Elsewhere it is argv unchanged. Always argc entries.
std::vector<std::string> utf8_args(int argc, char** argv);

// Raise the OS sleep granularity for the process. Windows otherwise rounds
// std::this_thread::sleep_for up to its ~15.6 ms default tick, which breaks the
// VBL pacer and the DSPP audio worker's sub-millisecond waits. No-op elsewhere.
void enable_fine_timer_resolution();

// Directory holding the running executable (empty if it cannot be found).
std::filesystem::path executable_dir();

// Look for the runtime 256px icon PNG (imsa-wcr-m2-recomp-256.png) in the usual
// installation, bundle, build, or source locations relative to the executable.
std::filesystem::path find_runtime_icon(const std::filesystem::path& exe_dir = executable_dir());

// Programmatically apply the platform window/dock icon on systems that support it.
void apply_platform_app_icon(const std::filesystem::path& icon_path);

// True when a file can be created in dir. Leaves nothing behind.
bool directory_writable(const std::filesystem::path& dir);

// Top folder of a portable release package (the Windows ZIP and Linux tar.gz)
// when exe_dir is its bin folder, which holds a portable.txt marker. Otherwise
// empty. A marker rather than the folder name keeps /usr/bin and the AppImage
// out of portable mode. Outside Windows a package folder the user cannot write
// to, such as one in /opt, also returns empty, so storage falls back to the
// user's data folder. Windows always stays in the package, and the launcher
// reports a folder it cannot write to.
std::filesystem::path portable_root(const std::filesystem::path& exe_dir);

// Portable storage policy on its own: the writable card directory for the given
// host executable, <root>/userdata/card in a portable package and otherwise
// <exe dir>/saves/card beside the executable (a build tree). Callers pass the
// executable they located rather than relying on the process working directory. Exported separately from
// default_microcard_dir_for so filesystem fixtures can exercise the Windows
// build layouts on every host platform.
std::filesystem::path microcard_dir_beside(const std::filesystem::path& executable);

// Default writable MicroCard volume for a host executable the caller is about
// to run, used when M2_MICROCARD_ROOT is unset. It is never inside the asset
// tree.
//   macOS:   ~/Library/Application Support/imsa-wcr-m2-recomp/card
//   Windows: <package root>/userdata/card, or <host executable directory>/saves/card in a build tree
//   others:  <package root>/userdata/card in the tar.gz, else $XDG_DATA_HOME/imsa-wcr-m2-recomp/card, else ~/.local/share/imsa-wcr-m2-recomp/card
// The launcher passes the host path it will launch, so save management follows
// the same directory as the standalone host even when CMake puts the two
// executables in sibling build trees (build/launcher and build/host, including
// multi-config configuration subdirectories).
std::filesystem::path default_microcard_dir_for(const std::filesystem::path& executable);

// Default writable MicroCard volume shared by m2host and the launcher, used
// when M2_MICROCARD_ROOT is unset. This is the current executable's default;
// callers locating another executable use default_microcard_dir_for.
std::filesystem::path default_microcard_dir();

// Original developer framebuffer dumps, beside the default card directory.
std::filesystem::path default_screenshot_dir();

// Default screenshot destination for a host executable the caller is about to
// run, used when M2_SCREENSHOT_ROOT is unset. On Windows it is the screenshots
// folder beside the card's folder, matching the host's own default even when
// CMake puts the launcher and host in sibling build trees.
// A Linux tar.gz does the same. Other macOS and Linux installs keep the
// default_screenshot_dir() policy, which does not depend on the executable
// location.
std::filesystem::path default_screenshot_dir_for(
    const std::filesystem::path& executable);

}  // namespace m2host::platform
