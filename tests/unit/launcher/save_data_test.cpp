#include "save_data.h"
#include "host_path.h"
#include "platform.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if !defined(_WIN32)
#include <unistd.h>
#endif
namespace fs = std::filesystem;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary); f.write(bytes.data(),bytes.size());
    if (!f) throw std::runtime_error("fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),{}};
}
// Restores the caller's M2_MICROCARD_ROOT even when a check aborts the test.
struct EnvRestore {
    std::string value;
    ~EnvRestore() {
        if (value.empty()) m2host::platform::unset_env("M2_MICROCARD_ROOT");
        else m2host::platform::set_env("M2_MICROCARD_ROOT", value.c_str());
    }
};
// Windows resolves the card beside the host executable. These fixtures create
// both executables, run the launcher's real host discovery, then apply that
// policy, so every build layout is covered on any host platform. They simulate
// the Windows path rules; no Windows binaries are executed.
void check_save_root_layouts(const fs::path& temp) {
    using m2host::platform::microcard_dir_beside;
    const fs::path host_name = launcher_host::executable_name();
    const auto touch = [](const fs::path& file) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file).close();
    };
    // Portable install: launcher and host ship in one directory, so the card
    // stays beside the host exactly as before this change.
    const fs::path portable = temp / "portable";
    touch(portable / "m2launcher");
    touch(portable / host_name);
    const fs::path portable_host = launcher_host::find_host_executable(portable);
    check(portable_host == portable / host_name,
          "fixture launcher did not find the host beside itself");
    check(microcard_dir_beside(portable_host) == portable / "saves" / "card",
          "portable card is not beside the host executable");
    // Portable release package: both executables and the marker in bin, with
    // saves, settings and game files in the userdata folder beside bin.
    const fs::path package = temp / "package";
    touch(package / "bin" / "m2launcher");
    touch(package / "bin" / host_name);
    touch(package / "bin" / "portable.txt");
    const fs::path package_host = launcher_host::find_host_executable(package / "bin");
    check(package_host == package / "bin" / host_name,
          "fixture launcher did not find the host in the package bin folder");
    check(microcard_dir_beside(package_host) == package / "userdata" / "card",
          "package card is not in the top-level userdata folder");
    check(m2host::platform::portable_root(package / "bin") == package,
          "package bin folder did not resolve to the package root");
    check(m2host::platform::portable_root(portable).empty(),
          "a folder without the marker was treated as a package");
#if defined(__APPLE__)
    check(m2host::platform::default_microcard_dir_for(package_host) ==
              m2host::platform::default_microcard_dir(),
          "macOS card moved into a portable package folder");
#else
    check(m2host::platform::default_microcard_dir_for(package_host) ==
              package / "userdata" / "card",
          "portable package does not default to its userdata folder");
#endif
    // Same folder name without the marker, like /usr/bin or the AppImage.
    const fs::path system_bin = temp / "usr" / "bin";
    touch(system_bin / host_name);
    check(m2host::platform::portable_root(system_bin).empty(),
          "a bin folder without the marker was treated as a package");
    check(m2host::platform::directory_writable(package),
          "a writable folder was reported as read-only");
    check(fs::directory_iterator(package) != fs::directory_iterator() &&
              !fs::exists(package / ".imsa-write-test"),
          "the write check left its file behind");
#if !defined(_WIN32)
    // A package unpacked somewhere read-only, like /opt. Root ignores folder
    // permissions, so this cannot be checked when running as root.
    if (geteuid() != 0) {
        const fs::path locked = temp / "locked";
        touch(locked / "bin" / "portable.txt");
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        const bool writable = m2host::platform::directory_writable(locked);
        const fs::path root = m2host::platform::portable_root(locked / "bin");
        fs::permissions(locked, fs::perms::owner_all);
        check(!writable, "a read-only folder was reported as writable");
        check(root.empty(), "a read-only package did not fall back to the user's data folder");
    }
#endif
    // CMake single-config build: build/launcher/m2launcher and
    // build/host/m2host. The launcher must use the host's directory, not its
    // own, or save management and a standalone host split again.
    const fs::path build = temp / "split" / "build";
    touch(build / "launcher" / "m2launcher");
    touch(build / "host" / host_name);
    const fs::path split_host = launcher_host::find_host_executable(build / "launcher");
    check(split_host == build / "host" / host_name,
          "fixture launcher did not find the host in the split build tree");
    check(microcard_dir_beside(split_host) == build / "host" / "saves" / "card",
          "split build card is not beside the host executable");
    check(microcard_dir_beside(split_host) != build / "launcher" / "saves" / "card",
          "split build card still follows the launcher directory");
    // Multi-config generators add a configuration directory to both trees.
    for (const char* config : {"Debug", "Release"}) {
        const fs::path launcher = build / "launcher" / config;
        const fs::path host = build / "host" / config / host_name;
        touch(launcher / "m2launcher");
        touch(host);
        const fs::path found = launcher_host::find_host_executable(launcher);
        check(found == host, "fixture launcher did not find the multi-config host");
        check(microcard_dir_beside(found) == build / "host" / config / "saves" / "card",
              "multi-config card is not beside the host executable");
    }
    check(microcard_dir_beside(build / "host" / "Debug" / host_name) !=
              microcard_dir_beside(build / "host" / "Release" / host_name),
          "multi-config layouts lost their per-configuration card");
    // The Controls screenshot tooltip follows the selected host too. On
    // Windows that means <host dir>/saves/screenshots in the split build
    // tree, not the launcher's own directory; macOS/Linux keep the existing
    // user-profile policy regardless of where the host lives. The path
    // arithmetic below is portable, so this covers the Windows layout on any
    // host platform; only a Windows build executes the branch in
    // default_screenshot_dir_for.
    check(microcard_dir_beside(split_host).parent_path() / "screenshots" ==
              build / "host" / "saves" / "screenshots",
          "screenshot path arithmetic does not follow the host card");
#if defined(_WIN32)
    check(m2host::platform::default_screenshot_dir_for(split_host) ==
              build / "host" / "saves" / "screenshots",
          "split build screenshots are not beside the host executable");
    check(m2host::platform::default_screenshot_dir_for(split_host) !=
              build / "launcher" / "saves" / "screenshots",
          "split build screenshots still follow the launcher directory");
#else
    check(m2host::platform::default_screenshot_dir_for(split_host) ==
              m2host::platform::default_screenshot_dir(),
          "screenshot default changed on this platform");
#endif
    check(m2host::platform::default_screenshot_dir_for({}) ==
              m2host::platform::default_screenshot_dir(),
          "empty host path changed the screenshot default");
    // An explicit M2_MICROCARD_ROOT wins over every layout and is made
    // absolute, matching the host's precedence.
    EnvRestore restore{m2host::platform::get_env("M2_MICROCARD_ROOT")};
    const fs::path override_root = temp / "override saves" / "card";
    m2host::platform::set_env("M2_MICROCARD_ROOT",
                              m2host::platform::path_to_utf8(override_root).c_str());
    check(launcher_saves::save_root(split_host) == override_root,
          "M2_MICROCARD_ROOT did not override the launcher save root");
    m2host::platform::set_env("M2_MICROCARD_ROOT", "relative-card");
    check(launcher_saves::save_root(portable_host) == fs::absolute("relative-card"),
          "relative M2_MICROCARD_ROOT was not made absolute");
    // With no override the launcher takes the host's platform default.
    if (restore.value.empty()) m2host::platform::unset_env("M2_MICROCARD_ROOT");
    else m2host::platform::set_env("M2_MICROCARD_ROOT", restore.value.c_str());
#if defined(_WIN32)
    check(m2host::platform::default_microcard_dir_for(split_host) ==
              microcard_dir_beside(split_host),
          "Windows host default is not beside the host executable");
#else
    check(m2host::platform::default_microcard_dir_for(split_host) ==
              m2host::platform::default_microcard_dir(),
          "launcher host default disagrees with the host platform default");
#endif
    // Nothing locatable falls back to the running executable's policy.
    check(m2host::platform::default_microcard_dir_for({}) ==
              m2host::platform::default_microcard_dir(),
          "empty host path did not fall back to the executable default");
    check(microcard_dir_beside({}).empty(), "empty host path produced a card path");
}
int main() {
    const auto temp=fs::temp_directory_path()/("imsa-saves-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto card=temp/"card", backups=temp/"backups";
    try {
        check_save_root_layouts(temp);
        check(launcher_saves::inspect(card).files==0,"fresh profile should have no saves");
        const std::string prefs("\x01\x00\x7f\xff",4);
        write(card/"IMSAprefs",prefs);
        write(card/"IMSA_HiScores","racing scores");
        write(card/"shooter2d/highscore","shooter scores");
        write(card/"empty","");
        auto summary=launcher_saves::inspect(card);
        check(summary.error.empty() && summary.files==4 && summary.bytes==31,"save inventory wrong");
        auto first=launcher_saves::backup(card,backups);
        check(first.ok,first.message.c_str());
        check(read(first.backup/"card/IMSAprefs")==prefs,"backup changed binary save bytes");
        write(card/"IMSAprefs","new prefs");
        write(card/"new-only","new file");
        check(!launcher_saves::restore(card,backups,{}).ok,"empty selection reset saves");
        auto restored=launcher_saves::restore(card,backups,first.backup);
        check(restored.ok,restored.message.c_str());
        check(read(card/"IMSAprefs")==prefs && !fs::exists(card/"new-only"),"restore merged or changed original saves");
        check(read(restored.backup/"card/IMSAprefs")=="new prefs" && fs::exists(restored.backup/"card/new-only"),"restore failed to preserve previous progress");
        check(read(card/"shooter2d/highscore")=="shooter scores","second game's save was lost");
        // Same-sized corruption must fail, not just a missing-file check.
        write(first.backup/"card/IMSAprefs","oops");
        check(!launcher_saves::restore(card,backups,first.backup).ok,"corrupt backup accepted");
        check(read(card/"IMSAprefs")==prefs,"failed restore touched current saves");
        auto reset=launcher_saves::reset(card,backups);
        check(reset.ok,reset.message.c_str());
        check(launcher_saves::inspect(card).files==0,"reset did not clear both games' saves");
        check(read(reset.backup/"card/IMSAprefs")==prefs,"reset did not back up current data");
        check(launcher_saves::restore(card,backups,reset.backup).ok,"could not undo reset");
        auto listed=launcher_saves::backups(backups);
        check(listed.size()==4,"backup list omitted safety backups");
        // Malformed records, escaping names and failed safety backups cannot reset saves.
        const auto invalid=backups/"invalid";
        fs::create_directories(invalid/"card");
        write(invalid/"backup.txt","IMSA-CARD 1\n\"unfinished");
        check(!launcher_saves::restore(card,backups,invalid).ok,"truncated record accepted");
        write(invalid/"backup.txt","IMSA-CARD 1\n\"../outside\" 4 123\n");
        check(!launcher_saves::restore(card,backups,invalid).ok,"escaping record accepted");
        write(temp/"blocked","not a directory");
        check(!launcher_saves::reset(card,temp/"blocked/backups").ok,"reset proceeded without safety backup");
        check(read(card/"IMSAprefs")==prefs,"failed safety backup changed current saves");
        check(!launcher_saves::backup(card,card/"backups").ok,"nested backup permitted recursion");
        std::error_code ec;
        fs::create_symlink(card/"IMSAprefs",card/"link",ec);
        if (!ec) {
            check(!launcher_saves::reset(card,backups).ok,"symlink-containing card was reset");
            check(read(card/"IMSAprefs")==prefs,"link rejection changed save data");
        } else std::cout << "symlink fixture unavailable: " << ec.message() << '\n';
        fs::remove_all(temp);
        std::cout << "save-data management checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; std::error_code ec; fs::remove_all(temp,ec); return 1;
    }
}
