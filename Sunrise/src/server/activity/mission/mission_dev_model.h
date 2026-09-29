#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../../state/activity/definition.h"

namespace sunrise::server::activity::mission::dev_model {

/** Where a step stands in the running attempt. */
enum class Progress : std::uint8_t {
    pending,
    active,
    done,
};

/** What a player trigger volume means for the running attempt. */
enum class ZoneState : std::uint8_t {
    /** Not armed: a crossing reports nothing. */
    idle,
    /** Armed: a crossing reports. */
    armed,
    /** Armed and one the active step ends on. */
    expected,
    /** Reported once in this attempt. */
    crossed,
};

/** One player trigger volume the program arms, with its authored bounds when the SDK has them. */
struct Zone final {
    std::string slot{};
    std::string name{};
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    ZoneState state{ZoneState::idle};
    bool bounded{};
};

/** One described line a step or an encounter holds, and the zone it waits on, if any. */
struct Line final {
    std::string zone{};
    std::string text{};
};

/** One step of the main chain, in chain order. */
struct Step final {
    std::string id{};
    /** Mission variable holding the step's flow progress. */
    std::string key{};
    std::string goal{};
    std::vector<Line> ends{};
    std::vector<Line> beats{};
    Progress progress{Progress::pending};
};

/** One encounter and the step it waits on. */
struct Fight final {
    std::string id{};
    std::string after{};
    Line line{};
};

/** The running program as the in-game mission panel reads it. */
struct Model final {
    state::activity::SessionBinding binding{};
    std::string title{};
    std::vector<Step> steps{};
    std::vector<Zone> zones{};
    std::vector<Fight> fights{};
    std::array<char, 256> lastError{};
    std::uint64_t attemptGeneration{};
    /** Index of the step the attempt is on, or -1. */
    int active{-1};
    bool running{};
    bool faulted{};
    bool described{};
    bool commandPending{};
};

/** What a zone is to one step. */
enum class Role : std::uint8_t {
    /** The step does not use it. */
    none,
    /** The step before ends on it, so the step starts there. */
    start,
    /** The step ends on it. */
    end,
    /** One of the step's beats or fights waits on it. */
    used,
};

/** @return One role per zone of the model, in zone order, for the step at `index`. */
[[nodiscard]] std::vector<Role> roles(const Model& model, int index);

/** Selects the step the world overlay and the editor show; -1 follows the active step. */
void set_focus(int index) noexcept;

/** @return The selected step, or -1 when the active step is followed. */
[[nodiscard]] int focus() noexcept;

/** @return The step the overlay shows: the selected one, else the active one, else -1. */
[[nodiscard]] int shown_step(const Model& model) noexcept;

/** Draws every zone instead of the shown step's. */
void set_show_all(bool on) noexcept;

[[nodiscard]] bool show_all() noexcept;

/** @return The model of the first open program, rebuilt at most every quarter second; null when none. */
[[nodiscard]] std::shared_ptr<const Model> current() noexcept;

/** @return The zone of one slot id, or null. */
[[nodiscard]] const Zone* find_zone(const Model& model, std::string_view slot) noexcept;

/** @return An authored name written for a person: no type prefix, spaces between words. */
[[nodiscard]] std::string human(std::string_view name);

} // namespace sunrise::server::activity::mission::dev_model
