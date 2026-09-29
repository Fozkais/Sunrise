#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::unlock_flags {

/** Clears every extracted unlock flag. */
void clear() noexcept;

/** Clears every flag name. */
void clear_names() noexcept;

/**
 * Checks one complete unlock flag table.
 * @param definitions Candidate rows in slot order.
 * @return True when the rows fit storage and each names a flag.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the extracted unlock flag table in one step.
 * @param definitions Complete rows in slot order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one flag by its slot.
 * @param slot Position in the installed table.
 * @param definition Receives the row.
 * @return True when the table holds that slot.
 */
[[nodiscard]] bool find_slot(std::uint16_t slot, Definition& definition) noexcept;

/**
 * Finds the flag a bank row stores.
 * @param bank Bank code.
 * @param row Row inside that bank.
 * @param slot Receives the flag's slot.
 * @return True when exactly one flag is stored there.
 */
[[nodiscard]] bool
find_bank_row(std::uint16_t bank, std::uint16_t row, std::uint16_t& slot) noexcept;

/**
 * Copies every row in slot order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return The unlock flag row count, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/**
 * Checks one complete flag name table.
 * @param names Candidate rows in ascending slot order.
 * @return True when the rows fit storage, each holds a bounded name, and the slots are unique.
 */
[[nodiscard]] bool valid_names(std::span<const Name> names) noexcept;

/**
 * Replaces the flag names in one step.
 * @param names Complete rows in ascending slot order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace_names(std::span<const Name> names) noexcept;

/**
 * Finds the name of one flag.
 * @param slot Slot of the flag.
 * @param name Receives the row.
 * @return True when the flag has a name.
 */
[[nodiscard]] bool find_name(std::uint16_t slot, Name& name) noexcept;

/**
 * Copies every name in ascending slot order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_names(std::span<Name> output, std::size_t& count) noexcept;

/** @return The flag name row count, read under the lock. */
[[nodiscard]] std::size_t name_count() noexcept;

} // namespace sunrise::state::build_data::unlock_flags
