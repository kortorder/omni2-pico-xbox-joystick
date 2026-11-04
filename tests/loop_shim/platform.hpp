#pragma once
#include <cstdint>
bool tusb_init();
void sleep_us(std::uint64_t);
void tight_loop_contents();
