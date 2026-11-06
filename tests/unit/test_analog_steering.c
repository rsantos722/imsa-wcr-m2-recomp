// Authored fixtures; native-force differential coverage lives in tire_probe.
#include "analog_steering.h"
#include "ppc_ctx.h"
#include <stdio.h>
#include <stdlib.h>

// The no-disc runtime omits MMIO and memory-event delivery.
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t a) { (void)a; return 0; }
__attribute__((weak)) void m2_oob_shadow_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t a, uint32_t n, uint32_t pc) { (void)a; (void)n; (void)pc; }
__attribute__((weak)) void m2_guest_event_c(const M2GuestEvent *e) { (void)e; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void test_slip_analysis(void) {
    const float radians=3.14159265358979323846f/180.f, bound=60.f*radians;
    m2_steering_tire t={600.f,.2f,2.f,2.f,281250.f,1.f,0.f};
    m2_steering_slip_analysis a;
    CHECK(m2_steering_analyze_slip(&t,.99f,bound,&a));
    CHECK(a.kind==M2_SLIP_ASYMPTOTIC && isnan(a.peak_slip));
    CHECK(fabsf(a.max_force-1200.f)<.001f);
    CHECK(fabsf(a.reference_slip/radians-12.8981f)<.001f);
    CHECK(fabsf(m2_steering_lateral_force(&t,a.reference_slip)/a.max_force-.99f)<.00001f);
    CHECK(m2_steering_analyze_slip(&t,1.f,bound,&a) && isnan(a.reference_slip));
    CHECK(m2_steering_analyze_slip(&t,.99f,5.f*radians,&a) && isnan(a.reference_slip));
    t.sliding_mu=1.f;
    CHECK(m2_steering_analyze_slip(&t,1.f,bound,&a));
    CHECK(a.kind==M2_SLIP_FINITE_PEAK && a.peak_slip==a.reference_slip);
    CHECK(fabsf(a.peak_slip/radians-3.88477f)<.001f && a.max_force>600.f);
    CHECK(m2_steering_lateral_force(&t,a.peak_slip*.9f)<a.max_force);
    CHECK(m2_steering_lateral_force(&t,a.peak_slip*1.1f)<a.max_force);
    // Combined slip can leave a local peak below the eventual plateau.
    // The earliest near-plateau crossing can be before that peak or after it.
    t.longitudinal_slip=.05f; t.rolling_ratio=1.05f;
    CHECK(m2_steering_analyze_slip(&t,.99f,bound,&a));
    CHECK(a.kind==M2_SLIP_ASYMPTOTIC && a.reference_slip<10.f*radians);
    CHECK(m2_steering_analyze_slip(&t,.998f,bound,&a));
    CHECK(a.kind==M2_SLIP_ASYMPTOTIC && a.reference_slip>10.f*radians);
    // Differential grid uses the original force helper, independently of the
    // derivative/bracketing calculation, including both sides of r=.75.
    const float ratios[]={.2f,.5f,.7f,.749f,.75f,1.f,1.3f};
    const float slips[]={0.f,.01f,.05f,.15f,.5f};
    const float fractions[]={.9f,.99f,1.f};
    for (unsigned r=0;r<7;++r) for (unsigned k=0;k<5;++k)
    for (unsigned rho=0;rho<3;++rho) for (unsigned fraction=0;fraction<3;++fraction) {
        t.sliding_mu=t.static_mu*ratios[r];
        t.longitudinal_slip=slips[k]; t.rolling_ratio=.5f+.5f*rho;
        CHECK(m2_steering_analyze_slip(&t,fractions[fraction],bound,&a));
        CHECK(a.kind!=M2_SLIP_INVALID && isfinite(a.max_force));
        if (ratios[r]>=.75f) CHECK(a.kind==M2_SLIP_ASYMPTOTIC && isnan(a.peak_slip));
        if (isfinite(a.reference_slip)) {
            CHECK(a.reference_slip>0.f && a.reference_slip<=bound);
            CHECK(fabsf(m2_steering_lateral_force(&t,a.reference_slip)/a.max_force-fractions[fraction])<.00001f);
        }
        for (unsigned i=0;i<=320;++i) {
            const float angle=i*.25f*radians;
            const float force=m2_steering_lateral_force(&t,angle);
            CHECK(force<=a.max_force+.00002f*a.max_force);
            if (angle<=bound && (!isfinite(a.reference_slip) || angle<a.reference_slip-.00001f))
                CHECK(force<=fractions[fraction]*a.max_force+.00002f*a.max_force);
        }
        if (a.kind==M2_SLIP_FINITE_PEAK)
            CHECK(fabsf(m2_steering_lateral_force(&t,a.peak_slip)-a.max_force)<.00002f*a.max_force);
    }
    t=(m2_steering_tire){600.f,.2f,2.f,2.f,281250.f,1.f,0.f};
    CHECK(!m2_steering_analyze_slip(NULL,.99f,bound,&a) && a.kind==M2_SLIP_INVALID);
    CHECK(!m2_steering_analyze_slip(&t,0.f,bound,&a));
    CHECK(!m2_steering_analyze_slip(&t,1.01f,bound,&a));
    CHECK(!m2_steering_analyze_slip(&t,NAN,bound,&a));
    CHECK(!m2_steering_analyze_slip(&t,.99f,0.f,&a));
    CHECK(!m2_steering_analyze_slip(&t,.99f,NAN,&a));
    CHECK(!m2_steering_analyze_slip(&t,.99f,90.f*radians,&a));
    CHECK(!m2_steering_analyze_slip(&t,.99f,bound,NULL));
    t.longitudinal_slip=1.f;
    CHECK(!m2_steering_analyze_slip(&t,.99f,bound,&a) && a.kind==M2_SLIP_INVALID);
    t.longitudinal_slip=0.f;
    t.load=0.f;
    CHECK(!m2_steering_analyze_slip(&t,.99f,bound,&a) && isnan(a.reference_slip));
}
enum { CAR=0x200000, BODY=0x202000, CFG=0x203000, TIRE=0x204000,
       WHEEL=0x208000, TRAVEL=0x20a000, ROTATION=0x20a020 };
static const float pi=3.14159265358979323846f;
static float settled(PPCContext *c,float input) {
    float value=0.f;
    for (unsigned i=0;i<120;++i) value=m2_analog_steering_command(c,input);
    return value;
}
static m2_steering_snapshot snapshot(void) {
    m2_steering_snapshot out;m2_analog_steering_get_snapshot(&out);return out;
}
static void fixture(PPCContext *c,float speed) {
    memset(c->mem+CAR,0,0xb000);
    wr32(c,CAR,BODY);wr32(c,CAR+0x428,CFG);
    wrf32(c,CFG+0x98,.8f);
    for (unsigned i=0;i<5;++i) wrf32(c,CFG+0x9c+4*i,1.f);
    for (unsigned i=0;i<4;++i) {
        wr32(c,CAR+4+4*i,WHEEL+0x400*i);wr32(c,CAR+0x14+4*i,TIRE+0x800*i);
        wr32(c,TIRE+0x800*i+0x1e0,CAR);
        wrf32(c,TIRE+0x800*i+0x234,2.f);wrf32(c,TIRE+0x800*i+0x238,2.f);
        wrf32(c,TIRE+0x800*i+0x23c,-281250.f);
    }
    wrf32(c,BODY+8,speed);wrf32(c,BODY+0x1a4,1.f/60.f);
    m2_analog_steering_set_enabled(1);m2_analog_steering_select_car(c,CAR);
}
static void observe(PPCContext *c,unsigned corner,float steer,float flow,float kappa,float load) {
    const unsigned tire=TIRE+0x800*corner,wheel=WHEEL+0x400*corner;
    wrf32(c,wheel+0x1f0,steer);
    wrf32(c,TRAVEL,100.f*tanf(steer-flow));wrf32(c,TRAVEL+8,100.f);
    wrf32(c,ROTATION+8,-100.f*(1.f+kappa));
    c->gpr[31]=tire;c->gpr[28]=TRAVEL;c->gpr[27]=ROTATION;
    c->fpr[27]=load;c->fpr[28]=.2f;
    const PPCContext before=*c;
    unsigned char memory[0xb000];memcpy(memory,c->mem+CAR,sizeof(memory));
    m2_analog_steering_observe(c);
    CHECK(!memcmp(&before,c,sizeof(before)) && !memcmp(memory,c->mem+CAR,sizeof(memory)));
}
static void front(PPCContext *c,float flow,float kappa) {
    observe(c,0,0.f,flow,kappa,600.f);observe(c,1,0.f,flow,kappa,600.f);
}
static void test_optimum(void) {
    m2_steering_tire t={600.f,.2f,2.f,2.f,281250.f,1.f,0.f};
    m2_steering_cornering peak,brake,scaled;
    CHECK(m2_steering_cornering_optimum(&t,pi/3.f,&peak));
    CHECK(peak.slip>11.f*pi/180.f && peak.slip<12.f*pi/180.f && !peak.at_bound);
    for (unsigned i=0;i<=600;++i) {
        const float a=i*(pi/1800.f);
        const float independent=m2_steering_lateral_force(&t,a)*cosf(a);
        CHECK(independent<=peak.force+.002f);
    }
    t.static_mu*=2.f;t.sliding_mu*=2.f;t.stiffness*=2.f;
    CHECK(m2_steering_cornering_optimum(&t,pi/3.f,&scaled));
    CHECK(fabsf(scaled.slip-peak.slip)<.0001f && fabsf(scaled.force-2.f*peak.force)<.01f);
    t.static_mu/=2.f;t.sliding_mu/=2.f;t.stiffness/=2.f;
    t.longitudinal_slip=.15f;t.rolling_ratio=.85f;
    CHECK(m2_steering_cornering_optimum(&t,pi/3.f,&brake));
    CHECK(brake.slip>peak.slip && brake.force<peak.force);
    CHECK(m2_steering_cornering_optimum(&t,pi/180.f,&brake) && brake.at_bound);
    t.longitudinal_slip=1.f;
    CHECK(!m2_steering_cornering_optimum(&t,pi/3.f,&brake) && isnan(brake.slip));
    CHECK(isnan(m2_steering_cornering_force(&t,.1f)));
    t.longitudinal_slip=0.f;
    CHECK(!m2_steering_cornering_optimum(&t,0.f,&brake));
    CHECK(!m2_steering_cornering_optimum(NULL,pi/3.f,&brake));
    CHECK(!m2_steering_cornering_optimum(&t,pi/3.f,NULL));
}
static void test_live(PPCContext *c) {
    const float speeds[]={0.f,20.f,60.f,150.f,300.f,600.f,1000.f};
    const float travels[]={.02f,.1f,.5f,1.f};
    const m2_steering_tire tire={600.f,.2f,2.f,2.f,281250.f,1.f,0.f};
    m2_steering_cornering peak;m2_steering_cornering_optimum(&tire,pi/3.f,&peak);
    for (unsigned v=0;v<7;++v) for (int side=-1;side<=1;side+=2)
    for (unsigned travel=0;travel<4;++travel) {
        fixture(c,speeds[v]);front(c,0.f,0.f);
        const float out=side*.8f*settled(c,side*travels[travel]);
        const m2_steering_snapshot live=snapshot();
        CHECK(out>0.f && out<=.8f && !live.fallback && (live.contact_mask&3)==3);
        CHECK(fabsf(live.front_force-2.f*peak.force)<.01f);
        if (v>=2) {
            const float requested=powf(travels[travel],1.6f);
            CHECK(fabsf(m2_steering_cornering_force(&tire,out)/peak.force-requested)<.0001f);
            if (travel==3) CHECK(fabsf(out-peak.slip)<.0001f);
        }
        if (!v) CHECK(fabsf(out-.8f*powf(travels[travel],1.6f))<.00002f);
    }
    // A straight-moving mounted tire has positive lateral contact travel for
    // positive steer. Its own steer must cancel out of the recovered flow.
    fixture(c,150.f);
    observe(c,0,.2f,0.f,0.f,600.f);observe(c,1,.2f,0.f,0.f,600.f);
    CHECK(fabsf(.8f*settled(c,1.f)-peak.slip)<.0001f);
    // Changes to live stiffness and longitudinal slip change the calculation.
    fixture(c,150.f);front(c,0.f,0.f);const float original=.8f*settled(c,1.f);
    for (unsigned i=0;i<2;++i) wrf32(c,TIRE+0x800*i+0x23c,-562500.f);
    front(c,0.f,0.f);CHECK(.8f*settled(c,1.f)<original);
    const float coast_force=snapshot().front_force;
    front(c,0.f,-.15f);settled(c,1.f);
    CHECK(snapshot().front_force<coast_force);
    CHECK(snapshot().front_slip>original*.8f);
}
static void test_intent_and_release(PPCContext *c) {
    for (int side=-1;side<=1;side+=2) {
        fixture(c,150.f);front(c,0.f,0.f);
        for (unsigned i=1;i<=6;++i) {
            m2_analog_steering_command(c,(float)side);
            CHECK(fabsf(side*snapshot().intent-(1.f-expf(-(float)i/(60.f*.10f))))<.00001f);
        }
        const float held=.8f*settled(c,(float)side);
        for (unsigned i=1;i<=3;++i) {
            const float angle=.8f*m2_analog_steering_command(c,0.f);
            CHECK(fabsf(angle-held*expf(-(float)i/(60.f*.014f)))<.00001f);
        }
        float previous=0.f;
        for (unsigned i=0;i<120;++i) previous=m2_analog_steering_command(c,-(float)side);
        CHECK(previous*side<0.f);
        m2_analog_steering_release();
        float first=0.f;
        for (unsigned pulse=0;pulse<8;++pulse) {
            m2_analog_steering_command(c,(float)side);
            const float peak=side*m2_analog_steering_command(c,(float)side);
            if (!pulse) first=peak;
            CHECK(peak>0.f && peak<first*1.05f);
            for (unsigned i=0;i<5;++i) m2_analog_steering_command(c,0.f);
        }
    }
    float samples[2];
    for (unsigned hz=60;hz<=120;hz+=60) {
        fixture(c,150.f);front(c,0.f,0.f);wrf32(c,BODY+0x1a4,1.f/hz);
        for (unsigned i=0;i<hz/10;++i) m2_analog_steering_command(c,1.f);
        samples[hz/60-1]=snapshot().angle;
    }
    CHECK(fabsf(samples[0]-samples[1])<.00001f);
    // Six native body substeps still constitute one 60 Hz input interval.
    fixture(c,150.f);front(c,0.f,0.f);
    wr32(c,BODY+0x1fc,0x20a100);
    wr32(c,0x20a100+0x1c,6);wrf32(c,0x20a100+0x10,1.f/360.f);
    wrf32(c,BODY+0x1a4,1.f/360.f);
    for (unsigned i=0;i<6;++i) m2_analog_steering_command(c,1.f);
    CHECK(fabsf(snapshot().angle-samples[0])<.00001f);
}
static void test_curve_refresh(PPCContext *c) {
    fixture(c,150.f);
    // Compare the live cached path with the public uncached solver while
    // changing every observed parameter, then recover from locked fronts.
    for (unsigned sample=0;sample<10;++sample) {
        m2_steering_tire t={512.f,.2f,2.f,2.f,281250.f,1.f,0.f};
        float kappa=0.f;
        if (sample==1) t.load=1024.f;
        if (sample==2) t.half_patch=.15f;
        if (sample==3) t.static_mu=1.8f;
        if (sample==4) t.sliding_mu=1.1f;
        if (sample==5) t.stiffness=500000.f;
        if (sample==6) kappa=.15f;
        if (sample==7) kappa=-.15f;
        if (sample==8) kappa=-1.f;
        m2_analog_steering_select_car(c,CAR);
        for (unsigned corner=0;corner<4;++corner) {
            const unsigned tire=TIRE+0x800*corner;
            wrf32(c,tire+0x234,t.static_mu);wrf32(c,tire+0x238,t.sliding_mu);
            wrf32(c,tire+0x23c,-t.stiffness);
            observe(c,corner,0.f,0.f,kappa,t.load);
            c->fpr[28]=t.half_patch;m2_analog_steering_observe(c);
        }
        const float rolling=-rdf32(c,ROTATION+8);
        t.rolling_ratio=fmaxf(fabsf(rolling),.001f)/100.f;
        t.longitudinal_slip=(100.f-rolling)/100.f;
        m2_steering_cornering peak;
        const int valid=m2_steering_cornering_optimum(&t,pi/3.f,&peak);
        settled(c,1.f);
        CHECK(snapshot().fallback==!valid);
        if (valid) {
            CHECK(fabsf(snapshot().angle-peak.slip)<.000001f);
            CHECK(snapshot().front_force==2.f*peak.force);
            settled(c,.5f);
            CHECK(fabsf(m2_steering_cornering_force(&t,snapshot().angle)/peak.force-
                         powf(.5f,1.6f))<.00001f);
        }
    }
}
static void test_recovery_and_observations(PPCContext *c) {
    for (int side=-1;side<=1;side+=2) {
        fixture(c,300.f);front(c,0.f,0.f);
        const float ordinary=side*settled(c,(float)side);
        // Opposing front travel never clamps manual steering to zero.
        front(c,-side*.6f,0.f);
        CHECK(fabsf(side*settled(c,(float)side)-ordinary)<.00001f);
        front(c,side*.2f,0.f);
        CHECK(side*settled(c,(float)side)>ordinary);
        front(c,0.f,0.f);
        // Rear saturation cannot reduce the ordinary steering target.
        observe(c,2,0.f,-side*.4f,0.f,600.f);observe(c,3,0.f,-side*.4f,0.f,600.f);
        CHECK(fabsf(side*settled(c,(float)side)-ordinary)<.00001f);
        // Steering toward saturated rear travel opens manual alignment.
        observe(c,2,0.f,side*.4f,0.f,600.f);observe(c,3,0.f,side*.4f,0.f,600.f);
        CHECK(side*settled(c,(float)side)>ordinary);
        CHECK(snapshot().recovery_flow>0.f);
    }
    fixture(c,300.f);front(c,0.f,0.f);
    const float yaws[]={0.f,-.049f,-.051f,-.5f,.5f,NAN};
    float first=0.f;
    for (unsigned i=0;i<6;++i) {
        m2_analog_steering_release();wrf32(c,BODY+0x70,yaws[i]);
        const float angle=m2_analog_steering_command(c,1.f);
        if (!i) first=angle;
        CHECK(fabsf(angle-first)<.00001f);
    }
    // Contacts expire rather than becoming persistent invented grip data.
    m2_analog_steering_select_car(c,CAR);m2_analog_steering_select_car(c,CAR);
    settled(c,.5f);CHECK(snapshot().fallback && snapshot().contact_mask==0);
    CHECK(fabsf(snapshot().angle-.8f*powf(.5f,1.6f))<.00002f);
    front(c,0.f,0.f);settled(c,.5f);CHECK(!snapshot().fallback);
    wr32(c,TIRE+0x1e0,CAR+4);wr32(c,TIRE+0x800+0x1e0,CAR+4);
    m2_analog_steering_select_car(c,CAR);m2_analog_steering_select_car(c,CAR);
    front(c,0.f,0.f);settled(c,.5f);CHECK(snapshot().fallback);
}
static void test_native_curve(PPCContext *c) {
    float reference[8];
    for (unsigned curve=0;curve<2;++curve) {
        fixture(c,150.f);front(c,0.f,0.f);
        if (curve) for (unsigned i=0;i<5;++i) wrf32(c,CFG+0x9c+4*i,6.f-i);
        for (unsigned i=0;i<8;++i) {
            const float u=m2_analog_steering_command(c,.5f);
            const float angle=curve ? .8f*u/(6.f-4.f*fabsf(u)) : .8f*u;
            if (!curve) reference[i]=angle;
            else CHECK(fabsf(angle-reference[i])<.00001f);
        }
    }
    wrf32(c,CFG+0x9c,NAN);
    CHECK(m2_analog_steering_command(c,.7f)==.7f);
    CHECK(isfinite(m2_analog_steering_command(c,NAN)));
}
int main(void) {
    test_slip_analysis();test_optimum();
    PPCContext c={0};c.mem=calloc(1,RECOMP_GUEST_MEM);if (!c.mem) return 1;
    m2_analog_steering_set_enabled(0);
    CHECK(m2_analog_steering_command(&c,-.7f)==-.7f);
    test_live(&c);test_intent_and_release(&c);test_curve_refresh(&c);
    test_recovery_and_observations(&c);test_native_curve(&c);
    m2_analog_steering_set_enabled(0);free(c.mem);
    return failures!=0;
}
