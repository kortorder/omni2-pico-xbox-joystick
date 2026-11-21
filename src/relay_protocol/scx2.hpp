// Original project implementation of bridge/PROTOCOL.md. No third-party source is embedded.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include "trigger_output.hpp"

namespace scx2 {
using Token = std::array<std::uint8_t, 16>;
enum class Kind : std::uint8_t { hello=1, ready, state, ack, stop, stopped };
enum class Format : std::uint8_t { legacy = 2, pressure = 3, usb_buttons = 4, usb_pressure = 5 };
constexpr std::uint8_t proportional_triggers = 1;
constexpr std::uint8_t usb_button_triggers = 2;
constexpr std::uint8_t usb_proportional_triggers = 4;
enum class UsbOutput { xac, xinput };
struct State {
    std::uint16_t buttons = 0;
    std::uint8_t hat = 8;
    std::array<std::int16_t, 4> axes{}; // left X/Y, right X/Y; Y up.
    TriggerPressure triggers{};
    bool valid() const;
    bool neutral() const;
    bool operator==(const State& other) const;
};
struct Packet {
    Format format = Format::legacy;
    Kind kind = Kind::hello;
    Token session{}, boot{}, challenge{};
    std::uint64_t sequence = 0;
    bool hid_ready = false;
    std::uint8_t capabilities = 0; // Format-3/4/5 HELLO/READY only.
    State state{};
};
struct Wire { std::array<std::uint8_t, 80> bytes{}; std::size_t size = 0; };
bool valid(const Packet& packet);
std::optional<Wire> encode(const Packet& packet);
std::optional<Packet> decode(const std::uint8_t* data, std::size_t size);
enum class Port { left, right };
using HidReport = std::array<std::uint8_t, 10>;
std::optional<HidReport> hid_report(const State& state, Port port = Port::left, Format format = Format::legacy);
extern const std::uint8_t hid_descriptor[];
extern const std::size_t hid_descriptor_size;

enum class Fault { none, clock, entropy, queue_overflow, usb_transfer, ticket_exhausted, trigger_output };
struct UsbWork { std::uint64_t ticket = 0; State state{}; bool neutral_barrier = false; Format format = Format::legacy; };

// Supply fresh random tokens in the board adapter. Tests inject deterministic tokens.
using TokenSource = Token (*)(void* context);
class Receiver {
public:
    static constexpr std::uint64_t lease_us = 1'000'000;
    static constexpr std::size_t queue_capacity = 16;
    Receiver(Token boot, TokenSource source, void* context = nullptr, TriggerOutputBackend* triggers = nullptr, UsbOutput usb_output = UsbOutput::xac, std::uint64_t lease_duration_us = lease_us);
    void set_usb_available(bool available, std::uint64_t now);
    void poll(std::uint64_t now);
    std::optional<Packet> receive(const Packet& packet, std::uint64_t now);
    std::optional<UsbWork> take_report(std::uint64_t now);
    std::optional<Packet> complete_report(std::uint64_t ticket, bool success, std::uint64_t now);
    bool usb_ready() const { return usb_available_ && neutral_confirmed_ && !entropy_failed_; }
    bool ready() const { return usb_ready() && !trigger_faulted_; }
    bool armed() const { return active_ && armed_; }
    bool active() const { return active_; }
    std::size_t queued() const { return count_; }
    Fault fault() const { return fault_; }
    State completed_output() const { return completed_; }
    Format completed_format() const { return completed_format_; }
    TriggerPressure accepted_trigger_output() const { return accepted_triggers_; }
    Token boot() const { return boot_; }
private:
    struct Job { UsbWork work; Packet ack; bool has_ack = false; };
    Token boot_, session_{}, challenge_{};
    TokenSource token_source_;
    void* token_context_;
    TriggerOutputBackend* triggers_;
    UsbOutput usb_output_;
    const std::uint64_t lease_duration_us_;
    Format format_ = Format::legacy;
    bool trigger_neutral_confirmed_ = false, trigger_faulted_ = false;
    TriggerPressure accepted_triggers_{};
    bool usb_available_ = false, neutral_confirmed_ = false, neutral_pending_ = true;
    bool active_ = false, armed_ = false, entropy_failed_ = false;
    std::uint64_t last_sequence_ = 0, completed_sequence_ = 0, deadline_ = 0;
    std::optional<std::uint64_t> last_time_;
    std::uint64_t ticket_ = 0;
    std::uint64_t challenge_counter_ = 0;
    std::array<Job, queue_capacity> queue_{};
    std::size_t head_ = 0, count_ = 0;
    std::optional<Job> in_flight_;
    std::optional<Packet> last_stop_;
    bool stop_completed_ = false;
    Fault fault_ = Fault::none;
    State completed_{};
    Format completed_format_ = Format::legacy;
    bool advance(std::uint64_t now);
    void retire();
    bool matches(const Packet& packet) const;
    Packet reply(Kind kind, std::uint64_t sequence, bool hid_ready = false) const;
    void fail(Fault fault);
    bool pressure_ready() const;
};
} // namespace scx2
