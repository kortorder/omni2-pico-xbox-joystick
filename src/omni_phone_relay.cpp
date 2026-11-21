#include "omni_phone_relay.hpp"
namespace omni {
namespace { PhoneRelay *attached = nullptr; }
PhoneRelay *phone_relay() { return attached; }
void phone_relay_attach(PhoneRelay *relay) { attached = relay; }
std::optional<scx2::Packet> PhoneRelay::receive(const scx2::Packet &p, std::uint64_t now) {
    if (!network_available_ || p.format != scx2::Format::legacy || p.state.buttons ||
        p.state.hat != 8 || p.state.axes[0] || p.state.axes[1] || !p.state.triggers.neutral())
        return {};
    return receiver_.receive(p, now);
}
void PhoneRelay::reset(std::uint64_t now) {
    receiver_.set_usb_available(false, now);
    work_.reset();
    submitted_ = false;
}
void PhoneRelay::network(bool available, std::uint64_t now) {
    if (!available) reset(now);
    network_available_ = available;
}
void PhoneRelay::poll(bool available, std::uint64_t now) {
    receiver_.poll(now);
    // A prepared but rejected USB submission must not outlive its source session.
    if (work_ && !submitted_ && !work_->neutral_barrier && !receiver_.active()) reset(now);
    receiver_.set_usb_available(available && network_available_, now);
    if (!available || !network_available_) {
        work_.reset();
        submitted_ = false;
    }
}
void PhoneRelay::prepare(std::uint64_t now) {
    if (!work_) work_ = receiver_.take_report(now);
}
Axes PhoneRelay::source() const {
    if (!receiver_.armed()) return {};
    const auto state = work_ ? work_->state : receiver_.completed_output();
    // SCX2 Y is up-positive, while the unchanged chair HID is down-positive.
    return {state.axes[2], static_cast<std::int16_t>(-state.axes[3])};
}
void PhoneRelay::submitted(std::uint64_t ticket) {
    if (work_ && work_->ticket == ticket) submitted_ = true;
}
void PhoneRelay::complete(std::uint64_t ticket, bool success, std::uint64_t now) {
    if (!work_ || !submitted_ || work_->ticket != ticket) return;
    const auto reply = receiver_.complete_report(ticket, success, now);
    work_.reset();
    submitted_ = false;
    if (reply && reply_) reply_(*reply, reply_context_);
}
} // namespace omni
