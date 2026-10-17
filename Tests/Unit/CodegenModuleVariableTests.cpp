// Module variable storage: both emitters place an initial value with a non-zero byte in initialized data and an
// all-zero one in zero-fill storage, at the type's size and alignment, under a writable data symbol that code reaches
// through a relocation.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/ConstantData.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"

#include <cstdint>
#include <doctest.h>
#include <span>
#include <string>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] std::vector<std::uint8_t> StoredBytes(const RcuFile &object, const RcuSymbol &symbol) {
    for (const auto &section : object.sections) {
        if (&section - object.sections.data() == symbol.sectionIdx) {
            const auto begin = section.data.begin() + symbol.value;
            return {begin, begin + symbol.size};
        }
    }
    return {};
}

[[nodiscard]] std::vector<std::uint8_t> Scalar(const std::string_view literal, const TypeRef::Kind kind) {
    std::vector<std::uint8_t> data;
    AppendScalarConstant(data, literal, TypeRef::MakePrimitive(kind));
    return data;
}

constexpr const char *Source = R"(
    var Zero: int64 = 0;
    pub var Seed: uint32 = 0xDEADBEEF;
    var Wide: int128 = -2i128;
    var Buffer: byte[64] = [0; 64];
    pub func Touch() -> int64 {
        Zero = Zero + 1;
        Buffer[3] = 7;
        Wide = Wide + 1i128;
        return Zero + Seed as int64;
    }
    func Main() -> int { return Touch() as int; }
)";
} // namespace

TEST_CASE("both emitters place module variables by their initial bytes") {
    const auto package = Rux::Testing::CompileToAArch64Lir(Source, "linux-x86_64");
    const auto x86Objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    const auto armObjects = AArch64RcuEmitter(package, "test").Generate();
    REQUIRE_FALSE(x86Objects.empty());
    REQUIRE_FALSE(armObjects.empty());
    for (const auto *object : {&x86Objects.front(), &armObjects.front()}) {
        const auto *zero = Rux::Testing::FindSymbol(*object, "test::Zero");
        REQUIRE(zero != nullptr);
        CHECK(zero->sectionIdx == RCU_BSS_IDX);
        CHECK(zero->kind == RcuSymKind::Data);
        CHECK(zero->size == 8);
        CHECK(zero->value % 8 == 0);

        const auto *buffer = Rux::Testing::FindSymbol(*object, "test::Buffer");
        REQUIRE(buffer != nullptr);
        CHECK(buffer->sectionIdx == RCU_BSS_IDX);
        CHECK(buffer->size == 64);

        const auto *seed = Rux::Testing::FindSymbol(*object, "test::Seed");
        REQUIRE(seed != nullptr);
        CHECK(seed->sectionIdx == RCU_DATA_IDX);
        CHECK(seed->kind == RcuSymKind::Data);
        CHECK(seed->visibility != RcuSymVis::Local);
        CHECK(StoredBytes(*object, *seed) == std::vector<std::uint8_t>{0xEF, 0xBE, 0xAD, 0xDE});

        const auto *wide = Rux::Testing::FindSymbol(*object, "test::Wide");
        REQUIRE(wide != nullptr);
        CHECK(wide->sectionIdx == RCU_DATA_IDX);
        CHECK(wide->value % 16 == 0);
        CHECK(StoredBytes(*object, *wide) == Scalar("-2", TypeRef::Kind::Int128));

        // Code reaches the storage through a relocation, never a copy of its value.
        CHECK_FALSE(Rux::Testing::RelocsFor(*object, RCU_TEXT_IDX, "test::Zero").empty());
        CHECK_FALSE(Rux::Testing::RelocsFor(*object, RCU_TEXT_IDX, "test::Buffer").empty());

        // The zero-fill section is the last one, holds only zeros and is writable.
        REQUIRE(object->sections.size() == 4);
        const auto &bss = object->sections[RCU_BSS_IDX];
        CHECK(bss.name == ".bss");
        CHECK(bss.type == RcuSecType::Bss);
        CHECK((bss.flags & RcuSecFlag::Write) != 0);
        for (const std::uint8_t byte : bss.data) {
            CHECK(byte == 0);
        }
    }
}

TEST_CASE("a module without zero-initialized variables keeps three sections") {
    const auto package = Rux::Testing::CompileToAArch64Lir(R"(
        var Seed: uint32 = 9;
        func Main() -> int { Seed = Seed + 1; return Seed as int; }
    )",
                                                           "linux-x86_64");
    const auto objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_FALSE(objects.empty());
    CHECK(objects.front().sections.size() == 3);
}

TEST_CASE("the assembly printer writes module variables to data and zero-fill sections") {
    const auto package = Rux::Testing::CompileToAArch64Lir(Source, "linux-x86_64");
    AssemblyModulePrinter printer(Target::OS::Linux);
    printer.EmitModuleData(package.modules.front());
    const auto assembly = printer.Finalize();
    CHECK(assembly.find("section .bss") != std::string::npos);
    CHECK(assembly.find("test::Zero:  ; var int64\n    resb  8") != std::string::npos);
    CHECK(assembly.find("global test::Seed") != std::string::npos);
    CHECK(assembly.find("0xef, 0xbe, 0xad, 0xde") != std::string::npos);
}
