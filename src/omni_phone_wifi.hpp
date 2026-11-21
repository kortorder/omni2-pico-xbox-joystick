#pragma once
#include <array>
#include <cstdint>
namespace omni {
using PhoneWifiStatus = std::array<std::uint8_t, 12>;
int phone_wifi_radio_init();
void phone_wifi_start();
void phone_wifi_poll(std::uint64_t now);
PhoneWifiStatus phone_wifi_status();
} // namespace omni
