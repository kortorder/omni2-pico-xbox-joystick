#include "omni_nudge_settings.hpp"
#include "omni_usb.hpp"
#include "btstack_tlv.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace {
using namespace omni;
std::vector<std::uint8_t> record;
NudgeMap runtime{};
bool available = true, deny = false, torn = false, read_error = false;
unsigned writes = 0, checks = 0;
void check(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::exit(1);
    }
    ++checks;
}
int read(void *, std::uint32_t tag, std::uint8_t *out, std::uint32_t n) {
    check(tag == 0x4f4d4e33, "settings use separate OMN3 tag");
    if (read_error)
        return -1;
    if (!out)
        return static_cast<int>(record.size());
    const auto size = std::min<std::size_t>(n, record.size());
    std::copy_n(record.begin(), size, out);
    return static_cast<int>(size);
}
int write(void *, std::uint32_t tag, const std::uint8_t *bytes, std::uint32_t n) {
    check(tag == 0x4f4d4e33 && n == 16, "one full settings record only");
    ++writes;
    if (deny)
        return 2;
    record.assign(bytes, bytes + n);
    if (torn)
        record[11] ^= 1;
    return 0;
}
void remove(void *, std::uint32_t) { check(false, "no Bluetooth or reset deletion"); }
const btstack_tlv_t fake{read, write, remove};
NudgeSettingsWire cmd(std::uint32_t rev, std::array<std::uint8_t, 4> slots) {
    NudgeSettingsWire e{'N', 'G', 'W', '1'};
    for (unsigned i = 0; i < 4; ++i)
        e[4 + i] = static_cast<std::uint8_t>(rev >> (8 * i));
    std::copy(slots.begin(), slots.end(), e.begin() + 8);
    e[12] = 'S';
    e[13] = 'A';
    e[14] = 'V';
    e[15] = 'E';
    return e;
}
std::uint32_t rev(const NudgeSettingsWire &r) {
    return std::uint32_t(r[4]) | (std::uint32_t(r[5]) << 8) | (std::uint32_t(r[6]) << 16) |
           (std::uint32_t(r[7]) << 24);
}
void expect(std::uint32_t revision, std::array<std::uint8_t, 4> slots, unsigned source,
            unsigned result) {
    const auto r = nudge_settings_read();
    check(r[0] == 'N' && r[1] == 'G' && r[2] == 'R' && r[3] == '1' && !r[14] && !r[15],
          "exact GET header and reserved zero");
    check(rev(r) == revision && r[12] == source && r[13] == result &&
              std::equal(slots.begin(), slots.end(), r.begin() + 8),
          "GET source, revision, outcome and right/left/up/down slots");
    check(runtime.right == slots[0] && runtime.left == slots[1] && runtime.up == slots[2] &&
              runtime.down == slots[3],
          "runtime uses the same named directions");
}
void save(const NudgeSettingsWire &command) {
    NudgeProposal p;
    check(nudge_settings_prepare(command, p), "valid conditional save");
    nudge_settings_pending();
    nudge_settings_commit(p);
}
} // namespace
bool omni::usb_set_nudge_map(NudgeMap m) {
    check(m.valid(), "never apply out-of-range HID slots");
    runtime = m;
    return true;
}
extern "C" void btstack_tlv_get_instance(const btstack_tlv_t **t, void **c) {
    *t = available ? &fake : nullptr;
    *c = nullptr;
}
int main() {
    using namespace omni;
    const std::array<std::uint8_t, 4> initial{12, 4, 5, 3}, changed{0, 12, 5, 0}, off{};
    nudge_settings_init();
    expect(0, initial, 0, 0);
    NudgeProposal p;
    auto invalid = cmd(0, changed);
    invalid[8] = 13;
    check(!nudge_settings_prepare(invalid, p), "reject 13");
    expect(0, initial, 0, 4);
    invalid = cmd(0, changed);
    invalid[12] = 'X';
    check(!nudge_settings_prepare(invalid, p) && !writes, "reject missing SAVE intent");
    invalid = cmd(0, changed);
    invalid[2] = 'R';
    check(!nudge_settings_prepare(invalid, p), "read packet cannot write");
    check(!nudge_settings_prepare(cmd(1, changed), p), "reject stale revision");
    expect(0, initial, 0, 3);
    check(nudge_settings_prepare(cmd(0, changed), p), "accept off slots");
    nudge_settings_commit(p);
    check(!writes, "prepare alone cannot write");
    nudge_settings_pending();
    nudge_settings_abort();
    nudge_settings_commit(p);
    check(!writes, "abort fences proposed save");
    expect(0, initial, 0, 6);
    save(cmd(0, changed));
    expect(1, changed, 1, 2);
    const auto preserved = record;
    check(record.size() == 16 && writes == 1, "single stored value");
    nudge_settings_init();
    expect(1, changed, 1, 0);
    check(!nudge_settings_prepare(cmd(0, off), p) && record == preserved,
          "stale editor cannot overwrite newer value");
    deny = true;
    save(cmd(1, off));
    expect(1, changed, 1, 5);
    check(record == preserved, "storage failure retains prior record exactly");
    deny = false;
    torn = true;
    save(cmd(1, off));
    expect(0, off, 2, 5);
    torn = false;
    nudge_settings_init();
    expect(0, off, 2, 0); // Single-bit damage in otherwise plausible slots is detected.
    save(cmd(0, off));
    expect(1, off, 1, 2);
    nudge_settings_init();
    expect(1, off, 1, 0);
    check(writes == 4, "explicit repair of invalid settings");
    record.resize(7);
    nudge_settings_init();
    expect(0, off, 2, 0);
    read_error = true;
    nudge_settings_init();
    expect(0, off, 3, 0);
    read_error = false;
    available = false;
    nudge_settings_init();
    check(!nudge_settings_prepare(cmd(0, changed), p), "no flash backend, fail closed");
    expect(0, off, 3, 5);
    std::printf("PASS %u settings storage/codec assertions\n", checks);
}
