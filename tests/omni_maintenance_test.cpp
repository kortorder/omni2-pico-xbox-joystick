#include "omni_maintenance.hpp"
#if OMNI_CAPTURE_SHORT_KEY || OMNI_SHORT_KEY_COMPAT
#include "omni_bluetooth.hpp"
#endif
#include "omni_nudge_settings.hpp"
#include "btstack_tlv.h"
#include "omni_stick.hpp"
#include "omni_usb.hpp"
#include "tusb.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using namespace omni::maintenance;
using Bytes = std::array<std::uint8_t, 6>;
unsigned checks = 0, request_count = 0, detaches = 0, boots = 0;
bool mounted = true, suspended = false, accept = true;
std::uint64_t clock_us = 100;
std::array<std::uint8_t, 8> answer{};
MotionReply motion_answer{};
omni::NudgeSettingsWire readback{},
    host_packet{'N', 'G', 'W', '1', 0, 0, 0, 0, 0, 12, 5, 3, 'S', 'A', 'V', 'E'}, stored{};
unsigned stored_length = 0, flash_writes = 0, host_bytes = 16;
Bytes last_hid{};
unsigned hid_count = 0;
struct EnterROM {};
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
    ++checks;
}
const tusb_control_request_t probe = {probe_type, probe_request, request_value, request_index, 8};
const tusb_control_request_t enter = {enter_type, enter_request, request_value, request_index, 0};
const tusb_control_request_t motion = {probe_type, motion_request, request_value, request_index, 64};
const tusb_control_request_t capture = {probe_type, capture_request, request_value, request_index, 8};
#if OMNI_CAPTURE_SHORT_KEY
bool candidate_available = false;
// Synthetic byte order only. The real Bluetooth host separately tests authentication and expiry.
constexpr PeerPin synthetic_pin{0x10, 0x21, 0x32, 0x43, 0x54, 0x65};
#endif
#if OMNI_SHORT_KEY_COMPAT
MotionReply test_motion{1, 64, 1, 3};
bool motion_available = true;
#endif
bool send(uint8_t stage, const tusb_control_request_t &req) {
    return tud_vendor_control_xfer_cb(0, stage, &req);
}
const tusb_control_request_t get_settings = {probe_type, nudge_read_request, request_value,
                                             request_index, 16};
const tusb_control_request_t set_settings = {enter_type, nudge_write_request, request_value,
                                             request_index, 16};
void probe_ok() {
    check(send(CONTROL_STAGE_SETUP, probe), "exact capability setup");
    check(send(CONTROL_STAGE_DATA, probe), "capability data stage");
    check(send(CONTROL_STAGE_ACK, probe), "complete capability status");
    check(answer == capability, "exact compatible signature");
}
void complete() { tud_hid_report_complete_cb(0, last_hid.data(), last_hid.size()); }
} // namespace
#if OMNI_CAPTURE_SHORT_KEY
bool omni::bt_copy_short_key_candidate(std::array<std::uint8_t, 6> &out) {
    out = candidate_available ? synthetic_pin : PeerPin{};
    return candidate_available;
}
#endif
#if OMNI_SHORT_KEY_COMPAT
bool omni::bt_copy_motion_snapshot(std::array<std::uint8_t, 64> &out) {
    out = motion_available ? test_motion : MotionReply{};
    return motion_available;
}
#endif
extern "C" void btstack_tlv_get_instance(const btstack_tlv_t **t, void **c) {
    static const btstack_tlv_t flash{
        [](void *, std::uint32_t tag, std::uint8_t *data, std::uint32_t n) {
            check(tag == 0x4f4d4e33, "storage reads only settings tag");
            if (!data)
                return static_cast<int>(stored_length);
            const auto length = std::min(n, stored_length);
            std::copy_n(stored.begin(), length, data);
            return static_cast<int>(length);
        },
        [](void *, std::uint32_t tag, const std::uint8_t *data, std::uint32_t n) {
            check(tag == 0x4f4d4e33 && n == 16 && omni::usb_neutral_confirmed(),
                  "persist only full record after fresh neutral");
            std::copy_n(data, n, stored.begin());
            stored_length = n;
            ++flash_writes;
            return 0;
        },
        [](void *, std::uint32_t) { check(false, "no key deletion"); }};
    *t = &flash;
    *c = nullptr;
}
uint64_t time_us_64() { return clock_us; }
[[noreturn]] void reset_usb_boot(uint32_t mask, uint32_t disabled) {
    check(!mask && !disabled, "stock ROM BOOTSEL mode");
    ++boots;
    throw EnterROM{};
}
extern "C" bool tud_mounted() { return mounted; }
extern "C" bool tud_suspended() { return suspended; }
extern "C" bool tud_control_xfer(uint8_t, const tusb_control_request_t *req, void *p, uint16_t n) {
    if (!accept || (n != 8 && n != 16 && n != 64) || !p)
        return false;
    ++request_count;
    if (n == 64)
        std::memcpy(motion_answer.data(), p, n);
    else if (n == 16 && req->bmRequestType == probe_type)
        std::memcpy(readback.data(), p, n);
    else if (n == 16)
        std::memcpy(p, host_packet.data(), std::min<unsigned>(n, host_bytes));
    else
        std::memcpy(answer.data(), p, n);
    return true;
}
extern "C" bool tud_control_status(uint8_t, const tusb_control_request_t *) { return accept; }
extern "C" void tud_task_ext(uint32_t, bool) {}
extern "C" bool tud_hid_ready() { return true; }
extern "C" bool tud_disconnect() {
    ++detaches;
    return true;
}
extern "C" bool tud_connect() { return true; }
extern "C" bool tud_hid_report(uint8_t id, const void *bytes, uint16_t n) {
    if (id || n != 6)
        return false;
    std::memcpy(last_hid.data(), bytes, n);
    ++hid_count;
    return true;
}
int main() {
    omni::StickMapper mapper;
    tud_mount_cb();
    omni::nudge_settings_init();
    check(!send(CONTROL_STAGE_SETUP, get_settings) && !send(CONTROL_STAGE_SETUP, set_settings),
          "no settings read/write without probe");
#if !OMNI_CAPTURE_SHORT_KEY
    auto get = []() {
        probe_ok();
        check(send(CONTROL_STAGE_SETUP, get_settings) && send(CONTROL_STAGE_DATA, get_settings) &&
                  send(CONTROL_STAGE_ACK, get_settings),
              "fresh GET completes");
    };
    auto begin_set = []() {
        probe_ok();
        check(send(CONTROL_STAGE_SETUP, set_settings), "exact reviewed SET accepted after probe");
    };
    auto queue_set = [&]() {
        begin_set();
        check(send(CONTROL_STAGE_DATA, set_settings) && send(CONTROL_STAGE_ACK, set_settings),
              "validated OUT plus finished status queues save");
    };
    get();
    check(readback[0] == 'N' && readback[4] == 0 && readback[8] == 12 && readback[9] == 4 &&
              readback[10] == 5 && readback[11] == 3 && readback[12] == 0,
          "fresh settings read in right/left/up/down order");
    probe_ok();
    auto wrong_settings = set_settings;
    wrong_settings.wLength = 15;
    check(!send(CONTROL_STAGE_SETUP, wrong_settings), "reject wrong settings payload size");
    host_bytes = 11;
    begin_set();
    check(!send(CONTROL_STAGE_DATA, set_settings) && !flash_writes, "short OUT cannot reuse SAVE tail");
    host_bytes = 16;
    begin_set();
    check(!send(CONTROL_STAGE_ACK, set_settings), "ACK without validated DATA cannot write");
    tud_suspend_cb(false);
    tud_resume_cb();
    begin_set();
    check(send(CONTROL_STAGE_DATA, set_settings), "settings received only");
    tud_suspend_cb(false);
    tud_resume_cb();
    check(!send(CONTROL_STAGE_ACK, set_settings) && !flash_writes,
          "suspend before write ACK fences commit");
    mapper.connect();
    omni::usb_task(mapper, ++clock_us);
    complete();
    mapper.motion(48, 0, ++clock_us);
    omni::usb_task(mapper, clock_us);
    check(last_hid[0] == 0xff && last_hid[1] == 0x7f, "nonzero in-flight report before save");
    queue_set();
    get();
    check(readback[13] == 1 && !flash_writes, "SET ACK leaves old revision and pending result");
    check(omni::maintenance_poll(mapper, ++clock_us) && !flash_writes,
          "wait for pending old USB transfer");
    complete();
    check(omni::maintenance_poll(mapper, ++clock_us) && last_hid == Bytes{} && !flash_writes,
          "old completion cannot satisfy new neutral barrier");
    complete();
    check(omni::maintenance_poll(mapper, ++clock_us) && flash_writes == 1,
          "completed neutral permits one verified store");
    get();
    check(readback[4] == 1 && readback[8] == 0 && readback[9] == 12 && readback[12] == 1 &&
              readback[13] == 2 && omni::usb_nudge_map().right == 0 && omni::usb_nudge_map().left == 12,
          "confirmed exact settings readback and runtime");
    begin_set();
    check(!send(CONTROL_STAGE_DATA, set_settings) && flash_writes == 1, "replayed stale edit rejected");
    get();
    check(readback[13] == 3, "stale outcome exposed");
    host_packet[4] = 1;
    queue_set();
    tud_suspend_cb(false);
    tud_resume_cb();
    get();
    check(readback[13] == 6 && readback[4] == 1 && flash_writes == 1,
          "USB reset before flash preserves mappings");
    queue_set();
    check(omni::maintenance_poll(mapper, ++clock_us), "new save waiting for neutral");
    clock_us += 1'000'000;
    check(omni::maintenance_poll(mapper, clock_us) && flash_writes == 1,
          "unpolled neutral times out without flash");
    get();
    check(readback[13] == 6, "timeout result is visible");
    tud_umount_cb();
    tud_mount_cb();
#else
    probe_ok();
    check(!send(CONTROL_STAGE_SETUP, get_settings), "capture image excludes settings GET");
    probe_ok();
    check(!send(CONTROL_STAGE_SETUP, set_settings), "capture image excludes settings SET");
#endif
    check(!send(CONTROL_STAGE_SETUP, motion), "motion snapshot requires fresh capability");
    probe_ok();
#if OMNI_SHORT_KEY_COMPAT
    test_motion[20] = 4;
    test_motion[59] = 0xf0;
    test_motion[62] = 2;
    check(send(CONTROL_STAGE_SETUP, motion) && motion_answer == test_motion,
          "exact pinned snapshot returned");
    check(send(CONTROL_STAGE_DATA, motion) && send(CONTROL_STAGE_ACK, motion),
          "read-only data and status complete");
    check(!omni::maintenance_poll(mapper, clock_us) && !send(CONTROL_STAGE_SETUP, enter),
          "motion snapshot does not arm BOOTSEL");
    check(!send(CONTROL_STAGE_SETUP, motion), "snapshot consumes capability");
    probe_ok();
    auto wrong = motion;
    wrong.wLength = 8;
    check(!send(CONTROL_STAGE_SETUP, wrong), "wrong snapshot length refused");
    probe_ok();
    clock_us += 3'000'000;
    check(!send(CONTROL_STAGE_SETUP, motion), "expired capability refuses snapshot");
    probe_ok();
    motion_available = false;
    check(!send(CONTROL_STAGE_SETUP, motion), "unavailable data refuses transfer");
    motion_available = true;
#else
    check(!send(CONTROL_STAGE_SETUP, motion) && !send(CONTROL_STAGE_DATA, motion) &&
              !send(CONTROL_STAGE_ACK, motion),
          "ordinary and capture builds exclude motion diagnostics");
#endif
    // A private read consumes its own short-lived capability and never enters maintenance.
    check(!send(CONTROL_STAGE_SETUP, capture), "capture requires a fresh probe");
    probe_ok();
    const auto prior_reads = request_count;
    check(!send(CONTROL_STAGE_SETUP, capture) && request_count == prior_reads,
          "absent eligible peer returns no address or dummy pin");
#if OMNI_CAPTURE_SHORT_KEY
    candidate_available = true;
    auto bad = capture;
    bad.wLength = 6;
    probe_ok();
    check(!send(CONTROL_STAGE_SETUP, bad), "wrong capture length refused");
    probe_ok();
    check(send(CONTROL_STAGE_SETUP, capture), "eligible strict capture read accepted");
    check(answer[0] == 1 && answer[1] == 7 &&
              std::equal(synthetic_pin.begin(), synthetic_pin.end(), answer.begin() + 2),
          "capture snapshot preserves canonical six-byte address and version");
    check(send(CONTROL_STAGE_DATA, capture) && send(CONTROL_STAGE_ACK, capture),
          "read stage and its acknowledgement finish without a boot request");
    check(!send(CONTROL_STAGE_SETUP, enter) && !omni::maintenance_poll(mapper, clock_us),
          "reading a pin does not arm or enter ROM");
    check(!send(CONTROL_STAGE_SETUP, capture), "second read also needs a fresh capability");
    probe_ok();
    clock_us += 3'000'000;
    check(!send(CONTROL_STAGE_SETUP, capture), "stale probe cannot export");
    probe_ok();
    accept = false;
    check(!send(CONTROL_STAGE_SETUP, capture), "unsent snapshot does not commit");
    accept = true;
    check(!send(CONTROL_STAGE_ACK, capture), "stale capture ACK rejected");
    candidate_available = false;
#else
    check(!send(CONTROL_STAGE_DATA, capture) && !send(CONTROL_STAGE_ACK, capture),
          "ordinary firmware has no capture transfer");
#endif
    check(!omni::maintenance_poll(mapper, clock_us), "ordinary polling has no maintenance effect");
    check(!send(CONTROL_STAGE_SETUP, enter), "no entry without capability");
    for (auto wrong :
         {tusb_control_request_t{0x41, enter_request, request_value, request_index, 0},
          tusb_control_request_t{0x21, enter_request, request_value, request_index, 0},
          tusb_control_request_t{0xc0, enter_request, request_value, request_index, 0},
          tusb_control_request_t{enter_type, probe_request, request_value, request_index, 0},
          tusb_control_request_t{enter_type, enter_request, 0, request_index, 0},
          tusb_control_request_t{enter_type, enter_request, request_value, 0, 0},
          tusb_control_request_t{enter_type, enter_request, request_value, request_index, 8}}) {
        probe_ok();
        check(!send(CONTROL_STAGE_SETUP, wrong), "wrong type, recipient, signature or length refused");
        check(!omni::maintenance_poll(mapper, clock_us), "malformed request cannot commit");
    }
    check(!tud_vendor_control_xfer_cb(1, CONTROL_STAGE_SETUP, &probe), "wrong USB port");
    mounted = false;
    check(!send(CONTROL_STAGE_SETUP, probe), "unconfigured USB cannot probe");
    mounted = true;
    suspended = true;
    check(!send(CONTROL_STAGE_SETUP, probe), "suspended USB cannot probe");
    suspended = false;
    accept = false;
    check(!send(CONTROL_STAGE_SETUP, probe), "failed capability transfer stays idle");
    accept = true;
    check(!send(CONTROL_STAGE_ACK, probe), "ACK without a valid setup cannot arm");
    check(send(CONTROL_STAGE_SETUP, probe), "start incomplete probe");
    check(!send(CONTROL_STAGE_SETUP, enter), "incomplete probe cannot authorize entry");
    probe_ok();
    clock_us += 3'000'000;
    check(!send(CONTROL_STAGE_SETUP, enter), "capability expires");
    probe_ok();
    tud_umount_cb();
    tud_mount_cb();
    check(!send(CONTROL_STAGE_SETUP, enter), "capability does not cross a USB bus session");
    probe_ok();
    check(send(CONTROL_STAGE_SETUP, enter), "start interrupted entry");
    tud_suspend_cb(false);
    tud_resume_cb();
    check(!send(CONTROL_STAGE_ACK, enter), "suspend cancels unconfirmed entry");
    probe_ok();
    check(send(CONTROL_STAGE_SETUP, enter), "start delayed entry");
    clock_us += 3'000'000;
    check(!send(CONTROL_STAGE_ACK, enter), "late entry ACK cannot boot");
    probe_ok();
    accept = false;
    check(!send(CONTROL_STAGE_SETUP, enter), "unaccepted status cannot latch entry");
    accept = true;
    check(!send(CONTROL_STAGE_ACK, enter), "no confirmation after failed status");

    mapper.connect();
    omni::usb_task(mapper, clock_us);
    complete();
    mapper.motion(48, 0, clock_us + 1);
    omni::usb_task(mapper, clock_us + 1);
    complete();
    check(last_hid == Bytes{0xff, 0x7f, 0, 0, 0, 0}, "live motion before maintenance");
    probe_ok();
    check(send(CONTROL_STAGE_SETUP, enter), "accept exact enter after probe");
    check(!omni::maintenance_poll(mapper, clock_us), "setup alone does not change live USB state");
    check(send(CONTROL_STAGE_ACK, enter), "only exact completed status latches entry");
    auto start = ++clock_us;
    const unsigned sent = hid_count;
    check(omni::maintenance_poll(mapper, start) && last_hid == Bytes{} && hid_count == sent + 1,
          "main sends actual USB neutral before detach");
    complete();
    mapper.motion(48, 0, start + 1);
    check(mapper.sample(start + 1) == omni::Axes{}, "further mouse reports ignored");
    check(omni::maintenance_poll(mapper, start + 199999) && !detaches && !boots, "full grace interval");
    check(omni::maintenance_poll(mapper, start + 200000) && detaches == 1 && !boots,
          "D+ detached first");
    tud_umount_cb();
    check(omni::maintenance_poll(mapper, start + 349999) && !boots && detaches == 1,
          "full disconnect delay");
    try {
        omni::maintenance_poll(mapper, start + 350000);
        check(false, "ROM returned");
    } catch (EnterROM &) {
        check(boots == 1 && detaches == 1, "reset once in main after completed request and detach");
    }
    std::printf("PASS: %u wired maintenance firmware checks. No physical USB or reboot.\n", checks);
}
