#include "CodeGen/X86_64/RuntimeHelpers.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] RcuModuleBuilder MakeBuilder() {
    RcuModuleDescription description;
    description.arch = RcuArch::X86_64;
    description.sourcePath = "RuntimeHelpers.rux";
    description.packageName = "RuntimeHelperTests";
    return RcuModuleBuilder(std::move(description));
}

[[nodiscard]] const RcuSymbol *FindSymbol(const RcuFile &file, const std::string_view name) {
    const auto symbol = std::ranges::find(file.symbols, name, &RcuSymbol::name);
    return symbol == file.symbols.end() ? nullptr : &*symbol;
}
} // namespace

TEST_CASE("x86-64 runtime helper emitter writes nothing when no helper is reachable") {
    auto builder = MakeBuilder();
    X86_64RuntimeHelperEmitter helpers(builder, CallingConvention::SysV);

    helpers.EmitRequested();
    auto result = builder.Finalize();

    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    CHECK(result.file->symbols.empty());
    CHECK(result.file->sections[RCU_TEXT_IDX].data.empty());
    CHECK(result.file->sections[RCU_TEXT_IDX].relocs.empty());
}

TEST_CASE("x86-64 runtime helpers emit dependencies and bodies in deterministic order") {
    auto builder = MakeBuilder();
    X86_64RuntimeHelperEmitter helpers(builder, CallingConvention::SysV);
    static constexpr std::array<std::uint8_t, 5> Call = {0xE8, 0, 0, 0, 0};

    (void)builder.Append(RcuModuleSection::Text, Call);
    helpers.AddCallRelocation(1, X86_64RuntimeHelper::FloatPower32);
    (void)builder.Append(RcuModuleSection::Text, Call);
    helpers.AddCallRelocation(6, X86_64RuntimeHelper::IntegerPower);
    helpers.EmitRequested();
    auto result = builder.Finalize();

    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    const auto &file = *result.file;
    const RcuSymbol *integerPower = FindSymbol(file, "__rux_ipow");
    const RcuSymbol *floatPower64 = FindSymbol(file, "__rux_powf64");
    const RcuSymbol *floatPower32 = FindSymbol(file, "__rux_powf32");
    REQUIRE(integerPower != nullptr);
    REQUIRE(floatPower64 != nullptr);
    REQUIRE(floatPower32 != nullptr);
    CHECK(integerPower->value == Call.size() * 2);
    CHECK(floatPower64->value == integerPower->value + integerPower->size);
    CHECK(floatPower32->value == floatPower64->value + floatPower64->size);

    const auto &relocations = file.sections[RCU_TEXT_IDX].relocs;
    REQUIRE(relocations.size() == 3);
    CHECK(file.symbols[relocations[0].symbolIndex].name == "__rux_powf32");
    CHECK(file.symbols[relocations[1].symbolIndex].name == "__rux_ipow");
    CHECK(file.symbols[relocations[2].symbolIndex].name == "__rux_powf64");
    CHECK(relocations[2].sectionOffset >= floatPower32->value);
    CHECK(relocations[2].sectionOffset < floatPower32->value + floatPower32->size);

    static constexpr std::array<std::uint8_t, 46> SystemVIntegerPower = {
        0x48, 0x89, 0xF9, 0x48, 0x89, 0xF2, 0x48, 0x85, 0xD2, 0x78, 0x20, 0xB8, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xD2, 0x74, 0x18, 0x48, 0xF7, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x74, 0x04, 0x48, 0x0F,
        0xAF, 0xC1, 0x48, 0x0F, 0xAF, 0xC9, 0x48, 0xD1, 0xFA, 0xEB, 0xE5, 0x31, 0xC0, 0xC3,
    };
    const auto bodyBegin = file.sections[RCU_TEXT_IDX].data.begin() + integerPower->value;
    CHECK(
        std::equal(SystemVIntegerPower.begin(), SystemVIntegerPower.end(), bodyBegin, bodyBegin + integerPower->size));
}

TEST_CASE("x86-64 integer power helper preserves the Win64 body") {
    auto builder = MakeBuilder();
    X86_64RuntimeHelperEmitter helpers(builder, CallingConvention::Win64);
    static constexpr std::array<std::uint8_t, 5> Call = {0xE8, 0, 0, 0, 0};

    (void)builder.Append(RcuModuleSection::Text, Call);
    helpers.AddCallRelocation(1, X86_64RuntimeHelper::IntegerPower);
    helpers.EmitRequested();
    auto result = builder.Finalize();

    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    const RcuSymbol *integerPower = FindSymbol(*result.file, "__rux_ipow");
    REQUIRE(integerPower != nullptr);
    REQUIRE(integerPower->size == 40);
    const auto &text = result.file->sections[RCU_TEXT_IDX].data;
    CHECK(text[integerPower->value] == 0x48);
    CHECK(text[integerPower->value + 1] == 0x85);
    CHECK(text[integerPower->value + integerPower->size - 1] == 0xC3);
}
