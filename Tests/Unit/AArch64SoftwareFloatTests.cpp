#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/Layout.h"

#include <array>
#include <doctest.h>

using namespace Rux;
using namespace Rux::Layout;

namespace {
[[nodiscard]] TypeRef Float(const TypeRef::Kind kind) {
    return TypeRef::MakePrimitive(kind);
}

[[nodiscard]] LirFunc Identity(const std::string &name, const TypeRef &type) {
    LirFunc function;
    function.name = name;
    function.returnType = type;
    function.params.push_back({0, type, "value"});
    LirBlock block;
    block.label = "entry";
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 0;
    block.term->retType = type;
    function.blocks.push_back(std::move(block));
    return function;
}

[[nodiscard]] LirFunc ConstantAndNegate(const std::string &name, const TypeRef &type) {
    LirFunc function;
    function.name = name;
    function.returnType = type;
    LirInstr constant;
    constant.op = LirOpcode::Const;
    constant.dst = 0;
    constant.type = type;
    constant.strArg = "1.5";
    LirInstr negate;
    negate.op = LirOpcode::Neg;
    negate.dst = 1;
    negate.type = type;
    negate.srcs = {0};
    LirBlock block;
    block.label = "entry";
    block.instrs = {constant, negate};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 1;
    block.term->retType = type;
    function.blocks.push_back(std::move(block));
    return function;
}

[[nodiscard]] AArch64CallPlanner Planner(const Target::OS os) {
    static const LayoutMap noLayouts;
    static const std::unordered_set<std::string> noInterfaces;
    static const std::vector<LirStructDecl> noDeclarations;
    return AArch64CallPlanner(noLayouts, noInterfaces, noDeclarations, os);
}
} // namespace

TEST_CASE("AArch64 uses integer ABI lanes for software floats") {
    for (const Target::OS os : {Target::OS::Windows, Target::OS::Linux, Target::OS::FreeBSD, Target::OS::MacOS}) {
        CAPTURE(static_cast<int>(os));
        const AArch64CallPlanner planner = Planner(os);

        const AArch64ArgumentLocation tiny = planner.PlanResult(Float(TypeRef::Kind::Float8));
        CHECK_EQ(tiny.kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(tiny.count, 1);

        const AArch64ArgumentLocation quad = planner.PlanResult(Float(TypeRef::Kind::Float128));
        CHECK_EQ(quad.kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(quad.count, 2);
        CHECK_FALSE(planner.ReturnsInMemory(Float(TypeRef::Kind::Float128)));
        CHECK(planner.ReturnsInMemory(Float(TypeRef::Kind::Float256)));
    }
}

TEST_CASE("AArch64 frame plans reserve indirect software-float results") {
    const TypeRef float128 = Float(TypeRef::Kind::Float128);
    const TypeRef float256 = Float(TypeRef::Kind::Float256);
    for (const Target::OS os : {Target::OS::Windows, Target::OS::Linux, Target::OS::FreeBSD, Target::OS::MacOS}) {
        CAPTURE(static_cast<int>(os));
        const AArch64FramePlan lanes = PlanAArch64Frame(Identity("Lanes", float128), {}, {}, {}, os);
        const AArch64FramePlan indirect = PlanAArch64Frame(Identity("Indirect", float256), {}, {}, {}, os);
        CHECK_EQ(lanes.IndirectResultOffset(), 0);
        CHECK_NE(indirect.IndirectResultOffset(), 0);
        CHECK_GE(indirect.SlotOffsets().at(0), 24);
    }
}

TEST_CASE("AArch64 RCU materializes and negates every software-float width") {
    LirModule module;
    module.name = "SoftwareFloat.rux";
    for (const TypeRef::Kind kind : {TypeRef::Kind::Float8, TypeRef::Kind::Float16, TypeRef::Kind::Float80,
                                     TypeRef::Kind::Float128, TypeRef::Kind::Float256, TypeRef::Kind::Float512}) {
        const TypeRef type = Float(kind);
        module.funcs.push_back(Identity("Identity" + std::to_string(static_cast<int>(kind)), type));
        module.funcs.push_back(ConstantAndNegate("Negate" + std::to_string(static_cast<int>(kind)), type));
    }
    LirPackage package;
    package.modules.push_back(std::move(module));

    constexpr std::array profiles = {Target::OS::Windows, Target::OS::Linux, Target::OS::FreeBSD, Target::OS::MacOS};
    for (const Target::OS os : profiles) {
        CAPTURE(static_cast<int>(os));
        AArch64RcuEmitter emitter(package, "SoftwareFloat", os);
        const auto objects = emitter.Generate();
        CHECK(emitter.Diagnostics().empty());
        REQUIRE_EQ(objects.size(), 1);
        CHECK_GE(objects.front().symbols.size(), 12);
        CHECK_FALSE(objects.front().sections[RCU_TEXT_IDX].data.empty());
    }
}
