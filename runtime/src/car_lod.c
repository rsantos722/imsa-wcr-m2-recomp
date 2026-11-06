// car_lod.c — opt-in highest-detail car models.
//
// M_Support.c::MS_ScanList (guest 0x21a68) selects a car LOD band from two
// XZ-distance thresholds (`0x21e54..0x21e6c`) and passes that transient index
// to MS_addcar (0x23d70) at 0x21e94. MS_addgroup (0x23888) clamps the index to
// the model's authored group count, where band 0 is the highest-detail model.
//
// When M2_DISABLE_CAR_LOD is exactly "1", keep the transient index at 0 so
// opponents retain their full model at every distance. Any other value keeps
// the guest's bands. Only the argument to MS_addcar is overridden: transforms,
// culling, shadows, simulation and race rules are unchanged.

#include "recomp_helpers.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_disable_car_lod = ATOMIC_VAR_INIT(-1);

static int car_lod_disabled_cached(void) {
    int disabled = atomic_load_explicit(&g_disable_car_lod,
                                        memory_order_acquire);
    if (disabled >= 0)
        return disabled;

    const char *value = getenv("M2_DISABLE_CAR_LOD");
    const int detected = value != NULL && strcmp(value, "1") == 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(
            &g_disable_car_lod, &expected, detected,
            memory_order_release, memory_order_acquire))
        return expected;
    return detected;
}

uint32_t m2_car_lod_level(uint32_t guest_level) {
    return car_lod_disabled_cached() ? 0u : guest_level;
}

void m2_car_lod_state_reset(void) {
    atomic_store_explicit(&g_disable_car_lod, -1, memory_order_release);
}
