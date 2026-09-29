#pragma once

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Finds the boot-flow step accessor behind `in_world`.
 * Nothing is detoured: the accessor is called, so a miss reads as out of world.
 * @return True when the target was found.
 */
[[nodiscard]] bool install_world_step() noexcept;

/** Clears the boot-flow step accessor. */
void uninstall_world_step() noexcept;

/**
 * Detours the client's loading-cinematics switch so that it answers true while an activity
 * continuation is under way; otherwise it answers as shipped.
 * @return True when the target was found and the detour attached.
 */
[[nodiscard]] bool install_loading_suppression() noexcept;

/** Detaches the loading-cinematics detour. */
void uninstall_loading_suppression() noexcept;

/**
 * Attaches the read-only lifetime gate probe, a diagnostic on step 38's joinability gate.
 * @return True when the reader and its helpers were found and the detour attached.
 */
[[nodiscard]] bool install_lifetime_gate_probe() noexcept;

/** Detaches the lifetime gate probe. */
void uninstall_lifetime_gate_probe() noexcept;

} // namespace sunrise::client::hooks::bootflow
