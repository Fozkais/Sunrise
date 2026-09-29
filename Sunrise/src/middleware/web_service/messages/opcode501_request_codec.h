#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../web_service_envelope.h"
#include "opcode501_codec.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** Size of the native customisation header both character records carry. */
inline constexpr std::size_t kCustomisationSize = 36;

/** The create-character request: identity triple plus the look the player chose. */
struct Request {
    std::uint8_t characterClass{};
    std::uint8_t gender{};
    std::uint8_t race{};
    /** The chosen look in the native record layout; meaningful only when `customised`. */
    std::array<std::uint8_t, kCustomisationSize> customisation{};
    bool customised{};
};

/**
 * Parses the create-character request's identity triple and customisation header.
 *
 * The payload is a big-endian bitstream, not a byte-aligned struct. Wire order is depth-first
 * over `{ int8 triple[3]; CharHeader header; char name[64]; }`, where `triple` is race, gender
 * and class in that order; each `triple` entry is a 1-bit presence flag followed by 8 value bits
 * biased by +0x80. The 234-bit header that follows has no presence bits: a 16-bit index, three
 * 8-bit bytes (bias 0x80), ten more 16-bit indices, a 32-bit hash and a 2-bit value, where an
 * index is biased by 0x8000 so the empty index -1 travels as 0x7FFF. The fields land in the
 * native 36-byte record layout. The name that follows is discarded. The layout was read off one
 * capture: the hash came out as the engine's no-hash sentinel and the widths add up to 234 bits.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the parsed identity triple and customisation header.
 * @return True when the payload holds the full identity-and-header span, every presence flag is
 *         set, and race, gender and class are all in range.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
