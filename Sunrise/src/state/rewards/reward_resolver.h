#pragma once

#include "../account/account_state.h"
#include "../build_data/rewards/definition.h"
#include "../unlocks/definition.h"

namespace sunrise::state::rewards {

/** Pass engrams select equipment; ordinary acquisitions expand every declared lane. */
enum class Selection : std::uint8_t { all, equipment };

struct Grant {
    std::uint16_t itemIndex{build_data::rewards::kAbsent};
    std::int32_t quantity{};
    std::array<build_data::rewards::SocketOverride, build_data::rewards::kSocketsPerItem> sockets{};
    std::size_t socketCount{};
};

struct Result {
    std::array<Grant, build_data::rewards::kGrantCapacity> grants{};
    std::size_t count{};
};

/** The seed belongs to the prepared server transaction, so validation repeats the same draw. */
struct Context {
    const unlocks::Table& unlocks;
    CharacterClass characterClass{};
    std::uint64_t seed{};
    /** Optional refusal reason; points to a static diagnostic label. */
    const char** refusal{};
};

[[nodiscard]] bool eligible(std::span<const build_data::rewards::Instruction> instructions,
                            const Context& context,
                            bool& result) noexcept;

/** Plans an acquisition without changing State; retains wrappers opened separately. */
[[nodiscard]] bool resolve(const Context& context,
                           std::uint16_t itemIndex,
                           std::uint32_t quantity,
                           Selection selection,
                           Result& result) noexcept;

} // namespace sunrise::state::rewards
