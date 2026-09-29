#pragma once

#include <d3d11.h>

#include "../../hooks/teleport/runtime.h"

namespace sunrise::client::ui::activity::mission_zone_overlay {

/**
 * Draws the running mission's player trigger volumes as world boxes coloured by their state in
 * the attempt, and names the ones that matter on screen: those the step waits on, those armed,
 * and any within reach.
 * @return True when anything was drawn, so the frame submits its draw data.
 */
[[nodiscard]] bool draw(ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11RenderTargetView* target,
                        const hooks::teleport::CameraPose& camera) noexcept;

} // namespace sunrise::client::ui::activity::mission_zone_overlay
