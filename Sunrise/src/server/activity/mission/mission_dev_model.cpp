/**
 * The in-game mission panel's view of the running program: the description the program declares,
 * joined with its committed variables.
 *
 * The description is one record per line, fields separated by tabs:
 *   mission <key>
 *   zone    <slot id> <name> <min x> <min y> <min z> <max x> <max y> <max z>   bounds may be "-"
 *   step    <id> <progress variable> <goal>
 *   end     <step id> <zone slot id or -> <text>
 *   beat    <step id> <zone slot id or -> <text>
 *   fight   <id> <after step id> <zone slot id or -> <text>
 * A step's progress variable holds 1 once it has started and 2 once it has finished. A zone is
 * armed while `armed/<slot id>` is true and crossed once `hit/<slot id>` is true.
 */

#include "mission_dev_model.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <new>
#include <unordered_map>

#include "mission_script_runtime.h"

namespace sunrise::server::activity::mission::dev_model {
namespace {

/** The panel, the HUD and the world overlay all read one model; it is rebuilt this often. */
constexpr std::uint64_t kRefreshMs = 250;

SRWLOCK g_lock{SRWLOCK_INIT};
std::shared_ptr<const Model> g_model{};
std::uint64_t g_builtAt{};
std::atomic<int> g_focus{-1};
std::atomic_bool g_showAll{false};

/** @return The fields of one tab-separated record. */
[[nodiscard]] std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> output{};
    std::size_t cursor = 0;
    while (true) {
        const std::size_t tab = line.find('\t', cursor);
        output.push_back(line.substr(cursor, tab == std::string_view::npos ? tab : tab - cursor));
        if (tab == std::string_view::npos) {
            return output;
        }
        cursor = tab + 1;
    }
}

/** @return The field, or an empty view when the record is shorter. */
[[nodiscard]] std::string_view field(const std::vector<std::string_view>& row, std::size_t index) {
    return index < row.size() ? row[index] : std::string_view{};
}

/** @return A zone slot field, empty for "-". */
[[nodiscard]] std::string zone_field(std::string_view value) {
    return value == "-" ? std::string{} : std::string(value);
}

/** Reads six space-separated floats. @return False when any is missing. */
[[nodiscard]] bool read_bounds(std::string_view text, Zone& zone) noexcept {
    std::array<float, 6> values{};
    const char* cursor = text.data();
    const char* const end = text.data() + text.size();
    for (float& value : values) {
        while (cursor < end && *cursor == ' ') {
            ++cursor;
        }
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc{}) {
            return false;
        }
        cursor = parsed.ptr;
    }
    zone.minimum = {values[0], values[1], values[2]};
    zone.maximum = {values[3], values[4], values[5]};
    return true;
}

/** @return The step with this id, or null. */
[[nodiscard]] Step* find_step(Model& model, std::string_view id) noexcept {
    for (Step& step : model.steps) {
        if (step.id == id) {
            return &step;
        }
    }
    return nullptr;
}

/** Fills the static part of the model from the program's description. */
void parse(std::string_view description, Model& model) {
    std::size_t cursor = 0;
    while (cursor < description.size()) {
        const std::size_t newline = description.find('\n', cursor);
        const std::string_view line = description.substr(
            cursor, newline == std::string_view::npos ? newline : newline - cursor);
        cursor = newline == std::string_view::npos ? description.size() : newline + 1;
        const std::vector<std::string_view> row = fields(line);
        const std::string_view kind = field(row, 0);
        if (kind == "mission") {
            model.title = field(row, 1);
        } else if (kind == "zone") {
            Zone zone{};
            zone.slot = field(row, 1);
            zone.name = field(row, 2);
            zone.bounded = read_bounds(field(row, 3), zone);
            model.zones.push_back(std::move(zone));
        } else if (kind == "step") {
            Step step{};
            step.id = field(row, 1);
            step.key = field(row, 2);
            step.goal = field(row, 3);
            model.steps.push_back(std::move(step));
        } else if (kind == "end" || kind == "beat") {
            Step* const step = find_step(model, field(row, 1));
            if (step != nullptr) {
                (kind == "end" ? step->ends : step->beats)
                    .push_back({zone_field(field(row, 2)), std::string(field(row, 3))});
            }
        } else if (kind == "fight") {
            model.fights.push_back({std::string(field(row, 1)),
                                    std::string(field(row, 2)),
                                    {zone_field(field(row, 3)), std::string(field(row, 4))}});
        }
    }
    model.described = !model.steps.empty();
}

/** Joins the committed variables: step progress, the active step and every zone's state. */
void apply_state(const std::vector<DevVariable>& variables, Model& model) {
    std::unordered_map<std::string_view, std::string_view> values{};
    values.reserve(variables.size());
    for (const DevVariable& variable : variables) {
        values.emplace(variable.key, variable.value);
    }
    const auto value = [&](const std::string& key) {
        const auto found = values.find(key);
        return found == values.end() ? std::string_view{} : found->second;
    };
    for (std::size_t index = 0; index < model.steps.size(); ++index) {
        Step& step = model.steps[index];
        const std::string_view progress = value(step.key);
        step.progress = progress == "2"   ? Progress::done
                        : progress == "1" ? Progress::active
                                          : Progress::pending;
        if (step.progress == Progress::active && model.active < 0) {
            model.active = static_cast<int>(index);
        }
    }
    for (Zone& zone : model.zones) {
        if (value("hit/" + zone.slot) == "true") {
            zone.state = ZoneState::crossed;
        } else if (value("armed/" + zone.slot) == "true") {
            zone.state = ZoneState::armed;
        }
    }
    if (model.active >= 0) {
        for (const Line& end : model.steps[static_cast<std::size_t>(model.active)].ends) {
            for (Zone& zone : model.zones) {
                if (zone.slot == end.zone && zone.state != ZoneState::crossed) {
                    zone.state = ZoneState::expected;
                }
            }
        }
    }
}

/** @return A fresh model of the first open program, or null when none is open. */
[[nodiscard]] std::shared_ptr<const Model> build() {
    DevView view{};
    if (!dev_view(view)) {
        return nullptr;
    }
    auto model = std::make_shared<Model>();
    model->binding = view.binding;
    model->lastError = view.lastError;
    model->attemptGeneration = view.attemptGeneration;
    model->running = view.running;
    model->faulted = view.faulted;
    model->commandPending = view.commandPending;
    parse(view.description, *model);
    apply_state(view.variables, *model);
    return model;
}

} // namespace

std::shared_ptr<const Model> current() noexcept {
    const std::uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    if (g_builtAt == 0 || now - g_builtAt >= kRefreshMs) {
        g_builtAt = now;
        try {
            g_model = build();
        } catch (const std::bad_alloc&) {
            g_model = nullptr;
        }
    }
    std::shared_ptr<const Model> model = g_model;
    ReleaseSRWLockExclusive(&g_lock);
    return model;
}

std::vector<Role> roles(const Model& model, int index) {
    std::vector<Role> output(model.zones.size(), Role::none);
    if (index < 0 || static_cast<std::size_t>(index) >= model.steps.size()) {
        return output;
    }
    const auto mark = [&](const std::string& slot, Role role) {
        for (std::size_t zone = 0; zone < model.zones.size(); ++zone) {
            // An end outranks a start, and both outrank a use.
            if (model.zones[zone].slot == slot && !slot.empty()
                && static_cast<int>(role) > static_cast<int>(output[zone])) {
                output[zone] = role;
            }
        }
    };
    const Step& step = model.steps[static_cast<std::size_t>(index)];
    for (const Line& beat : step.beats) {
        mark(beat.zone, Role::used);
    }
    for (const Fight& fight : model.fights) {
        if (fight.after == step.id) {
            mark(fight.line.zone, Role::used);
        }
    }
    if (index > 0) {
        for (const Line& end : model.steps[static_cast<std::size_t>(index - 1)].ends) {
            mark(end.zone, Role::start);
        }
    }
    for (const Line& end : step.ends) {
        mark(end.zone, Role::end);
    }
    return output;
}

void set_focus(int index) noexcept {
    g_focus.store(index, std::memory_order_release);
}

int focus() noexcept {
    return g_focus.load(std::memory_order_acquire);
}

int shown_step(const Model& model) noexcept {
    const int selected = focus();
    if (selected >= 0 && static_cast<std::size_t>(selected) < model.steps.size()) {
        return selected;
    }
    return model.active;
}

void set_show_all(bool on) noexcept {
    g_showAll.store(on, std::memory_order_release);
}

bool show_all() noexcept {
    return g_showAll.load(std::memory_order_acquire);
}

const Zone* find_zone(const Model& model, std::string_view slot) noexcept {
    for (const Zone& zone : model.zones) {
        if (zone.slot == slot) {
            return &zone;
        }
    }
    return nullptr;
}

std::string human(std::string_view name) {
    constexpr std::array<std::string_view, 8> kPrefixes{
        "pt_", "ho_", "sc_", "of_", "ap_", "o_", "tv_", "cue_"};
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                                       : character);
    });
    for (const std::string_view prefix : kPrefixes) {
        if (lower.starts_with(prefix) && lower.size() > prefix.size()) {
            lower.erase(0, prefix.size());
            break;
        }
    }
    std::replace(lower.begin(), lower.end(), '_', ' ');
    return lower;
}

} // namespace sunrise::server::activity::mission::dev_model
