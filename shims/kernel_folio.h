// kernel_folio.h — kernel folio (id 1) vectors used by `driving/game`.
//
// Ordinals come from the selector ABI manifest (authoritative). This header
// is the raw-pointer facade for folio-loader consumers. The production static
// recompiler uses the fixed-width guest-EA/handle
// adapters in runtime/include/recomp_helpers.h; these declarations are
// not the selector-contract authority for that path.
//
// NOTE: libc-flavoured vectors (memset/memmove/printf/sprintf/strcpy/...)
// intentionally use `m2_`-prefixed names so they cannot collide with the
// host's <cstring>/<cstdio>; the folio loader registers host implementations
// (or thin forwarders) under their vector numbers.
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- memory -----------------------------------------------------------------
void* m2_AllocMem(int32_t mem_type, uint32_t size);          // vector 2
void* m2_AllocMemMasked(uint32_t mask, int32_t t, uint32_t n); // vector 35
void m2_FreeMem(void* p);                                    // vector 3

// --- program exit -----------------------------------------------------------
// kernel:8 `exit` x43. Named m2_kernel_exit to avoid ::exit collision.
// Task-local: terminates the calling guest task (or records the request when
// no task guard is active); never terminates the host process.
void m2_kernel_exit(int32_t code);                           // vector 8

// --- libc flavour -----------------------------------------------------------
void* m2_memmove(void* dst, const void* src, uint32_t n);    // vector 31
void* m2_memset(void* dst, int32_t c, uint32_t n);           // vector 33
uint32_t m2_strlen(const char* s);                           // vector 312
char* m2_strcpy(char* dst, const char* src);                 // vector 315
char* m2_strcat(char* dst, const char* src);                 // vector 317
int32_t m2_strcmp(const char* a, const char* b);             // vector 320
int32_t m2_printf(const char* fmt, ...);                     // vector 309
int32_t m2_sprintf(char* buf, const char* fmt, ...);         // vector 324
void m2_PrintError(const char* who, const char* what,
                   const char* whom, int32_t err);            // vector 310 clib_PrintError
void m2_PrintfSysErr(int32_t err);                            // vector 311

// --- items / lists / nodes --------------------------------------------------
m2_item_t m2_LookupItem(int32_t type, const char* name);     // vector 34
m2_item_t m2_FindItem(m2_item_t parent, const char* name);   // vector 163 (sig PROVISIONAL)
void m2_PrepList(m2_list_t* l);                              // vector 37
void m2_AddHead(m2_list_t* l, m2_node_t* n);                 // vector 44
void m2_AddTail(m2_list_t* l, m2_node_t* n);                 // vector 42
m2_node_t* m2_RemHead(m2_list_t* l);                         // vector 45
m2_node_t* m2_RemTail(m2_list_t* l);                         // vector 43
void m2_RemNode(m2_node_t* n);                               // vector 40

// --- signals / semaphores (thin wrappers over host primitives) ---------------
m2_signal_t m2_AllocSignal(int32_t bit_num);                 // vector 144
void m2_FreeSignal(m2_signal_t sig);                         // vector 143
int32_t m2_SendSignal(m2_item_t task, m2_signal_t sig);      // vector 165
m2_signal_t m2_WaitSignal(m2_signal_t mask);                 // vector 166
void m2_LockSemaphore(m2_item_t sem);                        // vector 160
void m2_UnlockSemaphore(m2_item_t sem);                      // vector 161

// --- time -------------------------------------------------------------------
// Host-pointer forms of the source contracts. The production guest-EA
// adapters live in recomp_helpers.h; this facade writes/copies the 8-byte
// records without asserting an unrecovered M2 tick frequency.
void m2_SampleSystemTimeTT(void* out_ticks);                 // vector 21
void m2_ConvertTimerTicksToTimeVal(const void* ticks,
                                   void* out_timeval);       // vector 27

// --- cache / misc no-ops on host ---------------------------------------------
void m2_FlushDCacheAll(void);                                // vector 136
m2_err_t m2_DeleteItem(m2_item_t item);                      // vector 164

#ifdef __cplusplus
}  // extern "C"
#endif
