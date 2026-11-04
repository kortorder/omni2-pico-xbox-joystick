// Relative Bluetooth mouse motion to one absolute USB joystick. No allocation or I/O.
#pragma once
#include <array>
#include <cstdint>

namespace omni {
struct Axes {
    std::int16_t x = 0;
    std::int16_t y = 0; // USB coordinates: positive right/down.
};
constexpr bool operator==(Axes a, Axes b) {
    return a.x == b.x && a.y == b.y;
}
constexpr bool operator!=(Axes a, Axes b) {
    return !(a == b);
}

class StickMapper {
  public:
    // Prototype starting sensitivity, independent of mouse report frequency.
    static constexpr std::uint64_t stationary_us = 120000;
    static constexpr std::int32_t counts_for_full_scale = 48;
    void connect();
    void disconnect();
    void clear_motion();
    void motion(std::int32_t dx, std::int32_t dy, std::uint64_t now_us);
    Axes sample(std::uint64_t now_us);
    std::uint32_t generation() const {
        return generation_;
    }

  private:
    static constexpr std::uint64_t bin_us = 4000;
    // One extra slot prevents overwriting a still-live bin at a boundary.
    struct Bin {
        std::uint64_t last = 0;
        std::int32_t x = 0, y = 0;
    };
    std::array<Bin, stationary_us / bin_us + 1> bins_{};
    bool connected_ = false;
    std::uint64_t last_time_ = 0;
    std::uint32_t generation_ = 0;
};
} // namespace omni
