#pragma once
#include <cstdint>
void watchdog_enable(std::uint32_t, bool);
void watchdog_update();
void watchdog_reboot(std::uint32_t, std::uint32_t, std::uint32_t);
