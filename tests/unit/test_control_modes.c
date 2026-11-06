// Custom control adapter: digital-pad steering ramp state and analog priority.
//
// ProcessInput's mode-0 block is guest code. These checks exercise the host
// adapter around it: protocol selection, digital direction bits, and the
// analog channel taking priority over the guest ramp while deflected.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control_modes.h"
#include "m2_cport_bits.h"
#include "ppc_ctx.h"

// ppc_ctx.h's accessors reference runtime MMIO/trace hooks. The adapter is
// linked from m2_runtime, which leaves recomp_runtime.c out of the no-game
// build, so supply inert definitions.
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t a) { (void)a; return 0; }
__attribute__((weak)) void m2_oob_shadow_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc) {
    (void)ea; (void)size; (void)pc;
}
__attribute__((weak)) void m2_guest_event_c(const M2GuestEvent* event) { (void)event; }

// Guest ABI addresses used by the adapter (runtime/src/control_modes.c).
enum {
    PLAYER_INPUT = 0x1812b8u,
    PLAYER_OUTPUT = 0x17c7c4u,
    STATE_PRIMARY = 0x1877d8u,
    LAYOUT_PREF = 0x17d91du,
    PAD1 = 0x17c5f8u,
    PAD2 = 0x17c664u,
    WHEEL = 0x17c6a0u,
};

static int g_failures;
static float g_adjust_scale = 1.f;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// The real guest functions are supplied by generated code. The adapter only
// needs the steering scale here; ProcessInput's digital block is simulated by
// the test between restore and finish.
void FN_adjustSteering__09600(PPCContext* c) { c->fpr[1] *= g_adjust_scale; }
void FN_Cpad_TranslateInput__0fe8c(PPCContext* c) { (void)c; }
void FN_Joy_TranslateInput__1e438(PPCContext* c) { (void)c; }

static void reset_context(PPCContext* c) {
    memset(c, 0, sizeof(*c));
    c->mem = (unsigned char*)calloc(1, RECOMP_GUEST_MEM);
    if (!c->mem) {
        printf("FAIL cannot allocate guest memory\n");
        ++g_failures;
        return;
    }
    wr32(c, PLAYER_INPUT + 0x00u, 0u);
    wr32(c, PLAYER_INPUT + 0x04u, 0u);
    wr32(c, PLAYER_INPUT + 0x08u, 0u);
    wr32(c, PLAYER_INPUT + 0x20u, PAD1);
    wr8(c, PLAYER_INPUT + 0x29u, 7u);
    wr32(c, STATE_PRIMARY, 3u);
    wrf32(c, PLAYER_OUTPUT + 0x08u, 0.f);
    wrf32(c, PLAYER_OUTPUT + 0x0cu, 0.f);
    wrf32(c, PLAYER_OUTPUT + 0x10u, 0.f);
    c->gpr[3] = PLAYER_INPUT;
    c->gpr[28] = 0x180000u;  // view pointer; only g_adjust_scale is consulted
    c->gpr[30] = PLAYER_INPUT;
    c->gpr[31] = PLAYER_OUTPUT;
}

static void free_context(PPCContext* c) {
    free(c->mem);
    c->mem = NULL;
}

static void test_translate_digital_direction(void) {
    printf("custom translate steering bits\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_CUSTOM);

    m2_controls_publish(0.f, 1.f, 0.f, 0.f);
    c.gpr[3] = PLAYER_INPUT;
    CHECK(m2_controls_translate(&c) == 1);
    CHECK((rd32(&c, PLAYER_INPUT + 0x14u) & 0x2000u) != 0u);
    CHECK((rd32(&c, PLAYER_INPUT + 0x14u) & 0x1000u) == 0u);

    m2_controls_publish(0.f, -1.f, 0.f, 0.f);
    c.gpr[3] = PLAYER_INPUT;
    CHECK(m2_controls_translate(&c) == 1);
    CHECK((rd32(&c, PLAYER_INPUT + 0x14u) & 0x1000u) != 0u);
    CHECK((rd32(&c, PLAYER_INPUT + 0x14u) & 0x2000u) == 0u);

    // A held axis must not synthesize the digital direction bits.
    m2_controls_publish(1.f, 0.f, 0.f, 0.f);
    c.gpr[3] = PLAYER_INPUT;
    CHECK(m2_controls_translate(&c) == 1);
    CHECK((rd32(&c, PLAYER_INPUT + 0x14u) & 0x3000u) == 0u);
    free_context(&c);
}

static void test_translate_patched_pedals(void) {
    printf("patched pedal byte orientation\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_PATCHED);
    m2_controls_publish(0.f, 0.f, 0.6f, 0.2f);

    // Wheel: high nibble is brake, low nibble is throttle.
    wr8(&c, LAYOUT_PREF, 0u);
    c.gpr[3] = PLAYER_INPUT;
    CHECK(m2_controls_translate(&c) == 1);
    CHECK(rd8(&c, PLAYER_INPUT + 0x2bu) == ((3u << 4) | 9u));

    // Analog stick: positive signed Y is brake, negative is throttle.
    wr8(&c, LAYOUT_PREF, 2u);
    c.gpr[3] = PLAYER_INPUT;
    CHECK(m2_controls_translate(&c) == 1);
    CHECK(rd8(&c, PLAYER_INPUT + 0x2bu) == 77u);

    free_context(&c);
}

static void test_prepare_forces_digital_pad(void) {
    printf("custom prepare selects the digital-pad protocol\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_CUSTOM);
    wr32(&c, PLAYER_INPUT + 0x20u, WHEEL);
    wr8(&c, PLAYER_INPUT + 0x29u, 7u);
    m2_controls_custom_prepare(&c);
    CHECK(rd32(&c, PLAYER_INPUT + 0x20u) == PAD2);
    CHECK(rd8(&c, PLAYER_INPUT + 0x29u) == 0u);

    // Original mode leaves the guest's selection alone.
    m2_controls_set_mode(M2_CONTROLS_ORIGINAL);
    wr32(&c, PLAYER_INPUT + 0x20u, WHEEL);
    wr8(&c, PLAYER_INPUT + 0x29u, 7u);
    m2_controls_custom_prepare(&c);
    CHECK(rd32(&c, PLAYER_INPUT + 0x20u) == WHEEL);
    CHECK(rd8(&c, PLAYER_INPUT + 0x29u) == 7u);
    free_context(&c);
}

static void test_analog_priority(void) {
    printf("custom analog priority\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_CUSTOM);
    m2_controls_publish(0.5f, 0.f, 0.75f, 0.25f);

    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    CHECK(rdf32(&c, PLAYER_OUTPUT + 0x08u) == 0.f);
    // The guest's mode-0 block would integrate this frame's ramp here, and it
    // reuses r30 for the translated word before the epilogue.
    wrf32(&c, PLAYER_OUTPUT + 0x08u, 0.25f);
    c.gpr[30] = 0x2000u;
    m2_controls_custom_finish(&c);
    // The stick is deflected, so its command controls steering alone. A held
    // D-pad would otherwise add 0.25 here.
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - (-0.5f)) < 1e-6f);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x0cu) - 0.75f) < 1e-6f);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x10u) - 0.25f) < 1e-6f);

    // The background ramp still saved the guest's value.
    c.gpr[30] = PLAYER_INPUT;
    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    CHECK(rdf32(&c, PLAYER_OUTPUT + 0x08u) == 0.25f);
    // The D-pad is still held, so the guest advances the hidden ramp while the
    // stick remains in control.
    wrf32(&c, PLAYER_OUTPUT + 0x08u, 0.4f);
    m2_controls_custom_finish(&c);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - (-0.5f)) < 1e-6f);

    // Stick centres: the digital ramp takes over immediately at its native
    // hidden state.
    m2_controls_publish(0.f, 0.f, 0.f, 0.f);
    c.gpr[30] = PLAYER_INPUT;
    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    CHECK(rdf32(&c, PLAYER_OUTPUT + 0x08u) == 0.4f);
    // No digital input: the guest decays the ramp by 0.1 and the output follows.
    wrf32(&c, PLAYER_OUTPUT + 0x08u, 0.3f);
    m2_controls_custom_finish(&c);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - 0.3f) < 1e-6f);

    // Opposing directional inputs: the stick still wins while deflected.
    m2_controls_publish(0.5f, 0.f, 0.f, 0.f);
    c.gpr[30] = PLAYER_INPUT;
    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    wrf32(&c, PLAYER_OUTPUT + 0x08u, 0.7f);
    m2_controls_custom_finish(&c);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - (-0.5f)) < 1e-6f);
    free_context(&c);
}

static void test_non_digital_path_resets_ramp(void) {
    printf("custom nibble/mode-7 frames reset the ramp state\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_CUSTOM);
    m2_controls_publish(0.5f, 0.f, 0.f, 0.f);
    // A mode-7 frame skips the 0x986c restore but still reaches the finish hook.
    m2_controls_custom_prepare(&c);
    wrf32(&c, PLAYER_OUTPUT + 0x08u, -0.5f);
    m2_controls_custom_finish(&c);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - (-0.5f)) < 1e-6f);
    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    CHECK(rdf32(&c, PLAYER_OUTPUT + 0x08u) == 0.f);
    free_context(&c);
}

static void test_speed_scale(void) {
    printf("custom analog contribution uses guest adjustSteering\n");
    PPCContext c;
    reset_context(&c);
    m2_controls_set_mode(M2_CONTROLS_CUSTOM);
    g_adjust_scale = 0.25f;
    m2_controls_publish(-1.f, 0.f, 0.f, 0.f);
    m2_controls_custom_prepare(&c);
    m2_controls_custom_restore(&c);
    m2_controls_custom_finish(&c);
    CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT + 0x08u) - 0.25f) < 1e-6f);
    g_adjust_scale = 1.f;
    free_context(&c);
}

static void improved_begin(PPCContext *c) {
    c->gpr[3] = PLAYER_INPUT; c->gpr[4] = PLAYER_OUTPUT; c->gpr[6] = 0x220000;
    m2_controls_process_begin(c);
    c->gpr[30] = PLAYER_INPUT; c->gpr[31] = PLAYER_OUTPUT;
    m2_controls_custom_prepare(c);
}
static void test_improved_profiles(void) {
    PPCContext c;
    reset_context(&c);
    wr32(&c, 0x220000, 0x222000); // body
    wr32(&c, 0x220428, 0x223000); // steering configuration
    wrf32(&c, 0x222008, 150.f);
    wrf32(&c, 0x2221a4, 1.f/60.f);
    wrf32(&c, 0x223098, .8f);
    wrf32(&c, 0x223030, 5.f); wrf32(&c, 0x223054, -5.f);
    for (unsigned i=0; i<5; ++i) wrf32(&c, 0x22309c+4*i, 1.f);
    float first = 0.f;
    for (int mode=M2_CONTROLS_ORIGINAL; mode<=M2_CONTROLS_CUSTOM; ++mode)
    for (int arcade=0; arcade<2; ++arcade) {
        m2_controls_set_mode((m2_controls_mode)mode);
        m2_analog_steering_set_enabled(1);
        g_adjust_scale = arcade ? .25f : 1.f;
        wr32(&c, PLAYER_INPUT+0x20, PAD2);
        wr32(&c, PAD2+0x38, 0);
        wr8(&c, PLAYER_INPUT+0x29, 0);
        wrf32(&c, PLAYER_OUTPUT+0x0c, .2f); wrf32(&c, PLAYER_OUTPUT+0x10, .8f);
        m2_controls_publish(.75f, 0.f, .6f, .4f);
        improved_begin(&c);
        m2_controls_custom_restore(&c);
        wrf32(&c, PLAYER_OUTPUT+8, .3f); // original digital ramp progressed
        m2_controls_custom_finish(&c);
        const float result = rdf32(&c, PLAYER_OUTPUT+8);
        CHECK(result < 0.f && result > -.75f);
        if (!mode && !arcade) first = result;
        CHECK(fabsf(result-first) < 1e-6f); // identical in Arcade and Simulation
        CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT+0x0c) - (mode==M2_CONTROLS_CUSTOM ? .6f : .2f)) < 1e-6f);
        CHECK(fabsf(rdf32(&c, PLAYER_OUTPUT+0x10) - (mode==M2_CONTROLS_CUSTOM ? .4f : .8f)) < 1e-6f);
        m2_controls_publish(0.f, 0.f, 0.f, 0.f);
        improved_begin(&c);
        m2_controls_custom_restore(&c);
        CHECK(rdf32(&c, PLAYER_OUTPUT+8) == .3f);
        wrf32(&c, PLAYER_OUTPUT+8, .2f);
        m2_controls_custom_finish(&c);
        CHECK(rdf32(&c, PLAYER_OUTPUT+8) == .2f); // digital unchanged
        if (mode != M2_CONTROLS_CUSTOM) {
            wr8(&c, PLAYER_INPUT+0x29, 7);
            improved_begin(&c); // native analog decoder's neutral must center
            wrf32(&c, PLAYER_OUTPUT+8, .1f);
            m2_controls_custom_finish(&c);
            CHECK(rdf32(&c, PLAYER_OUTPUT+8) == 0.f);
        }
        for (unsigned protocol=0; protocol<2; ++protocol) {
            m2_analog_steering_set_enabled(1);
            wr8(&c, PLAYER_INPUT+0x29, protocol ? 7 : 0);
            m2_controls_publish(.75f, 0.f, 0.f, 0.f);
            for (unsigned frame=0; frame<6; ++frame) {
                improved_begin(&c);
                if (rd8(&c, PLAYER_INPUT+0x29)==0) m2_controls_custom_restore(&c);
                wrf32(&c, PLAYER_OUTPUT+8, 0.f); // neutral native digital ramp
                m2_controls_custom_finish(&c);
            }
            const float held=rdf32(&c, PLAYER_OUTPUT+8);
            m2_controls_publish(0.f, 0.f, 0.f, 0.f);
            improved_begin(&c);
            if (rd8(&c, PLAYER_INPUT+0x29)==0) m2_controls_custom_restore(&c);
            wrf32(&c, PLAYER_OUTPUT+8, 0.f);
            m2_controls_custom_finish(&c);
            const float returning=rdf32(&c, PLAYER_OUTPUT+8);
            CHECK(returning<0.f && returning>held); // release is not instant
            // A D-pad command interrupts that tail without blending or lag.
            wr8(&c, PLAYER_INPUT+0x29, 0);
            improved_begin(&c);
            m2_controls_custom_restore(&c);
            wrf32(&c, PLAYER_OUTPUT+8, .2f);
            m2_controls_custom_finish(&c);
            CHECK(rdf32(&c, PLAYER_OUTPUT+8)==.2f);
            improved_begin(&c);
            m2_controls_custom_restore(&c);
            wrf32(&c, PLAYER_OUTPUT+8, 0.f);
            m2_controls_custom_finish(&c);
            CHECK(rdf32(&c, PLAYER_OUTPUT+8)==0.f); // no old analog tail resumes
        }
    }
    m2_analog_steering_set_enabled(0);
    m2_controls_set_mode(M2_CONTROLS_ORIGINAL);
    m2_controls_publish(.75f, 0.f, 0.f, 0.f);
    improved_begin(&c);
    wrf32(&c, PLAYER_OUTPUT+8, -.75f);
    m2_controls_custom_finish(&c);
    CHECK(rdf32(&c, PLAYER_OUTPUT+8) == -.75f); // opt-out is untouched
    g_adjust_scale = 1.f;
    free_context(&c);
}

int main(void) {
    test_translate_digital_direction();
    test_translate_patched_pedals();
    test_prepare_forces_digital_pad();
    test_analog_priority();
    test_non_digital_path_resets_ramp();
    test_speed_scale();
    test_improved_profiles();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("control mode checks passed\n");
    return 0;
}
