// Zeroization: what the intrinsic lowers to, and that the write survives the pass that removes stores nothing reads.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "zeroize.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "zeroize.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "zeroize.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "zeroize.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// Every store in a lowered function, in emission order.
std::vector<const LirInstr *> StoresOf(const LirPackage &package, const std::string &name) {
    std::vector<const LirInstr *> stores;
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name != name) {
                continue;
            }
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    if (instruction.op == LirOpcode::Store) {
                        stores.push_back(&instruction);
                    }
                }
            }
        }
    }
    return stores;
}

const std::string kZeroizePrelude = R"(
    intrinsic func Zeroize(memory: *var uint8, length: uint64);
)";
} // namespace

TEST_CASE("zeroization lowers to a byte loop whose writes are marked as required") {
    const HirPackage package = LowerSource(kZeroizePrelude + R"(
        func Clear(memory: *var uint8, length: uint64) {
            Zeroize(memory, length);
        }
    )");

    const HirFunc &clear = RequireFunction(package, "Clear");
    REQUIRE(clear.body.has_value());
    REQUIRE_EQ(clear.body->stmts.size(), 1);
    const auto *statement = dynamic_cast<const HirExprStmt *>(clear.body->stmts.front().get());
    REQUIRE(statement != nullptr);
    const auto *inlined = dynamic_cast<const HirBlockExpr *>(statement->expr.get());
    REQUIRE(inlined != nullptr);

    // The pointer, the length and the index are named once; the loop is what follows them.
    REQUIRE_EQ(inlined->block.stmts.size(), 4);
    const auto *loop = dynamic_cast<const HirWhileStmt *>(inlined->block.stmts[3].get());
    REQUIRE(loop != nullptr);
    REQUIRE_EQ(loop->body.stmts.size(), 2);
    const auto *write = dynamic_cast<const HirExprStmt *>(loop->body.stmts.front().get());
    REQUIRE(write != nullptr);
    const auto *assignment = dynamic_cast<const HirAssignExpr *>(write->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK(assignment->isVolatile);
    CHECK_EQ(assignment->type, TypeRef::MakeUInt8());
}

TEST_CASE("a zeroizing store survives dead-store elimination") {
    HirPackage hir = LowerSource(kZeroizePrelude + R"(
        func Clear() -> uint8 {
            var secret: uint8[4] = [1u8, 2u8, 3u8, 4u8];
            Zeroize(@secret[0], 4u64);
            return 0u8;
        }
    )");

    LirPackage lir = HirToLirLowering(std::move(hir), TargetContext::CreateNative()).Generate();
    const auto volatileCount = [](const std::vector<const LirInstr *> &stores) {
        return std::ranges::count_if(stores, [](const LirInstr *store) { return store->isVolatile; });
    };
    // Counted before the pipeline runs and compared by value afterwards: the passes rewrite the instruction
    // vectors, so pointers taken now would not survive to the comparison.
    const auto requiredStores = volatileCount(StoresOf(lir, "Clear"));
    REQUIRE_GT(requiredStores, 0);

    // Nothing reads the cleared bytes back, so without the marking the store is exactly what this pass removes.
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    const auto result = pipeline.RunLir(lir);
    REQUIRE(result.reachedFixedPoint);
    CHECK_EQ(volatileCount(StoresOf(lir, "Clear")), requiredStores);
}

TEST_CASE("a zeroization intrinsic declared with the wrong signature is rejected") {
    const auto wrongPointer = AnalyzeSource(R"(
        intrinsic func Zeroize(memory: *uint8, length: uint64);
    )");

    REQUIRE_EQ(wrongPointer.size(), 1);
    CHECK_EQ(wrongPointer[0].message, "zeroization intrinsic 'Zeroize' must be declared as "
                                      "'func Zeroize(memory: *var uint8, length: uint64)'");
    REQUIRE_EQ(wrongPointer[0].notes.size(), 1);
    CHECK_EQ(wrongPointer[0].notes[0], "parameter 'memory' has type '*uint8'");

    const auto wrongLength = AnalyzeSource(R"(
        intrinsic func Zeroize(memory: *var uint8, length: uint32);
    )");

    REQUIRE_EQ(wrongLength.size(), 1);
    CHECK_EQ(wrongLength[0].notes[0], "parameter 'length' has type 'uint32'");

    const auto returnsSomething = AnalyzeSource(R"(
        intrinsic func Zeroize(memory: *var uint8, length: uint64) -> bool;
    )");

    REQUIRE_EQ(returnsSomething.size(), 1);
    CHECK_EQ(returnsSomething[0].notes[0], "it returns 'bool8'");
}

TEST_CASE("a correctly declared zeroization intrinsic is accepted") {
    const auto diagnostics = AnalyzeSource(kZeroizePrelude);

    CHECK(diagnostics.empty());
}
