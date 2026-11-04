#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace omni {
enum class MouseName : std::uint8_t { unsupported, named_slot2, pointing_slot2 };
MouseName match_mouse_name(const std::uint8_t *name, std::size_t length, std::uint32_t device_class);
struct MouseInput {
    std::int16_t dx{}, dy{}, wheel{};
    std::uint8_t buttons{};
};
// A compiled, bounded map of relative Mouse application fields. Other HID
// collections and report IDs can share the link without changing button state.
class MouseReports {
  public:
    bool configure(const std::uint8_t *descriptor, std::size_t size);
    std::uint8_t report_id(const std::uint8_t *report, std::size_t size) const {
        return numbered_ && report && size ? report[0] : 0;
    }
    enum class Result { mouse, unrelated, malformed };
    Result read(const std::uint8_t *report, std::size_t size, MouseInput &output);

  private:
    struct Field {
        std::uint8_t id, usage, bits;
        bool signed_value;
        std::uint16_t offset;
    };
    struct Layout {
        std::uint8_t id;
        std::uint16_t bits;
    };
    std::array<Field, 48> fields_{};
    std::array<Layout, 16> layouts_{};
    std::size_t field_count_{}, layout_count_{};
    std::uint8_t buttons_{};
    bool numbered_{}, valid_{};
};
} // namespace omni
