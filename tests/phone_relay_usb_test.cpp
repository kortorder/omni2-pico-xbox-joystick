// Real USB adapter + shared SCX2 receiver; only hardware and time are substituted.
#include "omni_phone_relay.hpp"
#include "omni_maintenance.hpp"
#include "omni_usb.hpp"
#include "tusb.h"
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
using namespace omni;
using namespace scx2;
using Report = std::array<std::uint8_t, 6>;
unsigned checks = 0;
std::uint64_t now = 1;
bool mounted = true, suspended = false, accepts = true;
std::vector<Report> reports;
std::vector<Packet> replies;
unsigned disconnects = 0;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s at %llu\n", name, (unsigned long long)now); std::exit(1); }
}
Token token(void *) { Token out{}; out[0] = 3; return out; }
void reply(const Packet &packet, void *) { replies.push_back(packet); }
PhoneRelay relay(token(nullptr), token, nullptr, reply);
StickMapper chair;
Packet live;
Axes axes(const Report &r) {
    return {std::int16_t(r[0] | (r[1] << 8)), std::int16_t(r[2] | (r[3] << 8))};
}
void tick(std::uint64_t step = 1000) { now += step; usb_task(chair, now); }
void complete() { auto r = reports.back(); tud_hid_report_complete_cb(0, r.data(), r.size()); }
void settle() { tick(); complete(); tick(); complete(); }
void connect() {
    // Global and source-only neutral barriers may each need a physical transfer.
    settle();
    Packet hello; hello.session[0] = std::uint8_t(now % 255 + 1);
    auto ready = relay.receive(hello, now);
    check(ready && ready->hid_ready, "offer after real neutral");
    live = *ready; live.kind = Kind::state; live.hid_ready = false; live.sequence = 1;
    check(!relay.receive(live, now), "neutral pending until USB");
    const auto count = replies.size(); tick();
    check(replies.size() == count, "USB submit alone does not ACK neutral");
    complete(); check(replies.size() == count + 1 && relay.armed(), "neutral ACK on completion");
}
void send(Axes canonical) {
    ++live.sequence;
    live.state.axes[2] = canonical.x; live.state.axes[3] = canonical.y;
    check(!relay.receive(live, now), "accepted work does not ACK early");
}
void assert_report(Axes expected, const char *name) { check(axes(reports.back()) == expected, name); }
}
std::uint64_t time_us_64() { return now; }
void omni::maintenance_usb_reset() {}
extern "C" void tud_task_ext(std::uint32_t timeout, bool irq) { check(!timeout && !irq, "nonblocking USB poll"); }
extern "C" bool tud_mounted() { return mounted; }
extern "C" bool tud_suspended() { return suspended; }
extern "C" bool tud_hid_ready() { return true; }
extern "C" bool tud_disconnect() { ++disconnects; return true; }
extern "C" bool tud_connect() { return true; }
extern "C" bool tud_hid_report(std::uint8_t id, const void *data, std::uint16_t length) {
    check(!id && length == 6, "unchanged six-byte descriptor report");
    if (!accepts) return false;
    Report r{}; std::memcpy(r.data(), data, length); reports.push_back(r); return true;
}
int main() {
    phone_relay_attach(&relay); tud_mount_cb(); chair.connect(); connect();
    for (const auto a : {Axes{14000,0}, Axes{-14000,0}, Axes{0,14000}, Axes{0,-14000}}) {
        chair.clear_motion(); chair.motion(0, 24, now);
        send(a); const auto before = replies.size(); tick();
        assert_report({a.x, std::int16_t(-a.y)}, "phone priority and exactly one Y inversion");
        check(replies.size() == before, "nonzero ACK waits for transfer completion");
        complete(); check(replies.back().sequence == live.sequence, "matching source sequence ACK");
    }
    chair.clear_motion(); chair.motion(24,0,now); send({}); tick();
    assert_report({16384,0}, "phone neutral preserves live physical chair");
    const auto before = replies.size(); complete();
    check(replies.size() == before + 1 && replies.back().sequence == live.sequence,
          "neutral acknowledged by combined non-neutral chair report");
    send({9000,8000}); tick(); complete();
    chair.disconnect(); tick(); assert_report({9000,-8000}, "BT loss does not retire phone"); complete();
    chair.connect(); chair.motion(-24,0,now); tick(); complete();
    check(relay.armed(), "BT reconnect does not retire phone");
    now += PhoneRelay::lease_us; chair.clear_motion(); chair.motion(-24,0,now); tick();
    assert_report({-16384,0}, "relay lease expiry hands back live chair"); complete();
    check(!relay.armed(), "expired source is fenced");
    connect(); send({7000,0}); tick(); complete();
    auto stop = live; stop.kind=Kind::stop; stop.state={}; ++stop.sequence;
    check(!relay.receive(stop,now), "STOP waits for neutral source completion");
    chair.clear_motion(); chair.motion(0,-24,now); tick(); assert_report({0,-16384}, "STOP leaves chair active");
    complete(); check(replies.back().kind == Kind::stopped, "STOPPED after physical handoff");
    connect(); send({8000,0}); tick(); // deliberately leave transfer pending
    const auto stale = reports.back(); const auto count = replies.size();
    suspended=true; tud_suspend_cb(false); complete();
    check(replies.size()==count && !relay.active(), "USB suspend fences old completion");
    suspended=false; tud_resume_cb(); tick(); assert_report({}, "resume requires physical whole-device neutral"); complete();
    tick(); complete(); check(!relay.armed(), "resume never replays phone hold");
    connect(); send({4000,0}); accepts=false; tick();
    now+=PhoneRelay::lease_us; accepts=true; chair.clear_motion(); chair.motion(24,0,now); tick();
    assert_report({16384,0}, "unsent expired work cannot replay"); complete();
    check(!relay.armed(), "unsent expired source retired");
    connect(); send({12000,0}); tick(); complete();
    usb_request_neutral(chair); tick(); assert_report({}, "maintenance is global neutral barrier"); complete();
    check(!relay.active(), "maintenance invalidates remote session");
    connect(); send({13000,0}); tick(); complete();
    relay.network(false,now); chair.clear_motion(); chair.motion(0,24,now); tick();
    assert_report({0,16384}, "Wi-Fi loss preserves chair source"); complete();
    relay.network(true,now); settle(); connect();
    auto invalid=live; ++invalid.sequence;
    for (unsigned field=0;field<5;++field) {
        invalid.state={}; invalid.format=Format::legacy;
        if(field==0) invalid.state.axes[0]=1;
        if(field==1) invalid.state.axes[1]=1;
        if(field==2) invalid.state.buttons=1;
        if(field==3) invalid.state.hat=0;
        if(field==4) invalid.format=Format::usb_buttons;
        check(!relay.receive(invalid,now), "non-RS input rejected");
    }
    send({5000,0}); tick(); complete();
    tud_hid_report_failed_cb(0,HID_REPORT_TYPE_INPUT,nullptr,0); tick();
    assert_report({}, "USB failure retires both sources"); complete(); check(!relay.active(), "USB failure fences session");
    connect(); send({5000,0}); tick(); // outstanding physical nonzero
    now+=400000; tick(); check(disconnects==1 && !relay.active(), "stalled USB detaches and retires remote");
    // A delayed callback cannot satisfy the new epoch's barrier.
    const auto n=replies.size(); tud_hid_report_complete_cb(0,stale.data(),stale.size());
    check(replies.size()==n,"old callback cannot ACK new generation");
    std::printf("PASS: %u optional relay/real USB adapter checks\n",checks);
}
