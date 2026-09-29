#include <cstring>

#include "internal.h"

namespace sunrise::client::content::items::packages {

/**
 * Reads the installed unlock flag table: each flag's hash and the bank row that stores it.
 * The table is keyed by slot, so a flag's position is its slot and the rows publish in that order.
 * @param source Installed package source.
 * @param storage Pass storage receiving the flag rows.
 * @param root Investment root blob.
 * @return True when the table read and produced at least one row.
 */
bool build_unlock_flags(const reader::Source& source,
                        Storage& storage,
                        std::span<const std::byte> root) noexcept {
    namespace domain = state::build_data::unlock_flags;
    storage.unlockFlagCount = 0;
    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kUnlockFlagSlotTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.unlockSlotTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.unlockSlotTable},
                                  tables::kTableArrayDescriptor,
                                  rows)
        || rows.elementClass != tables::kUnlockSlotRowClass || rows.count == 0
        || rows.count > storage.unlockFlagRows.size()) {
        return false;
    }
    const std::span<const std::byte> table{storage.unlockSlotTable};
    if (rows.dataOffset > table.size()
        || rows.count > (table.size() - rows.dataOffset) / tables::kUnlockSlotRowStride) {
        return false;
    }
    const auto count = static_cast<std::size_t>(rows.count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        const std::size_t at = rows.dataOffset + slot * tables::kUnlockSlotRowStride;
        domain::Definition& row = storage.unlockFlagRows[slot];
        row = {};
        std::memcpy(&row.hash, table.data() + at + tables::kUnlockSlotHashOffset, sizeof row.hash);
        std::memcpy(&row.bank, table.data() + at + tables::kUnlockSlotBankOffset, sizeof row.bank);
        std::memcpy(
            &row.row, table.data() + at + tables::kUnlockSlotBankIndexOffset, sizeof row.row);
    }
    storage.unlockFlagCount = count;
    return true;
}

} // namespace sunrise::client::content::items::packages
