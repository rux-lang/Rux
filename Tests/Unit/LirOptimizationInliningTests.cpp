#include "Optimization/LirInliner.h"
#include "Optimization/LirMemoryPromotion.h"
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
