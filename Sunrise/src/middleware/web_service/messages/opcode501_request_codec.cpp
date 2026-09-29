#include "opcode501_request_codec.h"

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The identity triple (27 bits) and customisation header (234 bits) end here; the 64-byte name
 * follows and is discarded. */
constexpr std::size_t kIdentityAndHeaderBits = 261;
/** Each triple entry is a 1-bit presence flag plus an 8-bit +0x80-biased value. */
constexpr std::uint64_t kByteBias = 0x80U;

/**
 * Reads one 1-bit presence flag followed by an 8-bit +0x80-biased value.
 * @return True when the field is present and readable. An absent field fails closed: nothing on
 *         the wire carries the value that was skipped, so there is nothing to fall back to.
 */
[[nodiscard]] bool read_optional_byte(encoding::bits::Reader& reader, std::uint8_t& value) noexcept {
    std::uint64_t present = 0;
    if (!reader.read(1, present) || present == 0) {
        return false;
    }
    std::uint64_t raw = 0;
    if (!reader.read(8, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

/** The header carries no presence bits: every field is a fixed-width biased scalar. */
constexpr std::uint8_t kIndexBits = 16;
constexpr std::uint64_t kIndexBias = 0x8000U;
/** The last field is a 2-bit value. */
constexpr std::uint8_t kFlagBits = 2;

/** Reads one definition index: 16 bits, biased by 0x8000, so an empty index (-1) is 0x7FFF. */
[[nodiscard]] bool read_index(encoding::bits::Reader& reader, std::uint16_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(kIndexBits, raw)) {
        return false;
    }
    value = static_cast<std::uint16_t>(raw - kIndexBias);
    return true;
}

/** Reads one header byte: 8 bits, biased by 0x80. */
[[nodiscard]] bool read_header_byte(encoding::bits::Reader& reader, std::uint8_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(8, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

/** Stores one little-endian value into the record block. */
template <typename T>
void put(std::array<std::uint8_t, kCustomisationSize>& block, std::size_t offset, T value) noexcept {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        block[offset + index] = static_cast<std::uint8_t>(
            static_cast<std::uint64_t>(value) >> (8U * index));
    }
}

/** Byte offsets of the header fields in the native record, in request struct order. */
constexpr std::size_t kFirstIndexOffset = 0;
constexpr std::array<std::size_t, 3> kByteOffsets{2, 3, 4};
constexpr std::size_t kIndexRunOffset = 6;
constexpr std::size_t kIndexRunCount = 10;
constexpr std::size_t kHashOffset = 28;
constexpr std::size_t kLastByteOffset = 32;

/** Decodes the 234-bit header into the native record block. */
[[nodiscard]] bool read_header(encoding::bits::Reader& reader,
                               std::array<std::uint8_t, kCustomisationSize>& block) noexcept {
    block = {};
    std::uint16_t index = 0;
    if (!read_index(reader, index)) {
        return false;
    }
    put(block, kFirstIndexOffset, index);
    for (const std::size_t offset : kByteOffsets) {
        std::uint8_t value = 0;
        if (!read_header_byte(reader, value)) {
            return false;
        }
        block[offset] = value;
    }
    for (std::size_t run = 0; run < kIndexRunCount; ++run) {
        if (!read_index(reader, index)) {
            return false;
        }
        put(block, kIndexRunOffset + run * 2U, index);
    }
    std::uint64_t hash = 0;
    std::uint64_t flag = 0;
    if (!reader.read(32, hash) || !reader.read(kFlagBits, flag)) {
        return false;
    }
    put(block, kHashOffset, static_cast<std::uint32_t>(hash));
    block[kLastByteOffset] = static_cast<std::uint8_t>(flag);
    return true;
}

/** Highest valid race, gender and class wire values, matching the state enums. */
constexpr std::uint8_t kMaxRace = 2;
constexpr std::uint8_t kMaxGender = 1;
constexpr std::uint8_t kMaxClass = 2;

} // namespace

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() * 8U < kIdentityAndHeaderBits) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    if (!read_optional_byte(reader, request.race) || !read_optional_byte(reader, request.gender)
        || !read_optional_byte(reader, request.characterClass)) {
        return false;
    }
    if (request.race > kMaxRace || request.gender > kMaxGender
        || request.characterClass > kMaxClass) {
        return false;
    }
    // A header that does not decode leaves the default look rather than refusing the creation.
    request.customised = read_header(reader, request.customisation);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode501
