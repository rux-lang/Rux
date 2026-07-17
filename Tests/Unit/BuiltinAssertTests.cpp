#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Platform.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

namespace {

constexpr std::string_view AssertIntrinsics = R"(
    intrinsic func Assert(condition: bool, message: char8[]);

    intrinsic func DebugAssert(condition: bool, message: char8[]);
)";

LirPackage CompileToLir(const std::string &source, const bool debugAssertions) {
    Lexer lexer(std::string(AssertIntrinsics) + source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    CompileTimeContext context;
    context.debugAssertions = debugAssertions;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    auto model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering hirLowering(model);
    HirToLirLowering lirLowering(hirLowering.Generate());
    return lirLowering.Generate();
}

std::vector<SemanticDiagnostic> Analyze(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    return analyzer.Analyze().diagnostics;
}

std::vector<const LirInstr *> Assertions(const LirPackage &package) {
    std::vector<const LirInstr *> result;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                for (const auto &instruction : block.instrs) {
                    if (instruction.op == LirOpcode::Assert) {
                        result.push_back(&instruction);
                    }
                }
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("Assert and DebugAssert lower to traps when enabled") {
    auto package = CompileToLir(R"(
        func Main() -> int {
            var first = true;
            var second = true;
            Assert(first, "always");
            DebugAssert(second, "debug");
            return 0;
        }
    )",
                                true);

    const auto assertions = Assertions(package);
    REQUIRE_EQ(assertions.size(), 2);
    CHECK_EQ(assertions[0]->strArg, "always");
    CHECK_EQ(assertions[1]->strArg, "debug");
    CHECK_EQ(assertions[0]->srcs.size(), 2);
    CHECK_EQ(assertions[0]->sourceFile, "test.rux");
    CHECK_EQ(assertions[0]->sourceFunction, "Main");
    CHECK_NE(assertions[0]->sourceLine, 0);
    CHECK_NE(assertions[0]->sourceColumn, 0);

    const std::string assembly = AssemblyPrinter(package).Generate();
    CHECK_EQ(assembly.find("ud2") != std::string::npos, true);
    CHECK_EQ(assembly.find("assert_ok") != std::string::npos, true);
#if RUX_OS_WINDOWS
    CHECK_EQ(assembly.find("call    GetStdHandle") != std::string::npos, true);
    CHECK_EQ(assembly.find("call    WriteFile") != std::string::npos, true);
#else
    CHECK_EQ(assembly.find("syscall") != std::string::npos, true);
#endif

    const auto objects = RcuEmitter(package, "test", Target::OS::Windows).Generate();
    REQUIRE_FALSE(objects.empty());
    CHECK(std::ranges::any_of(objects.front().symbols,
                              [](const RcuSymbol &symbol) { return symbol.name == "GetStdHandle"; }));
    CHECK(std::ranges::any_of(objects.front().symbols,
                              [](const RcuSymbol &symbol) { return symbol.name == "WriteFile"; }));
}

TEST_CASE("release-mode DebugAssert is removed without evaluating arguments") {
    const auto package = CompileToLir(R"(
        func SideEffect() -> bool {
            return false;
        }

        func Main() -> int {
            var condition = true;
            Assert(condition, "always");
            DebugAssert(SideEffect(), "disabled");
            return 0;
        }
    )",
                                      false);

    const auto assertions = Assertions(package);
    REQUIRE_EQ(assertions.size(), 1);
    CHECK_EQ(assertions[0]->strArg, "always");

    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            if (function.name != "Main") {
                continue;
            }
            for (const auto &block : function.blocks) {
                for (const auto &instruction : block.instrs) {
                    CHECK_NE(instruction.strArg, "SideEffect");
                }
            }
        }
    }
}

TEST_CASE("assertion intrinsics require declarations and enforce their signatures") {
    const auto undefinedDiagnostics = Analyze(R"(
        func Main() { Assert(true, "missing import"); }
    )");
    CHECK(std::ranges::any_of(undefinedDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message == "undefined name 'Assert'";
    }));

    const auto signatureDiagnostics = Analyze(R"(
        intrinsic func Assert(condition: bool, message: char8[]);
        intrinsic func DebugAssert(condition: bool, message: char8[]);

        func Main() -> int {
            Assert("not bool", "condition");
            DebugAssert(true, 2);
            return 0;
        }
    )");
    CHECK(std::ranges::any_of(signatureDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message ==
               "no matching overload for 'Assert' with argument types (Slice<char8>, Slice<char8>)";
    }));
    CHECK(std::ranges::any_of(signatureDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message == "no matching overload for 'DebugAssert' with argument types (bool8, int)";
    }));
}
