#include "omni_stick.hpp"
#include <algorithm>
#include <cmath>

namespace omni {
void StickMapper::clear_motion() {
    bins_.fill({});
    last_time_ = 0;
}
void StickMapper::connect() {
    clear_motion();
    connected_ = true;
    ++generation_;
}
void StickMapper::disconnect() {
    clear_motion();
    connected_ = false;
    ++generation_;
}

void StickMapper::motion(std::int32_t dx, std::int32_t dy, std::uint64_t now) {
    if (!connected_)
        return;
    if (now < last_time_) {
        clear_motion();
        return;
    }
    last_time_ = now;
    if (dx == 0 && dy == 0)
        return; // Zero/button reports do not extend a movement's lifetime.
    const auto number = now / bin_us;
    auto &bin = bins_[number % bins_.size()];
    if (bin.last / bin_us != number)
        bin = {};
    bin.last = now;
    // Bounds protect both relative 32-bit HID fields and bursts in the same 4ms bin.
    constexpr std::int64_t bound = 1 << 20;
    bin.x = static_cast<std::int32_t>(std::clamp(std::int64_t(bin.x) + dx, -bound, bound));
    bin.y = static_cast<std::int32_t>(std::clamp(std::int64_t(bin.y) + dy, -bound, bound));
}

Axes StickMapper::sample(std::uint64_t now) {
    if (!connected_)
        return {};
    if (now < last_time_) {
        clear_motion();
        return {};
    }
    last_time_ = now;
    std::int64_t x = 0, y = 0;
    for (const auto &bin : bins_) {
        if (now - bin.last < stationary_us) {
            x += bin.x;
            y += bin.y;
        }
    }
    if (x == 0 && y == 0)
        return {};
    // Saturate radially to retain the movement angle even beyond full deflection.
    const float length_squared = static_cast<float>(x * x + y * y);
    const float radius = length_squared > counts_for_full_scale * counts_for_full_scale
                             ? std::sqrt(length_squared)
                             : static_cast<float>(counts_for_full_scale);
    const float scale = 32767.0f / radius;
    const auto axis = [scale](std::int64_t value) {
        const auto normalized = static_cast<float>(value) * scale;
        return static_cast<std::int16_t>(std::lround(std::clamp(normalized, -32767.0f, 32767.0f)));
    };
    return {axis(x), axis(y)};
}
} // namespace omni
