// shim_types.h — minimal shared types for the M2 host shim layer.
//
// Provisional pending a full cross-check against the public M2 SDK 2.7
// headers; sizes follow PPC32/EABI and are stable, but struct layouts marked
// PROVISIONAL must be re-validated before production guest code relies on them.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t m2_err_t;    // 0 = success, negative = error (3DO convention)
typedef int32_t m2_item_t;   // OS Item handles (kernel/named resources)
typedef int32_t m2_signal_t; // signal mask bit index from AllocSignal

#define M2_OK 0

// Intrusive doubly-linked node used by kernel list ops
// (AddHead/AddTail/RemHead/RemTail/AddTail/RemNode — kernel:40..45).
// Layout PROVISIONAL: field order next/prev vs prev/next not yet pinned;
// host list ops only maintain their own consistency this phase.
typedef struct m2_node {
    struct m2_node* next;
    struct m2_node* prev;
} m2_node_t;

typedef struct m2_list {
    m2_node_t head;
    m2_node_t tail;
    uint32_t count;
} m2_list_t;

// TagArg array element (audio/audiopatch instrument + patch templates).
// Layout PROVISIONAL vs M2 SDK TagArg.
typedef struct m2_tag_arg {
    uint32_t tag;
    int32_t value;
} m2_tag_arg_t;

// Filesystem seek whence for SeekRawFile (filesystem:39). Values are the
// source-defined SET/CUR/END origins; the M2 implementation clamps the target
// and returns the previous cursor position.
enum {
    M2_SEEK_SET = 0,
    M2_SEEK_CUR = 1,
    M2_SEEK_END = 2,
};

#ifdef __cplusplus
}  // extern "C"
#endif
