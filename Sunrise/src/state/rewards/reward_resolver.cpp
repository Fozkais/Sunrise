#include "reward_resolver.h"

#include <algorithm>
#include <cmath>

#include "../build_data/rewards/reward_catalog.h"
#include "../build_data/runtime.h"
#include "middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::state::rewards {
namespace {

namespace definitions = build_data::rewards;
/** The empty bucket tag inherits the enclosing pool's bucket constraint. */
constexpr std::uint32_t kEmptyTag = 0x811C9DC5U;
/** Native postfix expressions use at most 256 signed 32-bit stack values. */
constexpr std::size_t kExpressionCapacity = 256;

bool refuse(const Context& context, const char* reason) noexcept {
    if (context.refusal != nullptr) {
        *context.refusal = reason;
    }
    return false;
}

bool apply_condition_operator(const definitions::Instruction& instruction,
                              std::span<std::int32_t> stack,
                              std::size_t& size,
                              const Context& context) noexcept {
    if (!definitions::valid_instruction(instruction)) {
        return refuse(context, "condition_opcode");
    }
    using Op = middleware::content::packages::tables::UnlockOpcode;
    const auto opcode = static_cast<Op>(instruction.opcode);
    const bool unary = opcode == Op::logicalNot || opcode == Op::negate;
    if (size < (unary ? 1U : 2U)) {
        return refuse(context, "condition_shape");
    }
    const auto right = unary ? 0 : stack[--size];
    auto& left = stack[size - 1];
    switch (opcode) {
    case Op::logicalNot:
        left = left == 0;
        break;
    case Op::negate:
        left = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(left));
        break;
    case Op::logicalOr:
        left = left != 0 || right != 0;
        break;
    case Op::logicalAnd:
        left = left != 0 && right != 0;
        break;
    case Op::equal:
        left = left == right;
        break;
    case Op::greaterThan:
        left = left > right;
        break;
    case Op::greaterOrEqual:
        left = left >= right;
        break;
    case Op::lessOrEqual:
        left = left <= right;
        break;
    case Op::add:
        left = static_cast<std::int32_t>(static_cast<std::uint32_t>(left)
                                         + static_cast<std::uint32_t>(right));
        break;
    case Op::lessThan:
        left = left < right;
        break;
    default:
        return refuse(context, "condition_opcode");
    }
    return true;
}

bool condition(definitions::View data,
               definitions::Range expression,
               const Context& context,
               bool& result) noexcept {
    result = expression.count == 0;
    if (!definitions::fits(expression, data.instructions)) {
        return refuse(context, "condition_shape");
    }
    std::array<std::int32_t, kExpressionCapacity> stack{};
    std::size_t size = 0;
    for (const auto& instruction : data.instructions.subspan(expression.first, expression.count)) {
        const auto operand = instruction.operand;
        std::int32_t value = 0;
        using Read = definitions::BankRead;
        using Op = middleware::content::packages::tables::UnlockOpcode;
        switch (instruction.opcode) {
        case static_cast<std::uint32_t>(Read::accountFlag):
            if (operand >= context.unlocks.accountFlags.size()) {
                return refuse(context, "condition_shape");
            }
            value = context.unlocks.accountFlags[operand] == unlocks::kFlagSet;
            break;
        case static_cast<std::uint32_t>(Read::profileFlag):
            if (operand >= context.unlocks.profileFlags.size()) {
                return refuse(context, "condition_shape");
            }
            value = context.unlocks.profileFlags[operand] == unlocks::kFlagSet;
            break;
        case static_cast<std::uint32_t>(Read::characterFlag):
            if (operand >= context.unlocks.characterObjectFlags.size()) {
                return refuse(context, "condition_shape");
            }
            value = context.unlocks.characterObjectFlags[operand] == unlocks::kFlagSet;
            break;
        case static_cast<std::uint32_t>(Read::accountValue):
            if (operand >= context.unlocks.objectiveValues.size()) {
                return refuse(context, "condition_shape");
            }
            value = context.unlocks.objectiveValues[operand];
            break;
        case static_cast<std::uint32_t>(Read::characterValue):
            if (operand >= context.unlocks.characterObjectValues.size()) {
                return refuse(context, "condition_shape");
            }
            value = context.unlocks.characterObjectValues[operand];
            break;
        case static_cast<std::uint32_t>(Read::characterClass):
            value = operand == static_cast<std::uint32_t>(context.characterClass);
            break;
        case static_cast<std::uint32_t>(Op::constant):
            value = static_cast<std::int32_t>(operand);
            break;
        case static_cast<std::uint32_t>(Read::externalFlag):
            return refuse(context, "external_flag");
        case static_cast<std::uint32_t>(Read::externalValue):
            return refuse(context, "external_value");
        default:
            if (!apply_condition_operator(instruction, stack, size, context)) {
                return false;
            }
            continue;
        }
        if (size == stack.size()) {
            return refuse(context, "condition_shape");
        }
        stack[size++] = value;
    }
    if (expression.count != 0) {
        if (size != 1) {
            return refuse(context, "condition_shape");
        }
        result = stack[0] != 0;
    }
    return true;
}

struct Resolver {
    definitions::View data;
    const Context& context;
    Result& result;
    std::uint64_t random;
    Selection selection;

    double fraction() noexcept {
        // SplitMix64 makes a prepared seed replayable without shared random state.
        random += 0x9E3779B97F4A7C15ULL;
        auto bits = random;
        bits = (bits ^ (bits >> 30)) * 0xBF58476D1CE4E5B9ULL;
        bits = (bits ^ (bits >> 27)) * 0x94D049BB133111EBULL;
        bits ^= bits >> 31;
        return static_cast<double>(bits >> 11) * 0x1.0p-53;
    }

    bool weight(const definitions::Entry& entry,
                std::uint32_t category,
                std::uint32_t bucket,
                std::size_t depth,
                double& output) noexcept {
        output = 0;
        if (entry.categoryHash != category
            || (bucket != kEmptyTag && entry.bucketHash != kEmptyTag
                && entry.bucketHash != bucket)) {
            return true;
        }
        if (selection == Selection::equipment && entry.poolIndex == definitions::kAbsent) {
            build_data::items::details::Definition item{};
            if (!build_data::find_configured_item_detail(entry.itemIndex, item)
                || !item.equipmentSlot.has_value()) {
                return true;
            }
        }
        bool enabled = false;
        if (!condition(data, entry.condition, context, enabled)) {
            return false;
        }
        if (!enabled) {
            return true;
        }
        double value = entry.weight;
        if (!definitions::fits(entry.modifiers, data.modifiers)) {
            return false;
        }
        for (const auto& modifier :
             data.modifiers.subspan(entry.modifiers.first, entry.modifiers.count)) {
            if (!condition(data, modifier.condition, context, enabled)) {
                return false;
            }
            if (enabled) {
                if (modifier.valueIndex != definitions::kAbsent) {
                    return refuse(context, "indexed_weight");
                }
                value = modifier.value;
            }
        }
        if (!std::isfinite(value) || value < 0) {
            return false;
        }
        if (value == 0) {
            return true;
        }
        if (entry.poolIndex != definitions::kAbsent) {
            double total = 0;
            if (!pool_weight(entry.poolIndex,
                             category,
                             entry.bucketHash == kEmptyTag ? bucket : entry.bucketHash,
                             depth + 1,
                             total)) {
                return false;
            }
            if (total == 0) {
                return true;
            }
        } else if (entry.itemIndex != definitions::kAbsent) {
            if (entry.itemIndex >= data.items.size()) {
                return false;
            }
            for (std::size_t i = 0; i < result.count; ++i) {
                if (result.grants[i].itemIndex == entry.itemIndex) {
                    return true;
                }
            }
        } else {
            return refuse(context, "reward_mapping");
        }
        output = value;
        return true;
    }

    bool pool_weight(std::uint16_t index,
                     std::uint32_t category,
                     std::uint32_t bucket,
                     std::size_t depth,
                     double& total) noexcept {
        total = 0;
        if (depth >= definitions::kTraversalDepth || index >= data.pools.size()) {
            return false;
        }
        const auto range = data.pools[index].entries;
        if (!definitions::fits(range, data.entries)) {
            return false;
        }
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, bucket, depth, value)) {
                return false;
            }
            total += value;
        }
        return std::isfinite(total);
    }

    bool draw(std::uint16_t index,
              std::uint32_t category,
              std::uint32_t bucket,
              std::size_t depth,
              double total) noexcept {
        const auto range = data.pools[index].entries;
        double remaining = fraction() * total;
        const definitions::Entry* chosen = nullptr;
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, bucket, depth, value)) {
                return false;
            }
            if (value == 0) {
                continue;
            }
            chosen = &entry;
            remaining -= value;
            if (remaining < 0) {
                break;
            }
        }
        if (chosen == nullptr
            || (chosen->quantity != 1 && chosen->poolIndex != definitions::kAbsent)) {
            return refuse(context, "nested_quantity");
        }
        if (chosen->poolIndex != definitions::kAbsent) {
            const auto childBucket = chosen->bucketHash == kEmptyTag ? bucket : chosen->bucketHash;
            double childTotal = 0;
            return pool_weight(chosen->poolIndex, category, childBucket, depth + 1, childTotal)
                   && childTotal > 0
                   && draw(chosen->poolIndex, category, childBucket, depth + 1, childTotal);
        }
        if (chosen->quantity == 0 || chosen->quantity > INT32_MAX
            || result.count == result.grants.size()
            || !definitions::fits(chosen->sockets, data.sockets)) {
            return false;
        }
        auto& grant = result.grants[result.count++];
        grant.itemIndex = chosen->itemIndex;
        grant.quantity = static_cast<std::int32_t>(chosen->quantity);
        if (chosen->sockets.count > grant.sockets.size()) {
            return false;
        }
        grant.socketCount = chosen->sockets.count;
        std::copy_n(
            data.sockets.begin() + chosen->sockets.first, grant.socketCount, grant.sockets.begin());
        return true;
    }
};

bool resolve_item(definitions::View data,
                  const Context& context,
                  std::uint16_t itemIndex,
                  std::uint32_t quantity,
                  Selection selection,
                  Result& result) noexcept {
    result = {};
    if (context.refusal != nullptr) {
        *context.refusal = "reward_shape";
    }
    if (itemIndex >= data.items.size() || quantity == 0 || quantity > INT32_MAX) {
        return false;
    }
    const auto& item = data.items[itemIndex];
    if (item.definitionHash == 0) {
        return refuse(context, "item_unavailable");
    }
    Result staged{};
    // Stored engrams retain their wrapper until an opening transaction.
    if (item.poolIndex == definitions::kAbsent
        || (item.flags & definitions::kOpenOnAcquisition) == 0) {
        staged.grants[0].itemIndex = itemIndex;
        staged.grants[0].quantity = static_cast<std::int32_t>(quantity);
        staged.count = 1;
    } else {
        if (quantity != 1 || item.selectionCount == 0
            || item.selectionCount > item.selections.size()) {
            return false;
        }
        Resolver resolver{data, context, staged, context.seed, selection};
        for (std::size_t i = 0; i < item.selectionCount; ++i) {
            const auto& declared = item.selections[i];
            if (declared.count > staged.grants.size()) {
                return false;
            }
            for (std::size_t j = 0; j < declared.count; ++j) {
                double total = 0;
                if (!resolver.pool_weight(
                        item.poolIndex, declared.categoryHash, kEmptyTag, 0, total)) {
                    return false;
                }
                // A fixed bundle can have fewer eligible members after an acquisition unlock.
                if (total == 0) {
                    break;
                }
                if (!resolver.draw(item.poolIndex, declared.categoryHash, kEmptyTag, 0, total)) {
                    return false;
                }
            }
        }
        if (staged.count == 0) {
            return false;
        }
    }
    result = staged;
    if (context.refusal != nullptr) {
        *context.refusal = nullptr;
    }
    return true;
}

} // namespace

bool eligible(std::span<const definitions::Instruction> instructions,
              const Context& context,
              bool& result) noexcept {
    definitions::View view{};
    view.instructions = instructions;
    if (instructions.size() > UINT32_MAX) {
        return false;
    }
    return condition(view, {0, static_cast<std::uint32_t>(instructions.size())}, context, result);
}

bool resolve(const Context& context,
             std::uint16_t itemIndex,
             std::uint32_t quantity,
             Selection selection,
             Result& result) noexcept {
    struct Request {
        const Context& context;
        std::uint16_t item;
        std::uint32_t quantity;
        Selection selection;
        Result& result;
    } request{context, itemIndex, quantity, selection, result};
    result = {};
    if (context.refusal != nullptr) {
        *context.refusal = "reward_item";
    }
    return definitions::read(&request, [](void* raw, definitions::View data) noexcept {
        auto& value = *static_cast<Request*>(raw);
        return resolve_item(
            data, value.context, value.item, value.quantity, value.selection, value.result);
    });
}

} // namespace sunrise::state::rewards
