// iff_folio.h — IFF folio (id 2) vectors used by `driving/game`.
//
// 15 distinct vectors, 25 calls total — the game parses cel/UTF/font/scene
// containers through this folio. Prototypes PROVISIONAL vs M2 SDK 2.7
// (3DO Opera IFF heritage); opaque handles keep the host free to implement
// parsers natively later.
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m2_iff_parser m2_iff_parser_t;   // opaque
typedef struct m2_iff_context m2_iff_context_t; // opaque

typedef int32_t (*m2_iff_handler_fn)(m2_iff_context_t* ctx,
                                     uint32_t chunk_type,
                                     void* user);

// vector 24: CreateIFFParserVA — tagged variadic on OS; provisional 2-arg
// host form (stream handle + flags). Revisit when SDK headers land.
m2_iff_parser_t* m2_CreateIFFParserVA(void* stream, int32_t flags);
m2_err_t m2_DeleteIFFParser(m2_iff_parser_t* p);            // vector 1
m2_err_t m2_ParseIFF(m2_iff_parser_t* p, int32_t mode);     // vector 2
int32_t m2_ReadChunk(m2_iff_parser_t* p, void* buf, uint32_t size); // vector 3
void m2_RegisterPropChunks(m2_iff_parser_t* p);             // vector 7
void* m2_FindPropChunk(m2_iff_parser_t* p, uint32_t type);  // vector 10 (sig PROVISIONAL)
m2_iff_context_t* m2_GetCurrentContext(m2_iff_parser_t* p); // vector 13
void* m2_AllocContextInfo(m2_iff_parser_t* p, uint32_t a, uint32_t b); // vector 15
void m2_FreeContextInfo(m2_iff_parser_t* p, void* ci);      // vector 16
void* m2_FindContextInfo(m2_iff_parser_t* p, uint32_t a);   // vector 17
m2_err_t m2_StoreContextInfo(m2_iff_parser_t* p, void* ci); // vector 18
void m2_InstallEntryHandler(m2_iff_parser_t* p, uint32_t type, m2_iff_handler_fn fn, void* user); // vector 20
void m2_InstallExitHandler(m2_iff_parser_t* p, uint32_t type, m2_iff_handler_fn fn, void* user);  // vector 21
m2_err_t m2_SeekChunk(m2_iff_parser_t* p, int32_t offset, int32_t whence); // vector 22
int32_t m2_GetIFFOffset(m2_iff_parser_t* p);                // vector 23

#ifdef __cplusplus
}  // extern "C"
#endif
