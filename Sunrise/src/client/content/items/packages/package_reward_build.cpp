#include "package_reward_build.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/internal.h"
#include "../../../../state/build_data/rewards/reward_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "core/logging/log.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::rewards;

/** Investment-root slots identify reward pools and unlock-slot bindings. */
constexpr std::size_t kPoolSlot = 88;
constexpr std::size_t kExpressionSlot = 109;
constexpr std::size_t kFlagSlot = 112;
constexpr std::size_t kValueSlot = 114;
/** Native reward schema classes and fixed row sizes. */
constexpr std::uint32_t kPoolClass = 0x80807553U;
constexpr std::uint32_t kPoolRowClass = 0x8080748CU;
constexpr std::uint32_t kEntryClass = 0x8080748EU;
constexpr std::uint32_t kExpressionClass = 0x80807D31U;
constexpr std::uint32_t kModifierClass = 0x80807490U;
constexpr std::uint32_t kSocketClass = 0x80803062U;
constexpr std::uint32_t kWrapperClass = 0x808077CCU;
constexpr std::uint32_t kSelectionClass = 0x808077CFU;
constexpr std::uint32_t kFlagTableClass = 0x80807D49U, kFlagRowClass = 0x80807D4FU;
constexpr std::uint32_t kValueTableClass = 0x80807C92U, kValueRowClass = 0x80807C96U;
constexpr std::uint32_t kExpressionTableClass = 0x80807C49U, kExpressionRowClass = 0x80807C4FU;
constexpr std::uint32_t kConditionClass = 0x80807D2FU;
constexpr std::size_t kPoolStride = 24, kEntryStride = 80, kModifierStride = 24;
constexpr std::size_t kBindingStride = 8, kExpressionRowStride = 24;
constexpr std::size_t kSocketStride = 12, kSelectionStride = 12;
/** Item headers hold a relative wrapper pointer and an acquired-unlock slot. */
constexpr std::size_t kWrapperField = 0x58;
constexpr std::size_t kAcquiredFlagField = 0xDA;

/** Byte offsets inside the serialized reward rows. */
constexpr std::size_t kEntryQuantityOffset = 4, kEntryPoolOffset = 8;
constexpr std::size_t kEntryCategoryOffset = 20, kEntryWeightOffset = 24;
constexpr std::size_t kEntryBucketOffset = 28, kEntryConditionOffset = 32;
constexpr std::size_t kEntryModifiersOffset = 48, kEntrySocketsOffset = 64;
constexpr std::size_t kModifierValueIndexOffset = 16, kModifierValueOffset = 20;
constexpr std::size_t kPoolEntriesOffset = 8, kExpressionBodyOffset = 8;
constexpr std::size_t kWrapperSelectionsOffset = 8, kWrapperFlagsOffset = 24;
constexpr std::size_t kSelectionCountOffset = 4;
constexpr std::size_t kSocketPlugOffset = 2, kSocketPlugSetOffset = 4;
constexpr std::size_t kSocketRollSetOffset = 6, kSocketSelectionOffset = 8;
/** Relative definition bodies are preceded by their four-byte schema class. */
constexpr std::size_t kDefinitionClassPrefixSize = sizeof(std::uint32_t);

template <typename T> bool append(std::vector<T>& bank, T value, std::size_t capacity) noexcept {
    if (bank.size() >= capacity) {
        return false;
    }
    try {
        bank.push_back(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool root_table(const reader::Source& source,
                reader::Scratch& scratch,
                std::span<const std::byte> root,
                std::size_t slot,
                std::vector<std::byte>& blob,
                std::uint32_t expectedClass = 0) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t cls = 0;
    return tables::slot_tag(root, slot, tag) && tables::package_of(tag) != tables::kAbsentPackageId
           && reader::read_tag(source, scratch, tag, blob, cls)
           && (expectedClass == 0 || cls == expectedClass);
}

bool read_reward_entry(std::span<const std::byte> blob,
                       std::size_t at,
                       domain::Entry& entry) noexcept {
    return tables::read(blob, at, entry.itemIndex)
           && tables::read(blob, at + kEntryQuantityOffset, entry.quantity)
           && tables::read(blob, at + kEntryPoolOffset, entry.poolIndex)
           && tables::read(blob, at + kEntryCategoryOffset, entry.categoryHash)
           && tables::read(blob, at + kEntryWeightOffset, entry.weight)
           && tables::read(blob, at + kEntryBucketOffset, entry.bucketHash)
           && std::isfinite(entry.weight) && entry.weight >= 0;
}

} // namespace

bool RewardBuild::entry(std::span<const std::byte> blob, std::size_t at) noexcept {
    domain::Entry out{};
    if (!read_reward_entry(blob, at, out)
        || !conditions.read(blob, at + kEntryConditionOffset, instructions, out.condition)) {
        return false;
    }
    tables::Array rows{};
    if (!tables::read_array(
            blob, at + kEntryModifiersOffset, kModifierClass, kModifierStride, rows)) {
        return false;
    }
    out.modifiers = {static_cast<std::uint32_t>(modifiers.size()),
                     static_cast<std::uint32_t>(rows.count)};
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto offset = rows.dataOffset + i * kModifierStride;
        domain::Modifier modifier{};
        if (!conditions.read(blob, offset, instructions, modifier.condition)
            || !tables::read(blob, offset + kModifierValueIndexOffset, modifier.valueIndex)
            || !tables::read(blob, offset + kModifierValueOffset, modifier.value)
            || !std::isfinite(modifier.value)
            || !append(modifiers, modifier, domain::kModifierCapacity)) {
            return false;
        }
    }
    std::array<domain::SocketOverride, domain::kSocketsPerItem> overrides{};
    std::size_t count = 0;
    if (!read_reward_sockets(blob, at + kEntrySocketsOffset, overrides, count)
        || count > domain::kSocketOverrideCapacity - sockets.size()) {
        return false;
    }
    out.sockets = {static_cast<std::uint32_t>(sockets.size()), static_cast<std::uint32_t>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        if (overrides[i].socketType == domain::kAbsent
            || !append(sockets, overrides[i], domain::kSocketOverrideCapacity)) {
            return false;
        }
    }
    return append(entries, out, domain::kEntryCapacity);
}

void RewardConditions::load_class_flags(const reader::Source& source,
                                        reader::Scratch& scratch,
                                        std::span<const std::byte> root) noexcept {
    classFlags_.fill(domain::kAbsent);
    // Class rows name a default finisher whose use gate supplies the computed class flag.
    constexpr std::size_t kClassSlot = 12, kClassStride = 32, kDefaultFinisherOffset = 24;
    constexpr std::uint32_t kClassTable = 0x808075BEU, kClassRow = 0x808074FAU;
    constexpr std::size_t kUseConditionPointer = 24;
    constexpr std::uint32_t kUseConditionClass = 0x80802980U;
    std::vector<std::byte> classes, index, blob;
    tables::Array classRows{}, itemRows{};
    if (!root_table(source, scratch, root, kClassSlot, classes, kClassTable)
        || !tables::read_array(
            classes, tables::kTableArrayDescriptor, kClassRow, kClassStride, classRows)
        || classRows.count != classFlags_.size()
        || !root_table(source, scratch, root, tables::kItemTableSlot, index)
        || !tables::read_array(index,
                               tables::kTableArrayDescriptor,
                               tables::kItemIndexTableClass,
                               tables::kItemIndexRowStride,
                               itemRows)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=pkg stage=class_flags result=skip reason=class_tables");
        return;
    }
    for (std::size_t i = 0; i < classFlags_.size(); ++i) {
        std::uint16_t itemIndex = domain::kAbsent;
        tables::IndexRow item{};
        tables::Array condition{};
        std::uint32_t cls = 0, opcode = 0, flag = domain::kAbsent;
        std::int64_t relative = 0;
        if (!tables::read(std::span<const std::byte>{classes},
                          classRows.dataOffset + i * kClassStride + kDefaultFinisherOffset,
                          itemIndex)
            || !tables::index_row(index, itemRows, itemIndex, item)
            || !reader::read_tag(source, scratch, item.targetTag, blob, cls)
            || cls != tables::kItemDefinitionClass
            || !tables::read(std::span<const std::byte>{blob}, kUseConditionPointer, relative)
            || relative <= 0 || static_cast<std::uint64_t>(relative) > blob.size()
            || kUseConditionPointer > blob.size() - static_cast<std::size_t>(relative)) {
            core::log::writef(
                core::log::Channel::client,
                core::log::Level::warn,
                "ev=pkg stage=class_flags class=%zu result=skip reason=default_finisher",
                i);
            continue;
        }
        const auto at = kUseConditionPointer + static_cast<std::size_t>(relative);
        if (tables::read(std::span<const std::byte>{blob}, at - kDefinitionClassPrefixSize, cls)
            && cls == kUseConditionClass
            && tables::read_array(
                blob, at, kExpressionClass, tables::kUnlockInstructionStride, condition)
            && condition.count == 1
            && tables::read(std::span<const std::byte>{blob}, condition.dataOffset, opcode)
            && opcode == static_cast<std::uint32_t>(tables::UnlockOpcode::flag)
            && tables::read(std::span<const std::byte>{blob},
                            condition.dataOffset + tables::kUnlockInstructionOperandOffset,
                            flag)
            && flag < domain::kAbsent) {
            classFlags_[i] = static_cast<std::uint16_t>(flag);
        } else {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=pkg stage=class_flags class=%zu result=skip reason=use_condition",
                              i);
        }
    }
}

bool RewardConditions::load(const reader::Source& source,
                            reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept {
    maps_ = &maps;
    load_class_flags(source, scratch, root);
    // Shared expressions are expanded before the runtime evaluates reward conditions.
    return root_table(source, scratch, root, kFlagSlot, flags_, kFlagTableClass)
           && tables::read_array(
               flags_, tables::kTableArrayDescriptor, kFlagRowClass, kBindingStride, flagRows_)
           && root_table(source, scratch, root, kValueSlot, values_, kValueTableClass)
           && tables::read_array(
               values_, tables::kTableArrayDescriptor, kValueRowClass, kBindingStride, valueRows_)
           && root_table(
               source, scratch, root, kExpressionSlot, expressions_, kExpressionTableClass)
           && tables::read_array(expressions_,
                                 tables::kTableArrayDescriptor,
                                 kExpressionRowClass,
                                 kExpressionRowStride,
                                 expressionRows_);
}

bool RewardConditions::bind(domain::Instruction& instruction) const noexcept {
    const auto opcode =
        static_cast<middleware::content::packages::tables::UnlockOpcode>(instruction.opcode);
    const bool flag = opcode == middleware::content::packages::tables::UnlockOpcode::flag;
    if (!flag && opcode != middleware::content::packages::tables::UnlockOpcode::loadValue) {
        return true;
    }
    const auto& rows = flag ? flagRows_ : valueRows_;
    const std::span<const std::byte> blob = flag ? flags_ : values_;
    if (instruction.operand >= rows.count) {
        return false;
    }
    const std::size_t at = rows.dataOffset + instruction.operand * kBindingStride;
    std::uint32_t hash = 0;
    if (!tables::read(blob, at, hash)) {
        return false;
    }
    using B = domain::BankRead;
    if (flag) {
        for (std::size_t characterClass = 0; characterClass < classFlags_.size();
             ++characterClass) {
            if (classFlags_[characterClass] != domain::kAbsent
                && instruction.operand == classFlags_[characterClass]) {
                instruction = {static_cast<std::uint32_t>(B::characterClass),
                               static_cast<std::uint32_t>(characterClass)};
                return true;
            }
        }
    }
    const auto& account = flag ? maps_->accountFlag : maps_->accountValue;
    const auto& character = flag ? maps_->characterFlag : maps_->characterValue;
    const auto accountIndex = bank_index(account, static_cast<std::int32_t>(instruction.operand));
    const auto characterIndex =
        bank_index(character, static_cast<std::int32_t>(instruction.operand));
    if (accountIndex != kUnmappedSlot) {
        instruction = {static_cast<std::uint32_t>(flag ? B::accountFlag : B::accountValue),
                       accountIndex};
    } else if (flag
               && bank_index(maps_->profileFlag, static_cast<std::int32_t>(instruction.operand))
                      != kUnmappedSlot) {
        instruction = {
            static_cast<std::uint32_t>(B::profileFlag),
            bank_index(maps_->profileFlag, static_cast<std::int32_t>(instruction.operand))};
    } else if (characterIndex != kUnmappedSlot) {
        instruction = {static_cast<std::uint32_t>(flag ? B::characterFlag : B::characterValue),
                       characterIndex};
    } else {
        instruction = {static_cast<std::uint32_t>(flag ? B::externalFlag : B::externalValue), hash};
    }
    return true;
}

bool RewardConditions::append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<domain::Instruction>& bank,
                                         std::size_t depth) const noexcept {
    tables::Array rows{};
    if (depth >= domain::kTraversalDepth
        || !tables::read_array(
            blob, at, kExpressionClass, tables::kUnlockInstructionStride, rows)) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Instruction instruction{};
        if (!tables::read(
                blob, rows.dataOffset + i * tables::kUnlockInstructionStride, instruction.opcode)
            || !tables::read(blob,
                             rows.dataOffset + i * tables::kUnlockInstructionStride
                                 + tables::kUnlockInstructionOperandOffset,
                             instruction.operand)) {
            return false;
        }
        if (static_cast<middleware::content::packages::tables::UnlockOpcode>(instruction.opcode)
            == middleware::content::packages::tables::UnlockOpcode::expression) {
            const auto before = bank.size();
            if (instruction.operand >= expressionRows_.count
                || !append_expression(expressions_,
                                      expressionRows_.dataOffset
                                          + instruction.operand * kExpressionRowStride
                                          + kExpressionBodyOffset,
                                      bank,
                                      depth + 1)
                || bank.size() == before) {
                return false;
            }
        } else if (!bind(instruction) || !domain::valid_instruction(instruction)
                   || !append(bank, instruction, domain::kInstructionCapacity)) {
            return false;
        }
    }
    return true;
}

bool RewardConditions::read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<domain::Instruction>& bank,
                            domain::Range& range) const noexcept {
    const auto first = bank.size();
    if (!append_expression(blob, at, bank, 0)) {
        bank.resize(first);
        return false;
    }
    range = {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bank.size() - first)};
    return true;
}

bool RewardConditions::read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<domain::Instruction> output,
                                 std::size_t& count) const noexcept {
    count = 0;
    tables::Array rows{};
    std::vector<domain::Instruction> instructions;
    if (!tables::read_array(blob, at, kConditionClass, tables::kUnlockExpressionFieldSize, rows)) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Range expression{};
        if (!read(blob,
                  rows.dataOffset + i * tables::kUnlockExpressionFieldSize,
                  instructions,
                  expression)
            || expression.count == 0) {
            return false;
        }
        // Every expression attached to a progression reward must hold.
        if (i != 0
            && !append(instructions,
                       domain::Instruction{
                           static_cast<std::uint32_t>(
                               middleware::content::packages::tables::UnlockOpcode::logicalAnd),
                           0},
                       output.size())) {
            return false;
        }
        if (instructions.size() > output.size()) {
            return false;
        }
    }
    std::copy(instructions.begin(), instructions.end(), output.begin());
    count = instructions.size();
    return true;
}

bool read_reward_sockets(std::span<const std::byte> blob,
                         std::size_t at,
                         std::span<domain::SocketOverride> output,
                         std::size_t& count) noexcept {
    count = 0;
    tables::Array rows{};
    if (!tables::read_array(blob, at, kSocketClass, kSocketStride, rows)
        || rows.count > output.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t p = rows.dataOffset + i * kSocketStride;
        auto& socket = output[i];
        if (!tables::read(blob, p, socket.socketType)
            || !tables::read(blob, p + kSocketPlugOffset, socket.plugItem)
            || !tables::read(blob, p + kSocketPlugSetOffset, socket.plugSet)
            || !tables::read(blob, p + kSocketRollSetOffset, socket.rollSet)
            || !tables::read(blob, p + kSocketSelectionOffset, socket.selection)) {
            return false;
        }
    }
    count = static_cast<std::size_t>(rows.count);
    return true;
}

bool RewardBuild::load(const reader::Source& source,
                       reader::Scratch& scratch,
                       std::span<const std::byte> root,
                       const SlotMaps& maps) noexcept {
    loaded_ = false;
    pools.clear();
    entries.clear();
    instructions.clear();
    modifiers.clear();
    sockets.clear();
    std::vector<std::byte> blob;
    tables::Array rows{};
    if (!conditions.load(source, scratch, root, maps)
        || !root_table(source, scratch, root, kPoolSlot, blob, kPoolClass)
        || !tables::read_array(
            blob, tables::kTableArrayDescriptor, kPoolRowClass, kPoolStride, rows)
        || rows.count == 0 || rows.count > domain::kPoolCapacity) {
        return false;
    }
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto at = rows.dataOffset + i * kPoolStride;
        domain::Pool pool{};
        tables::Array members{};
        if (tables::read(blob, at, pool.definitionHash) && pool.definitionHash != 0
            && tables::read_array(
                blob, at + kPoolEntriesOffset, kEntryClass, kEntryStride, members)) {
            pool.entries.first = static_cast<std::uint32_t>(entries.size());
            for (std::size_t j = 0; j < members.count; ++j) {
                const auto beforeInstructions = instructions.size(),
                           beforeModifiers = modifiers.size(), beforeSockets = sockets.size();
                if (!entry(blob, members.dataOffset + j * kEntryStride)) {
                    instructions.resize(beforeInstructions);
                    modifiers.resize(beforeModifiers);
                    sockets.resize(beforeSockets);
                    ++skipped;
                }
            }
            pool.entries.count = static_cast<std::uint32_t>(entries.size()) - pool.entries.first;
        } else {
            pool = {};
            ++skipped;
        }
        if (!append(pools, pool, domain::kPoolCapacity)) {
            return false;
        }
    }
    if (skipped != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=rewards skipped=%zu",
                          skipped);
    }
    loaded_ = true;
    return true;
}

bool RewardBuild::read_item(std::uint32_t hash,
                            std::span<const std::byte> blob,
                            domain::Item& item) noexcept {
    std::uint16_t acquired = domain::kAbsent;
    std::int64_t relative = 0;
    std::uint32_t cls = 0;
    if (!tables::read(blob, kWrapperField, relative)
        || !tables::read(blob, kAcquiredFlagField, acquired)) {
        return false;
    }
    item.definitionHash = hash;
    if (acquired != domain::kAbsent) {
        domain::Instruction flag{
            static_cast<std::uint32_t>(middleware::content::packages::tables::UnlockOpcode::flag),
            acquired};
        if (!conditions.bind(flag) || !domain::valid_instruction(flag)) {
            return false;
        }
        if (flag.opcode == static_cast<std::uint32_t>(domain::BankRead::accountFlag)) {
            item.acquiredFlag = static_cast<std::uint16_t>(flag.operand);
        }
    }
    if (relative != 0) {
        if (relative < 0 || static_cast<std::uint64_t>(relative) > blob.size()
            || kWrapperField > blob.size() - static_cast<std::size_t>(relative)) {
            return false;
        }
        const auto at = kWrapperField + static_cast<std::size_t>(relative);
        tables::Array selections{};
        if (!tables::read(blob, at - kDefinitionClassPrefixSize, cls) || cls != kWrapperClass
            || !tables::read(blob, at, item.poolIndex)
            || !tables::read(blob, at + kWrapperFlagsOffset, item.flags)
            || !tables::read_array(
                blob, at + kWrapperSelectionsOffset, kSelectionClass, kSelectionStride, selections)
            || selections.count > item.selections.size()) {
            return false;
        }
        item.selectionCount = static_cast<std::uint8_t>(selections.count);
        for (std::size_t j = 0; j < selections.count; ++j) {
            auto& selection = item.selections[j];
            const auto p = selections.dataOffset + j * kSelectionStride;
            if (!tables::read(blob, p, selection.categoryHash)
                || !tables::read(blob, p + kSelectionCountOffset, selection.count)) {
                return false;
            }
        }
    }
    return true;
}

bool RewardBuild::begin_items(std::size_t count) noexcept {
    try {
        if (!loaded_ || count == 0 || count > domain::kItemCapacity) {
            return false;
        }
        items.assign(count, {});
        return true;
    } catch (...) {
        return false;
    }
}

void RewardBuild::item(std::uint16_t index,
                       std::uint32_t hash,
                       std::span<const std::byte> blob) noexcept {
    domain::Item parsed{};
    if (index < items.size() && read_item(hash, blob, parsed)) {
        items[index] = parsed;
    }
}

bool RewardBuild::publish() noexcept {
    if (!loaded_) {
        return false;
    }
    std::size_t write = 0, socketWrite = 0;
    for (auto& pool : pools) {
        const auto range = pool.entries;
        pool.entries.first = static_cast<std::uint32_t>(write);
        for (std::size_t i = range.first; i < range.first + range.count; ++i) {
            const auto& row = entries[i];
            bool valid = (row.itemIndex == domain::kAbsent || row.itemIndex < items.size())
                         && (row.poolIndex == domain::kAbsent || row.poolIndex < pools.size());
            for (const auto& socket :
                 std::span(sockets).subspan(row.sockets.first, row.sockets.count)) {
                valid &= socket.plugItem == domain::kAbsent || socket.plugItem < items.size();
            }
            if (valid) {
                auto retained = row;
                retained.sockets.first = static_cast<std::uint32_t>(socketWrite);
                for (const auto& socket :
                     std::span(sockets).subspan(row.sockets.first, row.sockets.count)) {
                    sockets[socketWrite++] = socket;
                }
                entries[write++] = retained;
            }
        }
        pool.entries.count = static_cast<std::uint32_t>(write) - pool.entries.first;
    }
    entries.resize(write);
    sockets.resize(socketWrite);
    for (auto& item : items) {
        if (item.poolIndex != domain::kAbsent && item.poolIndex >= pools.size()) {
            item = {};
        }
    }
    return state::build_data::publish_reward_definitions(
        {pools, entries, items, instructions, modifiers, sockets});
}

} // namespace sunrise::client::content::items::packages
