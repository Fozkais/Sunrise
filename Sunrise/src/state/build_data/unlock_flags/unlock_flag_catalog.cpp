#include "unlock_flag_catalog.h"

#include <algorithm>
#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::unlock_flags {
namespace {

core::threading::SrwLock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<Name, kDefinitionCapacity> g_names;

/** @return True when the row holds a bounded name and nothing past it. */
[[nodiscard]] bool canonical(const Name& row) noexcept {
    if (row.length == 0 || row.length > kNameLength || row.references == 0
        || (row.source != NameSource::collectible && row.source != NameSource::record)) {
        return false;
    }
    for (std::size_t index = 0; index < kNameLength; ++index) {
        const auto byte = static_cast<unsigned char>(row.text[index]);
        // Storage past the name must stay zero, so two caches of one build match byte for byte.
        if (index < row.length ? byte < 0x20U : byte != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears every extracted unlock flag under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_definitions.clear();
}

/** Clears every flag name under the catalog lock. */
void clear_names() noexcept {
    const std::lock_guard guard(g_lock);
    g_names.clear();
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

/** Checks one complete flag name table. */
bool valid_names(std::span<const Name> names) noexcept {
    if (names.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < names.size(); ++row) {
        // A repeated or unordered slot would make the lookup depend on row order.
        if (!canonical(names[row]) || (row != 0 && names[row - 1].slot >= names[row].slot)) {
            return false;
        }
    }
    return true;
}

/** Replaces the flag names in one step. */
bool replace_names(std::span<const Name> names) noexcept {
    if (!valid_names(names)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_names.replace(names);
}

/** Finds the name of one flag. */
bool find_name(std::uint16_t slot, Name& name) noexcept {
    name = {};
    const std::shared_lock guard(g_lock);
    const std::span<const Name> rows = g_names.rows();
    const auto found =
        std::lower_bound(rows.begin(), rows.end(), slot, [](const Name& row, std::uint16_t key) {
            return row.slot < key;
        });
    const bool present = found != rows.end() && found->slot == slot;
    if (present) {
        name = *found;
    }
    return present;
}

/** Copies every name in ascending slot order. */
bool snapshot_names(std::span<Name> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_names.snapshot(output, count);
}

/** @return The flag name row count, read under the lock. */
std::size_t name_count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_names.count();
}

} // namespace sunrise::state::build_data::unlock_flags
