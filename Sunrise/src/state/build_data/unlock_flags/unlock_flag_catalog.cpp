#include "unlock_flag_catalog.h"

#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::unlock_flags {
namespace {

core::threading::SrwLock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

} // namespace

/** Clears every extracted unlock flag under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_definitions.clear();
}

/** Checks one complete unlock flag table. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (const Definition& definition : definitions) {
        // The hash is what names a flag to the client, so a row without one is not a flag.
        if (definition.hash == 0) {
            return false;
        }
    }
    return true;
}

/** Replaces the extracted unlock flag table in one step. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_definitions.replace(definitions);
}

/** Finds one flag by its slot. */
bool find_slot(std::uint16_t slot, Definition& definition) noexcept {
    definition = {};
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    if (slot >= rows.size()) {
        return false;
    }
    definition = rows[slot];
    return true;
}

/** Finds the one flag a bank row stores. */
bool find_bank_row(std::uint16_t bank, std::uint16_t row, std::uint16_t& slot) noexcept {
    slot = 0;
    if (bank == kNoBank || row == kNoRow) {
        return false;
    }
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    std::size_t matches = 0;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].bank == bank && rows[index].row == row) {
            slot = static_cast<std::uint16_t>(index);
            ++matches;
        }
    }
    // A row shared by two flags has no single name, so it resolves to neither.
    if (matches != 1) {
        slot = 0;
    }
    return matches == 1;
}

/** Copies every row in slot order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return The unlock flag row count, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::unlock_flags
