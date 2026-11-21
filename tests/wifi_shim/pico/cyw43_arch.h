#pragma once
#include "lwip/netif.h"
constexpr int CYW43_ITF_STA=0, CYW43_LINK_DOWN=0, CYW43_LINK_UP=3;
constexpr int CYW43_AUTH_WPA2_AES_PSK=1, CYW43_COUNTRY_USA=0;
constexpr unsigned CYW43_NONE_PM=0x10;
struct WifiState { struct netif netif[1]; };
extern WifiState cyw43_state;
int cyw43_arch_init_with_country(int);
void cyw43_arch_lwip_begin();
void cyw43_arch_lwip_end();
void cyw43_arch_enable_sta_mode();
int cyw43_wifi_pm(WifiState*,std::uint32_t);
int cyw43_arch_wifi_connect_async(const char*,const char*,int);
int cyw43_tcpip_link_status(WifiState*,int);
