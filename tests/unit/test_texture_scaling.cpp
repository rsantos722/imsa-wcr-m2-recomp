// Presentation texture filtering: the option parsing the two GL backends
// share for M2_TEXTURE_FILTER / M2_BITMAP_FILTER / M2_HIRES_EFFECTS.
// Expected mappings come from the launcher contracts, not from running the
// implementation.
#include "texture_scaling.h"

#include <cstdio>
#include <cstdlib>

using namespace m2host;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void test_requests() {
    set_env("M2_TEXTURE_FILTER", "");
    expect(texture_filter_request() == TextureFilterMode::guest, "filter unset");
    set_env("M2_TEXTURE_FILTER", "linear");
    expect(texture_filter_request() == TextureFilterMode::linear, "filter linear");
    set_env("M2_TEXTURE_FILTER", "sharp");
    expect(texture_filter_request() == TextureFilterMode::sharp, "filter sharp");
    set_env("M2_TEXTURE_FILTER", "bogus");
    expect(texture_filter_request() == TextureFilterMode::guest, "filter bogus");
    set_env("M2_BITMAP_FILTER", "sharp");
    expect(bitmap_filter_request() == TextureFilterMode::sharp, "bitmap sharp");
    set_env("M2_BITMAP_FILTER", "nearest");
    expect(bitmap_filter_request() == TextureFilterMode::guest, "bitmap nearest");
    set_env("M2_HIRES_EFFECTS", "");
    expect(!hires_effects_requested(), "hires effects unset");
    set_env("M2_HIRES_EFFECTS", "0");
    expect(!hires_effects_requested(), "hires effects 0");
    set_env("M2_HIRES_EFFECTS", "1");
    expect(hires_effects_requested(), "hires effects 1");
}
}  // namespace

int main() {
    test_requests();
    if (failures == 0) std::puts("texture_scaling_test: ok");
    return failures == 0 ? 0 : 1;
}
