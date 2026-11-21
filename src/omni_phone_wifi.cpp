#include "omni_phone_wifi.hpp"
#include "omni_phone_relay.hpp"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include OMNI_WIFI_CONFIG_HEADER
#include <optional>

namespace {
constexpr std::uint16_t port = 42737;
std::optional<omni::PhoneRelay> relay;
udp_pcb *socket = nullptr;
ip_addr_t peer{};
std::uint16_t peer_port = 0;
std::uint64_t next_join = 0;
std::uint8_t wifi_state = 1;
bool joined = false;
bool low_latency_ready = false;
scx2::Token random_token(void *) {
    scx2::Token token{};
    for (unsigned i = 0; i < token.size(); i += 8) {
        const auto value = get_rand_64();
        for (unsigned j = 0; j < 8; ++j) token[i + j] = std::uint8_t(value >> (8 * j));
    }
    return token;
}
void send_to(const scx2::Packet &packet, const ip_addr_t &address, std::uint16_t target_port) {
    const auto bytes = scx2::encode(packet);
    if (!socket || !target_port || !joined || !bytes) return;
    cyw43_arch_lwip_begin();
    if (auto *buffer = pbuf_alloc(PBUF_TRANSPORT, bytes->size, PBUF_RAM)) {
        if (pbuf_take(buffer, bytes->bytes.data(), bytes->size) == ERR_OK)
            (void)udp_sendto(socket, buffer, &address, target_port);
        pbuf_free(buffer);
    }
    cyw43_arch_lwip_end();
}
void send(const scx2::Packet &packet, void *) { send_to(packet, peer, peer_port); }
void receive(void *, udp_pcb *, pbuf *buffer, const ip_addr_t *address, u16_t source_port) {
    if (!buffer) return;
    const bool ipv4 = address && IP_IS_V4(address);
    const ip_addr_t source = ipv4 ? *address : ip_addr_t{};
    std::array<std::uint8_t, 80> bytes{};
    const auto count = buffer->tot_len;
    const bool copied = count <= bytes.size() && pbuf_copy_partial(buffer, bytes.data(), count, 0) == count;
    pbuf_free(buffer);
    if (!copied || !relay || !source_port || !ipv4) return;
    if (relay->active() && (!ip_addr_cmp(&peer, &source) || source_port != peer_port)) return;
    const auto packet = scx2::decode(bytes.data(), count);
    if (!packet) return;
    const auto reply = relay->receive(*packet, time_us_64());
    if (reply) {
        // An unavailable offer must not steal the endpoint of a pending STOPPED.
        if (reply->kind == scx2::Kind::ready && reply->hid_ready) {
            peer = source;
            peer_port = source_port;
        }
        send_to(*reply, source, source_port);
    }
}
void join(std::uint64_t now) {
    next_join = now + 15'000'000;
    wifi_state = cyw43_arch_wifi_connect_async(SC_WIFI_SSID, SC_WIFI_PASSWORD,
                                              CYW43_AUTH_WPA2_AES_PSK) == 0 ? 2 : 5;
}
} // namespace
int omni::phone_wifi_radio_init() { return cyw43_arch_init_with_country(SC_WIFI_COUNTRY); }
void omni::phone_wifi_start() {
    relay.emplace(random_token(nullptr), random_token, nullptr, send);
    relay->network(false, time_us_64());
    phone_relay_attach(&*relay);
    cyw43_arch_enable_sta_mode();
    // The USB-powered controller needs prompt packets, rather than PM2 beacon waits.
    // The SDK requires this call after the Wi-Fi interface has been brought up.
    low_latency_ready = cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM) == 0;
    if (!low_latency_ready) {
        wifi_state = 5;
        return; // Bluetooth/USB still work; do not offer an unconfigured relay link.
    }
    join(time_us_64());
}
void omni::phone_wifi_poll(std::uint64_t now) {
    if (!relay) return;
    if (!low_latency_ready) {
        relay->network(false, now);
        return;
    }
    const auto link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    const bool up = link == CYW43_LINK_UP;
    if (!up) {
        wifi_state = link > CYW43_LINK_DOWN ? 2 : 4;
        relay->network(false, now); // Wi-Fi loss retires only the phone source.
        joined = false;
        if (socket) {
            cyw43_arch_lwip_begin();
            udp_remove(socket);
            cyw43_arch_lwip_end();
            socket = nullptr;
        }
        peer_port = 0;
        if (now >= next_join || next_join - now > 15'000'000) join(now);
        return;
    }
    if (!socket) {
        cyw43_arch_lwip_begin();
        socket = udp_new_ip_type(IPADDR_TYPE_V4);
        if (socket && udp_bind(socket, IP_ANY_TYPE, port) != ERR_OK) {
            udp_remove(socket);
            socket = nullptr;
        }
        if (socket) udp_recv(socket, receive, nullptr);
        cyw43_arch_lwip_end();
    }
    joined = socket != nullptr;
    wifi_state = joined ? 3 : 5;
    relay->network(joined, now);
}
omni::PhoneWifiStatus omni::phone_wifi_status() {
    PhoneWifiStatus out{1, 1, wifi_state, 0}; // version, enabled, status, reserved
    if (joined) {
        const auto *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
        out[4] = ip4_addr1(ip); out[5] = ip4_addr2(ip); out[6] = ip4_addr3(ip); out[7] = ip4_addr4(ip);
    }
    out[8] = std::uint8_t(port >> 8); out[9] = std::uint8_t(port);
    out[10] = relay && relay->ready(); out[11] = relay && relay->active();
    return out;
}
