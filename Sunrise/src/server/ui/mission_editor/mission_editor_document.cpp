#include "mission_editor_document.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <new>

#include "lauxlib.h"
#include "lua.h"

namespace sunrise::server::ui::mission_editor::document {
namespace {

/** The campaign's field order, so a rewritten file reads as the authored one did. */
constexpr std::array<std::string_view, 62> kFieldOrder{
    "key", "title", "id", "after", "comment", "barrier", "checkpoint", "revisit", "directive",
    "navpoint", "waypoint", "progress", "trigger", "monitor", "on", "finished", "interacted",
    "spoken", "cleared", "after_ms", "events", "objective", "groups", "approach", "hold", "sensors", "intro",
    "state", "cinematic", "spawn_set", "arm", "watch", "effect", "effects", "slot", "filter", "target",
    "enabled", "lines", "scenes", "scene", "bind", "keys", "spawn", "stop", "retire", "squads",
    "squad", "group", "count", "land", "cutscene", "interact", "sequence", "ends", "legs",
    "steps", "encounters", "notes", "note", "directive_sensor", "debug_start"};

/** Fields whose integers are hashes, written in hexadecimal. */
[[nodiscard]] bool hex_field(std::string_view key) noexcept {
    return key == "spawn_set" || key == "keys" || key == "hash";
}

/** A flat table longer than this is written one entry per line. */
constexpr std::size_t kFlatWidth = 90;

[[nodiscard]] std::size_t order_of(std::string_view key) noexcept {
    const auto found = std::find(kFieldOrder.begin(), kFieldOrder.end(), key);
    return static_cast<std::size_t>(found - kFieldOrder.begin());
}

/** Converts the value on top of the stack. Depth is bounded; data files are shallow. */
[[nodiscard]] bool read_value(lua_State* state, int depth, Value& output, std::string& error) {
    if (depth > 32) {
        error = "data nests too deep";
        return false;
    }
    switch (lua_type(state, -1)) {
    case LUA_TNIL:
        output = {};
        return true;
    case LUA_TBOOLEAN:
        output = Value::of(lua_toboolean(state, -1) != 0);
        return true;
    case LUA_TNUMBER:
        output = {};
        output.kind = Value::Kind::number;
        output.integer = lua_isinteger(state, -1) != 0;
        output.number = output.integer ? static_cast<double>(lua_tointeger(state, -1))
                                       : lua_tonumber(state, -1);
        return true;
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* const text = lua_tolstring(state, -1, &length);
        output = Value::of(std::string_view(text, length));
        return true;
    }
    case LUA_TTABLE:
        break;
    default:
        error = "data holds a value that is neither a scalar nor a table";
        return false;
    }
    output = Value::table();
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
    for (lua_Integer index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        Value item{};
        const bool read = read_value(state, depth + 1, item, error);
        lua_pop(state, 1);
        if (!read) {
            return false;
        }
        output.items.push_back(std::move(item));
    }
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        // Array entries were read above; only named fields are left.
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* const key = lua_tolstring(state, -2, &length);
            Value field{};
            if (!read_value(state, depth + 1, field, error)) {
                lua_pop(state, 2);
                return false;
            }
            output.fields.emplace_back(std::string(key, length), std::move(field));
        }
        lua_pop(state, 1);
    }
    std::stable_sort(output.fields.begin(), output.fields.end(), [](const auto& left, const auto& right) {
        const std::size_t a = order_of(left.first);
        const std::size_t b = order_of(right.first);
        return a != b ? a < b : left.first < right.first;
    });
    return true;
}

[[nodiscard]] std::string quoted(std::string_view text) {
    std::string output = "\"";
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            output += '\\';
            output += character;
        } else if (character == '\n') {
            output += "\\n";
        } else if (static_cast<unsigned char>(character) < 32) {
            std::array<char, 8> escape{};
            std::snprintf(escape.data(), escape.size(), "\\%03d", character);
            output += escape.data();
        } else {
            output += character;
        }
    }
    return output + "\"";
}

[[nodiscard]] bool identifier(std::string_view key) noexcept {
    if (key.empty() || (key[0] >= '0' && key[0] <= '9')) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char character) {
        return character == '_' || (character >= 'a' && character <= 'z')
               || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
    });
}

[[nodiscard]] std::string write_value(const Value& value, int indent, std::string_view field) {
    switch (value.kind) {
    case Value::Kind::nil:
        return "nil";
    case Value::Kind::boolean:
        return value.boolean ? "true" : "false";
    case Value::Kind::number: {
        std::array<char, 40> text{};
        if (value.integer) {
            const auto integer = static_cast<long long>(value.number);
            if (hex_field(field) && integer > 0xFFFF) {
                std::snprintf(text.data(), text.size(), "0x%08llX", integer);
            } else {
                std::snprintf(text.data(), text.size(), "%lld", integer);
            }
        } else {
            std::snprintf(text.data(), text.size(), "%.17g", value.number);
        }
        return text.data();
    }
    case Value::Kind::string:
        return quoted(value.text);
    case Value::Kind::table:
        break;
    }
    std::vector<std::string> entries{};
    for (const Value& item : value.items) {
        entries.push_back(write_value(item, indent + 1, field));
    }
    for (const auto& [key, item] : value.fields) {
        if (!item.is_nil()) {
            entries.push_back((identifier(key) ? key : "[" + quoted(key) + "]") + " = "
                              + write_value(item, indent + 1, key));
        }
    }
    std::string flat = "{";
    bool multiline = false;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        flat += (index == 0 ? "" : ", ") + entries[index];
        multiline = multiline || entries[index].find('\n') != std::string::npos;
    }
    flat += "}";
    if (!multiline && flat.size() <= kFlatWidth) {
        return flat;
    }
    const std::string pad(static_cast<std::size_t>(indent + 1) * 4, ' ');
    std::string output = "{\n";
    for (const std::string& entry : entries) {
        output += pad + entry + ",\n";
    }
    return output + std::string(static_cast<std::size_t>(indent) * 4, ' ') + "}";
}

[[nodiscard]] bool read_file(const std::wstring& path, std::string& output) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool read = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
                && size.QuadPart < 16LL * 1024 * 1024;
    if (read) {
        output.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD done = 0;
        read = output.empty()
               || (ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &done, nullptr)
                       != FALSE
                   && done == output.size());
    }
    CloseHandle(file);
    return read;
}

} // namespace

Value Value::of(std::string_view value) {
    Value output{};
    output.kind = Kind::string;
    output.text = value;
    return output;
}

Value Value::of(bool value) {
    Value output{};
    output.kind = Kind::boolean;
    output.boolean = value;
    return output;
}

Value Value::of_integer(std::int64_t value) {
    Value output{};
    output.kind = Kind::number;
    output.integer = true;
    output.number = static_cast<double>(value);
    return output;
}

Value Value::table() {
    Value output{};
    output.kind = Kind::table;
    return output;
}

Value* Value::get(std::string_view key) noexcept {
    for (auto& [name, value] : fields) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

const Value* Value::get(std::string_view key) const noexcept {
    for (const auto& [name, value] : fields) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string_view Value::text_of(std::string_view key) const noexcept {
    const Value* const value = get(key);
    return value != nullptr && value->is_string() ? std::string_view(value->text)
                                                  : std::string_view{};
}

Value& Value::at(std::string_view key) {
    if (Value* const found = get(key)) {
        return *found;
    }
    fields.emplace_back(std::string(key), Value{});
    std::stable_sort(fields.begin(), fields.end(), [](const auto& left, const auto& right) {
        const std::size_t a = order_of(left.first);
        const std::size_t b = order_of(right.first);
        return a != b ? a < b : left.first < right.first;
    });
    return *get(key);
}

void Value::set(std::string_view key, Value value) {
    if (value.is_nil()) {
        erase(key);
        return;
    }
    at(key) = std::move(value);
}

void Value::erase(std::string_view key) {
    fields.erase(std::remove_if(fields.begin(), fields.end(),
                                [&](const auto& field) { return field.first == key; }),
                 fields.end());
}

bool load(const std::wstring& path, Document& output, std::string& error) {
    output = {};
    std::string source{};
    if (!read_file(path, source)) {
        error = "the file cannot be read";
        return false;
    }
    // The head comment block is kept as written.
    std::size_t cursor = 0;
    while (cursor < source.size() && source.compare(cursor, 2, "--") == 0) {
        const std::size_t newline = source.find('\n', cursor);
        const std::size_t end = newline == std::string::npos ? source.size() : newline + 1;
        output.header.append(source, cursor, end - cursor);
        cursor = end;
    }
    lua_State* const state = luaL_newstate();
    if (state == nullptr) {
        error = "no Lua state";
        return false;
    }
    bool loaded = false;
    try {
        if (luaL_loadbufferx(state, source.data(), source.size(), "data", "t") != LUA_OK
            || lua_pcall(state, 0, 1, 0) != LUA_OK) {
            const char* const message = lua_tostring(state, -1);
            error = message != nullptr ? message : "the chunk failed";
        } else if (!lua_istable(state, -1)) {
            error = "the chunk does not return a table";
        } else {
            loaded = read_value(state, 0, output.root, error);
        }
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        loaded = false;
    }
    lua_close(state);
    output.path = path;
    output.source = std::move(source);
    output.loaded = loaded;
    return loaded;
}

bool changed_on_disk(const Document& document) {
    std::string current{};
    return !document.path.empty() && read_file(document.path, current)
           && current != document.source;
}

std::string to_lua(const Document& document) {
    return document.header + "return " + write_value(document.root, 0, {}) + "\n";
}

bool save(Document& document, std::string& error) {
    if (changed_on_disk(document)) {
        error = "data.lua changed on disk since it was read (Revert reads it again)";
        return false;
    }
    const std::string text = to_lua(document);
    // Written beside the file first, so a failed write never leaves half a mission.
    const std::wstring temporary = document.path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "the file cannot be written";
        return false;
    }
    DWORD written = 0;
    const bool complete =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
        && written == text.size();
    CloseHandle(file);
    if (!complete || MoveFileExW(temporary.c_str(), document.path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                         == FALSE) {
        DeleteFileW(temporary.c_str());
        error = "the file cannot be replaced";
        return false;
    }
    document.source = text;
    return true;
}

} // namespace sunrise::server::ui::mission_editor::document
