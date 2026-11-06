// Portfolio View scanout is independent of the Triangle Engine destination.
// The host copies an immutable display snapshot, presents it, then acknowledges
// that exact generation before View display/render notifications are sent.
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define M2_DISPLAY_WIDTH 640u
#define M2_DISPLAY_HEIGHT 480u
int m2_guest_display_copy(uint32_t *rgba, size_t pixels, uint64_t *generation);
// Same immutable snapshot as m2_guest_display_copy, with the host-native
// RGBA content hash already computed when the guest View is modified.  The
// hash is presentation metadata only; the copied pixels remain authoritative.
int m2_guest_display_copy_hashed(uint32_t *rgba, size_t pixels,
                                 uint64_t *generation, uint64_t *content_hash);
// Returns the guest buffer selected by an overlay View whose pixels remain
// renderer-owned. Zero means the View uses its immutable RAM snapshot.
uint32_t m2_guest_display_te_target(void);
// While VideoPlayer's guest-owned overlay mode is active, returns the two
// display-buffer bases that must begin with the last View image before its
// first TE overlay list runs.
int m2_guest_video_overlay_targets(uint32_t *view_base, uint32_t *draw_base);
int m2_guest_video_overlay_active(void);
void m2_guest_display_presented(uint64_t generation);
// Acknowledge one TE DISPLAYFRAMEBUFFER after its selected target has reached
// the host presentation boundary. While paced presentation is active, the
// next display command waits for this edge just as Portfolio waits for the
// previous View state to become visible.
//
// presented_target is the guest framebuffer the presented pixels actually came
// from, or 0 when a direct Bitmap/Projector image owned scanout instead of the
// addressed TE target. The count alone cannot express that identity, so a name
// that disagrees with the guest's own choice is recorded as an explicit
// ownership refusal and still releases the edge: the released driver waits on
// the View display callback, which carries no image argument and fires on the
// next presented field whatever the View holds, and it defines no error return
// for "not visible yet". Count those refusals with
// m2_guest_te_display_refusals().
void m2_guest_te_display_presented(uint32_t presented_target);
// Number of TE display acknowledgements whose presented image was not the
// guest's own DISPLAYFRAMEBUFFER target. Diagnostic only: it never withholds
// the fence and never changes a presented pixel.
uint64_t m2_guest_te_display_refusals(void);
#ifdef __cplusplus
}
#endif
