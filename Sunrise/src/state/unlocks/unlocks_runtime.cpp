#include "unlocks_runtime.h"

#include <array>
#include <limits>
#include <mutex>
#include <shared_mutex>

#include "../../core/logging/log.h"
#include "../build_data/unlock_flags/unlock_flag_catalog.h"
#include "../investment/store_internal.h"

namespace sunrise::state::unlocks {
namespace store = investment::store;
namespace {

namespace flags = build_data::unlock_flags;

/** Logs one flag changing value, with the name its bank row carries when it has one. */
void log_flag_change(const char* bank,
                     std::uint16_t bankCode,
                     std::uint16_t row,
                     std::uint8_t before,
                     std::uint8_t after) noexcept {
    std::array<char, 96> name{};
    (void)flags::describe(bankCode, row, name);
    core::log::writef(core::log::Channel::state,
                      core::log::Level::debug,
                      "ev=unlock stage=flag bank=%s row=%u value=%u->%u name=%s",
                      bank,
                      static_cast<unsigned>(row),
                      static_cast<unsigned>(before),
                      static_cast<unsigned>(after),
                      name.data());
}

/** Logs every row of one flag bank that differs between two copies. */
template <std::size_t Capacity>
void log_flag_bank_changes(const char* bank,
                           std::uint16_t bankCode,
                           const std::array<std::uint8_t, Capacity>& before,
                           const std::array<std::uint8_t, Capacity>& after) noexcept {
    for (std::size_t row = 0; row < Capacity; ++row) {
        if (before[row] != after[row]) {
            log_flag_change(
                bank, bankCode, static_cast<std::uint16_t>(row), before[row], after[row]);
        }
    }
}

/** Saves one flag and, when the state log takes debug events, logs the change it made. */
bool write_flag(store::Bank bank,
                const char* label,
                std::uint16_t bankCode,
                std::uint16_t index,
                std::uint8_t value) noexcept {
    std::int32_t before = 0;
    const bool observed = core::log::accepts(core::log::Channel::state, core::log::Level::debug)
                          && store::read_unlock(bank, index, before);
    if (!store::write_unlock(bank, index, value)) {
        return false;
    }
    if (observed && before != value) {
        log_flag_change(label, bankCode, index, static_cast<std::uint8_t>(before), value);
    }
    return true;
}

} // namespace

/** Replaces the saved unlock banks in one transaction. */
void publish(const Table& table) noexcept {
    (void)store::write_unlocks(table);
}

/** @return A call-local copy of the saved banks. */
Table get() noexcept {
    Table table;
    if (!store::read_unlocks(table)) {
        table = {};
    }
    return table;
}

/** Reads the requested character's banks and the shared account banks together. */
bool snapshot(Table& output, int characterSlot) noexcept {
    return store::read_unlocks(output, characterSlot);
}

/** Clears the saved banks for the active account and selected character. */
void clear() noexcept {
    (void)store::write_unlocks(Table{});
}

/** Applies one mutation and reports success only after its database commit. */
bool mutate(void* context, void (*apply)(void*, Table&) noexcept) noexcept {
    if (apply == nullptr) {
        return false;
    }
    store::Transaction transaction;
    Table table;
    if (!transaction.ready() || !store::read_unlocks(table)) {
        return false;
    }
    // The flag banks are small next to the table, so copying them costs nothing when logging is on.
    const bool observed = core::log::accepts(core::log::Channel::state, core::log::Level::debug);
    const auto accountBefore = observed ? table.accountFlags : decltype(table.accountFlags){};
    const auto characterBefore =
        observed ? table.characterObjectFlags : decltype(table.characterObjectFlags){};
    apply(context, table);
    if (!store::write_unlocks(table) || !transaction.commit()) {
        return false;
    }
    if (observed) {
        log_flag_bank_changes("account", flags::kAccountBank, accountBefore, table.accountFlags);
        log_flag_bank_changes("character_object",
                              flags::kCharacterObjectBank,
                              characterBefore,
                              table.characterObjectFlags);
    }
    return true;
}

/** Reads one saved accountFlags entry. */
bool account_flag_set(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded =
        index < kAccountFlagCapacity && store::read_unlock(store::Bank::accountFlags, index, value);
    return loaded && value == kFlagSet;
}

/** Reads one saved characterObjectFlags entry. */
bool character_object_flag_set(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded = index < kCharacterObjectFlagCapacity
                        && store::read_unlock(store::Bank::characterObjectFlags, index, value);
    return loaded && value == kFlagSet;
}

/** Reads one saved objectiveValues entry. */
std::int32_t objective_value(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded = index < kObjectiveValueCapacity
                        && store::read_unlock(store::Bank::objectiveValues, index, value);
    return loaded ? value : 0;
}

/** Reads one saved accountProgressions entry. */
std::int32_t account_progression(std::uint16_t definitionIndex) noexcept {
    std::int32_t value = 0;
    const bool loaded =
        definitionIndex < build_data::progressions::kDefinitionCapacity
        && store::read_unlock(store::Bank::accountProgressions, definitionIndex, value);
    return loaded ? value : 0;
}

/** Saves one bounded accountFlags entry. */
bool set_account_flag(std::uint16_t index, std::uint8_t value) noexcept {
    return index < kAccountFlagCapacity
           && write_flag(store::Bank::accountFlags, "account", flags::kAccountBank, index, value);
}

/** Saves one bounded objectiveValues entry. */
bool set_objective_value(std::uint16_t index, std::int32_t value) noexcept {
    return index < kObjectiveValueCapacity
           && store::write_unlock(store::Bank::objectiveValues, index, value);
}

/** Saves one bounded characterObjectFlags entry. */
bool set_character_object_flag(std::uint16_t index, std::uint8_t value) noexcept {
    return index < kCharacterObjectFlagCapacity
           && write_flag(store::Bank::characterObjectFlags,
                         "character_object",
                         flags::kCharacterObjectBank,
                         index,
                         value);
}

/** Saves one bounded characterObjectValues entry. */
bool set_character_object_value(std::uint16_t index, std::int32_t value) noexcept {
    return index < kCharacterObjectValueCapacity
           && store::write_unlock(store::Bank::characterObjectValues, index, value);
}

/** Saves one bounded accountProgressions entry. */
bool set_account_progression(std::uint16_t definitionIndex, std::int32_t value) noexcept {
    return definitionIndex < build_data::progressions::kDefinitionCapacity
           && store::write_unlock(store::Bank::accountProgressions, definitionIndex, value);
}

/** Adds to an objective under the same transaction that guards overflow. */
bool add_objective_value(std::uint16_t index, std::int32_t amount) noexcept {
    store::Transaction transaction;
    std::int32_t value = 0;
    if (!transaction.ready() || index >= kObjectiveValueCapacity
        || !store::read_unlock(store::Bank::objectiveValues, index, value)
        || (amount > 0 && value > (std::numeric_limits<std::int32_t>::max)() - amount)
        || (amount < 0 && value < (std::numeric_limits<std::int32_t>::min)() - amount)) {
        return false;
    }
    return store::write_unlock(store::Bank::objectiveValues, index, value + amount)
           && transaction.commit();
}

} // namespace sunrise::state::unlocks
