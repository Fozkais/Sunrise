#include "activity_section.h"

#include <Windows.h>

#include <array>

namespace sunrise::state::activity::mission::activity_section {
namespace {

/** A host serves a handful of activity sessions at once. */
constexpr std::size_t kCapacity = 8;

struct Entry final {
    std::uint64_t sessionId{};
    std::uint32_t key{kNoSection};
    bool used{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Entry, kCapacity> g_entries{};

} // namespace

void set(std::uint64_t sessionId, std::uint32_t key) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    Entry* slot = nullptr;
    for (Entry& entry : g_entries) {
        if (entry.used && entry.sessionId == sessionId) {
            slot = &entry;
            break;
        }
        if (!entry.used && slot == nullptr) {
            slot = &entry;
        }
    }
    if (slot != nullptr) {
        *slot = {sessionId, key, true};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::uint32_t get(std::uint64_t sessionId) noexcept {
    std::uint32_t key = kNoSection;
    AcquireSRWLockShared(&g_lock);
    for (const Entry& entry : g_entries) {
        if (entry.used && entry.sessionId == sessionId) {
            key = entry.key;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return key;
}

void clear(std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (Entry& entry : g_entries) {
        if (entry.used && entry.sessionId == sessionId) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::state::activity::mission::activity_section
