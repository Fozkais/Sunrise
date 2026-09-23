#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "season_pass_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the season pass reward list is published. */
bool season_pass_ready() noexcept {
    return season_pass::count() != 0;
}

/** Publishes the season pass reward list with its fixed socket overrides. */
bool publish_season_pass(std::span<const season_pass::Reward> rewards) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(season_pass::replace(rewards), season_pass::clear);
}

/** Reads one season pass reward row. */
bool find_season_pass_reward(std::uint16_t rewardIndex, season_pass::Reward& reward) noexcept {
    return season_pass::find(rewardIndex, reward);
}

} // namespace sunrise::state::build_data
