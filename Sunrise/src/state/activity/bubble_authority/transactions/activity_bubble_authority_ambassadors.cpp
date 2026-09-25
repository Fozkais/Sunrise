#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../runtime/storage/internal.h"
#include "../../member_mutation.h"
#include "../../transactions/internal.h"
#include "../ambassadors.h"

namespace sunrise::state::activity::bubble_authority {

/** Refreshes one shared record's ambassadors and republishes every member when one moved. */
void refresh_shared_ambassadors(ActivityState& state,
                                SessionRecord& record,
                                AmbassadorChanges& changes) noexcept {
    changes = {};
    if (!record.sharedMembers || !core::settings::get().server.activation.singlePrivateAmbassador
        || !refresh_ambassadors(record, changes)) {
        return;
    }
    // A lone member's body names its own slot whether or not it is the ambassador, so its body
    // is unchanged and a new revision would only resend it.
    if (publishing_member_count(record) < 2 || !can_republish_members(record)
        || state.stateRevision == kMaximumRevision) {
        return;
    }
    republish_members(record);
    record.recordRevision = ++state.stateRevision;
}

/** Logs each moved bubble of one refresh. */
void report_ambassador_changes(std::uint64_t sessionId, const AmbassadorChanges& changes) noexcept {
    for (std::size_t index = 0; index < changes.count; ++index) {
        const AmbassadorChange& change = changes.entries[index];
        const char* const result = change.fromKey == 0 ? "assigned"
                                   : change.toKey == 0 ? "cleared"
                                                       : "moved";
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=activity stage=ambassador result=%s soid=0x%llX "
                                          "bubble=%u from=0x%llX to=0x%llX",
                                          result,
                                          static_cast<unsigned long long>(sessionId),
                                          static_cast<unsigned>(change.bubble),
                                          static_cast<unsigned long long>(change.fromKey),
                                          static_cast<unsigned long long>(change.toKey));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Reads the member that simulates one bubble of an activity session. */
bool bubble_ambassador(std::uint64_t sessionId,
                       std::uint8_t bubble,
                       std::uint64_t& memberKey) noexcept {
    memberKey = 0;
    if (sessionId == kAbsentSessionId || bubble >= kAmbassadorBubbleCount) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    const bool found = target != kInvalidSessionSlot;
    if (found) {
        memberKey = state.sessions[target].bubbleAuthority.ambassadorKeys[bubble];
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

} // namespace sunrise::state::activity::bubble_authority
