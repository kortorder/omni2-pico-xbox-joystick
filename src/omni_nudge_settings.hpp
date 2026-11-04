#pragma once
#include "omni_nudges.hpp"
#include <array>
#include <cstdint>

namespace omni {
// Wire v1 has a fixed right, left, up, down order. NudgeMap uses named members.
using NudgeSettingsWire = std::array<std::uint8_t, 16>;
struct NudgeProposal {
    std::uint32_t previous_revision{};
    NudgeMap map{};
};
void nudge_settings_init(); // After CYW43 creates the BTstack flash TLV singleton.
NudgeSettingsWire nudge_settings_read();
bool nudge_settings_prepare(const NudgeSettingsWire &packet, NudgeProposal &proposal);
void nudge_settings_pending();
void nudge_settings_abort();
void nudge_settings_commit(const NudgeProposal &proposal); // Only after USB neutral completion.
} // namespace omni
