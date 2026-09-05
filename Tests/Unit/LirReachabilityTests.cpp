#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

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

TEST_CASE("Release prunes unreachable declarations deterministically while Debug retains them") {
    LirModule module;
    module.name = "Program";
    module.funcs.push_back(ReturningFunction("Main"));
    module.funcs.push_back(ReturningFunction("Live"));
    module.funcs.push_back(ReturningFunction("Dead"));
    module.funcs.push_back(ExternFunction("UsedExtern"));
    module.funcs.push_back(ExternFunction("UnusedExtern"));

    const auto appendReference = [&](const LirOpcode opcode, std::string name) {
        LirInstr instruction;
        instruction.op = opcode;
        instruction.strArg = std::move(name);
        module.funcs[0].blocks[0].instrs.push_back(std::move(instruction));
    };
    appendReference(LirOpcode::Call, "Live");
    appendReference(LirOpcode::Call, "UsedExtern");
    appendReference(LirOpcode::Load, "LiveData");
    appendReference(LirOpcode::Load, "LiveVariable");
    appendReference(LirOpcode::Load, "LiveVtable");

    module.consts = {Constant("LiveData"), Constant("DeadData", false, {"1", "2"})};
    module.vtables = {{"LiveVtable", {"Live"}}, {"DeadVtable", {"Dead"}}};
    module.externVars = {{"LiveVariable", false, TypeRef::MakeInt32()},
                         {"UnusedVariable", false, TypeRef::MakeInt32()}};

    LirPackage original;
    original.modules.push_back(std::move(module));
    auto debugPackage = original;
    auto firstReleasePackage = original;
    auto secondReleasePackage = original;

    auto debugPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug, ArtifactKind::Executable);
    const auto debugReport = debugPipeline.RunLir(debugPackage);
    REQUIRE(debugReport.reachedFixedPoint);
    CHECK(debugReport.lirPruning.TotalDeclarations() == 0);
    CHECK(debugPackage.modules[0].funcs.size() == 5);
    CHECK(debugPackage.modules[0].consts.size() == 2);
    CHECK(debugPackage.modules[0].vtables.size() == 2);
    CHECK(debugPackage.modules[0].externVars.size() == 2);

    auto firstPipeline =
        Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release, ArtifactKind::Executable);
    auto secondPipeline =
        Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release, ArtifactKind::Executable);
    const auto firstReport = firstPipeline.RunLir(firstReleasePackage);
    const auto secondReport = secondPipeline.RunLir(secondReleasePackage);
    REQUIRE(firstReport.reachedFixedPoint);
    REQUIRE(secondReport.reachedFixedPoint);

    const auto &stats = firstReport.lirPruning;
    CHECK(stats.functionDefinitions == 1);
    CHECK(stats.constants == 1);
    CHECK(stats.vtables == 1);
    CHECK(stats.externDeclarations == 2);
    CHECK(stats.TotalDeclarations() == 5);
    CHECK(stats.estimatedIrNodes == 10);
    CHECK(secondReport.lirPruning.functionDefinitions == stats.functionDefinitions);
    CHECK(secondReport.lirPruning.constants == stats.constants);
    CHECK(secondReport.lirPruning.vtables == stats.vtables);
    CHECK(secondReport.lirPruning.externDeclarations == stats.externDeclarations);
    CHECK(secondReport.lirPruning.estimatedIrNodes == stats.estimatedIrNodes);

    const auto declarationNames = [](const LirPackage &package) {
        std::vector<std::string> names;
        const auto &resultModule = package.modules[0];
        for (const auto &function : resultModule.funcs) {
            names.push_back(function.name);
        }
        for (const auto &constant : resultModule.consts) {
            names.push_back(constant.name);
        }
        for (const auto &vtable : resultModule.vtables) {
            names.push_back(vtable.label);
        }
        for (const auto &variable : resultModule.externVars) {
            names.push_back(variable.name);
        }
        return names;
    };
    CHECK(declarationNames(firstReleasePackage) ==
          std::vector<std::string>{"Main", "Live", "UsedExtern", "LiveData", "LiveVtable", "LiveVariable"});
    CHECK(declarationNames(secondReleasePackage) == declarationNames(firstReleasePackage));
}

TEST_CASE("Release library pruning preserves public APIs and their private dependencies") {
    LirModule module;
    module.name = "Library";
    module.funcs.push_back(ReturningFunction("PublicApi"));
    module.funcs.back().isPublic = true;
    module.funcs.push_back(ReturningFunction("PrivateHelper"));
    module.funcs.push_back(ReturningFunction("DeadPrivate"));
    module.funcs.push_back(ExternFunction("PublicExtern", true));
    LirInstr call;
    call.op = LirOpcode::Call;
    call.strArg = "PrivateHelper";
    module.funcs[0].blocks[0].instrs.push_back(std::move(call));
    module.consts = {Constant("PublicData", true), Constant("DeadData")};
    module.externVars = {{"PublicVariable", true, TypeRef::MakeInt32()},
                         {"UnusedVariable", false, TypeRef::MakeInt32()}};

    LirPackage package;
    package.modules.push_back(std::move(module));
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release, ArtifactKind::SharedLibrary);
    const auto report = pipeline.RunLir(package);

    REQUIRE(report.reachedFixedPoint);
    CHECK(report.lirPruning.functionDefinitions == 1);
    CHECK(report.lirPruning.constants == 1);
    CHECK(report.lirPruning.externDeclarations == 1);
    REQUIRE(package.modules[0].funcs.size() == 3);
    CHECK(package.modules[0].funcs[0].name == "PublicApi");
    CHECK(package.modules[0].funcs[1].name == "PrivateHelper");
    CHECK(package.modules[0].funcs[2].name == "PublicExtern");
    REQUIRE(package.modules[0].consts.size() == 1);
    CHECK(package.modules[0].consts[0].name == "PublicData");
    REQUIRE(package.modules[0].externVars.size() == 1);
    CHECK(package.modules[0].externVars[0].name == "PublicVariable");
}

TEST_CASE("both RCU backends omit pruned Release symbols and retain Debug symbols") {
    LirModule module;
    module.name = "Symbols";
    module.funcs.push_back(ReturningFunction("Main"));
    module.funcs.push_back(ReturningFunction("Unused"));
    LirPackage original;
    original.modules.push_back(std::move(module));
    auto debugPackage = original;
    auto releasePackage = original;

    auto debugPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug, ArtifactKind::Executable);
    auto releasePipeline =
        Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release, ArtifactKind::Executable);
    REQUIRE(debugPipeline.RunLir(debugPackage).reachedFixedPoint);
    REQUIRE(releasePipeline.RunLir(releasePackage).reachedFixedPoint);

    const auto x86Debug = RcuEmitter(debugPackage, "test", Target::OS::Linux).Generate();
    const auto x86Release = RcuEmitter(releasePackage, "test", Target::OS::Linux).Generate();
    const auto aarch64Debug = AArch64RcuEmitter(debugPackage, "test", Target::OS::Linux).Generate();
    const auto aarch64Release = AArch64RcuEmitter(releasePackage, "test", Target::OS::Linux).Generate();

    CHECK(HasRcuSymbol(x86Debug, "Unused"));
    CHECK_FALSE(HasRcuSymbol(x86Release, "Unused"));
    CHECK(HasRcuSymbol(aarch64Debug, "Unused"));
    CHECK_FALSE(HasRcuSymbol(aarch64Release, "Unused"));
    CHECK(HasRcuSymbol(x86Release, "Main"));
    CHECK(HasRcuSymbol(aarch64Release, "Main"));
}
