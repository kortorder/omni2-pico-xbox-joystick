#pragma once
#include "omni_nudges.hpp"
#include "omni_stick.hpp"
namespace omni {
// Call on the same core as the Bluetooth callbacks, from the main polling loop.
void usb_task(StickMapper &mapper, std::uint64_t now_us);
void usb_nudge_report(std::int16_t dx, std::int16_t dy, std::int16_t wheel, std::uint8_t buttons,
                      std::uint64_t now_us);
void usb_cancel_nudges();
bool usb_set_nudge_map(NudgeMap map);
NudgeMap usb_nudge_map();
// Begin one neutral barrier before a settings change. Poll usb_task until confirmed.
void usb_request_neutral(StickMapper &mapper);
bool usb_neutral_confirmed();
// Internal diagnostics only. HID host reads these through the Feature report.
void usb_status(std::uint8_t state, std::uint32_t mouse_reports, std::uint8_t pairing_stage = 0,
                std::uint8_t pairing_detail = 0);
} // namespace omni
