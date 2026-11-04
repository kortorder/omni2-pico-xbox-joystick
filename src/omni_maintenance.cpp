#include "omni_maintenance.hpp"
#if OMNI_CAPTURE_SHORT_KEY || OMNI_SHORT_KEY_COMPAT
#include "omni_bluetooth.hpp"
#include <algorithm>
#endif
#if !OMNI_CAPTURE_SHORT_KEY
#include "omni_nudge_settings.hpp"
#endif
#include "omni_stick.hpp"
#include "omni_usb.hpp"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"

namespace {
using namespace omni::maintenance;
enum class Gate {
    idle,
    probe_pending,
    armed,
    enter_pending,
    private_read_pending,
    settings_read_pending,
    settings_receive,
    settings_validated,
    requested,
    centering,
    detached
};
Gate gate = Gate::idle;
std::uint64_t changed_at = 0;
constexpr std::uint64_t window_us = 3'000'000;
auto probe_data = capability;
#if OMNI_CAPTURE_SHORT_KEY
CaptureReply capture_data{};
#elif OMNI_SHORT_KEY_COMPAT
MotionReply motion_data{};
#endif
#if !OMNI_CAPTURE_SHORT_KEY
omni::NudgeSettingsWire settings_data{};
omni::NudgeProposal proposal{};
enum class SavePhase { none, requested, centering };
SavePhase save = SavePhase::none;
std::uint64_t save_started = 0;
#endif

bool recent(std::uint64_t now, std::uint64_t duration) {
    return now >= changed_at && now - changed_at < duration;
}
bool committed() { return gate == Gate::requested || gate == Gate::centering || gate == Gate::detached; }
void end_control() {
#if OMNI_CAPTURE_SHORT_KEY
    capture_data.fill(0);
#elif OMNI_SHORT_KEY_COMPAT
    motion_data.fill(0);
#endif
#if !OMNI_CAPTURE_SHORT_KEY
    settings_data.fill(0);
#endif
    if (!committed())
        gate = Gate::idle;
}
} // namespace

void omni::maintenance_usb_reset() {
    end_control();
#if !OMNI_CAPTURE_SHORT_KEY
    if (save != SavePhase::none) {
        save = SavePhase::none;
        nudge_settings_abort();
    }
#endif
}

extern "C" bool tud_vendor_control_xfer_cb(std::uint8_t port, std::uint8_t stage,
                                           const tusb_control_request_t *request) {
    if (port != 0 || !request || committed() || !tud_mounted() || tud_suspended())
        return false;
    const bool signature = request->wValue == request_value && request->wIndex == request_index;
    const bool probe = signature && request->bmRequestType == probe_type &&
                       request->bRequest == probe_request && request->wLength == capability.size();
    const bool enter = signature && request->bmRequestType == enter_type &&
                       request->bRequest == enter_request && request->wLength == 0;
#if OMNI_CAPTURE_SHORT_KEY
    const bool private_read = signature && request->bmRequestType == probe_type &&
                              request->bRequest == capture_request &&
                              request->wLength == capture_data.size();
#elif OMNI_SHORT_KEY_COMPAT
    const bool private_read = signature && request->bmRequestType == probe_type &&
                              request->bRequest == motion_request &&
                              request->wLength == motion_data.size();
#endif
#if !OMNI_CAPTURE_SHORT_KEY
    const bool settings_read = signature && request->bmRequestType == probe_type &&
                               request->bRequest == nudge_read_request &&
                               request->wLength == settings_data.size();
    const bool settings_write = signature && request->bmRequestType == enter_type &&
                                request->bRequest == nudge_write_request &&
                                request->wLength == settings_data.size();
#endif
    const auto now = time_us_64();
    if (stage == CONTROL_STAGE_SETUP) {
        const bool armed = gate == Gate::armed && recent(now, window_us);
        gate = Gate::idle;
#if OMNI_CAPTURE_SHORT_KEY
        capture_data.fill(0);
        if (private_read && armed) {
            PeerPin pin{};
            if (!omni::bt_copy_short_key_candidate(pin))
                return false;
            capture_data[0] = 1;
            capture_data[1] = 7;
            std::copy(pin.begin(), pin.end(), capture_data.begin() + 2);
            if (!tud_control_xfer(port, request, capture_data.data(), capture_data.size())) {
                capture_data.fill(0);
                return false;
            }
            gate = Gate::private_read_pending;
            return true;
        }
#endif
#if !OMNI_CAPTURE_SHORT_KEY
        if (settings_read && armed) {
            settings_data = omni::nudge_settings_read();
            if (!tud_control_xfer(port, request, settings_data.data(), settings_data.size()))
                return false;
            gate = Gate::settings_read_pending;
            return true;
        }
        // Only the readback command can run while a previously accepted save
        // is waiting for the host to complete the neutral HID transfer.
        if (save != SavePhase::none && !probe)
            return false;
        if (settings_write && armed) {
            settings_data.fill(0xff); // A short OUT transfer cannot inherit an old valid SAVE tail.
            if (!tud_control_xfer(port, request, settings_data.data(), settings_data.size()))
                return false;
            gate = Gate::settings_receive;
            return true;
        }
#endif
#if OMNI_SHORT_KEY_COMPAT
        motion_data.fill(0);
        if (private_read && armed) {
            if (!omni::bt_copy_motion_snapshot(motion_data) ||
                !tud_control_xfer(port, request, motion_data.data(), motion_data.size()))
                return false;
            gate = Gate::private_read_pending;
            return true;
        }
#endif
        if (probe && tud_control_xfer(port, request, probe_data.data(), probe_data.size())) {
            gate = Gate::probe_pending;
            changed_at = now;
            return true;
        }
        if (enter && armed && tud_control_status(port, request)) {
            gate = Gate::enter_pending;
            return true;
        }
        return false;
    }
    if (!recent(now, window_us)) {
        omni::maintenance_usb_reset();
        return false;
    }
#if OMNI_CAPTURE_SHORT_KEY || OMNI_SHORT_KEY_COMPAT
    if (private_read && gate == Gate::private_read_pending) {
        if (stage == CONTROL_STAGE_DATA)
            return true;
        if (stage == CONTROL_STAGE_ACK) {
            end_control(); // A fresh probe is required for any subsequent command.
            return true;
        }
    }
#endif
#if !OMNI_CAPTURE_SHORT_KEY
    if (settings_read && gate == Gate::settings_read_pending) {
        if (stage == CONTROL_STAGE_DATA)
            return true;
        if (stage == CONTROL_STAGE_ACK) {
            end_control();
            return true;
        }
    }
    if (settings_write && (gate == Gate::settings_receive || gate == Gate::settings_validated)) {
        if (!recent(now, window_us)) {
            end_control();
            omni::nudge_settings_abort();
            return false;
        }
        if (stage == CONTROL_STAGE_DATA && gate == Gate::settings_receive) {
            if (!omni::nudge_settings_prepare(settings_data, proposal)) {
                end_control();
                return false;
            }
            gate = Gate::settings_validated;
            return true;
        }
        if (stage == CONTROL_STAGE_ACK && gate == Gate::settings_validated) {
            end_control();
            save = SavePhase::requested;
            omni::nudge_settings_pending();
            return true;
        }
    }
#endif
    if (probe && gate == Gate::probe_pending) {
        if (stage == CONTROL_STAGE_DATA)
            return true;
        if (stage == CONTROL_STAGE_ACK) {
            gate = Gate::armed;
            changed_at = now;
            return true;
        }
    }
    if (enter && gate == Gate::enter_pending && stage == CONTROL_STAGE_ACK) {
        gate = Gate::requested; // No reboot inside a USB callback.
        return true;
    }
    return false;
}

bool omni::maintenance_poll(StickMapper &mapper, std::uint64_t now) {
#if !OMNI_CAPTURE_SHORT_KEY
    if (save != SavePhase::none) {
        if (save == SavePhase::requested) {
            usb_request_neutral(mapper);
            save = SavePhase::centering;
            save_started = now;
        }
        usb_task(mapper,
                 now); // Main skips BT polling until a fresh neutral completes or this operation aborts.
        if (save == SavePhase::none)
            return true; // A reset/suspend may have canceled it inside tud_task_ext.
        if (!tud_mounted() || tud_suspended() || now < save_started || now - save_started >= 1'000'000) {
            save = SavePhase::none;
            nudge_settings_abort();
            return true;
        }
        if (usb_neutral_confirmed()) {
            save = SavePhase::none;
            nudge_settings_commit(proposal);
        }
        return true;
    }
#endif
    if (!committed())
        return false;
    if (gate == Gate::requested) {
        mapper.disconnect();
        gate = Gate::centering;
        changed_at = now;
    }
    if (gate == Gate::centering) {
        usb_task(mapper, now); // Preserve the ordinary neutral barrier and HID transfer code.
        if (!recent(now, 200'000)) {
            tud_disconnect();
            gate = Gate::detached;
            changed_at = now;
        }
    } else if (gate == Gate::detached && !recent(now, 150'000)) {
        reset_usb_boot(0, 0);
    }
    return true;
}
