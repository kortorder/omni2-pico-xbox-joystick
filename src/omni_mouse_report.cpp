#include "omni_mouse_report.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace omni {
namespace {
bool matches(const std::uint8_t *bytes, std::size_t size, const char *text) {
    if (size != std::strlen(text))
        return false;
    for (std::size_t i = 0; i < size; ++i) {
        auto c = bytes[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<std::uint8_t>(c - 'A' + 'a');
        if (c != static_cast<std::uint8_t>(text[i]))
            return false;
    }
    return true;
}
struct Global {
    std::uint32_t page = 0, size = 0, count = 0;
    std::uint8_t id = 0;
    bool negative = false;
};
struct Local {
    std::array<std::uint32_t, 32> usages{};
    std::size_t count = 0;
    std::uint32_t minimum = 0, maximum = 0;
    bool has_min = false, has_max = false;
    std::uint32_t get(std::size_t n) const {
        if (n < count)
            return usages[n];
        if (has_min && has_max && minimum <= maximum && (minimum >> 16) == (maximum >> 16))
            return minimum +
                   std::min<std::uint32_t>(static_cast<std::uint32_t>(n - count), maximum - minimum);
        return count ? usages[count - 1] : 0;
    }
};
std::int16_t bounded(std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}
} // namespace
MouseName match_mouse_name(const std::uint8_t *name, std::size_t n, std::uint32_t device_class) {
    if (!name || n > 248)
        return MouseName::unsupported;
    while (n && (name[n - 1] == 0 || name[n - 1] == ' '))
        --n;
    if (matches(name, n, "r-net mouse 2"))
        return MouseName::named_slot2;
    // Peripheral major class (0x0500); minor bit 7 means pointing device,
    // including a combined keyboard/pointing device. Other generic mice do not qualify.
    if ((device_class & 0x1f80) == 0x0580 && matches(name, n, "mouse 2"))
        return MouseName::pointing_slot2;
    return MouseName::unsupported;
}
bool MouseReports::configure(const std::uint8_t *d, std::size_t n) {
    *this = {};
    if (!d || !n || n > 2048)
        return false;
    Global g;
    Local local;
    std::array<Global, 4> stack{};
    std::size_t depth = 0;
    std::array<bool, 16> collections{};
    std::size_t level = 0;
    bool has_x = false, has_y = false;
    for (std::size_t p = 0; p < n;) {
        const auto prefix = d[p++];
        if (prefix == 0xfe)
            return false; // Long items unsupported.
        const std::size_t length = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (n - p < length)
            return false;
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < length; ++i)
            v |= std::uint32_t(d[p++]) << (8 * i);
        const auto tag = prefix >> 4, type = (prefix >> 2) & 3;
        if (type == 1) {
            switch (tag) {
            case 0:
                if (v > 0xffff)
                    return false;
                g.page = v;
                break;
            case 1:
                g.negative = length && (v & (std::uint32_t(1) << (length * 8 - 1)));
                break;
            case 7:
                g.size = v;
                break;
            case 8:
                if (!v || v > 255)
                    return false;
                g.id = static_cast<std::uint8_t>(v);
                numbered_ = true;
                break;
            case 9:
                g.count = v;
                break;
            case 10:
                if (depth == stack.size())
                    return false;
                stack[depth++] = g;
                break;
            case 11:
                if (!depth)
                    return false;
                g = stack[--depth];
                break;
            default:
                break;
            }
            continue;
        }
        if (type == 2) {
            const auto usage = length == 4 ? v : (g.page << 16) | v;
            if (tag == 0) {
                if (local.count == local.usages.size())
                    return false;
                local.usages[local.count++] = usage;
            } else if (tag == 1) {
                local.minimum = usage;
                local.has_min = true;
            } else if (tag == 2) {
                local.maximum = usage;
                local.has_max = true;
            } else if (tag == 10)
                return false; // Alternative usage sets are ambiguous here.
            continue;
        }
        if (type != 0)
            continue;
        if (tag == 10) {
            if (level == collections.size())
                return false;
            const bool mouse = v == 1 ? local.get(0) == 0x00010002 : (level && collections[level - 1]);
            collections[level++] = mouse;
        } else if (tag == 12) {
            if (!level)
                return false;
            --level;
        } else if (tag == 8) {
            if (!g.size || g.size > 32 || !g.count || g.count > 512 || g.count * g.size > 4096)
                return false;
            std::size_t li = 0;
            while (li < layout_count_ && layouts_[li].id != g.id)
                ++li;
            if (li == layout_count_) {
                if (li == layouts_.size())
                    return false;
                layouts_[layout_count_++] = {g.id, 0};
            }
            auto &offset = layouts_[li].bits;
            if (offset + g.count * g.size > 4096)
                return false;
            if (level && collections[level - 1] && (v & 3) == 2) {
                for (std::uint32_t j = 0; j < g.count; ++j) {
                    const auto usage = local.get(j);
                    const auto page = usage >> 16, id = usage & 0xffff;
                    const bool axis = page == 1 && (id == 0x30 || id == 0x31 || id == 0x38) && (v & 4);
                    const bool button = page == 9 && id >= 1 && id <= 8 && !(v & 4);
                    if (!axis && !button)
                        continue;
                    if (field_count_ == fields_.size())
                        return false;
                    fields_[field_count_++] = {g.id, static_cast<std::uint8_t>(id),
                                               static_cast<std::uint8_t>(g.size), g.negative,
                                               static_cast<std::uint16_t>(offset + j * g.size)};
                    has_x |= axis && id == 0x30;
                    has_y |= axis && id == 0x31;
                }
            }
            offset = static_cast<std::uint16_t>(offset + g.size * g.count);
        }
        local = {};
    }
    if (numbered_)
        for (std::size_t i = 0; i < layout_count_; ++i)
            if (layouts_[i].id == 0)
                return false;
    valid_ = !depth && !level && has_x && has_y;
    return valid_;
}
MouseReports::Result MouseReports::read(const std::uint8_t *report, std::size_t n, MouseInput &out) {
    out = {};
    if (!valid_ || !report || !n)
        return Result::malformed;
    std::uint8_t id = 0;
    if (numbered_) {
        id = *report++;
        --n;
    }
    std::size_t li = 0;
    while (li < layout_count_ && layouts_[li].id != id)
        ++li;
    if (li == layout_count_)
        return Result::unrelated;
    if (n != (layouts_[li].bits + 7u) / 8u)
        return Result::malformed;
    bool found = false;
    auto buttons = buttons_;
    for (std::size_t i = 0; i < field_count_; ++i) {
        const auto &f = fields_[i];
        if (f.id != id)
            continue;
        found = true;
        std::uint32_t raw = 0;
        for (unsigned bit = 0; bit < f.bits; ++bit) {
            const auto index = f.offset + bit;
            raw |= std::uint32_t((report[index / 8] >> (index % 8)) & 1u) << bit;
        }
        std::int64_t v = raw;
        if (f.signed_value && (raw & (std::uint32_t(1) << (f.bits - 1))))
            v -= std::int64_t(1) << f.bits;
        if (f.usage == 0x30)
            out.dx = bounded(std::int64_t(out.dx) + v);
        else if (f.usage == 0x31)
            out.dy = bounded(std::int64_t(out.dy) + v);
        else if (f.usage == 0x38)
            out.wheel = bounded(std::int64_t(out.wheel) + v);
        else {
            const auto mask = static_cast<std::uint8_t>(1u << (f.usage - 1));
            if (v)
                buttons |= mask;
            else
                buttons &= static_cast<std::uint8_t>(~mask);
        }
    }
    if (!found)
        return Result::unrelated;
    out.buttons = buttons_ = buttons;
    return Result::mouse;
}
} // namespace omni
