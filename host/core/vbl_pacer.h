// vbl_pacer.h — rational NTSC VBL deadline scheduler.
#pragma once

#include <cstdint>

namespace m2host::timing {

class VblPacer {
public:
    // 59.94 fields/second = 60000/1001 Hz, as used by the BDA projector setup
    // and boot clock conversion.
    static constexpr uint64_t kPeriodNumeratorNs = 1001ull * 1000000000ull;
    static constexpr uint32_t kPeriodDenominator = 60000u;
    static constexpr uint64_t kResyncNs = 500000000ull;

    // Returns true for at most one permit at a call. A late caller is
    // re-anchored after the current permit; missed VBLs are never replayed as
    // a burst into the guest's one-bit signal.
    bool due(uint64_t now_ns);

    // The next absolute deadline is exposed for deterministic contract tests.
    uint64_t next_deadline_ns() const { return next_ns_; }

private:
    void advance();

    uint64_t next_ns_ = 0;
    uint32_t fractional_ns_ = 0;
    bool started_ = false;
};

}  // namespace m2host::timing
