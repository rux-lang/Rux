// Frame slot sharing: which registers and which alloca data regions of a function may occupy the same storage.
//
// The LIR is built by hand because what is tested is a property of liveness over a control-flow graph: where a value
// is written, where it is last read, and which edges carry it. A source program says none of that directly.

#include "CodeGen/FrameSlotSharing.h"

#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] LirInstr Define(const LirReg dst, const std::vector<LirReg> &srcs = {}) {
    LirInstr instr;
    instr.op = srcs.empty() ? LirOpcode::Const : LirOpcode::Add;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.srcs = srcs;
    return instr;
}

[[nodiscard]] LirInstr Phi(const LirReg dst, std::vector<std::pair<LirReg, std::uint32_t>> incoming) {
    LirInstr instr;
    instr.op = LirOpcode::Phi;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.phiPreds = std::move(incoming);
    return instr;
}

[[nodiscard]] LirInstr Alloca(const LirReg dst) {
    LirInstr instr;
    instr.op = LirOpcode::Alloca;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    return instr;
}

[[nodiscard]] LirInstr Load(const LirReg dst, const LirReg address) {
    LirInstr instr;
    instr.op = LirOpcode::Load;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.srcs = {address};
    return instr;
}

[[nodiscard]] LirInstr Store(const LirReg value, const LirReg address) {
    LirInstr instr;
    instr.op = LirOpcode::Store;
    instr.type = TypeRef::MakeInt64();
    instr.srcs = {value, address};
    return instr;
}

[[nodiscard]] LirInstr Call(const std::vector<LirReg> &arguments) {
    LirInstr instr;
    instr.op = LirOpcode::Call;
    instr.type = TypeRef::MakeOpaque();
    instr.strArg = "Keep";
    instr.srcs = arguments;
    return instr;
}

[[nodiscard]] LirInstr FieldOf(const LirReg dst, const LirReg base) {
    LirInstr instr;
    instr.op = LirOpcode::FieldPtr;
    instr.dst = dst;
    instr.type = TypeRef::MakePointer(TypeRef::MakeInt64());
    instr.srcs = {base};
    instr.strArg = "field";
    return instr;
}

[[nodiscard]] LirBlock Block(std::vector<LirInstr> instrs, const LirTerminator &term) {
    LirBlock block;
    block.instrs = std::move(instrs);
    block.term = term;
    return block;
}

[[nodiscard]] LirTerminator Return(const LirReg value) {
    LirTerminator term;
    term.kind = LirTermKind::Return;
    term.retVal = value;
    term.retType = TypeRef::MakeInt64();
    return term;
}

[[nodiscard]] LirTerminator Jump(const std::uint32_t target) {
    LirTerminator term;
    term.kind = LirTermKind::Jump;
    term.trueTarget = target;
    return term;
}

[[nodiscard]] LirTerminator Branch(const LirReg condition, const std::uint32_t whenTrue,
                                   const std::uint32_t whenFalse) {
    LirTerminator term;
    term.kind = LirTermKind::Branch;
    term.cond = condition;
    term.trueTarget = whenTrue;
    term.falseTarget = whenFalse;
    return term;
}

[[nodiscard]] LirFunc FuncOf(std::vector<LirBlock> blocks) {
    LirFunc func;
    func.name = "Main";
    func.returnType = TypeRef::MakeInt64();
    func.blocks = std::move(blocks);
    return func;
}

[[nodiscard]] FrameSlotSharing Registers(const LirFunc &func) {
    return ShareFrameSlots(func, [](const LirInstr &) { return 8; });
}

[[nodiscard]] FrameSlotSharing Data(const LirFunc &func) {
    return ShareAllocaData(func, [](const LirInstr &) { return 8; });
}

[[nodiscard]] std::optional<std::uint32_t> SlotOf(const FrameSlotSharing &sharing, const LirReg reg) {
    const auto found = sharing.slotOf.find(reg);
    return found == sharing.slotOf.end() ? std::nullopt : std::optional<std::uint32_t>(found->second);
}
} // namespace

TEST_CASE("Frame slot sharing reuses a slot once its value has been read for the last time") {
    // %0 = c; %1 = c; %2 = %0 + %1; %3 = c; %4 = %2 + %3; return %4
    const LirFunc func =
        FuncOf({Block({Define(0), Define(1), Define(2, {0, 1}), Define(3), Define(4, {2, 3})}, Return(4))});
    const FrameSlotSharing sharing = Registers(func);

    // Both operands are live at once, and a result never takes the slot of its own operand.
    CHECK_NE(SlotOf(sharing, 0), SlotOf(sharing, 1));
    CHECK_NE(SlotOf(sharing, 2), SlotOf(sharing, 0));
    CHECK_NE(SlotOf(sharing, 2), SlotOf(sharing, 1));
    // %0 and %1 are dead by the time %3 is written, so it takes one of their slots; five values fit in three.
    CHECK_NE(SlotOf(sharing, 3), SlotOf(sharing, 2));
    CHECK_EQ(sharing.slotSizes.size(), 3U);
}

TEST_CASE("Frame slot sharing keeps a parameter and a register written twice out of it") {
    LirFunc func = FuncOf({Block({Define(1), Define(2), Define(2), Define(3, {0, 1})}, Return(3))});
    func.params.push_back({0, TypeRef::MakeInt64(), "value"});
    const FrameSlotSharing sharing = Registers(func);

    CHECK_FALSE(SlotOf(sharing, 0).has_value());
    CHECK_FALSE(SlotOf(sharing, 2).has_value());
    CHECK(SlotOf(sharing, 1).has_value());
}

TEST_CASE("Frame slot sharing keeps a value alive around the loop that reads it") {
    // entry: %0 = c; jump loop
    // loop:  %1 = %0 + %0; %2 = c; %3 = %2 + %2; branch %3, loop, exit
    // exit:  %4 = c; return %4
    //
    // %0 is read for the last time, in instruction order, before %2 is written. The back edge brings that read
    // around again, so %2 must not take its slot.
    const LirFunc func =
        FuncOf({Block({Define(0)}, Jump(1)), Block({Define(1, {0, 0}), Define(2), Define(3, {2, 2})}, Branch(3, 1, 2)),
                Block({Define(4)}, Return(4))});
    const FrameSlotSharing sharing = Registers(func);

    CHECK_NE(SlotOf(sharing, 0), SlotOf(sharing, 1));
    CHECK_NE(SlotOf(sharing, 0), SlotOf(sharing, 2));
    CHECK_NE(SlotOf(sharing, 0), SlotOf(sharing, 3));
    // Past the loop nothing reads it.
    CHECK(SlotOf(sharing, 4).has_value());
    CHECK_LT(sharing.slotSizes.size(), 5U);
}

TEST_CASE("Frame slot sharing treats a phi as written on the edges into its block") {
    // entry: %0 = c; %1 = c; branch %0, left, right
    // left:  %2 = c; jump merge
    // right: %3 = c; jump merge
    // merge: %4 = phi [%2, left], [%3, right]; %5 = phi [%1, left], [%1, right]; %6 = %4 + %5; return %6
    const LirFunc func = FuncOf(
        {Block({Define(0), Define(1)}, Branch(0, 1, 2)), Block({Define(2)}, Jump(3)), Block({Define(3)}, Jump(3)),
         Block({Phi(4, {{2, 1}, {3, 2}}), Phi(5, {{1, 1}, {1, 2}}), Define(6, {4, 5})}, Return(6))});
    const FrameSlotSharing sharing = Registers(func);

    // The two phis are written together, and each is written while the values the other reads are still needed.
    CHECK_NE(SlotOf(sharing, 4), SlotOf(sharing, 5));
    CHECK_NE(SlotOf(sharing, 4), SlotOf(sharing, 1));
    CHECK_NE(SlotOf(sharing, 5), SlotOf(sharing, 2));
    CHECK_NE(SlotOf(sharing, 5), SlotOf(sharing, 3));
    // The two arms never run together, so what they compute shares.
    CHECK_EQ(SlotOf(sharing, 2), SlotOf(sharing, 3));
}

TEST_CASE("Frame slot sharing sizes a shared slot for its largest member") {
    const LirFunc func = FuncOf({Block({Define(0), Define(1, {0}), Define(2), Define(3, {2})}, Return(3))});
    const FrameSlotSharing sharing =
        ShareFrameSlots(func, [](const LirInstr &instruction) { return instruction.dst == 2 ? 64 : 8; });

    REQUIRE(SlotOf(sharing, 0).has_value());
    REQUIRE_EQ(SlotOf(sharing, 0), SlotOf(sharing, 2));
    CHECK_EQ(sharing.slotSizes.at(*SlotOf(sharing, 0)), 64);
}

TEST_CASE("Frame slot sharing overlays allocas only the function's own loads and stores reach") {
    // %0 = alloca; %1 = c; store %1, %0; %2 = load %0
    // %3 = alloca; store %2, %3; %4 = load %3; return %4
    const LirFunc func =
        FuncOf({Block({Alloca(0), Define(1), Store(1, 0), Load(2, 0), Alloca(3), Store(2, 3), Load(4, 3)}, Return(4))});
    const FrameSlotSharing sharing = Data(func);

    REQUIRE(SlotOf(sharing, 0).has_value());
    CHECK_EQ(SlotOf(sharing, 0), SlotOf(sharing, 3));
}

TEST_CASE("Frame slot sharing leaves an alloca whose address leaves the function alone") {
    // The first address is passed to a call, the second is stored as a value, and the fourth is returned. Each may be
    // kept by something outside the function's own loads and stores.
    const LirFunc func = FuncOf(
        {Block({Alloca(0), Call({0}), Alloca(1), Alloca(2), Store(1, 2), Alloca(3), Alloca(4), Define(5), Store(5, 4)},
               Return(3))});
    const FrameSlotSharing sharing = Data(func);

    CHECK_FALSE(SlotOf(sharing, 0).has_value());
    CHECK_FALSE(SlotOf(sharing, 1).has_value());
    CHECK_FALSE(SlotOf(sharing, 3).has_value());
    CHECK(SlotOf(sharing, 2).has_value());
    CHECK(SlotOf(sharing, 4).has_value());
}

TEST_CASE("Frame slot sharing keeps an alloca alive while an address taken from it is used") {
    // %0 = alloca; %1 = fieldptr %0; %2 = alloca; %3 = c; store %3, %2; %4 = load %1; return %4
    //
    // The alloca's own register is not mentioned after the field address is taken, but the load through that address
    // reads its data, so the second alloca must not overlay it.
    const LirFunc func =
        FuncOf({Block({Alloca(0), FieldOf(1, 0), Alloca(2), Define(3), Store(3, 2), Load(4, 1)}, Return(4))});
    const FrameSlotSharing sharing = Data(func);

    REQUIRE(SlotOf(sharing, 0).has_value());
    REQUIRE(SlotOf(sharing, 2).has_value());
    CHECK_NE(SlotOf(sharing, 0), SlotOf(sharing, 2));

    // Passing the field address to a call makes the alloca it came from escape.
    const LirFunc escaping = FuncOf({Block({Alloca(0), FieldOf(1, 0), Call({1}), Define(2)}, Return(2))});
    CHECK_FALSE(SlotOf(Data(escaping), 0).has_value());
}
