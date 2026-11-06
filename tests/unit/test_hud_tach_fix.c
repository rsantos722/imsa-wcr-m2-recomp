// Exterior HUD tachometer range fix: rescale the gauge from the player
// engine's redline while preserving the shipped Ferrari mapping.
#include "recomp_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The no-disc runtime omits MMIO and memory-event delivery.
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t a) { (void)a; return 0; }
__attribute__((weak)) void m2_oob_shadow_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t a, uint32_t n, uint32_t pc) { (void)a; (void)n; (void)pc; }
__attribute__((weak)) void m2_guest_event_c(const M2GuestEvent* e) { (void)e; }

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
    enum { ENGINE_EA = 0x182408 };
    const float reference = 0.015f * 11500.0f;
    PPCContext c;
    memset(&c, 0, sizeof(c));
    c.mem = (uint8_t*)calloc(1, ENGINE_EA + 4u);
    if (!c.mem) {
        fprintf(stderr, "FAIL: allocation\n");
        return 1;
    }

    // Default on: the 8500 RPM engines are rescaled so their redline lands
    // where the 11500 RPM Ferrari redline already did.
    set_env("M2_HUD_TACH_FIX", NULL);
    m2_hud_tach_fix_state_reset();
    wrf32(&c, ENGINE_EA, 8500.0f);
    c.fpr[1] = 0.015f;
    m2_hud_tach_scale_step(&c);
    CHECK(fabsf(c.fpr[1] - reference / 8500.0f) < 1.0e-6f);
    CHECK(m2_hud_tach_fix_enabled() == 1);

    // The Ferrari reference scale is unchanged.
    wrf32(&c, ENGINE_EA, 11500.0f);
    c.fpr[1] = 0.015f;
    m2_hud_tach_scale_step(&c);
    CHECK(fabsf(c.fpr[1] - 0.015f) < 1.0e-6f);

    // =0 keeps the original fixed scale.
    set_env("M2_HUD_TACH_FIX", "0");
    m2_hud_tach_fix_state_reset();
    wrf32(&c, ENGINE_EA, 8500.0f);
    c.fpr[1] = 0.015f;
    m2_hud_tach_scale_step(&c);
    CHECK(c.fpr[1] == 0.015f);
    CHECK(m2_hud_tach_fix_enabled() == 0);

    // Any other value enables it again.
    set_env("M2_HUD_TACH_FIX", "1");
    m2_hud_tach_fix_state_reset();
    c.fpr[1] = 0.015f;
    m2_hud_tach_scale_step(&c);
    CHECK(fabsf(c.fpr[1] - reference / 8500.0f) < 1.0e-6f);

    // An uninitialized or invalid engine redline leaves the caller's scale.
    wrf32(&c, ENGINE_EA, 0.0f);
    c.fpr[1] = 0.015f;
    m2_hud_tach_scale_step(&c);
    CHECK(c.fpr[1] == 0.015f);

    free(c.mem);
    return failures ? 1 : 0;
}
