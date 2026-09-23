#include "season_pass_catalog.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "../../unlocks/definition.h"
#include "../rewards/reward_catalog.h"
#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::season_pass {
namespace {
core::threading::SrwLock g_lock;
Table<Reward, kRewardCapacity> g_rewards;
} // namespace

void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rewards.clear();
}

bool valid(std::span<const Reward> rewards) noexcept {
    return !rewards.empty() && rewards.size() <= kRewardCapacity
           && std::all_of(rewards.begin(), rewards.end(), [](const Reward& reward) {
                  if (reward.itemHash == 0) {
                      return reward.quantity == 0 && reward.conditionCount == 0
                             && reward.socketCount == 0
                             && reward.claimFlagIndex == kUnavailableFlagIndex;
                  }
                  return reward.quantity != 0 && reward.quantity <= INT32_MAX
                         && (reward.claimFlagIndex == kUnavailableFlagIndex
                             || reward.claimFlagIndex < unlocks::kAccountFlagCapacity)
                         && reward.socketCount <= reward.sockets.size()
                         && reward.conditionCount <= reward.condition.size()
                         && std::all_of(reward.condition.begin(),
                                        reward.condition.begin() + reward.conditionCount,
                                        rewards::valid_instruction);
              });
}

bool replace(std::span<const Reward> rewards) noexcept {
    if (!valid(rewards)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_rewards.replace(rewards);
}

bool find(std::uint16_t rewardIndex, Reward& reward) noexcept {
    reward = {};
    const std::shared_lock guard(g_lock);
    if (rewardIndex >= g_rewards.count() || g_rewards.rows()[rewardIndex].itemHash == 0) {
        return false;
    }
    reward = g_rewards.rows()[rewardIndex];
    return true;
}

bool snapshot(std::span<Reward> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.snapshot(output, count);
}

std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.count();
}

} // namespace sunrise::state::build_data::season_pass
