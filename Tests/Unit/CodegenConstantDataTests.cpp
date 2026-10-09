// Serialization of scalar constants into initialized data, at every width the back ends store.

#include "CodeGen/ConstantData.h"

#include <cstdint>
#include <doctest.h>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] std::vector<std::uint8_t> Bytes(const std::string_view literal, const TypeRef::Kind kind) {
    std::vector<std::uint8_t> data;
    AppendScalarConstant(data, literal, TypeRef::MakePrimitive(kind));
    return data;
}
} // namespace

TEST_CASE("narrow scalar constants keep their little-endian bytes") {
    CHECK(Bytes("0x1234", TypeRef::Kind::UInt16) == std::vector<std::uint8_t>{0x34, 0x12});
    CHECK(Bytes("-1", TypeRef::Kind::Int32) == std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF});
    CHECK(Bytes("true", TypeRef::Kind::Bool8) == std::vector<std::uint8_t>{1});
    CHECK(Bytes("1.0", TypeRef::Kind::Float32) == std::vector<std::uint8_t>{0x00, 0x00, 0x80, 0x3F});
}

TEST_CASE("wide integer constants are written word by word") {
    const auto value = Bytes("0x0102030405060708090A0B0C0D0E0F10", TypeRef::Kind::UInt128);
    REQUIRE(value.size() == 16);
    for (std::size_t index = 0; index < value.size(); ++index) {
        CHECK(value[index] == 16 - index);
    }
    const auto negative = Bytes("-2", TypeRef::Kind::Int256);
    REQUIRE(negative.size() == 32);
    CHECK(negative[0] == 0xFE);
    for (std::size_t index = 1; index < negative.size(); ++index) {
        CHECK(negative[index] == 0xFF);
    }
}

TEST_CASE("wide bool constants hold their value in the low byte and zeros above it") {
    for (const auto kind : {TypeRef::Kind::Bool128, TypeRef::Kind::Bool256, TypeRef::Kind::Bool512}) {
        const auto truth = Bytes("true", kind);
        const auto falsehood = Bytes("false", kind);
        REQUIRE(truth.size() == falsehood.size());
        CHECK(truth[0] == 1);
        CHECK(falsehood[0] == 0);
        for (std::size_t index = 1; index < truth.size(); ++index) {
            CHECK(truth[index] == 0);
            CHECK(falsehood[index] == 0);
        }
    }
}

TEST_CASE("software float constants are written as their encoding") {
    CHECK(Bytes("1.0", TypeRef::Kind::Float16) == std::vector<std::uint8_t>{0x00, 0x3C});
    CHECK(Bytes("-2.0", TypeRef::Kind::Float8) == std::vector<std::uint8_t>{0xC0});

    const auto quad = Bytes("1.0", TypeRef::Kind::Float128);
    REQUIRE(quad.size() == 16);
    CHECK(quad[15] == 0x3F);
    CHECK(quad[14] == 0xFF);
    for (std::size_t index = 0; index < 14; ++index) {
        CHECK(quad[index] == 0);
    }

    // float80 stores its eighty value bits in sixteen bytes; the six above them are padding and stay zero.
    const auto extended = Bytes("1.0", TypeRef::Kind::Float80);
    REQUIRE(extended.size() == 16);
    CHECK(extended[7] == 0x80);
    CHECK(extended[8] == 0xFF);
    CHECK(extended[9] == 0x3F);
    for (std::size_t index = 10; index < 16; ++index) {
        CHECK(extended[index] == 0);
    }
}
