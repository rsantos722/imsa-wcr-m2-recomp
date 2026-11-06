// test_platform.cpp — platform helpers: executable dir, runtime icon resolution.
#include "platform.h"
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
    std::fprintf(stderr, "FAIL %s: got '%s' expected '%s'\n", what,
                 actual.string().c_str(), expected.string().c_str());
    ++g_failures;
}

void expect_true(const char* what, bool condition) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++g_failures;
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "dummy";
}

}  // namespace

int main() {
    // 1. Executable dir
    const fs::path exe_dir = m2host::platform::executable_dir();
    expect_true("executable_dir is non-empty", !exe_dir.empty());
    expect_true("executable_dir exists", fs::is_directory(exe_dir));

    // 2. find_runtime_icon with empty / invalid paths
    expect("find_runtime_icon empty", m2host::platform::find_runtime_icon({}), {});
    expect("find_runtime_icon invalid",
           m2host::platform::find_runtime_icon("/non/existent/path/never/used"), {});

    // 3. find_runtime_icon mock fixtures
    std::error_code ec;
    const fs::path fixture_base =
        fs::temp_directory_path(ec) / ("m2_platform_test_icons" + test_scratch_suffix());
    fs::remove_all(fixture_base, ec);
    fs::create_directories(fixture_base, ec);

    // Beside executable
    const fs::path beside_dir = fixture_base / "beside" / "bin";
    fs::create_directories(beside_dir, ec);
    const fs::path beside_icon = beside_dir / "imsa-wcr-m2-recomp-256.png";
    touch(beside_icon);
    expect("find_runtime_icon beside executable",
           m2host::platform::find_runtime_icon(beside_dir),
           beside_icon.lexically_normal());

    // macOS app bundle: Contents/MacOS and Contents/Resources
    const fs::path macos_bin_dir = fixture_base / "bundle" / "App.app" / "Contents" / "MacOS";
    fs::create_directories(macos_bin_dir, ec);
    const fs::path bundle_icon = fixture_base / "bundle" / "App.app" / "Contents" / "Resources" / "imsa-wcr-m2-recomp-256.png";
    touch(bundle_icon);
    expect("find_runtime_icon app bundle Resources",
           m2host::platform::find_runtime_icon(macos_bin_dir),
           bundle_icon.lexically_normal());

    // Linux install: prefix/bin and prefix/share/icons/hicolor/...
    const fs::path linux_bin_dir = fixture_base / "linux" / "bin";
    fs::create_directories(linux_bin_dir, ec);
    const fs::path linux_icon = fixture_base / "linux" / "share" / "icons" / "hicolor" / "256x256" / "apps" / "imsa-wcr-m2-recomp.png";
    touch(linux_icon);
    expect("find_runtime_icon linux share icons",
           m2host::platform::find_runtime_icon(linux_bin_dir),
           linux_icon.lexically_normal());

    // Build tree: build/host vs build/launcher
    const fs::path build_host_dir = fixture_base / "build" / "host";
    fs::create_directories(build_host_dir, ec);
    const fs::path build_launcher_icon = fixture_base / "build" / "launcher" / "imsa-wcr-m2-recomp-256.png";
    touch(build_launcher_icon);
    expect("find_runtime_icon build tree launcher dir",
           m2host::platform::find_runtime_icon(build_host_dir),
           build_launcher_icon.lexically_normal());

    // Clean up fixtures
    fs::remove_all(fixture_base, ec);

    // 4. apply_platform_app_icon safe with empty or non-existent path
    m2host::platform::apply_platform_app_icon({});
    m2host::platform::apply_platform_app_icon("/non/existent/icon.png");

    if (g_failures != 0) {
        std::fprintf(stderr, "m2_platform_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("m2_platform_test: PASS\n");
    return 0;
}
