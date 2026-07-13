#include <array>
#include <doctest.h>
#include <string_view>

#include "Semantic/PrimitiveConstants.h"

using namespace Rux;

TEST_CASE("every primitive exposes its recommended constants") {
    const CompileTimeContext context;

    constexpr std::array widthTypes{"bool",   "bool8",  "bool16", "bool32", "char",  "char8",   "char16",
                                    "char32", "int",    "int8",   "int16",  "int32", "int64",   "uint",
                                    "uint8",  "uint16", "uint32", "uint64", "float", "float32", "float64"};
    for (const std::string_view type : widthTypes) {
        CAPTURE(type);
        CHECK(LookupPrimitiveConstant(type, "Bits", context).has_value());
        CHECK(LookupPrimitiveConstant(type, "Bytes", context).has_value());
    }

    constexpr std::array integerTypes{"int",  "int8",  "int16",  "int32",  "int64",
                                      "uint", "uint8", "uint16", "uint32", "uint64"};
    for (const std::string_view type : integerTypes) {
        CAPTURE(type);
        CHECK(LookupPrimitiveConstant(type, "Min", context).has_value());
        CHECK(LookupPrimitiveConstant(type, "Max", context).has_value());
    }

    constexpr std::array characterTypes{"char", "char8", "char16", "char32"};
    for (const std::string_view type : characterTypes) {
        CAPTURE(type);
        CHECK(LookupPrimitiveConstant(type, "Min", context).has_value());
        CHECK(LookupPrimitiveConstant(type, "Max", context).has_value());
    }

    constexpr std::array floatTypes{"float", "float32", "float64"};
    constexpr std::array floatConstants{"Lowest", "Max", "MinPositive", "Epsilon", "Infinity", "NaN"};
    for (const std::string_view type : floatTypes) {
        for (const std::string_view name : floatConstants) {
            CAPTURE(type);
            CAPTURE(name);
            CHECK(LookupPrimitiveConstant(type, name, context).has_value());
        }
    }

    CHECK_FALSE(LookupPrimitiveConstant("bool", "Min", context).has_value());
    CHECK_FALSE(LookupPrimitiveConstant("float64", "Min", context).has_value());
    CHECK_FALSE(LookupPrimitiveConstant("int32", "Infinity", context).has_value());
}

TEST_CASE("native integer constants follow the target pointer width") {
    CompileTimeContext context;
    context.target.pointer_size = 4;

    CHECK_EQ(LookupPrimitiveConstant("int", "Bits", context)->value, "32");
    CHECK_EQ(LookupPrimitiveConstant("int", "Bytes", context)->value, "4");
    CHECK_EQ(LookupPrimitiveConstant("int", "Min", context)->value, "-2147483648");
    CHECK_EQ(LookupPrimitiveConstant("int", "Max", context)->value, "2147483647");
    CHECK_EQ(LookupPrimitiveConstant("uint", "Max", context)->value, "4294967295");

    context.target.pointer_size = 8;
    CHECK_EQ(LookupPrimitiveConstant("int", "Bits", context)->value, "64");
    CHECK_EQ(LookupPrimitiveConstant("int", "Min", context)->value, "-9223372036854775808");
    CHECK_EQ(LookupPrimitiveConstant("uint", "Max", context)->value, "18446744073709551615");
}

TEST_CASE("primitive aliases expose constants with their canonical types") {
    const CompileTimeContext context;

    const auto boolean = LookupPrimitiveConstant("bool", "Bits", context);
    const auto character = LookupPrimitiveConstant("char", "Max", context);
    const auto floating = LookupPrimitiveConstant("float", "Epsilon", context);

    REQUIRE(boolean);
    REQUIRE(character);
    REQUIRE(floating);
    CHECK_EQ(boolean->value, "8");
    CHECK_EQ(character->type, TypeRef::MakeChar32());
    CHECK_EQ(character->value, "1114111");
    CHECK_EQ(floating->type, TypeRef::MakeFloat64());
}
