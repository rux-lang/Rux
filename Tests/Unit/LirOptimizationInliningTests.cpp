#include "Optimization/LirInliner.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirSsaVerifier.h"
#include "OptimizerTestSupport.h"

#include <unordered_set>

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
std::size_t Calls(const LirPackage &package) {
    std::size_t count = 0;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                count += std::ranges::count_if(block.instrs, [](const auto &instruction) {
                    return instruction.op == LirOpcode::Call || instruction.op == LirOpcode::CallIndirect;
                });
            }
        }
    }
    return count;
}
} // namespace

TEST_CASE("Bounded inlining clones branches returns and loop-edge phis") {
    auto package = CompileToLir(R"(
        func Select(value: int) -> int {
            if value < 0 { return -value; }
            if value == 0 { return 7; }
            return value + 3;
        }
        func Wrap(value: int) -> int { return Select(value) + 1; }
        func Work(limit: int) -> int {
            var total = 0;
            for i in 0..limit {
                if i == 4 { total += Wrap(i); } else { total += Select(-i); }
                total += Select(i);
            }
            return total;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    REQUIRE_GT(Calls(package), 0);
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Calls(package), 0);
    std::vector<Diagnostic> diagnostics;
    Optimization::PassContext context;
    context.diagnostics = &diagnostics;
    Optimization::LirCfgVerifier cfg;
    Optimization::LirSsaVerifier ssa;
    static_cast<void>(cfg.Run(package, context));
    static_cast<void>(ssa.Run(package, context));
    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
        CHECK_FALSE(diagnostic.IsError());
    }
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Bounded inlining leaves mutual recursion and its wrappers callable") {
    auto package = CompileToLir(R"(
        func First(value: int) -> int { return value == 0 ? 1 : Second(value - 1); }
        func Second(value: int) -> int { return value == 0 ? 2 : First(value - 1); }
        func Wrap(value: int) -> int { return First(value); }
        func Work(value: int) -> int { return Wrap(value); }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    const auto before = Calls(package);
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::None);
    CHECK_EQ(Calls(package), before);
}

TEST_CASE("Inlining a wide software result introduces no numeric conversion") {
    auto package = CompileToLir(R"(
        func Sum(left: float128, right: float128) -> float128 { return left + right; }
        func Work(left: float128, right: float128) -> float128 { return Sum(left, right); }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::Changed);
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            if (function.name != "Work") {
                continue;
            }
            for (const auto &block : function.blocks) {
                for (const auto &instruction : block.instrs) {
                    CHECK(instruction.op != LirOpcode::Cast);
                }
            }
        }
    }
}

TEST_CASE("Leaf inlining substitutes scalar parameters with fresh definitions") {
    auto package = CompileToLir(R"(
        func Rotate(value: uint64, amount: uint64) -> uint64 {
            return (value >> amount) | (value << (64 - amount));
        }
        func Work(input: uint64) -> uint64 { return Rotate(input, 3) ^ Rotate(input, 17); }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    REQUIRE_EQ(Calls(package), 2);
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Calls(package), 0);
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            std::unordered_set<LirReg> definitions;
            for (const auto &parameter : function.params) {
                CHECK(definitions.insert(parameter.reg).second);
            }
            for (const auto &block : function.blocks) {
                for (const auto &instruction : block.instrs) {
                    if (instruction.dst != LirNoReg) {
                        CHECK(definitions.insert(instruction.dst).second);
                    }
                }
            }
        }
    }
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Leaf inlining keeps recursion indirect calls and trapping helpers bounded") {
    auto package = CompileToLir(R"(
        func Recurse(value: int) -> int { return Recurse(value); }
        func Divide(value: int) -> int { return value / 0; }
        func Work(value: int, callback: func(int) -> int) -> int {
            return Recurse(value) + Divide(value) + callback(value);
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    const auto calls = Calls(package);
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::None);
    CHECK_EQ(Calls(package), calls);
}

TEST_CASE("Leaf inlining expansion runs once per optimization phase") {
    auto package = CompileToLir(R"(
        func Increment(value: int) -> int { return value + 1; }
        func Work(value: int) -> int { return Increment(value); }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    Optimization::LirInliner inliner;
    Optimization::PassContext later;
    later.iteration = 1;
    CHECK(inliner.Run(package, later) == Optimization::PassChange::None);
    CHECK_EQ(Calls(package), 1);
}

TEST_CASE("Scalar reference leaf inlining preserves updates through a lowered local address") {
    auto package = CompileToLir(R"(
        func Bump(value: &var uint64) -> uint64 { value += 1; return value; }
        func Work(input: uint64) -> uint64 {
            var state = input;
            let result = Bump(state);
            return state + result;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    Optimization::LirInliner inliner;
    CHECK(inliner.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Calls(package), 0);
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
}
