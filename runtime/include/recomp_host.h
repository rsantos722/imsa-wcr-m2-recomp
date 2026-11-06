// recomp_host.h — host-side accessors into libm2recomp.
//
// Savestates need the live guest machine without going through the boot
// dispatch. m2_recomp_context() lazily performs the same setup as
// m2_game_entry()'s guest_mem_init() (context alloc, 64 MiB guest RAM,
// text/data image copies at the synthetic link bases, FTZ/DAZ clear per
// the determinism policy in ppc_ctx.h) but never dispatches into guest code — safe to call before or
// instead of the boot path (tests, tools).
#pragma once

#include "ppc_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live guest CPU context + guest RAM image. Lazily initialized; never NULL.
// Set the game data directory containing driving/game, shooter2d, etc.
void m2_recomp_set_game_root(const char* root);
const char* m2_recomp_get_game_root(void);

// Live guest CPU context + guest RAM image. Lazily initialized; never NULL.
PPCContext* m2_recomp_context(void);

// Host-only access to the static guest-kernel context after
// m2_guest_prepare_restore() or normal guest installation. The pointer never
// crosses into guest registers; it exists for savestate/contract tests that
// need to inspect the fixed-width Message/port/IOReq pools.
struct m2k_context* m2_guest_kernel_context(void);

// Whether the PRODUCTION guest-kernel registry installs this ordinal. The gate
// boundary installs m2_guest_kernel_handlers() (the template every typed
// handler writes itself into), while a boot installs the subset listed in
// recomp_entry.c's g_guest_kernel_ordinals. A selector implemented in the
// template but absent from that list is therefore live in every focused test
// and dead in every boot, and no test that installs the template can see it.
int m2_recomp_kernel_ordinal_registered(uint32_t ordinal);

// Prepare a fresh process for a checkpoint restore without spawning the
// guest's boot task. This installs the guest kernel against the initialized
// image; the M2EX loader then reconstructs task handles and attaches only
// proven STARTED/continuation records.
int m2_guest_prepare_restore(void);

// Set the statically linked executable whose module Item is the current
// host-side boot context. Guest tasks retain their own module identity.
void m2_guest_set_boot_module(uint32_t module_slot, uint32_t module_item);
// Same effect as m2_guest_set_boot_module without touching the calling thread's
// TLS module identity. Used when the publisher is a guest task still
// executing inside its own image (the imsa_shell launcher).
void m2_guest_set_boot_module_host(uint32_t module_slot, uint32_t module_item);
// Resolve a guest file name against the calling task's inherited directory.
int m2_guest_resolve_file_path(const char* path, char* out, uint32_t capacity);
// Adopt a guest-created application task as the one the host frame pump and
// VBL bridge follow. The guest kernel calls this from the CreateTask/
// CreateTaskVA boundary, so the task is the one the guest itself asked for
// (under the imsa_shell boot, the launcher's), not a host-chosen handle.
// `module_slot` selects the active executable's frame observer, exactly as
// m2_guest_set_boot_module does, and the publisher's own current-module
// identity is left untouched. imsa_shell is never published: it is the
// launcher, not an application.
void m2_guest_publish_main_task(uint32_t module_slot, uint32_t task,
                                uint32_t module_item);
// Active executable's frame observer; driving uses its retained diagnostic
// carve, direct shooter boot uses the original-main-loop host counter.
uint32_t m2_guest_current_frame_c(void);

// Publish the restored guest main-task handle to the host frame/VBL bridge.
// The handle is guest-visible scalar state; the host uses it only for query
// and platform signal delivery.
void m2_guest_restore_main_task(uint32_t task);

// Bounded lifecycle for the selected main task spawned by m2_game_entry(): the
// task is queried, joined only when already terminal. If guest StateDriver
// is still running it is reported live and the guest kernel context is left
// fully intact — no cancellation, no destroy under a live task. Call once
// after the host frame loop, before host service shutdown completes.
void m2_recomp_shutdown(void);

// Number of guest tasks still STARTED/RUNNING. Used by the host at process
// exit: while any guest task can still call a host service, running static
// destructors would free that service under a live thread, so the process must
// terminate without host static teardown instead.
int m2_guest_live_task_count(void);

#ifdef __cplusplus
}
#endif
