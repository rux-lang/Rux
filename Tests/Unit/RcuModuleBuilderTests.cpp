#include "CodeGen/RcuModuleBuilder.h"
#include "Object/Rcu/RcuSerialization.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
RcuModuleDescription Description(const std::uint8_t arch = RcuArch::X86_64) {
    RcuModuleDescription description;
    description.arch = arch;
    description.sourcePath = "Source/Main.rux";
    description.packageName = "BuilderTests";
    description.buildTimestamp = 123456;
    description.ruxVersion = 0x01'02'03;
    description.compilerFlags = 7;
    description.sourceHash[0] = 0xA5;
    return description;
}

RcuSymbolDeclaration Function(std::string name) {
    return {.name = std::move(name), .typeName = "int", .kind = RcuSymKind::Func, .visibility = RcuSymVis::Global};
}

RcuSymbolDeclaration Constant(std::string name) {
    return {.name = std::move(name), .typeName = {}, .kind = RcuSymKind::Const, .visibility = RcuSymVis::Local};
}
} // namespace

TEST_CASE("RCU module builder finalizes fixed sections and metadata deterministically") {
    RcuModuleBuilder builder(Description(RcuArch::AArch64));

    const auto main = builder.DeclareSymbol(Function("Main"));
    REQUIRE(main == 0);
    REQUIRE(builder.BeginFunction(*main));
    constexpr std::array code = {std::uint8_t{0xC0}, std::uint8_t{0x03}, std::uint8_t{0x5F}, std::uint8_t{0xD6}};
    CHECK(builder.Append(RcuModuleSection::Text, code) == 0);
    REQUIRE(builder.EndFunction(*main));

    constexpr std::array literal = {std::uint8_t{'o'}, std::uint8_t{'k'}, std::uint8_t{0}};
    CHECK(builder.Append(RcuModuleSection::RoData, literal) == 0);
    const auto string = builder.AddDefinition(Constant("__str0"), RcuModuleSection::RoData, 0, literal.size());
    REQUIRE(string == 1);
    REQUIRE(builder.RecordInternedLiteral("string", "ok", *string));
    CHECK(builder.InternedLiteral("string", "ok") == string);
    CHECK_FALSE(builder.InternedLiteral("f64", "ok").has_value());

    constexpr std::array pointer = {std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0},
                                    std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}};
    CHECK(builder.Append(RcuModuleSection::Data, pointer) == 0);
    const auto slot = builder.AddDefinition(
        {.name = "Pointer", .typeName = {}, .kind = RcuSymKind::Data, .visibility = RcuSymVis::Local},
        RcuModuleSection::Data, 0, 8);
    REQUIRE(slot == 2);
    REQUIRE(builder.AddRelocation(RcuModuleSection::Data, 0, *main, RcuRelType::Abs64));

    const auto puts = builder.DeclareExternal("puts", RcuSymKind::ExternFunc, "libc");
    REQUIRE(puts == 3);
    CHECK(builder.DeclareExternal("puts", RcuSymKind::ExternFunc, "libc") == puts);

    auto result = builder.Finalize();
    REQUIRE_FALSE(result.HasErrors());
    REQUIRE(result.file.has_value());
    const auto &file = *result.file;
    CHECK(file.arch == RcuArch::AArch64);
    CHECK(file.flags == RcuFileFlag::HasMetadata);
    CHECK(file.hasMetadata);
    CHECK(file.sourcePath == "Source/Main.rux");
    CHECK(file.packageName == "BuilderTests");
    CHECK(file.buildTimestamp == 123456);
    CHECK(file.ruxVersion == 0x01'02'03);
    CHECK(file.compilerFlags == 7);
    CHECK(file.sourceHash[0] == 0xA5);

    REQUIRE(file.sections.size() == 3);
    CHECK(file.sections[RCU_TEXT_IDX].name == ".text");
    CHECK(file.sections[RCU_TEXT_IDX].alignment == 16);
    CHECK(file.sections[RCU_TEXT_IDX].data == std::vector(code.begin(), code.end()));
    CHECK(file.sections[RCU_RODATA_IDX].name == ".rodata");
    CHECK(file.sections[RCU_RODATA_IDX].alignment == 8);
    CHECK(file.sections[RCU_DATA_IDX].name == ".data");
    CHECK(file.sections[RCU_DATA_IDX].alignment == 8);
    REQUIRE(file.sections[RCU_DATA_IDX].relocs.size() == 1);
    CHECK(file.sections[RCU_DATA_IDX].relocs[0].symbolIndex == *main);

    REQUIRE(file.symbols.size() == 4);
    CHECK(file.symbols[*main].value == 0);
    CHECK(file.symbols[*main].size == code.size());
    CHECK(file.symbols[*puts].sectionIdx == RCU_SEC_EXTERNAL);
}

TEST_CASE("RCU module builder preserves the serialized metadata contract") {
    RcuModuleBuilder builder(Description());
    constexpr std::array code = {std::uint8_t{0x31}, std::uint8_t{0xC0}, std::uint8_t{0xC3}};
    CHECK(builder.Append(RcuModuleSection::Text, code) == 0);
    const auto main = builder.AddDefinition(Function("Main"), RcuModuleSection::Text, 0, code.size());
    REQUIRE(main == 0);
    auto built = builder.Finalize();
    REQUIRE(built.file.has_value());

    RcuFile legacy;
    legacy.arch = RcuArch::X86_64;
    legacy.flags = RcuFileFlag::HasMetadata;
    legacy.hasMetadata = true;
    legacy.sourcePath = "Source/Main.rux";
    legacy.packageName = "BuilderTests";
    legacy.buildTimestamp = 123456;
    legacy.ruxVersion = 0x01'02'03;
    legacy.compilerFlags = 7;
    legacy.sourceHash[0] = 0xA5;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    text.data.assign(code.begin(), code.end());
    legacy.sections.push_back(std::move(text));
    legacy.sections.push_back({.name = ".rodata",
                               .type = RcuSecType::RoData,
                               .flags = RcuSecFlag::Alloc | RcuSecFlag::Read,
                               .alignment = 8,
                               .data = {},
                               .relocs = {}});
    legacy.sections.push_back({.name = ".data",
                               .type = RcuSecType::Data,
                               .flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write,
                               .alignment = 8,
                               .data = {},
                               .relocs = {}});
    legacy.symbols.push_back({"Main", "int", 0, 3, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    CHECK(SerializeRcuFile(*built.file) == SerializeRcuFile(legacy));
}

TEST_CASE("RCU module builder rejects duplicate definitions and conflicting declarations early") {
    RcuModuleBuilder builder(Description());
    builder.SectionData(RcuModuleSection::Data).resize(8);
    const auto value = builder.DeclareSymbol(
        {.name = "Value", .typeName = {}, .kind = RcuSymKind::Data, .visibility = RcuSymVis::Global});
    REQUIRE(value.has_value());
    REQUIRE(builder.DefineSymbol(*value, RcuModuleSection::Data, 0, 8));
    CHECK_FALSE(builder.DefineSymbol(*value, RcuModuleSection::Data, 0, 8));
    CHECK_FALSE(builder.DeclareExternal("Value", RcuSymKind::ExternData).has_value());
    REQUIRE(builder.Diagnostics().size() == 2);
    CHECK(builder.Diagnostics()[0].message == "duplicate RCU symbol definition for 'Value'");
    CHECK(builder.Diagnostics()[1].message == "conflicting RCU symbol declaration for 'Value'");

    const auto result = builder.Finalize();
    CHECK(result.HasErrors());
    CHECK_FALSE(result.file.has_value());
}

TEST_CASE("RCU module builder rejects invalid relocation indices and offsets early") {
    RcuModuleBuilder builder(Description());
    builder.SectionData(RcuModuleSection::Text).resize(4);
    const auto target = builder.DeclareExternal("Target", RcuSymKind::ExternFunc);
    REQUIRE(target == 0);
    CHECK_FALSE(builder.AddRelocation(RcuModuleSection::Text, 0, 7, RcuRelType::Rel32));
    CHECK_FALSE(builder.AddRelocation(RcuModuleSection::Text, 4, *target, RcuRelType::Rel32));
    CHECK(builder.Relocations(RcuModuleSection::Text).empty());
    REQUIRE(builder.AddRelocation(RcuModuleSection::Text, 0, *target, RcuRelType::Rel32, -4));
    REQUIRE(builder.Relocations(RcuModuleSection::Text).size() == 1);
    CHECK(builder.Relocations(RcuModuleSection::Text)[0].addend == -4);
}

TEST_CASE("RCU module builder rejects unfinished internal symbols at finalization") {
    RcuModuleBuilder builder(Description());
    REQUIRE(builder.DeclareSymbol(Function("Pending")).has_value());
    const auto result = builder.Finalize();
    REQUIRE(result.HasErrors());
    CHECK_FALSE(result.file.has_value());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].message == "RCU symbol 'Pending' was declared but not defined");
}

TEST_CASE("RCU module builder aligns section buffers without owning literal encoding") {
    RcuModuleBuilder builder(Description());
    builder.SectionData(RcuModuleSection::RoData).push_back(0xFF);
    CHECK(builder.AlignSection(RcuModuleSection::RoData, 8) == 8);
    CHECK(builder.SectionData(RcuModuleSection::RoData) == std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0, 0, 0, 0});
    CHECK(builder.AlignSection(RcuModuleSection::RoData, 4) == 8);

    CHECK(builder.AlignSection(RcuModuleSection::RoData, 3) == 8);
    CHECK(builder.Diagnostics().back().message ==
          "cannot align RCU section '.rodata' to 3 bytes: alignment must be a power of two");
}

TEST_CASE("RCU module builder rejects unknown architectures and architecture-specific relocations") {
    RcuModuleBuilder unknown(Description(0xFF));
    const auto unknownResult = unknown.Finalize();
    REQUIRE(unknownResult.HasErrors());
    CHECK(unknownResult.diagnostics.front().message ==
          "cannot construct an RCU module for unknown architecture byte 255");

    RcuModuleBuilder x86(Description(RcuArch::X86_64));
    x86.SectionData(RcuModuleSection::Text).resize(4);
    const auto target = x86.DeclareExternal("Target", RcuSymKind::ExternFunc);
    REQUIRE(target.has_value());
    CHECK_FALSE(x86.AddRelocation(RcuModuleSection::Text, 0, *target, RcuRelType::AArch64Call26));
    CHECK(x86.Diagnostics().back().message == "RCU relocation 'AARCH64_CALL26' is not available for x86-64 modules");
}

TEST_CASE("RCU module builder reports relocation widths and owning sections") {
    RcuModuleBuilder builder(Description(RcuArch::AArch64));
    builder.SectionData(RcuModuleSection::Text).resize(3);
    builder.SectionData(RcuModuleSection::Data).resize(8);
    const auto target = builder.DeclareExternal("Target", RcuSymKind::ExternFunc);
    REQUIRE(target.has_value());

    CHECK_FALSE(builder.AddRelocation(RcuModuleSection::Text, 0, *target, RcuRelType::AArch64Call26));
    CHECK(builder.Diagnostics().back().message == "RCU relocation 'AARCH64_CALL26' for symbol 'Target' at offset 0 "
                                                  "needs 4 bytes in section '.text' of 3 bytes");

    CHECK_FALSE(builder.AddRelocation(RcuModuleSection::Data, 0, *target, RcuRelType::AArch64Call26));
    CHECK(builder.Diagnostics().back().message ==
          "AArch64 instruction relocation 'AARCH64_CALL26' requires section '.text', found '.data'");

    CHECK_FALSE(builder.AddRelocation(RcuModuleSection::Data, 0, *target, 999));
    CHECK(builder.Diagnostics().back().message ==
          "cannot add unknown RCU relocation type 999 in section '.data' for symbol 'Target'");
}

TEST_CASE("RCU module builder truncates speculative section data and relocations together") {
    RcuModuleBuilder builder(Description(RcuArch::AArch64));
    const auto target = builder.DeclareExternal("Target", RcuSymKind::ExternFunc);
    REQUIRE(target.has_value());
    builder.SectionData(RcuModuleSection::Text).resize(8);
    REQUIRE(builder.AddRelocation(RcuModuleSection::Text, 0, *target, RcuRelType::AArch64Call26));
    REQUIRE(builder.AddRelocation(RcuModuleSection::Text, 4, *target, RcuRelType::AArch64Call26));

    REQUIRE(builder.TruncateSection(RcuModuleSection::Text, 4, 1));
    CHECK(builder.SectionData(RcuModuleSection::Text).size() == 4);
    REQUIRE(builder.Relocations(RcuModuleSection::Text).size() == 1);
    CHECK(builder.Relocations(RcuModuleSection::Text).front().sectionOffset == 0);
    CHECK_FALSE(builder.TruncateSection(RcuModuleSection::Text, 5, 1));
    CHECK(builder.Diagnostics().back().message == "cannot grow an RCU section while truncating it");
}
