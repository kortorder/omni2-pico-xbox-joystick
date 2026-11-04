// The Mac observer receives USB joystick input and reads the diagnostic Feature report.
#include "target_identity.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;
using Bytes = std::uint8_t;
struct Axes {
    int x, y;
    unsigned buttons = 0;
    bool neutral() const { return x == 0 && y == 0 && buttons == 0; }
};
struct Status {
    unsigned state;
    std::uint32_t reports;
    Bytes stage = 0, detail = 0;
};
constexpr const char* stateNames[]{"starting","unpaired","searching","connecting","connected","reconnecting","fault"};
std::optional<Status> decodeStatus(const Bytes* p, size_t n) {
    if (!p || n != 8 || p[0] != 1 || p[1] >= 7) return {};
    return Status{p[1], std::uint32_t(p[2]) | (std::uint32_t(p[3]) << 8) |
                        (std::uint32_t(p[4]) << 16) | (std::uint32_t(p[5]) << 24),
                  p[6], p[7]};
}

std::string hexByte(Bytes value) {
    char text[5];
    std::snprintf(text, sizeof(text), "0x%02X", unsigned(value));
    return text;
}

// Stage codes are the two optional tail bytes of Feature v1. A failure
// describes the retained diagnostic, whereas status.state is the live state.
std::string pairingDetail(const Status& s) {
    const auto n = std::to_string(s.detail);
    const auto code = hexByte(s.detail);
    switch (s.stage) {
        case 0: return s.detail == 0 ? "no stage detail available" : "unknown detail " + code;
        case 1: return "inquiry started (scan " + n + ")";
        case 2: return "inquiry completed (" + n + " results)";
        case 3: return "remote name lookup (checked " + n + ")";
        case 4:
            if (s.detail == 1) return "selected R-net Mouse 2";
            if (s.detail == 2) return "selected Mouse 2 with pointing-device class";
            return "selected candidate (type " + n + ")";
        case 5: return "HID connection started (attempt " + n + ")";
        case 6: return "base Bluetooth link connected";
        case 7: return "PIN response sent";
        case 8: return "secure pairing confirmation sent";
        case 9: return "authentication completed";
        case 10: return "encryption established (security level " + n + ")";
        case 11: return "link key observed (key type " + n + ")";
        case 12: return s.detail == 0 ? "outgoing HID opened" : "incoming HID opened";
        case 13: return "relative-mouse descriptor accepted";
        case 14: return "HID report protocol confirmed";
        case 15: return "mouse ready";
        case 16: return "mouse report received (report ID " + n + ")";
        case 0x80: return "last failure: inquiry start, status " + code;
        case 0x81: return "last failure: ambiguous slot selection (" + n + " candidates)";
        case 0x82: return "last failure: HID connection start, status " + code;
        case 0x83: return "last failure: base Bluetooth link, HCI status " + code;
        case 0x84: return "last failure: authentication, HCI status " + code;
        case 0x85: return "last failure: encryption, HCI status " + code;
        case 0x86: return "last failure: HID open, status " + code;
        case 0x87: return "last failure: HID descriptor retrieval, status " + code;
        case 0x88: return "last failure: unsupported mouse descriptor (size capped at 255: " + n + ")";
        case 0x89: return "last failure: report protocol handshake, status " + code;
        case 0x8a: {
            const char* checks[] = {"HID-open", "key", "encryption", "descriptor", "protocol"};
            std::string missing;
            for (unsigned i = 0; i < 5; ++i) {
                if ((s.detail & (1u << i)) == 0) {
                    if (!missing.empty()) missing += ", ";
                    missing += checks[i];
                }
            }
            return "last failure: connection timeout; missing " +
                   (missing.empty() ? std::string("none of the five tracked prerequisites") : missing) +
                   " (flags " + code + ")";
        }
        case 0x8b: return "last failure: Bluetooth disconnected, reason " + code;
        case 0x8c: return "last failure: malformed mouse report (report ID " + n + ")";
        case 0x8d:
            if (s.detail == 1) return "last failure: could not load stored pairing";
            if (s.detail == 2) return "last failure: could not save pairing";
            if (s.detail == 3) return "last failure: saved link key missing";
            return "last failure: pairing storage, detail " + code;
        case 0x8e: return "last failure: link teardown timed out";
        case 0x8f: return "last failure: malformed HID " +
                          (s.detail == 1 ? std::string("framing") :
                           s.detail == 2 ? std::string("length") : "detail " + code);
        case 0x90: return "last failure: Bluetooth security request, status " + code;
        case 0x91: return "last failure: below required security level, reported encryption key size " + n + " bytes";
        default: return "unknown stage " + hexByte(s.stage) + ", detail " + code;
    }
}
std::optional<Axes> decodeAxes(const Bytes* p, size_t n) {
    if (!p || (n != 4 && n != 6) || (n == 6 && (p[5] & 0xf0))) return {};
    auto integer = [](Bytes low, Bytes high) {
        int raw = int(low) + 256 * int(high);
        return raw >= 32768 ? raw - 65536 : raw;
    };
    Axes axes{integer(p[0],p[1]), integer(p[2],p[3])};
    if (n == 6) axes.buttons = p[4] | (unsigned(p[5]) << 8);
    if (axes.x == -32768 || axes.y == -32768) return {};
    return axes;
}
enum Side { right, left, up, down, neither };
constexpr const char* directions[]{"RIGHT","LEFT","UP","DOWN"};
// Require substantial signed motion on exactly one dominant axis. The cross-axis
// tolerance accommodates a human hand but rejects a diagonal as independent proof.
Side side(Axes a) {
    int x = std::abs(a.x), y = std::abs(a.y);
    if (x >= 512 && y <= x/4) return a.x > 0 ? right : left;
    if (y >= 512 && x <= y/4) return a.y > 0 ? down : up;
    return neither;
}

struct Trace {
    enum class Phase { baseline, move, center, finished };
    Phase phase = Phase::baseline;
    int completed = 0;
    std::optional<long> firstNeutral;
    long lastInput = 0;
    unsigned neutralSamples = 0, inputs = 0;
    bool initialNonneutral = false;
    void input(Axes a, long ms) {
        ++inputs;
        lastInput = ms;
        if (inputs == 1 && !a.neutral()) initialNonneutral = true;
        if (a.neutral()) {
            if (!firstNeutral) firstNeutral = ms;
            ++neutralSamples;
            if (neutralSamples >= 2 && ms - *firstNeutral >= 120) {
                if (phase == Phase::baseline) phase = Phase::move;
                else if (phase == Phase::center) phase = ++completed == 4 ? Phase::finished : Phase::move;
            }
            return;
        }
        firstNeutral.reset(); neutralSamples = 0;
        Side s = side(a);
        if (phase == Phase::move && s == completed) phase = Phase::center;
        // Crossing into a different cardinal before a clean idle cancels this trial.
        else if (phase == Phase::center && s != neither && s != completed) phase = Phase::move;
    }
    bool settled() const {
        return firstNeutral && neutralSamples >= 2 && lastInput - *firstNeutral >= 120;
    }
};

struct Session {
    explicit Session(bool directionsMode) : directionsMode(directionsMode) {}
    bool directionsMode, failed = false, removed = false;
    size_t inputBytes = 4;
    Clock::time_point start = Clock::now();
    Trace trace;
    std::optional<Status> status;
    std::optional<std::uint32_t> baselineCount;
    bool countAdvanced = false, finalFeatureRead = false;
    std::optional<Axes> last;
    long lastPrinted = -1000;
    long lastFeaturePrinted = -5000;
    long ms() const { return std::chrono::duration_cast<Millis>(Clock::now() - start).count(); }
    void hint() const {
        if (!directionsMode) return;
        if (trace.phase == Trace::Phase::move)
            std::printf("  Move %s in Bluetooth Mouse mode.\n", directions[trace.completed]);
        else if (trace.phase == Trace::Phase::center)
            std::printf("  Release to idle and let both axes return to zero.\n");
        else if (trace.phase == Trace::Phase::finished)
            std::printf("  All four cardinal/idle sequences observed. Checking final Bluetooth status.\n");
    }
    bool passed() const {
        if (failed || removed || trace.initialNonneutral || !status || status->state == 6) return false;
        if (!trace.settled()) return false;
        if (!directionsMode) return true;
        return trace.phase == Trace::Phase::finished && finalFeatureRead && status->state == 4 && countAdvanced;
    }
};
void inputReport(void* context, IOReturn result, void*, IOHIDReportType type, uint32_t reportID,
                 Bytes* bytes, CFIndex n) {
    auto& s = *static_cast<Session*>(context);
    auto axes = decodeAxes(bytes, n);
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || reportID != 0 ||
        n != static_cast<CFIndex>(s.inputBytes) || !axes) {
        std::fprintf(stderr, "Rejected unexpected input report. result=0x%x type=%d id=%u bytes=%ld\n",
                     unsigned(result), int(type), reportID, long(n));
        s.failed = true; return;
    }
    auto before = s.trace.phase;
    auto t = s.ms();
    s.trace.input(*axes, t);
    if (!s.last || before != s.trace.phase || s.last->neutral() != axes->neutral() ||
        s.last->buttons != axes->buttons ||
        (!axes->neutral() && t - s.lastPrinted >= 200)) {
        std::printf("%6ld ms USB INPUT x=%d y=%d buttons=0x%03x\n",t,axes->x,axes->y,axes->buttons);
        s.lastPrinted = t;
    }
    if (s.trace.initialNonneutral) {
        std::fprintf(stderr, "First USB input was nonneutral. Return controls to rest and restart the observation.\n");
        s.failed = true;
    } else if (before != s.trace.phase) s.hint();
    s.last = axes;
    std::fflush(stdout);
}
void removed(void* context, IOReturn, void*) {
    static_cast<Session*>(context)->removed = true;
    std::fprintf(stderr, "The selected Omni Right Stick disconnected during capture.\n");
}
bool getDiagnostic(IOHIDDeviceRef d, Session& session) {
    std::array<Bytes,8> bytes{}; CFIndex size = bytes.size();
    auto rc = IOHIDDeviceGetReport(d,kIOHIDReportTypeFeature,0,bytes.data(),&size);
    auto status = decodeStatus(bytes.data(),size);
    if (rc != kIOReturnSuccess || !status) {
        std::fprintf(stderr,"Cannot validate diagnostic feature v1 (result=0x%x, bytes=%ld).\n",unsigned(rc),long(size));
        return false;
    }
    const auto now = session.ms();
    bool firstMouse = !session.countAdvanced && session.baselineCount &&
                      status->reports != *session.baselineCount;
    if (!session.status || status->state != session.status->state ||
        status->stage != session.status->stage || status->detail != session.status->detail || firstMouse ||
        (status->reports != session.status->reports && now - session.lastFeaturePrinted >= 5000)) {
        std::printf("%6ld ms FEATURE Bluetooth=%s mouseReports=%u stage=%s detail=%s (%s)\n",
                    now,stateNames[status->state],status->reports,hexByte(status->stage).c_str(),
                    hexByte(status->detail).c_str(),pairingDetail(*status).c_str());
        session.lastFeaturePrinted = now;
        std::fflush(stdout);
    }
    if (!session.baselineCount) session.baselineCount = status->reports;
    session.countAdvanced |= status->reports != *session.baselineCount;
    session.finalFeatureRead = session.trace.phase == Trace::Phase::finished;
    session.status = status;
    if (status->state == 6) { std::fprintf(stderr,"Pico reports Bluetooth fault.\n"); return false; }
    return true;
}
size_t inputLayout(IOHIDDeviceRef device) {
    if (!IOHIDDeviceConformsTo(device,1,4)) return 0;
    auto elements = IOHIDDeviceCopyMatchingElements(device,nullptr,kIOHIDOptionsTypeNone);
    if (!elements) return false;
    unsigned mask = 0, buttons = 0, inputs = 0;
    for (CFIndex i=0; i<CFArrayGetCount(elements); ++i) {
        auto e = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements,i)));
        auto t = IOHIDElementGetType(e);
        if (t < kIOHIDElementTypeInput_Misc || t > kIOHIDElementTypeInput_ScanCodes) continue;
        ++inputs;
        auto usage = IOHIDElementGetUsage(e);
        if (IOHIDElementGetUsagePage(e)==9 && usage>=1 && usage<=12 &&
            !IOHIDElementGetReportID(e) && IOHIDElementGetReportSize(e)==1 &&
            !IOHIDElementIsRelative(e) && IOHIDElementGetLogicalMin(e)==0 &&
            IOHIDElementGetLogicalMax(e)==1) {
            buttons |= 1u << (usage-1);
            continue;
        }
        if (IOHIDElementGetUsagePage(e) != 1 || (usage != 0x30 && usage != 0x31) ||
            IOHIDElementGetReportID(e) || IOHIDElementGetReportSize(e) != 16 || IOHIDElementIsRelative(e) ||
            IOHIDElementGetLogicalMin(e) != -32767 || IOHIDElementGetLogicalMax(e) != 32767) continue;
        mask |= usage == 0x30 ? 1 : 2;
    }
    CFRelease(elements);
    if (mask == 3 && inputs == 14 && buttons == 0xfff) return 6;
    if (mask == 3 && inputs == 2 && !buttons) return 4;
    return 0;
}
void number(CFMutableDictionaryRef d, CFStringRef key, int value) {
    auto n=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&value);
    CFDictionarySetValue(d,key,n); CFRelease(n);
}

int observe(bool directionsMode, bool watch, unsigned seconds) {
    Session session(directionsMode);
    auto deadline = session.start + std::chrono::seconds(seconds);
    // A USB driver's synchronous control transfer can stall. Guarantee a hard
    // process bound as well as the ordinary per-loop deadline below.
    alarm(seconds + 2);
    std::array<Bytes,64> buffer{};
    auto manager = IOHIDManagerCreate(kCFAllocatorDefault,kIOHIDManagerOptionNone);
    auto match = CFDictionaryCreateMutable(kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    number(match,CFSTR(kIOHIDVendorIDKey),omni_mac::vid); number(match,CFSTR(kIOHIDProductIDKey),omni_mac::pid);
    CFDictionarySetValue(match,CFSTR(kIOHIDTransportKey),CFSTR("USB"));
    CFDictionarySetValue(match,CFSTR(kIOHIDManufacturerKey),CFSTR("SimpleController prototype"));
    CFDictionarySetValue(match,CFSTR(kIOHIDProductKey),CFSTR("Omni Right Stick"));
    IOHIDManagerSetDeviceMatching(manager,match); CFRelease(match);
    IOHIDManagerScheduleWithRunLoop(manager,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    IOReturn rc=IOHIDManagerOpen(manager,kIOHIDOptionsTypeNone);
    IOHIDDeviceRef device=nullptr;
    int outcome=2;
    if (rc != kIOReturnSuccess) std::fprintf(stderr,"Cannot open target HID service (0x%x).\n",unsigned(rc));
    else {
        std::printf("Listening for one USB %04x:%04x Omni Right Stick, up to %u seconds. Leave controls at rest.\n",omni_mac::vid,omni_mac::pid,seconds);
        std::fflush(stdout);
        auto nextStatus=session.start;
        while (Clock::now()<deadline && !session.failed && !session.removed && (watch || !session.passed())) {
            if (!device) {
                auto devices = IOHIDManagerCopyDevices(manager);
                if (devices) {
                    auto count = CFSetGetCount(devices);
                    if (count>1) { std::fprintf(stderr,"More than one matching Pico.\n"); session.failed=true; }
                    if (count==1) {
                        const void* value=nullptr; CFSetGetValues(devices,&value);
                        device=static_cast<IOHIDDeviceRef>(const_cast<void*>(CFRetain(value)));
                        session.inputBytes = inputLayout(device);
                        if (!session.inputBytes) { std::fprintf(stderr,"Unexpected joystick input layout.\n"); session.failed=true; }
                        else {
                            IOHIDDeviceRegisterInputReportCallback(device,buffer.data(),buffer.size(),inputReport,&session);
                            IOHIDDeviceRegisterRemovalCallback(device,removed,&session);
                            std::printf("Matched USB joystick with signed X/Y and %u buttons. Waiting for real input reports.\n",session.inputBytes==6 ? 12u : 0u);
                            std::fflush(stdout);
                        }
                    }
                    CFRelease(devices);
                }
            }
            if (device && !session.failed && Clock::now()>=nextStatus) {
                session.failed=!getDiagnostic(device,session);
                nextStatus=Clock::now()+Millis(500);
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.05,false);
        }
        bool captured = !session.failed && !session.removed && session.status && session.trace.inputs > 0;
        outcome = (watch ? captured : session.passed()) ? 0 : 1;
        std::printf("%s.%s inputs=%u, verified cardinal/idle pairs=%d/4, elapsed=%ld ms\n",
            outcome ? "INCOMPLETE" : (watch ? "CAPTURED" : "PASS"),
            watch ? " Observation" : (directionsMode ? " Directional capture" : " USB-neutral/feature probe"),session.trace.inputs,
            session.trace.completed,session.ms());
        if (!device) std::fprintf(stderr,"Target was not observed before deadline.\n");
    }
    IOHIDManagerUnscheduleFromRunLoop(manager,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    IOHIDManagerClose(manager,kIOHIDOptionsTypeNone);
    if (device) CFRelease(device);
    CFRelease(manager); alarm(0);
    return outcome;
}

int selfTest() {
    unsigned count=0;
    auto ck=[&](bool ok,const char* name) {
        if (!ok) {std::fprintf(stderr,"FAILED %s\n",name); std::exit(1);}
        ++count;
    };
    Bytes v[]{1,4,0xef,0xcd,0xab,0x89,0,0};
    ck(decodeStatus(v,8)->reports==0x89abcdefu,"feature little endian");
    for(unsigned state=0;state<=6;++state) { v[1]=state; ck(decodeStatus(v,8)->state==state,"state accepted"); }
    v[1]=7; ck(!decodeStatus(v,8),"future state rejects"); v[1]=4; v[0]=2;
    ck(!decodeStatus(v,8),"unknown version"); v[0]=1;
    ck(pairingDetail(*decodeStatus(v,8))=="no stage detail available","legacy all-zero report");
    v[6]=2; v[7]=0;
    ck(pairingDetail(*decodeStatus(v,8))=="inquiry completed (0 results)","scan with no results");
    v[6]=0x84;v[7]=5;
    ck(decodeStatus(v,8)->reports==0x89abcdefu &&
       pairingDetail(*decodeStatus(v,8))=="last failure: authentication, HCI status 0x05",
       "retained failure with exact HCI status and counter");
    v[6]=0x8a; v[7]=9;
    ck(pairingDetail(*decodeStatus(v,8))=="last failure: connection timeout; missing key, encryption, protocol (flags 0x09)",
       "timeout flags describe the unmet prerequisites");
    v[6]=0xee;v[7]=0xaa;
    ck(pairingDetail(*decodeStatus(v,8))=="unknown stage 0xEE, detail 0xAA","future stage preserves base status");
    v[6]=0x90;v[7]=0x05;
    ck(pairingDetail(*decodeStatus(v,8))=="last failure: Bluetooth security request, status 0x05",
       "security request failure keeps raw HCI reason");
    v[6]=0x91;v[7]=0;
    ck(pairingDetail(*decodeStatus(v,8))=="last failure: below required security level, reported encryption key size 0 bytes",
       "insufficient security with unavailable key length");
    ck(!decodeStatus(v,7),"bad diagnostic length");
    Bytes p[]{1,128,255,127}; auto a=decodeAxes(p,4);
    ck(a && a->x==-32767 && a->y==32767,"signed extremes"); p[0]=0;
    ck(!decodeAxes(p,4),"forbidden negative endpoint"); ck(!decodeAxes(p,3),"short axis input");
    Bytes withButtons[]{0,0,0,0,0x01,0x08};
    auto with=decodeAxes(withButtons,6);
    ck(with && with->buttons==0x801 && !with->neutral(),"slots 1/12 count as nonneutral input");
    withButtons[4]=0;withButtons[5]=0;ck(decodeAxes(withButtons,6)->neutral(),"released buttons centered");
    withButtons[5]=0x10;ck(!decodeAxes(withButtons,6),"nonzero button padding rejected");
    ck(side({1000,251})==neither && side({-8000,1000})==left,"independent axis threshold");
    Trace t;
    t.input({0,0},0); t.input({0,0},80); ck(t.phase==Trace::Phase::baseline,"short idle");
    t.input({0,0},120); ck(t.phase==Trace::Phase::move,"sustained neutral");
    t.input({-5000,0},150); t.input({5000,5000},170); ck(t.phase==Trace::Phase::move,"wrong/diagonal");
    t.input({5000,0},180); t.input({0,-7000},190); ck(t.phase==Trace::Phase::move,"mixed trial invalidated");
    std::array<Axes,4> moves{{{5000,0},{-5000,20},{12,-5000},{0,5000}}};
    long stamp=200;
    for(auto m : moves) {
        t.input(m,stamp); ck(t.phase==Trace::Phase::center,"correct cardinal");
        t.input({0,0},stamp+20); t.input({0,0},stamp+130);
        ck(t.phase==Trace::Phase::center,"less than required interval");
        t.input({0,0},stamp+140); stamp+=200;
    }
    ck(t.completed==4 && t.phase==Trace::Phase::finished,"four independent cardinal/idle traces");
    Trace first; first.input({0,1024},0); ck(first.initialNonneutral,"bad first sample");
    Session partial(true); partial.trace=t; partial.status=Status{4,4}; partial.countAdvanced=true;
    ck(!partial.passed(),"requires fresh final status"); partial.finalFeatureRead=true;
    ck(partial.passed(),"valid full result"); partial.status=Status{5,4};
    ck(!partial.passed(),"reconnecting cannot pass movement");
    partial.status=Status{4,5}; partial.trace.input({1,0},1100);
    ck(!partial.passed(),"new motion invalidates final idle");
    std::printf("%u dry assertions passed. No device opened.\n",count);
    return 0;
}
} // namespace

int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--self-test") return selfTest();
    if (argc==3 && (std::string_view(argv[1])=="--probe" || std::string_view(argv[1])=="--directions" || std::string_view(argv[1])=="--watch")) {
        char* end=nullptr; long n=std::strtol(argv[2],&end,10);
        if (end!=argv[2] && *end==0 && n>=2 && n<=600) return observe(std::string_view(argv[1])=="--directions",std::string_view(argv[1])=="--watch",unsigned(n));
    }
    std::fprintf(stderr,"Usage: omni-observer --self-test | --probe SECONDS | --watch SECONDS | --directions SECONDS (2..600)\n");
    return 2;
}
