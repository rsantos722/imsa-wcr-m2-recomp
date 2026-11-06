#include "vbl_pacer.h"

#include <cstdint>
#include <cstdio>

static int fails;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++fails; \
    } \
} while (0)

int main() {
    using m2host::timing::VblPacer;

    VblPacer pacer;
    constexpr uint64_t start_ns = 100000000000ull;
    CHECK(pacer.due(start_ns));
    CHECK(!pacer.due(start_ns));

    uint64_t previous = start_ns;
    uint64_t total_ns = 0;
    for (uint32_t i = 0; i < 60000u; ++i) {
        const uint64_t deadline = pacer.next_deadline_ns();
        const uint64_t interval = deadline - previous;
        CHECK(interval == 16683333ull || interval == 16683334ull);
        total_ns += interval;
        CHECK(pacer.due(deadline));
        previous = deadline;
    }
    // 60000 NTSC fields occupy exactly 1001 seconds. This catches the
    // one-second drift caused by rounding every field to 1/60 second.
    CHECK(total_ns == 1001000000000ull);

    VblPacer stalled;
    CHECK(stalled.due(7u));
    const uint64_t first_deadline = stalled.next_deadline_ns();
    CHECK(stalled.due(first_deadline + 3u * 16683334ull));
    // The late call grants one permit and leaves only one future deadline;
    // missed fields cannot be emitted as a burst.
    CHECK(stalled.next_deadline_ns() > first_deadline + 3u * 16683334ull);
    CHECK(!stalled.due(stalled.next_deadline_ns() - 1u));

    std::printf("test_vbl_pacer: %s\n", fails ? "FAIL" : "OK");
    return fails != 0;
}
