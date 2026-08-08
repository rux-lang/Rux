#include "System/Json.h"

#include <doctest.h>
#include <string>

using namespace Rux::System;

TEST_CASE("ParseJson reads the scalar kinds") {
    const auto document = ParseJson(R"({ "n": null, "t": true, "f": false, "i": 42, "s": "text" })");
    REQUIRE(document.has_value());
    REQUIRE(document->IsObject());

    REQUIRE(document->Find("n") != nullptr);
    CHECK(document->Find("n")->IsNull());
    CHECK(document->BoolAt("t"));
    CHECK_FALSE(document->BoolAt("f", true));
    REQUIRE(document->Find("i") != nullptr);
    CHECK(document->Find("i")->AsNumber() == doctest::Approx(42.0));
    CHECK(document->StringAt("s") == "text");
}

TEST_CASE("ParseJson reads numbers with fractions and exponents") {
    const auto document = ParseJson(R"([0, -1, 1.5, -2.25, 1e3, 2.5E-2])");
    REQUIRE(document.has_value());
    REQUIRE(document->Elements().size() == 6);
    CHECK(document->Elements()[0].AsNumber() == doctest::Approx(0.0));
    CHECK(document->Elements()[1].AsNumber() == doctest::Approx(-1.0));
    CHECK(document->Elements()[2].AsNumber() == doctest::Approx(1.5));
    CHECK(document->Elements()[3].AsNumber() == doctest::Approx(-2.25));
    CHECK(document->Elements()[4].AsNumber() == doctest::Approx(1000.0));
    CHECK(document->Elements()[5].AsNumber() == doctest::Approx(0.025));
}

TEST_CASE("ParseJson walks nested arrays and objects") {
    const auto document = ParseJson(R"({
        "data": {
            "versions": [
                { "version": "1.0.0", "dependencies": [ { "alias": "Json" } ] },
                { "version": "1.1.0", "dependencies": [] }
            ]
        }
    })");
    REQUIRE(document.has_value());

    const JsonValue *data = document->Find("data");
    REQUIRE(data != nullptr);
    const JsonValue *versions = data->Find("versions");
    REQUIRE(versions != nullptr);
    REQUIRE(versions->IsArray());
    REQUIRE(versions->Elements().size() == 2);
    CHECK(versions->Elements()[0].StringAt("version") == "1.0.0");

    const JsonValue *dependencies = versions->Elements()[0].Find("dependencies");
    REQUIRE(dependencies != nullptr);
    REQUIRE(dependencies->Elements().size() == 1);
    CHECK(dependencies->Elements()[0].StringAt("alias") == "Json");
    CHECK(versions->Elements()[1].Find("dependencies")->Elements().empty());
}

TEST_CASE("ParseJson decodes string escapes") {
    const auto document = ParseJson(R"(["a\"b", "c\\d", "e\/f", "g\th\r\n", "Aé€", "😀"])");
    REQUIRE(document.has_value());
    const auto &items = document->Elements();
    REQUIRE(items.size() == 6);
    CHECK(items[0].AsString() == "a\"b");
    CHECK(items[1].AsString() == "c\\d");
    CHECK(items[2].AsString() == "e/f");
    CHECK(items[3].AsString() == "g\th\r\n");
    // A \u escape becomes UTF-8; the two- and three-byte forms differ.
    CHECK(items[4].AsString() == "A\xC3\xA9\xE2\x82\xAC");
    // A surrogate pair becomes the one code point it encodes, not two halves.
    CHECK(items[5].AsString() == "\xF0\x9F\x98\x80");
}

TEST_CASE("ParseJson replaces an unpaired surrogate rather than emitting invalid UTF-8") {
    const auto lone = ParseJson(R"(["\ud83d"])");
    REQUIRE(lone.has_value());
    CHECK(lone->Elements()[0].AsString() == "\xEF\xBF\xBD");

    const auto low = ParseJson(R"(["\ude00"])");
    REQUIRE(low.has_value());
    CHECK(low->Elements()[0].AsString() == "\xEF\xBF\xBD");
}

TEST_CASE("ParseJson accepts empty containers and surrounding whitespace") {
    CHECK(ParseJson("  {}  ").has_value());
    CHECK(ParseJson("\n[]\r\n").has_value());
    CHECK(ParseJson(R"({ "a": [], "b": {} })").has_value());
}

TEST_CASE("ParseJson rejects malformed documents") {
    CHECK_FALSE(ParseJson("").has_value());
    CHECK_FALSE(ParseJson("{").has_value());
    CHECK_FALSE(ParseJson(R"({ "a" 1 })").has_value());
    CHECK_FALSE(ParseJson(R"({ "a": 1, })").has_value());
    CHECK_FALSE(ParseJson(R"([1 2])").has_value());
    CHECK_FALSE(ParseJson(R"("unterminated)").has_value());
    CHECK_FALSE(ParseJson("tru").has_value());
    CHECK_FALSE(ParseJson("01").has_value()); // Trailing content after the number 0.
    CHECK_FALSE(ParseJson(R"({} {})").has_value());
    CHECK_FALSE(ParseJson(R"(["\q"])").has_value());
    CHECK_FALSE(ParseJson(R"(["\u00g0"])").has_value());
}

TEST_CASE("ParseJson bounds nesting depth") {
    std::string deep;
    for (std::size_t i = 0; i <= jsonMaxDepth + 1; ++i) {
        deep += '[';
    }
    for (std::size_t i = 0; i <= jsonMaxDepth + 1; ++i) {
        deep += ']';
    }
    CHECK_FALSE(ParseJson(deep).has_value());
}

TEST_CASE("Accessors are tolerant of the wrong kind") {
    const auto document = ParseJson(R"({ "n": 1, "s": "text" })");
    REQUIRE(document.has_value());
    CHECK(document->StringAt("n").empty());
    CHECK(document->StringAt("absent").empty());
    CHECK(document->Find("absent") == nullptr);
    CHECK(document->BoolAt("s", true));
    CHECK(document->Elements().empty());
    CHECK(document->Find("s")->Members().empty());
}

TEST_CASE("JsonLookupString finds a string at any depth, outermost first") {
    CHECK(JsonLookupString(R"({ "code": "not_found" })", "code") == "not_found");
    CHECK(JsonLookupString(R"({ "data": { "github_login": "octocat" } })", "github_login") == "octocat");
    CHECK(JsonLookupString(R"({ "errors": [ { "code": "inner" } ], "code": "outer" })", "code") == "outer");
    CHECK(JsonLookupString(R"({ "key": 42 })", "key").empty());
    CHECK(JsonLookupString(R"({ "key": "value" })", "missing").empty());
    CHECK(JsonLookupString("not json", "key").empty());
}

TEST_CASE("JsonFindProblemErrors reads the errors array") {
    constexpr const char *problem = R"({
        "type": "https://api.rux-lang.dev/problems/invalid_request",
        "code": "invalid_request",
        "detail": "One or more fields are invalid.",
        "errors": [
            { "code": "invalid_name", "detail": "must contain only lowercase letters", "pointer": "/name" },
            { "code": "invalid_version" }
        ]
    })";
    const auto errors = JsonFindProblemErrors(problem);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].code == "invalid_name");
    CHECK(errors[0].detail == "must contain only lowercase letters");
    CHECK(errors[1].code == "invalid_version");
    CHECK(errors[1].detail.empty());
}

TEST_CASE("JsonFindProblemErrors is empty without an errors array") {
    CHECK(JsonFindProblemErrors(R"({ "code": "version_conflict" })").empty());
    CHECK(JsonFindProblemErrors(R"({ "errors": {} })").empty());
    CHECK(JsonFindProblemErrors("not json").empty());
}
