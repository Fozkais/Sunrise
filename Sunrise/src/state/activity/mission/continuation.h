#pragma once

#include <cstdint>
#include <mutex>

namespace sunrise::state::activity::mission::continuation {

/** The activity a completed mission asks the client to move on to, and the session that asked. */
struct Request final {
    std::uint64_t sessionId{};
    std::uint64_t createdRevision{};
    /** Catalog index of the next activity. */
    std::int16_t activity{-1};
};

namespace detail {

inline std::mutex g_mutex;
inline Request g_request;

} // namespace detail

/** Records the request of a session whose mission just completed; a newer one replaces it. */
inline void request(std::uint64_t sessionId,
                    std::uint64_t createdRevision,
                    std::int16_t activity) noexcept {
    const std::lock_guard lock(detail::g_mutex);
    detail::g_request = {sessionId, createdRevision, activity};
}

/** @return The pending request, or one with a negative activity when there is none. */
[[nodiscard]] inline Request pending() noexcept {
    const std::lock_guard lock(detail::g_mutex);
    return detail::g_request;
}

/** Drops the pending request once the client has taken it or can no longer honour it. */
inline void clear() noexcept {
    const std::lock_guard lock(detail::g_mutex);
    detail::g_request = {};
}

} // namespace sunrise::state::activity::mission::continuation
