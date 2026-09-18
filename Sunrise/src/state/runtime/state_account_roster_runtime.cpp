/** Character creation and deletion logic. */

#include <cstdint>

#include "../../core/logging/log.h"
#include "../account/account_state.h"
#include "../investment/store_internal.h"
#include "runtime.h"

namespace sunrise::state {

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
