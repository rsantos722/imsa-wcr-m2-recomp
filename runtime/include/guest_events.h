#ifndef GUEST_EVENTS_H
#define GUEST_EVENTS_H

// Bounded event envelope shared by guest provenance hooks and generated
// tracepoints. Scalar and text fields have fixed widths. The callback is
// synchronous: the tracepoint view pointer is only valid for the duration of
// the callback and is never part of an artifact. Artifact writers must
// serialize guest values, not host pointers.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M2_GUEST_EVENT_SCHEMA_VERSION 1u
#define M2_GUEST_EVENT_TEXT_MAX 64u

enum {
    M2_GUEST_EVENT_WRITE = 1u,
    M2_GUEST_EVENT_READ = 2u,
    M2_GUEST_EVENT_COPY = 3u,
    M2_GUEST_EVENT_ALLOC = 4u,
    M2_GUEST_EVENT_FREE = 5u,
    M2_GUEST_EVENT_OOB = 6u,
    M2_GUEST_EVENT_TRACEPOINT = 7u,
};

struct M2TracepointCapture;

typedef struct M2GuestEvent {
    uint32_t schema_version;
    uint32_t kind;
    uint32_t pc;
    uint32_t lr;
    uint32_t frame_seq;
    uint32_t ea;
    uint32_t src_ea;
    uint32_t size;
    uint32_t value;
    uint32_t width;
    uint32_t tracepoint_id;
    const struct M2TracepointCapture* tracepoint;
    char tag[M2_GUEST_EVENT_TEXT_MAX];
    char region[M2_GUEST_EVENT_TEXT_MAX];
} M2GuestEvent;

// Strongly overridden by the host trace writer. Standalone recompilation and
// focused guest tests link the weak bridge in trace_stubs.c. Provenance events
// are already runtime-filtered by watched ranges/configuration before they
// reach this callback.
extern void m2_guest_event_c(const M2GuestEvent* event)
    __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif // GUEST_EVENTS_H
