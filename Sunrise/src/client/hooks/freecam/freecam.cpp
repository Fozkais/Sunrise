#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "../../input/window_focus.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::freecam {
namespace {

using Vector = std::array<float, 3>;

constexpr float kDefaultSpeed = 20.0F;
constexpr float kMinimumSpeed = 1.0F;
constexpr float kMaximumSpeed = 400.0F;
constexpr float kFastFactor = 4.0F;
constexpr float kSlowFactor = 0.25F;
constexpr float kRadiansPerCount = 0.0025F;
/** Pitch stops short of straight up or down, where the view basis degenerates. */
constexpr float kPitchLimit = 1.5F;
/** A frame longer than this is a pause, not motion to integrate. */
constexpr float kMaximumStep = 0.1F;
/** `look_at` places the camera this far back and this far up from its target. */
constexpr float kLookBack = 18.0F;
constexpr float kLookUp = 9.0F;

// Physical key positions, so the layout does not matter: W A S D C.
constexpr UINT kScanForward = 0x11;
constexpr UINT kScanLeft = 0x1E;
constexpr UINT kScanBack = 0x1F;
constexpr UINT kScanRight = 0x20;
constexpr UINT kScanDown = 0x2E;

std::atomic_bool g_enabled{false};
/** Set by `set_enabled` and `look_at`; the camera hook reads the game pose then clears it. */
std::atomic_bool g_seed{false};
std::atomic<int> g_mouseX{0};
std::atomic<int> g_mouseY{0};
std::atomic<int> g_wheel{0};

// Owned by the camera-hook thread, apart from the look-at request below.
Vector g_position{};
float g_yaw{};
float g_pitch{};
float g_speed{kDefaultSpeed};
LARGE_INTEGER g_lastTick{};

/** The last pose written, which `current` answers with; ready once one frame has flown. */
SRWLOCK g_poseLock{SRWLOCK_INIT};
teleport::CameraPose g_lastPose{};
std::atomic_bool g_poseReady{false};

SRWLOCK g_requestLock{SRWLOCK_INIT};
bool g_lookRequested{};
Vector g_lookTarget{};
Vector g_published{};
bool g_publishedValid{};

[[nodiscard]] bool key_down(UINT scan) noexcept {
    const UINT key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}

[[nodiscard]] bool virtual_down(int key) noexcept {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

[[nodiscard]] Vector forward_of(float yaw, float pitch) noexcept {
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
}

[[nodiscard]] Vector cross(const Vector& a, const Vector& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

[[nodiscard]] Vector normalized(const Vector& value) noexcept {
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    return length > 1e-6F ? Vector{value[0] / length, value[1] / length, value[2] / length}
                          : Vector{1.0F, 0.0F, 0.0F};
}

/** Points the camera from its position at one world point. */
void aim(const Vector& target) noexcept {
    const Vector towards = normalized(
        {target[0] - g_position[0], target[1] - g_position[1], target[2] - g_position[2]});
    g_yaw = std::atan2(towards[1], towards[0]);
    g_pitch = std::clamp(std::asin(std::clamp(towards[2], -1.0F, 1.0F)), -kPitchLimit, kPitchLimit);
}

/** @return Seconds since the previous frame, bounded so a pause does not jump the camera. */
[[nodiscard]] float frame_seconds() noexcept {
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float seconds = g_lastTick.QuadPart == 0 || frequency.QuadPart == 0
                              ? 0.0F
                              : static_cast<float>(now.QuadPart - g_lastTick.QuadPart)
                                    / static_cast<float>(frequency.QuadPart);
    g_lastTick = now;
    return std::clamp(seconds, 0.0F, kMaximumStep);
}

} // namespace

void set_enabled(bool on) noexcept {
    if (on && !g_enabled.load(std::memory_order_acquire)) {
        g_seed.store(true, std::memory_order_release);
    }
    g_enabled.store(on, std::memory_order_release);
    if (!on) {
        g_poseReady.store(false, std::memory_order_release);
        AcquireSRWLockExclusive(&g_requestLock);
        g_publishedValid = false;
        ReleaseSRWLockExclusive(&g_requestLock);
    }
}

bool enabled() noexcept {
    return g_enabled.load(std::memory_order_acquire);
}

void look_at(const std::array<float, 3>& target) noexcept {
    AcquireSRWLockExclusive(&g_requestLock);
    g_lookTarget = target;
    g_lookRequested = true;
    ReleaseSRWLockExclusive(&g_requestLock);
    set_enabled(true);
}

bool position(std::array<float, 3>& output) noexcept {
    AcquireSRWLockShared(&g_requestLock);
    const bool valid = g_publishedValid;
    output = g_published;
    ReleaseSRWLockShared(&g_requestLock);
    return valid;
}

void apply(std::uint32_t playerIndex) noexcept {
    if (!g_enabled.load(std::memory_order_acquire)) {
        g_lastTick = {};
        return;
    }
    if (g_seed.exchange(false, std::memory_order_acq_rel)) {
        teleport::CameraPose game{};
        if (teleport::read_camera_pose(playerIndex, game)) {
            g_position = game.position;
            g_yaw = std::atan2(game.forward[1], game.forward[0]);
            g_pitch = std::clamp(std::asin(std::clamp(game.forward[2], -1.0F, 1.0F)),
                                 -kPitchLimit,
                                 kPitchLimit);
        }
        g_mouseX.store(0);
        g_mouseY.store(0);
        g_wheel.store(0);
    }
    AcquireSRWLockExclusive(&g_requestLock);
    if (g_lookRequested) {
        g_lookRequested = false;
        const Vector back = forward_of(g_yaw, 0.0F);
        g_position = {g_lookTarget[0] - back[0] * kLookBack,
                      g_lookTarget[1] - back[1] * kLookBack,
                      g_lookTarget[2] + kLookUp};
        aim(g_lookTarget);
    }
    ReleaseSRWLockExclusive(&g_requestLock);

    const float step = frame_seconds();
    const bool focused = client::input::game_focused();
    const int mouseX = g_mouseX.exchange(0);
    const int mouseY = g_mouseY.exchange(0);
    const int wheel = g_wheel.exchange(0);
    if (focused) {
        g_yaw -= static_cast<float>(mouseX) * kRadiansPerCount;
        g_pitch = std::clamp(g_pitch - static_cast<float>(mouseY) * kRadiansPerCount,
                             -kPitchLimit,
                             kPitchLimit);
        if (wheel != 0) {
            g_speed = std::clamp(g_speed * (wheel > 0 ? 1.25F : 0.8F), kMinimumSpeed, kMaximumSpeed);
        }
    }
    const Vector forward = forward_of(g_yaw, g_pitch);
    const Vector right = normalized(cross(forward, {0.0F, 0.0F, 1.0F}));
    const Vector up = cross(right, forward);
    if (focused) {
        float speed = g_speed;
        if (virtual_down(VK_SHIFT)) {
            speed *= kFastFactor;
        }
        if (virtual_down(VK_MENU)) {
            speed *= kSlowFactor;
        }
        Vector move{};
        const auto add = [&](const Vector& axis, float amount) {
            for (std::size_t lane = 0; lane < move.size(); ++lane) {
                move[lane] += axis[lane] * amount;
            }
        };
        add(forward, (key_down(kScanForward) ? 1.0F : 0.0F) - (key_down(kScanBack) ? 1.0F : 0.0F));
        add(right, (key_down(kScanRight) ? 1.0F : 0.0F) - (key_down(kScanLeft) ? 1.0F : 0.0F));
        add({0.0F, 0.0F, 1.0F},
            (virtual_down(VK_SPACE) ? 1.0F : 0.0F) - (key_down(kScanDown) ? 1.0F : 0.0F));
        for (std::size_t lane = 0; lane < move.size(); ++lane) {
            g_position[lane] += move[lane] * speed * step;
        }
    }
    teleport::CameraPose pose{};
    pose.position = g_position;
    pose.forward = forward;
    pose.up = up;
    static_cast<void>(teleport::write_camera_pose(playerIndex, pose));
    AcquireSRWLockExclusive(&g_poseLock);
    g_lastPose = pose;
    ReleaseSRWLockExclusive(&g_poseLock);
    g_poseReady.store(true, std::memory_order_release);
    AcquireSRWLockExclusive(&g_requestLock);
    g_published = g_position;
    g_publishedValid = true;
    ReleaseSRWLockExclusive(&g_requestLock);
}

bool current(float* position, float* forward, float* up) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) || !g_poseReady.load(std::memory_order_acquire)
        || position == nullptr || forward == nullptr || up == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_poseLock);
    const teleport::CameraPose pose = g_lastPose;
    ReleaseSRWLockShared(&g_poseLock);
    for (std::size_t lane = 0; lane < 3; ++lane) {
        position[lane] = pose.position[lane];
        forward[lane] = pose.forward[lane];
        up[lane] = pose.up[lane];
    }
    return true;
}

void observe_raw_input(HRAWINPUT input) noexcept {
    RAWINPUT raw{};
    UINT size = sizeof raw;
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER))
            == static_cast<UINT>(-1)
        || raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        g_mouseX.fetch_add(raw.data.mouse.lLastX);
        g_mouseY.fetch_add(raw.data.mouse.lLastY);
    }
    if ((raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
        g_wheel.fetch_add(static_cast<SHORT>(raw.data.mouse.usButtonData));
    }
}

} // namespace sunrise::client::hooks::freecam
