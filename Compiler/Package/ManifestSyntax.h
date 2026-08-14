#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::ManifestDetail {
/**
 * @brief A 1-based location in manifest source.
 *
 * These types are the private syntax boundary between TOML parsing and the
 * public Version 1 manifest schema. They intentionally retain only the TOML
 * surface that Rux manifests support.
 */
struct Location {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

struct Value;

struct KeyValue {
    std::string key;
    Location keyLocation;
    std::unique_ptr<Value> value;
};

struct Value {
    enum class Kind {
        String,
        Integer,
        Boolean,
        Array,
        InlineTable,
    };

    Kind kind = Kind::String;
    Location location;
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<std::unique_ptr<Value>> array;
    std::vector<KeyValue> table;

    [[nodiscard]] std::string_view KindName() const noexcept;
};

struct Table {
    std::string name;
    Location location;
    std::vector<KeyValue> entries;
};

struct Document {
    std::vector<Table> tables;
};

struct SyntaxError {
    Location location;
    std::string message;
};

/**
 * @brief Parse the supported manifest TOML surface into a private value tree.
 *
 * Duplicate keys are syntax errors. Duplicate sections remain visible in the
 * document so Version 1 schema validation can diagnose them at the later
 * section's location.
 */
[[nodiscard]] std::expected<Document, SyntaxError> ParseManifestSyntax(std::string_view text);
} // namespace Rux::ManifestDetail
