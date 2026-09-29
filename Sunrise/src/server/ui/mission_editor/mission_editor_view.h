#pragma once

struct ImFont;

namespace sunrise::server::ui::mission_editor::view {

/** Hands the view the heading font and the window's pixel scale. */
void set_fonts(ImFont* heading, float scale) noexcept;

/**
 * Draws the mission editor into the current Dear ImGui context: the running mission's steps, the
 * selected step's start, end and zones with the spectator camera's controls, and the editor of a
 * mission kept as data. The caller holds the interface lock.
 */
void draw() noexcept;

} // namespace sunrise::server::ui::mission_editor::view
