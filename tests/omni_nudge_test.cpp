// Host checks. No radio, USB hardware, or console involved.
#include "omni_usb.hpp"
#include "tusb.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace omni;
constexpr std::uint64_t ready_at = 500000;
constexpr std::uint64_t gesture = 600000;
unsigned checks = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
    ++checks;
}
std::uint16_t bit(unsigned position) {
    return static_cast<std::uint16_t>(1u << (position - 1));
}
void prime(NudgeMapper &n) {
    n.report(0, 0, 0, 0, 100000);
    n.report(0, 0, 0, 0, ready_at);
}
void click(NudgeMapper &n, unsigned button, std::uint64_t start, std::uint64_t end) {
    n.report(0, 0, 0, button, start);
    n.report(0, 0, 0, 0, end);
}
void pure() {
    NudgeMapper mapper;
    prime(mapper);
    click(mapper, 2, gesture, gesture + 200000);
    check(mapper.sample(gesture + 280000) == 0, "no personal map at startup");
    check(mapper.set_map({}), "accept personal defaults");
    check(!mapper.set_map({0, 13, 0, 0}) && mapper.map().right == 12,
          "reject out-of-range map and keep last valid map");
    prime(mapper);
    click(mapper, 2, gesture, gesture + 200000);
    check(mapper.sample(gesture + 279999) == 0, "no action during post-click hesitation window");
    check(mapper.sample(gesture + 280000) == bit(12), "realistic isolated right-click becomes B");
    check(mapper.sample(gesture + 359999) == bit(12), "B held for bounded pulse");
    check(mapper.sample(gesture + 360000) == 0, "release at exact pulse boundary");
    click(mapper, 1, gesture + 400000, gesture + 420000);
    check(mapper.sample(gesture + 500000) == bit(4), "separate left-click becomes RB");

    mapper.reset();
    prime(mapper);
    mapper.report(0, 0, 3, 0, gesture);
    check(mapper.sample(gesture + 80000) == bit(5), "one positive wheel report produces X");
    for (unsigned i = 1; i <= 5; ++i) {
        auto time = gesture + i * 200000;
        mapper.report(0, 0, 3, 0, time);
        check(mapper.sample(time + 80000) == 0, "200ms wheel repeats coalesce");
    }
    auto later = gesture + 5*200000 + NudgeMapper::wheel_gap_us;
    mapper.report(0, 0, -3, 0, later);
    check(mapper.sample(later + 80000) == bit(3), "new reverse burst after gap produces RS click");
    mapper.report(0, 0, -3, 0, later + 200000);
    check(mapper.sample(later + 280000) == 0, "changing wheel magnitude never multiplies presses");

    mapper.reset();
    prime(mapper);
    mapper.report(1, 0, 0, 0, gesture);
    click(mapper, 2, gesture + 110000, gesture + 140000);
    check(mapper.sample(gesture + 220000) == 0, "click started while stick motion was recent stays rejected");
    mapper.report(0, 0, 0, 1, gesture + 300000);
    mapper.report(1, 0, 0, 1, gesture + 310000);
    mapper.report(-1, 0, 0, 1, gesture + 320000);
    mapper.report(0, 0, 0, 0, gesture + 500000);
    check(mapper.sample(gesture + 580000) == 0, "out-and-back motion during press invalidates whole click");
    click(mapper, 2, gesture + 600000, gesture + 640000);
    mapper.report(0, -1, 0, 0, gesture + 719999);
    check(mapper.sample(gesture + 720000) == 0, "motion one microsecond before eligibility cancels pending click");
    mapper.report(0, 0, 1, 0, gesture + 800000);
    mapper.report(0, 0, 1, 0, gesture + 1000000);
    check(mapper.sample(gesture + 1080000) == 0, "tail of wheel first seen during movement never becomes a fresh nudge");
    mapper.report(0, 0, 1, 0, gesture + 1500000);
    check(mapper.sample(gesture + 1580000) == bit(5), "new isolated wheel after quiet qualifies");
    mapper.report(-1, 0, 0, 0, gesture + 1581000);
    check(mapper.sample(gesture + 1581000) == 0, "live movement cancels active pulse immediately");

    mapper.reset();
    prime(mapper);
    mapper.report(0, 0, 0, 1, gesture);
    mapper.report(0, 0, 0, 3, gesture + 1000);
    mapper.report(0, 0, 0, 2, gesture + 2000);
    mapper.report(0, 0, 0, 0, gesture + 3000);
    check(mapper.sample(gesture + 85000) == 0, "overlapping left/right click produces neither action");
    mapper.report(0, 0, 1, 0, gesture + 200000);
    mapper.report(0, 0, -1, 0, gesture + 201000);
    check(mapper.sample(gesture + 280000) == 0, "opposite wheels during guard are ambiguous");
    click(mapper, 2, gesture + 700000, gesture + 710000);
    mapper.report(0, 0, 1, 0, gesture + 720000);
    check(mapper.sample(gesture + 790000) == 0, "click and wheel during guard do not compete");

    mapper.reset();
    prime(mapper);
    mapper.report(0, 0, 1, 0, gesture);
    check(mapper.sample(gesture + 1000000) == 0, "late observation never replays an expired pulse");
    mapper.report(0, 0, -1, 0, gesture + 1100000);
    check(mapper.set_map({0, 2, 1, 10}), "remap all gestures");
    check(mapper.sample(gesture + 1180000) == 0, "remap discards unresolved old gesture");
    mapper.report(0, 0, 0, 2, gesture + 1200000);
    mapper.report(0, 0, 0, 0, gesture + 1250000);
    check(mapper.sample(gesture + 1330000) == 0, "press already held across remap cannot activate");
    mapper.report(0, 0, 0, 0, gesture + 1700000);
    click(mapper, 1, gesture + 1800000, gesture + 1810000);
    check(mapper.sample(gesture + 1890000) == 0, "disabled individual nudge is inert");
    click(mapper, 2, gesture + 2100000, gesture + 2110000);
    check(mapper.sample(gesture + 2190000) == bit(2), "new isolated click honors reassignment");
    mapper.report(0, 0, 1, 0, gesture + 2300000);
    check(mapper.sample(gesture + 2200000) == 0, "clock rollback cancels; no future revival");
    check(mapper.sample(gesture + 2380000) == 0, "rolled-back candidate remains discarded");
    mapper.reset();
    const auto end = std::numeric_limits<std::uint64_t>::max();
    mapper.report(0, 0, 0, 0, end - 900000);
    mapper.report(0, 0, 0, 0, end - 500000);
    mapper.report(0, 0, 1, 0, end - 90000);
    check(mapper.sample(end) == bit(1), "eligibility remains valid at maximum clock value");
    check(mapper.sample(0) == 0, "wrapped clock never leaves an action down");
}

using Report = std::array<std::uint8_t, 6>;
std::vector<Report> sent;
bool ready = true, suspended = false;
unsigned detaches = 0;
void complete() {
    auto r = sent.back();
    tud_hid_report_complete_cb(0, r.data(), r.size());
}
std::uint16_t mask(Report r) { return r[4] | (r[5] << 8); }
void usb() {
    StickMapper s;
    s.connect();
    check(usb_nudge_map().right == 0, "USB runtime starts with every action off");
    check(usb_set_nudge_map({}), "explicitly apply personal assignments");
    tud_mount_cb();
    usb_task(s, 0);
    check(sent.size() == 1 && sent.back() == Report{} && !usb_neutral_confirmed(),
          "only neutral report offered on mount; not yet confirmed");
    complete();
    check(usb_neutral_confirmed(), "mount neutral completes");
    usb_nudge_report(0, 0, 0, 0, 100000);
    usb_nudge_report(0, 0, 0, 0, ready_at);
    usb_nudge_report(0, 0, 0, 2, gesture);
    usb_nudge_report(0, 0, 0, 0, gesture + 20000);
    usb_task(s, gesture + 100000);
    check(mask(sent.back()) == bit(12) && sent.back()[5] == 8,
          "right click really packs B in physical HID position 12");
    complete();
    usb_task(s, gesture + 180000);
    check(sent.back() == Report{}, "button-up submitted without requiring new mouse report");
    complete();
    usb_nudge_report(0, 0, 0, 1, 1200000);
    usb_nudge_report(0, 0, 0, 0, 1230000);
    usb_task(s, 1310000);
    check(mask(sent.back()) == bit(4), "left click really packs RB in physical position 4");
    // Change settings while RB is pending. Its stale completion must not satisfy the save barrier.
    usb_request_neutral(s);
    check(!usb_neutral_confirmed(), "request always starts a new completion barrier");
    complete();
    check(!usb_neutral_confirmed(), "retired button completion cannot confirm neutral");
    usb_task(s, 1310010);
    check(sent.back() == Report{} && !usb_neutral_confirmed(), "neutral actually submitted after stale press");
    complete();
    check(usb_neutral_confirmed(), "matching neutral completion permits save");
    check(!usb_set_nudge_map({1,2,3,13}) && usb_nudge_map().down == 3,
          "invalid settings rejected atomically");
    usb_nudge_report(0, 0, 0, 2, 1320000);
    usb_nudge_report(0, 0, 0, 0, 1340000);
    usb_task(s, 1420000);
    check(mask(sent.back()) == 0, "held button crossing reset cannot reappear");
    complete();
    usb_nudge_report(0, 0, 0, 0, 1750000);
    usb_nudge_report(0, 0, -3, 0, 1800000);
    usb_task(s, 1880000);
    check(mask(sent.back()) == bit(3), "reverse wheel reaches USB RS button");
    complete();
    // Camera takes priority without delay or attenuation. Even with live axes, a stranded
    // button-up must not remain indefinitely on the HID host under backpressure.
    ready = false;
    s.motion(48, 0, 1881000);
    usb_nudge_report(48, 0, 0, 0, 1881000);
    usb_task(s, 1881000);
    Report current{};
    check(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, current.data(), current.size()) == 6 &&
              current == Report{255,127,0,0,0,0}, "camera moves immediately and drops the pulse");
    for (std::uint64_t t = 1891000; t < 2231000; t += 10000) {
        s.motion(48,0,t);
        usb_nudge_report(48,0,0,0,t);
        usb_task(s,t);
    }
    check(detaches == 0, "ordinary backpressure receives existing release grace");
    s.motion(48,0,2231000);
    usb_task(s,2231000);
    check(detaches == 1, "lost button release detaches even when right-stick motion remains nonzero");
    ready = true;
    usb_task(s,2481000); tud_mount_cb(); usb_task(s,2481010);
    check(sent.back() == Report{}, "re-enumeration starts with zero axes and all 12 buttons released");
    complete();
    usb_nudge_report(0,0,0,0,2500000); usb_nudge_report(0,0,0,0,2950000);
    usb_nudge_report(0,0,3,0,3000000);
    suspended = true; tud_suspend_cb(false); usb_task(s,3001000);
    suspended = false; tud_resume_cb(); usb_task(s,3080000);
    check(sent.back() == Report{}, "suspend/resume cancels pending wheel nudge");
    complete();
    usb_nudge_report(0,0,0,0,3100000); usb_nudge_report(0,0,0,0,3550000);
    usb_nudge_report(0,0,3,0,3600000);
    s.disconnect(); usb_cancel_nudges(); usb_task(s,3680000);
    check(sent.back() == Report{}, "Bluetooth loss cancels deferred action before deadline");
}
void repeated_with_busy_endpoint() {
    constexpr std::uint64_t t = 10000000;
    StickMapper stick;
    stick.connect();
    tud_mount_cb();
    usb_task(stick,t); complete();
    usb_set_nudge_map({});
    usb_nudge_report(0,0,0,0,t+100000); usb_nudge_report(0,0,0,0,t+500000);
    usb_nudge_report(0,0,0,2,t+600000); usb_nudge_report(0,0,0,0,t+620000);
    usb_task(stick,t+700000);
    check(mask(sent.back())==bit(12), "first B press reaches endpoint");
    complete();
    ready = false;
    usb_task(stick,t+780000); // Pulse ended but the up report cannot be submitted yet.
    usb_nudge_report(0,0,0,2,t+800000); usb_nudge_report(0,0,0,0,t+820000);
    ready = true;
    usb_task(stick,t+900000);
    check(sent.back()==Report{}, "owed button-up is sent first even as repeated B is due");
    complete();
    usb_task(stick,t+901000);
    check(mask(sent.back())==bit(12), "later live B can follow a completed release");
    complete();
    usb_task(stick,t+980000); complete();
    check(usb_neutral_confirmed(), "repeated button fully released");
}
} // namespace

extern "C" void tud_task_ext(std::uint32_t, bool) {}
extern "C" bool tud_mounted() {return true;}
extern "C" bool tud_suspended() {return suspended;}
extern "C" bool tud_hid_ready() {return ready;}
extern "C" bool tud_disconnect() {++detaches; return true;}
extern "C" bool tud_connect() {return true;}
extern "C" bool tud_hid_report(std::uint8_t id, const void *p, std::uint16_t n) {
    if (id || n != Report{}.size()) return false;
    Report r{};
    std::memcpy(r.data(),p,n);
    sent.push_back(r);
    return true;
}
namespace omni { void maintenance_usb_reset() {} }
int main() {
    pure(); usb(); repeated_with_busy_endpoint();
    std::printf("PASS: %u nudge recognition, mapping, lifecycle and USB release checks.\n",checks);
}
