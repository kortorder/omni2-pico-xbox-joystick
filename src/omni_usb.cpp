#include "omni_usb.hpp"
#include "omni_maintenance.hpp"
#include "tusb.h"
#include <algorithm>
#include <array>
#include <cstring>

static_assert(OMNI_USB_VID > 0 && OMNI_USB_VID < 65536);
static_assert(OMNI_USB_PID > 0 && OMNI_USB_PID < 65536);
static_assert(OMNI_USB_POWER_MA >= 2 && OMNI_USB_POWER_MA <= 500 && OMNI_USB_POWER_MA % 2 == 0);

namespace {
using namespace omni;
using Report = std::array<std::uint8_t, 6>;
// Microsoft XAC input-device specification v1.6, section 10: primary stick
// on the right USB-A port supplies right-stick input. No secondary stick.
constexpr std::uint8_t report_descriptor[] = {
    0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, // Generic Desktop, Joystick, Application
    0x09, 0x30, 0x09, 0x31,             // X and Y
    0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, // -32767 .. +32767
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, // 2 x 16-bit absolute variable INPUT
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, // Button page, positions 1 through 12
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x04, 0x81, 0x03, // Four constant padding bits
    0x06, 0x00, 0xff, 0x09, 0x01,       // Vendor status, separate HID report type
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x08, 0xb1, 0x02, 0xc0};
constexpr std::uint8_t device_descriptor[] = {18,
                                              1,
                                              0x00,
                                              0x02,
                                              0,
                                              0,
                                              0,
                                              64,
                                              OMNI_USB_VID & 255,
                                              OMNI_USB_VID >> 8,
                                              OMNI_USB_PID & 255,
                                              OMNI_USB_PID >> 8,
                                              0x00,
                                              0x01,
                                              1,
                                              2,
                                              0,
                                              1};
constexpr std::uint8_t config_descriptor[] = {
    9,
    2,
    34,
    0,
    1,
    1,
    0,
    0x80,
    OMNI_USB_POWER_MA / 2, // bus powered, one interface
    9,
    4,
    0,
    0,
    1,
    3,
    0,
    0,
    0, // HID, non-boot, one endpoint
    9,
    0x21,
    0x11,
    0x01,
    0,
    1,
    0x22,
    sizeof(report_descriptor),
    0,
    7,
    5,
    0x81,
    3,
    16,
    0,
    4 // interrupt IN, full speed, 4ms interval
};
static_assert(sizeof(config_descriptor) == 34);

Report pack(Axes a, std::uint16_t buttons) {
    const auto x = static_cast<std::uint16_t>(a.x), y = static_cast<std::uint16_t>(a.y);
    return {std::uint8_t(x), std::uint8_t(x >> 8), std::uint8_t(y), std::uint8_t(y >> 8),
            std::uint8_t(buttons), std::uint8_t((buttons >> 8) & 0x0f)};
}
std::uint16_t button_mask(const Report &r) {
    return static_cast<std::uint16_t>(r[4] | (r[5] << 8));
}
NudgeMapper nudges;

struct Stream {
    bool discard_motion = true, require_neutral = true, pending = false;
    bool may_be_nonzero = false, timing_release = false;
    std::uint16_t maybe_held_buttons = 0, button_release_owed = 0;
    bool detached = false, awaiting_mount = false;
    std::uint32_t source_generation = 0, epoch = 0, inflight_epoch = 0;
    std::uint64_t sent_at = 0, release_at = 0, detached_at = 0;
    Report desired{}, inflight{}, completed{};
    void barrier(bool reset_usb) {
        ++epoch;
        require_neutral = true;
        nudges.reset();
        desired = {};
        if (reset_usb) {
            pending = false;
            discard_motion = true;
        }
    }
    void bus_reset() {
        barrier(true);
        may_be_nonzero = false;
        maybe_held_buttons = button_release_owed = 0;
        timing_release = false;
    }
    void preserve_button_release() {
        // If the endpoint is busy for the entire gap between two presses of
        // the same button, require a real up transfer before another down.
        const auto wanted = button_mask(desired);
        button_release_owed |= maybe_held_buttons & ~wanted;
        const auto allowed = static_cast<std::uint16_t>(wanted & ~button_release_owed);
        desired[4] = static_cast<std::uint8_t>(allowed);
        desired[5] = static_cast<std::uint8_t>(allowed >> 8);
    }
    bool needs_recovery(std::uint64_t now) {
        if (!may_be_nonzero) {
            timing_release = false;
            return false;
        }
        if (pending && now >= sent_at && now - sent_at >= 350000)
            return true;
        if (!require_neutral && desired != Report{} && !button_release_owed) {
            timing_release = false;
            return false;
        }
        if (!timing_release || now < release_at) {
            timing_release = true;
            release_at = now;
        }
        return now - release_at >= 350000;
    }
} stream;
std::array<std::uint8_t, 8> diagnostic = {1, 0, 0, 0, 0, 0, 0, 0};

std::uint16_t copy_report(const std::uint8_t *data, std::size_t size, std::uint8_t *buffer,
                          std::uint16_t capacity) {
    if (!buffer)
        return 0;
    const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(size, capacity));
    std::memcpy(buffer, data, length);
    return length;
}
} // namespace

void omni::usb_nudge_report(std::int16_t dx, std::int16_t dy, std::int16_t wheel,
                            std::uint8_t buttons, std::uint64_t now) {
    if (!stream.require_neutral && !stream.discard_motion && !stream.detached &&
        !stream.awaiting_mount && tud_mounted() && !tud_suspended())
        nudges.report(dx, dy, wheel, buttons, now);
}
void omni::usb_cancel_nudges() {
    nudges.reset();
    stream.desired[4] = stream.desired[5] = 0;
}
bool omni::usb_set_nudge_map(NudgeMap map) {
    if (!nudges.set_map(map))
        return false;
    stream.desired[4] = stream.desired[5] = 0;
    return true;
}
omni::NudgeMap omni::usb_nudge_map() { return nudges.map(); }
void omni::usb_request_neutral(StickMapper &mapper) {
    mapper.clear_motion();
    stream.barrier(false);
}
bool omni::usb_neutral_confirmed() {
    return tud_mounted() && !tud_suspended() && !stream.require_neutral && !stream.detached &&
           !stream.awaiting_mount && !stream.may_be_nonzero && stream.desired == Report{} &&
           stream.completed == Report{};
}

void omni::usb_status(std::uint8_t state, std::uint32_t count, std::uint8_t pairing_stage,
                      std::uint8_t pairing_detail) {
    diagnostic = {1,
                  state,
                  std::uint8_t(count),
                  std::uint8_t(count >> 8),
                  std::uint8_t(count >> 16),
                  std::uint8_t(count >> 24),
                  pairing_stage,
                  pairing_detail};
}

void omni::usb_task(StickMapper &mapper, std::uint64_t now) {
    if (mapper.generation() != stream.source_generation) {
        stream.source_generation = mapper.generation();
        // Keep the slot for a previously submitted transfer until its callback.
        // Its completion cannot satisfy the new connection's neutral barrier.
        stream.barrier(false);
    }
    const auto axes = mapper.sample(now);
    const auto buttons = nudges.sample(now);
    stream.desired = pack(axes, axes == Axes{} ? buttons : 0);
    stream.preserve_button_release();
    tud_task_ext(0, false); // Poll without blocking the Bluetooth run loop.
    if (stream.detached) {
        if (now >= stream.detached_at && now - stream.detached_at >= 250000) {
            stream.detached = false;
            stream.awaiting_mount = true;
            tud_connect();
        }
        return;
    }
    if (stream.awaiting_mount)
        return;
    if (!tud_mounted() || tud_suspended())
        return;
    if (stream.discard_motion) {
        mapper.clear_motion();
        stream.desired = {};
        stream.discard_motion = false;
    }
    if (stream.needs_recovery(now)) {
        // Endpoint completion was lost or hardware stayed busy after motion.
        // Drop the RP2040 D+ pull-up so the host can observe device removal.
        stream.bus_reset();
        mapper.clear_motion();
        stream.detached_at = now;
        stream.detached = true;
        tud_disconnect();
        return;
    }
    if (stream.pending || !tud_hid_ready())
        return;
    const Report next = stream.require_neutral ? Report{} : stream.desired;
    if (!stream.require_neutral && next == stream.completed && now >= stream.sent_at &&
        now - stream.sent_at < 40000)
        return;
    // TinyUSB copies this report into its interrupt transfer buffer synchronously.
    if (tud_hid_report(0, next.data(), next.size())) {
        stream.maybe_held_buttons |= button_mask(next);
        stream.inflight = next;
        stream.pending = true;
        stream.inflight_epoch = stream.epoch;
        stream.sent_at = now;
        if (next != Report{})
            stream.may_be_nonzero = true;
    }
}

extern "C" const std::uint8_t *tud_descriptor_device_cb() {
    return device_descriptor;
}
extern "C" const std::uint8_t *tud_descriptor_configuration_cb(std::uint8_t index) {
    return index == 0 ? config_descriptor : nullptr;
}
extern "C" const std::uint8_t *tud_hid_descriptor_report_cb(std::uint8_t instance) {
    return instance == 0 ? report_descriptor : nullptr;
}
extern "C" const std::uint16_t *tud_descriptor_string_cb(std::uint8_t index, std::uint16_t) {
    static std::uint16_t text[64];
    if (index == 0) {
        text[0] = 0x0304;
        text[1] = 0x0409;
        return text;
    }
    const char *value = index == 1   ? "SimpleController prototype"
                        : index == 2 ? "Omni Right Stick"
                                     : nullptr;
    if (!value)
        return nullptr;
    const auto n = std::min<std::size_t>(std::strlen(value), 63);
    for (std::size_t i = 0; i < n; ++i)
        text[i + 1] = static_cast<unsigned char>(value[i]);
    text[0] = static_cast<std::uint16_t>(0x0300 | (2 * n + 2));
    return text;
}

extern "C" void tud_mount_cb() {
    omni::maintenance_usb_reset();
    stream.bus_reset();
    stream.awaiting_mount = false;
}
extern "C" void tud_umount_cb() {
    omni::maintenance_usb_reset();
    stream.bus_reset();
}
extern "C" void tud_suspend_cb(bool) {
    omni::maintenance_usb_reset();
    stream.barrier(true);
    stream.timing_release = false;
}
extern "C" void tud_resume_cb() {
    omni::maintenance_usb_reset();
    stream.barrier(true);
    stream.timing_release = false;
}
extern "C" void tud_hid_report_complete_cb(std::uint8_t instance, const std::uint8_t *data,
                                           std::uint16_t size) {
    if (instance != 0 || !stream.pending)
        return;
    stream.pending = false;
    if (!data || size != stream.inflight.size() ||
        std::memcmp(data, stream.inflight.data(), size) != 0) {
        stream.barrier(true);
        return;
    }
    if (stream.inflight_epoch != stream.epoch)
        return;
    stream.completed = stream.inflight;
    stream.maybe_held_buttons = button_mask(stream.completed);
    stream.button_release_owed &= stream.maybe_held_buttons;
    if (stream.completed == Report{}) {
        stream.require_neutral = false;
        stream.may_be_nonzero = false;
        stream.timing_release = false;
    }
}
extern "C" void tud_hid_report_failed_cb(std::uint8_t instance, hid_report_type_t type,
                                         const std::uint8_t *, std::uint16_t) {
    if (instance == 0 && type == HID_REPORT_TYPE_INPUT)
        stream.barrier(true);
}
extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t instance, std::uint8_t id,
                                               hid_report_type_t type, std::uint8_t *buffer,
                                               std::uint16_t capacity) {
    if (instance != 0 || id != 0)
        return 0;
    if (type == HID_REPORT_TYPE_FEATURE)
        return copy_report(diagnostic.data(), diagnostic.size(), buffer, capacity);
    if (type != HID_REPORT_TYPE_INPUT)
        return 0;
    const Report current = stream.require_neutral ? Report{} : stream.desired;
    return copy_report(current.data(), current.size(), buffer, capacity);
}
extern "C" void tud_hid_set_report_cb(std::uint8_t, std::uint8_t, hid_report_type_t,
                                      const std::uint8_t *, std::uint16_t) {
    // No host-writable commands or settings.
}
