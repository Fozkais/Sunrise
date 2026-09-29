/**
 * The mission-program instance table, the durable state commit, the timers and the service slice.
 * The service slice and every lifecycle entry point take the mission runtime lock.
 */

#include "mission_script_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/mission/activity_section.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../bap/runtime.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../host_runtime.h"
#include "mission_script_event_batch.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_sdk_bridge.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {

std::array<RuntimeInstance, host::kInstanceCapacity> g_instances{};

namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
bool g_enabled{};
bool g_pathReady{};
/** The one developer command waiting for an idle program; it outlives a reload. */
lua_vm::DevCommand g_devCommand{};
bool g_devCommandPending{};

static_assert(mission_state::kSquadMemberCapacity == lua_vm::kSquadMemberCapacity);

/** Copies the complete VM outbox into typed State values in delivery order. */
[[nodiscard]] bool
snapshot_state_intents(const lua_vm::Vm& vm,
                       std::vector<mission_state::TypedIntent>& output) noexcept {
    return lua_vm::snapshot_intents(vm, output);
}

[[nodiscard]] bool earlier_timer(const lua_vm::MissionTimer& left,
                                 const lua_vm::MissionTimer& right) noexcept {
    return left.deadlineTick < right.deadlineTick
           || (left.deadlineTick == right.deadlineTick && left.sequence < right.sequence);
}

/** @return The life of the instance's own player, from its retained participation levels. */
[[nodiscard]] PlayerLife own_player_life(const RuntimeInstance& instance) noexcept {
    if (!instance.occupied
        || instance.playerLifeGeneration != instance.view.activityClientGeneration) {
        return PlayerLife::unknown;
    }
    for (const PlayerLifeObservation& life : instance.playerLife) {
        if (instance.playerKey != 0 && life.playerKey == instance.playerKey) {
            return life.life();
        }
    }
    return PlayerLife::unknown;
}

/** @return The life of one member's own player, from the levels its client reported. */
[[nodiscard]] PlayerLife member_player_life(const MemberLifeWatch& member) noexcept {
    for (const PlayerLifeObservation& life : member.playerLife) {
        if (member.playerKey != 0 && life.playerKey == member.playerKey) {
            return life.life();
        }
    }
    return PlayerLife::unknown;
}

} // namespace

/**
 * Writes one bounded mission-script diagnostic line.
 * @param instance Binding the line reports, or null before one is bound.
 * @param fields Extra key=value pairs, appended as written.
 * @param error Free text; quoted and placed last so it never splits the pairs.
 */
void log_line(core::log::Level level,
              const RuntimeInstance* instance,
              std::string_view stage,
              std::string_view result,
              std::string_view fields,
              std::string_view error) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const unsigned long long session = instance == nullptr ? 0 : instance->view.binding.sessionId;
    const unsigned activityRow = instance == nullptr ? 0 : instance->identity.activityRow;
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=mission_script stage=%.*s result=%.*s session=%llu "
                                      "activity_row=%u",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      static_cast<int>(result.size()),
                                      result.data(),
                                      session,
                                      activityRow);
    if (written <= 0) {
        return;
    }
    std::size_t length = (std::min)(static_cast<std::size_t>(written), line.size() - 1);
    if (!fields.empty()) {
        const int piece = std::snprintf(line.data() + length,
                                        line.size() - length,
                                        " %.*s",
                                        static_cast<int>(fields.size()),
                                        fields.data());
        if (piece > 0) {
            length = (std::min)(length + static_cast<std::size_t>(piece), line.size() - 1);
        }
    }
    if (!error.empty()) {
        const int piece = std::snprintf(line.data() + length,
                                        line.size() - length,
                                        " error=\"%.*s\"",
                                        static_cast<int>(error.size()),
                                        error.data());
        if (piece > 0) {
            length = (std::min)(length + static_cast<std::size_t>(piece), line.size() - 1);
        }
    }
    core::log::write(core::log::Channel::server, level, {line.data(), length});
}

/**
 * Raises a fireteam event on each private instance whose party life counts changed. The party
 * is the committed destination peers; a missing or loading member counts as unknown.
 */
void publish_fireteam_life(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.publicTarget || !instance.sessionRosterObserved
            || instance.programStatus != ProgramStatus::loaded) {
            continue;
        }
        FireteamLife counts{};
        counts.add(own_player_life(instance));
        // Each other member of this same activity reports its own player; a member that left
        // frees its entry and no longer counts.
        for (MemberLifeWatch& member : instance.memberLife) {
            if (member.generation == 0) {
                continue;
            }
            server::bap::ActivityLinkView link{};
            if (!server::bap::activity_link_view(instance.view.binding, member.generation, link)) {
                member = {};
                continue;
            }
            counts.add(member_player_life(member));
        }
        for (const SessionRosterWatch& peer : instance.sessionRoster) {
            if (!peer.used) {
                continue;
            }
            PlayerLife life = PlayerLife::unknown;
            for (const RuntimeInstance& candidate : g_instances) {
                if (candidate.occupied && !candidate.publicTarget
                    && candidate.view.binding.sessionId == peer.sessionId
                    && candidate.view.binding.createdRevision == peer.createdRevision) {
                    life = own_player_life(candidate);
                    break;
                }
            }
            counts.add(life);
        }
        if (instance.fireteamLifePublished && counts == instance.lastFireteamLife) {
            continue;
        }
        instance.lastFireteamLife = counts;
        instance.fireteamLifePublished = true;
        host::Event event{};
        event.kind = host::EventKind::fireteamState;
        event.binding = instance.view.binding;
        event.sequence = instance.missionStateRevision;
        event.sourceGeneration = instance.view.activityClientGeneration;
        event.missionSequence = instance.lastMissionSequence;
        event.tick = now;
        event.fireteamAlive = counts.alive;
        event.fireteamDead = counts.dead;
        event.fireteamUnknown = counts.unknown;
        push_script_event(instance, event);
        std::array<char, 96> fields{};
        const int length = std::snprintf(fields.data(),
                                         fields.size(),
                                         "alive=%u dead=%u unknown=%u",
                                         static_cast<unsigned>(counts.alive),
                                         static_cast<unsigned>(counts.dead),
                                         static_cast<unsigned>(counts.unknown));
        if (length > 0) {
            log_line(core::log::Level::debug,
                     &instance,
                     "fireteam_life",
                     "changed",
                     {fields.data(), static_cast<std::size_t>(length)});
        }
    }
}

namespace {

/**
 * Merges the type-13 participation records of one Sense snapshot into one player-slot table.
 * @param playerKey The player key the reporting link's message 5 binds.
 */
void merge_participation(std::array<PlayerLifeObservation, kParticipationSlotCount>& table,
                         std::uint64_t playerKey,
                         const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != kParticipationSlotType
            || observation.key.senseSchema != kParticipationSenseSchema
            || observation.key.objectTag != kParticipationObjectTag
            || observation.key.slotIndex < kFirstParticipationSlot
            || observation.key.slotIndex >= kFirstParticipationSlot + kParticipationSlotCount
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue) {
            continue;
        }
        PlayerLifeObservation& level = table[observation.key.slotIndex - kFirstParticipationSlot];
        update_player_life(
            level, std::span(sense.values).subspan(observation.firstValue, observation.valueCount));
        // The host writes the link's own player key on every participation record of that link,
        // so a record that reports state is that player's.
        if ((level.seen & kLifeSeenKey) == 0 && (level.seen & kLifeSeenState) != 0
            && playerKey != 0) {
            level.playerKey = playerKey;
            level.seen |= kLifeSeenKey;
        }
    }
}

} // namespace

/** Merges the type-13 participation records of one Sense snapshot into the instance. */
void observe_player_life(RuntimeInstance& instance,
                         const host::SenseObservationSnapshot& sense) noexcept {
    if (instance.playerLifeGeneration != sense.sourceGeneration) {
        instance.playerLife = {};
        instance.playerLifeGeneration = sense.sourceGeneration;
    }
    merge_participation(instance.playerLife, instance.playerKey, sense);
}

/** Merges another member's participation records into that member's entry. */
void observe_member_life(RuntimeInstance& instance,
                         const host::SenseObservationSnapshot& sense) noexcept {
    server::bap::ActivityLinkView link{};
    if (sense.sourceGeneration == 0
        || sense.sourceGeneration == instance.view.activityClientGeneration
        || !server::bap::activity_link_view(instance.view.binding, sense.sourceGeneration, link)) {
        return;
    }
    MemberLifeWatch* entry = nullptr;
    MemberLifeWatch* spare = nullptr;
    for (MemberLifeWatch& candidate : instance.memberLife) {
        if (candidate.generation == sense.sourceGeneration) {
            entry = &candidate;
            break;
        }
        if (spare == nullptr && candidate.generation == 0) {
            spare = &candidate;
        }
    }
    if (entry == nullptr) {
        if (spare == nullptr) {
            log_line(core::log::Level::warn, &instance, "fireteam_life", "member_capacity");
            return;
        }
        entry = spare;
        *entry = {};
        entry->generation = sense.sourceGeneration;
    }
    entry->playerKey = link.playerKey;
    merge_participation(entry->playerLife, entry->playerKey, sense);
}

/**
 * Appends one host-state event for the script.
 * Host-state bursts are intentionally dynamic: authored Sense updates can raise more events than
 * any fixed bound without making the events invalid.
 */
void push_script_event(RuntimeInstance& instance, const host::Event& event) noexcept {
    if (instance.programStatus != ProgramStatus::loaded
        || !lua_vm::handles_event(instance.vm, event.kind)) {
        return;
    }
    try {
        host::Event admitted = event;
        if (admitted.attemptGeneration == 0) {
            admitted.attemptGeneration = instance.dispatchAttemptGeneration != 0
                                             ? instance.dispatchAttemptGeneration
                                             : instance.attempt.generation;
        }
        instance.scriptEvents.push_back(admitted);
    } catch (const std::bad_alloc&) {
        fault_instance(instance, "mission event allocation failed");
    }
}

void push_program_event(RuntimeInstance& instance, const host::Event& event) noexcept {
    if (instance.programStatus != ProgramStatus::loaded) {
        return;
    }
    try {
        instance.scriptEvents.push_back(event);
    } catch (const std::bad_alloc&) {
        log_line(core::log::Level::warn, &instance, "script_event", "allocation_failure");
    }
}

bool describe_dialogue_cue(const RuntimeInstance& instance,
                           std::uint32_t slotRow,
                           std::uint16_t cue,
                           host::Event& event) noexcept {
    event.dialogueSlotRow = slotRow;
    event.dialogueCue = cue;
    if (instance.view.catalog == nullptr) {
        return false;
    }
    const sdk::Catalog& catalog = *instance.view.catalog;
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    if (slotRow >= slots.size()) {
        return false;
    }
    const format::Slot& slot = slots[slotRow];
    if (slot.slotType != format::kDialogueSlotType || slot.objectIndex >= objects.size()
        || slot.slotIndex
               > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
        return false;
    }
    const auto cues = sdk::slot_dialogue_cues(catalog, slot);
    if (cue >= cues.size()) {
        return false;
    }
    event.dialogueRegistryKey = objects[slot.objectIndex].objectKey;
    event.dialogueSlotType = static_cast<std::int8_t>(slot.slotType);
    event.dialogueSlotIndex = static_cast<std::int16_t>(slot.slotIndex);
    // The client reports no line end; the authored window is the only length a cue carries.
    event.dialogueDurationMs = sdk::authored_milliseconds(cues[cue].authoredWindowSeconds);
    return true;
}

/**
 * Queues an exact host-policy transition without consuming a client mission-input sequence.
 * @param binding Activity generation that accepted the damage.
 * @param sourceGeneration ActivityClient generation that owns the policy.
 * @param registryKey Authored squad registry key.
 * @param slotIndex Native squad slot index.
 */
void report_squad_provoked(const state::activity::SessionBinding& binding,
                           std::uint64_t sourceGeneration,
                           std::uint32_t registryKey,
                           std::uint16_t slotIndex) noexcept {
    if (sourceGeneration == 0 || registryKey == 0
        || slotIndex > static_cast<std::uint16_t>((std::numeric_limits<std::int16_t>::max)())) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    auto* instance = find_instance(binding);
    if (instance != nullptr && instance->view.activityClientGeneration == sourceGeneration) {
        host::Event event{};
        event.kind = host::EventKind::squadProvoked;
        event.binding = binding;
        event.sequence = instance->missionStateRevision;
        event.missionSequence = instance->lastMissionSequence;
        event.sourceGeneration = sourceGeneration;
        event.tick = GetTickCount64();
        event.firstRegistryKey = registryKey;
        event.firstSlotType = sdk::format::kSquadSlotType;
        event.firstSlotIndex = slotIndex;
        push_script_event(*instance, event);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Retains the last VM stage and status shown on the panel. */
void note_vm_status(RuntimeInstance& instance,
                    std::string_view stage,
                    std::string_view status) noexcept {
    copy_text(instance.lastVmStage, stage);
    copy_text(instance.lastVmStatus, status);
}

/** Frees one slot; its queued events are retired unless the caller keeps them for a reattach. */
void clear_instance(RuntimeInstance& instance, bool clearPending) noexcept {
    if (instance.occupied && clearPending) {
        server::gameplay::squad_entity_retirement::cancel_placed_transition(
            instance.view.binding, instance.view.activityClientGeneration);
        clear_pending_events(instance.view.binding);
        // A mission's section ends with it; a reload or a reattach keeps it.
        ::sunrise::state::activity::mission::activity_section::clear(
            instance.view.binding.sessionId);
    } else if (instance.occupied) {
        reset_pending_events_for_reattach(instance.view.binding);
    }
    lua_vm::close(instance.vm);
    instance.worldView = {};
    instance.view = {};
    instance.identity = {};
    instance.programKey = {};
    instance.lastVmStage = {};
    instance.lastVmStatus = {};
    instance.attempt = {};
    instance.dispatchAttemptGeneration = 0;
    instance.dispatchInputSequence = 0;
    instance.eventsSeen = 0;
    instance.eventsCommitted = 0;
    instance.intentsTransportStaged = 0;
    instance.lastEventSequence = 0;
    instance.lastLoggedRevision = 0;
    instance.variablesLogged = 0;
    instance.lastMissionSequence = 0;
    instance.missionPhase = 0;
    instance.missionStateRevision = 0;
    instance.activityStateRevision = 0;
    instance.durableIntentSequence = 0;
    instance.durableHostOutputRevision = 0;
    instance.expectedScriptableRevision = 0;
    instance.deliveryDeadline = 0;
    instance.firstIntentAttempt = 0;
    instance.nextIntentAttempt = 0;
    instance.firstStartAttempt = 0;
    instance.nextStartAttempt = 0;
    instance.pendingTimerEvent = {};
    instance.firstTimerAttempt = 0;
    instance.nextTimerAttempt = 0;
    instance.intentAttempts = 0;
    instance.startAttempts = 0;
    instance.timerAttempts = 0;
    instance.durablePendingIntentCount = 0;
    instance.lastIntentStatus = (std::numeric_limits<std::uint16_t>::max)();
    instance.initialStateRegion = -1;
    instance.activeRegion = -1;
    instance.partyRegions = {};
    instance.partyRegionsAttempt = 0;
    instance.programStatus = ProgramStatus::none;
    instance.deliveryStage = DeliveryStage::idle;
    instance.publicTarget = false;
    instance.playerKey = 0;
    instance.missionStateBound = false;
    instance.missionStarted = false;
    instance.missionStateFaulted = false;
    instance.initialStateDeclared = false;
    instance.initialStateSelected = false;
    instance.missionReattached = false;
    instance.startPending = false;
    instance.timerPending = false;
    instance.triggerOccupancy = {};
    instance.ghostObservations = {};
    instance.actorPathObservations = {};
    instance.squadObservations = {};
    instance.damageObservations = {};
    instance.combatantDamageObservations = {};
    instance.deviceObservations = {};
    instance.objectInteractionObservations = {};
    instance.sceneObservations = {};
    instance.objectiveObservations = {};
    instance.sessionRoster = {};
    instance.sessionRosterObserved = false;
    instance.playerLife = {};
    instance.playerLifeGeneration = 0;
    instance.memberLife = {};
    instance.lastFireteamLife = {};
    instance.fireteamLifePublished = false;
    std::vector<host::Event>{}.swap(instance.scriptEvents);
    instance.firstScriptEventAttempt = 0;
    instance.nextScriptEventAttempt = 0;
    instance.scriptEventAttempts = 0;
    instance.scriptEventRead = 0;
    instance.occupied = false;
}

/** @return The open slot for one exact binding, or null. */
RuntimeInstance* find_instance(const state::activity::SessionBinding& binding) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (instance.occupied && same_binding(instance.view.binding, binding)) {
            return &instance;
        }
    }
    return nullptr;
}

/** @return One unused slot, or null when every slot is open. */
RuntimeInstance* free_instance() noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied) {
            return &instance;
        }
    }
    return nullptr;
}

/** Copies one committed authoritative snapshot into the runtime's exact compare baseline. */
void accept_mission_state(RuntimeInstance& instance,
                          const mission_state::Snapshot& snapshot) noexcept {
    if (instance.attempt.generation != snapshot.state.attempt.generation) {
        instance.triggerOccupancy = {};
        instance.squadObservations = {};
        instance.ghostObservations = {};
        instance.damageObservations = {};
        instance.combatantDamageObservations = {};
        instance.deviceObservations = {};
        instance.objectInteractionObservations = {};
        instance.actorPathObservations = {};
        instance.sceneObservations = {};
        instance.objectiveObservations = {};
        // Player life is kept: the client sends only changed fields and never resends the region.
        instance.fireteamLifePublished = false;
        instance.timerPending = false;
        instance.pendingTimerEvent = {};
    }
    instance.attempt = snapshot.state.attempt;
    lua_vm::publish_attempt(instance.vm, instance.attempt);
    if (!lua_vm::publish_device_requests(
            instance.vm, snapshot.state.deviceRequests, instance.view.activityClientGeneration)) {
        lua_vm::fault(instance.vm, "device request snapshot allocation failed");
        instance.programStatus = ProgramStatus::programError;
    }
    publish_ghost_levels(instance);
    if (!lua_vm::publish_populations(instance.vm, snapshot.state.squadPopulations)) {
        lua_vm::fault(instance.vm, "population snapshot allocation failed");
        instance.programStatus = ProgramStatus::programError;
    }
    instance.missionStateRevision = snapshot.state.revision;
    instance.lastMissionSequence = snapshot.state.inputSequence;
    instance.activityStateRevision = snapshot.activityStateRevision;
    instance.missionPhase = snapshot.state.phase;
    instance.missionStarted = snapshot.state.started;
    instance.missionStateFaulted = snapshot.state.faulted;
    instance.durablePendingIntentCount = snapshot.state.pendingIntents.size();
    instance.durableIntentSequence = snapshot.state.pendingIntents.empty()
                                         ? mission_state::kAbsentIntentSequence
                                         : snapshot.state.pendingIntents.front().sequence;
    instance.durableHostOutputRevision =
        snapshot.state.pendingIntents.empty()
            ? mission_state::kAbsentHostOutputRevision
            : snapshot.state.pendingIntents.front().hostOutputRevision;
}

/** Marks the already-faulted VM in durable State when its exact compare still matches. */
void persist_mission_fault(RuntimeInstance& instance) noexcept {
    if (!instance.missionStateBound) {
        return;
    }
    mission_state::Snapshot snapshot{};
    const mission_state::Status status = mission_state::fault(
        instance.view.binding, instance.programKey, instance.missionStateRevision, snapshot);
    if (status == mission_state::Status::ready) {
        accept_mission_state(instance, snapshot);
        return;
    }
    log_line(core::log::Level::warn, &instance, "state_fault", mission_state::status_name(status));
}

/** Faults both the VM and the exact server-owned mission record. */
void fault_instance(RuntimeInstance& instance, std::string_view reason) noexcept {
    server::gameplay::squad_entity_retirement::cancel_placed_transition(
        instance.view.binding, instance.view.activityClientGeneration);
    lua_vm::fault(instance.vm, reason);
    instance.programStatus = ProgramStatus::programError;
    persist_mission_fault(instance);
}

namespace {

/** Variable counts whose first crossing is logged; the last leaves six rows of headroom. */
constexpr std::size_t kVariableCap = mission_state::kVariableCapacity;
constexpr std::array<std::size_t, 7> kVariableBands{kVariableCap / 2,
                                                    kVariableCap * 3 / 4,
                                                    kVariableCap * 7 / 8,
                                                    kVariableCap * 15 / 16,
                                                    kVariableCap * 31 / 32,
                                                    kVariableCap * 63 / 64,
                                                    kVariableCap - 6};

/** One key family: the leading segment of a variable name, three for a flow's own rows. */
struct VariableFamily final {
    std::string_view name{};
    std::size_t count{};
};

/** @return The family a durable variable name belongs to. */
std::string_view variable_family(const mission_state::StateKey& key) noexcept {
    const std::string_view text(key.bytes.data(), key.length);
    const std::size_t wanted = text.starts_with("flow.") ? 3 : 1;
    std::size_t breaks = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if ((text[index] == '.' || text[index] == '/') && ++breaks == wanted) {
            return text.substr(0, index);
        }
    }
    return text;
}

/**
 * Reports the script variable table as it crosses each band toward its capacity, with the
 * families that fill it, so a mission that fills the table can be trimmed where it is spent.
 */
void note_variable_pressure(RuntimeInstance& instance,
                            std::span<const mission_state::ScriptVariable> variables) noexcept {
    std::size_t band = 0;
    for (const std::size_t threshold : kVariableBands) {
        if (variables.size() >= threshold) {
            band = threshold;
        }
    }
    if (band <= instance.variablesLogged) {
        instance.variablesLogged = band;
        return;
    }
    instance.variablesLogged = band;
    std::array<VariableFamily, 48> families{};
    std::size_t used = 0;
    std::size_t other = 0;
    for (const mission_state::ScriptVariable& variable : variables) {
        const std::string_view name = variable_family(variable.key);
        std::size_t slot = 0;
        while (slot < used && families[slot].name != name) {
            ++slot;
        }
        if (slot == used) {
            if (used == families.size()) {
                ++other;
                continue;
            }
            families[used++].name = name;
        }
        ++families[slot].count;
    }
    std::sort(families.begin(),
              families.begin() + static_cast<std::ptrdiff_t>(used),
              [](const VariableFamily& left, const VariableFamily& right) {
                  return left.count > right.count;
              });
    std::string fields = "variables=" + std::to_string(variables.size()) + " capacity="
                         + std::to_string(mission_state::kVariableCapacity) + " families=";
    for (std::size_t index = 0; index < used && index < 14; ++index) {
        fields += index == 0 ? "" : ",";
        fields += families[index].name;
        fields += '=';
        fields += std::to_string(families[index].count);
    }
    if (other != 0) {
        fields += ",other=" + std::to_string(other);
    }
    log_line(core::log::Level::warn, &instance, "variables", "pressure", fields);
}

} // namespace

/** Commits the VM's phase/revision/start transaction into exact server-owned State. */
bool commit_mission_state(RuntimeInstance& instance,
                          bool started,
                          std::uint64_t nextInputSequence) noexcept {
    if (!instance.missionStateBound) {
        fault_instance(instance, "mission program has no authoritative State binding");
        return false;
    }
    lua_vm::Snapshot vm{};
    lua_vm::snapshot(instance.vm, vm);
    std::vector<mission_state::TypedIntent> pendingIntents{};
    std::array<mission_state::ScriptVariable, mission_state::kVariableCapacity> variables{};
    std::array<mission_state::MissionTimer, mission_state::kTimerCapacity> timers{};
    std::size_t variableCount = 0;
    std::size_t timerCount = 0;
    std::uint64_t nextTimerSequence = mission_state::kAbsentTimerSequence;
    std::uint64_t nextIntentKey = mission_state::kAbsentIntentKey;
    if (!snapshot_state_intents(instance.vm, pendingIntents)) {
        fault_instance(instance, "mission VM outbox snapshot was refused");
        return false;
    }
    if (!lua_vm::snapshot_durable_state(instance.vm,
                                        variables,
                                        variableCount,
                                        timers,
                                        timerCount,
                                        nextTimerSequence,
                                        nextIntentKey)) {
        fault_instance(instance, "mission VM durable-state snapshot was refused");
        return false;
    }
    const mission_state::CommitCandidate transaction{
        .variables = {variables.data(), variableCount},
        .timers = {timers.data(), timerCount},
        .pendingIntents = pendingIntents,
        .nextRevision = vm.stateRevision,
        .nextInputSequence = nextInputSequence,
        .nextTimerSequence = nextTimerSequence,
        .nextIntentKey = nextIntentKey,
        .phase = vm.phase,
        .started = started,
    };
    mission_state::Snapshot snapshot{};
    const mission_state::Status status = mission_state::commit(instance.view.binding,
                                                               instance.programKey,
                                                               instance.missionStateRevision,
                                                               instance.lastMissionSequence,
                                                               transaction,
                                                               snapshot);
    if (status == mission_state::Status::ready) {
        note_variable_pressure(instance, {variables.data(), variableCount});
        const std::uint32_t previousPhase = instance.missionPhase;
        accept_mission_state(instance, snapshot);
        if (instance.missionPhase != previousPhase) {
            queue_phase_entered(instance, previousPhase);
        }
        return true;
    }
    log_line(core::log::Level::warn, &instance, "state_commit", mission_state::status_name(status));
    fault_instance(instance, "authoritative mission State commit was refused");
    instance.programStatus = ProgramStatus::programError;
    return false;
}

namespace {

/**
 * Settles the inputs of every instance whose activity has no script. No program will consume
 * them, and an unsettled row is retained by the Host feed until its capacity refuses new ones.
 */
void retire_scriptless_inputs() noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.programStatus != ProgramStatus::missing) {
            continue;
        }
        mission_state::InputSequenceSnapshot inputState{};
        if (!mission_state::input_sequence_snapshot(instance.view.binding, inputState)
            || inputState.issued == inputState.committed) {
            continue;
        }
        const mission_state::Status status =
            mission_state::retire_unbound_inputs(instance.view.binding);
        if (status != mission_state::Status::ready) {
            log_line(core::log::Level::warn,
                     &instance,
                     "events",
                     mission_state::status_name(status),
                     "reason=scriptless_inputs_retained");
        }
    }
}

/** Delivers queued derived events in arrival order until one needs output or the batch is spent. */
void service_script_events(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        const auto ready = [&] {
            if (!instance.occupied || instance.scriptEventRead >= instance.scriptEvents.size()
                || instance.programStatus != ProgramStatus::loaded || instance.startPending) {
                return false;
            }
            lua_vm::Intent pendingIntent{};
            return instance.deliveryStage == DeliveryStage::idle
                   && !lua_vm::pending_intent(instance.vm, pendingIntent);
        };
        const auto dispatch = [&] {
            const bool firstAttempt = instance.scriptEventAttempts == 0;
            if (firstAttempt) {
                instance.firstScriptEventAttempt = now;
            }
            ++instance.scriptEventAttempts;
            host::Event& head = instance.scriptEvents[instance.scriptEventRead];
            head.tick = now;
            static_cast<void>(dispatch_event(instance, head, nullptr, nullptr, firstAttempt, now));
            retire_script_event(instance);
        };
        static_cast<void>(drain_script_event_batch(ready, dispatch));
    }
}

/** Selects at most one due timer per instance without adding a second durable event feed. */
void service_timers(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.programStatus != ProgramStatus::loaded
            || instance.startPending) {
            continue;
        }
        lua_vm::Intent pendingIntent{};
        if (instance.deliveryStage != DeliveryStage::idle
            || lua_vm::pending_intent(instance.vm, pendingIntent)) {
            continue;
        }
        if (!instance.timerPending) {
            mission_state::InputSequenceSnapshot inputState{};
            if (has_pending_host_input(instance)
                || !mission_state::input_sequence_snapshot(instance.view.binding, inputState)
                || inputState.faulted || inputState.issued != inputState.committed) {
                continue;
            }
            std::array<lua_vm::ScriptVariable, lua_vm::kVariableCapacity> variables{};
            std::array<lua_vm::MissionTimer, lua_vm::kTimerCapacity> timers{};
            std::size_t variableCount = 0;
            std::size_t timerCount = 0;
            std::uint64_t nextTimerSequence = mission_state::kAbsentTimerSequence;
            std::uint64_t nextIntentKey = mission_state::kAbsentIntentKey;
            if (!lua_vm::snapshot_durable_state(instance.vm,
                                                variables,
                                                variableCount,
                                                timers,
                                                timerCount,
                                                nextTimerSequence,
                                                nextIntentKey)) {
                fault_instance(instance, "mission timer snapshot was refused");
                continue;
            }
            const lua_vm::MissionTimer* selected = nullptr;
            for (std::size_t index = 0; index < timerCount; ++index) {
                if (timers[index].deadlineTick <= now
                    && (selected == nullptr || earlier_timer(timers[index], *selected))) {
                    selected = &timers[index];
                }
            }
            if (selected == nullptr) {
                continue;
            }
            instance.pendingTimerEvent = {};
            instance.pendingTimerEvent.attemptGeneration = instance.attempt.generation;
            instance.pendingTimerEvent.binding = instance.view.binding;
            instance.pendingTimerEvent.timerName = selected->key;
            instance.pendingTimerEvent.sequence = selected->sequence;
            instance.pendingTimerEvent.tick = now;
            instance.pendingTimerEvent.sourceGeneration = instance.view.activityClientGeneration;
            instance.pendingTimerEvent.timerDeadlineTick = selected->deadlineTick;
            instance.pendingTimerEvent.timerSequence = selected->sequence;
            instance.pendingTimerEvent.missionSequence = instance.lastMissionSequence;
            instance.pendingTimerEvent.kind = host::EventKind::timerElapsed;
            // A runtime dialogue timer closes its cue; the source is described again from the
            // catalog, and a cue it no longer describes still finishes with its row and cue.
            std::uint32_t dialogueSlotRow = 0;
            std::uint16_t dialogueCue = 0;
            if (lua_vm::parse_dialogue_timer_key(selected->key, dialogueSlotRow, dialogueCue)) {
                instance.pendingTimerEvent.kind = host::EventKind::dialogueFinished;
                static_cast<void>(describe_dialogue_cue(
                    instance, dialogueSlotRow, dialogueCue, instance.pendingTimerEvent));
            }
            instance.firstTimerAttempt = now;
            instance.nextTimerAttempt = now;
            instance.timerAttempts = 0;
            instance.timerPending = true;
        }
        const bool firstAttempt = instance.timerAttempts == 0;
        ++instance.timerAttempts;
        static_cast<void>(dispatch_event(
            instance, instance.pendingTimerEvent, nullptr, nullptr, firstAttempt, now));
        instance.pendingTimerEvent = {};
        instance.firstTimerAttempt = 0;
        instance.nextTimerAttempt = 0;
        instance.timerAttempts = 0;
        instance.timerPending = false;
    }
}

} // namespace

/** Clears all state, reads the enable switch, and resolves the script and SDK Lua paths. */
void initialize() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    clear_attach_diagnostics();
    clear_all_pending_events();
    for (RuntimeInstance& instance : g_instances) {
        clear_instance(instance);
    }
    g_enabled = core::settings::get().server.activation.missionScripting;
    g_pathReady = false;
    reset_feed_cursors();
    if (!g_enabled) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_pathReady = resolve_script_paths();
    if (!g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    log_line(core::log::Level::info, nullptr, "initialize", "enabled");
    ReleaseSRWLockExclusive(&g_lock);
}

/** One pass: attach, start, events, timers, then delivery for every open instance. */
namespace {

// A step this long blocks the game's loopback sends; the step that took it is named.
constexpr std::uint64_t kSlowStepMilliseconds = 40;

/** Runs one step of the mission tick and logs it when it runs past the slow limit. */
template <typename Step> void timed_step(const char* name, Step&& step) noexcept {
    const std::uint64_t started = GetTickCount64();
    step();
    const std::uint64_t elapsed = GetTickCount64() - started;
    if (elapsed < kSlowStepMilliseconds) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=core stage=service result=slow step=mission.%s ms=%llu",
                                      name,
                                      static_cast<unsigned long long>(elapsed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Copies one field into a fixed command buffer. @return False when it does not fit. */
template <std::size_t N>
[[nodiscard]] bool copy_field(std::array<char, N>& output, std::string_view value) noexcept {
    output = {};
    if (value.size() >= N) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.begin());
    return true;
}

/** @return One committed variable value written out as text. */
[[nodiscard]] std::string variable_text(const mission_state::VariableValue& value) {
    switch (value.kind) {
    case mission_state::VariableValueKind::boolean:
        return value.booleanValue ? "true" : "false";
    case mission_state::VariableValueKind::integer:
        return std::to_string(value.integerValue);
    case mission_state::VariableValueKind::real:
        return std::to_string(value.realValue);
    case mission_state::VariableValueKind::string:
        return {value.stringValue.data(),
                (std::min)(static_cast<std::size_t>(value.stringLength), value.stringValue.size())};
    }
    return {};
}

/**
 * Writes a program description out with each `{id}` token replaced by the authored name the pinned
 * SDK view gives that slot, squad or scene; a token it cannot name is kept as written.
 */
[[nodiscard]] std::string expand_names(const lua_vm::DefinitionApi& api, std::string_view text) {
    std::string output{};
    output.reserve(text.size());
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t open = text.find('{', cursor);
        const std::size_t close = open == std::string_view::npos ? open : text.find('}', open);
        if (close == std::string_view::npos) {
            output.append(text.substr(cursor));
            break;
        }
        output.append(text.substr(cursor, open - cursor));
        const std::string_view token = text.substr(open + 1, close - open - 1);
        std::string_view name{};
        if (token.starts_with("slot/")) {
            lua_vm::SlotDefinition slot{};
            if (api.resolveSlotId != nullptr && api.resolveSlotId(api.context, token, slot)) {
                name = slot.name;
            }
        } else if (token.starts_with("squad/")) {
            lua_vm::SquadDefinition squad{};
            if (api.resolveSquadId != nullptr && api.resolveSquadId(api.context, token, squad)) {
                name = squad.name;
            }
        } else if (token.starts_with("symbol/")) {
            lua_vm::SceneDefinition scene{};
            lua_vm::SlotDefinition slot{};
            if (api.resolveSceneId != nullptr && api.resolveSlotRow != nullptr
                && api.resolveSceneId(api.context, token, scene)
                && api.resolveSlotRow(api.context, scene.slotRow, slot)) {
                name = slot.name;
            }
        }
        output.append(name.empty() ? token : name);
        cursor = close + 1;
    }
    return output;
}

/** Runs the waiting developer command in the first program that is started and idle. */
void service_dev_command(std::uint64_t now) noexcept {
    if (!g_devCommandPending) {
        return;
    }
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied) {
            continue;
        }
        lua_vm::Intent pendingIntent{};
        if (instance.programStatus != ProgramStatus::loaded || !instance.missionStarted
            || instance.startPending || instance.deliveryStage != DeliveryStage::idle
            || lua_vm::pending_intent(instance.vm, pendingIntent)) {
            return;
        }
        g_devCommandPending = false;
        const lua_vm::CallStatus status = lua_vm::dev_command(instance.vm, g_devCommand, now);
        note_vm_status(instance, "dev", lua_vm::status_name(status));
        log_line(core::log::Level::info,
                 &instance,
                 "dev_command",
                 lua_vm::status_name(status),
                 {},
                 g_devCommand.name.data());
        if (status == lua_vm::CallStatus::committed || status == lua_vm::CallStatus::noHandler) {
            static_cast<void>(commit_mission_state(instance, true, instance.lastMissionSequence));
        } else if (status != lua_vm::CallStatus::inactive) {
            persist_mission_fault(instance);
        }
        return;
    }
}

} // namespace

/**
 * Runs one mission tick under the runtime lock, timing each step.
 * @param now Service tick every step measures its deadlines from.
 */
void service(std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    timed_step("synchronize", [now] { synchronize_instances(now); });
    timed_step("starts", [now] { service_pending_starts(now); });
    timed_step("delivery_events", [now] { consume_delivery_events(now); });
    timed_step("inputs", [now] { consume_mission_inputs(now); });
    timed_step("scriptless", [] { retire_scriptless_inputs(); });
    timed_step("timers", [now] { service_timers(now); });
    timed_step("script_events", [now] { service_script_events(now); });
    timed_step("dev_command", [now] { service_dev_command(now); });
    timed_step("dispatch", [now] {
        for (RuntimeInstance& instance : g_instances) {
            if (instance.occupied) {
                if (instance.programStatus == ProgramStatus::programError) {
                    reconcile_terminal_delivery(instance);
                } else {
                    dispatch_intent(instance, now);
                }
            }
        }
    });
    ReleaseSRWLockExclusive(&g_lock);
}

bool dev_view(DevView& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    try {
        for (const RuntimeInstance& instance : g_instances) {
            if (!instance.occupied) {
                continue;
            }
            found = true;
            output.binding = instance.view.binding;
            output.running = instance.programStatus == ProgramStatus::loaded
                             && instance.missionStarted && !instance.startPending;
            output.attemptGeneration = instance.attempt.generation;
            output.stateRevision = instance.missionStateRevision;
            output.commandPending = g_devCommandPending;
            output.description =
                expand_names(sdk_bridge::definition_api(instance.view, instance.worldView),
                             lua_vm::debug_description(instance.vm));
            lua_vm::Snapshot diagnostics{};
            lua_vm::snapshot(instance.vm, diagnostics);
            output.lastError = diagnostics.lastError;
            output.faulted = diagnostics.faulted;
            std::array<lua_vm::ScriptVariable, lua_vm::kVariableCapacity> variables{};
            std::array<lua_vm::MissionTimer, lua_vm::kTimerCapacity> timers{};
            std::size_t variableCount = 0;
            std::size_t timerCount = 0;
            std::uint64_t nextTimerSequence = 0;
            std::uint64_t nextIntentKey = 0;
            if (lua_vm::snapshot_durable_state(instance.vm,
                                               variables,
                                               variableCount,
                                               timers,
                                               timerCount,
                                               nextTimerSequence,
                                               nextIntentKey)) {
                output.variables.reserve(variableCount);
                for (std::size_t index = 0; index < variableCount; ++index) {
                    const lua_vm::ScriptVariable& row = variables[index];
                    output.variables.push_back({std::string(row.key.bytes.data(), row.key.length),
                                                variable_text(row.value)});
                }
            }
            break;
        }
    } catch (const std::bad_alloc&) {
        found = false;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void cancel_dev_command() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_devCommandPending = false;
    g_devCommand = {};
    ReleaseSRWLockExclusive(&g_lock);
}

std::uint32_t first_activity_row() noexcept {
    AcquireSRWLockShared(&g_lock);
    std::uint32_t row = 0;
    for (const RuntimeInstance& instance : g_instances) {
        if (instance.occupied && instance.view.activityRow != format::kAbsentIndex) {
            row = instance.view.activityRow + 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return row;
}

bool queue_dev_command(std::string_view name,
                       std::string_view step,
                       std::string_view slot) noexcept {
    lua_vm::DevCommand command{};
    if (name.empty() || !copy_field(command.name, name) || !copy_field(command.step, step)
        || !copy_field(command.slot, slot)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const bool queued = !g_devCommandPending;
    if (queued) {
        g_devCommand = command;
        g_devCommandPending = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return queued;
}

/** Copies every open instance row and every retained attach row for the panel. */
void snapshot(DiagnosticsSnapshot& output) noexcept {
    std::destroy_at(&output);
    std::construct_at(&output);
    AcquireSRWLockShared(&g_lock);
    output.enabled = g_enabled;
    output.pathReady = g_pathReady;
    for (const RuntimeInstance& instance : g_instances) {
        if (instance.occupied && output.instanceCount < output.instances.size()) {
            copy_diagnostics(instance, output.instances[output.instanceCount++]);
        }
    }
    snapshot_attach_diagnostics(output);
    ReleaseSRWLockShared(&g_lock);
}

/** Closes every instance and authorizes one program replacement so the next attach recompiles. */
bool reload() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    bool reloaded = true;
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied) {
            continue;
        }
        log_line(core::log::Level::info, &instance, "reload", "requested");
        if (instance.missionStateBound && instance.missionStateFaulted) {
            mission_state::Snapshot recovered{};
            const mission_state::Status status =
                mission_state::recover(instance.view.binding,
                                       instance.programKey,
                                       instance.missionStateRevision,
                                       recovered);
            if (status != mission_state::Status::ready) {
                log_line(core::log::Level::warn,
                         &instance,
                         "reload",
                         mission_state::status_name(status));
                reloaded = false;
                continue;
            }
            accept_mission_state(instance, recovered);
            clear_pending_events(instance.view.binding);
        }
        if (!authorize_reload(instance)) {
            log_line(core::log::Level::warn, &instance, "reload", "capacity");
            reloaded = false;
            continue;
        }
        clear_instance(instance, false);
    }
    clear_attach_diagnostics();
    ReleaseSRWLockExclusive(&g_lock);
    return reloaded;
}

/** Closes every instance and clears every global this unit owns. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (RuntimeInstance& instance : g_instances) {
        clear_instance(instance);
    }
    clear_all_pending_events();
    clear_attach_diagnostics();
    clear_script_paths();
    clear_feed_cursors();
    g_enabled = false;
    g_pathReady = false;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::activity::mission
