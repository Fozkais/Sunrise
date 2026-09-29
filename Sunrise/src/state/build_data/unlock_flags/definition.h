#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::unlock_flags {

/** The installed build declares 21,613 flags; a slot is a 16-bit index into the table. */
inline constexpr std::size_t kDefinitionCapacity = 32768;

/** A flag that no bank stores carries this row. */
inline constexpr std::uint16_t kNoRow = 0xFFFFU;

/**
 * Bank codes the installed flag table names. The codes are the engine's own.
 * Code 0 marks a flag that no bank stores, so nothing can hold it set.
 */
inline constexpr std::uint16_t kNoBank = 0;
inline constexpr std::uint16_t kAccountBank = 1;
inline constexpr std::uint16_t kProfileBank = 2;
inline constexpr std::uint16_t kCharacterObjectBank = 3;
inline constexpr std::uint16_t kCharacterBank = 6;

/** One installed unlock flag, addressed by its position in the table, which is its slot. */
struct Definition {
    /** Authored hash the client identifies the flag by. */
    std::uint32_t hash{};
    /** Bank code that stores the flag, or the no-bank code. */
    std::uint16_t bank{};
    /** Row inside that bank, or the no-row value. */
    std::uint16_t row{kNoRow};
};

} // namespace sunrise::state::build_data::unlock_flags
