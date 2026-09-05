#pragma once

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Ir/Hir/Hir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirConstantPropagation.h"
#include "Optimization/LirDeadCodeElimination.h"
#include "Optimization/LirReachability.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <memory>
#include <string_view>

namespace Rux::Testing::OptimizerTestSupport {
using namespace Rux;

inline HirPackage CompileToHir(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", "windows");
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    return hirLowering.Generate();
}

inline HirPackage CompileAndOptimize(const std::string &source) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return package;
}

inline LirPackage CompileToLir(const std::string &source, const BuildProfile profile) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(profile);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return HirToLirLowering(std::move(package), TargetContext::CreateNative()).Generate();
}

class RecordingHirPass final : public Optimization::HirPass {
public:
    RecordingHirPass(std::string_view inputName, std::vector<std::string_view> &inputRuns, std::size_t inputChanges)
        : name(inputName)
        , runs(inputRuns)
        , changesRemaining(inputChanges) {
    }

    [[nodiscard]] std::string_view Name() const noexcept override {
        return name;
    }

    Optimization::PassChange Run(HirPackage &, const Optimization::PassContext &context) override {
        runs.push_back(name);
        contexts.push_back(context);
        if (changesRemaining == 0) {
            return Optimization::PassChange::None;
        }
        --changesRemaining;
        return Optimization::PassChange::Changed;
    }

    std::vector<Optimization::PassContext> contexts;

private:
    std::string_view name;
    std::vector<std::string_view> &runs;
    std::size_t changesRemaining;
};

inline LirTerminator JumpTo(const std::uint32_t target) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Jump;
    terminator.trueTarget = target;
    return terminator;
}

inline LirTerminator BranchTo(const LirReg condition, const std::uint32_t trueTarget, const std::uint32_t falseTarget) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Branch;
    terminator.cond = condition;
    terminator.trueTarget = trueTarget;
    terminator.falseTarget = falseTarget;
    return terminator;
}

inline LirTerminator ReturnValue(const std::optional<LirReg> value = std::nullopt, TypeRef type = {}) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Return;
    terminator.retVal = value;
    terminator.retType = std::move(type);
    return terminator;
}

inline LirTerminator UnreachableTerminator() {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Unreachable;
    return terminator;
}

inline LirFunc ReturningFunction(std::string name) {
    LirFunc function;
    function.name = std::move(name);
    function.blocks.resize(1);
    function.blocks[0].label = "entry";
    function.blocks[0].term = ReturnValue();
    return function;
}

inline LirFunc ExternFunction(std::string name, const bool isPublic = false) {
    LirFunc function;
    function.name = std::move(name);
    function.isPublic = isPublic;
    function.isExtern = true;
    return function;
}

inline LirConstDecl Constant(std::string name, const bool isPublic = false, std::vector<std::string> elements = {}) {
    LirConstDecl constant;
    constant.name = std::move(name);
    constant.isPublic = isPublic;
    constant.elements = std::move(elements);
    return constant;
}

inline bool HasRcuSymbol(const std::vector<RcuFile> &files, const std::string_view name) {
    return std::ranges::any_of(files, [name](const RcuFile &file) {
        return std::ranges::any_of(file.symbols, [name](const RcuSymbol &symbol) { return symbol.name == name; });
    });
}

} // namespace Rux::Testing::OptimizerTestSupport
