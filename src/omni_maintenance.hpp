// Omni maintenance requests run on the wired USB control endpoint.
#pragma once
#include <array>
#include <cstdint>

namespace omni {
class StickMapper;
namespace maintenance {
constexpr std::uint8_t probe_type = 0xc0, probe_request = 0xb7; // Vendor IN, device.
constexpr std::uint8_t enter_type = 0x40, enter_request = 0xb8; // Vendor OUT, device.
constexpr std::uint8_t capture_request = 0xb9;                  // Vendor IN, strict capture image only.
constexpr std::uint8_t motion_request = 0xba;                   // Vendor IN, pinned personal image only.
constexpr std::uint8_t nudge_read_request = 0xbb,
                       nudge_write_request = 0xbc; // Operating images only, via wired USB.
using PeerPin = std::array<std::uint8_t, 6>;
using CaptureReply = std::array<std::uint8_t, 8>; // Format 1, key length 7, canonical address.
using MotionReply = std::array<std::uint8_t, 64>; // Format 1, aggregate mouse diagnostics.
constexpr std::uint16_t request_value = 0x4f4d, request_index = 0x4e49;
constexpr std::array<std::uint8_t, 8> capability{'O', 'M', 'N', 'I', 'B', 'L', '0', '1'};
} // namespace maintenance
void maintenance_usb_reset();
bool maintenance_poll(StickMapper &mapper, std::uint64_t now_us);
} // namespace omni
