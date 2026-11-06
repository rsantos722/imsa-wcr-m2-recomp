// guest_module.c — deterministic guest module/item identity model.
// See include/guest_module.h for the model and guest-safety contract.
// Pure static-pool state machine: no malloc, no host pointers in guest-visible
// values, identical op sequences yield identical handle streams. Single-
// threaded by contract (recomp thread only), so no locks.

#include "guest_module.h"

#include <string.h>

#include "shim_abi.h"   // M2_FOLIO_AUDIO

static m2m_item_t m2m_make_handle(uint32_t type, uint32_t slot, uint32_t gen) {
    return (gen << M2M_HANDLE_GEN_SHIFT) |
           ((slot << M2M_HANDLE_SLOT_SHIFT) & M2M_HANDLE_SLOT_MASK) |
           (type & M2M_HANDLE_TYPE_MASK);
}

static uint32_t m2m_err_item(int32_t code) {
    return (uint32_t)code;   // negative Err in Item clothing
}

void m2m_init(m2m_context* ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

// ---- kernel selector 262 -----------------------------------------------------
m2m_item_t m2m_find_current_module(m2m_context* ctx) {
    if (!ctx) return m2m_err_item(M2M_ERR_BAD_ITEM);
    if (!ctx->module_used[0]) {
        ctx->module_used[0] = 1;
        ctx->module_gen[0] = ++ctx->gen_counter;
    }
    // Minted once, then returned unchanged for the context's lifetime: every
    // call site must observe the same "current module" identity.
    return m2m_make_handle(M2M_TYPE_MODULE, 0, ctx->module_gen[0]);
}

// Module operand resolver for selector 103: the handle must be a live module
// of THIS context. Stale generations (slot re-use can't happen here today,
// but the check keeps the handle grammar honest) fail closed.
static int32_t m2m_resolve_module(const m2m_context* ctx, m2m_item_t module) {
    if (m2m_handle_type(module) != M2M_TYPE_MODULE) return M2M_ERR_BAD_ITEM;
    if (module == M2M_ITEM_NONE) return M2M_ERR_BAD_ITEM;
    uint32_t slot = m2m_handle_slot(module);
    if (slot >= M2M_MAX_MODULES) return M2M_ERR_BAD_ITEM;
    if (!ctx->module_used[slot]) return M2M_ERR_BAD_ITEM;
    if (ctx->module_gen[slot] != m2m_handle_gen(module)) return M2M_ERR_BAD_ITEM;
    return M2M_OK;
}

// ---- bounded big-endian guest string copy --------------------------------------
int32_t m2m_rd8_guest(void* ud, uint32_t ea) {
    PPCContext* c = (PPCContext*)ud;
    if (!c || !c->mem || ea >= RECOMP_GUEST_MEM) return M2M_ERR_FAULT;
    return (int32_t)c->mem[ea];
}

int32_t m2m_copy_guest_string(void* ud, m2m_read8_fn rd, uint32_t ea,
                              char* out, uint32_t cap) {
    if (!rd || !out || cap == 0) return M2M_ERR_RANGE;
    // A string of length L needs L+1 observed bytes (chars + terminator), so
    // an exact fit requires up to cap reads. The scan touches exactly
    // ea .. ea + cap - 1 and never reads past the bound.
    for (uint32_t i = 0; i < cap; ++i) {
        int32_t b = rd(ud, ea + i);
        if (b < 0) { out[0] = '\0'; return M2M_ERR_FAULT; }
        if (b == 0) { out[i] = '\0'; return (int32_t)i; }   // len excl. NUL
        out[i] = (char)b;
    }
    out[cap - 1] = '\0';
    return M2M_ERR_RANGE;   // no terminator within the bound
}

// ---- export table ----------------------------------------------------------------
// Minimum demanded by the audio boot chain (docs/game/audio.md): the
// name baked after FN_OpenAudioFolio__73de8 resolves to the audio folio.
// The vendor loader's FindImportIndex uses ASCII strcasecmp, so the model
// folds only A-Z here rather than inheriting the host locale. Anything else —
// including the empty string — is deterministically M2M_ERR_NAME.
typedef struct {
    const char* name;
    uint32_t    folio;
} m2m_export;

static const m2m_export M2M_EXPORTS[] = {
    { "audio", M2_FOLIO_AUDIO },
};
#define M2M_EXPORT_COUNT (sizeof(M2M_EXPORTS) / sizeof(M2M_EXPORTS[0]))

static int m2m_ascii_case_equal(const char* a, const char* b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= (unsigned char)'A' && ca <= (unsigned char)'Z')
            ca = (unsigned char)(ca + ((unsigned char)'a' - (unsigned char)'A'));
        if (cb >= (unsigned char)'A' && cb <= (unsigned char)'Z')
            cb = (unsigned char)(cb + ((unsigned char)'a' - (unsigned char)'A'));
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

static const m2m_export* m2m_lookup(const char* name) {
    for (uint32_t i = 0; i < M2M_EXPORT_COUNT; ++i) {
        if (m2m_ascii_case_equal(name, M2M_EXPORTS[i].name))
            return &M2M_EXPORTS[i];
    }
    return NULL;
}

// ---- kernel selector 103 -------------------------------------------------------
m2m_item_t m2m_import_by_name(m2m_context* ctx, m2m_item_t module,
                              void* rd_ud, m2m_read8_fn rd, uint32_t name_ea) {
    if (!ctx) return m2m_err_item(M2M_ERR_BAD_ITEM);
    int32_t rc = m2m_resolve_module(ctx, module);
    if (rc != M2M_OK) return m2m_err_item(rc);

    char name[M2M_MAX_NAME + 1];
    rc = m2m_copy_guest_string(rd_ud, rd, name_ea, name, sizeof(name));
    if (rc < 0) return m2m_err_item(rc);

    const m2m_export* ex = m2m_lookup(name);
    if (!ex) return m2m_err_item(M2M_ERR_NAME);

    // Repeat-import identity: one slot per resolved folio, claimed once and
    // never regenerated or freed, so every re-import returns the same handle.
    for (uint32_t i = 0; i < M2M_MAX_IMPORTS; ++i) {
        if (ctx->imports[i].used && ctx->imports[i].folio == ex->folio)
            return m2m_make_handle(M2M_TYPE_FOLIO, i, ctx->imports[i].gen);
    }
    for (uint32_t i = 0; i < M2M_MAX_IMPORTS; ++i) {
        if (!ctx->imports[i].used) {
            ctx->imports[i].used = 1;
            ctx->imports[i].folio = ex->folio;
            ctx->imports[i].gen = ++ctx->gen_counter;
            return m2m_make_handle(M2M_TYPE_FOLIO, i, ctx->imports[i].gen);
        }
    }
    return m2m_err_item(M2M_ERR_NO_SLOT);
}

uint32_t m2m_import_folio(const m2m_context* ctx, m2m_item_t item) {
    if (!ctx) return 0;
    if (item == M2M_ITEM_NONE || m2m_handle_type(item) != M2M_TYPE_FOLIO) return 0;
    uint32_t slot = m2m_handle_slot(item);
    if (slot >= M2M_MAX_IMPORTS) return 0;
    const m2m_import* im = &ctx->imports[slot];
    // Full {used, generation} liveness check, mirroring the kernel slice: a
    // stale or foreign-context handle fails instead of aliasing this pool.
    if (!im->used || im->gen != m2m_handle_gen(item)) return 0;
    return im->folio;
}
