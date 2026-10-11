#include "Ir/Lir/Dominance.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirLoopInvariants.h"
#include "Optimization/LirLoopUnrolling.h"
#include "Optimization/LirLoopVersioning.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirScalarReplacement.h"
#include "Optimization/LirSsaVerifier.h"
#include "Optimization/LirValueNumbering.h"
#include "OptimizerTestSupport.h"

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
        func Sum(limit: int, scale: int, a: int, b: int, c: int, d: int) -> int {
            var result = 0;
            for i in 0..limit { result += scale * 3 + scale * i + a * i + b * i + c * i + d * i; }
            return result;
        }
    )");
    Optimization::LirLoopInvariants motion;
    static_cast<void>(motion.Run(package, {}));
    CHECK(SsaErrors(package).empty());
    const auto &entry = package.modules[0].funcs[0].blocks[0].instrs;
    CHECK_FALSE(std::ranges::any_of(entry, [](const auto &instruction) { return instruction.op == LirOpcode::Mul; }));
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
                    instruction.strArg = "2";
                if (mode == 1 && instruction.op == LirOpcode::Const && instruction.strArg == "0")
                    instruction.strArg = "1";
                if (mode == 2 && instruction.op == LirOpcode::Store)
                    instruction.isVolatile = true;
                if (mode == 3 && instruction.op == LirOpcode::Phi)
                    instruction.type = TypeRef::MakeInt();
            }
        }
        Optimization::LirLoopVersioning versioning;
        CHECK(versioning.Run(package, {}) == Optimization::PassChange::None);
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

TEST_CASE("Loop versioning remaps exit phis and rejects unmerged live outs") {
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
    auto unmerged = package;
    unmerged.modules[0].funcs[0].blocks[target].term = ReturnValue(induction, TypeRef::MakeUInt());
    Optimization::LirLoopVersioning versioning;
    CHECK(SsaErrors(unmerged).empty());
    CHECK(versioning.Run(unmerged, {}) == Optimization::PassChange::None);
    LirInstr exitPhi;
    exitPhi.op = LirOpcode::Phi;
    exitPhi.dst = next;
    exitPhi.type = TypeRef::MakeUInt();
    exitPhi.phiPreds = {{induction, header}};
    function.blocks[target].instrs.insert(function.blocks[target].instrs.begin(), exitPhi);
    function.blocks[target].term = ReturnValue(next, exitPhi.type);
    REQUIRE(SsaErrors(package).empty());
    REQUIRE(versioning.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(SsaErrors(package).empty());
    const auto &incoming = function.blocks[target].instrs.front().phiPreds;
    REQUIRE_EQ(incoming.size(), 2);
    CHECK_NE(incoming[0].first, incoming[1].first);
    CHECK_NE(incoming[0].second, incoming[1].second);
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
