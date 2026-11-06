// Optional thumbstick steering. The guest still computes all actual forces.
// See docs/recompilation/analog-steering.md for equations and response tuning.
#include "analog_steering.h"
#include "ppc_ctx.h"

#include <stdatomic.h>

static const float kPi = 3.14159265358979323846f;
// Shared controller tuning; these never modify guest tire parameters.
static const float kStickPower = 1.6f;
static const float kInputTime = 0.10f; // seconds, normalized driver intent
static const float kUnwindTime = 0.03f;
static const float kCenterTime = 0.014f; // seconds
static const float kAlignmentTime = 0.06f;
enum { kCorneringIntervals = 128 };
typedef struct {
    double scale, gain, slip, ratio, projection;
} cornering_curve;
typedef struct {
    double lateral[kCorneringIntervals], cosine[kCorneringIntervals];
} cornering_grid;
static _Atomic int s_enabled;
// Simulation-thread state, reset before boot or when the player car changes.
static struct {
    uint32_t car;
    uint64_t tick;
    float angle, intent, alignment[2];
    int alignment_ready;
    cornering_grid grid;
    int grid_ready;
    m2_steering_snapshot snapshot;
    struct {
        m2_steering_tire tire;
        m2_steering_tire analyzed_tire;
        cornering_curve curve;
        m2_steering_cornering peak;
        int analyzed, peak_valid;
        uint64_t tick;
        float flow, slip, forward;
    } contacts[4]; // native corner order: two fronts, then two rears
} s;

static float clamp(float x, float lo, float hi) {
    return fmaxf(lo, fminf(x, hi));
}
static int span(uint32_t p, uint32_t size) {
    return p >= 0x1000u && p <= RECOMP_GUEST_MEM - size;
}
static int valid_tire(const m2_steering_tire *t) {
    return t && isfinite(t->load) && t->load > 0.f &&
        isfinite(t->half_patch) && t->half_patch > 0.f &&
        isfinite(t->static_mu) && t->static_mu > 0.f &&
        isfinite(t->sliding_mu) && t->sliding_mu > 0.f &&
        isfinite(t->stiffness) && t->stiffness > 0.f &&
        isfinite(t->rolling_ratio) && t->rolling_ratio > 0.f &&
        isfinite(t->longitudinal_slip);
}

float m2_steering_lateral_force(const m2_steering_tire *t, float angle) {
    if (!valid_tire(t) || !isfinite(angle) || fabsf(angle) >= kPi * 0.5f) return NAN;
    // Dimensionless reduction of patchforce@40bec..40cd8, before relaxation.
    // The normal-force identity Fz=pi*a*b*K eliminates the cross-patch width.
    // Combined sliding retains longitudinal slip; removing it would predict
    // the wrong lateral response while braking or spinning a wheel.
    const double lateral = tan((double)angle);
    const double sliding = hypot(lateral, (double)t->longitudinal_slip);
    if (sliding == 0.) return 0.f;
    const double u = sliding / t->rolling_ratio * t->stiffness * kPi *
        t->half_patch * t->half_patch / (2. * t->static_mu * t->load);
    const double x = fmax(-1., fmin(1., 1. - 2. / (1. + u * u)));
    const double elastic = u * (1. - x) * (1. - x);
    const double sliding_part = t->sliding_mu / t->static_mu *
        (asin(x) + kPi * 0.5 + x * sqrt(fmax(0., 1. - x * x)));
    return (float)(t->static_mu * t->load * (elastic + sliding_part) / kPi * lateral / sliding);
}

// Dimensionless native patch integral G(u), with r=mu_k/mu_s. This form
// avoids asin cancellation near the plateau. Both the analysis tools and
// live turning-force optimizer use this continuous native curve.
static double slip_curve(double u, double r) {
    const double pi = 3.14159265358979323846;
    if (u <= 1.) {
        const double z = u * u;
        return (2. * r * atan(u) + 2. * u * (2. - r + r * z) / ((1. + z) * (1. + z))) / pi;
    }
    const double w = 1. / u, z = w * w;
    return r + (2. / pi) * (-r * atan(w) + w * (r + (2. - r) * z) / ((1. + z) * (1. + z)));
}

// Sign of dFy/du with longitudinal sliding v=gain*abs(k) held constant.
static double slip_slope(double u, double v, double r) {
    const double pi = 3.14159265358979323846;
    const double d = 3. - 4. * r, j = v / u;
    double ug;
    if (u <= 1.) {
        const double z = u * u;
        ug = 4. * u * (1. - d * z) / (pi * (1. + z) * (1. + z) * (1. + z));
    } else {
        const double w = 1. / u, z = w * w;
        ug = 4. * w * z * (z - d) / (pi * (1. + z) * (1. + z) * (1. + z));
    }
    return ug * (1. - j * j) + slip_curve(u, r) * j * j;
}

static double analyzed_force(double angle, double gain, double k, double r, double scale) {
    const double lateral = tan(angle), q = hypot(lateral, k);
    return q == 0. ? 0. : scale * slip_curve(gain * q, r) * lateral / q;
}

int m2_steering_analyze_slip(const m2_steering_tire *t, float fraction,
                           float bound, m2_steering_slip_analysis *out) {
    const double pi = 3.14159265358979323846;
    if (!out) return 0;
    *out = (m2_steering_slip_analysis){M2_SLIP_INVALID, NAN, NAN, NAN, NAN};
    if (!valid_tire(t) || t->longitudinal_slip >= 1.f ||
        !isfinite(fraction) || fraction <= 0.f || fraction > 1.f ||
        !isfinite(bound) || bound <= 0.f || (double)bound >= pi * .5) return 0;
    const double r = (double)t->sliding_mu / t->static_mu;
    const double scale = (double)t->static_mu * t->load;
    const double gain = t->stiffness * pi * t->half_patch * t->half_patch /
                        (2. * scale * t->rolling_ratio);
    const double k = fabs((double)t->longitudinal_slip), v = gain * k;
    const double d = 3. - 4. * r;
    double local_peak = NAN, peak = NAN, maximum = (double)t->sliding_mu * t->load;
    if (!isfinite(gain) || gain <= 0. || !isfinite(v * v) || !isfinite((float)maximum)) return 0;
    // Pure lateral G'(u)=4*(1-(3-4r)*u^2)/(pi*(1+u^2)^3).
    // r>=.75 is monotone. Otherwise combined slip can create a local maximum
    // followed by a minimum; find that maximum and compare it to the asymptote.
    if (d > 0.) {
        double u = 1. / sqrt(d);
        if (v > 0.) {
            const double b = 5. * d + 3. + 4. * d * v * v;
            const double c = 3. + 2. * (d + 3.) * v * v;
            const double disc = b * b - 4. * d * c;
            if (!isfinite(disc)) return 0;
            const double minimum = sqrt((b + sqrt(fmax(0., disc))) / (2. * d));
            double lo = fmax(v, u), hi = minimum;
            u = NAN;
            if (hi > lo && slip_slope(hi, v, r) < 0.) {
                for (unsigned i = 0; i < 60; ++i) {
                    const double mid = (lo + hi) * .5;
                    if (slip_slope(mid, v, r) > 0.) lo = mid;
                    else hi = mid;
                }
                u = (lo + hi) * .5;
            }
        }
        if (isfinite(u)) {
            local_peak = atan((u / gain) * sqrt(fmax(0., 1. - (v / u) * (v / u))));
            const double force = analyzed_force(local_peak, gain, k, r, scale);
            if (force >= maximum) { maximum = force; peak = local_peak; }
        }
    }
    const double at_bound = analyzed_force(bound, gain, k, r, scale);
    if (!isfinite((float)maximum) || (float)maximum <= 0.f || !isfinite((float)at_bound)) return 0;
    out->kind = isfinite(peak) ? M2_SLIP_FINITE_PEAK : M2_SLIP_ASYMPTOTIC;
    out->peak_slip = (float)peak;
    out->max_force = (float)maximum;
    out->force_at_bound = (float)at_bound;
    if (fraction == 1.f) {
        if (isfinite(peak) && peak <= bound) out->reference_slip = (float)peak;
        return 1;
    }
    // A sub-asymptotic local peak might reach the requested fraction before
    // the eventual plateau does. Use it as the first-crossing bracket too.
    const double target = fraction * maximum;
    double hi = bound;
    if (isfinite(local_peak) && analyzed_force(local_peak, gain, k, r, scale) >= target)
        hi = fmin(hi, local_peak);
    if (analyzed_force(hi, gain, k, r, scale) >= target) {
        double lo = 0.;
        for (unsigned i = 0; i < 48; ++i) {
            const double mid = (lo + hi) * .5;
            if (analyzed_force(mid, gain, k, r, scale) < target) lo = mid;
            else hi = mid;
        }
        out->reference_slip = (float)hi;
    }
    return 1;
}

// Prepare once per observed parameter change, rather than for every sample
// in the peak search and force inversion. Preserve the original arithmetic.
static int prepare_cornering(const m2_steering_tire *t, cornering_curve *curve) {
    if (!valid_tire(t) || t->longitudinal_slip>=1.f) return 0;
    curve->scale=(double)t->static_mu*t->load;
    curve->gain=t->stiffness*3.14159265358979323846*t->half_patch*t->half_patch /
                (2.*curve->scale*t->rolling_ratio);
    curve->slip=fabs((double)t->longitudinal_slip);
    curve->ratio=(double)t->sliding_mu/t->static_mu;
    curve->projection=1.-t->longitudinal_slip;
    return 1;
}

static float cornering_sample(const cornering_curve *curve, double lateral, double cosine) {
    const double q=hypot(lateral,curve->slip);
    const double force=curve->projection*cosine*(q==0. ? 0. :
        curve->scale*slip_curve(curve->gain*q,curve->ratio)*lateral/q);
    return isfinite((float)force) ? (float)force : NAN;
}

static float cornering_force(const cornering_curve *curve, float angle) {
    return cornering_sample(curve,tan((double)angle),cos((double)angle));
}

float m2_steering_cornering_force(const m2_steering_tire *t, float angle) {
    cornering_curve curve;
    if (!isfinite(angle) || fabsf(angle)>=kPi*.5f || !prepare_cornering(t,&curve)) return NAN;
    return cornering_force(&curve,angle);
}

// Only angle-grid trigonometry is cached, never tire forces or approximate
// grip values. Using the same float angles keeps search samples identical.
static void prepare_grid(cornering_grid *grid, float bound) {
    for (unsigned i=1;i<=kCorneringIntervals;++i) {
        const float angle=bound*i/kCorneringIntervals;
        grid->lateral[i-1]=tan((double)angle);
        grid->cosine[i-1]=cos((double)angle);
    }
}

static int cornering_optimum(const cornering_curve *curve, float bound,
                            const cornering_grid *grid, m2_steering_cornering *out) {
    unsigned best=0;
    float maximum=0.f;
    for (unsigned i=1;i<=kCorneringIntervals;++i) {
        const float force=grid ? cornering_sample(curve,grid->lateral[i-1],grid->cosine[i-1]) :
                                cornering_force(curve,bound*i/kCorneringIntervals);
        if (!isfinite(force)) return 0;
        if (force>maximum) { maximum=force; best=i; }
    }
    if (!(maximum>0.f)) return 0;
    float lo=bound*(best-1)/kCorneringIntervals;
    float hi=bound*fminf(best+1,kCorneringIntervals)/kCorneringIntervals;
    for (unsigned i=0;i<32;++i) {
        const float a=lo+(hi-lo)/3.f, z=hi-(hi-lo)/3.f;
        if (cornering_force(curve,a)<cornering_force(curve,z)) lo=a;
        else hi=z;
    }
    const float candidate=(lo+hi)*.5f;
    const float force=cornering_force(curve,candidate);
    const float end=cornering_force(curve,bound);
    *out=end>=force ? (m2_steering_cornering){bound,end,1} : (m2_steering_cornering){candidate,force,0};
    return 1;
}

int m2_steering_cornering_optimum(const m2_steering_tire *t, float bound,
                                m2_steering_cornering *out) {
    if (!out) return 0;
    *out=(m2_steering_cornering){NAN,NAN,0};
    cornering_curve curve;
    if (!isfinite(bound) || bound<=0.f || bound>=kPi*.5f || !prepare_cornering(t,&curve)) return 0;
    return cornering_optimum(&curve,bound,NULL,out);
}

static float slip_for_force(const cornering_curve *curve, const m2_steering_cornering *peak, float fraction) {
    if (fraction>=1.f) return peak->slip;
    float lo=0.f, hi=peak->slip;
    const float target=fraction*peak->force;
    for (unsigned i=0;i<24;++i) {
        const float mid=(lo+hi)*.5f;
        if (cornering_force(curve,mid)<target) lo=mid;
        else hi=mid;
    }
    return (lo+hi)*.5f;
}

void m2_analog_steering_get_snapshot(m2_steering_snapshot *out) {
    if (out) *out=s.snapshot;
}

static int recent(unsigned corner) {
    return s.contacts[corner].tick && s.contacts[corner].tick + 1 >= s.tick;
}

static float smoothstep(float x) { return x * x * (3.f - 2.f * x); }

int m2_analog_steering_enabled(void) { return atomic_load(&s_enabled); }
void m2_analog_steering_reset(void) { memset(&s, 0, sizeof(s)); }
void m2_analog_steering_set_enabled(int enabled) {
    atomic_store(&s_enabled, enabled != 0);
    m2_analog_steering_reset();
}
void m2_analog_steering_release(void) { s.angle = s.intent = 0.f; }

void m2_analog_steering_select_car(PPCContext *c, uint32_t car) {
    if (!m2_analog_steering_enabled()) return;
    if (!c || !c->mem || !span(car, 0x135c) || !span(rd32(c, car), 0x210)) car = 0;
    if (car != s.car) {
        m2_analog_steering_reset();
        s.car = car;
    }
    ++s.tick;
}

void m2_analog_steering_observe(PPCContext *c) {
    if (!m2_analog_steering_enabled() || !s.car || !c || !c->mem) return;
    // At patchforce@40b80: r31=tire, r28=travel velocity, r27=rotational
    // contact velocity; f27=normal force and f28=half patch length. All are
    // original values after interpolate_coefficients, even at exact zero slip.
    const uint32_t tire = c->gpr[31];
    if (!span(tire, 0x5a4) || rd32(c, tire + 0x1e0) != s.car ||
        !span(c->gpr[28], 12) || !span(c->gpr[27], 12)) return;
    unsigned index;
    for (index = 0; index < 4; ++index)
        if (rd32(c, s.car + 0x14u + index * 4u) == tire) break;
    if (index == 4) return;
    const uint32_t wheel = rd32(c, s.car + 4u + index * 4u);
    if (!span(wheel, 0x210)) return;
    const float forward = rdf32(c, c->gpr[28] + 8u);
    const float lateral = rdf32(c, c->gpr[28]);
    const float rolling = -rdf32(c, c->gpr[27] + 8u);
    const float steer = rdf32(c, wheel + 0x1f0u);
    if (!isfinite(forward) || !isfinite(lateral) || !isfinite(rolling) ||
        !isfinite(steer) || fabsf(forward) < 0.1f) return;
    m2_steering_tire t = {
        c->fpr[27], c->fpr[28], rdf32(c, tire + 0x234u),
        rdf32(c, tire + 0x238u), -rdf32(c, tire + 0x23cu),
        fmaxf(fabsf(rolling), 0.001f) / fabsf(forward),
        (forward - rolling) / fabsf(forward),
    };
    if (!valid_tire(&t)) return;
    s.contacts[index].tire = t;
    // tirefn builds its contact frame from axle x surface normal. Its lateral
    // travel is positive for positive wheel steer on a straight-moving car.
    // Subtract that slip to recover travel, so our own steer cannot feed back
    // as extra alignment. The raw wyrot frame has the opposite lateral sign.
    s.contacts[index].slip = atanf(lateral / forward);
    s.contacts[index].flow = steer - s.contacts[index].slip;
    s.contacts[index].forward = forward;
    s.contacts[index].tick = s.tick;
}

static float native_angle(float u, float scale, const float points[5]) {
    const float x = clamp(fabsf(u), 0.f, 1.f) * 4.f;
    const unsigned i = (unsigned)fminf(x, 3.f);
    const float divisor = points[i] + (x - i) * (points[i + 1] - points[i]);
    return u * scale / divisor;
}

static float native_command(float angle, float scale, const float points[5]) {
    if (angle == 0.f) return 0.f;
    float lo = 0.f, hi = 1.f;
    for (unsigned i = 0; i < 24; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (native_angle(mid, scale, points) < fabsf(angle)) lo = mid;
        else hi = mid;
    }
    return copysignf((lo + hi) * 0.5f, angle);
}

float m2_analog_steering_command(PPCContext *c, float input) {
    if (!m2_analog_steering_enabled()) return input;
    if (!isfinite(input)) input = 0.f;
    input = clamp(input, -1.f, 1.f);
    if (fabsf(input) <= 0.001f) input = 0.f;
    if (!c || !c->mem || !s.car) return input;
    const uint32_t cfg = rd32(c, s.car + 0x428u), body = rd32(c, s.car);
    if (!span(cfg, 0xb0) || !span(body, 0x210)) return input;
    const float scale = rdf32(c, cfg + 0x98u);
    float points[5], previous = 0.f;
    if (!isfinite(scale) || scale <= 0.f) return input;
    // Keep the actual car's piecewise steering-ratio curve. A malformed or
    // non-monotone curve retains the raw command instead of inventing angles.
    for (unsigned i = 0; i < 5; ++i) {
        points[i] = rdf32(c, cfg + 0x9cu + i * 4u);
        if (!isfinite(points[i]) || points[i] <= 0.f) return input;
        const float angle = (i * 0.25f) * scale / points[i];
        if (i && angle <= previous) return input;
        previous = angle;
    }
    const float max_angle = fminf(previous, kPi / 3.f);
    const float vx = rdf32(c, body), vz = rdf32(c, body + 8u);
    const float speed = hypotf(vx, vz);
    if (!isfinite(speed)) return input;
    float dt = rdf32(c, body + 0x1a4u);
    // ProcessInput runs once per world update, not once per body substep.
    // Full cars can integrate six times within that input interval.
    const uint32_t world = rd32(c, body + 0x1fcu);
    if (span(world, 0x30)) {
        const uint32_t steps = rd32(c, world + 0x1cu);
        if (steps > 0 && steps <= 60) dt = rdf32(c, world + 0x10u) * steps;
    }
    if (!isfinite(dt) || dt < 0.001f || dt > 0.05f) dt = 1.f / 60.f;
    const float shaped=copysignf(powf(fabsf(input),kStickPower),input);
    const float time=input==0.f ? kCenterTime :
        (shaped*s.intent<0.f || fabsf(shaped)<fabsf(s.intent)) ? kUnwindTime : kInputTime;
    s.intent+=(shaped-s.intent)*(-expm1f(-dt/time));
    if (fabsf(shaped-s.intent)<.00001f) s.intent=shaped;
    const float magnitude=fabsf(s.intent);
    s.snapshot=(m2_steering_snapshot){.car=s.car,.tick=s.tick,.intent=s.intent};
    float weight=0.f, requested=0.f, front_flow=0.f, reference=0.f;
    float rear_weight=0.f, recovery_flow[2]={0.f,0.f};
    for (unsigned i=0;i<4;++i) {
        if (!recent(i) || s.contacts[i].forward<=.1f) continue;
        if (!s.contacts[i].analyzed || memcmp(&s.contacts[i].analyzed_tire,&s.contacts[i].tire,sizeof(m2_steering_tire))) {
            if (!s.grid_ready) { prepare_grid(&s.grid,kPi/3.f); s.grid_ready=1; }
            s.contacts[i].peak_valid=prepare_cornering(&s.contacts[i].tire,&s.contacts[i].curve) &&
                cornering_optimum(&s.contacts[i].curve,kPi/3.f,&s.grid,&s.contacts[i].peak);
            s.contacts[i].analyzed_tire=s.contacts[i].tire;
            s.contacts[i].analyzed=1;
        }
        if (!s.contacts[i].peak_valid) continue;
        const m2_steering_cornering peak=s.contacts[i].peak;
        s.snapshot.contact_mask|=1u<<i;
        const float load=s.contacts[i].tire.load;
        if (i<2) {
            weight+=load;
            requested+=load*slip_for_force(&s.contacts[i].curve,&peak,magnitude);
            reference+=load*peak.slip;
            front_flow+=load*s.contacts[i].flow;
            s.snapshot.front_force+=peak.force;
        } else {
            // Beyond the measured rear turning-force peak, steering toward
            // rear travel can need more alignment than front travel alone.
            // This changes manual recovery alignment, never input rate or an
            // ordinary range ceiling. No opposing-yaw threshold is involved.
            const float excess=smoothstep(clamp(fabsf(s.contacts[i].slip)/peak.slip-1.f,0.f,1.f));
            rear_weight+=load;
            recovery_flow[0]+=load*excess*fmaxf(s.contacts[i].flow,0.f);
            recovery_flow[1]+=load*excess*fmaxf(-s.contacts[i].flow,0.f);
            s.snapshot.rear_force+=peak.force;
        }
    }
    float target=magnitude*max_angle;
    if (weight>0.f) {
        requested/=weight; front_flow/=weight; reference/=weight;
        for (unsigned i=0;i<2;++i) {
            if (rear_weight>0.f) recovery_flow[i]/=rear_weight;
            const float raw=fmaxf(fmaxf((i ? -1.f : 1.f)*front_flow,0.f),recovery_flow[i]);
            s.alignment[i]=s.alignment_ready ? s.alignment[i]+(raw-s.alignment[i])*(-expm1f(-dt/kAlignmentTime)) : raw;
        }
        s.alignment_ready=1;
        const float alignment=s.alignment[s.intent<0.f ? 1 : 0];
        const float live_target=requested+magnitude*alignment;
        const float blend=smoothstep(clamp((speed-5.f)/20.f,0.f,1.f));
        target+=blend*(live_target-target);
        s.snapshot.front_slip=reference;
        s.snapshot.recovery_flow=recovery_flow[s.intent<0.f ? 1 : 0];
    } else {
        // No live estimate is available (startup, air, reverse, locked tires).
        // Retain proportional native authority with the same intent filter;
        // do not invent a grip coefficient or silently hold a steering cap.
        s.snapshot.fallback=1;
    }
    target=copysignf(fminf(target,max_angle),s.intent);
    s.snapshot.target_angle=target;
    if (input==0.f) {
        // Preserve the short physical-angle release independently of the
        // nonlinear force-to-slip mapping used for a deflected stick.
        s.angle*=expf(-dt/kCenterTime);
        if (fabsf(s.angle)<.0001f) s.angle=0.f;
    } else s.angle=target;
    s.angle=clamp(s.angle,-max_angle,max_angle);
    s.snapshot.angle=s.angle;
    return native_command(s.angle,scale,points);
}
