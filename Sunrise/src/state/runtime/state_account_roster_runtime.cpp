/** Character creation and deletion logic. */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "../../core/logging/log.h"
#include "../account/account_state.h"
#include "../build_data/items/item_catalog.h"
#include "../unlocks/definition.h"
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
    /** Another definition sharing the item's name, used when the first is not in the build. */
    std::uint32_t alternateHash{};
};

/**
 * The starting kit of a new Guardian as the campaign opens, before Adieu swaps it for the damaged
 * set: the Traveler's Chosen sidearm, the Sorrow-MG2 submachine gun and no heavy weapon, and the
 * cosmetic/utility slots (ghost, vehicle, ship, clan banner, emote, finisher, artifact) the real
 * client draws identically for every class. Weapons and armor are level 1, which the light
 * calculation lifts to its 750 power floor. The cosmetic rows come from the authored character
 * the seed used to carry. The sidearm is the exotic Traveler's Chosen; its damaged common form
 * belongs to Adieu.
 */
constexpr std::array<StarterItem, 9> kSharedStarterItems{{
    {authored_inventory::EquipmentSlot::kinetic, 1853180924U, 1},
    {authored_inventory::EquipmentSlot::energy, 1195725819U, 1},
    {authored_inventory::EquipmentSlot::ghost, 4135938409U, 0},
    {authored_inventory::EquipmentSlot::vehicle, 3317837688U, 0},
    {authored_inventory::EquipmentSlot::ship, 292872938U, 0},
    {authored_inventory::EquipmentSlot::clanBanner, 1460578929U, 0},
    {authored_inventory::EquipmentSlot::emote, 2038017661U, 0},
    {authored_inventory::EquipmentSlot::finisher, 152583919U, 0},
    {authored_inventory::EquipmentSlot::artifact, 1631206822U, 0},
}};

/**
 * The 7 equipment slots the real client renders per class: the campaign's opening armor set, the
 * class item, the subclass (and the abilities/animations it drives), and the class-flavored emblem
 * variant. Equipping another class's items here is the "Hunter wearing Titan gear and subclass"
 * mixup this build must not produce. Indexed by `CharacterClass`'s own wire value (titan=0,
 * hunter=1, warlock=2). Titan wears the legendary Brave Titan look, Hunter the Daring Hunter set
 * and Warlock the Wise Warlock set; the catalog holds those two only as common items. Each set
 * exists twice in the catalog, in one contiguous run of five (gauntlets, chest, class item,
 * helmet, legs). The primary hash of every piece comes from one run so a set is worn whole, the
 * alternate from the other run: Titan 781-785 / 9682-9686, Hunter 9433-9437 / 694-698, Warlock
 * 10384-10388 / 1055-1059. The default subclasses are Sentinel for the Titan, Arcstrider for the
 * Hunter and Dawnblade for the Warlock: the catalog stores its nine subclasses at indices
 * 1188-1196, three per class (Hunter, Titan, Warlock) in arc, solar, void order. Emblems come
 * from the bundled authored characters.
 */
constexpr std::array<std::array<StarterItem, 7>, 3> kPerClassStarterItems{{
    // titan
    {{
        {authored_inventory::EquipmentSlot::helmet, 4081859017U, 1, 4225579453U},
        {authored_inventory::EquipmentSlot::gauntlets, 1490387264U, 1, 2688111404U},
        {authored_inventory::EquipmentSlot::chest, 2682045448U, 1, 1940451444U},
        {authored_inventory::EquipmentSlot::legs, 185326970U, 1, 3758301014U},
        {authored_inventory::EquipmentSlot::classItem, 89175653U, 1, 2684281417U},
        {authored_inventory::EquipmentSlot::subclass, 3382391785U, 0},
        {authored_inventory::EquipmentSlot::emblem, 0x71B4CC1BU, 0},
    }},
    // hunter
    {{
        {authored_inventory::EquipmentSlot::helmet, 3303733201U, 1, 3285934677U},
        {authored_inventory::EquipmentSlot::gauntlets, 1961861544U, 1, 1961777956U},
        {authored_inventory::EquipmentSlot::chest, 2261200416U, 1, 3640744220U},
        {authored_inventory::EquipmentSlot::legs, 2597269762U, 1, 3892423886U},
        {authored_inventory::EquipmentSlot::classItem, 1188458845U, 1, 1056171153U},
        {authored_inventory::EquipmentSlot::subclass, 1334959255U, 0},
        {authored_inventory::EquipmentSlot::emblem, 0x71B4CC1AU, 0},
    }},
    // warlock
    {{
        {authored_inventory::EquipmentSlot::helmet, 2214399070U, 1, 3081865122U},
        {authored_inventory::EquipmentSlot::gauntlets, 1018337679U, 1, 2421406347U},
        {authored_inventory::EquipmentSlot::chest, 3997262569U, 1, 2209865285U},
        {authored_inventory::EquipmentSlot::legs, 1634414641U, 1, 3471587229U},
        {authored_inventory::EquipmentSlot::classItem, 1016461220U, 1, 1331814296U},
        {authored_inventory::EquipmentSlot::subclass, 0xCF88FEA5U, 0},
        {authored_inventory::EquipmentSlot::emblem, 0x71B4CC19U, 0},
    }},
}};

/**
 * Character flags a new character holds. TEMPORARY, until mission progress writes them itself.
 * The two release flags of authored characters (82 and 90, the Dreaming City and Ana Bray) are
 * left out: a new account starts without them.
 */
constexpr std::array<std::uint16_t, 4> kStartingCharacterFlags{16, 17, 18, 19};

/**
 * Character-object flag of the "New Light" activity (definition 1041). Left clear, the client
 * sends a new character into the Cosmodrome mission; set, it skips it. TEMPORARY, until the
 * mission writes it itself on completion.
 */
constexpr std::uint16_t kNewLightObjectFlag = 59;

/** Character-object flag of the "Tower Approach" activity (definition 1044). */
constexpr std::uint16_t kTowerApproachObjectFlag = 60;

/** Character-object flag that releases the Tower destination (definition 1085). */
constexpr std::uint16_t kTowerReleaseObjectFlag = 79;

/**
 * Destination the orbit screen names for a new character: the one the authored characters carry.
 * Left at zero the orbit title reads "TRIAL MODE" instead of a destination.
 */
constexpr std::uint32_t kStartingOrbitDestination = 308080871;

/** Name of the optional file, beside the database, that lists a new character's unlock rows. */
constexpr const wchar_t* kStartFileName = L"character_start_unlocks.txt";

/** Maps a bank name written in the start file to its store bank. */
[[nodiscard]] bool parse_start_bank(const char* name, investment::store::Bank& bank) noexcept {
    using Bank = investment::store::Bank;
    struct Named {
        const char* name;
        Bank bank;
    };
    constexpr std::array<Named, 4> kNames{{
        {"characterFlags", Bank::characterFlags},
        {"characterObjectFlags", Bank::characterObjectFlags},
        {"characterObjectValues", Bank::characterObjectValues},
        {"characterProgressions", Bank::characterProgressions},
    }};
    for (const Named& entry : kNames) {
        if (std::strcmp(entry.name, name) == 0) {
            bank = entry.bank;
            return true;
        }
    }
    return false;
}

/**
 * Writes a new character's starting unlock rows into the selected character's banks.
 * With `character_start_unlocks.txt` beside the database, every line `<bank> <slot> [<value>]`
 * is one row (`#` starts a comment, the value defaults to a set flag); without it the built-in
 * flags above are used. The file lets a starting profile be tried without a rebuild.
 * @return The number of rows written.
 */
std::size_t apply_starting_unlocks() noexcept {
    const auto write = [](investment::store::Bank bank, unsigned slot, int value) noexcept {
        if (!investment::store::write_unlock(bank, static_cast<std::uint16_t>(slot), value)) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=create_character stage=unlocks result=fail bank=%d slot=%u",
                              static_cast<int>(bank),
                              slot);
            return false;
        }
        return true;
    };
    std::size_t written = 0;
    const auto& directory = investment::store::data_directory();
    std::FILE* file = nullptr;
    if (!directory.empty()) {
        const auto path = directory / kStartFileName;
        if (_wfopen_s(&file, path.c_str(), L"r") != 0) {
            file = nullptr;
        }
    }
    if (file == nullptr) {
        for (const std::uint16_t flag : kStartingCharacterFlags) {
            written += write(investment::store::Bank::characterFlags, flag, unlocks::kFlagSet) ? 1U : 0U;
        }
        for (const std::uint16_t flag :
             {kNewLightObjectFlag, kTowerApproachObjectFlag, kTowerReleaseObjectFlag}) {
            written += write(investment::store::Bank::characterObjectFlags, flag, unlocks::kFlagSet)
                           ? 1U
                           : 0U;
        }
        return written;
    }
    std::array<char, 160> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        if (line[0] == '#') {
            continue;
        }
        // "<bank> <slot> [<value>]", split by hand so a malformed line is skipped, never read past.
        char* cursor = line.data();
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        char* nameEnd = cursor;
        while (*nameEnd != '\0' && *nameEnd != ' ' && *nameEnd != '\t') {
            ++nameEnd;
        }
        if (nameEnd == cursor || *nameEnd == '\0') {
            continue;
        }
        *nameEnd = '\0';
        char* after = nullptr;
        const unsigned long slot = std::strtoul(nameEnd + 1, &after, 10);
        if (after == nameEnd + 1) {
            continue;
        }
        long value = unlocks::kFlagSet;
        char* valueEnd = nullptr;
        const long parsed = std::strtol(after, &valueEnd, 10);
        if (valueEnd != after) {
            value = parsed;
        }
        investment::store::Bank bank{};
        if (parse_start_bank(cursor, bank) && slot <= 0xFFFFU) {
            written += write(bank, static_cast<unsigned>(slot), static_cast<int>(value)) ? 1U : 0U;
        }
    }
    std::fclose(file);
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=create_character stage=unlocks file=1 rows=%zu",
                      written);
    return written;
}

/** TEMPORARY: set to leave the helmet off, to see whether the customised head is drawn. */
constexpr bool kOmitHelmetForLookTest = false;

/**
 * Equips the class-appropriate starter loadout, each item given a freshly allocated instance SOID
 * instead of a fixed one: the same hashes are granted to every created character of a class, so a
 * literal SOID would collide the moment more than one has ever carried this loadout.
 * @param account In-out account; scanned for collision-free SOIDs as each item is assigned one.
 *                Must already count `character` towards `characterCount`, so the scan sees this
 *                character's own items as they are assigned, not just prior characters.
 * @param character In-out character, already appended to account.characters and counted. Its
 *                  `characterClass` selects which armor/class-item/subclass row is granted.
 * @return False when a fresh SOID could not be allocated; the character is left without a
 *         loadout, exactly as it started, rather than half-equipped.
 */
[[nodiscard]] bool seed_starter_loadout(AccountState& account, CharacterState& character) noexcept {
    const auto& perClass =
        kPerClassStarterItems[static_cast<std::size_t>(character.characterClass)];
    std::int32_t maxMutationSerial = -1;
    std::int32_t serial = 0;
    const auto place = [&](const StarterItem& starter) noexcept {
        // A name can belong to two definitions across the catalog's history; the log says which
        // of them this build carries so a wrong pick shows up at the first creation.
        build_data::items::Definition known;
        build_data::items::Definition other;
        const bool firstKnown = build_data::items::find_hash(starter.definitionHash, known);
        const bool otherKnown = starter.alternateHash != 0
                                && build_data::items::find_hash(starter.alternateHash, other);
        core::log::writef(core::log::Channel::server,
                          core::log::Level::info,
                          "ev=create_character stage=kit slot=%u hash=%u known=%d tier=%u "
                          "bucket=%u alt=%u altknown=%d",
                          static_cast<unsigned>(starter.slot),
                          static_cast<unsigned>(starter.definitionHash),
                          firstKnown ? 1 : 0,
                          static_cast<unsigned>(known.tier),
                          static_cast<unsigned>(known.bucketId),
                          static_cast<unsigned>(starter.alternateHash),
                          otherKnown ? 1 : 0);
        const std::uint32_t chosenHash =
            !firstKnown && otherKnown ? starter.alternateHash : starter.definitionHash;
        std::uint64_t freshSoid = 0;
        if (!runtime::detail::next_item_instance_soid(account, freshSoid)) {
            return false;
        }
        authored_inventory::Item item{};
        item.instanceSoid = freshSoid;
        item.definitionHash = chosenHash;
        item.level = starter.level;
        item.quantity = 1;
        item.mutationSerial = serial;
        item.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
        character.equipment.slots[static_cast<std::size_t>(starter.slot)] = item;
        maxMutationSerial = serial;
        ++serial;
        return true;
    };
    for (const StarterItem& starter : kSharedStarterItems) {
        if (!place(starter)) {
            character.equipment = {};
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             "ev=create_character stage=loadout result=fail reason=soid_exhausted");
            return false;
        }
    }
    for (const StarterItem& starter : perClass) {
        if (kOmitHelmetForLookTest && starter.slot == authored_inventory::EquipmentSlot::helmet) {
            continue;
        }
        if (!place(starter)) {
            character.equipment = {};
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             "ev=create_character stage=loadout result=fail reason=soid_exhausted");
            return false;
        }
    }
    character.nextInventorySerial = static_cast<std::uint32_t>(maxMutationSerial + 1);
    return true;
}

} // namespace

bool create_character(std::uint8_t characterClass,
                      std::uint8_t gender,
                      std::uint8_t race,
                      std::span<const std::uint8_t> customisation,
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
    // A new Guardian starts at level 1.
    character.level = 1;
    // The authored characters carry these two presentation values, and the preview flag is what
    // the family-4 object mirrors into its preview fields. A created character left both at zero.
    character.previewAvailable = true;
    character.appearanceValue = 1.0F;
    character.lastOrbitedDestination = kStartingOrbitDestination;
    if (customisation.size() == character.customisation.size()) {
        std::copy(customisation.begin(), customisation.end(), character.customisation.begin());
        character.customised = true;
        std::array<char, 2U * kCustomisationSize + 1U> hex{};
        for (std::size_t offset = 0; offset < customisation.size(); ++offset) {
            std::snprintf(
                hex.data() + 2U * offset, 3U, "%02X", static_cast<unsigned>(customisation[offset]));
        }
        core::log::writef(core::log::Channel::server,
                          core::log::Level::info,
                          "ev=create_character stage=look hex=%s",
                          hex.data());
    }

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
    // The new character is the selected one, so these writes land on its own banks.
    (void)apply_starting_unlocks();
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
