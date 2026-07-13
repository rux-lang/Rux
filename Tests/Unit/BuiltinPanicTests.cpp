#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <vector>

#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

using namespace Rux;

namespace {

LirPackage CompileToLir(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    CompileTimeContext context;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    const auto model = analyzer.Analyze();
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

const LirFunc *FindFunction(const LirPackage &package, const std::string_view name) {
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            if (function.name == name) {
                return &function;
            }
        }
    }
    return nullptr;
}

const LirInstr *FindInstruction(const LirFunc &function, const LirOpcode opcode) {
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op == opcode) {
                return &instruction;
            }
        }
    }
    return nullptr;
}

bool HasConstant(const LirFunc &function, const std::string_view value) {
    for (const auto &block : function.blocks) {
        if (std::ranges::any_of(block.instrs, [&](const LirInstr &instruction) {
                return instruction.op == LirOpcode::Const && instruction.strArg == value;
            })) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Panic is an import-free builtin with source context") {
    const auto package = CompileToLir(R"(
        func Main() {
            let message: char8[] = "unrecoverable state";
            Panic(message);
        }
    )");

    const LirFunc *main = FindFunction(package, "Main");
    REQUIRE(main != nullptr);
    const LirInstr *panic = FindInstruction(*main, LirOpcode::Panic);
    REQUIRE(panic != nullptr);
    CHECK_EQ(panic->srcs.size(), 1);
    CHECK_EQ(panic->sourceFile, "test.rux");
    CHECK_EQ(panic->sourceFunction, "Main");
    CHECK_NE(panic->sourceLine, 0);
    REQUIRE_FALSE(main->blocks.empty());
    REQUIRE(main->blocks.front().term.has_value());
    CHECK_EQ(main->blocks.front().term->kind, LirTermKind::Unreachable);

    const std::string assembly = AssemblyPrinter(package).Generate();
    CHECK(assembly.find("ud2") != std::string::npos);
}

TEST_CASE("#NoReturn marks functions and terminates caller control flow") {
    const auto package = CompileToLir(R"(
        #NoReturn()
        func Stop(message: char8[]) {
            Panic(message);
        }

        func Main() -> int {
            Stop("stop now");
            return 99;
        }
    )");

    const LirFunc *stop = FindFunction(package, "Stop");
    REQUIRE(stop != nullptr);
    CHECK(stop->isNoReturn);

    const LirFunc *main = FindFunction(package, "Main");
    REQUIRE(main != nullptr);
    REQUIRE_FALSE(main->blocks.empty());
    REQUIRE(main->blocks.front().term.has_value());
    CHECK_EQ(main->blocks.front().term->kind, LirTermKind::Unreachable);
    CHECK(FindInstruction(*main, LirOpcode::Call) != nullptr);
    CHECK_FALSE(HasConstant(*main, "99"));
}

TEST_CASE("Panic enforces its signature and reserves its name") {
    const auto redefinitionDiagnostics = Analyze(R"(
        func Panic(message: char8[]) {}
        func Main() {}
    )");
    CHECK(std::ranges::any_of(redefinitionDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.find("'Panic' is already defined") != std::string::npos;
    }));

    const auto signatureDiagnostics = Analyze(R"(
        func Main() {
            Panic(42);
        }
    )");
    CHECK(std::ranges::any_of(signatureDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.find("cannot pass 'int' to parameter of type 'Slice<char8>'") != std::string::npos;
    }));
}

TEST_CASE("#NoReturn rejects arguments, non-functions, return types, and duplicates") {
    Lexer lexer(R"(
        #NoReturn("invalid")
        const Value = 1;

        #NoReturn()
        func Typed() -> int { return 1; }

        #NoReturn()
        #NoReturn()
        func Duplicate() {}
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();
    REQUIRE(parsed.HasErrors());
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("does not accept arguments") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("can only be applied to a function") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("cannot declare a return type") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("duplicate '#NoReturn' attribute") != std::string::npos;
    }));

    const auto returnDiagnostics = Analyze(R"(
        #NoReturn()
        func Returns() {
            return;
        }
    )");
    CHECK(std::ranges::any_of(returnDiagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.find("return is not allowed in a '#NoReturn' function") != std::string::npos;
    }));
}
