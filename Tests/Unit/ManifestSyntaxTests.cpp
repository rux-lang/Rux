#include "Package/ManifestSyntax.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux::ManifestDetail;

namespace {
Document Parsed(const std::string_view text) {
    auto result = ParseManifestSyntax(text);
    if (!result) {
        REQUIRE_MESSAGE(result.has_value(), "expected manifest syntax to parse: ", result.error().message);
    }
    return std::move(*result);
}

SyntaxError Rejected(const std::string_view text) {
    auto result = ParseManifestSyntax(text);
    REQUIRE_MESSAGE(!result.has_value(), "expected manifest syntax to be rejected");
    return std::move(result.error());
}
} // namespace

TEST_CASE("Manifest syntax retains tables, values and source locations") {
    const Document document = Parsed(R"(# Leading trivia.
[Manifest]
Version = +1
Name = "quote \" slash \\ tab \t unicode \u00E9"
Enabled = true
Items = [
    "first",
    -2,
    false,
    { Path = "../Package", Count = 3 },
]

[Build.Defines]
Channel = "Nightly"
)");

    REQUIRE(document.tables.size() == 2);
    const Table &manifest = document.tables[0];
    CHECK(manifest.name == "Manifest");
    CHECK(manifest.location.line == 2);
    CHECK(manifest.location.column == 1);
    REQUIRE(manifest.entries.size() == 4);

    const KeyValue &version = manifest.entries[0];
    CHECK(version.key == "Version");
    CHECK(version.keyLocation.line == 3);
    CHECK(version.value->kind == Value::Kind::Integer);
    CHECK(version.value->integer == 1);
    CHECK(version.value->location.column == 11);

    const Value &name = *manifest.entries[1].value;
    CHECK(name.kind == Value::Kind::String);
    CHECK(name.text == "quote \" slash \\ tab \t unicode \xc3\xa9");

    const Value &enabled = *manifest.entries[2].value;
    CHECK(enabled.kind == Value::Kind::Boolean);
    CHECK(enabled.boolean);

    const Value &items = *manifest.entries[3].value;
    CHECK(items.kind == Value::Kind::Array);
    REQUIRE(items.array.size() == 4);
    CHECK(items.array[0]->kind == Value::Kind::String);
    CHECK(items.array[0]->text == "first");
    CHECK(items.array[1]->kind == Value::Kind::Integer);
    CHECK(items.array[1]->integer == -2);
    CHECK(items.array[2]->kind == Value::Kind::Boolean);
    CHECK_FALSE(items.array[2]->boolean);

    const Value &inlineTable = *items.array[3];
    CHECK(inlineTable.kind == Value::Kind::InlineTable);
    REQUIRE(inlineTable.table.size() == 2);
    CHECK(inlineTable.table[0].key == "Path");
    CHECK(inlineTable.table[0].value->text == "../Package");
    CHECK(inlineTable.table[1].key == "Count");
    CHECK(inlineTable.table[1].value->integer == 3);

    CHECK(document.tables[1].name == "Build.Defines");
    CHECK(document.tables[1].location.line == 13);
}

TEST_CASE("Manifest syntax preserves duplicate sections for schema validation") {
    const Document document = Parsed("[Build]\nOutput = \"One\"\n\n[Build]\nOutput = \"Two\"\n");
    REQUIRE(document.tables.size() == 2);
    CHECK(document.tables[0].name == "Build");
    CHECK(document.tables[1].name == "Build");
    CHECK(document.tables[1].location.line == 4);
}

TEST_CASE("Manifest syntax rejects duplicate keys where they are declared") {
    SUBCASE("table key") {
        const SyntaxError error = Rejected("[Package]\nName = \"One\"\nName = \"Two\"\n");
        CHECK(error.location.line == 3);
        CHECK(error.location.column == 1);
        CHECK(error.message == "duplicate key 'Name' in table '[Package]'");
    }

    SUBCASE("inline-table key") {
        const SyntaxError error = Rejected("[Dependencies]\nIo = { Path = \"A\", Path = \"B\" }\n");
        CHECK(error.location.line == 2);
        CHECK(error.location.column == 20);
        CHECK(error.message == "duplicate key 'Path'");
    }
}

TEST_CASE("Manifest syntax failures retain their original location and message") {
    SUBCASE("key before a table") {
        const SyntaxError error = Rejected("Version = 1\n[Manifest]\nVersion = 1\n");
        CHECK(error.location.line == 1);
        CHECK(error.location.column == 1);
        CHECK(error.message == "expected a table header such as '[Manifest]'");
    }

    SUBCASE("unterminated string") {
        const SyntaxError error = Rejected("[Package]\nName = \"open\n");
        CHECK(error.location.line == 2);
        CHECK(error.location.column == 8);
        CHECK(error.message == "unterminated string");
    }

    SUBCASE("unsupported number syntax") {
        const SyntaxError error = Rejected("[Build.Defines]\nRatio = 1.5\n");
        CHECK(error.location.line == 2);
        CHECK(error.location.column == 9);
        CHECK(error.message == "expected an integer");
    }

    SUBCASE("integer range") {
        const SyntaxError error = Rejected("[Build.Defines]\nCount = 9223372036854775808\n");
        CHECK(error.location.line == 2);
        CHECK(error.location.column == 9);
        CHECK(error.message == "integer is out of range");
    }

    SUBCASE("Unicode scalar range") {
        const SyntaxError error = Rejected("[Package]\nName = \"\\uD800\"\n");
        CHECK(error.location.line == 2);
        CHECK(error.location.column == 10);
        CHECK(error.message == "escape is not a Unicode scalar value");
    }
}
