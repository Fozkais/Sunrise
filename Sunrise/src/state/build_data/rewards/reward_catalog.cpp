#include "reward_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "../../account/account_state.h"
#include "../../unlocks/definition.h"
#include "../table.h"
#include "core/threading/srw_lock.h"
#include "middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::state::build_data::rewards {
namespace {

core::threading::SrwLock g_lock;
Table<Pool, kPoolCapacity> g_pools;
Table<Entry, kEntryCapacity> g_entries;
Table<Item, kItemCapacity> g_items;
Table<Instruction, kInstructionCapacity> g_instructions;
Table<Modifier, kModifierCapacity> g_modifiers;
Table<SocketOverride, kSocketOverrideCapacity> g_sockets;

View view() noexcept {
    return {g_pools.rows(),
            g_entries.rows(),
            g_items.rows(),
            g_instructions.rows(),
            g_modifiers.rows(),
            g_sockets.rows()};
}

bool valid_depth(View data,
                 std::size_t index,
                 std::array<std::uint8_t, kPoolCapacity>& heights,
                 std::size_t depth = 0) noexcept {
    constexpr std::uint8_t kVisiting = 0xFF;
    if (depth >= kTraversalDepth) {
        return false;
    }
    if (heights[index] != 0) {
        // Reused subtrees must also fit when reached through a deeper path.
        return heights[index] != kVisiting && heights[index] <= kTraversalDepth - depth;
    }
    heights[index] = kVisiting;
    std::uint8_t height = 1;
    const Range range = data.pools[index].entries;
    for (const Entry& entry : data.entries.subspan(range.first, range.count)) {
        if (entry.poolIndex == kAbsent) {
            continue;
        }
        if (!valid_depth(data, entry.poolIndex, heights, depth + 1)) {
            return false;
        }
        height = (std::max)(height, static_cast<std::uint8_t>(heights[entry.poolIndex] + 1));
    }
    heights[index] = height;
    return true;
}

} // namespace

void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_pools.clear();
    g_entries.clear();
    g_items.clear();
    g_instructions.clear();
    g_modifiers.clear();
    g_sockets.clear();
}

bool ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_pools.count() != 0 && g_items.count() != 0;
}

bool valid_instruction(const Instruction& instruction) noexcept {
    const auto slot = instruction.operand;
    switch (static_cast<BankRead>(instruction.opcode)) {
    case BankRead::accountFlag:
        return slot < unlocks::kAccountFlagCapacity;
    case BankRead::profileFlag:
        return slot < unlocks::kProfileFlagCapacity;
    case BankRead::characterFlag:
        return slot < unlocks::kCharacterObjectFlagCapacity;
    case BankRead::accountValue:
        return slot < unlocks::kObjectiveValueCapacity;
    case BankRead::characterValue:
        return slot < unlocks::kCharacterObjectValueCapacity;
    case BankRead::characterClass:
        return slot < kCharacterClassCount;
    case BankRead::externalFlag:
    case BankRead::externalValue:
        return true;
    default:
        break;
    }
    using Op = middleware::content::packages::tables::UnlockOpcode;
    switch (static_cast<Op>(instruction.opcode)) {
    case Op::constant:
    case Op::logicalNot:
    case Op::logicalOr:
    case Op::logicalAnd:
    case Op::equal:
    case Op::greaterThan:
    case Op::greaterOrEqual:
    case Op::lessThan:
    case Op::lessOrEqual:
    case Op::add:
    case Op::negate:
        return true;
    default:
        return false;
    }
}

bool valid(View data) noexcept {
    if (data.pools.empty() || data.pools.size() > kPoolCapacity || data.entries.empty()
        || data.entries.size() > kEntryCapacity || data.items.empty()
        || data.items.size() > kItemCapacity || data.instructions.size() > kInstructionCapacity
        || data.modifiers.size() > kModifierCapacity
        || data.sockets.size() > kSocketOverrideCapacity) {
        return false;
    }
    for (const Pool& pool : data.pools) {
        if ((pool.definitionHash == 0 && pool.entries.count != 0)
            || !fits(pool.entries, data.entries)) {
            return false;
        }
    }
    for (const Entry& entry : data.entries) {
        if ((entry.itemIndex != kAbsent && entry.itemIndex >= data.items.size())
            || (entry.poolIndex != kAbsent && entry.poolIndex >= data.pools.size())
            || !fits(entry.condition, data.instructions) || !fits(entry.modifiers, data.modifiers)
            || !fits(entry.sockets, data.sockets) || !std::isfinite(entry.weight)
            || entry.weight < 0) {
            return false;
        }
    }
    for (const Item& item : data.items) {
        if ((item.definitionHash == 0
             && (item.poolIndex != kAbsent || item.acquiredFlag != kAbsent
                 || item.selectionCount != 0))
            || item.selectionCount > item.selections.size()
            || (item.acquiredFlag != kAbsent && item.acquiredFlag >= unlocks::kAccountFlagCapacity)
            || (item.poolIndex != kAbsent && item.poolIndex >= data.pools.size())) {
            return false;
        }
    }
    if (!std::all_of(data.instructions.begin(), data.instructions.end(), valid_instruction)) {
        return false;
    }
    for (const Modifier& modifier : data.modifiers) {
        if (!fits(modifier.condition, data.instructions) || !std::isfinite(modifier.value)) {
            return false;
        }
    }
    for (const SocketOverride& socket : data.sockets) {
        if (socket.socketType == kAbsent
            || (socket.plugItem != kAbsent && socket.plugItem >= data.items.size())) {
            return false;
        }
    }
    std::array<std::uint8_t, kPoolCapacity> heights{};
    for (std::size_t index = 0; index < data.pools.size(); ++index) {
        if (!valid_depth(data, index, heights)) {
            return false;
        }
    }
    return true;
}

bool replace(View data) noexcept {
    if (!valid(data)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_pools.replace(data.pools) && g_entries.replace(data.entries)
           && g_items.replace(data.items) && g_instructions.replace(data.instructions)
           && g_modifiers.replace(data.modifiers) && g_sockets.replace(data.sockets);
}

bool read(void* context, bool (*consume)(void*, View) noexcept) noexcept {
    const std::shared_lock guard(g_lock);
    return consume != nullptr && g_pools.count() != 0 && consume(context, view());
}

bool find_item(std::uint16_t itemIndex, Item& item) noexcept {
    item = {};
    const std::shared_lock guard(g_lock);
    if (itemIndex >= g_items.count() || g_items.rows()[itemIndex].definitionHash == 0) {
        return false;
    }
    item = g_items.rows()[itemIndex];
    return true;
}

bool snapshot(std::span<Pool> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_pools.snapshot(output, count);
}

bool snapshot(std::span<Entry> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_entries.snapshot(output, count);
}

bool snapshot(std::span<Item> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_items.snapshot(output, count);
}

bool snapshot(std::span<Instruction> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_instructions.snapshot(output, count);
}

bool snapshot(std::span<Modifier> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_modifiers.snapshot(output, count);
}

bool snapshot(std::span<SocketOverride> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_sockets.snapshot(output, count);
}

} // namespace sunrise::state::build_data::rewards
