#pragma once
#include "analog_steering.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    M2_CONTROLS_ORIGINAL = 0,
    M2_CONTROLS_PATCHED = 1,
    M2_CONTROLS_CUSTOM = 2
} m2_controls_mode;

typedef struct PPCContext PPCContext;
// Set before guest boot. Replay/diagnostic callers retain Original mode.
void m2_controls_set_mode(m2_controls_mode mode);
// Analog and digital steering are separate Custom channels: digital bindings
// drive the guest's own ramped digital-pad path, analog bindings the absolute
// analog path. Each value is -1..+1, right positive. The analog channel has
// priority while it is deflected.
void m2_controls_publish(float analog_steering, float digital_steering,
                         float throttle, float brake);
// Translator-owned opt-in hooks.
int m2_controls_translate(PPCContext *c);
void m2_controls_process_begin(PPCContext *c);
// ProcessInput hooks: capture the player's car, select Custom's digital-pad
// protocol, retain the native digital ramp, and arbitrate analog steering.
// Improved analog steering is independent of the profile; pedals are Custom.
void m2_controls_custom_prepare(PPCContext *c);
void m2_controls_custom_restore(PPCContext *c);
void m2_controls_custom_finish(PPCContext *c);

#ifdef __cplusplus
}
#endif
