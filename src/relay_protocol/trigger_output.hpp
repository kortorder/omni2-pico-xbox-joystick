// Original portable contract. This file does not select pins, voltages or a circuit.
#pragma once
#include <cstdint>

namespace scx2 {
struct TriggerPressure {
    std::uint16_t left = 0, right = 0; // 0 released, 65535 full scale.
    bool neutral() const { return left == 0 && right == 0; }
    bool operator==(const TriggerPressure& other) const { return left == other.left && right == other.right; }
};
enum class TriggerApplyResult { accepted, expired, fault };

// A real backend must enforce its calibrated electrical limits and have a hardware
// released default during reset/Hi-Z/power loss. Methods must be bounded,
// nonblocking and non-reentrant. A digital contact switch cannot implement this
// proportional capability. The owner must outlive Receiver.
class TriggerOutputBackend {
public:
    virtual ~TriggerOutputBackend() = default;
    virtual bool ready() const noexcept = 0;
    // true means the complete pressure command was accepted, NOT measured at XAC.
    // false means the output cannot be trusted; Receiver latches a fault.
    virtual bool apply(TriggerPressure pressure) noexcept = 0;
    // Active-session operation. The deadline uses the Receiver's monotonic
    // microsecond domain. Check it immediately before enabling physical output;
    // expire released without a sticky hardware fault if work crossed the lease.
    // UINT64_MAX means no lease bound (used only for inactive neutral work).
    virtual TriggerApplyResult apply_before(TriggerPressure pressure,
                                             std::uint64_t deadline_us) noexcept = 0;
    // Immediate best-effort physical disable/zero, even if ready() is false.
    // Never defer this behind queued USB work. No success feedback is inferred.
    virtual void release() noexcept = 0;
};
} // namespace scx2
