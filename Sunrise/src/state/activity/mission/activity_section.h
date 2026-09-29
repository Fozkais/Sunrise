#pragma once

#include <cstdint>

namespace sunrise::state::activity::mission::activity_section {

/** FNV-1a of nothing: the empty section key, which plays nothing. */
inline constexpr std::uint32_t kNoSection = 0x811C9DC5U;

/**
 * The section a mission program says the activity is in: a scenario state hash or a section
 * object's registry key. The lifetime Auth carries it, and the client looks it up in the
 * scenario's key-to-sequence table: the sequence found plays as the next move begins (a section's
 * title card, a cinematic state's global fade).
 */

/** Names one session's section. Any thread. */
void set(std::uint64_t sessionId, std::uint32_t key) noexcept;

/** @return The session's section key, or kNoSection. Any thread. */
[[nodiscard]] std::uint32_t get(std::uint64_t sessionId) noexcept;

/** Forgets one session's section. Any thread. */
void clear(std::uint64_t sessionId) noexcept;

} // namespace sunrise::state::activity::mission::activity_section
