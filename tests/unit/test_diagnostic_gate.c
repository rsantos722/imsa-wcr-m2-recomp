// Diagnostic suspension gate: startup reservation, quiescence, immediate
// re-pause, waking a blocked task while paused, and its retained continuation.
#include "diagnostic_gate.h"
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
static _Atomic unsigned progress;
static _Atomic int stop;
static pthread_mutex_t external = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event = PTHREAD_COND_INITIALIZER;
static int ready;

static int await_pause(void) {
    for (unsigned n = 0; n < 10000; ++n) {
        if (m2_diagnostic_is_paused()) return 1;
        usleep(100);
    }
    return 0;
}
static void* runner(void* unused) {
    (void)unused;
    m2_diagnostic_task_enter(0);
    while (!atomic_load(&stop)) {
        atomic_fetch_add(&progress, 1);
        m2_diagnostic_checkpoint(0x1234);
    }
    m2_diagnostic_task_leave(0);
    return NULL;
}
static void* waiter(void* unused) {
    (void)unused;
    m2_diagnostic_task_enter(1);
    pthread_mutex_lock(&external);
    while (!ready) m2_diagnostic_cond_wait(&event, &external);
    atomic_fetch_add(&progress, 1);
    pthread_mutex_unlock(&external);
    m2_diagnostic_task_leave(1);
    return NULL;
}
int main(void) {
    CHECK(!m2_diagnostic_enabled());
    m2_diagnostic_checkpoint(1); // normal mode is a no-op
    m2_diagnostic_enable();
    CHECK(m2_diagnostic_is_paused());
    m2_diagnostic_task_reserve(0);
    CHECK(!m2_diagnostic_is_paused()); // includes not-yet-started pthread
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, runner, NULL) == 0);
    CHECK(await_pause());
    CHECK(atomic_load(&progress) == 0);
    for (unsigned i = 0; i < 100; ++i) {
        m2_diagnostic_resume();
        if (i == 0) usleep(1000);
        m2_diagnostic_request_pause(); // also tests immediate re-pause race
        CHECK(await_pause());
        unsigned snapshot = atomic_load(&progress);
        usleep(1000);
        CHECK(atomic_load(&progress) == snapshot);
    }
    CHECK(atomic_load(&progress) > 0);
    atomic_store(&stop, 1);
    m2_diagnostic_resume();
    CHECK(pthread_join(worker, NULL) == 0);

    m2_diagnostic_task_reserve(1);
    CHECK(pthread_create(&worker, NULL, waiter, NULL) == 0);
    m2_diagnostic_task tasks[M2_DIAGNOSTIC_TASKS];
    unsigned n;
    for (n = 0; n < 10000; ++n) {
        m2_diagnostic_tasks(tasks);
        if (tasks[1].state == M2_DIAG_WAITING) break;
        usleep(100);
    }
    CHECK(n < 10000);
    m2_diagnostic_request_pause();
    CHECK(await_pause());
    unsigned snapshot = atomic_load(&progress);
    // Waking a sleeping task while paused must not execute its continuation
    // or leave the external mutex locked while it parks.
    pthread_mutex_lock(&external);
    ready = 1;
    pthread_cond_signal(&event);
    pthread_mutex_unlock(&external);
    usleep(10000);
    CHECK(atomic_load(&progress) == snapshot);
    CHECK(pthread_mutex_trylock(&external) == 0);
    pthread_mutex_unlock(&external);
    CHECK(m2_diagnostic_is_paused());
    m2_diagnostic_resume();
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(atomic_load(&progress) == snapshot + 1);
    m2_diagnostic_request_pause();
    CHECK(await_pause());
    puts("OK: startup reservation, quiescence, immediate re-pause, blocked wake, retained continuation");
    return 0;
}
