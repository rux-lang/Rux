#include "System/Process.h"

#include <doctest.h>

using namespace Rux::System;

TEST_CASE("JsonLookupString finds string values in flat objects") {
    constexpr const char *json = R"({ "name": "Std", "repository": "https://example.com/std" })";
    CHECK(JsonLookupString(json, "name") == "Std");
    CHECK(JsonLookupString(json, "repository") == "https://example.com/std");
    CHECK(JsonLookupString(json, "missing").empty());
}

TEST_CASE("JsonLookupString tolerates whitespace around the colon") {
    CHECK(JsonLookupString(R"({ "key"  :   "value" })", "key") == "value");
}

TEST_CASE("JsonLookupString ignores keys without string values") {
    CHECK(JsonLookupString(R"({ "key": 42 })", "key").empty());
}

TEST_CASE("JsonFindPackageRepository selects the matching array entry") {
    constexpr const char *index = R"([
        { "name": "Std", "repository": "https://example.com/std" },
        { "name": "Json", "repository": "https://example.com/json" }
    ])";
    CHECK(JsonFindPackageRepository(index, "Std") == "https://example.com/std");
    CHECK(JsonFindPackageRepository(index, "Json") == "https://example.com/json");
    CHECK(JsonFindPackageRepository(index, "Missing").empty());
}

TEST_CASE("JsonFindPackageRepository is not confused by braces inside strings") {
    constexpr const char *index = R"([
        { "name": "Weird", "description": "has { braces } and \"quotes\"", "repository": "https://example.com/weird" }
    ])";
    CHECK(JsonFindPackageRepository(index, "Weird") == "https://example.com/weird");
}
