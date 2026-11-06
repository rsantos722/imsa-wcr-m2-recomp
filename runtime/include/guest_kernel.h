// guest_kernel.h — self-contained guest kernel runtime slice.
//
// Deterministic, allocation-free model of the Opera kernel surfaces the game
// actually calls (docs/m2/portfolio-os.md): message ports (kernel:172 CreateMsgPort),
// signals (kernel:143/144/165/166), IOReqs (kernel:170 CreateIOReq,
// 0x81 WaitIO / 0x84 DoIO / 0x8E SendIO) and guest tasks
// (kernel:55 CreateThread / kernel:84 CreateThreadVA). This module backs the
// recompiled guest only.
//
// Guest-safety contract:
//   * Every value that can cross into guest code is a fixed-width uint32_t
//     item handle or payload word. No host pointer is ever returned or stored
//     in a guest-visible field.
//   * Handles are (generation << 12) | (slot << 4) | type. Slots are handed
//     out lowest-free-first from static pools and generations come from a
//     monotonic counter. Cold contexts start at generation one; a context
//     restarted after m2k_destroy continues its counter so old handles cannot
//     alias a newly initialized item at the same storage address.
//   * Stale handles (slot reused after delete/free) fail generation checks
//     instead of aliasing a newer item.
//
// Blocking primitives (m2k_wait_signal/m2k_wait_port/m2k_wait_io) use one
// pthread mutex + condition variable per context: no busy-waiting, no host
// CPU spin. POSIX hosts only (matches the rest of the repo's CI targets).
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "recomp_helpers.h"   // bounded TagArg scan for the _va creators

#ifdef __cplusplus
extern "C" {
#endif

// ---- error codes (3DO convention: 0 ok, negative error) ------------------
enum {
    M2K_OK            = 0,
    M2K_ERR_BAD_ITEM  = -1,   // NULL/zero handle, bad gen/slot/type mismatch
    M2K_ERR_NO_SLOT   = -2,   // pool exhausted
    M2K_ERR_EMPTY     = -3,   // FIFO empty / nothing matched (non-blocking)
    M2K_ERR_STATE     = -4,   // operation invalid for the item's current state
    M2K_ERR_RANGE     = -5,   // signal bit out of range / zero mask
    // Portfolio NOTOWNER: MakeKErr(ER_SEVERE, ER_C_STND, ER_NotOwner).
    M2K_ERR_NOT_OWNER = (int32_t)0xd57b9012u,
    // Portfolio ER_Kr_BadLockArg (non-standard kernel error 16).
    M2K_ERR_BAD_LOCK_ARG = (int32_t)0xd57b9110u,
    // Portfolio KILLED: MakeKErr(ER_SEVERE, ER_C_NSTND,
    // ER_Kr_TaskKilled), from M2 SDK src/includes/kernel/operror.h.
    M2K_ERR_KILLED    = (int32_t)0xd57b9115u,
    // Portfolio NOSUPPORT: MakeKErr(ER_SEVERE, ER_C_STND, ER_NotSupported),
    // the "this item type has no ir_SetPriority vector" refusal (item.c:960).
    M2K_ERR_NO_SUPPORT = (int32_t)0xd57b9010u,
    // Portfolio MAKEKERR(ER_SEVERE, ER_C_NSTND, ER_Kr_BadPriority), the
    // internalChangeTaskPri band rejection for an unprivileged task
    // (M2 SDK src/folios/kernel/task.c:540, operror.h ER_Kr_BadPriority = 9).
    M2K_ERR_BAD_PRIORITY = (int32_t)0xd57b9109u,
    // Portfolio BADSUBTYPE: MakeKErr(ER_SEVERE, ER_C_STND, ER_BadSubType) —
    // internalSetPriorityKernelItem's answer for a kernel item that is not a
    // TASKNODE (M2 SDK src/folios/kernel/item.c:1466-1468, operror.h ER_BadSubType
    // = 7).
    M2K_ERR_BAD_SUBTYPE = (int32_t)0xd57b9007u,
};

#define M2K_ITEM_NONE 0u

// Self means the calling task. Calls made by host code without a current
// task have no task to target and wake every waiter instead.
#define M2K_TASK_SELF 0u

// ---- 32-bit guest item handles -------------------------------------------
// bits [3:0]  type | bits [11:4] slot index | bits [31:12] generation (1-based)
typedef uint32_t m2k_item_t;

enum {
    M2K_TYPE_MSGPORT = 1,
    M2K_TYPE_MESSAGE = 2,
    M2K_TYPE_IOREQ   = 3,
    M2K_TYPE_TASK    = 4,
    // Host-kernel extension banks for Message descriptors. The guest treats
    // Items as opaque signed words; separate types preserve the 8-bit slot
    // layout while supplying the source kernel's growable item-table behavior.
    M2K_TYPE_MESSAGE_EXT = 6,
    // Type 7 is the host-local EventBroker token; filesystem/local Item
    // handles occupy 8-11, so the second bank uses the next free nibble.
    M2K_TYPE_MESSAGE_EXT2 = 12,
    M2K_TYPE_MESSAGE_EXT3 = 13,
};

#define M2K_HANDLE_TYPE_MASK  0x0000000Fu
#define M2K_HANDLE_SLOT_SHIFT 4
#define M2K_HANDLE_SLOT_MASK  0x00000FF0u
#define M2K_HANDLE_GEN_SHIFT  12
#define M2K_HANDLE_GEN_MASK   0xFFFFF000u

static inline uint32_t m2k_handle_type(uint32_t h) { return h & M2K_HANDLE_TYPE_MASK; }
static inline uint32_t m2k_handle_slot(uint32_t h) { return (h & M2K_HANDLE_SLOT_MASK) >> M2K_HANDLE_SLOT_SHIFT; }
static inline uint32_t m2k_handle_gen(uint32_t h)  { return (h & M2K_HANDLE_GEN_MASK) >> M2K_HANDLE_GEN_SHIFT; }

// ---- pool sizes ------------------------------------------------------------
#define M2K_MAX_PORTS    64u
// The original kernel allocates Message items dynamically; the streaming
// library's message pool alone fills 254 of the 256-entry type-2 slice. Keep
// that original-width slice and a separate bounded transient extension below.
#define M2K_MAX_MESSAGES 256u
// Three extension banks cover the observed persistent intro stream pool plus
// a subsequent 254-entry preview stream and its control messages. They are
// linked in port FIFOs with negative references (-2 and below), leaving the
// existing -1 empty sentinel and all type-2 handles ABI-compatible.
#define M2K_MESSAGE_BANK_SIZE 256u
#define M2K_MAX_EXT_MESSAGES (3u * M2K_MESSAGE_BANK_SIZE)
static inline int m2k_is_message_handle(uint32_t h) {
    const uint32_t type = m2k_handle_type(h);
    return type == M2K_TYPE_MESSAGE || type == M2K_TYPE_MESSAGE_EXT ||
           type == M2K_TYPE_MESSAGE_EXT2 ||
           type == M2K_TYPE_MESSAGE_EXT3;
}

// Object slots are contiguous across the original and extension pools for
// host-backed guest Message descriptors and savestate copies.
static inline uint32_t m2k_message_object_slot(uint32_t h) {
    if (m2k_handle_type(h) == M2K_TYPE_MESSAGE_EXT)
        return M2K_MAX_MESSAGES + m2k_handle_slot(h);
    if (m2k_handle_type(h) == M2K_TYPE_MESSAGE_EXT2)
        return M2K_MAX_MESSAGES + M2K_MESSAGE_BANK_SIZE +
               m2k_handle_slot(h);
    if (m2k_handle_type(h) == M2K_TYPE_MESSAGE_EXT3)
        return M2K_MAX_MESSAGES + 2u * M2K_MESSAGE_BANK_SIZE +
               m2k_handle_slot(h);
    return m2k_handle_slot(h);
}
// The original kernel allocates IOReq items dynamically. The booted guest
// service graph (audio/SoundPlayer plus the streaming library) holds more than
// an initial 16-entry slice and had every one of 32 entries live when the
// FMV stream file tried to open its request. Keep a bounded 128-entry slice for
// the observed production graph; the guest still owns every request lifetime
// through DeleteIOReq/DeleteItem.
#define M2K_MAX_IOREQS   128u
// The guest's own worker census needs ten concurrent tasks on the standing
// route (the lifecycle/concurrency contract): the imsa_shell
// launcher, the game task, soundtrack_thread_func@0x28108, ebthread@0x1fe9c,
// trackInit_1@0x166dc, trackInit_2@0x1726c, DataStreamThread@0x692a0,
// DataAcqThread@0x6a388, MPEGVideoSubscriberThread@0x64b88,
// MPEGAudioSubscriberThread@0x60d6c and MPADecoderThread@0x63cf8. A bound
// below that is a host-invented resource limit, not guest behaviour: the
// Portfolio kernel creates all of them, and the original CreateThreadVA
// answers a refusal with a negative Err. Twelve keeps headroom over the
// census so a route that overlaps the two track-init workers is still served.
#define M2K_MAX_TASKS    12u
#define M2K_MAX_SEMAPHORES 32u
#define M2K_SIG_BITS     32u   // signal mask width (uint32_t)
// Portfolio reserves the low system bits and bit 31. Guest AllocSignal(0)
// therefore searches the user-visible bits above this mask.
#define M2K_SIGNAL_RESERVED_MASK UINT32_C(0x800000ff)

// ---- message port FIFO ------------------------------------------------------
// A message lives in one of five states. CreateMsg() (kernel:171) mints a
// message in M2K_MSG_HELD with its reply port retained; SendMsg() moves a
// held message onto a port queue (M2K_MSG_QUEUED); WaitPort()/GetMsg()
// dequeue it (M2K_MSG_DISPATCHED); ReplyMsg() frees the slot again.
enum {
    M2K_MSG_FREE = 0,
    M2K_MSG_QUEUED,
    M2K_MSG_DISPATCHED,      // returned by get_msg, awaiting reply
    M2K_MSG_REPLIED,
    M2K_MSG_HELD,            // created by CreateMsg, not yet sent
};

typedef struct m2k_message {
    uint32_t generation;     // pinned at send time; dies at reply
    uint8_t  state;
    uint8_t  return_to_sender; // replied onto its reply port, await sender GetMsg
    int32_t  origin_port;    // pool index of the sending port, -1 none
    m2k_item_t owner;        // task whose resource table owns this Message Item
    m2k_item_t holder;       // task holding a dispatched message, or NONE
    uint32_t what;
    uint32_t value;          // guest payload word (never a host pointer)
    m2k_item_t reply_port;   // guest message reply port, or M2K_ITEM_NONE
    uint32_t data_ptr;       // guest data EA, or 0 for the compact test path
    uint32_t data_size;      // guest data byte count
    int32_t  reply_code;
    int32_t  next;           // FIFO link: pool/ref successor, -1 end
} m2k_message;

typedef struct m2k_msgport {
    uint32_t generation;
    uint8_t  used;
    uint32_t priority;       // M2K_TAG_PORT_PRIORITY payload, default 0
    int32_t  head, tail;     // queue refs: pool index, ext ref, or -1
    uint32_t queued_count;
    int32_t  last_reply_code;// reply code observed on this port's messages
    uint32_t signal_mask;    // mp_Signal: owner wake mask (one bit, 0 = none)
    m2k_item_t owner;        // task that created the port, M2K_ITEM_NONE = ctx
    uint8_t  signal_allocated; // mask came from this port's AllocSignal
    uint32_t userdata;       // M2K_TAG_PORT_USERDATA payload, default 0
} m2k_msgport;

// ---- IOReq completion state machine ----------------------------------------
// IDLE --do_io/send_io--> IN_FLIGHT --(backend completes)--> COMPLETE.
// Most host-backed requests complete immediately; source-shaped timer and
// retained-device requests use the same condvar path for real asynchronous
// completion and AbortIO.
typedef enum {
    M2K_IO_IDLE = 0,
    M2K_IO_IN_FLIGHT = 1,
    M2K_IO_COMPLETE = 2,
} m2k_io_state;

typedef struct m2k_ioreq {
    uint32_t generation;
    uint8_t  used;
    uint8_t  state;          // m2k_io_state
    int32_t  status;         // completion status, valid once COMPLETE
    uint32_t device;         // M2K_TAG_IO_DEVICE payload, default 0
    uint32_t what, arg;      // payload words recorded at send/do time
    uint64_t complete_seq;   // 1-based order of completion within ctx
    uint32_t signal_mask;    // CREATEIOREQ_TAG_SIGNAL wake mask (0 = none)
    m2k_item_t owner;        // task that created the request, NONE = context
    m2k_item_t reply_port;   // CREATEIOREQ_TAG_REPLYPORT (NONE = none)
    m2k_item_t msg_item;     // internal message item for reply port delivery
    uint32_t user_data;      // ioi_UserData from IOInfo
    uint32_t metronome_signal; // TIMERCMD_METRONOME_USEC signal mask
    uint64_t metronome_interval_usec; // recurring interval while IN_FLIGHT
    uint64_t metronome_next_fire_ns; // host realtime deadline, rebuilt on load
    uint8_t timer_oneshot;   // source-shaped one-shot usec timer is in flight
    uint64_t timer_delay_usec; // remaining/requested delay for savestate rebase
    uint64_t timer_deadline_ns; // host realtime deadline while live
    uint8_t timer_vbl;       // source-shaped VBL timer request is in flight
    uint8_t timer_vbl_metronome; // recurring VBL request signals instead
    uint16_t timer_vbl_reserved;
    uint32_t timer_vbl_interval; // source ioi_Offset countdown
    uint32_t timer_vbl_actual; // source io_Actual countdown
    uint32_t timer_vbl_signal; // source ioi_CmdOptions for metronomes
    uint64_t timer_vbl_completion_tick; // VBL count that completed a one-shot
} m2k_ioreq;

// ---- guest tasks --------------------------------------------------------------
// Host pthreads behind fixed-width guest task handles. The entrypoint is a
// host-side function registered by recompiled guest code; the only value that
// crosses into guest view is the uint32_t handle — no pthread_t or host
// pointer is ever exposed. State advances monotonically
// STARTED -> RUNNING -> {COMPLETED | FAILED}; the terminal transition happens
// under the context lock, so joiners sleeping on the condvar observe it
// deterministically regardless of host scheduling.
typedef struct m2k_context m2k_context;

typedef enum {
    M2K_TASK_STARTED  = 0,   // spawned, body not entered yet
    M2K_TASK_RUNNING  = 1,   // inside fn
    M2K_TASK_COMPLETED = 2,  // fn returned >= 0
    M2K_TASK_FAILED   = 3,   // fn returned < 0
} m2k_task_state;

typedef int32_t (*m2k_task_fn)(m2k_context* ctx, uint32_t arg);

// Extended entrypoint: carries an opaque host-side cookie instead of the
// uint32_t guest payload word. Still never guest-visible.
typedef int32_t (*m2k_task_fn_ex)(m2k_context* ctx, void* user_data);

typedef struct m2k_task {
    uint32_t    generation;
    uint8_t     used;
    uint8_t     state;       // m2k_task_state
    uint8_t     priority;    // Portfolio ItemNode priority (larger runs first)
    uint8_t     signal_wait_active;
    uint32_t    signal_wait_mask;
    uint64_t    kernel_boundary_seq;
    // Portfolio quantum state (M2 SDK src/folios/kernel/taskswap.c): `quantum` is
    // t_Quantum, `quantum_remainder` is t_QuantumRemainder, and
    // `quantum_expired` is TASK_QUANTUM_EXPIRED, which NewReadyTask arm (1)
    // reads. `run_started_ns` is the host's taskStartTime for this task's
    // current launch; `ready`/`ready_at_head`/`ready_seq` are the ready-order
    // placement NewReadyTask and LandReady give it; `quantum_timer_armed` plus
    // `quantaclock_deadline_ns` are this task's loaded quantaclock. The
    // vendor's `timerArmed`/`_quantaclock` pair belongs to whichever task is
    // running, so it is per task here and only its own Launch and arms can arm
    // or discharge it. See guest_kernel.c "Portfolio quantum scheduling".
    uint64_t    quantum;
    uint64_t    quantum_remainder;
    uint64_t    run_started_ns;
    uint64_t    ready_seq;
    uint64_t    quantaclock_deadline_ns;
    uint8_t     quantum_expired;
    uint8_t     quantum_timer_armed;
    uint8_t     ready;
    uint8_t     ready_at_head;
    uint32_t    sig_allocated; // task-owned signal allocation mask
    uint32_t    sig_pending; // signals queued for this task only
    uint8_t     wait_active; // task is sleeping in WaitPort
    m2k_item_t  wait_port;   // port item for the active WaitPort
    m2k_item_t  wait_msg;    // named message, or 0 for any message
    m2k_item_t  wait_result; // message delivered directly to the waiter
    uint32_t    arg;         // guest payload word handed to fn
    m2k_task_fn fn;          // uint32_t-payload entrypoint (never guest-visible)
    m2k_task_fn_ex fn_ex;    // extended entrypoint; set => trampoline uses it
    void*       user_data;   // opaque cookie handed to fn_ex
    m2k_item_t  owner;       // creating task; receives SIGF_DEADTASK on exit
    int32_t     exit_status; // fn/fn_ex result, valid once terminal
    // CREATETASK_TAG_MSGFROMCHILD (kernel/task.h:248, value 19): the creator's
    // message port. M2K_ITEM_NONE means the task was created without the tag.
    // The task trampoline queues one status message here after publishing the
    // terminal state, carrying the exit status in msg_Result, this task's item
    // in msg_DataPtr, and the terminating task's item in msg_DataSize.
    m2k_item_t  exit_notify_port;
    m2k_item_t  exit_notify_data;  // item number reported as msg_DataSize
} m2k_task;

// Keep the public LockSemaphore exclusive/shared ownership, recursive counts,
// and FIFO waiter order without exposing host synchronization objects through
// the guest ABI.
typedef struct m2k_semaphore {
    uint8_t     used;
    uint8_t     shared;
    m2k_item_t  locker;
    uint32_t    nest_count;
    m2k_item_t  shared_lockers[M2K_MAX_TASKS];
    uint32_t    shared_nest[M2K_MAX_TASKS];
    m2k_item_t  waiters[M2K_MAX_TASKS];
    uint8_t     waiter_shared[M2K_MAX_TASKS];
    uint8_t     wait_count;
} m2k_semaphore;

// Trampoline cookie pool (host-internal): lets a detached thread recover its
// context + slot without heap allocation. A slot stays pinned until its own
// thread publishes a terminal state, so cookies are never rewritten live.
typedef struct m2k_task_boot {
    m2k_context* ctx;
    int32_t      slot;
} m2k_task_boot;

// ---- context ----------------------------------------------------------------
struct m2k_context {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint8_t         initialized;

    // Signals: allocated-bit ownership + pending delivery set.
    uint32_t        sig_allocated;
    uint32_t        sig_pending;

    m2k_msgport     ports[M2K_MAX_PORTS];
    m2k_message     msgs[M2K_MAX_MESSAGES];
    m2k_message     ext_msgs[M2K_MAX_EXT_MESSAGES];
    m2k_ioreq       ioreqs[M2K_MAX_IOREQS];
    m2k_task        tasks[M2K_MAX_TASKS];
    m2k_task_boot   boots[M2K_MAX_TASKS];
    m2k_semaphore   semaphores[M2K_MAX_SEMAPHORES];

    uint64_t        completions;  // total IO completions (order counter)
    uint64_t        vblank_count; // KernelVBLHandler's source-owned counter
    uint32_t        gen_counter;  // monotonic generation source across restart

    // Monotonic sources for the two ready-order placements: a wake joins at
    // the tail (FIFO ascending), a landing with time remaining rejoins at the
    // head (newest first).
    uint64_t        ready_tail_seq;
    uint64_t        ready_head_seq;
};

void m2k_init(m2k_context* ctx);
void m2k_destroy(m2k_context* ctx);

// ---- message ports ----------------------------------------------------------
// _va variants parse an optional bounded TagArg list (NULL tolerated = all
// defaults); unknown tags are ignored and entries behind M2_TAG_END are
// invisible per the shared m2_tag_* helpers.
enum {
    M2K_TAG_PORT_PRIORITY = 1,   // u32 value: recorded on the port, default 0
    M2K_TAG_IO_DEVICE     = 2,   // u32 value: recorded on the ioreq, default 0
    M2K_TAG_PORT_SIGNAL   = 10,  // CREATEPORT_TAG_SIGNAL: explicit mp_Signal
    M2K_TAG_PORT_USERDATA = 11,  // CREATEPORT_TAG_USERDATA: mp_UserData
    // CREATEIOREQ_TAG_* (kernel/io.h): the CreateItem(MKNODEID(KERNELNODE,
    // IOREQNODE)) shape FMVCreateIOReq uses.
    M2K_TAG_IO_REPLYPORT  = 10,  // optional completion reply port
    M2K_TAG_IO_DEVICE_VA  = 11,  // device Item (required)
    M2K_TAG_IO_SIGNAL     = 12,  // signal sent to the owner when IO is done
};

// SIGF_IODONE (kernel/task.h): default IO completion signal bit.
#define M2K_SIGF_IODONE 8u
// MKNODEID(KERNELNODE=1, IOREQNODE=14): the CreateItemVA ctype CreateIOReq
// lowers to. An item with this type is a real kernel IOReq.
#define M2K_KERNEL_IOREQ_CTYPE 0x0000010eu
// MKNODEID(KERNELNODE=1, MESSAGENODE=9): the CreateItemVA ctype CreateMsg
// lowers to (CreateForwardMsg 0x65d30). Tag 10 is CREATEMSG_TAG_REPLYPORT.
#define M2K_KERNEL_MESSAGE_CTYPE 0x00000109u

m2k_item_t m2k_create_msgport(m2k_context* ctx);
m2k_item_t m2k_create_msgport_va(m2k_context* ctx, const m2_tag_arg* tags,
                                 uint32_t max_count);
int32_t    m2k_delete_msgport(m2k_context* ctx, m2k_item_t port);
int32_t    m2k_delete_msg(m2k_context* ctx, m2k_item_t msg);

int32_t    m2k_send_msg(m2k_context* ctx, m2k_item_t port,
                        uint32_t what, uint32_t value);
// Non-blocking dequeue (Opera GetMsg semantics). On success fills msg_out
// (handle stays valid until reply) plus the payload words.
int32_t    m2k_get_msg(m2k_context* ctx, m2k_item_t port, m2k_item_t* msg_out,
                       uint32_t* what_out, uint32_t* value_out);
// Blocks until the port has at least one message (Opera WaitPort). If the
// caller is a guest task, an arriving message is delivered directly to a
// matching waiter, as Portfolio does, and the returned item is stored in
// msg_out when that output is requested by m2k_wait_port_any().
int32_t    m2k_wait_port(m2k_context* ctx, m2k_item_t port);
// WaitPort(port, 0) with the delivered message item returned to the caller.
int32_t    m2k_wait_port_any(m2k_context* ctx, m2k_item_t port,
                            m2k_item_t* msg_out);
// Blocks until the named message is queued on port, then removes that exact
// message. This is the WaitPort(port, msg) contract; unlike m2k_wait_port it
// must not consume an unrelated reply that happens to share the port.
int32_t    m2k_wait_port_msg(m2k_context* ctx, m2k_item_t port,
                             m2k_item_t msg);
// Only DISPATCHED messages accept a reply; replying frees the message slot.
int32_t    m2k_reply_msg(m2k_context* ctx, m2k_item_t msg, int32_t reply_code);
// As ReplyMsg returns a retained message to a synchronous WaitPort waiter,
// publish an optional guest-visible msg_Result while the kernel lock is still
// held and before the waiter can observe the reply. A NULL writer keeps the
// ordinary host-kernel form.
typedef void (*m2k_reply_result_writer)(void* opaque, uint32_t result);
int32_t    m2k_reply_msg_with_result(m2k_context* ctx, m2k_item_t msg,
                                     int32_t reply_code,
                                     m2k_reply_result_writer writer,
                                     void* opaque);
// Snapshot the guest-visible fields needed to materialize MESSAGE(msg).
// Returned EAs are guest values supplied by the caller; this function never
// exposes a host pointer.
int32_t    m2k_message_info(m2k_context* ctx, m2k_item_t msg,
                           uint32_t* reply_port, uint32_t* data_ptr,
                           uint32_t* data_size, int32_t* reply_code);
// Snapshot the guest-visible MsgPort fields: mp_Signal (owner wake mask) and
// mp_UserData. Returns M2K_ERR_BAD_ITEM for an unknown/stale port handle.
int32_t    m2k_msgport_info(m2k_context* ctx, m2k_item_t port,
                           uint32_t* signal_mask, uint32_t* userdata);

// CreateMsg(name, pri, replyPort) source contract (portfolio_os_m2
// msgport.h): mint a held Message item retaining msg_ReplyPort. The message
// is not queued anywhere until SendMsg()/SendSmallMsg() moves it. Returns the
// message item handle, or M2K_ITEM_NONE when the pool is exhausted.
m2k_item_t m2k_create_msg(m2k_context* ctx, m2k_item_t reply_port);
// Same Message contract for the recovered CreateMsgItem pool filler. It uses
// the original type-2 range first and the bounded host extension when the
// complete guest-visible Message pool is already occupied.
m2k_item_t m2k_create_msg_pool(m2k_context* ctx, m2k_item_t reply_port);

// SendMsg(port, msg, dataPtr, dataSize) source contract: queue an existing
// held message to a real message port, carrying the guest data EA and size on
// the message (msg_DataPtr/msg_DataSize) while preserving msg_ReplyPort.
// data_ea/size stay guest values; nothing is dereferenced. Returns M2K_OK,
// M2K_ERR_BAD_ITEM (bad port/message handle), or M2K_ERR_STATE (message not
// held by this path).
int32_t m2k_send_msg_item(m2k_context* ctx, m2k_item_t port, m2k_item_t msg,
                          uint32_t data_ea, uint32_t data_size);

// ---- signals -----------------------------------------------------------------
// bit_num >= 0: allocate that bit; -1: lowest free bit. Returns bit index.
// Guest-task callers use their own allocation mask; host callers without a
// current task retain the context compatibility mask used by the local tests.
int32_t    m2k_alloc_signal(m2k_context* ctx, int32_t bit_num);
void       m2k_free_signal(m2k_context* ctx, int32_t bit_num);
// Release a bit from a specific task. This is used when a host-side Item
// deletion must free a signal owned by a task that is not the deleting caller.
void       m2k_free_signal_for_task(m2k_context* ctx, m2k_item_t task,
                                    int32_t bit_num);
int32_t    m2k_send_signal(m2k_context* ctx, m2k_item_t task, int32_t bit);
// Take (pending & mask) from the calling task, or from the compatibility
// context queue when called by host code outside a task; 0 when unmatched.
int32_t    m2k_poll_signal(m2k_context* ctx, uint32_t mask);
// Block until any masked bit is pending, then consume+return those bits.
int32_t    m2k_wait_signal(m2k_context* ctx, uint32_t mask);
// One-bit variant: blocks for any matching bit, consumes and returns only the
// lowest one. Used where recovered guest code dispatches on exact single-bit
// matches and the vendor's SendSignal preemption would separate a send pair.
int32_t    m2k_wait_signal_one(m2k_context* ctx, uint32_t mask);
// Signal bits reserved (AllocSignal) by the calling task, or by the context
// compatibility allocator when no task is current. A wait mask fully covered
// by this set has a guest producer that must deliver through SendSignal, so
// WaitSignal blocks for it instead of synthesizing a wake.
uint32_t   m2k_signal_allocated_mask(m2k_context* ctx);

// ---- IOReqs -------------------------------------------------------------------
m2k_item_t m2k_create_ioreq(m2k_context* ctx);
m2k_item_t m2k_create_ioreq_va(m2k_context* ctx, const m2_tag_arg* tags,
                               uint32_t max_count);
// Return the device Item recorded by CreateIOReq, preserving generation/type
// validation under the kernel context lock.
int32_t    m2k_ioreq_device(m2k_context* ctx, m2k_item_t io,
                            m2k_item_t* device_out);
int32_t    m2k_ioreq_reply_port(m2k_context* ctx, m2k_item_t io,
                               m2k_item_t* reply_port_out);
int32_t    m2k_ioreq_set_user_data(m2k_context* ctx, m2k_item_t io,
                                   uint32_t user_data);
// Requires IDLE or COMPLETE; rejects in-flight requests.
int32_t    m2k_delete_ioreq(m2k_context* ctx, m2k_item_t io);
// Synchronous: IDLE -> COMPLETE inline, returns completion status.
int32_t    m2k_do_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg);
// Asynchronous arm: IDLE|COMPLETE -> COMPLETE (immediate backend this slice).
int32_t    m2k_send_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg);
// Two-phase asynchronous IO:
// Arms an IO request: IDLE|COMPLETE -> IN_FLIGHT, records what and arg.
int32_t    m2k_arm_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg);
// Arm a recurring timer-device metronome. The request stays IN_FLIGHT until
// AbortIO/DeleteIOReq and is discoverable only by its owner task's matching
// WaitSignal mask.
int32_t    m2k_arm_metronome(m2k_context* ctx, m2k_item_t io, uint32_t what,
                             uint32_t arg, uint32_t signal_mask,
                             uint64_t interval_usec);
// Arm a source-shaped one-shot timer request. The IOReq remains IN_FLIGHT
// until its deadline or AbortIO; completion uses the request's normal signal
// or reply-port contract.
int32_t    m2k_arm_timer(m2k_context* ctx, m2k_item_t io, uint32_t what,
                         uint32_t arg, uint64_t delay_usec);
// Arm a source-shaped VBL timer request. One-shot requests complete after the
// requested number of host-delivered VBL events; metronomes signal their owner
// and remain in flight until AbortIO.
int32_t    m2k_arm_vbl_timer(m2k_context* ctx, m2k_item_t io, uint32_t what,
                             uint32_t arg, uint32_t interval,
                             uint32_t signal_mask, int metronome);
// Deliver one authentic host VBL event to the guest kernel timer boundary and
// return the resulting source-owned 64-bit VBL count.
uint64_t   m2k_signal_vbl(m2k_context* ctx);
uint64_t   m2k_vblank_count(m2k_context* ctx);
// Snapshot one-shot VBL completions produced by the specified VBL event.
// Handles and actual counts are written up to `capacity` entries.
uint32_t   m2k_vbl_completions_at(m2k_context* ctx, uint64_t tick,
                                  m2k_item_t* io_out, uint32_t* actual_out,
                                  uint32_t capacity);
int32_t    m2k_ioreq_vbl_actual(m2k_context* ctx, m2k_item_t io,
                                uint32_t* actual_out);
// Rebuild host wall-clock deadlines for serialized source-shaped one-shot
// timer requests. Guest-visible request state and the requested interval stay
// in the portable extra-state record; only this host deadline is rebased.
void       m2k_rebase_timer_deadlines(m2k_context* ctx);
// Find an in-flight metronome owned by `task` whose signal intersects `mask`.
// Returns M2K_OK and the exact timer mask/interval, or M2K_ERR_EMPTY.
int32_t    m2k_metronome_for_wait(m2k_context* ctx, m2k_item_t task,
                                  uint32_t mask, uint32_t* signal_out,
                                  uint64_t* interval_usec_out);
// Completes an in-flight IO request: IN_FLIGHT -> COMPLETE, records status,
// delivers completion signal / reply message, and broadcasts condvar.
int32_t    m2k_complete_io(m2k_context* ctx, m2k_item_t io, int32_t status);
// Aborts an in-flight IO request: IN_FLIGHT -> COMPLETE (status -1 / aborted),
// delivers completion and broadcasts condvar.
int32_t    m2k_abort_io(m2k_context* ctx, m2k_item_t io);
// Blocks until COMPLETE, returns the stored status.
int32_t    m2k_wait_io(m2k_context* ctx, m2k_item_t io);
int32_t    m2k_query_io(m2k_context* ctx, m2k_item_t io,
                        int32_t* state_out, int32_t* status_out);

// ---- guest tasks ----------------------------------------------------------------
// m2k_create_task spawns a detached host thread that runs fn(ctx, arg) and
// classifies its return value (>= 0 completed, < 0 failed). m2k_create_task_ex
// is the extended form: the body receives an opaque user_data pointer instead
// of the uint32_t payload word; classification and lifecycle are identical.
// m2k_create_task remains as a thin wrapper over the same spawn path.
// m2k_join_task
// blocks on the context condvar until the terminal state and returns fn's
// status; it is safe to call repeatedly. m2k_delete_task frees the pool slot
// but refuses STARTED/RUNNING tasks (M2K_ERR_STATE), so a slot is never
// recycled under a live thread. All callers must be joined or deleted before
// m2k_destroy. Stale/foreign handles reject like every other pool.
m2k_item_t m2k_create_task(m2k_context* ctx, m2k_task_fn fn, uint32_t arg);
m2k_item_t m2k_create_task_ex(m2k_context* ctx, m2k_task_fn_ex fn_ex,
                              void* user_data);
m2k_item_t m2k_create_task_ex_priority(m2k_context* ctx,
                                       m2k_task_fn_ex fn_ex,
                                       void* user_data, uint8_t priority);
// CREATETASK_TAG_MSGFROMCHILD (kernel/task.h:248). Same spawn as
// m2k_create_task_ex_priority, plus the creator's message port: once the task
// publishes its terminal state the kernel queues one status message on
// `notify_port` whose msg_Result is the task's exit status, msg_DataPtr the
// terminated task's item, and msg_DataSize `notify_data` — the item of the
// thread or task that terminated it (the task itself when it exits on its
// own). Pass M2K_ITEM_NONE to create a task with no status message, which is
// what every other creation path does.
m2k_item_t m2k_create_task_ex_notify_priority(m2k_context* ctx,
                                             m2k_task_fn_ex fn_ex,
                                             void* user_data, uint8_t priority,
                                             m2k_item_t notify_port,
                                             m2k_item_t notify_data);
// Recreate the host execution machinery for a task record restored from a
// checkpoint only when the task was still STARTED (its body had not entered).
// The task slot/generation and guest-visible scalar state must already be
// present; this attaches a fresh detached pthread to that existing handle,
// without consuming a new generation. RUNNING tasks are rejected because this
// substrate does not serialize a native stack or an arbitrary continuation PC.
int32_t    m2k_rehydrate_task_ex(m2k_context* ctx, m2k_item_t task,
                                 m2k_task_fn_ex fn_ex, void* user_data);
// Attach a fresh detached pthread to a task restored in RUNNING state when a
// higher layer has separately proven an original lifted-function continuation
// point. This is intentionally distinct from m2k_rehydrate_task_ex(): it does
// not authorize entrypoint restart or infer a continuation from task state.
int32_t    m2k_rehydrate_task_ex_continuation(m2k_context* ctx,
                                              m2k_item_t task,
                                              m2k_task_fn_ex fn_ex,
                                              void* user_data);
// Guest-facing TaskNode support: return the item for the task currently
// executing on this context, and read the live signal word for a valid task
// item. Host callers without a current guest task get M2K_ITEM_NONE from the
// first function; both functions leave the task state unchanged.
m2k_item_t m2k_current_task_item(m2k_context* ctx);
int32_t    m2k_task_signal_bits(m2k_context* ctx, m2k_item_t task,
                                uint32_t* bits_out);
int32_t    m2k_task_priority(m2k_context* ctx, m2k_item_t task,
                             uint8_t* priority_out);
// kernel:157 SetItemPri(Item, uint8) -> item.c internalSetItemPriority ->
// item.c:1460 internalSetPriorityKernelItem -> task.c internalChangeTaskPri.
// Returns the PREVIOUS priority (>= 0) or a negative Portfolio Err:
//   M2K_ERR_BAD_ITEM  (vendor BADITEM, item.c:951)
//   M2K_ERR_BAD_PRIORITY (MAKEKERR(ER_SEVERE,ER_C_NSTND,ER_Kr_BadPriority),
//                        task.c:540)
// The source applies its 10..199 band only to tasks without
// n_ItemFlags ITEMNODE_PRIVILEGED (task.c:537-543); `privileged` carries that
// caller-side bit. The priority change itself is total: n_Priority is written
// and the ready queue is priority-sorted on read (m2k_ready_head_locked), which
// is this model's equivalent of the source's REMOVENODE + NewReadyTask
// re-insertion. Scheduling state (quantum, ready placement) is not disturbed.
int32_t    m2k_set_task_priority(m2k_context* ctx, m2k_item_t task,
                                uint8_t newpri, int privileged,
                                uint8_t* old_priority_out);

// Vendor taskswap.c equal-priority state as the host models it, for the
// focused quantum gate and for diagnosis. `timer_armed` is this task's loaded
// quantaclock; `ready` is the ready-order membership.
typedef struct m2k_task_quantum {
    uint64_t quantum;             // t_Quantum (TimerTicks)
    uint64_t quantum_remainder;   // t_QuantumRemainder
    uint64_t ready_seq;           // placement stamp while ready
    uint8_t  expired;             // TASK_QUANTUM_EXPIRED
    uint8_t  ready;               // in the ready-order set
    uint8_t  ready_at_head;       // landed with time remaining (head placement)
    uint8_t  timer_armed;         // the quantaclock is loaded for this task
} m2k_task_quantum;

int32_t    m2k_task_quantum_state(m2k_context* ctx, m2k_item_t task,
                                  m2k_task_quantum* out);
// Mark entry to the next guest-visible kernel/folio boundary. A SendSignal
// that wakes a strictly higher-priority waiter blocks only until this point,
// mirroring Portfolio's immediate reschedule without a general scheduler. The
// boundary is also where the quantaclock is observed (`_cblank`) and where an
// expired task yields to the ready-order head (`CheckForTaskSwap`).
void       m2k_task_note_kernel_boundary(m2k_context* ctx);
// Kernel-lock attribution diagnostic gate (M2_KERNEL_LOCK_DIAG); always
// callable, returns 0 unless the environment enabled the collector.
int        m2_kernel_lock_diag_enabled(void);
// Default task quantum (M2 SDK kernel/task.h DEFAULT_QUANTA_USECS) in the
// host's guest-visible TimerTicks unit.
#define M2K_QUANTUM_USECS 15000u
int32_t    m2k_query_task(m2k_context* ctx, m2k_item_t task,
                          int32_t* state_out, int32_t* status_out);
int32_t    m2k_join_task(m2k_context* ctx, m2k_item_t task);
int32_t    m2k_delete_task(m2k_context* ctx, m2k_item_t task);

// Portfolio public semaphore subset used by the lifted image. Lock returns
// 1 on acquisition, 0 for a nonwaiting miss, and contention is FIFO.
#define M2K_SEM_WAIT       1u
#define M2K_SEM_SHAREDREAD 2u
m2k_item_t m2k_create_semaphore(m2k_context* ctx);
int32_t    m2k_lock_semaphore(m2k_context* ctx, m2k_item_t semaphore,
                              uint32_t flags);
int32_t    m2k_unlock_semaphore(m2k_context* ctx, m2k_item_t semaphore);

// Reclaim the kernel Items owned by the calling guest task at the original
// task-exit boundary.  This is used by the lifted guest-task trampoline after
// the guest body has returned and before SIGF_DEADTASK is delivered to its
// owner.  Host compatibility tasks that intentionally retain their test Items
// do not call this boundary.
int32_t    m2k_reclaim_current_task_items(m2k_context* ctx);

#ifdef __cplusplus
}
#endif
