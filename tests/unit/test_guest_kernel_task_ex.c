// Guest task extended API (m2k_create_task_ex and m2k_rehydrate_task_ex):
//   1. user_data reaches the body verbatim (opaque pointer round-trip).
//   2. Completion is exactly once: the body runs a single time and repeated
//      joins observe the same terminal status without re-running it.
//   3. A restored task slot can regain one host entrypoint without consuming
//      a new guest handle generation or allowing duplicate host threads.
#include <stdio.h>
#include "guest_kernel.h"

typedef struct {
    uint32_t cookie;
    int      calls;
} task_ud;

static int32_t task_body(m2k_context* ctx, void* p) {
    task_ud* ud = (task_ud*)p;
    ud->calls++;
    return ud->cookie == 0xC0FFEEu ? 42 : -1;
}

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void) {
    m2k_context ctx;
    m2k_init(&ctx);

    task_ud ud = { 0xC0FFEEu, 0 };
    m2k_item_t h = m2k_create_task_ex(&ctx, task_body, &ud);
    CHECK(h != M2K_ITEM_NONE);
    CHECK(m2k_handle_type(h) == M2K_TYPE_TASK);

    int32_t rc = m2k_join_task(&ctx, h);
    CHECK(rc == 42);
    CHECK(ud.calls == 1);              // user_data delivered, ran once

    int32_t state = -1, status = -1;
    CHECK(m2k_query_task(&ctx, h, &state, &status) == M2K_OK);
    CHECK(state == M2K_TASK_COMPLETED && status == 42);

    rc = m2k_join_task(&ctx, h);       // repeat join: same status, no re-run
    CHECK(rc == 42);
    CHECK(ud.calls == 1);              // exactly-once completion

    CHECK(m2k_delete_task(&ctx, h) == M2K_OK);
    m2k_destroy(&ctx);

    m2k_context restored;
    m2k_init(&restored);
    const uint32_t slot = 3;
    const uint32_t generation = 37;
    restored.gen_counter = generation;
    restored.tasks[slot].used = 1;
    restored.tasks[slot].generation = generation;
    restored.tasks[slot].state = M2K_TASK_STARTED;
    restored.tasks[slot].sig_allocated = M2K_SIGNAL_RESERVED_MASK;
    const m2k_item_t restored_h =
        (generation << M2K_HANDLE_GEN_SHIFT) |
        (slot << M2K_HANDLE_SLOT_SHIFT) | M2K_TYPE_TASK;
    task_ud rehydrated_ud = { 0xC0FFEEu, 0 };
    CHECK(m2k_rehydrate_task_ex(&restored, restored_h, task_body,
                                &rehydrated_ud) == M2K_OK);
    CHECK(m2k_join_task(&restored, restored_h) == 42);
    CHECK(rehydrated_ud.calls == 1);
    CHECK(m2k_handle_gen(restored_h) == generation);
    CHECK(m2k_query_task(&restored, restored_h, &state, &status) == M2K_OK);
    CHECK(state == M2K_TASK_COMPLETED && status == 42);
    CHECK(m2k_rehydrate_task_ex(&restored, restored_h, task_body,
                                &rehydrated_ud) == M2K_ERR_STATE);
    CHECK(m2k_delete_task(&restored, restored_h) == M2K_OK);

    const uint32_t running_slot = 4;
    const uint32_t running_generation = 38;
    restored.tasks[running_slot].used = 1;
    restored.tasks[running_slot].generation = running_generation;
    restored.tasks[running_slot].state = M2K_TASK_RUNNING;
    const m2k_item_t running_h =
        (running_generation << M2K_HANDLE_GEN_SHIFT) |
        (running_slot << M2K_HANDLE_SLOT_SHIFT) | M2K_TYPE_TASK;
    CHECK(m2k_rehydrate_task_ex(&restored, running_h, task_body,
                                &rehydrated_ud) == M2K_ERR_STATE);
    CHECK(restored.tasks[running_slot].fn_ex == NULL &&
          restored.tasks[running_slot].user_data == NULL);
    restored.tasks[running_slot].used = 0;

    // The explicit continuation API accepts a restored RUNNING record only
    // after the caller has selected an original lifted resume label. The
    // substrate itself preserves the existing handle generation and attaches
    // exactly one fresh host body.
    restored.tasks[running_slot].used = 1;
    restored.tasks[running_slot].generation = running_generation;
    restored.tasks[running_slot].state = M2K_TASK_RUNNING;
    task_ud continued_ud = { 0xC0FFEEu, 0 };
    CHECK(m2k_rehydrate_task_ex_continuation(&restored, running_h, task_body,
                                             &continued_ud) == M2K_OK);
    CHECK(m2k_join_task(&restored, running_h) == 42);
    CHECK(continued_ud.calls == 1);
    CHECK(m2k_handle_gen(running_h) == running_generation);
    CHECK(m2k_query_task(&restored, running_h, &state, &status) == M2K_OK);
    CHECK(state == M2K_TASK_COMPLETED && status == 42);
    CHECK(m2k_rehydrate_task_ex_continuation(&restored, running_h, task_body,
                                             &continued_ud) == M2K_ERR_STATE);
    CHECK(m2k_delete_task(&restored, running_h) == M2K_OK);

    m2k_destroy(&restored);

    printf("OK: user_data callback + exactly-once completion + task rehydration/continuation\n");
    return 0;
}
