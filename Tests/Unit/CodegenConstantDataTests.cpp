// Serialization of scalar constants into initialized data, at every width the back ends store.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/ConstantData.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"

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

TEST_CASE("wide character constants hold their code point in the low word and zeros above it") {
    for (const auto kind : {TypeRef::Kind::Char128, TypeRef::Kind::Char256, TypeRef::Kind::Char512}) {
        const auto value = Bytes("1114111", kind);
        REQUIRE(value.size() == static_cast<std::size_t>(TypeRef::MakePrimitive(kind).SizeInBytes().value_or(0)));
        CHECK(value[0] == 0xFF);
        CHECK(value[1] == 0xFF);
        CHECK(value[2] == 0x10);
        for (std::size_t index = 3; index < value.size(); ++index) {
            CHECK(value[index] == 0);
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

TEST_CASE("module scalar constants publish their complete aligned read-only value") {
    const auto package = Rux::Testing::CompileToAArch64Lir(R"(
        const PAD: int8 = -7i8;
        pub const WIDE: int128 = 0x0102030405060708090A0B0C0D0E0F10i128;
        pub func Address() -> *int128 { return @WIDE; }
    )",
                                                           "linux-x86_64");
    const auto objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_FALSE(objects.empty());
    const auto &object = objects.front();
    const auto *symbol = Rux::Testing::FindSymbol(object, "test::WIDE");
    REQUIRE(symbol != nullptr);
    CHECK(symbol->sectionIdx == RCU_RODATA_IDX);
    CHECK(symbol->size == 16);
    CHECK(symbol->value % 16 == 0);
    CHECK(symbol->visibility == RcuSymVis::Exported);
    CHECK(Rux::Testing::RodataOf(object, "test::WIDE") ==
          Bytes("0x0102030405060708090A0B0C0D0E0F10", TypeRef::Kind::Int128));
    CHECK_FALSE(Rux::Testing::RelocsFor(object, RCU_TEXT_IDX, "test::WIDE").empty());
    AssemblyModulePrinter printer(Target::OS::Linux);
    printer.EmitModuleData(package.modules.front());
    const auto assembly = printer.Finalize();
    CHECK(assembly.find("align 16\ntest::WIDE:") != std::string::npos);
    CHECK(assembly.find("0x10, 0x0f, 0x0e, 0x0d") != std::string::npos);
}

TEST_CASE("both emitters preserve every scalar module constant width") {
    const auto package = Rux::Testing::CompileToAArch64Lir(R"(
        const BYTE: int8 = -7i8;
        const WORD: int64 = -123456789i64;
        const WIDE: int128 = -2i128;
        const HUGE: uint512 = (1u512 << 500u32) + 37u512;
        const SINGLE: float32 = 1.5f32;
        const QUAD: float128 = -2.5f128;
        const EXTENDED: float80 = 1.0f80;
        const FLAG: bool256 = true;
        const CHARACTER: char128 = c128'😀';
        func Main() -> int { return 0; }
    )");
    const auto x86Objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    const auto armObjects = AArch64RcuEmitter(package, "test").Generate();
    for (const auto *object : {&x86Objects.front(), &armObjects.front()}) {
        for (const auto &constant : package.modules.front().consts) {
            const auto *symbol = Rux::Testing::FindSymbol(*object, constant.name);
            REQUIRE(symbol != nullptr);
            CHECK(symbol->sectionIdx == RCU_RODATA_IDX);
            CHECK(symbol->size == constant.type.SizeInBytes().value_or(0));
            CHECK(symbol->value % constant.type.AlignInBytes().value_or(1) == 0);
            CHECK(symbol->visibility == RcuSymVis::Local);
            REQUIRE(constant.scalars.size() == 1);
            CHECK(Rux::Testing::RodataOf(*object, constant.name) ==
                  Bytes(constant.scalars.front().literal, constant.type->kind));
        }
    }
}
