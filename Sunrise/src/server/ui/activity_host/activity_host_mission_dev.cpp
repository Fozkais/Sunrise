#include "activity_host_mission_dev.h"

#include <array>
#include <cstddef>
#include <imgui.h>
#include <memory>
#include <string>

#include "../../../client/hooks/teleport/runtime.h"
#include "../../../core/ui/hud/overlay.h"
#include "../../activity/mission/mission_dev_model.h"
#include "../../activity/mission/mission_script_runtime.h"
#include "../mission_editor/mission_editor_window.h"

namespace sunrise::server::ui::activity_host::mission_dev {
namespace {

namespace hud = core::ui::hud;
namespace mission = server::activity::mission;
namespace model = server::activity::mission::dev_model;
namespace teleport = client::hooks::teleport;

constexpr ImVec4 kDone{0.55F, 0.55F, 0.6F, 1.0F};
constexpr ImVec4 kActive{0.31F, 1.0F, 0.47F, 1.0F};
constexpr ImVec4 kArmed{0.35F, 0.67F, 1.0F, 1.0F};
constexpr ImVec4 kIdle{0.75F, 0.45F, 0.9F, 1.0F};

/** The last action's outcome, shown under the toolbar. */
std::string g_status{};

[[nodiscard]] ImVec4 zone_color(model::ZoneState state) noexcept {
    switch (state) {
    case model::ZoneState::expected:
        return kActive;
    case model::ZoneState::armed:
        return kArmed;
    case model::ZoneState::crossed:
        return kDone;
    case model::ZoneState::idle:
        return kIdle;
    }
    return kIdle;
}

[[nodiscard]] const char* zone_word(model::ZoneState state) noexcept {
    switch (state) {
    case model::ZoneState::expected:
        return "waited on";
    case model::ZoneState::armed:
        return "armed";
    case model::ZoneState::crossed:
        return "crossed";
    case model::ZoneState::idle:
        return "not armed";
    }
    return "";
}

/** Queues one command and records what happened for the status line. */
void command(const char* name, const std::string& step, const std::string& slot) noexcept {
    g_status = mission::queue_dev_command(name, step, slot)
                   ? std::string("queued: ") + name + (step.empty() ? "" : " " + step)
                   : std::string("refused: a command is already waiting");
}

/** Replays from one step, reloading the script from disk first when asked. */
void replay(const std::string& step, bool reload) noexcept {
    if (reload && !mission::reload()) {
        g_status = "reload refused: the mission runtime is not ready";
        return;
    }
    command("replay", step, {});
    if (reload) {
        g_status += " after the reload";
    }
}

/**
 * Moves the player to one zone's centre, a little above it. A volume's floor often lies under the
 * ground, so the player drops onto the ground rather than through it.
 */
void move_to(const model::Zone& zone) noexcept {
    teleport::request_move_to({(zone.minimum[0] + zone.maximum[0]) * 0.5F,
                               (zone.minimum[1] + zone.maximum[1]) * 0.5F,
                               (zone.minimum[2] + zone.maximum[2]) * 0.5F + 1.5F});
    g_status = "moving to " + model::human(zone.name);
}

/** One zone row: its state, its name and its two actions. */
void draw_zone_line(const model::Zone& zone, const char* prefix) noexcept {
    ImGui::PushID(zone.slot.c_str());
    ImGui::TextColored(zone_color(zone.state), "%s%s", prefix, model::human(zone.name).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", zone_word(zone.state));
    ImGui::SameLine();
    if (ImGui::SmallButton("Cross")) {
        command("cross", {}, zone.slot);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Report this zone as crossed, as the client does when the player enters it.");
    }
    if (zone.bounded) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Go")) {
            move_to(zone);
        }
    }
    ImGui::PopID();
}

/** One described line; a line waiting on a zone shows that zone's state and actions. */
void draw_line(const model::Model& current, const model::Line& line, const char* prefix) noexcept {
    const model::Zone* const zone =
        line.zone.empty() ? nullptr : model::find_zone(current, line.zone);
    if (zone == nullptr) {
        ImGui::TextWrapped("%s%s", prefix, line.text.c_str());
        return;
    }
    draw_zone_line(*zone, prefix);
    if (!line.text.empty()) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", line.text.c_str());
        ImGui::Unindent();
    }
}

void draw_toolbar(const model::Model& current) noexcept {
    ImGui::Text("%s", current.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("attempt %llu  %s",
                        static_cast<unsigned long long>(current.attemptGeneration),
                        current.running ? "running" : "not running");
    if (current.faulted) {
        ImGui::TextColored({1.0F, 0.45F, 0.45F, 1.0F}, "Faulted: %s", current.lastError.data());
    }
    if (ImGui::Button(mission_editor::window::is_open() ? "Show the editor window"
                                                        : "Open the editor window")) {
        mission_editor::window::open();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A window of its own, for a second screen: sequences, zones, spectator "
                          "camera and the mission editor.");
    }
    bool shown = hud::enabled(hud::Overlay::missionSteps);
    if (ImGui::Checkbox("Show zones in the world and the sequence on the HUD", &shown)) {
        hud::set_enabled(hud::Overlay::missionSteps, shown);
    }
    if (ImGui::Button("Reload script")) {
        g_status = mission::reload() ? "reload queued: the program keeps its progress"
                                     : "reload refused: the mission runtime is not ready";
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads the script from disk again. Progress and what is in the world stay.");
    }
    if (current.active >= 0) {
        const std::string& step = current.steps[static_cast<std::size_t>(current.active)].id;
        ImGui::SameLine();
        if (ImGui::Button("Reload and replay this sequence")) {
            replay(step, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Replay this sequence")) {
            replay(step, false);
        }
    }
    if (current.commandPending) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel waiting command")) {
            mission::cancel_dev_command();
            g_status = "command dropped";
        }
    }
    if (!g_status.empty()) {
        ImGui::TextDisabled("%s", g_status.c_str());
    }
}

void draw_steps(const model::Model& current) noexcept {
    for (std::size_t index = 0; index < current.steps.size(); ++index) {
        const model::Step& step = current.steps[index];
        ImGui::PushID(static_cast<int>(index));
        const bool active = static_cast<int>(index) == current.active;
        const ImVec4 color = step.progress == model::Progress::done ? kDone
                             : active                               ? kActive
                                                                    : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const std::string title = std::to_string(index + 1) + "  " + model::human(step.id)
                                  + (step.goal.empty() ? "" : "   - " + step.goal);
        const bool open = ImGui::TreeNodeEx(
            "##step", active ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", title.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Replay from here")) {
            replay(step.id, true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Reloads the script, removes what it placed, and starts again at this sequence as "
                "if the sequences before it had been played.");
        }
        if (open) {
            for (const model::Line& end : step.ends) {
                draw_line(current, end, "ends when reached: ");
            }
            for (const model::Line& beat : step.beats) {
                draw_line(current, beat, "- ");
            }
            for (const model::Fight& fight : current.fights) {
                if (fight.after == step.id) {
                    draw_line(current, fight.line, "fight: ");
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void draw_zones(const model::Model& current) noexcept {
    if (!ImGui::CollapsingHeader("All zones")) {
        return;
    }
    for (const model::Zone& zone : current.zones) {
        draw_zone_line(zone, "");
    }
}

} // namespace

void draw() noexcept {
    const std::shared_ptr<const model::Model> current = model::current();
    if (current == nullptr) {
        ImGui::TextDisabled("No mission program is open.");
        return;
    }
    draw_toolbar(*current);
    if (!current->described) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "This script declares no sequence description. A campaign-kit mission declares one; "
            "give its content `mission = mission` so zones carry their names and bounds.");
        return;
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##mission_dev_steps", {0.0F, 0.0F})) {
        draw_steps(*current);
        draw_zones(*current);
    }
    ImGui::EndChild();
}

} // namespace sunrise::server::ui::activity_host::mission_dev
