// Reset/reopen lifecycle.
//
// Exercise the production recomp kernel binding through a no-boot restore
// boundary, a completed guest task, shutdown, and a second restore boundary.
// The old item handle must not cross the destroyed kernel epoch, while the
// reopened context must still accept normal message-port traffic.

#include <stdint.h>
#include <stdio.h>

#include "guest_kernel.h"
#include "recomp_host.h"

static int32_t completed_task(m2k_context* ctx, uint32_t arg) {
    (void)ctx;
    return arg == 0x51u ? 0 : -1;
}

int main(void) {
    int failures = 0;
    PPCContext* cpu = m2_recomp_context();
    if (!cpu || !cpu->mem) {
        fprintf(stderr, "m2_reset_reopen_lifecycle_test: no guest context\n");
        return 1;
    }

    if (m2_guest_prepare_restore() != 0) {
        fprintf(stderr, "m2_reset_reopen_lifecycle_test: initial prepare failed\n");
        return 1;
    }
    m2k_context* first = m2_guest_kernel_context();
    const m2k_item_t old_port = first ? m2k_create_msgport(first) : M2K_ITEM_NONE;
    if (!first || old_port == M2K_ITEM_NONE) {
        fprintf(stderr, "m2_reset_reopen_lifecycle_test: initial port failed\n");
        ++failures;
    }

    // A terminal task is joined and deleted before shutdown; this proves the
    // normal task-owner lifecycle does not leave a live callback in the pool.
    const m2k_item_t task = first
        ? m2k_create_task(first, completed_task, 0x51u) : M2K_ITEM_NONE;
    if (task == M2K_ITEM_NONE || m2k_join_task(first, task) != M2K_OK ||
        m2k_delete_task(first, task) != M2K_OK)
        ++failures;

    m2_recomp_shutdown();
    if (m2_guest_kernel_context() != NULL)
        ++failures;

    if (m2_guest_prepare_restore() != 0) {
        fprintf(stderr, "m2_reset_reopen_lifecycle_test: reopen prepare failed\n");
        ++failures;
    }
    m2k_context* second = m2_guest_kernel_context();
    const m2k_item_t new_port = second
        ? m2k_create_msgport(second) : M2K_ITEM_NONE;
    if (!second || new_port == M2K_ITEM_NONE || new_port == old_port)
        ++failures;
    if (second && m2k_send_msg(second, old_port, 1u, 2u) != M2K_ERR_BAD_ITEM)
        ++failures;
    if (second && new_port != M2K_ITEM_NONE) {
        if (m2k_send_msg(second, new_port, 3u, 4u) != M2K_OK)
            ++failures;
        m2k_item_t message = M2K_ITEM_NONE;
        if (m2k_get_msg(second, new_port, &message, NULL, NULL) != M2K_OK)
            ++failures;
    }

    m2_recomp_shutdown();
    if (failures != 0) {
        fprintf(stderr, "m2_reset_reopen_lifecycle_test: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("m2_reset_reopen_lifecycle_test: OK");
    return 0;
}
