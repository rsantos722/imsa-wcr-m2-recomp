// small_folio_shims.cpp — iff (2), graphics (3) and audiopatch (5) shims.
#include <cstdio>
#include <cstdlib>

#include "audio_folio.h"
#include "audiopatch_folio.h"
#include "filesystem_folio.h"
#include "graphics_folio.h"
#include "iff_folio.h"
#include "folio_loader.h"

// --- iff ----------------------------------------------------------------------
namespace {
int g_iff_log = 0;
void iff_trace(const char* fn) {
    if (++g_iff_log <= 16 || (g_iff_log % 256) == 0)
        std::printf("[iff] %s (#%d)\n", fn, g_iff_log);
}
}  // namespace

extern "C" {

m2_iff_parser_t* m2_CreateIFFParserVA(void* stream, int32_t flags) {
    (void)flags;
    std::printf("[iff] CreateIFFParserVA(stream=%p)\n", stream);
    return reinterpret_cast<m2_iff_parser_t*>(stream);
}
m2_err_t m2_DeleteIFFParser(m2_iff_parser_t* p) { (void)p; return M2_OK; }
m2_err_t m2_ParseIFF(m2_iff_parser_t* p, int32_t mode) {
    (void)p;
    (void)mode;
    return -1;
}
int32_t m2_ReadChunk(m2_iff_parser_t*, void*, uint32_t) { return 0; }
void m2_RegisterPropChunks(m2_iff_parser_t*) { iff_trace("RegisterPropChunks"); }
void* m2_FindPropChunk(m2_iff_parser_t*, uint32_t type) {
    iff_trace("FindPropChunk");
    return nullptr;
}
m2_iff_context_t* m2_GetCurrentContext(m2_iff_parser_t*) { return nullptr; }
void* m2_AllocContextInfo(m2_iff_parser_t* p, uint32_t a, uint32_t b) {
    (void)p;
    return std::malloc(a * b > 0 ? a + b : 1);
}
void m2_FreeContextInfo(m2_iff_parser_t*, void* ci) { std::free(ci); }
void* m2_FindContextInfo(m2_iff_parser_t*, uint32_t) { return nullptr; }
m2_err_t m2_StoreContextInfo(m2_iff_parser_t*, void*) { return M2_OK; }
void m2_InstallEntryHandler(m2_iff_parser_t*, uint32_t, m2_iff_handler_fn, void*) {}
void m2_InstallExitHandler(m2_iff_parser_t*, uint32_t, m2_iff_handler_fn, void*) {}
m2_err_t m2_SeekChunk(m2_iff_parser_t*, int32_t, int32_t) { return -1; }
int32_t m2_GetIFFOffset(m2_iff_parser_t*) { return 0; }

// --- graphics -----------------------------------------------------------------
m2_err_t m2_AddViewToViewList(m2_item_t view_item, m2_item_t view_list_item) {
    std::printf("[graphics] AddViewToViewList(view=%d, list=%d)\n",
                view_item, view_list_item);
    return M2_OK;
}
m2_err_t m2_RemoveView(m2_item_t view_item) {
    std::printf("[graphics] RemoveView(view=%d)\n", view_item);
    return M2_OK;
}

// --- audiopatch ---------------------------------------------------------------
m2_item_t m2_CreatePatchTemplate(const m2_tag_arg_t* tags) {
    std::printf("[audiopatch] CreatePatchTemplate(tags=%p)\n", (const void*)tags);
    return 0x50544331 /*'PTC1'*/;
}

}  // extern "C"

void iff_shim_register() {
    using namespace m2host;
    folio_register(M2_FOLIO_IFF, 1, reinterpret_cast<void*>(&m2_DeleteIFFParser));
    folio_register(M2_FOLIO_IFF, 2, reinterpret_cast<void*>(&m2_ParseIFF));
    folio_register(M2_FOLIO_IFF, 3, reinterpret_cast<void*>(&m2_ReadChunk));
    folio_register(M2_FOLIO_IFF, 7, reinterpret_cast<void*>(&m2_RegisterPropChunks));
    folio_register(M2_FOLIO_IFF, 10, reinterpret_cast<void*>(&m2_FindPropChunk));
    folio_register(M2_FOLIO_IFF, 13, reinterpret_cast<void*>(&m2_GetCurrentContext));
    folio_register(M2_FOLIO_IFF, 15, reinterpret_cast<void*>(&m2_AllocContextInfo));
    folio_register(M2_FOLIO_IFF, 16, reinterpret_cast<void*>(&m2_FreeContextInfo));
    folio_register(M2_FOLIO_IFF, 17, reinterpret_cast<void*>(&m2_FindContextInfo));
    folio_register(M2_FOLIO_IFF, 18, reinterpret_cast<void*>(&m2_StoreContextInfo));
    folio_register(M2_FOLIO_IFF, 20, reinterpret_cast<void*>(&m2_InstallEntryHandler));
    folio_register(M2_FOLIO_IFF, 21, reinterpret_cast<void*>(&m2_InstallExitHandler));
    folio_register(M2_FOLIO_IFF, 22, reinterpret_cast<void*>(&m2_SeekChunk));
    folio_register(M2_FOLIO_IFF, 23, reinterpret_cast<void*>(&m2_GetIFFOffset));
    folio_register(M2_FOLIO_IFF, 24, reinterpret_cast<void*>(&m2_CreateIFFParserVA));
}

void graphics_shim_register() {
    using namespace m2host;
    folio_register(M2_FOLIO_GRAPHICS, 3, reinterpret_cast<void*>(&m2_AddViewToViewList));
    folio_register(M2_FOLIO_GRAPHICS, 5, reinterpret_cast<void*>(&m2_RemoveView));
}

void audiopatch_shim_register() {
    using namespace m2host;
    folio_register(M2_FOLIO_AUDIOPATCH, 0,
                   reinterpret_cast<void*>(&m2_CreatePatchTemplate));
}
