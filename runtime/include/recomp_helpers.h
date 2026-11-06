#pragma once
#include "control_modes.h"
// recomp_helpers.h — thin wrapper for generated code.
// ppc_ctx.h already provides the full PPC helpers (CR, XER, FMA etc).
// This header adds only the recomp-specific stubs that generated bl sites use.

#include "ppc_ctx.h"
#include "shim_abi.h"
#include "audio_dispatch.h"
#include "selector_observed.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Unsupported / indirect stubs (implemented in src/recomp_runtime.c)
void RECOMP_UNSUPPORTED(PPCContext* c, uint32_t pc, const char* what);
void RECOMP_INDIRECT(PPCContext* c);
// Explicit-target form: resolves `target` (not c->ctr). Generated blrl
// sites pass their saved lr target; bctr/switch sites pass c->ctr.
void RECOMP_INDIRECT_FROM(PPCContext* c, uint32_t target);

// Dispatch a source-audited guest tail transfer without growing the native C
// stack for each subsequent tail hop.  The caller remains the active C frame,
// so a normal return from the tail chain still returns to that caller.
void recomp_tail_call(PPCContext* c, uint32_t target_guest);
// Checkpoint continuation bridge.  The generated resume-capable functions
// call m2_guest_resume_target() at their entry; it returns a nonzero saved
// in-function guest PC only for the host task continuation currently being
// reattached.  Normal guest calls never arm this TLS boundary.
void m2_guest_resume_begin(PPCContext* c, uint32_t function_ea);
uint32_t m2_guest_resume_target(PPCContext* c, uint32_t function_ea);
// Demand-driven TE consumer (implemented in src/recomp_runtime.c).
void m2_guest_te_satisfy_list(PPCContext* c, uint32_t list_ea);

// Optional presentation/testing aid.  When M2_PLAYER_AI=1, the player-car
// controller decision in complexSim uses the original guest AI path.  The
// guest-owned selector byte itself is never changed, so race progression and
// rolling-start state transitions continue to observe authentic guest state.
uint32_t m2_player_ai_controller_selector(uint32_t guest_selector);

// Optional presentation enhancement.  When M2_PLAYER_CAR_VARIANT is exactly
// 1 or 2, the player car's transient MS_buildcar/MS_rebuildcar appearance
// argument becomes that value, provided the loaded car pack authored the
// variant.  The caller passes the guest variant byte and the two guest
// car-body table entries the override would select, so a pack without the
// variant (the Skid Pad special pack authors only variant 0) falls back to
// the guest's own choice.  Physics, opponent selection and race rules keep
// using the guest's body index.
uint32_t m2_player_car_variant(uint32_t guest_variant, uint32_t entry_variant1,
                               uint32_t entry_variant2);

// Optional presentation enhancement.  When M2_DISABLE_CAR_LOD is exactly "1",
// MS_ScanList's transient distance-based car LOD index stays at 0 so
// MS_addgroup selects each car's highest-detail authored group at every
// distance.  Any other value preserves the guest's original LOD bands;
// transforms, culling, shadows, simulation and race rules keep using the
// guest's values.
uint32_t m2_car_lod_level(uint32_t guest_level);
void m2_car_lod_state_reset(void);

// Optional audio presentation fix.  When M2_SKIDPAD_ENGINE_FIX is exactly
// "1", the skidpad change-car apply rebinds the two engine-sound channels
// from the already-built per-engine sample structures when the player's
// engine index changes in-session, so the audible Up/Down pair follows the
// newly selected engine.  The guest simulation, car model, opponents, race
// rules and timing are untouched; only the audio binding is refreshed.
// Any other value preserves the original stale entry-time binding.
int m2_skidpad_engine_fix_enabled(void);
void m2_skidpad_engine_rebind(PPCContext *c);

// Optional enhancements: restore unfinished credits. When M2_RESTORE_CREDITS
// is exactly "1", the main-menu Credits option routes to the restored
// scrolling credits routine instead of placeholder video playback. Any other
// value preserves authentic original behavior (credits.mov video playback).
int m2_restore_credits_enabled(void);
int m2_credits_state_step(PPCContext *c);
void m2_credits_state_reset(void);
void m2_credits_text_blend_rearm(PPCContext *c);

// Optional enhancements: restore the unused post-race summary screen
// (UI_DrawBestTimes, ui.c 0x379cc). When M2_RESTORE_BEST_TIMES is exactly
// "1", a race result that reaches the post-race high-score states (StateDriver
// secondary 31/32) first shows the summary until a new button press. Any
// other value preserves the original results flow.
int m2_restore_best_times_enabled(void);
void m2_best_times_arm(PPCContext *c);
void m2_best_times_background_done(PPCContext *c);
void m2_best_times_dnf_fill(PPCContext *c);
int m2_best_times_state_step(PPCContext *c);
void m2_best_times_state_reset(void);

// Optional enhancements: restore the unused drafting effect (HandleDrafting,
// ai.c 0x6d44). When M2_RESTORE_DRAFTING is exactly "1", CarSimFunction zeroes
// the player's Vwind each frame and, with at least one opponent, runs
// HandleDrafting before the player record goes to the sim. Any other value
// preserves the original physics.
int m2_restore_drafting_enabled(void);
void m2_drafting_step(PPCContext *c);
void m2_drafting_state_reset(void);

// Optional enhancements: unlock the secret track (activatesecrettrack,
// state.c 0x2e578). When M2_UNLOCK_SECRET_TRACK is exactly "1", doButtonMenu
// runs activatesecrettrack (setting configraceNumSub[0] at 0x17d968 to 6) so
// track 5 (Psychedelia / leary) is selectable in Choose Track without driving
// into the Skid Pad trigger volume. Any other value preserves the default 5.
int m2_unlock_secret_track_enabled(void);
void m2_unlock_secret_track_step(PPCContext *c);
void m2_unlock_secret_track_state_reset(void);

// Default-on fix: rescale the exterior-camera HUD tachometer (game.c
// DrawInfoBar, 0x1608c) from the shipped fixed 0.015 (180 units / 12000 RPM)
// to the player engine's redline, so the 8500 RPM engines reach the gauge's
// yellow zone like the 11500 RPM Ferrari. Unset or any value but "0" applies
// the per-engine scale; "0" keeps the original fixed scale. Presentation only.
int m2_hud_tach_fix_enabled(void);
void m2_hud_tach_scale_step(PPCContext *c);
void m2_hud_tach_fix_state_reset(void);

// Default-on fix: bound repRestorePhantom's disc read (replay2.c 0x27250) to
// its 0x11978-byte buffer. The shipped Laguna phantom file is 1,354,716 bytes
// and the original read of fi_ByteCount bytes overruns the heap. Unset or any
// value but "0" bounds the read; "0" preserves the original overrun.
int m2_laguna_phantom_fix_enabled(void);
void m2_phantom_restore_read_step(PPCContext *c);
void m2_laguna_phantom_fix_state_reset(void);
// Presentation-only scope around UI_ShowMerc; restore the shared race context
// after drawing with the menu's original clipping width.
uint32_t m2_menu_preview_clip_begin(PPCContext *c, uint32_t close);
void m2_menu_preview_clip_end(PPCContext *c, uint32_t close, uint32_t saved);

// ---- typed kernel selector dispatch registry ----------------------------
// Typed handler slots for the four hot kernel selectors, keyed by ordinal
// (never by name). Registration is optional: an empty registry leaves every
// call site on the generic lookup path below, so host behavior is unchanged
// until an owner installs handlers.

// Kernel-folio ordinals (see shims/abi/selector-names.json).
enum {
    M2_KSEL_ALLOC_MEM          = 2,    // AllocMem          (21 static call sites)
    M2_KSEL_REALLOC_MEM        = 5,    // ReallocMem(old, oldSize, newSize, flags)
    M2_KSEL_FREE_MEM           = 3,    // FreeMem           (56 static call sites)
    M2_KSEL_EXIT               = 8,    // exit              (43 static call sites; task-local terminate)
    M2_KSEL_MEMMOVE            = 31,   // memmove           (76 static call sites)
    M2_KSEL_MEMSET             = 33,   // memset            (113 static call sites)
    M2_KSEL_ALLOC_MEM_MASKED   = 35,   // AllocMemMasked    (13 static call sites)
    M2_KSEL_ALLOC_MEM_DEBUG    = 57,   // AllocMemDebug     (2 static kernel sites; reached via mallocDebug + parse_utf_file)
    M2_KSEL_CHECK_IO           = 4,    // CheckIO           (6 static call sites)
    M2_KSEL_GET_CACHE_INFO     = 38,   // GetCacheInfo      (4 sites: GS_Create 0x1b45c, M_Init 0x5f62c etc., BLK001, CacheInfo 28 bytes)
    M2_KSEL_CREATE_ITEM_VA     = 80,   // CreateItemVA      (12 sites: Knob, Instrument etc., TagArg VA, BLK002 next)
    M2_KSEL_CREATE_ITEM         = 119, // CreateItem        (3 sites: VStreamInit 0x3cda8, 0x70a50, 0x73980; r3=type, r4=TagArg list EA)
    // Context-aware Diab tagged-variadic trampolines. These are handled
    // directly by recomp_call_kernel_dispatch, not stored in the ordinary
    // m2_kernel_dispatch function-pointer registry.
    M2_KSEL_TAG_CALL_0         = 89,   // __tagCall_0 (no fixed args)
    M2_KSEL_TAG_CALL_1         = 90,   // __tagCall_1 (one fixed arg)
    M2_KSEL_TAG_CALL_2         = 91,   // __tagCall_2 (two fixed args)
    M2_KSEL_SCAVENGE_MEM       = 17,   // ScavengeMem       (7 sites, all StateDriver)
    M2_KSEL_LOOKUP_ITEM        = 34,   // LookupItem        (broker + StateDriver windows)
    M2_KSEL_CREATE_TASK_VA     = 83,   // CreateTaskVA      (module, name, TagArg VA)
    M2_KSEL_CREATE_THREAD_VA   = 84,   // CreateThreadVA    (1 static call site)
    M2_KSEL_OPEN_MODULE        = 268,  // OpenModule        (path, type, tags)
    M2_KSEL_CLOSE_MODULE       = 269,  // CloseModule       (module Item)
    M2_KSEL_IMPORT_BY_NAME     = 103,  // ImportByName      (4 static call sites)
    M2_KSEL_WAIT_IO            = 129,  // WaitIO            (17 static call sites)
    M2_KSEL_WAIT_PORT          = 130,  // WaitPort          (8 static call sites)
    M2_KSEL_DO_IO              = 132,  // DoIO              (10 static call sites)
    M2_KSEL_ABORT_IO           = 141,  // AbortIO           (8 static call sites)
    M2_KSEL_SEND_IO            = 142,  // SendIO            (19 static call sites)
    M2_KSEL_FREE_SIGNAL        = 143,  // FreeSignal        (16 static call sites)
    M2_KSEL_ALLOC_SIGNAL       = 144,  // AllocSignal       (21 static call sites)
    M2_KSEL_GET_MSG            = 146,  // GetMsg            (27 static call sites)
    M2_KSEL_REPLY_MSG          = 147,  // ReplyMsg          (12 static call sites)
    M2_KSEL_SEND_MSG           = 150,  // SendMsg           (broker window)
    M2_KSEL_SEND_SMALL_MSG     = 151,  // SendSmallMsg      (1 static call site)
    M2_KSEL_GET_THIS_MSG       = 152,  // GetThisMsg        (1 static call site)
    M2_KSEL_FIND_ITEM          = 163,  // FindItem          (2 sites in StateDriver)
    M2_KSEL_SEND_SIGNAL        = 165,  // SendSignal        (15 static call sites)
    M2_KSEL_WAIT_SIGNAL        = 166,  // WaitSignal        (33 static call sites)
    M2_KSEL_CREATE_BUFFERED_MSG = 168, // CreateBufferedMsg (broker window)
    M2_KSEL_CREATE_IOREQ       = 170,  // CreateIOReq       (17 static call sites)
    M2_KSEL_CREATE_MSG         = 171,  // CreateMsg         (4 static call sites)
    M2_KSEL_CREATE_MSGPORT     = 172,  // CreateMsgPort     (sym 0x100ac)
    M2_KSEL_LOCK_SEMAPHORE     = 160,  // LockSemaphore   ( semaphore P)
    M2_KSEL_UNLOCK_SEMAPHORE   = 161,  // UnlockSemaphore ( semaphore V)
    M2_KSEL_CREATE_SEMAPHORE   = 174,  // CreateSemaphore (4 static call sites)
    M2_KSEL_CREATE_SMALL_MSG   = 175,  // CreateSmallMsg    (1 static call site)
    M2_KSEL_FIND_NAMED_ITEM    = 176,  // FindNamedItem     (broker window)
    M2_KSEL_FIND_CURRENT_MODULE = 262, // FindCurrentModule (8 static call sites)
    M2_KSEL_PRINTF             = 309,  // printf            (475 static call sites)
    M2_KSEL_ITOF               = 325,  // __itof            (201 static call sites)
    M2_KSEL_UTOF               = 326,  // __utof            (55 static call sites)
    M2_KSEL_VA_ARG             = 267,  // __va_arg          (15 static call sites: CreateDeviceStackListVA x14, __vfscanf x1)
    M2_KSEL_CREATE_THREAD      = 555,  // CreateThread      (6 static call sites)
    M2_KSEL_CREATE_TASK        = 553,  // CreateTask        (module, name, TagArg list)
    M2_KSEL_GET_TE_READ_POINTER  = 558, // GetTEReadPointer  (7 static call sites)
    M2_KSEL_SET_TE_WRITE_POINTER = 561, // SetTEWritePointer (5 static call sites)
    M2_KSEL_CREATE_SPINLOCK    = 205,  // CreateSpinLock    (2 sites, all M_InitMP)
    M2_KSEL_DELETE_SPINLOCK    = 206,  // DeleteSpinLock    (4 sites: M_InitMP x2, M_EndMP x2)
    M2_KSEL_CREATE_DEVICE_STACK_LIST = 67, // CreateDeviceStackList (1 site: Matrix_Perspective.c:CreateDeviceStackListVA 0x73708)
    M2_KSEL_DELETE_DEVICE_STACK_LIST = 68, // DeleteDeviceStackList (8 sites: CreateDeviceStackListVA 0x73894/0x738b4, CreateMPIOReq 0x73a70/0x73a90, OpenDeviceStack 0x73f80, MPS_InitMPEGStill 0x20284, FMVOpenVideo 0x65290/0x652b0)
    M2_KSEL_OPEN_DEVICE_STACK = 69, // OpenDeviceStack (5 sites: MPS_InitMPEGStill 0x20278, FMVOpenVideo 0x652a0, CreateMPIOReq 0x738a8/0x73a84, OpenTEDevice 0x73f68)
    M2_KSEL_CLOSE_DEVICE_STACK = 70, // CloseDeviceStack (7 sites: gstate.c:GS_Delete 0x1cba0, mps.c:MPS_TeardownMPEGStill 0x20a58, Matrix_Perspective.c:FMVCloseDevice 0x651b0, CreateMPIOReq 0x738e8, CreateTimerIOReq 0x73ac4, DeleteMPIOReq 0x73c08, DeleteTimerIOReq 0x73c7c)
    M2_KSEL_FLUSH_DCACHE       = 51,   // FlushDCache      (cache maintenance; host-coherent no-op)
    M2_KSEL_WRITE_BACK_DCACHE  = 52,   // WriteBackDCache  (cache maintenance; host-coherent no-op)
    M2_KSEL_FREE_MEM_DEBUG     = 59,   // FreeMemDebug     (r3=EA, r4=size, r5/r6=debug metadata)
    M2_KSEL_STRNCPY            = 316,  // strncpy          (bounded guest-RAM copy; r3=dst,r4=src,r5=n)
    M2_KSEL_TOUPPER            = 322,  // toupper          (ASCII upcase; r3=char -> r3)
    M2_KSEL_OBTAIN_SPINLOCK    = 566,  // ObtainSpinLock   (M_InitMP acquire; no-op on ST recomp)
    M2_KSEL_RELEASE_SPINLOCK   = 567,  // ReleaseSpinLock  (M_InitMP release; no-op on ST recomp)
    M2_KSEL_OPEN_ITEM           = 162,  // OpenItem        (item lifecycle; returns item handle)
    M2_KSEL_CLOSE_ITEM          = 159,  // CloseItem       (item lifecycle; validates, does not free)
    M2_KSEL_DELETE_ITEM         = 164,  // DeleteItem      (item lifecycle; generation-safe free)
    M2_KSEL_CHECK_ITEM          = 29,   // CheckItem       (Item,type,type -> Node EA/NULL)
    M2_KSEL_FIND_NAMED_NODE     = 36,   // FindNamedNode   (list walk by name; returns node EA or 0)
    M2_KSEL_UNIVERSAL_INSERT_NODE = 28, // UniversalInsertNode (callback-sorted list insert)
    M2_KSEL_FIND_TAG_ARG        = 1,    // FindTagArg      (tag-list walk; returns TagArg EA or 0)
    M2_KSEL_GET_MEM_INFO        = 16,   // GetMemInfo      (fills 28-byte MemInfo struct)
    M2_KSEL_UNIMPORT_BY_NAME    = 100,  // UnimportByName  (loader verdict: 0 for an image import name, negative otherwise)
    M2_KSEL_DEBUG_PUT_STR       = 118,  // DebugPutStr     (debug console; guest NUL-str -> stderr)
    M2_KSEL_DEBUG_PUT_CHAR      = 120,  // DebugPutChar    (debug console; one char -> stderr)
    M2_KSEL_VCPRINTF            = 24,   // vcprintf        (debug console; fmt + guest va_list -> stderr)
    M2_KSEL_STRLEN              = 312,  // strlen          (guest C string length)
    M2_KSEL_STRCPY              = 315,  // strcpy          (guest C string copy)
    M2_KSEL_STRCAT              = 317,  // strcat          (guest C string append)
    M2_KSEL_STRCMP              = 320,  // strcmp          (guest C string compare)
    // --- promoted ords (call shape proven from generated-lift windows) ---
    M2_KSEL_SAMPLE_SYSTEM_TIME_TT = 21, // SampleSystemTimeTT (writes 8-byte TimerTicks to out ptr)
    M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL = 27, // ConvertTimerTicksToTimeVal
    M2_KSEL_PREP_LIST         = 37,   // PrepList        (init 16-byte List anchor in place)
    M2_KSEL_REM_NODE          = 40,   // RemNode         (unlink node r3 from its list)
    M2_KSEL_ADD_TAIL          = 42,   // AddTail         (List* r3, Node* r4)
    M2_KSEL_REM_TAIL          = 43,   // RemTail         (List* r3 -> Node* or NULL)
    M2_KSEL_ADD_HEAD          = 44,   // AddHead         (List* r3, Node* r4)
    M2_KSEL_REM_HEAD          = 45,   // RemHead         (List* r3 -> Node* or NULL)
    M2_KSEL_FLUSH_DCACHE_ALL  = 136,  // FlushDCacheAll  (whole-D-cache no-op)
    M2_KSEL_CLIB_PRINT_ERROR   = 310,  // clib_PrintError (level r3, category r4, fmt r5, val r6)
    M2_KSEL_PRINTF_SYS_ERR     = 311,  // PrintfSysErr    (err code r3 only)
    M2_KSEL_SPRINTF           = 324,  // sprintf         (buf r3, fmt r4, varargs r5..r10)
    M2_KSEL_IS_MASTER_CPU     = 105,  // IsMasterCPU     (no args, pure SR14-id predicate -> 1 master at the modeled id; 4 sites: GS_SendIO 0x1c000, GS_SendList 0x1c494, GS_LowLatency 0x1c8d0, M_ClistManagerC 0x5f590; all cmpwi r3,0, no guest-RAM traffic)
    // ---- folio services named by the original shooter `main` -------------
    // kernel:48  SubTimerTicks(tt1, tt2, result) — vendor timer.s saturating
    //   64-bit subtract (tt2 - tt1), zero on the source's negative result.
    M2_KSEL_SUB_TIMER_TICKS = 48,
    // kernel:149 ReadHardwareRandomNumber(void) — 32-bit hardware entropy word
    //   whose only consumer is the guest's srand (main@0x364 -> srand@0x368).
    M2_KSEL_READ_HARDWARE_RANDOM_NUMBER = 149,
    // kernel:157 SetItemPri(Item, uint8 newpri) — vendor item.c/internalChangeTaskPri
    //   re-prioritization; the shooter boosts the current task by 0x14 around
    //   WaitSignal/GS_SendList (main@0x73c) and restores it (main@0x758).
    M2_KSEL_SET_ITEM_PRI = 157,
    // kernel:133 FindAndOpenItem(int32 cType, TagArg *tp) — vendor
    //   internalFindAndOpenItem (folios/kernel/item.c:2062): find an item of
    //   cType whose tags match, otherwise load one from the folio that claims
    //   cType, and return it already opened. shooter2d calls it once from
    //   textstuff.c inittextstuff@0xfb78 through FindAndOpenNamedItem@0x121a4
    //   with cType 0x601 (MKNODEID(NST_FONT, FONT_FONT_NODE)) and the tag list
    //   {TAG_ITEM_NAME, "Helvetica.24pt"}; the returned Item is what the
    //   font folio's CreateTextState then CheckItems.
    M2_KSEL_FIND_AND_OPEN_ITEM = 133,
};

// Pin the StateDriver-slice ordinals to the checked-in selector metadata
// (shims/abi/selector-names.json): a renumbered
// ordinal would silently retarget every generated call site.
// _Static_assert is C-only; GCC rejects it in the C++ host TUs.
#ifdef __cplusplus
#define M2_STATIC_ASSERT static_assert
#else
#define M2_STATIC_ASSERT _Static_assert
#endif
M2_STATIC_ASSERT(M2_KSEL_SCAVENGE_MEM == 17, "kernel:17 is ScavengeMem");
M2_STATIC_ASSERT(M2_KSEL_FIND_ITEM == 163, "kernel:163 is FindItem");

// Diab compiler prologue/epilogue register save/restore helpers (kernel
// vector family: _savegpr_/restgpr_/_savegprs_*_l/_restgprs_*_l/…, the fpr
// _savefprs_*_l/_restfprs_*_l groups). These are NOT OS services — Diab 3.7a
// emits them out-of-line for function prologue/epilogue to spill/restore
// callee-saved registers on the stack. On the recompiled model they are
// proven architectural no-ops: the generated C manages its own host register
// state and the translator elides these helpers (/* Diab prologue elided */,
// 430 instructions folded — see the recompilation limitations and
// shims/diab_runtime.h). A genuine no-op (leave guest registers and
// gpr[3] untouched) is therefore the faithful typed handling, distinct from
// the dummy-success generic scaffold.
static inline bool m2_is_diab_helper(uint32_t o) {
    switch (o) {
    case 352: case 353: case 354: case 355: case 356: case 357: case 358: case 359:
    case 381: case 384: case 385: case 386: case 387: case 388: case 389: case 390:
    case 391: case 392: case 393: case 394: case 417: case 418: case 419: case 421:
    case 422: case 424: case 425: case 426: case 427: case 428: case 429: case 430:
    case 460: case 461: case 462: case 463: case 464: case 465: case 466: case 467:
    case 489: case 492: case 493: case 494: case 495: case 496: case 497: case 498:
    case 499: case 500: case 501: case 502: case 525: case 526: case 527: case 529:
    case 530: case 532: case 533: case 534: case 535: case 536: case 537: case 538:
        return true;
    default: return false;
    }
}

// Generic kernel ordinals retained only as an explicitly classified
// discovery scaffold. They are not ABI implementations: callers must not
// treat their dummy results as recovered behavior, and traces/reporting keep
// these selectors visible until a typed contract is established.
//
// As of the IsMasterCPU#105 promotion the set is empty: 105 is now a typed
// predicate (M2_KSEL_IS_MASTER_CPU, portfolio cpu.s mfsr SR14; master==1).
// The 89/90/91 __tagCall_0/1/2 trampolines are handled by the context-aware
// typed path below, and the 352-538 Diab register save/restore family is
// handled by m2_is_diab_helper().
static inline bool m2_is_generic_kernel_stub(uint32_t o) {
    switch (o) {
    default: return false;
    }
}
static inline bool m2_is_tag_call_helper(uint32_t o) {
    switch (o) {
    case M2_KSEL_TAG_CALL_0:
    case M2_KSEL_TAG_CALL_1:
    case M2_KSEL_TAG_CALL_2:
        return true;
    default:
        return false;
    }
}
static inline bool m2_is_generic_audio_stub(uint32_t o) {
    switch (o) {
    default: return false;
    }
}
// Last generic item created — used so a subsequent FindItem can return the
// handle that CreateItemVA/CreateItem just minted (guest often does
// CreateItemVA → FindItem for the same name). Stored in recomp_runtime.c
// single instance (shared across TUs).
void m2_set_last_generic_item(uint32_t h);
uint32_t m2_get_last_generic_item(void);

// Discovery scaffold: returns conservative placeholder values for selectors
// not yet reached on the supported guest path. This function is intentionally
// not considered an ABI implementation; promotion requires a typed contract
// and focused tests.
static inline int m2_handle_generic_kernel(PPCContext* c, uint32_t o) {
    if (!c) return 0;
    uint32_t h = 0;
    // Throttled diagnostic for StateDriver (first 60 generic calls)
    {
        static int _gen_cnt = 0;
        if (_gen_cnt < 60) {
            fprintf(stderr, "[gen] kernel ord=%u gpr3=%#x gpr4=%#x\n", o, c->gpr[3], c->gpr[4]);
            _gen_cnt++;
            if (_gen_cnt == 60) fprintf(stderr, "[gen] (further generic kernel calls suppressed)\n");
        }
    }
    switch (o) {
    // Item creators — must be non-zero and remembered for FindItem
    // (ord 174 CreateSemaphore is typed; 80 CreateItemVA is typed and so is
    // handled by the typed dispatch, never reaching this scaffold).
    case 119: case 162: case 205:
        h = 0x6000u + o;
        c->gpr[3] = h;
        m2_set_last_generic_item(h);
        return 1;
    case 69: // OpenItem — return -1 (no hardware device opened in headless/sim)
        c->gpr[3] = (uint32_t)-1;
        return 1;
   // FindTagArg — not-found is 0
    case 1: c->gpr[3] = 0; return 1;
    // NOTE: IsMasterCPU#105 is served by the typed predicate
    // (M2_KSEL_IS_MASTER_CPU) and does not reach this scaffold.
    // String helpers — return dest pointer (r3)
    case 316: /* strncpy */ c->gpr[3] = c->gpr[3]; return 1;
    case 322: /* toupper */ c->gpr[3] = (uint32_t)toupper((int)c->gpr[3]); return 1;
    default:
        // Default success: many wait/list ops treat non-zero as success,
        // but zero also means "not found" for some. We use 1 as success.
        c->gpr[3] = 1; return 1;
    }
}
static inline int m2_handle_generic_audio(PPCContext* c, uint32_t o) {
    if (!c) return 0;
    (void)o;
    c->gpr[3] = 1; return 1;
}

// Handler prototypes match kernel_folio.h / diab_runtime.h declarations.
typedef void*   (*m2fn_ReallocMem_t)(void *old, int32_t old_size, int32_t new_size, uint32_t flags);
typedef void*   (*m2fn_AllocMem_t)(int32_t size, uint32_t mem_flags);
// FreeMem(void *p, int32 size): r3 is the guest EA and r4 is the byte count,
// or TRACKED_SIZE (-1) for an allocation made with MEMTYPE_TRACKSIZE.
typedef void    (*m2fn_FreeMem_t)(void* p, int32_t size);
// kernel:8 exit(status): terminates the calling task/thread, never returns
// (portfolio_os_m2 folios/kernel/taskexit.c: SetExitStatus(status); KillSelf()).
// The host model unwinds the current guest task to its m2k trampoline; status
// lands in the task's exit_status. Negative values remain system-reserved.
typedef void    (*m2fn_Exit_t)(uint32_t status);
// Guest printf: fmt is guest EA (uint32_t), varargs are guest words in r4..r10 + stack spill at r1+8.
// Handler must bounds-check fmt and any %s args via m2m_copy_guest_string_ctx (cap GK_MAX_NAME_LEN),
// format into a bounded host buffer (1024), and return chars printed. See kernel 309.
typedef int32_t (*m2fn_printf_t)(uint32_t fmt_ea, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7);
// Guest vcprintf (portfolio_os_m2 src/includes/stdio.h): r3=fmt EA,
// r4=OutputFunc callback EA, r5=callback user-data word, r6=guest va_list EA
// (Diab layout: +0 u8 gpr idx, +1 u8 fpr idx, +4 u32 overflow area, +8 u32
// reg_save area). Varargs are read on demand via gk_va_arg(va_ea, 1); the
// callback/user-data words remain guest values and never become host pointers.
typedef int32_t (*m2fn_vcprintf_t)(uint32_t fmt_ea, uint32_t output_func_ea,
                                   uint32_t user_data_ea, uint32_t va_ea);
typedef uint32_t (*m2fn_strlen_t)(uint32_t s);
typedef uint32_t (*m2fn_strcpy_t)(uint32_t dst, uint32_t src);
typedef uint32_t (*m2fn_strcat_t)(uint32_t dst, uint32_t src);
typedef int32_t  (*m2fn_strcmp_t)(uint32_t a, uint32_t b);
// kernel:105 IsMasterCPU — no args, pure predicate -> 1 (master) / 0 (slave).
// Pinned from all 4 generated windows (zero regs before bl, cmpwi r3,0 after,
// no guest-RAM traffic) + portfolio_os_m2 cpu.s (mfsr r3,SR14; master CPU ID 0
// => 1). Single-CPU recomp is always master. Both values boot to State 1;
// master==1 is the vendor-faithful concrete value.
typedef int32_t (*m2fn_IsMasterCPU_t)(void);

// kernel:105 source mapping (M2 SDK src/folios/kernel/cpu.s:IsMasterCPU):
//   mfsr r3,SR14    // CPU id code
//   cmpwi r3,0      // which CPU
//   bne- 0f         // not the master
//   li r3,1; blr    // on the master
// 0: li r3,0; blr   // on the slave
// SPR SR14 holds the CPU id, and the source answers the master branch only for
// id 0. The recomp is one host context that models the master CPU of the M2's
// dual-CPU pair, so the modeled SR14 reading is M2_MODEL_CPU_SR14 (0) and the
// production handler answers m2_cpu_sr14_is_master(M2_MODEL_CPU_SR14). Both
// source branches are therefore pinned against the source mapping (rather than
// an inline constant), and the modeled single-master configuration is an
// explicit value a test can assert. Presenting the slave configuration would
// require a second host CPU context; none exists, so the slave branch is
// asserted at this mapping and the boundary at the modeled id.
#define M2_MODEL_CPU_SR14 0u
static inline int32_t m2_cpu_sr14_is_master(uint32_t sr14) {
    return sr14 == 0u ? 1 : 0;
}
typedef float   (*m2fn_itof_t)(int32_t v);
// kernel:326 __utof: unsigned 32-bit -> float. Pinned from all 55 generated
// windows: gpr[3] in, fpr[1] out (op59 single-precision / stfs / fmr+fcmpu
// consumers, no frsp), unsigned-domain producers (divwu remainder, lbz, lwz,
// rlwinm, slw) vs the signed producers (divw/srawi/neg) at __itof#325 sites.
typedef float   (*m2fn_utof_t)(uint32_t v);
// kernel:267 __va_arg (Diab PPC varargs helper): r3=va_list EA (12-byte
// struct { u8 gpr; u8 fpr; u16 pad; u32 overflow_arg_area; u32
// reg_save_area }), r4=1 type-class constant at every one of the 15 sites.
// Returns the EA of the next 4-byte word slot — reg_save_area+gpr*4 while
// gpr<8, else overflow_arg_area — and advances the list by one word.
// Layout proven from the two consumers' inlined va_start
// (CreateDeviceStackListVA 0x73478; fscanf 0x74c20 -> __vfscanf 0x74cc4).
typedef uint32_t (*m2fn_VaArg_t)(uint32_t va_list_ea, uint32_t type_class);
// libc-flavoured memory services (kernel 33 memset / kernel 31 memmove).
// Standard C call shape: r3=dst guest EA, r4=fill byte (memset) or src guest
// EA (memmove), r5=byte count. Writes happen inside bounded guest RAM; both
// return the original dst (r3) per C/3DO semantics even though most generated
// sites discard the return and only the side-effect matters.
typedef void*   (*m2fn_memset_t)(void* dst, int32_t c, uint32_t n);
typedef void*   (*m2fn_memmove_t)(void* dst, const void* src, uint32_t n);
// AllocMemMasked (kernel 35): r3=minSize, r4=memFlags, r5=careBits,
// r6=stateBits. The recovered DSAllocMemAligned wrapper forwards the latter
// two values from the MPEG buffer contract, so they are guest-visible ABI
// arguments and must not be collapsed into the size/flags pair.
typedef void*   (*m2fn_AllocMemMasked_t)(int32_t min_size, uint32_t mem_flags,
                                         uint32_t care_bits,
                                         uint32_t state_bits);
// AllocMemDebug (kernel 57): both kernel sites pass (r3=size, r4=0x200,
// r5=source-tag string EA, r6=source line). mallocDebug (0x757d4) forwards its
// (size, tag, line) and injects r4=0x200; parse_utf_file 0x4f6f8 is the single
// direct site. r4/r5/r6 are debug metadata no lifted caller consumes.
typedef void*   (*m2fn_AllocMemDebug_t)(int32_t size, uint32_t flags,
                                        uint32_t tag_ea, uint32_t line);

// StateDriver window (state.c): ScavengeMem(arg) — the sole constant site
// (game_text_04.c:0x300f0) sets r3=1; the other six leave caller leftovers,
// so the operand is a recorded payload word. Every one of the seven sites
// overwrites r3 immediately after the call: the result is never consumed.
// FindItem(type_tag, name_ea) — both StateDriver sites pass (0x105, 0) and
// feed the result straight into LookupItem, then probe +0x2c of that result;
// name_ea=0 means no guest string operand exists to validate.
// GetCacheInfo(info_ea, info_size) — fills CacheInfo (28 bytes: Flags, ICacheSize, Line, Set, DCacheSize, Line, Set) at guest EA info_ea with bounded wr32 BE; info_size is 28 at all 4 sites (GS_Create 0x1b45c, M_Init 0x5f62c etc.). See kernel/cache.h and spinlock.c usage of cinfo_DCacheLineSize for AllocMemAligned.
// CreateItemVA(ctype, tag, ...) — variadic TagArg VA (12 sites, e.g. CreateKnob 0x73778: ctype 0x403 tag 0x11 etc., FMVCreateIOReq 0x650b8: ctype 0x10e tags 0xb/0xc). Tags are uint32 tag/value pairs terminated by TAG_END 0, passed in r4..r10 + 8(sp) spill. Handler copies bounded TagArg list with m2_tag_arg checks, validates string tags (TAG_ITEM_NAME etc.) via bounded guest string, mints distinct u32 handle, stores for FindItem bridge.
typedef int32_t (*m2fn_ScavengeMem_t)(uint32_t arg);
typedef uint32_t (*m2fn_FindItem_t)(uint32_t type_tag, uint32_t name_ea);
typedef void (*m2fn_GetCacheInfo_t)(uint32_t info_ea, uint32_t info_size);
typedef uint32_t (*m2fn_CreateItemVA_t)(uint32_t ctype, uint32_t tag1, uint32_t val1, uint32_t tag2, uint32_t val2, uint32_t tag3, uint32_t val3, uint32_t tag4);
// CreateItem(ctype, taglist_ea) — r3=item type, r4=guest EA of a TagArg list
// ({tag,value} u32 pairs, TAG_END=0 terminated) or 0 for no tags. 3 sites:
// VStreamInit 0x3cda8, 0x70a50, 0x73980. Handler copies the bounded list,
// validates string tags via bounded guest reads, mints a distinct u32 handle.
typedef uint32_t (*m2fn_CreateItem_t)(uint32_t ctype, uint32_t taglist_ea);

// Promoted handler prototypes (call shapes proven from generated-lift windows).
// 3DO Portfolio List: 16 bytes {l_Flags@0, l_Head@4, l_Filler@8,
// l_Tail@12}. The empty-list sentinels are l_Head == &l_Filler and
// l_Tail == &l_Head. Nodes carry a 2-word link {n_Next@0, n_Prev@4}; the
// standard Node name is at +0x10. All EAs are bounds-checked against guest
// RAM; invalid EAs fail closed.
typedef void (*m2fn_PrepList_t)(uint32_t list_ea);          // kernel:37  r3=List*
typedef void (*m2fn_RemNode_t)(uint32_t node_ea);           // kernel:40  r3=Node*
typedef void (*m2fn_AddTail_t)(uint32_t list_ea, uint32_t node_ea);   // kernel:42
typedef uint32_t (*m2fn_RemTail_t)(uint32_t list_ea);       // kernel:43 -> Node* or 0
typedef void (*m2fn_AddHead_t)(uint32_t list_ea, uint32_t node_ea);   // kernel:44
typedef uint32_t (*m2fn_RemHead_t)(uint32_t list_ea);       // kernel:45 -> Node* or 0
// SampleSystemTimeTT(TimerTicks* out r3) — writes 8-byte TimerTicks (BE); returns
// ignored. ConvertTimerTicksToTimeVal(TimerTicks* r3, TimeVal* r4) — copies the
// 8-byte value into the TimeVal {int32 seconds@0, int32 micros@4} (BE); returns
// ignored. FlushDCacheAll(void) — host-coherent whole-D-cache no-op.
typedef void (*m2fn_SampleSystemTimeTT_t)(uint32_t out_ea);
typedef void (*m2fn_ConvertTimerTicksToTimeVal_t)(uint32_t ticks_ea, uint32_t tv_ea);
typedef void (*m2fn_FlushDCacheAll_t)(void);                // kernel:136
// clib_PrintError(who r3, what r4, whom r5, err r6) — source ABI is fixed four
// arguments and returns void. PrintfSysErr(err r3) likewise returns void; no
// format string or variadic tail is part of either selector.
// sprintf(buf_ea r3, fmt_ea r4, varargs r5..r10 + stack/FPRs) — formats the guest
// format string at r4 into the guest RAM buffer at r3, returns the char count.
typedef void (*m2fn_clib_PrintError_t)(uint32_t who_ea, uint32_t what_ea,
                                        uint32_t whom_ea, int32_t err);
typedef void (*m2fn_PrintfSysErr_t)(int32_t err);
typedef int32_t (*m2fn_sprintf_t)(uint32_t buf_ea, uint32_t fmt_ea, uint32_t a1,
                                   uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                   uint32_t a6, uint32_t a7);
// SubTimerTicks(const TimerTicks* tt1 r3, const TimerTicks* tt2 r4,
//               TimerTicks* result r5) — 8-byte big-endian TimerTicks in and
// out. Vendor timer.s:185-197 computes the 64-bit `tt2 - tt1` with a borrowed
// low word and stores zero when the source's `bge` guard rejects the result.
// `result` may alias either operand (timer.s:161-163), so the handler reads
// both inputs before writing.
typedef void (*m2fn_SubTimerTicks_t)(uint32_t tt1_ea, uint32_t tt2_ea,
                                     uint32_t result_ea);
// ReadHardwareRandomNumber(void) — no arguments, one 32-bit word in r3.
typedef uint32_t (*m2fn_ReadHardwareRandomNumber_t)(void);
// SetItemPri(Item i r3, uint8 newpri r4) — previous priority (>= 0) or a
// negative Portfolio Err in r3.
typedef int32_t (*m2fn_SetItemPri_t)(uint32_t item, uint8_t newpri);
// FindAndOpenItem(cType, tags) — r3=int32 item type (MkNodeID), r4=guest EA of
// a TagArg list ({tag,value} pairs, TAG_END=0) or 0 for no tags; returns the
// Item, already opened, or a negative Portfolio Err.
typedef uint32_t (*m2fn_FindAndOpenItem_t)(int32_t ctype, uint32_t taglist_ea);

// ---- gate-family handler prototypes (EABI window marshalled by dispatch) --
// Item-bearing handlers exchange fixed-width uint32_t guest item handles only
// (m2k_item_t layout); no host pointer ever crosses into a guest register.

// Opera CreateMsgPort(void): no guest args, u32 item handle return. Context
// binding is host-side (the registrant closes over its m2k_context), so the
// slot stays free of kernel-header dependencies here.
typedef uint32_t (*m2fn_CreateMsgPort_t)(void);
// CheckIO(io): non-blocking completion probe. Callers branch on the raw word
// (generated windows: `cmpwi r3,0 / bne`), so the mapping is: completed-ok ->
// 1, completed-error -> negative status, still pending -> 0, invalid -> -1.
typedef int32_t  (*m2fn_CheckIO_t)(uint32_t io);
// CreateThread(code_ea, name_ea, pri, stack_size, tags_ea): the five words
// follow the Portfolio kernel/task.h prototype. code_ea is a guest EA resolved
// to host code at create time via recomp_lookup; the remaining words are
// recorded at the call boundary while the task body receives no argument.
typedef uint32_t (*m2fn_CreateThread_t)(uint32_t code_ea, uint32_t name_ea,
                                        uint32_t pri, uint32_t stack_size,
                                        uint32_t tags_ea);
// Static OpenModule/CloseModule/CreateTask bridge. OpenModule accepts only
// the bounded known image paths and source-documented NULL tags. CreateTask
// starts that image's main entry and reads only CREATETASK_TAG_ARGC/ARGP.
typedef uint32_t (*m2fn_OpenModule_t)(uint32_t path_ea, uint32_t type,
                                      uint32_t tags_ea);
typedef int32_t (*m2fn_CloseModule_t)(uint32_t module_item);
typedef uint32_t (*m2fn_CreateTask_t)(uint32_t module_item, uint32_t name_ea,
                                      uint32_t tags_ea);
typedef uint32_t (*m2fn_CreateTaskVA_t)(uint32_t module_item, uint32_t name_ea,
                                        uint32_t tag_a, uint32_t val_a,
                                        uint32_t tag_b, uint32_t val_b,
                                        uint32_t tag_c, uint32_t val_c);
// CreateThreadVA per the Portfolio prototype (portfolio_os_m2
// kernel/task.h): Item CreateThreadVA(void (*code)(), const char *name,
// uint8 pri, int32 stackSize, uint32 tag, ...). The sole generated caller,
// NewThread (guest 0x6ac14 -> selector site 0x6ac4c), shuffles its args into
// exactly this window before trapping: r3 passes through untouched and both
// sites pass lifted thread-body starts (FN_DataStreamThread__692a0 /
// FN_DataAcqThread__6a388), old-r6 lands in r4 (name), (old-r4+7)&~7 rounds
// the stack into r6, and tags 14=CREATETASK_TAG_ARGC / 15=CREATETASK_TAG_ARGP
// carry old-r7/old-r8 with a TAG_END word stored at 8(sp). code_ea resolves
// to host code at create time via recomp_lookup; name/tag operands stay
// recorded fixed-width payload words. Returns u32 TASK item handle.
typedef uint32_t (*m2fn_CreateThreadVA_t)(uint32_t code_ea, uint32_t name_ea,
                                          uint32_t pri, uint32_t stack_size,
                                          uint32_t tag_a, uint32_t val_a,
                                          uint32_t tag_b, uint32_t val_b);
// ImportByName(module_item, name_ea): bounded NUL-terminated guest string
// lookup in the deterministic module table; 0 when unresolved.
typedef uint32_t (*m2fn_ImportByName_t)(uint32_t module_item,
                                        uint32_t name_ea);
// FindCurrentModule(void): stable module item token for this slice.
typedef uint32_t (*m2fn_FindCurrentModule_t)(void);
// WaitIO(io): blocks until COMPLETE, returns stored status.
typedef int32_t  (*m2fn_WaitIO_t)(uint32_t io);
// WaitPort(port, aux): source contract (portfolio_os_m2 kernel/msgport.c)
// Item WaitPort(Item msgPort, uint32 msg) — returns the incoming message
// item and dequeues it; 0 when the port is empty or invalid (the guest
// ebthread loop treats that as "keep waiting"). The r4 word is marshalled
// as aux: when it names a broker record EA (the ConnectEventBroker
// adapter) the call is the status-only readiness wait so the broker consumer
// keeps reading the record fields directly.
typedef int32_t  (*m2fn_WaitPort_t)(uint32_t port, uint32_t aux);
// DoIO/SendIO(io, info_ea): both operands stay guest-visible values; the
// IOInfo struct layout is not established, so info_ea travels as the request
// payload word without dereference.
typedef int32_t  (*m2fn_DoIO_t)(uint32_t io, uint32_t info_ea);
typedef int32_t  (*m2fn_SendIO_t)(uint32_t io, uint32_t info_ea);
// AbortIO(io): truthful state answer; every generated caller discards r3.
typedef int32_t  (*m2fn_AbortIO_t)(uint32_t io);
// FreeSignal(signal_mask) / AllocSignal(signal_mask): Portfolio signal masks.
// AllocSignal(0) allocates one user bit and returns its mask; an explicit mask
// requests those bits and returns the allocated mask or zero on failure.
typedef void     (*m2fn_FreeSignal_t)(int32_t signal_mask);
typedef int32_t  (*m2fn_AllocSignal_t)(int32_t signal_mask);
// GetMsg(port): non-blocking dequeue; Opera NULL-item convention — 0 when the
// port is empty or invalid (generated callers test nonzero).
typedef uint32_t (*m2fn_GetMsg_t)(uint32_t port);
// ReplyMsg(msg): reply code not carried by any proven window; 0 applied.
typedef int32_t  (*m2fn_ReplyMsg_t)(uint32_t msg);
// SendSmallMsg(port, msg_item, value, aux): the sole generated pairing sends
// a CreateSmallMsg item down a port; aux (r6=0 at that site) is ignored.
typedef int32_t  (*m2fn_SendSmallMsg_t)(uint32_t port, uint32_t msg_item,
                                        uint32_t value, uint32_t aux);
// GetThisMsg(item): same dequeue semantics as GetMsg (sole generated caller
// discards the result).
typedef m2fn_GetMsg_t m2fn_GetThisMsg_t;
// SendSignal(task, signal_mask): deliver one or more Portfolio signal masks.
typedef int32_t  (*m2fn_SendSignal_t)(uint32_t task, uint32_t signal_mask);
// WaitSignal(mask): blocks until a masked bit pends, consumes+returns bits.
typedef int32_t  (*m2fn_WaitSignal_t)(uint32_t mask);
// CreateIOReq(name_ea, pri, device, reply_port): u32 IOREQ item handle.
// The guest API passes the file/device Item in r5; the handler records that
// operand so a later DoIO can select the corresponding device boundary.
typedef uint32_t (*m2fn_CreateIOReq_t)(uint32_t name_ea, uint32_t pri,
                                       uint32_t device, uint32_t reply_port);
// CreateMsg(name_ea, prio, payload) / CreateSmallMsg(what, value, aux):
// mint deferred message items from the bounded local pool (type-tagged u32
// handles); payload words are recorded verbatim, never dereferenced.
typedef uint32_t (*m2fn_CreateMsg_t)(uint32_t name_ea, uint32_t prio,
                                     uint32_t payload);
typedef uint32_t (*m2fn_CreateSmallMsg_t)(uint32_t what, uint32_t value,
                                          uint32_t aux);
// Event-broker window (generated ConnectEventBroker): FindNamedItem answers
// only the bounded guest name "eventbroker" with a stable item;
// CreateBufferedMsg mints an in-guest record EA as its token; LookupItem
// returns one fixed in-guest descriptor EA (never a host pointer); SendMsg
// fills the record fields in place instead of queueing on a port.
typedef uint32_t (*m2fn_FindNamedItem_t)(uint32_t type_tag, uint32_t name_ea);
typedef uint32_t (*m2fn_CreateBufferedMsg_t)(uint32_t arg0, uint32_t arg1);
typedef uint32_t (*m2fn_LookupItem_t)(uint32_t item);
// SendMsg: both operands are guest words; whichever names a broker record EA
// is the message record (the other rides along as the port word). data_ea
// and size carry the message payload (e.g. flavor).
typedef int32_t  (*m2fn_SendMsg_t)(uint32_t word_a, uint32_t word_b,
                                    uint32_t data_ea, uint32_t size);
// Semaphore: CreateSemaphore(name_ea, pri) mints a signal-backed item handle
// via m2k_alloc_signal; Lock/Unlock are P/V on that item (folio 1 ord 160/161).
typedef uint32_t (*m2fn_CreateSemaphore_t)(uint32_t name_ea, uint32_t pri);
typedef int32_t  (*m2fn_LockSemaphore_t)(uint32_t sema, uint32_t flags);
typedef int32_t  (*m2fn_UnlockSemaphore_t)(uint32_t sema);
// TE instruction channel.  The vendor SetTEWritePointer leaf is void and
// leaves r3 unchanged after storing it to the IWP register; the host consumes
// accepted lists synchronously, so GetTEReadPointer observes that completed
// consumer frontier.
typedef uint32_t (*m2fn_GetTEReadPointer_t)(void);
typedef void     (*m2fn_SetTEWritePointer_t)(uint32_t write_ptr);
// SpinLock init/teardown (kernel 205/206, the M_InitMP family). Both take the
// guest EA of an in-guest lock record — the guest carves 0x30 bytes via
// AllocMemMasked#35 and passes the record EA / record+0x20 — and return an
// int32 status: 0 = success (proven: both M_InitMP 205 sites `cmpwi r3,0;
// bne` to the -2 error path). All four 206 sites discard the result. No
// guest-visible memory write is proven to be required (the record is already
// zeroed by the allocator and no code reads the lock words), so handlers
// validate the EA bound and report status only.
typedef int32_t  (*m2fn_CreateSpinLock_t)(uint32_t lock_ea);
typedef int32_t  (*m2fn_DeleteSpinLock_t)(uint32_t lock_ea);
// CreateDeviceStackList (kernel 67, Matrix_Perspective.c:CreateDeviceStackListVA 0x73708):
// takes r3=listOut EA (List** pointer in guest RAM), r4=desc EA (array of 16-byte
// DeviceStackDesc records terminated by 0 tag). Allocates a 16-byte List struct
// in guest RAM, initializes it as empty (l_Head = list_ea + 8 for ISEMPTYLIST verification),
// writes list_ea to *listOut, and returns 0 (Err) in r3.
typedef int32_t  (*m2fn_CreateDeviceStackList_t)(uint32_t listOut_ea, uint32_t desc_ea);
// DeleteDeviceStackList (kernel 68): the teardown of CreateDeviceStackList#67.
// Takes r3=list_ea (the List* written into *listOut by CreateDeviceStackList),
// frees the 16-byte List struct, and returns 0 (Err) in r3. All 8 generated
// call sites pass r3 = list_ea (either the live list register r31 or the
// *listOut slot reloaded from the stack) and discard the return value.
typedef int32_t  (*m2fn_DeleteDeviceStackList_t)(uint32_t list_ea);
// OpenDeviceStack (kernel 69): opens a device stack from the List built by
// CreateDeviceStackList#67. Takes r3=ds_ea, the device-stack node EA each caller
// loads as *(list_ea+4); returns 0 (Err success) so FMVOpenVideo / CreateMPIOReq /
// OpenTEDevice proceed down their device-setup success paths (they branch on `>=0`).
// Returns -1 when the EA is unbound, null, or outside the bound guest RAM.
typedef int32_t  (*m2fn_OpenDeviceStack_t)(uint32_t ds_ea);
// CloseDeviceStack (kernel 70): closes the device stack opened by OpenDeviceStack#69.
// Takes r3=ds_ea, the same device-stack node EA each of the 7 generated call sites loads
// (gs_Delete: *(gstate+0x3c); MPS_TeardownMPEGStill: *(0x6d8c global); FMVCloseDevice:
// *(fmv_struct+0); CreateMPIOReq/CreateTimerIOReq: the live r31 = OpenDeviceStack result;
// DeleteMPIOReq/DeleteTimerIOReq: *(node+0x18) after a *(node+0x24) walk). Returns 0
// (Err success) so the teardown paths proceed; 6 of 7 sites discard the return, and
// MPS_TeardownMPEGStill 0x20a58 propagates it as its own Err (so 0 = success is correct).
// Returns -1 when the EA is unbound, null, or outside the bound guest RAM.
typedef int32_t  (*m2fn_CloseDeviceStack_t)(uint32_t ds_ea);
// FlushDCache/WriteBackDCache (kernel 51/52): cache-coherence maintenance over a
// [addr, addr+len) range. The recomp host RAM image is ordinary coherent host
// memory, so no flush/storeback is required; the call is a no-op. The proven call
// windows set r3=0 / r4=addr / r5=len and discard the (void) result, so the handler
// takes (addr, len) and the dispatch leaves gpr[3] as the caller's pre-call value (0).
typedef void (*m2fn_FlushDCache_t)(uint32_t addr, uint32_t len);
typedef void (*m2fn_WriteBackDCache_t)(uint32_t addr, uint32_t len);
// FreeMemDebug (kernel 59): FreeMemDebug(void *mem, int32 size,
// const char *sourceFile, int32 sourceLine). All four values are guest words;
// sourceFile is an EA, not a host pointer. mem/size use the same task-owned
// reclamation path as FreeMem, and the void wrapper preserves gpr[3].
typedef void (*m2fn_FreeMemDebug_t)(uint32_t mem, uint32_t mem_size,
                                    uint32_t source_file_ea,
                                    uint32_t source_line);
// strncpy (kernel 316): r3=dst EA, r4=src EA, r5=byte cap; returns dst EA in r3.
// Bounded byte-wise copy within the guest RAM image (mirrors gk_memset bounds).
typedef uint32_t (*m2fn_strncpy_t)(uint32_t dst, uint32_t src, uint32_t n);
// toupper (kernel 322): r3=input byte; returns the uppercased byte in r3.
typedef uint32_t (*m2fn_toupper_t)(uint32_t c);
// ObtainSpinLock/ReleaseSpinLock (kernel 566/567, M_InitMP family): source
// spinlock.h declares Obtain as bool (1 acquired, 0 failed) and Release as void.
// The single-threaded host returns 1 for a bounded lock record and 0 for an
// invalid/unbound EA; Release preserves gpr[3] because it is void.
typedef uint32_t (*m2fn_ObtainSpinLock_t)(uint32_t lock_ea);
typedef void (*m2fn_ReleaseSpinLock_t)(uint32_t lock_ea);
// OpenItem/CloseItem (kernel 162/159): Portfolio OS item lifecycle. OpenItem
// takes (item,args) and returns the open handle or a negative Err; CloseItem
// validates the item, does not reclaim it, and returns an Err. The recovered
// OpenItem callers pass NULL args and store the returned handle back to the
// same field; CloseItem callers discard the returned Err.
typedef uint32_t (*m2fn_OpenItem_t)(uint32_t item, uint32_t args_ea);
typedef int32_t (*m2fn_CloseItem_t)(uint32_t item);
// DeleteItem (kernel 164): Portfolio OS item deletion — companion to CloseItem#159.
// Proven window (game_text_09.c:0x69944): r3 = item EA loaded from a field, the
// call is followed by `addi r3,r31,0`, so the source Err is discarded by this
// caller. DeleteIOReq is a source-level alias for this selector; typed dispatch
// recognizes the substrate IOReq handle and reclaims it there.
// Local handles reclaim their pool slot and invalidate their generation; other
// already-supported item-like EAs remain outside this pool.
typedef int32_t (*m2fn_DeleteItem_t)(uint32_t item);
// CheckItem (kernel 29): CheckItem(Item, uint8 subsystemType, uint8 nodeType)
// returns the matching guest Node EA or NULL. The r4/r5 values are load-bearing
// at DeleteMPIOReq/DeleteTimerIOReq (1, IOREQNODE=14), not an error/status ABI.
// CheckItem is a one-argument validity view for host-only callers;
// production guest dispatch uses the adjacent exact-ABI CheckItemNode slot.
typedef uint32_t (*m2fn_CheckItem_t)(uint32_t item);
typedef uint32_t (*m2fn_CheckItemNode_t)(uint32_t item,
                                         uint32_t subsystem_type,
                                         uint32_t node_type);
// FindNamedNode (kernel 36): Node* FindNamedNode(List* list, char* name) -> node EA or 0.
typedef uint32_t (*m2fn_FindNamedNode_t)(uint32_t list, uint32_t name);
// UniversalInsertNode (kernel 28): void UniversalInsertNode(List* list,
// Node* node, bool (*compare)(Node*, Node*)). The callback EA is a guest
// function address resolved by the generated image.
typedef void (*m2fn_UniversalInsertNode_t)(uint32_t list, uint32_t node,
                                           uint32_t compare);
// FindTagArg (kernel 1): TagArg* FindTagArg(const TagArg* tagList, uint32 tag)
// -> guest EA of the last matching TagArg pair or 0.
typedef uint32_t (*m2fn_FindTagArg_t)(uint32_t taglist, uint32_t tag);
// GetMemInfo (kernel 16): void GetMemInfo(MemInfo* info, uint32 size,
// uint32 memFlags). The Portfolio implementation writes a bounded prefix of
// the 7-word MemInfo record and has no return value; preserve r3 in the guest
// context rather than inventing a pointer return.
typedef void (*m2fn_GetMemInfo_t)(uint32_t info, uint32_t size, uint32_t sel);
// UnimportByName (kernel 100): Item UnimportByName(const char* name, Item folio).
typedef uint32_t (*m2fn_UnimportByName_t)(uint32_t folio, uint32_t name);
// DebugPutStr/DebugPutChar (kernel 118/120): Opera debug-console output.
// DebugPutStr takes a guest NUL-terminated string EA in r3; DebugPutChar takes a
// char byte in r3. Both are void on the guest side (callers discard the result
// and advance a pointer), so a host stderr sink is a faithful debug proxy.
typedef void (*m2fn_DebugPutStr_t)(uint32_t str_ea);
typedef void (*m2fn_DebugPutChar_t)(uint32_t c);

typedef struct m2_kernel_dispatch {
    m2fn_ReallocMem_t ReallocMem; // kernel:5, source shrink-only semantics
    m2fn_AllocMem_t AllocMem;   // kernel:2
    m2fn_AllocMemMasked_t AllocMemMasked; // kernel:35 (r3..r6 -> guest EA)
    m2fn_AllocMemDebug_t AllocMemDebug;   // kernel:57 (r3=size, r4=0x200, r5=tag EA, r6=line -> guest EA)
    m2fn_FreeMem_t  FreeMem;    // kernel:3
    m2fn_Exit_t     Exit;       // kernel:8  (task-local exit; never returns with a live guard)
    m2fn_memset_t   MemSet;     // kernel:33 (void* dst, int c, size_t n -> dst)
    m2fn_memmove_t  MemMove;    // kernel:31 (void* dst, const void* src, size_t n -> dst)
    m2fn_CheckIO_t  CheckIO;    // kernel:4
    m2fn_GetCacheInfo_t GetCacheInfo; // kernel:38 (CacheInfo 28 bytes)
    m2fn_CreateItemVA_t CreateItemVA; // kernel:80 (TagArg VA, 12 sites)
    m2fn_CreateItem_t CreateItem;     // kernel:119 (TagArg list EA, 3 sites)
    m2fn_ScavengeMem_t ScavengeMem; // kernel:17
    m2fn_LookupItem_t LookupItem; // kernel:34
    m2fn_CreateThreadVA_t CreateThreadVA; // kernel:84
    m2fn_CreateTaskVA_t CreateTaskVA; // kernel:83
    m2fn_CreateTask_t CreateTask; // kernel:553
    m2fn_OpenModule_t OpenModule; // kernel:268
    m2fn_CloseModule_t CloseModule; // kernel:269
    m2fn_ImportByName_t ImportByName;     // kernel:103
    m2fn_WaitIO_t   WaitIO;     // kernel:129
    m2fn_WaitPort_t WaitPort;   // kernel:130
    m2fn_DoIO_t     DoIO;       // kernel:132
    m2fn_AbortIO_t  AbortIO;    // kernel:141
    m2fn_SendIO_t   SendIO;     // kernel:142
    m2fn_FreeSignal_t FreeSignal; // kernel:143
    m2fn_AllocSignal_t AllocSignal; // kernel:144
    m2fn_GetMsg_t   GetMsg;     // kernel:146
    m2fn_ReplyMsg_t ReplyMsg;   // kernel:147
    m2fn_SendMsg_t SendMsg;     // kernel:150
    m2fn_SendSmallMsg_t SendSmallMsg; // kernel:151
    m2fn_GetThisMsg_t GetThisMsg; // kernel:152
    m2fn_FindItem_t FindItem;   // kernel:163
    m2fn_SendSignal_t SendSignal; // kernel:165
    m2fn_WaitSignal_t WaitSignal; // kernel:166
    m2fn_CreateBufferedMsg_t CreateBufferedMsg; // kernel:168
    m2fn_CreateIOReq_t CreateIOReq; // kernel:170
    m2fn_CreateMsg_t CreateMsg; // kernel:171
    m2fn_CreateMsgPort_t CreateMsgPort; // kernel:172
    m2fn_CreateSemaphore_t CreateSemaphore; // kernel:174
    m2fn_LockSemaphore_t LockSemaphore;   // kernel:160
    m2fn_UnlockSemaphore_t UnlockSemaphore; // kernel:161
    m2fn_CreateSmallMsg_t CreateSmallMsg; // kernel:175
    m2fn_FindNamedItem_t FindNamedItem; // kernel:176
    m2fn_FindCurrentModule_t FindCurrentModule; // kernel:262
    m2fn_printf_t   printf;     // kernel:309 (typed slot; see dispatch note)
    m2fn_vcprintf_t vcprintf;   // kernel:24  (typed slot; guest va_list -> stderr)
    m2fn_strlen_t strlen;       // kernel:312
    m2fn_strcpy_t strcpy;       // kernel:315
    m2fn_strcat_t strcat;       // kernel:317
    m2fn_strcmp_t strcmp;       // kernel:320
    m2fn_itof_t     itof;       // kernel:325 (__itof)
    m2fn_utof_t     utof;       // kernel:326 (__utof)
    m2fn_VaArg_t    VaArg;      // kernel:267 (__va_arg)
    m2fn_CreateThread_t CreateThread; // kernel:555
    m2fn_GetTEReadPointer_t GetTEReadPointer;   // kernel:558
    m2fn_SetTEWritePointer_t SetTEWritePointer; // kernel:561
    m2fn_CreateSpinLock_t CreateSpinLock;   // kernel:205 (M_InitMP family)
    m2fn_DeleteSpinLock_t DeleteSpinLock;   // kernel:206 (M_InitMP family)
    m2fn_CreateDeviceStackList_t CreateDeviceStackList; // kernel:67
    m2fn_DeleteDeviceStackList_t DeleteDeviceStackList; // kernel:68
    m2fn_OpenDeviceStack_t OpenDeviceStack; // kernel:69
    m2fn_CloseDeviceStack_t CloseDeviceStack; // kernel:70
    m2fn_FlushDCache_t FlushDCache;       // kernel:51
    m2fn_WriteBackDCache_t WriteBackDCache; // kernel:52
    m2fn_FreeMemDebug_t FreeMemDebug;     // kernel:59
    m2fn_strncpy_t StrNCpy;               // kernel:316
    m2fn_toupper_t ToUpper;               // kernel:322
    m2fn_ObtainSpinLock_t ObtainSpinLock; // kernel:566
    m2fn_ReleaseSpinLock_t ReleaseSpinLock; // kernel:567
    m2fn_OpenItem_t OpenItem;       // kernel:162
    m2fn_CloseItem_t CloseItem;     // kernel:159
    m2fn_DeleteItem_t DeleteItem;   // kernel:164
    m2fn_CheckItem_t CheckItem;      // kernel:29 host-only validity view
    m2fn_CheckItemNode_t CheckItemNode; // kernel:29 exact Node-returning ABI
    m2fn_FindNamedNode_t FindNamedNode; // kernel:36
    m2fn_UniversalInsertNode_t UniversalInsertNode; // kernel:28
    m2fn_FindTagArg_t FindTagArg;   // kernel:1
    m2fn_GetMemInfo_t GetMemInfo;    // kernel:16
    m2fn_UnimportByName_t UnimportByName; // kernel:100
    m2fn_DebugPutStr_t DebugPutStr; // kernel:118
    m2fn_DebugPutChar_t DebugPutChar; // kernel:120
    // ---- promoted slots ----
    m2fn_PrepList_t PrepList;                 // kernel:37
    m2fn_RemNode_t RemNode;                  // kernel:40
    m2fn_AddTail_t AddTail;                  // kernel:42
    m2fn_RemTail_t RemTail;                  // kernel:43
    m2fn_AddHead_t AddHead;                  // kernel:44
    m2fn_RemHead_t RemHead;                  // kernel:45
    m2fn_SampleSystemTimeTT_t SampleSystemTimeTT; // kernel:21
    m2fn_ConvertTimerTicksToTimeVal_t ConvertTimerTicksToTimeVal; // kernel:27
    m2fn_FlushDCacheAll_t FlushDCacheAll;    // kernel:136
    m2fn_clib_PrintError_t clib_PrintError;  // kernel:310
    m2fn_PrintfSysErr_t PrintfSysErr;        // kernel:311
    m2fn_sprintf_t Sprintf;                  // kernel:324
    m2fn_IsMasterCPU_t IsMasterCPU;          // kernel:105
    m2fn_SubTimerTicks_t SubTimerTicks;      // kernel:48
    m2fn_ReadHardwareRandomNumber_t ReadHardwareRandomNumber; // kernel:149
    m2fn_SetItemPri_t SetItemPri;            // kernel:157
    m2fn_FindAndOpenItem_t FindAndOpenItem;  // kernel:133
} m2_kernel_dispatch;

static inline void m2_kernel_dispatch_init(m2_kernel_dispatch* r) {
    // Zero every slot: an all-NULL registry leaves each call site on the
    // generic lookup path, preserving the pre-typed behavior exactly.
    for (unsigned i = 0; i < sizeof(*r) / sizeof(void*); ++i)
        ((void**)r)[i] = NULL;
}

// Returns 1 iff (folio, ordinal) is one of the known kernel selectors.
static inline int m2_kernel_selector_known(uint32_t folio, uint32_t ordinal) {
    if (folio != M2_FOLIO_KERNEL) return 0;
    switch (ordinal) {
    case M2_KSEL_ALLOC_MEM: case M2_KSEL_ALLOC_MEM_MASKED: case M2_KSEL_ALLOC_MEM_DEBUG:
    case M2_KSEL_REALLOC_MEM:
    case M2_KSEL_FREE_MEM:
    case M2_KSEL_EXIT:
    case M2_KSEL_CHECK_IO:
    case M2_KSEL_MEMSET: case M2_KSEL_MEMMOVE:
    case M2_KSEL_GET_CACHE_INFO: case M2_KSEL_CREATE_ITEM_VA: case M2_KSEL_CREATE_ITEM: case M2_KSEL_SCAVENGE_MEM:
    case M2_KSEL_LOOKUP_ITEM:
    case M2_KSEL_CREATE_THREAD_VA: case M2_KSEL_CREATE_TASK_VA:
    case M2_KSEL_OPEN_MODULE: case M2_KSEL_CLOSE_MODULE:
    case M2_KSEL_CREATE_TASK: case M2_KSEL_IMPORT_BY_NAME:
    case M2_KSEL_WAIT_IO: case M2_KSEL_WAIT_PORT: case M2_KSEL_DO_IO:
    case M2_KSEL_ABORT_IO: case M2_KSEL_SEND_IO: case M2_KSEL_FREE_SIGNAL:
    case M2_KSEL_ALLOC_SIGNAL: case M2_KSEL_GET_MSG: case M2_KSEL_REPLY_MSG:
    case M2_KSEL_SEND_SMALL_MSG: case M2_KSEL_SEND_MSG: case M2_KSEL_GET_THIS_MSG:
    case M2_KSEL_FIND_ITEM:
    case M2_KSEL_SEND_SIGNAL:     case M2_KSEL_WAIT_SIGNAL: case M2_KSEL_CREATE_BUFFERED_MSG:
    case M2_KSEL_CREATE_IOREQ: case M2_KSEL_CREATE_MSG:
    case M2_KSEL_CREATE_MSGPORT: case M2_KSEL_CREATE_SEMAPHORE: case M2_KSEL_LOCK_SEMAPHORE: case M2_KSEL_UNLOCK_SEMAPHORE: case M2_KSEL_CREATE_SMALL_MSG:
    case M2_KSEL_FIND_NAMED_ITEM:
    case M2_KSEL_FIND_CURRENT_MODULE: case M2_KSEL_PRINTF: case M2_KSEL_ITOF:
    case M2_KSEL_UTOF:
    case M2_KSEL_VA_ARG:
    case M2_KSEL_CREATE_THREAD: case M2_KSEL_GET_TE_READ_POINTER:
    case M2_KSEL_SET_TE_WRITE_POINTER:
    case M2_KSEL_CREATE_SPINLOCK: case M2_KSEL_DELETE_SPINLOCK:
    case M2_KSEL_CREATE_DEVICE_STACK_LIST:
    case M2_KSEL_DELETE_DEVICE_STACK_LIST:
    case M2_KSEL_OPEN_DEVICE_STACK:
    case M2_KSEL_CLOSE_DEVICE_STACK:
    case M2_KSEL_FLUSH_DCACHE: case M2_KSEL_WRITE_BACK_DCACHE:
    case M2_KSEL_FREE_MEM_DEBUG:
    case M2_KSEL_STRNCPY: case M2_KSEL_TOUPPER:
    case M2_KSEL_OBTAIN_SPINLOCK: case M2_KSEL_RELEASE_SPINLOCK:
    case M2_KSEL_OPEN_ITEM: case M2_KSEL_CLOSE_ITEM: case M2_KSEL_DELETE_ITEM:
    case M2_KSEL_CHECK_ITEM:
    case M2_KSEL_FIND_NAMED_NODE: case M2_KSEL_UNIVERSAL_INSERT_NODE: case M2_KSEL_FIND_TAG_ARG:
    case M2_KSEL_GET_MEM_INFO: case M2_KSEL_UNIMPORT_BY_NAME:
    case M2_KSEL_DEBUG_PUT_STR: case M2_KSEL_DEBUG_PUT_CHAR:
    case M2_KSEL_VCPRINTF:
    case M2_KSEL_STRLEN: case M2_KSEL_STRCPY: case M2_KSEL_STRCAT: case M2_KSEL_STRCMP:
    // ---- promoted ords ----
    case M2_KSEL_PREP_LIST: case M2_KSEL_REM_NODE:
    case M2_KSEL_ADD_TAIL: case M2_KSEL_REM_TAIL:
    case M2_KSEL_ADD_HEAD: case M2_KSEL_REM_HEAD:
    case M2_KSEL_SAMPLE_SYSTEM_TIME_TT: case M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL:
    case M2_KSEL_FLUSH_DCACHE_ALL:
    case M2_KSEL_CLIB_PRINT_ERROR: case M2_KSEL_PRINTF_SYS_ERR: case M2_KSEL_SPRINTF:
    case M2_KSEL_IS_MASTER_CPU:
    case M2_KSEL_SUB_TIMER_TICKS: case M2_KSEL_READ_HARDWARE_RANDOM_NUMBER:
    case M2_KSEL_SET_ITEM_PRI: case M2_KSEL_FIND_AND_OPEN_ITEM:
        return 1;
    default:
        return 0;
    }
}

// Type-checked registration by ordinal: 0 ok, -1 for unknown ordinal/NULL fn.
static inline int m2_kernel_dispatch_register(m2_kernel_dispatch* r,
                                              uint32_t ordinal, void* fn) {
    if (!r || !fn) return -1;
    switch (ordinal) {
    case M2_KSEL_ALLOC_MEM: r->AllocMem = (m2fn_AllocMem_t)fn; return 0;
    case M2_KSEL_REALLOC_MEM: r->ReallocMem = (m2fn_ReallocMem_t)fn; return 0;
    case M2_KSEL_ALLOC_MEM_MASKED:
        r->AllocMemMasked = (m2fn_AllocMemMasked_t)fn; return 0;
    case M2_KSEL_ALLOC_MEM_DEBUG:
        r->AllocMemDebug = (m2fn_AllocMemDebug_t)fn; return 0;
    case M2_KSEL_FREE_MEM:  r->FreeMem  = (m2fn_FreeMem_t)fn;  return 0;
    case M2_KSEL_EXIT:      r->Exit     = (m2fn_Exit_t)fn;     return 0;
    case M2_KSEL_MEMSET:    r->MemSet   = (m2fn_memset_t)fn;   return 0;
    case M2_KSEL_MEMMOVE:   r->MemMove  = (m2fn_memmove_t)fn;  return 0;
    case M2_KSEL_CHECK_IO:  r->CheckIO  = (m2fn_CheckIO_t)fn;  return 0;
    case M2_KSEL_GET_CACHE_INFO:
        r->GetCacheInfo = (m2fn_GetCacheInfo_t)fn; return 0;
    case M2_KSEL_CREATE_ITEM_VA:
        r->CreateItemVA = (m2fn_CreateItemVA_t)fn; return 0;
    case M2_KSEL_CREATE_ITEM:
        r->CreateItem = (m2fn_CreateItem_t)fn; return 0;
    case M2_KSEL_SCAVENGE_MEM:
        r->ScavengeMem = (m2fn_ScavengeMem_t)fn; return 0;
    case M2_KSEL_LOOKUP_ITEM:
        r->LookupItem = (m2fn_LookupItem_t)fn; return 0;
    case M2_KSEL_CREATE_THREAD_VA:
        r->CreateThreadVA = (m2fn_CreateThreadVA_t)fn; return 0;
    case M2_KSEL_CREATE_TASK_VA:
        r->CreateTaskVA = (m2fn_CreateTaskVA_t)fn; return 0;
    case M2_KSEL_OPEN_MODULE:
        r->OpenModule = (m2fn_OpenModule_t)fn; return 0;
    case M2_KSEL_CLOSE_MODULE:
        r->CloseModule = (m2fn_CloseModule_t)fn; return 0;
    case M2_KSEL_CREATE_TASK:
        r->CreateTask = (m2fn_CreateTask_t)fn; return 0;
    case M2_KSEL_IMPORT_BY_NAME:
        r->ImportByName = (m2fn_ImportByName_t)fn; return 0;
    case M2_KSEL_WAIT_IO:   r->WaitIO   = (m2fn_WaitIO_t)fn;   return 0;
    case M2_KSEL_WAIT_PORT: r->WaitPort = (m2fn_WaitPort_t)fn; return 0;
    case M2_KSEL_DO_IO:     r->DoIO     = (m2fn_DoIO_t)fn;     return 0;
    case M2_KSEL_ABORT_IO:  r->AbortIO  = (m2fn_AbortIO_t)fn;  return 0;
    case M2_KSEL_SEND_IO:   r->SendIO   = (m2fn_SendIO_t)fn;   return 0;
    case M2_KSEL_FREE_SIGNAL:
        r->FreeSignal = (m2fn_FreeSignal_t)fn; return 0;
    case M2_KSEL_ALLOC_SIGNAL:
        r->AllocSignal = (m2fn_AllocSignal_t)fn; return 0;
    case M2_KSEL_GET_MSG:   r->GetMsg   = (m2fn_GetMsg_t)fn;   return 0;
    case M2_KSEL_REPLY_MSG: r->ReplyMsg = (m2fn_ReplyMsg_t)fn; return 0;
    case M2_KSEL_SEND_MSG:  r->SendMsg  = (m2fn_SendMsg_t)fn;  return 0;
    case M2_KSEL_SEND_SMALL_MSG:
        r->SendSmallMsg = (m2fn_SendSmallMsg_t)fn; return 0;
    case M2_KSEL_GET_THIS_MSG:
        r->GetThisMsg = (m2fn_GetThisMsg_t)fn; return 0;
    case M2_KSEL_FIND_ITEM:
        r->FindItem = (m2fn_FindItem_t)fn; return 0;
    case M2_KSEL_SEND_SIGNAL:
        r->SendSignal = (m2fn_SendSignal_t)fn; return 0;
    case M2_KSEL_WAIT_SIGNAL:
        r->WaitSignal = (m2fn_WaitSignal_t)fn; return 0;
    case M2_KSEL_CREATE_BUFFERED_MSG:
        r->CreateBufferedMsg = (m2fn_CreateBufferedMsg_t)fn; return 0;
    case M2_KSEL_CREATE_IOREQ:
        r->CreateIOReq = (m2fn_CreateIOReq_t)fn; return 0;
    case M2_KSEL_CREATE_MSG:
        r->CreateMsg = (m2fn_CreateMsg_t)fn; return 0;
    case M2_KSEL_CREATE_MSGPORT:
        r->CreateMsgPort = (m2fn_CreateMsgPort_t)fn; return 0;
    case M2_KSEL_CREATE_SEMAPHORE:
        r->CreateSemaphore = (m2fn_CreateSemaphore_t)fn; return 0;
    case M2_KSEL_LOCK_SEMAPHORE:
        r->LockSemaphore = (m2fn_LockSemaphore_t)fn; return 0;
    case M2_KSEL_UNLOCK_SEMAPHORE:
        r->UnlockSemaphore = (m2fn_UnlockSemaphore_t)fn; return 0;
    case M2_KSEL_CREATE_SMALL_MSG:
        r->CreateSmallMsg = (m2fn_CreateSmallMsg_t)fn; return 0;
    case M2_KSEL_FIND_NAMED_ITEM:
        r->FindNamedItem = (m2fn_FindNamedItem_t)fn; return 0;
    case M2_KSEL_FIND_CURRENT_MODULE:
        r->FindCurrentModule = (m2fn_FindCurrentModule_t)fn; return 0;
    case M2_KSEL_PRINTF:    r->printf   = (m2fn_printf_t)fn;   return 0;
    case M2_KSEL_VCPRINTF:  r->vcprintf = (m2fn_vcprintf_t)fn; return 0;
    case M2_KSEL_STRLEN:    r->strlen   = (m2fn_strlen_t)fn;   return 0;
    case M2_KSEL_ITOF:      r->itof     = (m2fn_itof_t)fn;     return 0;
    case M2_KSEL_UTOF:      r->utof     = (m2fn_utof_t)fn;     return 0;
    case M2_KSEL_VA_ARG:    r->VaArg    = (m2fn_VaArg_t)fn;    return 0;
    case M2_KSEL_CREATE_THREAD:
        r->CreateThread = (m2fn_CreateThread_t)fn; return 0;
    case M2_KSEL_GET_TE_READ_POINTER:
        r->GetTEReadPointer = (m2fn_GetTEReadPointer_t)fn; return 0;
    case M2_KSEL_SET_TE_WRITE_POINTER:
        r->SetTEWritePointer = (m2fn_SetTEWritePointer_t)fn; return 0;
    case M2_KSEL_CREATE_SPINLOCK:
        r->CreateSpinLock = (m2fn_CreateSpinLock_t)fn; return 0;
    case M2_KSEL_DELETE_SPINLOCK:
        r->DeleteSpinLock = (m2fn_DeleteSpinLock_t)fn; return 0;
    case M2_KSEL_CREATE_DEVICE_STACK_LIST:
        r->CreateDeviceStackList = (m2fn_CreateDeviceStackList_t)fn; return 0;
    case M2_KSEL_DELETE_DEVICE_STACK_LIST:
        r->DeleteDeviceStackList = (m2fn_DeleteDeviceStackList_t)fn; return 0;
    case M2_KSEL_OPEN_DEVICE_STACK:
        r->OpenDeviceStack = (m2fn_OpenDeviceStack_t)fn; return 0;
    case M2_KSEL_CLOSE_DEVICE_STACK:
        r->CloseDeviceStack = (m2fn_CloseDeviceStack_t)fn; return 0;
    case M2_KSEL_FLUSH_DCACHE:
        r->FlushDCache = (m2fn_FlushDCache_t)fn; return 0;
    case M2_KSEL_WRITE_BACK_DCACHE:
        r->WriteBackDCache = (m2fn_WriteBackDCache_t)fn; return 0;
    case M2_KSEL_FREE_MEM_DEBUG:
        r->FreeMemDebug = (m2fn_FreeMemDebug_t)fn; return 0;
    case M2_KSEL_STRNCPY:
        r->StrNCpy = (m2fn_strncpy_t)fn; return 0;
    case M2_KSEL_STRCPY:
        r->strcpy = (m2fn_strcpy_t)fn; return 0;
    case M2_KSEL_STRCAT:
        r->strcat = (m2fn_strcat_t)fn; return 0;
    case M2_KSEL_STRCMP:
        r->strcmp = (m2fn_strcmp_t)fn; return 0;
    case M2_KSEL_TOUPPER:
        r->ToUpper = (m2fn_toupper_t)fn; return 0;
    case M2_KSEL_OBTAIN_SPINLOCK:
        r->ObtainSpinLock = (m2fn_ObtainSpinLock_t)fn; return 0;
    case M2_KSEL_RELEASE_SPINLOCK:
        r->ReleaseSpinLock = (m2fn_ReleaseSpinLock_t)fn; return 0;
    case M2_KSEL_OPEN_ITEM:
        r->OpenItem = (m2fn_OpenItem_t)fn; return 0;
    case M2_KSEL_CLOSE_ITEM:
        r->CloseItem = (m2fn_CloseItem_t)fn; return 0;
    case M2_KSEL_DELETE_ITEM:
        r->DeleteItem = (m2fn_DeleteItem_t)fn; return 0;
    case M2_KSEL_CHECK_ITEM:
        r->CheckItemNode = (m2fn_CheckItemNode_t)fn; return 0;
    case M2_KSEL_FIND_NAMED_NODE:
        r->FindNamedNode = (m2fn_FindNamedNode_t)fn; return 0;
    case M2_KSEL_UNIVERSAL_INSERT_NODE:
        r->UniversalInsertNode = (m2fn_UniversalInsertNode_t)fn; return 0;
    case M2_KSEL_FIND_TAG_ARG:
        r->FindTagArg = (m2fn_FindTagArg_t)fn; return 0;
    case M2_KSEL_GET_MEM_INFO:
        r->GetMemInfo = (m2fn_GetMemInfo_t)fn; return 0;
    case M2_KSEL_UNIMPORT_BY_NAME:
        r->UnimportByName = (m2fn_UnimportByName_t)fn; return 0;
    case M2_KSEL_DEBUG_PUT_STR:
        r->DebugPutStr = (m2fn_DebugPutStr_t)fn; return 0;
    case M2_KSEL_DEBUG_PUT_CHAR:
        r->DebugPutChar = (m2fn_DebugPutChar_t)fn; return 0;
    // ---- promoted ords ----
    case M2_KSEL_PREP_LIST:
        r->PrepList = (m2fn_PrepList_t)fn; return 0;
    case M2_KSEL_REM_NODE:
        r->RemNode = (m2fn_RemNode_t)fn; return 0;
    case M2_KSEL_ADD_TAIL:
        r->AddTail = (m2fn_AddTail_t)fn; return 0;
    case M2_KSEL_REM_TAIL:
        r->RemTail = (m2fn_RemTail_t)fn; return 0;
    case M2_KSEL_ADD_HEAD:
        r->AddHead = (m2fn_AddHead_t)fn; return 0;
    case M2_KSEL_REM_HEAD:
        r->RemHead = (m2fn_RemHead_t)fn; return 0;
    case M2_KSEL_SAMPLE_SYSTEM_TIME_TT:
        r->SampleSystemTimeTT = (m2fn_SampleSystemTimeTT_t)fn; return 0;
    case M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL:
        r->ConvertTimerTicksToTimeVal = (m2fn_ConvertTimerTicksToTimeVal_t)fn; return 0;
    case M2_KSEL_FLUSH_DCACHE_ALL:
        r->FlushDCacheAll = (m2fn_FlushDCacheAll_t)fn; return 0;
    case M2_KSEL_CLIB_PRINT_ERROR:
        r->clib_PrintError = (m2fn_clib_PrintError_t)fn; return 0;
    case M2_KSEL_PRINTF_SYS_ERR:
        r->PrintfSysErr = (m2fn_PrintfSysErr_t)fn; return 0;
    case M2_KSEL_SPRINTF:
        r->Sprintf = (m2fn_sprintf_t)fn; return 0;
    case M2_KSEL_IS_MASTER_CPU:
        r->IsMasterCPU = (m2fn_IsMasterCPU_t)fn; return 0;
    case M2_KSEL_SUB_TIMER_TICKS:
        r->SubTimerTicks = (m2fn_SubTimerTicks_t)fn; return 0;
    case M2_KSEL_READ_HARDWARE_RANDOM_NUMBER:
        r->ReadHardwareRandomNumber =
            (m2fn_ReadHardwareRandomNumber_t)fn; return 0;
    case M2_KSEL_SET_ITEM_PRI:
        r->SetItemPri = (m2fn_SetItemPri_t)fn; return 0;
    case M2_KSEL_FIND_AND_OPEN_ITEM:
        r->FindAndOpenItem = (m2fn_FindAndOpenItem_t)fn; return 0;
    default: return -1;
    }
}

// Typed dispatch over the EABI register window (args r3.., int ret r3, float
// ret f1 — generated __itof sites read c->fpr[1], e.g. game_text_11.c).
// Every gate-family case marshals its operands out of c->gpr and lands the
// result in c->gpr[3] (the EABI integer return register generated code
// reads). Returns 1 when handled. printf is variadic: its slot is fully typed
// for direct/test invocation, but guest-side vararg forwarding is deferred,
// so it reports 0 and falls through to the untyped m2_selector_lookup path.
void m2_guest_kernel_set_selector_pc(uint32_t pc);
PPCContext* m2_guest_kernel_get_active_context(void);
void m2_guest_kernel_set_active_context(PPCContext* c);
void m2_guest_kernel_note_boundary(void);
// Same boundary note plus the (folio, ordinal) identity for the bounded
// kernel-lock attribution diagnostic (M2_KERNEL_LOCK_DIAG). Production callers
// go through this entry; the identity is ignored unless the diagnostic is on.
void m2_guest_kernel_note_boundary_sel(uint32_t folio, uint32_t ordinal);
// 1 when the (folio, ordinal) pair is one of the image's C-library helpers
// that the lifted call sites route through the selector path (memmove,
// memset, __itof, __utof). These carry no kernel-boundary note; every other
// selector does. See the definition in recomp_runtime.c for the evidence and
// the M2_KERNEL_NOTE_LIBRARY_ALL falsification control.
int m2_selector_is_library_helper(uint32_t folio, uint32_t ordinal);

// Guest-visible IOReq completion fields (io_Actual/io_Flags/io_Error) as the
// device boundary published them. Contract tests read the same object the
// guest's own completion probe reads.
void m2_guest_ioreq_completion(uint32_t io, uint32_t* actual_out,
                               uint32_t* flags_out, uint32_t* error_out);

// Execute one source-backed Diab DECVASTUB trampoline. The helper reconstructs
// the bounded guest TagArg window used by __tagCall_0/1/2, calls the local
// wrapper through recomp_lookup(), and restores the caller's guest stack.
int m2_guest_tag_call(PPCContext* c, uint32_t fixed_arg_count);

// Execute an exact imported Diab DECVASTUB trampoline. The loader normally
// resolves r0 to the imported folio export; recomp uses the proven selector
// pair directly after reconstructing the same bounded TagArg window.
int m2_guest_import_tag_call(PPCContext* c, uint32_t fixed_arg_count,
                             uint32_t folio, uint32_t ordinal, uint32_t pc);

// Optional diagnostic for raw kernel memory-service writes.  The generated
// PPC stores already pass through wr16/wr32, but memset/memmove update the
// shared guest RAM in bulk and therefore need a separate bounded trace when
// a guest object changes between two ordinary stores and its next read.
static inline int m2_light_memory_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = (getenv("M2_LIGHT_MEMORY_TRACE") != NULL) ? 1 : 0;
    return enabled;
}

static inline int m2_guest_range_intersects(uint32_t ea, uint32_t len,
                                            uint32_t lo, uint32_t hi) {
    if (!len || ea >= hi) return 0;
    return (uint64_t)ea + (uint64_t)len > (uint64_t)lo;
}

static inline int m2_light_memory_trace_needed(uint32_t dst, uint32_t src,
                                               uint32_t len, int is_move) {
    if (!m2_light_memory_trace_enabled()) return 0;
    if (m2_guest_range_intersects(dst, len, 0x93ab00u, 0x93ab40u)) return 1;
    return is_move && m2_guest_range_intersects(src, len, 0x93ab00u, 0x93ab40u);
}

static inline void m2_trace_light_memory_service(PPCContext* c, const char* kind,
                                                 uint32_t dst, uint32_t src,
                                                 uint32_t len, uint32_t before18,
                                                 uint32_t before24) {
    if (!c || !c->mem) return;
    fprintf(stderr,
            "[recomp] light-memory %s pc=%#x dst=%#x src=%#x len=%#x "
            "field18=%#x->%#x field24=%#x->%#x\n",
            kind, c->pc, dst, src, len, before18, rd32(c, 0x93ab18u),
            before24, rd32(c, 0x93ab24u));
}

static inline void m2_trace_light_object_change(PPCContext* c, uint32_t folio,
                                                uint32_t ordinal, uint32_t pc,
                                                uint32_t before18,
                                                uint32_t before24) {
    if (!c || !c->mem || !m2_light_memory_trace_enabled()) return;
    const uint32_t after18 = rd32(c, 0x93ab18u);
    const uint32_t after24 = rd32(c, 0x93ab24u);
    if (after18 == before18 && after24 == before24) return;
    fprintf(stderr,
            "[recomp] light-object-change folio=%u ord=%u pc=%#x "
            "field18=%#x->%#x field24=%#x->%#x\n",
            folio, ordinal, pc, before18, after18, before24, after24);
}

static inline int recomp_call_kernel_dispatch_impl(PPCContext* c, uint32_t folio,
                                                   uint32_t ordinal, uint32_t pc,
                                                   const m2_kernel_dispatch* r) {
    if (!c || folio != M2_FOLIO_KERNEL) return 0;
    m2_guest_kernel_set_selector_pc(pc);
    if (m2_is_tag_call_helper(ordinal))
        return m2_guest_tag_call(c, ordinal - M2_KSEL_TAG_CALL_0);
    if (!r) return 0;
    switch (ordinal) {
    case M2_KSEL_ALLOC_MEM:
        if (!r->AllocMem) return 0;
        // Guest pointers are 32-bit: handlers must hand out 32-bit-clean
        // addresses (guest memory), not raw host heap pointers.
        c->gpr[3] = (uint32_t)(uintptr_t)r->AllocMem((int32_t)c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_REALLOC_MEM:
        if (!r->ReallocMem) return 0;
        c->gpr[3] = (uint32_t)(uintptr_t)r->ReallocMem(
            (void *)(uintptr_t)c->gpr[3], (int32_t)c->gpr[4],
            (int32_t)c->gpr[5], c->gpr[6]);
        return 1;
    case M2_KSEL_FREE_MEM:
        if (!r->FreeMem) return 0;
        r->FreeMem((void*)(uintptr_t)c->gpr[3], (int32_t)c->gpr[4]);
        return 1;
    case M2_KSEL_EXIT:
        if (!r->Exit) return 0;
        // r3 = status. The handler unwinds the calling guest task to its exit
        // guard (never returning here). With no live guard it records the
        // request and returns without terminating the host process.
        r->Exit(c->gpr[3]);
        return 1;
    case M2_KSEL_ALLOC_MEM_MASKED:
        if (!r->AllocMemMasked) return 0;
        // r3=minSize, r4=flags, r5=careBits, r6=stateBits. The original
        // AllocMemMasked ABI uses all four words; in particular the recovered
        // MPEG path supplies alignment masks through r5/r6.
        c->gpr[3] = (uint32_t)(uintptr_t)r->AllocMemMasked((int32_t)c->gpr[3],
                                                           c->gpr[4],
                                                           c->gpr[5],
                                                           c->gpr[6]);
        return 1;
    case M2_KSEL_ALLOC_MEM_DEBUG:
        if (!r->AllocMemDebug) return 0;
        // r3=size, r4=0x200 constant, r5=source-tag string EA, r6=source line
        // (debug metadata never consumed by lifted callers); only size feeds
        // the allocator, result EA lands in r3.
        c->gpr[3] = (uint32_t)(uintptr_t)r->AllocMemDebug((int32_t)c->gpr[3],
                                                          c->gpr[4],
                                                          c->gpr[5],
                                                          c->gpr[6]);
        return 1;
    case M2_KSEL_MEMSET:
        if (!r->MemSet) return 0;
        // r3=dst guest EA, r4=fill byte, r5=byte count -> returns dst in r3
        // (bounded writes happen inside the handler on the shared guest RAM).
        {
            const int trace = m2_light_memory_trace_needed(c->gpr[3], 0,
                                                            c->gpr[5], 0);
            const uint32_t before18 = trace ? rd32(c, 0x93ab18u) : 0;
            const uint32_t before24 = trace ? rd32(c, 0x93ab24u) : 0;
        c->gpr[3] = (uint32_t)(uintptr_t)r->MemSet((void*)(uintptr_t)c->gpr[3],
                                                   (int32_t)c->gpr[4], c->gpr[5]);
            if (trace)
                m2_trace_light_memory_service(c, "memset", c->gpr[3],
                                              (uint32_t)c->gpr[4], c->gpr[5],
                                              before18, before24);
        }
        return 1;
    case M2_KSEL_MEMMOVE:
        if (!r->MemMove) return 0;
        // r3=dst guest EA, r4=src guest EA, r5=byte count -> returns dst in r3.
        {
            const int trace = m2_light_memory_trace_needed(c->gpr[3], c->gpr[4],
                                                            c->gpr[5], 1);
            const uint32_t before18 = trace ? rd32(c, 0x93ab18u) : 0;
            const uint32_t before24 = trace ? rd32(c, 0x93ab24u) : 0;
            const uint32_t dst = c->gpr[3];
            const uint32_t src = c->gpr[4];
        c->gpr[3] = (uint32_t)(uintptr_t)r->MemMove((void*)(uintptr_t)c->gpr[3],
                                                    (const void*)(uintptr_t)c->gpr[4],
                                                    c->gpr[5]);
            if (trace)
                m2_trace_light_memory_service(c, "memmove", dst, src, c->gpr[5],
                                              before18, before24);
        }
        return 1;
    case M2_KSEL_CHECK_IO:
        if (!r->CheckIO) return 0;
        c->gpr[3] = (uint32_t)r->CheckIO(c->gpr[3]);
        return 1;
    case M2_KSEL_GET_CACHE_INFO:
        if (!r->GetCacheInfo) return 0;
        r->GetCacheInfo(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_CREATE_ITEM_VA:
        if (!r->CreateItemVA) return 0;
        c->gpr[3] = r->CreateItemVA(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6],
                                    c->gpr[7], c->gpr[8], c->gpr[9], c->gpr[10]);
        return 1;
    case M2_KSEL_CREATE_ITEM:
        if (!r->CreateItem) return 0;
        // r3=ctype (item type), r4=guest EA of a TagArg list (0 => no tags).
        c->gpr[3] = r->CreateItem(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_SCAVENGE_MEM:
        if (!r->ScavengeMem) return 0;
        // arg=r3 rides as the recorded payload word; int32 ret lands in r3
        // (every generated site overwrites r3 before consuming it).
        c->gpr[3] = (uint32_t)r->ScavengeMem(c->gpr[3]);
        return 1;
    case M2_KSEL_LOOKUP_ITEM:
        if (!r->LookupItem) return 0;
        c->gpr[3] = r->LookupItem(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_THREAD_VA:
        if (!r->CreateThreadVA) return 0;
        // Documented Portfolio window marshalled by NewThread @ 0x6ac4c:
        // code=r3 name=r4 pri=r5 stack=r6, then TagArg pairs riding r7..r10
        // (observed: 14=CREATETASK_TAG_ARGC / 15=CREATETASK_TAG_ARGP).
        c->gpr[3] = r->CreateThreadVA(c->gpr[3], c->gpr[4], c->gpr[5],
                                      c->gpr[6], c->gpr[7], c->gpr[8],
                                      c->gpr[9], c->gpr[10]);
        return 1;
    case M2_KSEL_CREATE_TASK_VA:
        if (!r->CreateTaskVA) return 0;
        c->gpr[3] = r->CreateTaskVA(c->gpr[3], c->gpr[4], c->gpr[5],
                                    c->gpr[6], c->gpr[7], c->gpr[8],
                                    c->gpr[9], c->gpr[10]);
        return 1;
    case M2_KSEL_OPEN_MODULE:
        if (!r->OpenModule) return 0;
        c->gpr[3] = r->OpenModule(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_CLOSE_MODULE:
        if (!r->CloseModule) return 0;
        c->gpr[3] = (uint32_t)r->CloseModule(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_TASK:
        if (!r->CreateTask) return 0;
        c->gpr[3] = r->CreateTask(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_IMPORT_BY_NAME:
        if (!r->ImportByName) return 0;
        c->gpr[3] = r->ImportByName(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_WAIT_IO:
        if (!r->WaitIO) return 0;
        c->gpr[3] = (uint32_t)r->WaitIO(c->gpr[3]);
        return 1;
    case M2_KSEL_WAIT_PORT:
        if (!r->WaitPort) return 0;
        c->gpr[3] = (uint32_t)r->WaitPort(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_DO_IO:
        if (!r->DoIO) return 0;
        c->gpr[3] = (uint32_t)r->DoIO(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_ABORT_IO:
        if (!r->AbortIO) return 0;
        c->gpr[3] = (uint32_t)r->AbortIO(c->gpr[3]);
        return 1;
    case M2_KSEL_SEND_IO:
        if (!r->SendIO) return 0;
        c->gpr[3] = (uint32_t)r->SendIO(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_FREE_SIGNAL:
        if (!r->FreeSignal) return 0;
        r->FreeSignal((int32_t)c->gpr[3]);
        return 1;
    case M2_KSEL_ALLOC_SIGNAL:
        if (!r->AllocSignal) return 0;
        c->gpr[3] = (uint32_t)r->AllocSignal((int32_t)c->gpr[3]);
        return 1;
    case M2_KSEL_GET_MSG:
        if (!r->GetMsg) return 0;
        c->gpr[3] = r->GetMsg(c->gpr[3]);
        return 1;
    case M2_KSEL_REPLY_MSG:
        if (!r->ReplyMsg) return 0;
        c->gpr[3] = (uint32_t)r->ReplyMsg(c->gpr[3]);
        return 1;
    case M2_KSEL_SEND_MSG:
        if (!r->SendMsg) return 0;
        c->gpr[3] = (uint32_t)r->SendMsg(c->gpr[3], c->gpr[4],
                                         c->gpr[5], c->gpr[6]);
        return 1;
    case M2_KSEL_SEND_SMALL_MSG:
        if (!r->SendSmallMsg) return 0;
        c->gpr[3] = (uint32_t)r->SendSmallMsg(c->gpr[3], c->gpr[4],
                                              c->gpr[5], c->gpr[6]);
        return 1;
    case M2_KSEL_GET_THIS_MSG:
        if (!r->GetThisMsg) return 0;
        c->gpr[3] = r->GetThisMsg(c->gpr[3]);
        return 1;
    case M2_KSEL_FIND_ITEM:
        if (!r->FindItem) return 0;
        // type_tag=r3 name_ea=r4 -> u32 item handle in r3 (both generated
        // sites pass (0x105, 0); see m2fn_FindItem_t note).
        c->gpr[3] = r->FindItem(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_SEND_SIGNAL:
        if (!r->SendSignal) return 0;
        c->gpr[3] = (uint32_t)r->SendSignal(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_WAIT_SIGNAL:
        if (!r->WaitSignal) return 0;
        c->gpr[3] = (uint32_t)r->WaitSignal(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_BUFFERED_MSG:
        if (!r->CreateBufferedMsg) return 0;
        c->gpr[3] = r->CreateBufferedMsg(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_CREATE_IOREQ:
        if (!r->CreateIOReq) return 0;
        c->gpr[3] = r->CreateIOReq(c->gpr[3], c->gpr[4], c->gpr[5],
                                   c->gpr[6]);
        return 1;
    case M2_KSEL_CREATE_MSG:
        if (!r->CreateMsg) return 0;
        c->gpr[3] = r->CreateMsg(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_CREATE_MSGPORT:
        if (!r->CreateMsgPort) return 0;
        // u32 guest item handle lands straight in the EABI return register.
        c->gpr[3] = r->CreateMsgPort();
        return 1;
    case M2_KSEL_CREATE_SEMAPHORE:
        if (!r->CreateSemaphore) return 0;
        c->gpr[3] = r->CreateSemaphore(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_LOCK_SEMAPHORE:
        if (!r->LockSemaphore) return 0;
        c->gpr[3] = (uint32_t)r->LockSemaphore(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_UNLOCK_SEMAPHORE:
        if (!r->UnlockSemaphore) return 0;
        c->gpr[3] = (uint32_t)r->UnlockSemaphore(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_SMALL_MSG:
        if (!r->CreateSmallMsg) return 0;
        c->gpr[3] = r->CreateSmallMsg(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_FIND_NAMED_ITEM:
        if (!r->FindNamedItem) return 0;
        c->gpr[3] = r->FindNamedItem(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_FIND_CURRENT_MODULE:
        if (!r->FindCurrentModule) return 0;
        c->gpr[3] = r->FindCurrentModule();
        return 1;
    case M2_KSEL_PRINTF:
        if (!r->printf) return 0;
        c->gpr[3] = (uint32_t)r->printf(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6],
                                        c->gpr[7], c->gpr[8], c->gpr[9], c->gpr[10]);
        return 1;
    case M2_KSEL_VCPRINTF:
        if (!r->vcprintf) return 0;
        // vcprintf(fmt, OutputFunc, userData, va_list); the callback and
        // user-data words remain guest values even when the host sink is used.
        c->gpr[3] = (uint32_t)r->vcprintf(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]);
        return 1;
    case M2_KSEL_STRLEN:
        if (!r->strlen) return 0;
        c->gpr[3] = r->strlen(c->gpr[3]);
        return 1;
    case M2_KSEL_STRCMP:
        if (!r->strcmp) return 0;
        // r3=a EA, r4=b EA -> signed int result (<0/0/>0) in r3.
        c->gpr[3] = (uint32_t)r->strcmp(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_ITOF:
        if (!r->itof) return 0;
        c->fpr[1] = r->itof((int32_t)c->gpr[3]);
        return 1;
    case M2_KSEL_UTOF:
        if (!r->utof) return 0;
        c->fpr[1] = r->utof((uint32_t)c->gpr[3]);
        return 1;
    case M2_KSEL_VA_ARG:
        if (!r->VaArg) return 0;
        // r3=va_list EA, r4=1 type class (the only class exercised in this
        // image). The next 4-byte word-slot EA lands in r3; fail-closed EA 0
        // for unproven classes / bad va_list is applied inside the handler.
        c->gpr[3] = r->VaArg(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_CREATE_THREAD:
        if (!r->CreateThread) return 0;
        c->gpr[3] = r->CreateThread(c->gpr[3], c->gpr[4], c->gpr[5],
                                    c->gpr[6], c->gpr[7]);
        return 1;
    case M2_KSEL_GET_TE_READ_POINTER:
        if (!r->GetTEReadPointer) return 0;
        // WaitTEPass first poll (caller pc 0x1b2fc) carries the GS list EA
        // in r3 (prologue preserves it); satisfy the fence from the
        // producer frontier before reading. Spin polls (0x1b310) and other
        // callers skip this (r3 not a list there).
        if (c && c->mem && c->pc == 0x1b2fcu)
            m2_guest_te_satisfy_list(c, c->gpr[3]);
        else if (c && c->mem && c->pc == 0x1c234u) {
            c->gpr[3] = c->gpr[30] & 0x3fffffffu;
            return 1;
        } else if (c && c->mem && c->pc == 0x1c254u) {
            c->gpr[3] = c->gpr[27];
            return 1;
        }
        c->gpr[3] = r->GetTEReadPointer();
        return 1;
    case M2_KSEL_SET_TE_WRITE_POINTER:
        if (!r->SetTEWritePointer) return 0;
        // te.s stores r3 to the IWP MMIO word and returns without modifying
        // r3.  Preserve that exact void-leaf ABI at the dispatch boundary.
        r->SetTEWritePointer(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_SPINLOCK:
        if (!r->CreateSpinLock) return 0;
        // r3 = guest EA of the lock record (record / record+0x20 from the
        // M_InitMP AllocMemMasked#35 carve). 0 = success per the proven
        // `cmpwi r3,0; bne -> -2 error path` in both M_InitMP sites.
        c->gpr[3] = (uint32_t)r->CreateSpinLock(c->gpr[3]);
        return 1;
    case M2_KSEL_DELETE_SPINLOCK:
        if (!r->DeleteSpinLock) return 0;
        // r3 = guest EA of the lock record; all four generated sites discard
        // the return, the status is still routed through gpr[3].
        c->gpr[3] = (uint32_t)r->DeleteSpinLock(c->gpr[3]);
        return 1;
    case M2_KSEL_CREATE_DEVICE_STACK_LIST:
        if (!r->CreateDeviceStackList) return 0;
        // r3 = listOut EA (List** pointer in guest RAM), r4 = desc EA (array of
        // 16-byte DeviceStackDesc records terminated by 0 tag).
        // Allocates 16-byte List struct in guest RAM, initializes as empty
        // (l_Head = list_ea + 8 for ISEMPTYLIST verification), writes list_ea
        // to *listOut, and returns 0 (Err) in r3.
        c->gpr[3] = (uint32_t)r->CreateDeviceStackList(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_DELETE_DEVICE_STACK_LIST:
        if (!r->DeleteDeviceStackList) return 0;
        // r3 = list_ea (the List* from CreateDeviceStackList#67). Frees the
        // 16-byte List struct and returns 0 (Err) in r3; all 8 generated sites
        // discard the return value (each overwrites gpr[3] with an error code or
        // branches on a prior OpenDeviceStack/CreateMPIOReq result in r31).
        c->gpr[3] = (uint32_t)r->DeleteDeviceStackList(c->gpr[3]);
        return 1;
    case M2_KSEL_OPEN_DEVICE_STACK:
        if (!r->OpenDeviceStack) return 0;
        // r3 = ds_ea (the device-stack node EA the 5 generated sites load as
        // *(list_ea+4)); returns 0 (Err) in r3. FMVOpenVideo 0x652a0 and
        // CreateMPIOReq 0x738a8/0x73a84 branch on `>=0` to continue (CreateIOReq
        // ord 170, etc.); OpenTEDevice 0x73f68 returns the result directly;
        // MPS_InitMPEGStill 0x20278 stores it to an out-param (*r29).
        c->gpr[3] = (uint32_t)r->OpenDeviceStack(c->gpr[3]);
        return 1;
    case M2_KSEL_CLOSE_DEVICE_STACK:
        if (!r->CloseDeviceStack) return 0;
        // r3 = ds_ea (the device-stack node EA the 7 generated sites load as
        // documented in the m2fn_CloseDeviceStack_t typedef). Returns 0 (Err)
        // in r3; 6 of 7 sites discard it (each overwrites gpr[3] with a
        // success/error code) and MPS_TeardownMPEGStill 0x20a58 returns it as
        // its own Err, so 0 = success is the contract.
        c->gpr[3] = (uint32_t)r->CloseDeviceStack(c->gpr[3]);
        return 1;
    case M2_KSEL_FLUSH_DCACHE:
        if (!r->FlushDCache) return 0;
        // r4=addr, r5=len; (void) result, gpr[3] left as caller's pre-call value.
        r->FlushDCache(c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_WRITE_BACK_DCACHE:
        if (!r->WriteBackDCache) return 0;
        r->WriteBackDCache(c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_FREE_MEM_DEBUG:
        if (!r->FreeMemDebug) return 0;
        // r3=mem, r4=size, r5=sourceFile EA, r6=sourceLine; source is void,
        // so gpr[3] is left untouched here.
        r->FreeMemDebug(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]);
        return 1;
    case M2_KSEL_STRNCPY:
        if (!r->StrNCpy) return 0;
        // r3=dst, r4=src, r5=cap -> dst lands in r3.
        c->gpr[3] = r->StrNCpy(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_STRCPY: {
        // Route through a local: the member name `strcpy` is a function-like
        // libc macro, so a direct r->strcpy(...) call would macro-expand.
        m2fn_strcpy_t _cpy = r->strcpy;
        if (!_cpy) return 0;
        // r3=dst, r4=src (NUL-terminated guest strings) -> dst lands in r3.
        c->gpr[3] = _cpy(c->gpr[3], c->gpr[4]);
        return 1;
    }
    case M2_KSEL_STRCAT: {
        m2fn_strcat_t _cat = r->strcat;
        if (!_cat) return 0;
        // r3=dst, r4=src -> appended dst lands in r3.
        c->gpr[3] = _cat(c->gpr[3], c->gpr[4]);
        return 1;
    }
    case M2_KSEL_TOUPPER:
        if (!r->ToUpper) return 0;
        c->gpr[3] = r->ToUpper(c->gpr[3]);
        return 1;
    case M2_KSEL_OBTAIN_SPINLOCK:
        if (!r->ObtainSpinLock) return 0;
        c->gpr[3] = (uint32_t)r->ObtainSpinLock(c->gpr[3]);
        return 1;
    case M2_KSEL_RELEASE_SPINLOCK:
        if (!r->ReleaseSpinLock) return 0;
        r->ReleaseSpinLock(c->gpr[3]);
        return 1;
    case M2_KSEL_OPEN_ITEM:
        if (!r->OpenItem) return 0;
        // r3 = item, r4 = optional args; returns Item or negative Err.
        c->gpr[3] = r->OpenItem(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_CLOSE_ITEM:
        if (!r->CloseItem) return 0;
        // r3 = item; source returns Err in r3.
        c->gpr[3] = (uint32_t)r->CloseItem(c->gpr[3]);
        return 1;
    case M2_KSEL_DELETE_ITEM:
        if (!r->DeleteItem) return 0;
        // r3 = item; source returns Err. Recovered callers may discard it.
        c->gpr[3] = (uint32_t)r->DeleteItem(c->gpr[3]);
        return 1;
    case M2_KSEL_CHECK_ITEM:
        if (!r->CheckItemNode) return 0;
        // r3 = item, r4 = subsystem type, r5 = node type; returns Node EA/NULL.
        c->gpr[3] = r->CheckItemNode(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_FIND_NAMED_NODE:
        if (!r->FindNamedNode) return 0;
        // r3 = list, r4 = name; returns node EA or 0.
        c->gpr[3] = r->FindNamedNode(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_UNIVERSAL_INSERT_NODE:
        if (!r->UniversalInsertNode) return 0;
        // r3 = list, r4 = node, r5 = guest comparison callback; void on the
        // guest side.
        r->UniversalInsertNode(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_FIND_TAG_ARG:
        if (!r->FindTagArg) return 0;
        // r3 = tagList, r4 = tag; returns the matching TagArg EA or 0.
        c->gpr[3] = r->FindTagArg(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_GET_MEM_INFO:
        if (!r->GetMemInfo) return 0;
        // r3 = info ptr, r4 = size, r5 = memFlags; void return.
        r->GetMemInfo(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_UNIMPORT_BY_NAME:
        if (!r->UnimportByName) return 0;
        // r3 = folio, r4 = name; returns unimported item or 0.
        c->gpr[3] = r->UnimportByName(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_DEBUG_PUT_STR:
        if (!r->DebugPutStr) return 0;
        r->DebugPutStr(c->gpr[3]);
        return 1;
    case M2_KSEL_DEBUG_PUT_CHAR:
        if (!r->DebugPutChar) return 0;
        r->DebugPutChar(c->gpr[3]);
        return 1;
    // ---- promoted ords (see typedef block for the proven call shapes) --
    case M2_KSEL_PREP_LIST:
        if (!r->PrepList) return 0;
        // r3 = List*; initialize the 16-byte anchor in place; return ignored.
        r->PrepList(c->gpr[3]);
        return 1;
    case M2_KSEL_REM_NODE:
        if (!r->RemNode) return 0;
        // r3 = Node*; unlink from its list (no list pointer in the window).
        r->RemNode(c->gpr[3]);
        return 1;
    case M2_KSEL_ADD_TAIL:
        if (!r->AddTail) return 0;
        // r3 = List*, r4 = Node*; link node as last element; return ignored.
        r->AddTail(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_REM_TAIL:
        if (!r->RemTail) return 0;
        // r3 = List* -> Node* or 0 (0 is the load-bearing empty-list signal).
        c->gpr[3] = r->RemTail(c->gpr[3]);
        return 1;
    case M2_KSEL_ADD_HEAD:
        if (!r->AddHead) return 0;
        // r3 = List*, r4 = Node*; link node as first element; return ignored.
        r->AddHead(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_REM_HEAD:
        if (!r->RemHead) return 0;
        // r3 = List* -> Node* or 0 (callers NULL-test and dereference).
        c->gpr[3] = r->RemHead(c->gpr[3]);
        return 1;
    case M2_KSEL_SAMPLE_SYSTEM_TIME_TT:
        if (!r->SampleSystemTimeTT) return 0;
        // r3 = TimerTicks* out (8-byte BE); return ignored.
        r->SampleSystemTimeTT(c->gpr[3]);
        return 1;
    case M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL:
        if (!r->ConvertTimerTicksToTimeVal) return 0;
        // r3 = TimerTicks*, r4 = TimeVal* out; return ignored.
        r->ConvertTimerTicksToTimeVal(c->gpr[3], c->gpr[4]);
        return 1;
    case M2_KSEL_FLUSH_DCACHE_ALL:
        if (!r->FlushDCacheAll) return 0;
        // Whole-D-cache flush; host-coherent no-op. r3 is the caller's scratch
        // (0 at every one of the 15 sites); leave it untouched like the 51/52
        // cache no-ops do.
        r->FlushDCacheAll();
        return 1;
    case M2_KSEL_CLIB_PRINT_ERROR:
        if (!r->clib_PrintError) return 0;
        // clib_PrintError(who, what, whom, err) is a fixed four-argument void
        // service; preserve the caller's return register.
        r->clib_PrintError(c->gpr[3], c->gpr[4], c->gpr[5],
                           (int32_t)c->gpr[6]);
        return 1;
    case M2_KSEL_PRINTF_SYS_ERR:
        if (!r->PrintfSysErr) return 0;
        // r3 = error code only (no format string in any of the 145 sites);
        // source returns void, so preserve the error word in gpr[3].
        r->PrintfSysErr((int32_t)c->gpr[3]);
        return 1;
    case M2_KSEL_SPRINTF:
        if (!r->Sprintf) return 0;
        // r3 = buf EA, r4 = fmt EA, r5..r10 = varargs -> char count in r3.
        c->gpr[3] = (uint32_t)r->Sprintf(c->gpr[3], c->gpr[4], c->gpr[5],
                                         c->gpr[6], c->gpr[7], c->gpr[8],
                                         c->gpr[9], c->gpr[10], 0);
        return 1;
    case M2_KSEL_IS_MASTER_CPU:
        if (!r->IsMasterCPU) return 0;
        // No args; pure predicate -> 1 (master). All 4 sites cmpwi r3,0.
        c->gpr[3] = (uint32_t)r->IsMasterCPU();
        return 1;
    case M2_KSEL_SUB_TIMER_TICKS:
        if (!r->SubTimerTicks) return 0;
        // r3 = TimerTicks*, r4 = TimerTicks*, r5 = TimerTicks* out; void.
        r->SubTimerTicks(c->gpr[3], c->gpr[4], c->gpr[5]);
        return 1;
    case M2_KSEL_READ_HARDWARE_RANDOM_NUMBER:
        if (!r->ReadHardwareRandomNumber) return 0;
        // No arguments; the single 32-bit word lands in the return register.
        c->gpr[3] = r->ReadHardwareRandomNumber();
        return 1;
    case M2_KSEL_SET_ITEM_PRI:
        if (!r->SetItemPri) return 0;
        // r3 = Item, r4 = uint8 newpri (low byte); previous priority or a
        // negative Err in r3.
        c->gpr[3] = (uint32_t)r->SetItemPri(c->gpr[3], (uint8_t)c->gpr[4]);
        return 1;
    case M2_KSEL_FIND_AND_OPEN_ITEM:
        if (!r->FindAndOpenItem) return 0;
        // r3 = int32 item type (MkNodeID), r4 = guest EA of the TagArg list.
        // The handler reads guest memory through the calling PPCContext, which
        // recomp_call_kernel_dispatch has just made the active one.
        c->gpr[3] = r->FindAndOpenItem((int32_t)c->gpr[3], c->gpr[4]);
        return 1;
    default:
        // Diab compiler prologue/epilogue register save/restore helpers (kernel
    // vector family: _savegpr/restgpr/_savegprs_*_l/_restgprs_*_l/… + fpr
    // groups _savefprs_*_l/_restfprs_*_l). Proven architectural no-ops on the
    // recompiled model (the lift elides them; the host manages its own
    // register state — see diab_runtime.h, LIMITATIONS §2). Faithful typed
    // handling returns 1 and leaves gpr[3] untouched (no fabricated dummy).
    if (m2_is_diab_helper(ordinal)) {
        return 1;
    }
    // Generic bridge: handle the 98 previously-stubbed kernel
        // vectors with a trivial dummy success so the guest does not spin.
        if (m2_is_generic_kernel_stub(ordinal)) {
            return m2_handle_generic_kernel(c, ordinal);
        }
        return 0;
    }
}

// Kernel handlers that consume guest varargs must read the stack belonging to
// the PPC context that made this selector call.  Guest tasks have separate
// register contexts while sharing the same bounded RAM image, so scope that
// context around the typed dispatch and restore nested callers on return.
static inline int recomp_call_kernel_dispatch(PPCContext* c, uint32_t folio,
                                              uint32_t ordinal, uint32_t pc,
                                              const m2_kernel_dispatch* r) {
    if (!c || folio != M2_FOLIO_KERNEL) return 0;
    PPCContext* previous = m2_guest_kernel_get_active_context();
    m2_guest_kernel_set_active_context(c);
    const int handled = recomp_call_kernel_dispatch_impl(c, folio, ordinal,
                                                          pc, r);
    m2_guest_kernel_set_active_context(previous);
    m2_selector_note(folio, ordinal, handled);
    return handled;
}

// Trace hooks — weak C ABI, strong impl in host/diagnostics/trace.cpp
// when m2host links it. Standalone m2recomp tests link without host trace, so
// calls are guarded by null-check (weak symbol resolves to 0 when absent).
extern void m2_trace_selector_call_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                                     uint32_t gpr3, uint32_t gpr4) __attribute__((weak));
extern void m2_trace_selector_return_c(uint32_t folio, uint32_t ordinal, uint32_t pc,
                                       uint32_t ret) __attribute__((weak));
extern void m2_trace_guest_enter_c(uint32_t pc, const char* sym) __attribute__((weak));
extern void m2_trace_guest_exit_c(uint32_t pc, const char* sym, uint32_t ret) __attribute__((weak));
extern void m2_trace_frame_complete_c(uint32_t seq, uint32_t pc, uint32_t state) __attribute__((weak));
extern void m2_trace_state_transition_c(uint32_t from_state, uint32_t to_state, uint32_t state_ea, uint32_t pc) __attribute__((weak));
// Typed audio boundary. The strong implementation lives in
// host/audio/audio_shim.cpp; standalone recomp supplies the same fixed-width
// ownership/state portion through the weak runtime fallback and leaves DSP
// resource execution and playback unresolved.
extern int m2_audio_dispatch_c(PPCContext* c, uint32_t ordinal,
                               uint32_t pc) __attribute__((weak));

// Process-wide registry consulted by recomp_call_selector inside RECOMP_BUILD
// TUs. Defined once in src/recomp_runtime.c so every generated TU shares it.
// Starts empty (= generic-only); set_global(NULL) restores that state.
m2_kernel_dispatch* m2_kernel_dispatch_global(void);
void m2_kernel_dispatch_set_global(const m2_kernel_dispatch* r);

// ---- guest-kernel handler set + binding (src/recomp_runtime.c) -----------
// m2_guest_kernel_handlers() returns a fully populated dispatch template:
// every gate-family slot bound to the shared production handlers that run
// against the state installed by m2_guest_kernel_bind(). NULL slots (printf,
// AllocMem/FreeMem, __itof — owned elsewhere) stay NULL. Registrants copy the
// template into their registry and validate via m2_kernel_dispatch_register.
//
// bind() attaches the kernel context and the CPU context whose `mem` is the
// shared guest RAM image; unbind() resets every pool to the freshly-bound
// state and detaches. Binding is required: with no binding the handlers fail
// closed (M2K_ERR_BAD_ITEM / M2K_ITEM_NONE).
struct m2k_context;
const m2_kernel_dispatch* m2_guest_kernel_handlers(void);
int  m2_guest_kernel_bind(struct m2k_context* kctx, PPCContext* cpu);
void m2_guest_kernel_unbind(void);

// Kernel exit#8 task-local guard. A guard is one bounded setjmp frame that
// encloses a whole guest task body (or the synchronous boot fallback); the
// typed Exit handler unwinds to the innermost guard on the calling thread
// instead of terminating the host process. `fn` runs inside the guard frame,
// so longjmp back into it is defined.
//
// m2_guest_exit_guard_run: returns 0 when fn returned normally; returns 1 when
// exit#8 unwound the call, with the guest status in *status_out.
// m2_guest_exit_unwind: called by the Exit handler. Returns 0 when no guard is
// active on this thread (the caller records the request and returns, so the
// host process is never terminated). Otherwise it does not return.
int  m2_guest_exit_guard_run(int32_t (*fn)(PPCContext*, void*), PPCContext* c,
                             void* user, int32_t* status_out);
int  m2_guest_exit_unwind(int32_t status);
// Last unguarded exit#8 request (research/diagnostic observation): returns 1
// and fills *status_out when a guest exit reached the handler outside any
// task guard. The flag is sticky until m2_guest_exit_pending_clear().
int  m2_guest_exit_pending(int32_t* status_out);
void m2_guest_exit_pending_clear(void);

// Extra state for savestate (H): heap_next + StateDriver ctx (r27 etc.)
size_t m2_guest_extra_state_size(void);
int m2_guest_extra_state_save(uint8_t* buf, size_t cap, size_t* out_len);
int m2_guest_extra_state_load(const uint8_t* buf, size_t len);

// Guest AllocMem-family page-pool geometry and prefix accounting, as the
// production first-fit allocator uses them. A focused gate reads these rather
// than restating the numbers, so a geometry or tracking-prefix change cannot
// leave a reference model comparing against stale constants.
typedef struct m2_guest_heap_window {
    uint32_t base;       // lowest address the pool may hand out
    uint32_t limit;      // exclusive upper bound (== guest RAM size)
    uint32_t page_size;  // page-pool page granularity
    uint32_t align;      // returned-address alignment
} m2_guest_heap_window;

void     m2_guest_heap_geometry(m2_guest_heap_window* out);
// Bytes reserved ahead of the returned pointer: the tracking word for
// MEMTYPE_TRACKSIZE, a full FreeBlock when a mask must also be satisfied, else 0.
uint32_t m2_guest_heap_track_prefix(uint32_t mem_flags, uint32_t care_bits);

// First-fit search work counters, cumulative since process start. A search is
// required to visit a bounded number of live records per call, so total visits
// grow linearly with allocation count. This is what lets a focused gate pin the
// search's complexity class without a load-sensitive timing assertion.
typedef struct m2_guest_heap_scan_cost {
    uint64_t searches;       // find_start calls
    uint64_t record_visits;  // live records examined across all calls
} m2_guest_heap_scan_cost;

void m2_guest_heap_scan_stat(m2_guest_heap_scan_cost* out);

// Live AllocMem-family allocations. gk_FreeMem writes nothing into guest RAM,
// so this is the observable for "an owned allocation was actually released",
// and a second release of the same block leaves it unchanged.
uint32_t m2_guest_heap_live_blocks(void);

// Typed uint32 selector invocation (defined in src/recomp_runtime.c): runs
// the globally registered typed handler for (folio, ord) and writes its u32
// result into c->gpr[3]. Returns 1 when a typed handler fired; otherwise it
// performs the same generic lookup fallback as recomp_call_selector and
// returns 0. Unimplemented selectors fail closed: c->gpr[3] is forced to 0
// (Item-none) so guest callers observe a clean failure instead of a stale
// argument register (see recomp_call_selector).
int recomp_call_selector_u32(PPCContext* c, uint32_t folio, uint32_t ord,
                             uint32_t pc);

// Shim dispatch wrapper — generated bl sites call through this so we keep a
// single call-site shape regardless of the underlying folio prototype.
//
// Fail-closed rule: when no typed handler covers (folio, ord), the call is an
// unimplemented selector. The looked-up shim fn is still NOT invoked (typed
// marshalling is deferred — calling blind would be a prototype mismatch), but
// c->gpr[3] is forced to 0 so the EABI return register reads as Item-none /
// "not found". Leaving the caller's r3 untouched made guests that poll a
// selector result spin forever on a register that never changes (observed:
// FindMicroCard's dir-enumeration loop after the StateDriver lift reached
// hs_loadhighscores). Zero satisfies both Opera result conventions that gate
// control flow here: `== 0` handle checks take their absent/error path, and
// enumerate-until-negative loops are unreachable because every preceding
// open/find in the chain also reports 0.
// Typed folio-4 (filesystem) dispatch — OpenFile/CloseFile plus the bounded
// directory-enumeration and raw-file windows (see filesystem_folio.h and the
// recovered call sites). Defined in recomp_runtime.c; returns 1 when ord is
// handled, 0 to fall through fail-closed.
int recomp_call_fs_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc);
// Typed folio-2 (IFF) dispatch — the bounded ScanAIFF property/context
// window used by the recovered SoundPlayer path. Defined in recomp_runtime.c;
// returns 1 when the observed selector contract is handled.
int recomp_call_iff_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc);
// Typed folio-3 (graphics) dispatch — the source-backed
// ModifyGraphicsItemVA(BMTAG_BUFFER) publication boundary (vector 1) plus the
// recovered view-list window: AddViewToViewList (vector 3) and RemoveView
// (vector 5) with view.c validation, default-projector membership, and the
// exact GFX_ERR_* identities. Other graphics selectors and tag shapes remain
// fail-closed until their call contracts are recovered.
int recomp_call_graphics_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc);
// Typed folio-5 (audiopatch) dispatch — CreatePatchTemplate (#0) constructs a
// custom Patch Template from a PatchCmd list.
int recomp_call_audiopatch_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc);

// Typed folio-7 MPEG-audio boundary. The recovered game calls the four
// mpegaudiodecoder exports directly through R_PPC_IMPREL24 imports:
// CreateMPAudioDecoder(#1), DeleteMPAudioDecoder(#2), MPAFlush(#3), and
// MPAudioDecode(#0). The host executable supplies the FFmpeg-backed handler;
// the recomp library keeps a weak fail-closed fallback for standalone tests.
int m2_mpegaudio_dispatch(PPCContext* c, uint32_t ordinal, uint32_t pc);

// Bounded movie-path trace (`M2_MPEG_TRACE`). Logs the folio call sequence
// (and return) from the VideoPlayer entry window so the earliest failing
// dependency in the recovered VideoStream setup stays identifiable; the
// window state lives in recomp_runtime.c so calls from every generated
// translation unit share one trigger and one bounded budget.
int m2_mpeg_sel_trace_call(PPCContext* c, uint32_t folio, uint32_t ord,
                           uint32_t pc) __attribute__((weak));
void m2_mpeg_sel_trace_ret(uint32_t ord, uint32_t pc, uint32_t r3)
    __attribute__((weak));

#if defined(RECOMP_BUILD)
// The driving image contains the same source-built GState routines the
// shooter imports from the standalone GState folio. Route only the ordinals
// implemented by that lifted source through this adapter.
int recomp_call_gstate_dispatch(PPCContext* c, uint32_t ord, uint32_t pc);
// font folio (id 9) — src/font_folio.c. CreateTextState/DeleteTextState/
// DrawText over the Font Items kernel:133 FindAndOpenItem loaded.
int recomp_call_font_dispatch(PPCContext* c, uint32_t ord, uint32_t pc);
// kernel:133 FindAndOpenItem handler body, installed in the shared registry.
uint32_t m2_font_find_and_open_item(int32_t ctype, uint32_t taglist_ea);
// Release a font Item's guest-visible payload; called from the kernel
// DeleteItem handler so an Item cannot outlive its font image copy.
void m2_font_release_item(uint32_t item);
#endif

static inline void recomp_call_selector_inner(PPCContext* c, uint32_t folio, uint32_t ord, uint32_t pc) {
#if defined(RECOMP_BUILD)
    const int light_object_watch = m2_light_memory_trace_enabled();
    const uint32_t light_before18 = light_object_watch ? rd32(c, 0x93ab18u) : 0;
    const uint32_t light_before24 = light_object_watch ? rd32(c, 0x93ab24u) : 0;
    // Trace: emit selector_call before any handler runs
    if (m2_trace_selector_call_c) m2_trace_selector_call_c(folio, ord, pc, c->gpr[3], c->gpr[4]);
    // Optional stderr trace for StateDriver boot diagnosis
    {
        static int _m2_trace_cnt = 0;
        static int _m2_trace_enabled = -1;
        if (_m2_trace_enabled == -1) {
            const char* e = getenv("M2_TRACE");
            _m2_trace_enabled = (e && (e[0]=='1' || e[0]=='y' || e[0]=='Y')) ? 1 : 0;
        }
        if (_m2_trace_enabled && _m2_trace_cnt < 5000) {
            fprintf(stderr, "[trace] folio=%u ord=%u pc=%#x gpr3=%#x gpr4=%#x\n", folio, ord, pc, c->gpr[3], c->gpr[4]);
            _m2_trace_cnt++;
            if (_m2_trace_cnt == 5000) fprintf(stderr, "[trace] (further selector calls suppressed)\n");
        }
    }
    // StateDriver window: emit first 200 calls with pc in 0x02fa4c..0x031718 even past 300-limit
    {
        static int _sd_cnt = 0;
        static int _sd_enabled = -1;
        if (_sd_enabled == -1) {
            const char* e = getenv("M2_TRACE");
            _sd_enabled = (e && (e[0]=='1' || e[0]=='y' || e[0]=='Y')) ? 1 : 0;
        }
        if (_sd_enabled && pc >= 0x02fa4c && pc <= 0x031718 && _sd_cnt < 200) {
            fprintf(stderr, "[sd-trace] folio=%u ord=%u pc=%#x gpr3=%#x gpr4=%#x\n", folio, ord, pc, c->gpr[3], c->gpr[4]);
            _sd_cnt++;
        }
    }
    // Typed fast path: fires only when a handler is installed globally.
    if (recomp_call_kernel_dispatch(c, folio, ord, pc, m2_kernel_dispatch_global())) {
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // GState's lifted source is already present in the driving image. This
    // typed bridge gives shooter2d the exact shared service routines while
    // preserving their guest-side allocations, IO, and command-list behavior.
    if (folio == M2_FOLIO_GSTATE && recomp_call_gstate_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed font folio boundary (folio 9): CreateTextState / DeleteTextState /
    // DrawText for the Font Items loaded through kernel:133 FindAndOpenItem.
    if (folio == M2_FOLIO_FONT && recomp_call_font_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed filesystem path (folio 4): OpenFile/CloseFile plus the bounded
    // directory/raw-file windows.
    if (folio == M2_FOLIO_FILESYSTEM && recomp_call_fs_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed IFF path: ScanAIFF parses the source CBD2 AIFF-C headers into
    // guest-visible ContextNode/PropChunk/ContextInfo records. Other IFF
    // parser services remain on the fail-closed path until their call shape
    // is recovered.
    if (folio == M2_FOLIO_IFF && recomp_call_iff_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed graphics boundary: ModifyGraphicsItemVA currently covers the
    // recovered bitmap-buffer publication shape only. View and other tag
    // processors remain unresolved and fail closed below.
    if (folio == M2_FOLIO_GRAPHICS && recomp_call_graphics_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed audiopatch boundary (folio 5): CreatePatchTemplate (#0) constructs
    // an instrument template from a PatchCmd list.
    if (folio == M2_FOLIO_AUDIOPATCH && recomp_call_audiopatch_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Typed MPEG-audio module boundary (folio 7). This is a platform service
    // boundary for the original subscriber, not replacement guest logic.
    if (folio == M2_FOLIO_MPEGAUDIODECODER &&
        m2_mpegaudio_dispatch(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Bounded audio-folio-6 trigger trace. Default-off (env gate),
    // capped at 64 hits per run. Logs (ord, caller pc, r3, r4, state, frame)
    // so we can identify which audio calls fire under natural guest execution
    // and from which callers. No behavior change — observation only.
    if (folio == M2_FOLIO_AUDIO) {
        static uint32_t _audio_trigger_emitted = 0;
        if (_audio_trigger_emitted < 256u) {
            const char* _at_env = getenv("M2_AUDIO_TRIGGER");
            if (_at_env && (_at_env[0] == '1' || _at_env[0] == 'y' || _at_env[0] == 'Y')) {
                uint32_t _at_primary = 0, _at_secondary = 0, _at_frame = 0;
                if (c && c->mem) {
                    if (RECOMP_GUEST_MEM > 0x1877d8u + 4u)
                        _at_primary = rd32(c, 0x1877d8u);
                    if (RECOMP_GUEST_MEM > 0x1877dcu + 4u)
                        _at_secondary = rd32(c, 0x1877dcu);
                    if (RECOMP_GUEST_MEM > M2_GUEST_FRAME_COUNTER_EA + 4u)
                        _at_frame = rd32(c, M2_GUEST_FRAME_COUNTER_EA);
                }
                ++_audio_trigger_emitted;
                fprintf(stderr,
                        "[audio-trigger] n=%u ord=%u pc=%#x r3=%#x r4=%#x "
                        "state=(%u,%u) frame=%u\n",
                        _audio_trigger_emitted, ord, pc, c->gpr[3], c->gpr[4],
                        _at_primary, _at_secondary, _at_frame);
            }
        }
    }
    // Typed audio boundary: source-backed Item selectors share the local
    // fixed-width item namespace, while status selectors route to their
    // ordinal-specific retained Item/attachment/trigger/knob/graph handlers.
    // Full FDMA/DSPP breadth remains open, but recovered resource/sample and
    // playback side effects do cross this path.
    if (folio == M2_FOLIO_AUDIO && m2_is_audio_typed_selector(ord) &&
        m2_audio_dispatch_c && m2_audio_dispatch_c(c, ord, pc)) {
        m2_selector_note(folio, ord, 1);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
    // Fail-safe branch. No game-called audio ordinal is a generic stub, so
    // this predicate is empty in practice; the guard stays fail-visible in
    // case one ever is.
    if (folio == M2_FOLIO_AUDIO && m2_is_generic_audio_stub(ord)) {
        m2_handle_generic_audio(c, ord);
        m2_trace_light_object_change(c, folio, ord, pc, light_before18, light_before24);
        if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
        return;
    }
#endif
    void* fn = m2_selector_lookup(folio, ord);
    (void)fn; (void)pc;
    m2_selector_note(folio, ord, 0);
    c->gpr[3] = 0;   // fail closed: unimplemented selector -> Item-none
    if (m2_trace_selector_return_c) m2_trace_selector_return_c(folio, ord, pc, c->gpr[3]);
}

extern void m2_diagnostic_checkpoint(uint32_t pc) __attribute__((weak));
static inline void recomp_call_selector(PPCContext* c, uint32_t folio, uint32_t ord, uint32_t pc) {
    m2_guest_kernel_note_boundary_sel(folio, ord);
    if (m2_diagnostic_checkpoint) m2_diagnostic_checkpoint(pc);    const int traced =
        (m2_mpeg_sel_trace_call != 0) &&
        m2_mpeg_sel_trace_call(c, folio, ord, pc);
    recomp_call_selector_inner(c, folio, ord, pc);
    if (traced && m2_mpeg_sel_trace_ret != 0)
        m2_mpeg_sel_trace_ret(ord, pc, c->gpr[3]);
}

// ---- 32-bit guest TagArg pairs -------------------------------------------
// Guest tag lists are arrays of {tag, value} u32 pairs terminated by TAG_END
// (= 0). Helpers below are bounds-checked and reject NULL input; they never
// read past list[max_count - 1].

#define M2_TAG_END 0u
#define M2_TAG_JUMP 0xfffffffeu

typedef struct m2_tag_arg {
    uint32_t tag;
    uint32_t value;
} m2_tag_arg;

// Bounded iterator step: returns the index of the next live (non-END) entry
// after `cursor` (-1 starts at index 0), or -1 once out of bounds, at the
// M2_TAG_END terminator, or when list is NULL.
static inline int32_t m2_tag_iter_next(const m2_tag_arg* list,
                                       uint32_t max_count, int32_t cursor) {
    if (!list) return -1;
    int32_t i = cursor < 0 ? 0 : cursor + 1;
    if (i >= 0 && (uint32_t)i < max_count && list[i].tag != M2_TAG_END)
        return i;
    return -1;
}

// Bounded find built on the iterator: returns the index of the first entry
// whose tag matches, or -1 for NULL input, no match within max_count, or a
// match hidden behind the M2_TAG_END terminator.
static inline int32_t m2_tag_find(const m2_tag_arg* list, uint32_t max_count,
                                  uint32_t tag) {
    if (!list) return -1;
    for (int32_t i = m2_tag_iter_next(list, max_count, -1); i >= 0;
         i = m2_tag_iter_next(list, max_count, i)) {
        if (list[i].tag == tag) return i;
    }
    return -1;
}

// Host-sim switch query (recomp_entry.c)
int m2_guest_main_query(int32_t* state_out, int32_t* status_out);

// Guest-owned frame tick (generated StateDriver L2fb88)
// Incremented at the proven StateDriver loop head (0x2fb88), records pc and
// primary state (BSS 0x1877d8), emits [guest_trace] event and snapshot.
// Host reads via m2_guest_frame_query; never mutates guest memory.
void m2_guest_frame_tick(PPCContext* c, uint32_t pc);
// Direct shooter boot uses its original main loop, with a host-private
// observer counter. It never reads or writes the driving StateDriver carve.
void m2_guest_shooter_frame_tick(PPCContext* c, uint32_t pc);
uint32_t m2_guest_current_frame_c(void);
int m2_guest_frame_query(uint32_t* seq_out, uint32_t* pc_out, uint32_t* state_out,
                         uint32_t* snapshot_out, uint32_t snapshot_words);

// Capture-only host pacing. This is a harness boundary, not an emulation of
// an original guest signal: when enabled, the StateDriver waits on the
// host-private bit below at each loop head, and publishes the last completed
// guest frame there before waiting. The host can therefore permit one guest
// application frame and pump its CLT submissions only after that frame's
// work has returned to the loop head. Disabled by default.
#define M2_GUEST_FRAME_PACING_SIGNAL_BIT 5
void m2_guest_frame_pacing_set(int enabled);
int m2_guest_frame_pacing_enabled(void);
uint32_t m2_guest_frame_pacing_completed(void);
// Checkpoint park epoch: number of loop-head parking events
// since the last m2_guest_frame_pacing_set. When it advances, the guest task
// is blocked at a quiescent frame boundary safe for save/load.
uint32_t m2_guest_frame_park_count(void);

// Host VBL bridge; the source-backed wait contract is recorded in
// the compatibility signal-wait notes.
// Host-owned pacing signal that unblocks the guest StateDriver's
// WaitSignal(mask 0x10, bit 4) at 0x30f40. Host calls once per host frame;
// guest consumes via m2k_wait_signal(mask 0x10). The dynamic loader mask at
// 0x3110c remains outside this VBL bridge. Returns
// M2K_OK (0) when delivered, negative when no guest task is live.
int m2_guest_signal_vbl(void);
// Deliver one host VBL to the source-shaped timer device. The host entrypoint
// calls this before distributing the same event to guest VBL signal waiters.
int m2_guest_timer_vbl_tick(void);
// Deliver the same host VBL to guest GState video-sync signals registered at
// the source GS_SendList/GS_SendIO WaitSignal sites. Internal host bridge;
// callers normally use m2_guest_signal_vbl().
int m2_guest_signal_gstate_vbl(void);
// Guest main argc/argv delivery. Stages the host-provided argument vector
// into guest-visible memory; the first guest task created afterwards (the
// game's main task) receives r3=argc / r4=argv, matching the original OS
// launch contract consumed by guest main → Parse_Args (-V/-T boot args).
// Call before m2_game_entry(). Pass 0/NULL to clear. No effect on guest tasks
// created before staging or after consumption.
void m2_guest_set_main_args(int argc, char* const* argv);

// Peek/consume the staged main args without creating a task: used by the
// synchronous fallback path in m2_game_entry. Consume clears delivery so a
// later CreateThread cannot double-deliver.
uint32_t m2_guest_main_args_peek(uint32_t* argv_ea);
void m2_guest_main_args_consume(void);

// Checkpoint split signals: VBL bit only (no pacing permit)
// and pacing permit only (no VBL). Zero behavior change when unused; lets the
// checkpoint probe park the guest at a loop head and release single frames.
int m2_guest_signal_vbl_only(void);
int m2_guest_grant_frame_permit(void);

// Host ControlPort input bridge
// Select the guest-facing controller type before boot: 0 = Control Pad
// (digital, default), 1 = analog Joystick. The Event Broker advertises one
// Stick only for the analog type, so AssignInputs chooses the matching guest
// input path independently of the saved bindings.
void m2_guest_set_controller_type(int analog);
// Nonzero while shooter2d is the running application, however it was reached.
int m2_guest_shooter_active(void);
// Translates host buttons / axes to 3DO ControlPort encoding and writes
// to the guest event broker buffer (GK_BROKER_INPUT_DATA_EA).
void m2_guest_input_set_cport(uint32_t buttons, uint8_t stick_x, uint8_t stick_y);
// Live host delivery retains rising edges until the guest completes its next
// GetCPortData semaphore transaction. This prevents a down/up pair between
// guest polls from collapsing to an unobservable released level.
void m2_guest_input_set_cport_edges(uint32_t buttons, uint32_t pressed,
                                    uint8_t stick_x, uint8_t stick_y);

// Visited primary and secondary state bitmasks
uint32_t m2_guest_visited_primary_mask(void);
uint32_t m2_guest_visited_secondary_mask(void);

#ifdef __cplusplus
}
#endif
