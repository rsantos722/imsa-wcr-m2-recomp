// test_utf8_path.cpp — runtime file opens with non-ASCII paths.
//
// The runtime receives paths as UTF-8 (the host converts argv and
// --game-data with platform::path_to_utf8). On Windows, passing those bytes
// to narrow fopen() resolves them against the ANSI code page, so an install
// such as C:\Users\Jörg\Games\IMSA cannot load. This creates a directory name
// that requires Unicode and drives both runtime readers through it.
#include "platform.h"
#include "sha256.h"
#include "utf8_path.h"
#include "test_scratch.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// UTF-8 bytes, spelled as escapes so the result does not depend on the
// compiler's source encoding: "m2 utf8 äöü 日本".
constexpr const char* kDirName =
    "m2 utf8 \xc3\xa4\xc3\xb6\xc3\xbc \xe6\x97\xa5\xe6\x9c\xac";
constexpr const char* kFileName = "guest image \xc3\xa9.bin";
constexpr const char* kContents = "guest image payload\n";

int fail(const char* what) {
    std::fprintf(stderr, "m2_utf8_path: FAIL: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path dir =
        fs::temp_directory_path(ec) /
        m2host::platform::path_from_utf8(kDirName + test_scratch_suffix());
    if (ec) return fail("temp_directory_path");
    fs::remove_all(dir, ec);
    ec.clear();
    fs::create_directories(dir, ec);
    if (ec) return fail("create_directories");

    const std::string file = m2host::platform::path_to_utf8(
        dir / m2host::platform::path_from_utf8(kFileName));

    FILE* f = m2_fopen_utf8(file.c_str(), "wb");
    if (!f) return fail("m2_fopen_utf8(wb)");
    const size_t wanted = std::strlen(kContents);
    const size_t wrote = std::fwrite(kContents, 1, wanted, f);
    std::fclose(f);
    if (wrote != wanted) return fail("fwrite");

    f = m2_fopen_utf8(file.c_str(), "rb");
    if (!f) return fail("m2_fopen_utf8(rb)");
    char buffer[64] = {};
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    if (read != wanted || std::memcmp(buffer, kContents, wanted) != 0)
        return fail("readback");

    uint8_t digest[32];
    uint64_t size = 0;
    if (m2_sha256_file(file.c_str(), digest, &size) != 0)
        return fail("m2_sha256_file");
    m2_sha256_ctx ctx;
    m2_sha256_init(&ctx);
    m2_sha256_update(&ctx, kContents, wanted);
    uint8_t expected[32];
    m2_sha256_final(&ctx, expected);
    if (size != static_cast<uint64_t>(wanted) ||
        std::memcmp(digest, expected, sizeof(digest)) != 0)
        return fail("sha256 digest");

    const std::string missing = m2host::platform::path_to_utf8(
        dir / m2host::platform::path_from_utf8("missing \xc3\xa9.bin"));
    f = m2_fopen_utf8(missing.c_str(), "rb");
    if (f) {
        std::fclose(f);
        return fail("missing file opened");
    }

    fs::remove_all(dir, ec);
    std::printf("m2_utf8_path: ok\n");
    return 0;
}
