#include "mission_editor_view.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

#include "../../../client/hooks/freecam/freecam.h"
#include "../../../client/hooks/teleport/runtime.h"
#include "../../activity/mission/mission_dev_model.h"
#include "../../activity/mission/mission_script_runtime.h"
#include "mission_editor_document.h"

namespace sunrise::server::ui::mission_editor::view {
namespace {

namespace freecam = client::hooks::freecam;
namespace mission = server::activity::mission;
namespace model = server::activity::mission::dev_model;
namespace teleport = client::hooks::teleport;
using document::Value;

constexpr ImVec4 kDone{0.55F, 0.56F, 0.62F, 1.0F};
constexpr ImVec4 kActive{0.31F, 1.0F, 0.47F, 1.0F};
constexpr ImVec4 kStart{1.0F, 0.82F, 0.27F, 1.0F};
constexpr ImVec4 kUsed{0.35F, 0.67F, 1.0F, 1.0F};
constexpr ImVec4 kWarn{1.0F, 0.66F, 0.35F, 1.0F};
constexpr ImVec4 kBad{1.0F, 0.45F, 0.45F, 1.0F};

/** One thing a picker offers: the name the data stores, what a person reads, and a hint. */
struct Choice final {
    std::string value{};
    std::string label{};
    std::string detail{};
};

/** What the SDK offers, read once from the mission's catalog.lua. */
struct Catalog final {
    std::vector<Choice> triggers{};
    std::vector<Choice> monitors{};
    std::vector<Choice> objects{};
    std::vector<Choice> anchors{};
    std::vector<Choice> effects{};
    std::vector<Choice> filters{};
    std::vector<Choice> cinematics{};
    std::vector<Choice> sceneSlots{};
    std::vector<Choice> scenes{};
    std::vector<Choice> cues{};
    std::vector<Choice> directives{};
    std::vector<Choice> squads{};
    std::vector<Choice> states{};
    std::vector<Choice> spawnSets{};
    std::vector<Choice> groups{};
    std::vector<Choice> objectives{};
    /** Each scene's authored event keys, in hexadecimal, in authored order. */
    std::vector<std::pair<std::string, std::vector<std::string>>> sceneKeys{};
    /** Each scene's squads, so a scene's cells can be bound at once. */
    std::vector<std::pair<std::string, std::vector<std::string>>> sceneSquads{};
    std::vector<std::string> cells{};
    bool loaded{};
};

// Editor state. Everything here runs on the editor window's thread, under the interface lock.
std::string g_status{};
document::Document g_document{};
std::string g_documentError{};
Catalog g_catalog{};
std::uint32_t g_documentRow{};
bool g_dirty{};
int g_selected{-1};
/** The event being edited, or -1; an event and a sequence are never selected together. */
int g_selectedEvent{-1};
std::array<char, 128> g_filter{};
/** Replays play one sequence alone: the chain stops once it has ended. */
bool g_solo{};
ImFont* g_heading{};
float g_scale{1.0F};

// ------------------------------------------------------------------------------------ design

[[nodiscard]] ImU32 color_u32(const ImVec4& color, float alpha = 1.0F) {
    return ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, color.w * alpha});
}

/** A line of text in the heading font. */
void heading(const char* text, float size = 19.0F) {
    ImGui::PushFont(g_heading, size * g_scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

/** A rounded label on a tinted background, laid out as one item. */
void pill(const char* text, const ImVec4& color) {
    const ImVec2 padding{8.0F * g_scale, 2.0F * g_scale};
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 end{at.x + size.x + padding.x * 2.0F, at.y + size.y + padding.y * 2.0F};
    ImDrawList* const list = ImGui::GetWindowDrawList();
    list->AddRectFilled(at, end, color_u32(color, 0.18F), (end.y - at.y) * 0.5F);
    list->AddText({at.x + padding.x, at.y + padding.y}, color_u32(color), text);
    ImGui::Dummy({end.x - at.x, end.y - at.y});
}

/** A button in the accent colour, for the action a panel is there for. */
bool primary_button(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.17F, 0.34F, 0.58F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.23F, 0.43F, 0.71F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.13F, 0.27F, 0.47F, 1.0F});
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return pressed;
}

/** A button that shows whether its mode is on. */
bool toggle_button(const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.12F, 0.42F, 0.25F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.16F, 0.52F, 0.31F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.10F, 0.34F, 0.20F, 1.0F});
    }
    const bool pressed = ImGui::Button(label);
    if (on) {
        ImGui::PopStyleColor(3);
    }
    return pressed;
}

/** Where the open card started and its right edge; cards do not nest. */
ImVec2 g_cardStart{};
float g_cardRight{};

/**
 * Opens a raised card; `end_card` closes it. A card is a group with a panel drawn behind it, not a
 * child window: a child window sized to its content is capped at the visible height, which cut a
 * long card off and stopped the scroll halfway.
 * @return Always true; the pair keeps the shape of a Begin/End call.
 */
bool begin_card(const char* id) {
    ImGui::PushID(id);
    ImDrawList* const list = ImGui::GetWindowDrawList();
    list->ChannelsSplit(2);
    list->ChannelsSetCurrent(1);
    g_cardStart = ImGui::GetCursorScreenPos();
    g_cardRight = g_cardStart.x + ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({0.0F, 8.0F * g_scale});
    ImGui::Indent(14.0F * g_scale);
    ImGui::BeginGroup();
    return true;
}

void end_card() {
    ImGui::EndGroup();
    ImGui::Unindent(14.0F * g_scale);
    ImGui::Dummy({0.0F, 6.0F * g_scale});
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ImVec2 end{g_cardRight, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y};
    list->ChannelsSetCurrent(0);
    const float rounding = 8.0F * g_scale;
    list->AddRectFilled(g_cardStart, end, ImGui::GetColorU32(ImGuiCol_ChildBg), rounding);
    list->AddRect(g_cardStart, end, ImGui::GetColorU32(ImGuiCol_Border), rounding);
    list->ChannelsMerge();
    ImGui::PopID();
    ImGui::Dummy({0.0F, 4.0F * g_scale});
}

/**
 * A heading that folds what follows it. ImGui keeps each one's state by its id, so a label with a
 * changing text names its id after `###`.
 * @return True when it is open.
 */
bool fold(const char* title, float size = 17.0F) {
    ImGui::PushFont(g_heading, size * g_scale);
    const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen
                                                   | ImGuiTreeNodeFlags_NoTreePushOnOpen);
    ImGui::PopFont();
    return open;
}

// ------------------------------------------------------------------------------------ helpers

[[nodiscard]] std::string upper(std::string_view text) {
    std::string output(text);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return output;
}

[[nodiscard]] bool contains_folded(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto folded = [](unsigned char character) { return std::tolower(character); };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return folded(a) == folded(b); })
           != text.end();
}

[[nodiscard]] std::string shorten(std::string_view text, std::size_t width = 60) {
    return text.size() <= width ? std::string(text) : std::string(text.substr(0, width)) + "...";
}

[[nodiscard]] std::string string_of(const Value* holder, std::string_view key) {
    return holder == nullptr ? std::string{} : std::string(holder->text_of(key));
}

void put_string(Value& holder, std::string_view key, const std::string& text) {
    holder.set(key, text.empty() ? Value{} : Value::of(text));
}

[[nodiscard]] std::vector<std::string> strings_of(const Value* list) {
    std::vector<std::string> output{};
    if (list == nullptr) {
        return output;
    }
    if (list->is_string()) {
        output.push_back(list->text);
        return output;
    }
    for (const Value& item : list->items) {
        if (item.is_string()) {
            output.push_back(item.text);
        } else if (item.is_table()) {
            output.emplace_back(item.text_of("cue"));
        }
    }
    return output;
}

[[nodiscard]] Value list_of(const std::vector<std::string>& values) {
    Value output = Value::table();
    for (const std::string& value : values) {
        output.items.push_back(Value::of(value));
    }
    return output;
}

[[nodiscard]] bool flag(const Value& holder, std::string_view key) {
    const Value* const value = holder.get(key);
    return value != nullptr && value->kind == Value::Kind::boolean && value->boolean;
}

[[nodiscard]] std::int64_t integer_of(const Value* value) {
    return value != nullptr && value->kind == Value::Kind::number
               ? static_cast<std::int64_t>(value->number)
               : 0;
}

void changed() {
    g_dirty = true;
}

/** Edits one text field. @return True when it changed. */
bool edit_text(const char* label, std::string& text, bool multiline = false) {
    std::array<char, 2048> buffer{};
    const std::size_t length = (std::min)(text.size(), buffer.size() - 1);
    std::copy_n(text.begin(), length, buffer.begin());
    const bool edited = multiline
                            ? ImGui::InputTextMultiline(label, buffer.data(), buffer.size(),
                                                        {-1.0F, ImGui::GetTextLineHeight() * 3.0F})
                            : ImGui::InputText(label, buffer.data(), buffer.size());
    if (edited) {
        text = buffer.data();
    }
    return edited;
}

/**
 * Keeps an id to what the runtime takes in a mission variable name, which the kit builds from it:
 * letters, digits, `_`, `-`, `.` and `/`. A space becomes `_`; anything else is dropped.
 */
[[nodiscard]] std::string clean_id(std::string_view text) {
    std::string output{};
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '_' || character == '-' || character == '.'
            || character == '/') {
            output.push_back(character);
        } else if (character == ' ') {
            output.push_back('_');
        }
    }
    return output;
}

/** Edits an id field, cleaned as it is typed. @return True when it changed. */
bool edit_id(std::string& id) {
    if (!edit_text("id", id)) {
        return false;
    }
    id = clean_id(id);
    return true;
}

/** A button naming the current choice; it opens a searchable list. @return True when it changed. */
bool pick(const char* id, std::string& value, const std::vector<Choice>& choices,
          const char* empty = "(none)") {
    ImGui::PushID(id);
    const auto current = std::find_if(choices.begin(), choices.end(),
                                      [&](const Choice& choice) { return choice.value == value; });
    const std::string shown = value.empty() ? std::string(empty)
                              : current != choices.end() ? current->label
                                                         : value + " (unknown)";
    bool picked = false;
    if (ImGui::Button((shorten(shown, 48) + "##button").c_str())) {
        g_filter = {};
        ImGui::OpenPopup("choices");
    }
    if (ImGui::IsItemHovered() && current != choices.end() && !current->detail.empty()) {
        ImGui::SetTooltip("%s\n%s", current->value.c_str(), current->detail.c_str());
    }
    if (ImGui::BeginPopup("choices")) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(560.0F);
        ImGui::InputTextWithHint("##filter", "search...", g_filter.data(), g_filter.size());
        if (ImGui::BeginChild("##list", {560.0F, 380.0F})) {
            if (ImGui::Selectable(empty)) {
                value.clear();
                picked = true;
                ImGui::CloseCurrentPopup();
            }
            const std::string_view needle(g_filter.data());
            for (const Choice& choice : choices) {
                if (!contains_folded(choice.label, needle) && !contains_folded(choice.value, needle)
                    && !contains_folded(choice.detail, needle)) {
                    continue;
                }
                ImGui::PushID(choice.value.c_str());
                if (ImGui::Selectable(shorten(choice.label, 90).c_str(), choice.value == value)) {
                    value = choice.value;
                    picked = true;
                    ImGui::CloseCurrentPopup();
                }
                if (!choice.detail.empty()) {
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", shorten(choice.detail, 110).c_str());
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

/** A small button that asks for a removal. */
[[nodiscard]] bool remove_button(const char* id) {
    ImGui::SameLine();
    ImGui::PushID(id);
    const bool pressed = ImGui::SmallButton("x");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remove");
    }
    ImGui::PopID();
    return pressed;
}

// ------------------------------------------------------------------------ catalog and file

/** Adds every slot of one type to a choice list; `at` becomes a position hint. */
void add_slots(const Value& slots, std::string_view type, std::vector<Choice>& output) {
    const Value* const list = slots.get(type);
    if (list == nullptr) {
        return;
    }
    for (const Value& slot : list->items) {
        Choice choice{};
        choice.value = slot.text_of("name");
        choice.label = model::human(slot.text_of("label").empty() ? slot.text_of("name")
                                                                  : slot.text_of("label"));
        if (const Value* const at = slot.get("at"); at != nullptr && at->items.size() == 3) {
            std::array<char, 64> text{};
            std::snprintf(text.data(), text.size(), "at %.0f, %.0f, %.0f", at->items[0].number,
                          at->items[1].number, at->items[2].number);
            choice.detail = text.data();
        }
        output.push_back(std::move(choice));
    }
}

void build_catalog(const Value& root) {
    g_catalog = {};
    if (const Value* const slots = root.get("slots")) {
        add_slots(*slots, "31", g_catalog.triggers);
        add_slots(*slots, "30", g_catalog.monitors);
        add_slots(*slots, "4", g_catalog.objects);
        add_slots(*slots, "47", g_catalog.anchors);
        add_slots(*slots, "4", g_catalog.anchors);
        add_slots(*slots, "26", g_catalog.effects);
        add_slots(*slots, "34", g_catalog.filters);
        add_slots(*slots, "6", g_catalog.cinematics);
        add_slots(*slots, "43", g_catalog.sceneSlots);
        add_slots(*slots, "3", g_catalog.objectives);
        if (const Value* const cells = slots->get("2")) {
            for (const Value& cell : cells->items) {
                g_catalog.cells.emplace_back(cell.text_of("name"));
            }
        }
    }
    if (const Value* const scenes = root.get("scenes")) {
        for (const Value& scene : scenes->items) {
            std::vector<std::string> squads = strings_of(scene.get("squads"));
            std::string detail{};
            for (const std::string& squad : squads) {
                detail += (detail.empty() ? "spawns " : ", ") + model::human(squad);
            }
            g_catalog.scenes.push_back({std::string(scene.text_of("name")),
                                        model::human(scene.text_of("name")), detail});
            g_catalog.sceneKeys.emplace_back(std::string(scene.text_of("name")),
                                             strings_of(scene.get("keys")));
            g_catalog.sceneSquads.emplace_back(std::string(scene.text_of("name")), std::move(squads));
        }
    }
    if (const Value* const cues = root.get("cues")) {
        for (const Value& cue : cues->items) {
            if (cue.text_of("text").empty()) {
                continue;
            }
            std::array<char, 48> detail{};
            std::snprintf(detail.data(), detail.size(), "%s, %.1f s",
                          std::string(cue.text_of("name")).c_str(),
                          static_cast<double>(integer_of(cue.get("duration_ms"))) / 1000.0);
            g_catalog.cues.push_back({std::string(cue.text_of("name")),
                                      "\"" + std::string(cue.text_of("text")) + "\"", detail.data()});
        }
    }
    if (const Value* const directives = root.get("directives")) {
        for (const Value& directive : directives->items) {
            g_catalog.directives.push_back({std::string(directive.text_of("name")),
                                            std::string(directive.text_of("title")),
                                            std::string(directive.text_of("description"))});
        }
    }
    if (const Value* const squads = root.get("squads")) {
        for (const Value& squad : squads->items) {
            g_catalog.squads.push_back({std::string(squad.text_of("name")),
                                        model::human(squad.text_of("name")),
                                        std::to_string(integer_of(squad.get("members"))) + " members"});
        }
    }
    if (const Value* const states = root.get("states")) {
        for (const Value& state : states->items) {
            const std::int64_t ordinal = integer_of(state.get("ordinal"));
            g_catalog.states.push_back(
                {std::string(state.text_of("name")),
                 "region " + std::to_string(integer_of(state.get("region")))
                     + (ordinal != 0 ? " (cinematic " + std::to_string(ordinal) + ")" : ""),
                 std::string(state.text_of("name"))});
        }
    }
    if (const Value* const sets = root.get("spawn_sets")) {
        for (const Value& set : sets->items) {
            std::string label = std::string(set.text_of("hash"));
            if (const Value* const closest = set.get("near"); closest != nullptr && closest->is_table()) {
                label = "near " + model::human(closest->text_of("trigger")) + " ("
                        + std::to_string(integer_of(closest->get("metres"))) + " m)";
            }
            g_catalog.spawnSets.push_back({std::string(set.text_of("hash")), label,
                                           std::string(set.text_of("hash"))});
        }
    }
    if (const Value* const groups = root.get("task_groups")) {
        for (const auto& [objective, list] : groups->fields) {
            g_catalog.groups.push_back({objective, model::human(objective), "every group"});
            for (const Value& group : list.items) {
                if (group.is_string()) {
                    g_catalog.groups.push_back({objective + "." + group.text,
                                                model::human(objective) + " / " + group.text, ""});
                }
            }
        }
    }
    g_catalog.loaded = true;
}

/** @return The folder the running mission's script lives in, or empty. */
[[nodiscard]] std::wstring mission_folder(std::uint32_t row) {
    std::array<char, 260> controller{};
    std::wstring root{};
    if (row == 0 || !mission::controller_file_name(row, controller) || !mission::script_root(root)) {
        return {};
    }
    const std::string file(controller.data());
    const std::string stem = file.substr(0, file.find('/'));
    return root + L"\\" + std::wstring(stem.begin(), stem.end());
}

[[nodiscard]] Value* document_events();

/** How often an unedited document looks at its file for a change made elsewhere. */
constexpr std::uint64_t kDiskCheckMs = 1000;
std::uint64_t g_diskCheckedAt{};

/**
 * Loads the running mission's data.lua and catalog.lua once per mission, and again when the file
 * changes on disk while nothing is edited here: a stale copy saved later would undo that change.
 */
void ensure_document(std::uint32_t row) {
    if (row == g_documentRow && (g_document.loaded || !g_documentError.empty())) {
        const std::uint64_t now = GetTickCount64();
        if (g_dirty || !g_document.loaded || now - g_diskCheckedAt < kDiskCheckMs) {
            return;
        }
        g_diskCheckedAt = now;
        if (!document::changed_on_disk(g_document)) {
            return;
        }
        g_status = "data.lua changed on disk: read again";
    }
    g_documentRow = row;
    g_document = {};
    g_documentError.clear();
    g_catalog = {};
    g_dirty = false;
    const std::wstring folder = mission_folder(row);
    if (folder.empty()) {
        g_documentError = "no mission script is open";
        return;
    }
    std::string error{};
    if (!document::load(folder + L"\\data.lua", g_document, error)) {
        g_documentError = "this mission is not kept as data (data.lua: " + error + ")";
        return;
    }
    // Created now, so a pointer into the document never moves when the list is first needed.
    (void)document_events();
    document::Document catalog{};
    if (document::load(folder + L"\\catalog.lua", catalog, error)) {
        build_catalog(catalog.root);
    } else {
        g_status = "catalog.lua is missing: the pickers are empty (regenerate it with the tool)";
    }
}

[[nodiscard]] Value* document_steps() {
    return g_document.loaded ? g_document.root.get("steps") : nullptr;
}

/** @return The mission's events (the kit's encounters), created empty when missing; null unloaded. */
[[nodiscard]] Value* document_events() {
    if (!g_document.loaded) {
        return nullptr;
    }
    Value& events = g_document.root.at("encounters");
    if (!events.is_table()) {
        events = Value::table();
    }
    return &events;
}

/** @return The zone of the running model with this data name (PT_X), or null. */
[[nodiscard]] const model::Zone* zone_named(const model::Model& current, std::string_view name) {
    for (const model::Zone& zone : current.zones) {
        if (!name.empty() && upper(zone.name) == name) {
            return &zone;
        }
    }
    return nullptr;
}

/** @return Where an event starts, written for a person. */
[[nodiscard]] std::string event_start(const Value& event) {
    const std::string after(event.text_of("after"));
    std::string text = after.empty() || after == "arrival" ? "at mission start"
                                                           : "after sequence " + model::human(after);
    const std::string trigger(event.text_of("trigger"));
    if (!trigger.empty()) {
        text += ", when the player reaches " + model::human(trigger);
    }
    return text;
}

/** @return True for an event in the older form, which starts on its own after a sequence. */
[[nodiscard]] bool starts_itself(const Value& event) {
    return event.get("after") != nullptr || event.get("trigger") != nullptr
           || event.get("monitor") != nullptr;
}

/** Calls `visit` on every table that can start an event: sequences, events and their steps. */
template <typename Visit>
void each_holder(Visit&& visit) {
    Value* const events = document_events();
    for (Value* const list : {document_steps(), events}) {
        if (list == nullptr) {
            continue;
        }
        for (Value& holder : list->items) {
            visit(holder);
            if (Value* const items = holder.get("sequence")) {
                for (Value& item : items->items) {
                    visit(item);
                }
            }
        }
    }
}

/** Renames one event wherever a step starts or clears it; an empty name removes it there. */
void rename_event(std::string_view from, std::string_view to) {
    each_holder([&](Value& holder) {
        for (const char* key : {"events", "cleared"}) {
            const Value* const list = holder.get(key);
            if (list == nullptr) {
                continue;
            }
            const bool single = list->is_string();
            std::vector<std::string> kept{};
            bool touched = false;
            for (const std::string& id : strings_of(list)) {
                if (id != from) {
                    kept.push_back(id);
                    continue;
                }
                touched = true;
                if (!to.empty()) {
                    kept.emplace_back(to);
                }
            }
            if (!touched) {
                continue;
            }
            if (kept.empty()) {
                holder.erase(key);
            } else if (single && kept.size() == 1) {
                holder.set(key, Value::of(kept[0]));
            } else {
                holder.set(key, list_of(kept));
            }
        }
    });
}

/** One place that starts an event: a sequence or an event, when it starts or at one of its steps. */
struct Reference final {
    std::string owner{};
    std::size_t index{};
    bool event{};
    /** The step, or -1 for the holder's own start. */
    int step{-1};
    /** The zone that step waits on, if any. */
    std::string on{};
};

/** @return Every place that starts the event, sequences first. */
[[nodiscard]] std::vector<Reference> references(std::string_view id) {
    std::vector<Reference> output{};
    const auto starts = [&](const Value& holder) {
        const std::vector<std::string> ids = strings_of(holder.get("events"));
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    const auto scan = [&](const Value* list, bool event) {
        if (list == nullptr) {
            return;
        }
        for (std::size_t index = 0; index < list->items.size(); ++index) {
            const Value& holder = list->items[index];
            const Reference base{std::string(holder.text_of("id")), index, event};
            if (starts(holder)) {
                output.push_back(base);
            }
            if (const Value* const items = holder.get("sequence")) {
                for (std::size_t step = 0; step < items->items.size(); ++step) {
                    if (starts(items->items[step])) {
                        Reference reference = base;
                        reference.step = static_cast<int>(step);
                        reference.on = items->items[step].text_of("on");
                        output.push_back(std::move(reference));
                    }
                }
            }
        }
    };
    const Value* const events = document_events();
    scan(document_steps(), false);
    scan(events, true);
    return output;
}

[[nodiscard]] std::string reference_text(const Reference& reference) {
    std::string text = (reference.event ? "event " : "sequence ") + model::human(reference.owner);
    text += reference.step < 0 ? ", when it starts" : ", step " + std::to_string(reference.step + 1);
    if (!reference.on.empty()) {
        text += " (reaches " + model::human(reference.on) + ")";
    }
    return text;
}

/**
 * Moves an event of the older form into the sequence it waited on: a step that waits on its zone
 * and starts it, or the sequence's own start when it had no zone.
 * @return What was done, for the status line.
 */
std::string move_into_sequence(Value& event) {
    Value* const steps = document_steps();
    if (steps == nullptr || steps->items.empty()) {
        return "there is no sequence to move it into";
    }
    const std::string after(event.text_of("after"));
    Value* target = nullptr;
    for (Value& step : steps->items) {
        if (step.text_of("id") == after) {
            target = &step;
        }
    }
    if (after.empty() || after == "arrival") {
        target = &steps->items[0];
    }
    if (target == nullptr) {
        return "its sequence " + after + " does not exist";
    }
    std::string zone(event.text_of("trigger"));
    if (zone.empty()) {
        zone = event.text_of("monitor");
    }
    const std::string id(event.text_of("id"));
    const auto add_to = [&](Value& holder) {
        std::vector<std::string> ids = strings_of(holder.get("events"));
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
        holder.set("events", list_of(ids));
    };
    std::string text = "moved " + model::human(id) + " into " + model::human(target->text_of("id"));
    if (zone.empty()) {
        add_to(*target);
        text += ", when it starts";
    } else {
        Value& items = target->at("sequence");
        if (!items.is_table()) {
            items = Value::table();
        }
        Value* found = nullptr;
        for (Value& item : items.items) {
            if (item.text_of("on") == zone && item.get("events") != nullptr) {
                found = &item;
            }
        }
        if (found == nullptr) {
            Value item = Value::table();
            item.set("on", Value::of(zone));
            items.items.push_back(std::move(item));
            found = &items.items.back();
        }
        add_to(*found);
        text += ", as a step on " + model::human(zone);
        if (const Value* const ends = target->get("ends"); ends != nullptr && flag(*ends, "sequence")) {
            text += "; this sequence ends once its steps have played, so it now waits for that zone too";
        }
    }
    event.erase("after");
    event.erase("trigger");
    event.erase("monitor");
    changed();
    return text;
}

/** @return Every event, for the pickers. */
[[nodiscard]] std::vector<Choice> event_choices() {
    std::vector<Choice> output{};
    if (const Value* const events = document_events()) {
        for (const Value& event : events->items) {
            const std::string id(event.text_of("id"));
            output.push_back({id, model::human(id), shorten(event.text_of("comment"), 100)});
        }
    }
    return output;
}

/** @return What one step waits on and does, in a few words, for its folded header. */
[[nodiscard]] std::string moment_summary(const Value& item) {
    std::string text{};
    const auto add = [&](const std::string& part) {
        text += (text.empty() ? "" : ", ") + part;
    };
    if (const std::string on(item.text_of("on")); !on.empty()) {
        add("reaches " + model::human(on));
    } else if (const std::string scene(item.text_of("finished")); !scene.empty()) {
        add("once " + model::human(scene) + " finishes");
    } else if (const std::string object(item.text_of("interacted")); !object.empty()) {
        add("once " + model::human(object) + " is used");
    } else if (const std::string cue(item.text_of("spoken")); !cue.empty()) {
        add("once " + cue + " is said");
    } else if (item.get("cleared") != nullptr) {
        add("once cleared");
    }
    if (const std::int64_t ms = integer_of(item.get("after_ms")); ms > 0) {
        std::array<char, 32> delay{};
        std::snprintf(delay.data(), delay.size(), "%.1f s later", static_cast<double>(ms) / 1000.0);
        add(delay.data());
    }
    std::string does{};
    const auto act = [&](const std::string& part) {
        does += (does.empty() ? "" : ", ") + part;
    };
    for (const std::string& id : strings_of(item.get("events"))) {
        act("starts " + model::human(id));
    }
    if (const Value* const scenes = item.get("scenes")) {
        for (const Value& entry : scenes->items) {
            act("plays " + model::human(entry.text_of("scene")));
        }
    }
    const auto effect_word = [&](const Value& effect) {
        act((effect.get("enabled") != nullptr && !flag(effect, "enabled") ? "removes " : "applies ")
            + model::human(effect.text_of("slot")));
    };
    if (const Value* const effect = item.get("effect")) {
        effect_word(*effect);
    }
    if (const Value* const effects = item.get("effects")) {
        for (const Value& effect : effects->items) {
            effect_word(effect);
        }
    }
    if (const std::size_t lines = strings_of(item.get("lines")).size(); lines > 0) {
        act("says " + std::to_string(lines) + (lines == 1 ? " line" : " lines"));
    }
    if (item.get("stop") != nullptr) {
        act("stops scenes");
    }
    if (item.get("land") != nullptr) {
        act("moves the player");
    }
    if (text.empty()) {
        text = "right away";
    }
    return shorten(text + (does.empty() ? "" : ":  " + does), 110);
}

/**
 * Gives the first leg every zone the mission waits on that no leg arms or watches yet: the kit
 * refuses a zone no leg arms. A revisit sequence's own end zones stay unarmed, as the kit wants.
 * @return How many zones were added.
 */
std::size_t arm_used_zones() {
    Value* const legs = g_document.root.get("legs");
    if (legs == nullptr || legs->items.empty()) {
        return 0;
    }
    std::vector<std::string> known{};
    for (const Value& leg : legs->items) {
        for (const char* key : {"arm", "watch"}) {
            for (const std::string& zone : strings_of(leg.get(key))) {
                known.push_back(zone);
            }
        }
    }
    std::vector<std::string> used{};
    if (const Value* const steps = document_steps()) {
        for (const Value& step : steps->items) {
            if (const Value* const ends = step.get("ends"); ends != nullptr && !flag(step, "revisit")) {
                for (const std::string& zone : strings_of(ends->get("trigger"))) {
                    used.push_back(zone);
                }
            }
        }
    }
    each_holder([&](Value& holder) {
        for (const char* key : {"on", "trigger", "monitor"}) {
            if (const std::string zone(holder.text_of(key)); !zone.empty()) {
                used.push_back(zone);
            }
        }
    });
    const auto listed = [](const std::vector<Choice>& choices, const std::string& name) {
        return std::any_of(choices.begin(), choices.end(),
                           [&](const Choice& choice) { return choice.value == name; });
    };
    Value& first = legs->items[0];
    std::size_t added = 0;
    for (const std::string& zone : used) {
        if (std::find(known.begin(), known.end(), zone) != known.end()) {
            continue;
        }
        // A monitor is watched; anything else, a trigger, is armed.
        const char* const key = listed(g_catalog.monitors, zone) ? "watch" : "arm";
        std::vector<std::string> list = strings_of(first.get(key));
        list.push_back(zone);
        first.set(key, list_of(list));
        known.push_back(zone);
        ++added;
    }
    return added;
}

/** Saves the file; with `step`, reloads the script and replays that step in the new one. */
/**
 * Finds a choice left empty, such as a step that waits for a scene with no scene picked: the kit
 * refuses the whole script for it.
 * @return Where it is, written for a person, or empty when every choice is made.
 */
[[nodiscard]] std::string find_blank(const Value& value, const std::string& where) {
    if (value.is_string()) {
        return value.text.empty() ? where : std::string{};
    }
    if (!value.is_table()) {
        return {};
    }
    for (const auto& [key, field] : value.fields) {
        if (key == "comment") {
            continue;
        }
        if (key == "signal") {
            const Value* const keys = field.get("keys");
            if (keys == nullptr || keys->items.empty()) {
                return where + ": keys to send";
            }
        }
        const bool steps = key == "sequence" && field.is_table();
        if (steps) {
            for (std::size_t index = 0; index < field.items.size(); ++index) {
                if (std::string found = find_blank(field.items[index], where + ", step " + std::to_string(index + 1));
                    !found.empty()) {
                    return found;
                }
            }
            continue;
        }
        if (std::string found = find_blank(field, where + ": " + key); !found.empty()) {
            return found;
        }
    }
    for (const Value& item : value.items) {
        if (std::string found = find_blank(item, where); !found.empty()) {
            return found;
        }
    }
    return {};
}

void save(const std::string& step) {
    for (const auto& [list, word] : {std::pair{"steps", "sequence "}, std::pair{"encounters", "event "}}) {
        if (const Value* const holders = g_document.root.get(list)) {
            for (const Value& holder : holders->items) {
                if (const std::string blank =
                        find_blank(holder, word + model::human(holder.text_of("id")));
                    !blank.empty()) {
                    g_status = "not saved: nothing chosen in " + blank;
                    return;
                }
            }
        }
    }
    const std::size_t armed = arm_used_zones();
    std::string error{};
    if (!document::save(g_document, error)) {
        g_status = "save failed: " + error;
        return;
    }
    g_dirty = false;
    const std::string arming =
        armed == 0 ? std::string{} : " (" + std::to_string(armed) + " new zones armed on the first leg)";
    if (step.empty()) {
        g_status = "saved" + arming;
        return;
    }
    if (!mission::reload()) {
        g_status = "saved, but the reload was refused" + arming;
        return;
    }
    if (step == "-") {
        g_status = "saved and reloading: progress is kept" + arming;
        return;
    }
    g_status = (mission::queue_dev_command("replay", step, g_solo ? "solo" : "")
                    ? "saved, reloading, replaying " + step
                    : "saved and reloading; a command was already waiting")
               + arming;
}

// ------------------------------------------------------------------------------ zones

[[nodiscard]] std::array<float, 3> centre(const model::Zone& zone) {
    return {(zone.minimum[0] + zone.maximum[0]) * 0.5F, (zone.minimum[1] + zone.maximum[1]) * 0.5F,
            (zone.minimum[2] + zone.maximum[2]) * 0.5F};
}

/** @return The zone whose centre is nearest the spectator camera, else the game camera. */
[[nodiscard]] const model::Zone* nearest_zone(const model::Model& current) {
    std::array<float, 3> from{};
    teleport::CameraPose pose{};
    if (!freecam::position(from)) {
        if (!teleport::camera_pose(pose)) {
            return nullptr;
        }
        from = pose.position;
    }
    const model::Zone* best = nullptr;
    float bestDistance = 0.0F;
    for (const model::Zone& zone : current.zones) {
        if (!zone.bounded) {
            continue;
        }
        const std::array<float, 3> at = centre(zone);
        const float distance = std::hypot(at[0] - from[0], at[1] - from[1], at[2] - from[2]);
        if (best == nullptr || distance < bestDistance) {
            best = &zone;
            bestDistance = distance;
        }
    }
    return best;
}

void zone_row(const model::Zone& zone, const char* role, const ImVec4& color) {
    ImGui::PushID(zone.slot.c_str());
    ImGui::AlignTextToFramePadding();
    pill(role, color);
    ImGui::SameLine();
    ImGui::Text("%s", model::human(zone.name).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(zone.state == model::ZoneState::crossed  ? "crossed"
                        : zone.state == model::ZoneState::idle ? "not armed"
                                                               : "armed");
    const float buttons = 190.0F * g_scale;
    ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - buttons, ImGui::GetCursorPosX()));
    if (zone.bounded) {
        if (ImGui::SmallButton("Look")) {
            freecam::look_at(centre(zone));
            g_status = "spectator camera on " + model::human(zone.name);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fly the spectator camera to this zone. Nothing triggers.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Go")) {
            freecam::set_enabled(false);
            // A volume's floor often lies under the ground: drop in from above its centre.
            const std::array<float, 3> at = centre(zone);
            teleport::request_move_to({at[0], at[1], at[2] + 1.5F});
            g_status = "moving the player to " + model::human(zone.name);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Move the player into this zone. It reports as crossed.");
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Cross")) {
        g_status = mission::queue_dev_command("cross", {}, zone.slot) ? "crossing "
                                                                            + model::human(zone.name)
                                                                      : "a command is waiting";
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Report this zone as crossed, as the client does when the player enters it.");
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------- toolbar

void draw_toolbar(const model::Model* current) {
    if (!begin_card("##toolbar")) {
        end_card();
        return;
    }
    if (current == nullptr) {
        heading("No mission running", 24.0F);
        ImGui::TextDisabled("Start a mission in game; this window follows it.");
    } else {
        heading(current->title.c_str(), 24.0F);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0F * g_scale);
        pill(current->faulted ? "FAULTED" : current->running ? "RUNNING" : "STARTING",
             current->faulted ? kBad : current->running ? kActive : kWarn);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0F * g_scale);
        pill(("attempt " + std::to_string(current->attemptGeneration)).c_str(), kDone);
    }
    const bool flying = freecam::enabled();
    if (toggle_button(flying ? "Spectator camera: ON" : "Spectator camera", flying)) {
        freecam::set_enabled(!flying);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fly freely in the game window: W A S D, Space up, C down, Shift fast, Alt slow,\n"
                          "mouse to look, wheel for speed. The player stays still and nothing triggers.");
    }
    ImGui::SameLine();
    if (toggle_button(g_solo ? "One sequence at a time: ON" : "One sequence at a time", g_solo)) {
        g_solo = !g_solo;
        // On: the running sequence is the last to play. Off: the chain goes on.
        std::string last{};
        if (g_solo && current != nullptr && current->active >= 0) {
            last = current->steps[static_cast<std::size_t>(current->active)].id;
        }
        const bool queued = mission::queue_dev_command("solo", last, {});
        g_status = !queued  ? "a command is waiting: toggle again in a moment"
                   : g_solo ? (last.empty() ? "replays now play one sequence alone"
                                            : "the chain stops once " + model::human(last) + " has ended")
                            : "the chain goes on";
        if (!queued) {
            g_solo = !g_solo;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A replay plays only its sequence: its steps and the events they start. Once\n"
                          "it has ended, the next sequence does not start. Turn it off to go on.");
    }
    ImGui::SameLine();
    const bool everything = model::show_all();
    if (toggle_button(everything ? "All zones: ON" : "All zones", everything)) {
        model::set_show_all(!everything);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw every zone of the mission, not only the selected sequence's.");
    }
    ImGui::SameLine();
    if (g_document.loaded) {
        const bool save_now = g_dirty ? primary_button("Save") : ImGui::Button("Save");
        if (save_now || (g_dirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            save({});
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Writes every change to data.lua (Ctrl+S). The running script keeps\n"
                              "the old one until it reloads.");
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(g_dirty ? "Save and reload script" : "Reload script")) {
        if (g_dirty) {
            save("-");
        } else {
            g_status = mission::reload() ? "reloading: progress is kept" : "reload refused";
            g_documentRow = 0;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads the script from disk again. Progress and what is in the world stay.");
    }
    if (g_dirty) {
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            g_documentRow = 0;
            g_status = "changes dropped: data.lua read again";
        }
        ImGui::SameLine();
        pill("UNSAVED", kWarn);
    }
    if (current != nullptr && current->commandPending) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel waiting command")) {
            mission::cancel_dev_command();
        }
    }
    if (current != nullptr && current->faulted) {
        ImGui::TextColored(kBad, "Script fault: %s", current->lastError.data());
    }
    if (!g_status.empty()) {
        ImGui::TextDisabled("%s", g_status.c_str());
    }
    end_card();
}

// -------------------------------------------------------------------------- step list

/** @return The model's index of a step id, or -1. */
[[nodiscard]] int model_index(const model::Model* current, std::string_view id) {
    if (current == nullptr) {
        return -1;
    }
    for (std::size_t index = 0; index < current->steps.size(); ++index) {
        if (current->steps[index].id == id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void select(const model::Model* current, int index, std::string_view id) {
    g_selected = index;
    model::set_focus(model_index(current, id));
}

void draw_step_list(const model::Model* current) {
    const float rowHeight = ImGui::GetTextLineHeight() + 12.0F * g_scale;
    const auto row = [&](const char* id, const std::string& number, const std::string& name,
                         const ImVec4& dot, bool filled, bool selected) {
        const bool clicked = ImGui::Selectable(id, selected, 0, {0.0F, rowHeight});
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* const list = ImGui::GetWindowDrawList();
        const float middle = (min.y + max.y) * 0.5F;
        const ImVec2 centre{min.x + 12.0F * g_scale, middle};
        if (filled) {
            list->AddCircleFilled(centre, 4.5F * g_scale, color_u32(dot));
        } else {
            list->AddCircle(centre, 4.5F * g_scale, color_u32(dot), 0, 1.5F * g_scale);
        }
        const float textY = middle - ImGui::GetTextLineHeight() * 0.5F;
        list->AddText({min.x + 26.0F * g_scale, textY}, color_u32(kDone), number.c_str());
        list->AddText({min.x + 52.0F * g_scale, textY},
                      color_u32(filled && dot.x < 0.5F ? kActive : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                      name.c_str());
        return clicked;
    };
    const bool following = g_selected < 0 && g_selectedEvent < 0;
    if (row("##follow", "", "Follow the running sequence", kUsed, following, following)) {
        g_selected = -1;
        g_selectedEvent = -1;
        model::set_focus(-1);
    }
    ImGui::Dummy({0.0F, 4.0F * g_scale});
    Value* const events = document_events();
    Value* const steps = document_steps();
    if (fold("Sequences###sequences", 18.0F)) {
        const std::size_t count = steps != nullptr ? steps->items.size()
                                  : current != nullptr ? current->steps.size()
                                                       : 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::string id = steps != nullptr ? std::string(steps->items[index].text_of("id"))
                                                    : current->steps[index].id;
            const int shown = model_index(current, id);
            const bool active = current != nullptr && shown >= 0 && shown == current->active;
            const bool done = current != nullptr && shown >= 0
                              && current->steps[static_cast<std::size_t>(shown)].progress
                                     == model::Progress::done;
            const std::string name =
                model::human(id) + (shown < 0 && current != nullptr ? "  (unsaved)" : "");
            ImGui::PushID(static_cast<int>(index));
            if (row("##step", std::to_string(index + 1), name, active ? kActive : done ? kDone : kUsed,
                    active || done, g_selected == static_cast<int>(index))) {
                g_selectedEvent = -1;
                select(current, static_cast<int>(index), id);
            }
            ImGui::PopID();
        }
    }
    if (events == nullptr) {
        return;
    }
    ImGui::Dummy({0.0F, 8.0F * g_scale});
    const bool open = fold("Events###events", 18.0F);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ event")) {
        Value fresh = Value::table();
        fresh.set("id", Value::of("event_" + std::to_string(events->items.size() + 1)));
        events->items.push_back(std::move(fresh));
        g_selectedEvent = static_cast<int>(events->items.size()) - 1;
        g_selected = -1;
        changed();
    }
    const bool older = std::any_of(events->items.begin(), events->items.end(), starts_itself);
    if (older) {
        ImGui::SameLine();
        if (ImGui::SmallButton("move all into sequences")) {
            std::size_t moved = 0;
            for (Value& event : events->items) {
                if (starts_itself(event)) {
                    (void)move_into_sequence(event);
                    ++moved;
                }
            }
            g_status = "moved " + std::to_string(moved) + " events into their sequences as steps";
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Events that still start on their own after a sequence become steps of that\n"
                              "sequence: a step that waits on the event's zone and starts it.");
        }
    }
    if (!open) {
        return;
    }
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextDisabled("Chains of actions: squads, scenes, effects and their own steps. "
                        "A step of a sequence starts them.");
    ImGui::PopTextWrapPos();
    for (std::size_t index = 0; index < events->items.size(); ++index) {
        const Value& event = events->items[index];
        const std::vector<Reference> started = references(event.text_of("id"));
        const bool itself = starts_itself(event);
        const bool idle = started.empty() && !itself;
        ImGui::PushID(static_cast<int>(index) + 100000);
        if (row("##event", "", model::human(event.text_of("id")) + (idle ? "  (not started)" : ""),
                idle ? kDone : kWarn, !idle, g_selectedEvent == static_cast<int>(index))) {
            g_selectedEvent = static_cast<int>(index);
            g_selected = -1;
            model::set_focus(-1);
        }
        if (ImGui::IsItemHovered()) {
            if (itself) {
                ImGui::SetTooltip("Older form: starts on its own %s.", event_start(event).c_str());
            } else if (started.empty()) {
                ImGui::SetTooltip("No step starts it yet.");
            } else {
                ImGui::SetTooltip("Started by %s%s", reference_text(started[0]).c_str(),
                                  started.size() > 1 ? " and elsewhere" : "");
            }
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------- step overview

bool zone_pick(const char* id, std::string& value, const model::Model* current);
bool name_list(const char* id, Value& holder, std::string_view key, const std::vector<Choice>& choices,
               const char* add, const model::Model* current = nullptr, bool zones = false);

/**
 * The zones a sequence starts and ends on, as the data holds them: it starts where the sequence
 * before it ends, so its START zones are that sequence's end zones.
 */
void edit_start_end(const model::Model& current, std::string_view id) {
    Value* const steps = document_steps();
    if (steps == nullptr) {
        return;
    }
    std::size_t index = 0;
    while (index < steps->items.size() && steps->items[index].text_of("id") != id) {
        ++index;
    }
    if (index == steps->items.size()) {
        return;
    }
    const auto ends_of = [&](std::size_t at) -> Value& {
        Value& ends = steps->items[at].at("ends");
        if (!ends.is_table()) {
            ends = Value::table();
        }
        return ends;
    };
    if (!ImGui::TreeNodeEx("Change the START and END zones###start_end", 0)) {
        return;
    }
    ImGui::PushID("start_end");
    if (index > 0) {
        pill("START", kStart);
        ImGui::SameLine();
        ImGui::TextDisabled("where %s ends: the player reaches",
                            model::human(steps->items[index - 1].text_of("id")).c_str());
        ImGui::PushID("start");
        name_list("zones", ends_of(index - 1), "trigger", g_catalog.triggers, "+ START zone", &current,
                  true);
        ImGui::PopID();
    } else {
        pill("START", kStart);
        ImGui::SameLine();
        ImGui::TextDisabled("the first sequence starts when the player arrives");
    }
    pill("END", kActive);
    ImGui::SameLine();
    ImGui::TextDisabled("the player reaches");
    ImGui::PushID("end");
    name_list("zones", ends_of(index), "trigger", g_catalog.triggers, "+ END zone", &current, true);
    ImGui::PopID();
    ImGui::TextDisabled("Save and reload to see them in the world; the save arms new zones.");
    ImGui::PopID();
    ImGui::TreePop();
}

void draw_overview(const model::Model& current, int shown) {
    if (!begin_card("##overview")) {
        end_card();
        return;
    }
    if (shown < 0 || static_cast<std::size_t>(shown) >= current.steps.size()) {
        heading("In the world", 18.0F);
        ImGui::TextDisabled("This sequence is not in the running script yet: save and reload.");
        end_card();
        return;
    }
    const model::Step& step = current.steps[static_cast<std::size_t>(shown)];
    const std::vector<model::Role> roles = model::roles(current, shown);
    const bool open = fold(
        ("Sequence " + std::to_string(shown + 1) + "  " + model::human(step.id) + "###overview").c_str(),
        20.0F);
    if (shown == current.active) {
        ImGui::SameLine();
        pill("RUNNING", kActive);
    }
    if (!open) {
        end_card();
        return;
    }
    if (!step.goal.empty()) {
        ImGui::TextDisabled("Goal");
        ImGui::SameLine();
        ImGui::Text("%s", step.goal.c_str());
    }
    ImGui::Dummy({0.0F, 4.0F * g_scale});
    bool any = false;
    for (const model::Role role : {model::Role::start, model::Role::end, model::Role::used}) {
        for (std::size_t index = 0; index < current.zones.size(); ++index) {
            if (roles[index] == role) {
                any = true;
                zone_row(current.zones[index],
                         role == model::Role::start ? "START" : role == model::Role::end ? "END" : "USES",
                         role == model::Role::start ? kStart : role == model::Role::end ? kActive : kUsed);
            }
        }
    }
    for (const model::Line& end : step.ends) {
        if (end.zone.empty()) {
            pill("END", kActive);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", end.text.c_str());
        }
    }
    if (!any && step.ends.empty()) {
        ImGui::TextDisabled("This sequence uses no zone.");
    }
    edit_start_end(current, step.id);
    // The first sequence also shows what the mission starts with, beside it.
    const auto starts_here = [&](const model::Fight& fight) {
        return fight.after == step.id || (shown == 0 && fight.after == "arrival");
    };
    if (!step.beats.empty() || std::any_of(current.fights.begin(), current.fights.end(), starts_here)) {
        ImGui::SeparatorText("What happens");
        for (const model::Line& beat : step.beats) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", beat.text.c_str());
        }
        for (const model::Fight& fight : current.fights) {
            if (!starts_here(fight)) {
                continue;
            }
            pill(fight.after == "arrival" ? "EVENT at start" : "EVENT", kWarn);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", fight.line.text.c_str());
        }
        ImGui::TextDisabled("A scene plays its own authored dialogue and animation.");
    }
    ImGui::Dummy({0.0F, 4.0F * g_scale});
    if (primary_button("Replay from this sequence")) {
        if (mission::reload()) {
            g_status = mission::queue_dev_command("replay", step.id, g_solo ? "solo" : "")
                           ? "reloading and replaying " + step.id
                           : "a command is already waiting";
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("reloads the script, removes what it placed, replays from here");
    end_card();
}

// -------------------------------------------------------------------------- the editor

/** One zone field: the picker, and a button that takes the zone nearest the camera. */
bool zone_pick(const char* id, std::string& value, const model::Model* current) {
    bool picked = pick(id, value, g_catalog.triggers);
    if (current != nullptr) {
        ImGui::SameLine();
        ImGui::PushID(id);
        if (ImGui::SmallButton("nearest")) {
            if (const model::Zone* const zone = nearest_zone(*current)) {
                value = upper(zone->name);
                picked = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The zone nearest the spectator camera (or the player's camera).");
        }
        ImGui::PopID();
    }
    return picked;
}

/** A list of names, each with its picker and a remove button, and an add button. */
bool name_list(const char* id, Value& holder, std::string_view key, const std::vector<Choice>& choices,
               const char* add, const model::Model* current, bool zones) {
    ImGui::PushID(id);
    std::vector<std::string> values = strings_of(holder.get(key));
    bool edited = false;
    for (std::size_t index = 0; index < values.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        edited |= zones ? zone_pick("item", values[index], current)
                        : pick("item", values[index], choices);
        if (remove_button("remove")) {
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
            edited = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(add)) {
        values.emplace_back();
        edited = true;
    }
    if (edited) {
        // A single trigger stays a single name, as the kit takes either.
        if (values.empty()) {
            holder.erase(key);
        } else if (key == "trigger" && values.size() == 1) {
            holder.set(key, Value::of(values[0]));
        } else {
            holder.set(key, list_of(values));
        }
        changed();
    }
    ImGui::PopID();
    return edited;
}

/** One effect's fields. @return True when its remove button was pressed. */
bool edit_one_effect(Value& effect) {
    ImGui::Text("Effect");
    ImGui::SameLine();
    std::string slot = string_of(&effect, "slot");
    if (pick("slot", slot, g_catalog.effects)) {
        put_string(effect, "slot", slot);
        // The hop-on's own filter is the usual one: HO_X goes with OF_X.
        if (effect.text_of("filter").empty() && slot.starts_with("HO_")) {
            const std::string filter = "OF_" + slot.substr(3);
            if (std::any_of(g_catalog.filters.begin(), g_catalog.filters.end(),
                            [&](const Choice& choice) { return choice.value == filter; })) {
                put_string(effect, "filter", filter);
            }
        }
        changed();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("on");
    ImGui::SameLine();
    std::string filter = string_of(&effect, "filter");
    if (pick("filter", filter, g_catalog.filters, "(its filter)")) {
        put_string(effect, "filter", filter);
        changed();
    }
    ImGui::SameLine();
    std::string target = string_of(&effect, "target");
    if (pick("target", target, g_catalog.objects, "(the players)")) {
        put_string(effect, "target", target);
        changed();
    }
    const bool removed = remove_button("remove");
    ImGui::Indent();
    bool off = effect.get("enabled") != nullptr && !flag(effect, "enabled");
    if (ImGui::Checkbox("remove it", &off)) {
        effect.set("enabled", off ? Value::of(false) : Value{});
        changed();
    }
    ImGui::SameLine();
    bool decor = effect.get("arm") != nullptr && !flag(effect, "arm");
    if (ImGui::Checkbox("decor (its authored filter, not the players)", &decor)) {
        effect.set("arm", decor ? Value::of(false) : Value{});
        changed();
    }
    if (!slot.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("apply now")) {
            const std::string what = "on " + (filter.empty() ? "OF_" + slot.substr(3) : filter)
                                     + (target.empty() ? "" : " " + target);
            g_status = mission::queue_dev_command("effect", what, slot) ? "effect tester: " + what
                                                                         : "a command is waiting";
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Applies this effect in game now, as set here, to see what it does.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("remove now")) {
            g_status = mission::queue_dev_command("effect", "off " + (filter.empty() ? "OF_" + slot.substr(3) : filter), slot) ? "effect tester: off " + slot
                                                                          : "a command is waiting";
        }
    }
    ImGui::Unindent();
    return removed;
}

/**
 * The holder's effects, applied together. One effect is written as `effect`, several as the
 * `effects` list; the kit runs `effect` first, then the list in order.
 */
void edit_effect(Value& holder) {
    ImGui::PushID("effect");
    bool dropSingle = false;
    if (Value* const single = holder.get("effect")) {
        dropSingle = edit_one_effect(*single);
    }
    std::ptrdiff_t dropped = -1;
    if (Value* const list = holder.get("effects")) {
        for (std::size_t index = 0; index < list->items.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (edit_one_effect(list->items[index])) {
                dropped = static_cast<std::ptrdiff_t>(index);
            }
            ImGui::PopID();
        }
    }
    // Changes to the holder's fields come after the drawing, which held pointers into them.
    if (dropSingle) {
        holder.erase("effect");
        changed();
    }
    if (dropped >= 0) {
        Value& list = holder.at("effects");
        list.items.erase(list.items.begin() + dropped);
        if (list.items.empty()) {
            holder.erase("effects");
        }
        changed();
    }
    if (ImGui::SmallButton("+ effect")) {
        Value fresh = Value::table();
        if (holder.get("effect") == nullptr && holder.get("effects") == nullptr) {
            holder.set("effect", std::move(fresh));
        } else {
            Value list = holder.get("effects") != nullptr ? *holder.get("effects") : Value::table();
            if (const Value* const single = holder.get("effect")) {
                list.items.insert(list.items.begin(), *single);
                holder.erase("effect");
            }
            list.items.push_back(std::move(fresh));
            holder.set("effects", std::move(list));
        }
        changed();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Effects of one step are applied together, in this order.");
    }
    ImGui::PopID();
}

/** @return A hexadecimal key as a number, or 0. */
[[nodiscard]] std::int64_t parse_key(std::string_view text) {
    std::int64_t value = 0;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return value;
}

/** Sends one scene tester command to the running script. */
void test_scene(const std::string& scene, const std::string& what) {
    g_status = mission::queue_dev_command("scene", what, scene)
                   ? "scene tester: " + what + " " + model::human(scene)
                   : "a command is already waiting: try again in a moment";
}

/**
 * One scene's event keys: which ones its activation sends, and a tester that plays the scene
 * with no key, then sends any key on its own, to see which part of the scene each one starts.
 */
void edit_scene_keys(Value& entry, const std::string& scene) {
    const auto found = std::find_if(g_catalog.sceneKeys.begin(), g_catalog.sceneKeys.end(),
                                    [&](const auto& pair) { return pair.first == scene; });
    if (found == g_catalog.sceneKeys.end() || found->second.empty()) {
        return;
    }
    const std::vector<std::string>& authored = found->second;
    const Value* const chosen = entry.get("keys");
    const auto sent = [&](std::int64_t key) {
        if (chosen == nullptr) {
            return true;
        }
        return std::any_of(chosen->items.begin(), chosen->items.end(),
                           [&](const Value& value) { return integer_of(&value) == key; });
    };
    std::size_t count = 0;
    for (const std::string& key : authored) {
        count += sent(parse_key(key)) ? 1 : 0;
    }
    const std::string title = "keys: " + std::to_string(count) + " of " + std::to_string(authored.size())
                              + " sent when it plays###keys";
    if (!ImGui::TreeNodeEx(title.c_str(), 0)) {
        return;
    }
    if (ImGui::SmallButton("Play it now, no key")) {
        test_scene(scene, "play");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Activates the scene in game with no event key (binding its cells when a\n"
                          "step binds them), so each key can then be sent on its own.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop it")) {
        test_scene(scene, "stop");
    }
    std::vector<std::int64_t> keep{};
    bool edited = false;
    for (std::size_t index = 0; index < authored.size(); ++index) {
        const std::int64_t key = parse_key(authored[index]);
        ImGui::PushID(static_cast<int>(index));
        bool on = sent(key);
        const std::string label = "key " + std::to_string(index + 1) + "  " + authored[index];
        if (ImGui::Checkbox(label.c_str(), &on)) {
            edited = true;
        }
        if (on) {
            keep.push_back(key);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("send")) {
            test_scene(scene, authored[index]);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sends this key to the playing scene now. Keys add up: a key already\n"
                              "sent stays sent until the scene stops.");
        }
        ImGui::PopID();
    }
    if (edited) {
        // Every key is the default; the data lists keys only when some are left out.
        if (keep.size() == authored.size()) {
            entry.erase("keys");
        } else {
            Value list = Value::table();
            for (const std::int64_t key : keep) {
                list.items.push_back(Value::of_integer(key));
            }
            entry.set("keys", std::move(list));
        }
        changed();
    }
    ImGui::TreePop();
}

void edit_scenes(Value& holder) {
    ImGui::PushID("scenes");
    Value* const list = holder.get("scenes");
    if (list != nullptr) {
        for (std::size_t index = 0; index < list->items.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            Value& entry = list->items[index];
            ImGui::Text("Plays");
            ImGui::SameLine();
            std::string scene = string_of(&entry, "scene");
            if (pick("scene", scene, g_catalog.scenes)) {
                put_string(entry, "scene", scene);
                changed();
            }
            ImGui::SameLine();
            const std::vector<std::string> bound = strings_of(entry.get("bind"));
            if (ImGui::SmallButton(bound.empty() ? "bind its combatants" : "unbind")) {
                std::vector<std::string> cells{};
                if (bound.empty()) {
                    for (const auto& [name, squads] : g_catalog.sceneSquads) {
                        if (name != scene) {
                            continue;
                        }
                        for (const std::string& squad : squads) {
                            for (const std::string& cell : g_catalog.cells) {
                                if (cell.starts_with(squad + "_CELL")) {
                                    cells.push_back(cell);
                                }
                            }
                        }
                    }
                }
                entry.set("bind", cells.empty() ? Value{} : list_of(cells));
                changed();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("A bound scene spawns its own combatants. Never also place them as squads.");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(plays its own dialogue)");
            if (!bound.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%zu combatants", bound.size());
            }
            const bool dropped = remove_button("remove");
            ImGui::Indent();
            edit_scene_keys(entry, scene);
            ImGui::Unindent();
            if (dropped) {
                list->items.erase(list->items.begin() + static_cast<std::ptrdiff_t>(index));
                if (list->items.empty()) {
                    holder.erase("scenes");
                }
                changed();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::SmallButton("+ scene")) {
        Value entry = Value::table();
        entry.set("scene", Value::of(std::string_view("")));
        holder.at("scenes").kind = Value::Kind::table;
        holder.at("scenes").items.push_back(std::move(entry));
        changed();
    }
    ImGui::PopID();
}

/** Keys sent to a scene already playing, such as a later part of a scene an earlier step started. */
void edit_signal(Value& holder) {
    ImGui::PushID("signal");
    Value* const signal = holder.get("signal");
    if (signal == nullptr) {
        if (ImGui::SmallButton("+ send keys to a playing scene")) {
            Value fresh = Value::table();
            fresh.set("scene", Value::of(std::string_view("")));
            fresh.set("keys", Value::table());
            holder.set("signal", std::move(fresh));
            changed();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sends event keys to a scene that is already playing, without starting it\n"
                              "again: the way to play the next part of a scene an earlier step started.");
        }
        ImGui::PopID();
        return;
    }
    ImGui::Text("Sends keys to");
    ImGui::SameLine();
    std::string scene = string_of(signal, "scene");
    if (pick("scene", scene, g_catalog.scenes)) {
        signal->set("scene", Value::of(scene));
        changed();
    }
    if (remove_button("remove")) {
        holder.erase("signal");
        changed();
        ImGui::PopID();
        return;
    }
    const auto found = std::find_if(g_catalog.sceneKeys.begin(), g_catalog.sceneKeys.end(),
                                    [&](const auto& pair) { return pair.first == scene; });
    if (found != g_catalog.sceneKeys.end()) {
        const Value* const chosen = signal->get("keys");
        std::vector<std::int64_t> keep{};
        bool edited = false;
        ImGui::Indent();
        for (std::size_t index = 0; index < found->second.size(); ++index) {
            const std::int64_t key = parse_key(found->second[index]);
            bool on = chosen != nullptr
                      && std::any_of(chosen->items.begin(), chosen->items.end(),
                                     [&](const Value& value) { return integer_of(&value) == key; });
            ImGui::PushID(static_cast<int>(index));
            edited |= ImGui::Checkbox(("key " + std::to_string(index + 1) + "  " + found->second[index]).c_str(),
                                      &on);
            ImGui::SameLine();
            if (ImGui::SmallButton("send")) {
                test_scene(scene, found->second[index]);
            }
            ImGui::PopID();
            if (on) {
                keep.push_back(key);
            }
        }
        if (keep.empty()) {
            ImGui::TextColored(kBad, "check at least one key");
        }
        ImGui::Unindent();
        if (edited) {
            Value list = Value::table();
            for (const std::int64_t key : keep) {
                list.items.push_back(Value::of_integer(key));
            }
            signal->set("keys", std::move(list));
            changed();
        }
    }
    ImGui::PopID();
}

void edit_lines(Value& holder) {
    ImGui::PushID("lines");
    ImGui::TextDisabled("Says");
    name_list("cues", holder, "lines", g_catalog.cues, "+ line");
    ImGui::PopID();
}

void edit_move(Value& holder) {
    ImGui::PushID("move");
    Value* const land = holder.get("land");
    if (land == nullptr) {
        if (ImGui::SmallButton("+ fade and move the player")) {
            Value value = Value::table();
            value.set("state", Value::of(std::string_view("")));
            holder.set("land", std::move(value));
            changed();
        }
    } else {
        if (land->is_string()) {
            const std::string state = land->text;
            *land = Value::table();
            land->set("state", Value::of(state));
        }
        ImGui::Text("Moves the player to");
        ImGui::SameLine();
        std::string state = string_of(land, "state");
        if (pick("state", state, g_catalog.states)) {
            put_string(*land, "state", state);
            changed();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("at");
        ImGui::SameLine();
        std::array<char, 16> hex{};
        const std::int64_t current = integer_of(land->get("spawn_set"));
        if (current != 0) {
            std::snprintf(hex.data(), hex.size(), "0x%08llX", static_cast<long long>(current));
        }
        std::string set = hex.data();
        if (pick("spawn", set, g_catalog.spawnSets, "(the region's own spawn)")) {
            std::int64_t parsed = 0;
            if (set.size() > 2) {
                std::from_chars(set.data() + 2, set.data() + set.size(), parsed, 16);
            }
            land->set("spawn_set", parsed != 0 ? Value::of_integer(parsed) : Value{});
            changed();
        }
        if (remove_button("remove")) {
            holder.erase("land");
            changed();
        }
    }
    ImGui::PopID();
}

void edit_actions(Value& holder) {
    ImGui::PushID("events");
    ImGui::TextDisabled("Starts events");
    name_list("events", holder, "events", event_choices(), "+ start an event");
    ImGui::PopID();
    edit_effect(holder);
    edit_scenes(holder);
    ImGui::PushID("stop");
    ImGui::TextDisabled("Stops");
    name_list("stops", holder, "stop", g_catalog.scenes, "+ stop a scene");
    ImGui::PopID();
    edit_signal(holder);
    edit_lines(holder);
    edit_move(holder);
}

/** What a moment waits on, in the data's field names. */
constexpr std::array<const char*, 6> kWaits{"", "on", "finished", "interacted", "spoken", "cleared"};
constexpr std::array<const char*, 6> kWaitWords{"right after the step before",
                                                "the player reaches a zone",
                                                "a scene finishes",
                                                "the player uses an object",
                                                "a line has been said",
                                                "fights are cleared"};

void edit_moment(Value& item, const model::Model* current) {
    int wait = 0;
    for (std::size_t index = 1; index < kWaits.size(); ++index) {
        if (item.get(kWaits[index]) != nullptr) {
            wait = static_cast<int>(index);
        }
    }
    ImGui::SetNextItemWidth(240.0F);
    if (ImGui::Combo("##wait", &wait, kWaitWords.data(), static_cast<int>(kWaitWords.size()))) {
        for (std::size_t index = 1; index < kWaits.size(); ++index) {
            item.erase(kWaits[index]);
        }
        if (wait > 0) {
            item.set(kWaits[static_cast<std::size_t>(wait)], Value::of(std::string_view("")));
        }
        changed();
    }
    if (wait > 0) {
        ImGui::SameLine();
        const std::string key = kWaits[static_cast<std::size_t>(wait)];
        std::string value = string_of(&item, key);
        bool edited = false;
        if (key == "on") {
            edited = zone_pick("wait", value, current);
        } else if (key == "finished") {
            edited = pick("wait", value, g_catalog.sceneSlots);
        } else if (key == "interacted") {
            edited = pick("wait", value, g_catalog.objects);
        } else if (key == "spoken") {
            edited = pick("wait", value, g_catalog.cues);
        } else {
            ImGui::SetNextItemWidth(260.0F);
            edited = edit_text("##cleared", value);
        }
        if (edited) {
            // An empty choice stays in the data as "", so the save can name what is missing.
            item.set(key, Value::of(value));
            changed();
        }
        if (value.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kBad, "choose one");
        }
    }
    ImGui::SameLine();
    float seconds = static_cast<float>(integer_of(item.get("after_ms"))) / 1000.0F;
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::InputFloat("s later##delay", &seconds, 0.5F, 1.0F, "%.1f")) {
        const auto ms = static_cast<std::int64_t>(std::lround((std::max)(seconds, 0.0F) * 1000.0F));
        item.set("after_ms", ms > 0 ? Value::of_integer(ms) : Value{});
        changed();
    }
    ImGui::Indent();
    edit_actions(item);
    ImGui::Unindent();
}

void edit_moments(Value& step, const model::Model* current) {
    Value* const list = step.get("sequence");
    if (list != nullptr) {
        for (std::size_t index = 0; index < list->items.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::Separator();
            const bool open = ImGui::TreeNodeEx(
                ("Step " + std::to_string(index + 1) + "   " + moment_summary(list->items[index])
                 + "###moment")
                    .c_str(),
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::SameLine();
            if (ImGui::SmallButton("up") && index > 0) {
                std::swap(list->items[index], list->items[index - 1]);
                changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("down") && index + 1 < list->items.size()) {
                std::swap(list->items[index], list->items[index + 1]);
                changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) {
                list->items.erase(list->items.begin() + static_cast<std::ptrdiff_t>(index));
                if (list->items.empty()) {
                    step.erase("sequence");
                }
                changed();
                ImGui::PopID();
                break;
            }
            if (open) {
                ImGui::TextDisabled("Waits for");
                ImGui::SameLine();
                edit_moment(list->items[index], current);
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button("+ step")) {
        step.at("sequence").kind = Value::Kind::table;
        step.at("sequence").items.push_back(Value::table());
        changed();
    }
}

void edit_ends(Value& step, const model::Model* current) {
    Value& ends = step.at("ends");
    if (!ends.is_table()) {
        ends = Value::table();
    }
    ImGui::TextDisabled("The first of these to happen moves on to the next sequence.");
    ImGui::Text("The player reaches");
    name_list("triggers", ends, "trigger", g_catalog.triggers, "+ zone", current, true);
    bool sequence = flag(ends, "sequence");
    if (ImGui::Checkbox("every step has played", &sequence)) {
        ends.set("sequence", sequence ? Value::of(true) : Value{});
        changed();
    }
    ImGui::SameLine();
    bool cutscene = flag(ends, "cutscene");
    if (ImGui::Checkbox("the cutscene has ended", &cutscene)) {
        ends.set("cutscene", cutscene ? Value::of(true) : Value{});
        changed();
    }
    ImGui::Text("The player uses");
    ImGui::SameLine();
    std::string interact = string_of(&ends, "interact");
    if (pick("interact", interact, g_catalog.objects)) {
        put_string(ends, "interact", interact);
        changed();
    }
    ImGui::Text("A scene finishes");
    ImGui::SameLine();
    std::string scene = string_of(&ends, "scene");
    if (pick("scene", scene, g_catalog.sceneSlots)) {
        put_string(ends, "scene", scene);
        changed();
    }
    ImGui::Text("The player arrives in region");
    ImGui::SameLine();
    std::string region = string_of(&ends, "region");
    ImGui::SetNextItemWidth(200.0F);
    if (edit_text("##region", region)) {
        put_string(ends, "region", region);
        changed();
    }
    if (ends.fields.empty()) {
        ImGui::TextColored(kWarn, "No end: the mission stays on this sequence.");
    }
}

/** The sequence's name, place and goal. @return False when the sequence list changed under it. */
bool edit_step_identity(Value& steps, std::size_t index) {
    Value& step = steps.items[index];
    std::string id = string_of(&step, "id");
    ImGui::SetNextItemWidth(260.0F);
    if (edit_id(id)) {
        put_string(step, "id", id);
        changed();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("up") && index > 0) {
        std::swap(steps.items[index], steps.items[index - 1]);
        g_selected = static_cast<int>(index) - 1;
        changed();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("down") && index + 1 < steps.items.size()) {
        std::swap(steps.items[index], steps.items[index + 1]);
        g_selected = static_cast<int>(index) + 1;
        changed();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ sequence after")) {
        Value fresh = Value::table();
        fresh.set("id", Value::of("sequence_" + std::to_string(steps.items.size() + 1)));
        steps.items.insert(steps.items.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(fresh));
        g_selected = static_cast<int>(index) + 1;
        changed();
        return false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("delete sequence")) {
        steps.items.erase(steps.items.begin() + static_cast<std::ptrdiff_t>(index));
        g_selected = -1;
        changed();
        return false;
    }
    std::string comment = string_of(&step, "comment");
    if (edit_text("##comment", comment, true)) {
        put_string(step, "comment", comment);
        changed();
    }
    for (const char* key : {"barrier", "checkpoint", "revisit"}) {
        bool value = flag(step, key);
        if (ImGui::Checkbox(key, &value)) {
            step.set(key, value ? Value::of(true) : Value{});
            changed();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Text("Goal");
    ImGui::SameLine();
    std::string directive = string_of(&step, "directive");
    if (pick("directive", directive, g_catalog.directives)) {
        put_string(step, "directive", directive);
        changed();
    }
    ImGui::SameLine();
    ImGui::Text("marker on");
    ImGui::SameLine();
    std::string navpoint = string_of(&step, "navpoint");
    if (pick("navpoint", navpoint, g_catalog.anchors)) {
        put_string(step, "navpoint", navpoint);
        changed();
    }
    return true;
}

void edit_step(Value& steps, std::size_t index, const model::Model* current) {
    ImGui::PushID(static_cast<int>(index));
    begin_card("##identity");
    if (fold("Sequence###identity") && !edit_step_identity(steps, index)) {
        end_card();
        ImGui::PopID();
        return;
    }
    end_card();
    Value& step = steps.items[index];
    begin_card("##start");
    if (fold("When the sequence starts###start")) {
        ImGui::PushID("start");
        edit_actions(step);
        ImGui::PopID();
    }
    end_card();
    begin_card("##moments");
    if (fold("Steps, in order###moments")) {
        edit_moments(step, current);
    }
    end_card();
    begin_card("##ends");
    if (fold("The sequence ends when###ends")) {
        edit_ends(step, current);
    }
    end_card();
    if (const Value* const notes = step.get("notes"); notes != nullptr && !notes->items.empty()) {
        if (ImGui::CollapsingHeader("Generator notes")) {
            for (const Value& note : notes->items) {
                ImGui::TextColored(kWarn, "%s", note.text.c_str());
            }
            if (ImGui::SmallButton("clear the notes")) {
                step.erase("notes");
                changed();
            }
        }
    }
    ImGui::PopID();
}

/** The event's squads: each with its picker, its count and a remove button. */
void edit_squads(Value& event) {
    Value& list = event.at("squads");
    if (!list.is_table()) {
        list = Value::table();
    }
    for (std::size_t index = 0; index < list.items.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        Value& unit = list.items[index];
        std::string squad = string_of(&unit, "squad");
        if (pick("squad", squad, g_catalog.squads)) {
            put_string(unit, "squad", squad);
            unit.erase("slot");
            changed();
        }
        ImGui::SameLine();
        int count = static_cast<int>(integer_of(unit.get("count")));
        ImGui::SetNextItemWidth(110.0F * g_scale);
        if (ImGui::InputInt("members##count", &count)) {
            unit.set("count", count > 0 ? Value::of_integer(count) : Value{});
            changed();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 places the squad's own authored members.");
        }
        if (remove_button("remove")) {
            list.items.erase(list.items.begin() + static_cast<std::ptrdiff_t>(index));
            changed();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ squad")) {
        list.items.push_back(Value::table());
        changed();
    }
    if (list.items.empty()) {
        event.erase("squads");
    }
}

/** Where the event is started from, and a way to start it from one more sequence. */
void edit_started_by(Value& event) {
    const std::string id(event.text_of("id"));
    if (starts_itself(event)) {
        ImGui::TextColored(kWarn, "Older form: it starts on its own %s.", event_start(event).c_str());
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextDisabled("An event is data a step starts. Moved into its sequence, it becomes a step "
                            "there, so the sequence shows when it happens.");
        ImGui::PopTextWrapPos();
        if (primary_button("Move it into its sequence")) {
            g_status = move_into_sequence(event);
        }
        return;
    }
    const std::vector<Reference> started = references(id);
    if (started.empty()) {
        ImGui::TextColored(kWarn, "No step starts it yet: it never plays.");
    }
    for (std::size_t index = 0; index < started.size(); ++index) {
        const Reference& reference = started[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::Bullet();
        ImGui::TextUnformatted(reference_text(reference).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("open")) {
            if (reference.event) {
                g_selectedEvent = static_cast<int>(reference.index);
            } else {
                g_selectedEvent = -1;
                select(model::current().get(), static_cast<int>(reference.index), reference.owner);
            }
        }
        ImGui::PopID();
    }
    std::vector<Choice> sequences{};
    if (const Value* const steps = document_steps()) {
        for (const Value& step : steps->items) {
            const std::string name(step.text_of("id"));
            sequences.push_back({name, model::human(name), "adds a step at the end of its steps"});
        }
    }
    ImGui::Text("Start it from");
    ImGui::SameLine();
    std::string chosen{};
    if (pick("add_to", chosen, sequences, "(choose a sequence)") && !chosen.empty()) {
        Value* const steps = document_steps();
        for (std::size_t index = 0; steps != nullptr && index < steps->items.size(); ++index) {
            Value& step = steps->items[index];
            if (step.text_of("id") != chosen) {
                continue;
            }
            Value item = Value::table();
            item.set("events", list_of({id}));
            Value& items = step.at("sequence");
            if (!items.is_table()) {
                items = Value::table();
            }
            items.items.push_back(std::move(item));
            changed();
            g_status = "added a step to " + model::human(chosen) + ": choose what it waits for there";
            g_selectedEvent = -1;
            select(model::current().get(), static_cast<int>(index), chosen);
            break;
        }
    }
}

void edit_event(Value& events, std::size_t index, const model::Model* current) {
    Value& event = events.items[index];
    ImGui::PushID(static_cast<int>(index) + 100000);
    begin_card("##event_identity");
    if (fold("Event###event_identity")) {
        std::string id(event.text_of("id"));
        ImGui::SetNextItemWidth(260.0F * g_scale);
        if (edit_id(id)) {
            // The steps that start it follow the new name.
            const std::string previous(event.text_of("id"));
            rename_event(previous, id);
            put_string(event, "id", id);
            changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("delete event")) {
            rename_event(id, {});
            events.items.erase(events.items.begin() + static_cast<std::ptrdiff_t>(index));
            g_selectedEvent = -1;
            changed();
            end_card();
            ImGui::PopID();
            return;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Also removes it from the steps that start it.");
        }
        std::string comment(event.text_of("comment"));
        if (edit_text("##comment", comment, true)) {
            put_string(event, "comment", comment);
            changed();
        }
    }
    end_card();

    begin_card("##event_started");
    if (fold("Started by###event_started")) {
        edit_started_by(event);
    }
    end_card();
    if (static_cast<std::size_t>(g_selectedEvent) != index) {
        ImGui::PopID();
        return;
    }

    begin_card("##event_squads");
    if (fold("Squads it places###event_squads")) {
        ImGui::TextDisabled("Never place a squad a bound scene already spawns: it would be doubled.");
        edit_squads(event);
        ImGui::Text("Objective");
        ImGui::SameLine();
        std::string objective(event.text_of("objective"));
        if (pick("objective", objective, g_catalog.objectives, "(none)")) {
            put_string(event, "objective", objective);
            changed();
        }
        ImGui::SameLine();
        ImGui::Text("task groups");
        ImGui::SameLine();
        std::string groups(event.text_of("groups"));
        if (pick("groups", groups, g_catalog.groups, "(none)")) {
            put_string(event, "groups", groups);
            changed();
        }
        bool hold = flag(event, "hold");
        if (ImGui::Checkbox("hold its squads until released", &hold)) {
            event.set("hold", hold ? Value::of(true) : Value{});
            changed();
        }
    }
    end_card();

    begin_card("##event_start");
    if (fold("When the event starts###event_start")) {
        ImGui::PushID("start");
        edit_actions(event);
        ImGui::PopID();
    }
    end_card();

    begin_card("##event_steps");
    if (fold("Steps, in order###event_steps")) {
        edit_moments(event, current);
    }
    end_card();
    ImGui::PopID();
}

/** The event's card in the world: the zones its steps wait on and what it plays. */
void draw_event_overview(const model::Model* current, const Value& event) {
    begin_card("##event_overview");
    if (!fold(("Event  " + model::human(event.text_of("id")) + "###event_overview").c_str(), 20.0F)) {
        end_card();
        return;
    }
    const std::vector<Reference> started = references(event.text_of("id"));
    if (starts_itself(event)) {
        ImGui::TextDisabled("Starts on its own %s", event_start(event).c_str());
    } else if (started.empty()) {
        ImGui::TextColored(kWarn, "No step starts it yet.");
    } else {
        ImGui::TextDisabled("Started by %s%s", reference_text(started[0]).c_str(),
                            started.size() > 1 ? " and elsewhere" : "");
    }
    if (current != nullptr) {
        std::vector<std::string> zones{std::string(event.text_of("trigger"))};
        for (const Reference& reference : started) {
            zones.push_back(reference.on);
        }
        for (std::size_t index = 0; index < zones.size(); ++index) {
            if (const model::Zone* const zone = zone_named(*current, zones[index]);
                zone != nullptr
                && std::find(zones.begin(), zones.begin() + static_cast<std::ptrdiff_t>(index),
                             zones[index])
                       == zones.begin() + static_cast<std::ptrdiff_t>(index)) {
                zone_row(*zone, "START", kStart);
            }
        }
    }
    if (const Value* const list = event.get("scenes")) {
        for (const Value& entry : list->items) {
            pill("SCENE", kUsed);
            ImGui::SameLine();
            ImGui::Text("%s", model::human(entry.text_of("scene")).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("plays its own dialogue and animation");
        }
    }
    if (const Value* const squads = event.get("squads"); squads != nullptr && !squads->items.empty()) {
        pill("SQUADS", kWarn);
        ImGui::SameLine();
        ImGui::Text("%zu placed", squads->items.size());
    }
    end_card();
}

void draw_event_editor(const model::Model* current) {
    Value* const events = document_events();
    if (events == nullptr || g_selectedEvent < 0
        || static_cast<std::size_t>(g_selectedEvent) >= events->items.size()) {
        return;
    }
    const Value& event = events->items[static_cast<std::size_t>(g_selectedEvent)];
    // An event replays with the first sequence that starts it, or the one it waits on.
    std::string after(event.text_of("after"));
    for (const Reference& reference : references(event.text_of("id"))) {
        if (!reference.event) {
            after = reference.owner;
            break;
        }
    }
    if (after.empty() || after == "arrival") {
        const Value* const steps = document_steps();
        after = steps != nullptr && !steps->items.empty() ? std::string(steps->items[0].text_of("id"))
                                                          : std::string{};
    }
    if (primary_button(g_dirty ? "Save and replay its sequence" : "Replay its sequence")) {
        if (g_dirty) {
            save(after);
        } else if (mission::reload()) {
            g_status = mission::queue_dev_command("replay", after, g_solo ? "solo" : "") ? "replaying " + after
                                                                       : "a command is waiting";
        }
    }
    ImGui::Dummy({0.0F, 4.0F * g_scale});
    edit_event(*events, static_cast<std::size_t>(g_selectedEvent), current);
}

void draw_editor(const model::Model* current) {
    Value* const steps = document_steps();
    if (steps == nullptr) {
        ImGui::TextColored(kWarn, "%s", g_documentError.c_str());
        return;
    }
    if (g_selected < 0 || static_cast<std::size_t>(g_selected) >= steps->items.size()) {
        ImGui::TextDisabled("Select a sequence on the left to edit it.");
        return;
    }
    const std::string id(steps->items[static_cast<std::size_t>(g_selected)].text_of("id"));
    if (primary_button(g_dirty ? "Save and replay this sequence" : "Replay this sequence")) {
        if (g_dirty) {
            save(id);
        } else if (mission::reload()) {
            g_status = mission::queue_dev_command("replay", id, g_solo ? "solo" : "") ? "replaying " + id
                                                                    : "a command is waiting";
        }
    }
    ImGui::Dummy({0.0F, 4.0F * g_scale});
    edit_step(*steps, static_cast<std::size_t>(g_selected), current);
}

void draw_content() {
    const std::shared_ptr<const model::Model> current = model::current();
    const std::uint32_t row = mission::first_activity_row();
    if (row != 0) {
        ensure_document(row);
    }
    draw_toolbar(current.get());
    const float listWidth = 320.0F * g_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F * g_scale, 10.0F * g_scale});
    const bool listOpen = ImGui::BeginChild("##steps", {listWidth, 0.0F},
                                            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (listOpen) {
        draw_step_list(current.get());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    const bool stepOpen = ImGui::BeginChild("##step", {0.0F, 0.0F});
    ImGui::PopStyleColor();
    if (stepOpen && g_selectedEvent >= 0) {
        if (Value* const events = document_events();
            events != nullptr && static_cast<std::size_t>(g_selectedEvent) < events->items.size()) {
            draw_event_overview(current.get(), events->items[static_cast<std::size_t>(g_selectedEvent)]);
        }
        ImGui::Dummy({0.0F, 4.0F * g_scale});
        heading("Edit", 20.0F);
        draw_event_editor(current.get());
    } else if (stepOpen) {
        int shown = -1;
        if (current != nullptr) {
            if (g_selected >= 0) {
                if (Value* const steps = document_steps();
                    steps != nullptr && static_cast<std::size_t>(g_selected) < steps->items.size()) {
                    shown = model_index(current.get(),
                                        steps->items[static_cast<std::size_t>(g_selected)].text_of("id"));
                } else {
                    shown = model::shown_step(*current);
                }
            } else {
                shown = current->active;
            }
            model::set_focus(g_selected >= 0 ? shown : -1);
            draw_overview(*current, shown);
        }
        ImGui::Dummy({0.0F, 4.0F * g_scale});
        heading("Edit", 20.0F);
        if (g_selected < 0 && current != nullptr && current->active >= 0 && document_steps() != nullptr) {
            ImGui::TextDisabled("Following the running sequence. Select it on the left to edit it.");
        } else {
            draw_editor(current.get());
        }
    }
    ImGui::EndChild();
}

} // namespace

void set_fonts(ImFont* heading, float scale) noexcept {
    g_heading = heading;
    g_scale = scale > 0.0F ? scale : 1.0F;
}

void draw() noexcept {
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                        | ImGuiWindowFlags_NoSavedSettings
                                        | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##mission_editor", nullptr, kFlags)) {
        try {
            draw_content();
        } catch (...) {
            g_status = "the editor ran out of memory drawing this frame";
        }
    }
    ImGui::End();
}

} // namespace sunrise::server::ui::mission_editor::view
