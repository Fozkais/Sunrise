#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "../../../../middleware/content/packages/tables/activity_display_name_reader.h"
#include "../../../../state/build_data/unlock_flags/unlock_flag_catalog.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::unlock_flags;
namespace display = middleware::content::packages::tables::activity_display_names;

/** The reader and scratch a string bank is read through. */
struct PackageContext final {
    const reader::Source* source{};
    reader::Scratch* scratch{};
};

/** Adapts the package reader while requiring the exact class the string reader asks for. */
[[nodiscard]] bool read_tag(void* opaque,
                            std::uint32_t tag,
                            std::uint32_t expectedClass,
                            std::vector<std::byte>& output) noexcept {
    output.clear();
    if (opaque == nullptr) {
        return false;
    }
    const auto& context = *static_cast<const PackageContext*>(opaque);
    std::uint32_t actualClass = 0;
    return context.source != nullptr && context.scratch != nullptr
           && reader::read_tag(*context.source, *context.scratch, tag, output, actualClass)
           && actualClass == expectedClass;
}

/** A flag that some definition refers to, and the string reference that names that definition. */
struct Candidate final {
    std::uint16_t slot{};
    domain::NameSource source{};
    std::uint16_t references{};
    display::BankedReference name{};
};

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/** Reads one localized reference: the string bank row, then the string hash. */
[[nodiscard]] bool read_reference(std::span<const std::byte> blob,
                                  std::size_t offset,
                                  display::BankedReference& reference) noexcept {
    return read(blob, offset, reference.bankIndex) && read(blob, offset + 4U, reference.stringHash);
}

/**
 * Collects the flags collectibles test, each with the item name of the first collectible to test
 * it.
 * @param source Installed package source.
 * @param storage Pass storage holding the collectible scratch and the item strings table.
 * @param flagCount Slots the flag table holds.
 * @param output Receives one candidate per tested flag.
 * @return True when the item strings table read.
 */
[[nodiscard]] bool collect_collectible_names(const reader::Source& source,
                                             Storage& storage,
                                             std::size_t flagCount,
                                             std::vector<Candidate>& output) {
    std::size_t collectibleCount = 0;
    std::uint32_t tableClass = 0;
    tables::Array itemRows{};
    if (!state::build_data::collectibles::snapshot(storage.collectibleRows, collectibleCount)
        || !reader::read_tag(source,
                             storage.scratch,
                             tables::kItemStringsIndexTag,
                             storage.itemStringsTable,
                             tableClass)
        || !tables::find_array_at(std::span<const std::byte>{storage.itemStringsTable},
                                  tables::kTableArrayDescriptor,
                                  itemRows)
        || itemRows.elementClass != tables::kItemStringsIndexRowClass) {
        return false;
    }
    const std::span<const std::byte> itemTable{storage.itemStringsTable};
    std::vector<std::uint16_t> references(flagCount, 0);
    for (std::size_t index = 0; index < collectibleCount; ++index) {
        const auto& collectible = storage.collectibleRows[index];
        const std::uint16_t slot = collectible.acquiredFlagSlot;
        if (slot >= flagCount
            || collectible.itemDefinitionIndex
                   == state::build_data::collectibles::kUnavailableItemDefinitionIndex
            || references[slot] == (std::numeric_limits<std::uint16_t>::max)()) {
            continue;
        }
        if (references[slot]++ != 0) {
            continue;
        }
        // The first collectible to test a flag names it. A collectible whose item strings will not
        // read leaves the flag to the next one, so the count starts again from this flag's row.
        tables::IndexRow entry{};
        Candidate candidate{};
        candidate.slot = slot;
        candidate.source = domain::NameSource::collectible;
        if (!tables::index_row(itemTable, itemRows, collectible.itemDefinitionIndex, entry)
            || !reader::read_tag(source, storage.scratch, entry.targetTag, storage.definition)
            || !read_reference(std::span<const std::byte>{storage.definition},
                               tables::kItemStringsNameOffset,
                               candidate.name)) {
            references[slot] = 0;
            continue;
        }
        output.push_back(candidate);
    }
    for (Candidate& candidate : output) {
        candidate.references = references[candidate.slot];
    }
    return true;
}

/**
 * Collects the flags records set when completed, each with the name of the first record to set it.
 * @param source Installed package source.
 * @param storage Pass storage receiving the record and display tables.
 * @param root Investment root blob.
 * @param flagCount Slots the flag table holds.
 * @param taken Flags a collectible already named.
 * @param output Receives one candidate per newly tested flag.
 * @return True when both record tables read.
 */
[[nodiscard]] bool collect_record_names(const reader::Source& source,
                                        Storage& storage,
                                        std::span<const std::byte> root,
                                        std::size_t flagCount,
                                        const std::vector<bool>& taken,
                                        std::vector<Candidate>& output) {
    std::uint32_t tableTag = 0;
    std::uint32_t displayClass = 0;
    tables::Array rows{};
    tables::Array displayRows{};
    if (!tables::slot_tag(root, tables::kRecordTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child)
        || !tables::find_array_at(
            std::span<const std::byte>{storage.child}, tables::kTableArrayDescriptor, rows)
        || !reader::read_tag(source,
                             storage.scratch,
                             tables::kRecordDisplayTableTag,
                             storage.displayTable,
                             displayClass)
        || !tables::find_array_at(std::span<const std::byte>{storage.displayTable},
                                  tables::kTableArrayDescriptor,
                                  displayRows)
        || displayRows.elementClass != tables::kRecordDisplayRowClass
        || rows.count > displayRows.count
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kRecordRowStride
               > storage.child.size()
        || displayRows.dataOffset
                   + static_cast<std::size_t>(displayRows.count) * tables::kRecordDisplayRowStride
               > storage.displayTable.size()) {
        return false;
    }
    const std::span<const std::byte> table{storage.child};
    const std::span<const std::byte> displayTable{storage.displayTable};
    std::vector<std::uint16_t> references(flagCount, 0);
    const std::size_t start = output.size();
    for (std::size_t row = 0; row < rows.count; ++row) {
        std::int16_t slot = 0;
        if (!read(table,
                  rows.dataOffset + row * tables::kRecordRowStride
                      + tables::kRecordCompletionFlagOffset,
                  slot)
            || slot < 0 || static_cast<std::size_t>(slot) >= flagCount || taken[slot]
            || references[slot] == (std::numeric_limits<std::uint16_t>::max)()) {
            continue;
        }
        if (references[slot]++ != 0) {
            continue;
        }
        Candidate candidate{};
        candidate.slot = static_cast<std::uint16_t>(slot);
        candidate.source = domain::NameSource::record;
        if (!read_reference(displayTable,
                            displayRows.dataOffset + row * tables::kRecordDisplayRowStride
                                + tables::kRecordDisplayNameOffset,
                            candidate.name)) {
            references[slot] = 0;
            continue;
        }
        output.push_back(candidate);
    }
    for (std::size_t index = start; index < output.size(); ++index) {
        output[index].references = references[output[index].slot];
    }
    return true;
}

/** Copies a name into a flag row, cut at a character boundary and free of control characters. */
[[nodiscard]] bool store_name(const display::Name& name, domain::Name& row) noexcept {
    std::size_t length = (std::min)(static_cast<std::size_t>(name.length), domain::kNameLength);
    // Never end on the first bytes of a multi-byte character.
    while (length != 0 && length < name.length
           && (static_cast<unsigned char>(name.value[length]) & 0xC0U) == 0x80U) {
        --length;
    }
    if (length == 0) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        const char letter = name.value[index];
        row.text[index] = static_cast<unsigned char>(letter) < 0x20U ? ' ' : letter;
    }
    row.length = static_cast<std::uint8_t>(length);
    return true;
}

} // namespace

/**
 * Names the unlock flags that a collectible tests or a record sets, in English.
 * A flag several definitions refer to takes the name of the first one and remembers how many.
 * A collectible names a flag before a record does.
 * @param source Installed package source.
 * @param storage Pass storage receiving the name rows.
 * @param globals Globals container blob, which names the string bank index.
 * @param root Investment root blob.
 * @return True when the tables read and the names resolved; an unnamed flag is not a failure.
 */
bool build_unlock_flag_names(const reader::Source& source,
                             Storage& storage,
                             std::span<const std::byte> globals,
                             std::span<const std::byte> root) noexcept {
    storage.unlockFlagNameCount = 0;
    const std::size_t flagCount = state::build_data::unlock_flags::count();
    std::uint32_t bankIndexTag = 0;
    if (flagCount == 0 || !state::build_data::collectible_definitions_ready()
        || !tables::child_tag(globals, tables::kStringBankIndexChild, bankIndexTag)
        || bankIndexTag == 0) {
        return false;
    }
    try {
        std::vector<Candidate> candidates;
        if (!collect_collectible_names(source, storage, flagCount, candidates)) {
            return false;
        }
        std::vector<bool> taken(flagCount, false);
        for (const Candidate& candidate : candidates) {
            taken[candidate.slot] = true;
        }
        if (!collect_record_names(source, storage, root, flagCount, taken, candidates)) {
            return false;
        }
        std::vector<display::BankedReference> references;
        references.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            references.push_back(candidate.name);
        }
        PackageContext context{&source, &storage.scratch};
        const display::Source strings{&context, &read_tag, 0, bankIndexTag};
        display::Snapshot names;
        if (references.empty() || !display::resolve_banked(strings, references, names)
            || names.names.size() != candidates.size()) {
            return false;
        }
        // Rows go out in slot order, whichever kind named the slot.
        std::vector<std::size_t> order(candidates.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            return candidates[left].slot < candidates[right].slot;
        });
        for (const std::size_t index : order) {
            const display::Name& name = names.names[index];
            if (name.authoredEmpty
                || storage.unlockFlagNameCount >= storage.unlockFlagNames.size()) {
                continue;
            }
            domain::Name& row = storage.unlockFlagNames[storage.unlockFlagNameCount];
            row = {};
            row.slot = candidates[index].slot;
            row.source = candidates[index].source;
            row.references = candidates[index].references;
            if (store_name(name, row)) {
                ++storage.unlockFlagNameCount;
            }
        }
        return true;
    } catch (...) {
        storage.unlockFlagNameCount = 0;
        return false;
    }
}

} // namespace sunrise::client::content::items::packages
