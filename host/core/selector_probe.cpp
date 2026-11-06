// selector_probe.cpp — guest selector registration and observation probes.
#include "selector_probe.h"

#ifdef M2_HAVE_RECOMP

#include "recomp_helpers.h"
#include "font_folio.h"

namespace m2host {

void* guest_registry_probe(uint32_t folio, uint32_t ordinal) {
    if (folio == M2_FOLIO_KERNEL && m2_is_diab_helper(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio == M2_FOLIO_KERNEL && m2_is_generic_kernel_stub(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio == M2_FOLIO_KERNEL && m2_is_tag_call_helper(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio == M2_FOLIO_AUDIO && m2_is_audio_typed_selector(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio == M2_FOLIO_AUDIO && m2_is_generic_audio_stub(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio == M2_FOLIO_FONT && m2_font_selector_owned(ordinal))
        return reinterpret_cast<void*>(0x1);
    if (folio != M2_FOLIO_KERNEL) return nullptr;

    m2_kernel_dispatch* live = m2_kernel_dispatch_global();
    if (!live) return nullptr;
    m2_kernel_dispatch scratch;
    m2_kernel_dispatch_init(&scratch);
    static void* const kSentinel = reinterpret_cast<void*>(&guest_registry_probe);
    if (m2_kernel_dispatch_register(&scratch, ordinal, kSentinel) != 0)
        return nullptr;
    const unsigned slots = sizeof(scratch) / sizeof(void*);
    for (unsigned i = 0; i < slots; ++i)
        if ((reinterpret_cast<void* const*>(&scratch))[i] == kSentinel)
            return (reinterpret_cast<void**>(live))[i];
    return nullptr;
}

int guest_observed_probe(uint32_t folio, uint32_t ordinal) {
    return m2_selector_observed(folio, ordinal, nullptr, nullptr);
}

}  // namespace m2host

#else

namespace m2host {

void* guest_registry_probe(uint32_t, uint32_t) { return nullptr; }
int guest_observed_probe(uint32_t, uint32_t) { return -1; }

}  // namespace m2host

#endif  // M2_HAVE_RECOMP
