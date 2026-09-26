#include <algorithm>
#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/squad_objective_auth.h"
#include "activity_sdk_device_internal.h"

namespace sunrise::server::activity::activity_sdk_devices {

/** Resolves both SDK slots and the authored group before queuing an objective decision. */
Status
assign_combat_objective_reserved(const state::activity_sdk::BoundView& view,
                                 const state::activity::mission::TypedIntent& decision,
                                 const host::ScriptableOutputReservation& reservation) noexcept {
    namespace format = state::activity_sdk::format;
    namespace objective = middleware::bap::activity_message::squad_objective;
    if (view.catalog == nullptr
        || decision.kind != state::activity::mission::IntentKind::assignCombatObjective) {
        return Status::invalidView;
    }
    const auto slots = view.catalog->slots();
    if (decision.firstRow >= slots.size() || decision.secondRow >= slots.size()) {
        return Status::invalidSlot;
    }
    const auto& squad = slots[decision.firstRow];
    const auto& target = slots[decision.secondRow];
    if (squad.slotType != format::kSquadSlotType
        || squad.componentClass != format::kSquadComponentClass
        || squad.authSchema != objective::kSchema || target.slotType != format::kObjectiveSlotType
        || target.componentClass != format::kObjectiveComponentClass
        || target.authSchema != middleware::bap::activity_message::scriptable_auth::kType3Schema
        || squad.objectIndex != target.objectIndex
        || (squad.flags & format::kSlotSchemaJoinExact) == 0
        || (target.flags & format::kSlotSchemaJoinExact) == 0
        || decision.entryIndex < objective::kNoTaskGroup
        || decision.entryIndex >= objective::kTaskGroupCount) {
        return Status::invalidSlot;
    }
    const auto groups = view.catalog->combat_objective_groups();
    if (decision.entryIndex != objective::kNoTaskGroup
        && !std::any_of(groups.begin(), groups.end(), [&](const auto& group) {
               return group.slotIndex == decision.secondRow
                      && group.groupIndex == static_cast<std::uint32_t>(decision.entryIndex);
           })) {
        return Status::invalidValue;
    }
    detail::PreparedDevice prepared{};
    Status status = detail::prepare_slot(view, decision.firstRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    // The objective composes onto the squad's placement, so it goes to the link that placed it.
    state::activity_sdk::BoundView routed = view;
    const server::bap::SquadRoute route = server::bap::squad_route(
        view.binding,
        view.activityClientGeneration,
        prepared.target.registryKey,
        prepared.target.slotIndex,
        -1);
    if (route.generation != view.activityClientGeneration) {
        routed.activityClientGeneration = route.generation;
        detail::PreparedDevice candidate{};
        status = detail::prepare_slot(routed, decision.firstRow, candidate);
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=activity stage=squad_route result=%s "
                                          "reason=objective slot=%u owner_gen=%llu gen=%llu",
                                          status == Status::ready ? "routed" : "fallback",
                                          static_cast<unsigned>(decision.firstRow),
                                          static_cast<unsigned long long>(
                                              view.activityClientGeneration),
                                          static_cast<unsigned long long>(route.generation));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        if (status == Status::ready) {
            prepared = candidate;
        } else {
            routed = view;
        }
    }
    detail::PreparedDevice objectiveSlot{};
    const Status objectiveStatus =
        detail::prepare_slot(routed, decision.secondRow, objectiveSlot);
    if (objectiveStatus != Status::ready
        || prepared.activityClientGeneration != objectiveSlot.activityClientGeneration
        || prepared.target.registryKey != objectiveSlot.target.registryKey) {
        return objectiveStatus == Status::ready ? Status::targetUnavailable : objectiveStatus;
    }
    return host::request_squad_objective(
               view.binding,
               prepared.target,
               prepared.target.stateLocalRoster ? &prepared.generatedRosterGroup : nullptr,
               decision,
               objectiveSlot.target.slotIndex,
               prepared.activityClientGeneration,
               reservation)
               ? Status::queued
               : Status::refused;
}

} // namespace sunrise::server::activity::activity_sdk_devices
