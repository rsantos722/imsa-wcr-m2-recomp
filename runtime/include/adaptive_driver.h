// adaptive_driver.h — shared adaptive input driver.
//
// The guest StateDriver owns application frames; a host (m2host frame pump or
// a standalone probe) supplies VBL, input, and state observation.  Hand-timed
// input scripts are brittle because guest timing drifts run to run (e.g. the
// startup menu appears at frame 606 in one run and 661 in another).  This
// driver is an *adaptive* alternative: the host feeds it the observed guest
// state once per guest frame and it answers "what input should be held for
// the next frame".
//
// The script is a flat list of steps.  Each step is one of:
//   M2_DRV_WAIT_STATE   — wait until the guest primary state equals `a`
//   M2_DRV_WAIT_SEC     — wait until the guest secondary state equals `a`
//   M2_DRV_WAIT_FRAMES  — wait `a` guest frames (no input)
//   M2_DRV_PRESS        — hold button `a` for `b` guest frames, then release
//   M2_DRV_STEP_UNTIL   — while guest byte at EA `b` != `c`: press `a` for
//                         `d` frames, release `e` frames (the probe's cursor
//                         loop: step a D-pad until the cursor reaches target)
//
// The host calls m2_drv_tick() once per guest frame with the observed
// seq/state/sec.  The driver advances its internal script state, and on
// frames where input should be held it returns 1 and fills *buttons/*stick.
// Memory reads (STEP_UNTIL) go through the read8 callback the host supplies;
// it must return the byte at a guest EA, or 0 if unavailable.
//
// This file is plain C so both m2host (C++) and standalone probes (C) can
// link it through the shared m2recomp library.

#ifndef M2_ADAPTIVE_DRIVER_H
#define M2_ADAPTIVE_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    M2_DRV_WAIT_STATE = 1,
    M2_DRV_WAIT_SEC = 2,
    M2_DRV_WAIT_FRAMES = 3,
    M2_DRV_PRESS = 4,
    M2_DRV_STEP_UNTIL = 5,
};

typedef struct m2_drv_step {
    int kind;
    uint32_t a; /* state / seconds / button / frames     */
    uint32_t b; /* frames / EA for STEP_UNTIL            */
    uint32_t c; /* target byte for STEP_UNTIL            */
    uint32_t d; /* press frames for STEP_UNTIL           */
    uint32_t e; /* release frames for STEP_UNTIL         */
    uint8_t sx; /* stick_x while a button is held        */
    uint8_t sy; /* stick_y while a button is held        */
} m2_drv_step;

typedef struct m2_driver {
    const m2_drv_step* steps;
    int step_count;
    int step_index;

    /* per-step sub-state */
    int phase;           /* 0 = entering, 1 = pressing, 2 = releasing */
    uint32_t phase_seq;  /* guest seq when the current phase began */
    uint32_t press_frames;
    uint32_t release_frames;

    /* memory reader: returns byte at guest EA (0 on miss) */
    uint8_t (*read8)(void* user, uint32_t ea);
    void* user;

    uint32_t last_seq;
    int started;
    int done;

    /* current output */
    uint32_t buttons;
    uint8_t stick_x;
    uint8_t stick_y;

    /* diagnostics (capped) */
    uint32_t diag;
    uint32_t diag_limit;
} m2_driver;

void m2_drv_init(m2_driver* d, const m2_drv_step* steps, int step_count,
                 uint8_t (*read8)(void*, uint32_t), void* user);

/* Feed observed guest state once per guest frame.
 * Returns 1 when input should be held for the next frame (fills *buttons,
 * *stick_x, *stick_y) and 0 otherwise.  Returns -1 when the script finished. */
int m2_drv_tick(m2_driver* d, uint32_t guest_seq, uint32_t primary_state,
                uint32_t secondary_state, uint32_t* buttons, uint8_t* stick_x,
                uint8_t* stick_y);

int m2_drv_done(const m2_driver* d);

#ifdef __cplusplus
}
#endif

#endif /* M2_ADAPTIVE_DRIVER_H */
