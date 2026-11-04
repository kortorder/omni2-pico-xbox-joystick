#pragma once
#include "omni_mouse_report.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace omni {
// Read-only, aggregate Bluetooth input evidence for the private pinned build.
// The mapper and USB output never read these values.
class MotionCounters {
  public:
    void new_link() { previous_buttons_ = 0; }
    void observe(MouseReports::Result result, std::uint8_t id, const MouseInput &input = {}) {
        increment(raw);
        last_id_ = id;
        if (result == MouseReports::Result::unrelated) {
            outcome_ = 2;
            increment(unrelated);
            return;
        }
        if (result == MouseReports::Result::malformed) {
            outcome_ = 3;
            increment(malformed);
            return;
        }
        outcome_ = 1;
        increment(valid);
        const bool down = (input.buttons & static_cast<std::uint8_t>(~previous_buttons_)) != 0;
        const bool up = (previous_buttons_ & static_cast<std::uint8_t>(~input.buttons)) != 0;
        previous_buttons_ = input.buttons;
        if (input.dx < 0)
            increment(negative_x);
        else if (input.dx > 0)
            increment(positive_x);
        if (input.dy < 0)
            increment(negative_y);
        else if (input.dy > 0)
            increment(positive_y);
        if (input.dx || input.dy) {
            last_x_ = input.dx;
            last_y_ = input.dy;
        }
        if (input.wheel < 0)
            increment(negative_wheel);
        else if (input.wheel > 0)
            increment(positive_wheel);
        if (input.wheel)
            last_wheel_ = input.wheel;
        if (down)
            increment(button_down);
        if (up)
            increment(button_up);
        if (input.buttons)
            last_buttons_ = input.buttons;
        if (!input.dx && !input.dy && !input.wheel && !input.buttons && !up)
            increment(zero_only);
        else
            last_meaningful_id_ = id;
    }
    void encode(std::array<std::uint8_t, 64> &out) const {
        out.fill(0);
        out[0] = 1;
        out[1] = 64;
        out[2] = outcome_;
        out[3] = last_id_;
        for (std::size_t i = 0; i < counts_.size(); ++i)
            for (unsigned b = 0; b < 4; ++b)
                out[4 + 4 * i + b] = static_cast<std::uint8_t>(counts_[i] >> (8 * b));
        const auto s16 = [&](unsigned offset, std::int16_t n) {
            const auto u = static_cast<std::uint16_t>(n);
            out[offset] = static_cast<std::uint8_t>(u);
            out[offset + 1] = static_cast<std::uint8_t>(u >> 8);
        };
        s16(56, last_x_);
        s16(58, last_y_);
        s16(60, last_wheel_);
        out[62] = last_buttons_;
        out[63] = last_meaningful_id_;
    }

  private:
    enum Counter : unsigned {
        raw,
        valid,
        unrelated,
        malformed,
        negative_x,
        positive_x,
        negative_y,
        positive_y,
        negative_wheel,
        positive_wheel,
        button_down,
        button_up,
        zero_only,
        count
    };
    void increment(Counter c) {
        if (counts_[c] != std::numeric_limits<std::uint32_t>::max())
            ++counts_[c];
    }
    std::array<std::uint32_t, count> counts_{};
    std::int16_t last_x_ = 0, last_y_ = 0, last_wheel_ = 0;
    std::uint8_t previous_buttons_ = 0, outcome_ = 0, last_id_ = 0, last_buttons_ = 0,
                 last_meaningful_id_ = 0;
};
} // namespace omni
