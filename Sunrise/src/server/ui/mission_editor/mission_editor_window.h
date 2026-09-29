#pragma once

namespace sunrise::server::ui::mission_editor::window {

/**
 * The mission editor's own OS window, apart from the game's: it can sit on a second screen. It
 * runs on its own thread with its own D3D11 device and Dear ImGui context, and takes the
 * interface lock for each frame and each message, so it never races the game's interface.
 */

/** Opens the window, or brings it to the front when it is already open. */
void open() noexcept;

/** @return True while the window is open. */
[[nodiscard]] bool is_open() noexcept;

/** Asks the window to close; it closes on its own thread. */
void close() noexcept;

/**
 * Closes the window and frees its interface context. The caller holds the interface lock, as
 * the renderer's own shutdown does, so this never waits on it.
 */
void shutdown_locked() noexcept;

} // namespace sunrise::server::ui::mission_editor::window
