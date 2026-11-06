// folio_loader.cpp — selector-keyed dispatch table.
#include "folio_loader.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <string>

#include "selector_names.gen.h"
#include "shim_abi.h"

// Weak default stubs for subsystems that may not be linked in standalone test targets
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void audio_shim_register() {}
__attribute__((weak)) void mpeg_decoder_register() {}
#endif

namespace m2host {

namespace {

using Table = ShimFn* [M2_FOLIO_COUNT][M2_VECTORS_PER_FOLIO];
Table g_table = {};
bool g_installed = false;

const void* g_stub = nullptr;

// Optional live-guest ownership check for the report (folio_loader.h).
GuestSelectorProbeFn g_guest_probe = nullptr;

// Optional observed-census probe (folio_loader.h).
SelectorObservedFn g_observed_probe = nullptr;

// Which generated name table the booted image's register supplies.
const SelectorName* g_names = kSelectorNames;
size_t g_names_count = kSelectorNamesCount;

void shim_stub_unimplemented() {
}

const SelectorName* find_selector_name(uint32_t sel) {
    auto begin = g_names;
    auto end = g_names + g_names_count;
    auto it = std::lower_bound(
        begin, end, sel,
        [](const SelectorName& e, uint32_t v) { return e.sel < v; });
    return (it != end && it->sel == sel) ? it : nullptr;
}

}  // namespace

void folio_set_observed_probe(SelectorObservedFn probe) {
    g_observed_probe = probe;
}

void folio_set_name_source(bool shooter2d) {
    g_names = shooter2d ? kSelectorNamesShooter2d : kSelectorNames;
    g_names_count = shooter2d ? kSelectorNamesShooter2dCount
                              : kSelectorNamesCount;
}

void folio_register(uint32_t folio, uint32_t ordinal, void* fn) {
    if (folio >= M2_FOLIO_COUNT || ordinal >= M2_VECTORS_PER_FOLIO || !fn) {
        std::fprintf(stderr, "m2host: bad registration f=%u o=%u fn=%p\n",
                     folio, ordinal, fn);
        return;
    }
    g_table[folio][ordinal] = reinterpret_cast<ShimFn*>(fn);
}

// C-linkage definition matching shim_abi.h's declaration.
extern "C" void* m2_selector_lookup(uint32_t folio, uint32_t ordinal) {
    if (!g_installed) folio_install_all();
    if (folio >= M2_FOLIO_COUNT || ordinal >= M2_VECTORS_PER_FOLIO)
        return const_cast<void*>(g_stub);
    return reinterpret_cast<void*>(g_table[folio][ordinal]);
}

const char* selector_name(uint32_t folio, uint32_t ordinal) {
    if (const SelectorName* sn = find_selector_name(m2_make_selector(folio, ordinal)))
        return sn->name;
    return nullptr;
}

void folio_set_guest_probe(GuestSelectorProbeFn probe) {
    g_guest_probe = probe;
}

namespace {

const char* folio_display_name(uint32_t folio) {
    static const char* kFolioNames[M2_FOLIO_COUNT] = {
        "(unused)", "kernel", "iff", "graphics",
        "filesystem", "audiopatch", "audio", "mpegaudiodecoder",
        "gstate", "font",
    };
    return folio < M2_FOLIO_COUNT ? kFolioNames[folio] : "(unknown)";
}

bool static_implemented(uint32_t f, uint32_t o) {
    return reinterpret_cast<void*>(g_table[f][o]) != const_cast<void*>(g_stub) ||
           (g_guest_probe && g_guest_probe(f, o) != nullptr);
}

int observed(uint32_t f, uint32_t o) {
    return g_observed_probe ? g_observed_probe(f, o) : -1;
}

}  // namespace

void folio_print_report(FILE* out) {
    std::fprintf(out,
                 "[folio-report] static = host table + guest probe "
                 "(prediction); observed = dispatch census this process%s\n",
                 g_observed_probe ? "" : " (census off: no probe installed)");

    for (uint32_t f = 1; f < M2_FOLIO_COUNT; ++f) {
        int static_count = 0;
        for (uint32_t o = 0; o < M2_VECTORS_PER_FOLIO; ++o)
            if (static_implemented(f, o)) ++static_count;

        int owned = 0, served = 0, unhandled = 0, stubbed = 0, calls_missing = 0;
        (void)unhandled;
        std::vector<std::string> lines;
        for (const SelectorName* sn = g_names; sn != g_names + g_names_count; ++sn) {
            const uint32_t f2 = sn->sel >> 16, o2 = sn->sel & 0xFFFFu;
            if (f2 != f) continue;
            char buf[256];
            if (static_implemented(f, o2)) {
                ++owned;
                std::snprintf(buf, sizeof buf,
                              "    OWNED  %s:%-3u %s (%d calls) [static]",
                              folio_display_name(f), o2,
                              sn->name ? sn->name : "(unresolved)", sn->call_count);
                lines.emplace_back(buf);
                continue;
            }
            const int obs = observed(f, o2);
            if (obs == 1) {
                ++served;
                std::snprintf(buf, sizeof buf,
                              "    OWNED  %s:%-3u %s (%d calls) [observed served]",
                              folio_display_name(f), o2,
                              sn->name ? sn->name : "(unresolved)", sn->call_count);
                lines.emplace_back(buf);
                continue;
            }
            ++stubbed;
            calls_missing += sn->call_count;
            if (obs == 0) ++unhandled;
            std::snprintf(buf, sizeof buf,
                          "    STUB   %s:%-3u %s (%d calls) [%s]",
                          folio_display_name(f), o2,
                          sn->name ? sn->name : "(unresolved)", sn->call_count,
                          obs == 0 ? "observed unhandled"
                                   : "not dispatched this run");
            lines.emplace_back(buf);
        }
        std::fprintf(out,
                     "[folio %u %-16s] static-impl:%4d  named:%3d "
                     "(owned %3d / served %3d / stub %3d)  missing-call-sites:%4d\n",
                     f, folio_display_name(f), static_count,
                     owned + served + stubbed, owned, served, stubbed,
                     calls_missing);
        for (const std::string& l : lines) std::fprintf(out, "%s\n", l.c_str());
    }
}

void folio_print_observed_report(FILE* out) {
    if (!g_observed_probe) {
        std::fprintf(out, "[folio-observed] census off: no probe installed\n");
        return;
    }
    std::fprintf(out,
                 "[folio-observed] measured = dispatch census after the run; "
                 "a selector counted handled is never a stub\n");
    for (uint32_t f = 1; f < M2_FOLIO_COUNT; ++f) {
        int served = 0, unhandled = 0, stubbed = 0, calls_missing = 0, seen = 0;
        std::vector<std::string> lines;
        for (const SelectorName* sn = g_names; sn != g_names + g_names_count; ++sn) {
            const uint32_t f2 = sn->sel >> 16, o2 = sn->sel & 0xFFFFu;
            if (f2 != f) continue;
            const int obs = observed(f, o2);
            if (obs < 0) {
                ++stubbed;
                calls_missing += sn->call_count;
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "    STUB   %s:%-3u %s (%d calls) [never dispatched]",
                              folio_display_name(f), o2,
                              sn->name ? sn->name : "(unresolved)", sn->call_count);
                lines.emplace_back(buf);
                continue;
            }
            ++seen;
            char buf[256];
            if (obs == 1) {
                ++served;
                std::snprintf(buf, sizeof buf,
                              "    OWNED  %s:%-3u %s (%d calls) [observed served]",
                              folio_display_name(f), o2,
                              sn->name ? sn->name : "(unresolved)", sn->call_count);
            } else {
                ++unhandled;
                std::snprintf(buf, sizeof buf,
                              "    STUB   %s:%-3u %s (%d calls) [observed unhandled]",
                              folio_display_name(f), o2,
                              sn->name ? sn->name : "(unresolved)", sn->call_count);
            }
            lines.emplace_back(buf);
        }
        std::fprintf(out,
                     "[folio %u %-16s] measured: reached %3d "
                     "(served %3d / unhandled %3d)  never-dispatched(stub) %3d  "
                     "missing-call-sites:%4d\n",
                     f, folio_display_name(f), seen, served, unhandled,
                     stubbed, calls_missing);
        for (const std::string& l : lines) std::fprintf(out, "%s\n", l.c_str());
    }
}

void folio_install_all() {
    if (g_installed) return;
    g_installed = true;

    g_stub = reinterpret_cast<const void*>(&shim_stub_unimplemented);
    for (auto& row : g_table)
        for (auto& slot : row)
            slot = reinterpret_cast<ShimFn*>(const_cast<void*>(g_stub));

    kernel_shim_register();
    iff_shim_register();
    graphics_shim_register();
    filesystem_shim_register();
    audiopatch_shim_register();
    audio_shim_register();
    mpeg_decoder_register();
}

}  // namespace m2host
