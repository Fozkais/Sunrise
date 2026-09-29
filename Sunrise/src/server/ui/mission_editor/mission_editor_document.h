#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sunrise::server::ui::mission_editor::document {

/**
 * One value of a mission kept as data: a Lua scalar or table, read from and written back to
 * `data.lua`. A table keeps its array part and its named fields apart, fields in file order.
 */
struct Value final {
    enum class Kind : std::uint8_t {
        nil,
        boolean,
        number,
        string,
        table,
    };

    Kind kind{Kind::nil};
    bool boolean{};
    double number{};
    /** The number came in as a Lua integer, so it is written back as one. */
    bool integer{};
    std::string text{};
    std::vector<Value> items{};
    std::vector<std::pair<std::string, Value>> fields{};

    [[nodiscard]] static Value of(std::string_view value);
    [[nodiscard]] static Value of(bool value);
    [[nodiscard]] static Value of_integer(std::int64_t value);
    [[nodiscard]] static Value table();

    [[nodiscard]] bool is_nil() const noexcept { return kind == Kind::nil; }
    [[nodiscard]] bool is_table() const noexcept { return kind == Kind::table; }
    [[nodiscard]] bool is_string() const noexcept { return kind == Kind::string; }
    /** @return The named field, or null. */
    [[nodiscard]] Value* get(std::string_view key) noexcept;
    [[nodiscard]] const Value* get(std::string_view key) const noexcept;
    /** @return The named field's text, or empty when it is missing or not a string. */
    [[nodiscard]] std::string_view text_of(std::string_view key) const noexcept;
    /** @return The named field, created as nil when missing. */
    Value& at(std::string_view key);
    /** Sets one named field; a nil value removes it. */
    void set(std::string_view key, Value value);
    void erase(std::string_view key);
};

/** A mission kept as data, with where it came from. */
struct Document final {
    Value root{};
    std::wstring path{};
    /** The comment lines at the head of the file, kept on save. */
    std::string header{};
    /** The file exactly as it was last read or written, to notice a change made elsewhere. */
    std::string source{};
    bool loaded{};
};

/**
 * Reads one Lua data file: a chunk that returns a table of scalars and tables. It runs in a
 * state with no library and no global.
 * @return False with `error` set when the file cannot be read or is not such a chunk.
 */
[[nodiscard]] bool load(const std::wstring& path, Document& output, std::string& error);

/**
 * Writes the document back as Lua data, keys in the campaign's field order. A file changed on
 * disk since it was read is left alone, so an edit made elsewhere is never overwritten.
 */
[[nodiscard]] bool save(Document& document, std::string& error);

/** @return True when the file on disk no longer holds what the document read or wrote. */
[[nodiscard]] bool changed_on_disk(const Document& document);

/** @return The document as Lua source, without writing it. */
[[nodiscard]] std::string to_lua(const Document& document);

} // namespace sunrise::server::ui::mission_editor::document
