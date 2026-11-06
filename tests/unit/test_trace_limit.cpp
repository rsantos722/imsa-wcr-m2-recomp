#include "trace.h"
#include <cstdio>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main() {
    using namespace m2host::trace;
    CHECK(!trace_kind_enabled(EventKind::Input));
    TraceConfig cfg;
    cfg.build_id = "contract-test";
    cfg.run_id = "contract-test";
    cfg.max_bytes = 1;
    CHECK(trace_init(cfg));
    CHECK(trace_kind_enabled(EventKind::Input));
    CHECK(trace_kind_enabled(EventKind::SelectorCall));
    CHECK(!trace_byte_limit_reached());
    trace_input(1, 1, 0, 128, 128, 0, 0);
    CHECK(trace_byte_limit_reached());
    const auto sequence = trace_sequence();
    trace_input(0, 0, 1, 128, 128, 0, 0);
    CHECK(trace_sequence() == sequence); // collector stops after explicit cap
    trace_shutdown();
    CHECK(!trace_kind_enabled(EventKind::Input));
    cfg.max_bytes = 4096;
    CHECK(trace_init(cfg));
    CHECK(!trace_byte_limit_reached());
    trace_input(1, 1, 0, 128, 128, 0, 0);
    CHECK(!trace_byte_limit_reached());
    trace_emit(EventKind::Input, 0, 0, 0, std::string(4096, ' '));
    CHECK(trace_byte_limit_reached());
    trace_shutdown();
    cfg.max_bytes = 0; // ordinary non-diagnostic traces retain their behavior
    CHECK(trace_init(cfg));
    trace_emit(EventKind::Input, 0, 0, 0, std::string(8192, ' '));
    CHECK(!trace_byte_limit_reached());
    trace_shutdown();

    // The lock-free kind guard follows the configured filter, so helpers can
    // return before formatting when their kind is not selected.
    TraceConfig filtered;
    filtered.build_id = "contract-test";
    filtered.run_id = "contract-test";
    filtered.filter = {EventKind::Input};
    CHECK(trace_init(filtered));
    CHECK(trace_kind_enabled(EventKind::Input));
    CHECK(!trace_kind_enabled(EventKind::SelectorCall));
    CHECK(trace_sequence() == 0);
    trace_selector_call(1, 2, 3, 4, 5, 6, 7);
    CHECK(trace_sequence() == 0);
    trace_input(1, 1, 0, 128, 128, 0, 0);
    CHECK(trace_sequence() == 1);
    trace_shutdown();
    CHECK(!trace_kind_enabled(EventKind::Input));

    std::puts("OK: trace byte cap, explicit incomplete status, reset, disabled cap, kind guard");
    return 0;
}
