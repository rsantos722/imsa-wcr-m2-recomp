// test_dspp_exec.c — golden execution for the DSPP interpreter.
//
// The interpreter is the guest-visible DSP program engine. Before changing
// operand decoding or dispatch, pin a small program with a known data-memory
// result so a decoder regression cannot silently alter audio state.
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

// MOVE direct dst=0x10, ADDR src=0x20
#define MOVE_DIRECT(dst) (uint16_t)(0x9800u | ((dst) & 0x03FFu))
#define OPERAND_ADDR(addr) (uint16_t)(0x8000u | ((addr) & 0x03FFu))
#define OPERAND_ADDR_IND(addr) (uint16_t)(0x8000u | 0x0400u | ((addr) & 0x03FFu))
#define OPERAND_REG(regdi) (uint16_t)(0xA000u | ((regdi) & 0x001Fu))
#define OPERAND_IMMEDIATE(value) (uint16_t)(0xC000u | ((value) & 0x1FFFu))
#define DSPP_SLEEP 0x8380u

int main(void) {
    dspp_template t;
    memset(&t, 0, sizeof(t));
    t.code_word_count = 5;
    t.code_words[0] = MOVE_DIRECT(0x010);
    t.code_words[1] = OPERAND_ADDR(0x020);
    t.code_words[2] = MOVE_DIRECT(0x011);
    t.code_words[3] = OPERAND_IMMEDIATE(0x123);
    t.code_words[4] = DSPP_SLEEP;

    dspp_sim *sim = dspp_sim_create(&t);
    CHECK(sim != NULL);
    if (!sim) return 1;

    dspp_sim_poke_data(sim, 0x020, 0xBEEF);
    dspp_sim_tick_n(sim, 3);

    // The direct address operand copied the source word, and the immediate
    // operand wrote its decoded 13-bit value. Repeating the cyclic program
    // must leave the same result.
    CHECK(dspp_sim_peek_data(sim, 0x010) == 0xBEEF);
    CHECK(dspp_sim_peek_data(sim, 0x011) == 0x0123);
    CHECK(dspp_sim_peek_data(sim, 0x020) == 0xBEEF);

    dspp_sim_destroy(sim);

    // Register operand: rbase[0] is address 0 at cycle start, so the MOVE
    // reads data[0] through the register form rather than a direct address.
    // Shared-space sims: each program runs its own words from its own base,
    // and both read and write the one shared data image. A jump leaves the
    // running sim's words and fetches through the space's code map.
    dspp_sim_space *space = dspp_sim_space_create();
    CHECK(space != NULL);
    if (space) {
        dspp_template first;
        memset(&first, 0, sizeof(first));
        first.code_word_count = 3;
        first.code_words[0] = MOVE_DIRECT(0x030);
        first.code_words[1] = OPERAND_IMMEDIATE(0x111);
        first.code_words[2] = DSPP_SLEEP;
        dspp_sim *a = dspp_sim_create_in_space(&first, space);
        CHECK(a != NULL);

        // Words 0..2 belong to `a`, so this sim starts at word 3. It jumps
        // into `a`, which writes 0x030 and sleeps; the MOVE after the jump
        // must not run.
        dspp_template second;
        memset(&second, 0, sizeof(second));
        second.code_word_count = 3;
        second.code_words[0] = (uint16_t)(0x8400u | 0x000u); // JUMP word 0
        second.code_words[1] = MOVE_DIRECT(0x031);
        second.code_words[2] = OPERAND_IMMEDIATE(0x222);
        dspp_sim *b = dspp_sim_create_in_space(&second, space);
        CHECK(b != NULL);
        if (a && b) {
            dspp_sim_tick(b);
            CHECK(dspp_sim_peek_data(a, 0x030) == 0x0111);
            CHECK(dspp_sim_peek_data(a, 0x031) == 0x0000);
            CHECK(dspp_sim_peek_data(b, 0x030) == 0x0111);
        }
        if (b) dspp_sim_destroy(b);
        if (a) dspp_sim_destroy(a);
        dspp_sim_space_destroy(space);
    }

    // Operand slots start empty for each instruction: an immediate parsed by
    // one MOVE must not shadow the address operand of the next.
    dspp_template slots;
    memset(&slots, 0, sizeof(slots));
    slots.code_word_count = 5;
    slots.code_words[0] = MOVE_DIRECT(0x010);
    slots.code_words[1] = OPERAND_IMMEDIATE(0x0AA);
    slots.code_words[2] = MOVE_DIRECT(0x011);
    slots.code_words[3] = OPERAND_ADDR(0x012);
    slots.code_words[4] = DSPP_SLEEP;
    sim = dspp_sim_create(&slots);
    CHECK(sim != NULL);
    if (sim) {
        dspp_sim_poke_data(sim, 0x012, 0x5555);
        dspp_sim_tick_n(sim, 2);
        CHECK(dspp_sim_peek_data(sim, 0x010) == 0x00AA);
        CHECK(dspp_sim_peek_data(sim, 0x011) == 0x5555);
        dspp_sim_destroy(sim);
    }

    dspp_template reg;
    memset(&reg, 0, sizeof(reg));
    reg.code_word_count = 3;
    reg.code_words[0] = MOVE_DIRECT(0x010);
    reg.code_words[1] = OPERAND_REG(0);
    reg.code_words[2] = DSPP_SLEEP;
    sim = dspp_sim_create(&reg);
    CHECK(sim != NULL);
    if (sim) {
        dspp_sim_poke_data(sim, 0x000, 0xBEEF);
        dspp_sim_tick(sim);
        CHECK(dspp_sim_peek_data(sim, 0x010) == 0xBEEF);
        dspp_sim_destroy(sim);
    }

    // Indirect address: data[0x20] names data[0x30].
    dspp_template indirect;
    memset(&indirect, 0, sizeof(indirect));
    indirect.code_word_count = 3;
    indirect.code_words[0] = MOVE_DIRECT(0x010);
    indirect.code_words[1] = OPERAND_ADDR_IND(0x020);
    indirect.code_words[2] = DSPP_SLEEP;
    sim = dspp_sim_create(&indirect);
    CHECK(sim != NULL);
    if (sim) {
        dspp_sim_poke_data(sim, 0x020, 0x030);
        dspp_sim_poke_data(sim, 0x030, 0xCAFE);
        dspp_sim_tick(sim);
        CHECK(dspp_sim_peek_data(sim, 0x010) == 0xCAFE);
        dspp_sim_destroy(sim);
    }

    if (failures != 0) {
        fprintf(stderr, "m2_dspp_exec_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("m2_dspp_exec_test: PASS\n");
    return 0;
}
