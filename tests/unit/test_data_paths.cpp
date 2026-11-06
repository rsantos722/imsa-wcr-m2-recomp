// test_data_paths.cpp — pin the filesystem-root to game-data-root mapping.
#include "data_paths.h"
#include "test_scratch.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(const char* what, const fs::path& actual, const fs::path& expected) {
    if (actual == expected) return;
    std::printf("FAIL %s: got '%s' expected '%s'\n", what,
                actual.string().c_str(), expected.string().c_str());
    ++g_failures;
}

void make_game(const fs::path& root) {
    fs::create_directories(root / "driving");
    std::ofstream(root / "driving" / "game") << "fixture";
}

}  // namespace

int main() {
    const fs::path base =
        fs::temp_directory_path() / ("m2_data_paths_test" + test_scratch_suffix());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    make_game(base);
    fs::create_directories(base / "driving" / "data", ec);
    fs::create_directories(base / "shooter2d", ec);

    expect("game root", m2host::find_game_data_root(base), base);
    expect("driving/data", m2host::find_game_data_root(base / "driving" / "data"),
           base);
    expect("shooter2d", m2host::find_game_data_root(base / "shooter2d"), base);
    const fs::path unrelated =
        fs::temp_directory_path() / ("m2_data_paths_unrelated" + test_scratch_suffix());
    fs::remove_all(unrelated, ec);
    fs::create_directories(unrelated, ec);
    expect("unrelated root", m2host::find_game_data_root(unrelated), {});

    // App asset root resolution
    expect("resolve_app_asset_root driving",
           m2host::resolve_app_asset_root(base, false),
           base / "driving" / "data");
    expect("resolve_app_asset_root shooter2d",
           m2host::resolve_app_asset_root(base, true),
           base / "shooter2d");
    expect("resolve_app_asset_root empty",
           m2host::resolve_app_asset_root({}, false),
           fs::path{});

    // Guest root resolution
    expect("resolve_guest_root default driving",
           m2host::resolve_guest_root(base, {}, false),
           base / "driving" / "data");
    expect("resolve_guest_root default shooter2d",
           m2host::resolve_guest_root(base, {}, true),
           base / "shooter2d");
    expect("resolve_guest_root disc root driving",
           m2host::resolve_guest_root(base, base, false),
           base / "driving" / "data");
    expect("resolve_guest_root disc root shooter2d",
           m2host::resolve_guest_root(base, base, true),
           base / "shooter2d");

    const fs::path custom_path = base / "custom" / "path";
    expect("resolve_guest_root explicit root preserved driving",
           m2host::resolve_guest_root(base, custom_path, false),
           custom_path);
    expect("resolve_guest_root explicit root preserved shooter2d",
           m2host::resolve_guest_root(base, custom_path, true),
           custom_path);
    expect("resolve_guest_root empty game_data_dir",
           m2host::resolve_guest_root({}, custom_path, false),
           custom_path);

    fs::remove_all(base, ec);
    fs::remove_all(unrelated, ec);
    if (g_failures != 0) {
        std::printf("m2_data_paths_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_data_paths_test: PASS\n");
    return 0;
}
