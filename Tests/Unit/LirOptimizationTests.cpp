#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

TEST_CASE("LIR CFG verifier reports malformed control flow without traversing invalid blocks") {
    LirFunc function;
    function.name = "Broken";
    function.blocks.resize(3);
    function.blocks[0].label = "entry";
    function.blocks[0].term = BranchTo(0, 2, 7);
    function.blocks[1].label = "unterminated";
    LirInstr unexpectedPhi;
    unexpectedPhi.dst = 1;
    unexpectedPhi.op = LirOpcode::Phi;
    unexpectedPhi.type = TypeRef::MakeInt32();
    unexpectedPhi.phiPreds = {{0, 0}};
    function.blocks[1].instrs.push_back(std::move(unexpectedPhi));
    function.blocks[2].label = "target";
    LirInstr incompletePhi;
    incompletePhi.dst = 2;
    incompletePhi.op = LirOpcode::Phi;
    incompletePhi.type = TypeRef::MakeInt32();
    function.blocks[2].instrs.push_back(std::move(incompletePhi));
    function.blocks[2].term = ReturnValue();

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    const auto report = pipeline.RunLir(package);

    REQUIRE(report.HasErrors());
    CHECK_FALSE(report.reachedFixedPoint);
    CHECK(report.iterations == 1);
    CHECK(report.diagnostics.size() == 4);
    CHECK(report.diagnostics[0].message.find("branch terminator targets invalid block 7") != std::string::npos);
    CHECK(report.diagnostics[1].message.find("block has no terminator") != std::string::npos);
    CHECK(report.diagnostics[2].message.find("phi names block 0, which is not an actual predecessor") !=
          std::string::npos);
    CHECK(report.diagnostics[3].message.find("phi has no value for predecessor block 0") != std::string::npos);
}

TEST_CASE("LIR CFG cleanup folds branches and remaps reachable blocks and phi predecessors") {
    LirFunc function;
    function.name = "Choose";
    function.blocks.resize(5);
    function.blocks[0].label = "entry";
    function.blocks[1].label = "untaken";
    function.blocks[2].label = "taken";
    function.blocks[3].label = "orphan";
    function.blocks[4].label = "merge";

    LirInstr condition;
    condition.dst = 0;
    condition.op = LirOpcode::Const;
    condition.type = TypeRef::MakeBool();
    condition.strArg = "true";
    function.blocks[0].instrs.push_back(std::move(condition));
    function.blocks[0].term = BranchTo(0, 2, 1);

    function.blocks[1].term = JumpTo(4);
    function.blocks[2].term = JumpTo(4);
    function.blocks[3].term = UnreachableTerminator();

    LirInstr phi;
    phi.dst = 3;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeInt32();
    phi.phiPreds = {{1, 1}, {2, 2}};
    function.blocks[4].instrs.push_back(std::move(phi));
    function.blocks[4].term = ReturnValue(LirReg{3}, TypeRef::MakeInt32());

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Optimization::LirCfgCleanup cleanup;
    const Optimization::PassContext context{BuildProfile::Release};
    const auto change = cleanup.Run(package, context);

    CHECK(change == Optimization::PassChange::Changed);
    REQUIRE(package.modules[0].funcs[0].blocks.size() == 3);
    const auto &blocks = package.modules[0].funcs[0].blocks;
    CHECK(blocks[0].label == "entry");
    CHECK(blocks[1].label == "taken");
    CHECK(blocks[2].label == "merge");
    REQUIRE(blocks[0].term);
    CHECK(blocks[0].term->kind == LirTermKind::Jump);
    CHECK(blocks[0].term->trueTarget == 1);
    REQUIRE(blocks[1].term);
    CHECK(blocks[1].term->trueTarget == 2);
    REQUIRE(blocks[2].instrs.size() == 1);
    CHECK(blocks[2].instrs[0].phiPreds == std::vector<std::pair<LirReg, std::uint32_t>>{{2, 1}});
}

TEST_CASE("LIR CFG cleanup folds a known switch to its matching case") {
    LirFunc function;
    function.name = "Select";
    function.blocks.resize(4);
    function.blocks[0].label = "entry";
    function.blocks[1].label = "one";
    function.blocks[2].label = "two";
    function.blocks[3].label = "fallback";

    LirInstr condition;
    condition.dst = 0;
    condition.op = LirOpcode::Const;
    condition.type = TypeRef::MakeInt32();
    condition.strArg = "2";
    function.blocks[0].instrs.push_back(std::move(condition));
    LirTerminator switchTerminator;
    switchTerminator.kind = LirTermKind::Switch;
    switchTerminator.cond = 0;
    switchTerminator.retType = TypeRef::MakeInt32();
    switchTerminator.defaultTarget = 3;
    switchTerminator.cases = {{"1", 1}, {"2", 2}};
    function.blocks[0].term = std::move(switchTerminator);
    function.blocks[1].term = ReturnValue();
    function.blocks[2].term = ReturnValue();
    function.blocks[3].term = ReturnValue();

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Optimization::LirPassPipeline pipeline(BuildProfile::Release);
    pipeline.Add(std::make_unique<Optimization::LirConstantPropagation>());
    pipeline.Add(std::make_unique<Optimization::LirCfgCleanup>());
    const auto report = pipeline.Run(package);

    CHECK_FALSE(report.HasErrors());
    CHECK(report.reachedFixedPoint);
    REQUIRE(package.modules[0].funcs[0].blocks.size() == 2);
    const auto &blocks = package.modules[0].funcs[0].blocks;
    CHECK(blocks[0].label == "entry");
    CHECK(blocks[1].label == "two");
    REQUIRE(blocks[0].term);
    CHECK(blocks[0].term->kind == LirTermKind::Jump);
    CHECK(blocks[0].term->trueTarget == 1);
}

TEST_CASE("LIR propagation joins constants and exposes a constant branch to CFG cleanup") {
    LirFunc function;
    function.name = "JoinConstants";
    function.params.push_back({0, TypeRef::MakeBool(), "condition"});
    function.blocks.resize(6);
    function.blocks[0].label = "entry";
    function.blocks[1].label = "left";
    function.blocks[2].label = "right";
    function.blocks[3].label = "merge";
    function.blocks[4].label = "taken";
    function.blocks[5].label = "untaken";
    function.blocks[0].term = BranchTo(0, 1, 2);

    LirInstr left;
    left.dst = 1;
    left.op = LirOpcode::Const;
    left.type = TypeRef::MakeInt32();
    left.strArg = "40";
    function.blocks[1].instrs.push_back(std::move(left));
    function.blocks[1].term = JumpTo(3);

    LirInstr right;
    right.dst = 2;
    right.op = LirOpcode::Const;
    right.type = TypeRef::MakeInt32();
    right.strArg = "40";
    function.blocks[2].instrs.push_back(std::move(right));
    function.blocks[2].term = JumpTo(3);

    LirInstr phi;
    phi.dst = 3;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeInt32();
    phi.phiPreds = {{1, 1}, {2, 2}};
    function.blocks[3].instrs.push_back(std::move(phi));

    LirInstr two;
    two.dst = 4;
    two.op = LirOpcode::Const;
    two.type = TypeRef::MakeInt32();
    two.strArg = "2";
    function.blocks[3].instrs.push_back(std::move(two));

    LirInstr sum;
    sum.dst = 5;
    sum.op = LirOpcode::Add;
    sum.type = TypeRef::MakeInt32();
    sum.srcs = {3, 4};
    function.blocks[3].instrs.push_back(std::move(sum));

    LirInstr expected;
    expected.dst = 6;
    expected.op = LirOpcode::Const;
    expected.type = TypeRef::MakeInt32();
    expected.strArg = "42";
    function.blocks[3].instrs.push_back(std::move(expected));

    LirInstr comparison;
    comparison.dst = 7;
    comparison.op = LirOpcode::CmpEq;
    comparison.type = TypeRef::MakeBool();
    comparison.srcs = {5, 6};
    function.blocks[3].instrs.push_back(std::move(comparison));
    function.blocks[3].term = BranchTo(7, 4, 5);
    function.blocks[4].term = ReturnValue();
    function.blocks[5].term = ReturnValue();

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Optimization::LirPassPipeline pipeline(BuildProfile::Release);
    pipeline.Add(std::make_unique<Optimization::LirConstantPropagation>());
    pipeline.Add(std::make_unique<Optimization::LirCfgCleanup>());
    const auto report = pipeline.Run(package);

    CHECK_FALSE(report.HasErrors());
    CHECK(report.reachedFixedPoint);
    const auto &blocks = package.modules[0].funcs[0].blocks;
    REQUIRE(blocks.size() == 5);
    REQUIRE(blocks[3].instrs.size() == 5);
    CHECK(blocks[3].instrs[0].op == LirOpcode::Const);
    CHECK(blocks[3].instrs[0].strArg == "40");
    CHECK(blocks[3].instrs[2].op == LirOpcode::Const);
    CHECK(blocks[3].instrs[2].strArg == "42");
    CHECK(blocks[3].instrs[4].op == LirOpcode::Const);
    CHECK(blocks[3].instrs[4].strArg == "true");
    REQUIRE(blocks[3].term);
    CHECK(blocks[3].term->kind == LirTermKind::Jump);
    CHECK(blocks[4].label == "taken");
}

TEST_CASE("LIR propagation copies only into pure users") {
    LirFunc function;
    function.name = "CopyJoin";
    function.params = {{0, TypeRef::MakeBool(), "condition"}, {1, TypeRef::MakeInt32(), "value"}};
    function.blocks.resize(4);
    function.blocks[0].term = BranchTo(0, 1, 2);
    function.blocks[1].term = JumpTo(3);
    function.blocks[2].term = JumpTo(3);

    LirInstr phi;
    phi.dst = 2;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeInt32();
    phi.phiPreds = {{1, 1}, {1, 2}};
    function.blocks[3].instrs.push_back(std::move(phi));

    LirInstr one;
    one.dst = 3;
    one.op = LirOpcode::Const;
    one.type = TypeRef::MakeInt32();
    one.strArg = "1";
    function.blocks[3].instrs.push_back(std::move(one));

    LirInstr add;
    add.dst = 4;
    add.op = LirOpcode::Add;
    add.type = TypeRef::MakeInt32();
    add.srcs = {2, 3};
    function.blocks[3].instrs.push_back(std::move(add));

    LirInstr call;
    call.dst = 5;
    call.op = LirOpcode::Call;
    call.type = TypeRef::MakeInt32();
    call.strArg = "Observe";
    call.srcs = {2};
    function.blocks[3].instrs.push_back(std::move(call));

    LirInstr load;
    load.dst = 6;
    load.op = LirOpcode::Load;
    load.type = TypeRef::MakeInt32();
    load.srcs = {2};
    function.blocks[3].instrs.push_back(std::move(load));
    function.blocks[3].term = ReturnValue(4, TypeRef::MakeInt32());

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    const auto report = pipeline.RunLir(package);

    CHECK_FALSE(report.HasErrors());
    CHECK(report.reachedFixedPoint);
    const auto &instructions = package.modules[0].funcs[0].blocks[3].instrs;
    CHECK(instructions[2].srcs == std::vector<LirReg>{1, 3});
    CHECK(instructions[3].srcs == std::vector<LirReg>{2});
    CHECK(instructions[4].srcs == std::vector<LirReg>{2});
    CHECK(instructions[3].op == LirOpcode::Call);
    CHECK(instructions[4].op == LirOpcode::Load);
}

TEST_CASE("LIR propagation stops at conflicting joins and unsupported evaluation") {
    LirFunc function;
    function.name = "ConflictingJoin";
    function.params.push_back({0, TypeRef::MakeBool(), "condition"});
    function.blocks.resize(4);
    function.blocks[0].term = BranchTo(0, 1, 2);

    LirInstr one;
    one.dst = 1;
    one.op = LirOpcode::Const;
    one.type = TypeRef::MakeInt32();
    one.strArg = "1";
    function.blocks[1].instrs.push_back(std::move(one));
    function.blocks[1].term = JumpTo(3);

    LirInstr two;
    two.dst = 2;
    two.op = LirOpcode::Const;
    two.type = TypeRef::MakeInt32();
    two.strArg = "2";
    function.blocks[2].instrs.push_back(std::move(two));
    function.blocks[2].term = JumpTo(3);

    LirInstr phi;
    phi.dst = 3;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeInt32();
    phi.phiPreds = {{1, 1}, {2, 2}};
    function.blocks[3].instrs.push_back(std::move(phi));

    LirInstr zero;
    zero.dst = 4;
    zero.op = LirOpcode::Const;
    zero.type = TypeRef::MakeInt32();
    zero.strArg = "0";
    function.blocks[3].instrs.push_back(std::move(zero));

    LirInstr division;
    division.dst = 5;
    division.op = LirOpcode::Div;
    division.type = TypeRef::MakeInt32();
    division.srcs = {1, 4};
    function.blocks[3].instrs.push_back(std::move(division));
    function.blocks[3].term = ReturnValue(3, TypeRef::MakeInt32());

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    const auto report = pipeline.RunLir(package);

    CHECK_FALSE(report.HasErrors());
    CHECK(report.reachedFixedPoint);
    const auto &instructions = package.modules[0].funcs[0].blocks[3].instrs;
    CHECK(instructions[0].op == LirOpcode::Phi);
    CHECK(instructions[2].op == LirOpcode::Div);
}

TEST_CASE("LIR dead code elimination removes pure results and dead local storage") {
    LirFunc function;
    function.name = "LocalCleanup";
    function.params = {{0, TypeRef::MakeInt32(), "value"}, {1, TypeRef::MakePointer(TypeRef::MakeInt32()), "external"}};
    function.blocks.resize(1);
    auto &instructions = function.blocks[0].instrs;

    const auto append = [&](const LirOpcode op, const LirReg dst, std::vector<LirReg> srcs = {}) -> LirInstr & {
        LirInstr instruction;
        instruction.op = op;
        instruction.dst = dst;
        instruction.type = TypeRef::MakeInt32();
        instruction.srcs = std::move(srcs);
        instructions.push_back(std::move(instruction));
        return instructions.back();
    };

    append(LirOpcode::Alloca, 2);
    append(LirOpcode::Add, 3, {0, 0});
    append(LirOpcode::Store, LirNoReg, {3, 2});
    append(LirOpcode::Mul, 4, {0, 0});
    append(LirOpcode::Store, LirNoReg, {4, 2});
    append(LirOpcode::Load, 5, {2});
    append(LirOpcode::Add, 6, {5, 0});

    append(LirOpcode::Alloca, 7);
    auto &call = append(LirOpcode::Call, 8);
    call.strArg = "Observe";
    append(LirOpcode::Store, LirNoReg, {8, 7});

    append(LirOpcode::Store, LirNoReg, {3, 1});
    append(LirOpcode::Alloca, 9);
    auto &field = append(LirOpcode::FieldPtr, 10, {9});
    field.strArg = "field";
    append(LirOpcode::Store, LirNoReg, {3, 10});

    append(LirOpcode::Add, 11, {0, 0});
    append(LirOpcode::Mul, 14, {11, 0});
    append(LirOpcode::Div, 12, {0, 0});
    auto &namedLoad = append(LirOpcode::Load, 13);
    namedLoad.strArg = "ExternalValue";
    function.blocks[0].term = ReturnValue(6, TypeRef::MakeInt32());

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Optimization::LirDeadCodeElimination pass;
    const Optimization::PassContext context{BuildProfile::Release};
    CHECK(pass.Run(package, context) == Optimization::PassChange::Changed);

    const auto &remaining = package.modules[0].funcs[0].blocks[0].instrs;
    CHECK_FALSE(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 7; }));
    CHECK_FALSE(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 11; }));
    CHECK_FALSE(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 14; }));
    CHECK(std::ranges::count_if(remaining,
                                [](const LirInstr &instruction) { return instruction.op == LirOpcode::Store; }) == 3);
    CHECK(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 8; }));
    CHECK(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 12; }));
    CHECK(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 13; }));
    CHECK(std::ranges::any_of(remaining, [](const LirInstr &instruction) { return instruction.dst == 10; }));
}

TEST_CASE("LIR dead code elimination preserves escaped addresses and observable instructions") {
    LirFunc function;
    function.name = "Effects";
    function.params = {{0, TypeRef::MakePointer(TypeRef::MakeOpaque()), "callee"},
                       {1, TypeRef::MakeBool(), "condition"}};
    function.blocks.resize(1);

    LirInstr slot;
    slot.op = LirOpcode::Alloca;
    slot.dst = 2;
    slot.type = TypeRef::MakeInt32();
    function.blocks[0].instrs.push_back(std::move(slot));

    LirInstr value;
    value.op = LirOpcode::Const;
    value.dst = 3;
    value.type = TypeRef::MakeInt32();
    value.strArg = "1";
    function.blocks[0].instrs.push_back(std::move(value));

    LirInstr store;
    store.op = LirOpcode::Store;
    store.srcs = {3, 2};
    store.type = TypeRef::MakeInt32();
    function.blocks[0].instrs.push_back(std::move(store));

    LirInstr directCall;
    directCall.op = LirOpcode::Call;
    directCall.dst = LirNoReg;
    directCall.srcs = {2};
    directCall.strArg = "Escape";
    function.blocks[0].instrs.push_back(std::move(directCall));

    LirInstr indirectCall;
    indirectCall.op = LirOpcode::CallIndirect;
    indirectCall.dst = 4;
    indirectCall.type = TypeRef::MakeInt32();
    indirectCall.srcs = {0};
    function.blocks[0].instrs.push_back(std::move(indirectCall));

    LirInstr assertion;
    assertion.op = LirOpcode::Assert;
    assertion.dst = LirNoReg;
    assertion.srcs = {1, 2};
    function.blocks[0].instrs.push_back(std::move(assertion));

    LirInstr panic;
    panic.op = LirOpcode::Panic;
    panic.dst = LirNoReg;
    panic.srcs = {2};
    function.blocks[0].instrs.push_back(std::move(panic));
    function.blocks[0].term = UnreachableTerminator();

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Optimization::LirDeadCodeElimination pass;
    const Optimization::PassContext context{BuildProfile::Release};
    CHECK(pass.Run(package, context) == Optimization::PassChange::None);
    CHECK(package.modules[0].funcs[0].blocks[0].instrs.size() == 7);
}
