#pragma once
#include <cstdint>
#include <array>

namespace omni {
enum class BtState { starting, unpaired, searching, connecting, connected, reconnecting, fault };
struct BtCallbacks {
    void (*mouse)(std::int16_t dx, std::int16_t dy, std::int16_t wheel, std::uint8_t buttons);
    void (*neutral)();
};
// Single polling context. Initialize the SDK CYW43/BTstack transport first.
bool bt_start(BtCallbacks callbacks);
void bt_tick(std::uint64_t now_ms);
void bt_pair();   // Renew discovery when unpaired. A saved mouse keeps its identity.
void bt_forget(); // Explicitly remove the saved peer/key and neutralize any current input.
BtState bt_state();
// Last diagnostic milestone or latched failure. The USB feature exposes only
// codes/counters, never a Bluetooth address, device name, key or raw input.
enum class BtStage : std::uint8_t {
    none = 0,
    inquiry_started = 1,
    inquiry_complete = 2,
    name_lookup = 3,
    slot2_selected = 4,
    hid_connect_started = 5,
    acl_connected = 6,
    pin_sent = 7,
    ssp_confirmed = 8,
    authenticated = 9,
    encrypted = 10,
    link_key_seen = 11,
    hid_open = 12,
    descriptor_accepted = 13,
    report_protocol = 14,
    mouse_ready = 15,
    mouse_report = 16,
    inquiry_failed = 0x80,
    ambiguous = 0x81,
    connect_failed = 0x82,
    acl_failed = 0x83,
    authentication_failed = 0x84,
    encryption_failed = 0x85,
    hid_open_failed = 0x86,
    descriptor_missing = 0x87,
    descriptor_unsupported = 0x88,
    protocol_failed = 0x89,
    connect_timeout = 0x8a,
    disconnected = 0x8b,
    malformed_report = 0x8c,
    storage = 0x8d,
    teardown_timeout = 0x8e,
    malformed_hid = 0x8f,
    security_request_failed = 0x90,
    security_below_level2 = 0x91,
    personal_policy = 0x92
};
struct BtDiagnostic {
    std::uint8_t stage{}, detail{};
};
BtDiagnostic bt_diagnostic();
// Pinned build only: a v1 fixed-size, little-endian aggregate of authenticated
// Bluetooth inputs (no addresses, raw HID payload or controller credentials).
// Other variants clear out and return false. Forget resets all counts.
bool bt_copy_motion_snapshot(std::array<std::uint8_t, 64> &out);
// A hung SDP teardown needs a bounded neutral USB shutdown and firmware restart.
bool bt_recovery_requested();
// Only the strict wired-observation build can export a just-observed address.
// Returns false and zeros out in all other builds or without a fresh verified observation.
bool bt_copy_short_key_candidate(std::array<std::uint8_t, 6> &out);
} // namespace omni
