#pragma once
#include "udp.h"
struct netif { ip_addr_t address{}; };
inline const ip_addr_t *netif_ip4_addr(const netif *n) {return &n->address;}
inline auto ip4_addr1(const ip_addr_t *a) {return a->bytes[0];}
inline auto ip4_addr2(const ip_addr_t *a) {return a->bytes[1];}
inline auto ip4_addr3(const ip_addr_t *a) {return a->bytes[2];}
inline auto ip4_addr4(const ip_addr_t *a) {return a->bytes[3];}
