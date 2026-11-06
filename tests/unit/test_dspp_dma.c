// test_dspp_dma.c — DSPP FDMA serviceable-channel mask and priority.
//
// The simulator services one DMA channel before every DSP instruction. This
// test pins the serviceable mask's externally visible contract: no enabled
// channel means no service, enabled channels are considered in descending
// hardware order, disabling a channel removes it, and shared-space sims see
// the same one hardware-shaped DMA state.
#include "dsp_template_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do {                                                    \
    if (!(expr)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
        ++failures;                                                         \
    }                                                                       \
} while (0)

// DSPN_OPCODE_SLEEP (dspp_exec_control mode 0 / super-special SLEEP). The
// simulator only needs a valid cyclic program for these DMA-only checks.
#define TEST_DSPP_SLEEP 0x8380u

static dspp_template make_template(void) {
    dspp_template t;
    memset(&t, 0, sizeof(t));
    t.code_word_count = 1;
    t.code_words[0] = TEST_DSPP_SLEEP;
    return t;
}

// Arm an input-direction descriptor. The FDMA service consumes the buffer
// into the DSPP FIFO; its observable effect here is that the channel is
// ready, remains in descending hardware priority, and is retired after the
// four-sample descriptor is drained.
static void arm_input_channel(dspp_sim *sim, uint32_t channel,
                              uint8_t *buffer) {
    dspp_sim_dma_set_control(sim, channel, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, channel, buffer, 4, 0) == 0);
}

static void arm_output_channel(dspp_sim *sim, uint32_t channel,
                               uint8_t *buffer) {
    dspp_sim_dma_set_control(sim, channel, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, channel, buffer, 8,
                                   DSPP_SIM_DMA_TO_RAM) == 0);
}

// DSPI FIFO registers and the DSPX channel reset, as the DSPP and CPU see them.
#define TEST_FIFO_DATA(channel) (0x0300u + (channel) * 8u + 4u)
#define TEST_FIFO_DEPTH(channel) (0x0300u + (channel) * 8u + 5u)
#define TEST_DSPX_CHANNEL_RESET 0x66030u

// Three MOVEs and a SLEEP: four instructions, so four chances per tick for
// the FDMA to service a channel.
static dspp_template make_moving_template(void) {
    dspp_template t;
    memset(&t, 0, sizeof(t));
    t.code_word_count = 7;
    for (uint32_t i = 0; i < 3u; ++i) {
        t.code_words[i * 2u] = (uint16_t)(0x9800u | (0x010u + i));
        t.code_words[i * 2u + 1u] = (uint16_t)(0x8000u | 0x020u);
    }
    t.code_words[6] = TEST_DSPP_SLEEP;
    return t;
}

static uint32_t fifo_depth(dspp_sim *sim, uint32_t channel) {
    return dspp_sim_dspi_read(sim, TEST_FIFO_DEPTH(channel));
}

static void drain_words(dspp_sim *sim, uint32_t channel, uint32_t words) {
    for (uint32_t i = 0; i < words; ++i)
        (void)dspp_sim_dspi_read(sim, TEST_FIFO_DATA(channel));
}

// A tick skips the FDMA check once it has found nothing to service. Each case
// below reaches that idle state, changes channel state or a FIFO depth through
// one write path, and requires the next tick to act on the change.
static void test_tick_resumes_fdma_after_idle(void) {
    const dspp_template moving = make_moving_template();
    uint8_t input[64];
    for (uint32_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;

    // DSPP-side FIFO reads free room for a stalled input channel.
    dspp_sim *sim = dspp_sim_create(&moving);
    CHECK(sim != NULL);
    if (!sim) return;
    dspp_sim_dma_set_control(sim, 2, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, 2, input, 32, 0) == 0);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 2) == 8u);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 2) == 8u);
    drain_words(sim, 2, 4);
    CHECK(fifo_depth(sim, 2) == 4u);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 2) == 8u);

    // A channel reset empties the FIFO of a channel with data still to send.
    dspp_sim_dma_register_write(sim, TEST_DSPX_CHANNEL_RESET, 1u << 2);
    CHECK(fifo_depth(sim, 2) == 0u);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 2) == 8u);

    // Enabling a channel after idle ticks makes it serviceable.
    dspp_sim_dma_set_control(sim, 9, 0, 0);
    CHECK(dspp_sim_dma_set_current(sim, 9, input, 4, 0) == 0);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 9) == 0u);
    dspp_sim_dma_set_control(sim, 9, 1, 0);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 9) == 4u);
    dspp_sim_destroy(sim);

    // Output channel: a host FIFO push reaches the service threshold. The
    // push shares its cursor with DMA reads, so only the drain is observable.
    uint8_t output[8];
    sim = dspp_sim_create(&moving);
    CHECK(sim != NULL);
    if (!sim) return;
    dspp_sim_dma_set_control(sim, 4, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, 4, output, 4,
                                   DSPP_SIM_DMA_TO_RAM) == 0);
    dspp_sim_tick(sim);
    for (uint32_t i = 0; i < 4u; ++i)
        dspp_sim_fifo_push(sim, 4, (int16_t)(0x1100 + i));
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 4) == 0u);
    dspp_sim_destroy(sim);

    // Output channel: a DSPP-side FIFO write reaches the service threshold.
    memset(output, 0, sizeof(output));
    sim = dspp_sim_create(&moving);
    CHECK(sim != NULL);
    if (!sim) return;
    dspp_sim_dma_set_control(sim, 5, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, 5, output, 4,
                                   DSPP_SIM_DMA_TO_RAM) == 0);
    dspp_sim_tick(sim);
    for (uint32_t i = 0; i < 4u; ++i)
        dspp_sim_dspi_write(sim, TEST_FIFO_DATA(5), (uint16_t)(0x2200u + i));
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 5) == 0u);
    CHECK(output[6] == 0x22 && output[7] == 0x03);
    dspp_sim_destroy(sim);

    // One service per instruction, highest channel first, also holds when two
    // stalled channels become ready together. A one-instruction program can
    // only service one of them per tick.
    const dspp_template sleep_only = make_template();
    sim = dspp_sim_create(&sleep_only);
    CHECK(sim != NULL);
    if (!sim) return;
    uint8_t low[32] = {0};
    uint8_t high[32] = {0};
    dspp_sim_dma_set_control(sim, 1, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, 1, low, 16, 0) == 0);
    dspp_sim_dma_set_control(sim, 3, 1, 0);
    CHECK(dspp_sim_dma_set_current(sim, 3, high, 16, 0) == 0);
    for (uint32_t i = 0; i < 5u; ++i) dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 1) == 8u && fifo_depth(sim, 3) == 8u);
    drain_words(sim, 1, 4);
    drain_words(sim, 3, 4);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 3) == 8u && fifo_depth(sim, 1) == 4u);
    dspp_sim_tick(sim);
    CHECK(fifo_depth(sim, 3) == 8u && fifo_depth(sim, 1) == 8u);
    dspp_sim_destroy(sim);

    // Shared-space sims share one hardware block, so a change made through
    // one sim resumes servicing in the other sim's tick.
    dspp_sim_space *space = dspp_sim_space_create();
    CHECK(space != NULL);
    if (!space) return;
    dspp_sim *a = dspp_sim_create_in_space(&moving, space);
    dspp_sim *b = dspp_sim_create_in_space(&moving, space);
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        dspp_sim_dma_set_control(a, 7, 1, 0);
        CHECK(dspp_sim_dma_set_current(a, 7, input, 32, 0) == 0);
        dspp_sim_tick(a);
        dspp_sim_tick(b);
        CHECK(fifo_depth(a, 7) == 8u);
        drain_words(b, 7, 4);
        dspp_sim_tick(a);
        CHECK(fifo_depth(a, 7) == 8u);
    }
    if (a) dspp_sim_destroy(a);
    if (b) dspp_sim_destroy(b);
    dspp_sim_space_destroy(space);
}

int main(void) {
    test_tick_resumes_fdma_after_idle();

    const dspp_template t = make_template();
    dspp_sim *sim = dspp_sim_create(&t);
    CHECK(sim != NULL);
    if (!sim) return 1;

    // No enabled channel: the mask fast path must report no service.
    CHECK(dspp_sim_dma_step(sim) == -1);

    uint8_t low_buffer[8] = {0x10, 0x00, 0x10, 0x01, 0x10, 0x02, 0x10, 0x03};
    uint8_t high_buffer[8] = {0x30, 0x00, 0x30, 0x01, 0x30, 0x02, 0x30, 0x03};
    arm_input_channel(sim, 1, low_buffer);
    arm_input_channel(sim, 3, high_buffer);

    // Both channels are ready; the higher channel number wins the hardware
    // priority order.
    CHECK(dspp_sim_dma_step(sim) == 3);
    CHECK(dspp_sim_dma_step(sim) == 1);

    // Both transfers completed and disabled their channels.
    CHECK(dspp_sim_dma_step(sim) == -1);

    // Disabling an enabled channel must clear its serviceable bit.
    uint8_t disabled_buffer[8] = {0};
    arm_input_channel(sim, 5, disabled_buffer);
    dspp_sim_dma_set_control(sim, 5, 0, 0);
    CHECK(dspp_sim_dma_step(sim) == -1);

    // Re-enabling with a fresh descriptor makes it serviceable again.
    arm_input_channel(sim, 5, disabled_buffer);
    CHECK(dspp_sim_dma_step(sim) == 5);

    // Output-direction channels become serviceable only at the four-word
    // FIFO threshold; below it the per-instruction check must not service
    // them.
    uint8_t output_buffer[16] = {0};
    arm_output_channel(sim, 6, output_buffer);
    CHECK(dspp_sim_dma_step(sim) == -1);
    dspp_sim_fifo_push(sim, 6, 0x6000);
    dspp_sim_fifo_push(sim, 6, 0x6001);
    dspp_sim_fifo_push(sim, 6, 0x6002);
    CHECK(dspp_sim_dma_step(sim) == -1);
    dspp_sim_fifo_push(sim, 6, 0x6003);
    CHECK(dspp_sim_dma_step(sim) == 6);

    dspp_sim_destroy(sim);

    // Shared-space sims resolve DMA state and the serviceable mask through
    // the one hardware block: a channel enabled by one sim is serviceable by
    // another sim in the same space.
    dspp_sim_space *space = dspp_sim_space_create();
    CHECK(space != NULL);
    if (space) {
        dspp_sim *a = dspp_sim_create_in_space(&t, space);
        dspp_sim *b = dspp_sim_create_in_space(&t, space);
        CHECK(a != NULL && b != NULL);
        if (a && b) {
            uint8_t shared_buffer[8] = {0};
            dspp_sim_dma_set_control(a, 7, 1, 0);
            CHECK(dspp_sim_dma_set_current(b, 7, shared_buffer, 4, 0) == 0);
            CHECK(dspp_sim_dma_step(b) == 7);
        }
        if (a) dspp_sim_destroy(a);
        if (b) dspp_sim_destroy(b);
        dspp_sim_space_destroy(space);
    }

    // The export table revision must move on both register and drop: the
    // per-DAC-frame staging reuses a resolved gMixer/Amplitude address until
    // this value changes.
    dspp_template exp_t;
    memset(&exp_t, 0, sizeof(exp_t));
    exp_t.code_word_count = 1;
    exp_t.code_words[0] = TEST_DSPP_SLEEP;
    exp_t.resource_count = 1;
    snprintf(exp_t.resources[0].name, sizeof(exp_t.resources[0].name),
             "gTestExport");
    exp_t.resources[0].type = 4;    // DRSC_TYPE_OUTPUT
    exp_t.resources[0].flags = 0x40; // DRSC_F_EXPORT
    exp_t.resources[0].many = 1;
    space = dspp_sim_space_create();
    CHECK(space != NULL);
    if (space) {
        const uint32_t gen_empty = dspp_sim_space_export_generation(space);
        dspp_sim *exporter = dspp_sim_create_in_space(&exp_t, space);
        CHECK(exporter != NULL);
        const uint32_t gen_open = dspp_sim_space_export_generation(space);
        CHECK(gen_open != gen_empty);
        uint32_t addr = 0xFFFFFFFFu;
        CHECK(dspp_sim_space_export_address(space, "gTestExport", &addr) == 0);
        CHECK(addr != 0xFFFFFFFFu);
        if (exporter) dspp_sim_destroy(exporter);
        CHECK(dspp_sim_space_export_generation(space) != gen_open);
        addr = 0xFFFFFFFFu;
        CHECK(dspp_sim_space_export_address(space, "gTestExport", &addr) != 0);
        dspp_sim_space_destroy(space);
    }

    if (failures != 0) {
        fprintf(stderr, "m2_dspp_dma_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("m2_dspp_dma_test: PASS\n");
    return 0;
}
