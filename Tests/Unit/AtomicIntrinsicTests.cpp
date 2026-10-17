// Atomic intrinsics: the types and memory orders a call may use, what it lowers to, and that no optimization removes
// an atomic operation whose result nothing reads.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
// The declarations Core makes, inlined because no package is loaded here.
const std::string kAtomicPrelude = R"(
    enum MemoryOrder: uint8 { Relaxed = 0, Acquire = 1, Release = 2, AcquireRelease = 3, SequentiallyConsistent = 4 }
    intrinsic func AtomicLoad<T>(address: *T, order: MemoryOrder) -> T;
    intrinsic func AtomicStore<T>(address: *var T, value: T, order: MemoryOrder);
    intrinsic func AtomicExchange<T>(address: *var T, value: T, order: MemoryOrder) -> T;
    intrinsic func AtomicCompareExchange<T>(address: *var T, expected: T, desired: T, success: MemoryOrder,
                                            failure: MemoryOrder) -> T;
    intrinsic func AtomicFetchAdd<T>(address: *var T, value: T, order: MemoryOrder) -> T;
    intrinsic func AtomicFetchAnd<T>(address: *var T, value: T, order: MemoryOrder) -> T;
    intrinsic func Fence(order: MemoryOrder);
    intrinsic func SpinHint();
)";

std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(kAtomicPrelude + source, "atomics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "atomics.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

LirPackage LowerSource(const std::string &source) {
    Lexer lexer(kAtomicPrelude + source, "atomics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "atomics.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    HirPackage hir = AstToHirLowering(model).Generate();
    return HirToLirLowering(std::move(hir), TargetContext::CreateNative()).Generate();
}

std::vector<LirInstr> AtomicsOf(const LirPackage &package, const std::string &name) {
    std::vector<LirInstr> found;
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name != name) {
                continue;
            }
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    if (instruction.op >= LirOpcode::AtomicLoad) {
                        found.push_back(instruction);
                    }
                }
            }
        }
    }
    return found;
}

bool HasMessage(const std::vector<SemanticDiagnostic> &diagnostics, const std::string &fragment) {
    return std::ranges::any_of(diagnostics, [&](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.find(fragment) != std::string::npos;
    });
}
} // namespace

TEST_CASE("atomic intrinsics accept integers, bools and pointers of one machine word or less") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() -> int {
            var small: uint8 = 0;
            var flag: bool = false;
            var count: int64 = 0;
            var slot: *int64 = null;
            AtomicStore(@small, 1u8, MemoryOrder::Release);
            AtomicExchange(@flag, true, MemoryOrder::AcquireRelease);
            AtomicFetchAdd(@count, 1i64, MemoryOrder::Relaxed);
            AtomicFetchAnd(@flag, false, MemoryOrder::Relaxed);
            AtomicStore(@slot, @count as *int64, MemoryOrder::SequentiallyConsistent);
            Fence(MemoryOrder::Acquire);
            SpinHint();
            return AtomicLoad(@count, MemoryOrder::Acquire) as int;
        }
    )");
    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("atomic intrinsics refuse values no single instruction moves") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() -> int {
            var wide: int128 = 0;
            var real: float64 = 0.0;
            var flag: bool = false;
            AtomicLoad(@wide, MemoryOrder::Relaxed);
            AtomicExchange(@real, 1.0, MemoryOrder::Relaxed);
            AtomicFetchAdd(@flag, true, MemoryOrder::Relaxed);
            return 0;
        }
    )");
    CHECK(HasMessage(diagnostics, "'AtomicLoad' cannot act on type 'int128'"));
    CHECK(HasMessage(diagnostics, "'AtomicExchange' cannot act on type 'float64'"));
    CHECK(HasMessage(diagnostics, "'AtomicFetchAdd' cannot act on type 'bool8'"));
}

TEST_CASE("atomic intrinsics refuse memory orders the operation cannot have") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() -> int {
            var count: int64 = 0;
            let order = MemoryOrder::Relaxed;
            AtomicLoad(@count, MemoryOrder::Release);
            AtomicStore(@count, 1i64, MemoryOrder::Acquire);
            Fence(MemoryOrder::Relaxed);
            AtomicCompareExchange(@count, 0i64, 1i64, MemoryOrder::Relaxed, MemoryOrder::Acquire);
            AtomicCompareExchange(@count, 0i64, 1i64, MemoryOrder::SequentiallyConsistent, MemoryOrder::Release);
            AtomicLoad(@count, order);
            return 0;
        }
    )");
    CHECK(HasMessage(diagnostics, "'AtomicLoad' cannot use memory order 'Release'"));
    CHECK(HasMessage(diagnostics, "'AtomicStore' cannot use memory order 'Acquire'"));
    CHECK(HasMessage(diagnostics, "'Fence' cannot use memory order 'Relaxed'"));
    CHECK(HasMessage(diagnostics, "'AtomicCompareExchange' cannot use memory order 'Acquire'"));
    CHECK(HasMessage(diagnostics, "'AtomicCompareExchange' cannot use memory order 'Release'"));
    CHECK(HasMessage(diagnostics, "the memory order of 'AtomicLoad' must be written as a 'MemoryOrder' case"));
}

TEST_CASE("an atomic call in a generic body is checked for each instantiation") {
    const auto diagnostics = AnalyzeSource(R"(
        func Bump<T>(address: *var T, step: T) -> T {
            return AtomicFetchAdd(address, step, MemoryOrder::Relaxed);
        }
        func Main() -> int {
            var count: int32 = 0;
            var real: float32 = 0.0;
            Bump(@count, 1i32);
            Bump(@real, 1.0f32);
            return 0;
        }
    )");
    CHECK(HasMessage(diagnostics, "'AtomicFetchAdd' cannot act on type 'float32'"));
    CHECK_FALSE(HasMessage(diagnostics, "type 'int32'"));
}

TEST_CASE("atomic intrinsics lower to atomic instructions carrying their orders") {
    LirPackage lir = LowerSource(R"(
        func Main() -> int {
            var count: int64 = 0;
            AtomicStore(@count, 5i64, MemoryOrder::Release);
            let old = AtomicCompareExchange(@count, 5i64, 6i64, MemoryOrder::SequentiallyConsistent,
                                            MemoryOrder::Acquire);
            Fence(MemoryOrder::SequentiallyConsistent);
            return old as int;
        }
    )");
    const auto atomics = AtomicsOf(lir, "Main");
    REQUIRE_EQ(atomics.size(), 3);
    CHECK(atomics[0].op == LirOpcode::AtomicStore);
    CHECK(atomics[0].memoryOrder == 2);
    CHECK(atomics[0].srcs.size() == 2);
    CHECK(atomics[1].op == LirOpcode::AtomicCmpXchg);
    CHECK(atomics[1].memoryOrder == 4);
    CHECK(atomics[1].failureOrder == 1);
    CHECK(atomics[1].srcs.size() == 3);
    CHECK(atomics[2].op == LirOpcode::Fence);
    CHECK(atomics[2].memoryOrder == 4);
}

TEST_CASE("an atomic operation whose result nothing reads survives optimization") {
    LirPackage lir = LowerSource(R"(
        func Main() -> int {
            var count: int64 = 0;
            AtomicLoad(@count, MemoryOrder::Acquire);
            AtomicFetchAdd(@count, 1i64, MemoryOrder::Relaxed);
            SpinHint();
            return 0;
        }
    )");
    const auto before = AtomicsOf(lir, "Main").size();
    REQUIRE_EQ(before, 3);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    const auto result = pipeline.RunLir(lir);
    REQUIRE(result.reachedFixedPoint);
    CHECK_EQ(AtomicsOf(lir, "Main").size(), before);
}

TEST_CASE("AArch64 emits ordered loads and exclusive-access loops") {
    const auto package = Rux::Testing::CompileToAArch64Lir(kAtomicPrelude + R"(
        func Main() -> int {
            var count: int64 = 0;
            let seen = AtomicLoad(@count, MemoryOrder::Acquire);
            AtomicFetchAdd(@count, seen, MemoryOrder::AcquireRelease);
            AtomicCompareExchange(@count, 1i64, 2i64, MemoryOrder::Release, MemoryOrder::Relaxed);
            Fence(MemoryOrder::Acquire);
            return 0;
        }
    )");
    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    REQUIRE_FALSE(objects.empty());
    const auto words = Rux::Testing::FunctionWords(objects.front(), "Main");
    const auto has = [&](const std::uint32_t mask, const std::uint32_t value) {
        return std::ranges::any_of(words, [&](const std::uint32_t word) { return (word & mask) == value; });
    };
    CHECK(has(0xFFFFFC00U, 0xC8DFFC00U)); // ldar x?, [x?]
    CHECK(has(0xFFFFFC00U, 0xC85FFC00U)); // ldaxr x?, [x?]
    CHECK(has(0xFFE0FC00U, 0xC800FC00U)); // stlxr w?, x?, [x?]
    CHECK(has(0xFFFFFC00U, 0xC85F7C00U)); // ldxr x?, [x?] for the compare-exchange with no acquire
    CHECK(has(0xFFFFFFFFU, 0xD5033F5FU)); // clrex on the mismatch path
    CHECK(has(0xFFFFFFFFU, 0xD50339BFU)); // dmb ishld
}
