#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

// Type-32 toggle sensors (`tg_*`, `t_kv_*` in the authored data): switches for whole beats, such
// as Homecoming's `tg_plaza_battle`, `t_kv_missiles` and `tg_shield_gen`. The Auth body is class
// 0x8080955A, from the client's reflection database: one ClientRef, then a 2-bit integer stored
// with a bias of one. 55 + 2 = 57 bits, exactly the fixed width the slot metadata declares. As
// with every Auth body, the length is what makes it safe to send: a body of the wrong bit count
// is not refused, it misparses and stalls the client. What the reference names and what each
// state means is not established, so both are passed through from the script.

namespace sunrise::middleware::bap::activity_message::toggle_auth {

namespace fields = auth_fields;

inline constexpr std::uint8_t kSlotType = 32;
inline constexpr std::uint32_t kComponentClass = 0x80809556U;
inline constexpr std::uint32_t kSchema = 0x8080955AU;
/** The state is a 2-bit integer stored with a bias of one: -1..2 on the wire as 0..3. */
inline constexpr std::uint8_t kStateWidth = 2;
inline constexpr std::int32_t kStateBias = 1;
inline constexpr std::int32_t kMinimumState = -1;
inline constexpr std::int32_t kMaximumState = 2;
inline constexpr std::size_t kBitCount = 57;
inline constexpr std::size_t kByteCount = (kBitCount + 7) / 8;

/** One toggle control. `targetRegistryKey == 0` writes the unset reference. */
struct Request final {
    std::uint32_t targetRegistryKey{};
    std::uint32_t targetSlotType{};
    std::uint16_t targetSlotIndex{};
    std::int32_t state{};
};

/**
 * Encodes one toggle body.
 * @param output Exactly kByteCount.
 * @return False on an out-of-range state or reference index, or a size mismatch.
 */
[[nodiscard]] inline bool encode(const Request& request, std::span<std::byte> output) noexcept {
    if (output.size() != kByteCount || request.state < kMinimumState
        || request.state > kMaximumState
        || request.targetSlotIndex > fields::kMaximumClientRefIndex) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const bool reference = request.targetRegistryKey == 0
                               ? fields::write_absent_client_ref(writer)
                               : fields::write_client_ref(writer,
                                                          request.targetRegistryKey,
                                                          request.targetSlotType,
                                                          request.targetSlotIndex);
    return reference
           && writer.write(static_cast<std::uint32_t>(request.state + kStateBias), kStateWidth)
           && fields::finish_exact(writer, kBitCount, kByteCount, written);
}

} // namespace sunrise::middleware::bap::activity_message::toggle_auth
