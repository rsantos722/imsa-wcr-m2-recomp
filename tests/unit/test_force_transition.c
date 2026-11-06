// M2_FORCE_TRANSITION: replace the random dotrans request (type 2) with a
// named or numbered effect so one screen transition can be re-tested without
// waiting for the random index. Explicit types and ordinary swaps pass
// through.
#include "ppc_ctx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The no-disc runtime omits MMIO and memory-event delivery. Defining the
// provenance state here keeps the archive's trace-stub object (which pulls
// the host audio/selector surface) out of this standalone test.
__attribute__((weak)) bool m2_provenance_active = false;
__attribute__((weak)) bool m2_provenance_reads_active = false;
__attribute__((weak)) void m2_provenance_record_read(uint32_t ea, uint32_t value,
    uint8_t width, uint32_t pc, uint32_t lr, uint32_t frame_seq) {
    (void)ea; (void)value; (void)width; (void)pc; (void)lr; (void)frame_seq;
}
__attribute__((weak)) void m2_provenance_report_oob(uint32_t ea, uint8_t width,
    uint32_t pc, uint32_t lr) {
    (void)ea; (void)width; (void)pc; (void)lr;
}
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t a) { (void)a; return 0; }
__attribute__((weak)) void m2_oob_shadow_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t a, uint32_t n, uint32_t pc) { (void)a; (void)n; (void)pc; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

int main(void) {
    CHECK(m2_parse_forced_transition("") == 0u);
    CHECK(m2_parse_forced_transition("off") == 0u);
    CHECK(m2_parse_forced_transition("rotate") == 15u);
    CHECK(m2_parse_forced_transition("rotate-reverse") == 16u);
    CHECK(m2_parse_forced_transition("scroll") == 7u);
    CHECK(m2_parse_forced_transition("wipe4") == 6u);
    CHECK(m2_parse_forced_transition("stripes") == 11u);
    CHECK(m2_parse_forced_transition("grid") == 12u);
    CHECK(m2_parse_forced_transition("square2") == 14u);
    CHECK(m2_parse_forced_transition("15") == 15u);
    CHECK(m2_parse_forced_transition("0x10") == 16u);
    CHECK(m2_parse_forced_transition("17") == 0u);
    CHECK(m2_parse_forced_transition("bogus") == 0u);

    set_env("M2_FORCE_TRANSITION", "rotate");
    PPCContext c;
    memset(&c, 0, sizeof(c));
    c.mem = (uint8_t*)calloc(1, M2_DOTRANS_TYPE_EA + 4u);
    if (!c.mem) {
        fprintf(stderr, "FAIL: allocation\n");
        return 1;
    }
    c.mem[M2_DOTRANS_TYPE_EA + 3] = (uint8_t)M2_DOTRANS_RANDOM;
    CHECK(rd32(&c, M2_DOTRANS_TYPE_EA) == 15u);
    c.mem[M2_DOTRANS_TYPE_EA + 3] = 15u;
    CHECK(rd32(&c, M2_DOTRANS_TYPE_EA) == 15u);
    c.mem[M2_DOTRANS_TYPE_EA + 3] = 0u;
    CHECK(rd32(&c, M2_DOTRANS_TYPE_EA) == 0u);
    free(c.mem);
    return failures == 0 ? 0 : 1;
}
