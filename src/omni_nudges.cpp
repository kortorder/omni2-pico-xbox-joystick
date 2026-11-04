#include "omni_nudges.hpp"

namespace omni {
bool NudgeMap::valid() const {
    return left <= 12 && right <= 12 && up <= 12 && down <= 12;
}
std::uint8_t NudgeMap::button(NudgeDirection direction) const {
    switch (direction) {
    case NudgeDirection::left: return left;
    case NudgeDirection::right: return right;
    case NudgeDirection::up: return up;
    case NudgeDirection::down: return down;
    }
    return 0;
}
bool NudgeMapper::set_map(NudgeMap map) {
    if (!map.valid())
        return false;
    reset();
    map_ = map;
    return true;
}
void NudgeMapper::reset() {
    last_time_.reset();
    baseline_at_.reset();
    motion_at_.reset();
    last_wheel_.reset();
    click_.reset();
    pulse_.reset();
    previous_buttons_ = 0;
    primed_ = false;
}
bool NudgeMapper::advance(std::uint64_t now) {
    if (last_time_ && now < *last_time_) {
        reset();
        return false;
    }
    last_time_ = now;
    if (pulse_ && now - pulse_->at >= quiet_after_us + pulse_us)
        pulse_.reset();
    return true;
}
void NudgeMapper::report(std::int16_t dx, std::int16_t dy, std::int16_t wheel,
                         std::uint8_t buttons, std::uint64_t now) {
    if (!advance(now))
        return;
    const auto previous = previous_buttons_;
    previous_buttons_ = buttons;
    const bool moved = dx != 0 || dy != 0;
    if (!primed_) {
        // Fresh, quiet reports after connection, remount, or changing the map.
        // A button held over reset or a still-running scroll burst cannot arm.
        if (moved || buttons || wheel)
            baseline_at_.reset();
        else if (!baseline_at_)
            baseline_at_ = now;
        else if (now - *baseline_at_ >= wheel_gap_us)
            primed_ = true;
        return;
    }

    const bool continued_wheel = last_wheel_ && now - *last_wheel_ < wheel_gap_us;
    if (wheel)
        last_wheel_ = now; // Track suppressed bursts too: their tails are not new gestures.
    if (moved) {
        motion_at_ = now;
        click_.reset();
        pulse_.reset();
        return;
    }
    if (motion_at_ && now - *motion_at_ < quiet_before_us)
        return;

    if (wheel) {
        // Conflicting button/wheel evidence is ambiguous. Changing wheel sign
        // inside a burst cannot start a second pulse.
        const auto direction = wheel > 0 ? NudgeDirection::up : NudgeDirection::down;
        if (buttons || previous || click_ || (pulse_ && pulse_->direction != direction)) {
            pulse_.reset();
            click_.reset();
            return;
        }
        if (!continued_wheel && !pulse_)
            pulse_ = Gesture{direction, now};
        return;
    }
    if (buttons == previous)
        return;
    if (buttons) {
        // A simultaneous or overlapping click is not a new isolated gesture.
        if (previous || continued_wheel || pulse_ || (buttons != 1 && buttons != 2)) {
            click_.reset();
            pulse_.reset();
        } else {
            click_ = Gesture{buttons == 1 ? NudgeDirection::left : NudgeDirection::right, now};
        }
    } else if (click_) {
        // Emit only after release. A pointer change during the press has already
        // invalidated the click, even if the instantaneous axis has recentered.
        if (now - click_->at <= 5'000'000)
            pulse_ = Gesture{click_->direction, now};
        click_.reset();
    }
}
std::uint16_t NudgeMapper::sample(std::uint64_t now) {
    if (!advance(now) || !pulse_ || now - pulse_->at < quiet_after_us)
        return 0;
    const auto position = map_.button(pulse_->direction);
    return position ? static_cast<std::uint16_t>(1u << (position - 1)) : 0;
}
} // namespace omni
