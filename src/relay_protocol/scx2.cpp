// Original project implementation; specification/design references are in ../PROTOCOL.md.
#include "scx2.hpp"
#include <algorithm>
#include <limits>

namespace scx2 {
namespace {
bool nonzero(const Token& t) { return std::any_of(t.begin(), t.end(), [](auto b) { return b != 0; }); }
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
std::uint16_t read16(const std::uint8_t* p) { return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]); }
void write16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v); }
std::int16_t signed16(std::uint16_t v) { return static_cast<std::int16_t>(v < 0x8000 ? std::int32_t(v) : std::int32_t(v) - 65536); }
}
bool State::valid() const {
    return (buttons & 0xfc00) == 0 && hat <= 8 &&
        std::none_of(axes.begin(), axes.end(), [](auto a) { return a == -32768; });
}
bool State::neutral() const { return *this == State{}; }
bool State::operator==(const State& o) const { return buttons == o.buttons && hat == o.hat && axes == o.axes && triggers == o.triggers; }
bool valid(const Packet& p) {
    if (!nonzero(p.session) || (p.format!=Format::legacy && p.format!=Format::pressure && p.format!=Format::usb_buttons && p.format!=Format::usb_pressure)) return false;
    const auto capability=p.format==Format::pressure ? proportional_triggers : (p.format==Format::usb_buttons ? usb_button_triggers : (p.format==Format::usb_pressure ? usb_proportional_triggers : 0));
    if (p.format==Format::legacy && (p.capabilities || !p.state.triggers.neutral())) return false;
    if (p.format==Format::usb_buttons &&
        ((p.state.triggers.left!=0 && p.state.triggers.left!=65535) || (p.state.triggers.right!=0 && p.state.triggers.right!=65535))) return false;
    if (p.kind!=Kind::state && !p.state.neutral()) return false;
    if (p.capabilities && ((p.kind!=Kind::hello && p.kind!=Kind::ready) || p.capabilities!=capability)) return false;
    switch (p.kind) {
    case Kind::hello: return p.sequence == 0 && !p.hid_ready && !nonzero(p.boot) && !nonzero(p.challenge) && p.capabilities==capability;
    case Kind::ready: return p.sequence == 0 && nonzero(p.boot) && nonzero(p.challenge) == p.hid_ready && p.capabilities==(p.hid_ready ? capability : 0);
    case Kind::state: return p.sequence > 0 && p.sequence < maximum && !p.hid_ready && nonzero(p.boot) && nonzero(p.challenge) && p.state.valid();
    case Kind::ack: return p.sequence > 0 && p.sequence < maximum && p.hid_ready && nonzero(p.boot) && nonzero(p.challenge);
    case Kind::stop: case Kind::stopped: return p.sequence > 0 && !p.hid_ready && nonzero(p.boot) && nonzero(p.challenge);
    }
    return false;
}
std::optional<Wire> encode(const Packet& p) {
    if (!valid(p)) return std::nullopt;
    Wire w; auto* b = w.bytes.data(); w.size = p.kind == Kind::state ? (p.format!=Format::legacy ? 80 : 76) : 64;
    b[0]='S'; b[1]='C'; b[2]='X'; b[3]='2'; b[4]=std::uint8_t(p.format); b[5]=std::uint8_t(p.kind); b[6]=p.hid_ready ? 1 : 0; b[7]=p.capabilities;
    std::copy(p.session.begin(), p.session.end(), b+8); std::copy(p.boot.begin(), p.boot.end(), b+24);
    std::copy(p.challenge.begin(), p.challenge.end(), b+40);
    for (unsigned i=0; i<8; ++i) b[56+i]=std::uint8_t(p.sequence >> (56-8*i));
    if (p.kind == Kind::state) {
        write16(b+64, p.state.buttons); b[66]=p.state.hat;
        for (unsigned i=0; i<4; ++i) write16(b+68+2*i, static_cast<std::uint16_t>(p.state.axes[i]));
        if (p.format!=Format::legacy) { write16(b+76,p.state.triggers.left); write16(b+78,p.state.triggers.right); }
    }
    return w;
}
std::optional<Packet> decode(const std::uint8_t* b, std::size_t size) {
    if (!b || (size != 64 && size != 76 && size != 80) || b[0]!='S' || b[1]!='C' || b[2]!='X' || b[3]!='2' ||
        (b[4]!=2 && b[4]!=3 && b[4]!=4 && b[4]!=5) || b[5]<1 || b[5]>6 || b[6]>1 || b[7]>4) return std::nullopt;
    Packet p; p.format=Format(b[4]); p.kind=Kind(b[5]); p.hid_ready=b[6]!=0; p.capabilities=b[7];
    if (size != (p.kind == Kind::state ? (p.format!=Format::legacy ? 80u : 76u) : 64u)) return std::nullopt;
    std::copy(b+8,b+24,p.session.begin()); std::copy(b+24,b+40,p.boot.begin()); std::copy(b+40,b+56,p.challenge.begin());
    for (unsigned i=0; i<8; ++i) p.sequence=(p.sequence<<8)|b[56+i];
    if (p.kind == Kind::state) {
        if (b[67]!=0) return std::nullopt;
        p.state.buttons=read16(b+64); p.state.hat=b[66];
        for (unsigned i=0; i<4; ++i) p.state.axes[i]=signed16(read16(b+68+2*i));
        if (p.format!=Format::legacy) { p.state.triggers.left=read16(b+76); p.state.triggers.right=read16(b+78); }
    }
    return valid(p) ? std::optional<Packet>(p) : std::nullopt;
}

// One Gamepad collection: 12 buttons + a null-capable 4-bit hat + X/Y/Rx/Ry.
// Independently authored descriptor; report packing below must stay in sync.
const std::uint8_t hid_descriptor[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,
    0x05,0x09,0x19,0x01,0x29,0x0c,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0c,0x81,0x02,
    0xa4,0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,0x35,0x00,0x46,0x3b,0x01,
    0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,0xb4,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x33,0x09,0x34,0x16,0x01,0x80,0x26,0xff,0x7f,
    0x75,0x10,0x95,0x04,0x81,0x02,0xc0
};
const std::size_t hid_descriptor_size = sizeof(hid_descriptor);
std::optional<HidReport> hid_report(const State& s, Port port, Format format) {
    if (!s.valid() || (format!=Format::legacy && format!=Format::pressure && format!=Format::usb_buttons)) return std::nullopt;
    if (format==Format::usb_buttons && ((s.triggers.left!=0 && s.triggers.left!=65535) || (s.triggers.right!=0 && s.triggers.right!=65535))) return std::nullopt;
    constexpr unsigned left[] = {5,6,11,12,4,10,7,8,3,9};
    constexpr unsigned right[] = {11,12,5,6,10,4,1,2,9,3};
    const auto* slots = port == Port::left ? left : right;
    std::uint16_t bits = std::uint16_t(s.hat) << 12;
    for (unsigned i=0;i<10;++i) if (s.buttons & (1u<<i)) bits |= std::uint16_t(1u << (slots[i]-1));
    // X1/X2 are spare digital inputs, not the secondary stick axes.
    // Xbox Accessories must map X1 to LT and X2 to RT explicitly.
    if (format==Format::usb_buttons) {
        if(s.triggers.left) bits |= std::uint16_t(1u << (port==Port::left ? 0 : 6));
        if(s.triggers.right) bits |= std::uint16_t(1u << (port==Port::left ? 1 : 7));
    }
    HidReport report{}; report[0]=std::uint8_t(bits); report[1]=std::uint8_t(bits>>8);
    for (unsigned i=0;i<4;++i) {
        unsigned source = port == Port::left ? i : (i+2)%4;
        std::int32_t axis=s.axes[source]; if (i%2) axis=-axis; // HID convention is down-positive Y.
        auto raw=static_cast<std::uint16_t>(static_cast<std::int16_t>(axis));
        report[2+2*i]=std::uint8_t(raw); report[3+2*i]=std::uint8_t(raw>>8);
    }
    return report;
}

Receiver::Receiver(Token boot, TokenSource source, void* context, TriggerOutputBackend* triggers, UsbOutput usb_output, std::uint64_t lease_duration_us)
    : boot_(boot), token_source_(source), token_context_(context), triggers_(triggers), usb_output_(usb_output), lease_duration_us_(lease_duration_us) {
    if (triggers_) triggers_->release();
    if (!nonzero(boot_) || !token_source_) { entropy_failed_=true; fault_=Fault::entropy; }
    if (lease_duration_us_ < 50'000 || lease_duration_us_ > lease_us) { entropy_failed_=true; fault_=Fault::clock; }
}
void Receiver::retire() {
    if (triggers_) triggers_->release();
    accepted_triggers_={}; trigger_neutral_confirmed_=false;
    active_=false; armed_=false; session_={}; challenge_={}; last_sequence_=0; completed_sequence_=0; deadline_=0;
    head_=0; count_=0; neutral_confirmed_=false; neutral_pending_=true;
}
void Receiver::fail(Fault fault) { retire(); fault_=fault; if(fault==Fault::trigger_output) trigger_faulted_=true; }
bool Receiver::pressure_ready() const {
    return triggers_ && triggers_->ready() && trigger_neutral_confirmed_ && !trigger_faulted_;
}
bool Receiver::advance(std::uint64_t now) {
    if ((last_time_ && now<*last_time_) || lease_duration_us_ < 50'000 || lease_duration_us_ > lease_us || now>maximum-lease_duration_us_) { fail(Fault::clock); return false; }
    last_time_=now;
    if (active_ && now>=deadline_) retire();
    if (active_ && format_==Format::pressure && !pressure_ready()) { fail(Fault::trigger_output); return false; }
    if (triggers_ && !triggers_->ready()) {
        // A backend reinitialization invalidates earlier neutral confirmation,
        // even while no client owns output. Never offer from a stale zero write.
        if (trigger_neutral_confirmed_) triggers_->release();
        trigger_neutral_confirmed_=false; accepted_triggers_={};
    }
    if (!active_ && triggers_ && triggers_->ready() && !trigger_faulted_ && !trigger_neutral_confirmed_) {
        if (!triggers_->apply({})) { fail(Fault::trigger_output); return false; }
        accepted_triggers_={}; trigger_neutral_confirmed_=true;
    }
    return true;
}
void Receiver::poll(std::uint64_t now) { (void)advance(now); }
void Receiver::set_usb_available(bool available, std::uint64_t now) {
    if (!advance(now)) return;
    if (available != usb_available_) {
        retire(); usb_available_=available;
        // Unmount invalidates the adapter's outstanding transfer; its old ticket is fenced.
        if (!available) in_flight_.reset();
    }
}
bool Receiver::matches(const Packet& p) const {
    return active_ && p.format==format_ && p.session==session_ && p.boot==boot_ && p.challenge==challenge_;
}
Packet Receiver::reply(Kind kind, std::uint64_t seq, bool hid_ready) const {
    Packet p; p.format=format_; p.kind=kind; p.capabilities=(kind==Kind::ready && hid_ready) ? (format_==Format::pressure ? proportional_triggers : (format_==Format::usb_buttons ? usb_button_triggers : (format_==Format::usb_pressure ? usb_proportional_triggers : 0))) : 0; p.session=session_; p.boot=boot_; p.challenge=challenge_; p.sequence=seq; p.hid_ready=hid_ready; return p;
}
std::optional<Packet> Receiver::receive(const Packet& p, std::uint64_t now) {
    if (!advance(now) || !valid(p)) return std::nullopt;
    if (p.kind == Kind::hello) {
        const bool format_supported = usb_output_==UsbOutput::xinput ? p.format==Format::usb_pressure : p.format!=Format::usb_pressure;
        if (!format_supported || !((p.format==Format::usb_buttons || p.format==Format::usb_pressure) ? usb_ready() : ready()) || (p.format==Format::pressure && !pressure_ready())) { Packet out; out.format=p.format; out.kind=Kind::ready; out.session=p.session; out.boot=boot_; return nonzero(boot_) ? std::optional<Packet>(out) : std::nullopt; }
        if (active_) return p.session==session_ && p.format==format_ ? std::optional<Packet>(reply(Kind::ready,0,true)) : std::nullopt;
        Token next=token_source_(token_context_);
        if (!nonzero(next) || challenge_counter_==maximum) { entropy_failed_=true; fail(Fault::entropy); return std::nullopt; }
        ++challenge_counter_;
        for (unsigned i=0;i<8;++i) next[8+i]=std::uint8_t(challenge_counter_ >> (56-8*i));
        challenge_=next; session_=p.session; format_=p.format; active_=true; armed_=false;
        last_sequence_=0; completed_sequence_=0; deadline_=now+lease_duration_us_;
        return reply(Kind::ready,0,true);
    }
    if (p.kind == Kind::stop && last_stop_ && p.format==last_stop_->format && p.session==last_stop_->session && p.boot==last_stop_->boot &&
        p.challenge==last_stop_->challenge && p.sequence==last_stop_->sequence) {
        return stop_completed_ ? last_stop_ : std::nullopt;
    }
    if (!matches(p) || !usb_available_) return std::nullopt;
    if (p.kind == Kind::stop) {
        if (p.sequence<=last_sequence_) return std::nullopt;
        last_stop_=reply(Kind::stopped,p.sequence); stop_completed_=false; retire(); return std::nullopt;
    }
    if (p.kind != Kind::state) return std::nullopt;
    if (p.sequence<last_sequence_) return std::nullopt;
    if (p.sequence==last_sequence_) return completed_sequence_==p.sequence ? std::optional<Packet>(reply(Kind::ack,p.sequence,true)) : std::nullopt;
    if (!armed_ && (last_sequence_!=0 || p.sequence!=1 || !p.state.neutral())) return std::nullopt;
    if (count_==queue_capacity) { fail(Fault::queue_overflow); return std::nullopt; }
    Job job; job.work.state=p.state; job.work.format=p.format; job.ack=reply(Kind::ack,p.sequence,true); job.has_ack=true;
    queue_[(head_+count_)%queue_capacity]=job; ++count_;
    last_sequence_=p.sequence; deadline_=now+lease_duration_us_;
    return std::nullopt;
}
std::optional<UsbWork> Receiver::take_report(std::uint64_t now) {
    if (!advance(now) || !usb_available_ || in_flight_) return std::nullopt;
    if (!neutral_pending_ && count_==0) return std::nullopt;
    if (ticket_==maximum) { fail(Fault::ticket_exhausted); usb_available_=false; return std::nullopt; }
    Job job;
    if (neutral_pending_) { job.work.neutral_barrier=true; neutral_pending_=false; }
    else { job=queue_[head_]; head_=(head_+1)%queue_capacity; --count_; }
    job.work.ticket=++ticket_; in_flight_=job; return job.work;
}
std::optional<Packet> Receiver::complete_report(std::uint64_t ticket, bool success, std::uint64_t now) {
    const bool clock_ok=advance(now);
    if (!in_flight_ || in_flight_->work.ticket!=ticket) return std::nullopt;
    Job job=*in_flight_; in_flight_.reset();
    if (!success) { usb_available_=false; fail(Fault::usb_transfer); return std::nullopt; }
    completed_=job.work.state; completed_format_=job.work.format;
    if (!clock_ok) return std::nullopt; // Consume a completed ticket; neutral must not be stranded.
    if (job.work.neutral_barrier) {
        neutral_confirmed_=usb_available_; neutral_pending_=false;
        if (last_stop_ && !stop_completed_) { stop_completed_=true; return last_stop_; }
        return std::nullopt;
    }
    if (!matches(job.ack) || !usb_available_) return std::nullopt;
    if (format_==Format::pressure) {
        // Only a still-live current ticket can assert pressure. completed_ alone
        // may describe a stale physical USB transfer and is never a GPIO source.
        if (!pressure_ready()) {
            fail(Fault::trigger_output); return std::nullopt;
        }
        const auto result=triggers_->apply_before(job.work.state.triggers,deadline_);
        if (result==TriggerApplyResult::expired) { retire(); return std::nullopt; }
        if (result==TriggerApplyResult::fault) { fail(Fault::trigger_output); return std::nullopt; }
        accepted_triggers_=job.work.state.triggers;
    }
    completed_sequence_=job.ack.sequence;
    if (completed_sequence_==1) armed_=true;
    return job.ack;
}
} // namespace scx2
