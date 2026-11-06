// Guest input frame: determinism, guest-memory round-trip, error paths, and
// big-endian layout.
#include <stdio.h>
#include <string.h>
#include "guest_input_frame.h"

#include "guest_events.h"

// ppc_ctx.h's accessors reach the MMIO shadow in recomp_runtime.c and the trace
// hooks in trace_stubs.c. Builds without game data have no recomp_runtime.c,
// and trace_stubs.c needs host audio, so this test (which stays inside guest
// memory) supplies weak inert definitions instead.
__attribute__((weak)) uint32_t m2_oob_shadow_read(uint32_t a) { (void)a; return 0; }
__attribute__((weak)) void m2_oob_shadow_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
__attribute__((weak)) void m2_trace_memory_fault_c(uint32_t ea, uint32_t size, uint32_t pc) {
    (void)ea; (void)size; (void)pc;
}
__attribute__((weak)) void m2_guest_event_c(const M2GuestEvent* event) { (void)event; }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)

static int test_determinism(void) {
    m2_input_frame_ctx a, b;
    m2_input_frame_init(&a);
    m2_input_frame_init(&b);
    m2_input_snapshot s = {0};
    s.buttons = 0x5u; // A+C per CportBits
    s.pressed = 0x5u;
    s.released = 0u;
    s.stick_x = 200;
    s.stick_y = 30;
    s.device = 0;
    CHECK(m2_input_frame_set_snapshot(&a, &s) == M2_IF_OK);
    CHECK(m2_input_frame_set_snapshot(&b, &s) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&a, 16) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&b, 16) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&a, 16) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&b, 16) == M2_IF_OK);
    // contexts must be byte-identical
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    m2_frame_tick ta, tb;
    CHECK(m2_input_frame_tick_get(&a, &ta) == M2_IF_OK);
    CHECK(m2_input_frame_tick_get(&b, &tb) == M2_IF_OK);
    CHECK(ta.frame_index == 2 && ta.seq == 2 && ta.dt_ms == 16);
    CHECK(tb.frame_index == 2);
    // snapshot round-trip
    m2_input_snapshot out;
    CHECK(m2_input_frame_get_snapshot(&a, &out) == M2_IF_OK);
    CHECK(out.buttons == 0x5u && out.stick_x == 200 && out.stick_y == 30);
    printf("determinism ok frame=%llu seq=%u\n", (unsigned long long)ta.frame_index, ta.seq);
    return 0;
}

static int test_guest_mem_roundtrip(void) {
    // 64 MiB guest image like ppc_ctx.h RECOMP_GUEST_MEM
    static unsigned char mem[RECOMP_GUEST_MEM];
    memset(mem, 0xAA, sizeof(mem));
    PPCContext c = {0};
    c.mem = mem;
    m2_input_frame_ctx ctx;
    m2_input_frame_init(&ctx);
    m2_input_snapshot s = {0};
    s.buttons = 0x00000003u; // A+B
    s.pressed = 0x1u;
    s.released = 0x2u;
    s.stick_x = 128;
    s.stick_y = 255;
    s.device = 2;
    CHECK(m2_input_frame_set_snapshot(&ctx, &s) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&ctx, 16) == M2_IF_OK);
    CHECK(m2_input_frame_advance(&ctx, 17) == M2_IF_OK);
    uint32_t base = 0x00310000u; // caller-chosen window, clear of broker carve 0x00300000 slice
    CHECK(m2_input_frame_write_combined_ctx(&ctx, &c, base) == M2_IF_OK);
    // validate BE bytes directly
    CHECK(rd32(&c, base + 0) == 0x00000003u);
    CHECK(rd32(&c, base + 4) == 0x00000001u);
    CHECK(rd32(&c, base + 8) == 0x00000002u);
    CHECK(rd8(&c, base + 12) == 128);
    CHECK(rd8(&c, base + 13) == 255);
    CHECK(rd8(&c, base + 14) == 2);
    CHECK(rd8(&c, base + 15) == 0);
    // frame at base+16
    m2_frame_tick tick;
    CHECK(m2_input_frame_read_tick_ctx(&c, base + 16, &tick) == M2_IF_OK);
    CHECK(tick.frame_index == 2 && tick.seq == 2 && tick.dt_ms == 17);
    // snapshot reader
    m2_input_snapshot rs;
    CHECK(m2_input_frame_read_snapshot_ctx(&c, base, &rs) == M2_IF_OK);
    CHECK(rs.buttons == s.buttons && rs.pressed == s.pressed && rs.released == s.released);
    CHECK(rs.stick_x == 128 && rs.stick_y == 255 && rs.device == 2);
    // separate tick writer
    uint32_t tbase = 0x00310100u;
    CHECK(m2_input_frame_write_tick_ctx(&ctx, &c, tbase) == M2_IF_OK);
    m2_frame_tick t2;
    CHECK(m2_input_frame_read_tick_ctx(&c, tbase, &t2) == M2_IF_OK);
    CHECK(t2.frame_index == tick.frame_index);
    printf("guest-mem roundtrip ok base=%#x frame=%llu\n", base, (unsigned long long)tick.frame_index);
    return 0;
}

static int32_t fail_wr32_cb(void* ud, uint32_t ea, uint32_t v){ (void)ud;(void)ea;(void)v; return -1; }
static int32_t ok_wr8_cb(void* ud, uint32_t ea, uint8_t v){ (void)ud;(void)ea;(void)v; return 0; }

static int test_error_paths(void) {
    m2_input_frame_ctx ctx;
    m2_input_frame_init(&ctx);
    // bad args
    CHECK(m2_input_frame_set_snapshot(NULL, NULL) == M2_IF_ERR_BAD_ARG);
    CHECK(m2_input_frame_set_snapshot(&ctx, NULL) == M2_IF_ERR_BAD_ARG);
    m2_input_snapshot bad = {0};
    bad.device = 4; // out of range
    CHECK(m2_input_frame_set_snapshot(&ctx, &bad) == M2_IF_ERR_RANGE);
    // uninitialized ctx rejected
    m2_input_frame_ctx u;
    memset(&u, 0, sizeof(u));
    CHECK(m2_input_frame_set_snapshot(&u, &bad) == M2_IF_ERR_STATE);
    CHECK(m2_input_frame_advance(&u, 16) == M2_IF_ERR_STATE);
    // dt range
    CHECK(m2_input_frame_advance(&ctx, 0) == M2_IF_ERR_RANGE);
    CHECK(m2_input_frame_advance(&ctx, 1001) == M2_IF_ERR_RANGE);
    CHECK(m2_input_frame_advance(&ctx, 16) == M2_IF_OK);
    // write bounds: null mem or out-of-range EA
    PPCContext c = {0};
    c.mem = NULL;
    CHECK(m2_input_frame_write_combined_ctx(&ctx, &c, 0x1000) == M2_IF_ERR_BAD_ARG);
    static unsigned char small[64];
    PPCContext c2 = {0};
    c2.mem = small; // but RECOMP_GUEST_MEM is 64MiB, so EA beyond mem size still fails via EA+size check
    // Instead use real sized mem but EA near top
    static unsigned char mem[RECOMP_GUEST_MEM];
    PPCContext c3 = {0};
    c3.mem = mem;
    CHECK(m2_input_frame_write_combined_ctx(&ctx, &c3, RECOMP_GUEST_MEM - 31) == M2_IF_ERR_BAD_ARG);
    // null wr callbacks
    CHECK(m2_input_frame_write_snapshot(&ctx, NULL, NULL, NULL, 0x1000) == M2_IF_ERR_BAD_ARG);
    // generic callback fault injection: wr32 returns error
    CHECK(m2_input_frame_write_snapshot(&ctx, NULL, fail_wr32_cb, ok_wr8_cb, 0x1000) == M2_IF_ERR_BAD_ARG);
    printf("error paths ok\n");
    return 0;
}

static int test_edges_and_reinit(void) {
    m2_input_frame_ctx ctx;
    m2_input_frame_init(&ctx);
    m2_input_snapshot s = {0};
    s.buttons = 0xFFFFFFFFu;
    s.pressed = 0xFFFFFFFFu;
    s.released = 0;
    s.stick_x = 1; // extreme
    s.stick_y = 255;
    s.device = 3;
    CHECK(m2_input_frame_set_snapshot(&ctx, &s) == M2_IF_OK);
    // pressed/released preserved exactly (edge masks are guest-visible)
    m2_input_snapshot out;
    CHECK(m2_input_frame_get_snapshot(&ctx, &out) == M2_IF_OK);
    CHECK(out.pressed == 0xFFFFFFFFu && out.released == 0);
    // re-init resets to pristine (frame 0, seq 0, no snapshot)
    m2_input_frame_init(&ctx);
    m2_frame_tick t;
    CHECK(m2_input_frame_tick_get(&ctx, &t) == M2_IF_OK);
    CHECK(t.frame_index == 0 && t.seq == 0 && t.dt_ms == 0);
    CHECK(m2_input_frame_get_snapshot(&ctx, &out) == M2_IF_OK);
    CHECK(out.buttons == 0 && out.stick_x == 0);
    printf("edges+reinit ok\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_determinism();
    rc |= test_guest_mem_roundtrip();
    rc |= test_error_paths();
    rc |= test_edges_and_reinit();
    if (rc == 0) printf("ALL PASS guest_input_frame\n");
    else printf("FAIL guest_input_frame rc=%d\n", rc);
    return rc;
}
