#include "vbl_pacer.h"

namespace m2host::timing {

void VblPacer::advance() {
    constexpr uint64_t whole_ns =
        kPeriodNumeratorNs / kPeriodDenominator;
    constexpr uint32_t remainder_ns = static_cast<uint32_t>(
        kPeriodNumeratorNs % kPeriodDenominator);
    next_ns_ += whole_ns;
    fractional_ns_ += remainder_ns;
    if (fractional_ns_ >= kPeriodDenominator) {
        ++next_ns_;
        fractional_ns_ -= kPeriodDenominator;
    }
}

bool VblPacer::due(uint64_t now_ns) {
    if (!started_ || (now_ns > next_ns_ &&
                      now_ns - next_ns_ > kResyncNs)) {
        next_ns_ = now_ns;
        fractional_ns_ = 0;
        started_ = true;
    }
    if (now_ns < next_ns_) return false;

    advance();
    // Preserve the one-permit-per-call behavior after a long host stall.
    // Re-anchor to the observed wall clock and schedule one future
    // deadline instead of replaying every missed VBL.
    if (next_ns_ <= now_ns) {
        next_ns_ = now_ns;
        fractional_ns_ = 0;
        advance();
    }
    return true;
}

}  // namespace m2host::timing
