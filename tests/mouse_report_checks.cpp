#include "omni_mouse_report.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using omni::MouseInput;
using omni::MouseName;
using omni::MouseReports;
namespace {
using Bytes = std::vector<std::uint8_t>;
int checks = 0;
void check(bool truth) {
    ++checks;
    if (!truth) {
        std::cerr << "failed assertion " << checks << '\n';
        std::exit(1);
    }
}
const Bytes standard = {5,    1,    9,    2,    0xa1, 1,    9, 1,    0xa1, 0,    5, 9,    0x19,
                        1,    0x29, 3,    0x15, 0,    0x25, 1, 0x75, 1,    0x95, 3, 0x81, 2,
                        0x75, 5,    0x95, 1,    0x81, 1,    5, 1,    9,    0x30, 9, 0x31, 9,
                        0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 8, 0x95, 3,    0x81, 6, 0xc0, 0xc0};
MouseName name(const char *n, unsigned cod = 0) {
    return omni::match_mouse_name(reinterpret_cast<const std::uint8_t *>(n), std::strlen(n), cod);
}
} // namespace
int main() {
    check(name("R-net Mouse 2") == MouseName::named_slot2);
    check(name("r-NET mouse 2 ") == MouseName::named_slot2);
    check(name("Mouse 2", 0x0580) == MouseName::pointing_slot2);
    check(name("Mouse 2", 0x05c0) == MouseName::pointing_slot2);
    for (auto n : {"Mouse 2", "R-net Mouse 1", "R-net Mouse 20", "R-net Bluetooth Mouse Module",
                   "R-net Mou", "Office Mouse"})
        check(name(n) == MouseName::unsupported);
    check(name("Mouse 2", 0x0540) == MouseName::unsupported);
    check(name("Mouse 2", 0x0480) == MouseName::unsupported);
    MouseReports parser;
    MouseInput out;
    check(parser.configure(standard.data(), standard.size()));
    auto read = [&](Bytes data) { return parser.read(data.data(), data.size(), out); };
    check(read({5, 0x80, 0x7f, 0xff}) == MouseReports::Result::mouse);
    check(out.buttons == 5 && out.dx == -128 && out.dy == 127 && out.wheel == -1);
    check(read({0, 0, 0}) == MouseReports::Result::malformed);
    check(read({0, 0, 0, 0, 0}) == MouseReports::Result::malformed);
    check(read({0, 0, 0, 0}) == MouseReports::Result::mouse && out.buttons == 0 && out.dx == 0);
    // Report ID 2 holds buttons; ID 3 contains packed, signed 12-bit X/Y only.
    const Bytes split = {5,    1,    9,    2,    0xa1, 1,    0x85, 2,    5, 9,    0x19, 1,    0x29,
                         3,    0x15, 0,    0x25, 1,    0x75, 1,    0x95, 3, 0x81, 2,    0x75, 5,
                         0x95, 1,    0x81, 1,    0x85, 3,    5,    1,    9, 0x30, 9,    0x31, 0x16,
                         0x00, 0xf8, 0x26, 0xff, 7,    0x75, 12,   0x95, 2, 0x81, 6,    0xc0};
    const std::uint8_t arbitrary[] = {7};
    check(parser.report_id(arbitrary, 1) == 0);
    check(parser.configure(split.data(), split.size()));
    check(parser.report_id(arbitrary, 1) == 7 && parser.report_id(nullptr, 0) == 0);
    check(read({2, 2}) == MouseReports::Result::mouse && out.buttons == 2);
    check(read({3, 0xf0, 0x4f, 0x06}) == MouseReports::Result::mouse && out.dx == -16 && out.dy == 100 &&
          out.buttons == 2);
    check(read({7, 0, 0, 0}) == MouseReports::Result::unrelated);
    check(read({2, 0}) == MouseReports::Result::mouse && out.buttons == 0);
    auto absolute = standard;
    absolute[absolute.size() - 3] = 2;
    check(!parser.configure(absolute.data(), absolute.size()));
    auto keyboard = standard;
    keyboard[3] = 6;
    check(!parser.configure(keyboard.data(), keyboard.size()));
    auto partial = standard;
    partial.pop_back();
    check(!parser.configure(partial.data(), partial.size()));
    auto oversized = standard;
    oversized[20] = 64;
    check(!parser.configure(oversized.data(), oversized.size()));
    const Bytes malformed = {0x77, 1, 2};
    check(!parser.configure(malformed.data(), malformed.size()));
    check(read({0, 0, 0, 0}) == MouseReports::Result::malformed);
    std::cout << checks << " slot/name/descriptor/report assertions passed\n";
}
