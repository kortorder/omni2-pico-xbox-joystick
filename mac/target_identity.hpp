// Use the same USB identity selected for this firmware build.
#pragma once
#ifndef OMNI_USB_VID
#error Choose the target USB identity using build.sh.
#endif
#ifndef OMNI_USB_PID
#error Choose the target USB identity using build.sh.
#endif
namespace omni_mac {
constexpr unsigned vid = OMNI_USB_VID;
constexpr unsigned pid = OMNI_USB_PID;
static_assert(vid > 0 && vid <= 0xffff && pid > 0 && pid <= 0xffff,
              "USB VID/PID must each be nonzero 16-bit values.");
}
