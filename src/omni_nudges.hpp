// Mouse gestures to brief Xbox Adaptive Controller right-port digital button presses.
#pragma once
#include <cstdint>
#include <optional>

namespace omni {
enum class NudgeDirection { left, right, up, down };
struct NudgeMap {
    std::uint8_t left = 4, right = 12, up = 5, down = 3;
    static constexpr NudgeMap off() { return {0, 0, 0, 0}; }
    bool valid() const;
    std::uint8_t button(NudgeDirection direction) const;
};
class NudgeMapper {
  public:
    static constexpr std::uint64_t quiet_before_us = 120000;
    static constexpr std::uint64_t quiet_after_us = 80000;
    static constexpr std::uint64_t pulse_us = 80000;
    static constexpr std::uint64_t wheel_gap_us = 400000;

    bool set_map(NudgeMap map);
    NudgeMap map() const { return map_; }
    void reset();
    void report(std::int16_t dx, std::int16_t dy, std::int16_t wheel, std::uint8_t buttons,
                std::uint64_t now_us);
    std::uint16_t sample(std::uint64_t now_us);

  private:
    struct Gesture {
        NudgeDirection direction;
        std::uint64_t at;
    };
    NudgeMap map_ = NudgeMap::off();
    std::optional<std::uint64_t> last_time_, baseline_at_, motion_at_, last_wheel_;
    std::optional<Gesture> click_, pulse_;
    std::uint8_t previous_buttons_ = 0;
    bool primed_ = false;
    bool advance(std::uint64_t now);
};
} // namespace omni
