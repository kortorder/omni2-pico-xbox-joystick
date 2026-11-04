// The actual Omni host state machine against a fake HCI/HID/TLV transport.
// Compile against the same pinned upstream event layouts as the board.
#include "omni_bluetooth.hpp"
#include "btstack_event.h"
#include "classic/hid_host.h"
#include "hci.h"
#include "l2cap.h"
#include "btstack_tlv.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
namespace {
using Bytes = std::vector<std::uint8_t>;
using Address = std::array<std::uint8_t, 6>;
const Address target = {1, 2, 3, 4, 5, 6}, other = {7, 8, 9, 10, 11, 12};
Bytes descriptor = {5, 1,    9, 2,    0xa1, 1,    5,    9,    0x19, 1,    0x29, 3,    0x15, 0, 0x25,
                    1, 0x75, 1, 0x95, 3,    0x81, 2,    0x75, 5,    0x95, 1,    0x81, 1,    5, 1,
                    9, 0x30, 9, 0x31, 0x15, 0x81, 0x25, 127,  0x75, 8,    0x95, 2,    0x81, 6, 0xc0};
btstack_packet_handler_t handler{};
unsigned checks = 0, connects = 0, scans = 0, drops = 0, declines = 0, discs = 0, acls = 0, moves = 0,
         neutrals = 0;
bool key = false, check_fixed_input = true;
struct Decoded {
    int x, y, wheel, buttons;
};
Decoded last_decoded{};
gap_security_level_t level = LEVEL_0;
Bytes record, provisional_record;
bool require_secure_sdp = false;
std::uint8_t floor_requirement = 0, controller_floor = 0;
link_key_type_t enrolled_type = UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192;
int (*accept_acl)(bd_addr_t, hci_link_type_t) = nullptr;
std::uint8_t encryption_bytes = 16;
std::string pin;
Address last{};
void check(bool condition, const char *name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        std::exit(1);
    }
    ++checks;
}
void emit(Bytes p) {
    p[1] = static_cast<std::uint8_t>(p.size() - 2);
    handler(HCI_EVENT_PACKET, 0, p.data(), p.size());
}
void address(Bytes &p, unsigned i, const Address &a) {
    std::reverse_copy(a.begin(), a.end(), p.begin() + i);
}
void find(const char *name, const Address &a, unsigned cod) {
    auto n = std::strlen(name);
    Bytes p(27 + n);
    p[0] = GAP_EVENT_INQUIRY_RESULT;
    address(p, 2, a);
    p[9] = cod;
    p[10] = cod >> 8;
    p[25] = 1;
    p[26] = n;
    std::memcpy(p.data() + 27, name, n);
    emit(p);
}
void incoming(const Address &a) {
    Bytes p(14);
    p[0] = HCI_EVENT_HID_META;
    p[2] = HID_SUBEVENT_INCOMING_CONNECTION;
    p[3] = 8;
    address(p, 5, a);
    emit(p);
}
void opened(int id, bool incoming = false) {
    Bytes p(15);
    p[0] = HCI_EVENT_HID_META;
    p[2] = HID_SUBEVENT_CONNECTION_OPENED;
    p[3] = id;
    address(p, 6, target);
    p[12] = 33;
    p[14] = incoming;
    emit(p);
}
void input(int id) {
    emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_REPORT, (std::uint8_t)id, 0, 4, 0, 0xa1, 1, 5, 0xff});
}
void persisted() {
    record = {1, 1};
    record.insert(record.end(), target.begin(), target.end());
}
void start() {
    check(omni::bt_start({[](auto x, auto y, auto wheel, auto b) {
                              last_decoded = {x, y, wheel, b};
                              if (check_fixed_input)
                                  check(x == 5 && y == -1 && wheel == 0 && b == 1,
                                        "mouse report values");
                              ++moves;
                          },
                          [] { ++neutrals; }}),
          "start HCI");
    emit({BTSTACK_EVENT_STATE, 1, HCI_STATE_WORKING});
    omni::bt_tick(1);
}
void choose() {
    find("R-net Mouse 1", other, 0x0580);
    find("Mouse 2", other, 0x0580);
    find("R-net Mouse 2", target, 0x0580);
    emit({GAP_EVENT_INQUIRY_COMPLETE, 1, 0});
    check(last == target && connects == 1, "preferred slot wins over generic and slot 1");
}
} // namespace
std::uint16_t little_endian_read_16(const std::uint8_t *p, int i) { return p[i] | (p[i + 1] << 8); }
std::uint32_t little_endian_read_24(const std::uint8_t *p, int i) {
    return little_endian_read_16(p, i) | (p[i + 2] << 16);
}
void reverse_bytes(const std::uint8_t *p, std::uint8_t *dest, int n) {
    std::reverse_copy(p, p + n, dest);
}
void l2cap_init() { require_secure_sdp = false; }
void l2cap_require_security_level_2_for_outgoing_sdp() { require_secure_sdp = true; }
std::uint8_t gap_encryption_key_size(hci_con_handle_t) { return encryption_bytes; }
void gap_set_security_level(gap_security_level_t) {}
void gap_set_bondable_mode(int) {}
void gap_ssp_set_io_capability(int) {}
void gap_ssp_set_authentication_requirement(int) {}
void gap_ssp_set_auto_accept(int) {}
void gap_connectable_control(std::uint8_t) {}
void gap_discoverable_control(std::uint8_t) {}
void hci_set_inquiry_mode(inquiry_mode_t) {}
int hci_power_control(HCI_POWER_MODE) {
    controller_floor = floor_requirement;
    return 0;
}
void gap_register_classic_connection_filter(int (*filter)(bd_addr_t, hci_link_type_t)) {
    accept_acl = filter;
}
void gap_set_required_encryption_key_size(std::uint8_t bytes) { floor_requirement = bytes; }
void hci_add_event_handler(btstack_packet_callback_registration_t *c) { handler = c->callback; }
void hid_host_init(std::uint8_t *, std::uint16_t) {}
void hid_host_register_packet_handler(btstack_packet_handler_t h) { handler = h; }
std::uint8_t hid_host_connect(bd_addr_t a, hid_protocol_mode_t m, std::uint16_t *id) {
    check(m == HID_PROTOCOL_MODE_REPORT, "report mode only");
    check(require_secure_sdp, "LEVEL_2 required on SDP before starting HID/SDP");
    std::copy_n(a, 6, last.begin());
    ++connects;
    *id = 7;
    return 0;
}
std::uint8_t hid_host_accept_connection(std::uint16_t, hid_protocol_mode_t) { return 0; }
std::uint8_t hid_host_decline_connection(std::uint16_t) {
    ++declines;
    return 0;
}
void hid_host_disconnect(std::uint16_t) { ++discs; }
void hci_disconnect_all() { ++acls; }
int gap_inquiry_start(std::uint8_t) {
    ++scans;
    return 0;
}
int gap_inquiry_stop() { return 0; }
int gap_remote_name_request(const bd_addr_t, std::uint8_t, std::uint16_t) { return 1; }
bool gap_get_link_key_for_bd_addr(bd_addr_t addr, link_key_t material, link_key_type_t *type) {
    if (!key || !std::equal(target.begin(), target.end(), addr))
        return false;
    std::fill_n(material, 16, 0x22);
    *type = enrolled_type;
    return true;
}
void gap_drop_link_key_for_bd_addr(bd_addr_t) {
    ++drops;
    key = false;
}
int gap_pin_code_response(const bd_addr_t, const char *s) {
    pin = s;
    return 0;
}
int gap_pin_code_negative(bd_addr_t) {
    pin = "denied";
    return 0;
}
int gap_ssp_confirmation_response(const bd_addr_t) { return 0; }
int gap_ssp_confirmation_negative(const bd_addr_t) { return 0; }
gap_security_level_t gap_security_level(hci_con_handle_t) { return level; }
void gap_request_security_level(hci_con_handle_t, gap_security_level_t) {}
const std::uint8_t *hid_descriptor_storage_get_descriptor_data(std::uint16_t) {
    return descriptor.data();
}
std::uint16_t hid_descriptor_storage_get_descriptor_len(std::uint16_t) { return descriptor.size(); }
void btstack_tlv_get_instance(const btstack_tlv_t **impl, void **ctx) {
    static const btstack_tlv_t storage = {
        [](void *, std::uint32_t tag, std::uint8_t *b, std::uint32_t n) {
            const auto &data = tag == 0x4f4d4e31 ? record : provisional_record;
            if (b)
                std::copy_n(data.begin(), std::min<std::size_t>(n, data.size()), b);
            return static_cast<int>(data.size());
        },
        [](void *, std::uint32_t tag, const std::uint8_t *b, std::uint32_t n) {
            (tag == 0x4f4d4e31 ? record : provisional_record).assign(b, b + n);
            return 0;
        },
        [](void *, std::uint32_t tag) { (tag == 0x4f4d4e31 ? record : provisional_record).clear(); }};
    *impl = &storage;
    *ctx = nullptr;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const std::string mode = argv[1];
    using omni::BtState;
    if (mode == "missing") {
        persisted();
        start();
        check(omni::bt_state() == BtState::fault && scans == 0 && connects == 0 && !record.empty(),
              "missing link key preserves selected peer");
        omni::bt_forget();
        omni::bt_pair();
        omni::bt_tick(2);
        check(record.empty() && scans == 1, "explicit forget then pair reopens commissioning");
    } else if (mode == "inbound") {
        persisted();
        key = true;
        start();
        check(connects == 1 && last == target, "reboot reconnects saved peer only");
        emit(
            {HCI_EVENT_HID_META, 0, HID_SUBEVENT_CONNECTION_OPENED, 7, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        incoming(other);
        check(declines == 1, "stranger inbound refused");
        incoming(target);
        opened(8, true);
        level = LEVEL_2;
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 8, 0, 0});
        input(8);
        check(moves == 0 && omni::bt_state() == BtState::connecting,
              "descriptor before incoming SET_PROTOCOL cannot drive");
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_SET_PROTOCOL_RESPONSE, 8, 0, 0,
              HID_PROTOCOL_MODE_REPORT});
        input(8);
        check(moves == 1 && omni::bt_state() == BtState::connected,
              "successful incoming protocol selection releases report gate");
    } else if (mode == "window") {
        start();
        omni::bt_tick(300001);
        check(omni::bt_state() == BtState::unpaired, "5 minute commissioning deadline");
        find("R-net Mouse 2", target, 0x0580);
        emit({GAP_EVENT_INQUIRY_COMPLETE, 1, 0});
        check(connects == 0, "stale inquiry cannot bind");
        omni::bt_pair();
        omni::bt_tick(300002);
        check(scans == 2, "explicit re-open");
    } else if (mode == "capture" || mode == "badcapture" || mode == "capture_reset" ||
               mode == "capture_dead_acl") {
        start();
        if (mode == "capture_reset")
            omni::bt_tick(285000);
        choose();
        Address out{};
        out.fill(255);
        check(!omni::bt_copy_short_key_candidate(out) && out == Address{},
              "no witness from selected name alone");
        Bytes acl(13);
        acl[0] = HCI_EVENT_CONNECTION_COMPLETE;
        acl[3] = 33;
        address(acl, 5, target);
        emit(acl);
        Bytes confirm(12);
        confirm[0] = HCI_EVENT_USER_CONFIRMATION_REQUEST;
        address(confirm, 2, mode == "badcapture" ? other : target);
        emit(confirm);
        key = true;
        level = LEVEL_0;
        encryption_bytes = 7;
        Bytes noted(25);
        noted[0] = HCI_EVENT_LINK_KEY_NOTIFICATION;
        address(noted, 2, target);
        noted[8] = 1;
        noted[24] = UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192;
        emit(noted);
        emit({HCI_EVENT_AUTHENTICATION_COMPLETE, 3, 0, 33, 0});
        emit({GAP_EVENT_SECURITY_LEVEL, 4, 34, 0, LEVEL_0, 0});
        check(!omni::bt_copy_short_key_candidate(out), "unrelated handle cannot certify pin");
        // A HID refusal may reach profile listeners before this ACL's raw GAP event.
        Bytes failed(15);
        failed[0] = HCI_EVENT_HID_META;
        failed[2] = HID_SUBEVENT_CONNECTION_OPENED;
        failed[3] = 7;
        failed[5] = 0x66;
        address(failed, 6, target);
        emit(failed);
        if (mode == "capture_dead_acl")
            emit({HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 33, 0, 0x13});
        emit({GAP_EVENT_SECURITY_LEVEL, 4, 33, 0, LEVEL_0, 0});
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
        if (mode == "capture" || mode == "capture_reset")
            check(omni::bt_copy_short_key_candidate(out) && out == target,
                  "fresh authenticated SSP, matching short encrypted ACL exports canonical pin");
        else
            check(!omni::bt_copy_short_key_candidate(out) && out == Address{},
                  "confirmation for unrelated peer cannot certify pin");
#else
        check(!omni::bt_copy_short_key_candidate(out) && out == Address{},
              "non-observation build cannot export a peer");
#endif
        check(!moves && record.empty() && provisional_record.empty(),
              "strict observation does not authorize output or save a weak bond");
        if (mode == "capture_reset") {
            omni::bt_tick(300001);
            check(!omni::bt_copy_short_key_candidate(out),
                  "commission timeout clears recent witness before its own 60s TTL");
            omni::bt_pair();
            out.fill(255);
            check(!omni::bt_copy_short_key_candidate(out) && out == Address{},
                  "opening a new window cannot revive a previous witness");
        } else {
            omni::bt_tick(60001);
            out.fill(255);
            check(!omni::bt_copy_short_key_candidate(out) && out == Address{},
                  "60-second witness expiry zeros output");
        }
    } else if (mode == "telemetry_unavailable") {
        start();
        std::array<std::uint8_t, 64> stats{};
        stats.fill(255);
        check(!omni::bt_copy_motion_snapshot(stats) &&
                  std::all_of(stats.begin(), stats.end(), [](auto b) { return b == 0; }),
              "other variants expose zero/no inbound data");
    } else if (mode == "telemetry" || mode == "telemetry_security" || mode == "telemetry_framing") {
        check_fixed_input = false;
        descriptor = {5,    1,    9,    2,    0xa1, 1,    0x85, 2,    5, 9,    0x19, 1,    0x29,
                      3,    0x15, 0,    0x25, 1,    0x75, 1,    0x95, 3, 0x81, 2,    0x75, 5,
                      0x95, 1,    0x81, 1,    5,    1,    9,    0x30, 9, 0x31, 9,    0x38, 0x15,
                      0x81, 0x25, 0x7f, 0x75, 8,    0x95, 3,    0x81, 6, 0xc0};
        std::array<std::uint8_t, 64> stats{};
        auto get = [&]() {
            check(omni::bt_copy_motion_snapshot(stats) && stats[0] == 1 && stats[1] == 64,
                  "stable initialized private snapshot");
        };
        auto u32 = [&](unsigned i) {
            return std::uint32_t(stats[i]) | (std::uint32_t(stats[i + 1]) << 8) |
                   (std::uint32_t(stats[i + 2]) << 16) | (std::uint32_t(stats[i + 3]) << 24);
        };
        auto s16 = [&](unsigned i) {
            return std::int16_t(std::uint16_t(stats[i]) | (std::uint16_t(stats[i + 1]) << 8));
        };
        auto report = [&](int id, Bytes data) {
            Bytes p = {HCI_EVENT_HID_META,
                       0,
                       HID_SUBEVENT_REPORT,
                       static_cast<std::uint8_t>(id),
                       0,
                       static_cast<std::uint8_t>(data.size()),
                       0};
            p.insert(p.end(), data.begin(), data.end());
            emit(p);
        };
        start();
        get();
        check(u32(4) == 0 && stats[2] == 0, "readable empty baseline");
        choose();
        report(7, {0xa1, 2, 1, 0xfc, 6, 0});
        get();
        check(!u32(4) && !moves, "unverified connection contributes no telemetry");
        key = true;
        level = LEVEL_2;
        encryption_bytes = 7;
        opened(7);
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0});
        if (mode == "telemetry_security") {
            level = LEVEL_0;
            report(7, {0xa1, 2, 1, 0xfc, 6, 0});
            get();
            check(!u32(4) && !moves, "unverified encryption cannot update raw counters");
        } else if (mode == "telemetry_framing") {
            emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_REPORT, 7, 0, 99, 0, 0xa1, 2});
            get();
            check(u32(4) == 1 && u32(16) == 1 && stats[2] == 3 && stats[3] == 2 && !moves,
                  "bad declared length counted without reading past actual bytes");
        } else {
            report(7, {0xa1, 2, 1, 0xfc, 6, 0});
            get();
            check(u32(4) == 1 && u32(8) == 1 && u32(20) == 1 && u32(32) == 1 && u32(44) == 1 && !u32(48),
                  "left/down and press edges counted independently");
            check(last_decoded.x == -4 && last_decoded.y == 6 && last_decoded.buttons == 1 && moves == 1,
                  "same decoded values still reach mapper");
            report(7, {0xa1, 2, 1, 0, 0, 0xff});
            report(7, {0xa1, 2, 0, 0, 0, 2});
            report(7, {0xa1, 2, 0, 0, 0, 0});
            get();
            check(u32(36) == 1 && u32(40) == 1 && u32(44) == 1 && u32(48) == 1 && u32(52) == 1,
                  "wheel signs, release, true zero-only distinguished");
            check(s16(56) == -4 && s16(58) == 6 && s16(60) == 2 && stats[62] == 1 && stats[63] == 2,
                  "latest nonzero motion persists across idle reports");
            report(7, {0xa1, 2, 0, 2, 0xfd, 0});
            report(7, {0xa1, 3, 0});
            report(99, {0xa1, 2, 0, 0x81, 0x7f, 0});
            report(7, {0xa2, 2, 0, 0x81, 0x7f, 0});
            get();
            check(u32(4) == 6 && u32(8) == 5 && u32(12) == 1 && !u32(16) && stats[2] == 2 &&
                      stats[3] == 3,
                  "unrelated ID separate, stale CID/non-input ignored");
            check(u32(24) == 1 && u32(28) == 1 && s16(56) == 2 && s16(58) == -3 && moves == 5,
                  "positive X/negative Y counts and ordinary callbacks preserved");
            report(7, {0xa1, 2, 1});
            get();
            check(u32(4) == 7 && u32(16) == 1 && stats[2] == 3 && stats[3] == 2 && discs && moves == 5,
                  "malformed report counted then rejected without movement");
            omni::bt_forget();
            get();
            check(!u32(4) && !u32(8) && !stats[62],
                  "Forget resets boot-local counters and retained values");
        }
    } else if (mode == "pinned" || mode == "pinned_weak" || mode == "pinned_debug") {
        start();
        check(controller_floor == 7 && floor_requirement == 16 && accept_acl,
              "only a one-peer controller may initialize with 7, idle host stays 16");
        Address a = target, b = other;
        check(!accept_acl(b.data(), HCI_LINK_TYPE_ACL) && !accept_acl(a.data(), HCI_LINK_TYPE_SCO) &&
                  !accept_acl(a.data(), HCI_LINK_TYPE_ACL),
              "unsolicited ACL and all SCO rejected before selecting pin");
        find("R-net Mouse 2", other, 0x0580);
        emit({GAP_EVENT_INQUIRY_COMPLETE, 1, 0});
        check(connects == 0 && floor_requirement == 16,
              "lookalike exact name on different address cannot lower floor");
        omni::bt_tick(1002);
        find("R-net Mouse 1", target, 0x0580);
        emit({GAP_EVENT_INQUIRY_COMPLETE, 1, 0});
        check(connects == 0, "pinned address cannot bypass Mouse 2 name rule");
        omni::bt_tick(2003);
        choose();
        check(floor_requirement == 7 && accept_acl(a.data(), HCI_LINK_TYPE_ACL) &&
                  !accept_acl(b.data(), HCI_LINK_TYPE_ACL),
              "only selected pinned ACL accepted during reduced host floor");
        Bytes req(8);
        req[0] = HCI_EVENT_PIN_CODE_REQUEST;
        address(req, 2, target);
        emit(req);
        check(pin == "denied", "no fixed PIN downgrade on personal variant");
        key = true;
        level = LEVEL_2;
        encryption_bytes = mode == "pinned_weak" ? 6 : 7;
        if (mode == "pinned_debug")
            enrolled_type = DEBUG_COMBINATION_KEY;
        opened(7);
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0});
        input(7);
        if (mode == "pinned") {
            check(omni::bt_state() == BtState::connected && moves == 1,
                  "authenticated SSP pinned mouse with 7-byte encryption reaches report path");
            encryption_bytes = 6;
            input(7);
            check(moves == 1 && discs && floor_requirement == 16,
                  "subminimum link recheck rejects new input and restores strict");
        } else
            check(record.empty() && !moves && omni::bt_state() != BtState::connected,
                  "subminimum or debug/legacy key cannot promote or drive");
        omni::bt_forget();
        check(floor_requirement == 16, "explicit forget restores strict host floor");
    } else if (mode == "pinned_saved") {
        persisted();
        key = true;
        start();
        Address a = target, b = other;
        check(controller_floor == 7 && connects == 1 && last == target &&
                  !accept_acl(b.data(), HCI_LINK_TYPE_ACL),
              "saved reconnect limited to exact address");
        input(7);
        check(!moves, "reconnect must validate encrypted descriptor");
        opened(7);
        level = LEVEL_2;
        encryption_bytes = 7;
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0});
        input(7);
        check(moves == 1, "saved verified short link can deliver input");
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_CONNECTION_CLOSED, 7, 0});
        check(floor_requirement == 16, "HID close restores strict threshold");
        incoming(other);
        check(declines && moves == 1, "unselected HID inbound rejected");
        check(accept_acl(a.data(), HCI_LINK_TYPE_ACL) && floor_requirement == 7,
              "only validated saved peer admitted for incoming reconnect");
        incoming(target);
        opened(8, true);
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 8, 0, 0});
        input(8);
        check(moves == 1, "incoming waits for SET_PROTOCOL too");
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_SET_PROTOCOL_RESPONSE, 8, 0, 0,
              HID_PROTOCOL_MODE_REPORT});
        input(8);
        check(moves == 2, "pinned incoming confirmed report mode");
        omni::bt_forget();
        check(floor_requirement == 16 && record.empty() && !key,
              "forget clears only selected state and restores strict");
    } else if (mode == "strict_floor") {
        start();
        check(controller_floor == 16 && floor_requirement == 16 && !accept_acl,
              "normal and capture initialize 16 without compatibility ACL filter");
        choose();
        check(floor_requirement == 16, "selected normal/capture peer keeps standard floor");
    } else if (mode == "diagnostic") {
        using omni::BtStage;
        const auto expect = [](BtStage stage, std::uint8_t detail, const char *label) {
            auto d = omni::bt_diagnostic();
            check(d.stage == static_cast<std::uint8_t>(stage) && d.detail == detail, label);
        };
        start();
        expect(BtStage::inquiry_started, 1, "scan count without peer identity");
        choose();
        expect(BtStage::hid_connect_started, 1, "selection begins first SDP/HID attempt");
        // A profile listener can receive its failure before our raw HCI listener
        // for the very same event. The lower layer may provide the better reason.
        Bytes failed(15);
        failed[0] = HCI_EVENT_HID_META;
        failed[2] = HID_SUBEVENT_CONNECTION_OPENED;
        failed[3] = 7;
        failed[5] = 4;
        address(failed, 6, target);
        emit(failed);
        expect(BtStage::hid_open_failed, 4, "HID failure captured");
        Bytes acl(13);
        acl[0] = HCI_EVENT_CONNECTION_COMPLETE;
        acl[2] = 4;
        address(acl, 5, target);
        emit(acl);
        expect(BtStage::acl_failed, 4,
               "actual HCI page timeout supersedes profile failure even after CID close");
        omni::bt_tick(1002);
        find("R-net Mouse 2", target, 0x0580);
        emit({GAP_EVENT_INQUIRY_COMPLETE, 1, 0});
        expect(BtStage::acl_failed, 4, "new discovery and connection leave failure readable");
        acl[2] = 0;
        acl[3] = 33;
        emit(acl);
        emit({HCI_EVENT_AUTHENTICATION_COMPLETE, 3, 5, 33, 0});
        expect(BtStage::authentication_failed, 5,
               "authentication failure from new attempt can replace previous failure");
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_CONNECTION_CLOSED, 7, 0});
        emit({HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 33, 0, 0x13});
        expect(BtStage::authentication_failed, 5,
               "ordinary close cannot overwrite authentication reason");
        omni::bt_pair();
        expect(BtStage::none, 0, "explicit new commissioning clears failure");
    } else if (mode == "diagnostic_timeout") {
        start();
        choose();
        opened(7);
        key = true;
        omni::bt_tick(20002);
        const auto d = omni::bt_diagnostic();
        check(d.stage == 0x8a && d.detail == 19,
              "timeout flags actual open, key, protocol; no descriptor or encryption");
    } else if (mode == "security_status" || mode == "key_length") {
        start();
        choose();
        Bytes acl(13);
        acl[0] = HCI_EVENT_CONNECTION_COMPLETE;
        acl[3] = 33;
        address(acl, 5, target);
        emit(acl);
        encryption_bytes = 7;
        emit({GAP_EVENT_SECURITY_LEVEL, 4, 33, 0, LEVEL_0,
              static_cast<std::uint8_t>(mode == "security_status" ? 5 : 0)});
        Bytes failed(15);
        failed[0] = HCI_EVENT_HID_META;
        failed[2] = HID_SUBEVENT_CONNECTION_OPENED;
        failed[3] = 7;
        failed[5] = 0x66;
        address(failed, 6, target);
        emit(failed);
        emit({HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 33, 0, 0x13});
        omni::bt_tick(1002);
        auto d = omni::bt_diagnostic();
        check(d.stage == (mode == "security_status" ? 0x90 : 0x91) &&
                  d.detail == (mode == "security_status" ? 5 : 7),
              "specific GAP failure/key length survives 0x66/close/retry");
        check(record.empty() && !moves, "security refusal cannot accept or drive");
    } else if (mode == "provisional" || mode == "expiry" || mode == "forget_provisional" ||
               mode == "bad_descriptor") {
        start();
        choose();
        Bytes acl(13);
        acl[0] = HCI_EVENT_CONNECTION_COMPLETE;
        acl[3] = 33;
        address(acl, 5, target);
        emit(acl);
        key = true;
        Bytes notified(25);
        notified[0] = HCI_EVENT_LINK_KEY_NOTIFICATION;
        address(notified, 2, target);
        notified[8] = 1;
        emit(notified);
        check(record.empty() && provisional_record.empty(),
              "key by itself is not authenticated commissioning");
        level = LEVEL_2;
        emit({GAP_EVENT_SECURITY_LEVEL, 4, 33, 0, LEVEL_2, 0});
        check(provisional_record.size() == 8 && record.empty() && !moves,
              "encrypted newly generated key marked provisionally only");
        if (mode == "bad_descriptor") {
            opened(7);
            emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0x11});
            check(!key && provisional_record.empty() && record.empty(),
                  "unusable HID descriptor clears provisional key");
        } else {
            Bytes failed(15);
            failed[0] = HCI_EVENT_HID_META;
            failed[2] = HID_SUBEVENT_CONNECTION_OPENED;
            failed[3] = 7;
            failed[5] = 0x66;
            address(failed, 6, target);
            emit(failed);
            check(key && provisional_record.size() == 8 && record.empty(),
                  "failed HID open preserves encrypted provisional key");
            if (mode == "expiry") {
                omni::bt_tick(300002);
                check(!key && provisional_record.empty() && record.empty() &&
                          omni::bt_state() == BtState::unpaired,
                      "expiry removes only provisional claim");
            } else if (mode == "forget_provisional") {
                omni::bt_forget();
                check(!key && provisional_record.empty() && record.empty(),
                      "explicit forget removes provisional key");
            } else {
                omni::bt_tick(3002);
                check(connects == 2 && last == target && scans == 1,
                      "next retry directly addresses only provisional peer");
                Bytes req(8);
                req[0] = HCI_EVENT_PIN_CODE_REQUEST;
                address(req, 2, target);
                emit(req);
                check(pin == "denied", "provisional retry cannot silently replace the link key");
                opened(7);
                emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0});
                input(7);
                check(key && moves == 1 && record.size() == 8 && provisional_record.empty(),
                      "verified mouse promotes selection while retaining key");
            }
        }
    } else if (mode == "interrupted" || mode == "promotion_interrupted") {
        persisted();
        provisional_record = record;
        if (mode == "interrupted")
            record.clear();
        key = true;
        start();
        if (mode == "interrupted")
            check(!key && provisional_record.empty() && record.empty() && connects == 0 && scans == 1,
                  "expired-on-reboot provisional cannot reconnect");
        else
            check(key && !record.empty() && provisional_record.empty() && connects == 1 &&
                      last == target,
                  "atomic selected promotion survives interrupted marker removal");
    } else if (mode == "timeout") {
        start();
        choose();
        omni::bt_tick(20002);
        check(discs && acls && !omni::bt_recovery_requested(),
              "timeout fences HID and ACL before grace");
        opened(7);
        input(7);
        check(!moves && record.empty(), "late opened/report cannot bind abandoned CID");
        omni::bt_tick(23003);
        check(omni::bt_recovery_requested() && omni::bt_state() == BtState::fault,
              "unclosed SDP requests bounded recovery");
    } else if (mode == "pair") {
        start();
        choose();
        Bytes req(8);
        req[0] = HCI_EVENT_PIN_CODE_REQUEST;
        address(req, 2, other);
        emit(req);
        check(pin == "denied", "unrelated PIN refused");
        address(req, 2, target);
        emit(req);
        check(pin == "1234", "selected Mouse 2 PIN");
        opened(7);
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_DESCRIPTOR_AVAILABLE, 7, 0, 0});
        input(7);
        check(!moves && record.empty(), "descriptor alone cannot store or drive");
        key = true;
        omni::bt_tick(10);
        check(record.empty(), "link key alone cannot drive cleartext");
        level = LEVEL_2;
        omni::bt_tick(11);
        input(7);
        check(record.size() == 8 && moves == 1, "encrypted mouse saved and accepted");
        emit({HCI_EVENT_HID_META, 0, HID_SUBEVENT_CONNECTION_CLOSED, 7, 0});
        input(7);
        check(moves == 1 && neutrals >= 2, "close neutralizes and stale report ignored");
        omni::bt_tick(2012);
        check(connects == 2 && last == target && drops == 0, "bonded reconnect no key deletion");
    } else
        return 2;
    std::cout << checks << " Bluetooth host assertions passed (" << mode << ")\n";
}
