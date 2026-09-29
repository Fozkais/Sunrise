#include "ui_hud_mission_steps_overlay.h"

#include <array>
#include <cmath>
#include <imgui.h>
#include <memory>
#include <string>

#include "../../../../client/hooks/teleport/runtime.h"
#include "../../../../server/activity/mission/mission_dev_model.h"

namespace sunrise::core::ui::hud::overlays::mission_steps {
namespace {

namespace model = server::activity::mission::dev_model;
namespace teleport = client::hooks::teleport;

/** Green for what the step waits on, as the world overlay draws it. */
constexpr ImVec4 kExpected{0.31F, 1.0F, 0.47F, 1.0F};

/** @return The distance from the local player to a zone's centre, or a negative value. */
[[nodiscard]] float reach(const model::Zone& zone) noexcept {
    teleport::Vector player{};
    if (!zone.bounded || !teleport::read_position(teleport::local_player_component(), player)) {
        return -1.0F;
    }
    float sum = 0.0F;
    for (std::size_t lane = 0; lane < player.size(); ++lane) {
        const float delta = (zone.minimum[lane] + zone.maximum[lane]) * 0.5F - player[lane];
        sum += delta * delta;
    }
    return std::sqrt(sum);
}

} // namespace

void draw() noexcept {
    const std::shared_ptr<const model::Model> current = model::current();
    if (current == nullptr) {
        ImGui::TextDisabled("no mission program");
        return;
    }
    if (!current->described) {
        ImGui::TextDisabled("%s: the script declares no sequence description", current->title.c_str());
        return;
    }
    ImGui::Text("%s", current->title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("attempt %llu", static_cast<unsigned long long>(current->attemptGeneration));
    if (current->faulted) {
        ImGui::TextColored({1.0F, 0.45F, 0.45F, 1.0F}, "faulted: %s", current->lastError.data());
    }
    if (current->active < 0) {
        ImGui::TextDisabled(current->running ? "no sequence running" : "waiting for the program");
        return;
    }
    const model::Step& step = current->steps[static_cast<std::size_t>(current->active)];
    ImGui::Text("Sequence %d/%zu  %s",
                current->active + 1,
                current->steps.size(),
                model::human(step.id).c_str());
    if (!step.goal.empty()) {
        ImGui::TextDisabled("Goal  %s", step.goal.c_str());
    }
    for (const model::Line& end : step.ends) {
        const model::Zone* const zone =
            end.zone.empty() ? nullptr : model::find_zone(*current, end.zone);
        if (zone == nullptr) {
            ImGui::TextDisabled("Ends  %s", end.text.c_str());
            continue;
        }
        const float metres = reach(*zone);
        if (metres >= 0.0F) {
            ImGui::TextColored(
                kExpected, "Reach  %s  %.0f m", model::human(zone->name).c_str(), metres);
        } else {
            ImGui::TextColored(kExpected, "Reach  %s", model::human(zone->name).c_str());
        }
    }
    if (current->commandPending) {
        ImGui::TextDisabled("developer command waiting");
    }
}

} // namespace sunrise::core::ui::hud::overlays::mission_steps
