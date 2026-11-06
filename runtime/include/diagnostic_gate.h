#pragma once

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in debugger suspension. Native guest stacks remain in this process.
// Enable before spawning tasks; normal execution does not acquire this lock.
#define M2_DIAGNOSTIC_TASKS 8u
enum { M2_DIAG_UNUSED, M2_DIAG_ACTIVE, M2_DIAG_PARKED, M2_DIAG_WAITING };
typedef struct {
    uint32_t state;
    uint32_t pc; // last platform/loop boundary, not an instruction pointer
} m2_diagnostic_task;

void m2_diagnostic_enable(void);
int m2_diagnostic_enabled(void);
void m2_diagnostic_resume(void);
void m2_diagnostic_request_pause(void);
int m2_diagnostic_is_paused(void);
void m2_diagnostic_tasks(m2_diagnostic_task out[M2_DIAGNOSTIC_TASKS]);
void m2_diagnostic_task_reserve(uint32_t slot);
void m2_diagnostic_task_enter(uint32_t slot);
void m2_diagnostic_task_leave(uint32_t slot);
void m2_diagnostic_checkpoint(uint32_t pc);
// Same condition-variable contract: enters and returns with mutex held.
// A sleeping guest cannot resume into its caller while the debugger is paused.
void m2_diagnostic_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int m2_diagnostic_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                 const struct timespec* deadline);
// Absolute CLOCK_MONOTONIC deadline. Darwin lacks condattr_setclock and
// winpthreads does not reliably honor it, so both convert the deadline (Darwin
// to its relative-wait primitive, Windows to a CLOCK_REALTIME deadline).
int m2_diagnostic_cond_timedwait_monotonic(
    pthread_cond_t* cond, pthread_mutex_t* mutex,
    const struct timespec* deadline);

#ifdef __cplusplus
}
#endif
