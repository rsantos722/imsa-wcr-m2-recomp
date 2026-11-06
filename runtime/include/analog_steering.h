#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct PPCContext PPCContext;

// Read-only inputs from patchforce after its native coefficient interpolation.
// Units are the guest's; slip limits/steering angles are radians.
typedef struct {
    float load, half_patch, static_mu, sliding_mu, stiffness;
    float rolling_ratio, longitudinal_slip;
} m2_steering_tire;

// Pure native-force reduction used by the estimator and differential tests.
float m2_steering_lateral_force(const m2_steering_tire *tire, float slip_angle);

typedef enum {
    M2_SLIP_INVALID = 0,
    M2_SLIP_FINITE_PEAK,
    M2_SLIP_ASYMPTOTIC
} m2_steering_slip_kind;
typedef struct {
    m2_steering_slip_kind kind;
    float peak_slip;       // radians; NAN when the maximum is an asymptote
    float max_force;       // peak force or asymptotic lateral capacity
    float reference_slip;  // first fraction-of-maximum crossing, or NAN
    float force_at_bound;
} m2_steering_slip_analysis;

// Frozen-coefficient analysis for forward-rolling contacts (k<1), not used by
// the player controller. fraction is in (0,1]; bound is in (0,pi/2).
// A valid analysis can have no reference within
// bound, and 100% of an asymptote is never reported as a finite slip angle.
int m2_steering_analyze_slip(const m2_steering_tire *tire, float fraction,
                           float bound, m2_steering_slip_analysis *result);

typedef struct {
    float slip, force;
    int at_bound;
} m2_steering_cornering;
// Force perpendicular to the planar contact's travel direction. Unsupported
// locked/reversed rolling returns NAN / false, never a fabricated optimum.
float m2_steering_cornering_force(const m2_steering_tire *tire, float slip);
int m2_steering_cornering_optimum(const m2_steering_tire *tire, float bound,
                                m2_steering_cornering *result);

typedef struct {
    uint32_t car, contact_mask;
    uint64_t tick;
    float intent, target_angle, angle;
    float front_slip, front_force, rear_force, recovery_flow;
    int fallback;
} m2_steering_snapshot;
// Simulation-thread telemetry for bounded integration tests; no guest writes.
void m2_analog_steering_get_snapshot(m2_steering_snapshot *result);

void m2_analog_steering_set_enabled(int enabled);
int m2_analog_steering_enabled(void);
void m2_analog_steering_reset(void);
// The following run on the guest simulation thread, once per human input and
// at patchforce@40b80. Observation never changes guest registers or memory.
void m2_analog_steering_select_car(PPCContext *c, uint32_t car);
void m2_analog_steering_observe(PPCContext *c);
void m2_analog_steering_release(void);
// Input/output are native normalized commands (-1..1, left positive).
float m2_analog_steering_command(PPCContext *c, float input);

#ifdef __cplusplus
}
#endif
