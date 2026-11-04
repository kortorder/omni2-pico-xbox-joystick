// Wired v1 nudge settings transport. The only persistent action is an explicit save.
#pragma once
#include "omni_maintenance.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace nudge {
using Slots = std::array<std::uint8_t, 4>;
using Wire = std::array<std::uint8_t, 16>;
struct Record {
    std::uint32_t revision = 0;
    Slots slots{};
    std::uint8_t storage = 0, result = 0;
};
constexpr unsigned get_request = omni::maintenance::nudge_read_request,
                   set_request = omni::maintenance::nudge_write_request;
inline std::optional<std::uint32_t> number(std::string_view s, std::uint32_t max) {
    std::uint32_t value = 0;
    if (s.empty() || s.size() > 10)
        return {};
    auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || value > max)
        return {};
    return value;
}
inline bool valid(const Slots &slots) {
    for (auto b : slots)
        if (b > 12)
            return false;
    return true;
}
inline std::optional<Record> decode(const Wire &w) {
    if (std::memcmp(w.data(), "NGR1", 4) != 0 || w[12] > 3 || w[13] > 6 || w[14] || w[15])
        return {};
    Record r;
    for (unsigned i = 0; i < 4; ++i) {
        r.revision |= static_cast<std::uint32_t>(w[4 + i]) << (8 * i);
        r.slots[i] = w[8 + i];
    }
    r.storage = w[12];
    r.result = w[13];
    if (!valid(r.slots) || (r.storage >= 2 && r.slots != Slots{}))
        return {};
    return r;
}
inline Wire encode(std::uint32_t revision, const Slots &slots) {
    Wire w{};
    std::memcpy(w.data(), "NGW1", 4);
    std::memcpy(w.data() + 12, "SAVE", 4);
    for (unsigned i = 0; i < 4; ++i) {
        w[4 + i] = static_cast<std::uint8_t>(revision >> (8 * i));
        w[8 + i] = slots[i];
    }
    return w;
}
inline void print(const Record &r) {
    std::printf("{\"revision\":%u,\"slots\":[%u,%u,%u,%u],\"storage\":%u,\"result\":%u}\n",
                r.revision, unsigned(r.slots[0]), unsigned(r.slots[1]), unsigned(r.slots[2]),
                unsigned(r.slots[3]), unsigned(r.storage), unsigned(r.result));
}
} // namespace nudge
