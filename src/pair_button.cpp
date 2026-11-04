// BOOTSEL sampling is adapted from Raspberry Pi's pico-examples/picoboard/button.
// Copyright (c) 2020 Raspberry Pi (Trading) Ltd. SPDX-License-Identifier: BSD-3-Clause
#include "pair_button.hpp"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "omni_bluetooth.hpp"
#include "pico/platform.h"

namespace omni {
namespace {
// Only called between radio polls on the single RP2040 core. Flash chip select
// must be restored before any flash code (including an IRQ handler) executes.
bool __no_inline_not_in_flash_func(read_button)() {
    const uint32_t flags = save_and_disable_interrupts();
    volatile uint32_t *ctrl = &ioqspi_hw->io[1].ctrl;
    hw_write_masked(ctrl, GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int settle = 0; settle < 1000; ++settle) {
    }
    const bool pressed = !(sio_hw->gpio_hi_in & (1u << 1));
    hw_write_masked(ctrl, GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}
} // namespace

void poll_pair_button(std::uint64_t now_ms) {
    static std::uint64_t next_poll = 0, started = 0;
    static bool was_down = false;
    if (now_ms < next_poll)
        return;
    next_poll = now_ms + 40;
    const bool down = read_button();
    if (down && !was_down)
        started = now_ms;
    if (!down && was_down && now_ms - started >= 80) {
        if (now_ms - started >= 8000)
            bt_forget();
        bt_pair();
    }
    was_down = down;
}
} // namespace omni
