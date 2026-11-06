/* test_scratch.h — per-process suffix for scratch directory names, so two
 * concurrent runs of the same test (for example ctest in two build trees at
 * once) never delete each other's fixtures. Test-only. */
#pragma once

#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

inline std::string test_scratch_suffix() {
#if defined(_WIN32)
    return "_" + std::to_string(_getpid());
#else
    return "_" + std::to_string(static_cast<long long>(getpid()));
#endif
}
