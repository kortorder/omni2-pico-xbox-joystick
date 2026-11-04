#include "hardware/watchdog.h"
#include "omni_bluetooth.hpp"
#include "omni_maintenance.hpp"
#include "omni_nudge_settings.hpp"
#include "omni_stick.hpp"
#include "omni_usb.hpp"
#include "pair_button.hpp"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "tusb.h"

namespace {
omni::StickMapper stick;
uint32_t reports;
bool input_live;

// Keep the camera path independent: every genuine delta reaches the same
// mapper whether or not a click/scroll is eligible for a nudge shortcut.
void mouse(int16_t x, int16_t y, int16_t wheel, uint8_t buttons) {
    if (omni::bt_state() != omni::BtState::connected)
        return;
    if (!input_live)
        stick.connect();
    input_live = true;
    ++reports;
    const auto now = time_us_64();
    stick.motion(x, y, now);
#if !OMNI_CAPTURE_SHORT_KEY
    omni::usb_nudge_report(x, y, wheel, buttons, now);
#else
    (void)wheel;
    (void)buttons;
#endif
}

void neutral() {
    input_live = false;
    stick.disconnect();
    omni::usb_cancel_nudges();
}

bool light(omni::BtState state, uint64_t ms) {
    using omni::BtState;
    switch (state) {
    case BtState::connected:
        return true;
    case BtState::connecting:
    case BtState::searching:
        return ms % 500 < 250;
    case BtState::reconnecting:
        return ms % 1500 < 150;
    case BtState::fault:
        return ms % 1000 < 500 && ms % 160 < 80;
    default:
        return false;
    }
}
} // namespace

int main() {
    // The RP2040 runs this loop on a single core. All Bluetooth callbacks run
    // inside cyw43_arch_poll, so the USB sample sees a consistent mouse state.
    if (!tusb_init())
        return 1;
    const bool radio_ready = cyw43_arch_init() == 0;
#if !OMNI_CAPTURE_SHORT_KEY
    // cyw43_arch_init sets up BTstack's flash TLV before HCI power-on.
    if (radio_ready)
        omni::nudge_settings_init();
#endif
    const bool host_ready = radio_ready && omni::bt_start({mouse, neutral});
    watchdog_enable(2000, false);
    bool last_led = false;
    uint64_t restart_at = 0;
    while (true) {
        watchdog_update();
        if (omni::maintenance_poll(stick, time_us_64())) {
            sleep_us(500);
            continue;
        }
        if (host_ready) {
            cyw43_arch_poll();
            omni::bt_tick(time_us_64() / 1000);
            omni::poll_pair_button(time_us_64() / 1000);
        }
        const uint64_t now = time_us_64();
        const auto state = host_ready ? omni::bt_state() : omni::BtState::fault;
        if (input_live && state != omni::BtState::connected)
            neutral();
        const auto pairing = omni::bt_diagnostic();
        omni::usb_status(static_cast<uint8_t>(state), reports, pairing.stage, pairing.detail);
        omni::usb_task(stick, now);
        if (host_ready && omni::bt_recovery_requested()) {
            // An abandoned SDP/HID channel cannot start a second session.
            // Keep servicing USB neutral before restarting the radio stack.
            if (!restart_at)
                restart_at = now + 200'000;
            if (now >= restart_at) {
                tud_disconnect();
                watchdog_reboot(0, 0, 10);
                for (;;)
                    tight_loop_contents();
            }
        }
        const bool led = light(state, now / 1000);
        if (radio_ready && led != last_led) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            last_led = led;
        }
        sleep_us(500);
    }
}
