#pragma once

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Rux::Testing::MoveConsumptionTestSupport {
using namespace Rux;

inline std::vector<SemanticDiagnostic> AnalyzeConsumptionDiagnostics(const std::string &source) {
    Lexer lexer(source, "move_consumption.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_consumption.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

inline HirPackage LowerConsumptionHir(const std::string &source) {
    Lexer lexer(source, "move_cleanup.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_cleanup.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

inline const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

inline LirPackage LowerConsumptionLir(const std::string &source) {
    Lexer lexer(source, "copy_lowering.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "copy_lowering.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), CompileTimeContext{}.target);
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

inline const LirFunc &RequireLirFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered LIR function " << name);
    throw std::runtime_error("missing lowered LIR function");
}

inline std::size_t LirCallCount(const LirFunc &function, const std::string &symbol) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call && instruction.strArg == symbol) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace Rux::Testing::MoveConsumptionTestSupport
