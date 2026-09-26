#pragma once

namespace sunrise::core::settings::server::activation {

/**
 * Gates for the default client-activation work, one per bounded domain.
 * Each domain finishes on its own, so partial work in one cannot make another read as ready.
 */
struct Settings {
    /**
     * The activation coordinator: the transition policy the host applies to a client request to
     * start a different activity. Off, such a request is framed and recorded and answered with
     * nothing.
     */
    bool defaultClientActivation{true};
    /**
     * Membership on a public-target link, so the client binds a world container to it.
     * On by default. Sending it hands the client's player create and destroy source to that
     * link's membership block, and a body that does not carry the local player destroys it.
     */
    bool activityPublicMembership{true};
    /**
     * Registers the client feature name that stops a channel closing when its last owner leaves.
     * Diagnostic only, and off by default. It answers whether that close is on the path to the
     * fast-travel freeze; it is not a fix and comes out once the question is settled.
     */
    bool preventOwnerlessChannelClose{false};
    /** Enables server Activity Host mission scripts. Off leaves compiled host policy in control. */
    bool missionScripting{false};
    /**
     * One ambassador per private bubble in a shared activity. The server picks one present member
     * for each private bubble and names that member's slot in every member's membership body, and
     * only that member's link is granted the bubble and handed its squads. Off, every member names
     * itself on every private record and claims each bubble it enters, as a solo player does.
     */
    bool singlePrivateAmbassador{true};
    /**
     * A fireteam member's move into a region the party has not reached yet reaches the mission
     * program, as the owner's own moves do, so the mission advances whoever gets there first.
     * Off, only the owner's region moves drive the program.
     */
    bool peerRegionFacts{true};
    /**
     * Every scripted dialogue line plays at once for every member, with its authored filter
     * volume dropped. A filtered line otherwise plays only for a client already in its volume
     * when the line arrives, alone or not. Off, every line keeps its filter.
     */
    bool unfilteredSharedDialogue{true};
    /**
     * A client granted the bubble it stands in is handed back the entities given up there, so
     * enemies, vehicles and objects it left behind move and answer again once it returns. Off,
     * those entities stay without an authoritative client until the activity restarts.
     */
    bool claimReleasedEntities{true};
};

} // namespace sunrise::core::settings::server::activation
