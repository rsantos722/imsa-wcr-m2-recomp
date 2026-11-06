// kernel_shim.cpp — kernel folio (id 1) shim implementations.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <mutex>

#include "diab_runtime.h"
#include "kernel_folio.h"
#include "shim_abi.h"
#include "folio_loader.h"

extern "C" {

// --- memory -----------------------------------------------------------------
void* m2_AllocMem(int32_t mem_type, uint32_t size) {
    (void)mem_type;
    return std::malloc(size);
}
void* m2_AllocMemMasked(uint32_t mask, int32_t type, uint32_t size) {
    (void)mask;
    return m2_AllocMem(type, size);
}
void m2_FreeMem(void* p) { std::free(p); }

// --- exit ---------------------------------------------------------------------
void m2_kernel_exit(int32_t code) {
    std::fprintf(stderr,
                 "[kernel] exit(%d) via host kernel shim, no task guard; "
                 "request recorded, host process retained\n", code);
}

// --- libc flavour -------------------------------------------------------------
void* m2_memmove(void* d, const void* s, uint32_t n) { return std::memmove(d, s, n); }
void* m2_memset(void* d, int32_t c, uint32_t n) { return std::memset(d, c, n); }
uint32_t m2_strlen(const char* s) { return static_cast<uint32_t>(std::strlen(s)); }
char* m2_strcpy(char* d, const char* s) { return std::strcpy(d, s); }
char* m2_strcat(char* d, const char* s) { return std::strcat(d, s); }
int32_t m2_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }

int32_t m2_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = std::vprintf(fmt, ap);
    va_end(ap);
    return n;
}
int32_t m2_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    int n = std::vsprintf(buf, fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(ap);
    return n;
}
void m2_PrintError(const char* who, const char* what,
                   const char* whom, int32_t err) {
    std::fprintf(stderr, "[kernel] clib_PrintError who=%s what=%s whom=%s err=%d\n",
                 who ? who : "<null>", what ? what : "<null>",
                 whom ? whom : "<null>", err);
    if (err < 0) m2_PrintfSysErr(err);
}
void m2_PrintfSysErr(int32_t err) {
    std::fprintf(stderr, "[syserr %d]\n", err);
}

// --- items / lists ------------------------------------------------------------
m2_item_t m2_LookupItem(int32_t /*type*/, const char* name) {
    return name ? 0x4B524E31 /*'KRN1'*/ : 0;
}
m2_item_t m2_FindItem(m2_item_t parent, const char* name) {
    (void)parent;
    return m2_LookupItem(0, name);
}

static void list_link(m2_node_t* prev, m2_node_t* next) {
    if (prev) prev->next = next;
    if (next) next->prev = prev;
}
static void list_unlink(m2_node_t* n) {
    list_link(n->prev, n->next);
    n->next = n->prev = nullptr;
}
void m2_PrepList(m2_list_t* l) {
    l->head.next = &l->tail;
    l->head.prev = nullptr;
    l->tail.prev = &l->head;
    l->tail.next = nullptr;
    l->count = 0;
}
void m2_AddHead(m2_list_t* l, m2_node_t* n) {
    list_link(n, l->head.next);
    list_link(&l->head, n);
    ++l->count;
}
void m2_AddTail(m2_list_t* l, m2_node_t* n) {
    list_link(l->tail.prev, n);
    list_link(n, &l->tail);
    ++l->count;
}
m2_node_t* m2_RemHead(m2_list_t* l) {
    m2_node_t* n = l->head.next;
    if (n == &l->tail) return nullptr;
    list_unlink(n);
    --l->count;
    return n;
}
m2_node_t* m2_RemTail(m2_list_t* l) {
    m2_node_t* n = l->tail.prev;
    if (n == &l->head) return nullptr;
    list_unlink(n);
    --l->count;
    return n;
}
void m2_RemNode(m2_node_t* n) { list_unlink(n); }

// --- signals ------------------------------------------------------------------
namespace {
uint32_t g_signal_bits = 0;
std::mutex g_kernel_mutex;
}  // namespace

m2_signal_t m2_AllocSignal(int32_t bit_num) {
    std::lock_guard<std::mutex> lk(g_kernel_mutex);
    if (bit_num >= 0 && bit_num < 32 && !(g_signal_bits & (1u << bit_num))) {
        g_signal_bits |= (1u << bit_num);
        return bit_num;
    }
    for (int i = 0; i < 32; ++i)
        if (!(g_signal_bits & (1u << i))) {
            g_signal_bits |= (1u << i);
            return i;
        }
    return -1;
}
void m2_FreeSignal(m2_signal_t sig) {
    std::lock_guard<std::mutex> lk(g_kernel_mutex);
    if (sig >= 0 && sig < 32) g_signal_bits &= ~(1u << sig);
}
int32_t m2_SendSignal(m2_item_t task, m2_signal_t sig) {
    (void)task;
    return (sig >= 0 && sig < 32) ? M2_OK : -1;
}
m2_signal_t m2_WaitSignal(m2_signal_t mask) {
    return mask;
}
void m2_LockSemaphore(m2_item_t sem) { (void)sem; }
void m2_UnlockSemaphore(m2_item_t sem) { (void)sem; }

// --- time -----------------------------------------------------------------------
void m2_SampleSystemTimeTT(void* out_ticks) {
    if (out_ticks) std::memset(out_ticks, 0, 8);
}
void m2_ConvertTimerTicksToTimeVal(const void* ticks, void* out_timeval) {
    if (ticks && out_timeval) std::memcpy(out_timeval, ticks, 8);
}

// --- cache / misc ---------------------------------------------------------------
void m2_FlushDCacheAll(void) {}
m2_err_t m2_DeleteItem(m2_item_t item) { (void)item; return M2_OK; }

#ifndef M2_HAVE_RECOMP
// --- Diab runtime helpers ---------------------------------------------------------
float m2___itof(int32_t v) { return static_cast<float>(v); }
float m2___utof(uint32_t v) { return static_cast<float>(v); }

#define M2_DEF_NOOP(n) \
    void _savegpr_##n(void) {} \
    void _restgpr_##n(void) {} \
    void _savegprs_##n##_l(void) {} \
    void _restgprs_##n##_l(void) {} \
    void _savefprs_##n##_l(void) {}
M2_DIAB_GPR_RANGE(M2_DEF_NOOP)
#undef M2_DEF_NOOP

void m2___tagCall_0(void) {}
void m2___tagCall_1(void) {}
void m2___tagCall_2(void) {}
#endif

}  // extern "C"

void kernel_shim_register() {
    using namespace m2host;
    folio_register(M2_FOLIO_KERNEL, 2, reinterpret_cast<void*>(&m2_AllocMem));
    folio_register(M2_FOLIO_KERNEL, 3, reinterpret_cast<void*>(&m2_FreeMem));
    folio_register(M2_FOLIO_KERNEL, 8, reinterpret_cast<void*>(&m2_kernel_exit));
    folio_register(M2_FOLIO_KERNEL, 21, reinterpret_cast<void*>(&m2_SampleSystemTimeTT));
    folio_register(M2_FOLIO_KERNEL, 27, reinterpret_cast<void*>(&m2_ConvertTimerTicksToTimeVal));
    folio_register(M2_FOLIO_KERNEL, 31, reinterpret_cast<void*>(&m2_memmove));
    folio_register(M2_FOLIO_KERNEL, 33, reinterpret_cast<void*>(&m2_memset));
    folio_register(M2_FOLIO_KERNEL, 34, reinterpret_cast<void*>(&m2_LookupItem));
    folio_register(M2_FOLIO_KERNEL, 35, reinterpret_cast<void*>(&m2_AllocMemMasked));
    folio_register(M2_FOLIO_KERNEL, 37, reinterpret_cast<void*>(&m2_PrepList));
    folio_register(M2_FOLIO_KERNEL, 40, reinterpret_cast<void*>(&m2_RemNode));
    folio_register(M2_FOLIO_KERNEL, 42, reinterpret_cast<void*>(&m2_AddTail));
    folio_register(M2_FOLIO_KERNEL, 43, reinterpret_cast<void*>(&m2_RemTail));
    folio_register(M2_FOLIO_KERNEL, 44, reinterpret_cast<void*>(&m2_AddHead));
    folio_register(M2_FOLIO_KERNEL, 45, reinterpret_cast<void*>(&m2_RemHead));
    folio_register(M2_FOLIO_KERNEL, 136, reinterpret_cast<void*>(&m2_FlushDCacheAll));
    folio_register(M2_FOLIO_KERNEL, 143, reinterpret_cast<void*>(&m2_FreeSignal));
    folio_register(M2_FOLIO_KERNEL, 144, reinterpret_cast<void*>(&m2_AllocSignal));
    folio_register(M2_FOLIO_KERNEL, 160, reinterpret_cast<void*>(&m2_LockSemaphore));
    folio_register(M2_FOLIO_KERNEL, 161, reinterpret_cast<void*>(&m2_UnlockSemaphore));
    folio_register(M2_FOLIO_KERNEL, 163, reinterpret_cast<void*>(&m2_FindItem));
    folio_register(M2_FOLIO_KERNEL, 164, reinterpret_cast<void*>(&m2_DeleteItem));
    folio_register(M2_FOLIO_KERNEL, 165, reinterpret_cast<void*>(&m2_SendSignal));
    folio_register(M2_FOLIO_KERNEL, 166, reinterpret_cast<void*>(&m2_WaitSignal));
    folio_register(M2_FOLIO_KERNEL, 309, reinterpret_cast<void*>(&m2_printf));
    folio_register(M2_FOLIO_KERNEL, 310, reinterpret_cast<void*>(&m2_PrintError));
    folio_register(M2_FOLIO_KERNEL, 311, reinterpret_cast<void*>(&m2_PrintfSysErr));
    folio_register(M2_FOLIO_KERNEL, 312, reinterpret_cast<void*>(&m2_strlen));
    folio_register(M2_FOLIO_KERNEL, 315, reinterpret_cast<void*>(&m2_strcpy));
    folio_register(M2_FOLIO_KERNEL, 317, reinterpret_cast<void*>(&m2_strcat));
    folio_register(M2_FOLIO_KERNEL, 320, reinterpret_cast<void*>(&m2_strcmp));
    folio_register(M2_FOLIO_KERNEL, 324, reinterpret_cast<void*>(&m2_sprintf));
    folio_register(M2_FOLIO_KERNEL, 325, reinterpret_cast<void*>(&m2___itof));
    folio_register(M2_FOLIO_KERNEL, 326, reinterpret_cast<void*>(&m2___utof));
}
