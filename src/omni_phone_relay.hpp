// Optional phone right-stick source. USB completion is the acknowledgement boundary.
#pragma once
#include "omni_stick.hpp"
#include "relay_protocol/scx2.hpp"

namespace omni {
class PhoneRelay {
  public:
    static constexpr std::uint64_t lease_us = 250000;
    using Reply = void (*)(const scx2::Packet &, void *);
    PhoneRelay(scx2::Token boot, scx2::TokenSource random, void *random_context,
               Reply reply, void *reply_context = nullptr)
        : receiver_(boot, random, random_context, nullptr, scx2::UsbOutput::xac, lease_us),
          reply_(reply), reply_context_(reply_context) {}
    // Only the two right-stick axes are accepted, even in an otherwise valid packet.
    std::optional<scx2::Packet> receive(const scx2::Packet &packet, std::uint64_t now);
    void poll(bool usb_available, std::uint64_t now);
    void reset(std::uint64_t now);
    void network(bool available, std::uint64_t now);
    // Prepared work is not submitted until TinyUSB has copied the combined report.
    void prepare(std::uint64_t now);
    Axes source() const;
    std::uint64_t pending_ticket() const { return work_ && !submitted_ ? work_->ticket : 0; }
    void submitted(std::uint64_t ticket);
    void complete(std::uint64_t ticket, bool success, std::uint64_t now);
    bool active() const { return receiver_.active(); }
    bool ready() const { return receiver_.ready(); }
    bool armed() const { return receiver_.armed(); }
  private:
    scx2::Receiver receiver_;
    std::optional<scx2::UsbWork> work_;
    bool submitted_ = false, network_available_ = true;
    Reply reply_;
    void *reply_context_;
};
// Single-core adapter hook. Default builds do not include or call this module.
PhoneRelay *phone_relay();
void phone_relay_attach(PhoneRelay *relay);
} // namespace omni
