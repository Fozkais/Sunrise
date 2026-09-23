#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../rewards/definition.h"

namespace sunrise::state::build_data::season_pass {

/** Reward rows the installed pass declares. The shipped build carries 196. */
inline constexpr std::size_t kRewardCapacity = 256;

/** A reward whose claim flag no mapping table addresses carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableFlagIndex = 0xFFFFU;

/** Bound on the expanded eligibility expression of one pass row. */
inline constexpr std::size_t kConditionCapacity = 64;

/** One reward row of the pass, in the native order the opcode-2400 claim names by index. */
struct Reward {
    /** Installed definition hash of the granted item. */
    std::uint32_t itemHash{};
    /** Units granted. */
    std::uint32_t quantity{};
    /** Native item-definition index the row names. */
    std::uint16_t itemIndex{};
    /** Account flag bank row this reward's claim sets, or kUnavailableFlagIndex. */
    std::uint16_t claimFlagIndex{kUnavailableFlagIndex};
    /** Rank the account needs before the row may be claimed. */
    std::uint8_t requiredRank{};
    std::array<rewards::SocketOverride, rewards::kSocketsPerItem> sockets{};
    std::uint8_t socketCount{};
    std::array<rewards::Instruction, kConditionCapacity> condition{};
    std::uint8_t conditionCount{};
};

} // namespace sunrise::state::build_data::season_pass
