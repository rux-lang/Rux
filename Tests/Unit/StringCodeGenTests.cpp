// What a string literal becomes in an object file: read-only data holding the text in the encoding the literal names,
// and a view whose length counts that encoding's code units. Both backends answer the same, since the shape is a
// property of the LIR type rather than of the machine.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
LirPackage CompileToLir(const std::string &source, const std::string_view triple) {
    CAPTURE(triple);
    Lexer lexer(source, "strings.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple));
    Parser parser(std::move(lexed.tokens), "strings.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    CompileTimeContext context;
    context.target = target;
    context.targetTriple = std::string(triple);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering hirLowering(model);
    auto hirPackage = hirLowering.Generate();
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(hirPackage).reachedFixedPoint);

    HirToLirLowering lirLowering(std::move(hirPackage), target);
    return lirLowering.Generate();
}

/// The one object each backend produces for `source`, so a case states its expectation once and checks it on both.
/// The emitters hold the package by reference, so it is named here rather than passed as a temporary.
RcuFile EmitX86_64(const std::string &source) {
    const LirPackage package = CompileToLir(source, "linux-x86_64");
    RcuEmitter emitter(package, "test", Target::OS::Linux);
    auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);
    return std::move(objects.front());
}

RcuFile EmitAArch64(const std::string &source) {
    const LirPackage package = CompileToLir(source, "linux-aarch64");
    AArch64RcuEmitter emitter(package, "test");
    auto objects = emitter.Generate();
    REQUIRE_EQ(objects.size(), 1);
    return std::move(objects.front());
}

/// 'A', U+00A2, U+20AC and U+1F680: one sample of each UTF-8 sequence width, the last of which needs a surrogate pair
/// in UTF-16. Written as escapes so the case does not depend on how this file itself is stored.
constexpr std::string_view Mixed = R"(A\u{A2}\u{20AC}\u{1F680})";

const std::vector<std::uint8_t> Utf8Bytes = {'A', 0xC2, 0xA2, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x9A, 0x80, 0};
const std::vector<std::uint8_t> Utf16Bytes = {0x41, 0x00, 0xA2, 0x00, 0xAC, 0x20, 0x3D, 0xD8, 0x80, 0xDE, 0x00, 0x00};
const std::vector<std::uint8_t> Utf32Bytes = {0x41, 0x00, 0x00, 0x00, 0xA2, 0x00, 0x00, 0x00, 0xAC, 0x20,
                                              0x00, 0x00, 0x80, 0xF6, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
} // namespace

TEST_CASE("a string literal's read-only data is transcoded into its own encoding") {
    const std::string source = std::format(R"(
        func Main() -> int {{
            let eight = s8"{0}";
            let sixteen = s16"{0}";
            let thirtyTwo = s32"{0}";
            return (eight.length + sixteen.length + thirtyTwo.length) as int;
        }}
    )",
                                           Mixed);

    for (const RcuFile &object : {EmitX86_64(source), EmitAArch64(source)}) {
        // The three literals interned in the order they appear, each with a terminator one whole code unit wide.
        CHECK_EQ(RodataOf(object, "__str0"), Utf8Bytes);
        CHECK_EQ(RodataOf(object, "__str1"), Utf16Bytes);
        CHECK_EQ(RodataOf(object, "__str2"), Utf32Bytes);
    }
}

TEST_CASE("the same text in two encodings is two distinct interned literals") {
    const std::string source = R"(
        func Main() -> int {
            let eight = s8"hi";
            let repeated = s8"hi";
            let sixteen = s16"hi";
            return (eight.length + repeated.length + sixteen.length) as int;
        }
    )";

    for (const RcuFile &object : {EmitX86_64(source), EmitAArch64(source)}) {
        // The repeat shares the first symbol, and the wider encoding does not: it is different bytes, so it is
        // different read-only data.
        CHECK_EQ(RodataOf(object, "__str0"), std::vector<std::uint8_t>{'h', 'i', 0});
        CHECK_EQ(RodataOf(object, "__str1"), std::vector<std::uint8_t>{'h', 0, 'i', 0, 0, 0});
        CHECK(FindSymbol(object, "__str2") == nullptr);
    }
}

TEST_CASE("a string constant publishes a transcoded body and a length in code units") {
    const std::string source = std::format(R"(
        const TEXT: string16 = s16"{0}";

        func Main() -> int {{
            return TEXT.length as int;
        }}
    )",
                                           Mixed);

    for (const RcuFile &object : {EmitX86_64(source), EmitAArch64(source)}) {
        CHECK_EQ(RodataOf(object, "TEXT$elements"), Utf16Bytes);

        // The header is a null data pointer the linker fills in, and a length that is already there. Five UTF-16
        // code units, not the ten bytes the same text takes in UTF-8.
        const auto header = RodataOf(object, "TEXT");
        REQUIRE_EQ(header.size(), 16);
        for (std::size_t index = 0; index < 8; ++index) {
            CHECK_EQ(header[index], 0);
        }
        CHECK_EQ(header[8], 5);
    }
}
