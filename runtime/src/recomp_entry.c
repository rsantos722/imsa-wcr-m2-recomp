// recomp_entry.c — statically linked guest images enter through m2_game_entry.
// M2_BOOT_APP selects `driving/game` (default) or `shooter2d`; both images
// share guest RAM and the typed module/task boundary.
//
// ---------------------------------------------------------------------------
// LaunchMe init order this entry implements (skip the ARM system side; use
// LaunchMe.m2 only as documentation). Recovered by disassembling
// LaunchMe.m2's `main` (imsa_shell.c, 332 bytes) + its kernel-folio selector
// calls; the primary-flow strings are "msgport", "driving", and "game":
//
// The same launcher contains "../shooter2d" and "about to create shooter2d
// task" strings for a companion application. The host's bounded OpenModule /
// CreateTask handlers resolve the two checked-in images without executing the
// ARM shell; M2_BOOT_APP is the documented direct-boot diagnostic control.
//
//   1. OpenGraphicsFolio()            local helper; device stack via .imp3do
//   2. iff:17 FindContextInfo(0)      context probe
//   3. CreateTEIOReq/DeleteTEIOReq    task-event IO channel sanity round-trip
//   4. CreateMsgPort("msgport")       parent<->child signaling; on failure
//                                     printf("ERROR: unable to create message
//                                     port\n") + exit(-1)
//   5. ChangeDirectory("driving")     cwd into the game tree
//   6. OpenModule("game")             load the PPC game module (driving/game)
//   7. LookupItem(...) + CreateTaskVA spawn the game task wired to the port
//   8. WaitPort / WaitSignal          shell idles until the game task exits
//   9. CloseModule + epilogue         teardown (_restgprs_26_l)
//
// Host equivalents (all done before m2_game_entry is called, see main.cpp):
//   1 -> ICelBlitterBackend/GL window   4 -> EventBroker ring
//   3 -> broker.init()                  5 -> FilesystemShim root
//   6 -> typed OpenModule resolves one of the statically linked images
//   7 -> typed CreateTask resolves that image's main entry (guest task; child
//        tasks spawn through CreateThread/CreateThreadVA)
//   8 -> main.cpp frame/sim loop        9 -> shutdown sequence in main.cpp
// ---------------------------------------------------------------------------

#include "ppc_ctx.h"
#include "recomp_host.h"
#include "recomp_helpers.h"
#include "guest_kernel.h"
#include "diagnostic_gate.h"
#include "shim_abi.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "sha256.h"
#include "utf8_path.h"

// Generated data image (game_data.c).
extern unsigned g_recomp_text_base;
extern unsigned g_recomp_data_base;
extern unsigned g_recomp_bss_base;
extern unsigned g_recomp_text_size;
extern unsigned g_recomp_data_size;
extern unsigned g_recomp_bss_size;
extern unsigned g_recomp_text_file_offset;
extern unsigned g_recomp_data_file_offset;
extern unsigned long long g_recomp_image_size;
extern const uint8_t g_recomp_image_sha256[32];
extern unsigned g_recomp_text_fixup_count;
extern const uint32_t g_recomp_text_fixups[][2];
extern unsigned g_recomp_data_fixup_count;
extern const uint32_t g_recomp_data_fixups[][2];

// Generated shooter2d data image (shooter2d_data.c).
extern unsigned g_shooter2d_recomp_text_base;
extern unsigned g_shooter2d_recomp_data_base;
extern unsigned g_shooter2d_recomp_bss_base;
extern unsigned g_shooter2d_recomp_text_size;
extern unsigned g_shooter2d_recomp_data_size;
extern unsigned g_shooter2d_recomp_bss_size;
extern unsigned g_shooter2d_recomp_text_file_offset;
extern unsigned g_shooter2d_recomp_data_file_offset;
extern unsigned long long g_shooter2d_recomp_image_size;
extern const uint8_t g_shooter2d_recomp_image_sha256[32];
extern unsigned g_shooter2d_recomp_text_fixup_count;
extern const uint32_t g_shooter2d_recomp_text_fixups[][2];
extern unsigned g_shooter2d_recomp_data_fixup_count;
extern const uint32_t g_shooter2d_recomp_data_fixups[][2];

// Generated imsa_shell data image (launchme_data.c)
extern unsigned g_launchme_recomp_text_base;
extern unsigned g_launchme_recomp_data_base;
extern unsigned g_launchme_recomp_bss_base;
extern unsigned g_launchme_recomp_text_size;
extern unsigned g_launchme_recomp_data_size;
extern unsigned g_launchme_recomp_bss_size;
extern unsigned g_launchme_recomp_text_file_offset;
extern unsigned g_launchme_recomp_data_file_offset;
extern unsigned long long g_launchme_recomp_image_size;
extern const uint8_t g_launchme_recomp_image_sha256[32];
extern unsigned g_launchme_recomp_text_fixup_count;
extern const uint32_t g_launchme_recomp_text_fixups[][2];
extern unsigned g_launchme_recomp_data_fixup_count;
extern const uint32_t g_launchme_recomp_data_fixups[][2];

// Generated guest entry points (game_text_*.c, shooter2d_text_*.c). Declared
// here rather than through the generated headers so this file also compiles
// in builds without game data.
void FN_main__1ef6c(PPCContext* c);
void FN_shooter2d_main__002a0(PPCContext* c);

static PPCContext* g_ctx = NULL;
static char g_imsa_game_data_root[1024] = {0};
static int g_launchme_installed = 0;

void m2_recomp_set_game_root(const char* root) {
    if (root && *root) {
        snprintf(g_imsa_game_data_root, sizeof(g_imsa_game_data_root), "%s", root);
    }
}

const char* m2_recomp_get_game_root(void) {
    if (g_imsa_game_data_root[0] != '\0') return g_imsa_game_data_root;
    const char* env = getenv("IMSA_DATA_DIR");
    if (!env || !*env) env = getenv("GAME_ROOT");
#ifdef IMSA_DEFAULT_DATA_DIR
    if (!env || !*env) env = IMSA_DEFAULT_DATA_DIR;
#endif
    return (env && *env) ? env : "";
}

static int m2_find_image_file(const char* rel_path, char* out_path, size_t out_cap) {
    const char* env_imsa = getenv("IMSA_DATA_DIR");
    const char* env_game = getenv("GAME_ROOT");
    const char* candidates[8];
    int count = 0;
    if (g_imsa_game_data_root[0]) {
        // An explicit --game-data root is authoritative. Do not mix images
        // from a stale build path when the requested root is incomplete.
        candidates[count++] = g_imsa_game_data_root;
    } else {
        if (env_imsa && *env_imsa) candidates[count++] = env_imsa;
        if (env_game && *env_game) candidates[count++] = env_game;
#ifdef IMSA_DEFAULT_DATA_DIR
        candidates[count++] = IMSA_DEFAULT_DATA_DIR;
#endif
        candidates[count++] = ".";
        candidates[count++] = "game-data";
        candidates[count++] = "../game-data";
    }

    for (int i = 0; i < count; ++i) {
        snprintf(out_path, out_cap, "%s/%s", candidates[i], rel_path);
        FILE* f = m2_fopen_utf8(out_path, "rb");
        if (f) {
            fclose(f);
            return 0;
        }
    }
    return -1;
}

static int m2_guest_image_install_from_file(
    const char* label,
    const char* rel_path,
    uint32_t text_base, uint32_t text_size, uint32_t text_off,
    uint32_t data_base, uint32_t data_size, uint32_t data_off,
    uint32_t bss_base, uint32_t bss_size,
    uint64_t expected_size, const uint8_t* expected_sha256,
    unsigned text_fixup_count, const uint32_t text_fixups[][2],
    unsigned data_fixup_count, const uint32_t data_fixups[][2])
{
    if (text_base > RECOMP_GUEST_MEM ||
        text_size > RECOMP_GUEST_MEM - text_base ||
        data_base > RECOMP_GUEST_MEM ||
        data_size > RECOMP_GUEST_MEM - data_base ||
        bss_base > RECOMP_GUEST_MEM ||
        bss_size > RECOMP_GUEST_MEM - bss_base) {
        fprintf(stderr, "[recomp] %s image exceeds guest RAM\n", label);
        return -1;
    }

    char file_path[1024];
    if (m2_find_image_file(rel_path, file_path, sizeof(file_path)) != 0) {
        // Optional companion images (shooter2d, launchme) might not be on a minimal disc
        fprintf(stderr, "[recomp] info: guest image '%s' not found (%s)\n", label, rel_path);
        return -1;
    }

    uint8_t actual_sha256[32];
    uint64_t actual_size = 0;
    if (m2_sha256_file(file_path, actual_sha256, &actual_size) != 0) {
        fprintf(stderr, "[recomp] failed to read guest image '%s' from %s\n", label, file_path);
        return -1;
    }

    if (actual_size != expected_size || memcmp(actual_sha256, expected_sha256, 32) != 0) {
        fprintf(stderr, "[recomp] guest image verification failed for '%s' (%s)\n", label, file_path);
        return -1;
    }

    FILE* f = m2_fopen_utf8(file_path, "rb");
    if (!f) {
        fprintf(stderr, "[recomp] failed to open '%s'\n", file_path);
        return -1;
    }

    if (fseek(f, (long)text_off, SEEK_SET) != 0 ||
        fread(g_ctx->mem + text_base, 1, text_size, f) != text_size) {
        fprintf(stderr, "[recomp] failed to read .text for '%s'\n", label);
        fclose(f);
        return -1;
    }

    if (data_size > 0) {
        if (fseek(f, (long)data_off, SEEK_SET) != 0 ||
            fread(g_ctx->mem + data_base, 1, data_size, f) != data_size) {
            fprintf(stderr, "[recomp] failed to read .data for '%s'\n", label);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    for (unsigned i = 0; i < text_fixup_count; ++i) {
        uint32_t off = text_fixups[i][0];
        uint32_t val = text_fixups[i][1];
        if (off > text_size || text_size - off < 4u) {
            fprintf(stderr, "[recomp] %s text fixup %u out of range (%#x)\n",
                    label, i, off);
            return -1;
        }
        uint8_t* p = g_ctx->mem + text_base + off;
        p[0] = (uint8_t)(val >> 24);
        p[1] = (uint8_t)(val >> 16);
        p[2] = (uint8_t)(val >> 8);
        p[3] = (uint8_t)(val);
    }

    for (unsigned i = 0; i < data_fixup_count; ++i) {
        uint32_t off = data_fixups[i][0];
        uint32_t val = data_fixups[i][1];
        if (off > data_size || data_size - off < 4u) {
            fprintf(stderr, "[recomp] %s data fixup %u out of range (%#x)\n",
                    label, i, off);
            return -1;
        }
        uint8_t* p = g_ctx->mem + data_base + off;
        p[0] = (uint8_t)(val >> 24);
        p[1] = (uint8_t)(val >> 16);
        p[2] = (uint8_t)(val >> 8);
        p[3] = (uint8_t)(val);
    }

    return 0;
}

static void guest_mem_init(void) {
    if (g_ctx) return;
    g_ctx = (PPCContext*)calloc(1, sizeof(PPCContext));
    if (!g_ctx) { perror("calloc PPCContext"); abort(); }
    g_ctx->mem = (uint8_t*)calloc(1, RECOMP_GUEST_MEM);
    if (!g_ctx->mem) { perror("calloc guest mem"); abort(); }

    // Load driving/game and apply fixups at runtime from user's verified game data:
    if (m2_guest_image_install_from_file(
            "driving/game", "driving/game",
            g_recomp_text_base, g_recomp_text_size, g_recomp_text_file_offset,
            g_recomp_data_base, g_recomp_data_size, g_recomp_data_file_offset,
            g_recomp_bss_base, g_recomp_bss_size,
            g_recomp_image_size, g_recomp_image_sha256,
            g_recomp_text_fixup_count, g_recomp_text_fixups,
            g_recomp_data_fixup_count, g_recomp_data_fixups) != 0) {
        fprintf(stderr, "[recomp] fatal: could not install driving/game from game data\n");
        abort();
    }

    // Install shooter2d if present
    m2_guest_image_install_from_file(
        "shooter2d", "shooter2d/shooter2d",
        g_shooter2d_recomp_text_base, g_shooter2d_recomp_text_size, g_shooter2d_recomp_text_file_offset,
        g_shooter2d_recomp_data_base, g_shooter2d_recomp_data_size, g_shooter2d_recomp_data_file_offset,
        g_shooter2d_recomp_bss_base, g_shooter2d_recomp_bss_size,
        g_shooter2d_recomp_image_size, g_shooter2d_recomp_image_sha256,
        g_shooter2d_recomp_text_fixup_count, g_shooter2d_recomp_text_fixups,
        g_shooter2d_recomp_data_fixup_count, g_shooter2d_recomp_data_fixups);

    // Install imsa_shell (LaunchMe.m2) when present. The default boot uses it
    // to perform the real launcher handoff; a data set without it falls back
    // to direct game boot below.
    g_launchme_installed = m2_guest_image_install_from_file(
        "imsa_shell", "LaunchMe.m2",
        g_launchme_recomp_text_base, g_launchme_recomp_text_size, g_launchme_recomp_text_file_offset,
        g_launchme_recomp_data_base, g_launchme_recomp_data_size, g_launchme_recomp_data_file_offset,
        g_launchme_recomp_bss_base, g_launchme_recomp_bss_size,
        g_launchme_recomp_image_size, g_launchme_recomp_image_sha256,
        g_launchme_recomp_text_fixup_count, g_launchme_recomp_text_fixups,
        g_launchme_recomp_data_fixup_count, g_launchme_recomp_data_fixups) == 0;

    // BSS is zeroed by calloc. r13: flat small-data model (30 r13-relative
    // words in 483KB text — compiler-profile.json), so r13 stays 0.
    g_ctx->gpr[13] = 0;
    g_ctx->gpr[1] = g_recomp_bss_base + g_recomp_bss_size + 0x10000; // stack top headroom
    // Zero the guest-owned frame carve (0x003F0000 gap). The
    // lifted StateDriver increments it at L2fb88; host never mutates it.
    {
        // Direct memset via g_ctx->mem — stays inside RECOMP_GUEST_MEM.
        const uint32_t base = M2_GUEST_FRAME_COUNTER_EA;
        if (base + 32 <= RECOMP_GUEST_MEM) {
            memset(g_ctx->mem + base, 0, 32);
        }
    }
    // Disable FTZ/DAZ (PPC has gradual underflow; see ppc_ctx.h)
#if defined(__x86_64__) || defined(_M_X64)
    {
        unsigned int mxcsr = 0;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        mxcsr &= ~0x8040u; // clear FTZ (bit 15) and DAZ (bit 6)
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
#endif
    m2_tracepoint_reset();
    m2_provenance_init();
}

// ---- guest kernel runtime slice ------------------------------------------
// Static m2k context + typed dispatch registry backing the guest's kernel
// selector calls. The full gate-family set (threads, msg ports, messages,
// signals, IOReqs, TE channel, module imports — see recomp_helpers.h) is
// installed from the shared handler template in recomp_runtime.c; every
// other ordinal keeps the generic lookup path, so unknown selectors behave
// exactly as before.
static m2k_context g_guest_kernel_ctx;
static m2k_context* g_guest_kernel_binding;
static m2_kernel_dispatch g_guest_kernel_registry;

// Ordinals installed in the production registry. guest_kernel_install()
// registers each from the shared handler template; this table is the
// authoritative inventory behind the install summary line.
static const uint32_t g_guest_kernel_ordinals[] = {
    M2_KSEL_ALLOC_MEM,
    M2_KSEL_ALLOC_MEM_MASKED,
    M2_KSEL_ALLOC_MEM_DEBUG,
    M2_KSEL_FREE_MEM,
    M2_KSEL_EXIT,
    M2_KSEL_MEMSET,
    M2_KSEL_MEMMOVE,
    M2_KSEL_CHECK_IO,
    M2_KSEL_GET_CACHE_INFO,
     M2_KSEL_CREATE_ITEM_VA,
     M2_KSEL_CREATE_ITEM,
     M2_KSEL_SCAVENGE_MEM,
    M2_KSEL_CREATE_THREAD_VA,
    M2_KSEL_CREATE_TASK_VA,
    M2_KSEL_OPEN_MODULE,
    M2_KSEL_CLOSE_MODULE,
    M2_KSEL_CREATE_TASK,
    M2_KSEL_IMPORT_BY_NAME,
    M2_KSEL_WAIT_IO,
    M2_KSEL_WAIT_PORT,
    M2_KSEL_DO_IO,
    M2_KSEL_ABORT_IO,
    M2_KSEL_SEND_IO,
    M2_KSEL_FREE_SIGNAL,
    M2_KSEL_LOCK_SEMAPHORE,
    M2_KSEL_UNLOCK_SEMAPHORE,
    M2_KSEL_ALLOC_SIGNAL,
    M2_KSEL_GET_MSG,
    M2_KSEL_REPLY_MSG,
    M2_KSEL_SEND_SMALL_MSG,
    M2_KSEL_GET_THIS_MSG,
    M2_KSEL_FIND_ITEM,
    M2_KSEL_SEND_SIGNAL,
    M2_KSEL_WAIT_SIGNAL,
    M2_KSEL_CREATE_IOREQ,
    M2_KSEL_CREATE_MSG,
    M2_KSEL_CREATE_MSGPORT,
    M2_KSEL_CREATE_SMALL_MSG,
    M2_KSEL_CREATE_SEMAPHORE,
    M2_KSEL_FIND_CURRENT_MODULE,
    M2_KSEL_CREATE_THREAD,
    M2_KSEL_GET_TE_READ_POINTER,
    M2_KSEL_SET_TE_WRITE_POINTER,
    M2_KSEL_FIND_NAMED_ITEM,
    M2_KSEL_CREATE_BUFFERED_MSG,
    M2_KSEL_LOOKUP_ITEM,
    M2_KSEL_SEND_MSG,
    M2_KSEL_PRINTF,
    M2_KSEL_ITOF,
    M2_KSEL_UTOF,
    M2_KSEL_VA_ARG,
    M2_KSEL_CREATE_SPINLOCK,
    M2_KSEL_DELETE_SPINLOCK,
    M2_KSEL_CREATE_DEVICE_STACK_LIST,
    M2_KSEL_DELETE_DEVICE_STACK_LIST,
    M2_KSEL_OPEN_DEVICE_STACK,
    M2_KSEL_CLOSE_DEVICE_STACK,
    M2_KSEL_FLUSH_DCACHE,
    M2_KSEL_WRITE_BACK_DCACHE,
    M2_KSEL_FREE_MEM_DEBUG,
    M2_KSEL_STRNCPY,
    M2_KSEL_STRCPY,
    M2_KSEL_STRCAT,
    M2_KSEL_TOUPPER,
    M2_KSEL_OBTAIN_SPINLOCK,
    M2_KSEL_RELEASE_SPINLOCK,
    M2_KSEL_OPEN_ITEM,
    M2_KSEL_CLOSE_ITEM,
    M2_KSEL_DELETE_ITEM,
    M2_KSEL_CHECK_ITEM,
    M2_KSEL_FIND_NAMED_NODE,
    M2_KSEL_UNIVERSAL_INSERT_NODE,
    M2_KSEL_FIND_TAG_ARG,
    M2_KSEL_GET_MEM_INFO,
    M2_KSEL_UNIMPORT_BY_NAME,
    M2_KSEL_DEBUG_PUT_STR,
    M2_KSEL_DEBUG_PUT_CHAR,
    M2_KSEL_VCPRINTF,
    M2_KSEL_STRLEN,
    M2_KSEL_STRCMP,
    // ---- promoted ords ----
    M2_KSEL_PREP_LIST,
    M2_KSEL_REM_NODE,
    M2_KSEL_ADD_TAIL,
    M2_KSEL_REM_TAIL,
    M2_KSEL_ADD_HEAD,
    M2_KSEL_REM_HEAD,
    M2_KSEL_SAMPLE_SYSTEM_TIME_TT,
    M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL,
    M2_KSEL_FLUSH_DCACHE_ALL,
    M2_KSEL_CLIB_PRINT_ERROR,
    M2_KSEL_PRINTF_SYS_ERR,
    M2_KSEL_SPRINTF,
    M2_KSEL_IS_MASTER_CPU,
    // ---- folio services named by the original shooter `main` ----
    M2_KSEL_SUB_TIMER_TICKS,
    M2_KSEL_READ_HARDWARE_RANDOM_NUMBER,
    M2_KSEL_SET_ITEM_PRI,
    // ---- font folio item loader (folios/font/fontitem.c LoadFont) ----
    M2_KSEL_FIND_AND_OPEN_ITEM,
};

static void guest_kernel_install(void) {
    if (g_guest_kernel_binding) return;
    m2k_init(&g_guest_kernel_ctx);
    if (m2_guest_kernel_bind(&g_guest_kernel_ctx, g_ctx) != 0) {
        fprintf(stderr, "[recomp] guest kernel bind failed\n");
        abort();
    }
    g_guest_kernel_binding = &g_guest_kernel_ctx;

    const m2_kernel_dispatch* tmpl = m2_guest_kernel_handlers();
    m2_kernel_dispatch_init(&g_guest_kernel_registry);
#define GK_INSTALL(field, ord) \
    do { \
        if (!tmpl->field || \
            m2_kernel_dispatch_register(&g_guest_kernel_registry, ord, \
                                        (void*)tmpl->field) != 0) { \
            fprintf(stderr, \
                    "[recomp] guest kernel registration failed: %s\n", #field); \
            abort(); \
        } \
    } while (0)
    GK_INSTALL(AllocMem, M2_KSEL_ALLOC_MEM);
    GK_INSTALL(ReallocMem, M2_KSEL_REALLOC_MEM);
    GK_INSTALL(AllocMemMasked, M2_KSEL_ALLOC_MEM_MASKED);
    GK_INSTALL(AllocMemDebug, M2_KSEL_ALLOC_MEM_DEBUG);
    GK_INSTALL(FreeMem, M2_KSEL_FREE_MEM);
    GK_INSTALL(Exit, M2_KSEL_EXIT);
    GK_INSTALL(MemSet, M2_KSEL_MEMSET);
    GK_INSTALL(MemMove, M2_KSEL_MEMMOVE);
    GK_INSTALL(CheckIO, M2_KSEL_CHECK_IO);
    GK_INSTALL(GetCacheInfo, M2_KSEL_GET_CACHE_INFO);
     GK_INSTALL(CreateItemVA, M2_KSEL_CREATE_ITEM_VA);
     GK_INSTALL(CreateItem, M2_KSEL_CREATE_ITEM);
    GK_INSTALL(ScavengeMem, M2_KSEL_SCAVENGE_MEM);
    GK_INSTALL(CreateThreadVA, M2_KSEL_CREATE_THREAD_VA);
    GK_INSTALL(CreateTaskVA, M2_KSEL_CREATE_TASK_VA);
    GK_INSTALL(OpenModule, M2_KSEL_OPEN_MODULE);
    GK_INSTALL(CloseModule, M2_KSEL_CLOSE_MODULE);
    GK_INSTALL(CreateTask, M2_KSEL_CREATE_TASK);
    GK_INSTALL(ImportByName, M2_KSEL_IMPORT_BY_NAME);
    GK_INSTALL(WaitIO, M2_KSEL_WAIT_IO);
    GK_INSTALL(WaitPort, M2_KSEL_WAIT_PORT);
    GK_INSTALL(DoIO, M2_KSEL_DO_IO);
    GK_INSTALL(AbortIO, M2_KSEL_ABORT_IO);
    GK_INSTALL(SendIO, M2_KSEL_SEND_IO);
    GK_INSTALL(FreeSignal, M2_KSEL_FREE_SIGNAL);
    GK_INSTALL(AllocSignal, M2_KSEL_ALLOC_SIGNAL);
    GK_INSTALL(GetMsg, M2_KSEL_GET_MSG);
    GK_INSTALL(ReplyMsg, M2_KSEL_REPLY_MSG);
    GK_INSTALL(SendSmallMsg, M2_KSEL_SEND_SMALL_MSG);
    GK_INSTALL(GetThisMsg, M2_KSEL_GET_THIS_MSG);
    GK_INSTALL(FindItem, M2_KSEL_FIND_ITEM);
    GK_INSTALL(SendSignal, M2_KSEL_SEND_SIGNAL);
    GK_INSTALL(WaitSignal, M2_KSEL_WAIT_SIGNAL);
    GK_INSTALL(CreateIOReq, M2_KSEL_CREATE_IOREQ);
    GK_INSTALL(CreateMsg, M2_KSEL_CREATE_MSG);
    GK_INSTALL(CreateMsgPort, M2_KSEL_CREATE_MSGPORT);
    GK_INSTALL(CreateSmallMsg, M2_KSEL_CREATE_SMALL_MSG);
    GK_INSTALL(FindCurrentModule, M2_KSEL_FIND_CURRENT_MODULE);
    GK_INSTALL(CreateThread, M2_KSEL_CREATE_THREAD);
    GK_INSTALL(GetTEReadPointer, M2_KSEL_GET_TE_READ_POINTER);
    GK_INSTALL(SetTEWritePointer, M2_KSEL_SET_TE_WRITE_POINTER);
    GK_INSTALL(FindNamedItem, M2_KSEL_FIND_NAMED_ITEM);
    GK_INSTALL(CreateBufferedMsg, M2_KSEL_CREATE_BUFFERED_MSG);
    GK_INSTALL(LookupItem, M2_KSEL_LOOKUP_ITEM);
    GK_INSTALL(SendMsg, M2_KSEL_SEND_MSG);
    GK_INSTALL(CreateSemaphore, M2_KSEL_CREATE_SEMAPHORE);
    GK_INSTALL(LockSemaphore, M2_KSEL_LOCK_SEMAPHORE);
    GK_INSTALL(UnlockSemaphore, M2_KSEL_UNLOCK_SEMAPHORE);
    GK_INSTALL(printf, M2_KSEL_PRINTF);
    GK_INSTALL(itof, M2_KSEL_ITOF);
    GK_INSTALL(utof, M2_KSEL_UTOF);
    GK_INSTALL(CreateSpinLock, M2_KSEL_CREATE_SPINLOCK);
    GK_INSTALL(DeleteSpinLock, M2_KSEL_DELETE_SPINLOCK);
    GK_INSTALL(VaArg, M2_KSEL_VA_ARG);
    GK_INSTALL(CreateDeviceStackList, M2_KSEL_CREATE_DEVICE_STACK_LIST);
    GK_INSTALL(DeleteDeviceStackList, M2_KSEL_DELETE_DEVICE_STACK_LIST);
    GK_INSTALL(OpenDeviceStack, M2_KSEL_OPEN_DEVICE_STACK);
    GK_INSTALL(CloseDeviceStack, M2_KSEL_CLOSE_DEVICE_STACK);
    GK_INSTALL(FlushDCache, M2_KSEL_FLUSH_DCACHE);
    GK_INSTALL(WriteBackDCache, M2_KSEL_WRITE_BACK_DCACHE);
    GK_INSTALL(FreeMemDebug, M2_KSEL_FREE_MEM_DEBUG);
    GK_INSTALL(StrNCpy, M2_KSEL_STRNCPY);
    GK_INSTALL(strcpy, M2_KSEL_STRCPY);
    GK_INSTALL(strcat, M2_KSEL_STRCAT);
    GK_INSTALL(ToUpper, M2_KSEL_TOUPPER);
    GK_INSTALL(ObtainSpinLock, M2_KSEL_OBTAIN_SPINLOCK);
    GK_INSTALL(ReleaseSpinLock, M2_KSEL_RELEASE_SPINLOCK);
    GK_INSTALL(OpenItem, M2_KSEL_OPEN_ITEM);
    GK_INSTALL(CloseItem, M2_KSEL_CLOSE_ITEM);
    GK_INSTALL(DeleteItem, M2_KSEL_DELETE_ITEM);
    GK_INSTALL(CheckItemNode, M2_KSEL_CHECK_ITEM);
    GK_INSTALL(FindNamedNode, M2_KSEL_FIND_NAMED_NODE);
    GK_INSTALL(UniversalInsertNode, M2_KSEL_UNIVERSAL_INSERT_NODE);
    GK_INSTALL(FindTagArg, M2_KSEL_FIND_TAG_ARG);
    GK_INSTALL(GetMemInfo, M2_KSEL_GET_MEM_INFO);
    GK_INSTALL(UnimportByName, M2_KSEL_UNIMPORT_BY_NAME);
    GK_INSTALL(DebugPutStr, M2_KSEL_DEBUG_PUT_STR);
    GK_INSTALL(DebugPutChar, M2_KSEL_DEBUG_PUT_CHAR);
    GK_INSTALL(vcprintf, M2_KSEL_VCPRINTF);
    GK_INSTALL(strlen, M2_KSEL_STRLEN);
    GK_INSTALL(strcmp, M2_KSEL_STRCMP);
    // ---- promoted ords ----
    GK_INSTALL(PrepList, M2_KSEL_PREP_LIST);
    GK_INSTALL(RemNode, M2_KSEL_REM_NODE);
    GK_INSTALL(AddTail, M2_KSEL_ADD_TAIL);
    GK_INSTALL(RemTail, M2_KSEL_REM_TAIL);
    GK_INSTALL(AddHead, M2_KSEL_ADD_HEAD);
    GK_INSTALL(RemHead, M2_KSEL_REM_HEAD);
    GK_INSTALL(SampleSystemTimeTT, M2_KSEL_SAMPLE_SYSTEM_TIME_TT);
    GK_INSTALL(ConvertTimerTicksToTimeVal, M2_KSEL_CONVERT_TIMER_TICKS_TO_TIME_VAL);
    GK_INSTALL(FlushDCacheAll, M2_KSEL_FLUSH_DCACHE_ALL);
    GK_INSTALL(clib_PrintError, M2_KSEL_CLIB_PRINT_ERROR);
    GK_INSTALL(PrintfSysErr, M2_KSEL_PRINTF_SYS_ERR);
    GK_INSTALL(Sprintf, M2_KSEL_SPRINTF);
    GK_INSTALL(IsMasterCPU, M2_KSEL_IS_MASTER_CPU);
    GK_INSTALL(SubTimerTicks, M2_KSEL_SUB_TIMER_TICKS);
    GK_INSTALL(ReadHardwareRandomNumber, M2_KSEL_READ_HARDWARE_RANDOM_NUMBER);
    GK_INSTALL(SetItemPri, M2_KSEL_SET_ITEM_PRI);
    GK_INSTALL(FindAndOpenItem, M2_KSEL_FIND_AND_OPEN_ITEM);
#undef GK_INSTALL
    m2_kernel_dispatch_set_global(&g_guest_kernel_registry);
    // Reset the guest-owned frame carve so a fresh boot always
    // starts from seq=0 even when g_ctx is reused (e.g., ctest re-entry).
    if (g_ctx && g_ctx->mem) {
        const uint32_t base = M2_GUEST_FRAME_COUNTER_EA;
        if (base + 32 <= RECOMP_GUEST_MEM) {
            memset(g_ctx->mem + base, 0, 32);
            m2_provenance_record_host_write(g_ctx->mem, base, 32, 0, 0, 0);
        }
    }
    printf("[recomp] guest-kernel-registry-installed: "
           "%zu gate-family selectors -> static m2k context\n",
           sizeof(g_guest_kernel_ordinals) / sizeof(g_guest_kernel_ordinals[0]));
}

static void guest_kernel_teardown(void) {
    if (!g_guest_kernel_binding) return;
    // Restore generic dispatch first, then reset + destroy the pools so a
    // re-entry starts from identical state.
    m2_kernel_dispatch_set_global(NULL);
    m2_guest_kernel_unbind();
    g_guest_kernel_binding = NULL;
    m2k_destroy(&g_guest_kernel_ctx);
}

int m2_recomp_kernel_ordinal_registered(uint32_t ordinal) {
    const size_t n =
        sizeof(g_guest_kernel_ordinals) / sizeof(g_guest_kernel_ordinals[0]);
    for (size_t i = 0; i < n; ++i)
        if (g_guest_kernel_ordinals[i] == ordinal) return 1;
    return 0;
}

PPCContext* m2_recomp_context(void) {
    // Lazy variant of guest_mem_init() for savestates/tests — same
    // setup (RAM + image copies + MXCSR policy), no dispatch into guest code.
    if (!g_ctx) {
        guest_mem_init();
        printf("[recomp] PPCContext ready via m2_recomp_context(): "
               "text_base=%#x data_base=%#x bss_base=%#x sp=%#x\n",
               g_recomp_text_base, g_recomp_data_base, g_recomp_bss_base,
               g_ctx->gpr[1]);
    }
    return g_ctx;
}

// Trace: expose guest context for snapshot collection.
// Returns the same g_ctx that guest_mem_init / m2_game_entry use. Never
// allocates if nothing is bound — returns NULL so snapshot falls back to
// unavailable.
PPCContext* m2_guest_ctx_for_trace(void) { return g_ctx; }

// Handle of the spawned game main task (0 = none created yet). Queried and
// conditionally joined by m2_recomp_shutdown(); kept set while StateDriver
// runs so the kernel context outlives the host frame loop.
static m2k_item_t g_main_thread = 0;

int m2_guest_prepare_restore(void) {
    if ((int32_t)g_main_thread > 0) {
        fprintf(stderr, "[recomp] checkpoint prepare refused: main task already exists\n");
        return -1;
    }
    guest_mem_init();
    guest_kernel_install();
    printf("[recomp] checkpoint restore boundary prepared: no guest boot task spawned\n");
    return 0;
}

void m2_guest_restore_main_task(uint32_t task) {
    g_main_thread = (m2k_item_t)task;
}

// Bumped by m2_guest_publish_main_task so the disc-shell boot can wait, with a
// bound, for the application task the guest itself creates.
static _Atomic uint32_t g_main_publish_seq;

// Adopt a guest-created application task (see recomp_host.h). The publish
// happens on the creating task's host thread, which under the imsa_shell boot
// is the launcher: m2_guest_set_boot_module_host deliberately leaves that
// thread's current-module identity alone, so the launcher's own
// FindCurrentModule/ImportByName still answer for imsa_shell.
void m2_guest_publish_main_task(uint32_t module_slot, uint32_t task,
                                uint32_t module_item) {
    if (task == 0) return;
    m2_guest_set_boot_module_host(module_slot, module_item);
    g_main_thread = (m2k_item_t)task;
    atomic_fetch_add_explicit(&g_main_publish_seq, 1u, memory_order_release);
}

// Query the guest main task state for the host's runtime ownership checks.
// Returns M2K_OK (0) on success, else error; fills state/status when non-null.
// Exposed to host/core/main.cpp.
int m2_guest_main_query(int32_t* state_out, int32_t* status_out) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) return -1;
    return m2k_query_task(&g_guest_kernel_ctx, g_main_thread, state_out, status_out);
}

// Host VBL bridge — host-owned timing service; the source-backed wait contract
// is recorded in the compatibility signal-wait notes.
// Host-owned pacing signal that unblocks StateDriver's exact
// WaitSignal(mask 0x10) at 0x30f40. Host calls once per host frame; guest
// consumes via m2k_wait_signal(mask 0x10) at the proven loop boundary. The
// dynamic loader mask at 0x3110c is intentionally not treated as VBL here.
// Capture-only frame pacing uses a separate host-private signal bit; it is
// delivered here alongside the existing VBL signal and never enters the
// guest-facing signal-mask ABI. Returns M2K_OK when delivered, negative when
// no guest task is live.
int m2_guest_signal_vbl(void) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) return -1;
    int rc = m2_guest_timer_vbl_tick();
    if (rc != M2K_OK) return rc;
    // Signal the StateDriver task specifically (bit 4 = mask 0x10). The
    // guest's WaitSignal(0x10) consumes exactly this bit. Using the task
    // handle (not SELF broadcast) keeps delivery targeted and preserves the
    // per-task sig_pending contract.
    rc = m2k_send_signal(&g_guest_kernel_ctx, g_main_thread, 4);
    if (rc != M2K_OK) return rc;
    // The GState folio's source-owned GS_SendList/GS_SendIO waits use the
    // guest's configured gs_VidSignal. The runtime records those exact task
    // and mask pairs and translates this same host VBL into their pending
    // signal bits; no generic WaitSignal wake is involved.
    rc = m2_guest_signal_gstate_vbl();
    if (rc != M2K_OK) return rc;
    if (m2_guest_frame_pacing_enabled()) {
        rc = m2k_send_signal(&g_guest_kernel_ctx, g_main_thread,
                             M2_GUEST_FRAME_PACING_SIGNAL_BIT);
    }
    return rc;
}

// Checkpoint split signals: the combined m2_guest_signal_vbl
// always co-delivers the pacing permit, which would release the parked task
// before a save/load completes. These wrappers deliver exactly one bit so the
// probe can park the guest at a loop head and grant single frame permits.
// Unused paths cost nothing; guest behavior is unchanged when pacing is off.
int m2_guest_signal_vbl_only(void) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) return -1;
    int rc = m2_guest_timer_vbl_tick();
    if (rc != M2K_OK) return rc;
    return m2k_send_signal(&g_guest_kernel_ctx, g_main_thread, 4);
}

int m2_guest_grant_frame_permit(void) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) return -1;
    return m2k_send_signal(&g_guest_kernel_ctx, g_main_thread,
                           M2_GUEST_FRAME_PACING_SIGNAL_BIT);
}

// Synchronous boot fallback: the selected guest main runs on this host thread
// without a task context, so install the same exit#8 guard a guest task body
// uses. A guest exit#8 unwinds here instead of returning into the guest stream.
static void (*g_sync_boot_entry)(PPCContext* c);

static int32_t m2_sync_main_call(PPCContext* c, void* user) {
    (void)user;
    if (g_sync_boot_entry) g_sync_boot_entry(c);
    return 0;
}

// Default boot: the disc's own launcher (<game-data>/LaunchMe.m2, module
// `imsa_shell`) runs as a guest task, so the driving/game -> shooter2d handoff
// is guest code — the launcher's own ChangeDirectory/OpenModule/CreateTaskVA
// and its `switch (*(msg + 0x28))` on the child's exit status — rather than a
// host shortcut. M2_BOOT_APP remains the documented direct-boot control and
// keeps its previous behaviour byte for byte.
// Boot args (sec-17 racePhase exit). The original OS hands the
// driving game its host-supplied argc/argv or M2_GUEST_ARGS override; the
// staged vector is consumed once, by the first guest task created with no
// explicit ARGC/ARGP. Under the imsa_shell boot that task is the driving game
// the launcher creates, so the shell boot must stage too — it does, from
// m2_boot_disc_shell, before the launcher's CreateTaskVA runs.
// The default boot (no M2_GUEST_ARGS) keeps GDP+0x5c at 0 so the sec-17 stall
// is reachable; staging "-V" sets GDP+0x5c=1, which takes the racePhase exit
// block and bypasses sec-17 entirely. M2_CARSIM_HOLD_TRACK_INIT defaults to
// blocking on the authentic signal; =0 injects the teardown bit at 0x18ea4
// and kills the sim after one tick.
static void m2_stage_game_args(int for_driving_game, int argc, char** argv) {
    if (!for_driving_game) {
        m2_guest_set_main_args(0, NULL);
        return;
    }
    const char* extra = getenv("M2_GUEST_ARGS");
    if (extra && extra[0] != '\0') {
        static char args_buf[1024];
        static char* args_vec[32];
        snprintf(args_buf, sizeof(args_buf), "%s", extra);
        int n = 0;
        args_vec[n++] = (char*)"imsa";
        for (char* p = strtok(args_buf, " \t"); p && n < 32;
             p = strtok(NULL, " \t")) {
            args_vec[n++] = p;
        }
        m2_guest_set_main_args(n, args_vec);
        return;
    }
    m2_guest_set_main_args(argc, argv);
}

static int m2_boot_disc_shell(int argc, char** argv) {
    // SHELL_NAME_EA is the pre-existing host-owned boot-name carve (the direct
    // boot path still stages its module path there). SHELL_TAGS_EA is the
    // launcher's CreateTask tag list, immediately after it and inside the same
    // zeroed 0x003F0000 gap.
    enum { SHELL_NAME_EA = 0x003f3000u, SHELL_TAGS_EA = 0x003f3020u,
           SHELL_PUBLISH_BUDGET_MS = 2000 };
    const char* module_path = "imsa_shell";
    guest_mem_init();
    guest_kernel_install();
    // Stage the driving game's arguments BEFORE the launcher runs: the first
    // guest task created with no explicit ARGC/ARGP consumes this vector, and
    // under this boot that is the game the launcher itself creates. The
    // launcher is created with an explicit empty argv further down, so it can
    // never take this vector.
    m2_stage_game_args(1 /* driving/game */, argc, argv);
    printf("[recomp] m2_game_entry: image=%s text %u bytes data %u bytes bss %u bytes\n",
           module_path, g_launchme_recomp_text_size, g_launchme_recomp_data_size,
           g_launchme_recomp_bss_size);
    const m2_kernel_dispatch* handlers = m2_guest_kernel_handlers();
    if (!handlers || !handlers->OpenModule || !handlers->CreateTask ||
        !handlers->CloseModule) {
        fprintf(stderr, "[recomp] module/task handlers unavailable\n");
        return 2;
    }
    if (SHELL_NAME_EA > RECOMP_GUEST_MEM - 32u) return 2;
    memset(g_ctx->mem + SHELL_NAME_EA, 0, 32u);
    memcpy(g_ctx->mem + SHELL_NAME_EA, module_path, strlen(module_path) + 1u);
    m2_provenance_record_host_write(g_ctx->mem, SHELL_NAME_EA,
                                    (uint32_t)strlen(module_path) + 1u,
                                    0, 0, 0);
    const uint32_t module_item = handlers->OpenModule(
        SHELL_NAME_EA, 0u /* OPENMODULE_FOR_TASK */, 0u /* tags */);
    if ((int32_t)module_item < 0 || module_item == 0) {
        fprintf(stderr, "[recomp] OpenModule(%s) failed: %#x\n", module_path,
                module_item);
        return 2;
    }
    m2_guest_set_boot_module(2u /* GK_MODULE_SHELL */, module_item);
    // Give the launcher an explicit empty argv. CreateTask with a NULL tag
    // list leaves every has_* clear, and gk_spawn_task then treats the task
    // as the consumer of the host-staged argument vector (m2_guest_set_main_args
    // -> M2_GUEST_ARGS) and zeroes it. The launcher must not consume it: the
    // staged arguments belong to the application task the launcher itself
    // creates, which is the driving game. ARGC=0/ARGP=0 says exactly that.
    if (SHELL_TAGS_EA > RECOMP_GUEST_MEM - 24u) return 2;
    wr32(g_ctx, SHELL_TAGS_EA + 0u, 14u /* CREATETASK_TAG_ARGC */);
    wr32(g_ctx, SHELL_TAGS_EA + 4u, 0u);
    wr32(g_ctx, SHELL_TAGS_EA + 8u, 15u /* CREATETASK_TAG_ARGP */);
    wr32(g_ctx, SHELL_TAGS_EA + 12u, 0u);
    wr32(g_ctx, SHELL_TAGS_EA + 16u, 0u /* TAG_END */);
    wr32(g_ctx, SHELL_TAGS_EA + 20u, 0u);
    const uint32_t shell = handlers->CreateTask(module_item, SHELL_NAME_EA,
                                                SHELL_TAGS_EA);
    if (shell == 0) {
        fprintf(stderr, "[recomp] CreateTask(%s) failed: %#x\n", module_path,
                shell);
        return 2;
    }
    printf("[recomp] imsa_shell task created: item=%#x; the launcher now "
           "creates the application task itself\n", shell);
    // The host frame pump requires a live application task before it starts,
    // and under this boot that task is the launcher's to create. Wait for the
    // guest to publish it, with a bound: the launcher's own work before its
    // CreateTaskVA is a handful of selectors, so this is a warm-up, not a
    // substitute for the guest doing the launch.
    //
    // A paused diagnostic session (--control-socket: the boot every named
    // m2ctl.py runtime gate uses) parks each guest thread at its first
    // suspension boundary, and nothing steps the guest again until
    // m2_game_entry has returned — DiagnosticControl::poll(), the frame loop
    // and the diagnostic DSPP service all live after this call. So under that
    // boot the launcher never reaches its CreateTaskVA while the host waits
    // here, and this wait is unsatisfiable by construction: a wall-clock
    // busy-wait against a guest that is never pumped. Yield the guest for the
    // duration of the wait instead — release the suspension gate so the
    // launcher can run — then re-request the pause before returning so the
    // session is still paused for its first command. DiagnosticControl::
    // suspend() in main.cpp completes the re-park and reports it if it cannot.
    // This is not a longer fuse: the bound below still fails the boot when the
    // guest genuinely does not publish.
    const int yield_to_diagnostic = m2_diagnostic_enabled();
    if (yield_to_diagnostic) m2_diagnostic_resume();
    int published = 0;
    for (int waited = 0; waited < SHELL_PUBLISH_BUDGET_MS; ++waited) {
        if (atomic_load_explicit(&g_main_publish_seq, memory_order_acquire) != 0u &&
            g_main_thread != 0) {
            int32_t state = -1;
            if (m2k_query_task(&g_guest_kernel_ctx, g_main_thread, &state,
                               NULL) == M2K_OK &&
                (state == M2K_TASK_STARTED || state == M2K_TASK_RUNNING)) {
                published = 1;
                break;
            }
        }
        struct timespec ts = {0, 1000000};   /* 1 ms */
        nanosleep(&ts, NULL);
    }
    if (yield_to_diagnostic) m2_diagnostic_request_pause();
    if (published) {
        printf("[recomp] imsa_shell published application task "
               "item=%#x\n", (unsigned)g_main_thread);
        return 0;
    }
    fprintf(stderr,
            "[recomp] imsa_shell did not publish an application task within "
            "%d ms; guest launch failed\n", SHELL_PUBLISH_BUDGET_MS);
    return 2;
}

int m2_game_entry(int argc, char** argv) {
    enum { BOOT_GAME = 0u, BOOT_SHOOTER2D = 1u, BOOT_NAME_EA = 0x003f3000u };
    const char* boot_app = getenv("M2_BOOT_APP");
    // M2_BOOT_APP is the documented dev-fidelity direct-boot control: with it
    // set, the host boots the named image itself and nothing below changes.
    // Without it, the disc's own launcher runs and the guest performs the
    // driving/game -> shooter2d handoff.
    int direct_boot = boot_app != NULL && boot_app[0] != '\0';
    uint32_t module_slot = BOOT_GAME;
    if (boot_app && boot_app[0] != '\0') {
        if (strcmp(boot_app, "shooter2d") == 0)
            module_slot = BOOT_SHOOTER2D;
        else if (strcmp(boot_app, "game") != 0 &&
                 strcmp(boot_app, "driving") != 0) {
            fprintf(stderr,
                    "[recomp] unsupported M2_BOOT_APP=%s (expected game or shooter2d)\n",
                    boot_app);
            return 2;
        }
    }
    guest_mem_init();
    if (!direct_boot && !g_launchme_installed) {
        fprintf(stderr,
                "[recomp] imsa_shell image unavailable; booting driving/game "
                "directly\n");
        direct_boot = 1;
    }
    if (!direct_boot)
        return m2_boot_disc_shell(argc, argv);
    const char* module_path = module_slot == BOOT_SHOOTER2D
                                  ? "../shooter2d" : "game";
    const uint32_t text_base = module_slot == BOOT_SHOOTER2D
                                   ? g_shooter2d_recomp_text_base
                                   : g_recomp_text_base;
    const uint32_t data_base = module_slot == BOOT_SHOOTER2D
                                   ? g_shooter2d_recomp_data_base
                                   : g_recomp_data_base;
    const uint32_t bss_base = module_slot == BOOT_SHOOTER2D
                                  ? g_shooter2d_recomp_bss_base
                                  : g_recomp_bss_base;
    const uint32_t text_size = module_slot == BOOT_SHOOTER2D
                                   ? g_shooter2d_recomp_text_size
                                   : g_recomp_text_size;
    const uint32_t data_size = module_slot == BOOT_SHOOTER2D
                                   ? g_shooter2d_recomp_data_size
                                   : g_recomp_data_size;
    const uint32_t bss_size = module_slot == BOOT_SHOOTER2D
                                  ? g_shooter2d_recomp_bss_size
                                  : g_recomp_bss_size;
    g_sync_boot_entry = module_slot == BOOT_SHOOTER2D
                            ? FN_shooter2d_main__002a0 : FN_main__1ef6c;
    printf("[recomp] m2_game_entry: image=%s text %u bytes data %u bytes bss %u bytes\n",
           module_path, text_size, data_size, bss_size);
    guest_mem_init();
    g_ctx->gpr[1] = bss_base + bss_size + 0x10000u;
    printf("[recomp] PPCContext ready: text_base=%#x data_base=%#x bss_base=%#x sp=%#x\n",
           text_base, data_base, bss_base, g_ctx->gpr[1]);
    guest_kernel_install();

    m2_stage_game_args(module_slot == BOOT_GAME, argc, argv);

    if (BOOT_NAME_EA > RECOMP_GUEST_MEM - 32u) return 2;
    memset(g_ctx->mem + BOOT_NAME_EA, 0, 32u);
    memcpy(g_ctx->mem + BOOT_NAME_EA, module_path, strlen(module_path) + 1u);
    m2_provenance_record_host_write(g_ctx->mem, BOOT_NAME_EA,
                                    (uint32_t)strlen(module_path) + 1u,
                                    0, 0, 0);

    // Resolve the selected static image through the same module and task
    // selectors the original shell uses. The frame pump stays on the host
    // thread while CreateTask runs the guest main asynchronously.
    const m2_kernel_dispatch* handlers = m2_guest_kernel_handlers();
    if (!handlers || !handlers->OpenModule || !handlers->CreateTask ||
        !handlers->CreateTaskVA || !handlers->CloseModule) {
        fprintf(stderr, "[recomp] module/task handlers unavailable\n");
        return 2;
    }
    const uint32_t module_item = handlers->OpenModule(
        BOOT_NAME_EA, 0u /* OPENMODULE_FOR_TASK */, 0u /* tags */);
    if ((int32_t)module_item < 0 || module_item == 0) {
        fprintf(stderr, "[recomp] OpenModule(%s) failed: %#x\n",
                module_path, module_item);
        return 2;
    }
    m2_guest_set_boot_module(module_slot, module_item);
    printf("[recomp] resolving module=%s item=%#x main=%#x [async]\n",
           module_path, module_item,
           text_base + (module_slot == BOOT_SHOOTER2D ? 0x2a0u : 0x1ef6cu));
    g_main_thread = module_slot == BOOT_SHOOTER2D
        ? handlers->CreateTaskVA(module_item, BOOT_NAME_EA,
                                 16u /* CREATETASK_TAG_CMDSTR */, BOOT_NAME_EA,
                                 0u /* TAG_END */, 0u, 0u, 0u)
        : handlers->CreateTask(module_item, BOOT_NAME_EA, 0u /* no tags */);
    if (g_main_thread != 0) {
        const int32_t close_rc = handlers->CloseModule(module_item);
        if (close_rc < 0)
            fprintf(stderr, "[recomp] CloseModule(%#x) failed: %d\n",
                    module_item, close_rc);
        printf("[recomp] guest module task created: item=%#x image=%s\n",
               (unsigned)g_main_thread, module_path);
        return 0;
    }
    if ((int32_t)g_main_thread < 0) {
        fprintf(stderr, "[recomp] CreateTask(%s) failed: %#x\n",
                module_path, (unsigned)g_main_thread);
        g_main_thread = 0;
    }

    // If the task service cannot create the task, run the same image entry
    // synchronously while its module Item is still open.
    fprintf(stderr, "[recomp] CreateTask yielded 0; executing guest entry synchronously\n");
    uint32_t argv_ea = 0;
    g_ctx->gpr[3] = m2_guest_main_args_peek(&argv_ea);
    g_ctx->gpr[4] = argv_ea;
    m2_guest_main_args_consume();
    int32_t exit_status = 0;
    if (m2_guest_exit_guard_run(m2_sync_main_call, g_ctx, NULL, &exit_status)) {
        (void)handlers->CloseModule(module_item);
        printf("[recomp] %s main() exit#8 status=%d\n", module_path, exit_status);
        return (int)exit_status;
    }
    (void)handlers->CloseModule(module_item);
    printf("[recomp] %s main() returned r3=%#x\n", module_path, g_ctx->gpr[3]);
    return (int)g_ctx->gpr[3];
}

// Bounded lifecycle: query the main task, join only when it is already
// terminal, and otherwise leave everything alive. The trampoline publishes
// its terminal state under the context lock and never touches the context
// afterwards (guest_kernel.c), so a terminal task joins instantly and
// teardown is safe. A non-terminal task means guest StateDriver is still
// executing on its host thread — there is no cancellation API to invent and
// destroying the context under a live task would expose freed pools/lock
// state — so we report it remains live and skip guest kernel teardown
// entirely (binding + registry stay installed for any later call).
int m2_guest_live_task_count(void) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) return 0;
    int live = 0;
    pthread_mutex_lock(&g_guest_kernel_ctx.lock);
    for (uint32_t i = 0; i < M2K_MAX_TASKS; ++i) {
        const m2k_task* t = &g_guest_kernel_ctx.tasks[i];
        if (t->used && (t->state == M2K_TASK_STARTED ||
                        t->state == M2K_TASK_RUNNING))
            ++live;
    }
    pthread_mutex_unlock(&g_guest_kernel_ctx.lock);
    return live;
}

void m2_recomp_shutdown(void) {
    if (g_main_thread == 0 || !g_guest_kernel_binding) {
        // No guest task owns the kernel context here (stub build or direct
        // dispatch fallback).
        printf("[recomp] no live main task to join (item=%#x)\n",
               (unsigned)g_main_thread);
        guest_kernel_teardown();
        m2_provenance_shutdown();
        return;
    }

    int32_t state = -1, status = 0;
    const int32_t qrc = m2k_query_task(&g_guest_kernel_ctx, g_main_thread,
                                       &state, &status);
    if (qrc != M2K_OK) {
        printf("[recomp] main task %#x query rc=%d; kernel context left "
               "untouched\n", (unsigned)g_main_thread, qrc);
        m2_provenance_flush();
        return;
    }
    if (state != M2K_TASK_COMPLETED && state != M2K_TASK_FAILED) {
        printf("[recomp] main task %#x still %s — StateDriver remains live; "
               "skipping guest kernel teardown\n",
               (unsigned)g_main_thread,
               state == M2K_TASK_STARTED ? "STARTED" : "RUNNING");
        m2_provenance_flush();
        return;
    }

    const int rc = m2k_join_task(&g_guest_kernel_ctx, g_main_thread);
    printf("[recomp] main task %#x joined terminal state=%d rc=%d\n",
           (unsigned)g_main_thread, state, rc);
    g_main_thread = 0;
    // A terminal main task is not the end of the guest's work. Under the
    // imsa_shell boot the launcher is a second live task: when the driving
    // game exits at the door, the launcher is still executing its case-1 arm
    // (reading msg_Result, then OpenModule/CreateTask for shooter2d) and
    // touching this same context. Tearing the kernel down underneath it would
    // free pools and lock state a live thread still calls. No-op on the
    // direct-boot paths, where the main task is the only live task.
    const int still_live = m2_guest_live_task_count();
    if (still_live > 0) {
        printf("[recomp] %d guest task(s) still live after the main task "
               "terminated — kernel context left intact\n", still_live);
        m2_provenance_flush();
        return;
    }
    m2_provenance_shutdown();
    guest_kernel_teardown();
}
