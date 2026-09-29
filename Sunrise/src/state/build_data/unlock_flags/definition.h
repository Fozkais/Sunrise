#pragma once

#include <array>
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

/** Most bytes of a flag name; a longer name is cut at a character boundary. */
inline constexpr std::size_t kNameLength = 62;

/** What a flag was named after. */
enum class NameSource : std::uint8_t {
    /** The name of the first collectible whose acquired state tests the flag. */
    collectible = 1,
    /** The name of the first record whose completion sets the flag. */
    record = 2,
};

/** The English name of one flag, taken from the first definition that refers to it. */
struct Name {
    /** Slot of the flag the name belongs to. */
    std::uint16_t slot{};
    NameSource source{};
    /** Bytes of the name in use. */
    std::uint8_t length{};
    /** Definitions of the named kind that refer to the flag; a shared flag has several. */
    std::uint16_t references{};
    /** UTF-8 text, zero past `length`. */
    std::array<char, kNameLength> text{};
};

} // namespace sunrise::state::build_data::unlock_flags
