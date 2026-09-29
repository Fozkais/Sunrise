#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::middleware::content::packages::tables::activity_display_names {

/** Class of the installed client activity table. */
inline constexpr std::uint32_t kActivityClientTableClass = 0x80805E0AU;
/** Class of the installed string-bank index. */
inline constexpr std::uint32_t kStringBankIndexClass = 0x80805F98U;
/** Per-bank container class. */
inline constexpr std::uint32_t kStringContainerClass = 0x80809A88U;
/** Per-language string-data class. */
inline constexpr std::uint32_t kLanguageDataClass = 0x80809A8AU;
/** Generated SDK display text must fit this storage including its terminator. */
inline constexpr std::size_t kDisplayNameCapacity = 256;

/** One exact English display name or an authored reference with no English bank entry. */
struct Name final {
    std::array<char, kDisplayNameCapacity> value{};
    std::uint16_t length{};
    std::uint32_t bankIndex{};
    std::uint32_t stringHash{};
    bool authoredEmpty{};
};

/** Complete dynamically read activity-name result. */
struct Snapshot final {
    std::vector<Name> names{};
    std::uint32_t resolvedCount{};
    std::uint32_t authoredEmptyCount{};
};

/** One direct localized-string reference carried by authored activity content. */
struct Reference final {
    std::uint32_t containerTag{};
    std::uint32_t stringHash{};
};

/** One localized-string reference as a definition row carries it: the bank row, then the hash. */
struct BankedReference final {
    std::uint32_t bankIndex{};
    std::uint32_t stringHash{};
};

/** Callback that reads one installed tag and verifies its class. */
using TagReader = bool (*)(void* context,
                           std::uint32_t tag,
                           std::uint32_t expectedClass,
                           std::vector<std::byte>& output) noexcept;

/** Injected installed-content source used by live and synthetic readers. */
struct Source final {
    void* context{};
    TagReader read{};
    /** Unique installed tag found by scanning kActivityClientTableClass. */
    std::uint32_t activityTableTag{};
    /** Unique installed tag found by scanning kStringBankIndexClass. */
    std::uint32_t stringBankIndexTag{};
};

/**
 * Reads every activity display reference and resolves exact English strings by bank and hash.
 * Missing banks, duplicate hashes, malformed UTF-8, or formatted strings fail the whole result.
 * A reference absent from an otherwise valid English bank is an authored empty name.
 * @param source Installed or synthetic tag source.
 * @param definitionHashes Activity-definition hashes in activity-index order.
 * @param output Receives only a complete, exact result.
 * @return True when every available authored reference resolves exactly.
 */
[[nodiscard]] bool build(const Source& source,
                         std::span<const std::uint32_t> definitionHashes,
                         Snapshot& output) noexcept;

/**
 * Resolves direct English container/hash references without an activity-table join.
 * Unsupported formatted references remain authored-empty so independent literal rows survive.
 */
[[nodiscard]] bool
resolve(const Source& source, std::span<const Reference> references, Snapshot& output) noexcept;

/**
 * Resolves exact English strings for references that name their bank by row.
 * A reference whose bank row is out of range, whose bank will not read, or whose hash the bank
 * does not hold is an authored empty name, so one bad bank never discards the others.
 * @param source Installed or synthetic tag source; only the bank index tag is used.
 * @param references Bank row and string hash of each string, in the order the names come back.
 * @param output Receives one name per reference.
 * @return True when the bank index read and every reference was answered.
 */
[[nodiscard]] bool resolve_banked(const Source& source,
                                  std::span<const BankedReference> references,
                                  Snapshot& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables::activity_display_names
