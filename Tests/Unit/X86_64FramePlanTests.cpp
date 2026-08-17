#include "CodeGen/X86_64/FramePlan.h"

#include <cstdint>
#include <doctest.h>
#include <unordered_set>
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

TEST_CASE("x86-64 frame plan owns hidden returns aggregate regions phi scratch and alignment") {
    const TypeRef triple = TypeRef::MakeNamed("Triple");
    LayoutMap layouts;
    layouts["Triple"] = StructLayout{.fields = {}, .totalSize = 24, .alignment = 8};

    LirFunc function;
    function.name = "Rotate";
    function.callConv = CallingConvention::SysV;
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

    const X86_64FramePlan plan = PlanX86_64Frame(function, layouts, {}, Target::OS::Linux);

    CHECK_EQ(plan.HiddenReturnOffset(), 8);
    CHECK_EQ(plan.SlotOffsets().at(0), 32);
    CHECK_EQ(plan.SlotOffsets().at(1), 40);
    CHECK_EQ(plan.AllocaDataOffsets().at(1), 64);
    CHECK_EQ(plan.RegisterTypes().at(1).kind, TypeRef::Kind::Pointer);
    CHECK_EQ(plan.PhiMoves().at(1).at(1).size(), 2);
    CHECK_EQ(plan.PhiTemporarySize(), 8);
    CHECK_EQ(plan.PhiTemporaryOffset(), 104);
    CHECK_EQ(plan.FrameSize(), 112);
    CHECK_EQ(plan.FrameSize() % 16, 0);
    CHECK_LE(plan.PhiTemporaryOffset(), plan.FrameSize());
}

TEST_CASE("x86-64 frame plan reserves an array of structs by its element layout") {
    // An alloca carrying its whole array type has to be sized through the layout map. Sizing it without one charges
    // eight bytes per struct element, so the region is short of what the array occupies and its last elements write
    // over the frame above them -- the saved frame pointer and the return address included.
    const TypeRef pair = TypeRef::MakeNamed("Pair");
    LayoutMap layouts;
    layouts["Pair"] = StructLayout{.fields = {}, .totalSize = 16, .alignment = 8};

    LirFunc function;
    function.name = "Hold";
    function.callConv = CallingConvention::SysV;

    LirInstr uncounted;
    uncounted.op = LirOpcode::Alloca;
    uncounted.dst = 0;
    uncounted.type = TypeRef::MakeArray(pair, 3);

    // The counted form names the element type and its count separately, and was always sized correctly. Both shapes
    // have to agree on the same forty-eight bytes.
    LirInstr counted;
    counted.op = LirOpcode::Alloca;
    counted.dst = 1;
    counted.type = pair;
    counted.strArg = "3";

    LirBlock block;
    block.instrs = {uncounted, counted};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    function.blocks.push_back(std::move(block));

    const X86_64FramePlan plan = PlanX86_64Frame(function, layouts, {}, Target::OS::Linux);

    const std::int32_t uncountedData = plan.AllocaDataOffsets().at(0);
    const std::int32_t countedData = plan.AllocaDataOffsets().at(1);
    const std::int32_t uncountedPointer = plan.SlotOffsets().at(0);

    CHECK_EQ(uncountedData - uncountedPointer, 48);
    CHECK_EQ(countedData - plan.SlotOffsets().at(1), 48);
    CHECK_LE(uncountedData, plan.FrameSize());
    CHECK_LE(countedData, plan.FrameSize());
}

TEST_CASE("x86-64 frame plan reserves a phi temporary as wide as the value it carries") {
    // A phi cycle spills its destination through one shared scratch region. Clamping that region to sixteen bytes
    // under-reserves any wider aggregate travelling through the cycle.
    const TypeRef triple = TypeRef::MakeNamed("Triple");
    LayoutMap layouts;
    layouts["Triple"] = StructLayout{.fields = {}, .totalSize = 24, .alignment = 8};

    LirFunc function;
    function.name = "Swap";
    function.callConv = CallingConvention::SysV;

    LirBlock entry;
    entry.instrs = {Define(0, triple), Define(1, triple)};
    entry.term = JumpTo(1);

    LirInstr firstPhi;
    firstPhi.op = LirOpcode::Phi;
    firstPhi.dst = 2;
    firstPhi.type = triple;
    firstPhi.phiPreds = {{0, 0}, {3, 1}};
    LirInstr secondPhi = firstPhi;
    secondPhi.dst = 3;
    secondPhi.phiPreds = {{1, 0}, {2, 1}};

    LirBlock loop;
    loop.instrs = {firstPhi, secondPhi};
    loop.term = JumpTo(1);
    function.blocks = {std::move(entry), std::move(loop)};

    const X86_64FramePlan plan = PlanX86_64Frame(function, layouts, {}, Target::OS::Linux);

    CHECK_EQ(plan.PhiTemporarySize(), 24);
    CHECK_LE(plan.PhiTemporaryOffset(), plan.FrameSize());
}

TEST_CASE("x86-64 frame plan records Win64 address homes and hidden return after callee saves") {
    const TypeRef slice = TypeRef::MakeNamed("Slice<int>");
    LirFunc function;
    function.name = "Identity";
    function.callConv = CallingConvention::Win64;
    function.returnType = slice;
    function.params.push_back({0, slice, "value"});

    LirBlock block;
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 0;
    block.term->retType = slice;
    function.blocks.push_back(std::move(block));

    const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);

    REQUIRE_EQ(plan.UsedPhysicalRegisters().size(), 1);
    CHECK_EQ(plan.UsedPhysicalRegisters().front(), 0);
    CHECK_EQ(plan.PhysicalRegisters().at(0), 0);
    CHECK_EQ(plan.HiddenReturnOffset(), 16);
    CHECK_EQ(plan.RegisterTypes().at(0).kind, TypeRef::Kind::Pointer);
    CHECK_EQ(plan.SlotOffsets().at(0), 24);
    CHECK_EQ(plan.FrameSize(), 32);
    CHECK_EQ(plan.PhiTemporaryOffset(), 0);
}

TEST_CASE("x86-64 frame plan caps callee saves and retains homes for spills") {
    LirFunc function;
    function.name = "Saturate";
    function.callConv = CallingConvention::SysV;
    function.returnType = TypeRef::MakeInt64();
    for (LirReg reg = 0; reg < 6; ++reg) {
        function.params.push_back({reg, TypeRef::MakeInt64(), "value"});
    }

    LirInstr combine = Define(6);
    combine.op = LirOpcode::Add;
    combine.srcs = {0, 1, 2, 3, 4, 5};
    LirBlock block;
    block.instrs.push_back(std::move(combine));
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 6;
    block.term->retType = TypeRef::MakeInt64();
    function.blocks.push_back(std::move(block));

    const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, Target::OS::Linux);

    CHECK_EQ(plan.UsedPhysicalRegisters(), std::vector<int>({0, 1, 2, 3, 4}));
    CHECK_EQ(plan.PhysicalRegisters().size(), 5);
    CHECK_FALSE(plan.PhysicalRegisters().contains(5));
    CHECK(plan.SlotOffsets().contains(5));
    CHECK(plan.SlotOffsets().contains(6));
    CHECK_EQ(plan.FrameSize() % 16, 0);
}
