// adaptive_driver.c — shared adaptive input driver implementation.
//
// See adaptive_driver.h for the contract.  The driver is a small state
// machine advanced once per guest frame by the host.  It replaces hand-timed
// input scripts (which break when guest timing drifts) with step conditions
// that wait on observed guest state.

#include "adaptive_driver.h"

#include <stdio.h>

void m2_drv_init(m2_driver* d, const m2_drv_step* steps, int step_count,
                 uint8_t (*read8)(void*, uint32_t), void* user)
{
    if (!d) return;
    d->steps = steps;
    d->step_count = step_count;
    d->step_index = 0;
    d->phase = 0;
    d->phase_seq = 0;
    d->press_frames = 0;
    d->release_frames = 0;
    d->read8 = read8;
    d->user = user;
    d->last_seq = 0;
    d->started = 0;
    d->done = 0;
    d->buttons = 0;
    d->stick_x = 128;
    d->stick_y = 128;
    d->diag = 0;
    d->diag_limit = 8;
}

int m2_drv_done(const m2_driver* d)
{
    return d && d->done;
}

static void drv_log(const m2_driver* d, const char* fmt, uint32_t a,
                    uint32_t b)
{
    if (d->diag < d->diag_limit) {
        fprintf(stderr, "[m2-drv] ");
        fprintf(stderr, fmt, a, b);
        fprintf(stderr, "\n");
    }
}

/* Advance to the next step (or finish).  Called when the current step's
 * condition is satisfied. */
static void drv_next(m2_driver* d, uint32_t seq)
{
    ++d->step_index;
    d->phase = 0;
    d->phase_seq = seq;
    d->buttons = 0;
    if (d->step_index >= d->step_count) {
        d->done = 1;
        if (d->diag < d->diag_limit) {
            fprintf(stderr, "[m2-drv] script complete\n");
        }
    }
}

int m2_drv_tick(m2_driver* d, uint32_t guest_seq, uint32_t primary_state,
                uint32_t secondary_state, uint32_t* buttons, uint8_t* stick_x,
                uint8_t* stick_y)
{
    if (!d || d->done) return -1;

    if (!d->started) {
        d->started = 1;
        d->last_seq = guest_seq;
    }

    /* Only evaluate once per distinct guest frame. */
    const int new_frame = guest_seq != d->last_seq;
    d->last_seq = guest_seq;

    for (;;) {
        if (d->step_index >= d->step_count) {
            d->done = 1;
            return -1;
        }
        const m2_drv_step* s = &d->steps[d->step_index];
        switch (s->kind) {
        case M2_DRV_WAIT_STATE:
            if (primary_state == s->a) {
                drv_log(d, "WAIT_STATE(%u) satisfied at seq=%u", s->a,
                        guest_seq);
                drv_next(d, guest_seq);
                continue;
            }
            break;
        case M2_DRV_WAIT_SEC:
            if (secondary_state == s->a) {
                drv_log(d, "WAIT_SEC(%u) satisfied at seq=%u", s->a,
                        guest_seq);
                drv_next(d, guest_seq);
                continue;
            }
            break;
        case M2_DRV_WAIT_FRAMES:
            if (d->phase == 0) {
                d->phase = 1;
                d->phase_seq = guest_seq;
            }
            if (new_frame && guest_seq >= d->phase_seq + s->a) {
                drv_log(d, "WAIT_FRAMES(%u) done at seq=%u", s->a, guest_seq);
                drv_next(d, guest_seq);
                continue;
            }
            break;
        case M2_DRV_PRESS: {
            /* Phase 1: hold `b` frames.  Phase 2: release `c` frames. */
            if (d->phase == 0) {
                d->phase = 1;
                d->phase_seq = guest_seq;
                d->press_frames = s->b;
                d->release_frames = s->c;
                d->buttons = s->a;
                d->stick_x = s->sx;
                d->stick_y = s->sy;
            }
            if (d->phase == 1 && new_frame &&
                guest_seq >= d->phase_seq + d->press_frames) {
                d->phase = 2;
                d->phase_seq = guest_seq;
                d->buttons = 0;
                d->stick_x = 128;
                d->stick_y = 128;
                drv_log(d, "PRESS(%u) released at seq=%u", s->a, guest_seq);
            }
            if (d->phase == 2 && new_frame &&
                guest_seq >= d->phase_seq + d->release_frames) {
                drv_log(d, "PRESS(%u) release window done at seq=%u", s->a,
                        guest_seq);
                drv_next(d, guest_seq);
                continue;
            }
            break;
        }
        case M2_DRV_STEP_UNTIL: {
            /* While guest byte at EA `b` != `c`: press `a` for `d` frames,
             * release `e` frames.  Re-check the byte after each release. */
            if (d->phase == 0) {
                uint8_t v = d->read8 ? d->read8(d->user, s->b) : 0;
                if (v == (uint8_t)s->c) {
                    drv_log(d, "STEP_UNTIL target %u reached at seq=%u",
                            s->c, guest_seq);
                    drv_next(d, guest_seq);
                    continue;
                }
                d->phase = 1;
                d->phase_seq = guest_seq;
                d->press_frames = s->d;
                d->release_frames = s->e;
                d->buttons = s->a;
                d->stick_x = s->sx;
                d->stick_y = s->sy;
            }
            if (d->phase == 1 && new_frame &&
                guest_seq >= d->phase_seq + d->press_frames) {
                d->phase = 2;
                d->phase_seq = guest_seq;
                d->buttons = 0;
                d->stick_x = 128;
                d->stick_y = 128;
            }
            if (d->phase == 2 && new_frame &&
                guest_seq >= d->phase_seq + d->release_frames) {
                /* Release window done: re-check target before next press. */
                d->phase = 0;
                d->phase_seq = guest_seq;
                d->buttons = 0;
            }
            break;
        }
        default:
            /* Unknown step kind: fail closed to avoid spinning forever. */
            if (d->diag < d->diag_limit) {
                fprintf(stderr, "[m2-drv] unknown step kind %d — aborting\n",
                        s->kind);
            }
            d->done = 1;
            return -1;
        }
        break;
    }

    *buttons = d->buttons;
    *stick_x = d->stick_x;
    *stick_y = d->stick_y;
    return d->buttons != 0;
}
