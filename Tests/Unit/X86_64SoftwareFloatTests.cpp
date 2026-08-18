#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"

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
} // namespace

TEST_CASE("x86-64 classifies non-native floats as software ABI values") {
    CHECK(IsNativeFloat(Float(TypeRef::Kind::Float32)));
    CHECK(IsNativeFloat(Float(TypeRef::Kind::Float64)));
    for (const TypeRef::Kind kind : {TypeRef::Kind::Float8, TypeRef::Kind::Float16, TypeRef::Kind::Float80,
                                     TypeRef::Kind::Float128, TypeRef::Kind::Float256, TypeRef::Kind::Float512}) {
        CAPTURE(static_cast<int>(kind));
        CHECK(IsSoftwareFloat(Float(kind)));
        CHECK_FALSE(IsNativeFloat(Float(kind)));
    }
}

TEST_CASE("x86-64 frame plans follow Win64 software-float aggregate rules") {
    const TypeRef float80 = Float(TypeRef::Kind::Float80);
    const X86_64FramePlan extended = PlanX86_64Frame(Identity("Extended", float80), {}, {}, Target::OS::Windows);
    CHECK_NE(extended.HiddenReturnOffset(), 0);
    CHECK_EQ(extended.RegisterTypes().at(0), float80);
    CHECK_GE(extended.SlotOffsets().at(0), 16);

    const TypeRef float8 = Float(TypeRef::Kind::Float8);
    const X86_64FramePlan tiny = PlanX86_64Frame(Identity("Tiny", float8), {}, {}, Target::OS::Windows);
    CHECK_EQ(tiny.HiddenReturnOffset(), 0);
    CHECK(tiny.PhysicalRegisters().contains(0));
}

TEST_CASE("x86-64 frame plans follow SysV software-float aggregate rules") {
    const TypeRef float128 = Float(TypeRef::Kind::Float128);
    const TypeRef float256 = Float(TypeRef::Kind::Float256);
    const X86_64FramePlan twoLanes = PlanX86_64Frame(Identity("TwoLanes", float128), {}, {}, Target::OS::Linux);
    const X86_64FramePlan memory = PlanX86_64Frame(Identity("Memory", float256), {}, {}, Target::OS::Linux);
    CHECK_EQ(twoLanes.HiddenReturnOffset(), 0);
    CHECK_NE(memory.HiddenReturnOffset(), 0);
    CHECK_GE(memory.SlotOffsets().at(0), 32);
}

TEST_CASE("x86-64 RCU materializes and negates every software-float width") {
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

    for (const Target::OS os : {Target::OS::Windows, Target::OS::Linux}) {
        CAPTURE(static_cast<int>(os));
        RcuEmitter emitter(package, "SoftwareFloat", os);
        const auto objects = emitter.Generate();
        CHECK(emitter.Diagnostics().empty());
        REQUIRE_EQ(objects.size(), 1);
        CHECK_GE(objects.front().symbols.size(), 12);
        CHECK_FALSE(objects.front().sections[RCU_TEXT_IDX].data.empty());
    }
}
