#pragma once

namespace sunrise::server::ui::activity_host::mission_dev {

/**
 * Draws the running mission program's steps and zones with the developer actions: reload the
 * script, replay from a step, cross a zone, move to a zone.
 */
void draw() noexcept;

} // namespace sunrise::server::ui::activity_host::mission_dev
