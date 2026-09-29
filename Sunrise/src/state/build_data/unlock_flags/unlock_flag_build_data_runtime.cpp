#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "unlock_flag_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the installed unlock flag table is published. */
bool unlock_flags_ready() noexcept {
    return unlock_flags::count() != 0;
}

/** Publishes every installed unlock flag. */
bool publish_unlock_flags(std::span<const unlock_flags::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(unlock_flags::replace(definitions), unlock_flags::clear);
}

} // namespace sunrise::state::build_data
