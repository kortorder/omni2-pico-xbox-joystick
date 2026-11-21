// Production main loop, real merge/USB/SCX2/maintenance; synthetic radio boundaries.
#include "omni_bluetooth.hpp"
#include "omni_maintenance.hpp"
#include "omni_phone_relay.hpp"
#include "omni_phone_wifi.hpp"
#include "btstack_tlv.h"
#include "tusb.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
int firmware_loop_main();
namespace {
struct End {};
std::uint64_t us = 0;
omni::BtCallbacks callbacks{};
omni::BtState bt = omni::BtState::connected;
std::array<std::uint8_t,6> tx{};
bool inflight = false;
unsigned ack_count = 0, checks = 0;
bool phone_right=false, preserved_during_bt_loss=false, handoff=false, stopped=false,
     phone_up=false, network_handoff=false, maintenance_neutral=false;
scx2::Token random(void*) { scx2::Token t{}; t[0]=9; return t; }
void check(bool ok,const char *s) { ++checks; if(!ok) {std::fprintf(stderr,"FAIL %s at %llu\n",s,(unsigned long long)us); std::exit(1);} }
void ack(const scx2::Packet &,void*) { ++ack_count; check(!inflight,"ACK follows actual USB completion"); }
omni::PhoneRelay relay(random(nullptr),random,nullptr,ack);
scx2::Packet packet;
void connect(unsigned session) {
    scx2::Packet hello; hello.session[0]=session;
    auto ready=relay.receive(hello,us); check(ready && ready->hid_ready,"loop offer after neutral");
    packet=*ready; packet.kind=scx2::Kind::state; packet.hid_ready=false; packet.sequence=1;
    relay.receive(packet,us);
}
void state(int x,int y) {
    ++packet.sequence; packet.state.axes[2]=x; packet.state.axes[3]=y; relay.receive(packet,us);
}
}
bool tusb_init() { tud_mount_cb(); return true; }
int omni::phone_wifi_radio_init() { return 0; }
void omni::phone_wifi_start() { phone_relay_attach(&relay); }
omni::PhoneWifiStatus omni::phone_wifi_status() { return {1,1,3,0,192,0,2,1,166,241,1,1}; }
void omni::phone_wifi_poll(std::uint64_t) {
    if(us==100000) connect(1);
    if(us>=150000 && us<400000 && us%50000==0) state(12000,0);
    if(us>=400000 && us<600000 && us%50000==0) state(0,0);
    if(us==600000) { auto stop=packet; stop.kind=scx2::Kind::stop; stop.state={}; ++stop.sequence; relay.receive(stop,us); }
    if(us==800000) connect(2);
    if(us==850000) state(0,20000);
    if(us==900000) relay.network(false,us);
    if(us==1050000) {
        using namespace omni::maintenance;
        tusb_control_request_t p{probe_type,probe_request,request_value,request_index,8};
        check(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&p),"maintenance probe");
        check(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&p),"maintenance probe ack");
        tusb_control_request_t e{enter_type,enter_request,request_value,request_index,0};
        check(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&e),"maintenance enter");
        check(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&e),"maintenance enter ack");
    }
}
void cyw43_arch_gpio_put(int,bool) {}
void watchdog_enable(std::uint32_t,bool) {}
void watchdog_update() {}
void watchdog_reboot(std::uint32_t,std::uint32_t,std::uint32_t) {std::abort();}
void tight_loop_contents() {std::abort();}
std::uint64_t time_us_64() {return us;}
void sleep_us(std::uint64_t elapsed) {us+=elapsed; if(us>=1150000) throw End{};}
bool omni::bt_start(BtCallbacks cb) {callbacks=cb; callbacks.neutral(); return true;}
omni::BtState omni::bt_state() {return bt;}
omni::BtDiagnostic omni::bt_diagnostic() {return {};}
bool omni::bt_recovery_requested() {return false;}
bool omni::bt_copy_motion_snapshot(std::array<std::uint8_t,64>&) {return false;}
void omni::bt_tick(std::uint64_t) {}
namespace omni {void poll_pair_button(std::uint64_t) {}}
void cyw43_arch_poll() {
    if(us==250000) {bt=omni::BtState::reconnecting; callbacks.neutral();}
    if(us==300000) bt=omni::BtState::connected;
    if(us>=200000 && us<=650000 && us%20000==0 && bt==omni::BtState::connected) callbacks.mouse(-1,0,0,0);
    if(us>=900000 && us<=1040000 && us%20000==0) callbacks.mouse(1,0,0,0);
}
extern "C" void btstack_tlv_get_instance(const btstack_tlv_t **impl,void **ctx) {
    static btstack_tlv_t tlv={[](void*,std::uint32_t,std::uint8_t*,std::uint32_t){return 0;},
        [](void*,std::uint32_t,const std::uint8_t*,std::uint32_t){return 1;},[](void*,std::uint32_t){}};
    *impl=&tlv; *ctx=nullptr;
}
extern "C" bool tud_hid_report(std::uint8_t id,const void*data,std::uint16_t size) {
    check(!inflight && !id && size==6,"one unchanged USB report in flight");
    std::copy_n(static_cast<const std::uint8_t*>(data),size,tx.begin()); inflight=true;
    const auto x=std::int16_t(tx[0]|(tx[1]<<8)), y=std::int16_t(tx[2]|(tx[3]<<8));
    if(us>=150000 && us<250000) phone_right|=x==12000 && y==0;
    if(us>=250000 && us<300000) {check(x==12000 && y==0,"phone survives Bluetooth disconnect"); preserved_during_bt_loss=true;}
    if(us>401000 && us<600000) {check(x<0 && y==0,"neutral returns current chair"); handoff=true;}
    if(us>601000 && us<650000) {check(x<0 && y==0,"STOP returns current chair"); stopped=true;}
    if(us>=850000 && us<900000) phone_up|=x==0 && y==-20000;
    if(us>=901000 && us<1040000) {check(x>0 && y==0,"network loss returns chair"); network_handoff=true;}
    if(us>=1051000) {check(x==0 && y==0 && !tx[4] && !tx[5],"maintenance retires both sources"); maintenance_neutral=true;}
    return true;
}
extern "C" void tud_task_ext(std::uint32_t,bool) {if(inflight) {inflight=false; tud_hid_report_complete_cb(0,tx.data(),tx.size());}}
extern "C" bool tud_mounted() {return true;}
extern "C" bool tud_suspended() {return false;}
extern "C" bool tud_hid_ready() {return !inflight;}
extern "C" bool tud_disconnect() {std::abort();}
extern "C" bool tud_connect() {std::abort();}
extern "C" bool tud_control_status(std::uint8_t,const tusb_control_request_t*) {return true;}
extern "C" bool tud_control_xfer(std::uint8_t,const tusb_control_request_t*,void*,std::uint16_t) {return true;}
[[noreturn]] void reset_usb_boot(std::uint32_t,std::uint32_t) {std::abort();}
int main() {
    try {firmware_loop_main();} catch(const End&) {}
    check(phone_right && preserved_during_bt_loss && handoff && stopped && phone_up && network_handoff && maintenance_neutral,
          "every main-loop source and maintenance scenario reached");
    check(ack_count>=10,"live heartbeat states acknowledged");
    std::printf("PASS: %u optional relay production-loop checks\n",checks);
}
