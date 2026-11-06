// Semantic test for the source image's modff/frexpf/ldexpf instruction spans.
//
// The Diab ELF marks these symbols st_size=0, but the original .text bytes are
// real leaf functions: modff@0x726e0..0x72734, frexpf@0x72c24..0x72c54, and
// ldexpf@0x72c54..0x72c7c. powf@0x72804 calls all three directly. This test
// calls the generated entries through the production dispatch table, so an
// empty zero-size body cannot satisfy it. __start@0x74134 is also a real
// one-instruction entry thunk; its dispatch presence is checked without
// launching the full game.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc_ctx.h"

typedef void (*RecompFn)(PPCContext *);
RecompFn recomp_lookup(unsigned guest);

static int fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++fails;                                                           \
        }                                                                      \
    } while (0)

static void reset(PPCContext *c, uint8_t *mem) {
    memset(c, 0, sizeof(*c));
    memset(mem, 0, 0x1000u);
    c->mem = mem;
    c->gpr[1] = 0x200u;
}

int main(void) {
    uint8_t *mem = (uint8_t *)calloc(1, 0x1000u);
    CHECK(mem != NULL);
    if (!mem) return 1;
    PPCContext c;

    RecompFn modff_guest = recomp_lookup(0x1726e0u);
    RecompFn frexpf_guest = recomp_lookup(0x172c24u);
    RecompFn ldexpf_guest = recomp_lookup(0x172c54u);
    RecompFn start_guest = recomp_lookup(0x174134u);
    CHECK(modff_guest != NULL);
    CHECK(frexpf_guest != NULL);
    CHECK(ldexpf_guest != NULL);
    CHECK(start_guest != NULL);

    if (modff_guest) {
        reset(&c, mem);
        c.fpr[1] = 3.25f;
        c.gpr[3] = 0x300u;
        modff_guest(&c);
        CHECK(c.fpr[1] == 0.25f);
        CHECK(rdf32(&c, 0x300u) == 3.0f);

        reset(&c, mem);
        c.fpr[1] = -3.25f;
        c.gpr[3] = 0x300u;
        modff_guest(&c);
        CHECK(c.fpr[1] == -0.25f);
        CHECK(rdf32(&c, 0x300u) == -3.0f);
    }

    if (frexpf_guest) {
        reset(&c, mem);
        c.fpr[1] = 8.0f;
        c.gpr[3] = 0x300u;
        frexpf_guest(&c);
        CHECK(c.fpr[1] == 0.5f);
        CHECK((int32_t)rd32(&c, 0x300u) == 4);

        reset(&c, mem);
        c.fpr[1] = -8.0f;
        c.gpr[3] = 0x300u;
        frexpf_guest(&c);
        CHECK(c.fpr[1] == -0.5f);
        CHECK((int32_t)rd32(&c, 0x300u) == 4);
    }

    if (ldexpf_guest) {
        reset(&c, mem);
        c.fpr[1] = 0.5f;
        c.gpr[3] = 4u;
        ldexpf_guest(&c);
        CHECK(c.fpr[1] == 8.0f);

        reset(&c, mem);
        c.fpr[1] = -0.75f;
        c.gpr[3] = (uint32_t)-2;
        ldexpf_guest(&c);
        CHECK(c.fpr[1] == -0.1875f);
    }

    free(mem);
    printf("%s: %s (%d failures)\n", __FILE__, fails ? "FAIL" : "OK",
           fails);
    return fails ? 1 : 0;
}
