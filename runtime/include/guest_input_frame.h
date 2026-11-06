// guest_input_frame.h — deterministic guest-visible input + frame tick substrate.
//
// Isolated bridge slice: explicit, allocation-free, caller-provided
// context that carries the host's per-frame control-port snapshot and monotonic
// frame tick into guest view without hidden globals.
//
// Conventions inspected:
//   * host/input/input_map.h ControlPortState — buttons (uint32_t),
//     pressed/released edge masks, raw steer/throttle/brake floats and the
//     packed guest bytes stick_x/stick_y (0..255, 128=center), device_index.
//     The byte packing is axis_to_byte() + ControlPortState::pack_bytes().
//   * host/input/broker_bridge.{h,cpp} — kernel msg-port surface that
//     synthesizes ConnectEventBroker replies (FindNamedItem 176 "eventbroker",
//     CreateMsgPort 172, CreateBufferedMsg 168, LookupItem 34, SendMsg 150,
//     WaitPort 130). Its reply record carries device count @+0x28, descriptor
//     pointer @+0x2c and a PROVISIONAL input tail at +0x38..0x3e (buttons,
//     stick_x/y, device_index). The synthetic item encoding is a process-global
//     vector `g_items` + `g_control_port` raw pointer — i.e. hidden global
//     state.
//
// This header keeps the guest view free of both:
//
//   * Guest-visible input snapshot (m2_input_snapshot) mirrors the ControlPortState
//     integer surface only: buttons/pressed/released + stick_x/stick_y + device.
//     No float crosses the boundary — the host packs the raw travel into bytes
//     before calling in, so every guest word is a fixed-width
//     integer. Identical set_snapshot sequences on identically initialized
//     contexts produce identical snapshots (deterministic, no host pointer leaks).
//   * Frame tick (m2_frame_tick) is a separate monotonic counter + dt payload.
//     Host owns time; it calls m2_input_frame_advance(dt_ms) once per host
//     frame before publishing the snapshot. The tick never reads the host clock
//     itself — determinism is "caller decides dt, substrate counts", which
//     extends the runtime's per-build determinism to input timing).
//   * No process globals. All state lives in m2_input_frame_ctx, provided by
//     the caller and reset by m2_input_frame_init(). Two contexts receiving the
//     same init → set_snapshot → advance sequence stay byte-identical. The guest
//     RAM image is never owned here: callers supply the write target EA and a
//     bounded byte writer (or a PPCContext* convenience wrapper), so the bridge
//     cannot collide with the broker carve (0x00300000) or task-stack carve
//     (0x00200000..0x00280000) unless the caller asks it to.
//
// Guest memory layout (BE, byte-exact, stable):
//   Snapshot record — 16 bytes at the caller-chosen snapshot EA:
//     +0x00 buttons    BE u32
//     +0x04 pressed    BE u32
//     +0x08 released   BE u32
//     +0x0c stick_x    u8
//     +0x0d stick_y    u8
//     +0x0e device     u8
//     +0x0f _pad       u8 (zero)
//   Frame record — 16 bytes at frame_ea (usually snapshot_ea + 16):
//     +0x00 frame_index BE u64 (hi @+0, lo @+4)
//     +0x08 dt_ms       BE u32
//     +0x0c seq         BE u32 (1-based advance count, mirrors completions)
//   The combined 32-byte window (snapshot @base, frame @base+16) is the
//   canonical "guest_input_frame" window when callers want one contiguous
//   block. Writers never read guest memory; readers are provided only for
//   host-side validation.
//
// Error convention: 0 ok, negative M2_IF_* (mirrors M2K/M2M style). Every
// guest-visible value is a fixed-width integer; no host pointer is ever
// returned or stored in a guest-visible field.
//
// Single-threaded by contract for the set/advance path (host frame thread
// only), like guest_module.h. The struct itself is plain data — callers that
// share it across threads must hold their own lock.
#pragma once

#include <stdint.h>

#include "ppc_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- error codes ----------------------------------------------------------
enum {
    M2_IF_OK           = 0,
    M2_IF_ERR_BAD_ARG  = -1,  // null ctx/out, out-of-range dt, bad EA writer
    M2_IF_ERR_STATE    = -2,  // operation requires initialized ctx
    M2_IF_ERR_RANGE    = -3,  // field value outside guest-legal range
};

#define M2_IF_NONE 0u

// ---- guest-visible snapshot ------------------------------------------------
// Mirrors ControlPortState integer surface (input_map.h). Float axes are
// intentionally absent — the host's pack_bytes() step resolves them
// to stick_x/stick_y before this point, keeping guest view deterministic
// across hosts (no float rounding divergence).
typedef struct m2_input_snapshot {
    uint32_t buttons;   // CportBits mask (kBtnA etc, input_map.h)
    uint32_t pressed;   // rising edges since the previous host frame
    uint32_t released;  // falling edges since the previous host frame
    uint8_t  stick_x;   // 0..255, 128=center (axis_to_byte(raw_steer))
    uint8_t  stick_y;   // 0..255, 128=center (axis_to_byte(raw_brake-raw_throttle))
    uint8_t  device;    // 0..3 (InputMap::kMaxPads index)
    uint8_t  _pad;      // zero, keeps BE u32 writes aligned
} m2_input_snapshot;

#define M2_INPUT_SNAPSHOT_SIZE 16u
#define M2_INPUT_OFF_BUTTONS  0u
#define M2_INPUT_OFF_PRESSED  4u
#define M2_INPUT_OFF_RELEASED 8u
#define M2_INPUT_OFF_STICK_X 12u
#define M2_INPUT_OFF_STICK_Y 13u
#define M2_INPUT_OFF_DEVICE  14u

// ---- frame tick -----------------------------------------------------------
// Monotonic, host-driven. frame_index starts at 0 after init and increments
// by exactly one per successful m2_input_frame_advance(). dt_ms is the
// caller-declared frame duration (1..1000); seq mirrors the advance count
// for ordering checks (like m2k_context::completions).
typedef struct m2_frame_tick {
    uint64_t frame_index; // monotonic, 0 after init
    uint32_t dt_ms;       // last advance dt, 0 until first advance
    uint32_t seq;         // 1-based advance count, 0 before first advance
} m2_frame_tick;

#define M2_FRAME_TICK_SIZE 16u
#define M2_FRAME_OFF_INDEX_HI 0u
#define M2_FRAME_OFF_INDEX_LO 4u
#define M2_FRAME_OFF_DT_MS    8u
#define M2_FRAME_OFF_SEQ     12u

#define M2_INPUT_FRAME_COMBINED_SIZE (M2_INPUT_SNAPSHOT_SIZE + M2_FRAME_TICK_SIZE)

// ---- context --------------------------------------------------------------
typedef struct m2_input_frame_ctx {
    uint8_t            initialized;
    uint8_t            _pad0[3];
    uint32_t           _magic;      // 'MIFR' when initialized, else 0
    m2_input_snapshot  snapshot;
    m2_frame_tick      tick;
} m2_input_frame_ctx;

#define M2_INPUT_FRAME_MAGIC 0x4D494652u // 'MIFR'

// Lifecycle: resets everything to the pristine state; identical op sequences
// after identically initialized contexts produce identical guest windows.
void m2_input_frame_init(m2_input_frame_ctx* ctx);

// Snapshot accessors: copy in/out of the ctx snapshot. No guest memory
// touched. Device must be <4, stick bytes are always legal (0..255).
int32_t m2_input_frame_set_snapshot(m2_input_frame_ctx* ctx,
                                    const m2_input_snapshot* snap);
int32_t m2_input_frame_get_snapshot(const m2_input_frame_ctx* ctx,
                                    m2_input_snapshot* out);

// Tick: advance once per host frame. dt_ms 1..1000 inclusive, 0 rejected.
// Increments tick.frame_index and tick.seq by one, records dt_ms.
int32_t m2_input_frame_advance(m2_input_frame_ctx* ctx, uint32_t dt_ms);
int32_t m2_input_frame_tick_get(const m2_input_frame_ctx* ctx,
                                m2_frame_tick* out);

// ---- guest RAM writers (bounded, BE) --------------------------------------
// Generic byte writers: return 0 on success, negative M2_IF_ERR_* on fault.
// The bridge never reads guest memory through these; it only scatters BE
// words/bytes at base EA .. base+size-1, strictly in address order.

// Write snapshot (16 B) at `ea`, frame tick (16 B) at `ea+16`, or the
// combined 32 B window. Each writer validates ctx (initialized) and the
// callback pointers / PPCContext mem bounds before touching anything; on
// fault no partial write is left observable beyond the already-scattered
// prefix? This implementation validates bounds up front and only then
// scatters, so either the whole record lands or -1 is returned with no
// guest bytes changed as far as the bounded writer can guarantee (PPCContext
// path checks ea+size <= RECOMP_GUEST_MEM).

typedef int32_t (*m2_wr8_fn)(void* ud, uint32_t ea, uint8_t v);
typedef int32_t (*m2_wr32_fn)(void* ud, uint32_t ea, uint32_t v);

// Generic callback path.
int32_t m2_input_frame_write_snapshot(const m2_input_frame_ctx* ctx,
                                      void* ud, m2_wr32_fn wr32, m2_wr8_fn wr8,
                                      uint32_t ea);
int32_t m2_input_frame_write_tick(const m2_input_frame_ctx* ctx,
                                  void* ud, m2_wr32_fn wr32,
                                  uint32_t ea);
int32_t m2_input_frame_write_combined(const m2_input_frame_ctx* ctx,
                                      void* ud, m2_wr32_fn wr32, m2_wr8_fn wr8,
                                      uint32_t ea);

// PPCContext convenience wrappers (bounds-checked against RECOMP_GUEST_MEM,
// BE scatter via wr32/wr8 semantics on c->mem). No write ever happens
// through a host pointer leaked to the guest.
int32_t m2_input_frame_write_snapshot_ctx(const m2_input_frame_ctx* ctx,
                                          PPCContext* c, uint32_t ea);
int32_t m2_input_frame_write_tick_ctx(const m2_input_frame_ctx* ctx,
                                      PPCContext* c, uint32_t ea);
int32_t m2_input_frame_write_combined_ctx(const m2_input_frame_ctx* ctx,
                                          PPCContext* c, uint32_t ea);

// ---- validation readers (host-side only, for tests) -----------------------
typedef int32_t (*m2_rd8_fn)(void* ud, uint32_t ea);
typedef int32_t (*m2_rd32_fn)(void* ud, uint32_t ea, uint32_t* out);

int32_t m2_input_frame_read_snapshot_ctx(const PPCContext* c, uint32_t ea,
                                         m2_input_snapshot* out);
int32_t m2_input_frame_read_tick_ctx(const PPCContext* c, uint32_t ea,
                                     m2_frame_tick* out);

#ifdef __cplusplus
}
#endif
