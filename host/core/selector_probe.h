// selector_probe.h — selector probes for folio_print_report().
#pragma once

#include <cstdint>

namespace m2host {

void* guest_registry_probe(uint32_t folio, uint32_t ordinal);
int guest_observed_probe(uint32_t folio, uint32_t ordinal);

}  // namespace m2host
