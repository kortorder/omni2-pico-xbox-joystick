#include "omni_bluetooth.hpp"
#include "omni_mouse_report.hpp"
#include "btstack_event.h"
#include "classic/hid_host.h"
#include "hci.h"
#include "l2cap.h"
#include "btstack_tlv.h"
#include <algorithm>
#include <array>
#include <cstring>

#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY && defined(OMNI_SHORT_KEY_COMPAT) &&      \
    OMNI_SHORT_KEY_COMPAT
#error "Strict observation and pinned short-key compatibility cannot coexist"
#endif
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
#include "omni_approved_peer.hpp"
#include "omni_motion_counters.hpp"
#ifndef ENABLE_MUTUAL_AUTHENTICATION_FOR_LEGACY_SECURE_CONNECTIONS
#error "Personal legacy compatibility requires mutual authentication"
#endif
static_assert(MAX_NR_HCI_CONNECTIONS == 1, "A process-wide key policy requires exactly one ACL slot");
#endif
namespace omni {
namespace {
constexpr std::uint32_t peer_tag = 0x4f4d4e31; // OMN1, separate from BTstack's link-key records.
constexpr std::uint32_t provisional_tag =
    0x4f4d4e32; // OMN2 marks an interrupted first pairing, never an accepted peer.
constexpr std::uint64_t pair_ms = 300'000, connect_ms = 20'000;
struct Candidate {
    bd_addr_t address{};
    std::uint8_t scan_mode{};
    std::uint16_t clock_offset{};
    std::uint32_t device_class{};
    MouseName name{};
    bool name_checked = false;
};
struct Peer {
    bd_addr_t address{};
    MouseName name{MouseName::unsupported};
};
BtCallbacks callbacks{};
BtState state = BtState::starting;
BtDiagnostic diagnostic{};
std::uint8_t scan_count = 0, attempt_count = 0, scan_results = 0;
unsigned failure_priority = 0;
Peer diagnostic_peer{};
hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
void progress(BtStage stage, std::uint8_t detail = 0) {
    if (diagnostic.stage < 0x80)
        diagnostic = {static_cast<std::uint8_t>(stage), detail};
}
void failure(BtStage stage, std::uint8_t detail = 0) {
    // Preserve an authentication/descriptor reason through the ensuing generic
    // close and discovery retries. A new candidate attempt gets a fresh chance
    // to replace it with its own specific reason.
    const unsigned rank = (stage == BtStage::disconnected || stage == BtStage::teardown_timeout) ? 1
                          : (stage == BtStage::acl_failed || stage == BtStage::authentication_failed ||
                             stage == BtStage::encryption_failed)
                              ? 3
                              : 2;
    const unsigned priority =
        (stage == BtStage::security_request_failed || stage == BtStage::security_below_level2) ? 4
                                                                                               : rank;
    if (priority > failure_priority ||
        (stage == BtStage::disconnected && diagnostic.stage == static_cast<std::uint8_t>(stage) &&
         !diagnostic.detail && detail)) {
        diagnostic = {static_cast<std::uint8_t>(stage), detail};
        failure_priority = priority;
    }
}
void increment(std::uint8_t &count) {
    if (count != 255)
        ++count;
}
Peer saved{}, attempt{}, provisional{};
bool fresh_key_seen = false;
bool working = false, started = false, inquiry = false, opened = false, closing = false,
     descriptor_ready = false;
bool protocol_ready = false, recovery_needed = false;
std::uint64_t close_deadline = 0;
std::uint64_t now_ms = 0, window_end = 0, next_try = 0, deadline = 0;
unsigned failures = 0;
std::array<Candidate, 12> candidates{};
std::size_t found = 0;
int name_query = -1;
std::uint16_t cid = 0;
hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
std::array<std::uint8_t, 2048> descriptor_storage{};
MouseReports reports;
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
MotionCounters motion_counters;
#endif

btstack_packet_callback_registration_t events{};
const btstack_tlv_t *tlv = nullptr;
void *tlv_context = nullptr;

bool known(const Peer &p) { return p.name != MouseName::unsupported; }
bool same(const bd_addr_t a, const bd_addr_t b) { return std::memcmp(a, b, 6) == 0; }
bool approved_address(const bd_addr_t addr) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    static constexpr std::array<std::uint8_t, 6> pin = {OMNI_PINNED_MOUSE2_BYTES};
    return std::equal(pin.begin(), pin.end(), addr);
#else
    (void)addr;
    return true;
#endif
}
[[maybe_unused]] bool ssp_key(unsigned type) {
    return type == UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192 ||
           type == AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192 ||
           type == UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256 ||
           type == AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;
}
bool has_key(const Peer &p) {
    link_key_t key{};
    link_key_type_t type{};
    if (!known(p) || !approved_address(p.address) ||
        !gap_get_link_key_for_bd_addr(const_cast<std::uint8_t *>(p.address), key, &type))
        return false;
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    return ssp_key(type) && std::any_of(key, key + sizeof(key), [](auto v) { return v != 0; });
#else
    return true;
#endif
}
void restore_strict_floor() {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    gap_set_required_encryption_key_size(16);
#endif
}
void allow_approved_floor(const bd_addr_t addr) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    if (approved_address(addr))
        gap_set_required_encryption_key_size(7);
#else
    (void)addr;
#endif
}
bool window() { return working && window_end && now_ms < window_end && !known(saved); }
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
struct Observation {
    Peer pending{}, confirmed{};
    bool ssp{}, authenticated{}, key_seen{}, short_encrypted{};
    std::uint64_t valid_until{};
    void begin(const Peer &p) {
        if (!same(p.address, confirmed.address)) {
            confirmed = {};
            valid_until = 0;
        }
        pending = p;
        ssp = false;
        authenticated = false;
        key_seen = false;
        short_encrypted = false;
    }
    void record() {
        if (!window() || !known(pending) || !ssp || !authenticated || !key_seen || !short_encrypted)
            return;
        confirmed = pending;
        valid_until = now_ms + 60'000;
        pending = {}; // A single exchange cannot continually refresh its own expiration.
    }
} observation;
#endif
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
int accept_classic_connection(bd_addr_t addr, hci_link_type_t type) {
    // Reject unselected inbound ACL/SCO requests before the HCI radio accepts
    // them. The host stack key floor is global; the firmware permits one ACL.
    if (!working || state == BtState::fault || closing || recovery_needed || type != HCI_LINK_TYPE_ACL ||
        !approved_address(addr))
        return 0;
    const bool outgoing_match =
        known(attempt) && state == BtState::connecting && same(addr, attempt.address);
    const bool saved_match = has_key(saved) && same(addr, saved.address);
    const bool provisional_match = window() && has_key(provisional) && same(addr, provisional.address);
    if (!outgoing_match && !saved_match && !provisional_match)
        return 0;
    allow_approved_floor(addr);
    return 1;
}
#endif
void neutral() {
    if (callbacks.neutral)
        callbacks.neutral();
}
void stop_search() {
    if (inquiry)
        gap_inquiry_stop();
    inquiry = false;
    found = 0;
    name_query = -1;
}
void fault() {
    restore_strict_floor();
    state = BtState::fault;
    window_end = 0;
    stop_search();
    neutral();
    gap_set_bondable_mode(0);
}
void reject_link() {
    neutral();
    restore_strict_floor();
    descriptor_ready = false;
    if (cid) {
        if (!closing) {
            closing = true;
            close_deadline = now_ms + 3000;
        }
        if (state != BtState::fault)
            state = known(saved) ? BtState::reconnecting : BtState::unpaired;
        // The pinned HID host cannot cancel an SDP-only connection with no HID
        // channels yet. Also retire its ACL. Main resets safely if neither closes.
        hid_host_disconnect(cid);
        if (cid)
            hci_disconnect_all();
    }
}
bool read_peer(std::uint32_t tag, Peer &p) {
    p = {};
    std::array<std::uint8_t, 8> record{};
    const int length = tlv->get_tag(tlv_context, tag, nullptr, 0);
    if (length == 0)
        return true;
    if (length != static_cast<int>(record.size()) ||
        tlv->get_tag(tlv_context, tag, record.data(), record.size()) != length || record[0] != 1 ||
        (record[1] != static_cast<std::uint8_t>(MouseName::named_slot2) &&
         record[1] != static_cast<std::uint8_t>(MouseName::pointing_slot2)))
        return false;
    if (std::all_of(record.begin() + 2, record.end(), [](auto value) { return value == 0; }))
        return false;
    p.name = static_cast<MouseName>(record[1]);
    std::copy_n(record.data() + 2, 6, p.address);
    return true;
}
bool write_peer(std::uint32_t tag, const Peer &p) {
    std::array<std::uint8_t, 8> record{1, static_cast<std::uint8_t>(p.name)};
    std::copy_n(p.address, 6, record.data() + 2);
    return tlv && tlv->store_tag(tlv_context, tag, record.data(), record.size()) == 0;
}
void clear_provisional() {
    if (known(provisional) && (!known(saved) || !same(saved.address, provisional.address)))
        gap_drop_link_key_for_bd_addr(provisional.address);
    if (tlv)
        tlv->delete_tag(tlv_context, provisional_tag);
    provisional = {};
}
bool load_peer() {
    btstack_tlv_get_instance(&tlv, &tlv_context);
    if (!tlv || !read_peer(peer_tag, saved) || !read_peer(provisional_tag, provisional))
        return false;
    // A power loss cannot turn incomplete commissioning into an authorized peer.
    // Commit selected first, then remove provisional: if power failed between the
    // two writes, the matching selected key survives this cleanup.
    if ((known(saved) && !approved_address(saved.address)) ||
        (known(provisional) && !approved_address(provisional.address)))
        return false;
    if (known(provisional))
        clear_provisional();
    return !known(saved) || has_key(saved);
}
bool save_peer() {
    if (!write_peer(peer_tag, attempt))
        return false;
    saved = attempt;
    if (known(provisional))
        clear_provisional();
    return true;
}
void keep_authenticated_key() {
    if (!window() || !fresh_key_seen || !known(attempt) || known(saved) || known(provisional) ||
        closing || !has_key(attempt) || acl_handle == HCI_CON_HANDLE_INVALID ||
        gap_security_level(acl_handle) < LEVEL_2)
        return;
    if (!write_peer(provisional_tag, attempt)) {
        gap_drop_link_key_for_bd_addr(attempt.address);
        failure(BtStage::storage, 4);
        fault();
        return;
    }
    provisional = attempt;
}
void finished() {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    motion_counters.new_link();
#endif
    neutral();
    keep_authenticated_key();
    restore_strict_floor();
    cid = 0;
    handle = HCI_CON_HANDLE_INVALID;
    opened = false;
    closing = false;
    descriptor_ready = false;
    protocol_ready = false;
    reports = {};
    // Hold only the newly observed, authenticated slot-2 key through this
    // commissioning window. Other failed, unaccepted attempts have no claim.
    if (!known(saved) && known(attempt) &&
        !(known(provisional) && window() && same(provisional.address, attempt.address)))
        gap_drop_link_key_for_bd_addr(attempt.address);
    attempt = {};
    fresh_key_seen = false;
    gap_set_bondable_mode(0);
    if (state == BtState::fault)
        return;
    if (known(saved)) {
        if (!has_key(saved)) {
            failure(BtStage::storage, 3);
            fault();
            return;
        }
        state = BtState::reconnecting;
        next_try = now_ms + std::min<std::uint64_t>(30'000, 2'000ULL << std::min(failures, 4u));
        ++failures;
    } else {
        state = window() ? BtState::searching : BtState::unpaired;
        next_try = now_ms + (known(provisional) ? 3'000 : 1'000);
    }
}
bool sufficient_link() {
    if (handle == HCI_CON_HANDLE_INVALID || gap_security_level(handle) < LEVEL_2)
        return false;
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    // Recheck the actual live ACL and native bond on every input. A changed
    // key or an unexpected security transition must neutralize, not emit data.
    const auto size = gap_encryption_key_size(handle);
    return approved_address(attempt.address) && has_key(attempt) && size >= 7 && size <= 16;
#else
    return true;
#endif
}
void maybe_ready() {
    if (state != BtState::connecting || closing || !opened || !descriptor_ready || !protocol_ready ||
        !has_key(attempt) || !sufficient_link())
        return;
    if (!known(saved) && !save_peer()) {
        failure(BtStage::storage, 2);
        fault();
        reject_link();
        return;
    }
    state = BtState::connected;
    diagnostic = {};
    failure_priority = 0;
    progress(BtStage::mouse_ready);
    failures = 0;
    window_end = 0;
    gap_set_bondable_mode(0);
}
void connect_to(const Peer &peer) {
    if (cid || closing || !known(peer) || !approved_address(peer.address))
        return;
    attempt = peer;
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
    observation.begin(peer);
#endif
    fresh_key_seen = false;
    diagnostic_peer = peer;
    failure_priority = 0;
    acl_handle = HCI_CON_HANDLE_INVALID;
    increment(attempt_count);
    state = BtState::connecting;
    deadline = now_ms + connect_ms;
    opened = false;
    descriptor_ready = false;
    protocol_ready = false;
    // New bonds only inside the finite commissioning window. Existing peer
    // reconnects using its stored link key and cannot silently pair a replacement.
    gap_set_bondable_mode(known(saved) || known(provisional) ? 0 : 1);
    progress(BtStage::hid_connect_started, attempt_count);
    allow_approved_floor(attempt.address);
    const auto status = hid_host_connect(attempt.address, HID_PROTOCOL_MODE_REPORT, &cid);
    if (status != ERROR_CODE_SUCCESS) {
        failure(BtStage::connect_failed, status);
        finished();
    }
}
void choose_candidate() {
    if (!window() || state != BtState::searching)
        return;
    for (std::size_t i = 0; i < found; ++i) {
        auto &c = candidates[i];
        if (c.name_checked)
            continue;
        c.name_checked = true;
        name_query = static_cast<int>(i);
        progress(BtStage::name_lookup, static_cast<std::uint8_t>(i + 1));
        deadline = now_ms + 6000;
        if (gap_remote_name_request(c.address, c.scan_mode, c.clock_offset | 0x8000) ==
            ERROR_CODE_SUCCESS)
            return;
        name_query = -1;
    }
    Peer only{};
    unsigned count = 0;
    for (const auto preferred : {MouseName::named_slot2, MouseName::pointing_slot2}) {
        for (std::size_t i = 0; i < found; ++i)
            if (candidates[i].name == preferred) {
                ++count;
                only.name = preferred;
                std::copy_n(candidates[i].address, 6, only.address);
            }
        if (count)
            break;
    }
    found = 0;
    next_try = now_ms + 1'000;
    // An unknown nearby peer or two recognized mice is not a selection.
    if (count == 1) {
        progress(BtStage::slot2_selected, static_cast<std::uint8_t>(only.name));
        connect_to(only);
    } else if (count > 1)
        failure(BtStage::ambiguous, static_cast<std::uint8_t>(count));
}
void inquiry_result(const std::uint8_t *packet, std::uint16_t size) {
    if (!inquiry || !window() || size < 27)
        return;
    increment(scan_results);
    bd_addr_t addr{};
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    if (!approved_address(addr))
        return;
    auto i = std::find_if(candidates.begin(), candidates.begin() + found,
                          [&](const Candidate &c) { return same(addr, c.address); });
    if (i == candidates.begin() + found) {
        if (found == candidates.size())
            return;
        i = &candidates[found++];
        *i = {};
        std::copy_n(addr, 6, i->address);
    }
    i->device_class = gap_event_inquiry_result_get_class_of_device(packet);
    i->scan_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    i->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    if (gap_event_inquiry_result_get_name_available(packet)) {
        const auto *name = gap_event_inquiry_result_get_name(packet);
        const auto len = gap_event_inquiry_result_get_name_len(packet);
        if (len <= size - 27) {
            i->name = match_mouse_name(name, len, i->device_class);
            i->name_checked = i->name != MouseName::unsupported;
        }
    }
    // Request a full remote name for truncated EIRs and Classic 2.1 peripherals
    // advertising no name. Do not spend the window looking up unrelated device classes.
    const auto cod = gap_event_inquiry_result_get_class_of_device(packet);
    if (!i->name_checked && cod && (cod & 0x1f00) != 0x0500)
        i->name_checked = true;
}
bool pairing_allowed(const bd_addr_t addr) {
    return state == BtState::connecting && !closing && cid && known(attempt) &&
           same(addr, attempt.address) && !known(saved) && !known(provisional) && window();
}
void hid_event(const std::uint8_t *p, std::uint16_t size) {
    if (size < 5)
        return;
    const auto sub = hci_event_hid_meta_get_subevent_code(p);
    const auto id = little_endian_read_16(p, 3);
    if (sub == HID_SUBEVENT_INCOMING_CONNECTION) {
        if (size < 14 || hid_subevent_incoming_connection_get_status(p) != 0)
            return;
        bd_addr_t addr{};
        hid_subevent_incoming_connection_get_address(p, addr);
        const Peer *allowed =
            known(saved) ? &saved : (window() && known(provisional) ? &provisional : nullptr);
        if (!working || state == BtState::fault || cid || !approved_address(addr) || !allowed ||
            !has_key(*allowed) || !same(addr, allowed->address)) {
            hid_host_decline_connection(id);
            return;
        }
        allow_approved_floor(addr);
        cid = id;
        attempt = *allowed;
        diagnostic_peer = *allowed;
        fresh_key_seen = false;
        failure_priority = 0;
        acl_handle = hid_subevent_incoming_connection_get_handle(p);
        increment(attempt_count);
        progress(BtStage::hid_connect_started, attempt_count);
        state = BtState::connecting;
        protocol_ready = false;
        deadline = now_ms + connect_ms;
        if (hid_host_accept_connection(id, HID_PROTOCOL_MODE_REPORT) != ERROR_CODE_SUCCESS) {
            hid_host_decline_connection(id);
            finished();
        }
        return;
    }
    if (!id || id != cid)
        return;
    if (sub == HID_SUBEVENT_CONNECTION_CLOSED) {
        failure(BtStage::disconnected);
        finished();
        return;
    }
    if (sub == HID_SUBEVENT_CONNECTION_OPENED) {
        if (size < 15 || hid_subevent_connection_opened_get_status(p) != ERROR_CODE_SUCCESS) {
            failure(BtStage::hid_open_failed,
                    size >= 6 ? hid_subevent_connection_opened_get_status(p) : 0xff);
            finished();
            return;
        }
        bd_addr_t addr{};
        hid_subevent_connection_opened_get_bd_addr(p, addr);
        if (closing || !same(addr, attempt.address)) {
            reject_link();
            return;
        }
        handle = hid_subevent_connection_opened_get_con_handle(p);
        acl_handle = handle;
        progress(BtStage::hid_open, hid_subevent_connection_opened_get_incoming(p) ? 1 : 0);
        opened = true;
        // Outgoing REPORT mode is selected during SDP. On incoming links the
        // stack emits a descriptor before SET_PROTOCOL; await its positive reply.
        protocol_ready = !hid_subevent_connection_opened_get_incoming(p);
        gap_request_security_level(handle, LEVEL_2);
        maybe_ready();
        return;
    }
    if (closing)
        return;
    if (sub == HID_SUBEVENT_VIRTUAL_CABLE_UNPLUG) {
        reject_link();
        return;
    }
    if (sub == HID_SUBEVENT_SET_PROTOCOL_RESPONSE) {
        if (size < 7 ||
            hid_subevent_set_protocol_response_get_handshake_status(p) !=
                HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL ||
            hid_subevent_set_protocol_response_get_protocol_mode(p) != HID_PROTOCOL_MODE_REPORT) {
            failure(BtStage::protocol_failed,
                    size >= 7 ? hid_subevent_set_protocol_response_get_handshake_status(p) : 0xff);
            reject_link();
            return;
        }
        protocol_ready = true;
        progress(BtStage::report_protocol);
        maybe_ready();
        return;
    }
    if (sub == HID_SUBEVENT_DESCRIPTOR_AVAILABLE) {
        descriptor_ready = size >= 6 &&
                           hid_subevent_descriptor_available_get_status(p) == ERROR_CODE_SUCCESS &&
                           reports.configure(hid_descriptor_storage_get_descriptor_data(id),
                                             hid_descriptor_storage_get_descriptor_len(id));
        if (!descriptor_ready) {
            if (known(provisional))
                clear_provisional();
            fresh_key_seen = false;
            if (size < 6 || hid_subevent_descriptor_available_get_status(p) != 0)
                failure(BtStage::descriptor_missing,
                        size >= 6 ? hid_subevent_descriptor_available_get_status(p) : 0xff);
            else
                failure(BtStage::descriptor_unsupported,
                        static_cast<std::uint8_t>(
                            std::min<unsigned>(hid_descriptor_storage_get_descriptor_len(id), 255)));
            reject_link();
            return;
        }
        progress(BtStage::descriptor_accepted);
        maybe_ready();
        return;
    }
    if (sub != HID_SUBEVENT_REPORT || state != BtState::connected)
        return;
    if (size < 7) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
        if (sufficient_link())
            motion_counters.observe(MouseReports::Result::malformed, 0);
#endif
        return;
    }
    if (!sufficient_link()) {
        failure(BtStage::encryption_failed, 0);
        reject_link();
        return;
    }
    const auto length = hid_subevent_report_get_report_len(p);
    const auto *bytes = hid_subevent_report_get_report(p);
    if (length != size - 7 || !length) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
        // Use only the actual event length for diagnostic reads. Incomplete
        // frames count as malformed; other explicit HIDP transaction types do not.
        if (size == 7 || bytes[0] == 0xa1)
            motion_counters.observe(MouseReports::Result::malformed,
                                    size > 8 ? reports.report_id(bytes + 1, size - 8) : 0);
#endif
        failure(BtStage::malformed_hid, 2);
        reject_link();
        return;
    }
    if (bytes[0] != 0xa1)
        return; // Only HIDP Input.
    MouseInput input;
    const auto result = reports.read(bytes + 1, length - 1, input);
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    motion_counters.observe(result, reports.report_id(bytes + 1, length - 1), input);
#endif
    if (result == MouseReports::Result::malformed) {
        failure(BtStage::malformed_report, reports.report_id(bytes + 1, length - 1));
        reject_link();
        return;
    }
    if (result == MouseReports::Result::mouse && callbacks.mouse) {
        progress(BtStage::mouse_report, reports.report_id(bytes + 1, length - 1));
        callbacks.mouse(input.dx, input.dy, input.wheel, input.buttons);
    }
}
void event_handler(std::uint8_t type, std::uint16_t, std::uint8_t *p, std::uint16_t size) {
    if (type != HCI_EVENT_PACKET || size < 2)
        return;
    bd_addr_t addr{};
    switch (hci_event_packet_get_type(p)) {
    case BTSTACK_EVENT_STATE:
        if (size < 3)
            return;
        if (btstack_event_state_get_state(p) == HCI_STATE_WORKING && !working) {
            working = true;
            restore_strict_floor();
            if (!load_peer()) {
                failure(BtStage::storage, 1);
                fault();
                return;
            }
            if (known(saved)) {
                state = BtState::reconnecting;
                next_try = now_ms;
            } else
                bt_pair();
        } else if (working && btstack_event_state_get_state(p) != HCI_STATE_WORKING) {
            working = false;
            restore_strict_floor();
            window_end = 0;
            neutral();
            state = BtState::fault;
        }
        break;
    case GAP_EVENT_INQUIRY_RESULT:
        inquiry_result(p, size);
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        if (inquiry) {
            inquiry = false;
            progress(BtStage::inquiry_complete, scan_results);
            choose_candidate();
        }
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (size < 9 || name_query < 0 || static_cast<std::size_t>(name_query) >= found)
            break;
        hci_event_remote_name_request_complete_get_bd_addr(p, addr);
        if (!same(addr, candidates[name_query].address))
            break;
        if (hci_event_remote_name_request_complete_get_status(p) == 0)
            candidates[name_query].name =
                match_mouse_name(p + 9, size - 9, candidates[name_query].device_class);
        name_query = -1;
        choose_candidate();
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        if (size < 8)
            break;
        hci_event_pin_code_request_get_bd_addr(p, addr);
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
        // The observed Omni2 used SSP. The compatibility exception never permits
        // a silent downgrade to fixed-PIN/legacy authentication.
        if (known(attempt) && same(addr, attempt.address))
            failure(BtStage::personal_policy, 1);
        gap_pin_code_negative(addr);
#else
        if (pairing_allowed(addr)) {
            progress(BtStage::pin_sent);
            gap_pin_code_response(addr, "1234");
        } else
            gap_pin_code_negative(addr);
#endif
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        if (size < 12)
            break;
        hci_event_user_confirmation_request_get_bd_addr(p, addr);
        if (pairing_allowed(addr)) {
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
            if (same(addr, observation.pending.address))
                observation.ssp = true;
#endif
            progress(BtStage::ssp_confirmed);
            gap_ssp_confirmation_response(addr);
        } else
            gap_ssp_confirmation_negative(addr);
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        if (size < 13 || !known(diagnostic_peer))
            break;
        hci_event_connection_complete_get_bd_addr(p, addr);
        if (!same(addr, diagnostic_peer.address))
            break;
        if (hci_event_connection_complete_get_status(p) != 0)
            failure(BtStage::acl_failed, hci_event_connection_complete_get_status(p));
        else {
            acl_handle = hci_event_connection_complete_get_connection_handle(p);
            progress(BtStage::acl_connected);
        }
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        if (size < 5 || acl_handle == HCI_CON_HANDLE_INVALID ||
            hci_event_authentication_complete_get_connection_handle(p) != acl_handle)
            break;
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
        if (known(observation.pending) && same(diagnostic_peer.address, observation.pending.address)) {
            observation.authenticated = hci_event_authentication_complete_get_status(p) == 0;
            observation.record();
        }
#endif
        if (hci_event_authentication_complete_get_status(p))
            failure(BtStage::authentication_failed, hci_event_authentication_complete_get_status(p));
        else
            progress(BtStage::authenticated);
        break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        if (size < 9 || !known(diagnostic_peer))
            break;
        hci_event_simple_pairing_complete_get_bd_addr(p, addr);
        if (same(addr, diagnostic_peer.address) && hci_event_simple_pairing_complete_get_status(p) != 0)
            failure(BtStage::authentication_failed, hci_event_simple_pairing_complete_get_status(p));
        break;
    case GAP_EVENT_SECURITY_LEVEL:
        if (size >= 6 && acl_handle != HCI_CON_HANDLE_INVALID &&
            gap_event_security_level_get_handle(p) == acl_handle) {
            const auto status = gap_event_security_level_get_status(p);
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
            // gap_encryption_key_size() returns zero unless this exact ACL is encrypted.
            if (known(observation.pending) &&
                same(diagnostic_peer.address, observation.pending.address)) {
                observation.short_encrypted = status == 0 &&
                                              gap_event_security_level_get_security_level(p) < LEVEL_2 &&
                                              gap_encryption_key_size(acl_handle) == 7;
                observation.record();
            }
#endif
            if (status)
                failure(BtStage::security_request_failed, status);
            else if (gap_event_security_level_get_security_level(p) < LEVEL_2)
                failure(BtStage::security_below_level2, gap_encryption_key_size(acl_handle));
            else {
                progress(BtStage::encrypted, gap_event_security_level_get_security_level(p));
                keep_authenticated_key();
            }
        }
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        if (size < 25)
            break;
        hci_event_link_key_request_get_bd_addr(p, addr);
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
        if (known(observation.pending) && same(addr, observation.pending.address)) {
            const auto t = p[24];
            observation.key_seen =
                ssp_key(t) && std::any_of(p + 8, p + 24, [](auto b) { return b != 0; });
            observation.record();
        }
#endif
        if (!known(attempt))
            break;
        if (same(addr, attempt.address)) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
            if (!ssp_key(p[24]) && !(p[24] == CHANGED_COMBINATION_KEY && has_key(attempt))) {
                failure(BtStage::personal_policy, 2);
                reject_link();
                break;
            }
#endif
            progress(BtStage::link_key_seen, p[24]);
            fresh_key_seen = true;
            keep_authenticated_key();
        }
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        if (size >= 6 && acl_handle != HCI_CON_HANDLE_INVALID &&
            hci_event_encryption_change_get_connection_handle(p) == acl_handle) {
            const auto status = hci_event_encryption_change_get_status(p);
            if (status || !hci_event_encryption_change_get_encryption_enabled(p)) {
                failure(BtStage::encryption_failed, status);
                if (opened && state == BtState::connected)
                    reject_link();
            } else
                progress(BtStage::encrypted, static_cast<std::uint8_t>(gap_security_level(acl_handle)));
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
        if (size >= 6 && acl_handle != HCI_CON_HANDLE_INVALID &&
            hci_event_disconnection_complete_get_connection_handle(p) == acl_handle)
            observation.pending = {}; // Never combine partial evidence from a recycled HCI handle.
#endif
        if (size >= 6 && acl_handle != HCI_CON_HANDLE_INVALID &&
            hci_event_disconnection_complete_get_connection_handle(p) == acl_handle)
            failure(BtStage::disconnected, hci_event_disconnection_complete_get_reason(p));
        if (size >= 6 && opened && hci_event_disconnection_complete_get_connection_handle(p) == handle) {
            neutral();
            descriptor_ready = false; // The HID close event owns CID retirement.
            if (!closing) {
                closing = true;
                close_deadline = now_ms + 3000;
            }
            state = known(saved) ? BtState::reconnecting : BtState::unpaired;
        }
        break;
    case HCI_EVENT_HID_META:
        hid_event(p, size);
        break;
    default:
        break;
    }
}
} // namespace
bool bt_start(BtCallbacks cb) {
    if (started || !cb.mouse || !cb.neutral)
        return false;
    started = true;
    callbacks = cb;
    neutral();
    l2cap_init();
    gap_set_security_level(LEVEL_2);
    // BTstack otherwise exempts SDP from the global encryption requirement.
    // Mouse 2 refused an outgoing channel for security (0x66). Authenticate
    // before SDP and keep the same LEVEL_2 policy for both HID channels.
    l2cap_require_security_level_2_for_outgoing_sdp();
    gap_set_bondable_mode(0);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    hid_host_init(descriptor_storage.data(), descriptor_storage.size());
    hid_host_register_packet_handler(event_handler);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    events.callback = event_handler;
    hci_add_event_handler(&events);
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    // The pinned SDK sends HCI Set_Min_Encryption_Key_Size during initialization
    // if the controller implements it. The runtime setter only changes the host
    // variable. Install the ACL filter BEFORE power-on, configure this personal
    // controller for 7, then restore the HOST floor at WORKING until the approved
    // peer is actually selected. Never admit a different device to this controller.
    gap_register_classic_connection_filter(accept_classic_connection);
    gap_set_required_encryption_key_size(7);
#else
    gap_set_required_encryption_key_size(16);
#endif
    if (hci_power_control(HCI_POWER_ON) != 0) {
        fault();
        return false;
    }
    return true;
}
void bt_tick(std::uint64_t time) {
    if (time < now_ms || !started)
        return;
    now_ms = time;
    if (closing && now_ms >= close_deadline) {
        failure(BtStage::teardown_timeout);
        recovery_needed = true;
        fault();
        return;
    }
    if (!working || state == BtState::fault)
        return;
    if (window_end && !window() && !known(saved)) {
        window_end = 0;
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
        observation = {};
#endif
        stop_search();
        if (known(provisional))
            clear_provisional();
        if (cid)
            reject_link();
        else
            state = BtState::unpaired;
    }
    if (state == BtState::connecting && !closing) {
        keep_authenticated_key();
        maybe_ready();
        if (state == BtState::connecting && now_ms >= deadline) {
            const auto gates =
                static_cast<std::uint8_t>((opened ? 1 : 0) | (has_key(attempt) ? 2 : 0) |
                                          (opened && gap_security_level(handle) >= LEVEL_2 ? 4 : 0) |
                                          (descriptor_ready ? 8 : 0) | (protocol_ready ? 16 : 0));
            failure(BtStage::connect_timeout, gates);
            reject_link();
        }
    }
    if (cid || closing || now_ms < next_try)
        return;
    if (known(saved) && state == BtState::reconnecting) {
        if (!has_key(saved)) {
            failure(BtStage::storage, 3);
            fault();
            return;
        }
        connect_to(saved);
    } else if (window() && state == BtState::searching) {
        if (known(provisional)) {
            if (has_key(provisional)) {
                connect_to(provisional);
                return;
            }
            clear_provisional();
        }
        if (name_query >= 0 && now_ms >= deadline) {
            name_query = -1;
            choose_candidate();
        } else if (!inquiry && name_query < 0) {
            found = 0;
            const auto result = gap_inquiry_start(3);
            inquiry = result == ERROR_CODE_SUCCESS;
            if (!inquiry) {
                failure(BtStage::inquiry_failed, static_cast<std::uint8_t>(result));
                next_try = now_ms + 1'000;
            } else {
                increment(scan_count);
                scan_results = 0;
                progress(BtStage::inquiry_started, scan_count);
            }
        }
    }
}
void bt_pair() {
    if (!working || known(saved) || state == BtState::fault || cid)
        return;
    diagnostic = {};
    failure_priority = 0;
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
    // A new commissioning action must not revive the previous window's evidence.
    observation = {};
#endif
    window_end = now_ms + pair_ms;
    state = BtState::searching;
    next_try = now_ms;
}
void bt_forget() {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    motion_counters = {};
#endif
    restore_strict_floor();
    window_end = 0;
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
    observation = {};
#endif
    stop_search();
    neutral();
    gap_set_bondable_mode(0);
    if (known(saved))
        gap_drop_link_key_for_bd_addr(saved.address);
    if (known(attempt))
        gap_drop_link_key_for_bd_addr(attempt.address);
    clear_provisional();
    if (tlv)
        tlv->delete_tag(tlv_context, peer_tag);
    saved = {};
    diagnostic_peer = {};
    state = BtState::unpaired;
    failures = 0;
    if (cid)
        reject_link();
    else
        attempt = {};
}
BtState bt_state() { return state; }
BtDiagnostic bt_diagnostic() { return diagnostic; }
bool bt_copy_motion_snapshot(std::array<std::uint8_t, 64> &out) {
#if defined(OMNI_SHORT_KEY_COMPAT) && OMNI_SHORT_KEY_COMPAT
    motion_counters.encode(out);
    return true;
#else
    out.fill(0);
    return false;
#endif
}
bool bt_recovery_requested() { return recovery_needed; }
bool bt_copy_short_key_candidate(std::array<std::uint8_t, 6> &out) {
    out.fill(0);
#if defined(OMNI_CAPTURE_SHORT_KEY) && OMNI_CAPTURE_SHORT_KEY
    if (window() && state != BtState::fault && known(observation.confirmed) &&
        now_ms < observation.valid_until) {
        std::copy_n(observation.confirmed.address, 6, out.begin());
        return true;
    }
#endif
    return false;
}
} // namespace omni
