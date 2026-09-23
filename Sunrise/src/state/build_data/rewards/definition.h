#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::rewards {

/** Native references use all bits set for an absent row. */
inline constexpr std::uint16_t kAbsent = 0xFFFF;
/** Bounds cover signed native indices and the flat banks they reference. */
inline constexpr std::size_t kPoolCapacity = 4096;
inline constexpr std::size_t kEntryCapacity = 32768;
inline constexpr std::size_t kItemCapacity = 32768;
inline constexpr std::size_t kInstructionCapacity = 131072;
inline constexpr std::size_t kModifierCapacity = 32768;
inline constexpr std::size_t kSocketOverrideCapacity = 32768;
/** Native reward overrides address the 12 ordinary sockets on an item. */
inline constexpr std::size_t kSocketsPerItem = 12;
/** A wrapper can select independently from four reward categories. */
inline constexpr std::size_t kSelectionCapacity = 4;
/** A resolved grant fits one bounded inventory transaction. */
inline constexpr std::size_t kGrantCapacity = 32;
/** Nested pools and referenced conditions share a bounded traversal depth. */
inline constexpr std::size_t kTraversalDepth = 32;

struct Range {
    std::uint32_t first{};
    std::uint32_t count{};
};

struct Instruction {
    std::uint32_t opcode{};
    std::uint32_t operand{};
};

/** Resolved bank reads occupy a separate range from native expression operators. */
enum class BankRead : std::uint32_t {
    accountFlag = 256,
    characterFlag,
    accountValue,
    characterValue,
    externalFlag,
    externalValue,
    characterClass,
    profileFlag,
};

/** A fixed override names its plug directly rather than selecting from a set. */
inline constexpr std::uint32_t kFixedPlugSelection = 0xFFFFFFFFU;
/** Wrappers without this bit remain inventory items until explicitly opened. */
inline constexpr std::uint32_t kOpenOnAcquisition = 1;

struct SocketOverride {
    std::uint16_t socketType{kAbsent};
    std::uint16_t plugItem{kAbsent};
    std::uint16_t plugSet{kAbsent};
    std::uint16_t rollSet{kAbsent};
    std::uint32_t selection{kFixedPlugSelection};
};

struct Modifier {
    Range condition{};
    std::uint16_t valueIndex{kAbsent};
    float value{};
};

struct Entry {
    std::uint16_t itemIndex{kAbsent};
    std::uint16_t poolIndex{kAbsent};
    std::uint32_t quantity{};
    std::uint32_t categoryHash{};
    std::uint32_t bucketHash{};
    float weight{};
    Range condition{};
    Range modifiers{};
    Range sockets{};
};

struct Pool {
    std::uint32_t definitionHash{};
    Range entries{};
};

struct Selection {
    std::uint32_t categoryHash{};
    std::uint32_t count{};
};

/** Dense item rows bind inventory definitions to their reward pools and acquisition flags. */
struct Item {
    std::uint32_t definitionHash{};
    std::uint16_t poolIndex{kAbsent};
    std::uint16_t acquiredFlag{kAbsent};
    std::array<Selection, kSelectionCapacity> selections{};
    std::uint8_t selectionCount{};
    std::uint32_t flags{};
};

struct View {
    std::span<const Pool> pools;
    std::span<const Entry> entries;
    std::span<const Item> items;
    std::span<const Instruction> instructions;
    std::span<const Modifier> modifiers;
    std::span<const SocketOverride> sockets;
};

template <typename T> [[nodiscard]] constexpr bool fits(Range range, std::span<T> bank) noexcept {
    return range.first <= bank.size() && range.count <= bank.size() - range.first;
}

} // namespace sunrise::state::build_data::rewards
