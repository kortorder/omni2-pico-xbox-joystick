// Wired Omni maintenance. Only explicit chair pin capture writes a file.
#include "omni_maintenance.hpp"
#include "target_identity.hpp"
#include "omni_motion_snapshot.hpp"
#include "omni_nudge_settings.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace omni::maintenance;
using omni_mac::vid;
using omni_mac::pid;
constexpr const char *usage =
    "Usage: omni-maintenance --self-test | --probe | --enter-bootsel "
    "0xLOCATION | --capture-chair-pin 0xLOCATION | "
    "--watch-chair-motion 0xLOCATION SECONDS (2..180) | --get-nudges 0xLOCATION | "
    "--set-nudges 0xLOCATION REV RIGHT LEFT UP DOWN\n";
struct Target {
    io_service_t service = IO_OBJECT_NULL;
    unsigned count = 0;
    std::uint32_t location = 0;
    ~Target() {
        if (service)
            IOObjectRelease(service);
    }
};
bool identity(unsigned v, unsigned p, std::string_view product, std::string_view manufacturer) {
    return v == vid && p == pid && product == "Omni Right Stick" &&
           manufacturer == "SimpleController prototype";
}
bool stringProperty(io_service_t device, CFStringRef primary, CFStringRef fallback,
                    char (&text)[128]) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(device, primary, kCFAllocatorDefault, 0);
    if (!property)
        property = IORegistryEntryCreateCFProperty(device, fallback, kCFAllocatorDefault, 0);
    if (!property)
        return false;
    const bool ok =
        CFGetTypeID(property) == CFStringGetTypeID() &&
        CFStringGetCString(static_cast<CFStringRef>(property), text, 128, kCFStringEncodingUTF8);
    CFRelease(property);
    return ok;
}
bool numericProperty(io_service_t device, CFStringRef key, std::uint32_t &value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(device, key, kCFAllocatorDefault, 0);
    if (!property)
        return false;
    std::int64_t number = 0;
    const bool valid =
        CFGetTypeID(property) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &number) &&
        number >= 0 && static_cast<std::uint64_t>(number) <= UINT32_MAX;
    CFRelease(property);
    if (valid)
        value = static_cast<std::uint32_t>(number);
    return valid;
}
void matchNumber(CFMutableDictionaryRef match, CFStringRef key, int value) {
    auto number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(match, key, number);
    CFRelease(number);
}
bool findTarget(Target &target) {
    auto matches = IOServiceMatching("IOUSBHostDevice");
    if (!matches)
        return false;
    matchNumber(matches, CFSTR("idVendor"), vid);
    matchNumber(matches, CFSTR("idProduct"), pid);
    io_iterator_t devices = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matches, &devices) != KERN_SUCCESS)
        return false;
    while (auto device = IOIteratorNext(devices)) {
        char product[128]{}, maker[128]{};
        std::uint32_t v = 0, p = 0, l = 0;
        if (numericProperty(device, CFSTR("idVendor"), v) &&
            numericProperty(device, CFSTR("idProduct"), p) &&
            stringProperty(device, CFSTR("USB Product Name"), CFSTR("kUSBProductString"),
                           product) &&
            stringProperty(device, CFSTR("USB Vendor Name"), CFSTR("kUSBVendorString"), maker) &&
            identity(v, p, product, maker)) {
            ++target.count;
            if (target.count == 1 && numericProperty(device, CFSTR("locationID"), l) && l != 0) {
                target.service = device;
                target.location = l;
                continue;
            }
        }
        IOObjectRelease(device);
    }
    IOObjectRelease(devices);
    return target.count == 1 && target.service != IO_OBJECT_NULL;
}

struct Device {
    IOUSBDeviceInterface320 **usb = nullptr;
    ~Device() {
        if (usb)
            (*usb)->Release(usb);
    }
    bool connect(const Target &target) {
        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(target.service, kIOUSBDeviceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &plugin,
                                              &score) != kIOReturnSuccess ||
            !plugin)
            return false;
        auto result =
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320),
                                      reinterpret_cast<LPVOID *>(&usb));
        (*plugin)->Release(plugin);
        if (result != S_OK || !usb)
            return false;
        UInt16 v = 0, p = 0;
        UInt32 location = 0;
        return (*usb)->GetDeviceVendor(usb, &v) == kIOReturnSuccess && v == vid &&
               (*usb)->GetDeviceProduct(usb, &p) == kIOReturnSuccess && p == pid &&
               (*usb)->GetLocationID(usb, &location) == kIOReturnSuccess &&
               location == target.location;
    }
    IOReturn operator()(IOUSBDevRequestTO &req) {
        // DeviceRequestTO works over endpoint zero. No USBDeviceOpen/Seize,
        // interface claim, reconfiguration or detach of a macOS driver.
        return (*usb)->DeviceRequestTO(usb, &req);
    }
};
std::optional<std::uint32_t> locationFrom(std::string_view text) {
    if (text.size() < 3 || text.size() > 10 || text.substr(0, 2) != "0x")
        return {};
    std::uint32_t value = 0;
    auto result = std::from_chars(text.data() + 2, text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !value)
        return {};
    return value;
}
bool authorized(unsigned number, std::uint32_t location, std::optional<std::uint32_t> expected) {
    return number == 1 && location != 0 && (!expected || *expected == location);
}
enum class Outcome { supported, requested, missing_capability, command_failed };
template <class Send> Outcome operate(Send &send, bool enter, IOReturn &status) {
    auto reply = capability;
    reply.fill(0);
    IOUSBDevRequestTO req{};
    req.bmRequestType = probe_type;
    req.bRequest = probe_request;
    req.wValue = request_value;
    req.wIndex = request_index;
    req.wLength = reply.size();
    req.pData = reply.data();
    req.noDataTimeout = 1000;
    req.completionTimeout = 1500;
    status = send(req);
    if (status != kIOReturnSuccess || req.wLenDone != reply.size() || reply != capability)
        return Outcome::missing_capability;
    if (!enter)
        return Outcome::supported;
    req.bmRequestType = enter_type;
    req.bRequest = enter_request;
    req.wLength = 0;
    req.wLenDone = 0;
    req.pData = nullptr;
    status = send(req);
    return status == kIOReturnSuccess && req.wLenDone == 0 ? Outcome::requested
                                                           : Outcome::command_failed;
}

template <class Send>
std::optional<omni::motion_watch::Snapshot> readMotion(Send &send, IOReturn &status) {
    if (operate(send, false, status) != Outcome::supported)
        return {};
    MotionReply wire{};
    IOUSBDevRequestTO req{};
    req.bmRequestType = probe_type;
    req.bRequest = motion_request;
    req.wValue = request_value;
    req.wIndex = request_index;
    req.wLength = wire.size();
    req.pData = wire.data();
    req.noDataTimeout = 1000;
    req.completionTimeout = 1500;
    status = send(req);
    if (status != kIOReturnSuccess || req.wLenDone != wire.size())
        return {};
    return omni::motion_watch::decode(wire);
}
std::optional<unsigned> watchDuration(std::string_view text) {
    unsigned n = 0;
    if (text.empty() || text.size() > 3)
        return {};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || n < 2 || n > 180)
        return {};
    return n;
}
int watchMotion(Device &device, unsigned seconds) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono;
    IOReturn status = 0;
    auto first = readMotion(device, status);
    if (!first) {
        std::fprintf(stderr,
                     "Directional observation unavailable (0x%08x). Use the pinned diagnostic "
                     "image and check the wired connection.\n",
                     unsigned(status));
        return 1;
    }
    auto previous = *first;
    const auto start = Clock::now(), end = start + std::chrono::seconds(seconds);
    std::printf("Inbound chair baseline recorded. Direction counters count mouse reports. Duration "
                "%u seconds.\n",
                seconds);
    std::fflush(stdout);
    while (Clock::now() < end) {
        std::this_thread::sleep_until(std::min(end, Clock::now() + milliseconds(250)));
        auto next = readMotion(device, status);
        if (!next) {
            std::fprintf(stderr, "Motion read failed (0x%08x); capture incomplete.\n",
                         unsigned(status));
            return 1;
        }
        const auto elapsed = duration_cast<milliseconds>(Clock::now() - start).count();
        const auto interval = omni::motion_watch::between(previous, *next);
        if (interval.reset) {
            std::fprintf(stderr,
                         "Inbound counters reset during capture; discarding this comparison.\n");
            return 1;
        }
        if (interval.changed())
            omni::motion_watch::print("input", elapsed, interval, *next);
        previous = *next;
    }
    omni::motion_watch::print("total", duration_cast<milliseconds>(Clock::now() - start).count(),
                              omni::motion_watch::between(*first, previous), previous);
    return 0;
}
template <class Send> std::optional<nudge::Record> readNudges(Send &send) {
    IOReturn status = 0;
    if (operate(send, false, status) != Outcome::supported)
        return {};
    nudge::Wire data{};
    IOUSBDevRequestTO req{};
    req.bmRequestType = probe_type;
    req.bRequest = nudge::get_request;
    req.wValue = request_value;
    req.wIndex = request_index;
    req.wLength = data.size();
    req.pData = data.data();
    req.noDataTimeout = 1000;
    req.completionTimeout = 1500;
    if (send(req) != kIOReturnSuccess || req.wLenDone != data.size())
        return {};
    return nudge::decode(data);
}
template <class Send, class Wait>
const char *saveNudges(Send &send, std::uint32_t revision, const nudge::Slots &slots,
                       nudge::Record &verified, Wait wait) {
    if (revision == UINT32_MAX || !nudge::valid(slots))
        return "Invalid revision or button choice.";
    auto before = readNudges(send);
    if (!before)
        return "Cannot read nudge settings. Check the USB connection and firmware version.";
    if (before->storage == 3)
        return "Pico storage is unavailable.";
    if (before->revision != revision || before->result == 1)
        return "Settings changed or are busy. Reload before saving.";
    IOReturn status = 0;
    if (operate(send, false, status) != Outcome::supported)
        return "Pico did not authorize the settings request.";
    auto data = nudge::encode(revision, slots);
    IOUSBDevRequestTO req{};
    req.bmRequestType = enter_type;
    req.bRequest = nudge::set_request;
    req.wValue = request_value;
    req.wIndex = request_index;
    req.wLength = data.size();
    req.pData = data.data();
    req.noDataTimeout = 1000;
    req.completionTimeout = 1500;
    if (send(req) != kIOReturnSuccess || req.wLenDone != data.size())
        return "The save request was not confirmed. Reload to see the Pico’s settings.";
    for (unsigned n = 0; n < 20; ++n) {
        wait();
        auto r = readNudges(send);
        if (!r)
            return "Lost contact while checking the save. Reload to see the Pico’s settings.";
        if (r->revision == revision + 1 && r->slots == slots && r->storage == 1 && r->result == 2) {
            verified = *r;
            return nullptr;
        }
        if (r->revision != revision || r->result == 3)
            return "The saved settings have changed. Reload before editing again.";
        if (r->storage == 3 || r->result == 5)
            return "Pico storage could not confirm this save. Reload before trying again.";
        if (r->result == 4)
            return "The Pico rejected these settings. Reload before trying again.";
        if (r->result == 6)
            return "The save was interrupted before storage. Reload to check the current settings.";
    }
    return "Save was not verified in time. Reload to check the Pico before trying again.";
}

bool validPin(const PeerPin &pin) {
    return !std::all_of(pin.begin(), pin.end(), [](auto x) { return x == 0; }) &&
           !std::all_of(pin.begin(), pin.end(), [](auto x) { return x == 0xff; });
}
template <class Send> bool capturePin(Send &send, IOReturn &status, PeerPin &pin) {
    pin.fill(0);
    if (operate(send, false, status) != Outcome::supported)
        return false;
    CaptureReply reply{};
    IOUSBDevRequestTO req{};
    req.bmRequestType = probe_type;
    req.bRequest = capture_request;
    req.wValue = request_value;
    req.wIndex = request_index;
    req.wLength = reply.size();
    req.pData = reply.data();
    req.noDataTimeout = 1000;
    req.completionTimeout = 1500;
    status = send(req);
    if (status != kIOReturnSuccess || req.wLenDone != reply.size() || reply[0] != 1 ||
        reply[1] != 7)
        return false;
    std::copy(reply.begin() + 2, reply.end(), pin.begin());
    if (!validPin(pin)) {
        pin.fill(0);
        return false;
    }
    return true;
}

// mkdtemp creates a unique user-private directory outside the workspace.
// Its contents become discoverable to the operator only after a complete save.
std::optional<std::string> savePrivatePin(const PeerPin &pin) {
    if (!validPin(pin))
        return {};
    char directory[] = "/private/tmp/omni-chair-pin-XXXXXX";
    if (!mkdtemp(directory))
        return {};
    const int parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    const bool privateDir = parent >= 0 && !fstat(parent, &info) && S_ISDIR(info.st_mode) &&
                            info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
    const int fd = privateDir ? openat(parent, "peer.bin",
                                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)
                              : -1;
    bool saved = fd >= 0 && !fchmod(fd, 0600) && !fstat(fd, &info) && S_ISREG(info.st_mode) &&
                 info.st_uid == geteuid() && (info.st_mode & 0777) == 0600;
    std::size_t written = 0;
    while (saved && written < pin.size()) {
        const auto n = write(fd, pin.data() + written, pin.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            saved = false;
        else
            written += static_cast<std::size_t>(n);
    }
    if (saved)
        saved = fsync(fd) == 0;
    if (fd >= 0 && close(fd) < 0)
        saved = false;
    if (!saved && fd >= 0)
        unlinkat(parent, "peer.bin", 0);
    if (parent >= 0)
        close(parent);
    if (!saved) {
        rmdir(directory);
        return {};
    }
    return std::string(directory) + "/peer.bin";
}

int selfTest() {
    unsigned passed = 0;
    auto expect = [&](bool ok) {
        if (!ok) {
            std::fprintf(stderr, "Dry check %u failed\n", passed + 1);
            std::exit(1);
        }
        ++passed;
    };
    expect(identity(vid, pid, "Omni Right Stick", "SimpleController prototype"));
    expect(!identity(vid, 1, "SCX2 unverified XAC bridge", "SimpleController prototype"));
    expect(!identity(vid, pid, "Other", "SimpleController prototype"));
    expect(!identity(vid, pid, "Omni Right Stick", "Other"));
    expect(!authorized(2, 0x1000, 0x1000) && !authorized(0, 0, {}));
    expect(!authorized(1, 0x1000, 0x1001) && authorized(1, 0x1000, 0x1000));
    expect(locationFrom("0x10a0") == 0x10a0 && !locationFrom("0x0") && !locationFrom("0x10junk") &&
           !locationFrom("-1"));
    unsigned sends = 0;
    int fault = 0;
    auto fake = [&](IOUSBDevRequestTO &req) -> IOReturn {
        ++sends;
        expect(req.wValue == request_value && req.wIndex == request_index &&
               req.completionTimeout == 1500);
        if (req.bRequest == probe_request && req.bmRequestType == probe_type) {
            expect(req.wLength == 8 && req.pData != nullptr);
            auto *out = static_cast<std::uint8_t *>(req.pData);
            std::copy(capability.begin(), capability.end(), out);
            if (fault == 1)
                out[0] ^= 0xff;
            req.wLenDone = fault == 2 ? 7 : 8;
            return fault == 3 ? kIOReturnNotPermitted : kIOReturnSuccess;
        }
        expect(req.bRequest == enter_request && req.bmRequestType == enter_type &&
               req.wLength == 0 && !req.pData);
        return fault == 4 ? kIOReturnError : kIOReturnSuccess;
    };
    IOReturn status = 0;
    expect(operate(fake, false, status) == Outcome::supported && sends == 1);
    for (fault = 1; fault <= 3; ++fault) {
        sends = 0;
        expect(operate(fake, true, status) == Outcome::missing_capability && sends == 1);
    }
    sends = 0;
    fault = 0;
    expect(operate(fake, true, status) == Outcome::requested && sends == 2);
    sends = 0;
    fault = 4;
    expect(operate(fake, true, status) == Outcome::command_failed && sends == 2);
    const PeerPin synthetic{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}; // Test-only identity.
    auto capture = [&](IOUSBDevRequestTO &req) -> IOReturn {
        if (req.bRequest == probe_request)
            return fake(req);
        ++sends;
        expect(req.bmRequestType == probe_type && req.bRequest == capture_request &&
               req.wLength == 8 && req.wValue == request_value && req.wIndex == request_index);
        auto *bytes = static_cast<std::uint8_t *>(req.pData);
        bytes[0] = fault == 5 ? 2 : 1;
        bytes[1] = fault == 6 ? 6 : 7;
        std::copy(synthetic.begin(), synthetic.end(), bytes + 2);
        if (fault == 8 || fault == 9)
            std::fill(bytes + 2, bytes + 8, fault == 8 ? 0 : 0xff);
        req.wLenDone = fault == 7 ? 7 : 8;
        return fault == 10 ? kIOReturnNotPermitted : kIOReturnSuccess;
    };
    PeerPin captured{};
    fault = 0;
    sends = 0;
    expect(capturePin(capture, status, captured) && captured == synthetic && sends == 2);
    for (fault = 5; fault <= 10; ++fault)
        expect(!capturePin(capture, status, captured) && captured == PeerPin{});
    fault = 1;
    sends = 0;
    expect(!capturePin(capture, status, captured) && sends == 1);
    expect(!savePrivatePin(PeerPin{}));
    const auto file = savePrivatePin(synthetic);
    expect(file.has_value() && file->find("/private/tmp/omni-chair-pin-") == 0);
    const std::string dir = file->substr(0, file->rfind('/'));
    struct stat info{};
    expect(lstat(dir.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           (info.st_mode & 0777) == 0700);
    expect(lstat(file->c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           (info.st_mode & 0777) == 0600 && info.st_size == 6);
    const int fd = open(file->c_str(), O_RDONLY | O_NOFOLLOW);
    expect(fd >= 0 && read(fd, captured.data(), captured.size()) == 6 && captured == synthetic);
    expect(close(fd) == 0 && unlink(file->c_str()) == 0 && rmdir(dir.c_str()) == 0);
    // These are numeric deltas between observations, independent of host or Pico clocks.
    using namespace omni::motion_watch;
    MotionReply zero{};
    zero[0] = 1;
    zero[1] = 64;
    auto before = decode(zero);
    expect(before && before->counters[raw] == 0 && before->outcome == 0);
    MotionReply sample = zero;
    sample[2] = 1;
    sample[3] = 4;
    auto storeCount = [&](Counter field, std::uint32_t n) {
        auto at = 4 + 4 * static_cast<unsigned>(field);
        for (unsigned i = 0; i < 4; ++i)
            sample[at + i] = static_cast<std::uint8_t>(n >> (i * 8));
    };
    storeCount(raw, 0x12346);
    storeCount(accepted, 0x12344);
    storeCount(left, 9);
    storeCount(down, 7);
    storeCount(wheel_negative, 3);
    storeCount(button_pressed, 1);
    storeCount(button_released, 1);
    storeCount(zero_only, 12);
    sample[56] = 0xfc;
    sample[57] = 0xff;
    sample[58] = 0x00;
    sample[59] = 0x80;
    sample[60] = 0xfe;
    sample[61] = 0xff;
    sample[62] = 2;
    sample[63] = 4;
    auto after = decode(sample);
    expect(after && after->counters[left] == 9 && after->counters[down] == 7 &&
           after->counters[raw] == 0x12346);
    expect(after->last_x == -4 && after->last_y == -32768 && after->last_wheel == -2 &&
           after->last_buttons == 2);
    before->counters[left] = 4;
    before->counters[down] = 6;
    before->counters[raw] = 0x12345;
    auto delta = between(*before, *after);
    expect(!delta.reset && delta.changed() && delta.counts[left] == 5 && delta.counts[down] == 1 &&
           delta.counts[raw] == 1 && delta.counts[right] == 0 &&
           delta.counts[wheel_negative] == 3 && delta.counts[button_pressed] == 1 &&
           delta.counts[zero_only] == 12);
    expect(!between(*after, *after).changed() && between(*after, *before).reset);
    after->counters[left] = UINT32_MAX;
    delta = between(*before, *after);
    expect(delta.lower_bounds[left] && delta.counts[left] == UINT32_MAX - 4);
    expect(watchDuration("2") == 2 && watchDuration("180") == 180 && !watchDuration("181") &&
           !watchDuration("0") && !watchDuration("2s"));
    unsigned motionReads = 0;
    auto motionFake = [&](IOUSBDevRequestTO &req) -> IOReturn {
        if (req.bRequest == probe_request)
            return fake(req);
        ++motionReads;
        expect(req.bmRequestType == probe_type && req.bRequest == motion_request &&
               req.wLength == 64 && req.wValue == request_value && req.wIndex == request_index &&
               req.pData);
        std::memcpy(req.pData, sample.data(), sample.size());
        req.wLenDone = fault == 11 ? 63 : 64;
        return fault == 12 ? kIOReturnNotPermitted : kIOReturnSuccess;
    };
    fault = 0;
    expect(readMotion(motionFake, status)->counters[down] == 7 && motionReads == 1);
    fault = 1;
    expect(!readMotion(motionFake, status) &&
           motionReads == 1); // failed capability skips second read.
    fault = 11;
    expect(!readMotion(motionFake, status));
    fault = 12;
    expect(!readMotion(motionFake, status));
    fault = 0;
    sample[1] = 63;
    expect(!readMotion(motionFake, status));
    sample[1] = 64;
    sample[2] = 4;
    expect(!readMotion(motionFake, status));
    sample[2] = 1;
    sample[0] = 2;
    expect(!readMotion(motionFake, status));
    nudge::Wire reply{};
    nudge::Slots defaults{12, 4, 5, 3}, wanted{0, 11, 3, 12};
    std::uint32_t saved_revision = 7;
    nudge::Slots saved_slots = defaults;
    unsigned storage = 1, result = 2, reads = 0, writes = 0, polls = 0;
    bool queued = false, short_read = false, wrong_slots = false, never_apply = false;
    unsigned write_failure = 0;
    auto nudgeFake = [&](IOUSBDevRequestTO &req) -> IOReturn {
        if (req.bRequest == probe_request)
            return fake(req);
        expect(req.wValue == request_value && req.wIndex == request_index && req.wLength == 16);
        if (req.bRequest == nudge::get_request && req.bmRequestType == probe_type) {
            ++reads;
            reply.fill(0);
            std::memcpy(reply.data(), "NGR1", 4);
            for (unsigned i = 0; i < 4; ++i) {
                reply[4 + i] = static_cast<std::uint8_t>(saved_revision >> (8 * i));
                reply[8 + i] = saved_slots[i];
            }
            reply[12] = storage;
            reply[13] = result;
            std::memcpy(req.pData, reply.data(), reply.size());
            req.wLenDone = (short_read ? 15 : 16);
            return kIOReturnSuccess;
        }
        expect(req.bRequest == nudge::set_request && req.bmRequestType == enter_type);
        nudge::Wire command{};
        std::memcpy(command.data(), req.pData, 16);
        expect(command == nudge::encode(saved_revision, wanted) &&
               !std::memcmp(command.data(), "NGW1", 4) &&
               !std::memcmp(command.data() + 12, "SAVE", 4));
        ++writes;
        req.wLenDone = 16;
        result = 1;
        queued = true;
        return kIOReturnSuccess;
    };
    auto wait = [&] {
        ++polls;
        if (!queued || never_apply || polls % 2)
            return;
        queued = false;
        if (write_failure) {
            result = write_failure;
            return;
        }
        ++saved_revision;
        storage = 1;
        result = 2;
        saved_slots = wrong_slots ? defaults : wanted;
    };
    fault = 0;
    nudge::Record confirmed;
    expect(readNudges(nudgeFake)->slots == defaults && writes == 0);
    expect(!saveNudges(nudgeFake, 7, wanted, confirmed, wait) && writes == 1 &&
           confirmed.revision == 8 && confirmed.slots == wanted && polls >= 2);
    expect(saveNudges(nudgeFake, 7, wanted, confirmed, wait) != nullptr && writes == 1);
    storage = 3;
    saved_slots = {};
    expect(saveNudges(nudgeFake, 8, wanted, confirmed, wait) != nullptr && writes == 1);
    storage = 2;
    saved_slots = {};
    saved_revision = 0;
    result = 0;
    expect(!saveNudges(nudgeFake, 0, wanted, confirmed, wait) && writes == 2 &&
           saved_revision == 1);
    write_failure = 6;
    expect(saveNudges(nudgeFake, 1, wanted, confirmed, wait) != nullptr && writes == 3);
    write_failure = 0;
    wrong_slots = true;
    result = 2;
    expect(saveNudges(nudgeFake, 1, wanted, confirmed, wait) != nullptr && writes == 4 &&
           saved_revision == 2);
    wrong_slots = false;
    never_apply = true;
    result = 2;
    expect(saveNudges(nudgeFake, 2, wanted, confirmed, wait) != nullptr && writes == 5);
    short_read = true;
    expect(!readNudges(nudgeFake));
    expect(!nudge::number("13", 12) && !nudge::number("-1", 12) &&
           !nudge::number("4294967296", UINT32_MAX) && nudge::number("12", 12) == 12);
    reply.fill(0);
    std::memcpy(reply.data(), "NGR1", 4);
    expect(nudge::decode(reply).has_value());
    reply[14] = 1;
    expect(!nudge::decode(reply));
    reply[14] = 0;
    reply[8] = 13;
    expect(!nudge::decode(reply));
    reply[8] = 12;
    reply[12] = 2;
    expect(!nudge::decode(reply));
    std::printf("PASS: %u Mac maintenance dry checks, synthetic private file only. No USB hardware "
                "accessed.\n",
                passed);
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        return selfTest();
    const bool probe = argc == 2 && std::string_view(argv[1]) == "--probe";
    const bool capture = argc == 3 && std::string_view(argv[1]) == "--capture-chair-pin";
    const bool get = argc == 3 && std::string_view(argv[1]) == "--get-nudges";
    const bool set = argc == 8 && std::string_view(argv[1]) == "--set-nudges";
    const auto revision = set ? nudge::number(argv[3], UINT32_MAX - 1) : std::nullopt;
    nudge::Slots slots{};
    if (set) {
        if (!revision) {
            std::fprintf(stderr, "%s", usage);
            return 2;
        }
        for (unsigned i = 0; i < 4; ++i) {
            auto slot = nudge::number(argv[4 + i], 12);
            if (!slot) {
                std::fprintf(stderr, "%s", usage);
                return 2;
            }
            slots[i] = static_cast<std::uint8_t>(*slot);
        }
    }
    const auto seconds = argc == 4 && std::string_view(argv[1]) == "--watch-chair-motion"
                             ? watchDuration(argv[3])
                             : std::nullopt;
    const auto expected =
        (seconds || get || set ||
         (argc == 3 && (capture || std::string_view(argv[1]) == "--enter-bootsel")))
            ? locationFrom(argv[2])
            : std::nullopt;
    if (!probe && !expected) {
        std::fprintf(stderr, "%s", usage);
        return 2;
    }
    alarm(seconds.value_or(0) + 8);
    Target target;
    if (!findTarget(target) || !authorized(target.count, target.location, expected)) {
        std::fprintf(stderr, "Expected exactly one Omni Right Stick matching the selected Mac USB "
                             "location. No command sent.\n");
        return 2;
    }
    Device device;
    if (!device.connect(target)) {
        std::fprintf(stderr, "Cannot access the selected USB control pipe. No command sent.\n");
        return 2;
    }
    IOReturn status = 0;
    if (get) {
        auto settings = readNudges(device);
        if (!settings) {
            std::fprintf(stderr, "This Pico did not return nudge settings. Check the connection "
                                 "and firmware version.\n");
            return 1;
        }
        nudge::print(*settings);
        return 0;
    }
    if (set) {
        nudge::Record result;
        if (auto error = saveNudges(device, *revision, slots, result, [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            })) {
            std::fprintf(stderr, "%s\n", error);
            return 1;
        }
        nudge::print(result);
        return 0;
    }
    if (seconds)
        return watchMotion(device, *seconds);
    if (capture) {
        PeerPin pin{};
        if (!capturePin(device, status, pin)) {
            std::fprintf(stderr,
                         "Capture unavailable. The strict firmware must have recently confirmed an "
                         "eligible Mouse 2 peer at seven bytes. No pin file saved.\n");
            return 1;
        }
        auto file = savePrivatePin(pin);
        if (!file) {
            std::fprintf(stderr, "Could not save the private pin file.\n");
            return 1;
        }
        std::printf("Saved private chair pin file: %s\n", file->c_str());
        return 0;
    }
    switch (operate(device, !probe, status)) {
    case Outcome::supported:
        std::printf("Maintenance supported. Mac USB location 0x%08x. Probe only.\n",
                    target.location);
        return 0;
    case Outcome::requested:
        std::printf("Omni accepted BOOTSEL at 0x%08x. Confirm RPI-RP2 appears before any update.\n",
                    target.location);
        return 0;
    case Outcome::missing_capability:
        std::fprintf(stderr,
                     "Capability unavailable or macOS denied USB access (0x%08x). No BOOTSEL "
                     "request sent.\n",
                     unsigned(status));
        return 1;
    case Outcome::command_failed:
        std::fprintf(
            stderr,
            "BOOTSEL request was not confirmed (0x%08x). Check for RPI-RP2 before retrying.\n",
            unsigned(status));
        return 1;
    }
    return 1;
}
