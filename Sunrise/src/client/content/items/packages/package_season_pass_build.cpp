#include <algorithm>
#include <limits>

#include "../../../../middleware/content/packages/tables/internal.h"
#include "../../../../state/build_data/season_pass/season_pass_catalog.h"
#include "../../../../state/progression/season_pass_reward_catalog.h"
#include "core/logging/log.h"
#include "internal.h"
#include "package_reward_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::season_pass;

bool read_season_reward(Storage& storage,
                        std::span<const std::byte> progressionTable,
                        const tables::Array& itemRows,
                        std::size_t at,
                        domain::Reward& reward) noexcept {
    std::uint32_t rank = 0;
    std::uint32_t itemIndex = 0;
    std::uint32_t claimSlot = 0;
    tables::IndexRow entry{};
    if (!tables::read(progressionTable, at + tables::kProgressionRewardRankOffset, rank)
        || !tables::read(
            progressionTable, at + tables::kProgressionRewardItemIndexOffset, itemIndex)
        || !tables::read(
            progressionTable, at + tables::kProgressionRewardQuantityOffset, reward.quantity)
        || !tables::read(
            progressionTable, at + tables::kProgressionRewardClaimSlotOffset, claimSlot)
        || reward.quantity == 0 || reward.quantity > INT32_MAX
        || rank > (std::numeric_limits<std::uint8_t>::max)()
        || itemIndex > (std::numeric_limits<std::uint16_t>::max)()
        || claimSlot > (std::numeric_limits<std::uint16_t>::max)()
        || !tables::index_row(storage.itemIndexTable, itemRows, itemIndex, entry)) {
        return false;
    }
    reward.itemHash = entry.definitionHash;
    reward.itemIndex = static_cast<std::uint16_t>(itemIndex);
    reward.requiredRank = static_cast<std::uint8_t>(rank);
    std::size_t socketCount = 0;
    if (!read_reward_sockets(progressionTable,
                             at + tables::kProgressionRewardSocketsOffset,
                             reward.sockets,
                             socketCount)) {
        return false;
    }
    reward.socketCount = static_cast<std::uint8_t>(socketCount);
    std::size_t conditionCount = 0;
    if (!storage.rewardBuild.conditions.read_list(progressionTable,
                                                  at + tables::kProgressionRewardConditionsOffset,
                                                  reward.condition,
                                                  conditionCount)) {
        return false;
    }
    reward.conditionCount = static_cast<std::uint8_t>(conditionCount);
    // A reward with no claim flag carries slot 0.
    if (claimSlot != 0) {
        reward.claimFlagIndex =
            bank_index(storage.slotMaps.accountFlag, static_cast<std::int32_t>(claimSlot));
    }

    if (!domain::valid(std::span(&reward, 1))
        || (claimSlot != 0 && reward.claimFlagIndex == domain::kUnavailableFlagIndex)
        || !std::all_of(reward.sockets.begin(),
                        reward.sockets.begin() + reward.socketCount,
                        [&](const auto& socket) {
                            return socket.socketType != state::build_data::rewards::kAbsent
                                   && (socket.plugItem == state::build_data::rewards::kAbsent
                                       || socket.plugItem < itemRows.count);
                        })) {
        return false;
    }
    return true;
}

} // namespace

/** Preserves native reward order for opcode-2400 claim indices. */
bool build_season_pass(const reader::Source& source,
                       Storage& storage,
                       std::span<const std::byte> root) noexcept {
    storage.seasonPassRewardCount = 0;

    std::uint32_t itemTableTag = 0;
    tables::Array itemRows{};
    if (!tables::slot_tag(root, tables::kItemTableSlot, itemTableTag) || itemTableTag == 0
        || !reader::read_tag(source, storage.scratch, itemTableTag, storage.itemIndexTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.itemIndexTable},
                                  tables::kTableArrayDescriptor,
                                  itemRows)
        || itemRows.elementClass != tables::kItemIndexTableClass) {
        return false;
    }

    std::uint32_t tableTag = 0;
    tables::Array progressions{};
    if (!tables::slot_tag(root, tables::kProgressionTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.progressionTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.progressionTable},
                                  tables::kTableArrayDescriptor,
                                  progressions)
        || progressions.elementClass != tables::kProgressionTableClass
        || progressions.count <= state::progression::season_pass::kProgressionDefinitionIndex) {
        return false;
    }
    const std::span<const std::byte> progressionTable{storage.progressionTable};
    const std::size_t passAt = progressions.dataOffset
                               + state::progression::season_pass::kProgressionDefinitionIndex
                                     * tables::kProgressionRowStride;
    tables::Array rewards{};
    if (!tables::read_array(progressionTable,
                            passAt + tables::kProgressionRewardField,
                            tables::kProgressionRewardRowClass,
                            tables::kProgressionRewardStride,
                            rewards)
        || rewards.count == 0 || rewards.count > domain::kRewardCapacity) {
        return false;
    }

    std::size_t skipped = 0;
    for (std::uint64_t row = 0; row < rewards.count; ++row) {
        const std::size_t at =
            rewards.dataOffset + static_cast<std::size_t>(row) * tables::kProgressionRewardStride;
        auto& reward = storage.seasonPassRewards[row];
        reward = {};
        if (!read_season_reward(storage, progressionTable, itemRows, at, reward)) {
            reward = {};
            ++skipped;
        }
    }
    if (skipped != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=season_pass skipped=%zu",
                          skipped);
    }
    storage.seasonPassRewardCount = static_cast<std::size_t>(rewards.count);
    return storage.seasonPassRewardCount != 0;
}

} // namespace sunrise::client::content::items::packages
