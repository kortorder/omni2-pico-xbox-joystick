// Host-only decoder for v1 of the pinned Pico's read-only, aggregated motion snapshot.
#pragma once
#include "omni_maintenance.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace omni::motion_watch {
enum Counter : unsigned {
    raw,
    accepted,
    unrelated,
    malformed,
    left,
    right,
    up,
    down,
    wheel_negative,
    wheel_positive,
    button_pressed,
    button_released,
    zero_only,
    counter_count
};
constexpr const char *labels[counter_count] = {
    "frames", "mouse",  "ignored", "malformed",    "left",           "right",    "up",
    "down",   "wheel-", "wheel+",  "button-press", "button-release", "zero-only"};
struct Snapshot {
    std::array<std::uint32_t, counter_count> counters{};
    std::uint8_t outcome{}, report_id{}, last_buttons{}, last_meaningful_id{};
    int last_x{}, last_y{}, last_wheel{};
};
inline int signedLE(const std::uint8_t *p) {
    const int value = p[0] | int(p[1]) << 8;
    return value >= 32768 ? value - 65536 : value;
}
inline std::optional<Snapshot> decode(const maintenance::MotionReply &wire) {
    if (wire[0] != 1 || wire[1] != wire.size() || wire[2] > 3)
        return {};
    Snapshot sample;
    sample.outcome = wire[2];
    sample.report_id = wire[3];
    for (unsigned i = 0; i < counter_count; ++i) {
        const auto *p = wire.data() + 4 + 4 * i;
        sample.counters[i] = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                             (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }
    sample.last_x = signedLE(&wire[56]);
    sample.last_y = signedLE(&wire[58]);
    sample.last_wheel = signedLE(&wire[60]);
    sample.last_buttons = wire[62];
    sample.last_meaningful_id = wire[63];
    return sample;
}
struct Delta {
    std::array<std::uint32_t, counter_count> counts{};
    std::array<bool, counter_count> lower_bounds{};
    bool reset = false;
    bool changed() const {
        return std::any_of(counts.begin(), counts.end(), [](auto n) { return n != 0; });
    }
};
inline Delta between(const Snapshot &first, const Snapshot &last) {
    Delta delta;
    for (unsigned i = 0; i < counter_count; ++i) {
        if (last.counters[i] < first.counters[i]) {
            delta.reset = true;
            return delta;
        }
        delta.counts[i] = last.counters[i] - first.counters[i];
        delta.lower_bounds[i] = last.counters[i] == UINT32_MAX;
    }
    return delta;
}
inline void print(const char *prefix, long long millis, const Delta &delta, const Snapshot &last) {
    std::printf("%lldms %s", millis, prefix);
    for (unsigned i = 0; i < counter_count; ++i)
        std::printf(" %s=%s%u", labels[i], delta.lower_bounds[i] ? ">=" : "", delta.counts[i]);
    const char *outcome[] = {"none", "mouse", "unrelated", "malformed"};
    std::printf(" outcome=%s/id%u retainedXY=(%d,%d) retainedWheel=%d retainedButtons=0x%02x "
                "meaningfulID=%u\n",
                outcome[last.outcome], unsigned(last.report_id), last.last_x, last.last_y,
                last.last_wheel, unsigned(last.last_buttons), unsigned(last.last_meaningful_id));
    std::fflush(stdout);
}
} // namespace omni::motion_watch
