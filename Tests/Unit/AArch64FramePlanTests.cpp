#include "CodeGen/AArch64/FramePlan.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <ranges>
#include <vector>

using namespace Rux;
using namespace Rux::Layout;

namespace {
[[nodiscard]] LirInstr Define(const LirReg dst, const TypeRef &type = TypeRef::MakeInt64()) {
    LirInstr instruction;
    instruction.op = LirOpcode::Const;
    instruction.dst = dst;
    instruction.type = type;
    instruction.strArg = "0";
    return instruction;
}

[[nodiscard]] LirTerminator JumpTo(const std::uint32_t target) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Jump;
    terminator.trueTarget = target;
    return terminator;
}
} // namespace

TEST_CASE("AArch64 frame plan owns hidden results aggregate regions phi scratch and alignment") {
    const TypeRef triple = TypeRef::MakeNamed("Triple");
    LayoutMap layouts;
    layouts["Triple"] = StructLayout{.fields = {}, .totalSize = 24, .alignment = 8};

    LirFunc function;
    function.name = "Rotate";
    function.returnType = triple;
    function.params.push_back({0, triple, "input"});

    LirInstr alloca;
    alloca.op = LirOpcode::Alloca;
    alloca.dst = 1;
    alloca.type = TypeRef::MakeInt64();
    alloca.strArg = "3";

    LirBlock entry;
    entry.instrs = {alloca, Define(2), Define(3)};
    entry.term = JumpTo(1);

    LirInstr firstPhi;
    firstPhi.op = LirOpcode::Phi;
    firstPhi.dst = 4;
    firstPhi.type = TypeRef::MakeInt64();
    firstPhi.phiPreds = {{2, 0}, {5, 1}};
    LirInstr secondPhi = firstPhi;
    secondPhi.dst = 5;
    secondPhi.phiPreds = {{3, 0}, {4, 1}};

    LirBlock loop;
    loop.instrs = {firstPhi, secondPhi};
    loop.term = JumpTo(1);
    function.blocks = {std::move(entry), std::move(loop)};

    const AArch64FramePlan plan = PlanAArch64Frame(function, layouts, {}, {}, Target::OS::Linux);

    CHECK_EQ(plan.IndirectResultOffset(), 16);
    CHECK_EQ(plan.CalleeSaveOffset(), 0);
    CHECK_EQ(plan.SlotOffsets().at(0), 24);
    CHECK_EQ(plan.SlotOffsets().at(1), 48);
    CHECK_EQ(plan.AllocaDataOffsets().at(1), 56);
    CHECK_EQ(plan.RegisterTypes().at(1).kind, TypeRef::Kind::Pointer);
    CHECK_EQ(plan.PhiMoves().at(1).at(1).size(), 2);
    CHECK_EQ(plan.PhiTemporarySize(), 8);
    CHECK_EQ(plan.PhiTemporaryOffset(), 112);
    CHECK_EQ(plan.FrameSize(), 128);
    CHECK_EQ(plan.FrameSize() % 16, 0);
}

TEST_CASE("AArch64 frame plan reserves X18 and records general and vector save runs") {
    LirFunc function;
    function.name = "Saturate";
    function.returnType = TypeRef::MakeInt64();
    for (LirReg reg = 0; reg < 11; ++reg) {
        function.params.push_back({reg, TypeRef::MakeInt64(), "integer"});
    }
    for (LirReg reg = 11; reg < 20; ++reg) {
        function.params.push_back({reg, TypeRef::MakeFloat64(), "float"});
    }

    LirInstr combine = Define(20);
    combine.op = LirOpcode::Add;
    combine.srcs = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    LirInstr combineFloats = Define(21, TypeRef::MakeFloat64());
    combineFloats.op = LirOpcode::Add;
    combineFloats.srcs = {11, 12, 13, 14, 15, 16, 17, 18, 19};
    LirBlock block;
    block.instrs = {std::move(combine), std::move(combineFloats)};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 20;
    block.term->retType = TypeRef::MakeInt64();
    function.blocks.push_back(std::move(block));

    const AArch64FramePlan plan = PlanAArch64Frame(function, {}, {}, {}, Target::OS::Windows);

    CHECK_EQ(plan.SavedGeneralRegisters(), std::vector<unsigned>({19, 20, 21, 22, 23, 24, 25, 26, 27, 28}));
    CHECK_EQ(plan.SavedVectorRegisters(), std::vector<unsigned>({8, 9, 10, 11, 12, 13, 14, 15}));
    CHECK_EQ(plan.GeneralRegisterHomes().size(), 10);
    CHECK_EQ(plan.VectorRegisterHomes().size(), 8);
    CHECK_FALSE(std::ranges::any_of(plan.GeneralRegisterHomes(), [](const auto &home) { return home.second == 18; }));
    CHECK_EQ(plan.CalleeSaveOffset(), 16);
    CHECK_EQ(plan.FrameSize() % 16, 0);
}

TEST_CASE("AArch64 frame plan keeps HFA results in registers on every OS profile") {
    const TypeRef quad = TypeRef::MakeNamed("Quad");
    const std::vector<LirStructDecl> declarations = {LirStructDecl{.name = "Quad",
                                                                   .isPublic = false,
                                                                   .typeParams = {},
                                                                   .fields = {{"a", TypeRef::MakeFloat64()},
                                                                              {"b", TypeRef::MakeFloat64()},
                                                                              {"c", TypeRef::MakeFloat64()},
                                                                              {"d", TypeRef::MakeFloat64()}}}};
    LayoutMap layouts;
    layouts["Quad"] = StructLayout{.fields = {}, .totalSize = 32, .alignment = 8};

    LirFunc function;
    function.name = "Identity";
    function.returnType = quad;
    function.params.push_back({0, quad, "value"});
    LirBlock block;
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 0;
    block.term->retType = quad;
    function.blocks.push_back(std::move(block));

    constexpr std::array profiles = {Target::OS::Linux, Target::OS::FreeBSD, Target::OS::MacOS, Target::OS::Windows};
    for (const Target::OS profile : profiles) {
        CAPTURE(profile);
        const AArch64FramePlan plan = PlanAArch64Frame(function, layouts, {}, declarations, profile);
        CHECK_EQ(plan.TargetOs(), profile);
        CHECK_EQ(plan.IndirectResultOffset(), 0);
        CHECK_EQ(plan.SlotOffsets().at(0), 16);
        CHECK_EQ(plan.FrameSize(), 48);
    }
}
