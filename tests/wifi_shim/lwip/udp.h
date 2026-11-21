#pragma once
#include <cstdint>
using u16_t = std::uint16_t;
struct ip_addr_t { std::uint8_t bytes[4]{}; };
inline bool ip_addr_cmp(const ip_addr_t *a,const ip_addr_t *b) {
    for(unsigned i=0;i<4;++i) if(a->bytes[i]!=b->bytes[i]) return false;
    return true;
}
#define IP_IS_V4(a) ((a) != nullptr)
constexpr int IPADDR_TYPE_V4=0, PBUF_TRANSPORT=0, PBUF_RAM=0, ERR_OK=0;
constexpr const ip_addr_t *IP_ANY_TYPE=nullptr;
struct udp_pcb {};
struct pbuf { std::uint16_t tot_len=0; std::uint8_t data[80]{}; ip_addr_t source{}; };
using udp_recv_fn=void(*)(void*,udp_pcb*,pbuf*,const ip_addr_t*,u16_t);
udp_pcb *udp_new_ip_type(int);
int udp_bind(udp_pcb*,const ip_addr_t*,u16_t);
void udp_recv(udp_pcb*,udp_recv_fn,void*);
void udp_remove(udp_pcb*);
pbuf *pbuf_alloc(int,std::uint16_t,int);
int pbuf_take(pbuf*,const void*,std::uint16_t);
std::uint16_t pbuf_copy_partial(pbuf*,void*,std::uint16_t,std::uint16_t);
void pbuf_free(pbuf*);
int udp_sendto(udp_pcb*,pbuf*,const ip_addr_t*,u16_t);
