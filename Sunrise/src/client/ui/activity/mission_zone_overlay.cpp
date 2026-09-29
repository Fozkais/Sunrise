#include "mission_zone_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <string>
#include <vector>

#include "../../../core/ui/world_marker/projection.h"
#include "../../../server/activity/mission/mission_dev_model.h"
#include "../../hooks/graphics/renderer/world_lines.h"

namespace sunrise::client::ui::activity::mission_zone_overlay {
namespace {

namespace lines = hooks::graphics::renderer::world_lines;
namespace model = server::activity::mission::dev_model;
namespace projection = core::ui::world_marker;

/** A zone further than this is named only when the step waits on it or it is armed. */
constexpr float kNameReach = 120.0F;
constexpr float kLineWidth = 2.5F;

/** One colour per zone state: waited on, armed, crossed, not armed. */
[[nodiscard]] lines::Color color_of(model::ZoneState state) noexcept {
    switch (state) {
    case model::ZoneState::expected:
        return {80, 255, 120, 255};
    case model::ZoneState::armed:
        return {90, 170, 255, 230};
    case model::ZoneState::crossed:
        return {140, 140, 150, 140};
    case model::ZoneState::idle:
        return {190, 110, 230, 110};
    }
    return {255, 255, 255, 255};
}

/** One colour per role in the shown step: where it starts, where it ends, what it uses. */
[[nodiscard]] lines::Color color_of(model::Role role, model::ZoneState state) noexcept {
    const std::uint8_t alpha = state == model::ZoneState::crossed ? 150 : 255;
    switch (role) {
    case model::Role::start:
        return {255, 210, 70, alpha};
    case model::Role::end:
        return {80, 255, 120, alpha};
    case model::Role::used:
        return {90, 170, 255, alpha};
    case model::Role::none:
        break;
    }
    return color_of(state);
}

[[nodiscard]] const char* role_word(model::Role role) noexcept {
    switch (role) {
    case model::Role::start:
        return "START  ";
    case model::Role::end:
        return "END  ";
    case model::Role::used:
        return "";
    case model::Role::none:
        break;
    }
    return "";
}

[[nodiscard]] ImU32 text_color(const lines::Color& color) noexcept {
    return IM_COL32(color.red, color.green, color.blue, 255);
}

[[nodiscard]] float distance(const std::array<float, 3>& left,
                             const std::array<float, 3>& right) noexcept {
    const float x = left[0] - right[0];
    const float y = left[1] - right[1];
    const float z = left[2] - right[2];
    return std::sqrt(x * x + y * y + z * z);
}

} // namespace

bool draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target,
          const hooks::teleport::CameraPose& camera) noexcept {
    const std::shared_ptr<const model::Model> current = model::current();
    if (current == nullptr || current->zones.empty()) {
        return false;
    }
    // Only the shown step's zones are drawn, unless every zone is asked for.
    const bool everything = model::show_all();
    std::vector<lines::Box> boxes{};
    std::vector<model::Role> roles{};
    try {
        roles = model::roles(*current, model::shown_step(*current));
        boxes.reserve(current->zones.size());
    } catch (...) {
        return false;
    }
    const auto drawn_zone = [&](std::size_t index) {
        return current->zones[index].bounded
               && (everything || roles[index] != model::Role::none);
    };
    for (std::size_t index = 0; index < current->zones.size(); ++index) {
        if (drawn_zone(index)) {
            const model::Zone& zone = current->zones[index];
            boxes.push_back({zone.minimum, zone.maximum, color_of(roles[index], zone.state)});
        }
    }
    if (boxes.empty()) {
        return false;
    }
    lines::Batch batch{};
    batch.boxes = boxes;
    batch.lineWidthPixels = kLineWidth;
    const bool drawn = lines::draw(device, context, target, camera, batch).rendered;

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return drawn;
    }
    const projection::Camera view{
        camera.position, camera.forward, camera.up, camera.horizontalFov, camera.aspect};
    const projection::Viewport area{
        viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y};
    ImDrawList* const list = ImGui::GetForegroundDrawList();
    bool named = false;
    for (std::size_t index = 0; index < current->zones.size(); ++index) {
        const model::Zone& zone = current->zones[index];
        if (!drawn_zone(index)) {
            continue;
        }
        const model::Role role = roles[index];
        // The label sits on the top face's centre, so it stays above the floor the zone covers.
        const std::array<float, 3> anchor{(zone.minimum[0] + zone.maximum[0]) * 0.5F,
                                          (zone.minimum[1] + zone.maximum[1]) * 0.5F,
                                          zone.maximum[2]};
        const float reach = distance(anchor, camera.position);
        const bool wanted = role != model::Role::none || reach <= kNameReach;
        projection::ScreenPoint point{};
        if (!wanted
            || projection::project(anchor, view, area, false, false, point)
                   != projection::ProjectionStatus::visible) {
            continue;
        }
        std::string label = role_word(role) + model::human(zone.name);
        label += "  " + std::to_string(static_cast<int>(reach)) + " m";
        const ImVec2 at{point.x, point.y};
        list->AddText({at.x + 1.0F, at.y + 1.0F}, IM_COL32(0, 0, 0, 220), label.c_str());
        list->AddText(at, text_color(color_of(role, zone.state)), label.c_str());
        named = true;
    }
    return drawn || named;
}

} // namespace sunrise::client::ui::activity::mission_zone_overlay
