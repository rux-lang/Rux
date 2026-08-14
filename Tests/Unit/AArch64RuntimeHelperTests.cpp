#include "CodeGen/AArch64/RuntimeHelpers.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] RcuModuleBuilder MakeBuilder() {
    RcuModuleDescription description;
    description.arch = RcuArch::AArch64;
    description.sourcePath = "RuntimeHelpers.rux";
    description.packageName = "RuntimeHelperTests";
    return RcuModuleBuilder(std::move(description));
}

[[nodiscard]] const RcuSymbol *FindSymbol(const RcuFile &file, const std::string_view name) {
    const auto symbol = std::ranges::find(file.symbols, name, &RcuSymbol::name);
    return symbol == file.symbols.end() ? nullptr : &*symbol;
}

[[nodiscard]] std::uint32_t ReadWord(const std::vector<std::uint8_t> &text, const std::size_t offset) {
    std::uint32_t word = 0;
    std::memcpy(&word, text.data() + offset, sizeof(word));
    return word;
}
} // namespace

TEST_CASE("AArch64 runtime helper emitter writes nothing when no helper is reachable") {
    auto builder = MakeBuilder();
    unsigned literalIndex = 0;
    std::vector<std::string> diagnostics;
    AArch64RuntimeHelperEmitter helpers(builder, literalIndex,
                                        [&](std::string message) { diagnostics.push_back(std::move(message)); });

    helpers.EmitRequested();
    auto result = builder.Finalize();

    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    CHECK(diagnostics.empty());
    CHECK(result.file->symbols.empty());
    CHECK(result.file->sections[RCU_TEXT_IDX].data.empty());
    CHECK(result.file->sections[RCU_TEXT_IDX].relocs.empty());
}

TEST_CASE("AArch64 runtime helpers emit dependencies and bodies in deterministic order") {
    auto builder = MakeBuilder();
    unsigned literalIndex = 0;
    std::vector<std::string> diagnostics;
    AArch64RuntimeHelperEmitter helpers(builder, literalIndex,
                                        [&](std::string message) { diagnostics.push_back(std::move(message)); });
    A64Enc enc(builder.SectionData(RcuModuleSection::Text));

    const std::uint32_t floatCall = enc.Size();
    REQUIRE(enc.Bl(0) == A64Status::Ok);
    helpers.AddCallRelocation(floatCall, AArch64RuntimeHelper::FloatPower32);
    const std::uint32_t integerCall = enc.Size();
    REQUIRE(enc.Bl(0) == A64Status::Ok);
    helpers.AddCallRelocation(integerCall, AArch64RuntimeHelper::IntegerPower);
    helpers.EmitRequested();
    auto result = builder.Finalize();

    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    CHECK(diagnostics.empty());
    const auto &file = *result.file;
    const RcuSymbol *integerPower = FindSymbol(file, "__rux_ipow");
    const RcuSymbol *floatPower32 = FindSymbol(file, "__rux_powf32");
    const RcuSymbol *floatPower64 = FindSymbol(file, "__rux_powf64");
    REQUIRE(integerPower != nullptr);
    REQUIRE(floatPower32 != nullptr);
    REQUIRE(floatPower64 != nullptr);
    CHECK(integerPower->value == 2 * A64Enc::InstrSize);
    CHECK(floatPower32->value == integerPower->value + integerPower->size);
    CHECK(floatPower64->value == floatPower32->value + floatPower32->size);

    const auto &relocations = file.sections[RCU_TEXT_IDX].relocs;
    REQUIRE(relocations.size() > 3);
    CHECK(file.symbols[relocations[0].symbolIndex].name == "__rux_powf32");
    CHECK(file.symbols[relocations[1].symbolIndex].name == "__rux_ipow");
    CHECK(file.symbols[relocations[2].symbolIndex].name == "__rux_powf64");
    CHECK(relocations[2].sectionOffset >= floatPower32->value);
    CHECK(relocations[2].sectionOffset < floatPower32->value + floatPower32->size);

    static constexpr std::array<std::uint32_t, 11> IntegerPowerWords = {
        0xAA0003E2, 0xD2800000, 0xB7F80101, 0xD2800020, 0xB40000C1, 0x36000041,
        0x9B027C00, 0x9B027C42, 0x9341FC21, 0x17FFFFFB, 0xD65F03C0,
    };
    const auto &text = file.sections[RCU_TEXT_IDX].data;
    REQUIRE(integerPower->size == IntegerPowerWords.size() * A64Enc::InstrSize);
    for (std::size_t index = 0; index < IntegerPowerWords.size(); ++index) {
        CHECK(ReadWord(text, integerPower->value + index * A64Enc::InstrSize) == IntegerPowerWords[index]);
    }
}
