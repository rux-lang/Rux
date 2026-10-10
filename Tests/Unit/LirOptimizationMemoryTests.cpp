#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirScalarReplacement.h"
#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
std::size_t Count(const LirPackage &package, const LirOpcode op) {
    std::size_t count = 0;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                count +=
                    std::ranges::count_if(block.instrs, [&](const auto &instruction) { return instruction.op == op; });
            }
        }
    }
    return count;
}

LirPackage Storage(const bool initialized, const bool volatileStore = false, const bool mixedWidth = false) {
    auto function = ReturningFunction("Storage");
    LirInstr slot;
    slot.op = LirOpcode::Alloca;
    slot.dst = 0;
    slot.type = TypeRef::MakeInt32();
    function.blocks[0].instrs.push_back(slot);
    LirInstr value;
    value.op = LirOpcode::Const;
    value.dst = 1;
    value.type = mixedWidth ? TypeRef::MakeInt64() : slot.type;
    value.strArg = "42";
    function.blocks[0].instrs.push_back(value);
    if (initialized) {
        LirInstr store;
        store.op = LirOpcode::Store;
        store.type = slot.type;
        store.srcs = {1, 0};
        store.isVolatile = volatileStore;
        function.blocks[0].instrs.push_back(store);
    }
    LirInstr load;
    load.op = LirOpcode::Load;
    load.type = slot.type;
    load.dst = 2;
    load.srcs = {0};
    function.blocks[0].instrs.push_back(load);
    function.blocks[0].term = ReturnValue(2, slot.type);
    LirModule module;
    module.funcs.push_back(function);
    LirPackage package;
    package.modules.push_back(module);
    return package;
}
} // namespace

TEST_CASE("Memory promotion forms loop-carried SSA values without memory traffic") {
    auto package = CompileToLir(R"(
        func Sum(n: int) -> int {
            var i = 0;
            var sum = 0;
            while i < n {
                let increment = i + 1;
                sum += increment;
                i += 1;
            }
            return sum;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    CHECK_GE(Count(package, LirOpcode::Phi), 2);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK_FALSE(verifier.RunLir(package).HasErrors());
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Memory promotion rewrites call arguments and branch merges") {
    auto package = CompileToLir(R"(
        func Identity(value: int) -> int { return value; }
        func Choose(test: bool, input: int) -> int {
            var result = input;
            if test { result = input + 2; } else { result = input - 3; }
            return Identity(result);
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    CHECK_EQ(Count(package, LirOpcode::Call), 1);
}

TEST_CASE("Memory promotion preserves uninitialized volatile and mismatched storage") {
    Optimization::LirMemoryPromotion promotion;
    auto uninitialized = Storage(false);
    auto volatileWrite = Storage(true, true);
    auto mixed = Storage(true, false, true);
    for (auto *package : {&uninitialized, &volatileWrite, &mixed}) {
        CHECK(promotion.Run(*package, {}) == Optimization::PassChange::None);
        CHECK_EQ(Count(*package, LirOpcode::Alloca), 1);
        CHECK_EQ(Count(*package, LirOpcode::Load), 1);
    }
    auto direct = Storage(true);
    CHECK(promotion.Run(direct, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(*direct.modules[0].funcs[0].blocks[0].term->retVal, 1);
}

TEST_CASE("Memory promotion leaves escaped storage visible to calls") {
    auto package = CompileToLir(R"(
        func Set(value: *var int) { *value = 42; }
        func Main() -> int { var value = 1; Set(@value); return value; }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    CHECK_GE(Count(package, LirOpcode::Alloca), 1);
    CHECK_GE(Count(package, LirOpcode::Load), 1);
    CHECK_GE(Count(package, LirOpcode::Store), 1);
}

TEST_CASE("Scalar replacement promotes private range descriptors and loop-local counters") {
    auto package = CompileToLir(R"(
        func Sum(n: int) -> int {
            var sum = 0;
            for i in 0..n {
                for j in 0..i { sum += j; }
            }
            return sum;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirScalarReplacement replacement;
    Optimization::LirMemoryPromotion promotion;
    CHECK(replacement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::FieldPtr), 0);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK_FALSE(verifier.RunLir(package).HasErrors());
}
