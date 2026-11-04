// Exercise the compiled production polling loop, not a second gesture mapper.
// Event spacing models the measured Mouse 2 report windows; this is a replay
// with synthetic boundary timings, not a second hardware recording.
#include "omni_bluetooth.hpp"
#include "omni_maintenance.hpp"
#include "btstack_tlv.h"
#include "tusb.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
int firmware_loop_main();
namespace {
struct End {};
std::uint64_t us{};
omni::BtCallbacks receiver{};
omni::BtState state = omni::BtState::connected;
std::array<std::uint8_t, 6> tx{};
bool inflight = false, moved = false, neutral_after_motion = false;
std::vector<unsigned> pressed;
unsigned last_mask{};
std::vector<std::uint64_t> on, off;
struct Event { unsigned ms; int x, y, wheel; unsigned buttons; };
// Click windows about 260 ms; wheel positive 3x, negative 6x, about 260 ms apart.
const Event inputs[]{
    {300,1,0,0,0},{320,1,0,0,0},{400,1,0,0,0},
    {1300,0,0,0,2},{1560,0,0,0,0},
    {2400,0,0,0,1},{2660,0,0,0,0},
    {3600,0,0,3,0},{3860,0,0,3,0},{4120,0,0,3,0},
    {5600,0,0,-3,0},{5860,0,0,-3,0},{6120,0,0,-3,0},
    {6380,0,0,-3,0},{6640,0,0,-3,0},{6900,0,0,-3,0},
    // Click that involves even a single count of camera motion must be vetoed.
    {8000,0,0,0,2},{8040,0,-1,0,2},{8260,0,0,0,0},
    // Link loss between down and up must not convert into a tap after reconnect.
    {9300,0,0,0,1},{9520,0,0,0,0}
};
std::size_t event_index{};
unsigned held{};
void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s at %llu us\n", message,
                           static_cast<unsigned long long>(us)); std::exit(1); }
}
}
// Hardware/time boundaries only. Boot, connected-input gating, mapper, nudge
// decisions, maintenance scheduler, HID pack and USB transfer completion are real.
bool tusb_init() { tud_mount_cb(); return true; }
int cyw43_arch_init() { return 0; }
void cyw43_arch_gpio_put(int, bool) {}
void watchdog_enable(std::uint32_t, bool) {}
void watchdog_update() {}
void watchdog_reboot(std::uint32_t, std::uint32_t, std::uint32_t) { std::abort(); }
void tight_loop_contents() { std::abort(); }
std::uint64_t time_us_64() { return us; }
void sleep_us(std::uint64_t elapsed) { us += elapsed; if (us >= 10500000) throw End{}; }
bool omni::bt_start(BtCallbacks cb) { receiver=cb; receiver.neutral(); return true; }
omni::BtState omni::bt_state() { return state; }
omni::BtDiagnostic omni::bt_diagnostic() { return {}; }
bool omni::bt_recovery_requested() { return false; }
bool omni::bt_copy_motion_snapshot(std::array<std::uint8_t,64>&) { return false; }
void omni::bt_tick(std::uint64_t) {}
namespace omni { void poll_pair_button(std::uint64_t) {} }
void cyw43_arch_poll() {
    if (us == 9440000) { state=omni::BtState::reconnecting; receiver.neutral(); }
    if (us == 9500000) state=omni::BtState::connected;
    if (us % 1000) return;
    if (event_index < sizeof(inputs)/sizeof(*inputs) && inputs[event_index].ms == us/1000) {
        const auto &event=inputs[event_index++]; held=event.buttons;
        receiver.mouse(event.x,event.y,event.wheel,held);
    } else if (us % 20000 == 0 && state==omni::BtState::connected) receiver.mouse(0,0,0,held);
}
extern "C" void btstack_tlv_get_instance(const btstack_tlv_t **impl, void **ctx) {
    static btstack_tlv_t tlv = {[](void*,std::uint32_t,std::uint8_t*,std::uint32_t){return 0;},
        [](void*,std::uint32_t,const std::uint8_t*,std::uint32_t){return 1;},
        [](void*,std::uint32_t){}};
    *impl=&tlv; *ctx=nullptr;
}
extern "C" bool tud_hid_report(std::uint8_t id,const void *data,std::uint16_t size) {
    require(!inflight && !id && size==tx.size(),"one six-byte HID report per transfer");
    std::copy_n(static_cast<const std::uint8_t*>(data),size,tx.begin()); inflight=true;
    const unsigned mask=unsigned(tx[4]) | (unsigned(tx[5])<<8);
    const bool axis=tx[0] || tx[1] || tx[2] || tx[3];
    if (mask != last_mask) {
        if (mask) { pressed.push_back(mask); on.push_back(us); }
        else off.push_back(us);
        last_mask=mask;
    }
    if (us >= 300000 && us < 1100000) {
        if (axis) moved=true;
        else if (moved) neutral_after_motion=true;
    }
    require(!axis || !mask,"camera motion is not accompanied by a shortcut");
    return true;
}
extern "C" void tud_task_ext(std::uint32_t,bool) {
    if (inflight) { inflight=false; tud_hid_report_complete_cb(0,tx.data(),tx.size()); }
}
extern "C" bool tud_mounted() { return true; }
extern "C" bool tud_suspended() { return false; }
extern "C" bool tud_hid_ready() { return !inflight; }
extern "C" bool tud_disconnect() { std::abort(); }
extern "C" bool tud_connect() { std::abort(); }
extern "C" bool tud_control_status(std::uint8_t,const tusb_control_request_t*) { return false; }
extern "C" bool tud_control_xfer(std::uint8_t,const tusb_control_request_t*,void*,std::uint16_t) { return false; }
[[noreturn]] void reset_usb_boot(std::uint32_t,std::uint32_t) { std::abort(); }
int main() {
    try { firmware_loop_main(); } catch (const End&) {}
    require(moved && neutral_after_motion,"one-count camera changes reach USB and center");
    require(pressed==std::vector<unsigned>({0x800,0x008,0x010,0x004}),
        "exactly one B, RB, X, RS and no mixed-motion or disconnect ghost");
    require(!last_mask && on.size()==off.size(),"every Xbox press released");
    for (std::size_t i=0;i<on.size();++i)
        require(off[i]-on[i]>=16000 && off[i]-on[i]<=500000,"bounded visible USB tap");
    std::puts("PASS: full polling-loop replay: 4 single taps (B, RB, X, RS), fine aim and center, mixed-motion veto, disconnect cleanup.");
}
