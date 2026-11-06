// Frozen-state tire bench. Calls the original setup_tire and patchforce through
// production dispatch; no game boot, force transcription, or live-car writes.
// The ABI/offsets below come from setup_wheels@40570, setup_tire@40414 and
// tirefn@40e08. See docs/recompilation/tire-bench.md for the experiment boundary.
#include "recomp_host.h"
#include "analog_steering.h"
#include "control_modes.h"
#include "diab_runtime.h"
#include "recomp_helpers.h"
#include "utf8_path.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*GuestFn)(PPCContext *);
extern GuestFn recomp_lookup(unsigned guest);
extern unsigned g_recomp_data_base;

// Standalone runtime links have an inert selector fallback. Use the same
// conversion helpers as the production host for ProcessInput/do_controls;
// fail visibly if these isolated fixtures unexpectedly need another folio.
void *m2_selector_lookup(uint32_t folio, uint32_t ordinal) {
    fprintf(stderr, "tire bench: unexpected selector %u:%u\n", folio, ordinal);
    abort();
}

enum {
    TIRE = 0x200000, WHEEL = 0x201000, CAR = 0x202000,
    RATES = 0x204000, SLIDING = 0x204100, TRAVEL = 0x204120,
    ROTATION = 0x204140, FORCE = 0x204160, STACK = 0x210000,
    TIRE_BYTES = 0x5a4, MAX_CASES = 8192, MAX_STEPS = 4096,
};
typedef struct {
    unsigned id, car, corner;
    float speed, angle, kappa, load, grip;
} Request;
typedef struct {
    float lateral, normal, longitudinal, contact_load, residual, half_patch;
    unsigned steps;
    int settled;
    const char *status;
} Result;
typedef struct {
    PPCContext *cpu;
    GuestFn setup, patch;
    float dt, temperature, wear;
    unsigned max_steps;
    uint32_t tire_template; // self-test override, selected by native initCarModel
} Bench;

static int span(uint32_t address, unsigned size) {
    return address >= g_recomp_data_base && address < 0x200000u &&
           size <= 0x200000u - address;
}

static void enter(Bench *b, GuestFn fn) {
    b->cpu->gpr[1] = STACK;
    fn(b->cpu);
}

static int initialize(Bench *b, const Request *q) {
    PPCContext *c = b->cpu;
    const uint32_t cfg = rd32(c, g_recomp_data_base + 0x5d78u + q->car * 4u);
    if (!span(cfg, 0x68)) return 0;
    const uint32_t corner = rd32(c, cfg + 0x58u + q->corner * 4u);
    if (!span(corner, 4)) return 0;
    const uint32_t source = b->tire_template ? b->tire_template : rd32(c, corner);
    if (!span(source, TIRE_BYTES)) return 0;
    // setup_wheels copies the shipped template, supplies its owner and wear,
    // then calls setup_tire. The bench supplies only that constructor slice.
    memset(c->mem + TIRE, 0, STACK + 0x100u - TIRE);
    memcpy(c->mem + TIRE, c->mem + source, TIRE_BYTES);
    wr32(c, TIRE + 0x1e0, CAR);
    wr32(c, CAR, WHEEL);
    wrf32(c, CAR + 0x12e8, q->grip);
    wrf32(c, CAR + 0x130c, b->temperature);
    wrf32(c, TIRE + 0x584, b->wear);
    wrf32(c, WHEEL + 0x1a4, b->dt);
    // Surface coefficient multipliers are explicit neutral fixture inputs.
    // No Catmull surface lookup or named asphalt/grass preset is claimed.
    wr32(c, TIRE + 0x564, RATES);
    for (unsigned i = 0; i < 3; ++i) wrf32(c, RATES + 4u * i, 1.f);
    c->gpr[3] = WHEEL;
    c->gpr[4] = TIRE;
    enter(b, b->setup);
    // setup_tire prepares the copied coefficient table and its lookup cursor.
    const float radius = rdf32(c, TIRE);
    return isfinite(radius) && radius > 0.f &&
           isfinite(rdf32(c, TIRE + 0x598));
}

static void patch_at(Bench *b, uint32_t tire, float height, float lateral,
                     float forward, float rolling) {
    PPCContext *c = b->cpu;
    // tirefn passes sliding velocity in r4, translational travel in r5, and
    // rotational contact velocity in r6. A freely rolling tire has zero
    // longitudinal sliding velocity, not zero longitudinal travel velocity.
    wrf32(c, SLIDING, lateral);
    wrf32(c, SLIDING + 8, forward - rolling);
    wrf32(c, TRAVEL, lateral);
    wrf32(c, TRAVEL + 8, forward);
    wrf32(c, ROTATION + 8, -rolling);
    c->gpr[3] = tire;
    c->gpr[4] = SLIDING;
    c->gpr[5] = TRAVEL;
    c->gpr[6] = ROTATION;
    c->gpr[7] = FORCE;
    c->fpr[1] = height;
    c->fpr[2] = b->dt;
    enter(b, b->patch);
}

static void patch(Bench *b, float height, float lateral, float forward, float rolling) {
    patch_at(b, TIRE, height, lateral, forward, rolling);
}

static Result sample(Bench *b, const Request *q) {
    Result r = {NAN, NAN, NAN, NAN, NAN, NAN, 0, 0, "initialization_failed"};
    if (!initialize(b, q)) return r;
    PPCContext *c = b->cpu;
    const float radius = rdf32(c, TIRE);
    float lo = 0.f, hi = radius;
    // Query native normal force with nonzero slip: patchforce's EXACT-zero
    // sliding branch returns a zero vector, including its normal component.
    // This calibration probe avoids that branch; measured sample forces below
    // retain it exactly. No artificial slip is added to the actual sample.
    patch(b, lo, 1.f, 100.f, 100.f);
    if (!isfinite(rdf32(c, FORCE + 4)) || rdf32(c, FORCE + 4) < q->load) {
        r.status = "load_unreachable";
        return r;
    }
    float height = 0.f;
    for (unsigned i = 0; i < 40; ++i) {
        height = (lo + hi) * 0.5f;
        patch(b, height, 1.f, 100.f, 100.f);
        r.contact_load = rdf32(c, FORCE + 4);
        if (!isfinite(r.contact_load)) {
            r.status = "nonfinite";
            return r;
        }
        if (fabsf(r.contact_load - q->load) <= fmaxf(0.001f, q->load * 0.001f))
            break;
        if (r.contact_load > q->load) lo = height;
        else hi = height;
    }
    if (fabsf(r.contact_load - q->load) > fmaxf(0.001f, q->load * 0.001f)) {
        r.status = "load_unreachable";
        return r;
    }
    const float lateral = -q->speed * tanf(q->angle * (3.14159265358979323846f / 180.f));
    r.half_patch = sqrtf(fmaxf(0.f, radius * radius - height * height));
    const float rolling = q->speed * (1.f + q->kappa);
    // patchforce@40d50..40d7c weights force history by
    // min(1, dt*(max(abs(rolling), .001)+abs(forward))/half_patch_length).
    // half_patch_length <= radius, so this reference call forces weight=1.
    // It changes only the reference relaxation dt; measured calls below keep
    // the requested dt. Small deltas alone can falsely converge at low speed.
    const float dt = b->dt;
    b->dt = fmaxf(dt, 2.f * radius / fmaxf(0.001f, fabsf(rolling) + q->speed));
    patch(b, height, lateral, q->speed, rolling);
    b->dt = dt;
    float target[3];
    for (unsigned j = 0; j < 3; ++j) {
        target[j] = rdf32(c, FORCE + 4u * j);
        if (!isfinite(target[j])) { r.status = "nonfinite"; return r; }
    }
    memset(c->mem + TIRE + 0x1fc, 0, 12); // fresh measured force history
    float previous[3] = {0};
    unsigned stable = 0;
    r.status = "unsettled";
    for (unsigned i = 1; i <= b->max_steps; ++i) {
        patch(b, height, lateral, q->speed, rolling);
        r.lateral = rdf32(c, FORCE);
        r.normal = rdf32(c, FORCE + 4);
        r.longitudinal = rdf32(c, FORCE + 8);
        r.steps = i;
        const float current[] = {r.lateral, r.normal, r.longitudinal};
        r.residual = 0.f;
        for (unsigned j = 0; j < 3; ++j) {
            if (!isfinite(current[j])) { r.status = "nonfinite"; return r; }
            r.residual = fmaxf(r.residual, fabsf(current[j] - previous[j]));
            r.residual = fmaxf(r.residual, fabsf(current[j] - target[j]));
            previous[j] = current[j];
        }
        // Require both a sustained small delta and agreement with the native
        // fully relaxed force, rather than mistaking slow change for settling.
        const float scale = fmaxf(q->load, fmaxf(fabsf(r.lateral), fabsf(r.longitudinal)));
        stable = r.residual <= fmaxf(0.0001f, scale * 0.00001f) ? stable + 1 : 0;
        if (i >= 16 && stable >= 8) {
            r.settled = 1;
            r.status = "ok";
            break;
        }
    }
    return r;
}

static float native_steering_angle(Bench *b, GuestFn controls, uint32_t car, float command) {
    wrf32(b->cpu,car+0x12d4,command);
    b->cpu->gpr[3]=car; enter(b,controls);
    return rdf32(b->cpu,car+0x1334);
}


static int steering_test(Bench *b) {
    enum { VCAR=0x220000, BODY=0x222000, WHEELS=0x223000, GDP=0x227000,
           MATRIX=0x228000, VECTOR=0x228040, LOCAL=0x228060,
           INPUT=0x1812b8, OUTPUT=0x17c7c4, PAD=0x17c664 };
    PPCContext *c=b->cpu;
    int failures=0;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "steering guest: FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
    GuestFn process=recomp_lookup(0x109680), controls=recomp_lookup(0x146dc0);
    GuestFn wyrot=recomp_lookup(0x14a1c8), local=recomp_lookup(0x13d250);
    CHECK(process && controls && wyrot && local);
    if (!process || !controls || !wyrot || !local) return 1;
    // This raw rotation frame has the opposite lateral sign from tirefn's
    // axle-cross-normal contact frame. The moving full-car fixture verifies
    // the observer against the actual mounted tire callback.
    for (unsigned i=0; i<9; ++i) wrf32(c, MATRIX+4*i, i%4==0 ? 1.f : 0.f);
    wrf32(c, VECTOR, 0.f); wrf32(c, VECTOR+4, 0.f); wrf32(c, VECTOR+8, 100.f);
    c->gpr[3]=MATRIX; c->fpr[1]=sinf(.2f); c->fpr[2]=cosf(.2f); enter(b, wyrot);
    c->gpr[3]=LOCAL; c->gpr[4]=VECTOR; c->gpr[5]=MATRIX; enter(b, local);
    CHECK(fabsf(.2f+atanf(rdf32(c, LOCAL)/rdf32(c, LOCAL+8))) < .00001f);

    wr32(c, 0x177870, 1); // ProcessInput's one-time device setup already done
    const uint32_t race=rd32(c, 0x17d988);
    const unsigned old_mode=rd8(c, race+4);
    const float speeds[]={0.f,60.f,150.f};
    for (unsigned car=0; car<4; ++car) for (int mode=0; mode<3; ++mode)
    for (unsigned physics=0; physics<2; ++physics) for (unsigned layout=0; layout<3; ++layout)
    for (unsigned speed=0; speed<3; ++speed) for (int sign=-1; sign<=1; sign+=2)
    for (unsigned travel=0; travel<3; ++travel) {
        const float magnitude=travel==0 ? .25f : travel==1 ? .5f : 1.f;
        memset(c->mem+VCAR, 0, 0x8000);
        const uint32_t cfg=rd32(c, g_recomp_data_base+0x5d78+4*car);
        wr32(c, VCAR, BODY); wr32(c, VCAR+0x428, cfg);
        for (unsigned i=0; i<4; ++i) wr32(c, VCAR+4+4*i, WHEELS+0x400*i);
        wrf32(c, BODY+8, speeds[speed]); wrf32(c, BODY+0x1a4, 1.f/60.f);
        wrf32(c, GDP+0x40, 150.f);
        wr8(c, VCAR+0x12ce, 1); // skip independent clutch/powertrain fixture
        wr8(c, race+4, physics);
        memset(c->mem+INPUT, 0, 0x30); memset(c->mem+OUTPUT, 0, 0x60);
        wr32(c, INPUT+0x20, layout==1 ? 0x17c6a0 : PAD);
        wr8(c, INPUT+0x29, layout==2 ? 7 : 0);
        wr8(c, INPUT+0x2a, 128); wr8(c, INPUT+0x2b, 128);
        m2_controls_set_mode((m2_controls_mode)mode);
        m2_analog_steering_set_enabled(1);
        const float expected=fminf(rdf32(c,cfg+0x98)/rdf32(c,cfg+0xac),3.14159265358979323846f/3.f);
        const float shaped=sign*powf(magnitude,1.6f)*expected;
        m2_controls_publish(-sign*magnitude, 0.f, .7f, .2f);
        for (unsigned frame=0; frame<120; ++frame) {
            c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
            enter(b, process);
        }
        const float command=rdf32(c, OUTPUT+8);
        CHECK(isfinite(command) && command*sign > 0.f);
        wrf32(c, VCAR+0x12d4, command);
        c->gpr[3]=VCAR; enter(b, controls);
        const float angle=rdf32(c, VCAR+0x1334);
        if (!isfinite(angle) || fabsf(angle-shaped) > .00002f) {
            fprintf(stderr, "native steering car=%u profile=%d physics=%u layout=%u speed=%u travel=%g command=%g angle=%g expected=%g\n",
                    car, mode, physics, layout, speed, magnitude, command, angle, shaped);
            ++failures;
        }
        // Exercise neutral through the real decoder and wheel conversion,
        // including Custom's digital protocol with no digital input held.
        m2_controls_publish(0.f, 0.f, 0.f, 0.f);
        float previous=angle;
        int monotone=1;
        for (unsigned frame=0; frame<30; ++frame) {
            c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
            enter(b, process);
            wrf32(c, VCAR+0x12d4, rdf32(c,OUTPUT+8));
            c->gpr[3]=VCAR; enter(b, controls);
            const float returning=rdf32(c,VCAR+0x1334);
            if (!frame) CHECK(returning*sign>0.f && fabsf(returning)<fabsf(angle));
            if (frame==2) CHECK(fabsf(returning)<.1f*fabsf(angle));
            if (!isfinite(returning) || returning*sign<0.f || fabsf(returning)>fabsf(previous)) monotone=0;
            previous=returning;
        }
        CHECK(monotone && previous==0.f);
        if (travel==2) {
            const float tap_angle=expected*(1.f-expf(-2.f/(60.f*.10f)));
            // Short full-stick taps must respond promptly through the native
            // curve, return cleanly, and not wind up over successive pulses.
            float first_peak=0.f;
            for (unsigned pulse=0; pulse<4; ++pulse) {
                float peak=0.f;
                m2_controls_publish(-(float)sign, 0.f, 0.f, 0.f);
                for (unsigned frame=0; frame<2; ++frame) {
                    c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
                    enter(b, process);
                    wrf32(c, VCAR+0x12d4, rdf32(c,OUTPUT+8));
                    c->gpr[3]=VCAR; enter(b, controls);
                    peak=sign*rdf32(c,VCAR+0x1334);
                }
                if (!pulse) {
                    first_peak=peak;
                    CHECK(fabsf(peak-tap_angle)<.00002f);
                }
                CHECK(peak<=expected+.00002f && peak<=first_peak*1.05f);
                m2_controls_publish(0.f, 0.f, 0.f, 0.f);
                previous=peak;
                for (unsigned frame=0; frame<4; ++frame) {
                    c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
                    enter(b, process);
                    wrf32(c, VCAR+0x12d4, rdf32(c,OUTPUT+8));
                    c->gpr[3]=VCAR; enter(b, controls);
                    const float returning=sign*rdf32(c,VCAR+0x1334);
                    CHECK(returning>=0.f && returning<=previous);
                    previous=returning;
                }
                CHECK(previous<.05f*peak);
            }
        }
    }
    // Fresh native observations replace the proportional fallback with the
    // actual contact's turning-force maximum. No fixed speed ceiling remains.
    m2_analog_steering_set_enabled(0);
    Request q={0,0,0,100.f,5.f,0.f,600.f,1.f};
    Result contact=sample(b,&q);
    CHECK(contact.settled);
    wr32(c,VCAR+0x14,TIRE);wr32(c,TIRE+0x1e0,VCAR);
    wrf32(c,VCAR+0x12e8,1.f);wrf32(c,BODY+8,100.f);
    wrf32(c,WHEELS+0x1f0,.2f);
    m2_analog_steering_set_enabled(1);m2_analog_steering_select_car(c,VCAR);
    const float radius=rdf32(c,TIRE);
    const float height=sqrtf(fmaxf(0.f,radius*radius-contact.half_patch*contact.half_patch));
    patch(b,height,100.f*tanf(.2f),100.f,100.f);
    m2_steering_tire observed={contact.contact_load,contact.half_patch,rdf32(c,TIRE+0x234),
        rdf32(c,TIRE+0x238),-rdf32(c,TIRE+0x23c),1.f,0.f};
    m2_steering_cornering peak;
    CHECK(m2_steering_cornering_optimum(&observed,3.14159265358979323846f/3.f,&peak));
    float command=0.f;
    for (unsigned i=0;i<120;++i) command=m2_analog_steering_command(c,1.f);
    const float ordinary=native_steering_angle(b,controls,VCAR,command);
    CHECK(fabsf(ordinary-peak.slip)<.0001f);
    // Travel toward the requested direction supplies alignment; opposite
    // travel cannot erase the requested steering side during recovery.
    wrf32(c,WHEELS+0x1f0,.2f);
    patch(b,height,-100.f*tanf(.05f),100.f,100.f);
    for (unsigned i=0;i<120;++i) command=m2_analog_steering_command(c,1.f);
    const float target=fminf(rdf32(c,rd32(c,VCAR+0x428)+0x98)/rdf32(c,rd32(c,VCAR+0x428)+0xac),.25f+peak.slip);
    CHECK(fabsf(native_steering_angle(b,controls,VCAR,command)-target)<.0001f);
    // Steer away from a large travel angle through the real ProcessInput,
    // native car curve, Ackermann, and patchforce observer on every frame.
    // Steering away from travel retains the ordinary range regardless of
    // opposing yaw, in every profile and native input protocol.
    for (int mode=0; mode<3; ++mode) for (unsigned physics=0; physics<2; ++physics)
    for (unsigned layout=0; layout<3; ++layout) for (int sign=-1; sign<=1; sign+=2) {
        m2_controls_set_mode((m2_controls_mode)mode);
        m2_analog_steering_set_enabled(1);
        memset(c->mem+INPUT, 0, 0x30); memset(c->mem+OUTPUT, 0, 0x60);
        wr32(c, INPUT+0x20, layout==1 ? 0x17c6a0 : PAD);
        wr8(c, INPUT+0x29, layout==2 ? 7 : 0);
        wr8(c, INPUT+0x2a, 128); wr8(c, INPUT+0x2b, 128);
        wr8(c, race+4, physics); wrf32(c, BODY+0x70, sign*.5f);
        m2_controls_publish((float)sign, 0.f, 0.f, 0.f); // host right-positive
        for (unsigned frame=0; frame<120; ++frame) {
            c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
            enter(b, process);
            wrf32(c, VCAR+0x12d4, rdf32(c,OUTPUT+8));
            c->gpr[3]=VCAR; enter(b, controls);
            const float steer=rdf32(c,WHEELS+0x1f0);
            patch(b,height,100.f*tanf(steer-sign*.6f),100.f,100.f);
        }
        const float angle=rdf32(c,VCAR+0x1334);
        if (!isfinite(angle) || angle*sign>=0.f || fabsf(angle+sign*ordinary)>.0001f) {
            fprintf(stderr,"opposite steering profile=%d physics=%u layout=%u flow=%g angle=%g expected=%g\n",
                    mode,physics,layout,sign*.6f,angle,-sign*ordinary);
            ++failures;
        }
        // A small held input isolates bound adaptation from wheel-angle slew.
        // Open and close one side through live native contact observations;
        // neither transition may jump straight to the newly available range.
        m2_controls_publish(-sign*.05f, 0.f, 0.f, 0.f);
        wrf32(c, BODY+0x70, -sign*.5f);
        float response[5];
        for (unsigned stage=0; stage<5; ++stage) {
            // Keep the ordinary reference slightly away from the requested
            // side, clear of roundoff at the countersteer flow-sign boundary.
            const float flow=(stage==1 || stage==2) ? sign*.25f : -sign*.01f;
            const unsigned frames=(stage==1 || stage==3) ? 1 : 120;
            for (unsigned frame=0; frame<frames; ++frame) {
                const float steer=rdf32(c,WHEELS+0x1f0);
                patch(b,height,100.f*tanf(steer-flow),100.f,100.f);
                c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
                enter(b, process);
                wrf32(c, VCAR+0x12d4, rdf32(c,OUTPUT+8));
                c->gpr[3]=VCAR; enter(b, controls);
            }
            response[stage]=sign*rdf32(c,VCAR+0x1334);
        }
        CHECK(response[1]>response[0] && response[1]<response[0]+.5f*(response[2]-response[0]));
        CHECK(response[3]<response[2] && response[3]>response[0]+.5f*(response[2]-response[0]));
        CHECK(fabsf(response[4]-response[0])<.00002f);
    }
    wr8(c, race+4, old_mode);
    m2_analog_steering_set_enabled(0);
    m2_controls_set_mode(M2_CONTROLS_ORIGINAL);
#undef CHECK
    fprintf(stderr, "steering guest: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures;
}


static int tire_brand_steering_test(Bench *b) {
    enum { VCAR=0x220000, BODY=0x222000, WHEELS=0x223000, GDP=0x227000,
           CFG=0x230000, ENGINE=0x231000, TRANS=0x232000, FRONT=0x233000, REAR=0x234000,
           REAR_TIRES=0x235000,
           INPUT=0x1812b8, OUTPUT=0x17c7c4, PAD=0x17c664 };
    PPCContext *c=b->cpu;
    int failures=0;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"tire brands: FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while (0)
    GuestFn model=recomp_lookup(0x116d10), process=recomp_lookup(0x109680), controls=recomp_lookup(0x146dc0);
    CHECK(model && process && controls);
    if (!model || !process || !controls) return 1;
    uint32_t templates[4], rear_templates[4];
    // Use the guest's actual tire-selection path; no copied coefficients or
    // brand-specific steering constants. All constructor outputs are scratch.
    for (unsigned brand=0; brand<4; ++brand) {
        wr32(c,STACK+8,brand);
        c->gpr[3]=CFG; c->gpr[4]=ENGINE; c->gpr[5]=TRANS;
        c->gpr[6]=FRONT; c->gpr[7]=REAR; c->gpr[8]=0; c->gpr[9]=0; c->gpr[10]=0;
        enter(b,model);
        templates[brand]=rd32(c,FRONT);
        rear_templates[brand]=rd32(c,REAR);
        CHECK(span(templates[brand],TIRE_BYTES) && span(rear_templates[brand],TIRE_BYTES));
    }
    const uint32_t race=rd32(c,0x17d988), cfg=rd32(c,g_recomp_data_base+0x5d78);
    const unsigned old_mode=rd8(c,race+4);
    float summary_angle[4]={0}, summary_force[4]={0};
    for (int mode=0; mode<3; ++mode) for (unsigned physics=0; physics<2; ++physics)
    for (unsigned layout=0; layout<3; ++layout) for (int sign=-1; sign<=1; sign+=2) {
        float first[4], force[4], final_force[4];
        for (unsigned brand=0; brand<4; ++brand) {
            m2_analog_steering_set_enabled(0);
            b->tire_template=rear_templates[brand];
            Request q={0,0,0,60.f,1.f,0.f,600.f,1.f};
            const Result rear=sample(b,&q);
            CHECK(rear.settled);
            const float rear_radius=rdf32(c,TIRE);
            const float rear_height=sqrtf(fmaxf(0.f,rear_radius*rear_radius-rear.half_patch*rear.half_patch));
            unsigned char original_rear[TIRE_BYTES];
            memcpy(original_rear,c->mem+rear_templates[brand],TIRE_BYTES);
            for (unsigned i=0; i<2; ++i) memcpy(c->mem+REAR_TIRES+0x800*i,c->mem+TIRE,TIRE_BYTES);
            b->tire_template=templates[brand];
            unsigned char original[TIRE_BYTES];
            memcpy(original,c->mem+templates[brand],TIRE_BYTES);
            const Result contact=sample(b,&q);
            CHECK(contact.settled);
            const float radius=rdf32(c,TIRE);
            const float height=sqrtf(fmaxf(0.f,radius*radius-contact.half_patch*contact.half_patch));
            memset(c->mem+VCAR,0,0x8000);
            wr32(c,VCAR,BODY); wr32(c,VCAR+0x428,cfg);
            for (unsigned i=0; i<4; ++i) wr32(c,VCAR+4+4*i,WHEELS+0x400*i);
            wr32(c,VCAR+0x14,TIRE); wr32(c,TIRE+0x1e0,VCAR);
            for (unsigned i=0; i<2; ++i) {
                wr32(c,VCAR+0x1c+4*i,REAR_TIRES+0x800*i);
                wr32(c,REAR_TIRES+0x800*i+0x1e0,VCAR);
            }
            wrf32(c,VCAR+0x12e8,1.f); wrf32(c,VCAR+0x130c,b->temperature);
            wrf32(c,BODY+0x1a4,1.f/60.f); wrf32(c,GDP+0x40,150.f);
            wr8(c,VCAR+0x12ce,1); wr8(c,race+4,physics);
            memset(c->mem+INPUT,0,0x30); memset(c->mem+OUTPUT,0,0x60);
            wr32(c,INPUT+0x20,layout==1 ? 0x17c6a0 : PAD);
            wr8(c,INPUT+0x29,layout==2 ? 7 : 0);
            wr8(c,INPUT+0x2a,128); wr8(c,INPUT+0x2b,128);
            m2_controls_set_mode((m2_controls_mode)mode);
            m2_analog_steering_set_enabled(1);
            m2_analog_steering_select_car(c,VCAR);
            float held=0.f;
            // Native front/rear observations drive every decoder update.
            // Add rear lateral saturation, recovery, combined braking use,
            // and restoration of grip to the ordinary input scenarios.
            float tight=0.f;
            for (unsigned phase=0; phase<11; ++phase) {
                if (phase>=2 && phase!=10) m2_analog_steering_release();
                if (phase>=4 && phase<=6) {
                    m2_analog_steering_set_enabled(1);
                    m2_analog_steering_select_car(c,VCAR);
                }
                const float speed=phase==3 ? 0.f : phase==4 ? 20.f : phase==5 ? 150.f : phase==6 ? 300.f : 60.f;
                const float contact_speed=phase==3 ? 60.f : speed;
                wrf32(c,BODY+8,speed);
                wrf32(c,BODY+0x70,(phase==2 || phase==8) ? -sign*.5f : 0.f);
                m2_controls_publish(phase==1 ? 0.f : -sign*((phase>=4 && phase<=6) ? .05f : 1.f),0.f,0.f,0.f);
                float small_start=0.f;
                for (unsigned frame=0; frame<(phase==0 || phase>=4 ? 120u : 1u); ++frame) {
                    const float steer=rdf32(c,WHEELS+0x1f0);
                    const float flow=phase==8 ? sign*.25f : 0.f;
                    patch(b,height,contact_speed*tanf(steer-flow),contact_speed,contact_speed);
                    for (unsigned i=0; i<2; ++i) {
                        const float alpha=phase==7 ? sign*.3f : phase==8 ? -sign*.3f : 0.f;
                        patch_at(b,REAR_TIRES+0x800*i,rear_height,contact_speed*tanf(alpha),contact_speed,
                                 contact_speed*(phase==9 ? .5f : 1.f));
                    }
                    c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
                    enter(b,process);
                    wrf32(c,VCAR+0x12d4,rdf32(c,OUTPUT+8));
                    c->gpr[3]=VCAR; enter(b,controls);
                    if (!frame) {
                        small_start=sign*rdf32(c,VCAR+0x1334);
                    }
                    if (phase==0 && (!frame || frame==119)) {
                        patch(b,height,60.f*tanf(rdf32(c,WHEELS+0x1f0)),60.f,60.f);
                        if (!frame) {
                            first[brand]=sign*rdf32(c,VCAR+0x1334);
                            force[brand]=-sign*rdf32(c,FORCE)/contact.contact_load;
                        } else final_force[brand]=-sign*rdf32(c,FORCE)/contact.contact_load;
                    }
                }
                const float angle=sign*rdf32(c,VCAR+0x1334);
                if (phase==0) {
                    m2_steering_tire tire={contact.contact_load,contact.half_patch,
                        rdf32(c,TIRE+0x234),rdf32(c,TIRE+0x238),-rdf32(c,TIRE+0x23c),1.f,0.f};
                    held=angle;
                    m2_steering_cornering peak;
                    CHECK(m2_steering_cornering_optimum(&tire,3.14159265358979323846f/3.f,&peak));
                    CHECK(fabsf(held-peak.slip)<.0001f);
                } else if (phase==1) CHECK(fabsf(angle-held*expf(-(1.f/60.f)/.014f))<.00002f);
                else if (phase==2) CHECK(angle>0.f && angle<held);
                else if (phase==3) {
                    const float max=fminf(rdf32(c,cfg+0x98)/rdf32(c,cfg+0xac),3.14159265358979323846f/3.f);
                    CHECK(fabsf(angle-max*(1.f-expf(-1.f/(60.f*.10f))))<.00002f);
                }
                else if (phase<=6) CHECK(small_start>0.f && angle>small_start);
                else if (phase==7) {
                    tight=angle;
                    CHECK(fabsf(angle-held)<.0001f);
                } else if (phase==8) CHECK(angle>held && small_start>0.f && small_start<angle);
                else if (phase==9) CHECK(fabsf(angle-tight)<.00002f);
                else CHECK(fabsf(angle-held)<.0001f);
            }
            // Full input reaches the live cornering optimum at all these
            // speeds, including where the previous native speed cap was lower.
            const float corner_mph[]={75.f,125.f,175.f};
            for (unsigned corner=0;corner<3;++corner) {
                const float mph=corner_mph[corner],speed=mph*22.f/15.f;
                m2_analog_steering_set_enabled(1);m2_analog_steering_select_car(c,VCAR);
                wrf32(c,BODY,0.f);wrf32(c,BODY+8,speed);wrf32(c,BODY+0x70,0.f);wrf32(c,GDP+0x40,mph);
                m2_controls_publish(-(float)sign,0.f,0.f,0.f);
                float angle=0.f;
                for (unsigned frame=0;frame<120;++frame) {
                    const float steer=rdf32(c,WHEELS+0x1f0);
                    patch(b,height,speed*tanf(steer),speed,speed);
                    for (unsigned i=0;i<2;++i) patch_at(b,REAR_TIRES+0x800*i,rear_height,0.f,speed,speed);
                    c->gpr[3]=INPUT;c->gpr[4]=OUTPUT;c->gpr[5]=GDP;c->gpr[6]=VCAR;enter(b,process);
                    angle=sign*native_steering_angle(b,controls,VCAR,rdf32(c,OUTPUT+8));
                }
                CHECK(fabsf(angle-held)<.0001f);
            }
            // Ordinary corrections at 200 mph: contact travel includes the
            // body's rotation, with rear force use below 20% of capacity.
            // Changing yaw must not select a different response rate or
            // introduce extra range beyond the observed travel alignment.
            const float corrections[]={.02f,.1f,1.f};
            const float rotations[]={0.f,-.049f,-.051f,-.125f,-.25f,.051f,.25f};
            const float correction_speed=200.f*22.f/15.f;
            for (unsigned drift=0; drift<2; ++drift) for (unsigned travel=0; travel<3; ++travel) {
                float reference[8];
                for (unsigned rotation=0; rotation<7; ++rotation) {
                    m2_analog_steering_set_enabled(1);
                    m2_analog_steering_select_car(c,VCAR);
                    const float yaw=sign*rotations[rotation];
                    const float lateral=drift ? sign*.005f*correction_speed : 0.f;
                    wrf32(c,BODY,lateral);
                    wrf32(c,BODY+8,correction_speed); wrf32(c,GDP+0x40,200.f);
                    wrf32(c,BODY+0x70,yaw);
                    m2_controls_publish(-sign*corrections[travel],0.f,0.f,0.f);
                    const float front_flow=-atanf((lateral+yaw*rdf32(c,cfg+0x30))/correction_speed);
                    const float rear_flow=-atanf((lateral+yaw*rdf32(c,cfg+0x54))/correction_speed);
                    for (unsigned frame=0; frame<8; ++frame) {
                        const float steer=rdf32(c,WHEELS+0x1f0);
                        patch(b,height,correction_speed*tanf(steer-front_flow),correction_speed,correction_speed);
                        for (unsigned i=0; i<2; ++i) {
                            patch_at(b,REAR_TIRES+0x800*i,rear_height,-correction_speed*tanf(rear_flow),correction_speed,correction_speed);
                            const float used=hypotf(rdf32(c,FORCE),rdf32(c,FORCE+8)) /
                                (rear.contact_load*rdf32(c,REAR_TIRES+0x800*i+0x238));
                            CHECK(isfinite(used) && used<.2f);
                        }
                        c->gpr[3]=INPUT; c->gpr[4]=OUTPUT; c->gpr[5]=GDP; c->gpr[6]=VCAR;
                        enter(b,process);
                        const float angle=sign*native_steering_angle(b,controls,VCAR,rdf32(c,OUTPUT+8));
                        m2_steering_snapshot snapshot;m2_analog_steering_get_snapshot(&snapshot);
                        if (!rotation) reference[frame]=angle;
                        else if (!isfinite(angle) || fabsf(angle-reference[frame]-
                            fabsf(snapshot.intent)*(fmaxf(sign*front_flow,0.f)-fmaxf(-sign*atanf(lateral/correction_speed),0.f)))>.00003f) {
                            fprintf(stderr,"ordinary correction profile=%d physics=%u layout=%u brand=%u drift=%u input=%g yaw=%g frame=%u angle=%g reference=%g\n",
                                    mode,physics,layout,brand,drift,corrections[travel],yaw,frame,angle,reference[frame]);
                            ++failures;
                        }
                    }
                }
            }
            CHECK(!memcmp(original,c->mem+templates[brand],TIRE_BYTES));
            CHECK(!memcmp(original_rear,c->mem+rear_templates[brand],TIRE_BYTES));
        }
        // All brands share the normalized intent filter. Their native
        // forces and contact-derived steering targets remain distinct.
        for (unsigned brand=1;brand<4;++brand) CHECK(fabsf(first[brand]-first[0])<.0001f);
        CHECK(force[1]>force[3] && force[1]<1.26f*force[3]);
        CHECK(final_force[1]>1.24f*final_force[3] && final_force[1]<1.27f*final_force[3]);
        memcpy(summary_angle,first,sizeof(first)); memcpy(summary_force,force,sizeof(force));
    }
    b->tire_template=0;
    wr8(c,race+4,old_mode);
    m2_analog_steering_set_enabled(0); m2_controls_set_mode(M2_CONTROLS_ORIGINAL);
#undef CHECK
    fprintf(stderr,"tire brands: initial angle rad [%g %g %g %g], force/load [%g %g %g %g]\n",
        summary_angle[0],summary_angle[1],summary_angle[2],summary_angle[3],
        summary_force[0],summary_force[1],summary_force[2],summary_force[3]);
    fprintf(stderr,"tire brands: %s (%d failures)\n",failures ? "FAIL" : "OK",failures);
    return failures;
}

// Forward, planar contact: rotate native wheel force back to a frame whose
// forward axis is the travel direction. The guest uses the inverse transform
// after patchforce (tirefn@41464..41470), including its longitudinal component.
static float native_cornering_force(Bench *b, float slip) {
    enum { MATRIX=0x240000, VECTOR=0x240040 };
    PPCContext *c=b->cpu;
    for (unsigned i=0;i<9;++i) wrf32(c,MATRIX+4*i,i%4==0 ? 1.f : 0.f);
    c->gpr[3]=MATRIX; c->fpr[1]=sinf(slip); c->fpr[2]=cosf(slip);
    enter(b,recomp_lookup(0x14a1c8));
    c->gpr[3]=VECTOR; c->gpr[4]=FORCE; c->gpr[5]=MATRIX;
    enter(b,recomp_lookup(0x13d288));
    return rdf32(c,VECTOR);
}

static float predicted_cornering_force(const m2_steering_tire *t, float slip) {
    return (1.f-t->longitudinal_slip)*m2_steering_lateral_force(t,slip)*cosf(slip);
}

// Bounded numerical candidate for the planar, travel-normal force maximum.
// Independent benchmark search for the production optimizer's objective.
static float cornering_peak(const m2_steering_tire *t, float bound) {
    if (t->longitudinal_slip>=1.f) return NAN; // locked/reversed rolling direction
    unsigned best=0;
    float maximum=0.f;
    for (unsigned i=1;i<=128;++i) {
        const float force=predicted_cornering_force(t,bound*i/128.f);
        if (force>maximum) { maximum=force; best=i; }
    }
    float lo=bound*(best ? best-1 : 0)/128.f, hi=bound*fminf(best+1,128)/128.f;
    for (unsigned i=0;i<40;++i) {
        const float a=lo+(hi-lo)/3.f, z=hi-(hi-lo)/3.f;
        if (predicted_cornering_force(t,a)<predicted_cornering_force(t,z)) lo=a;
        else hi=z;
    }
    const float candidate=(lo+hi)*.5f;
    return predicted_cornering_force(t,bound)>=predicted_cornering_force(t,candidate) ? bound : candidate;
}

static int slip_analysis_probe(Bench *b, float fraction, FILE *out) {
    enum { CFG=0x230000, ENGINE=0x231000, TRANS=0x232000, FRONT=0x233000, REAR=0x234000 };
    const float radians=3.14159265358979323846f/180.f;
    const float angles[]={0.f,1.f,2.f,3.f,5.f,7.5f,10.f,15.f,20.f,30.f,45.f,60.f};
    const float kappas[]={-1.f,-.5f,-.15f,0.f,.15f,.5f};
    PPCContext *c=b->cpu;
    GuestFn model=recomp_lookup(0x116d10);
    const uint32_t old_template=b->tire_template;
    unsigned cases=0, peaks=0, plateaus=0, locked_cases=0, reached=0;
    int failures=0;
    if (!model || !recomp_lookup(0x14a1c8) || !recomp_lookup(0x13d288)) return 1;
    if (out) fputs("case,brand,axle,temperature,wear,load_native,speed_fps,long_slip,static_mu,sliding_mu,stiffness,half_patch,kind,peak_slip_deg,max_force_native,reference_fraction,reference_slip_deg,reference_reached,slip_deg,lateral_force_native,lateral_force_model,settled,cornering_peak_deg,cornering_peak_force,cornering_peak_at_bound,cornering_force_native,cornering_force_model\n",out);
    for (unsigned brand=0;brand<4;++brand) {
        wr32(c,STACK+8,brand);
        c->gpr[3]=CFG; c->gpr[4]=ENGINE; c->gpr[5]=TRANS;
        c->gpr[6]=FRONT; c->gpr[7]=REAR; c->gpr[8]=0; c->gpr[9]=0; c->gpr[10]=0;
        enter(b,model);
        const uint32_t templates[]={rd32(c,FRONT),rd32(c,REAR)};
        for (unsigned axle=0;axle<2;++axle) {
            b->tire_template=templates[axle];
            if (!span(b->tire_template,TIRE_BYTES)) { ++failures; continue; }
            unsigned char original[TIRE_BYTES];
            memcpy(original,c->mem+b->tire_template,TIRE_BYTES);
            for (unsigned load=0;load<3;++load) for (unsigned k=0;k<6;++k) {
                Request q={cases++,0,0,100.f,5.f,kappas[k],300.f*(1u<<load),1.f};
                const Result contact=sample(b,&q);
                m2_steering_tire tire={contact.contact_load,contact.half_patch,
                    rdf32(c,TIRE+0x234),rdf32(c,TIRE+0x238),-rdf32(c,TIRE+0x23c),
                    fmaxf(fabsf(1.f+q.kappa),.001f/q.speed),-q.kappa};
                m2_steering_slip_analysis a;
                const int locked=q.kappa<=-1.f;
                const int analyzed=m2_steering_analyze_slip(&tire,fraction,60.f*radians,&a);
                if (!contact.settled || analyzed==locked) {
                    fprintf(stderr,"slip analysis: invalid brand=%u axle=%u load=%g temperature=%g wear=%g kappa=%g\n",
                            brand,axle,q.load,b->temperature,b->wear,q.kappa);
                    ++failures; continue;
                }
                peaks+=a.kind==M2_SLIP_FINITE_PEAK;
                plateaus+=a.kind==M2_SLIP_ASYMPTOTIC;
                locked_cases+=locked;
                reached+=isfinite(a.reference_slip);
                const float projected_peak=cornering_peak(&tire,60.f*radians);
                m2_steering_cornering live;
                const int live_valid=m2_steering_cornering_optimum(&tire,60.f*radians,&live);
                if (live_valid==locked || (!locked && fabsf(live.slip-projected_peak)>.001f)) ++failures;
                const float projected_max=locked ? 0.f : predicted_cornering_force(&tire,projected_peak);
                for (unsigned point=0;point<17;++point) {
                    if (point==12 && !isfinite(a.reference_slip)) continue;
                    if (point==13 && !isfinite(a.peak_slip)) continue;
                    if (point>=14 && !isfinite(projected_peak)) continue;
                    q.angle=point<12 ? angles[point] : point==12 ? a.reference_slip/radians : point==13 ? a.peak_slip/radians :
                        projected_peak/radians*(point==14 ? 1.f : point==15 ? .9f : 1.1f);
                    if (q.angle>60.f) continue;
                    const Result measured=sample(b,&q);
                    // Locked-wheel force magnitudes differ from the continuous
                    // reduction; the cause is unresolved. Do not claim that our
                    // continuous reduction predicts that branch: verify only
                    // its vanishing travel-normal force, and report no optimum.
                    const float predicted=locked ? NAN : m2_steering_lateral_force(&tire,q.angle*radians);
                    const float tolerance=.02f+.002f*fabsf(measured.lateral);
                    const float projected=native_cornering_force(b,q.angle*radians);
                    const float projected_prediction=predicted_cornering_force(&tire,q.angle*radians);
                    if (!measured.settled || !isfinite(projected) ||
                        (!locked && (!isfinite(predicted) || fabsf(measured.lateral-predicted)>tolerance ||
                         measured.lateral>a.max_force+tolerance ||
                         (point==12 && fabsf(measured.lateral-fraction*a.max_force)>tolerance) ||
                         (point==13 && fabsf(measured.lateral-a.max_force)>tolerance))) ||
                        fabsf(projected-projected_prediction)>tolerance ||
                        (isfinite(projected_peak) && projected>projected_max+tolerance) ||
                        (point==14 && fabsf(projected-projected_max)>tolerance)) {
                        if (failures<16) fprintf(stderr,"slip analysis: mismatch brand=%u axle=%u load=%g temperature=%g wear=%g kappa=%g angle=%g native=%g model=%g max=%g projected=%g expected=%g\n",
                                brand,axle,q.load,b->temperature,b->wear,q.kappa,q.angle,measured.lateral,predicted,a.max_force,projected,projected_prediction);
                        ++failures;
                    }
                    if (out) fprintf(out,"%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%d,%.9g,%.9g\n",
                        q.id,brand,axle,b->temperature,b->wear,q.load,q.speed,q.kappa,
                        tire.static_mu,tire.sliding_mu,tire.stiffness,tire.half_patch,
                        locked ? "locked" : a.kind==M2_SLIP_FINITE_PEAK ? "peak" : "plateau",a.peak_slip/radians,a.max_force,
                        fraction,a.reference_slip/radians,isfinite(a.reference_slip)!=0,q.angle,
                        measured.lateral,predicted,measured.settled,projected_peak/radians,projected_max,
                        isfinite(projected_peak) && projected_peak>=60.f*radians,projected,projected_prediction);
                }
            }
            if (memcmp(original,c->mem+b->tire_template,TIRE_BYTES)) ++failures;
        }
    }
    b->tire_template=old_template;
    fprintf(stderr,"slip analysis: %u cases, %u finite peaks, %u asymptotes, %u locked, %u references within 60 deg, %d failures (temperature=%g wear=%g fraction=%g)\n",
            cases,peaks,plateaus,locked_cases,reached,failures,b->temperature,b->wear,fraction);
    return failures;
}

static int self_test(Bench *b) {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "tire bench: FAIL %s\n", #x); ++failures; } } while (0)
    Request q = {0, 0, 0, 100.f, 5.f, 0.f, 600.f, 1.f};
    unsigned cfg = rd32(b->cpu, g_recomp_data_base + 0x5d78u);
    unsigned source = rd32(b->cpu, rd32(b->cpu, cfg + 0x58));
    unsigned char original[TIRE_BYTES];
    memcpy(original, b->cpu->mem + source, sizeof(original));
    Result positive = sample(b, &q), repeat = sample(b, &q);
    CHECK(positive.settled && positive.lateral > 0.f);
    CHECK(positive.lateral == repeat.lateral && positive.steps == repeat.steps);
    CHECK(fabsf(positive.longitudinal) < 0.001f);
    q.angle = -5.f;
    Result negative = sample(b, &q);
    CHECK(negative.settled && fabsf(negative.lateral + positive.lateral) < 0.01f);
    q.angle = 0.f;
    Result zero = sample(b, &q);
    CHECK(zero.settled && zero.lateral == 0.f && zero.longitudinal == 0.f);
    CHECK(zero.normal == 0.f && zero.contact_load > 0.f);
    q.kappa = -0.15f;
    Result brake = sample(b, &q);
    CHECK(brake.settled && brake.longitudinal < 0.f);
    q.kappa = 0.15f;
    Result drive = sample(b, &q);
    CHECK(drive.settled && drive.longitudinal > 0.f);
    q.angle = 5.f;
    unsigned max_steps = b->max_steps;
    b->max_steps = 1;
    Result short_run = sample(b, &q);
    CHECK(!short_run.settled && !strcmp(short_run.status, "unsettled"));
    b->max_steps = max_steps;
    q.speed = 0.00001f;
    q.kappa = 0.f;
    Result slow = sample(b, &q);
    CHECK(!slow.settled && !strcmp(slow.status, "unsettled"));
    q.load = 1.e9f;
    Result impossible = sample(b, &q);
    CHECK(!strcmp(impossible.status, "load_unreachable"));
    patch(b, rdf32(b->cpu, TIRE), 1.f, 100.f, 100.f);
    CHECK(rdf32(b->cpu, FORCE) == 0.f && rdf32(b->cpu, FORCE + 4) == 0.f);
    CHECK(!memcmp(original, b->cpu->mem + source, sizeof(original)));
    // Compare the estimator's reduction against ORIGINAL patchforce, across
    // the complete car/corner table and both braking and driven wheel slip.
    for (unsigned car = 0; car < 4; ++car) for (unsigned corner = 0; corner < 4; ++corner)
    for (unsigned load = 0; load < 3; ++load) for (int angle = -15; angle <= 15; angle += 5)
    for (int slip = -1; slip <= 1; ++slip) {
        q = (Request){0, car, corner, 100.f, (float)angle, slip * .15f, 300.f * (1u << load), 1.f};
        const Result measured = sample(b, &q);
        m2_steering_tire t = {
            measured.contact_load, measured.half_patch,
            rdf32(b->cpu, TIRE + 0x234), rdf32(b->cpu, TIRE + 0x238),
            -rdf32(b->cpu, TIRE + 0x23c), 1.f + q.kappa, -q.kappa,
        };
        const float predicted = m2_steering_lateral_force(&t, q.angle * (3.14159265358979323846f / 180.f));
        if (!measured.settled || !isfinite(predicted) || fabsf(predicted - measured.lateral) > .02f + .002f * fabsf(measured.lateral)) {
            fprintf(stderr, "force differential car=%u corner=%u load=%g angle=%g kappa=%g native=%g estimator=%g\n",
                    car, corner, q.load, q.angle, q.kappa, measured.lateral, predicted);
            ++failures;
        }
        // Rear reserve uses combined force magnitude, including near-zero
        // lateral slip under braking. Compare its radial equivalent directly
        // with both ORIGINAL patchforce components, not just our lateral math.
        const float alpha=q.angle*(3.14159265358979323846f/180.f);
        const float equivalent=atanf(hypotf(tanf(alpha),t.longitudinal_slip));
        t.longitudinal_slip=0.f;
        const float combined=m2_steering_lateral_force(&t,equivalent);
        const float native_combined=hypotf(measured.lateral,measured.longitudinal);
        if (!isfinite(combined) || fabsf(combined-native_combined)>.02f+.002f*native_combined) {
            fprintf(stderr,"combined force car=%u corner=%u angle=%g kappa=%g native=%g estimate=%g\n",
                    car,corner,q.angle,q.kappa,native_combined,combined);
            ++failures;
        }
    }
    failures += steering_test(b);
    failures += tire_brand_steering_test(b);
    const float temperature=b->temperature, wear=b->wear;
    const float temperatures[]={80.f,180.f,260.f}, wears[]={.3f,1.f};
    for (unsigned t=0;t<3;++t) for (unsigned w=0;w<2;++w) {
        b->temperature=temperatures[t]; b->wear=wears[w];
        failures+=slip_analysis_probe(b,.99f,NULL);
    }
    b->temperature=temperature; b->wear=wear;
#undef CHECK
    fprintf(stderr, "tire bench: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}

static int number(const char *text, float *out, float lo, float hi) {
    char *end;
    errno = 0;
    float value = strtof(text, &end);
    if (errno || end == text || *end || !isfinite(value) || value < lo || value > hi)
        return 0;
    *out = value;
    return 1;
}

int main(int argc, char **argv) {
    Bench b = {0};
    b.dt = 1.f / 60.f;
    b.temperature = 180.f; // explicit frozen native temperature, not ambient
    b.wear = 1.f;
    b.max_steps = 512;
    const char *output = NULL, *analysis_output = NULL;
    float reference_fraction=.99f;
    int test = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--self-test")) { test = 1; continue; }
        if (!strcmp(argv[i], "--help")) {
            puts("m2_tire_probe [--game-data DIR] [--self-test | --output CSV | --analyze-slip CSV]\n"
                 "  [--max-steps 16..4096] [--dt 0.0001..0.1]\n"
                 "  [--temperature 0..1000] [--wear 0..1] [--reference-fraction 0.0001..1]\n"
                 "stdin: id car(0..3) corner(0..1) speed_fps slip_deg kappa load_native grip_scale");
            return 0;
        }
        if (i + 1 >= argc) { fprintf(stderr, "missing option value\n"); return 2; }
        const char *key = argv[i++], *value = argv[i];
        float n;
        if (!strcmp(key, "--game-data")) m2_recomp_set_game_root(value);
        else if (!strcmp(key, "--output")) output = value;
        else if (!strcmp(key, "--analyze-slip")) analysis_output = value;
        else if (!strcmp(key, "--reference-fraction") && number(value,&n,.0001f,1.f)) reference_fraction=n;
        else if (!strcmp(key, "--dt") && number(value, &n, 0.0001f, 0.1f)) b.dt = n;
        else if (!strcmp(key, "--temperature") && number(value, &n, 0.f, 1000.f)) b.temperature = n;
        else if (!strcmp(key, "--wear") && number(value, &n, 0.f, 1.f)) b.wear = n;
        else if (!strcmp(key, "--max-steps") && number(value, &n, 16.f, MAX_STEPS) && floorf(n) == n)
            b.max_steps = (unsigned)n;
        else { fprintf(stderr, "invalid option/value: %s %s\n", key, value); return 2; }
    }
    if (test+(output!=NULL)+(analysis_output!=NULL)!=1) {
        fprintf(stderr, "choose --self-test, --output CSV, or --analyze-slip CSV\n");
        return 2;
    }
    const char *root = m2_recomp_get_game_root();
    char image_path[2048];
    if (!root || !*root) { fprintf(stderr, "tire bench: game data unavailable\n"); return 77; }
    if (snprintf(image_path, sizeof(image_path), "%s/driving/game", root) >= (int)sizeof(image_path)) {
        fprintf(stderr, "game data path is too long\n");
        return 2;
    }
    FILE *image = m2_fopen_utf8(image_path, "rb");
    if (!image) {
        const int missing = errno == ENOENT || errno == ENOTDIR;
        perror(image_path);
        return missing ? 77 : 1;
    }
    fclose(image);
    b.cpu = m2_recomp_context(); // verified image loader; never boots a guest task
    m2_kernel_dispatch conversions = {0};
    conversions.itof = m2___itof;
    conversions.utof = m2___utof;
    m2_kernel_dispatch_set_global(&conversions);
    b.setup = recomp_lookup(0x140414u);
    b.patch = recomp_lookup(0x140aa8u);
    if (!b.setup || !b.patch) { fprintf(stderr, "missing tire dispatch entries\n"); return 1; }
    if (test) return self_test(&b);
    if (analysis_output) {
        FILE *out=m2_fopen_utf8(analysis_output,"wx");
        if (!out) { perror(analysis_output); return 2; }
        int failed=slip_analysis_probe(&b,reference_fraction,out)!=0;
        if (fflush(out) || ferror(out)) failed=1;
        if (fclose(out)) failed=1;
        return failed;
    }
    FILE *out = m2_fopen_utf8(output, "wx");
    if (!out) { perror(output); return 2; }
    fputs("id,car,corner,speed_fps,slip_deg,long_slip,load_native,grip_scale,"
          "lateral_force_native,normal_force_native,longitudinal_force_native,"
          "contact_load_native,steps,settled,residual_native,status\n", out);
    char line[512];
    unsigned count = 0;
    int failed = 0;
    while (fgets(line, sizeof(line), stdin)) {
        Request q;
        char extra;
        if (++count > MAX_CASES || sscanf(line, "%u %u %u %f %f %f %f %f %c",
            &q.id, &q.car, &q.corner, &q.speed, &q.angle, &q.kappa, &q.load, &q.grip, &extra) != 8 ||
            q.id >= MAX_CASES || q.car > 3 || q.corner > 1 || !isfinite(q.speed) || q.speed < 0 || q.speed > 1000 ||
            !isfinite(q.angle) || fabsf(q.angle) > 60 || !isfinite(q.kappa) || q.kappa < -1 || q.kappa > 1 ||
            !isfinite(q.load) || q.load <= 0 || q.load > 100000 ||
            !isfinite(q.grip) || q.grip <= 0 || q.grip > 10) {
            fprintf(stderr, "invalid request at line %u\n", count);
            failed = 1;
            break;
        }
        Result r = sample(&b, &q);
        fprintf(out, "%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%d,%.9g,%s\n",
                q.id, q.car, q.corner, q.speed, q.angle, q.kappa, q.load, q.grip,
                r.lateral, r.normal, r.longitudinal, r.contact_load, r.steps, r.settled, r.residual, r.status);
        if (!r.settled) failed = 1;
    }
    if (!count || ferror(stdin)) failed = 1;
    if (fflush(out) || ferror(out)) failed = 1;
    if (fclose(out)) failed = 1;
    fprintf(stderr, "tire bench: %u requests; %s\n", count, failed ? "incomplete/failed samples" : "OK");
    return failed;
}
