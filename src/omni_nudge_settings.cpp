#include "omni_nudge_settings.hpp"
#include "omni_usb.hpp"
#include "btstack_tlv.h"
#include <algorithm>
#include <limits>
namespace omni {
namespace {
constexpr std::uint32_t tag = 0x4f4d4e33; // OMN3. OMN1/OMN2 belong to Bluetooth pairing.
constexpr std::array<std::uint8_t, 4> read_magic{'N', 'G', 'R', '1'}, write_magic{'N', 'G', 'W', '1'},
    store_magic{'N', 'G', 'S', '1'}, save_magic{'S', 'A', 'V', 'E'};
enum class Storage : std::uint8_t { defaults = 0, saved = 1, corrupt = 2, unavailable = 3 };
enum class Result : std::uint8_t {
    none = 0,
    pending = 1,
    applied = 2,
    stale = 3,
    invalid = 4,
    storage = 5,
    interrupted = 6
};
Storage storage = Storage::unavailable;
Result result = Result::none;
std::uint32_t revision = 0;
const btstack_tlv_t *tlv = nullptr;
void *context = nullptr;
NudgeMap off() {
    NudgeMap m;
    m.right = m.left = m.up = m.down = 0;
    return m;
}
NudgeMap active = off();
std::uint32_t get32(const NudgeSettingsWire &bytes, unsigned offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= std::uint32_t(bytes[offset + i]) << (i * 8);
    return value;
}
void put32(NudgeSettingsWire &bytes, unsigned offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t checksum(const NudgeSettingsWire &bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < 12; ++i) {
        crc ^= bytes[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
NudgeMap read_map(const NudgeSettingsWire &bytes) {
    NudgeMap m;
    m.right = bytes[8];
    m.left = bytes[9];
    m.up = bytes[10];
    m.down = bytes[11];
    return m;
}
void put_map(NudgeSettingsWire &bytes, const NudgeMap &m) {
    bytes[8] = m.right;
    bytes[9] = m.left;
    bytes[10] = m.up;
    bytes[11] = m.down;
}
void publish(Storage source, std::uint32_t rev, const NudgeMap &map) {
    storage = source;
    revision = rev;
    active = map; // Only validated maps/defaults/all-Off reach this point.
    (void)usb_set_nudge_map(active);
}
void load() {
    if (!tlv) {
        publish(Storage::unavailable, 0, off());
        return;
    }
    const auto len = tlv->get_tag(context, tag, nullptr, 0);
    if (len < 0) {
        publish(Storage::unavailable, 0, off());
        return;
    }
    if (len == 0) {
        publish(Storage::defaults, 0, NudgeMap{});
        return;
    }
    NudgeSettingsWire bytes{};
    if (len != static_cast<int>(bytes.size())) {
        publish(Storage::corrupt, 0, off());
        return;
    }
    if (tlv->get_tag(context, tag, bytes.data(), bytes.size()) != len) {
        publish(Storage::unavailable, 0, off());
        return;
    }
    const auto rev = get32(bytes, 4);
    const auto map = read_map(bytes);
    if (!std::equal(store_magic.begin(), store_magic.end(), bytes.begin()) || !rev || !map.valid() ||
        get32(bytes, 12) != checksum(bytes)) {
        publish(Storage::corrupt, 0, off());
        return;
    }
    publish(Storage::saved, rev, map);
}
} // namespace
void nudge_settings_init() {
    tlv = nullptr;
    context = nullptr;
    btstack_tlv_get_instance(&tlv, &context);
    load();
    result = Result::none;
}
NudgeSettingsWire nudge_settings_read() {
    NudgeSettingsWire out{};
    std::copy(read_magic.begin(), read_magic.end(), out.begin());
    put32(out, 4, revision);
    put_map(out, active);
    out[12] = static_cast<std::uint8_t>(storage);
    out[13] = static_cast<std::uint8_t>(result);
    return out;
}
bool nudge_settings_prepare(const NudgeSettingsWire &packet, NudgeProposal &proposal) {
    const auto map = read_map(packet);
    const auto expected = get32(packet, 4);
    if (!std::equal(write_magic.begin(), write_magic.end(), packet.begin()) ||
        !std::equal(save_magic.begin(), save_magic.end(), packet.begin() + 12) || !map.valid()) {
        result = Result::invalid;
        return false;
    }
    if (storage == Storage::unavailable || revision == std::numeric_limits<std::uint32_t>::max()) {
        result = Result::storage;
        return false;
    }
    if (expected != revision) {
        result = Result::stale;
        return false;
    }
    proposal = {expected, map};
    return true;
}
void nudge_settings_pending() { result = Result::pending; }
void nudge_settings_abort() { result = Result::interrupted; }
void nudge_settings_commit(const NudgeProposal &proposal) {
    if (result != Result::pending)
        return;
    if (proposal.previous_revision != revision) {
        result = Result::stale;
        return;
    }
    if (!proposal.map.valid() || !tlv || revision == std::numeric_limits<std::uint32_t>::max()) {
        result = Result::storage;
        return;
    }
    NudgeSettingsWire expected{};
    std::copy(store_magic.begin(), store_magic.end(), expected.begin());
    put32(expected, 4, revision + 1);
    put_map(expected, proposal.map);
    put32(expected, 12, checksum(expected));
    // BTstack writes one compound record (data first, then entry header) before
    // retiring the old entry. Publish only what a readback can verify, even if
    // the store reports an error after a partially completed write.
    const auto status = tlv->store_tag(context, tag, expected.data(), expected.size());
    NudgeSettingsWire actual{};
    const auto length = tlv->get_tag(context, tag, nullptr, 0);
    if (length == static_cast<int>(actual.size()) &&
        tlv->get_tag(context, tag, actual.data(), actual.size()) == length && actual == expected) {
        publish(Storage::saved, get32(actual, 4), proposal.map);
        result = Result::applied;
        return;
    }
    // A previous valid map may still be present. A torn record produces all Off.
    (void)status;
    load();
    result = Result::storage;
}
} // namespace omni
