// Arguments handed to the driving game's main(). The game is started with an
// empty argv unless M2_GUEST_ARGS says otherwise; the host's own command line
// is never forwarded (m2_game_entry takes no arguments).
#include <stdio.h>
#include <string.h>

#include "guest_boot_args.h"

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                    __LINE__, #cond);                                   \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static int parse(const char* text, char* buf, size_t buf_size, char** vec, int cap) {
    for (int i = 0; i < cap; ++i) vec[i] = NULL;
    return m2_guest_boot_args(text, buf, buf_size, vec, cap);
}

int main(void) {
    char buf[64];
    char* vec[8];

    // No M2_GUEST_ARGS: empty argv, as LaunchMe.m2 starts the game.
    CHECK(parse(NULL, buf, sizeof(buf), vec, 8) == 0);
    CHECK(parse("", buf, sizeof(buf), vec, 8) == 0);
    CHECK(parse(" \t ", buf, sizeof(buf), vec, 8) == 0);

    // One developer flag.
    CHECK(parse("-V", buf, sizeof(buf), vec, 8) == 2);
    CHECK(strcmp(vec[0], "imsa") == 0);
    CHECK(strcmp(vec[1], "-V") == 0);

    // Several flags with mixed separators.
    CHECK(parse("  -Vg\t-Tdata/tracks/x  -2 ", buf, sizeof(buf), vec, 8) == 4);
    CHECK(strcmp(vec[1], "-Vg") == 0);
    CHECK(strcmp(vec[2], "-Tdata/tracks/x") == 0);
    CHECK(strcmp(vec[3], "-2") == 0);

    // The vector never exceeds cap.
    CHECK(parse("-a -b -c -d -e", buf, sizeof(buf), vec, 3) == 3);
    CHECK(strcmp(vec[1], "-a") == 0);
    CHECK(strcmp(vec[2], "-b") == 0);

    // Input longer than the buffer is truncated, not overrun.
    char small[4];
    CHECK(parse("-Vgxyz", small, sizeof(small), vec, 8) == 2);
    CHECK(strcmp(vec[1], "-Vg") == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("guest boot args: all checks passed\n");
    return 0;
}
