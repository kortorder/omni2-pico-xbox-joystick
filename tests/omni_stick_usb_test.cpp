#include "omni_mouse_report.hpp"
#include "omni_stick.hpp"
#include "omni_usb.hpp"
#include "tusb.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using omni::Axes;
using omni::StickMapper;
using Bytes = std::array<std::uint8_t, 6>;

static unsigned passed;
static void expect(bool pass, const char *name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
    ++passed;
}
static void test_motion() {
    StickMapper s;
    s.motion(48, 0, 1);
    expect(s.sample(2) == Axes{}, "data before mouse connection cannot move joystick");
    s.connect();
    s.motion(24, 0, 1000);
    expect(s.sample(1000) == Axes{16384, 0}, "half right");
    s.motion(0, 0, 100000);
    expect(s.sample(120999) == Axes{16384, 0}, "zero report leaves time of last movement intact");
    expect(s.sample(121000) == Axes{}, "centers exactly 120 ms after last movement");
    s.motion(0, -48, 122000);
    expect(s.sample(122001) == Axes{0, -32767}, "up is negative USB Y");
    s.disconnect();
    expect(s.sample(122002) == Axes{}, "disconnect neutral immediately");
    s.motion(100, 100, 122003);
    expect(s.sample(122004) == Axes{}, "ignores motion after disconnection");
    s.connect();
    expect(s.sample(122005) == Axes{}, "reconnection starts neutral");
    s.motion(-48, 48, 122006);
    auto diagonal = s.sample(122006);
    expect(diagonal == Axes{-23170, 23170}, "large diagonal preserves angle within circular limit");
    s.clear_motion();
    s.motion(48, 24, 1000);
    diagonal = s.sample(1000);
    expect(std::abs(diagonal.x - 2 * diagonal.y) <= 1,
           "radial saturation preserves unequal axis angle");
    expect(s.sample(999) == Axes{}, "backward clock samples center");
    expect(s.sample(1001) == Axes{}, "stale motion cannot revive after a backward clock");
    s.motion(INT32_MAX, INT32_MIN, 200);
    expect(s.sample(200) == Axes{}, "out-of-order input discarded");
    for (unsigned i = 0; i < 300; ++i)
        s.motion(INT32_MAX, INT32_MIN, 300);
    diagonal = s.sample(300);
    expect(diagonal == Axes{23170, -23170}, "large signed bursts bounded");
    StickMapper fast, slow;
    fast.connect();
    slow.connect();
    for (unsigned i = 0; i < 8; ++i)
        fast.motion(3, 0, 1000 + 4000 * i);
    slow.motion(24, 0, 29000);
    expect(fast.sample(29000) == slow.sample(29000),
           "equal counts yield same deflection across report rates");
    fast.motion(-24, 0, 30000);
    expect(fast.sample(30000) == Axes{}, "reversal cancels earlier movement");
    expect(fast.sample(150000) == Axes{}, "all motion ages out with normal monotonic poll");
    s.connect();
    s.motion(1, 0, UINT64_MAX - 2);
    expect(s.sample(UINT64_MAX).x > 0, "expiry subtraction safe at end of clock range");
    expect(s.sample(5) == Axes{}, "clock restart neutralizes");
}

namespace {
bool mounted = true, suspended = false, ready = true, accepts = true, poll_is_nonblocking = true;
int disconnects = 0, reconnects = 0;
std::vector<Bytes> sent;
void complete() {
    auto b = sent.back();
    tud_hid_report_complete_cb(0, b.data(), static_cast<std::uint16_t>(b.size()));
}
std::uint16_t le16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
} // namespace
extern "C" void tud_task_ext(std::uint32_t timeout, bool in_isr) {
    poll_is_nonblocking &= timeout == 0 && !in_isr;
}
extern "C" bool tud_mounted() {
    return mounted;
}
extern "C" bool tud_suspended() {
    return suspended;
}
extern "C" bool tud_hid_ready() {
    return ready;
}
extern "C" bool tud_disconnect() {
    ++disconnects;
    return true;
}
extern "C" bool tud_connect() {
    ++reconnects;
    return true;
}
extern "C" bool tud_hid_report(std::uint8_t id, const void *data, std::uint16_t len) {
    if (!accepts || id != 0 || len != Bytes{}.size())
        return false;
    Bytes b{};
    std::memcpy(b.data(), data, len);
    sent.push_back(b);
    return true;
}
static void test_descriptor() {
    const auto *dev = tud_descriptor_device_cb(), *cfg = tud_descriptor_configuration_cb(0);
    expect(dev[0] == 18 && le16(dev + 8) == OMNI_USB_VID && le16(dev + 10) == OMNI_USB_PID,
           "descriptor has the configured vendor and product IDs");
    expect(le16(cfg + 2) == 34 && cfg[4] == 1 && cfg[8] == 250,
           "single HID, descriptor declares 500 mA");
    expect(cfg[14] == 3 && cfg[15] == 0 && cfg[16] == 0 && cfg[29] == 0x81 && cfg[30] == 3 &&
               cfg[33] == 4,
           "generic non-boot HID interrupt IN, four millisecond interval");
    expect(tud_descriptor_configuration_cb(1) == nullptr &&
               tud_hid_descriptor_report_cb(1) == nullptr,
           "refuses nonexistent configuration and HID interface");
    const auto *name = tud_descriptor_string_cb(2, 0x409);
    std::string product;
    for (std::size_t i = 1; i < (name[0] & 255) / 2u; ++i)
        product.push_back(static_cast<char>(name[i]));
    expect(product == "Omni Right Stick", "stable product name");
    // Interpret the descriptor's HID short items. Check what a host will decode
    // (report width, flags, usages and direction), not the source array text.
    const auto *desc = tud_hid_descriptor_report_cb(0);
    const std::size_t length = le16(cfg + 25);
    unsigned page = 0, size = 0, count = 0, minimum = 0, maximum = 0, inputs = 0, features = 0;
    unsigned usage_min = 0, usage_max = 0, input_bits = 0;
    std::vector<unsigned> usages;
    for (std::size_t i = 0; i < length;) {
        const unsigned prefix = desc[i++];
        const unsigned fields[4] = {0, 1, 2, 4}, len = fields[prefix & 3], tag = prefix & ~3u;
        if (i + len > length) {
            expect(false, "descriptor length");
            return;
        }
        unsigned val = 0;
        for (unsigned j = 0; j < len; ++j)
            val |= unsigned(desc[i++]) << (j * 8);
        switch (tag) {
        case 0x04:
            page = val;
            break;
        case 0x08:
            usages.push_back(val);
            break;
        case 0x18: usage_min = val; break;
        case 0x28: usage_max = val; break;
        case 0x14:
            minimum = val;
            break;
        case 0x24:
            maximum = val;
            break;
        case 0x74:
            size = val;
            break;
        case 0x94:
            count = val;
            break;
        case 0xa0:
            expect(page == 1 && usages == std::vector<unsigned>{4} && val == 1,
                   "Generic Desktop Joystick Application");
            usages.clear();
            break;
        case 0x80:
            if (inputs == 0)
                expect(page == 1 && usages == std::vector<unsigned>{0x30, 0x31} && size == 16 &&
                           count == 2 && minimum == 0x8001 && maximum == 32767 && val == 2,
                       "signed absolute primary X/Y retain offsets zero and two");
            else if (inputs == 1)
                expect(input_bits == 32 && page == 9 && usage_min == 1 && usage_max == 12 &&
                           size == 1 && count == 12 && minimum == 0 && maximum == 1 && val == 2,
                       "12 digital buttons follow primary axes");
            else
                expect(input_bits == 44 && val == 3 && size == 1 && count == 4,
                       "four high bits are constant padding");
            input_bits += size * count;
            usages.clear();
            ++inputs;
            break;
        case 0xb0:
            expect(page == 0xff00 && usages == std::vector<unsigned>{1} && size == 8 &&
                       count == 8 && val == 2 && minimum == 0 && maximum == 255,
                   "eight unsigned vendor Feature bytes");
            usages.clear();
            ++features;
            break;
        case 0x90:
        case 0x84:
            expect(false, "must not advertise output or report IDs");
            break;
        }
    }
    expect(input_bits == 48 && inputs == 3 && features == 1, "six-byte Input and separate Feature");
}
static void test_usb() {
    StickMapper s;
    s.connect();
    s.motion(48, 0, 100);
    tud_mount_cb();
    omni::usb_task(s, 101);
    expect(sent.size() == 1 && sent.back() == Bytes{}, "USB enumeration starts with neutral");
    s.motion(24, 0, 102);
    omni::usb_task(s, 102);
    expect(sent.size() == 1, "wait for actual input completion");
    std::uint8_t b[16]{};
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, b, 16) == Bytes{}.size() &&
               std::memcmp(b, Bytes{}.data(), Bytes{}.size()) == 0,
           "GET_INPUT neutral until neutral completion");
    complete();
    omni::usb_task(s, 103);
    expect(sent.back() == Bytes{0x00, 0x40, 0, 0}, "half-right input packed little endian");
    s.disconnect();
    s.connect();
    s.motion(0, -48, 104);
    omni::usb_task(s, 104);
    complete(); // this is still the previous generation's transfer
    omni::usb_task(s, 105);
    expect(sent.back() == Bytes{}, "quick reconnect preserves intervening neutral barrier");
    complete();
    omni::usb_task(s, 106);
    expect(sent.back() == Bytes{0, 0, 1, 0x80}, "new up motion follows neutral completion");
    complete();
    const auto before = sent.size();
    omni::usb_task(s, 20106);
    expect(sent.size() == before, "no unchanged report before heartbeat");
    omni::usb_task(s, 40106);
    expect(sent.size() == before + 1 && sent.back() == Bytes{0, 0, 1, 0x80}, "40 ms heartbeat");
    complete();
    omni::usb_task(s, 120104);
    expect(sent.back() == Bytes{}, "stationary timeout produces USB neutral without mouse event");
    complete();
    suspended = true;
    tud_suspend_cb(false);
    s.motion(48, 0, 120105);
    const auto asleep = sent.size();
    omni::usb_task(s, 120106);
    expect(sent.size() == asleep, "USB suspend sends nothing");
    suspended = false;
    tud_resume_cb();
    omni::usb_task(s, 120107);
    complete();
    omni::usb_task(s, 120108);
    expect(sent.size() == asleep + 1 && sent.back() == Bytes{}, "resume discards suspended motion");
    accepts = false;
    s.motion(-48, 0, 120109);
    omni::usb_task(s, 120110);
    expect(sent.size() == asleep + 1, "failed submission does not mark output delivered");
    accepts = true;
    omni::usb_task(s, 120111);
    expect(sent.back() == Bytes{1, 0x80, 0, 0}, "retry sends latest report");
    tud_hid_report_failed_cb(0, HID_REPORT_TYPE_INPUT, nullptr, 0);
    omni::usb_task(s, 120112);
    expect(sent.back() == Bytes{}, "failed completion clears motion and sends neutral");
    complete();
    omni::usb_status(5, 0x12345678u);
    const std::array<std::uint8_t, 8> diag = {1, 5, 0x78, 0x56, 0x34, 0x12, 0, 0};
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, sizeof(b)) == 8 &&
               !std::memcmp(b, diag.data(), 8),
           "read-only feature wire layout distinct from input");
    b[2] = 0xab;
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, 2) == 2 && b[2] == 0xab,
           "feature read respects request capacity");
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, 8);
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, 16) == 8 &&
               !std::memcmp(b, diag.data(), 8),
           "host cannot alter diagnostic or control output");
    // Progress/error codes are Bluetooth-owned. USB preserves them byte for
    // byte without changing the format, state, counter or interrupt joystick.
    omni::usb_status(5, 0x12345678u, 0x83, 0x05);
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, 16) == 8 &&
               !std::memcmp(b, diag.data(), 6) && b[6] == 0x83 && b[7] == 5,
           "pairing stage and detail use the two existing tail bytes");
    b[6] = 0xac;
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_FEATURE, b, 6) == 6 && b[6] == 0xac &&
               !std::memcmp(b, diag.data(), 6),
           "six-byte legacy prefix and bounded read survive diagnostic extension");
    expect(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, b, 16) == Bytes{}.size() &&
               !std::memcmp(b, Bytes{}.data(), Bytes{}.size()),
           "pairing detail never enters the stick input report");
    expect(tud_hid_get_report_cb(1, 0, HID_REPORT_TYPE_FEATURE, b, 16) == 0 &&
               tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, b, 16) == 0 &&
               tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, b, 16) == 0,
           "unsupported requests return no data");
    mounted = false;
    tud_umount_cb();
    s.motion(48, 0, 130000);
    omni::usb_task(s, 130000);
    mounted = true;
    tud_mount_cb();
    omni::usb_task(s, 130001);
    complete();
    expect(sent.back() == Bytes{}, "unmount/remount clears mouse history");
    expect(poll_is_nonblocking, "USB service never waits on the Bluetooth polling loop");
}

static void test_lost_completion() {
    StickMapper s;
    s.connect();
    tud_mount_cb();
    omni::usb_task(s, 1); // initial neutral is never polled
    omni::usb_task(s, 2000000);
    expect(disconnects == 0, "unpolled initial neutral never triggers a detach loop");
    complete();
    s.motion(48, 0, 2000001);
    omni::usb_task(s, 2000001); // active transfer remains outstanding
    omni::usb_task(s, 2120001); // mouse has stopped
    omni::usb_task(s, 2350000);
    expect(disconnects == 0, "late active IN completion gets the full recovery window");
    omni::usb_task(s, 2350001);
    expect(disconnects == 1, "uncompleted active IN triggers an observable detach at 350ms");
    auto before = sent.size();
    s.motion(-48, 0, 2400000);
    omni::usb_task(s, 2600000);
    expect(reconnects == 0 && sent.size() == before, "D+ remains detached for 250ms");
    omni::usb_task(s, 2600001);
    omni::usb_task(s, 2610000);
    expect(reconnects == 1 && sent.size() == before, "no new report until fresh configuration");
    tud_mount_cb();
    omni::usb_task(s, 2611000);
    complete();
    omni::usb_task(s, 2612000);
    expect(sent.size() == before + 1 && sent.back() == Bytes{},
           "re-enumeration clears stale motion and completes neutral first");
    // A completed active report can also be stranded by enduring endpoint backpressure.
    s.motion(0, 48, 2700000);
    omni::usb_task(s, 2700001);
    complete();
    ready = false;
    omni::usb_task(s, 2820000);
    omni::usb_task(s, 3169999);
    expect(disconnects == 1, "busy release receives full 350ms grace");
    omni::usb_task(s, 3170000);
    expect(disconnects == 2,
           "busy endpoint after confirmed motion cannot strand the neutral release");
    omni::usb_task(s, 3420000);
    tud_mount_cb();
    ready = true;
    omni::usb_task(s, 3421000);
    complete();
    s.motion(48, 0, 3430000);
    omni::usb_task(s, 3430001);
    suspended = true;
    tud_suspend_cb(false);
    omni::usb_task(s, 8000000);
    expect(disconnects == 2, "normal USB suspend does not trigger recovery");
    suspended = false;
    tud_resume_cb();
    omni::usb_task(s, 8000001);
    complete();
    expect(sent.back() == Bytes{} && disconnects == 2,
           "resume after motion starts centered without detaching");
    s.motion(-48, 0, 8100000);
    omni::usb_task(s, 8100001);
    complete();
    s.disconnect();
    omni::usb_task(s, 8100002); // neutral queued but its completion is missing
    omni::usb_task(s, 8450002);
    expect(disconnects == 3, "uncompleted neutral after active motion also triggers detach");
}
static void test_raw_mouse_to_usb() {
    // A synthetic numbered mouse report, deliberately ordered Y, wheel, X.
    // Only a descriptor-driven reader can place these values on the right axes.
    constexpr std::uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x09, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x31, 0x09,
        0x38, 0x09, 0x30, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0};
    omni::MouseReports decoder;
    expect(decoder.configure(descriptor, sizeof(descriptor)), "accept synthetic Mouse descriptor");
    StickMapper mapper;
    mapper.connect();
    tud_mount_cb();
    omni::usb_task(mapper, 100);
    complete();
    auto report = [&](std::array<std::uint8_t, 4> raw, std::uint64_t now) {
        omni::MouseInput input{};
        expect(decoder.read(raw.data(), raw.size(), input) == omni::MouseReports::Result::mouse,
               "accept numbered relative report");
        mapper.motion(input.dx, input.dy, now);
        omni::usb_task(mapper, now);
    };
    report({9, 0, 127, 24}, 200); // +24 X with maximum positive wheel.
    expect(sent.back() == Bytes{0, 0x40, 0, 0}, "raw right and wheel yield only half-right on USB");
    complete();
    omni::usb_task(mapper, 120200);
    expect(sent.back() == Bytes{}, "no new raw report is needed to release a stationary mouse");
    complete();
    report({9, 0xd0, 7, 0}, 120201); // -48 Y counts become full up, wheel is ignored.
    expect(sent.back() == Bytes{0, 0, 1, 0x80},
           "signed relative up becomes negative USB Y exactly once");
    complete();
    mapper.disconnect();
    omni::usb_task(mapper, 120202);
    expect(sent.back() == Bytes{}, "disconnect during raw motion emits neutral on USB");
    complete();
}
int main() {
    test_motion();
    test_descriptor();
    test_usb();
    test_raw_mouse_to_usb();
    test_lost_completion();
    std::printf(
        "PASS: %u Omni mouse-to-stick, USB joystick descriptor, transfer and diagnostic checks.\n",
        passed);
}
// The companion maintenance test links its actual USB command and boot sequence.
namespace omni { void maintenance_usb_reset() {} }
