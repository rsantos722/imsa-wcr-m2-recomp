#include "platform.h"

#include <cstdlib>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <shellapi.h>
#include <timeapi.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#endif

namespace m2host::platform {
namespace fs = std::filesystem;

#if defined(_WIN32)
namespace {
std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    out.resize(static_cast<size_t>(n) - 1);
    return out;
}
}  // namespace
#endif

std::FILE* fopen(const fs::path& p, const char* mode) {
#if defined(_WIN32)
    return ::_wfopen(p.c_str(), widen(mode).c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

std::string get_env(const char* name) {
#if defined(_WIN32)
    const wchar_t* v = ::_wgetenv(widen(name).c_str());
    return v ? path_to_utf8(fs::path(v)) : std::string{};
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

fs::path get_env_path(const char* name) {
    const std::string v = get_env(name);
    return v.empty() ? fs::path{} : path_from_utf8(v);
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    ::_wputenv_s(widen(name).c_str(), widen(value).c_str());
#else
    ::setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#if defined(_WIN32)
    ::_wputenv_s(widen(name).c_str(), L"");
#else
    ::unsetenv(name);
#endif
}

int process_id() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out(argv, argv + argc);
#if defined(_WIN32)
    int wargc = 0;
    if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        if (wargc == argc)
            for (int i = 0; i < argc; ++i) out[i] = path_to_utf8(fs::path(wargv[i]));
        LocalFree(wargv);
    }
#endif
    return out;
}

void enable_fine_timer_resolution() {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
}

fs::path executable_dir() {
#if defined(_WIN32)
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path();
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(buf.data(), ec);
        return ec ? fs::path(buf.data()).parent_path() : p.parent_path();
    }
    return {};
#elif defined(__linux__)
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : exe.parent_path();
#else
    return {};
#endif
}

fs::path find_runtime_icon(const fs::path& exe_dir) {
    if (exe_dir.empty()) return {};
    const fs::path candidates[] = {
        // Beside the executable (install or build tree copy)
        exe_dir / "imsa-wcr-m2-recomp-256.png",
        // macOS app bundle: Contents/MacOS/../Resources
        exe_dir / ".." / "Resources" / "imsa-wcr-m2-recomp-256.png",
        // Linux install: bin/../share/icons/hicolor
        exe_dir / ".." / "share" / "icons" / "hicolor" / "256x256" / "apps" / "imsa-wcr-m2-recomp.png",
        // CMake build tree: build/launcher/ beside build/host/
        exe_dir / ".." / "launcher" / "imsa-wcr-m2-recomp-256.png",
        // CMake multi-config build tree: build/launcher/<config>/
        exe_dir / ".." / ".." / "launcher" / exe_dir.filename() / "imsa-wcr-m2-recomp-256.png",
        // Source tree repository layout (developer fallback)
        exe_dir / ".." / "packaging" / "icons" / "imsa-wcr-m2-recomp-256.png",
        exe_dir / ".." / ".." / "packaging" / "icons" / "imsa-wcr-m2-recomp-256.png",
        exe_dir / ".." / ".." / ".." / "packaging" / "icons" / "imsa-wcr-m2-recomp-256.png",
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) return candidate.lexically_normal();
    }
    return {};
}

void apply_platform_app_icon(const fs::path& icon_path) {
#if defined(__APPLE__)
    if (icon_path.empty()) return;
    const std::string utf8 = path_to_utf8(icon_path);
    Class cls_NSApplication = objc_getClass("NSApplication");
    if (!cls_NSApplication) return;
    id app = ((id (*)(id, SEL))objc_msgSend)((id)cls_NSApplication, sel_registerName("sharedApplication"));
    if (!app) return;

    Class cls_NSString = objc_getClass("NSString");
    if (!cls_NSString) return;
    id str = ((id (*)(id, SEL, const char*))objc_msgSend)((id)cls_NSString, sel_registerName("stringWithUTF8String:"), utf8.c_str());
    if (!str) return;

    Class cls_NSURL = objc_getClass("NSURL");
    if (!cls_NSURL) return;
    id url = ((id (*)(id, SEL, id))objc_msgSend)((id)cls_NSURL, sel_registerName("fileURLWithPath:"), str);
    if (!url) return;

    Class cls_NSImage = objc_getClass("NSImage");
    if (!cls_NSImage) return;
    id image_alloc = ((id (*)(id, SEL))objc_msgSend)((id)cls_NSImage, sel_registerName("alloc"));
    id image = ((id (*)(id, SEL, id))objc_msgSend)(image_alloc, sel_registerName("initWithContentsOfURL:"), url);
    if (!image) return;

    ((void (*)(id, SEL, id))objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), image);

    id tile = ((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("dockTile"));
    if (tile) {
        struct CGSize { double width; double height; };
        struct CGPoint { double x; double y; };
        struct CGRect { CGPoint origin; CGSize size; };
        CGRect frame = {{0, 0}, {128, 128}};

        Class cls_NSImageView = objc_getClass("NSImageView");
        if (cls_NSImageView) {
            id iv_alloc = ((id (*)(id, SEL))objc_msgSend)((id)cls_NSImageView, sel_registerName("alloc"));
            id iv = ((id (*)(id, SEL, CGRect))objc_msgSend)(iv_alloc, sel_registerName("initWithFrame:"), frame);
            if (iv) {
                ((void (*)(id, SEL, id))objc_msgSend)(iv, sel_registerName("setImage:"), image);
                ((void (*)(id, SEL, long))objc_msgSend)(iv, sel_registerName("setImageScaling:"), 0); // NSImageScaleProportionallyDown
                ((void (*)(id, SEL, id))objc_msgSend)(tile, sel_registerName("setContentView:"), iv);
                ((void (*)(id, SEL))objc_msgSend)(tile, sel_registerName("display"));
            }
        }
    }
#else
    (void)icon_path;
#endif
}

bool directory_writable(const fs::path& dir) {
    const fs::path probe = dir / ".imsa-write-test";
    {
        std::ofstream out(probe);
        if (!out) return false;
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

fs::path portable_root(const fs::path& exe_dir) {
    std::error_code ec;
    if (!fs::is_regular_file(exe_dir / "portable.txt", ec)) return {};
    const fs::path root = exe_dir.parent_path();
#if !defined(_WIN32)
    if (!directory_writable(root)) return {};
#endif
    return root;
}

fs::path microcard_dir_beside(const fs::path& executable) {
    if (executable.empty()) return {};
    const fs::path exe_dir = executable.parent_path();
    const fs::path root = portable_root(exe_dir);
    if (!root.empty()) return root / "userdata" / "card";
    return exe_dir / "saves" / "card";
}

fs::path default_microcard_dir_for(const fs::path& executable) {
#if defined(_WIN32)
    if (const fs::path beside = microcard_dir_beside(executable); !beside.empty())
        return beside;
#elif defined(__APPLE__)
    (void)executable;
#else
    if (!executable.empty() && !portable_root(executable.parent_path()).empty())
        return microcard_dir_beside(executable);
#endif
    return default_microcard_dir();
}

fs::path default_microcard_dir() {
#if defined(_WIN32)
    if (const fs::path dir = executable_dir(); !dir.empty())
        return microcard_dir_beside(dir / "m2host.exe");
#else
#if !defined(__APPLE__)
    if (const fs::path dir = executable_dir(); !portable_root(dir).empty())
        return microcard_dir_beside(dir / "m2host");
#endif
    if (const fs::path xdg = get_env_path("XDG_DATA_HOME"); !xdg.empty())
        return fs::absolute(xdg) / "imsa-wcr-m2-recomp" / "card";
    if (const fs::path home = get_env_path("HOME"); !home.empty()) {
#if defined(__APPLE__)
        return home / "Library" / "Application Support" / "imsa-wcr-m2-recomp" / "card";
#else
        return home / ".local" / "share" / "imsa-wcr-m2-recomp" / "card";
#endif
    }
#endif
    std::error_code ec;
    return fs::temp_directory_path(ec) / "imsa-wcr-m2-recomp" / "card";
}

fs::path default_screenshot_dir() {
    return default_screenshot_dir_for(fs::path{});
}

fs::path default_screenshot_dir_for(const fs::path& executable) {
    return default_microcard_dir_for(executable).parent_path() / "screenshots";
}

}  // namespace m2host::platform
