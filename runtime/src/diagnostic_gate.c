#include "diagnostic_gate.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic int enabled;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int pause_requested = 1;
static m2_diagnostic_task tasks[M2_DIAGNOSTIC_TASKS];
static _Thread_local int task_slot = -1;

int m2_diagnostic_enabled(void) { return atomic_load(&enabled); }

void m2_diagnostic_enable(void) {
    pthread_mutex_lock(&lock);
    memset(tasks, 0, sizeof tasks);
    pause_requested = 1;
    atomic_store(&enabled, 1);
    pthread_mutex_unlock(&lock);
}

void m2_diagnostic_resume(void) {
    pthread_mutex_lock(&lock);
    pause_requested = 0;
    // Mark parked threads active before broadcasting. A subsequent pause
    // must wait for them to acknowledge it, including unscheduled threads.
    for (unsigned i = 0; i < M2_DIAGNOSTIC_TASKS; ++i)
        if (tasks[i].state == M2_DIAG_PARKED)
            tasks[i].state = M2_DIAG_ACTIVE;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

void m2_diagnostic_request_pause(void) {
    pthread_mutex_lock(&lock);
    pause_requested = 1;
    pthread_mutex_unlock(&lock);
}

int m2_diagnostic_is_paused(void) {
    pthread_mutex_lock(&lock);
    int paused = pause_requested;
    for (unsigned i = 0; i < M2_DIAGNOSTIC_TASKS; ++i)
        if (tasks[i].state == M2_DIAG_ACTIVE) paused = 0;
    pthread_mutex_unlock(&lock);
    return paused;
}

void m2_diagnostic_tasks(m2_diagnostic_task out[M2_DIAGNOSTIC_TASKS]) {
    pthread_mutex_lock(&lock);
    memcpy(out, tasks, sizeof tasks);
    pthread_mutex_unlock(&lock);
}

void m2_diagnostic_task_reserve(uint32_t slot) {
    if (!m2_diagnostic_enabled() || slot >= M2_DIAGNOSTIC_TASKS) return;
    pthread_mutex_lock(&lock);
    tasks[slot] = (m2_diagnostic_task){M2_DIAG_ACTIVE, 0};
    pthread_mutex_unlock(&lock);
}

void m2_diagnostic_checkpoint(uint32_t pc) {
    if (!m2_diagnostic_enabled() || task_slot < 0) return;
    pthread_mutex_lock(&lock);
    if (pc) tasks[task_slot].pc = pc;
    while (pause_requested) {
        tasks[task_slot].state = M2_DIAG_PARKED;
        pthread_cond_wait(&changed, &lock);
    }
    tasks[task_slot].state = M2_DIAG_ACTIVE;
    pthread_mutex_unlock(&lock);
}

void m2_diagnostic_task_enter(uint32_t slot) {
    if (!m2_diagnostic_enabled() || slot >= M2_DIAGNOSTIC_TASKS) return;
    task_slot = (int)slot;
    m2_diagnostic_checkpoint(0);
}

void m2_diagnostic_task_leave(uint32_t slot) {
    if (!m2_diagnostic_enabled() || slot >= M2_DIAGNOSTIC_TASKS) return;
    pthread_mutex_lock(&lock);
    tasks[slot].state = M2_DIAG_UNUSED;
    pthread_mutex_unlock(&lock);
    if (task_slot == (int)slot) task_slot = -1;
}

void m2_diagnostic_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!m2_diagnostic_enabled() || task_slot < 0) {
        pthread_cond_wait(cond, mutex);
        return;
    }
    pthread_mutex_lock(&lock);
    tasks[task_slot].state = M2_DIAG_WAITING;
    pthread_mutex_unlock(&lock);
    pthread_cond_wait(cond, mutex);
    // Never park holding a kernel or startup lock. The caller rechecks its
    // predicate after every wait, as required for ordinary spurious wakes.
    pthread_mutex_unlock(mutex);
    m2_diagnostic_checkpoint(0);
    pthread_mutex_lock(mutex);
}

int m2_diagnostic_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                 const struct timespec* deadline) {
    if (!m2_diagnostic_enabled() || task_slot < 0)
        return pthread_cond_timedwait(cond, mutex, deadline);
    pthread_mutex_lock(&lock);
    tasks[task_slot].state = M2_DIAG_WAITING;
    pthread_mutex_unlock(&lock);
    const int rc = pthread_cond_timedwait(cond, mutex, deadline);
    pthread_mutex_unlock(mutex);
    m2_diagnostic_checkpoint(0);
    pthread_mutex_lock(mutex);
    return rc;
}

static int m2_cond_timedwait_monotonic(pthread_cond_t* cond,
                                       pthread_mutex_t* mutex,
                                       const struct timespec* deadline) {
#if defined(__APPLE__) || defined(_WIN32)
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec relative = {0};
    if (deadline->tv_sec > now.tv_sec ||
        (deadline->tv_sec == now.tv_sec && deadline->tv_nsec > now.tv_nsec)) {
        relative.tv_sec = deadline->tv_sec - now.tv_sec;
        relative.tv_nsec = deadline->tv_nsec - now.tv_nsec;
        if (relative.tv_nsec < 0) {
            --relative.tv_sec;
            relative.tv_nsec += 1000000000L;
        }
    }
#if defined(__APPLE__)
    return pthread_cond_timedwait_relative_np(cond, mutex, &relative);
#else
    // winpthreads measures timedwait deadlines against the wall clock and
    // does not reliably honor a condattr clock, so rebase the remaining
    // monotonic interval onto CLOCK_REALTIME (the cond uses default attrs).
    struct timespec wall = {0};
    clock_gettime(CLOCK_REALTIME, &wall);
    wall.tv_sec += relative.tv_sec;
    wall.tv_nsec += relative.tv_nsec;
    if (wall.tv_nsec >= 1000000000L) {
        ++wall.tv_sec;
        wall.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &wall);
#endif
#else
    return pthread_cond_timedwait(cond, mutex, deadline);
#endif
}

int m2_diagnostic_cond_timedwait_monotonic(
    pthread_cond_t* cond, pthread_mutex_t* mutex,
    const struct timespec* deadline) {
    if (!m2_diagnostic_enabled() || task_slot < 0)
        return m2_cond_timedwait_monotonic(cond, mutex, deadline);
    pthread_mutex_lock(&lock);
    tasks[task_slot].state = M2_DIAG_WAITING;
    pthread_mutex_unlock(&lock);
    const int rc = m2_cond_timedwait_monotonic(cond, mutex, deadline);
    pthread_mutex_unlock(mutex);
    m2_diagnostic_checkpoint(0);
    pthread_mutex_lock(mutex);
    return rc;
}
