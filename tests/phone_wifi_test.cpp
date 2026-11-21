#include "omni_phone_wifi.hpp"
#include "omni_phone_relay.hpp"
#include "pico/cyw43_arch.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
namespace {
std::uint64_t now=1;
int link=0,joins=0;
int pm_calls=0;
bool sta_enabled=false,pm_fail=false;
udp_recv_fn callback=nullptr;
unsigned checks=0;
struct Out {scx2::Packet packet; ip_addr_t address; u16_t port;};
std::vector<Out> sent;
void check(bool ok,const char*name) {++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
void inject(const scx2::Packet &packet,ip_addr_t address,u16_t port) {
    auto wire=scx2::encode(packet);check(wire.has_value(),"valid fixture packet");
    auto *p=new pbuf; p->tot_len=wire->size;p->source=address;std::memcpy(p->data,wire->bytes.data(),wire->size);
    // Address deliberately lives in the same allocation as pbuf. ASan detects use after free.
    callback(nullptr,nullptr,p,&p->source,port);
}
void complete() {
    auto *r=omni::phone_relay();r->poll(true,++now);r->prepare(now);
    auto ticket=r->pending_ticket();check(ticket!=0,"source USB work exists");r->submitted(ticket);r->complete(ticket,true,++now);
}
}
WifiState cyw43_state;
std::uint64_t time_us_64(){return now;}
std::uint64_t get_rand_64(){return 123456;}
int cyw43_arch_init_with_country(int){return 0;}
void cyw43_arch_lwip_begin(){}
void cyw43_arch_lwip_end(){}
void cyw43_arch_enable_sta_mode(){sta_enabled=true;}
int cyw43_wifi_pm(WifiState*,std::uint32_t mode){
    ++pm_calls;check(sta_enabled&&joins==0&&mode==CYW43_NONE_PM,"disable PM only after STA-up and before joining");
    return pm_fail?-1:0;
}
int cyw43_arch_wifi_connect_async(const char*,const char*,int){check(pm_calls==1&&!pm_fail,"join requires successful PM setup");++joins;return 0;}
int cyw43_tcpip_link_status(WifiState*,int){return link;}
udp_pcb *udp_new_ip_type(int){return new udp_pcb;}
int udp_bind(udp_pcb*,const ip_addr_t*,u16_t port){check(port==42737,"dedicated relay port");return ERR_OK;}
void udp_recv(udp_pcb*,udp_recv_fn cb,void*){callback=cb;}
void udp_remove(udp_pcb*p){delete p;callback=nullptr;}
pbuf *pbuf_alloc(int,std::uint16_t length,int){auto*p=new pbuf;p->tot_len=length;return p;}
int pbuf_take(pbuf*p,const void*data,std::uint16_t n){std::memcpy(p->data,data,n);return ERR_OK;}
std::uint16_t pbuf_copy_partial(pbuf*p,void*data,std::uint16_t n,std::uint16_t at){std::memcpy(data,p->data+at,n);return n;}
void pbuf_free(pbuf*p){delete p;}
int udp_sendto(udp_pcb*,pbuf*p,const ip_addr_t*a,u16_t port){
    auto packet=scx2::decode(p->data,p->tot_len);check(packet.has_value(),"valid wire reply");sent.push_back({*packet,*a,port});return ERR_OK;
}
int main(int argc,char**){
    pm_fail=argc>1;
    check(omni::phone_wifi_radio_init()==0,"country-aware radio init");omni::phone_wifi_start();
    if(pm_fail){
        link=3;cyw43_state.netif[0].address={{192,0,2,9}};
        for(unsigned i=0;i<5;++i){now+=15000000;omni::phone_wifi_poll(now);omni::phone_relay()->poll(true,now);}
        const auto s=omni::phone_wifi_status();
        check(pm_calls==1&&joins==0&&!callback,"PM failure never joins or opens relay listener");
        check(s[2]==5&&s[4]==0&&s[7]==0&&s[10]==0&&s[11]==0,"PM failure never advertises IP or readiness");
        check(!omni::phone_relay()->active()&&!omni::phone_relay()->ready(),"PM failure keeps source retired");
        std::printf("PASS: %u low-latency configuration failure checks\n",checks);return 0;
    }
    check(joins==1,"asynchronous join begins once");omni::phone_wifi_poll(++now);
    check(omni::phone_wifi_status()[2]==4,"down reported accurately");
    link=3;cyw43_state.netif[0].address={{192,0,2,9}};omni::phone_wifi_poll(++now);complete();
    auto status=omni::phone_wifi_status();check(status[1]==1&&status[2]==3&&status[4]==192&&status[7]==9,"read-only live IP status");
    const ip_addr_t a{{192,0,2,1}},b{{192,0,2,2}};
    scx2::Packet hello;hello.session[0]=1;inject(hello,a,5000);
    check(sent.back().packet.hid_ready&&ip_addr_cmp(&sent.back().address,&a),"HELLO reply uses copied source address");
    auto state=sent.back().packet;state.kind=scx2::Kind::state;state.hid_ready=false;state.sequence=1;
    inject(state,a,5000);complete();check(sent.back().packet.kind==scx2::Kind::ack,"actual completion ACK goes to pinned sender");
    auto stop=state;stop.kind=scx2::Kind::stop;stop.sequence=2;inject(stop,a,5000);
    hello.session[0]=2;inject(hello,b,6000);check(!sent.back().packet.hid_ready&&ip_addr_cmp(&sent.back().address,&b),"unavailable offer replies to other endpoint");
    complete();check(sent.back().packet.kind==scx2::Kind::stopped&&ip_addr_cmp(&sent.back().address,&a)&&sent.back().port==5000,"unavailable HELLO cannot steal delayed STOPPED");
    link=0;omni::phone_wifi_poll(++now);status=omni::phone_wifi_status();
    check(status[2]==4&&status[4]==0&&status[7]==0&&!omni::phone_relay()->active(),"link loss clears diagnostics and phone source");
    check(joins==1,"loss does not spin asynchronous reconnect");now+=15000000;omni::phone_wifi_poll(now);check(joins==2,"bounded asynchronous Wi-Fi-only retry");
    std::printf("PASS: %u production Wi-Fi adapter checks (including pbuf address lifetime)\n",checks);
}
