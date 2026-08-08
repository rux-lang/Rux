#pragma once

// Reader for the JSON documents the package registry answers with.
//
// The registry's resolver index nests arrays inside objects inside the response
// envelope, which the flat key scanners the package commands used to carry
// cannot walk. One small parser over a closed value model serves every reader in
// the toolchain, so no third-party JSON dependency is needed.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux::System {
/// The six JSON value kinds.
enum class JsonKind : std::uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

/**
 * @brief One parsed JSON value.
 *
 * Accessors never throw and never assert on a kind mismatch: a caller reading a
 * string out of a number gets an empty string, and a missing member is a null
 * pointer. Registry responses may gain members compatibly, so a reader that
 * tolerates the unexpected is the contract rather than a convenience.
 */
class JsonValue {
public:
    using Member = std::pair<std::string, JsonValue>;

    JsonValue() = default;

    [[nodiscard]] JsonKind Kind() const noexcept {
        return kind;
    }

    [[nodiscard]] bool IsNull() const noexcept {
        return kind == JsonKind::Null;
    }

    [[nodiscard]] bool IsArray() const noexcept {
        return kind == JsonKind::Array;
    }

    [[nodiscard]] bool IsObject() const noexcept {
        return kind == JsonKind::Object;
    }

    /// The boolean value, or `fallback` when this is not a boolean.
    [[nodiscard]] bool AsBool(bool fallback = false) const noexcept {
        return kind == JsonKind::Bool ? boolean : fallback;
    }

    /// The numeric value, or `fallback` when this is not a number.
    [[nodiscard]] double AsNumber(double fallback = 0.0) const noexcept {
        return kind == JsonKind::Number ? number : fallback;
    }

    /// The string value, or an empty string when this is not a string.
    [[nodiscard]] const std::string &AsString() const noexcept;

    /// The elements of an array, or an empty range for every other kind.
    [[nodiscard]] const std::vector<JsonValue> &Elements() const noexcept;

    /// The members of an object in document order, or an empty range otherwise.
    [[nodiscard]] const std::vector<Member> &Members() const noexcept;

    /// The member named `key`, or nullptr when absent or when this is not an object.
    [[nodiscard]] const JsonValue *Find(std::string_view key) const noexcept;

    /// The string member named `key`, or an empty string when it is absent or not a string.
    [[nodiscard]] const std::string &StringAt(std::string_view key) const noexcept;

    /// The boolean member named `key`, or `fallback` when it is absent or not a boolean.
    [[nodiscard]] bool BoolAt(std::string_view key, bool fallback = false) const noexcept;

private:
    friend class JsonParser;

    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> elements;
    std::vector<Member> members;
};

/// Why a document was rejected, located at the byte that rejected it.
struct JsonParseError {
    std::string message;
    std::size_t offset = 0;
};

/// Bounds a document must respect. Both protect the parser from hostile input
/// rather than describing anything the registry legitimately sends.
inline constexpr std::size_t jsonMaxBytes = 16 * 1024 * 1024;
inline constexpr std::size_t jsonMaxDepth = 64;

/**
 * @brief Parse one complete JSON document.
 *
 * Trailing whitespace is allowed; trailing content is not.
 *
 * @param text The document bytes
 * @return The parsed value, or the reason the document was rejected
 */
[[nodiscard]] std::expected<JsonValue, JsonParseError> ParseJson(std::string_view text);

/**
 * @brief Find the first string-valued member named `key`, at any depth.
 *
 * Document order, outermost first. Kept because callers read one field out of a
 * response envelope or a problem document without modelling the whole shape.
 *
 * @return The value, or an empty string when the document has no such member.
 */
[[nodiscard]] std::string JsonLookupString(std::string_view json, std::string_view key);

/// One entry of an RFC 9457 problem document's "errors" array.
struct ProblemError {
    std::string code;
    std::string detail;
};

/**
 * @brief Read the "errors" array of a problem+json document.
 * @return The entries, or an empty vector when the document carries no such array.
 */
[[nodiscard]] std::vector<ProblemError> JsonFindProblemErrors(std::string_view json);
} // namespace Rux::System
