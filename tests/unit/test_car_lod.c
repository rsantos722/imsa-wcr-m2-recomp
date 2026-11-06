// Opt-in highest-detail car models: the runtime override used by MS_ScanList.
#include "recomp_helpers.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

int main(void) {
    // Unset preserves every guest-selected LOD band.
    set_env("M2_DISABLE_CAR_LOD", NULL);
    m2_car_lod_state_reset();
    CHECK(m2_car_lod_level(0u) == 0u);
    CHECK(m2_car_lod_level(1u) == 1u);
    CHECK(m2_car_lod_level(2u) == 2u);

    // The option has an exact-1 contract; every other value keeps the bands.
    set_env("M2_DISABLE_CAR_LOD", "0");
    m2_car_lod_state_reset();
    CHECK(m2_car_lod_level(1u) == 1u);
    CHECK(m2_car_lod_level(2u) == 2u);

    set_env("M2_DISABLE_CAR_LOD", "2");
    m2_car_lod_state_reset();
    CHECK(m2_car_lod_level(1u) == 1u);
    CHECK(m2_car_lod_level(2u) == 2u);

    // Exact "1" keeps the transient band at the highest-detail model.
    set_env("M2_DISABLE_CAR_LOD", "1");
    m2_car_lod_state_reset();
    CHECK(m2_car_lod_level(0u) == 0u);
    CHECK(m2_car_lod_level(1u) == 0u);
    CHECK(m2_car_lod_level(2u) == 0u);

    return failures ? 1 : 0;
}
