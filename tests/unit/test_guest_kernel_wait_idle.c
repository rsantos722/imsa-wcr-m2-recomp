// Guest tasks blocked in WaitSignal stay asleep until signaled:
//   1. Waiters do not wake each other. A wake that finds no signal must not
//      broadcast, or two waiters keep each other running and burn CPU.
//   2. A signal sent after that idle period still reaches each waiter, through
//      both m2k_wait_signal and m2k_wait_signal_one.
#include <stdio.h>
#include <time.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "guest_kernel.h"

#define WAITERS 6
#define TEST_BIT 20
#define TEST_MASK (1u << TEST_BIT)

typedef struct {
    int use_one;
    int32_t got;
} waiter_ud;

static int32_t waiter_body(m2k_context* ctx, void* p) {
    waiter_ud* ud = (waiter_ud*)p;
    ud->got = ud->use_one ? m2k_wait_signal_one(ctx, TEST_MASK)
                          : m2k_wait_signal(ctx, TEST_MASK);
    return 0;
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

#ifndef _WIN32
static double process_cpu_ms(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 +
           (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
}
#endif

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void) {
    m2k_context ctx;
    m2k_init(&ctx);

    waiter_ud uds[WAITERS];
    m2k_item_t tasks[WAITERS];
    for (int i = 0; i < WAITERS; ++i) {
        uds[i].use_one = i & 1;
        uds[i].got = -1;
        tasks[i] = m2k_create_task_ex(&ctx, waiter_body, &uds[i]);
        CHECK(tasks[i] != M2K_ITEM_NONE);
    }
    sleep_ms(100);  // Let every task reach its wait.

#ifndef _WIN32
    const long idle_ms = 400;
    const double before = process_cpu_ms();
    sleep_ms(idle_ms);
    const double used = process_cpu_ms() - before;
    printf("idle waiters used %.1f ms CPU over %ld ms\n", used, idle_ms);
    CHECK(used < idle_ms / 4.0);
#endif

    for (int i = 0; i < WAITERS; ++i) {
        CHECK(m2k_send_signal(&ctx, tasks[i], TEST_BIT) == M2K_OK);
        CHECK(m2k_join_task(&ctx, tasks[i]) == 0);
        CHECK(uds[i].got == (int32_t)TEST_MASK);
        CHECK(m2k_delete_task(&ctx, tasks[i]) == M2K_OK);
    }

    m2k_destroy(&ctx);
    printf("OK: idle signal waiters stay asleep and still receive signals\n");
    return 0;
}
