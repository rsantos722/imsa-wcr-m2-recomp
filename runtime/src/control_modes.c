// Launcher-selected input enhancements. Original guest tables and simulation
// remain intact. Only the player racing input records and ProcessInput's
// player-channel selection are adapted.
#include "control_modes.h"
#include "ppc_ctx.h"
#include "m2_cport_bits.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

void FN_Cpad_TranslateInput__0fe8c(PPCContext *c);
void FN_Joy_TranslateInput__1e438(PPCContext *c);
void FN_adjustSteering__09600(PPCContext *c);

enum {
    PLAYER_INPUT = 0x1812b8u, TRACK_INPUT = 0x181bd0u,
    PLAYER_OUTPUT = 0x17c7c4u, LAYOUT_PREF = 0x17d91du,
    PAD1 = 0x17c5f8u, PAD2 = 0x17c664u,
    WHEEL = 0x17c6a0u, STICK_PAD = 0x17c6dcu, JOY = 0x17c634u,
    // Layout descriptors carry a device-kind word past their translation
    // table. ProcessInput's mode-0 block uses it to choose the wheel/stick
    // nibble protocol instead of the digital-pad steering ramp.
    LAYOUT_KIND_OFFSET = 0x38u,
    LAYOUT_KIND_WHEEL = 0x1000000u, LAYOUT_KIND_STICK = 0x2000000u,
};
static _Atomic int s_mode;
static pthread_mutex_t s_input_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    float analog_steering, digital_steering, throttle, brake;
} s_input;
// Guest-space digital steering ramp state, left positive. The guest integrates
// it in ProcessInput's mode-0 block; it is kept separate so the analog command
// can take priority without the two inputs fighting.
static float s_digital_steer;
// Shaped analog travel below this is treated as centred. The host's per-binding
// deadzone normally makes it exactly zero; the epsilon only rejects float noise.
static const float kAnalogPriorityEpsilon = 0.001f;
// True when ProcessInput's digital steering block ran this iteration.
static int s_digital_ran;
// True while the current ProcessInput call is the player's record. r30 is
// reused for the translated word before the epilogue, so the player identity
// must be captured where the argument is still live.
static int s_custom_player;
static int s_improved_player, s_analog_protocol;
static uint32_t s_process_car;

static float bounded(float value, float lo, float hi) {
    return !isfinite(value) ? 0.f : fmaxf(lo, fminf(hi, value));
}
void m2_controls_publish(float analog_steering, float digital_steering,
                         float throttle, float brake) {
    pthread_mutex_lock(&s_input_lock);
    s_input.analog_steering = bounded(analog_steering, -1.f, 1.f);
    s_input.digital_steering = bounded(digital_steering, -1.f, 1.f);
    s_input.throttle = bounded(throttle, 0.f, 1.f);
    s_input.brake = bounded(brake, 0.f, 1.f);
    pthread_mutex_unlock(&s_input_lock);
}
void m2_controls_set_mode(m2_controls_mode mode) {
    atomic_store(&s_mode, mode >= M2_CONTROLS_ORIGINAL && mode <= M2_CONTROLS_CUSTOM
                            ? (int)mode : M2_CONTROLS_ORIGINAL);
    s_digital_steer = 0.f;
    s_digital_ran = 0;
    s_custom_player = 0;
    s_improved_player = s_analog_protocol = 0;
    s_process_car = 0;
    m2_analog_steering_reset();
    m2_controls_publish(0.f, 0.f, 0.f, 0.f);
}
static int racing_record(PPCContext *c, uint32_t input) {
    if (input != PLAYER_INPUT && input != TRACK_INPUT) return 0;
    const uint32_t table = rd32(c, input + 0x20u);
    return table == PAD1 || table == PAD2 || table == WHEEL || table == STICK_PAD;
}
static uint32_t pad_to_stick(uint32_t raw) {
    uint32_t result = 0;
    const uint32_t pad[] = {M2_CPORT_X, M2_CPORT_A, M2_CPORT_B, M2_CPORT_C,
        M2_CPORT_UP, M2_CPORT_DOWN, M2_CPORT_RIGHT, M2_CPORT_LEFT,
        M2_CPORT_START, M2_CPORT_L, M2_CPORT_R};
    const uint32_t stick[] = {M2_STICK_FIRE, M2_STICK_A, M2_STICK_B, M2_STICK_C,
        M2_STICK_UP, M2_STICK_DOWN, M2_STICK_RIGHT, M2_STICK_LEFT,
        M2_STICK_PLAY, M2_STICK_LEFTSHIFT, M2_STICK_RIGHTSHIFT};
    for (unsigned i = 0; i < sizeof(pad) / sizeof(pad[0]); ++i)
        if (raw & pad[i]) result |= stick[i];
    return result;
}
static uint32_t custom_actions(uint32_t raw, int race, int in_race) {
    uint32_t result = 0;
    if (race) {
        // Guest action bits: HUD displays, pause, view, and gear edges that
        // carry the original clutch flag (0x41/0x42). L/R use the shipped
        // down/up shoulder mapping so Custom matches the original tables.
        if (raw & M2_CPORT_C) result |= 0x8u;
        if (raw & M2_CPORT_START) result |= 0x10u;
        if (raw & M2_CPORT_D) result |= 0x4u;
        if (raw & M2_CPORT_L) result |= 0x42u;
        if (raw & M2_CPORT_R) result |= 0x41u;
        if (raw & M2_CPORT_F) result |= 0x10000u;
    } else {
        if (raw & M2_CPORT_A) result |= 0x10u;
        if (raw & M2_CPORT_START) result |= in_race ? 0x20u : 0x10u;
        if (raw & M2_CPORT_B) result |= 0x20u;
        if (raw & M2_CPORT_UP) result |= 0x1u;
        if (raw & M2_CPORT_DOWN) result |= 0x2u;
        if (raw & M2_CPORT_LEFT) result |= 0x4u;
        if (raw & M2_CPORT_RIGHT) result |= 0x8u;
    }
    return result;
}
int m2_controls_translate(PPCContext *c) {
    const int mode = atomic_load(&s_mode);
    if (mode == M2_CONTROLS_ORIGINAL || !c || !c->mem) return 0;
    const uint32_t input = c->gpr[3];
    if (input > RECOMP_GUEST_MEM - 0x2cu) return 0;
    const int race = racing_record(c, input);
    if (!race && (mode != M2_CONTROLS_CUSTOM ||
                  rd32(c, input + 0x20u) != M2_CPORT_TABLE_CPAD)) return 0;
    if (mode == M2_CONTROLS_CUSTOM) {
        pthread_mutex_lock(&s_input_lock);
        const float digital = s_input.digital_steering;
        pthread_mutex_unlock(&s_input_lock);
        const int in_race = rd32(c, 0x1877d8u) == 3u;
        if (input == PLAYER_INPUT && !in_race) s_digital_steer = 0.f;
        for (unsigned i = 0; i < 3; ++i)
            wr32(c, input + 0x14u + 4u * i,
                 custom_actions(rd32(c, input + 4u * i), race, in_race));
        if (race && input == PLAYER_INPUT) {
            // The mode-0 block reads its steering direction from translated
            // word 0. Digital bindings feed that ramp; analog bindings never
            // synthesize it, so a stick deflection alone cannot press it.
            uint32_t word0 = rd32(c, input + 0x14u);
            if (digital > 0.5f) word0 |= 0x2000u;       // right
            else if (digital < -0.5f) word0 |= 0x1000u; // left
            wr32(c, input + 0x14u, word0);
        }
    } else {
        const unsigned layout = rd8(c, LAYOUT_PREF);
        if (layout > 2u) return 0; // Preserve an invalid original preference.
        // The disc diagrams are wheel / digital pad / analog stick. Keep the
        // shipped table data; adapt only the player's transient input record.
        const uint32_t table = layout == 0u ? WHEEL : layout == 1u ? PAD2 : JOY;
        wr32(c, input + 0x20u, layout == 0u ? WHEEL : PAD2);
        wr8(c, input + 0x29u, layout == 2u ? 7u : 0u);
        for (unsigned i = 0; i < 3; ++i) {
            PPCContext work = *c;
            const uint32_t raw = rd32(c, input + 4u * i);
            work.gpr[3] = layout == 2u ? pad_to_stick(raw) : raw;
            work.gpr[4] = table;
            if (layout == 2u) FN_Joy_TranslateInput__1e438(&work);
            else FN_Cpad_TranslateInput__0fe8c(&work);
            wr32(c, input + 0x14u + 4u * i, work.gpr[3]);
        }
        if (layout != 1u) {
            pthread_mutex_lock(&s_input_lock);
            const float steer = s_input.analog_steering, throttle = s_input.throttle, brake = s_input.brake;
            pthread_mutex_unlock(&s_input_lock);
            // Original input protocols: 128-centered X; wheel pedal nibbles;
            // stick's single signed Y. Independent pedals belong to Custom.
            // ProcessInput stores the high nibble / positive Y split into
            // [out+0x10] (brake) and the low nibble / negative side into
            // [out+0xc] (throttle), so brake is the high/sign-positive side.
            wr8(c, input + 0x2au, (uint8_t)lroundf(128.f + steer * 127.f));
            wr8(c, input + 0x2bu, layout == 0u
                ? (uint8_t)(((unsigned)lroundf(brake * 15.f) << 4) | (unsigned)lroundf(throttle * 15.f))
                : (uint8_t)lroundf(128.f + (brake - throttle) * 127.f));
        }
    }
    c->gpr[3] = 0; // Ctrl_TranslateInput's original return value.
    return 1;
}
// ProcessInput selects its decoder from the guest's layout and mode fields.
// Custom forces the shipped digital-pad layout so its own mode-0 steering ramp
// runs; the analog command is arbitrated after that block.
void m2_controls_process_begin(PPCContext *c) {
    s_process_car = 0;
    if (!m2_analog_steering_enabled() || !c || !c->mem) return;
    if (c->gpr[3] != PLAYER_INPUT || c->gpr[4] != PLAYER_OUTPUT) return;
    s_process_car = c->gpr[6];
    m2_analog_steering_select_car(c, s_process_car);
}
void m2_controls_custom_prepare(PPCContext *c) {
    s_custom_player = 0;
    s_improved_player = s_analog_protocol = 0;
    s_digital_ran = 0;
    if (!c || !c->mem) return;
    if (c->gpr[30] != PLAYER_INPUT || c->gpr[31] != PLAYER_OUTPUT) return;
    s_custom_player = atomic_load(&s_mode) == M2_CONTROLS_CUSTOM;
    s_improved_player = m2_analog_steering_enabled() && s_process_car != 0;
    if (!s_custom_player && !s_improved_player) return;
    if (s_custom_player) {
        wr32(c, PLAYER_INPUT + 0x20u, PAD2);
        wr8(c, PLAYER_INPUT + 0x29u, 0u);
    }
    const uint32_t table = rd32(c, PLAYER_INPUT + 0x20u);
    const uint32_t kind = table <= RECOMP_GUEST_MEM - 0x40u
                              ? rd32(c, table + LAYOUT_KIND_OFFSET) : 0u;
    s_analog_protocol = rd8(c, PLAYER_INPUT + 0x29u) == 7u ||
        kind == LAYOUT_KIND_WHEEL || kind == LAYOUT_KIND_STICK;
}
void m2_controls_custom_restore(PPCContext *c) {
    if ((!s_custom_player && !s_improved_player) || !c || !c->mem) return;
    const uint32_t table = rd32(c, PLAYER_INPUT + 0x20u);
    const uint32_t kind = table <= RECOMP_GUEST_MEM - 0x40u
                              ? rd32(c, table + LAYOUT_KIND_OFFSET) : 0u;
    s_digital_ran = kind != LAYOUT_KIND_WHEEL && kind != LAYOUT_KIND_STICK;
    if (s_digital_ran) wrf32(c, PLAYER_OUTPUT + 8u, s_digital_steer);
}
void m2_controls_custom_finish(PPCContext *c) {
    if ((!s_custom_player && !s_improved_player) || !c || !c->mem) return;
    const int custom = s_custom_player, improved = s_improved_player;
    s_custom_player = 0;
    s_improved_player = 0;
    if (s_digital_ran) {
        s_digital_steer = bounded(rdf32(c, PLAYER_OUTPUT + 8u), -1.f, 1.f);
        s_digital_ran = 0;
    } else {
        // The nibble/mode-7 paths did not update the ramp state this frame.
        s_digital_steer = 0.f;
    }
    pthread_mutex_lock(&s_input_lock);
    const float analog = s_input.analog_steering, throttle = s_input.throttle, brake = s_input.brake;
    pthread_mutex_unlock(&s_input_lock);
    if (improved) {
        if (fabsf(analog) > kAnalogPriorityEpsilon || (!custom && s_analog_protocol) || s_digital_steer == 0.f) {
            // Bypass Arcade-only adjustSteering; do_controls consumes this
            // same physical-axis command in Simulation as well.
            const float steering = m2_analog_steering_command(c, -analog);
            wrf32(c, PLAYER_OUTPUT + 8u, bounded(steering, -1.f, 1.f));
        } else {
            // A live native digital ramp takes over immediately. Otherwise
            // a centered axis uses the analog controller's short return tail.
            m2_analog_steering_release();
            wrf32(c, PLAYER_OUTPUT + 8u, s_digital_steer);
        }
        if (custom) {
            wrf32(c, PLAYER_OUTPUT + 0xcu, throttle);
            wrf32(c, PLAYER_OUTPUT + 0x10u, brake);
        }
        return;
    }
    if (!custom) return;
    // ProcessInput's sign is left-positive. Retain its original speed-based
    // adjustSteering function for the analog contribution. While the analog
    // channel is deflected, it controls steering alone: the guest's digital
    // ramp keeps running in the background, so a held D-pad stays native and
    // takes over again as soon as the stick centres. do_controls and all
    // driving aids remain guest code.
    PPCContext work = *c;
    work.gpr[3] = c->gpr[28];
    work.fpr[1] = -analog;
    FN_adjustSteering__09600(&work);
    const float steering = fabsf(analog) > kAnalogPriorityEpsilon
                               ? work.fpr[1]
                               : s_digital_steer + work.fpr[1];
    wrf32(c, PLAYER_OUTPUT + 8u, bounded(steering, -1.f, 1.f));
    wrf32(c, PLAYER_OUTPUT + 0xcu, throttle);
    wrf32(c, PLAYER_OUTPUT + 0x10u, brake);
}
