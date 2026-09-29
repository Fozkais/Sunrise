#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::freecam {

/**
 * Spectator camera for the mission editor. While it is on, the game's current-camera getter
 * answers with the spectator pose, so the render view is built from it and from nothing the game's
 * camera layers compute; the pose block gets the same pose for whatever reads it directly. Every
 * mouse, keyboard and raw-input message is kept from the game, so the player's body stays where it
 * was and no trigger volume reports.
 * Controls while the game window has focus: move with the physical W A S D keys, Space and C
 * for up and down, Shift faster, Alt slower, the mouse to look, the wheel to change speed.
 */

/** Turns the camera on at the game camera's current pose, or off. */
void set_enabled(bool on) noexcept;

/** @return True while the spectator camera drives the view. */
[[nodiscard]] bool enabled() noexcept;

/** Turns the camera on, placed back from and above a world point and looking at it. */
void look_at(const std::array<float, 3>& target) noexcept;

/** @return The spectator camera's current position, or false while it is off. */
[[nodiscard]] bool position(std::array<float, 3>& output) noexcept;

/**
 * Replaces the pose the game just computed for one player's camera. Runs in the camera-transform
 * hook, before anything reads the pose.
 */
void apply(std::uint32_t playerIndex) noexcept;

/**
 * Gives the spectator pose to whoever asks the game for its current camera, as three floats each.
 * @return False while the camera is off, so the game answers.
 */
[[nodiscard]] bool current(float* position, float* forward, float* up) noexcept;

/** Takes the mouse motion and wheel of one WM_INPUT message while the camera is on. */
void observe_raw_input(HRAWINPUT input) noexcept;

} // namespace sunrise::client::hooks::freecam
