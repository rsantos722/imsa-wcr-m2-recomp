// Bounded native full-car fixture on the guest's fallback horizontal plane.
// No boot, renderer, track assets, powertrain input, or real saves. Constructors,
// suspension, four tire callbacks, Ackermann, aero and Newton integration are
// original guest functions. ProcessInput and the live observer are production.
#include "recomp_host.h"
#include "analog_steering.h"
#include "control_modes.h"
#include "recomp_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*GuestFn)(PPCContext *);
extern GuestFn recomp_lookup(unsigned guest);
void *m2_selector_lookup(uint32_t folio, uint32_t ordinal) {
    fprintf(stderr,"vehicle fixture: unexpected selector %u:%u\n",folio,ordinal);
    abort();
}
enum { STACK=0x210000, RECORD=0x280000, CFG=RECORD+0x1b4,
       ENGINE=0x281000, TRANS=0x281200, FRONT=0x281400, REAR=0x281500,
       INPUT=0x1812b8, OUTPUT=0x17c7c4, GDP=0x282000, PAD=0x17c664 };
static void call(PPCContext *c,unsigned address) {
    c->gpr[1]=STACK;
    GuestFn fn=recomp_lookup(address);
    if (!fn) abort();
    fn(c);
}
static unsigned create(PPCContext *c,unsigned model,unsigned brand,float speed,unsigned *world) {
    c->gpr[3]=6;c->gpr[4]=0;c->fpr[1]=1.f/360.f;
    call(c,0x1452c8);*world=c->gpr[3];
    wrf32(c,*world+8,-32.174f);
    memset(c->mem+RECORD,0,0x2000);
    wr32(c,STACK+8,brand);
    c->gpr[3]=CFG;c->gpr[4]=ENGINE;c->gpr[5]=TRANS;
    c->gpr[6]=FRONT;c->gpr[7]=REAR;
    c->gpr[8]=model;c->gpr[9]=0;c->gpr[10]=0;
    call(c,0x116d10); // original brand and vehicle setup
    wrf32(c,CFG+0x1ac,3.f);wrf32(c,CFG+0x1e4,180.f);
    wr32(c,CFG+0x1f8,1);wr32(c,CFG+0x1fc,1);
    for (unsigned i=0;i<9;++i) wrf32(c,CFG+0x1c0+4*i,i%4==0 ? 1.f:0.f);
    for (unsigned i=0;i<4;++i) wrf32(c,RECORD+0x3c4+4*i,1.f);
    wr32(c,RECORD+0x3e0,*world);
    c->gpr[3]=RECORD;call(c,0x13d8c4);
    const unsigned car=c->gpr[3],body=rd32(c,car);
    wr32(c,body+0x198,0); // release constructor's frozen body
    wrf32(c,body+8,speed);
    wrf32(c,car+0x12e8,1.f);
    wr8(c,car+0x12ce,1); // isolate steering from clutch/powertrain input
    for (unsigned i=0;i<4;++i) {
        const unsigned wheel=rd32(c,car+4+4*i),tire=rd32(c,car+0x14+4*i);
        wrf32(c,wheel+0x6c,speed/rdf32(c,tire));
    }
    wr32(c,RECORD+0x3d4,car);wr32(c,*world+0x24,RECORD);
    // No automatic speed-holding controls: those would brake toward the
    // descriptor's zero target. Manual controls are applied once per input.
    return car;
}

// The mounted tire callback, not an authored contact-angle fixture, supplies
// the feedback here. Incorrect contact-frame handedness turns our own steer
// into extra travel alignment: a held half stick or a 12-frame full-stick tap
// can then run to mechanical lock. Cover both sustained gain and tap response.
static int sensitivity_cases(PPCContext *c,unsigned race) {
    const float speeds[]={40.f,150.f,300.f};
    unsigned cases=0;
    int failures=0;
    for (unsigned model=0;model<4;++model) for (unsigned brand=0;brand<4;++brand)
    for (unsigned v=0;v<3;++v) {
        unsigned world;
        const unsigned car=create(c,model,brand,speeds[v],&world),body=rd32(c,car);
        memset(c->mem+INPUT,0,0x30);memset(c->mem+OUTPUT,0,0x60);
        wr32(c,INPUT+0x20,PAD);wr8(c,INPUT+0x29,7);
        wr8(c,INPUT+0x2a,128);wr8(c,INPUT+0x2b,128);
        wrf32(c,GDP+0x40,150.f);wr8(c,race+4,1);
        m2_controls_set_mode(M2_CONTROLS_CUSTOM);
        m2_analog_steering_set_enabled(1);
        unsigned observed=0;
        int failed=0;
        for (unsigned frame=0;frame<960;++frame) {
            float stick=frame>=240 && frame<360 ? .1f :
                        frame>=480 && frame<492 ? 1.f :
                        frame>=600 && frame<720 ? .5f :
                        frame>=840 && frame<846 ? 1.f : 0.f;
            if ((model+brand)%2) stick=-stick;
            m2_controls_publish(-stick,0.f,0.f,0.f);
            c->gpr[3]=INPUT;c->gpr[4]=OUTPUT;c->gpr[5]=GDP;c->gpr[6]=car;
            call(c,0x109680);
            wrf32(c,car+0x12d4,rdf32(c,OUTPUT+8));
            c->gpr[3]=car;call(c,0x146dc0);
            c->gpr[3]=world;call(c,0x145a5c);
            m2_steering_snapshot s;m2_analog_steering_get_snapshot(&s);
            if (frame>=240) {
                if ((s.contact_mask&3)==3) ++observed;
                if (frame<360 && fabsf(rdf32(c,body+0x70))>.1f) failed=1;
                if (frame>=480 && frame<600 && fabsf(s.angle)>.2f) failed=1;
                if (frame>=600 && frame<840 && fabsf(s.angle)>.08f) failed=1;
                if (frame>=840 && fabsf(s.angle)>.09f) failed=1;
                if (stick!=0.f && stick*s.angle<=0.f) failed=1;
            }
            for (unsigned i=0;i<3;++i)
                if (!isfinite(rdf32(c,body+4*i)) || !isfinite(rdf32(c,body+0xe4+4*i))) failed=1;
            if (failed) {
                fprintf(stderr,"vehicle sensitivity: car=%u brand=%u speed=%g frame=%u angle=%g yaw=%g contacts=%x fallback=%d\n",
                        model,brand,speeds[v],frame,s.angle,rdf32(c,body+0x70),s.contact_mask,s.fallback);
                break;
            }
        }
        if (observed<650) failed=1;
        failures+=failed;++cases;
    }
    fprintf(stderr,"vehicle sensitivity: %u cases, %d failures\n",cases,failures);
    return failures;
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    m2_recomp_set_game_root(argv[1]);
    if (m2_guest_prepare_restore()) return 77;
    PPCContext *c=m2_recomp_context();
    wr32(c,0x177870,1); // device records already supplied by this fixture
    const unsigned race=rd32(c,0x17d988);
    int failures=0;
    float max_yaw=0.f,max_angle=0.f;
    unsigned live_frames=0,cases=0;
    const clock_t start=clock();
    clock_t input_time=0;
    for (unsigned model=0;model<4;++model) for (unsigned brand=0;brand<4;++brand)
    for (unsigned mode=0;mode<3;++mode)
    for (unsigned physics=0;physics<2;++physics) for (unsigned v=0;v<3;++v) {
        const float speeds[]={40.f,150.f,300.f};
        unsigned world;
        const unsigned car=create(c,model,brand,speeds[v],&world),body=rd32(c,car);
        memset(c->mem+INPUT,0,0x30);memset(c->mem+OUTPUT,0,0x60);
        wr32(c,INPUT+0x20,PAD);wr8(c,INPUT+0x29,7);
        wr8(c,INPUT+0x2a,128);wr8(c,INPUT+0x2b,128);
        wrf32(c,GDP+0x40,150.f);wr8(c,race+4,physics);
        m2_controls_set_mode((m2_controls_mode)mode);
        m2_analog_steering_set_enabled(1);
        unsigned observed=0;
        int failed=0;
        float case_yaw=0.f;
        for (unsigned frame=0;frame<720;++frame) {
            // Settle, small correction, moderate held turn, release, taps,
            // then manual opposite lock. No host force or pose correction.
            float stick=frame<240 ? 0.f : frame<330 ? .05f : frame<450 ? .25f :
                        frame<480 ? 0.f : frame<600 ? (frame%12<2 ? .6f:0.f) : -.25f;
            if (brand%2) stick=-stick;
            m2_controls_publish(-stick,0.f,0.f,0.f);
            c->gpr[3]=INPUT;c->gpr[4]=OUTPUT;c->gpr[5]=GDP;c->gpr[6]=car;
            const clock_t input_start=clock();
            call(c,0x109680);
            input_time+=clock()-input_start;
            wrf32(c,car+0x12d4,rdf32(c,OUTPUT+8));
            c->gpr[3]=car;call(c,0x146dc0);
            c->gpr[3]=world;call(c,0x145a5c);
            m2_steering_snapshot s;m2_analog_steering_get_snapshot(&s);
            if (frame>=240) {
                if ((s.contact_mask&3)==3) { ++observed;++live_frames; }
                const float yaw=fabsf(rdf32(c,body+0x70));
                case_yaw=fmaxf(case_yaw,yaw);
                max_yaw=fmaxf(max_yaw,yaw);max_angle=fmaxf(max_angle,fabsf(s.angle));
                if (fabsf(s.angle-rdf32(c,car+0x1334))>.00002f) failed=1;
                // Small requests must not suddenly become full-lock commands
                // or spins. Larger inputs exercise finite coupled feedback.
                if (frame<330 && (yaw>.5f || fabsf(s.angle)>.1f)) failed=1;
                if (frame>=700 && s.angle*stick<=0.f) failed=1;
            }
            for (unsigned i=0;i<3;++i)
                if (!isfinite(rdf32(c,body+4*i)) || !isfinite(rdf32(c,body+0xe4+4*i))) failed=1;
            if (failed) {
                fprintf(stderr,"vehicle: failed car=%u brand=%u profile=%u physics=%u speed=%g frame=%u angle=%g yaw=%g contacts=%x fallback=%d\n",
                        model,brand,mode,physics,speeds[v],frame,s.angle,rdf32(c,body+0x70),s.contact_mask,s.fallback);
                break;
            }
        }
        if (observed<400 || case_yaw<.002f) {
            fprintf(stderr,"vehicle: insufficient motion/contacts %u/480 yaw=%g car=%u brand=%u profile=%u physics=%u speed=%g\n",
                    observed,case_yaw,model,brand,mode,physics,speeds[v]);
            failed=1;
        }
        failures+=failed;++cases;
    }
    fprintf(stderr,"vehicle: %u cases, %u live frames, max angle=%g yaw=%g, %.3fs CPU, %d failures\n",
            cases,live_frames,max_angle,max_yaw,(double)(clock()-start)/CLOCKS_PER_SEC,failures);
    fprintf(stderr,"vehicle: production input and live helper mean %.3f ms/update\n",
            1000.*input_time/CLOCKS_PER_SEC/(cases*720.));
    failures+=sensitivity_cases(c,race);
    m2_recomp_shutdown();
    return failures ? 1:0;
}
