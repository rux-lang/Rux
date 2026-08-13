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

using namespace Rux;

static HirPackage CompileToHir(const std::string &source) {
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

static HirPackage CompileAndOptimize(const std::string &source) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return package;
}

static LirPackage CompileToLir(const std::string &source, const BuildProfile profile) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(profile);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return HirToLirLowering(std::move(package), TargetContext::CreateNative()).Generate();
}

namespace {
class RecordingHirPass final : public Optimization::HirPass {
public:
    RecordingHirPass(std::string_view name, std::vector<std::string_view> &runs, std::size_t changesRemaining)
        : name_(name)
        , runs_(runs)
        , changesRemaining_(changesRemaining) {
    }

    [[nodiscard]] std::string_view Name() const noexcept override {
        return name_;
    }

    Optimization::PassChange Run(HirPackage &, const Optimization::PassContext &context) override {
        runs_.push_back(name_);
        contexts.push_back(context);
        if (changesRemaining_ == 0) {
            return Optimization::PassChange::None;
        }
        --changesRemaining_;
        return Optimization::PassChange::Changed;
    }

    std::vector<Optimization::PassContext> contexts;

private:
    std::string_view name_;
    std::vector<std::string_view> &runs_;
    std::size_t changesRemaining_;
};

LirTerminator JumpTo(const std::uint32_t target) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Jump;
    terminator.trueTarget = target;
    return terminator;
}

LirTerminator BranchTo(const LirReg condition, const std::uint32_t trueTarget, const std::uint32_t falseTarget) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Branch;
    terminator.cond = condition;
    terminator.trueTarget = trueTarget;
    terminator.falseTarget = falseTarget;
    return terminator;
}

LirTerminator ReturnValue(const std::optional<LirReg> value = std::nullopt, TypeRef type = {}) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Return;
    terminator.retVal = value;
    terminator.retType = std::move(type);
    return terminator;
}

LirTerminator UnreachableTerminator() {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Unreachable;
    return terminator;
}
} // namespace

TEST_CASE("optimization pipelines are selected explicitly by profile") {
    auto debug = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    auto release = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);

    CHECK(debug.HirPassNames().empty());
    CHECK(debug.LirPassNames() == std::vector<std::string_view>{"lir-cfg-verifier"});
    CHECK(release.HirPassNames() == std::vector<std::string_view>{"hir-constant-folder"});
    CHECK(release.LirPassNames() == std::vector<std::string_view>{"lir-cfg-verifier", "lir-constant-copy-propagation",
                                                                  "lir-dead-code-elimination", "lir-cfg-cleanup"});
}

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

TEST_CASE("executable LIR reachability follows package-wide code and data edges from Main") {
    LirModule entry;
    entry.name = "Entry";
    entry.funcs.resize(4);
    entry.funcs[0].name = "Main";
    entry.funcs[0].blocks.resize(1);
    entry.funcs[1].name = "DeadPublic";
    entry.funcs[1].isPublic = true;
    entry.funcs[2].name = "ExternLeaf";
    entry.funcs[2].isExtern = true;
    entry.funcs[2].blocks.resize(1);
    entry.funcs[3].name = "HiddenBehindExtern";

    const auto append = [&](const LirOpcode opcode, std::string symbol) {
        LirInstr instruction;
        instruction.op = opcode;
        instruction.strArg = std::move(symbol);
        entry.funcs[0].blocks[0].instrs.push_back(std::move(instruction));
    };
    append(LirOpcode::Call, "Support::CrossModule");
    append(LirOpcode::GlobalAddr, "AddressTaken");
    append(LirOpcode::GlobalAddr, "__vtable__Widget__Display");
    append(LirOpcode::Load, "&Support::PrivateData");
    append(LirOpcode::Call, "ExternLeaf");

    // Even a malformed extern carrying blocks must remain a leaf: its body is
    // not a package-owned definition and cannot introduce reachability.
    LirInstr hiddenCall;
    hiddenCall.op = LirOpcode::Call;
    hiddenCall.strArg = "HiddenBehindExtern";
    entry.funcs[2].blocks[0].instrs.push_back(std::move(hiddenCall));

    LirModule support;
    support.name = "Support";
    support.funcs.resize(4);
    support.funcs[0].name = "CrossModule";
    support.funcs[1].name = "AddressTaken";
    support.funcs[2].name = "Widget::Display";
    support.funcs[3].name = "DeadSupport";
    support.consts.resize(1);
    support.consts[0].name = "PrivateData";
    support.vtables.push_back({"__vtable__Widget__Display", {"Widget::Display"}});

    LirPackage package;
    package.modules.push_back(std::move(entry));
    package.modules.push_back(std::move(support));

    const auto result = Optimization::LirReachabilityAnalysis::Run(package, ArtifactKind::Executable);
    using Kind = Optimization::LirDeclarationKind;
    using Id = Optimization::LirDeclarationId;

    CHECK(result.IsReachable(Id{Kind::Function, 0, 0}));
    CHECK_FALSE(result.IsReachable(Id{Kind::Function, 0, 1}));
    CHECK(result.IsReachable(Id{Kind::Function, 0, 2}));
    CHECK_FALSE(result.IsReachable(Id{Kind::Function, 0, 3}));
    CHECK(result.IsReachable(Id{Kind::Function, 1, 0}));
    CHECK(result.IsReachable(Id{Kind::Function, 1, 1}));
    CHECK(result.IsReachable(Id{Kind::Function, 1, 2}));
    CHECK_FALSE(result.IsReachable(Id{Kind::Function, 1, 3}));
    CHECK(result.IsReachable(Id{Kind::Constant, 1, 0}));
    CHECK(result.IsReachable(Id{Kind::Vtable, 1, 0}));
    CHECK(result.ReachableDeclarations().size() == 7);
}

TEST_CASE("library LIR reachability roots public code and data for both artifact kinds") {
    LirModule module;
    module.name = "Library";
    module.funcs.resize(5);
    module.funcs[0].name = "PublicApi";
    module.funcs[0].isPublic = true;
    module.funcs[0].blocks.resize(1);
    module.funcs[1].name = "PrivateHelper";
    module.funcs[2].name = "DeadFunction";
    module.funcs[3].name = "PublicExtern";
    module.funcs[3].isPublic = true;
    module.funcs[3].isExtern = true;
    module.funcs[3].blocks.resize(1);
    module.funcs[4].name = "Widget::Render";

    LirInstr helperCall;
    helperCall.op = LirOpcode::Call;
    helperCall.strArg = "PrivateHelper";
    module.funcs[0].blocks[0].instrs.push_back(std::move(helperCall));
    LirInstr privateDataLoad;
    privateDataLoad.op = LirOpcode::Load;
    privateDataLoad.strArg = "PrivateVariable";
    module.funcs[0].blocks[0].instrs.push_back(std::move(privateDataLoad));

    LirInstr deadExternBodyCall;
    deadExternBodyCall.op = LirOpcode::Call;
    deadExternBodyCall.strArg = "DeadFunction";
    module.funcs[3].blocks[0].instrs.push_back(std::move(deadExternBodyCall));

    module.consts.resize(3);
    module.consts[0].name = "PublicData";
    module.consts[0].isPublic = true;
    module.consts[0].value = "PrivateData";
    module.consts[1].name = "PrivateData";
    module.consts[2].name = "DeadData";
    module.vtables.push_back({"__vtable__Widget__Render", {"Widget::Render"}});
    module.externVars = {{"PublicVariable", true, TypeRef::MakeInt32()},
                         {"PrivateVariable", false, TypeRef::MakeInt32()},
                         {"UnusedVariable", false, TypeRef::MakeInt32()}};

    LirPackage package;
    package.modules.push_back(std::move(module));
    const auto shared = Optimization::LirReachabilityAnalysis::Run(package, ArtifactKind::SharedLibrary);
    const auto archive = Optimization::LirReachabilityAnalysis::Run(package, ArtifactKind::StaticLibrary);
    using Kind = Optimization::LirDeclarationKind;
    using Id = Optimization::LirDeclarationId;

    CHECK(shared.ReachableDeclarations() == archive.ReachableDeclarations());
    CHECK(shared.IsReachable(Id{Kind::Function, 0, 0}));
    CHECK(shared.IsReachable(Id{Kind::Function, 0, 1}));
    CHECK_FALSE(shared.IsReachable(Id{Kind::Function, 0, 2}));
    CHECK(shared.IsReachable(Id{Kind::Function, 0, 3}));
    CHECK(shared.IsReachable(Id{Kind::Function, 0, 4}));
    CHECK(shared.IsReachable(Id{Kind::Constant, 0, 0}));
    CHECK(shared.IsReachable(Id{Kind::Constant, 0, 1}));
    CHECK_FALSE(shared.IsReachable(Id{Kind::Constant, 0, 2}));
    CHECK(shared.IsReachable(Id{Kind::Vtable, 0, 0}));
    CHECK(shared.IsReachable(Id{Kind::ExternVariable, 0, 0}));
    CHECK(shared.IsReachable(Id{Kind::ExternVariable, 0, 1}));
    CHECK_FALSE(shared.IsReachable(Id{Kind::ExternVariable, 0, 2}));
    CHECK(shared.ReachableDeclarations().size() == 9);
}

TEST_CASE("pass pipeline reports changes and preserves explicit order") {
    HirPackage package;
    std::vector<std::string_view> runs;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 4);
    auto first = std::make_unique<RecordingHirPass>("first", runs, 1);
    auto *firstObserver = first.get();
    pipeline.Add(std::move(first));
    pipeline.Add(std::make_unique<RecordingHirPass>("second", runs, 0));

    const auto report = pipeline.Run(package);

    CHECK(report.change == Optimization::PassChange::Changed);
    CHECK(report.reachedFixedPoint);
    CHECK(report.iterations == 2);
    CHECK(runs == std::vector<std::string_view>{"first", "second", "first", "second"});
    REQUIRE(firstObserver->contexts.size() == 2);
    CHECK(firstObserver->contexts[0].iteration == 0);
    CHECK(firstObserver->contexts[1].iteration == 1);
    CHECK(firstObserver->contexts[0].fixedPointLimit == 4);
}

TEST_CASE("pass pipeline stops at its fixed-point limit") {
    HirPackage package;
    std::vector<std::string_view> runs;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 3);
    pipeline.Add(std::make_unique<RecordingHirPass>("never-settles", runs, 10));

    const auto report = pipeline.Run(package);

    CHECK(report.change == Optimization::PassChange::Changed);
    CHECK_FALSE(report.reachedFixedPoint);
    CHECK(report.iterations == 3);
    CHECK(runs.size() == 3);
}

TEST_CASE("independent optimization pipelines do not share constant-folding analysis state") {
    HirPackage firstPackage;
    HirModule firstModule;
    HirFunc firstFunction;
    HirBlock firstBody;
    auto binding = std::make_unique<HirLetStmt>();
    binding->name = "private-value";
    binding->type = TypeRef::MakeInt32();
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->type = TypeRef::MakeInt32();
    literal->value = "41";
    binding->init = std::move(literal);
    firstBody.stmts.push_back(std::move(binding));
    firstFunction.body = std::move(firstBody);
    firstModule.funcs.push_back(std::move(firstFunction));
    firstPackage.modules.push_back(std::move(firstModule));

    HirPackage secondPackage;
    HirModule secondModule;
    HirConst independentConstant;
    auto independentValue = std::make_unique<HirVarExpr>();
    independentValue->name = "private-value";
    independentValue->type = TypeRef::MakeInt32();
    independentConstant.value = std::move(independentValue);
    secondModule.consts.push_back(std::move(independentConstant));
    secondPackage.modules.push_back(std::move(secondModule));

    auto firstPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    auto secondPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    CHECK(firstPipeline.RunHir(firstPackage).reachedFixedPoint);
    CHECK(secondPipeline.RunHir(secondPackage).reachedFixedPoint);

    CHECK(dynamic_cast<HirVarExpr *>(secondPackage.modules[0].consts[0].value.get()) != nullptr);
}

TEST_CASE("HIR-to-LIR optimization is gated by the build profile") {
    const std::string source = R"(
        func Main() -> int {
            if true {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    const auto debug = CompileToLir(source, BuildProfile::Debug);
    const auto release = CompileToLir(source, BuildProfile::Release);
    REQUIRE(debug.modules.size() == 1);
    REQUIRE(release.modules.size() == 1);
    REQUIRE(debug.modules[0].funcs.size() == 1);
    REQUIRE(release.modules[0].funcs.size() == 1);

    const auto &debugBlocks = debug.modules[0].funcs[0].blocks;
    const auto &releaseBlocks = release.modules[0].funcs[0].blocks;
    CHECK(debugBlocks.size() > releaseBlocks.size());
    CHECK(std::ranges::any_of(
        debugBlocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
    CHECK_FALSE(std::ranges::any_of(
        releaseBlocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
}

TEST_CASE("optimizer eliminates dead code after return") {
    std::string source = R"(
        func Main() -> int {
            return 42;
            let x = 10;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le corps de la fonction ne doit contenir que l'instruction return.
    // L'instruction "let x = 10;" doit être éliminée.

    CHECK(func.body->stmts.size() == 1);
    CHECK(dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get()) != nullptr);
}

TEST_CASE("optimizer folds constant true condition branch") {
    std::string source = R"(
        func Main() -> int {
            if true {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le bloc conditionnel if true doit être remplacé par les instructions de sa branche true.
    // "return 2;" dans la branche false ne doit pas être présent.
    REQUIRE(func.body->stmts.size() == 1);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);

    // Le retour doit être une constante litérale avec la valeur "1"
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "1");
}

TEST_CASE("optimizer folds constant false condition branch") {
    std::string source = R"(
        func Main() -> int {
            if false {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le bloc conditionnel if false doit être remplacé par la branche else.
    REQUIRE(func.body->stmts.size() == 1);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);

    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "2");
}

TEST_CASE("optimizer combines constant propagation, condition folding and dead code elimination") {
    std::string source = R"(
        func Main() -> int {
            let a = 1;
            if a == 1 {
                return 10;
            }
            return 20;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // 1. "let a = 1;" est conservé
    // 2. "if a == 1" est plié en "if true", donc aplati en "return 10;"
    // 3. "return 20;" après "return 10;" est éliminé en tant que code mort
    REQUIRE(func.body->stmts.size() == 2);

    auto *let = dynamic_cast<HirLetStmt *>(func.body->stmts[0].get());
    REQUIRE(let != nullptr);
    CHECK(let->name == "a");

    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[1].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "10");
}

TEST_CASE("optimizer folds integer wrapping and parses hex/binary literals") {
    std::string source = R"(
        func Main() -> int {
            let a: int32 = -2147483648i32;
            let b: int32 = 2147483647i32 + 1i32;
            let c: int64 = 0x00000000FFFFFFFFi64;
            if b == a && c == 4294967295i64 {
                return 100;
            }
            return 200;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    REQUIRE(func.body->stmts.size() == 4);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[3].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "100");
}

TEST_CASE("optimizer does not discard calls or potentially trapping expressions") {
    const std::string source = R"(
        func Observe() -> int {
            return 7;
        }

        func Main(value: int) -> int {
            let product = 0 * Observe();
            let remainder = Observe() % 1;
            let trap = 0 * (10 / value);
            return product + remainder + trap;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 2);
    auto &body = *package.modules[0].funcs[1].body;
    REQUIRE(body.stmts.size() == 4);

    const auto *product = dynamic_cast<const HirLetStmt *>(body.stmts[0].get());
    const auto *remainder = dynamic_cast<const HirLetStmt *>(body.stmts[1].get());
    const auto *trap = dynamic_cast<const HirLetStmt *>(body.stmts[2].get());
    REQUIRE(product != nullptr);
    REQUIRE(remainder != nullptr);
    REQUIRE(trap != nullptr);

    const auto *productExpr = dynamic_cast<const HirBinaryExpr *>(product->init.get());
    const auto *remainderExpr = dynamic_cast<const HirBinaryExpr *>(remainder->init.get());
    const auto *trapExpr = dynamic_cast<const HirBinaryExpr *>(trap->init.get());
    REQUIRE(productExpr != nullptr);
    REQUIRE(remainderExpr != nullptr);
    REQUIRE(trapExpr != nullptr);
    CHECK(dynamic_cast<const HirCallExpr *>(productExpr->right.get()) != nullptr);
    CHECK(dynamic_cast<const HirCallExpr *>(remainderExpr->left.get()) != nullptr);
    CHECK(dynamic_cast<const HirBinaryExpr *>(trapExpr->right.get()) != nullptr);
}

TEST_CASE("optimizer keeps lexical shadowing isolated") {
    const std::string source = R"(
        func Main(condition: bool) -> int {
            let value = 1;
            if condition {
                let value = 2;
                if condition {
                    return value;
                }
            }
            return value;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 3);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[2].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK(dynamic_cast<const HirVarExpr *>(returned->value->get()) != nullptr);
}

TEST_CASE("optimizer never substitutes an address operand") {
    const std::string source = R"(
        func Main() -> int {
            let value: int = 41;
            let pointer = @value;
            return value;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 3);
    const auto *pointer = dynamic_cast<const HirLetStmt *>(body.stmts[1].get());
    REQUIRE(pointer != nullptr);
    const auto *address = dynamic_cast<const HirUnaryExpr *>(pointer->init.get());
    REQUIRE(address != nullptr);
    CHECK(address->op == TokenKind::At);
    CHECK(dynamic_cast<const HirVarExpr *>(address->operand.get()) != nullptr);

    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[2].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK(dynamic_cast<const HirLiteralExpr *>(returned->value->get()) != nullptr);
}

TEST_CASE("optimizer invalidates facts across loops and mutation") {
    const std::string source = R"(
        func Main(condition: bool) -> int {
            let constant = 3;
            var mutable = 1;
            while condition {
                let inside = constant;
                mutable = inside;
            }
            mutable = constant;
            return constant + mutable;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 5);
    const auto *loop = dynamic_cast<const HirWhileStmt *>(body.stmts[2].get());
    REQUIRE(loop != nullptr);
    const auto *inside = dynamic_cast<const HirLetStmt *>(loop->body.stmts[0].get());
    REQUIRE(inside != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(inside->init.get()) != nullptr);

    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[4].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *sum = dynamic_cast<const HirBinaryExpr *>(returned->value->get());
    REQUIRE(sum != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(sum->left.get()) != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(sum->right.get()) != nullptr);
}
