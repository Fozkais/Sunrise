#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../member_selection.h"

namespace sunrise::state::activity::bubble_authority {

/** Bubbles a region record can name; the fallback authority slot is never simulated. */
inline constexpr std::size_t kAmbassadorBubbleCount = kFallbackBubble;
/** A bubble index no member is in. */
inline constexpr std::int32_t kNoBubble = -1;

/** One bubble whose ambassador changed in a single refresh. */
struct AmbassadorChange final {
    std::uint64_t fromKey{};
    std::uint64_t toKey{};
    std::uint8_t bubble{kInvalidBubble};
};

/** Every bubble one refresh moved. A refresh follows one report, so few ever move at once. */
struct AmbassadorChanges final {
    std::array<AmbassadorChange, kAmbassadorBubbleCount> entries{};
    std::size_t count{};
};

/** @return Bubble of one region index, or `kNoBubble` outside the 64 usable bubbles. */
[[nodiscard]] inline std::int32_t region_bubble(std::int32_t region) noexcept {
    if (region < 0 || region > kMaximumGrantSliceSetIndex) {
        return kNoBubble;
    }
    return region >> kSliceSetToBubbleShift;
}

/**
 * Names the bubble one member simulates.
 * The held region wins. While the client holds none, it is loading its pending region, and that
 * is where it will simulate once the load ends.
 * @param member Joined member's reported State.
 * @return Bubble index, or `kNoBubble` before the member reports a region.
 */
[[nodiscard]] inline std::int32_t
member_bubble(const membership::MembershipState& member) noexcept {
    if (member.currentReported && member.currentRegion.index >= 0) {
        return region_bubble(member.currentRegion.index);
    }
    return member.pendingReported ? region_bubble(member.region.index) : kNoBubble;
}

/**
 * Names the other bubble a member that holds a region is loading, if any.
 * It may claim that bubble while nobody simulates it, so a second member loading the same bubble
 * already names it and never claims the bubble itself. It is never enough to keep a bubble: after
 * a z-leg switch the pending leg names the region behind the player.
 * @param member Joined member's reported State.
 * @return Bubble index, or `kNoBubble` when the member is not moving to another bubble.
 */
[[nodiscard]] inline std::int32_t
heading_bubble(const membership::MembershipState& member) noexcept {
    if (!member.currentReported || member.currentRegion.index < 0 || !member.pendingReported) {
        return kNoBubble;
    }
    const std::int32_t pending = region_bubble(member.region.index);
    return pending != region_bubble(member.currentRegion.index) ? pending : kNoBubble;
}

/** @return Joined members that have published an identity, so they receive membership bodies. */
[[nodiscard]] inline std::size_t publishing_member_count(const SessionRecord& record) noexcept {
    std::size_t count = 0;
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        const auto* member = member_state(record, row);
        if (member != nullptr && member->hasIdentity) {
            ++count;
        }
    }
    return count;
}

/**
 * Picks again the member that simulates each bubble of one shared activity.
 * The activity's primary member always simulates the bubble it is in. It owns the mission
 * program, whose device, damage and wipe feeds read only its own client's reports, so a bubble it
 * stands in must stay its own exactly as when it plays alone. Any other bubble goes to the member
 * already simulating it while that member is still there, so two members standing together never
 * trade it back and forth, and otherwise to the lowest joined row in it. A bubble nobody is in
 * goes to a member on its way in, the primary member first and then the one already named, so a
 * second member loading the same bubble never claims it too. A member on its way in never takes a
 * bubble from one already in it: its pending leg may only be a precache, or the region a z-leg
 * switch left behind it. A bubble nobody is in or heading to has no ambassador.
 * @param record Joined record, held under the State write lock.
 * @param changes Cleared, then receives every bubble whose ambassador moved.
 * @return True when at least one bubble moved.
 */
inline bool refresh_ambassadors(SessionRecord& record, AmbassadorChanges& changes) noexcept {
    changes = {};
    std::array<std::uint64_t, kAmbassadorBubbleCount> inside{};
    std::array<std::uint64_t, kAmbassadorBubbleCount> heading{};
    std::array<bool, kAmbassadorBubbleCount> keeps{};
    std::array<bool, kAmbassadorBubbleCount> primaryInside{};
    std::array<bool, kAmbassadorBubbleCount> headingKeeps{};
    std::array<bool, kAmbassadorBubbleCount> primaryHeading{};
    auto& current = record.bubbleAuthority.ambassadorKeys;
    const auto usable = [](std::int32_t bubble) noexcept {
        return bubble >= 0 && static_cast<std::size_t>(bubble) < kAmbassadorBubbleCount;
    };
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        // The published identity is the one every recipient's member directory names.
        const auto* member = member_state(record, row);
        if (member == nullptr || !member->hasIdentity || member->identity.memberKey == 0) {
            continue;
        }
        const std::uint64_t key = member->identity.memberKey;
        // Rows are scanned in order, so the first one seen is row 0 when the primary is there.
        if (const std::int32_t bubble = member_bubble(*member); usable(bubble)) {
            const auto index = static_cast<std::size_t>(bubble);
            keeps[index] = keeps[index] || current[index] == key;
            primaryInside[index] = primaryInside[index] || row == 0;
            if (inside[index] == 0) {
                inside[index] = key;
            }
        }
        if (const std::int32_t bubble = heading_bubble(*member); usable(bubble)) {
            const auto index = static_cast<std::size_t>(bubble);
            headingKeeps[index] = headingKeeps[index] || current[index] == key;
            primaryHeading[index] = primaryHeading[index] || row == 0;
            if (heading[index] == 0) {
                heading[index] = key;
            }
        }
    }
    for (std::size_t bubble = 0; bubble < kAmbassadorBubbleCount; ++bubble) {
        // Row 0 is scanned first, so a primary member inside or heading in is the first key there.
        const std::uint64_t next = primaryInside[bubble]    ? inside[bubble]
                                   : keeps[bubble]          ? current[bubble]
                                   : inside[bubble] != 0    ? inside[bubble]
                                   : primaryHeading[bubble] ? heading[bubble]
                                   : headingKeeps[bubble]   ? current[bubble]
                                                            : heading[bubble];
        if (next == current[bubble]) {
            continue;
        }
        changes.entries[changes.count++] = {current[bubble], next, static_cast<std::uint8_t>(bubble)};
        current[bubble] = next;
    }
    return changes.count != 0;
}

/**
 * Refreshes the ambassadors of one shared activity and makes every member publish again when a
 * bubble moved, because each member's membership body names the ambassador of every bubble.
 * Does nothing while the host has `single_private_ambassador` off. A record with one member never
 * republishes, since its body names its own slot either way.
 * @param state Activity State held under the root write lock.
 * @param record Joined record whose members moved or left.
 * @param changes Cleared, then receives every moved bubble so the caller can log it unlocked.
 */
void refresh_shared_ambassadors(ActivityState& state,
                                SessionRecord& record,
                                AmbassadorChanges& changes) noexcept;

/**
 * Logs each moved bubble of one refresh. Called after the State lock is released.
 * @param sessionId Activity session the refresh ran on.
 * @param changes Bubbles the refresh moved.
 */
void report_ambassador_changes(std::uint64_t sessionId, const AmbassadorChanges& changes) noexcept;

/**
 * Reads the member that simulates one bubble of an activity session.
 * @param sessionId Joined activity session.
 * @param bubble Bubble index, 0 through 63.
 * @param memberKey Receives the ambassador's member key, or zero when nobody is in the bubble.
 * @return True when the session exists.
 */
[[nodiscard]] bool bubble_ambassador(std::uint64_t sessionId,
                                     std::uint8_t bubble,
                                     std::uint64_t& memberKey) noexcept;

} // namespace sunrise::state::activity::bubble_authority
