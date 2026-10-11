#include "Ir/Lir/Dominance.h"
#include "Ir/Lir/LirVectors.h"
#include "Optimization/ConstantEvaluator.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirDeadLoops.h"
#include "Optimization/LirFillWidening.h"
#include "Optimization/LirLoopInvariants.h"
#include "Optimization/LirLoopUnrolling.h"
#include "Optimization/LirLoopVersioning.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirPointerInduction.h"
#include "Optimization/LirScalarReplacement.h"
#include "Optimization/LirSsaVerifier.h"
#include "Optimization/LirValueNumbering.h"
#include "Optimization/LirViewParameters.h"
#include "OptimizerTestSupport.h"

#include <unordered_set>

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
LirPackage Promoted(const std::string &source) {
    auto package = CompileToLir(source, BuildProfile::Debug);
    Optimization::LirScalarReplacement replacement;
    Optimization::LirMemoryPromotion promotion;
    Optimization::LirConstantPropagation constants;
    static_cast<void>(replacement.Run(package, {}));
    static_cast<void>(promotion.Run(package, {}));
    static_cast<void>(constants.Run(package, {}));
    return package;
}

std::size_t Count(const LirPackage &package, const LirOpcode opcode) {
    std::size_t count = 0;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                count += std::ranges::count_if(block.instrs,
                                               [&](const auto &instruction) { return instruction.op == opcode; });
            }
        }
    }
    return count;
}

std::vector<Diagnostic> SsaErrors(LirPackage &package) {
    std::vector<Diagnostic> diagnostics;
    Optimization::PassContext context;
    context.diagnostics = &diagnostics;
    Optimization::LirSsaVerifier verifier;
    static_cast<void>(verifier.Run(package, context));
    return diagnostics;
}
} // namespace

TEST_CASE("CFG cleanup threads a constant phi input past a branch-only block") {
    // 0 branches on a parameter; 1 supplies another parameter and 2 supplies `false` to the phi 3 branches on.
    LirFunc function;
    function.name = "Thread";
    function.returnType = TypeRef::MakeInt();
    function.params = {{0, TypeRef::MakeBool(), "p"}, {1, TypeRef::MakeBool(), "q"}};
    function.blocks.resize(6);
    LirInstr constant;
    constant.op = LirOpcode::Const;
    constant.dst = 2;
    constant.type = TypeRef::MakeBool();
    constant.strArg = "false";
    function.blocks[0].instrs.push_back(constant);
    function.blocks[0].term = BranchTo(0, 1, 2);
    function.blocks[1].term = JumpTo(3);
    function.blocks[2].term = JumpTo(3);
    LirInstr phi;
    phi.op = LirOpcode::Phi;
    phi.dst = 3;
    phi.type = TypeRef::MakeBool();
    phi.phiPreds = {{1, 1}, {2, 2}};
    function.blocks[3].instrs.push_back(phi);
    function.blocks[3].term = BranchTo(3, 4, 5);
    for (const std::uint32_t block : {4U, 5U}) {
        LirInstr result;
        result.op = LirOpcode::Const;
        result.dst = 10 + block;
        result.type = TypeRef::MakeInt();
        result.strArg = std::to_string(block);
        function.blocks[block].instrs.push_back(result);
        function.blocks[block].term = ReturnValue(10 + block, TypeRef::MakeInt());
    }
    LirPackage package;
    package.modules.emplace_back();
    package.modules[0].funcs.push_back(function);
    Optimization::LirCfgCleanup cleanup;
    CHECK(cleanup.Run(package, {}) == Optimization::PassChange::Changed);
    const auto &blocks = package.modules[0].funcs[0].blocks;
    // The short-circuit edge now reaches the false target directly, and the phi keeps only the other edge.
    CHECK(blocks[2].term->kind == LirTermKind::Jump);
    CHECK_EQ(blocks[2].term->trueTarget, 5U);
    REQUIRE_EQ(blocks[3].instrs.size(), 1U);
    CHECK_EQ(blocks[3].instrs[0].phiPreds.size(), 1U);
    CHECK_EQ(blocks[3].instrs[0].phiPreds[0].second, 1U);
}

TEST_CASE("CFG cleanup folds a branch on a constant defined in another block") {
    // 0 defines `true` and jumps to 1, which branches on it; inlining leaves this shape behind a callee's guard.
    LirFunc function;
    function.name = "Fold";
    function.returnType = TypeRef::MakeInt();
    function.blocks.resize(4);
    LirInstr constant;
    constant.op = LirOpcode::Const;
    constant.dst = 1;
    constant.type = TypeRef::MakeBool();
    constant.strArg = "true";
    function.blocks[0].instrs.push_back(constant);
    function.blocks[0].term = JumpTo(1);
    function.blocks[1].term = BranchTo(1, 2, 3);
    for (const std::uint32_t block : {2U, 3U}) {
        LirInstr result;
        result.op = LirOpcode::Const;
        result.dst = 10 + block;
        result.type = TypeRef::MakeInt();
        result.strArg = std::to_string(block);
        function.blocks[block].instrs.push_back(result);
        function.blocks[block].term = ReturnValue(10 + block, TypeRef::MakeInt());
    }
    LirPackage package;
    package.modules.emplace_back();
    package.modules[0].funcs.push_back(function);
    Optimization::LirCfgCleanup cleanup;
    CHECK(cleanup.Run(package, {}) == Optimization::PassChange::Changed);
    const auto &blocks = package.modules[0].funcs[0].blocks;
    REQUIRE(blocks[1].term->kind == LirTermKind::Jump);
    CHECK_EQ(blocks[1].term->trueTarget, 2U);
}

TEST_CASE("Fill widening stores words where a loop stores one narrow value at every index") {
    const auto widened = [](const std::string &source) {
        auto package = Promoted(source);
        Optimization::LirCfgCleanup cleanup;
        Optimization::LirBlockCoalescing coalescing;
        static_cast<void>(cleanup.Run(package, {}));
        static_cast<void>(coalescing.Run(package, {}));
        REQUIRE(SsaErrors(package).empty());
        Optimization::LirFillWidening widening;
        const bool changed = widening.Run(package, {}) == Optimization::PassChange::Changed;
        CHECK(SsaErrors(package).empty());
        std::size_t words = 0;
        for (const auto &block : package.modules[0].funcs[0].blocks)
            for (const auto &instruction : block.instrs)
                words += instruction.op == LirOpcode::Store && instruction.type == TypeRef::MakeUInt64();
        CHECK_EQ(words, changed ? 1U : 0U);
        // A second run finds the element loop entered from the word loop, not from zero, and leaves it.
        Optimization::LirFillWidening again;
        CHECK(again.Run(package, {}) == Optimization::PassChange::None);
        return changed;
    };
    CHECK(widened(R"(
        func Fill(at: *var uint8, length: uint, value: uint8) {
            for index in 0u..length { at[index] = value; }
        }
    )"));
    CHECK(widened(R"(
        func Fill(at: *var int16, length: uint, value: int16) {
            for index in 0u..length { at[index] = value; }
        }
    )"));
    // Floating-point values convert by value, and a loop that also reads keeps its order of reads and writes.
    CHECK_FALSE(widened(R"(
        func Fill(at: *var float32, length: uint, value: float32) {
            for index in 0u..length { at[index] = value; }
        }
    )"));
    CHECK_FALSE(widened(R"(
        func Fill(at: *var uint8, length: uint, from: *uint8) {
            for index in 0u..length { at[index] = *from; }
        }
    )"));
}

TEST_CASE("Unrolling flattens a loop between two constants into its header") {
    auto package = Promoted(R"(
        func Assemble(at: *uint8) -> uint64 {
            var value: uint64 = 0u64;
            for b in 0u..8u {
                value = (value << 8u64) | (at[b] as uint64);
            }
            return value;
        }
    )");
    Optimization::LirCfgCleanup cleanup;
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(cleanup.Run(package, {}));
    static_cast<void>(coalescing.Run(package, {}));
    const auto loads = Count(package, LirOpcode::Load);
    Optimization::LirLoopUnrolling unrolling;
    REQUIRE(unrolling.Run(package, {}) == Optimization::PassChange::Changed);
    static_cast<void>(cleanup.Run(package, {}));
    CHECK(SsaErrors(package).empty());
    // Eight copies of the body, no comparison left to count with and no block that branches.
    CHECK_EQ(Count(package, LirOpcode::Load), loads * 8);
    CHECK_EQ(Count(package, LirOpcode::CmpLt), 0);
    CHECK_EQ(Count(package, LirOpcode::Phi), 0);
    for (const auto &block : package.modules[0].funcs[0].blocks)
        CHECK(block.term->kind != LirTermKind::Branch);
}

TEST_CASE("Dead loop elimination removes loops that only count and read") {
    const auto removed = [](const std::string &source) {
        auto package = Promoted(source);
        Optimization::LirCfgCleanup cleanup;
        static_cast<void>(cleanup.Run(package, {}));
        Optimization::LirDeadLoops deadLoops;
        const bool changed = deadLoops.Run(package, {}) == Optimization::PassChange::Changed;
        static_cast<void>(cleanup.Run(package, {}));
        CHECK(SsaErrors(package).empty());
        if (changed)
            CHECK_EQ(Count(package, LirOpcode::Phi), 0);
        return changed;
    };
    // An element drop that does nothing still visits every element; nothing it reads is used.
    CHECK(removed(R"(
        func Drop(values: *uint8, length: uint) -> uint {
            var index: uint = 0u;
            while index < length {
                let value = values[index];
                index += 1u;
            }
            return length;
        }
    )"));
    // A store, a count that could step past its limit forever, and a count read afterwards all keep their loops.
    CHECK_FALSE(removed(R"(
        func Clear(values: *var uint8, length: uint) {
            var index: uint = 0u;
            while index < length { values[index] = 0u8; index += 1u; }
        }
    )"));
    CHECK_FALSE(removed(R"(
        func Skip(length: uint) -> uint {
            var index: uint = 0u;
            while index < length { index += 2u; }
            return length;
        }
    )"));
    CHECK_FALSE(removed(R"(
        func Last(length: uint) -> uint {
            var index: uint = 0u;
            while index < length { index += 1u; }
            return index;
        }
    )"));
}

TEST_CASE("Unrolling pairs float64 work with the next iteration and devectorizes back") {
    const auto unrolled = [](const std::string &source) {
        auto package = Promoted(source);
        Optimization::LirCfgCleanup cleanup;
        Optimization::LirBlockCoalescing coalescing;
        static_cast<void>(cleanup.Run(package, {}));
        static_cast<void>(coalescing.Run(package, {}));
        Optimization::LirLoopUnrolling unrolling;
        REQUIRE(unrolling.Run(package, {}) == Optimization::PassChange::Changed);
        CHECK(SsaErrors(package).empty());
        return package;
    };
    auto paired = unrolled(R"(
        func Axpy(c: *var float64, b: *float64, a: float64, n: uint) {
            for j in 0u..n { c[j] = c[j] + a * b[j]; }
        }
    )");
    // One pair of loads, one multiply by the broadcast factor, one add and one store replace both iterations' work.
    CHECK_EQ(Count(paired, LirOpcode::VectorLoad), 2);
    CHECK_EQ(Count(paired, LirOpcode::VectorSplat), 1);
    CHECK_EQ(Count(paired, LirOpcode::VectorMul), 1);
    CHECK_EQ(Count(paired, LirOpcode::VectorAdd), 1);
    CHECK_EQ(Count(paired, LirOpcode::VectorStore), 1);
    auto &function = paired.modules[0].funcs[0];
    REQUIRE(HasVectorInstructions(function));
    const auto scalarLoads = Count(paired, LirOpcode::Load);
    Devectorize(function);
    CHECK_FALSE(HasVectorInstructions(function));
    CHECK(SsaErrors(paired).empty());
    CHECK_EQ(Count(paired, LirOpcode::Load), scalarLoads + 4);

    // A sum carried from one iteration to the next is a chain, not a pair: it stays scalar.
    const auto summed = unrolled(R"(
        func Sum(b: *float64, n: uint) -> float64 {
            var total = 0.0;
            for j in 0u..n { total = total + b[j]; }
            return total;
        }
    )");
    CHECK_EQ(Count(summed, LirOpcode::VectorLoad), 0);
    CHECK_EQ(Count(summed, LirOpcode::VectorAdd), 0);
}

TEST_CASE("Value numbering sees a bounds check through a same-width cast as the loop test") {
    auto package = Promoted(R"(
        func Sum(values: uint8[..]) -> uint {
            var total: uint = 0;
            var index: uint = 0;
            while index < values.length { total += values[index] as uint; index += 1; }
            return total;
        }
    )");
    Optimization::LirViewParameters views;
    static_cast<void>(views.Run(package, {}));
    const auto before = Count(package, LirOpcode::CmpLt);
    Optimization::LirValueNumbering numbering;
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    // `index < values.length` and the check `(index as uint64) < values.length` compare the same bits, and the check
    // runs only where the loop test already held, so its branch becomes a jump.
    CHECK(Count(package, LirOpcode::CmpLt) < before);
    std::size_t branches = 0;
    for (const auto &block : package.modules[0].funcs[0].blocks)
        branches += block.term && block.term->kind == LirTermKind::Branch;
    CHECK_EQ(branches, 1U);
}

TEST_CASE("View parameters read their fields once on entry when only read") {
    for (const bool escapes : {false, true}) {
        auto package = CompileToLir(escapes ? R"(
            func Sum(values: uint32[..], sink: *var *uint32[..]) -> uint32 {
                *sink = @values;
                var total = 0u32;
                var index: uint = 0;
                while index < values.length { total += values[index]; index += 1; }
                return total;
            }
        )"
                                            : R"(
            func Sum(values: uint32[..]) -> uint32 {
                var total = 0u32;
                var index: uint = 0;
                while index < values.length { total += values[index]; index += 1; }
                return total;
            }
        )",
                                    BuildProfile::Debug);
        Optimization::LirViewParameters views;
        const auto change = views.Run(package, {});
        CHECK(SsaErrors(package).empty());
        const auto &function = package.modules[0].funcs[0];
        REQUIRE(function.params[0].byAddress);
        // Count field loads of the view parameter outside the entry block.
        std::unordered_set<LirReg> fields;
        for (const auto &block : function.blocks)
            for (const auto &instruction : block.instrs)
                if (instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1 &&
                    instruction.srcs[0] == function.params[0].reg)
                    fields.insert(instruction.dst);
        std::size_t later = 0;
        for (std::size_t block = 1; block < function.blocks.size(); ++block)
            for (const auto &instruction : function.blocks[block].instrs)
                if (instruction.op == LirOpcode::Load && !instruction.srcs.empty() &&
                    fields.contains(instruction.srcs[0]))
                    ++later;
        CAPTURE(escapes);
        if (escapes) {
            CHECK(change == Optimization::PassChange::None);
            CHECK(later > 0);
        }
        else {
            CHECK(change == Optimization::PassChange::Changed);
            CHECK_EQ(later, 0U);
        }
    }
}

TEST_CASE("Dominators handle diamonds loops and unreachable blocks") {
    LirFunc function;
    function.blocks.resize(7);
    function.blocks[0].term = BranchTo(0, 1, 2);
    function.blocks[1].term = JumpTo(3);
    function.blocks[2].term = JumpTo(3);
    function.blocks[3].term = JumpTo(4);
    function.blocks[4].term = BranchTo(0, 3, 5);
    function.blocks[5].term = ReturnValue();
    function.blocks[6].term = JumpTo(5);
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    CHECK(dominance.Dominates(0, 5));
    CHECK(dominance.Dominates(3, 4));
    CHECK(dominance.Dominates(3, 5));
    CHECK_FALSE(dominance.Dominates(1, 3));
    CHECK_FALSE(dominance.Dominates(6, 5));
    CHECK_FALSE(dominance.Dominates(0, 6));
    CHECK_EQ(dominance.ImmediateDominators()[3], 0);
    CHECK_EQ(dominance.ReversePostorder().size(), 6);
}

TEST_CASE("SSA verifier accepts promoted loop phis and rejects missing duplicate and non-dominating values") {
    auto package = Promoted(R"(
        func Sum(n: int) -> int { var sum = 0; for i in 0..n { sum += i; } return sum; }
    )");
    CHECK(SsaErrors(package).empty());
    auto missing = package;
    missing.modules[0].funcs[0].blocks.back().term->retVal = 123456;
    CHECK_FALSE(SsaErrors(missing).empty());
    auto duplicate = package;
    duplicate.modules[0].funcs[0].blocks[0].instrs.push_back(duplicate.modules[0].funcs[0].blocks[0].instrs[0]);
    CHECK_FALSE(SsaErrors(duplicate).empty());
    auto branch = Promoted(R"(
        func Choose(test: bool, value: int) -> int {
            if test { return value + 1; }
            return value - 1;
        }
    )");
    auto &function = branch.modules[0].funcs[0];
    LirInstr value;
    value.dst = 123456;
    value.op = LirOpcode::Const;
    value.type = TypeRef::MakeInt64();
    value.strArg = "7";
    function.blocks[1].instrs.push_back(value);
    function.blocks.back().term->retVal = value.dst;
    CHECK_FALSE(SsaErrors(branch).empty());
}

TEST_CASE("Local numbering reuses expressions and loads without crossing aliasing writes") {
    auto package = Promoted(R"(
        func Read(pointer: *int, a: int, b: int) -> int {
            let first = *pointer;
            let x = a + b;
            let second = *pointer;
            let y = a + b;
            return first + second + x + y;
        }
    )");
    REQUIRE_EQ(Count(package, LirOpcode::Load), 2);
    Optimization::LirValueNumbering numbering;
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Load), 1);
    CHECK(SsaErrors(package).empty());
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::None);

    auto aliasing = Promoted(R"(
        func Read(pointer: *int, other: *var int) -> int {
            let first = *pointer;
            *other = 42;
            return first + *pointer;
        }
    )");
    static_cast<void>(numbering.Run(aliasing, {}));
    CHECK_EQ(Count(aliasing, LirOpcode::Load), 2);
    for (auto &instruction : package.modules[0].funcs[0].blocks[0].instrs) {
        if (instruction.op == LirOpcode::Load) {
            instruction.isVolatile = true;
        }
    }
    auto &instructions = package.modules[0].funcs[0].blocks[0].instrs;
    auto load = *std::ranges::find_if(instructions, [](const auto &i) { return i.op == LirOpcode::Load; });
    load.dst = 10000;
    instructions.push_back(load);
    static_cast<void>(numbering.Run(package, {}));
    CHECK_EQ(Count(package, LirOpcode::Load), 2);
}

TEST_CASE("Dominator numbering reuses integer expressions across branches and memory effects") {
    auto package = Promoted(R"(
        func Choose(test: bool, pointer: *var int, a: int, b: int) -> int {
            let first = a + b;
            *pointer = first;
            if test { return (a + b) * first; }
            return (a + b) - first;
        }
    )");
    REQUIRE_EQ(Count(package, LirOpcode::Add), 3);
    Optimization::LirValueNumbering numbering;
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Add), 1);
    CHECK_EQ(Count(package, LirOpcode::Store), 1);
    CHECK(SsaErrors(package).empty());
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::None);

    auto siblings = Promoted(R"(
        func Choose(test: bool, a: int, b: int) -> int {
            if test { return (a + b) * a; }
            return (a + b) * b;
        }
    )");
    static_cast<void>(numbering.Run(siblings, {}));
    CHECK_EQ(Count(siblings, LirOpcode::Add), 2);
    CHECK(SsaErrors(siblings).empty());

    auto floating = Promoted(R"(
        func Choose(test: bool, a: float64, b: float64) -> float64 {
            let first = a + b;
            if test { return (a + b) * first; }
            return (a + b) - first;
        }
    )");
    static_cast<void>(numbering.Run(floating, {}));
    CHECK_EQ(Count(floating, LirOpcode::Add), 3);
    CHECK(SsaErrors(floating).empty());
}

TEST_CASE("Exact typed store forwarding retains aliasing and volatile barriers") {
    auto package = Promoted(R"(
        func Read(pointer: *var int, value: int) -> int { *pointer = value; return *pointer; }
    )");
    REQUIRE_EQ(Count(package, LirOpcode::Load), 1);
    Optimization::LirValueNumbering numbering;
    auto volatileStore = package;
    for (auto &block : volatileStore.modules[0].funcs[0].blocks)
        for (auto &instruction : block.instrs)
            if (instruction.op == LirOpcode::Store)
                instruction.isVolatile = true;
    static_cast<void>(numbering.Run(volatileStore, {}));
    CHECK_EQ(Count(volatileStore, LirOpcode::Load), 1);
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 1);
    CHECK(SsaErrors(package).empty());

    auto aliasing = Promoted(R"(
        func Read(pointer: *var int, other: *var int, value: int) -> int {
            *pointer = value; *other = 42; return *pointer;
        }
    )");
    static_cast<void>(numbering.Run(aliasing, {}));
    CHECK_EQ(Count(aliasing, LirOpcode::Load), 1);
    CHECK(SsaErrors(aliasing).empty());
}

TEST_CASE("Repeated branch facts eliminate only dominated exact Boolean conditions") {
    auto package = Promoted(R"(
        func Choose(a: int, b: int) -> int {
            if a < b { if a < b { return 7; } return 11; }
            if a < b { return 13; }
            return 17;
        }
    )");
    const auto branches = [](const LirPackage &ir) {
        std::size_t count = 0;
        for (const auto &module : ir.modules)
            for (const auto &function : module.funcs)
                for (const auto &block : function.blocks)
                    count += block.term && block.term->kind == LirTermKind::Branch;
        return count;
    };
    REQUIRE_EQ(branches(package), 3);
    Optimization::LirValueNumbering numbering;
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(branches(package), 1);
    Optimization::LirCfgCleanup cleanup;
    static_cast<void>(cleanup.Run(package, {}));
    CHECK(SsaErrors(package).empty());

    auto changed = Promoted(R"(
        func Choose(pointer: *var int, a: int) -> int {
            if *pointer < a { *pointer = a; if *pointer < a { return 7; } return 11; }
            return 17;
        }
    )");
    static_cast<void>(numbering.Run(changed, {}));
    CHECK_EQ(branches(changed), 2);
    CHECK(SsaErrors(changed).empty());
}

TEST_CASE("Pattern binding frame addresses dominate uses without moving initialization") {
    auto package = CompileToLir(R"(
        variant Item { Empty, Value(int) }
        func Read(item: Item) -> int {
            return match item { .Empty => 0, .Value(value) => value };
        }
    )",
                                BuildProfile::Debug);
    REQUIRE_FALSE(SsaErrors(package).empty());
    std::vector<std::size_t> stores;
    for (const auto &block : package.modules[0].funcs[0].blocks) {
        stores.push_back(std::ranges::count_if(block.instrs, [](const auto &i) { return i.op == LirOpcode::Store; }));
    }
    Optimization::LirFrameAddressPlacement placement;
    CHECK(placement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    for (std::size_t block = 0; block < stores.size(); ++block) {
        CHECK_EQ(stores[block], std::ranges::count_if(package.modules[0].funcs[0].blocks[block].instrs,
                                                      [](const auto &i) { return i.op == LirOpcode::Store; }));
    }
    CHECK(placement.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Borrowed pattern addresses dominate arm uses without speculating loads") {
    auto package = CompileToLir(R"(
        struct Payload { number: int; }
        func Bump(item: &var (Payload ! int32)) {
            match item { .Failure(_) => {}, .Success(value) => { value.number += 1; } }
        }
    )",
                                BuildProfile::Debug);
    REQUIRE_FALSE(SsaErrors(package).empty());
    std::vector<std::size_t> loads;
    for (const auto &block : package.modules[0].funcs[0].blocks) {
        loads.push_back(std::ranges::count_if(block.instrs, [](const auto &i) { return i.op == LirOpcode::Load; }));
    }
    Optimization::LirFrameAddressPlacement placement;
    CHECK(placement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    for (std::size_t block = 0; block < loads.size(); ++block) {
        CHECK_EQ(loads[block], std::ranges::count_if(package.modules[0].funcs[0].blocks[block].instrs,
                                                     [](const auto &i) { return i.op == LirOpcode::Load; }));
    }
    CHECK(placement.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Ternary arms convert contextual integer literals before entering a typed phi") {
    auto package = CompileToLir(R"(
        func Count(rest: uint64) -> uint64 { return rest == 0 ? 0 : rest - 1; }
    )",
                                BuildProfile::Debug);
    CHECK(SsaErrors(package).empty());
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    CHECK_FALSE(pipeline.RunLir(package).HasErrors());
}

TEST_CASE("Pattern address normalization handles deep shared dependencies but never speculative loads") {
    const auto pointer = TypeRef::MakePointer(TypeRef::MakeInt64());
    auto package = CompileToLir("func Address(value: *int, test: bool) -> *int { return value; }", BuildProfile::Debug);
    auto &function = package.modules[0].funcs[0];
    const auto value = function.params[0].reg;
    const auto condition = function.params[1].reg;
    function.blocks.clear();
    function.blocks.resize(3);
    function.blocks[0].term = BranchTo(condition, 1, 2);
    LirInstr offset;
    offset.op = LirOpcode::Const;
    offset.dst = 100;
    offset.type = TypeRef::MakeUInt64();
    offset.strArg = "0";
    function.blocks[1].instrs.push_back(offset);
    auto previous = value;
    for (LirReg reg = 101; reg < 261; ++reg) {
        LirInstr address;
        address.op = LirOpcode::IndexPtr;
        address.type = pointer;
        address.dst = reg;
        address.srcs = {previous, offset.dst};
        function.blocks[1].instrs.push_back(address);
        previous = reg;
    }
    function.blocks[1].term = JumpTo(2);
    function.blocks[2].term = ReturnValue(previous, pointer);
    auto guardedLoad = package;
    auto &load = guardedLoad.modules[0].funcs[0].blocks[1].instrs.back();
    load.op = LirOpcode::Load;
    load.srcs = {value};
    REQUIRE_FALSE(SsaErrors(package).empty());
    Optimization::LirFrameAddressPlacement placement;
    CHECK(placement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    CHECK_EQ(function.blocks[0].instrs.size(), 161);
    CHECK(placement.Run(guardedLoad, {}) == Optimization::PassChange::None);
    CHECK_FALSE(SsaErrors(guardedLoad).empty());
}

TEST_CASE("Loop motion hoists invariant integer arithmetic but retains loads division and checks") {
    auto package = Promoted(R"(
        func Sum(limit: int, scale: int, pointer: *int) -> int {
            var sum = 0;
            for i in 0..limit { sum += scale * 3 + *pointer + 100 / (i + 1); }
            return sum;
        }
    )");
    REQUIRE(SsaErrors(package).empty());
    const auto beforeLoads = Count(package, LirOpcode::Load);
    const auto beforeDivision = Count(package, LirOpcode::Div);
    const auto beforeChecks = Count(package, LirOpcode::Assert);
    Optimization::LirLoopInvariants motion;
    CHECK(motion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    CHECK_EQ(Count(package, LirOpcode::Load), beforeLoads);
    CHECK_EQ(Count(package, LirOpcode::Div), beforeDivision);
    CHECK_EQ(Count(package, LirOpcode::Assert), beforeChecks);
    const auto &entry = package.modules[0].funcs[0].blocks[0].instrs;
    CHECK(std::ranges::any_of(entry, [](const auto &i) { return i.op == LirOpcode::Mul; }));
    CHECK_FALSE(
        std::ranges::any_of(entry, [](const auto &i) { return i.op == LirOpcode::Load || i.op == LirOpcode::Div; }));
}

TEST_CASE("Loop motion leaves recurrence constants near their uses") {
    auto package = Promoted(R"(
        func Sum(limit: int) -> int {
            var result = 0;
            for i in 0..limit { result = result * 7 + i; }
            return result;
        }
    )");
    Optimization::LirLoopInvariants motion;
    static_cast<void>(motion.Run(package, {}));
    CHECK(SsaErrors(package).empty());
    const auto &entry = package.modules[0].funcs[0].blocks[0].instrs;
    CHECK_FALSE(std::ranges::any_of(entry, [](const auto &instruction) {
        return instruction.op == LirOpcode::Const && instruction.strArg == "7";
    }));
}

TEST_CASE("Loop motion avoids adding another live value to a crowded loop") {
    auto package = Promoted(R"(
        func Sum(limit: int, scale: int, a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, j: int,
                 k: int, m: int) -> int {
            var result = 0;
            for i in 0..limit {
                result += scale * 3 + scale * i + a * i + b * i + c * i + d * i + e * i + f * i + g * i + h * i +
                          j * i + k * i + m * i;
            }
            return result;
        }
    )");
    Optimization::LirLoopInvariants motion;
    static_cast<void>(motion.Run(package, {}));
    CHECK(SsaErrors(package).empty());
    const auto &entry = package.modules[0].funcs[0].blocks[0].instrs;
    CHECK_FALSE(std::ranges::any_of(entry, [](const auto &instruction) { return instruction.op == LirOpcode::Mul; }));
}

TEST_CASE("Loop motion hoists header loads of a by-address view out of a loop that writes nothing") {
    for (const bool writes : {false, true}) {
        auto package = Promoted(writes ? R"(
            func Count(text: char8[..], target: char8, out: *var uint) -> uint {
                var found: uint = 0;
                var index: uint = 0;
                while index < text.length {
                    if text.data[index] == target { found += 1; }
                    *out = found;
                    index += 1;
                }
                return found;
            }
        )"
                                       : R"(
            func Count(text: char8[..], target: char8) -> uint {
                var found: uint = 0;
                var index: uint = 0;
                while index < text.length {
                    if text.data[index] == target { found += 1; }
                    index += 1;
                }
                return found;
            }
        )");
        REQUIRE(SsaErrors(package).empty());
        const auto &function = package.modules[0].funcs[0];
        REQUIRE(function.params[0].byAddress);
        Optimization::LirLoopInvariants motion;
        static_cast<void>(motion.Run(package, {}));
        CHECK(SsaErrors(package).empty());
        // The length and data words of the header are read once before the loop only when nothing in it can
        // write them.
        const auto &entry = package.modules[0].funcs[0].blocks[0].instrs;
        const auto entryLoads = std::ranges::count_if(entry, [](const auto &i) { return i.op == LirOpcode::Load; });
        CAPTURE(writes);
        if (writes)
            CHECK_EQ(entryLoads, 0);
        else
            CHECK_GE(entryLoads, 2);
    }
}

namespace {
LirPackage VersioningLoop() {
    return Promoted(R"(
        func Walk(limit: uint, row: uint, bound: uint, pointer: *var uint) -> uint {
            for i in 0u..limit {
                let index = row + i;
                if index < bound { *pointer += index; }
                else { return 123u; }
            }
            return *pointer;
        }
    )");
}
} // namespace

namespace {
/// The limit a split loop's fast copy tests its count against, once every input is a known constant: the fast
/// copy's header compares the count with it and leaves for the checked loop when the test fails.
std::optional<std::uint64_t> FastLimit(const LirFunc &function) {
    std::unordered_map<LirReg, const LirInstr *> definitions;
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.dst != LirNoReg)
                definitions.emplace(instruction.dst, &instruction);
    for (const auto &block : function.blocks) {
        if (!block.label.contains(".range.fast.") || block.term->kind != LirTermKind::Branch ||
            function.blocks[block.term->falseTarget].label.contains(".range.fast."))
            continue;
        const auto test = definitions.find(block.term->cond);
        if (test == definitions.end() || test->second->op != LirOpcode::CmpLt)
            continue;
        const auto limit = definitions.find(test->second->srcs[1]);
        if (limit == definitions.end() || limit->second->op != LirOpcode::Const)
            return std::nullopt;
        const auto value = Optimization::ParseConstant(limit->second->strArg, limit->second->type);
        return value ? std::optional<std::uint64_t>(value->RawBits()) : std::nullopt;
    }
    return std::nullopt;
}

/// Replaces parameters with constants, in parameter order from `first`, and propagates them.
void Substitute(LirPackage &package, const std::size_t first, const std::vector<std::uint64_t> &values) {
    auto &function = package.modules[0].funcs[0];
    std::vector<LirInstr> constants;
    for (std::size_t index = 0; index < values.size(); ++index) {
        LirInstr constant;
        constant.op = LirOpcode::Const;
        constant.dst = function.params[first + index].reg;
        constant.type = function.params[first + index].type;
        constant.strArg = std::to_string(values[index]);
        constants.push_back(std::move(constant));
    }
    function.params.erase(function.params.begin() + static_cast<std::ptrdiff_t>(first),
                          function.params.begin() + static_cast<std::ptrdiff_t>(first + values.size()));
    auto &entry = function.blocks[0];
    entry.instrs.insert(entry.instrs.begin(), constants.begin(), constants.end());
    Optimization::LirConstantPropagation propagation;
    static_cast<void>(propagation.Run(package, {}));
}
} // namespace

TEST_CASE("Guarded loop versioning preserves scalar fallback and SSA") {
    auto package = VersioningLoop();
    REQUIRE(SsaErrors(package).empty());
    auto &function = package.modules[0].funcs[0];
    const auto oldCount = function.blocks.size();
    const auto original = function.blocks;
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    CHECK(function.blocks.size() > oldCount);
    std::size_t removedChecks = 0;
    for (std::size_t block = 1; block < oldCount; ++block) {
        CHECK(function.blocks[block].term->kind == original[block].term->kind);
        CHECK(function.blocks[block].term->cond == original[block].term->cond);
    }
    for (std::size_t block = oldCount; block < function.blocks.size(); ++block) {
        CHECK(function.blocks[block].label.contains(".range.fast."));
        removedChecks += function.blocks[block].term->kind == LirTermKind::Branch;
    }
    CHECK_EQ(removedChecks, 1); // Only the iteration test remains in the fast loop.
    Optimization::PassContext later;
    later.iteration = 1;
    CHECK(versioning.Run(package, later) == Optimization::PassChange::None);
}

TEST_CASE("Loop versioning rejects unproven induction and volatile accesses") {
    for (const auto mode : {0, 1, 2, 3}) {
        auto package = VersioningLoop();
        for (auto &block : package.modules[0].funcs[0].blocks) {
            for (auto &instruction : block.instrs) {
                if (mode == 0 && instruction.op == LirOpcode::Const && instruction.strArg == "1")
                    instruction.strArg = "0";
                if (mode == 1 && instruction.op == LirOpcode::Const && instruction.strArg == "0")
                    instruction.strArg = "1";
                if (mode == 2 && instruction.op == LirOpcode::Store)
                    instruction.isVolatile = true;
                if (mode == 3 && instruction.op == LirOpcode::Phi)
                    instruction.type = TypeRef::MakeInt();
            }
        }
        Optimization::LirLoopVersioning versioning;
        CAPTURE(mode);
        // A unit-stride count may start at any non-negative constant; a zero step, a volatile access and a signed
        // count against an unsigned limit stay unversioned.
        CHECK(versioning.Run(package, {}) ==
              (mode == 1 ? Optimization::PassChange::Changed : Optimization::PassChange::None));
        // Mode 3 retypes only the phi, which leaves its inputs mismatched before the pass runs at all.
        if (mode != 3)
            CHECK(SsaErrors(package).empty());
    }
}

TEST_CASE("Loop versioning retains memory-dependent bounds") {
    auto package = Promoted(R"(
        func Walk(limit: uint, row: uint, bound: *var uint) {
            for i in 0u..limit {
                if row + i < *bound { *bound -= 1u; }
                else { return; }
            }
        }
    )");
    Optimization::LirLoopVersioning versioning;
    CHECK(versioning.Run(package, {}) == Optimization::PassChange::None);
    CHECK(SsaErrors(package).empty());
}

TEST_CASE("Split strided loops run fast exactly while every visited index fits") {
    for (const auto stride : {2u, 3u, 4u, 6u, 64u}) {
        auto original = VersioningLoop();
        for (auto &block : original.modules[0].funcs[0].blocks)
            for (auto &instruction : block.instrs)
                if (instruction.op == LirOpcode::Const && instruction.strArg == "1")
                    instruction.strArg = std::to_string(stride);
        Optimization::LirLoopVersioning versioning;
        REQUIRE(versioning.Run(original, {}) == Optimization::PassChange::Changed);
        REQUIRE(SsaErrors(original).empty());
        for (const auto limit : {0ull, 1ull, 2ull, 4ull, 7ull, 10ull, 0xFFFFFFFFFFFFFFFFull}) {
            for (const auto row : {0ull, 2ull, 0xFFFFFFFFFFFFFFFEull}) {
                for (const auto bound : {0ull, 1ull, 4ull, 9ull, 0xFFFFFFFFFFFFFFFDull}) {
                    auto package = original;
                    Substitute(package, 0, {limit, row, bound});
                    const auto fast = FastLimit(package.modules[0].funcs[0]);
                    REQUIRE(fast.has_value());
                    CAPTURE(stride);
                    CAPTURE(limit);
                    CAPTURE(row);
                    CAPTURE(bound);
                    // Every index below bound - row fits; none does when row exceeds the bound. The fast copy also
                    // stops where one more step could wrap.
                    const std::uint64_t fits = row <= bound ? bound - row : 0;
                    const std::uint64_t unwrapped = 0xFFFFFFFFFFFFFFFFull - (stride - 1);
                    CHECK_EQ(*fast, std::min({limit, fits, unwrapped}));
                }
            }
        }
    }
}

TEST_CASE("Signed loop guards cover every offset index the loop visits") {
    const auto original = Promoted(R"(
        func Walk(limit: int, bound: uint, pointer: *var uint) -> uint {
            for t in 16..limit {
                if ((t - 15) as uint) < bound { *pointer += 1u; }
                else { return 1u; }
                if (t as uint) < bound { *pointer += 2u; }
                else { return 2u; }
            }
            return *pointer;
        }
    )");
    auto versioned = original;
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(versioned, {}) == Optimization::PassChange::Changed);
    REQUIRE(SsaErrors(versioned).empty());
    for (const std::int64_t limit : {17, 18, 40, 81}) {
        for (const std::uint64_t bound : {0ull, 1ull, 2ull, 24ull, 25ull, 26ull, 65ull, 80ull, 81ull}) {
            auto package = versioned;
            auto &function = package.modules[0].funcs[0];
            std::vector<LirInstr> constants;
            for (std::size_t index = 0; index < 2; ++index) {
                LirInstr constant;
                constant.op = LirOpcode::Const;
                constant.dst = function.params[index].reg;
                constant.type = function.params[index].type;
                constant.strArg = index == 0 ? std::to_string(limit) : std::to_string(bound);
                constants.push_back(std::move(constant));
            }
            function.params.erase(function.params.begin(), function.params.begin() + 2);
            auto &entry = function.blocks[0];
            entry.instrs.insert(entry.instrs.begin(), constants.begin(), constants.end());
            Optimization::LirConstantPropagation propagation;
            static_cast<void>(propagation.Run(package, {}));
            const auto condition = std::ranges::find_if(
                entry.instrs, [&](const auto &instruction) { return instruction.dst == entry.term->cond; });
            REQUIRE(condition != entry.instrs.end());
            REQUIRE(condition->op == LirOpcode::Const);
            bool allFit = true;
            for (std::int64_t t = 16; t < limit; ++t)
                allFit = allFit && static_cast<std::uint64_t>(t - 15) < bound && static_cast<std::uint64_t>(t) < bound;
            CAPTURE(limit);
            CAPTURE(bound);
            CHECK((condition->strArg == "true") == allFit);
        }
    }
}

TEST_CASE("Loop versioning rejects unsupported strides") {
    for (const auto stride : {"65", "18446744073709551615"}) {
        auto package = VersioningLoop();
        for (auto &block : package.modules[0].funcs[0].blocks)
            for (auto &instruction : block.instrs)
                if (instruction.op == LirOpcode::Const && instruction.strArg == "1")
                    instruction.strArg = stride;
        Optimization::LirLoopVersioning versioning;
        CHECK(versioning.Run(package, {}) == Optimization::PassChange::None);
    }
}

TEST_CASE("Loop splitting hands the fast copy's state to the checked loop") {
    auto package = VersioningLoop();
    auto &function = package.modules[0].funcs[0];
    std::uint32_t header = 0;
    LirReg induction = LirNoReg;
    LirReg next = 0;
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst != LirNoReg)
                next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::Phi) {
                header = block;
                induction = instruction.dst;
            }
        }
    }
    REQUIRE(induction != LirNoReg);
    const auto target = function.blocks[header].term->falseTarget;
    LirInstr exitPhi;
    exitPhi.op = LirOpcode::Phi;
    exitPhi.dst = next;
    exitPhi.type = TypeRef::MakeUInt();
    exitPhi.phiPreds = {{induction, header}};
    function.blocks[target].instrs.insert(function.blocks[target].instrs.begin(), exitPhi);
    function.blocks[target].term = ReturnValue(induction, exitPhi.type);
    REQUIRE(SsaErrors(package).empty());
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    // The loop is still left only through its original header, whose count now arrives from the fast copy rather
    // than from the entry, so what is read after the loop is unchanged.
    const auto &phi = function.blocks[header].instrs.front();
    REQUIRE(phi.op == LirOpcode::Phi);
    REQUIRE_EQ(phi.phiPreds.size(), 2);
    CHECK(std::ranges::any_of(
        phi.phiPreds, [&](const auto &edge) { return function.blocks[edge.second].label.contains(".range.fast."); }));
    CHECK_EQ(function.blocks[target].instrs.front().phiPreds.size(), 1);
    CHECK_EQ(*function.blocks[target].term->retVal, induction);
}

TEST_CASE("Loop splitting keeps live outs read by phis after the exit") {
    auto original = VersioningLoop();
    auto &walk = original.modules[0].funcs[0];
    std::uint32_t header = 0;
    LirReg induction = LirNoReg;
    LirReg next = 0;
    for (std::uint32_t block = 0; block < walk.blocks.size(); ++block) {
        for (const auto &instruction : walk.blocks[block].instrs) {
            if (instruction.dst != LirNoReg)
                next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::Phi) {
                header = block;
                induction = instruction.dst;
            }
        }
    }
    REQUIRE(induction != LirNoReg);
    const auto target = walk.blocks[header].term->falseTarget;
    // The exit jumps to a block whose phi reads the loop's value, as an inlined callee's return does.
    auto &blocks = original.modules[0].funcs[0].blocks;
    const auto join = static_cast<std::uint32_t>(blocks.size());
    LirBlock returned;
    returned.label = "returned";
    LirInstr phi;
    phi.op = LirOpcode::Phi;
    phi.dst = next;
    phi.type = TypeRef::MakeUInt();
    phi.phiPreds = {{induction, target}};
    returned.instrs.push_back(std::move(phi));
    returned.term = ReturnValue(next, TypeRef::MakeUInt());
    blocks[target].term.emplace();
    blocks[target].term->kind = LirTermKind::Jump;
    blocks[target].term->trueTarget = join;
    blocks.push_back(std::move(returned));
    REQUIRE(SsaErrors(original).empty());
    Optimization::LirLoopVersioning versioning;
    CHECK(versioning.Run(original, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(original).empty());
    CHECK_EQ(blocks[join].instrs.front().phiPreds.front().first, induction);
}

TEST_CASE("Loop versioning proves byte-indexed and secondary-counter checks") {
    const auto original = Promoted(R"(
        func Walk(limit: uint, bound: uint, tableBound: uint, pointer: *var uint, bytes: *uint8) -> uint {
            var o: uint = 0u;
            for i in 0u..limit {
                if ((*bytes) as uint) < tableBound { *pointer += i; }
                else { return 1u; }
                if o < bound { *pointer += o; }
                else { return 2u; }
                o += 1u;
                if o + 1u < bound { *pointer += 1u; }
                else { return 3u; }
                o += 2u;
            }
            return o;
        }
    )");
    auto versioned = original;
    const auto oldCount = versioned.modules[0].funcs[0].blocks.size();
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(versioned, {}) == Optimization::PassChange::Changed);
    REQUIRE(SsaErrors(versioned).empty());
    // Only the counted test remains in the fast copy.
    std::size_t branches = 0;
    const auto &blocks = versioned.modules[0].funcs[0].blocks;
    for (std::size_t block = oldCount; block < blocks.size(); ++block)
        branches += blocks[block].term->kind == LirTermKind::Branch;
    CHECK_EQ(branches, 1);
    for (const std::uint64_t limit : {1ull, 2ull, 5ull, 0x100000000ull, 0x100000001ull}) {
        for (const std::uint64_t bound : {0ull, 1ull, 3ull, 13ull, 14ull, 15ull, 0x400000000ull}) {
            for (const std::uint64_t tableBound : {255ull, 256ull}) {
                auto package = versioned;
                auto &function = package.modules[0].funcs[0];
                const std::uint64_t arguments[] = {limit, bound, tableBound};
                std::vector<LirInstr> constants;
                for (std::size_t index = 0; index < 3; ++index) {
                    LirInstr constant;
                    constant.op = LirOpcode::Const;
                    constant.dst = function.params[index].reg;
                    constant.type = function.params[index].type;
                    constant.strArg = std::to_string(arguments[index]);
                    constants.push_back(std::move(constant));
                }
                function.params.erase(function.params.begin(), function.params.begin() + 3);
                auto &entry = function.blocks[0];
                entry.instrs.insert(entry.instrs.begin(), constants.begin(), constants.end());
                Optimization::LirConstantPropagation propagation;
                static_cast<void>(propagation.Run(package, {}));
                const auto fast = FastLimit(function);
                REQUIRE(fast.has_value());
                // Iteration i reads o = 3i and o + 2, so it fits while 3i + 2 < bound; every byte is below 256.
                const std::uint64_t secondaryFits = bound > 2 ? (bound - 3) / 3 + 1 : 0;
                CAPTURE(limit);
                CAPTURE(bound);
                CAPTURE(tableBound);
                CHECK_EQ(*fast, tableBound > 255 ? std::min(limit, secondaryFits) : 0);
                for (std::uint64_t i = 0; i < std::min<std::uint64_t>(*fast, 64); ++i)
                    CHECK(3 * i + 2 < bound);
            }
        }
    }
}

TEST_CASE("Loop versioning proves the tested value's checks in loops it cannot count") {
    // A sieve's stride and start are only known at run time, and an inclusive range adds an exit of its own; the
    // checks on the tested value still hold whenever the limit is below the bound.
    for (const auto *source : {R"(
        func Mark(at: *var uint8, length: uint, limit: uint, start: uint, step: uint) -> uint {
            var j = start;
            while j <= limit {
                if j < length { at[j] = 1u8; }
                else { return 1u; }
                j += step;
            }
            return 0u;
        }
    )",
                               R"(
        func Count(at: *uint8, length: uint, limit: uint, start: uint, step: uint) -> uint {
            var total: uint = 0u;
            for k in start..=limit {
                if k < length { total += at[k] as uint; }
                else { return 1u; }
            }
            return total + step;
        }
    )"}) {
        auto versioned = Promoted(source);
        Optimization::LirCfgCleanup cleanup;
        static_cast<void>(cleanup.Run(versioned, {}));
        Optimization::LirLoopVersioning versioning;
        REQUIRE(versioning.Run(versioned, {}) == Optimization::PassChange::Changed);
        REQUIRE(SsaErrors(versioned).empty());
        for (const std::uint64_t limit : {0ull, 5ull, 9ull, 10ull, 0xFFFFFFFFFFFFFFFFull}) {
            for (const std::uint64_t length : {0ull, 9ull, 10ull, 11ull}) {
                auto package = versioned;
                auto &function = package.modules[0].funcs[0];
                std::vector<LirInstr> constants;
                for (std::size_t index = 1; index < 3; ++index) {
                    LirInstr constant;
                    constant.op = LirOpcode::Const;
                    constant.dst = function.params[index].reg;
                    constant.type = function.params[index].type;
                    constant.strArg = std::to_string(index == 1 ? length : limit);
                    constants.push_back(std::move(constant));
                }
                function.params.erase(function.params.begin() + 1, function.params.begin() + 3);
                auto &entry = function.blocks[0];
                entry.instrs.insert(entry.instrs.begin(), constants.begin(), constants.end());
                Optimization::LirConstantPropagation propagation;
                static_cast<void>(propagation.Run(package, {}));
                const auto fast = FastLimit(function);
                REQUIRE(fast.has_value());
                CAPTURE(limit);
                CAPTURE(length);
                // `j <= limit` runs fast as `j < limit + 1` while that stays within the length.
                CHECK_EQ(*fast, limit < length ? limit + 1 : length);
            }
        }
    }
}

TEST_CASE("Loop versioning finds the counted test after a short-circuit operand") {
    const auto original = Promoted(R"(
        func Walk(limit: uint, bound: uint, pointer: *var uint, start: bool) -> bool {
            var same = start;
            var k: uint = 0;
            while same && k < limit {
                if k < bound { same = *pointer != k; }
                else { return false; }
                k += 1u;
            }
            return same;
        }
    )");
    auto versioned = original;
    // Cleanup threads the short-circuit's constant `false` past its merge, leaving `k < limit` to branch alone.
    Optimization::LirCfgCleanup cleanup;
    static_cast<void>(cleanup.Run(versioned, {}));
    REQUIRE(SsaErrors(versioned).empty());
    const auto oldCount = versioned.modules[0].funcs[0].blocks.size();
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(versioned, {}) == Optimization::PassChange::Changed);
    REQUIRE(SsaErrors(versioned).empty());
    // The fast copy keeps the test of `same` and the counted test, but not the bounds test.
    std::size_t branches = 0;
    const auto &blocks = versioned.modules[0].funcs[0].blocks;
    for (std::size_t block = oldCount; block < blocks.size(); ++block)
        branches += blocks[block].term->kind == LirTermKind::Branch;
    CHECK_EQ(branches, 2);
    for (const std::uint64_t limit : {0ull, 1ull, 5ull, 6ull}) {
        for (const std::uint64_t bound : {0ull, 5ull, 6ull}) {
            auto package = versioned;
            auto &function = package.modules[0].funcs[0];
            std::vector<LirInstr> constants;
            for (std::size_t index = 0; index < 2; ++index) {
                LirInstr constant;
                constant.op = LirOpcode::Const;
                constant.dst = function.params[index].reg;
                constant.type = function.params[index].type;
                constant.strArg = std::to_string(index == 0 ? limit : bound);
                constants.push_back(std::move(constant));
            }
            function.params.erase(function.params.begin(), function.params.begin() + 2);
            auto &entry = function.blocks[0];
            entry.instrs.insert(entry.instrs.begin(), constants.begin(), constants.end());
            Optimization::LirConstantPropagation propagation;
            static_cast<void>(propagation.Run(package, {}));
            const auto condition = std::ranges::find_if(
                entry.instrs, [&](const auto &instruction) { return instruction.dst == entry.term->cond; });
            REQUIRE(condition != entry.instrs.end());
            REQUIRE(condition->op == LirOpcode::Const);
            CAPTURE(limit);
            CAPTURE(bound);
            CHECK((condition->strArg == "true") == (limit <= bound));
        }
    }
}

TEST_CASE("Block coalescing preserves ordered effects and replaces unique-edge phis") {
    auto package = Promoted("func Walk(value: int, pointer: *var int) -> int { return value; }");
    auto &function = package.modules[0].funcs[0];
    const auto value = function.params[0].reg;
    const auto pointer = function.params[1].reg;
    function.blocks.clear();
    function.blocks.resize(3);
    function.blocks[0].term = JumpTo(2); // Deliberately non-layout order.
    LirInstr store;
    store.op = LirOpcode::Store;
    store.type = TypeRef::MakeInt();
    store.srcs = {value, pointer};
    store.isVolatile = true;
    function.blocks[0].instrs.push_back(store);
    LirInstr phi;
    phi.op = LirOpcode::Phi;
    phi.dst = 100;
    phi.type = TypeRef::MakeInt();
    phi.phiPreds = {{value, 0}};
    function.blocks[2].instrs.push_back(phi);
    LirInstr divide;
    divide.op = LirOpcode::Div;
    divide.dst = 101;
    divide.type = phi.type;
    divide.srcs = {100, value};
    function.blocks[2].instrs.push_back(divide);
    function.blocks[2].term = JumpTo(1);
    phi.dst = 102;
    phi.phiPreds = {{101, 2}};
    function.blocks[1].instrs.push_back(phi);
    store.srcs = {102, pointer};
    function.blocks[1].instrs.push_back(store);
    function.blocks[1].term = ReturnValue(102, phi.type);
    REQUIRE(SsaErrors(package).empty());
    Optimization::LirBlockCoalescing coalescing;
    REQUIRE(coalescing.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    REQUIRE_EQ(function.blocks.size(), 1);
    const auto &instructions = function.blocks[0].instrs;
    REQUIRE_EQ(instructions.size(), 3);
    CHECK(instructions[0].op == LirOpcode::Store);
    CHECK(instructions[0].isVolatile);
    CHECK(instructions[1].op == LirOpcode::Div);
    CHECK(instructions[1].srcs == std::vector<LirReg>{value, value});
    CHECK(instructions[2].op == LirOpcode::Store);
    CHECK(instructions[2].isVolatile);
    CHECK(instructions[2].srcs == std::vector<LirReg>{101, pointer});
    CHECK(function.blocks[0].term->retVal == 101);
}

TEST_CASE("Block coalescing keeps conditional joins and valid loop phi edges") {
    auto package = Promoted(R"(
        func Sum(test: bool, limit: int) -> int {
            var result = 0;
            for i in 0..limit {
                if test { result += i; }
                else { result -= i; }
            }
            return result;
        }
    )");
    const auto before = Count(package, LirOpcode::Phi);
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(coalescing.Run(package, {}));
    CHECK(SsaErrors(package).empty());
    CHECK_EQ(Count(package, LirOpcode::Phi), before);
    const auto &function = package.modules[0].funcs[0];
    CHECK_EQ(std::ranges::count_if(function.blocks,
                                   [](const auto &block) { return block.term->kind == LirTermKind::Branch; }),
             2);
    Optimization::PassContext later;
    later.iteration = 1;
    CHECK(coalescing.Run(package, later) == Optimization::PassChange::None);
}

namespace {
LirPackage UnrollingLoop() {
    auto package = Promoted(R"(
        func Walk(start: uint, limit: uint, pointer: *var uint) -> uint {
            var i = start;
            var a = 3u;
            var b = 5u;
            while i < limit {
                let old = a;
                a = b;
                b = old + i;
                *pointer = *pointer * 7u + a + b;
                i += 1u;
            }
            return a + b;
        }
    )");
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(coalescing.Run(package, {}));
    return package;
}
} // namespace

TEST_CASE("Loop unrolling preserves simultaneous recurrences scalar tail and valid SSA") {
    auto package = UnrollingLoop();
    REQUIRE(SsaErrors(package).empty());
    const auto blocks = package.modules[0].funcs[0].blocks.size();
    const auto stores = Count(package, LirOpcode::Store);
    Optimization::LirLoopUnrolling unrolling;
    REQUIRE(unrolling.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    CHECK_EQ(package.modules[0].funcs[0].blocks.size(), blocks + 3);
    CHECK_EQ(Count(package, LirOpcode::Store), stores + 2);
    Optimization::PassContext later;
    later.iteration = 1;
    CHECK(unrolling.Run(package, later) == Optimization::PassChange::None);
}

TEST_CASE("Loop unrolling rejects volatile memory nonunit steps and calls") {
    for (int mode = 0; mode < 3; ++mode) {
        auto package = UnrollingLoop();
        for (auto &block : package.modules[0].funcs[0].blocks) {
            for (auto &instruction : block.instrs) {
                if (mode == 0 && instruction.op == LirOpcode::Store)
                    instruction.isVolatile = true;
                if (mode == 1 && instruction.op == LirOpcode::Const && instruction.strArg == "1")
                    instruction.strArg = "2";
                if (mode == 2 && instruction.op == LirOpcode::Store) {
                    instruction.op = LirOpcode::Call;
                    instruction.strArg = "Opaque";
                }
            }
        }
        Optimization::LirLoopUnrolling unrolling;
        CHECK(unrolling.Run(package, {}) == Optimization::PassChange::None);
    }
}

TEST_CASE("Unrolling follows guarded versioning through a conditional entry edge") {
    auto package = VersioningLoop();
    Optimization::LirLoopVersioning versioning;
    REQUIRE(versioning.Run(package, {}) == Optimization::PassChange::Changed);
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(coalescing.Run(package, {}));
    Optimization::LirDeadCodeElimination dead;
    static_cast<void>(dead.Run(package, {}));
    Optimization::LirCfgCleanup cleanup;
    static_cast<void>(cleanup.Run(package, {}));
    REQUIRE(SsaErrors(package).empty());
    Optimization::LirLoopUnrolling unrolling;
    CHECK(unrolling.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
}

TEST_CASE("Load facts cross unique checked edges without entering sibling writes or joins") {
    Optimization::LirValueNumbering numbering;
    auto chain = Promoted(R"(
        func Read(pointer: *int, first: bool, second: bool) -> int {
            let before = *pointer;
            if first { return before; }
            if second { return before + *pointer; }
            return before - *pointer;
        }
    )");
    REQUIRE_EQ(Count(chain, LirOpcode::Load), 3);
    CHECK(numbering.Run(chain, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(chain, LirOpcode::Load), 1);
    CHECK(SsaErrors(chain).empty());
    auto siblings = Promoted(R"(
        func Read(pointer: *int, other: *var int, test: bool) -> int {
            let before = *pointer;
            if test { *other = 42; return before + *pointer; }
            return before - *pointer;
        }
    )");
    static_cast<void>(numbering.Run(siblings, {}));
    CHECK_EQ(Count(siblings, LirOpcode::Load), 2);
    CHECK(SsaErrors(siblings).empty());
    auto join = Promoted(R"(
        func Read(pointer: *int, other: *var int, test: bool) -> int {
            let before = *pointer;
            if test { *other = 42; }
            return before + *pointer;
        }
    )");
    static_cast<void>(numbering.Run(join, {}));
    CHECK_EQ(Count(join, LirOpcode::Load), 2);
    CHECK(SsaErrors(join).empty());
}

TEST_CASE("Inherited load facts respect calls volatile accesses and loop backedges") {
    Optimization::LirValueNumbering numbering;
    auto calls = Promoted(R"(
        func Touch(pointer: *var int) { *pointer = 99; }
        func Read(pointer: *var int, test: bool) -> int {
            let before = *pointer;
            if test { Touch(pointer); return before + *pointer; }
            return before;
        }
    )");
    static_cast<void>(numbering.Run(calls, {}));
    CHECK_EQ(Count(calls, LirOpcode::Load), 2);
    CHECK(SsaErrors(calls).empty());
    auto loop = Promoted(R"(
        func Read(pointer: *int, other: *var int, count: uint) -> int {
            var total = *pointer;
            for i in 0u..count { *other += 1; total += *pointer; }
            return total;
        }
    )");
    const auto loads = Count(loop, LirOpcode::Load);
    static_cast<void>(numbering.Run(loop, {}));
    CHECK_EQ(Count(loop, LirOpcode::Load), loads);
    CHECK(SsaErrors(loop).empty());
    auto volatileRead = Promoted(R"(
        func Read(pointer: *int, test: bool) -> int {
            let before = *pointer;
            if test { return before + *pointer; }
            return before;
        }
    )");
    for (auto &block : volatileRead.modules[0].funcs[0].blocks)
        for (auto &instruction : block.instrs)
            if (instruction.op == LirOpcode::Load)
                instruction.isVolatile = true;
    static_cast<void>(numbering.Run(volatileRead, {}));
    CHECK_EQ(Count(volatileRead, LirOpcode::Load), 2);
    CHECK(SsaErrors(volatileRead).empty());
}

TEST_CASE("Exact stored values forward through both unique successors") {
    auto package = Promoted(R"(
        func Read(pointer: *var int, value: int, test: bool) -> int {
            *pointer = value;
            if test { return *pointer + 1; }
            return *pointer - 1;
        }
    )");
    Optimization::LirValueNumbering numbering;
    REQUIRE_EQ(Count(package, LirOpcode::Load), 2);
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 1);
    CHECK(SsaErrors(package).empty());
}

TEST_CASE("Value numbering recognizes equal typed literals while retaining local constants") {
    auto package = Promoted(R"(
        func Read(value: int, test: bool) -> int {
            let before = value + 15;
            if test { return before; }
            return before + (value + 15);
        }
    )");
    REQUIRE_EQ(Count(package, LirOpcode::Add), 3);
    const auto constants = Count(package, LirOpcode::Const);
    Optimization::LirValueNumbering numbering;
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Add), 2);
    CHECK_EQ(Count(package, LirOpcode::Const), constants);
    CHECK(SsaErrors(package).empty());

    auto distinct = Promoted(R"(
        func Read(value: int, test: bool) -> int {
            let before = value + 15;
            if test { return before; }
            return before + (value + 16);
        }
    )");
    static_cast<void>(numbering.Run(distinct, {}));
    CHECK_EQ(Count(distinct, LirOpcode::Add), 3);
    CHECK(SsaErrors(distinct).empty());

    auto countTypes = Promoted(R"(
        func Read(value: uint64, test: bool) -> uint64 {
            let before = value >> 1u32;
            if test { return before; }
            return before ^ (value >> 1u64);
        }
    )");
    static_cast<void>(numbering.Run(countTypes, {}));
    CHECK_EQ(Count(countTypes, LirOpcode::Shr), 2);
    CHECK(SsaErrors(countTypes).empty());

    auto floating = Promoted(R"(
        func Read(value: float64, test: bool) -> float64 {
            let before = value + 1.0;
            if test { return before; }
            return before + (value + 1.0);
        }
    )");
    static_cast<void>(numbering.Run(floating, {}));
    CHECK_EQ(Count(floating, LirOpcode::Add), 3);
    CHECK(SsaErrors(floating).empty());
}

TEST_CASE("Literal expression keys remove repeated bounds without crossing writes") {
    auto package = Promoted(R"(
        func Read(value: uint) -> uint {
            if value < 15u { return value; }
            if value < 15u { return value + 1u; }
            return value + 2u;
        }
    )");
    Optimization::LirValueNumbering numbering;
    REQUIRE_EQ(Count(package, LirOpcode::CmpLt), 2);
    CHECK(numbering.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::CmpLt), 1);
    CHECK_EQ(
        std::ranges::count_if(package.modules[0].funcs[0].blocks,
                              [](const auto &block) { return block.term && block.term->kind == LirTermKind::Branch; }),
        1);
    CHECK(SsaErrors(package).empty());
    auto writes = Promoted(R"(
        func Read(pointer: *var int, other: *var int, test: bool) -> int {
            let before = *(pointer + 1u);
            if test { *other = 99; return before + *(pointer + 1u); }
            return before;
        }
    )");
    static_cast<void>(numbering.Run(writes, {}));
    CHECK_EQ(Count(writes, LirOpcode::Load), 2);
    CHECK(SsaErrors(writes).empty());
}

namespace {
LirPackage AffinePointers() {
    auto package = Promoted(R"(
        func Walk(start: uint, limit: uint, row: uint, pointer: *var uint, other: *uint) -> uint {
            var total = 0u;
            for i in start..limit {
                *(pointer + (row + i)) += 3u;
                total += *(other + (row + i));
            }
            return total;
        }
    )");
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(coalescing.Run(package, {}));
    Optimization::LirDeadCodeElimination dead;
    static_cast<void>(dead.Run(package, {}));
    Optimization::LirCfgCleanup cleanup;
    static_cast<void>(cleanup.Run(package, {}));
    return package;
}
} // namespace

TEST_CASE("Pointer induction advances affine row addresses and preserves memory order") {
    auto package = AffinePointers();
    REQUIRE(SsaErrors(package).empty());
    const auto loads = Count(package, LirOpcode::Load);
    const auto stores = Count(package, LirOpcode::Store);
    const auto phis = Count(package, LirOpcode::Phi);
    Optimization::LirPointerInduction induction;
    REQUIRE(induction.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(Count(package, LirOpcode::Phi) > phis);
    CHECK_EQ(Count(package, LirOpcode::Load), loads);
    CHECK_EQ(Count(package, LirOpcode::Store), stores);
    CHECK(SsaErrors(package).empty());
    Optimization::LirDeadCodeElimination dead;
    static_cast<void>(dead.Run(package, {}));
    Optimization::LirLoopUnrolling unrolling;
    CHECK(unrolling.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    Optimization::PassContext later;
    later.iteration = 1;
    CHECK(induction.Run(package, later) == Optimization::PassChange::None);
}

TEST_CASE("Pointer induction rejects volatile addresses and nonunit induction") {
    for (const bool volatileAddress : {false, true}) {
        auto package = AffinePointers();
        for (auto &block : package.modules[0].funcs[0].blocks) {
            for (auto &instruction : block.instrs) {
                if (volatileAddress && instruction.op == LirOpcode::IndexPtr)
                    instruction.isVolatile = true;
                if (!volatileAddress && instruction.op == LirOpcode::Const && instruction.strArg == "1")
                    instruction.strArg = "2";
            }
        }
        Optimization::LirPointerInduction induction;
        CHECK(induction.Run(package, {}) == Optimization::PassChange::None);
        CHECK(SsaErrors(package).empty());
    }
}

TEST_CASE("Pointer induction retains addresses captured across loop exits") {
    auto package = Promoted(R"(
        func Walk(start: uint, limit: uint, row: uint, pointer: *uint) -> *uint {
            var last = pointer;
            for i in start..limit { last = pointer + (row + i); }
            return last;
        }
    )");
    Optimization::LirBlockCoalescing coalescing;
    static_cast<void>(coalescing.Run(package, {}));
    Optimization::LirCfgCleanup cleanup;
    static_cast<void>(cleanup.Run(package, {}));
    REQUIRE(SsaErrors(package).empty());
    Optimization::LirPointerInduction induction;
    CHECK(induction.Run(package, {}) == Optimization::PassChange::None);
    CHECK(SsaErrors(package).empty());
}
