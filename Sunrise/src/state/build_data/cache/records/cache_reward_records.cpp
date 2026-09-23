#include "codec.h"

namespace sunrise::state::build_data::cache::records {

bool encode(const rewards::Pool& value, RewardPoolRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.entries = {value.entries.first, value.entries.count};
    return true;
}

bool decode(const RewardPoolRecord& record, rewards::Pool& value) noexcept {
    value = {};
    value.definitionHash = record.definitionHash;
    value.entries = {record.entries.first, record.entries.count};
    return true;
}

bool encode(const rewards::Entry& value, RewardEntryRecord& record) noexcept {
    record = {};
    record.itemIndex = value.itemIndex;
    record.poolIndex = value.poolIndex;
    record.mappingIndex = value.mappingIndex;
    record.quantity = value.quantity;
    record.categoryHash = value.categoryHash;
    record.weight = value.weight;
    record.condition = {value.condition.first, value.condition.count};
    record.modifiers = {value.modifiers.first, value.modifiers.count};
    record.sockets = {value.sockets.first, value.sockets.count};
    return true;
}

bool decode(const RewardEntryRecord& record, rewards::Entry& value) noexcept {
    value = {};
    value.itemIndex = record.itemIndex;
    value.poolIndex = record.poolIndex;
    value.mappingIndex = record.mappingIndex;
    value.quantity = record.quantity;
    value.categoryHash = record.categoryHash;
    value.weight = record.weight;
    value.condition = {record.condition.first, record.condition.count};
    value.modifiers = {record.modifiers.first, record.modifiers.count};
    value.sockets = {record.sockets.first, record.sockets.count};
    return true;
}

bool encode(const rewards::Item& value, RewardItemRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.poolIndex = value.poolIndex;
    record.acquiredFlag = value.acquiredFlag;
    for (std::size_t i = 0; i < record.selections.size(); ++i) {
        record.selections[i] = {value.selections[i].categoryHash, value.selections[i].count};
    }
    record.selectionCount = value.selectionCount;
    record.flags = value.flags;
    return true;
}

bool decode(const RewardItemRecord& record, rewards::Item& value) noexcept {
    value = {};
    value.definitionHash = record.definitionHash;
    value.poolIndex = record.poolIndex;
    value.acquiredFlag = record.acquiredFlag;
    for (std::size_t i = 0; i < value.selections.size(); ++i) {
        value.selections[i] = {record.selections[i].categoryHash, record.selections[i].count};
    }
    value.selectionCount = record.selectionCount;
    value.flags = record.flags;
    return true;
}

bool encode(const rewards::Instruction& value, RewardInstructionRecord& record) noexcept {
    record = {};
    record.opcode = value.opcode;
    record.operand = value.operand;
    return true;
}

bool decode(const RewardInstructionRecord& record, rewards::Instruction& value) noexcept {
    value = {};
    value.opcode = record.opcode;
    value.operand = record.operand;
    return true;
}

bool encode(const rewards::Modifier& value, RewardModifierRecord& record) noexcept {
    record = {};
    record.condition = {value.condition.first, value.condition.count};
    record.valueIndex = value.valueIndex;
    record.value = value.value;
    return true;
}

bool decode(const RewardModifierRecord& record, rewards::Modifier& value) noexcept {
    value = {};
    value.condition = {record.condition.first, record.condition.count};
    value.valueIndex = record.valueIndex;
    value.value = record.value;
    return true;
}

bool encode(const rewards::SocketOverride& value, RewardSocketOverrideRecord& record) noexcept {
    record = {};
    record.socketType = value.socketType;
    record.plugItem = value.plugItem;
    record.plugSet = value.plugSet;
    record.rollSet = value.rollSet;
    record.selection = value.selection;
    return true;
}

bool decode(const RewardSocketOverrideRecord& record, rewards::SocketOverride& value) noexcept {
    value = {};
    value.socketType = record.socketType;
    value.plugItem = record.plugItem;
    value.plugSet = record.plugSet;
    value.rollSet = record.rollSet;
    value.selection = record.selection;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
