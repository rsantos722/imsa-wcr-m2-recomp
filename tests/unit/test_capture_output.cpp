// test_capture_output.cpp — software --capture output failure reporting.
//
// A requested frame dump must not silently disappear when its directory or
// file cannot be created/written.  The backend retains the first failure for
// the host to turn into a nonzero status; a healthy destination remains a
// successful control.  The short-write case runs in a child with a zero file
// size limit so the flush/close path is exercised without a real full disk.
#include "platform.h"
#include "soft_capture.h"

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

std::string read_file(const fs::path& path)
{
    std::string contents;
    FILE* f = m2host::platform::fopen(path, "rb");
    if (!f) return contents;
    char buffer[512];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) != 0)
        contents.append(buffer, n);
    std::fclose(f);
    return contents;
}

}  // namespace

int main()
{
    std::error_code ec;
    const fs::path base =
        fs::temp_directory_path(ec) /
        ("m2_capture_output_" + std::to_string(m2host::platform::process_id()));
    if (ec) {
        std::fprintf(stderr, "FAIL: temp_directory_path: %s\n",
                     ec.message().c_str());
        return 1;
    }
    fs::remove_all(base, ec);
    ec.clear();
    fs::create_directories(base, ec);
    if (ec) {
        std::fprintf(stderr, "FAIL: create temp root: %s\n",
                     ec.message().c_str());
        return 1;
    }

    // A regular file where the capture directory should be.
    const fs::path blocker = base / "blocked-parent";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "not a directory\n";
    }
    {
        m2host::SoftCaptureBackend backend(8, 6, blocker / "capture");
        backend.frame_present();
        check(backend.has_output_error(),
              "blocked capture directory reports an output error");
        check(backend.output_error().find("create directory") !=
                  std::string::npos,
              "blocked capture directory error names the failed stage");
        check(!fs::exists(blocker / "capture"),
              "blocked capture directory creates no output");
    }

    // The directory exists, but the frame filename is occupied by a directory,
    // so the open (not create_directories) fails.
    fs::create_directories(base / "open-blocked" / "frame_0000.ppm", ec);
    check(!ec, "create open-blocked fixture");
    {
        m2host::SoftCaptureBackend backend(4, 4, base / "open-blocked");
        backend.frame_present();
        check(backend.has_output_error(),
              "unopenable capture frame reports an output error");
        check(backend.output_error().find("open") != std::string::npos,
              "unopenable capture frame error names open");
    }

    // Valid output control: exactly one P6 frame, with a correct header and
    // payload size.
    {
        const fs::path out_dir = base / "valid";
        m2host::SoftCaptureBackend backend(8, 6, out_dir);
        backend.frame_present();
        check(!backend.has_output_error(),
              "valid capture destination reports no output error");
        const fs::path frame = out_dir / "frame_0000.ppm";
        check(fs::exists(frame), "valid capture writes frame_0000.ppm");
        const std::string bytes = read_file(frame);
        const std::string header = "P6\n8 6\n255\n";
        check(bytes.size() == header.size() + 8u * 6u * 3u,
              "valid capture writes a complete PPM payload");
        check(bytes.compare(0, header.size(), header) == 0,
              "valid capture writes a P6 header");
        backend.frame_present();
        check(!backend.has_output_error(),
              "second valid capture frame keeps output healthy");
        check(fs::exists(out_dir / "frame_0001.ppm"),
              "second valid capture writes frame_0001.ppm");
    }

#if !defined(_WIN32)
    // Zero file-size limit with SIGXFSZ ignored: open succeeds, stdio buffers
    // accept the small image, and the first flush/fclose reports EFBIG.  This
    // covers the short-write/close path that a create/open-only test misses.
    {
        const fs::path limited_dir = base / "limited";
        fs::create_directories(limited_dir, ec);
        check(!ec, "create limited-write fixture");
        const pid_t pid = fork();
        if (pid == 0) {
            std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit {};
            limit.rlim_cur = 0;
            limit.rlim_max = 0;
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(77);
            m2host::SoftCaptureBackend backend(8, 6, limited_dir);
            backend.frame_present();
            _exit(backend.has_output_error() ? 0 : 1);
        }
        if (pid < 0) {
            check(false, "fork short-write child");
        } else {
            int status = 0;
            if (waitpid(pid, &status, 0) != pid) {
                check(false, "waitpid short-write child");
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
                std::fprintf(stderr,
                             "SKIP: RLIMIT_FSIZE unavailable for short-write check\n");
            } else {
                check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      "short write/flush/close reports an output error");
            }
        }
    }
#endif

    fs::remove_all(base, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "m2_capture_output_test: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("m2_capture_output_test: PASS\n");
    return 0;
}
