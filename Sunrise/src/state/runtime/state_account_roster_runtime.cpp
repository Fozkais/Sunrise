/** Character creation and deletion logic. */

#include <array>
#include <cstdint>

#include "../../core/logging/log.h"
#include "../account/account_state.h"
#include "../account/inventory/inventory_state.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

namespace authored_inventory = account::inventory;

/** One authored equipment slot this build seeds every newly created character with. */
struct StarterItem {
    authored_inventory::EquipmentSlot slot;
    std::uint32_t definitionHash;
    std::int32_t level;
};

/**
 * The account's own single authored default character already carries a complete, proven-valid
 * 17-slot loadout (every native equipment slot, including the 8 that feed the Family-4 light
 * divisor). Item definitions here carry no class restriction, so the same set is safe to grant
 * regardless of the requested class -- it is exactly what this build already ships as its one
 * default character's own gear. Hashes and levels are copied verbatim from
 * `resources/database/investment_defaults.sql`'s seed rows for character slot 0.
 */
constexpr std::array<StarterItem, 17> kStarterLoadout{{
    {authored_inventory::EquipmentSlot::kinetic, 3843477312U, 106},
    {authored_inventory::EquipmentSlot::energy, 1096206669U, 106},
    {authored_inventory::EquipmentSlot::heavy, 1864563948U, 106},
    {authored_inventory::EquipmentSlot::helmet, 4070132608U, 106},
    {authored_inventory::EquipmentSlot::gauntlets, 1636205905U, 106},
    {authored_inventory::EquipmentSlot::chest, 1863170823U, 106},
    {authored_inventory::EquipmentSlot::legs, 193869520U, 106},
    {authored_inventory::EquipmentSlot::classItem, 3044599574U, 106},
    {authored_inventory::EquipmentSlot::ghost, 4135938409U, 0},
    {authored_inventory::EquipmentSlot::vehicle, 3317837688U, 0},
    {authored_inventory::EquipmentSlot::ship, 292872938U, 0},
    {authored_inventory::EquipmentSlot::subclass, 3635991036U, 0},
    {authored_inventory::EquipmentSlot::clanBanner, 1460578929U, 0},
    {authored_inventory::EquipmentSlot::emblem, 1907674138U, 0},
    {authored_inventory::EquipmentSlot::emote, 2038017661U, 0},
    {authored_inventory::EquipmentSlot::finisher, 152583919U, 0},
    {authored_inventory::EquipmentSlot::artifact, 1631206822U, 0},
}};

/**
 * Equips the fixed starter loadout, each item given a freshly allocated instance SOID instead of
 * a fixed one: the same 17 hashes are granted to every created character, so a literal SOID would
 * collide the moment more than one character has ever carried this loadout.
 * @param account In-out account; scanned for collision-free SOIDs as each item is assigned one.
 *                Must already count `character` towards `characterCount`, so the scan sees this
 *                character's own items as they are assigned, not just prior characters.
 * @param character In-out character, already appended to account.characters and counted.
 * @return False when a fresh SOID could not be allocated; the character is left without a
 *         loadout, exactly as it started, rather than half-equipped.
 */
[[nodiscard]] bool seed_starter_loadout(AccountState& account, CharacterState& character) noexcept {
    std::int32_t maxMutationSerial = -1;
    std::int32_t serial = 0;
    for (const StarterItem& starter : kStarterLoadout) {
        std::uint64_t freshSoid = 0;
        if (!runtime::detail::next_item_instance_soid(account, freshSoid)) {
            character.equipment = {};
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             "ev=create_character stage=loadout result=fail reason=soid_exhausted");
            return false;
        }
        authored_inventory::Item item{};
        item.instanceSoid = freshSoid;
        item.definitionHash = starter.definitionHash;
        item.level = starter.level;
        item.quantity = 1;
        item.mutationSerial = serial;
        item.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
        character.equipment.slots[static_cast<std::size_t>(starter.slot)] = item;
        maxMutationSerial = serial;
        ++serial;
    }
    character.nextInventorySerial = static_cast<std::uint32_t>(maxMutationSerial + 1);
    return true;
}

} // namespace

bool create_character(std::uint8_t characterClass,
                      std::uint8_t gender,
                      std::uint8_t race,
                      std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    if (characterClass > static_cast<std::uint8_t>(CharacterClass::warlock)
        || gender > static_cast<std::uint8_t>(CharacterGender::female)
        || race > static_cast<std::uint8_t>(CharacterRace::exo)) {
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=create_character stage=range result=fail class=%u gender=%u race=%u",
                          static_cast<unsigned>(characterClass),
                          static_cast<unsigned>(gender),
                          static_cast<unsigned>(race));
        return false;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=create_character stage=request class=%u gender=%u race=%u chars=%zu "
                      "cap=%zu primary=0x%016llX",
                      static_cast<unsigned>(characterClass),
                      static_cast<unsigned>(gender),
                      static_cast<unsigned>(race),
                      candidate.characterCount,
                      candidate.characters.size(),
                      static_cast<unsigned long long>(candidate.primarySoid));
    if (candidate.characterCount >= candidate.characters.size() || candidate.primarySoid == 0) {
        investment::store::g_mutex.unlock();
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=create_character stage=capacity_or_primary result=fail");
        return false;
    }

    const std::size_t index = candidate.characterCount;
    // The slot index cannot name the SOID: deletion keeps every survivor's SOID, so after
    // deleting a middle character the next free index already belongs to a living one. The
    // lowest unused offset is taken instead, which reuses a freed SOID without ever colliding.
    for (std::uint64_t offset = 1U; offset <= candidate.characters.size(); ++offset) {
        const std::uint64_t soidCandidate = candidate.primarySoid + offset;
        bool taken = false;
        for (std::size_t existing = 0; existing < candidate.characterCount; ++existing) {
            if (candidate.characters[existing].soid == soidCandidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            characterSoid = soidCandidate;
            break;
        }
    }
    if (characterSoid == 0) {
        investment::store::g_mutex.unlock();
        core::log::write(
            core::log::Channel::state, core::log::Level::warn, "ev=create_character stage=soid result=fail");
        return false;
    }

    CharacterState& character = candidate.characters[index];
    character = {};
    character.soid = characterSoid;
    character.race = static_cast<CharacterRace>(race);
    character.gender = static_cast<CharacterGender>(gender);
    character.characterClass = static_cast<CharacterClass>(characterClass);

    // Select the new character and deselect all others.
    for (std::size_t i = 0; i < candidate.characterCount; ++i) {
        candidate.characters[i].selected = false;
    }
    character.selected = true;
    ++candidate.characterCount;

    // Counted above so the fresh-SOID scan inside sees this character's own items as they are
    // assigned, not just the characters that existed before it. Without a loadout the character
    // has no equipped gear, and the Family-4 encoder refuses to publish a character with nothing
    // equipped (an empty gear set has no light average to divide), so it would exist in the
    // database but never actually reach the client.
    if (!seed_starter_loadout(candidate, character)) {
        investment::store::g_mutex.unlock();
        characterSoid = 0;
        return false;
    }

    if (!account::valid(candidate) || !investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=create_character stage=commit result=fail primary=0x%016llX chars=%zu",
                          static_cast<unsigned long long>(candidate.primarySoid),
                          candidate.characterCount);
        characterSoid = 0;
        return false;
    }
    for (std::size_t i = 0; i < candidate.characters.size(); ++i) {
        investment::store::g_session.selected[i] = candidate.characters[i].selected;
    }
    investment::store::g_mutex.unlock();
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=create_character stage=commit result=ok soid=0x%016llX chars=%zu",
                      static_cast<unsigned long long>(characterSoid),
                      candidate.characterCount);
    return true;
}

bool delete_character(std::uint64_t characterSoid) noexcept {
    if (characterSoid == 0) {
        return false;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (candidate.characterCount == 0 || candidate.primarySoid == 0) {
        investment::store::g_mutex.unlock();
        return false;
    }

    std::size_t found = candidate.characterCount;
    for (std::size_t i = 0; i < candidate.characterCount; ++i) {
        if (candidate.characters[i].soid == characterSoid) {
            found = i;
            break;
        }
    }
    if (found == candidate.characterCount) {
        investment::store::g_mutex.unlock();
        return false;
    }

    // Survivors keep the SOID they were created with. Rebasing them onto their new slot index
    // silently renames living characters, which strands every object the client already holds
    // under the old SOID -- its roster entry, its Family-4 body and its banner all keyed by it.
    for (std::size_t i = found; i + 1 < candidate.characterCount; ++i) {
        candidate.characters[i] = candidate.characters[i + 1U];
    }
    --candidate.characterCount;
    candidate.characters[candidate.characterCount] = {};

    // Select the first character if the deleted one was selected.
    bool hasSelection = false;
    for (std::size_t i = 0; i < candidate.characterCount; ++i) {
        if (candidate.characters[i].selected) {
            hasSelection = true;
            break;
        }
    }
    if (!hasSelection && candidate.characterCount > 0) {
        candidate.characters[0].selected = true;
    }

    if (!account::valid(candidate) || !investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=delete_character stage=commit result=fail soid=0x%016llX",
                          static_cast<unsigned long long>(characterSoid));
        return false;
    }
    for (std::size_t i = 0; i < candidate.characters.size(); ++i) {
        investment::store::g_session.selected[i] =
            i < candidate.characterCount && candidate.characters[i].selected;
    }
    investment::store::g_mutex.unlock();
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=delete_character stage=commit result=ok soid=0x%016llX chars=%zu",
                      static_cast<unsigned long long>(characterSoid),
                      candidate.characterCount);
    return true;
}

} // namespace sunrise::state
