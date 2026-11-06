// guest_kernel.c — deterministic guest kernel runtime slice.
// See include/guest_kernel.h for the guest-safety contract. Everything lives
// in caller-provided static pools: no malloc, no host pointers handed out,
// identical op sequences yield identical handle streams.

// glibc declares Dl_info/dladdr only under _GNU_SOURCE; gnu11 alone does not
// define it. Must precede every system header.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "guest_kernel.h"
#include "diagnostic_gate.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

// The calling pthread identifies the guest task currently executing. Host
// callers leave this unset and use the context compatibility queue.
static _Thread_local m2k_task* g_current_task;
static _Thread_local m2k_context* g_current_context;

// M2_SIGNAL_TRACE is an opt-in diagnostic, not a logging sink. Keep one
// process-wide bound across send, WaitSignal, and WaitSignal-one records so
// a busy producer cannot consume an unbounded amount of output before the
// corresponding wait-side evidence is emitted.
static _Atomic uint32_t g_signal_trace_count;

static int m2k_signal_trace_take(void) {
    if (!getenv("M2_SIGNAL_TRACE")) return 0;
    const uint32_t n = atomic_fetch_add_explicit(&g_signal_trace_count, 1u,
                                                 memory_order_relaxed);
    return n < 256u;
}

// m2k_context is caller-owned storage and is commonly reinitialized in place
// by a restart/rehydration test. Keep the generation source outside that
// storage across a clean destroy/init pair; otherwise the first restarted
// port/task could reuse a stale handle verbatim. This is deliberately a small
// bounded registry, not a heap-backed lifetime system.
#define M2K_RESTART_RECORDS 32u
typedef struct {
    const m2k_context* ctx;
    uint32_t generation;
} m2k_restart_record;
static pthread_mutex_t g_restart_lock = PTHREAD_MUTEX_INITIALIZER;
static m2k_restart_record g_restart_records[M2K_RESTART_RECORDS];
static uint32_t g_restart_cursor;

static m2k_task* m2k_resolve_task_locked(m2k_context* ctx, m2k_item_t task);
static uint32_t m2k_make_handle(uint32_t type, uint32_t slot, uint32_t gen);
static int32_t m2k_append_message_locked(m2k_context* ctx, m2k_msgport* port,
                                         uint32_t index);
static int32_t m2k_io_complete_locked(m2k_context* ctx, m2k_ioreq* q,
                                      int32_t status);
static m2k_task* m2k_current_task_locked(m2k_context* ctx);

typedef struct m2k_ready_handoff {
    m2k_task* target;
    uint32_t generation;
    uint64_t boundary;
} m2k_ready_handoff;

// ---- Portfolio quantum scheduling (M2 SDK src/folios/kernel/taskswap.c) ---------
//
// taskswap.c fully specifies the equal-priority rule, so the host models its
// state at the host's kernel boundaries instead of leaving it to pthreads:
//
//   * per task, `t_Quantum` / `t_QuantumRemainder` / TASK_QUANTUM_EXPIRED;
//   * one quantaclock per task (`timerArmed` plus the loaded remainder), armed
//     by `Launch` when another ready task of equal priority exists and by
//     `NewReadyTask` arm (2a) when an equal-priority task becomes ready while
//     the sender still has time left;
//   * the ready order: a woken task joins at the tail (`NewReadyTask`), a
//     landed task with time remaining rejoins at the head (`LandReady`), and a
//     task that exhausted its quantum rejoins at the tail;
//   * `_cblank`/`CheckForTaskSwap`: the quantaclock marks the running task
//     expired at its next boundary, where it yields to the ready-order head.
//
// The host has no global scheduler, so "running" is per task: `run_started_ns`
// is that task's own launch, refreshed whenever it resumes from a kernel wait,
// and the ready order holds only tasks a wake has readied and not yet resumed.
// Modeled at those boundaries only; scheduler-stress construction is out of
// scope (the lifecycle/concurrency contract).
#define M2K_TICKS_PER_SECOND 8333333ull      // host time base = M2_TIME_BASE_TICKS_PER_SECOND
// M2K_QUANTUM_USECS (kernel/task.h DEFAULT_QUANTA_USECS) comes from the header.

static uint64_t m2k_monotonic_ns(void);
static uint64_t m2k_ticks_between_ns(uint64_t start_ns, uint64_t end_ns);
static void m2k_ready_insert_locked(m2k_context* ctx, m2k_task* t, int at_head);
static void m2k_ready_remove_locked(m2k_task* t);
static m2k_task* m2k_ready_head_locked(m2k_context* ctx, const m2k_task* skip);
static void m2k_task_land_ready_locked(m2k_context* ctx, m2k_task* t,
                                       uint64_t now_ns);
static void m2k_task_launch_locked(m2k_context* ctx, m2k_task* t,
                                   uint64_t now_ns);
static void m2k_arm_quantaclock_locked(m2k_task* t, uint64_t now_ns);
static void m2k_disarm_quantaclock_locked(m2k_task* t);

static m2k_ready_handoff m2k_ready_handoff_locked(m2k_context* ctx,
                                                   m2k_task* target) {
    const m2k_ready_handoff none = {0};
    m2k_task* sender = m2k_current_task_locked(ctx);
    if (!sender || !target || sender == target) return none;

    const uint64_t now_ns = m2k_monotonic_ns();
    // NewReadyTask(): a task the wake satisfies joins the ready order at the
    // tail whatever its priority; `Launch` removes it when it resumes.
    m2k_ready_insert_locked(ctx, target, 0);

    if (target->priority > sender->priority) {
        // Strictly higher: the awakened task preempts the sender outright.
        m2k_task_land_ready_locked(ctx, sender, now_ns);   // LandReady(ct)
        const m2k_ready_handoff handoff = {
            .target = target,
            .generation = target->generation,
            .boundary = target->kernel_boundary_seq,
        };
        return handoff;
    }
    if (target->priority < sender->priority) return none;

    // Equal priority: the three NewReadyTask arms that decide whether the
    // sender yields now or only when the quantaclock fires.
    int reschedule = 0;
    if (sender->quantum_expired) {
        reschedule = 1;                          // (1) already expired
    } else if (!sender->quantum_timer_armed) {
        const uint64_t elapsed = m2k_ticks_between_ns(sender->run_started_ns,
                                                      now_ns);
        // SubTimerTicks saturates: remainder - elapsed, floored at zero.
        sender->quantum_remainder = elapsed >= sender->quantum_remainder
                                        ? 0ull
                                        : sender->quantum_remainder - elapsed;
        if (sender->quantum_remainder != 0ull) {
            // (2a) less than a quantum: let it run, arm the quantaclock.
            m2k_arm_quantaclock_locked(sender, now_ns);
            return none;
        }
        sender->quantum_expired = 1;             // (2b) more than a quantum
        reschedule = 1;
    }
    // (3) timer already armed: the timer interrupt asks for the swap later.

    if (!reschedule) return none;
    m2k_task_land_ready_locked(ctx, sender, now_ns);     // LandReady(ct)
    m2k_task* launched = m2k_ready_head_locked(ctx, sender);
    if (!launched) return none;                 // SwapTasks(): no ready task
    const m2k_ready_handoff handoff = {
        .target = launched,
        .generation = launched->generation,
        .boundary = launched->kernel_boundary_seq,
    };
    return handoff;
}

static void m2k_wait_ready_handoff_locked(m2k_context* ctx,
                                           m2k_ready_handoff handoff) {
    if (!handoff.target) return;
    m2k_task* sender = m2k_current_task_locked(ctx);
    while (handoff.target->used &&
           handoff.target->generation == handoff.generation &&
           handoff.target->state != M2K_TASK_COMPLETED &&
           handoff.target->state != M2K_TASK_FAILED &&
           handoff.target->kernel_boundary_seq == handoff.boundary)
        m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
    // Launch(): the task that resumes here is the one that yielded.
    if (sender) m2k_task_launch_locked(ctx, sender, m2k_monotonic_ns());
}

static m2k_ready_handoff m2k_wake_port_owner_locked(
    m2k_context* ctx, const m2k_msgport* port);
static int m2k_deliver_to_waiter_locked(m2k_context* ctx, m2k_msgport* port,
                                        m2k_item_t port_item, uint32_t index,
                                        m2k_ready_handoff* handoff_out);

static m2k_task* m2k_current_task_locked(m2k_context* ctx) {
    if (g_current_context != ctx || !g_current_task ||
        g_current_task < ctx->tasks ||
        g_current_task >= ctx->tasks + M2K_MAX_TASKS ||
        !g_current_task->used)
        return NULL;
    return g_current_task;
}

static m2k_item_t m2k_task_handle_locked(const m2k_context* ctx,
                                         const m2k_task* task) {
    if (!task || task < ctx->tasks || task >= ctx->tasks + M2K_MAX_TASKS ||
        !task->used)
        return M2K_ITEM_NONE;
    return m2k_make_handle(M2K_TYPE_TASK, (uint32_t)(task - ctx->tasks),
                           task->generation);
}

// ---- quantum/ready-order helpers -------------------------------------------

static uint64_t m2k_quantum_ticks(void) {
    return ((uint64_t)M2K_QUANTUM_USECS * M2K_TICKS_PER_SECOND) /
           1000000ull;
}

// TimerTicks between two host monotonic samples, using the same conversion as
// the runtime's SampleSystemTimeTT time base.
static uint64_t m2k_ticks_between_ns(uint64_t start_ns, uint64_t end_ns) {
    const uint64_t delta = end_ns > start_ns ? end_ns - start_ns : 0ull;
    return (delta / 1000000000ull) * M2K_TICKS_PER_SECOND +
           ((delta % 1000000000ull) * M2K_TICKS_PER_SECOND) / 1000000000ull;
}

static uint64_t m2k_ns_for_ticks(uint64_t ticks) {
    return (ticks / M2K_TICKS_PER_SECOND) * 1000000000ull +
           ((ticks % M2K_TICKS_PER_SECOND) * 1000000000ull) /
               M2K_TICKS_PER_SECOND;
}

static int m2k_task_schedulable(const m2k_task* t) {
    return t->used && t->state != M2K_TASK_COMPLETED &&
           t->state != M2K_TASK_FAILED;
}

// Ready-order placement inside one priority: head-placed entries come first,
// newest insert first (InsertNodeFromHead puts each new node before the other
// equal-priority nodes), then tail-placed entries in FIFO order
// (InsertNodeFromTail puts each new node behind them).
static int m2k_ready_before(const m2k_task* a, const m2k_task* b) {
    if (a->ready_at_head != b->ready_at_head) return a->ready_at_head != 0;
    if (a->ready_seq == b->ready_seq) return 0;
    if (a->ready_at_head) return a->ready_seq > b->ready_seq;
    return a->ready_seq < b->ready_seq;
}

// RemHead() of the priority-sorted ready queue: highest priority first, then
// the placement above.
static m2k_task* m2k_ready_head_locked(m2k_context* ctx,
                                       const m2k_task* skip) {
    m2k_task* head = NULL;
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
        m2k_task* t = &ctx->tasks[i];
        if (t == skip || !t->ready || !m2k_task_schedulable(t)) continue;
        if (!head || t->priority > head->priority ||
            (t->priority == head->priority && m2k_ready_before(t, head)))
            head = t;
    }
    return head;
}

static int m2k_ready_has_equal_locked(const m2k_context* ctx,
                                      const m2k_task* t) {
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
        const m2k_task* peer = &ctx->tasks[i];
        if (peer == t || !peer->ready || !m2k_task_schedulable(peer))
            continue;
        if (peer->priority == t->priority) return 1;
    }
    return 0;
}

static void m2k_ready_insert_locked(m2k_context* ctx, m2k_task* t, int at_head) {
    if (!t->used || t->ready || !m2k_task_schedulable(t)) return;
    t->ready = 1;
    t->ready_at_head = at_head ? 1u : 0u;
    t->ready_seq = at_head ? ++ctx->ready_head_seq : ++ctx->ready_tail_seq;
}

static void m2k_ready_remove_locked(m2k_task* t) {
    t->ready = 0;
    t->ready_at_head = 0;
    t->ready_seq = 0;
}

static void m2k_disarm_quantaclock_locked(m2k_task* t) {
    t->quantum_timer_armed = 0;
    t->quantaclock_deadline_ns = 0;
}

// taskswap.c LoadTimer(_quantaclock, remainder): the countdown belongs to the
// task it was loaded for, so a peer's concurrent progress cannot clear or fire
// it (the vendor has one running task, and this is that task's timer).
static void m2k_arm_quantaclock_locked(m2k_task* t, uint64_t now_ns) {
    t->quantum_timer_armed = 1;
    t->quantaclock_deadline_ns = now_ns + m2k_ns_for_ticks(t->quantum_remainder);
}

// taskswap.c LandReady(): the running task is descheduled by the scheduler.
static void m2k_task_land_ready_locked(m2k_context* ctx, m2k_task* t,
                                       uint64_t now_ns) {
    if (t->quantum_expired) {
        t->quantum_remainder = t->quantum;
        t->quantum_expired = 0;
        m2k_ready_insert_locked(ctx, t, 0);      // InsertNodeFromTail
    } else {
        const uint64_t elapsed = m2k_ticks_between_ns(t->run_started_ns,
                                                      now_ns);
        t->quantum_remainder = elapsed >= t->quantum_remainder
                                   ? 0ull
                                   : t->quantum_remainder - elapsed;
        m2k_ready_insert_locked(ctx, t, 1);      // InsertNodeFromHead
    }
}

// taskswap.c LandWaiter(): a blocking wait restores the full quantum and
// clears the expiry flag. A waiting task cannot keep a loaded countdown.
static void m2k_task_block_locked(m2k_context* ctx, m2k_task* t) {
    (void)ctx;
    m2k_ready_remove_locked(t);
    t->quantum_remainder = t->quantum;
    t->quantum_expired = 0;
    m2k_disarm_quantaclock_locked(t);
}

// taskswap.c Launch(): the task becomes the running one. The quantaclock is
// loaded only when another ready task of equal priority exists, exactly as
// Launch does; it is unloaded when none does.
static void m2k_task_launch_locked(m2k_context* ctx, m2k_task* t,
                                   uint64_t now_ns) {
    m2k_ready_remove_locked(t);
    t->run_started_ns = now_ns;
    if (m2k_ready_has_equal_locked(ctx, t))
        m2k_arm_quantaclock_locked(t, now_ns);
    else
        m2k_disarm_quantaclock_locked(t);
}

static uint32_t m2k_make_handle(uint32_t type, uint32_t slot, uint32_t gen) {
    return (gen << M2K_HANDLE_GEN_SHIFT) |
           ((slot << M2K_HANDLE_SLOT_SHIFT) & M2K_HANDLE_SLOT_MASK) |
           (type & M2K_HANDLE_TYPE_MASK);
}

static uint32_t m2k_take_restart_generation(const m2k_context* ctx) {
    uint32_t generation = 0;
    pthread_mutex_lock(&g_restart_lock);
    for (uint32_t i = 0; i < M2K_RESTART_RECORDS; ++i) {
        if (g_restart_records[i].ctx == ctx) {
            generation = g_restart_records[i].generation;
            g_restart_records[i].ctx = NULL;
            g_restart_records[i].generation = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_restart_lock);
    return generation;
}

static void m2k_record_restart_generation(const m2k_context* ctx,
                                          uint32_t generation) {
    pthread_mutex_lock(&g_restart_lock);
    uint32_t slot = M2K_RESTART_RECORDS;
    for (uint32_t i = 0; i < M2K_RESTART_RECORDS; ++i) {
        if (g_restart_records[i].ctx == ctx) {
            slot = i;
            break;
        }
        if (slot == M2K_RESTART_RECORDS && !g_restart_records[i].ctx)
            slot = i;
    }
    if (slot == M2K_RESTART_RECORDS)
        slot = g_restart_cursor++ % M2K_RESTART_RECORDS;
    g_restart_records[slot].ctx = ctx;
    g_restart_records[slot].generation = generation;
    pthread_mutex_unlock(&g_restart_lock);
}

// --- bounded kernel-lock attribution (M2_KERNEL_LOCK_DIAG) -----------------
// Diagnostic-only. Attributes each blocked acquisition of ctx->lock to the
// call site of the observed holder, and counts acquisitions per site, so a
// wait-heavy path can name what actually holds the kernel lock. Off unless the
// environment sets M2_KERNEL_LOCK_DIAG; the default path stays one predictable
// branch to the plain mutex. Requires m2k_lock to be out of line so the site
// is the caller that takes the lock (see the noinline attribute below).
#define M2K_LOCK_DIAG_SITES 128u
typedef struct m2k_lock_diag_site {
    void*    pc;
    uint64_t acquisitions;
    uint64_t waits;
    uint64_t wait_ns;          // blocked time while this site was the waiter
    uint64_t owner_waits;      // blocked acquisitions attributed to this holder
    uint64_t owner_wait_ns;
    uint64_t max_owner_wait_ns;
} m2k_lock_diag_site;

static m2k_lock_diag_site g_lock_diag_sites[M2K_LOCK_DIAG_SITES];
static int g_lock_diag_state = -1;  // -1 unchecked, 0 off, 1 on
static atomic_uintptr_t g_lock_diag_owner;
static uint64_t g_lock_diag_total_acq;
static uint64_t g_lock_diag_total_waits;
static uint64_t g_lock_diag_total_wait_ns;
static _Thread_local void* g_lock_diag_last_pc;
static _Thread_local uint32_t g_lock_diag_last_slot;

static uint64_t m2k_lock_diag_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t m2k_lock_diag_slot(void* pc) {
    if (pc == g_lock_diag_last_pc) return g_lock_diag_last_slot;
    const uintptr_t key = (uintptr_t)pc >> 3;
    const uint32_t base =
        (uint32_t)((key * 2654435761u) & (M2K_LOCK_DIAG_SITES - 1u));
    for (uint32_t probe = 0; probe < M2K_LOCK_DIAG_SITES; ++probe) {
        const uint32_t i = (base + probe) & (M2K_LOCK_DIAG_SITES - 1u);
        if (g_lock_diag_sites[i].pc == pc || g_lock_diag_sites[i].pc == NULL) {
            if (g_lock_diag_sites[i].pc == NULL) g_lock_diag_sites[i].pc = pc;
            g_lock_diag_last_pc = pc;
            g_lock_diag_last_slot = i;
            return i;
        }
    }
    return 0u;
}

static void m2k_lock_diag_symbol(void* pc, char* out, size_t out_size) {
#if !defined(_WIN32)
    Dl_info info;
    if (pc && dladdr(pc, &info) && info.dli_sname) {
        snprintf(out, out_size, "%s+%#lx", info.dli_sname,
                 (unsigned long)((uintptr_t)pc - (uintptr_t)info.dli_saddr));
        return;
    }
#endif
    snprintf(out, out_size, "%p", pc);
}

void m2_guest_kernel_lock_diag_dump(void) {
    char name[160];
    fprintf(stderr,
                 "[kernel-lock] acquisitions=%llu blocked=%llu wait_total_ms=%.1f\n",
                 (unsigned long long)g_lock_diag_total_acq,
                 (unsigned long long)g_lock_diag_total_waits,
                 (double)g_lock_diag_total_wait_ns / 1e6);
    for (int rank = 0; rank < 12; ++rank) {
        m2k_lock_diag_site* best = NULL;
        for (uint32_t i = 0; i < M2K_LOCK_DIAG_SITES; ++i) {
            m2k_lock_diag_site* s = &g_lock_diag_sites[i];
            if (!s->pc || !s->owner_waits) continue;
            if (!best || s->owner_wait_ns > best->owner_wait_ns) best = s;
        }
        if (!best || !best->owner_wait_ns) break;
        m2k_lock_diag_symbol(best->pc, name, sizeof(name));
        fprintf(stderr,
                     "[kernel-lock] holder[%d] %s acq=%llu owner_waits=%llu "
                     "owner_wait_ms=%.1f max_us=%.1f\n",
                     rank, name, (unsigned long long)best->acquisitions,
                     (unsigned long long)best->owner_waits,
                     (double)best->owner_wait_ns / 1e6,
                     (double)best->max_owner_wait_ns / 1e3);
        best->owner_wait_ns = 0;  // consumed; keep totals in the first line
    }
    for (int rank = 0; rank < 12; ++rank) {
        m2k_lock_diag_site* best = NULL;
        for (uint32_t i = 0; i < M2K_LOCK_DIAG_SITES; ++i) {
            m2k_lock_diag_site* s = &g_lock_diag_sites[i];
            if (!s->pc || !s->waits) continue;
            if (!best || s->wait_ns > best->wait_ns) best = s;
        }
        if (!best || !best->wait_ns) break;
        m2k_lock_diag_symbol(best->pc, name, sizeof(name));
        fprintf(stderr,
                     "[kernel-lock] waiter[%d] %s acq=%llu waits=%llu "
                     "wait_ms=%.1f\n",
                     rank, name, (unsigned long long)best->acquisitions,
                     (unsigned long long)best->waits,
                     (double)best->wait_ns / 1e6);
        best->wait_ns = 0;
    }
}

// Out of line so __builtin_return_address(0) names the lock site itself
// (marked noinline below) rather than an enclosing inlined caller.
int m2_kernel_lock_diag_enabled(void) {
    if (g_lock_diag_state < 0) {
        g_lock_diag_state = getenv("M2_KERNEL_LOCK_DIAG") ? 1 : 0;
        if (g_lock_diag_state)
            fprintf(stderr, "[kernel-lock] diag enabled (M2_KERNEL_LOCK_DIAG)\n");
    }
    return g_lock_diag_state;
}

static int32_t m2k_lock_diagnostic(m2k_context* ctx, void* site) {
    if (!m2_kernel_lock_diag_enabled()) {
        pthread_mutex_lock(&ctx->lock);
        return M2K_OK;
    }
    if (pthread_mutex_trylock(&ctx->lock) == 0) {
        ++g_lock_diag_total_acq;
        ++g_lock_diag_sites[m2k_lock_diag_slot(site)].acquisitions;
        atomic_store_explicit(&g_lock_diag_owner, (uintptr_t)site,
                              memory_order_relaxed);
        return M2K_OK;
    }
    const uintptr_t owner =
        atomic_load_explicit(&g_lock_diag_owner, memory_order_relaxed);
    const uint64_t start = m2k_lock_diag_now_ns();
    pthread_mutex_lock(&ctx->lock);
    const uint64_t wait_ns = m2k_lock_diag_now_ns() - start;
    const uint32_t slot = m2k_lock_diag_slot(site);
    ++g_lock_diag_total_acq;
    ++g_lock_diag_total_waits;
    g_lock_diag_total_wait_ns += wait_ns;
    ++g_lock_diag_sites[slot].acquisitions;
    ++g_lock_diag_sites[slot].waits;
    g_lock_diag_sites[slot].wait_ns += wait_ns;
    if (owner) {
        const uint32_t owner_slot = m2k_lock_diag_slot((void*)owner);
        ++g_lock_diag_sites[owner_slot].owner_waits;
        g_lock_diag_sites[owner_slot].owner_wait_ns += wait_ns;
        if (wait_ns > g_lock_diag_sites[owner_slot].max_owner_wait_ns)
            g_lock_diag_sites[owner_slot].max_owner_wait_ns = wait_ns;
    }
    atomic_store_explicit(&g_lock_diag_owner, (uintptr_t)site,
                          memory_order_relaxed);
    return M2K_OK;
}

__attribute__((noinline)) static int32_t m2k_lock(m2k_context* ctx) {
    if (!ctx || !ctx->initialized) return M2K_ERR_BAD_ITEM;
    if (__builtin_expect(g_lock_diag_state == 0, 1)) {
        pthread_mutex_lock(&ctx->lock);
        return M2K_OK;
    }
    return m2k_lock_diagnostic(ctx, __builtin_return_address(0));
}

#define M2K_MESSAGE_CAPACITY (M2K_MAX_MESSAGES + M2K_MAX_EXT_MESSAGES)

static uint32_t m2k_message_index_from_handle(m2k_item_t h) {
    const uint32_t type = m2k_handle_type(h);
    const uint32_t slot = m2k_handle_slot(h);
    if (type == M2K_TYPE_MESSAGE && slot < M2K_MAX_MESSAGES)
        return slot;
    if (type == M2K_TYPE_MESSAGE_EXT && slot < M2K_MESSAGE_BANK_SIZE)
        return M2K_MAX_MESSAGES + slot;
    if (type == M2K_TYPE_MESSAGE_EXT2 && slot < M2K_MESSAGE_BANK_SIZE)
        return M2K_MAX_MESSAGES + M2K_MESSAGE_BANK_SIZE + slot;
    if (type == M2K_TYPE_MESSAGE_EXT3 && slot < M2K_MESSAGE_BANK_SIZE)
        return M2K_MAX_MESSAGES + 2u * M2K_MESSAGE_BANK_SIZE + slot;
    return UINT32_MAX;
}

static m2k_message* m2k_message_at_index(m2k_context* ctx, uint32_t index) {
    if (index < M2K_MAX_MESSAGES) return &ctx->msgs[index];
    if (index < M2K_MESSAGE_CAPACITY)
        return &ctx->ext_msgs[index - M2K_MAX_MESSAGES];
    return NULL;
}

static m2k_item_t m2k_message_handle_for_index(uint32_t index,
                                               uint32_t generation) {
    if (index < M2K_MAX_MESSAGES)
        return m2k_make_handle(M2K_TYPE_MESSAGE, index, generation);
    if (index < M2K_MAX_MESSAGES + M2K_MESSAGE_BANK_SIZE)
        return m2k_make_handle(M2K_TYPE_MESSAGE_EXT,
                               index - M2K_MAX_MESSAGES, generation);
    if (index < M2K_MAX_MESSAGES + 2u * M2K_MESSAGE_BANK_SIZE)
        return m2k_make_handle(M2K_TYPE_MESSAGE_EXT2,
                               index - M2K_MAX_MESSAGES -
                                   M2K_MESSAGE_BANK_SIZE,
                               generation);
    if (index < M2K_MESSAGE_CAPACITY)
        return m2k_make_handle(M2K_TYPE_MESSAGE_EXT3,
                               index - M2K_MAX_MESSAGES -
                                   2u * M2K_MESSAGE_BANK_SIZE,
                               generation);
    return M2K_ITEM_NONE;
}

// Queue refs preserve -1 as the sole empty sentinel. Extension descriptors
// are encoded as -2, -3, ... in the same int32_t FIFO link used by the
// original pool, so no guest-visible message handle or port layout changes.
static int32_t m2k_message_queue_ref(uint32_t index) {
    if (index < M2K_MAX_MESSAGES) return (int32_t)index;
    if (index < M2K_MESSAGE_CAPACITY)
        return -2 - (int32_t)(index - M2K_MAX_MESSAGES);
    return -1;
}

static uint32_t m2k_message_index_from_queue_ref(int32_t ref) {
    if (ref >= 0)
        return (uint32_t)ref < M2K_MAX_MESSAGES ? (uint32_t)ref : UINT32_MAX;
    if (ref <= -2 && ref >= -(int32_t)(M2K_MAX_EXT_MESSAGES + 1u)) {
        const uint32_t ext_slot = (uint32_t)(-2 - ref);
        return M2K_MAX_MESSAGES + ext_slot;
    }
    return UINT32_MAX;
}

static m2k_message* m2k_message_from_queue_ref(m2k_context* ctx,
                                                int32_t ref,
                                                uint32_t* index_out) {
    const uint32_t index = m2k_message_index_from_queue_ref(ref);
    m2k_message* m = m2k_message_at_index(ctx, index);
    if (index_out) *index_out = index;
    return m;
}

static uint32_t m2k_find_free_message_index_limit(const m2k_context* ctx,
                                                 uint32_t limit) {
    if (limit > M2K_MESSAGE_CAPACITY) limit = M2K_MESSAGE_CAPACITY;
    for (uint32_t i = 0; i < limit; ++i) {
        const m2k_message* m = i < M2K_MAX_MESSAGES
                                   ? &ctx->msgs[i]
                                   : &ctx->ext_msgs[i - M2K_MAX_MESSAGES];
        if (m->state == M2K_MSG_FREE) return i;
    }
    return UINT32_MAX;
}

static uint32_t m2k_find_free_message_index(const m2k_context* ctx) {
    return m2k_find_free_message_index_limit(ctx, M2K_MESSAGE_CAPACITY);
}

// Caller holds ctx->lock. The caller has already populated the message and
// will set its state before/after this link operation as appropriate.
static int32_t m2k_append_message_locked(m2k_context* ctx, m2k_msgport* port,
                                         uint32_t index) {
    const int32_t ref = m2k_message_queue_ref(index);
    if (ref == -1 || !m2k_message_at_index(ctx, index)) return M2K_ERR_BAD_ITEM;
    if (port->tail == -1) {
        port->head = ref;
    } else {
        m2k_message* tail = m2k_message_from_queue_ref(ctx, port->tail, NULL);
        if (!tail) return M2K_ERR_STATE;
        tail->next = ref;
    }
    port->tail = ref;
    port->queued_count++;
    return M2K_OK;
}

// Caller holds ctx->lock. Remove one queued message, including a message that
// is not the FIFO head. Portfolio WaitPort(port,msg) waits for the named item
// and removes that item, while GetMsg/WaitPort(port,0) remove the head.
static int32_t m2k_take_message_locked(m2k_context* ctx, m2k_msgport* port,
                                       int32_t target_ref,
                                       m2k_item_t* msg_out,
                                       uint32_t* what_out,
                                       uint32_t* value_out) {
    int32_t previous_ref = -1;
    int32_t current_ref = port->head;
    while (current_ref != -1) {
        uint32_t mi = UINT32_MAX;
        m2k_message* m = m2k_message_from_queue_ref(ctx, current_ref, &mi);
        if (!m) return M2K_ERR_STATE;
        if (current_ref == target_ref) {
            const int32_t next_ref = m->next;
            if (previous_ref == -1) {
                port->head = next_ref;
            } else {
                m2k_message* previous =
                    m2k_message_from_queue_ref(ctx, previous_ref, NULL);
                if (!previous) return M2K_ERR_STATE;
                previous->next = next_ref;
            }
            if (port->tail == current_ref) port->tail = previous_ref;
            if (port->queued_count == 0) return M2K_ERR_STATE;
            --port->queued_count;

            // A message delivered back through its reply port is held again
            // by its original sender; a normal receive is dispatched until
            // ReplyMsg.
            if (m->return_to_sender) {
                m->return_to_sender = 0;
                m->state = M2K_MSG_HELD;
            } else {
                m->state = M2K_MSG_DISPATCHED;
            }
            m->holder = m2k_task_handle_locked(ctx,
                                               m2k_current_task_locked(ctx));
            m->next = -1;

            if (msg_out)
                *msg_out = m2k_message_handle_for_index(mi, m->generation);
            if (what_out) *what_out = m->what;
            if (value_out) *value_out = m->value;
            return M2K_OK;
        }
        previous_ref = current_ref;
        current_ref = m->next;
    }
    return M2K_ERR_EMPTY;
}

// Caller holds ctx->lock. Remove a message from whichever port FIFO currently
// owns it without changing the message state. DeleteIOReq uses this for an
// already-completed reply message: freeing the descriptor without unlinking
// the FIFO node would leave a stale queue reference that could later corrupt
// GetMsg/WaitPort.
static int32_t m2k_unlink_queued_message_locked(m2k_context* ctx,
                                                uint32_t target_index) {
    const int32_t target_ref = m2k_message_queue_ref(target_index);
    if (target_ref == -1) return M2K_ERR_BAD_ITEM;
    for (uint32_t pi = 0; pi < M2K_MAX_PORTS; ++pi) {
        m2k_msgport* port = &ctx->ports[pi];
        if (!port->used || port->head == -1) continue;
        int32_t previous_ref = -1;
        int32_t current_ref = port->head;
        while (current_ref != -1) {
            m2k_message* current =
                m2k_message_from_queue_ref(ctx, current_ref, NULL);
            if (!current) return M2K_ERR_STATE;
            if (current_ref == target_ref) {
                const int32_t next_ref = current->next;
                if (previous_ref == -1) {
                    port->head = next_ref;
                } else {
                    m2k_message* previous =
                        m2k_message_from_queue_ref(ctx, previous_ref, NULL);
                    if (!previous) return M2K_ERR_STATE;
                    previous->next = next_ref;
                }
                if (port->tail == current_ref) port->tail = previous_ref;
                if (port->queued_count == 0) return M2K_ERR_STATE;
                --port->queued_count;
                current->next = -1;
                current->origin_port = -1;
                return M2K_OK;
            }
            previous_ref = current_ref;
            current_ref = current->next;
        }
    }
    return M2K_ERR_EMPTY;
}

// Port/IOReq resolver: validates type, slot range and generation against the
// entry's {generation, used} prefix.
static void* m2k_resolve_item(m2k_context* ctx, m2k_item_t h,
                              uint32_t want_type, void* pool,
                              uint32_t pool_len, size_t entry_size) {
    if (m2k_handle_type(h) != want_type) return NULL;
    uint32_t slot = m2k_handle_slot(h);
    if (slot >= pool_len) return NULL;
    typedef struct { uint32_t generation; uint8_t used; } m2k_hdr;
    m2k_hdr* e = (m2k_hdr*)((uint8_t*)pool + slot * entry_size);
    if (!e->used || e->generation != m2k_handle_gen(h)) return NULL;
    return (void*)e;
}

// Message resolver: alive = not FREE with a matching pinned generation.
static m2k_message* m2k_resolve_msg(m2k_context* ctx, m2k_item_t h) {
    if (!m2k_is_message_handle(h)) return NULL;
    const uint32_t index = m2k_message_index_from_handle(h);
    m2k_message* m = m2k_message_at_index(ctx, index);
    if (!m) return NULL;
    if (m->state == M2K_MSG_FREE || m->generation != m2k_handle_gen(h))
        return NULL;
    return m;
}

// ReplyMsg returns a completed message to its holder, who may re-send the
// same item (the streaming library reuses one control message for connect /
// subscribe / start requests). The resolver intentionally accepts FREE
// messages so callers can distinguish a stale generation from a state error;
// SendMsg still requires M2K_MSG_HELD before it can queue the item.
static m2k_message* m2k_resolve_msg_any(m2k_context* ctx, m2k_item_t h) {
    if (!m2k_is_message_handle(h)) return NULL;
    const uint32_t index = m2k_message_index_from_handle(h);
    m2k_message* m = m2k_message_at_index(ctx, index);
    if (!m) return NULL;
    if (m->generation != m2k_handle_gen(h)) return NULL;
    return m;
}

// Caller holds ctx->lock. This is the lock-held form of ReplyMsg used by the
// public selector path and by the kernel's task-exit cleanup. It mirrors
// internalReplyMsg()/internalDeliverMsg(): a valid retained reply port gets
// the same message back; a compact host-only message without one is released
// after recording its result.
static int32_t m2k_reply_message_locked(m2k_context* ctx, m2k_item_t msg,
                                        m2k_message* m, int32_t reply_code) {
    m->reply_code = reply_code;
    m->state = M2K_MSG_REPLIED;

    // Deliver the reply code back to the originating port when it survives.
    if (m->origin_port >= 0 && (uint32_t)m->origin_port < M2K_MAX_PORTS) {
        m2k_msgport* op = &ctx->ports[m->origin_port];
        if (op->used) op->last_reply_code = reply_code;
    }

    // Portfolio ReplyMsg returns a message with a retained reply port to that
    // port: the waiter's WaitPort(port, msg) dequeues the same item and holds
    // it again for re-send.
    m2k_msgport* rp = NULL;
    if (m->reply_port != M2K_ITEM_NONE &&
        m2k_handle_type(m->reply_port) == M2K_TYPE_MSGPORT) {
        const uint32_t pslot = m2k_handle_slot(m->reply_port);
        if (pslot < M2K_MAX_PORTS) {
            m2k_msgport* cand = &ctx->ports[pslot];
            if (cand->used &&
                cand->generation == m2k_handle_gen(m->reply_port))
                rp = cand;
        }
    }
    if (rp) {
        const uint32_t mi = m2k_message_index_from_handle(msg);
        if (mi == UINT32_MAX) return M2K_ERR_BAD_ITEM;
        m2k_ready_handoff handoff = {0};
        m->origin_port = -1;
        m->next = -1;
        m->return_to_sender = 1;
        if (m2k_deliver_to_waiter_locked(ctx, rp, m->reply_port, mi,
                                         &handoff)) {
            m2k_wait_ready_handoff_locked(ctx, handoff);
            return M2K_OK;
        }
        const int32_t append_rc = m2k_append_message_locked(ctx, rp, mi);
        if (append_rc != M2K_OK) {
            m->state = M2K_MSG_REPLIED;
            return append_rc;
        }
        m->state = M2K_MSG_QUEUED;
        handoff = m2k_wake_port_owner_locked(ctx, rp);
        pthread_cond_broadcast(&ctx->cond);
        m2k_wait_ready_handoff_locked(ctx, handoff);
        return M2K_OK;
    }

    // A compact message has no retained reply port in the host compatibility
    // path. Keep the result for the caller that already owns the descriptor,
    // then release the pool slot just as the existing adapter expects.
    m->state = M2K_MSG_FREE;
    m->owner = M2K_ITEM_NONE;
    m->holder = M2K_ITEM_NONE;
    m->origin_port = -1;
    m->what = 0;
    m->value = 0;
    m->data_ptr = 0;
    m->data_size = 0;
    m->next = -1;
    return M2K_OK;
}

void m2k_init(m2k_context* ctx) {
    if (!ctx) return;
    const uint32_t prior_generation = m2k_take_restart_generation(ctx);
    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->lock, NULL);
#if defined(__APPLE__) || defined(_WIN32)
    // Darwin provides a relative monotonic wait but no condattr clock setter;
    // winpthreads waits on the wall clock. diagnostic_gate.c converts the
    // monotonic deadline for both, so the cond keeps default attributes.
    pthread_cond_init(&ctx->cond, NULL);
#else
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&ctx->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
#endif
    for (uint32_t i = 0; i < M2K_MAX_PORTS; ++i) {
        ctx->ports[i].head = -1;
        ctx->ports[i].tail = -1;
        ctx->ports[i].last_reply_code = 0;
    }
    for (uint32_t i = 0; i < M2K_MAX_MESSAGES; ++i) {
        ctx->msgs[i].next = -1;
        ctx->msgs[i].origin_port = -1;
        ctx->msgs[i].owner = M2K_ITEM_NONE;
        ctx->msgs[i].holder = M2K_ITEM_NONE;
    }
    for (uint32_t i = 0; i < M2K_MAX_EXT_MESSAGES; ++i) {
        ctx->ext_msgs[i].next = -1;
        ctx->ext_msgs[i].origin_port = -1;
        ctx->ext_msgs[i].owner = M2K_ITEM_NONE;
        ctx->ext_msgs[i].holder = M2K_ITEM_NONE;
    }
    // The host EventBroker descriptor publishes its own broker-owned
    // semaphore Item as 0x7400. Keep that external source object live and
    // unlocked; guest CreateSemaphore allocations begin at the next slot.
    ctx->semaphores[0].used = 1;
    ctx->gen_counter = prior_generation;
    ctx->initialized = 1;
}

void m2k_destroy(m2k_context* ctx) {
    if (!ctx || !ctx->initialized) return;
    pthread_mutex_lock(&ctx->lock);
    const uint32_t generation = ctx->gen_counter;
    ctx->initialized = 0;
    pthread_mutex_unlock(&ctx->lock);
    m2k_record_restart_generation(ctx, generation);
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cond);
}

// ---- message ports -----------------------------------------------------------

static int32_t m2k_free_port_slot(const m2k_context* ctx) {
    for (uint32_t i = 0; i < M2K_MAX_PORTS; ++i)
        if (!ctx->ports[i].used) return (int32_t)i;
    return -1;
}

// Portfolio SendMsg delivers the port's mp_Signal to the owner task once the
// message is queued. The recovered DataStream/DataAcq threads wait on the OR
// of their ports' signals, so the send path must set the bit and broadcast.
// Caller holds ctx->lock.
static m2k_ready_handoff m2k_wake_port_owner_locked(
    m2k_context* ctx, const m2k_msgport* p) {
    const m2k_ready_handoff none = {0};
    if (!p || p->signal_mask == 0) return none;
    const uint32_t mask = p->signal_mask & ~M2K_SIGNAL_RESERVED_MASK;
    if (mask == 0) return none;
    m2k_task* owner = NULL;
    if (p->owner != M2K_ITEM_NONE)
        owner = m2k_resolve_task_locked(ctx, p->owner);
    if (owner) {
        const int wakes_waiter = owner->signal_wait_active &&
                                 (owner->signal_wait_mask & mask) != 0u;
        owner->sig_pending |= mask;
        pthread_cond_broadcast(&ctx->cond);
        return wakes_waiter ? m2k_ready_handoff_locked(ctx, owner) : none;
    } else {
        ctx->sig_pending |= mask;
        for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
            m2k_task* waiter = &ctx->tasks[i];
            if (waiter->used && waiter->state != M2K_TASK_COMPLETED &&
                waiter->state != M2K_TASK_FAILED)
                waiter->sig_pending |= mask;
        }
    }
    pthread_cond_broadcast(&ctx->cond);
    return none;
}

// Portfolio's internalDeliverMsg does not enqueue a message when the owner is
// asleep in WaitPort: it transfers the message straight to that task's held
// list and wakes the task. In particular, this path does not raise the port's
// signal bit. Modeling that distinction matters to the streaming library,
// which uses WaitPort for synchronous request replies and then WaitSignal for
// later status/frame events.
// Caller holds ctx->lock. Returns non-zero when the message was delivered
// directly to an active waiter.
static int m2k_deliver_to_waiter_locked(m2k_context* ctx, m2k_msgport* p,
                                        m2k_item_t port, uint32_t index,
                                        m2k_ready_handoff* handoff_out) {
    if (!p || p->owner == M2K_ITEM_NONE ||
        p->owner == M2K_TASK_SELF)
        return 0;
    m2k_task* task = m2k_resolve_task_locked(ctx, p->owner);
    m2k_message* msg = m2k_message_at_index(ctx, index);
    if (!task || !msg || !task->wait_active || task->wait_port != port)
        return 0;

    const m2k_item_t handle =
        m2k_message_handle_for_index(index, msg->generation);
    if (task->wait_msg != M2K_ITEM_NONE && task->wait_msg != handle)
        return 0;

    // A ReplyMsg delivery returns the message to its original holder, where
    // it is held again for re-send. A normal SendMsg delivery is dispatched
    // to the receiving task and remains replyable by that task.
    if (msg->return_to_sender) {
        msg->return_to_sender = 0;
        msg->state = M2K_MSG_HELD;
    } else {
        msg->state = M2K_MSG_DISPATCHED;
    }
    msg->holder = m2k_task_handle_locked(ctx, task);
    msg->origin_port = -1;
    msg->next = -1;
    task->wait_result = handle;
    task->wait_active = 0;
    if (handoff_out)
        *handoff_out = m2k_ready_handoff_locked(ctx, task);
    pthread_cond_broadcast(&ctx->cond);
    return 1;
}

m2k_item_t m2k_create_msgport_va(m2k_context* ctx, const m2_tag_arg* tags,
                                 uint32_t max_count) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;

    // Bounded TagArg scan: NULL tolerated (= defaults), unknown tags ignored,
    // entries behind max_count or M2_TAG_END invisible per the shared
    // m2_tag_* helpers.
    uint32_t priority = 0;
    int32_t ti = m2_tag_find(tags, max_count, M2K_TAG_PORT_PRIORITY);
    if (ti >= 0) priority = tags[ti].value;
    uint32_t requested_signal = 0;
    ti = m2_tag_find(tags, max_count, M2K_TAG_PORT_SIGNAL);
    if (ti >= 0) requested_signal = tags[ti].value;
    uint32_t userdata = 0;
    ti = m2_tag_find(tags, max_count, M2K_TAG_PORT_USERDATA);
    if (ti >= 0) userdata = tags[ti].value;

    int32_t slot = m2k_free_port_slot(ctx);
    if (slot < 0) { pthread_mutex_unlock(&ctx->lock); return M2K_ITEM_NONE; }

    m2k_msgport* p = &ctx->ports[slot];
    p->used = 1;
    p->generation = ++ctx->gen_counter;   // 1-based, monotonic per context
    p->priority = priority;
    p->head = -1;
    p->tail = -1;
    p->queued_count = 0;
    p->last_reply_code = 0;
    p->userdata = userdata;

    // mp_Signal contract (msgport.h): an explicit CREATEPORT_TAG_SIGNAL wins;
    // otherwise the kernel auto-allocates one bit for the owning task and
    // marks it so deletion releases it. Sends to the port set that bit pending
    // on the owner, which is how the recovered DataStream threads wake.
    m2k_task* owner_task = m2k_current_task_locked(ctx);
    p->owner = M2K_ITEM_NONE;
    if (owner_task) {
        const uint32_t owner_slot = (uint32_t)(owner_task - ctx->tasks);
        p->owner = m2k_make_handle(M2K_TYPE_TASK, owner_slot,
                                   owner_task->generation);
    }
    uint32_t* allocated = owner_task ? &owner_task->sig_allocated
                                     : &ctx->sig_allocated;
    if (requested_signal != 0 && (requested_signal & (requested_signal - 1u)) == 0) {
        p->signal_mask = requested_signal;
        p->signal_allocated = 0;
    } else {
        p->signal_mask = 0;
        p->signal_allocated = 0;
        for (uint32_t bit = 8; bit < M2K_SIG_BITS; ++bit) {
            if (!(*allocated & (1u << bit))) {
                *allocated |= 1u << bit;
                p->signal_mask = 1u << bit;
                p->signal_allocated = 1;
                break;
            }
        }
    }

    m2k_item_t h = m2k_make_handle(M2K_TYPE_MSGPORT, (uint32_t)slot, p->generation);
    pthread_mutex_unlock(&ctx->lock);
    return h;
}

m2k_item_t m2k_create_msgport(m2k_context* ctx) {
    return m2k_create_msgport_va(ctx, NULL, 0);
}

int32_t m2k_delete_msgport(m2k_context* ctx, m2k_item_t port) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (p->queued_count != 0 || p->head != -1) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // refuse to drop queued messages silently
    }
    if (p->signal_allocated && p->signal_mask != 0) {
        m2k_task* owner = (p->owner != M2K_ITEM_NONE)
                              ? m2k_resolve_task_locked(ctx, p->owner)
                              : NULL;
        // Portfolio's internalDeleteMsgPort calls internalFreeSignal, which
        // clears both the allocation and any pending delivery. A stale port
        // signal can otherwise satisfy an unrelated WaitSignal after reuse.
        if (owner) {
            owner->sig_allocated &= ~p->signal_mask;
            owner->sig_pending &= ~p->signal_mask;
        } else {
            ctx->sig_allocated &= ~p->signal_mask;
            ctx->sig_pending &= ~p->signal_mask;
        }
    }
    p->used = 0;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_send_msg(m2k_context* ctx, m2k_item_t port,
                     uint32_t what, uint32_t value) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }

    const uint32_t mi = m2k_find_free_message_index(ctx);
    if (mi == UINT32_MAX) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_NO_SLOT;
    }

    uint32_t pslot = m2k_handle_slot(port);
    m2k_message* m = m2k_message_at_index(ctx, mi);
    m->generation = ++ctx->gen_counter;   // handle born here, valid until reply
    m->state = M2K_MSG_QUEUED;
    m->origin_port = (int32_t)pslot;
    m->owner = M2K_ITEM_NONE;
    m->holder = M2K_ITEM_NONE;
    m->what = what;
    m->value = value;
    m->reply_port = M2K_ITEM_NONE;
    m->data_ptr = 0;
    m->data_size = 0;
    m->reply_code = 0;
    m->next = -1;

    m2k_ready_handoff handoff = {0};
    if (m2k_deliver_to_waiter_locked(ctx, p, port, mi, &handoff)) {
        m2k_wait_ready_handoff_locked(ctx, handoff);
        pthread_mutex_unlock(&ctx->lock);
        return M2K_OK;
    }
    const int32_t append_rc = m2k_append_message_locked(ctx, p, mi);
    if (append_rc != M2K_OK) {
        m->state = M2K_MSG_FREE;
        pthread_mutex_unlock(&ctx->lock);
        return append_rc;
    }

    handoff = m2k_wake_port_owner_locked(ctx, p); // Portfolio mp_Signal
    pthread_cond_broadcast(&ctx->cond);   // wake WaitPort sleepers
    m2k_wait_ready_handoff_locked(ctx, handoff);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

// CreateMsg(name, pri, replyPort): held message with the reply port retained.
// The slot stays reserved (not FREE) until SendMsg moves it onto a port queue
// or a stale handle is retired, so a held message can never be handed to a
// second sender.
static m2k_item_t m2k_create_msg_limited(m2k_context* ctx,
                                         m2k_item_t reply_port,
                                         uint32_t pool_limit) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;
    const uint32_t mi = m2k_find_free_message_index_limit(ctx, pool_limit);
    if (mi == UINT32_MAX) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ITEM_NONE;
    }

    m2k_message* m = m2k_message_at_index(ctx, mi);
    m->generation = ++ctx->gen_counter;
    m->state = M2K_MSG_HELD;
    m->origin_port = -1;
    m->owner = m2k_task_handle_locked(ctx, m2k_current_task_locked(ctx));
    m->holder = m->owner;
    m->what = 0;
    m->value = 0;
    m->reply_port = reply_port;
    m->data_ptr = 0;
    m->data_size = 0;
    m->reply_code = 0;
    m->next = -1;

    m2k_item_t h = m2k_message_handle_for_index(mi, m->generation);
    pthread_mutex_unlock(&ctx->lock);
    return h;
}

m2k_item_t m2k_create_msg(m2k_context* ctx, m2k_item_t reply_port) {
    return m2k_create_msg_limited(ctx, reply_port, M2K_MESSAGE_CAPACITY);
}

m2k_item_t m2k_create_msg_pool(m2k_context* ctx, m2k_item_t reply_port) {
    // CreateMsgItem is still the ordinary Portfolio CreateMsg contract.  The
    // name distinguishes the recovered stream-pool call site for diagnostics;
    // it must not impose a smaller capacity than CreateMsg itself.  The
    // stream image creates a 230-entry decoder pool after other guest message
    // users are live, so the bounded host extension is part of this same
    // guest-visible item pool when the original-width slice is full.
    return m2k_create_msg_limited(ctx, reply_port, M2K_MESSAGE_CAPACITY);
}

int32_t m2k_delete_msg(m2k_context* ctx, m2k_item_t msg) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_message* m = m2k_resolve_msg(ctx, msg);
    if (!m) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }

    // Portfolio internalDeleteMsg removes a Message from either its port
    // queue or its owner's held-message list before destroying the Item. The
    // stream pool normally deletes held messages, but support the queued case
    // too so DeleteItem cannot leave a stale FIFO reference behind.
    if (m->state == M2K_MSG_QUEUED) {
        const uint32_t mi = m2k_message_index_from_handle(msg);
        const int32_t rc = m2k_unlink_queued_message_locked(ctx, mi);
        if (rc != M2K_OK) {
            pthread_mutex_unlock(&ctx->lock);
            return rc;
        }
    }

    m->state = M2K_MSG_FREE;
    m->return_to_sender = 0;
    m->origin_port = -1;
    m->owner = M2K_ITEM_NONE;
    m->holder = M2K_ITEM_NONE;
    m->what = 0;
    m->value = 0;
    m->reply_port = M2K_ITEM_NONE;
    m->data_ptr = 0;
    m->data_size = 0;
    m->reply_code = 0;
    m->next = -1;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

// SendMsg(port, msg, dataPtr, dataSize): queue an existing held message to a
// real port, carrying the guest data EA/size on the message. The reply port
// set at CreateMsg is preserved untouched.
int32_t m2k_send_msg_item(m2k_context* ctx, m2k_item_t port, m2k_item_t msg,
                          uint32_t data_ea, uint32_t data_size) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    m2k_message* m = m2k_resolve_msg_any(ctx, msg);
    if (!m) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (m->state != M2K_MSG_HELD) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // already queued/replied; cannot re-send
    }

    m->data_ptr = data_ea;
    m->data_size = data_size;
    m->reply_code = 0;          // externalSendMsg clears msg_Result before delivery
    m->holder = M2K_ITEM_NONE;
    const uint32_t mi = m2k_message_index_from_handle(msg);
    if (mi == UINT32_MAX) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    m->origin_port = (int32_t)m2k_handle_slot(port);

    m->state = M2K_MSG_QUEUED;
    m2k_ready_handoff handoff = {0};
    if (m2k_deliver_to_waiter_locked(ctx, p, port, mi, &handoff)) {
        m2k_wait_ready_handoff_locked(ctx, handoff);
        pthread_mutex_unlock(&ctx->lock);
        return M2K_OK;
    }
    const int32_t append_rc = m2k_append_message_locked(ctx, p, mi);
    if (append_rc != M2K_OK) {
        m->state = M2K_MSG_HELD;
        pthread_mutex_unlock(&ctx->lock);
        return append_rc;
    }

    handoff = m2k_wake_port_owner_locked(ctx, p); // Portfolio mp_Signal
    pthread_cond_broadcast(&ctx->cond);
    m2k_wait_ready_handoff_locked(ctx, handoff);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_get_msg(m2k_context* ctx, m2k_item_t port, m2k_item_t* msg_out,
                    uint32_t* what_out, uint32_t* value_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (p->head == -1 || p->queued_count == 0) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_EMPTY;             // Opera GetMsg: non-blocking miss
    }

    const int32_t rc = m2k_take_message_locked(ctx, p, p->head,
                                               msg_out, what_out, value_out);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int32_t m2k_wait_port_common(m2k_context* ctx, m2k_item_t port,
                                    m2k_item_t wanted, m2k_item_t* msg_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p || (wanted != M2K_ITEM_NONE && !m2k_resolve_msg(ctx, wanted))) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }

    m2k_task* waiter = m2k_current_task_locked(ctx);
    const int32_t target_ref =
        wanted == M2K_ITEM_NONE
            ? -1
            : m2k_message_queue_ref(m2k_message_index_from_handle(wanted));
    for (;;) {
        if (!p->used) {
            if (waiter) waiter->wait_active = 0;
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_BAD_ITEM;
        }

        // A retained reply can be found behind other messages on the same
        // port. Scan for the requested item rather than consuming the FIFO
        // head, matching internalWaitPort(port,msg) in Portfolio.
        m2k_item_t received = M2K_ITEM_NONE;
        const int32_t rc = m2k_take_message_locked(
            ctx, p, wanted == M2K_ITEM_NONE ? p->head : target_ref,
            &received, NULL, NULL);
        if (rc == M2K_OK) {
            if (msg_out) *msg_out = received;
            pthread_mutex_unlock(&ctx->lock);
            return M2K_OK;
        }
        if (rc != M2K_ERR_EMPTY) {
            pthread_mutex_unlock(&ctx->lock);
            return rc;
        }
        if (wanted != M2K_ITEM_NONE && !m2k_resolve_msg(ctx, wanted)) {
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_BAD_ITEM;
        }

        if (!waiter) {
            // Host callers retain the old blocking condvar behavior. They do
            // not own guest ports, so there is no Portfolio task to receive a
            // direct waiter delivery.
            m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
            continue;
        }

        // Set the wait record while holding the same lock used by SendMsg /
        // ReplyMsg. A matching delivery can therefore transfer the message
        // directly to this task without first queuing it or raising the port
        // signal, exactly as internalDeliverMsg does for t_WaitItem.
        waiter->wait_active = 1;
        waiter->wait_port = port;
        waiter->wait_msg = wanted;
        waiter->wait_result = M2K_ITEM_NONE;
        m2k_task_block_locked(ctx, waiter);              // LandWaiter()
        while (waiter->wait_active && p->used)
            m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());

        if (!p->used) {
            waiter->wait_active = 0;
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_BAD_ITEM;
        }
        if (!waiter->wait_active && waiter->wait_result != M2K_ITEM_NONE) {
            if (msg_out) *msg_out = waiter->wait_result;
            waiter->wait_port = M2K_ITEM_NONE;
            waiter->wait_msg = M2K_ITEM_NONE;
            waiter->wait_result = M2K_ITEM_NONE;
            pthread_mutex_unlock(&ctx->lock);
            return M2K_OK;
        }
    }
}

int32_t m2k_wait_port_any(m2k_context* ctx, m2k_item_t port,
                          m2k_item_t* msg_out) {
    return m2k_wait_port_common(ctx, port, M2K_ITEM_NONE, msg_out);
}

int32_t m2k_wait_port(m2k_context* ctx, m2k_item_t port) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port,
                                                    M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    // This helper is the historical host-side readiness primitive: unlike
    // the selector-facing WaitPort(port,0) path, it only waits for a queued
    // message and leaves FIFO consumption to m2k_get_msg().
    while (p->used && p->head == -1 && p->queued_count == 0)
        m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
    const int32_t rc = p->used ? M2K_OK : M2K_ERR_BAD_ITEM;
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_wait_port_msg(m2k_context* ctx, m2k_item_t port,
                          m2k_item_t msg) {
    m2k_item_t received = M2K_ITEM_NONE;
    const int32_t rc = m2k_wait_port_common(ctx, port, msg, &received);
    return rc == M2K_OK ? (int32_t)received : rc;
}

int32_t m2k_reply_msg_with_result(m2k_context* ctx, m2k_item_t msg,
                                  int32_t reply_code,
                                  m2k_reply_result_writer writer,
                                  void* opaque) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_message* m = m2k_resolve_msg(ctx, msg);
    if (!m) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (m->state != M2K_MSG_DISPATCHED) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // still queued, or already replied
    }
    if (writer) writer(opaque, (uint32_t)reply_code);
    const int32_t rc = m2k_reply_message_locked(ctx, msg, m, reply_code);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_reply_msg(m2k_context* ctx, m2k_item_t msg, int32_t reply_code) {
    return m2k_reply_msg_with_result(ctx, msg, reply_code, NULL, NULL);
}

// Portfolio's KillSelf path calls ReplyHeldMessages after deleting the
// dying task's owned items. Only messages actually dispatched to this task
// are eligible here; a message still queued on a port belongs to that port,
// while a message returned to its sender is held by the sender again.
// Caller holds ctx->lock.
static void m2k_reply_task_messages_locked(m2k_context* ctx,
                                           const m2k_task* task) {
    const m2k_item_t task_item = m2k_task_handle_locked(ctx, task);
    if (task_item == M2K_ITEM_NONE) return;
    for (uint32_t i = 0; i < M2K_MESSAGE_CAPACITY; ++i) {
        m2k_message* m = m2k_message_at_index(ctx, i);
        if (!m || m->state != M2K_MSG_DISPATCHED ||
            m->holder != task_item)
            continue;
        const m2k_item_t msg = m2k_message_handle_for_index(i, m->generation);
        (void)m2k_reply_message_locked(ctx, msg, m, M2K_ERR_KILLED);
    }
}

int32_t m2k_message_info(m2k_context* ctx, m2k_item_t msg,
                         uint32_t* reply_port, uint32_t* data_ptr,
                         uint32_t* data_size, int32_t* reply_code) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_message* m = m2k_resolve_msg(ctx, msg);
    if (!m) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if (reply_port) *reply_port = m->reply_port;
    if (data_ptr) *data_ptr = m->data_ptr;
    if (data_size) *data_size = m->data_size;
    if (reply_code) *reply_code = m->reply_code;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_msgport_info(m2k_context* ctx, m2k_item_t port,
                         uint32_t* signal_mask, uint32_t* userdata) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if (signal_mask) *signal_mask = p->signal_mask;
    if (userdata) *userdata = p->userdata;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

// ---- signals -------------------------------------------------------------------

int32_t m2k_alloc_signal(m2k_context* ctx, int32_t bit_num) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* owner = m2k_current_task_locked(ctx);
    uint32_t* allocated = owner ? &owner->sig_allocated : &ctx->sig_allocated;
    int32_t rc;
    if (bit_num < -1 || bit_num >= (int32_t)M2K_SIG_BITS) {
        rc = M2K_ERR_RANGE;
    } else if (bit_num >= 0) {
        rc = (*allocated & (1u << bit_num)) ? M2K_ERR_STATE : bit_num;
    } else {
        rc = M2K_ERR_NO_SLOT;
        for (uint32_t b = 0; b < M2K_SIG_BITS; ++b) {
            if (!(*allocated & (1u << b))) { rc = (int32_t)b; break; }
        }
    }
    if (rc >= 0) *allocated |= 1u << rc;
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

void m2k_free_signal(m2k_context* ctx, int32_t bit_num) {
    if (!ctx || !ctx->initialized) return;
    if (bit_num < 0 || bit_num >= (int32_t)M2K_SIG_BITS) return;
    pthread_mutex_lock(&ctx->lock);
    m2k_task* owner = m2k_current_task_locked(ctx);
    if (owner) {
        owner->sig_allocated &= ~(1u << bit_num);
        owner->sig_pending &= ~(1u << bit_num);
    } else {
        ctx->sig_allocated &= ~(1u << bit_num);
        ctx->sig_pending &= ~(1u << bit_num);
        for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i)
            ctx->tasks[i].sig_pending &= ~(1u << bit_num);
    }
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

void m2k_free_signal_for_task(m2k_context* ctx, m2k_item_t task,
                              int32_t bit_num) {
    if (!ctx || !ctx->initialized || bit_num < 0 ||
        bit_num >= (int32_t)M2K_SIG_BITS)
        return;
    pthread_mutex_lock(&ctx->lock);
    // M2K_ITEM_NONE denotes the context compatibility allocator here. The
    // Cue bridge uses that value for a host-only Cue, and it must not be
    // confused with a guest task's SELF sentinel when deletion runs inside a
    // different guest task.
    m2k_task* owner = task == M2K_ITEM_NONE
                          ? NULL
                          : m2k_resolve_task_locked(ctx, task);
    if (owner) {
        owner->sig_allocated &= ~(1u << bit_num);
        owner->sig_pending &= ~(1u << bit_num);
    } else if (task == M2K_ITEM_NONE) {
        ctx->sig_allocated &= ~(1u << bit_num);
        ctx->sig_pending &= ~(1u << bit_num);
    }
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

int32_t m2k_send_signal(m2k_context* ctx, m2k_item_t task, int32_t bit) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    if (bit < 0 || bit >= (int32_t)M2K_SIG_BITS) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_RANGE;
    }
    m2k_ready_handoff handoff = {0};
    if (task == M2K_TASK_SELF) {
        // A host-side self send has no current task; retain the original
        // context-wide wake behavior used by the standalone tests.
        if (g_current_context == ctx && g_current_task &&
            g_current_task->used &&
            g_current_task->state == M2K_TASK_RUNNING) {
            g_current_task->sig_pending |= 1u << bit;
        } else {
            // Compatibility path for host-side schedulers/tests: a SELF
            // send outside a task wakes every active waiter, while retaining
            // the context queue for callers that poll from the host thread.
            ctx->sig_pending |= 1u << bit;
            for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
                m2k_task* waiter = &ctx->tasks[i];
                if (waiter->used && waiter->state != M2K_TASK_COMPLETED &&
                    waiter->state != M2K_TASK_FAILED)
                    waiter->sig_pending |= 1u << bit;
            }
        }
    } else {
        m2k_task* target = m2k_resolve_task_locked(ctx, task);
        if (!target) {
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_BAD_ITEM;
        }
        const uint32_t signal = 1u << bit;
        m2k_task* sender = m2k_current_task_locked(ctx);
        // Portfolio internalSignal() calls NewReadyTask() only when this bit
        // satisfies an active WaitSignal; NewReadyTask decides the handoff
        // from the two priorities and the sender's quantum state (see
        // "Portfolio quantum scheduling" above).
        if (sender && sender != target && target->signal_wait_active &&
            (target->signal_wait_mask & signal) != 0u)
            handoff = m2k_ready_handoff_locked(ctx, target);
        target->sig_pending |= signal;
    }
    pthread_cond_broadcast(&ctx->cond);     // wake WaitSignal sleepers
    if (m2k_signal_trace_take()) {
        m2k_task* t = (task == M2K_TASK_SELF)
                          ? m2k_current_task_locked(ctx)
                          : m2k_resolve_task_locked(ctx, task);
        m2k_task* sender = m2k_current_task_locked(ctx);
        fprintf(stderr,
                "[m2k-signal] send task=%p bit=%d pending=%#x "
                "sender_pri=%u target_pri=%u preempt=%u\n",
                (void*)t, bit, t ? t->sig_pending : 0u,
                sender ? sender->priority : 0u, t ? t->priority : 0u,
                handoff.target ? 1u : 0u);
    }
    // A Portfolio reschedule runs the launched task until its next kernel
    // boundary before this sender resumes. pthread_cond_wait releases the host
    // mutex, allowing that task to leave WaitSignal and execute the intervening
    // lifted guest instructions. Termination is also a boundary.
    m2k_wait_ready_handoff_locked(ctx, handoff);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

static uint64_t m2k_monotonic_ns(void) {
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t m2k_add_usec_to_ns(uint64_t now_ns, uint64_t usec) {
    const uint64_t delay_ns = usec > UINT64_MAX / 1000ull
                                  ? UINT64_MAX
                                  : usec * 1000ull;
    return delay_ns > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + delay_ns;
}

// Complete source-shaped one-shot timer requests when a kernel wait/query
// reaches the timer service. The released timer interrupt does this
// independently of the caller's wait site; servicing all due one-shots here
// preserves that ownership while keeping the bounded host substrate
// allocation-free.
static uint64_t m2k_service_one_shot_timers_locked(m2k_context* ctx) {
    const uint64_t now_ns = m2k_monotonic_ns();
    uint64_t earliest = 0;
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_IN_FLIGHT ||
            !q->timer_oneshot)
            continue;
        if (q->timer_deadline_ns <= now_ns) {
            (void)m2k_io_complete_locked(ctx, q, M2K_OK);
            continue;
        }
        if (earliest == 0u || q->timer_deadline_ns < earliest)
            earliest = q->timer_deadline_ns;
    }
    return earliest;
}

// Materialize any elapsed metronome interval into the owner's pending mask and
// return the earliest future realtime deadline for a matching request. Timer
// bits naturally coalesce while the owner is busy, as Portfolio signals do.
static uint64_t m2k_service_metronomes_locked(m2k_context* ctx, uint32_t mask,
                                               uint32_t* pending) {
    m2k_task* current = m2k_current_task_locked(ctx);
    const m2k_item_t owner = current
        ? m2k_task_handle_locked(ctx, current) : M2K_ITEM_NONE;
    const uint64_t now_ns = m2k_monotonic_ns();
    uint64_t earliest = 0;
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_IN_FLIGHT ||
            q->owner != owner || (q->metronome_signal & mask) == 0u)
            continue;
        const uint64_t interval_ns =
            q->metronome_interval_usec > UINT64_MAX / 1000ull
                ? UINT64_MAX : q->metronome_interval_usec * 1000ull;
        if (q->metronome_next_fire_ns == 0u) {
            q->metronome_next_fire_ns = interval_ns > UINT64_MAX - now_ns
                                            ? UINT64_MAX
                                            : now_ns + interval_ns;
        }
        if (q->metronome_next_fire_ns <= now_ns) {
            *pending |= q->metronome_signal;
            if (interval_ns == 0u) {
                q->metronome_next_fire_ns = now_ns;
            } else {
                const uint64_t elapsed = now_ns - q->metronome_next_fire_ns;
                const uint64_t steps = elapsed / interval_ns + 1u;
                q->metronome_next_fire_ns =
                    steps > (UINT64_MAX - q->metronome_next_fire_ns) /
                                interval_ns
                        ? UINT64_MAX
                        : q->metronome_next_fire_ns + steps * interval_ns;
            }
        }
        if (q->metronome_next_fire_ns > now_ns &&
            (earliest == 0u || q->metronome_next_fire_ns < earliest))
            earliest = q->metronome_next_fire_ns;
    }
    return earliest;
}

int32_t m2k_poll_signal(m2k_context* ctx, uint32_t mask) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    if (mask == 0) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_RANGE; }
    uint32_t* pending = g_current_context == ctx && g_current_task &&
                                g_current_task->used
                            ? &g_current_task->sig_pending
                            : &ctx->sig_pending;
    (void)m2k_service_one_shot_timers_locked(ctx);
    (void)m2k_service_metronomes_locked(ctx, mask, pending);
    int32_t got = (int32_t)(*pending & mask);
    *pending &= ~mask;                      // consume exactly the asked bits
    pthread_mutex_unlock(&ctx->lock);
    return got;
}

int32_t m2k_wait_signal(m2k_context* ctx, uint32_t mask) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    if (mask == 0) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_RANGE; }
    uint32_t* pending = g_current_context == ctx && g_current_task &&
                                g_current_task->used
                            ? &g_current_task->sig_pending
                            : &ctx->sig_pending;
    m2k_task* waiter = m2k_current_task_locked(ctx);
    int wait_announced = 0;
    while (!(*pending & mask)) {
        const uint64_t one_shot_deadline =
            m2k_service_one_shot_timers_locked(ctx);
        const uint64_t metronome_deadline =
            m2k_service_metronomes_locked(ctx, mask, pending);
        const uint64_t deadline_ns =
            one_shot_deadline == 0u
                ? metronome_deadline
                : (metronome_deadline == 0u
                       ? one_shot_deadline
                       : (one_shot_deadline < metronome_deadline
                              ? one_shot_deadline
                              : metronome_deadline));
        if (*pending & mask) break;
        if (waiter) {
            waiter->signal_wait_active = 1;
            waiter->signal_wait_mask = mask;
            // Announce the wait once. Rebroadcasting after a wake that found
            // nothing would wake every other sleeper on this condvar, and any
            // of them in this loop would do the same back, without end.
            if (!wait_announced) {
                wait_announced = 1;
                pthread_cond_broadcast(&ctx->cond);
            }
            m2k_task_block_locked(ctx, waiter);          // LandWaiter()
        }
        if (deadline_ns != 0u) {
            const struct timespec deadline = {
                .tv_sec = (time_t)(deadline_ns / 1000000000ull),
                .tv_nsec = (long)(deadline_ns % 1000000000ull),
            };
            (void)m2_diagnostic_cond_timedwait_monotonic(
                &ctx->cond, &ctx->lock, &deadline);
        } else {
            m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        }
        if (waiter) {
            waiter->signal_wait_active = 0;
            waiter->signal_wait_mask = 0;
            m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());
        }
    }
    int32_t got = (int32_t)(*pending & mask);
    *pending &= ~mask;
    pthread_cond_broadcast(&ctx->cond);
    if (m2k_signal_trace_take()) {
        fprintf(stderr, "[m2k-signal] wait task=%p mask=%#x got=%#x\n",
                (void*)(g_current_context == ctx ? g_current_task : NULL),
                mask, (uint32_t)got);
    }
    pthread_mutex_unlock(&ctx->lock);
    return got;
}

// Vendor SendSignal wakes a waiting task; on the M2's single core the woken
// task runs before the sender continues, so a task that dispatches on exact
// single-bit matches (soundtrack_thread_func) observes one bit per wait. The
// host's pthreads can coalesce a send pair (e.g., sndSetMusicVolume followed
// by sndStartMusic) into one return, which the guest's dispatch then drops.
// This variant blocks like m2k_wait_signal but consumes and returns only the
// lowest matching bit, leaving the rest pending for the next wait. Callers
// that must see the complete mask keep using m2k_wait_signal.
int32_t m2k_wait_signal_one(m2k_context* ctx, uint32_t mask) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    if (mask == 0) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_RANGE; }
    uint32_t* pending = g_current_context == ctx && g_current_task &&
                                g_current_task->used
                            ? &g_current_task->sig_pending
                            : &ctx->sig_pending;
    m2k_task* waiter = m2k_current_task_locked(ctx);
    int wait_announced = 0;
    while (!(*pending & mask)) {
        const uint64_t deadline_ns =
            m2k_service_metronomes_locked(ctx, mask, pending);
        if (*pending & mask) break;
        if (waiter) {
            waiter->signal_wait_active = 1;
            waiter->signal_wait_mask = mask;
            // Announce the wait once. Rebroadcasting after a wake that found
            // nothing would wake every other sleeper on this condvar, and any
            // of them in this loop would do the same back, without end.
            if (!wait_announced) {
                wait_announced = 1;
                pthread_cond_broadcast(&ctx->cond);
            }
            m2k_task_block_locked(ctx, waiter);          // LandWaiter()
        }
        if (deadline_ns != 0u) {
            const struct timespec deadline = {
                .tv_sec = (time_t)(deadline_ns / 1000000000ull),
                .tv_nsec = (long)(deadline_ns % 1000000000ull),
            };
            (void)m2_diagnostic_cond_timedwait_monotonic(
                &ctx->cond, &ctx->lock, &deadline);
        } else {
            m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        }
        if (waiter) {
            waiter->signal_wait_active = 0;
            waiter->signal_wait_mask = 0;
            m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());
        }
    }
    const uint32_t match = *pending & mask;
    const uint32_t bit = match & (~match + 1u); // lowest set bit
    *pending &= ~bit;
    pthread_cond_broadcast(&ctx->cond);
    if (m2k_signal_trace_take()) {
        fprintf(stderr, "[m2k-signal] wait1 task=%p mask=%#x got=%#x\n",
                (void*)(g_current_context == ctx ? g_current_task : NULL),
                mask, bit);
    }
    pthread_mutex_unlock(&ctx->lock);
    return (int32_t)bit;
}

// Deliver a timer-owned signal while the kernel context lock is held. This
// follows the completion path's owner semantics: a task-owned request wakes
// that task, while a host-owned request retains the compatibility context
// pending mask and wakes active tasks waiting on it.
static void m2k_deliver_timer_signal_locked(m2k_context* ctx,
                                            m2k_item_t owner,
                                            uint32_t mask) {
    if (mask == 0u) return;
    if (owner != M2K_ITEM_NONE) {
        m2k_task* task = m2k_resolve_task_locked(ctx, owner);
        if (task) task->sig_pending |= mask;
        return;
    }
    ctx->sig_pending |= mask;
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
        m2k_task* task = &ctx->tasks[i];
        if (task->used && task->state != M2K_TASK_COMPLETED &&
            task->state != M2K_TASK_FAILED)
            task->sig_pending |= mask;
    }
}

uint64_t m2k_signal_vbl(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return 0u;
    ++ctx->vblank_count;
    const uint64_t tick = ctx->vblank_count;
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_IN_FLIGHT || !q->timer_vbl)
            continue;

        ++q->timer_vbl_actual;
        if (q->timer_vbl_actual < q->timer_vbl_interval)
            continue;

        if (q->timer_vbl_metronome) {
            // VBLHandler resets io_Actual after each metronome signal and
            // leaves the IOReq in the waiter list.
            m2k_deliver_timer_signal_locked(ctx, q->owner,
                                            q->timer_vbl_signal);
            q->timer_vbl_actual = 0u;
        } else {
            // Preserve the source io_Actual value until the runtime bridge
            // publishes the completed guest IOReq. m2k_io_complete_locked
            // clears only active scheduling fields.
            q->timer_vbl_completion_tick = tick;
            (void)m2k_io_complete_locked(ctx, q, M2K_OK);
        }
    }
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return tick;
}

uint64_t m2k_vblank_count(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return 0u;
    const uint64_t count = ctx->vblank_count;
    pthread_mutex_unlock(&ctx->lock);
    return count;
}

uint32_t m2k_vbl_completions_at(m2k_context* ctx, uint64_t tick,
                                m2k_item_t* io_out, uint32_t* actual_out,
                                uint32_t capacity) {
    if (m2k_lock(ctx) != M2K_OK) return 0u;
    uint32_t count = 0u;
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        const m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_COMPLETE ||
            q->timer_vbl_completion_tick != tick)
            continue;
        if (count < capacity) {
            if (io_out)
                io_out[count] = m2k_make_handle(M2K_TYPE_IOREQ, i,
                                                q->generation);
            if (actual_out) actual_out[count] = q->timer_vbl_actual;
        }
        ++count;
    }
    pthread_mutex_unlock(&ctx->lock);
    return count;
}

int32_t m2k_ioreq_vbl_actual(m2k_context* ctx, m2k_item_t io,
                             uint32_t* actual_out) {
    if (!actual_out) return M2K_ERR_RANGE;
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    *actual_out = q->timer_vbl_actual;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

uint32_t m2k_signal_allocated_mask(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return 0u;
    m2k_task* owner = m2k_current_task_locked(ctx);
    const uint32_t allocated = owner ? owner->sig_allocated : ctx->sig_allocated;
    pthread_mutex_unlock(&ctx->lock);
    return allocated;
}

// ---- IOReqs ----------------------------------------------------------------------

static int32_t m2k_free_ioreq_slot(const m2k_context* ctx) {
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i)
        if (!ctx->ioreqs[i].used) return (int32_t)i;
    return -1;
}

m2k_item_t m2k_create_ioreq_va(m2k_context* ctx, const m2_tag_arg* tags,
                               uint32_t max_count) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;

    uint32_t device = 0;
    int32_t ti = m2_tag_find(tags, max_count, M2K_TAG_IO_DEVICE);
    if (ti < 0) ti = m2_tag_find(tags, max_count, M2K_TAG_IO_DEVICE_VA);
    if (ti >= 0) device = tags[ti].value;

    uint32_t reply_port = M2K_ITEM_NONE;
    int32_t rp = m2_tag_find(tags, max_count, M2K_TAG_IO_REPLYPORT);
    if (rp >= 0 && tags[rp].value != 0) reply_port = tags[rp].value;

    uint32_t signal_mask = 0;
    ti = m2_tag_find(tags, max_count, M2K_TAG_IO_SIGNAL);
    if (ti >= 0) {
        signal_mask = tags[ti].value;
        if (signal_mask == 0 && reply_port == M2K_ITEM_NONE)
            signal_mask = M2K_SIGF_IODONE;
    } else if (reply_port == M2K_ITEM_NONE) {
        signal_mask = M2K_SIGF_IODONE;
    }

    m2k_item_t msg_item = M2K_ITEM_NONE;
    if (reply_port != M2K_ITEM_NONE) {
        const uint32_t mi = m2k_find_free_message_index(ctx);
        if (mi != UINT32_MAX) {
            m2k_message* m = m2k_message_at_index(ctx, mi);
            m->generation = ++ctx->gen_counter;
            m->state = M2K_MSG_HELD;
            m->origin_port = -1;
            m->owner = m2k_task_handle_locked(ctx,
                                              m2k_current_task_locked(ctx));
            m->what = 0;
            m->value = 0;
            m->reply_port = reply_port;
            m->data_ptr = 0;
            m->data_size = 0;
            m->reply_code = 0;
            m->holder = m->owner;
            m->next = -1;
            msg_item = m2k_message_handle_for_index(mi, m->generation);
        }
    }

    int32_t slot = m2k_free_ioreq_slot(ctx);
    if (slot < 0) {
        if (msg_item != M2K_ITEM_NONE) {
            const uint32_t mi = m2k_message_index_from_handle(msg_item);
            m2k_message* m = m2k_message_at_index(ctx, mi);
            if (m) m->state = M2K_MSG_FREE;
        }
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ITEM_NONE;
    }

    m2k_ioreq* q = &ctx->ioreqs[slot];
    q->used = 1;
    q->generation = ++ctx->gen_counter;
    q->state = M2K_IO_IDLE;
    q->status = M2K_OK;
    q->device = device;
    q->what = 0;
    q->arg = 0;
    q->complete_seq = 0;
    q->signal_mask = signal_mask;
    q->reply_port = reply_port;
    q->msg_item = msg_item;
    q->user_data = 0;
    q->metronome_signal = 0;
    q->metronome_interval_usec = 0;
    q->metronome_next_fire_ns = 0;
    q->timer_oneshot = 0;
    q->timer_delay_usec = 0;
    q->timer_deadline_ns = 0;
    q->timer_vbl = 0;
    q->timer_vbl_metronome = 0;
    q->timer_vbl_reserved = 0;
    q->timer_vbl_interval = 0;
    q->timer_vbl_actual = 0;
    q->timer_vbl_signal = 0;
    q->timer_vbl_completion_tick = 0;
    q->owner = M2K_ITEM_NONE;
    m2k_task* owner_task = m2k_current_task_locked(ctx);
    if (owner_task) {
        const uint32_t owner_slot = (uint32_t)(owner_task - ctx->tasks);
        q->owner = m2k_make_handle(M2K_TYPE_TASK, owner_slot,
                                   owner_task->generation);
    }

    m2k_item_t h = m2k_make_handle(M2K_TYPE_IOREQ, (uint32_t)slot, q->generation);
    pthread_mutex_unlock(&ctx->lock);
    return h;
}

m2k_item_t m2k_create_ioreq(m2k_context* ctx) {
    return m2k_create_ioreq_va(ctx, NULL, 0);
}

int32_t m2k_ioreq_device(m2k_context* ctx, m2k_item_t io,
                         m2k_item_t* device_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if (device_out) *device_out = q->device;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_ioreq_reply_port(m2k_context* ctx, m2k_item_t io,
                             m2k_item_t* reply_port_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if (reply_port_out) *reply_port_out = q->reply_port;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_ioreq_set_user_data(m2k_context* ctx, m2k_item_t io,
                               uint32_t user_data) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    q->user_data = user_data;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

static int32_t m2k_delete_ioreq_locked(m2k_context* ctx, m2k_ioreq* q) {
    if (!q) return M2K_ERR_BAD_ITEM;
    // internalDeleteIOReq() in Portfolio's io.c aborts an outstanding request
    // before removing the IOReq item. Preserve that ordering: the completion
    // becomes observable first (including its signal/reply path), then the
    // request-owned completion message is reclaimed below.
    if (q->state == M2K_IO_IN_FLIGHT)
        (void)m2k_io_complete_locked(ctx, q, -1);
    if (q->msg_item != M2K_ITEM_NONE && m2k_is_message_handle(q->msg_item)) {
        const uint32_t mi = m2k_message_index_from_handle(q->msg_item);
        m2k_message* m = m2k_resolve_msg_any(ctx, q->msg_item);
        if (m) {
            if (m->state == M2K_MSG_QUEUED) {
                const int32_t unlink_rc =
                    m2k_unlink_queued_message_locked(ctx, mi);
                if (unlink_rc != M2K_OK) return unlink_rc;
            }
            m->state = M2K_MSG_FREE;
            m->return_to_sender = 0;
            m->origin_port = -1;
            m->owner = M2K_ITEM_NONE;
            m->what = 0;
            m->value = 0;
            m->reply_port = M2K_ITEM_NONE;
            m->data_ptr = 0;
            m->data_size = 0;
            m->reply_code = 0;
            m->holder = M2K_ITEM_NONE;
            m->next = -1;
        }
        q->msg_item = M2K_ITEM_NONE;
    }
    q->used = 0;
    return M2K_OK;
}

int32_t m2k_delete_ioreq(m2k_context* ctx, m2k_item_t io) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    const int32_t rc = m2k_delete_ioreq_locked(ctx, q);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int32_t m2k_io_arm_locked(m2k_context* ctx, m2k_ioreq* q,
                                 uint32_t what, uint32_t arg) {
    (void)ctx;
    q->state = M2K_IO_IN_FLIGHT;
    q->what = what;
    q->arg = arg;
    q->status = M2K_OK;
    q->metronome_signal = 0;
    q->metronome_interval_usec = 0;
    q->metronome_next_fire_ns = 0;
    q->timer_oneshot = 0;
    q->timer_delay_usec = 0;
    q->timer_deadline_ns = 0;
    q->timer_vbl = 0;
    q->timer_vbl_metronome = 0;
    q->timer_vbl_reserved = 0;
    q->timer_vbl_interval = 0;
    q->timer_vbl_actual = 0;
    q->timer_vbl_signal = 0;
    q->timer_vbl_completion_tick = 0;
    return M2K_OK;
}

static int32_t m2k_io_complete_locked(m2k_context* ctx, m2k_ioreq* q,
                                     int32_t status) {
    q->status = status;
    q->complete_seq = ++ctx->completions;
    q->state = M2K_IO_COMPLETE;
    q->metronome_signal = 0;
    q->metronome_interval_usec = 0;
    q->metronome_next_fire_ns = 0;
    q->timer_oneshot = 0;
    q->timer_delay_usec = 0;
    q->timer_deadline_ns = 0;
    q->timer_vbl = 0;
    q->timer_vbl_metronome = 0;
    q->timer_vbl_reserved = 0;
    q->timer_vbl_interval = 0;
    q->timer_vbl_signal = 0;

    // Portfolio io.c:712-722 completion dispatch:
    // If reply_port is set, deliver the IO completion message to that port.
    // Otherwise deliver signal_mask to the owner task.
    if (q->reply_port != M2K_ITEM_NONE && q->msg_item != M2K_ITEM_NONE) {
        m2k_msgport* rp = (m2k_msgport*)m2k_resolve_item(ctx, q->reply_port,
                                                         M2K_TYPE_MSGPORT,
                                                         ctx->ports, M2K_MAX_PORTS,
                                                         sizeof(m2k_msgport));
        const uint32_t mi = m2k_message_index_from_handle(q->msg_item);
        m2k_message* m = m2k_message_at_index(ctx, mi);
        if (rp && m) {
            m->reply_code = q->status;
            m->data_ptr = m2k_make_handle(M2K_TYPE_IOREQ,
                                          (uint32_t)(q - ctx->ioreqs),
                                          q->generation);
            m->data_size = q->user_data;
            m->origin_port = -1;
            m->holder = M2K_ITEM_NONE;
            m->next = -1;
            m->return_to_sender = 1;
            if (m2k_append_message_locked(ctx, rp, mi) == M2K_OK) {
                m->state = M2K_MSG_QUEUED;
                (void)m2k_wake_port_owner_locked(ctx, rp);
            }
        }
    } else if (q->signal_mask != 0u) {
        m2k_task* owner = (q->owner != M2K_ITEM_NONE)
                              ? m2k_resolve_task_locked(ctx, q->owner)
                              : NULL;
        if (owner) {
            owner->sig_pending |= q->signal_mask;
        } else {
            ctx->sig_pending |= q->signal_mask;
            for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
                m2k_task* waiter = &ctx->tasks[i];
                if (waiter->used && waiter->state != M2K_TASK_COMPLETED &&
                    waiter->state != M2K_TASK_FAILED)
                    waiter->sig_pending |= q->signal_mask;
            }
        }
    }
    pthread_cond_broadcast(&ctx->cond);
    return q->status;
}

int32_t m2k_do_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // synchronous DoIO re-arm allowed from IDLE or COMPLETE only
    }
    m2k_io_arm_locked(ctx, q, what, arg);
    int32_t rc = m2k_io_complete_locked(ctx, q, M2K_OK);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_send_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // double SendIO on one request
    }
    m2k_io_arm_locked(ctx, q, what, arg);   // IDLE or re-arm
    int32_t rc = m2k_io_complete_locked(ctx, q, M2K_OK);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_arm_io(m2k_context* ctx, m2k_item_t io, uint32_t what, uint32_t arg) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // request is already in flight
    }
    int32_t rc = m2k_io_arm_locked(ctx, q, what, arg);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_arm_metronome(m2k_context* ctx, m2k_item_t io, uint32_t what,
                          uint32_t arg, uint32_t signal_mask,
                          uint64_t interval_usec) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }
    (void)m2k_io_arm_locked(ctx, q, what, arg);
    q->metronome_signal = signal_mask;
    q->metronome_interval_usec = interval_usec;
    q->metronome_next_fire_ns =
        m2k_add_usec_to_ns(m2k_monotonic_ns(), interval_usec);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_arm_timer(m2k_context* ctx, m2k_item_t io, uint32_t what,
                      uint32_t arg, uint64_t delay_usec) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }
    (void)m2k_io_arm_locked(ctx, q, what, arg);
    q->timer_oneshot = 1;
    q->timer_delay_usec = delay_usec;
    q->timer_deadline_ns =
        m2k_add_usec_to_ns(m2k_monotonic_ns(), delay_usec);
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_arm_vbl_timer(m2k_context* ctx, m2k_item_t io, uint32_t what,
                          uint32_t arg, uint32_t interval,
                          uint32_t signal_mask, int metronome) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state == M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }
    (void)m2k_io_arm_locked(ctx, q, what, arg);
    q->timer_vbl = 1;
    q->timer_vbl_metronome = metronome ? 1u : 0u;
    q->timer_vbl_interval = interval;
    q->timer_vbl_signal = signal_mask;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

void m2k_rebase_timer_deadlines(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return;
    const uint64_t now_ns = m2k_monotonic_ns();
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_IN_FLIGHT) continue;
        // M2EX stores remaining monotonic nanoseconds in the otherwise
        // process-local deadline fields. Rebase that remaining phase onto
        // this process's monotonic epoch instead of restarting a full period.
        if (q->timer_oneshot)
            q->timer_deadline_ns =
                q->timer_deadline_ns > UINT64_MAX - now_ns
                    ? UINT64_MAX : now_ns + q->timer_deadline_ns;
        if (q->metronome_signal != 0u)
            q->metronome_next_fire_ns =
                q->metronome_next_fire_ns > UINT64_MAX - now_ns
                    ? UINT64_MAX : now_ns + q->metronome_next_fire_ns;
    }

    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

int32_t m2k_metronome_for_wait(m2k_context* ctx, m2k_item_t task,
                               uint32_t mask, uint32_t* signal_out,
                               uint64_t* interval_usec_out) {
    if (!signal_out || !interval_usec_out) return M2K_ERR_RANGE;
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        const m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->state != M2K_IO_IN_FLIGHT ||
            q->owner != task || (q->metronome_signal & mask) == 0u)
            continue;
        *signal_out = q->metronome_signal;
        *interval_usec_out = q->metronome_interval_usec;
        pthread_mutex_unlock(&ctx->lock);
        return M2K_OK;
    }
    pthread_mutex_unlock(&ctx->lock);
    return M2K_ERR_EMPTY;
}

int32_t m2k_complete_io(m2k_context* ctx, m2k_item_t io, int32_t status) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state != M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // can only complete in-flight requests
    }
    int32_t rc = m2k_io_complete_locked(ctx, q, status);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_abort_io(m2k_context* ctx, m2k_item_t io) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (q->state != M2K_IO_IN_FLIGHT) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // can only abort in-flight requests
    }
    int32_t rc = m2k_io_complete_locked(ctx, q, -1);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_wait_io(m2k_context* ctx, m2k_item_t io) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    m2k_task* waiter = m2k_current_task_locked(ctx);
    while (q->state == M2K_IO_IN_FLIGHT) {
        const uint64_t deadline_ns =
            m2k_service_one_shot_timers_locked(ctx);
        if (q->state != M2K_IO_IN_FLIGHT) break;
        if (waiter) m2k_task_block_locked(ctx, waiter);  // LandWaiter()
        if (deadline_ns != 0u) {
            const struct timespec deadline = {
                .tv_sec = (time_t)(deadline_ns / 1000000000ull),
                .tv_nsec = (long)(deadline_ns % 1000000000ull),
            };
            (void)m2_diagnostic_cond_timedwait_monotonic(
                &ctx->cond, &ctx->lock, &deadline);
        } else {
            m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        }
        if (waiter) m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());
    }
    int32_t rc = q->status;
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_query_io(m2k_context* ctx, m2k_item_t io,
                     int32_t* state_out, int32_t* status_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_ioreq* q = (m2k_ioreq*)m2k_resolve_item(ctx, io, M2K_TYPE_IOREQ,
                                                ctx->ioreqs, M2K_MAX_IOREQS,
                                                sizeof(m2k_ioreq));
    if (!q) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    (void)m2k_service_one_shot_timers_locked(ctx);
    if (state_out) *state_out = (int32_t)q->state;
    if (status_out) *status_out = q->status;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

// ---- semaphores --------------------------------------------------------------

#define M2K_SEMAPHORE_BASE UINT32_C(0x7400)
#define M2K_HOST_LOCKER    UINT32_MAX

static m2k_semaphore* m2k_resolve_semaphore_locked(m2k_context* ctx,
                                                    m2k_item_t item) {
    if (item < M2K_SEMAPHORE_BASE ||
        item >= M2K_SEMAPHORE_BASE + M2K_MAX_SEMAPHORES)
        return NULL;
    m2k_semaphore* semaphore =
        &ctx->semaphores[item - M2K_SEMAPHORE_BASE];
    return semaphore->used ? semaphore : NULL;
}

static m2k_item_t m2k_current_locker_locked(m2k_context* ctx) {
    m2k_task* task = m2k_current_task_locked(ctx);
    return task ? m2k_task_handle_locked(ctx, task) : M2K_HOST_LOCKER;
}

static int32_t m2k_shared_locker_slot(const m2k_semaphore* semaphore,
                                      m2k_item_t locker) {
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i)
        if (semaphore->shared_nest[i] != 0u &&
            semaphore->shared_lockers[i] == locker)
            return (int32_t)i;
    return -1;
}

static int32_t m2k_free_shared_locker_slot(const m2k_semaphore* semaphore) {
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i)
        if (semaphore->shared_nest[i] == 0u) return (int32_t)i;
    return -1;
}

static int m2k_semaphore_owned_locked(const m2k_semaphore* semaphore,
                                      m2k_item_t locker) {
    return semaphore->locker == locker ||
           m2k_shared_locker_slot(semaphore, locker) >= 0;
}

m2k_item_t m2k_create_semaphore(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;
    for (uint32_t i = 0; i < M2K_MAX_SEMAPHORES; ++i) {
        m2k_semaphore* semaphore = &ctx->semaphores[i];
        if (semaphore->used) continue;
        memset(semaphore, 0, sizeof(*semaphore));
        semaphore->used = 1;
        pthread_mutex_unlock(&ctx->lock);
        return M2K_SEMAPHORE_BASE + i;
    }
    pthread_mutex_unlock(&ctx->lock);
    return M2K_ITEM_NONE;
}

int32_t m2k_lock_semaphore(m2k_context* ctx, m2k_item_t item,
                           uint32_t flags) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_semaphore* semaphore = m2k_resolve_semaphore_locked(ctx, item);
    if (!semaphore) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if ((flags & ~(M2K_SEM_WAIT | M2K_SEM_SHAREDREAD)) != 0u) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_LOCK_ARG;
    }
    const m2k_item_t locker = m2k_current_locker_locked(ctx);
    const int shared_request = (flags & M2K_SEM_SHAREDREAD) != 0u;
    if (semaphore->locker == M2K_ITEM_NONE) {
        semaphore->locker = locker;
        semaphore->nest_count = 1;
        semaphore->shared = shared_request ? 1u : 0u;
        pthread_mutex_unlock(&ctx->lock);
        return 1;
    }
    if (semaphore->locker == locker) {
        if (semaphore->shared && !shared_request) {
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_BAD_LOCK_ARG;
        }
        ++semaphore->nest_count;
        pthread_mutex_unlock(&ctx->lock);
        return 1;
    }
    if (shared_request && semaphore->shared) {
        int32_t shared_slot = m2k_shared_locker_slot(semaphore, locker);
        if (shared_slot >= 0 || semaphore->wait_count == 0u) {
            if (shared_slot < 0)
                shared_slot = m2k_free_shared_locker_slot(semaphore);
            if (shared_slot < 0) {
                pthread_mutex_unlock(&ctx->lock);
                return M2K_ERR_STATE;
            }
            semaphore->shared_lockers[shared_slot] = locker;
            ++semaphore->shared_nest[shared_slot];
            pthread_mutex_unlock(&ctx->lock);
            return 1;
        }
    }
    if ((flags & M2K_SEM_WAIT) == 0u) {
        pthread_mutex_unlock(&ctx->lock);
        return 0;
    }
    // A host-only caller has no distinct Portfolio Task identity and cannot
    // be safely queued behind a guest owner. Production selector calls always
    // have a current guest task; fail closed for this diagnostic-only shape.
    if (locker == M2K_HOST_LOCKER ||
        semaphore->wait_count >= M2K_MAX_TASKS) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }
    semaphore->waiters[semaphore->wait_count++] = locker;
    semaphore->waiter_shared[semaphore->wait_count - 1u] =
        shared_request ? 1u : 0u;
    m2k_task* waiter = m2k_current_task_locked(ctx);
    while (semaphore->used &&
           !m2k_semaphore_owned_locked(semaphore, locker)) {
        if (waiter) m2k_task_block_locked(ctx, waiter);  // LandWaiter()
        m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        if (waiter) m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());
    }
    const int32_t rc = semaphore->used ? 1 : M2K_ERR_BAD_ITEM;
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_unlock_semaphore(m2k_context* ctx, m2k_item_t item) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_semaphore* semaphore = m2k_resolve_semaphore_locked(ctx, item);
    if (!semaphore) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    const m2k_item_t locker = m2k_current_locker_locked(ctx);
    if (semaphore->locker != locker) {
        const int32_t shared_slot =
            m2k_shared_locker_slot(semaphore, locker);
        if (shared_slot < 0) {
            pthread_mutex_unlock(&ctx->lock);
            return M2K_ERR_NOT_OWNER;
        }
        --semaphore->shared_nest[shared_slot];
        if (semaphore->shared_nest[shared_slot] == 0u)
            semaphore->shared_lockers[shared_slot] = M2K_ITEM_NONE;
        pthread_mutex_unlock(&ctx->lock);
        return M2K_OK;
    }
    if (semaphore->nest_count == 0u) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_NOT_OWNER;
    }
    if (--semaphore->nest_count != 0u) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_OK;
    }

    if (semaphore->shared) {
        for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
            if (semaphore->shared_nest[i] == 0u) continue;
            semaphore->locker = semaphore->shared_lockers[i];
            semaphore->nest_count = semaphore->shared_nest[i];
            semaphore->shared_lockers[i] = M2K_ITEM_NONE;
            semaphore->shared_nest[i] = 0;
            pthread_mutex_unlock(&ctx->lock);
            return M2K_OK;
        }
    }

    if (semaphore->wait_count != 0u) {
        const m2k_item_t next = semaphore->waiters[0];
        const int next_shared = semaphore->waiter_shared[0] != 0u;
        memmove(&semaphore->waiters[0], &semaphore->waiters[1],
                (semaphore->wait_count - 1u) * sizeof(semaphore->waiters[0]));
        memmove(&semaphore->waiter_shared[0], &semaphore->waiter_shared[1],
                (semaphore->wait_count - 1u) *
                    sizeof(semaphore->waiter_shared[0]));
        semaphore->waiters[--semaphore->wait_count] = M2K_ITEM_NONE;
        semaphore->waiter_shared[semaphore->wait_count] = 0;
        semaphore->locker = next;
        semaphore->nest_count = 1;
        semaphore->shared = next_shared ? 1u : 0u;
        m2k_ready_handoff handoff = m2k_ready_handoff_locked(
            ctx, m2k_resolve_task_locked(ctx, next));
        pthread_cond_broadcast(&ctx->cond);
        m2k_wait_ready_handoff_locked(ctx, handoff);

        // A shared waiter at the head admits the consecutive shared readers
        // behind it, exactly as internalUnlockSemaphore scans until the first
        // exclusive waiter.
        while (next_shared && semaphore->wait_count != 0u &&
               semaphore->waiter_shared[0]) {
            const int32_t shared_slot =
                m2k_free_shared_locker_slot(semaphore);
            if (shared_slot < 0) break;
            const m2k_item_t shared_locker = semaphore->waiters[0];
            semaphore->shared_lockers[shared_slot] = shared_locker;
            semaphore->shared_nest[shared_slot] = 1;
            memmove(&semaphore->waiters[0], &semaphore->waiters[1],
                    (semaphore->wait_count - 1u) *
                        sizeof(semaphore->waiters[0]));
            memmove(&semaphore->waiter_shared[0],
                    &semaphore->waiter_shared[1],
                    (semaphore->wait_count - 1u) *
                        sizeof(semaphore->waiter_shared[0]));
            semaphore->waiters[--semaphore->wait_count] = M2K_ITEM_NONE;
            semaphore->waiter_shared[semaphore->wait_count] = 0;
            handoff = m2k_ready_handoff_locked(
                ctx, m2k_resolve_task_locked(ctx, shared_locker));
            pthread_cond_broadcast(&ctx->cond);
            m2k_wait_ready_handoff_locked(ctx, handoff);
        }
    } else {
        semaphore->locker = M2K_ITEM_NONE;
        semaphore->shared = 0;
    }
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

static void m2k_release_task_semaphores_locked(m2k_context* ctx,
                                                m2k_item_t task) {
    for (uint32_t i = 0; i < M2K_MAX_SEMAPHORES; ++i) {
        m2k_semaphore* semaphore = &ctx->semaphores[i];
        if (!semaphore->used) continue;
        for (uint32_t w = 0; w < semaphore->wait_count;) {
            if (semaphore->waiters[w] != task) {
                ++w;
                continue;
            }
            memmove(&semaphore->waiters[w], &semaphore->waiters[w + 1u],
                    (semaphore->wait_count - w - 1u) *
                        sizeof(semaphore->waiters[0]));
            memmove(&semaphore->waiter_shared[w],
                    &semaphore->waiter_shared[w + 1u],
                    (semaphore->wait_count - w - 1u) *
                        sizeof(semaphore->waiter_shared[0]));
            semaphore->waiters[--semaphore->wait_count] = M2K_ITEM_NONE;
            semaphore->waiter_shared[semaphore->wait_count] = 0;
        }
        const int32_t shared_slot =
            m2k_shared_locker_slot(semaphore, task);
        if (shared_slot >= 0) {
            semaphore->shared_lockers[shared_slot] = M2K_ITEM_NONE;
            semaphore->shared_nest[shared_slot] = 0;
        }
        if (semaphore->locker != task) continue;
        if (semaphore->shared) {
            int promoted = 0;
            for (uint32_t s = 0; s < M2K_MAX_TASKS; ++s) {
                if (semaphore->shared_nest[s] == 0u) continue;
                semaphore->locker = semaphore->shared_lockers[s];
                semaphore->nest_count = semaphore->shared_nest[s];
                semaphore->shared_lockers[s] = M2K_ITEM_NONE;
                semaphore->shared_nest[s] = 0;
                promoted = 1;
                break;
            }
            if (promoted) continue;
        }
        if (semaphore->wait_count != 0u) {
            semaphore->locker = semaphore->waiters[0];
            semaphore->nest_count = 1;
            semaphore->shared = semaphore->waiter_shared[0];
            memmove(&semaphore->waiters[0], &semaphore->waiters[1],
                    (semaphore->wait_count - 1u) *
                        sizeof(semaphore->waiters[0]));
            memmove(&semaphore->waiter_shared[0],
                    &semaphore->waiter_shared[1],
                    (semaphore->wait_count - 1u) *
                        sizeof(semaphore->waiter_shared[0]));
            semaphore->waiters[--semaphore->wait_count] = M2K_ITEM_NONE;
            semaphore->waiter_shared[semaphore->wait_count] = 0;
            while (semaphore->shared && semaphore->wait_count != 0u &&
                   semaphore->waiter_shared[0]) {
                const int32_t slot =
                    m2k_free_shared_locker_slot(semaphore);
                if (slot < 0) break;
                semaphore->shared_lockers[slot] = semaphore->waiters[0];
                semaphore->shared_nest[slot] = 1;
                memmove(&semaphore->waiters[0], &semaphore->waiters[1],
                        (semaphore->wait_count - 1u) *
                            sizeof(semaphore->waiters[0]));
                memmove(&semaphore->waiter_shared[0],
                        &semaphore->waiter_shared[1],
                        (semaphore->wait_count - 1u) *
                            sizeof(semaphore->waiter_shared[0]));
                semaphore->waiters[--semaphore->wait_count] = M2K_ITEM_NONE;
                semaphore->waiter_shared[semaphore->wait_count] = 0;
            }
        } else {
            semaphore->locker = M2K_ITEM_NONE;
            semaphore->nest_count = 0;
            semaphore->shared = 0;
        }
    }
    pthread_cond_broadcast(&ctx->cond);
}

// ---- guest tasks ------------------------------------------------------------------

static int32_t m2k_free_task_slot(const m2k_context* ctx) {
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i)
        if (!ctx->tasks[i].used) return (int32_t)i;
    return -1;
}

// CREATETASK_TAG_MSGFROMCHILD status message. Queue one kernel-owned Message
// on `port` carrying the terminating task's exit status in msg_Result
// (m2k_message.reply_code, published at guest offset +0x28 by the runtime's
// LookupItem(MESSAGE)), the terminated task's item in msg_DataPtr, and the
// terminating task's item in msg_DataSize. Same construction and the same
// direct-delivery-then-append order as m2k_send_msg, so a port with a waiter
// parked in WaitPort is woken by delivery rather than by an append. Callers
// hold ctx->lock.
static int32_t m2k_send_status_msg_locked(m2k_context* ctx, m2k_item_t port,
                                          int32_t result, uint32_t data_ptr,
                                          uint32_t data_size) {
    m2k_msgport* p = (m2k_msgport*)m2k_resolve_item(ctx, port, M2K_TYPE_MSGPORT,
                                                    ctx->ports, M2K_MAX_PORTS,
                                                    sizeof(m2k_msgport));
    if (!p) return M2K_ERR_BAD_ITEM;
    const uint32_t mi = m2k_find_free_message_index(ctx);
    if (mi == UINT32_MAX) return M2K_ERR_NO_SLOT;
    m2k_message* m = m2k_message_at_index(ctx, mi);
    m->generation = ++ctx->gen_counter;   // handle born here, valid until reply
    m->state = M2K_MSG_QUEUED;
    m->origin_port = (int32_t)m2k_handle_slot(port);
    m->owner = M2K_ITEM_NONE;
    m->holder = M2K_ITEM_NONE;
    m->what = 0;
    m->value = 0;
    m->reply_port = M2K_ITEM_NONE;
    m->data_ptr = data_ptr;
    m->data_size = data_size;
    m->reply_code = result;
    m->next = -1;

    m2k_ready_handoff handoff = {0};
    if (m2k_deliver_to_waiter_locked(ctx, p, port, mi, &handoff)) {
        pthread_cond_broadcast(&ctx->cond);
        m2k_wait_ready_handoff_locked(ctx, handoff);
        return M2K_OK;
    }
    const int32_t append_rc = m2k_append_message_locked(ctx, p, mi);
    if (append_rc != M2K_OK) {
        m->state = M2K_MSG_FREE;
        return append_rc;
    }
    handoff = m2k_wake_port_owner_locked(ctx, p);   // Portfolio mp_Signal
    pthread_cond_broadcast(&ctx->cond);
    m2k_wait_ready_handoff_locked(ctx, handoff);
    return M2K_OK;
}

// Thread body. Runs detached; the slot is pinned until this thread publishes
// a terminal state under the lock, so every field read here is stable and no
// host thread can observe a half-built or recycled task entry.
static void* m2k_task_trampoline(void* v) {
    m2k_task_boot* b = (m2k_task_boot*)v;
    m2k_context* ctx = b->ctx;
    int32_t slot = b->slot;

    pthread_mutex_lock(&ctx->lock);
    m2k_task* t = &ctx->tasks[slot];
    m2k_task_fn_ex fn_ex = t->fn_ex;
    m2k_task_fn   fn    = fn_ex ? NULL : t->fn;
    uint32_t      arg   = t->arg;
    void*         user_data = t->user_data;
    t->state = M2K_TASK_RUNNING;
    // Preserve signals sent after creation but before the pthread entered.
    g_current_task = t;
    g_current_context = ctx;
    m2k_task_launch_locked(ctx, t, m2k_monotonic_ns());   // Launch()

    // Run the guest body without the lock so it can call kernel primitives
    // (wait_signal, wait_port, ...) which need to acquire it. The extended
    // entrypoint wins when present; otherwise the uint32_t-payload entrypoint
    // from m2k_create_task runs.
    pthread_mutex_unlock(&ctx->lock);
    m2_diagnostic_task_enter((uint32_t)slot);
    int32_t rc = fn_ex ? fn_ex(ctx, user_data) : fn(ctx, arg);

    g_current_task = NULL;
    g_current_context = NULL;
    pthread_mutex_lock(&ctx->lock);
    t = &ctx->tasks[slot];   // re-read: delete is refused while non-terminal,
                             // so the entry cannot have been recycled meanwhile
    const m2k_item_t owner = t->owner;
    const m2k_item_t task_item = m2k_task_handle_locked(ctx, t);
    m2k_release_task_semaphores_locked(ctx, task_item);
    m2k_reply_task_messages_locked(ctx, t);
    m2k_ready_remove_locked(t);          // terminal tasks leave the ready order
    m2k_disarm_quantaclock_locked(t);
    t->exit_status = rc;
    t->state = rc < 0 ? M2K_TASK_FAILED : M2K_TASK_COMPLETED;
    // Portfolio KillTask sends SIGF_DEADTASK (bit 0x10) to the child owner
    // when no explicit exit message exists. Keep that guest-visible event
    // distinct from the host join condition; the owner may use WaitSignal to
    // observe the child transition before it calls LookupItem/JoinTask.
    if (owner != M2K_ITEM_NONE) {
        m2k_task* owner_task = m2k_resolve_task_locked(ctx, owner);
        if (owner_task && owner_task != t)
            owner_task->sig_pending |= (1u << 4);
    }
    // CREATETASK_TAG_MSGFROMCHILD: "The kernel will send a status message to
    // this port whenever the thread or task being created exits. The message
    // is sent by the kernel after the task has been deleted. The msg_Result
    // field of the message contains the exit status of the task. ... The
    // msg_DataPtr field of the message contains the item number of the task
    // that just terminated. Finally, the msg_DataSize field contains the item
    // number of the thread or task that terminated the task."
    // (folios/kernel/task.c AUTODOC for CREATETASK_TAG_MSGFROMCHILD, 862-877.)
    // The terminal state above is already published, so this is the source's
    // "after the task has been deleted" point; the launcher's WaitPort then
    // returns the message item and reads msg_Result at +0x28.
    if (t->exit_notify_port != M2K_ITEM_NONE) {
        // M2K_ITEM_NONE means "no terminator was named", which is the
        // self-exit case the AUTODOC calls out: msg_DataSize is then the
        // item number of the task itself.
        const uint32_t terminator = t->exit_notify_data != M2K_ITEM_NONE
                                        ? t->exit_notify_data : task_item;
        const int32_t nrc = m2k_send_status_msg_locked(
            ctx, t->exit_notify_port, rc, task_item, terminator);
        if (nrc != M2K_OK)
            fprintf(stderr,
                    "[m2k] exit status message to port %#x failed: %d\n",
                    (unsigned)t->exit_notify_port, nrc);
    }
    pthread_cond_broadcast(&ctx->cond);   // wake join_task sleepers
    pthread_mutex_unlock(&ctx->lock);
    m2_diagnostic_task_leave((uint32_t)slot);
    return NULL;
}

// Shared spawn tail for m2k_create_task / m2k_create_task_ex. Exactly one of
// {fn, fn_ex} is set by the caller; the trampoline dispatches on fn_ex.
// Generation consumption and the pthread_create rollback are identical for
// both forms so handle streams stay deterministic regardless of entrypoint.
static m2k_item_t m2k_task_spawn(m2k_context* ctx, m2k_task_fn fn, uint32_t arg,
                                 m2k_task_fn_ex fn_ex, void* user_data,
                                 uint8_t priority, m2k_item_t notify_port,
                                 m2k_item_t notify_data) {
    if (!(fn || fn_ex)) return M2K_ITEM_NONE;
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;

    int32_t slot = m2k_free_task_slot(ctx);
    if (slot < 0) { pthread_mutex_unlock(&ctx->lock); return M2K_ITEM_NONE; }

    m2k_task* t = &ctx->tasks[slot];
    t->used = 1;
    t->generation = ++ctx->gen_counter;   // 1-based, monotonic per context
    t->state = M2K_TASK_STARTED;
    t->priority = priority;
    t->signal_wait_active = 0;
    t->signal_wait_mask = 0;
    t->kernel_boundary_seq = 0;
    // kernel/task.c initializes t_Quantum from DEFAULT_QUANTA_USECS and makes
    // the new task ready (CreateThread ends with NewReadyTask). The arms are
    // evaluated at the wake path; creation only records the placement.
    t->quantum = m2k_quantum_ticks();
    t->quantum_remainder = t->quantum;
    t->quantum_expired = 0;
    t->quantum_timer_armed = 0;
    t->quantaclock_deadline_ns = 0;
    t->run_started_ns = m2k_monotonic_ns();
    t->ready = 0;
    t->ready_at_head = 0;
    t->ready_seq = 0;
    t->arg = arg;
    t->fn = fn;
    t->fn_ex = fn_ex;
    t->user_data = user_data;
    m2k_task* owner_task = m2k_current_task_locked(ctx);
    t->owner = owner_task ? m2k_task_handle_locked(ctx, owner_task)
                           : M2K_ITEM_NONE;
    t->exit_status = 0;
    t->exit_notify_port = notify_port;
    t->exit_notify_data = notify_data;
    // Portfolio tasks reserve the low system signal bits and bit 31
    // (TaskNode starts with t_AllocatedSigs == 0x800000ff); user allocations
    // begin above the reserved system range.
    t->sig_allocated = M2K_SIGNAL_RESERVED_MASK;
    t->sig_pending = 0;
    t->wait_active = 0;
    t->wait_port = M2K_ITEM_NONE;
    t->wait_msg = M2K_ITEM_NONE;
    t->wait_result = M2K_ITEM_NONE;
    ctx->boots[slot].ctx = ctx;
    ctx->boots[slot].slot = slot;
    m2k_ready_insert_locked(ctx, t, 0);

    pthread_t tid;
    m2_diagnostic_task_reserve((uint32_t)slot);
    int prc = pthread_create(&tid, NULL, m2k_task_trampoline, &ctx->boots[slot]);
    if (prc != 0) {
        m2_diagnostic_task_leave((uint32_t)slot);
        t->used = 0;   // roll back: handle stream stays deterministic because
                       // the generation was consumed identically either way
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ITEM_NONE;
    }
    pthread_detach(tid);   // lifecycle sync happens via state + condvar, and
                           // the raw pthread_t never leaves this function
    m2k_item_t h = m2k_make_handle(M2K_TYPE_TASK, (uint32_t)slot, t->generation);
    pthread_mutex_unlock(&ctx->lock);
    return h;
}

m2k_item_t m2k_create_task(m2k_context* ctx, m2k_task_fn fn, uint32_t arg) {
    return m2k_task_spawn(ctx, fn, arg, NULL, NULL, 0, M2K_ITEM_NONE,
                          M2K_ITEM_NONE);
}

m2k_item_t m2k_create_task_ex(m2k_context* ctx, m2k_task_fn_ex fn_ex,
                              void* user_data) {
    return m2k_task_spawn(ctx, NULL, 0, fn_ex, user_data, 0, M2K_ITEM_NONE,
                          M2K_ITEM_NONE);
}

m2k_item_t m2k_create_task_ex_priority(m2k_context* ctx,
                                       m2k_task_fn_ex fn_ex,
                                       void* user_data, uint8_t priority) {
    return m2k_task_spawn(ctx, NULL, 0, fn_ex, user_data, priority,
                          M2K_ITEM_NONE, M2K_ITEM_NONE);
}

// CREATETASK_TAG_MSGFROMCHILD (kernel/task.h:248). Identical to
// m2k_create_task_ex_priority except that the created task carries a status
// message back to `notify_port` when it exits. `notify_data` is the item the
// source reports as msg_DataSize — the thread or task that terminated the
// task — which is the created task itself when it exits on its own.
m2k_item_t m2k_create_task_ex_notify_priority(m2k_context* ctx,
                                              m2k_task_fn_ex fn_ex,
                                              void* user_data, uint8_t priority,
                                              m2k_item_t notify_port,
                                              m2k_item_t notify_data) {
    return m2k_task_spawn(ctx, NULL, 0, fn_ex, user_data, priority, notify_port,
                          notify_data);
}

static int32_t m2k_rehydrate_task_ex_attach(m2k_context* ctx, m2k_item_t task,
                                            m2k_task_fn_ex fn_ex,
                                            void* user_data,
                                            uint8_t expected_state) {
    if (!fn_ex) return M2K_ERR_BAD_ITEM;
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;

    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    // A restored STARTED task has not entered its body, so restarting its
    // entrypoint is equivalent to completing the original pthread launch.
    // A restored RUNNING task is accepted only by the explicit continuation
    // wrapper below, after the caller has selected an original lifted label.
    // Never attach a second host thread to a task that already has a body.
    if (t->state != expected_state || t->fn || t->fn_ex || t->user_data ||
        t->state == M2K_TASK_COMPLETED || t->state == M2K_TASK_FAILED) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }

    const uint32_t slot = m2k_handle_slot(task);
    t->fn = NULL;
    t->fn_ex = fn_ex;
    t->user_data = user_data;
    t->state = expected_state;
    ctx->boots[slot].ctx = ctx;
    ctx->boots[slot].slot = (int32_t)slot;

    pthread_t tid;
    m2_diagnostic_task_reserve(slot);
    const int prc = pthread_create(&tid, NULL, m2k_task_trampoline,
                                   &ctx->boots[slot]);
    if (prc != 0) {
        m2_diagnostic_task_leave(slot);
        t->fn_ex = NULL;
        t->user_data = NULL;
        t->state = M2K_TASK_FAILED;
        t->exit_status = M2K_ERR_STATE;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;
    }
    pthread_detach(tid);
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_rehydrate_task_ex(m2k_context* ctx, m2k_item_t task,
                              m2k_task_fn_ex fn_ex, void* user_data) {
    return m2k_rehydrate_task_ex_attach(ctx, task, fn_ex, user_data,
                                         M2K_TASK_STARTED);
}

int32_t m2k_rehydrate_task_ex_continuation(m2k_context* ctx, m2k_item_t task,
                                           m2k_task_fn_ex fn_ex,
                                           void* user_data) {
    return m2k_rehydrate_task_ex_attach(ctx, task, fn_ex, user_data,
                                         M2K_TASK_RUNNING);
}

static m2k_task* m2k_resolve_task_locked(m2k_context* ctx, m2k_item_t task) {
    return (m2k_task*)m2k_resolve_item(ctx, task, M2K_TYPE_TASK,
                                       ctx->tasks, M2K_MAX_TASKS,
                                       sizeof(m2k_task));
}

m2k_item_t m2k_current_task_item(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ITEM_NONE;
    if (g_current_context != ctx || !g_current_task ||
        g_current_task < ctx->tasks ||
        g_current_task >= ctx->tasks + M2K_MAX_TASKS ||
        !g_current_task->used) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ITEM_NONE;
    }
    uint32_t slot = (uint32_t)(g_current_task - ctx->tasks);
    m2k_item_t item = m2k_make_handle(M2K_TYPE_TASK, slot,
                                      g_current_task->generation);
    pthread_mutex_unlock(&ctx->lock);
    return item;
}

int32_t m2k_task_signal_bits(m2k_context* ctx, m2k_item_t task,
                             uint32_t* bits_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }
    if (bits_out) *bits_out = t->sig_pending;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_task_priority(m2k_context* ctx, m2k_item_t task,
                          uint8_t* priority_out) {
    if (!priority_out || m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    *priority_out = t->priority;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

// kernel:157 SetItemPri on a TASKNODE, modelled from
// M2 SDK src/folios/kernel/task.c internalChangeTaskPri (line 533):
//
//   if ((t->t.n_ItemFlags & ITEMNODE_PRIVILEGED) == 0) {
//       if ((newpri > 199) || (newpri < 10))
//           return MAKEKERR(ER_SEVERE,ER_C_NSTND,ER_Kr_BadPriority);
//   }
//   oldpri = t->t.n_Priority;  ...  t->t.n_Priority = newpri;  ... return oldpri;
//
// The source then re-places the task on the priority-sorted ready queue
// (REMOVENODE + NewReadyTask, or RemHead + NewReadyTask when the task lowers
// its own priority).  This model's ready queue is sorted on read by
// m2k_ready_head_locked, so writing n_Priority is already equivalent to the
// re-insertion; nothing else about the task is touched.
int32_t m2k_set_task_priority(m2k_context* ctx, m2k_item_t task,
                              uint8_t newpri, int privileged,
                              uint8_t* old_priority_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (!privileged && (newpri > 199u || newpri < 10u)) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_PRIORITY;
    }
    const uint8_t oldpri = t->priority;
    t->priority = newpri;
    pthread_mutex_unlock(&ctx->lock);
    if (old_priority_out) *old_priority_out = oldpri;
    return (int32_t)oldpri;
}

void m2k_task_note_kernel_boundary(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return;
    m2k_task* task = m2k_current_task_locked(ctx);
    if (task) {
        const uint64_t now_ns = m2k_monotonic_ns();
        m2k_ready_handoff handoff = {0};
        // _cblank(): the quantaclock interrupt marks the running task expired.
        if (task->quantum_timer_armed &&
            now_ns >= task->quantaclock_deadline_ns) {
            task->quantum_expired = 1;
            m2k_disarm_quantaclock_locked(task);
        }
        // CheckForTaskSwap(): an expired task yields to the ready-order head
        // when one is at its priority or above. With no ready task there is
        // nothing to swap to and the flag stays set, exactly as the vendor
        // keeps TASK_QUANTUM_EXPIRED for NewReadyTask arm (1).
        if (task->quantum_expired) {
            m2k_task* head = m2k_ready_head_locked(ctx, task);
            if (head && head->priority >= task->priority) {
                m2k_task_land_ready_locked(ctx, task, now_ns);
                handoff = (m2k_ready_handoff){
                    .target = head,
                    .generation = head->generation,
                    .boundary = head->kernel_boundary_seq,
                };
            }
        }
        // This boundary is crossed before the swap, so a task waiting on it
        // never waits for work the yielding task does after the yield.
        ++task->kernel_boundary_seq;
        pthread_cond_broadcast(&ctx->cond);
        m2k_wait_ready_handoff_locked(ctx, handoff);
    }
    pthread_mutex_unlock(&ctx->lock);
}

int32_t m2k_task_quantum_state(m2k_context* ctx, m2k_item_t task,
                               m2k_task_quantum* out) {
    if (!out || m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    out->quantum = t->quantum;
    out->quantum_remainder = t->quantum_remainder;
    out->ready_seq = t->ready_seq;
    out->expired = t->quantum_expired;
    out->ready = t->ready;
    out->ready_at_head = t->ready_at_head;
    out->timer_armed = t->quantum_timer_armed;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_query_task(m2k_context* ctx, m2k_item_t task,
                       int32_t* state_out, int32_t* status_out) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (state_out) *state_out = (int32_t)t->state;
    if (status_out) *status_out = t->exit_status;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}

int32_t m2k_join_task(m2k_context* ctx, m2k_item_t task) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    m2k_task* waiter = m2k_current_task_locked(ctx);
    while (t->state != M2K_TASK_COMPLETED && t->state != M2K_TASK_FAILED) {
        if (waiter) m2k_task_block_locked(ctx, waiter);  // LandWaiter()
        m2_diagnostic_cond_wait(&ctx->cond, &ctx->lock);
        if (waiter) m2k_task_launch_locked(ctx, waiter, m2k_monotonic_ns());
    }
    int32_t rc = t->exit_status;
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int32_t m2k_reclaim_current_task_items(m2k_context* ctx) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* owner_task = m2k_current_task_locked(ctx);
    if (!owner_task) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_BAD_ITEM;
    }

    const m2k_item_t owner = m2k_task_handle_locked(ctx, owner_task);
    int32_t reclaimed = 0;
    int32_t first_error = M2K_OK;

    // Portfolio FreeResources() removes IOReq Items before the remaining
    // kernel Items.  Preserve that order so a request-owned completion
    // message cannot retain a live IOReq after the task has exited.
    for (uint32_t i = 0; i < M2K_MAX_IOREQS; ++i) {
        m2k_ioreq* q = &ctx->ioreqs[i];
        if (!q->used || q->owner != owner) continue;
        if (m2k_delete_ioreq_locked(ctx, q) == M2K_OK)
            ++reclaimed;
        else if (first_error == M2K_OK)
            first_error = M2K_ERR_STATE;
    }

    // Message Items remain registered to the task that created them even
    // while SendMsg transfers their current holder between a port and a
    // receiver. Portfolio FreeResources deletes those owned Items before
    // ReplyHeldMessages handles foreign messages left with the dying task.
    // DataStream relies on this: its successful TearDownStreamCB frees the
    // memory pools and intentionally leaves their Message Items to task exit.
    for (uint32_t i = 0; i < M2K_MESSAGE_CAPACITY; ++i) {
        m2k_message* m = m2k_message_at_index(ctx, i);
        if (!m || m->state == M2K_MSG_FREE || m->owner != owner) continue;
        if (m->state == M2K_MSG_QUEUED) {
            const int32_t rc = m2k_unlink_queued_message_locked(ctx, i);
            if (rc != M2K_OK) {
                if (first_error == M2K_OK) first_error = rc;
                continue;
            }
        }
        m->state = M2K_MSG_FREE;
        m->return_to_sender = 0;
        m->origin_port = -1;
        m->owner = M2K_ITEM_NONE;
        m->holder = M2K_ITEM_NONE;
        m->what = 0;
        m->value = 0;
        m->reply_port = M2K_ITEM_NONE;
        m->data_ptr = 0;
        m->data_size = 0;
        m->reply_code = 0;
        m->next = -1;
        ++reclaimed;
    }

    // The lifted DataStream/DataAcq teardown paths return all queued work
    // before their task body returns.  Match the source task-exit boundary for
    // those empty ports; leave a non-empty port intact rather than silently
    // dropping a cross-task message until the full ReplyHeldMessages/item
    // ownership contract is available here.
    for (uint32_t i = 0; i < M2K_MAX_PORTS; ++i) {
        m2k_msgport* p = &ctx->ports[i];
        if (!p->used || p->owner != owner) continue;
        if (p->queued_count != 0 || p->head != -1) {
            if (first_error == M2K_OK) first_error = M2K_ERR_STATE;
            continue;
        }
        if (p->signal_allocated && p->signal_mask != 0)
            owner_task->sig_allocated &= ~p->signal_mask;
        p->used = 0;
        ++reclaimed;
    }

    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return first_error != M2K_OK ? first_error : reclaimed;
}

int32_t m2k_delete_task(m2k_context* ctx, m2k_item_t task) {
    if (m2k_lock(ctx) != M2K_OK) return M2K_ERR_BAD_ITEM;
    m2k_task* t = m2k_resolve_task_locked(ctx, task);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return M2K_ERR_BAD_ITEM; }
    if (t->state == M2K_TASK_STARTED || t->state == M2K_TASK_RUNNING) {
        pthread_mutex_unlock(&ctx->lock);
        return M2K_ERR_STATE;   // never recycle a slot under a live thread
    }
    t->used = 0;
    pthread_mutex_unlock(&ctx->lock);
    return M2K_OK;
}
