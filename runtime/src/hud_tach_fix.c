// hud_tach_fix.c — default-on exterior HUD tachometer range fix.
//
// game.c::DrawInfoBar (guest 0x1608c) draws the exterior-camera HUD. Its
// caller passes the tachometer scale in f1 as 0.015, i.e. 180 gauge units over
// 12000 RPM. That happens to put the Ferrari V12's 11500 RPM redline at 172.5,
// inside the gauge's yellow zone, but the other three engines redline at
// 8500 RPM, so their bar stops at 127.5 and never reaches the zone.
//
// The player engine structure (guest symbol plyrEng) stores the redline in its
// first float. Rescale the gauge so every engine's redline lands where the
// Ferrari redline does, preserving the original gauge face and the Ferrari
// mapping. M2_HUD_TACH_FIX=0 keeps the original fixed scale.
//
// This is presentation-only: the guest simulation, shift points, audio and
// race rules are untouched.

#include "recomp_helpers.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define M2_HUD_TACH_ENGINE_EA 0x182408u  // plyrEng
#define M2_HUD_TACH_ENGINE_REDLINE_OFF 0x00u
#define M2_HUD_TACH_REFERENCE_REDLINE 11500.0f  // Ferrari V12, the shipped tuning

static atomic_int g_hud_tach_fix = ATOMIC_VAR_INIT(-1);

static int hud_tach_fix_enabled_cached(void) {
    int enabled = atomic_load_explicit(&g_hud_tach_fix, memory_order_acquire);
    if (enabled >= 0)
        return enabled;

    const char *value = getenv("M2_HUD_TACH_FIX");
    const int detected = value == NULL || strcmp(value, "0") != 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(
            &g_hud_tach_fix, &expected, detected,
            memory_order_release, memory_order_acquire))
        return expected;
    return detected;
}

int m2_hud_tach_fix_enabled(void) {
    return hud_tach_fix_enabled_cached();
}

void m2_hud_tach_scale_step(PPCContext *c) {
    if (!hud_tach_fix_enabled_cached() || !c || !c->mem)
        return;

    const float redline = rdf32(c, M2_HUD_TACH_ENGINE_EA + M2_HUD_TACH_ENGINE_REDLINE_OFF);
    if (!(redline > 0.0f))
        return;

    // Preserve whatever scale the caller supplied and adjust it by the same
    // ratio the shipped Ferrari tuning implies. For the Ferrari this is a
    // multiply by 1.0, so its gauge is bit-for-bit unchanged.
    c->fpr[1] *= M2_HUD_TACH_REFERENCE_REDLINE / redline;
}

void m2_hud_tach_fix_state_reset(void) {
    atomic_store_explicit(&g_hud_tach_fix, -1, memory_order_release);
}
