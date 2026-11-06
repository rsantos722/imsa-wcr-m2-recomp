// recomp_smoke_main.c — minimal smoke that proves the recomp library links
// and the guest memory + dispatch table are sane, without needing SDL/host.

#include "ppc_ctx.h"
#include "recomp_helpers.h"
#include "guest_kernel.h"
#include "guest_module.h"
#include <sched.h>
#include <stdio.h>
#include <string.h>

extern unsigned g_recomp_text_size;
extern unsigned g_recomp_data_size;
extern unsigned g_recomp_bss_size;
extern unsigned g_recomp_text_base;
extern unsigned g_recomp_data_base;
extern unsigned g_recomp_bss_base;

// From generated dispatch
typedef void (*RecompFn)(PPCContext*);
RecompFn recomp_lookup(unsigned guest);

int m2_game_entry(int argc, char** argv);
// ---- typed kernel selector dispatch registry test ------------------------
// Handlers hand out / accept 32-bit-clean guest addresses only (see
// recomp_call_kernel_dispatch note); the sentinel lives inside guest RAM.
#define T_ALLOC_SENTINEL 0x00200000u
static int g_t_printf_calls;
static int g_t_freed_ok;

static void* t_alloc(int32_t mem_type, uint32_t size) {
    (void)mem_type;
    return size == 128 ? (void*)(uintptr_t)T_ALLOC_SENTINEL : NULL;
}
static void t_free(void* p) {
    g_t_freed_ok = ((uint32_t)(uintptr_t)p == T_ALLOC_SENTINEL);
}
static int32_t t_printf(uint32_t fmt_ea, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    (void)fmt_ea; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    ++g_t_printf_calls;
    return 1;
}
static float t_itof(int32_t v) { return (float)v * 2.0f; }   // distinct from default

static int test_selector_registry(void) {
    int fails = 0;
    PPCContext rc = {0};
    static unsigned char rmem[64 << 20];
    rc.mem = rmem;

    m2_kernel_dispatch reg;
    m2_kernel_dispatch_init(&reg);
    if (reg.AllocMem || reg.FreeMem || reg.printf || reg.itof) { printf("[smoke] FAIL init not empty\n"); ++fails; }
    if (!m2_kernel_selector_known(M2_FOLIO_KERNEL, M2_KSEL_PRINTF) ||
        m2_kernel_selector_known(M2_FOLIO_AUDIO, 8) ||
        m2_kernel_selector_known(M2_FOLIO_KERNEL, 999)) { printf("[smoke] FAIL selector_known\n"); ++fails; }
    if (m2_kernel_dispatch_register(&reg, 999, (void*)t_alloc) != -1 ||
        m2_kernel_dispatch_register(&reg, M2_KSEL_ITOF, NULL) != -1) { printf("[smoke] FAIL register reject\n"); ++fails; }
    if (!m2_is_audio_status_selector(57) || !m2_is_audio_status_selector(69) ||
        !m2_is_audio_status_selector(70) || m2_is_audio_status_selector(27) ||
        !m2_is_audio_item_selector(8) || !m2_is_audio_item_selector(17) ||
        !m2_is_audio_item_selector(27) ||
        m2_is_audio_item_selector(70) || m2_is_generic_audio_stub(17) ||
        m2_is_generic_audio_stub(8) ||
        m2_is_generic_audio_stub(27) || m2_is_generic_audio_stub(69) ||
        m2_is_generic_audio_stub(70)) {
        printf("[smoke] FAIL audio boundary classification\n"); ++fails;
    }

    // Empty slots and unknown folios fall through to the untyped path.
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_ALLOC_MEM, 0xdead, &reg) != 0 ||
        recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_ITOF, 0xdead, NULL) != 0 ||
        recomp_call_kernel_dispatch(&rc, M2_FOLIO_AUDIO, 8, 0xdead, &reg) != 0) { printf("[smoke] FAIL fallback\n"); ++fails; }

    if (m2_kernel_dispatch_register(&reg, M2_KSEL_ALLOC_MEM, (void*)t_alloc) ||
        m2_kernel_dispatch_register(&reg, M2_KSEL_FREE_MEM, (void*)t_free) ||
        m2_kernel_dispatch_register(&reg, M2_KSEL_PRINTF, (void*)t_printf) ||
        m2_kernel_dispatch_register(&reg, M2_KSEL_ITOF, (void*)t_itof)) { printf("[smoke] FAIL register\n"); ++fails; }

    rc.gpr[3] = 42; rc.gpr[4] = 128;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_ALLOC_MEM, 0xdead, &reg) != 1 ||
        rc.gpr[3] != T_ALLOC_SENTINEL) { printf("[smoke] FAIL AllocMem ret=%#x\n", rc.gpr[3]); ++fails; }

    rc.gpr[3] = T_ALLOC_SENTINEL; g_t_freed_ok = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_FREE_MEM, 0xdead, &reg) != 1 ||
        !g_t_freed_ok) { printf("[smoke] FAIL FreeMem\n"); ++fails; }

    rc.gpr[3] = (uint32_t)-7; rc.fpr[1] = 0.0f;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_ITOF, 0xdead, &reg) != 1 ||
        rc.fpr[1] != -14.0f) { printf("[smoke] FAIL __itof f1=%g\n", (double)rc.fpr[1]); ++fails; }

    // printf: now typed via guest EA dispatch (was deferred). Direct slot call uses guest EA.
    // Setup a dummy guest fmt string for direct call check.
    {
        uint32_t fmt_ea = 0x1000;
        memcpy(rmem + fmt_ea, "test %d", 8);
        if (reg.printf(fmt_ea, 7, 0,0,0,0,0,0) != 1 || g_t_printf_calls != 1) { printf("[smoke] FAIL printf slot\n"); ++fails; }
    }
    {
        uint32_t fmt_ea = 0x1000;
        memcpy(rmem + fmt_ea, "test", 5);
        rc.gpr[3]=fmt_ea; rc.gpr[4]=0;
        if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_PRINTF, 0xdead, &reg) != 1) { printf("[smoke] FAIL printf dispatch\n"); ++fails; }
    }

    // Global wiring through recomp_call_selector, then clear it again.
    m2_kernel_dispatch_set_global(&reg);
    rc.gpr[3] = 21; rc.fpr[1] = 0.0f;
    recomp_call_selector(&rc, M2_FOLIO_KERNEL, M2_KSEL_ITOF, 0xbeef);
    if (rc.fpr[1] != 42.0f) { printf("[smoke] FAIL global dispatch f1=%g\n", (double)rc.fpr[1]); ++fails; }
    m2_kernel_dispatch_set_global(NULL);
    if (m2_kernel_dispatch_global()->itof) { printf("[smoke] FAIL global reset\n"); ++fails; }
    recomp_call_selector(&rc, M2_FOLIO_AUDIO, 8, 0xbeef);   // unknown -> untyped path

    printf(fails ? "[smoke] selector-registry FAIL (%d)\n" : "[smoke] selector-registry ok\n", fails);
    return fails;
}

// ---- typed u32 selector helper test (kernel:172 CreateMsgPort) -----------
// Fresh context bound through the typed registry slot; the guest-visible
// result must be a MSGPORT-typed handle, then it is deleted.
static m2k_context g_mp_sel_ctx;
static m2k_context* g_mp_sel_binding;

static uint32_t t_sel_create_msgport(void) {
    return m2k_create_msgport(g_mp_sel_binding);
}

static int test_msgport_selector(void) {
    int fails = 0;
    m2k_init(&g_mp_sel_ctx);
    g_mp_sel_binding = &g_mp_sel_ctx;

    m2_kernel_dispatch reg;
    m2_kernel_dispatch_init(&reg);
    if (m2_kernel_dispatch_register(&reg, M2_KSEL_CREATE_MSGPORT,
                                    (void*)t_sel_create_msgport)) {
        printf("[smoke] FAIL msgport register\n"); ++fails;
    }
    m2_kernel_dispatch_set_global(&reg);

    PPCContext rc = {0};
    uint32_t before = rc.gpr[3];
    // One assertion: helper fires and returns a live M2K_TYPE_MSGPORT handle.
    if (recomp_call_selector_u32(&rc, M2_FOLIO_KERNEL, M2_KSEL_CREATE_MSGPORT,
                                 0xcafe) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_MSGPORT) {
        printf("[smoke] FAIL msgport selector ret=%d h=%#x\n",
               recomp_call_selector_u32(&rc, M2_FOLIO_KERNEL,
                                        M2_KSEL_CREATE_MSGPORT, 0xcafe),
               rc.gpr[3]);
        ++fails;
    }
    m2k_delete_msgport(&g_mp_sel_ctx, rc.gpr[3]);
    (void)before;

    m2_kernel_dispatch_set_global(NULL);
    m2k_destroy(&g_mp_sel_ctx);

    printf(fails ? "[smoke] msgport-selector FAIL (%d)\n"
                 : "[smoke] msgport-selector ok\n", fails);
    return fails;
}

// ---- guest TagArg pair helpers test --------------------------------------
static int test_tag_helpers(void) {
    int fails = 0;

    // Found tag: exact index + value round-trip through the iterator.
    static const m2_tag_arg t_found[] = {{1, 10}, {2, 20}, {M2_TAG_END, 0}};
    if (m2_tag_find(t_found, 8, 1) != 0 ||
        m2_tag_find(t_found, 8, 2) != 1) { printf("[smoke] FAIL tag found\n"); ++fails; }
    int32_t i = m2_tag_iter_next(t_found, 8, -1);
    if (i != 0 || t_found[i].value != 10) { printf("[smoke] FAIL tag iter first\n"); ++fails; }
    i = m2_tag_iter_next(t_found, 8, i);
    if (i != 1 || t_found[i].value != 20) { printf("[smoke] FAIL tag iter second\n"); ++fails; }

    // Terminator: iteration stops at TAG_END even with a huge bound.
    i = m2_tag_iter_next(t_found, 8, i);
    if (i != -1) { printf("[smoke] FAIL tag iter terminator\n"); ++fails; }

    // Missing tag.
    if (m2_tag_find(t_found, 8, 99) != -1) { printf("[smoke] FAIL tag missing\n"); ++fails; }

    // Entries after TAG_END are invisible regardless of max_count.
    static const m2_tag_arg t_term[] = {{7, 70}, {M2_TAG_END, 0}, {8, 80}};
    if (m2_tag_find(t_term, 16, 8) != -1 ||
        m2_tag_iter_next(t_term, 16, 0) != -1) { printf("[smoke] FAIL tag post-END leak\n"); ++fails; }

    // Unterminated bounded input: no END present; bound of 2 must hide the
    // trailing {13,...} entry (a buggy scan past the bound would find it).
    static const m2_tag_arg t_unterm[] = {{11, 110}, {12, 120}, {13, 130}};
    if (m2_tag_find(t_unterm, 2, 13) != -1 ||
        m2_tag_find(t_unterm, 2, M2_TAG_END) != -1 ||
        m2_tag_iter_next(t_unterm, 3, 2) != -1) { printf("[smoke] FAIL tag bound overrun\n"); ++fails; }
    // In-bounds entries still resolve on the unterminated list.
    if (m2_tag_find(t_unterm, 2, 12) != 1 ||
        m2_tag_find(t_unterm, 3, 13) != 2) { printf("[smoke] FAIL tag bounded hit\n"); ++fails; }

    // Rejections: NULL list and zero bound never dereference.
    if (m2_tag_find(NULL, 8, 1) != -1 ||
        m2_tag_iter_next(NULL, 8, -1) != -1 ||
        m2_tag_find(t_found, 0, 1) != -1) { printf("[smoke] FAIL tag reject\n"); ++fails; }

    printf(fails ? "[smoke] tag-helpers FAIL (%d)\n" : "[smoke] tag-helpers ok\n", fails);
    return fails;
}

// ---- guest kernel runtime slice test -------------------------------------
static m2k_context g_kctx_a;
static m2k_context g_kctx_b;

#define TK_CHECK(cond, name) \
    do { if (!(cond)) { printf("[smoke] FAIL kernel %s\n", name); ++fails; } } while (0)

static int test_guest_kernel(void) {
    int fails = 0;
    m2k_init(&g_kctx_a);
    m2k_init(&g_kctx_b);

    // Deterministic handles: identical op streams on two fresh contexts.
    {
        uint32_t ha[4], hb[4];
        ha[0] = m2k_create_msgport(&g_kctx_a);
        ha[1] = m2k_create_ioreq(&g_kctx_a);
        ha[2] = m2k_create_msgport(&g_kctx_a);
        ha[3] = m2k_create_ioreq_va(&g_kctx_a,
                 (const m2_tag_arg[]){{M2K_TAG_IO_DEVICE, 7}, {M2_TAG_END, 0}}, 8);
        hb[0] = m2k_create_msgport(&g_kctx_b);
        hb[1] = m2k_create_ioreq(&g_kctx_b);
        hb[2] = m2k_create_msgport(&g_kctx_b);
        hb[3] = m2k_create_ioreq_va(&g_kctx_b,
                 (const m2_tag_arg[]){{M2K_TAG_IO_DEVICE, 7}, {M2_TAG_END, 0}}, 8);
        for (int i = 0; i < 4; ++i) {
            if (ha[i] == M2K_ITEM_NONE || ha[i] != hb[i]) {
                printf("[smoke] FAIL kernel determinism h%d a=%#x b=%#x\n",
                       i, ha[i], hb[i]);
                ++fails;
            }
        }
        TK_CHECK(m2k_handle_type(ha[0]) == M2K_TYPE_MSGPORT &&
                 m2k_handle_type(ha[1]) == M2K_TYPE_IOREQ &&
                 m2k_handle_slot(ha[0]) == 0 && m2k_handle_gen(ha[0]) >= 1,
                 "handle layout");

        // Delete + recreate reuses the same slot (lowest-free-first) with a
        // strictly newer generation, identically on both contexts.
        uint32_t ra = m2k_create_msgport(&g_kctx_a), rb = m2k_create_msgport(&g_kctx_b);
        for (int i = 0; i < 4; ++i) {
            if (i < 3 && (ra == ha[i] || rb == hb[i])) {
                printf("[smoke] FAIL kernel dup handle\n"); ++fails;
            }
        }
        if (m2k_delete_msgport(&g_kctx_a, ha[0]) != M2K_OK ||
            m2k_delete_msgport(&g_kctx_b, hb[0]) != M2K_OK) {
            printf("[smoke] FAIL kernel port delete\n"); ++fails;
        }
        uint32_t na = m2k_create_msgport(&g_kctx_a), nb = m2k_create_msgport(&g_kctx_b);
        if (na != nb || na == M2K_ITEM_NONE ||
            m2k_handle_slot(na) != m2k_handle_slot(ha[0]) ||
            m2k_handle_gen(na) <= m2k_handle_gen(ha[0])) {
            printf("[smoke] FAIL kernel gen bump old=%#x new=%#x\n",
                   ha[0], na);
            ++fails;
        }
        // Stale handle must now be rejected everywhere it can reach.
        if (m2k_send_msg(&g_kctx_a, ha[0], 1, 1) != M2K_ERR_BAD_ITEM ||
            m2k_delete_msgport(&g_kctx_a, ha[0]) != M2K_ERR_BAD_ITEM) {
            printf("[smoke] FAIL kernel stale handle\n"); ++fails;
        }
    }

    // FIFO order + reply semantics.
    {
        m2k_context* k = &g_kctx_a;
        uint32_t port = m2k_create_msgport(k);
        TK_CHECK(port != M2K_ITEM_NONE, "fifo port");
        TK_CHECK(m2k_send_msg(k, port, 10, 100) == M2K_OK, "send A");
        TK_CHECK(m2k_send_msg(k, port, 20, 200) == M2K_OK, "send B");
        TK_CHECK(m2k_send_msg(k, port, 30, 300) == M2K_OK, "send C");
        TK_CHECK(m2k_wait_port(k, port) == M2K_OK, "wait port nonempty");

        static const uint32_t ewhat[3] = {10, 20, 30};
        static const uint32_t eval_[3] = {100, 200, 300};
        for (int i = 0; i < 3; ++i) {
            uint32_t what = 0, val = 0; m2k_item_t msg = M2K_ITEM_NONE;
            int rc = m2k_get_msg(k, port, &msg, &what, &val);
            if (rc != M2K_OK || what != ewhat[i] || val != eval_[i] ||
                m2k_handle_type(msg) != M2K_TYPE_MESSAGE) {
                printf("[smoke] FAIL fifo order i=%d rc=%d what=%u val=%u\n",
                       i, rc, what, val);
                ++fails;
            }
            if (m2k_reply_msg(k, msg, -(i + 1)) != M2K_OK) {
                printf("[smoke] FAIL reply i=%d\n", i); ++fails;
            }
            // Double reply is state-rejected; the freed slot's stale handle
            // does not resolve at all.
            if (m2k_reply_msg(k, msg, -99) != M2K_ERR_BAD_ITEM) {
                printf("[smoke] FAIL double reply i=%d\n", i); ++fails;
            }
        }
        TK_CHECK(m2k_get_msg(k, port, NULL, NULL, NULL) == M2K_ERR_EMPTY,
                 "fifo drained");
        // Replies landed back on the sender's port in reply order.
        if (g_kctx_a.ports[m2k_handle_slot(port)].last_reply_code != -3) {
            printf("[smoke] FAIL reply code delivery r=%d\n",
                   g_kctx_a.ports[m2k_handle_slot(port)].last_reply_code);
            ++fails;
        }
        // Deleting a non-empty port is refused.
        TK_CHECK(m2k_send_msg(k, port, 40, 400) == M2K_OK, "refill");
        TK_CHECK(m2k_delete_msgport(k, port) == M2K_ERR_STATE, "delete nonempty");
        TK_CHECK(m2k_get_msg(k, port, NULL, NULL, NULL) == M2K_OK, "drain refill");
        // Type confusion: message handle where a port is expected.
        m2k_item_t probe = M2K_ITEM_NONE;
        TK_CHECK(m2k_get_msg(k, port, &probe, NULL, NULL) == M2K_ERR_EMPTY,
                 "drained again");
    }

    // Signal masks: allocation ownership, exact-mask consumption.
    {
        m2k_context* k = &g_kctx_b;
        int s0 = m2k_alloc_signal(k, -1);
        int s1 = m2k_alloc_signal(k, -1);
        int s5 = m2k_alloc_signal(k, 5);
        if (s0 != 0 || s1 != 1 || s5 != 5) {
            printf("[smoke] FAIL sig alloc s=%d,%d,%d\n", s0, s1, s5); ++fails;
        }
        if (m2k_alloc_signal(k, 5) != M2K_ERR_STATE ||   // duplicate bit
            m2k_alloc_signal(k, -1) != 2 ||              // next lowest free
            m2k_alloc_signal(k, 32) != M2K_ERR_RANGE ||  // out of range
            m2k_alloc_signal(k, -2) != M2K_ERR_RANGE) {
            printf("[smoke] FAIL sig alloc reject\n"); ++fails;
        }

        TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, s1) == M2K_OK, "sig send 1");
        // Exact-mask poll: only bit 1 comes out even though asked for more,
        // and unmasked bits stay pending.
        int got = m2k_poll_signal(k, (1u << s1) | (1u << s5));
        if (got != (1 << s1)) { printf("[smoke] FAIL sig poll got=%d\n", got); ++fails; }
        TK_CHECK(m2k_poll_signal(k, 1u << s1) == 0, "poll consumed");

        TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, s0) == M2K_OK, "sig send 0");
        TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, s1) == M2K_OK, "sig send 1b");
        got = m2k_wait_signal(k, 1u << s1);   // blocks only on the masked bit
        if (got != (1 << s1)) { printf("[smoke] FAIL sig wait got=%d\n", got); ++fails; }
        got = m2k_poll_signal(k, 1u << s0);   // bit 0 untouched by that wait
        if (got != (1 << s0)) { printf("[smoke] FAIL sig leftover got=%d\n", got); ++fails; }

        if (m2k_wait_signal(k, 0) != M2K_ERR_RANGE ||
            m2k_send_signal(k, 0x1234, s0) != M2K_ERR_BAD_ITEM ||  // no tasks yet
            m2k_send_signal(k, M2K_TASK_SELF, 33) != M2K_ERR_RANGE) {
            printf("[smoke] FAIL sig rejects\n"); ++fails;
        }

        // Free returns the bit for reuse and drops any pending delivery.
        TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, s5) == M2K_OK, "pre-free pend");
        m2k_free_signal(k, s5);
        if (m2k_alloc_signal(k, 5) != 5 ||
            m2k_poll_signal(k, 1u << 5) != 0) {
            printf("[smoke] FAIL sig free/realloc\n"); ++fails;
        }
    }

    // IOReq completion state machine + ordering.
    {
        m2k_context* k = &g_kctx_a;
        uint32_t io = m2k_create_ioreq_va(k,
                (const m2_tag_arg[]){{7, 0}, {M2K_TAG_IO_DEVICE, 42},
                                     {M2_TAG_END, 0}, {9, 9}},
                4);   // priority-style noise around the real tag; bound hides nothing
        TK_CHECK(io != M2K_ITEM_NONE, "io create");
        if (g_kctx_a.ioreqs[m2k_handle_slot(io)].device != 42) {
            printf("[smoke] FAIL io device tag\n"); ++fails;
        }
        int32_t st = -1, stat = -1;
        TK_CHECK(m2k_query_io(k, io, &st, &stat) == M2K_OK && st == M2K_IO_IDLE,
                 "io idle");

        // DoIO: sync execution; re-arm allowed from COMPLETE.
        TK_CHECK(m2k_do_io(k, io, 0xC0DE, 77) == M2K_OK, "do io");
        TK_CHECK(m2k_query_io(k, io, &st, &stat) == M2K_OK &&
                 st == M2K_IO_COMPLETE && stat == M2K_OK, "io complete state");
        TK_CHECK(m2k_do_io(k, io, 1, 1) == M2K_OK, "re-arm do io");
        TK_CHECK(m2k_wait_io(k, io) == M2K_OK, "wait after do");

        // SendIO: re-arm allowed from COMPLETE; payload recorded per run.
        // (Double-SendIO rejection needs an observable IN_FLIGHT window,
        // which the immediate-completion backend of this slice cannot
        // produce — deferred to the async device shim.)
        TK_CHECK(m2k_send_io(k, io, 0x1234, 88) == M2K_OK, "send io");
        TK_CHECK(g_kctx_a.ioreqs[m2k_handle_slot(io)].what == 0x1234 &&
                 g_kctx_a.ioreqs[m2k_handle_slot(io)].arg == 88,
                 "io payload");
        TK_CHECK(m2k_wait_io(k, io) == M2K_OK, "wait after send");

        // Completion counter counts each transition exactly once, in call
        // order across all requests of the context.
        uint64_t seq = g_kctx_a.ioreqs[m2k_handle_slot(io)].complete_seq;
        uint32_t io2 = m2k_create_ioreq(k);
        TK_CHECK(m2k_do_io(k, io2, 0, 0) == M2K_OK, "second io");
        uint64_t seq2 = g_kctx_a.ioreqs[m2k_handle_slot(io2)].complete_seq;
        if (!(seq2 == seq + 1 && seq >= 2 && g_kctx_a.completions == seq2)) {
            printf("[smoke] FAIL io seq %llu->%llu total=%llu\n",
                   (unsigned long long)seq, (unsigned long long)seq2,
                   (unsigned long long)g_kctx_a.completions);
            ++fails;
        }

        // Invalid-handle rejection: zero, type confusion, bad generation.
        uint32_t wrong_type = (io2 & ~M2K_HANDLE_TYPE_MASK) | M2K_TYPE_MSGPORT;
        uint32_t stale_io = ((m2k_handle_gen(io2) + 1u) << M2K_HANDLE_GEN_SHIFT) |
                            (m2k_handle_slot(io2) << M2K_HANDLE_SLOT_SHIFT) |
                            M2K_TYPE_IOREQ;
        if (m2k_wait_io(k, M2K_ITEM_NONE) != M2K_ERR_BAD_ITEM ||
            m2k_wait_io(k, wrong_type) != M2K_ERR_BAD_ITEM ||
            m2k_do_io(k, stale_io, 0, 0) != M2K_ERR_BAD_ITEM ||
            m2k_send_msg(k, io2, 1, 1) != M2K_ERR_BAD_ITEM ||   // ioreq as port
            m2k_do_io(k, m2k_create_msgport(k), 0, 0) != M2K_ERR_BAD_ITEM) {
            printf("[smoke] FAIL io invalid handles\n"); ++fails;
        }

        TK_CHECK(m2k_delete_ioreq(k, io) == M2K_OK, "io delete");
        TK_CHECK(m2k_wait_io(k, io) == M2K_ERR_BAD_ITEM, "deleted io dead");
        // Slot reuse gets a strictly newer generation: stale vs new never alias.
        uint32_t io3 = m2k_create_ioreq(k);
        if (m2k_handle_slot(io3) != m2k_handle_slot(io) ||
            m2k_handle_gen(io3) <= m2k_handle_gen(io)) {
            printf("[smoke] FAIL io slot reuse %#x -> %#x\n", io, io3); ++fails;
        }
    }

    // TagArg bounded-scan integration on create paths.
    {
        m2k_item_t p1 = m2k_create_msgport_va(&g_kctx_a, NULL, 0);
        if (g_kctx_a.ports[m2k_handle_slot(p1)].priority != 0) {
            printf("[smoke] FAIL tag default prio\n"); ++fails;
        }
        uint32_t p2 = m2k_create_msgport_va(&g_kctx_a,
                (const m2_tag_arg[]){{M2K_TAG_PORT_PRIORITY, 5},
                                     {999, 0xDEAD},
                                     {M2_TAG_END, 0}}, 16);
        if (p2 == M2K_ITEM_NONE ||
            g_kctx_a.ports[m2k_handle_slot(p2)].priority != 5) {
            printf("[smoke] FAIL tag prio parse\n"); ++fails;
        }
        // Priority hidden behind max_count stays invisible (bounded scan).
        uint32_t p3 = m2k_create_msgport_va(&g_kctx_a,
                (const m2_tag_arg[]){{M2K_TAG_PORT_PRIORITY, 9},
                                     {M2_TAG_END, 0}}, 0);
        if (p3 == M2K_ITEM_NONE ||
            g_kctx_a.ports[m2k_handle_slot(p3)].priority != 0) {
            printf("[smoke] FAIL tag bound hide\n"); ++fails;
        }
    }

    m2k_destroy(&g_kctx_a);
    m2k_destroy(&g_kctx_b);

    printf(fails ? "[smoke] guest-kernel FAIL (%d)\n" : "[smoke] guest-kernel ok\n",
           fails);
    return fails;
}

// ---- guest task lifecycle test -------------------------------------------
// Host pthreads behind fixed-width handles: create/join/delete round-trip,
// exactly-once callback execution, terminal-state determinism and
// stale/invalid handle rejection.
static m2k_context g_kctx_ta;
static m2k_context g_kctx_tb;
static int g_task_counter;

#define TASK_MAGIC 0x5A5Au

static int32_t t_noop(m2k_context* ctx, uint32_t arg) {
    (void)ctx; (void)arg;
    return (int32_t)TASK_MAGIC;
}
static int32_t t_inc_once(m2k_context* ctx, uint32_t arg) {
    (void)ctx;
    if (arg != TASK_MAGIC) return M2K_ERR_RANGE;   // payload word must survive
    ++g_task_counter;
    return (int32_t)TASK_MAGIC;
}
static int32_t t_fail(m2k_context* ctx, uint32_t arg) {
    (void)ctx; (void)arg;
    return -77;
}
static int32_t t_block_on_sig(m2k_context* ctx, uint32_t arg) {
    // Guest-visible mask travels in the u32 arg word, never a host pointer.
    return m2k_wait_signal(ctx, arg) > 0 ? 0 : -1;
}

static int32_t t_wait_for_targeted_sig(m2k_context* ctx, uint32_t arg) {
    return m2k_wait_signal(ctx, arg) > 0 ? 0 : -1;
}

static int test_guest_tasks(void) {
    int fails = 0;
    m2k_init(&g_kctx_ta);
    m2k_init(&g_kctx_tb);
    m2k_context* k = &g_kctx_ta;

    // Deterministic handles through the threaded create path: identical op
    // streams on two fresh contexts produce identical task handles.
    {
        uint32_t ha = m2k_create_task(k, t_noop, 0);
        uint32_t hb = m2k_create_task(&g_kctx_tb, t_noop, 0);
        if (ha == M2K_ITEM_NONE || ha != hb ||
            m2k_handle_type(ha) != M2K_TYPE_TASK ||
            m2k_handle_slot(ha) >= M2K_MAX_TASKS ||
            m2k_handle_gen(ha) < 1) {
            printf("[smoke] FAIL task handle stream a=%#x b=%#x\n", ha, hb);
            ++fails;
        }
        TK_CHECK(m2k_join_task(k, ha) == (int32_t)TASK_MAGIC, "noop join a");
        TK_CHECK(m2k_join_task(&g_kctx_tb, hb) == (int32_t)TASK_MAGIC, "noop join b");
        TK_CHECK(m2k_delete_task(k, ha) == M2K_OK, "noop delete a");
        TK_CHECK(m2k_delete_task(&g_kctx_tb, hb) == M2K_OK, "noop delete b");
    }

    // Exactly-once callback: join blocks until completion, counter advanced
    // by exactly one, state terminal, repeat join is stable.
    {
        int32_t st = -1, status = -1;
        g_task_counter = 0;
        uint32_t ht = m2k_create_task(k, t_inc_once, TASK_MAGIC);
        TK_CHECK(ht != M2K_ITEM_NONE, "inc create");
        TK_CHECK(m2k_query_task(k, ht, &st, &status) == M2K_OK &&
                 st >= M2K_TASK_STARTED && st <= M2K_TASK_FAILED,
                 "query pre-join range");
        TK_CHECK(m2k_join_task(k, ht) == (int32_t)TASK_MAGIC, "inc join");
        TK_CHECK(g_task_counter == 1, "counter exactly once");
        st = -1; status = -1;
        TK_CHECK(m2k_query_task(k, ht, &st, &status) == M2K_OK &&
                 st == M2K_TASK_COMPLETED && status == (int32_t)TASK_MAGIC,
                 "completed state");
        TK_CHECK(m2k_join_task(k, ht) == (int32_t)TASK_MAGIC, "rejoin stable");
        TK_CHECK(g_task_counter == 1, "no second increment");

        // Failure path: negative fn result classifies as FAILED and surfaces
        // as the join status.
        uint32_t hf = m2k_create_task(k, t_fail, 0);
        TK_CHECK(m2k_join_task(k, hf) == -77, "fail join status");
        st = -1;
        TK_CHECK(m2k_query_task(k, hf, &st, NULL) == M2K_OK &&
                 st == M2K_TASK_FAILED, "failed state");
        TK_CHECK(m2k_delete_task(k, hf) == M2K_OK, "fail delete");

        // Slot reuse: retire the joined task first (delete refuses anything
        // non-terminal), then the freed slot comes back lowest-free-first
        // with a strictly newer generation.
        TK_CHECK(m2k_join_task(k, ht) == (int32_t)TASK_MAGIC, "pre-reuse join");
        TK_CHECK(m2k_delete_task(k, ht) == M2K_OK, "retire ht");
        uint32_t ht2 = m2k_create_task(k, t_noop, 0);
        if (!(m2k_handle_slot(ht2) == m2k_handle_slot(ht) &&
              m2k_handle_gen(ht2) > m2k_handle_gen(ht))) {
            printf("[smoke] FAIL task slot reuse %#x -> %#x\n", ht, ht2);
            ++fails;
        }
        TK_CHECK(m2k_join_task(k, ht2) == (int32_t)TASK_MAGIC, "reuse join");
        TK_CHECK(m2k_delete_task(k, ht2) == M2K_OK, "reuse delete");
    }

    // Stale/invalid handle rejection across every task entrypoint.
    {
        if (m2k_create_task(k, NULL, 0) != M2K_ITEM_NONE) {
            printf("[smoke] FAIL task null fn\n"); ++fails;
        }
        uint32_t hp = m2k_create_msgport(k);
        TK_CHECK(hp != M2K_ITEM_NONE, "confusion port create");

        uint32_t ht = m2k_create_task(k, t_noop, 0);
        TK_CHECK(ht != M2K_ITEM_NONE, "reject probe create");
        TK_CHECK(m2k_join_task(k, M2K_ITEM_NONE) == M2K_ERR_BAD_ITEM, "join zero");
        TK_CHECK(m2k_delete_task(k, M2K_ITEM_NONE) == M2K_ERR_BAD_ITEM, "delete zero");
        TK_CHECK(m2k_query_task(k, M2K_ITEM_NONE, NULL, NULL) == M2K_ERR_BAD_ITEM, "query zero");
        TK_CHECK(m2k_join_task(k, hp) == M2K_ERR_BAD_ITEM, "port as task");
        TK_CHECK(m2k_do_io(k, ht, 0, 0) == M2K_ERR_BAD_ITEM, "task as ioreq");
        TK_CHECK(m2k_wait_port(k, ht) == M2K_ERR_BAD_ITEM, "task as port");

        // Type-flipped clone of a live handle still cannot resolve.
        uint32_t flipped = (ht & ~M2K_HANDLE_TYPE_MASK) | M2K_TYPE_MSGPORT;
        TK_CHECK(m2k_join_task(k, flipped) == M2K_ERR_BAD_ITEM, "flipped type");

        // Delete makes every operation on the old handle fail; the stale
        // generation rebuild cannot alias the recycled slot either.
        TK_CHECK(m2k_join_task(k, ht) == (int32_t)TASK_MAGIC, "stale setup join");
        TK_CHECK(m2k_delete_task(k, ht) == M2K_OK, "stale setup delete");
        TK_CHECK(m2k_join_task(k, ht) == M2K_ERR_BAD_ITEM, "join deleted");
        TK_CHECK(m2k_delete_task(k, ht) == M2K_ERR_BAD_ITEM, "double delete");
        uint32_t stale = ((m2k_handle_gen(ht)) << M2K_HANDLE_GEN_SHIFT) |
                         (m2k_handle_slot(ht) << M2K_HANDLE_SLOT_SHIFT) |
                         M2K_TYPE_TASK;
        TK_CHECK(m2k_query_task(k, stale, NULL, NULL) == M2K_ERR_BAD_ITEM, "stale query");
        TK_CHECK(m2k_delete_msgport(k, hp) == M2K_OK, "confusion cleanup");
    }

    // Delete refuses non-terminal tasks; unblocking via signals lets the
    // lifecycle finish normally.
    {
        int sb = m2k_alloc_signal(k, -1);
        TK_CHECK(sb >= 0, "blocker sig alloc");
        uint32_t hb = m2k_create_task(k, t_block_on_sig, 1u << sb);
        TK_CHECK(hb != M2K_ITEM_NONE, "blocker create");
        TK_CHECK(m2k_delete_task(k, hb) == M2K_ERR_STATE, "delete running refused");
        TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, sb) == M2K_OK, "unblock send");
        TK_CHECK(m2k_join_task(k, hb) == M2K_OK, "blocker join");
        int32_t st = -1;
        TK_CHECK(m2k_query_task(k, hb, &st, NULL) == M2K_OK &&
                 st == M2K_TASK_COMPLETED, "blocker completed");
        TK_CHECK(m2k_delete_task(k, hb) == M2K_OK, "blocker delete");
        TK_CHECK(m2k_join_task(k, hb) == M2K_ERR_BAD_ITEM, "deleted blocker dead");
    }

    // Explicit task handles route signals to only the selected recipient.
    // Two blocked tasks share no pending bits: signaling A must not wake B.
    {
        const int sa = m2k_alloc_signal(k, -1);
        const int sb = m2k_alloc_signal(k, -1);
        uint32_t ta = m2k_create_task(k, t_wait_for_targeted_sig, 1u << sa);
        uint32_t tb = m2k_create_task(k, t_wait_for_targeted_sig, 1u << sb);
        TK_CHECK(ta != M2K_ITEM_NONE && tb != M2K_ITEM_NONE,
                 "targeted task create");
        TK_CHECK(m2k_send_signal(k, ta, sa) == M2K_OK, "targeted send A");
        int32_t st_a = -1, st_b = -1;
        for (int spin = 0; spin < 1000; ++spin) {
            m2k_query_task(k, ta, &st_a, NULL);
            m2k_query_task(k, tb, &st_b, NULL);
            if (st_a == M2K_TASK_COMPLETED) break;
            sched_yield();
        }
        TK_CHECK(st_a == M2K_TASK_COMPLETED, "targeted A woke");
        TK_CHECK(st_b != M2K_TASK_COMPLETED, "targeted B isolated");
        TK_CHECK(m2k_send_signal(k, tb, sb) == M2K_OK, "targeted send B");
        TK_CHECK(m2k_join_task(k, ta) == M2K_OK, "targeted A join");
        TK_CHECK(m2k_join_task(k, tb) == M2K_OK, "targeted B join");
        TK_CHECK(m2k_delete_task(k, ta) == M2K_OK, "targeted A delete");
        TK_CHECK(m2k_delete_task(k, tb) == M2K_OK, "targeted B delete");
        m2k_free_signal(k, sa);
        m2k_free_signal(k, sb);
    }

    // Fixed-width pool: M2K_MAX_TASKS live tasks fill it exactly; the next
    // create reports NO_SLOT until the blocked tasks are drained.
    {
        int bits[M2K_MAX_TASKS];
        m2k_item_t handles[M2K_MAX_TASKS];
        for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
            bits[i] = m2k_alloc_signal(k, -1);
            char nm[64];
            snprintf(nm, sizeof(nm), "pool sig %u", i);
            TK_CHECK(bits[i] >= 0, nm);
            handles[i] = m2k_create_task(k, t_block_on_sig, 1u << bits[i]);
            snprintf(nm, sizeof(nm), "pool create %u", i);
            TK_CHECK(handles[i] != M2K_ITEM_NONE, nm);
        }
        TK_CHECK(m2k_create_task(k, t_noop, 0) == M2K_ITEM_NONE, "pool exhausted");
        for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
            char nm[64];
            snprintf(nm, sizeof(nm), "pool drain %u", i);
            TK_CHECK(m2k_send_signal(k, M2K_TASK_SELF, bits[i]) == M2K_OK, nm);
            TK_CHECK(m2k_join_task(k, handles[i]) == M2K_OK, nm);
            TK_CHECK(m2k_delete_task(k, handles[i]) == M2K_OK, nm);
        }
        uint32_t again = m2k_create_task(k, t_noop, 0);
        TK_CHECK(again != M2K_ITEM_NONE, "pool refill after drain");
        TK_CHECK(m2k_join_task(k, again) == (int32_t)TASK_MAGIC, "refill join");
        TK_CHECK(m2k_delete_task(k, again) == M2K_OK, "refill delete");
    }

    m2k_destroy(&g_kctx_ta);
    m2k_destroy(&g_kctx_tb);

    printf(fails ? "[smoke] guest-tasks FAIL (%d)\n" : "[smoke] guest-tasks ok\n",
           fails);
    return fails;
}

// ---- gate-family selector test (typed marshalling + production handlers) --
// Runs every installed kernel gate family through the SAME production handler
// template the guest-entry path installs, against a local kernel context and
// local guest RAM: fixed-width handles in/out of c->gpr[3], blocking semantics,
// bounded module table, TE mirror channel, thread spawn/join via recomp_lookup,
// and unknown-ordinal fallback preservation.
static m2k_context g_gate_kctx;
static PPCContext g_gate_cpu;
// Full guest image size: spawned task bodies execute lifted code that reads
// and writes baked absolute guest EAs (text/data/bss globals), so the bound
// context must expose the same addressable image as production.
static unsigned char g_gate_mem[RECOMP_GUEST_MEM];

#define GK_CHECK(cond, name) \
    do { if (!(cond)) { printf("[smoke] FAIL gate %s\n", name); ++fails; } } while (0)

static int test_gate_families(void) {
    int fails = 0;
    m2k_init(&g_gate_kctx);
    memset(&g_gate_cpu, 0, sizeof(g_gate_cpu));
    g_gate_cpu.mem = g_gate_mem;
    GK_CHECK(m2_guest_kernel_bind(&g_gate_kctx, &g_gate_cpu) == 0, "bind");

    m2_kernel_dispatch reg = *m2_guest_kernel_handlers();
    // Broker window (kernel 176/168/34/150) must be present in the bound template.
    // AllocMem/FreeMem are guest-owned heap and must be present.
    // printf is now typed (kernel 309) via gk_printf, so it must be present.
    if (!reg.WaitPort || !reg.CreateThread || !reg.ImportByName ||
        !reg.GetTEReadPointer || !reg.FindNamedItem ||
        !reg.CreateBufferedMsg || !reg.LookupItem || !reg.SendMsg ||
        !reg.AllocMem || !reg.FreeMem ||
        !reg.printf) {
        printf("[smoke] FAIL gate template\n"); ++fails;
    }
    m2_kernel_dispatch_set_global(&reg);
    PPCContext rc;
    memset(&rc, 0, sizeof(rc));

    // ---- messages: create -> small-send -> wait -> get -> reply ----------
    uint32_t port = 0, item = 0;
    rc.gpr[3] = 0x11;   // sentinel: must be overwritten by a real handle
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_MSGPORT, 0, &reg) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_MSGPORT) {
        printf("[smoke] FAIL gate msgport ret=%#x\n", rc.gpr[3]); ++fails;
    } else {
        port = rc.gpr[3];
    }
    rc.gpr[3] = 42; rc.gpr[4] = 4242; rc.gpr[5] = 7;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_SMALL_MSG, 0, &reg) != 1 ||
        rc.gpr[3] == 0) {
        printf("[smoke] FAIL gate smallmsg\n"); ++fails;
    } else {
        item = rc.gpr[3];
    }
    rc.gpr[3] = port; rc.gpr[4] = item; rc.gpr[5] = 99; rc.gpr[6] = 0;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_SEND_SMALL_MSG, 0, &reg) == 1 &&
             rc.gpr[3] == (uint32_t)M2K_OK, "sendsmallmsg");
    // The consumed item handle is dead now.
    rc.gpr[3] = port; rc.gpr[4] = item; rc.gpr[5] = 99; rc.gpr[6] = 0;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_SEND_SMALL_MSG, 0, &reg) == 1 &&
             rc.gpr[3] == (uint32_t)M2K_ERR_BAD_ITEM, "stale smallmsg");
    // WaitPort(port, aux) returns the incoming message item and dequeues it
    // (source contract, portfolio_os_m2 kernel/msgport.c). Verify the small
    // message's payload arrived intact before WaitPort consumes the queue,
    // then confirm WaitPort hands back a real M2K_TYPE_MESSAGE item and that
    // the item replies cleanly to release its slot.
    uint32_t what = 0, value = 0;
    GK_CHECK(m2k_get_msg(&g_gate_kctx, port, NULL, &what, &value) == M2K_OK &&
             what == 42 && value == 4242, "smallmsg payload");
    if (m2k_send_msg(&g_gate_kctx, port, 43, 4343) != M2K_OK) {
        printf("[smoke] FAIL gate waitport refill\n"); ++fails;
    }
    rc.gpr[3] = port; rc.gpr[4] = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_WAIT_PORT, 0, &reg) != 1 ||
        rc.gpr[3] == 0 || m2k_handle_type(rc.gpr[3]) != M2K_TYPE_MESSAGE) {
        printf("[smoke] FAIL gate waitport queued h=%#x\n", rc.gpr[3]); ++fails;
    } else {
        uint32_t wmsg = rc.gpr[3];
        rc.gpr[3] = wmsg;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_REPLY_MSG, 0, &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_OK, "reply waitport msg");
    }
    // GetMsg through dispatch: message available then Opera-zero when empty.
    if (m2k_send_msg(&g_gate_kctx, port, 10, 100) != M2K_OK) {
        printf("[smoke] FAIL gate refill\n"); ++fails;
    }
    rc.gpr[3] = port;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_GET_MSG, 0, &reg) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_MESSAGE) {
        printf("[smoke] FAIL gate getmsg h=%#x\n", rc.gpr[3]); ++fails;
    } else {
        uint32_t msg = rc.gpr[3];
        rc.gpr[3] = port;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_GET_MSG, 0,
                                             &reg) == 1 && rc.gpr[3] == 0,
                 "getmsg empty zero");
        rc.gpr[3] = msg;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_REPLY_MSG, 0,
                                             &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_OK, "replymsg");
        rc.gpr[3] = msg;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_REPLY_MSG, 0,
                                             &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_ERR_BAD_ITEM, "double replymsg");
    }
    // GetThisMsg shares the dequeue semantics (result discarded by caller).
    if (m2k_send_msg(&g_gate_kctx, port, 11, 110) != M2K_OK) {
        printf("[smoke] FAIL gate thismsg refill\n"); ++fails;
    }
    rc.gpr[3] = port;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_GET_THIS_MSG, 0,
                                         &reg) == 1 && rc.gpr[3] != 0,
             "getthismsg");
    rc.gpr[3] = port;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_GET_THIS_MSG, 0,
                                         &reg) == 1 && rc.gpr[3] == 0,
             "getthismsg empty");

    // ---- IOReqs: CheckIO/SendIO/WaitIO/DoIO/AbortIO ----------------------
    rc.gpr[3] = 0;
    rc.gpr[4] = 0;
    rc.gpr[5] = 0;
    rc.gpr[6] = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_IOREQ, 0, &reg) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_IOREQ) {
        printf("[smoke] FAIL gate ioreq\n"); ++fails;
    } else {
        uint32_t io = rc.gpr[3];
        rc.gpr[3] = io;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_CHECK_IO, 0,
                                             &reg) == 1 &&
                 (int32_t)rc.gpr[3] == M2K_ERR_STATE, "checkio idle");
        rc.gpr[3] = io; rc.gpr[4] = 0x00ABC000u;   // guest IOInfo EA word
        // Publish a documented, accepted command *with its documented argument
        // form* in the IOInfo. A zero word is CMD_WRITE (documented only in the
        // non-external SDK build), an arbitrary word is not a documented
        // command at all, and a bare command word without the shape its driver
        // validates is now refused too -- see device_command_contract.h.
        wr32(&g_gate_cpu, 0x00ABC000u, 2u);   // CMD_STATUS (accepted)
        wr32(&g_gate_cpu, 0x00ABC018u, 0x00ABE000u);  // ioi_Recv.iob_Buffer
        wr32(&g_gate_cpu, 0x00ABC01cu, 40u);          // ioi_Recv.iob_Len
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_SEND_IO, 0, &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_OK, "sendio");
        GK_CHECK(g_gate_kctx.ioreqs[m2k_handle_slot(io)].arg == 0x00ABC000u,
                 "sendio info ea recorded");
        rc.gpr[3] = io;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_CHECK_IO, 0,
                                             &reg) == 1 && rc.gpr[3] == 1,
                 "checkio done");
        rc.gpr[3] = io;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_WAIT_IO, 0, &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_OK, "waitio");
        rc.gpr[3] = io; rc.gpr[4] = 0x00ABD000u;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_ABORT_IO, 0, &reg) == 1 &&
                 (int32_t)rc.gpr[3] == M2K_ERR_STATE, "abortio completed");
        rc.gpr[3] = 0xDEAD0BADu;   // invalid handle: nonzero negative answer
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_CHECK_IO, 0, &reg) == 1 &&
                 (int32_t)rc.gpr[3] == M2K_ERR_BAD_ITEM, "checkio bad");
        rc.gpr[3] = io; rc.gpr[4] = 8;
        // The same documented shape at this scratch IOInfo.
        wr32(&g_gate_cpu, 8u, 2u);              // CMD_STATUS (accepted)
        wr32(&g_gate_cpu, 8u + 0x18u, 0x00ABE100u);  // ioi_Recv.iob_Buffer
        wr32(&g_gate_cpu, 8u + 0x1cu, 40u);          // ioi_Recv.iob_Len
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_DO_IO, 0, &reg) == 1 &&
                 rc.gpr[3] == (uint32_t)M2K_OK, "doio after send");
    }

    // ---- signals: guest selectors use Portfolio signal masks --------------
    rc.gpr[3] = 0;              // AllocSignal(0): lowest free user bit
    uint32_t sig_mask = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_ALLOC_SIGNAL, 0, &reg) == 1 &&
        rc.gpr[3] != 0 && (rc.gpr[3] & (rc.gpr[3] - 1u)) == 0) {
        sig_mask = rc.gpr[3];
    } else {
        printf("[smoke] FAIL gate allocsig r=%#x\n", rc.gpr[3]); ++fails;
    }
    rc.gpr[3] = 0; rc.gpr[4] = sig_mask;   // SendSignal(SELF, signal mask)
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_SEND_SIGNAL, 0, &reg) == 1 &&
             rc.gpr[3] == (uint32_t)M2K_OK, "sendsignal");
    rc.gpr[3] = sig_mask;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_WAIT_SIGNAL, 0, &reg) == 1 &&
             rc.gpr[3] == sig_mask, "waitsignal consumes");
    rc.gpr[3] = sig_mask;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_FREE_SIGNAL, 0, &reg) == 1,
             "freesignal");
    rc.gpr[3] = 0;                   // lowest user mask is reallocable
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_ALLOC_SIGNAL, 0,
                                         &reg) == 1 &&
             rc.gpr[3] == sig_mask, "realloc signal");
    rc.gpr[3] = 0x1234; rc.gpr[4] = sig_mask;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_SEND_SIGNAL, 0, &reg) == 1 &&
             rc.gpr[3] == (uint32_t)M2K_ERR_BAD_ITEM, "signal foreign task");

    // ---- task-event channel: write pointer mirrored to the read side ------
    rc.gpr[3] = 0x12345678u;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_SET_TE_WRITE_POINTER, 0,
                                         &reg) == 1 &&
             rc.gpr[3] == 0x12345678u,
             "sette");
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_GET_TE_READ_POINTER, 0,
                                         &reg) == 1 &&
             rc.gpr[3] == 0x12345678u, "gette mirror");

    // ---- event broker: kernel 176/168/34/150 ------------------------------
    // ConnectEventBroker sequence against the production handlers: named-item
    // lookup -> buffered record mint -> record echo -> in-place delivery.
    // Record/descriptor EAs come from the runtime's fixed carve (private
    // constants), so delivery is checked shape-wise: BE u32 sent count at
    // +0x28 and a distinct nonzero descriptor EA at +0x2c. WaitPort on the
    // drained port must answer 0 immediately — SendMsg delivers in place,
    // nothing is ever queued, so a blocking wait would hang the caller.
    {
        static const char broker_name[] = "eventbroker";
        memcpy(g_gate_mem + 0x4000, broker_name, sizeof(broker_name));
        uint32_t named = 0, record = 0;
        rc.gpr[3] = 0x10a; rc.gpr[4] = 0x4000;
        if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                        M2_KSEL_FIND_NAMED_ITEM, 0,
                                        &reg) != 1 || rc.gpr[3] == 0) {
            printf("[smoke] FAIL gate findnameditem r=%#x\n", rc.gpr[3]);
            ++fails;
        } else {
            named = rc.gpr[3];
        }
        rc.gpr[3] = 0; rc.gpr[4] = 0;
        if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                        M2_KSEL_CREATE_BUFFERED_MSG, 0,
                                        &reg) != 1 || rc.gpr[3] == 0) {
            printf("[smoke] FAIL gate createbufferedmsg r=%#x\n", rc.gpr[3]);
            ++fails;
        } else {
            record = rc.gpr[3];
        }
        if (record) {
            rc.gpr[3] = record;
            GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                                 M2_KSEL_LOOKUP_ITEM, 0,
                                                 &reg) == 1 &&
                     rc.gpr[3] == record, "lookupitem echoes record");
            rc.gpr[3] = record; rc.gpr[4] = named;   // port word rides along
            GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                                 M2_KSEL_SEND_MSG, 0,
                                                 &reg) == 1 && rc.gpr[3] == 0,
                     "sendmsg ok");
            GK_CHECK(rd32(&g_gate_cpu, record + 0x28) == 1,
                     "sendmsg record count");
            uint32_t desc = rd32(&g_gate_cpu, record + 0x2c);
            GK_CHECK(desc != 0 && desc != record, "sendmsg descriptor ea");
        }
        rc.gpr[3] = port; rc.gpr[4] = 0;
        GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                             M2_KSEL_WAIT_PORT, 0,
                                             &reg) == 1 && rc.gpr[3] == 0,
                 "waitport empty immediate");
    }

    // ---- modules: deterministic bounded table ------------------------------
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_FIND_CURRENT_MODULE, 0,
                                         &reg) == 1 && rc.gpr[3] != 0,
             "findcurrentmodule");
    uint32_t cur_mod = rc.gpr[3];
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_FIND_CURRENT_MODULE, 0,
                                         &reg) == 1 && rc.gpr[3] == cur_mod,
             "findcurrentmodule stable");
    static const char gk_import_name[] = "graphics";
    memcpy(g_gate_mem + 0x1000, gk_import_name, sizeof(gk_import_name));
    memcpy(g_gate_mem + 0x2000, "nope", 5);
    memset(g_gate_mem + 0x3000, 'X', 80);   // unterminated within the bound
    rc.gpr[3] = cur_mod; rc.gpr[4] = 0x1000;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_IMPORT_BY_NAME, 0, &reg) != 1 ||
        rc.gpr[3] == 0 || rc.gpr[3] == cur_mod) {
        printf("[smoke] FAIL gate import graphics=%#x\n", rc.gpr[3]); ++fails;
    }
    // Repeat-import identity: same name -> same folio Item.
    {
        PPCContext rc2;
        memset(&rc2, 0, sizeof(rc2));
        rc2.gpr[3] = cur_mod; rc2.gpr[4] = 0x1000;
        recomp_call_kernel_dispatch(&rc2, M2_FOLIO_KERNEL,
                                    M2_KSEL_IMPORT_BY_NAME, 0, &reg);
        GK_CHECK(rc2.gpr[3] == rc.gpr[3], "import stable");
    }
    // Failures are deterministic negative Errs per the Portfolio convention.
    rc.gpr[3] = cur_mod; rc.gpr[4] = 0x2000;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_IMPORT_BY_NAME, 0,
                                         &reg) == 1 &&
             (int32_t)rc.gpr[3] < 0, "import unknown negative");
    rc.gpr[3] = cur_mod; rc.gpr[4] = 0x3000;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_IMPORT_BY_NAME, 0,
                                         &reg) == 1 &&
             (int32_t)rc.gpr[3] < 0, "import unterminated negative");
    rc.gpr[3] = 0xDEAD; rc.gpr[4] = 0x1000;   // foreign module operand
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_IMPORT_BY_NAME, 0,
                                         &reg) == 1 &&
             (int32_t)rc.gpr[3] == M2M_ERR_BAD_ITEM, "import bad module");

    // ---- strings: strcpy/strcat build guest filenames in place ------------
    // trackInit_2 builds the QMF filename as strcpy(buf, trackname) +
    // strcat(buf, ".qmf"-style suffix) (kernel 315/317); without typed
    // handlers the buffer stays empty and InitCar prints "Error reading
    // track data base file" with an empty name. Verify the copies land in
    // guest RAM and the dispatcher marshals the r3/r4 window.
    memcpy(g_gate_mem + 0x5000, "roadusa", 8);
    memcpy(g_gate_mem + 0x5100, ".qmf", 5);
    memset(g_gate_mem + 0x5200, 0, 16);
    rc.gpr[3] = 0x5200; rc.gpr[4] = 0x5000;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_STRCPY, 0, &reg) == 1 &&
             rc.gpr[3] == 0x5200 &&
             memcmp(g_gate_mem + 0x5200, "roadusa", 8) == 0, "strcpy");
    rc.gpr[3] = 0x5200; rc.gpr[4] = 0x5100;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_STRCAT, 0, &reg) == 1 &&
             rc.gpr[3] == 0x5200 &&
             memcmp(g_gate_mem + 0x5200, "roadusa.qmf", 12) == 0,
             "strcat appends");
    // Fail closed on out-of-image source: dst untouched, no crash.
    rc.gpr[3] = 0x5200; rc.gpr[4] = 0xDEAD0000u;
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_STRCPY, 0, &reg) == 1 &&
             rc.gpr[3] == 0x5200 &&
             memcmp(g_gate_mem + 0x5200, "roadusa.qmf", 12) == 0,
             "strcpy oob src");

    // ---- threads: real spawn through recomp_lookup, join, delete ----------
    // Body = M_PreLight @ guest 0x15e568: a size-0 lifted function (empty C
    // body), so spawn/join exercises recomp_lookup + the full task lifecycle
    // with zero guest side effects. Do NOT spawn descriptor-driven bodies
    // here: M_SetupDynLit treats r3 as an arena/clist descriptor and chases
    // raw image bytes (0xa4/0x60c(r31), M_ClistManagerC grow path), so a fake
    // arg races concurrent bodies into wild wr32s (observed SIGSEGV on the
    // task thread while main blocked in m2k_join_task).
    const uint32_t kThreadProbeEA = 0x15e568u;
    // CreateThread (kernel 555) Portfolio window: code=r3 name=r4 pri=r5
    // stack=r6 tags=r7 — gk_CreateThread resolves code_ea via recomp_lookup
    // (the pre-9e64b78 4-arg window with code in r4 was a stale ABI defect).
    rc.gpr[3] = kThreadProbeEA; rc.gpr[4] = 0; rc.gpr[5] = 0;
    rc.gpr[6] = 0x2000; rc.gpr[7] = 0;
    uint32_t ht = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_THREAD, 0, &reg) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_TASK) {
        printf("[smoke] FAIL gate createthread h=%#x\n", rc.gpr[3]); ++fails;
    } else {
        ht = rc.gpr[3];
        uint8_t thread_priority = 0;
        if (m2k_task_priority(&g_gate_kctx, ht, &thread_priority) != M2K_OK ||
            thread_priority != 200u) {
            printf("[smoke] FAIL zero-priority CreateThread inheritance: %u\n",
                   thread_priority);
            ++fails;
        }
    }
    // VA slice (kernel 84): documented Portfolio window — code=r3, name=r4,
    // pri=r5, stack=r6, then TagArg pairs (NewThread passes 14=ARGC /
    // 15=ARGP). The body EA is the module-relative M_PreLight offset
    // (0x5e568 -> lookup key 0x15e568): a stale r4-as-entry marshalling or a
    // missing relocation base both leave r3 unresolvable and fail this spawn.
    rc.gpr[3] = kThreadProbeEA - 0x100000u;   // code: FN_M_PreLight__5e568
    rc.gpr[4] = 0;                            // name_ea: recorded word
    rc.gpr[5] = 10;                           // pri (TASK_MIN_PRIORITY)
    rc.gpr[6] = 0x800;                        // stackSize
    rc.gpr[7] = 14; rc.gpr[8] = 0;            // CREATETASK_TAG_ARGC = 0
    rc.gpr[9] = 15; rc.gpr[10] = 0;           // CREATETASK_TAG_ARGP = NULL
    uint32_t hva = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_THREAD_VA, 0, &reg) != 1 ||
        m2k_handle_type(rc.gpr[3]) != M2K_TYPE_TASK) {
        printf("[smoke] FAIL gate createthreadva h=%#x\n", rc.gpr[3]); ++fails;
    } else {
        hva = rc.gpr[3];
        uint8_t thread_priority = 0;
        if (m2k_task_priority(&g_gate_kctx, hva, &thread_priority) != M2K_OK ||
            thread_priority != 10u) {
            printf("[smoke] FAIL explicit CreateThreadVA priority: %u\n",
                   thread_priority);
            ++fails;
        }
    }
    rc.gpr[3] = 0xDEADBEEFu; rc.gpr[4] = 0;   // unresolvable EA fails closed
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_CREATE_THREAD_VA, 0,
                                         &reg) == 1 && rc.gpr[3] == 0,
             "createthreadva bad ea");
    rc.gpr[3] = 0xDEADBEEFu; rc.gpr[4] = 0;   // unresolvable code EA fails closed
    GK_CHECK(recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                         M2_KSEL_CREATE_THREAD, 0,
                                         &reg) == 1 && rc.gpr[3] == 0,
             "createthread bad ea");
    if (ht) {
        int32_t st = -1, status = -1;
        int32_t joined = m2k_join_task(&g_gate_kctx, ht);
        GK_CHECK(m2k_query_task(&g_gate_kctx, ht, &st, &status) == M2K_OK &&
                 st == (joined >= 0 ? M2K_TASK_COMPLETED : M2K_TASK_FAILED),
                 "thread terminal");
        GK_CHECK(m2k_delete_task(&g_gate_kctx, ht) == M2K_OK, "thread delete");
    }
    if (hva) {
        GK_CHECK(m2k_join_task(&g_gate_kctx, hva) >= 0, "va thread join");
        GK_CHECK(m2k_delete_task(&g_gate_kctx, hva) == M2K_OK, "va delete");
    }

    // ---- fallback preservation --------------------------------------------
    // Unknown ordinal/folio fall through; printf is now registered so it must dispatch.
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, 999, 0, &reg) != 0 ||
        recomp_call_kernel_dispatch(&rc, M2_FOLIO_AUDIO, 8, 0, &reg) != 0) {
        printf("[smoke] FAIL gate fallback\n"); ++fails;
    }
    // printf via dispatch should now succeed (was deferred). Provide a dummy fmt.
    {
        uint32_t fmt_ea = 0x5000;
        memcpy(g_gate_mem + fmt_ea, "fallback", 9);
        rc.gpr[3]=fmt_ea; rc.gpr[4]=0;
        if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL, M2_KSEL_PRINTF, 0, &reg) != 1) {
            printf("[smoke] FAIL gate printf fallback\n"); ++fails;
        }
    }

    // Fail-closed selectors: an unimplemented filesystem ordinal through the
    // generated-call wrapper forces gpr[3] to Item-none instead of leaving
    // the stale argument register (FindMicroCard livelock regression). The
    // directory ordinals 32/33/34 and the raw-file ordinals 35/36/37/38/39/40
    // are now all typed (38 WriteRawFile + 39 SeekRawFile promoted with the
    // writeRepNode/freopen call shapes), so ord 41 (OpenRawFileInDir, never
    // called by the game, no dispatch case) is the canonical fail-closed probe.
    rc.gpr[3] = 0x12345678u;   // looks like the stale name-pointer arg
    recomp_call_selector(&rc, M2_FOLIO_FILESYSTEM, 41, 0);
    GK_CHECK(rc.gpr[3] == 0, "selector failclosed fs41");
    // Typed raw-file write with an invalid handle fails closed negative
    // (truthful error, not Item-none 0 and not the stale register).
    rc.gpr[3] = 0x12345678u; rc.gpr[4] = 0x1000; rc.gpr[5] = 4;
    recomp_call_selector(&rc, M2_FOLIO_FILESYSTEM, 38, 0);
    GK_CHECK((int32_t)rc.gpr[3] < 0, "selector typed fs38 invalid-handle negative");
    // printf is now typed, so selector should not fail closed — it prints.
    {
        uint32_t fmt_ea = 0x5000;
        memcpy(g_gate_mem + fmt_ea, "seltest", 8);
        rc.gpr[3]=fmt_ea; rc.gpr[4]=0;
        recomp_call_selector(&rc, M2_FOLIO_KERNEL, M2_KSEL_PRINTF, 0);
        // printf returns chars printed (7), not 0
        if (rc.gpr[3]==0 || rc.gpr[3]==0x12345678u) { printf("[smoke] FAIL selector printf dispatched\n"); ++fails; }
    }
    rc.gpr[3] = 0x12345678u;
    recomp_call_selector_u32(&rc, M2_FOLIO_AUDIO, 8, 0);
    if (rc.gpr[3] != 0) { printf("[smoke] FAIL selector failclosed u32\n"); ++fails; }

    // Unbind: handlers fail closed with no binding.
    m2_guest_kernel_unbind();
    rc.gpr[3] = 0;
    if (recomp_call_kernel_dispatch(&rc, M2_FOLIO_KERNEL,
                                    M2_KSEL_CREATE_MSGPORT, 0, &reg) != 1 ||
        rc.gpr[3] != 0) {
        printf("[smoke] FAIL gate unbind closed\n"); ++fails;
    }

    m2_kernel_dispatch_set_global(NULL);
    m2k_destroy(&g_gate_kctx);

    printf(fails ? "[smoke] gate-families FAIL (%d)\n"
                 : "[smoke] gate-families ok\n", fails);
    return fails;
}

int main(void) {
    int sel_fails = test_selector_registry();
    int msgport_fails = test_msgport_selector();
    int tag_fails = test_tag_helpers();
    int kern_fails = test_guest_kernel();
    int task_fails = test_guest_tasks();
    int gate_fails = test_gate_families();
    printf("[smoke] recomp smoke start\n");
    printf("[smoke] text %u data %u bss %u\n", g_recomp_text_size, g_recomp_data_size, g_recomp_bss_size);
    printf("[smoke] bases text=%#x data=%#x bss=%#x\n", g_recomp_text_base, g_recomp_data_base, g_recomp_bss_base);
    // Verify lookup for a few known guest addresses
    unsigned test_addrs[] = {0x00100000, 0x00100000+0x7606c-4, 0x176070};
    for (unsigned i=0;i<sizeof(test_addrs)/sizeof(test_addrs[0]);++i) {
        RecompFn fn = recomp_lookup(test_addrs[i]);
        printf("[smoke] lookup %#x -> %p\n", test_addrs[i], (void*)fn);
    }
    // Call the recomp entry (it will alloc guest mem and call FN_M_SetupDynLit)
    m2_game_entry(0, 0);
    printf("[smoke] m2_game_entry returned\n");
    // Live handoff: m2_game_entry returns while the detached game-main task
    // still executes on its own host thread and owns the static kernel
    // context (teardown happens later in m2_recomp_shutdown), so the global
    // guest-kernel registry must still be installed here — preserved, not
    // cleared. No join: the smoke never blocks on the live task.
    int teardown_fails = 0;
    if (m2_kernel_dispatch_global()->CreateMsgPort == NULL ||
        m2_kernel_dispatch_global()->WaitPort == NULL ||
        m2_kernel_dispatch_global()->CreateThread == NULL) {
        printf("[smoke] FAIL guest-kernel registry not preserved across live handoff\n");
        ++teardown_fails;
    } else {
        printf("[smoke] guest-kernel registry preserved across live handoff\n");
    }
    // Direct call to one recompiled function: M_PreLight (size-0 body) proves
    // the dispatch-table call path. SetupDynLit is deliberately NOT called
    // here anymore — outside game boot it has no real arena descriptor, and
    // its zero-descriptor run chases image bytes through M_ClistManagerC.
    PPCContext ctx = {0};
    static unsigned char mem[64<<20];
    ctx.mem = mem;
    ctx.gpr[1] = 0x1812a0 + 0x8000;
    {
        RecompFn probe = recomp_lookup(0x15e568u);
        if (!probe) { printf("[smoke] FAIL probe lookup\n"); ++teardown_fails; }
        else probe(&ctx);
    }
    printf("[smoke] direct recomp_lookup call ok\n");
    printf("[smoke] PASS\n");
    return (sel_fails || msgport_fails || tag_fails || kern_fails ||
            task_fails || gate_fails || teardown_fails) ? 1 : 0;
}
