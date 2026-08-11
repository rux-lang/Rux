// The linear-scan allocation both back ends run over a function before they
// emit it: where each virtual register is live, how often it is named, and
// which of them the pool it is allocating from reaches.
//
// The LIR here is built by hand rather than compiled, because what is being
// tested is a property of a numbering rather than of a program: which
// instruction a mention falls on decides everything the pass answers, and a
// source program says nothing about that directly.

#include "CodeGen/LinearScan.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] LirInstr Define(const LirReg dst, const std::vector<LirReg> &srcs) {
    LirInstr instr;
    instr.op = LirOpcode::Add;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.srcs = srcs;
    return instr;
}

// A function of one block holding `instrs`, returning `result`.
[[nodiscard]] LirFunc FuncOf(std::vector<LirInstr> instrs, const LirReg result) {
    LirBlock block;
    block.instrs = std::move(instrs);
    LirTerminator term;
    term.kind = LirTermKind::Return;
    term.retVal = result;
    term.retType = TypeRef::MakeInt64();
    block.term = term;

    LirFunc func;
    func.name = "Main";
    func.returnType = TypeRef::MakeInt64();
    func.blocks.push_back(std::move(block));
    return func;
}

[[nodiscard]] const LiveInterval *IntervalOf(const std::vector<LiveInterval> &intervals, const LirReg reg) {
    const auto found = std::ranges::find_if(intervals, [reg](const LiveInterval &i) { return i.reg == reg; });
    return found == intervals.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<int> AssignedTo(const RegisterAssignment &assignment, const LirReg reg) {
    const auto found = assignment.physRegs.find(reg);
    return found == assignment.physRegs.end() ? std::nullopt : std::optional<int>(found->second);
}
} // namespace

TEST_CASE("Linear scan opens an interval at the first mention and closes it at the last") {
    // %0 = 0; %1 = 1; %2 = %0 + %1; %3 = %2 + %2; return %3
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {}), Define(2, {0, 1}), Define(3, {2, 2})}, 3);
    const auto intervals = ComputeLiveIntervals(func, {});

    REQUIRE_EQ(intervals.size(), 4);
    // %0 is defined by the first instruction and last read by the third, and
    // the terminator is numbered after the instructions rather than beside the
    // last of them.
    CHECK_EQ(IntervalOf(intervals, 0)->start, 0);
    CHECK_EQ(IntervalOf(intervals, 0)->end, 2);
    CHECK_EQ(IntervalOf(intervals, 1)->start, 1);
    CHECK_EQ(IntervalOf(intervals, 1)->end, 2);
    CHECK_EQ(IntervalOf(intervals, 2)->start, 2);
    CHECK_EQ(IntervalOf(intervals, 2)->end, 3);
    CHECK_EQ(IntervalOf(intervals, 3)->start, 3);
    CHECK_EQ(IntervalOf(intervals, 3)->end, 4);
}

TEST_CASE("Linear scan counts every mention of a register") {
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {0}), Define(2, {0}), Define(3, {0, 2})}, 3);
    const auto intervals = ComputeLiveIntervals(func, {});

    // %0 is named four times: its definition and three uses, two of which fall
    // on the same instruction and count separately.
    CHECK_EQ(IntervalOf(intervals, 0)->mentions, 4);
    CHECK_EQ(IntervalOf(intervals, 1)->mentions, 1);
    // %3 is defined and then returned.
    CHECK_EQ(IntervalOf(intervals, 3)->mentions, 2);
}

TEST_CASE("Linear scan opens a parameter's interval at the entry") {
    LirFunc func = FuncOf({Define(1, {0})}, 1);
    LirParam param;
    param.reg = 0;
    param.type = TypeRef::MakeInt64();
    func.params.push_back(param);

    const auto intervals = ComputeLiveIntervals(func, {});
    // A parameter is live before any instruction has run, since the prologue
    // wrote it, so its interval opens at zero however late it is first read.
    CHECK_EQ(IntervalOf(intervals, 0)->start, 0);
    CHECK_EQ(IntervalOf(intervals, 0)->end, 0);
}

TEST_CASE("Linear scan reads a parameter at the type its caller supplies") {
    LirFunc func = FuncOf({Define(1, {0})}, 1);
    LirParam param;
    param.reg = 0;
    param.type = TypeRef::MakeInt32();
    func.params.push_back(param);

    // Win64 passes a large composite as the address of the caller's copy, so
    // the register holds a pointer where the parameter's own type says
    // otherwise — which is the one thing about a function's registers this pass
    // cannot work out for itself.
    const ParamTypeMap overridden = {{0, TypeRef::MakePointer(TypeRef::MakeInt32())}};
    CHECK_EQ(IntervalOf(ComputeLiveIntervals(func, {}), 0)->type.kind, TypeRef::Kind::Int32);
    CHECK_EQ(IntervalOf(ComputeLiveIntervals(func, overridden), 0)->type.kind, TypeRef::Kind::Pointer);
}

TEST_CASE("Linear scan reports an alloca as the pointer its register holds") {
    LirInstr alloca;
    alloca.op = LirOpcode::Alloca;
    alloca.dst = 0;
    alloca.type = TypeRef::MakeInt64();
    const LirFunc func = FuncOf({alloca, Define(1, {0})}, 1);

    // An alloca's register holds the address of what it reserved rather than a
    // value of the type it names, which is what decides the register file it
    // could live in.
    CHECK_EQ(IntervalOf(ComputeLiveIntervals(func, {}), 0)->type.kind, TypeRef::Kind::Pointer);
}

TEST_CASE("Linear scan orders intervals by where they open and by register after that") {
    // Two definitions on one instruction is impossible, but a definition and
    // the first use of a register are not: %1 and %2 both open at instruction
    // one, and the order between them has to be the same on every run.
    const LirFunc func = FuncOf({Define(0, {}), Define(2, {1}), Define(3, {0, 2})}, 3);
    const auto intervals = ComputeLiveIntervals(func, {});

    for (std::size_t i = 1; i < intervals.size(); ++i) {
        const bool ordered = intervals[i - 1].start < intervals[i].start ||
                             (intervals[i - 1].start == intervals[i].start && intervals[i - 1].reg < intervals[i].reg);
        CHECK(ordered);
    }
}

TEST_CASE("Linear scan gives overlapping intervals registers of their own") {
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {}), Define(2, {0, 1})}, 2);
    const auto assignment = AllocateRegisters(ComputeLiveIntervals(func, {}), 4);

    REQUIRE_EQ(assignment.physRegs.size(), 3);
    // %0 and %1 are both live at the addition, so they cannot share; %2 opens
    // where they close, and cannot take either of them for the same reason —
    // an instruction reads its operands and writes its result.
    CHECK_NE(AssignedTo(assignment, 0), AssignedTo(assignment, 1));
    CHECK_NE(AssignedTo(assignment, 2), AssignedTo(assignment, 0));
    CHECK_NE(AssignedTo(assignment, 2), AssignedTo(assignment, 1));
}

TEST_CASE("Linear scan reuses a register once the interval holding it has ended") {
    // %0 dies at instruction one and %2 opens at instruction two, so the
    // register %0 held is free again by then.
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {0}), Define(2, {}), Define(3, {1, 2})}, 3);
    const auto assignment = AllocateRegisters(ComputeLiveIntervals(func, {}), 4);

    CHECK_EQ(AssignedTo(assignment, 2), AssignedTo(assignment, 0));
    CHECK_EQ(assignment.usedPhysRegs.size(), 3);
}

TEST_CASE("Linear scan leaves an interval no register reaches to the frame") {
    std::vector<LirInstr> instrs;
    for (LirReg reg = 0; reg < 6; ++reg) {
        instrs.push_back(Define(reg, {}));
    }
    // Every one of the six is still live here, so a pool of four can hold four
    // of them and no more.
    instrs.push_back(Define(6, {0, 1, 2, 3, 4, 5}));
    const LirFunc func = FuncOf(std::move(instrs), 6);
    const auto assignment = AllocateRegisters(ComputeLiveIntervals(func, {}), 4);

    CHECK_EQ(assignment.physRegs.size(), 4);
    CHECK_EQ(assignment.usedPhysRegs.size(), 4);
    // The four that were reached are the four that opened first, which is what
    // scanning in interval order means; nothing is evicted to make room for a
    // later one.
    for (LirReg reg = 0; reg < 4; ++reg) {
        CHECK_MESSAGE(AssignedTo(assignment, reg).has_value(), reg);
    }
    CHECK_FALSE(AssignedTo(assignment, 6).has_value());
}

TEST_CASE("Linear scan hands out the lowest register that is free") {
    // A pool is saved and restored by whatever prologue owns it, so an index
    // never handed out costs nothing: a function needing two registers takes
    // the first two rather than spreading over the pool.
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {0}), Define(2, {1})}, 2);
    const auto assignment = AllocateRegisters(ComputeLiveIntervals(func, {}), 10);

    REQUIRE_EQ(assignment.usedPhysRegs.size(), 2);
    CHECK_EQ(assignment.usedPhysRegs[0], 0);
    CHECK_EQ(assignment.usedPhysRegs[1], 1);
}

TEST_CASE("Linear scan hands out nothing when the pool is empty") {
    const LirFunc func = FuncOf({Define(0, {}), Define(1, {0})}, 1);
    const auto assignment = AllocateRegisters(ComputeLiveIntervals(func, {}), 0);

    CHECK(assignment.physRegs.empty());
    CHECK(assignment.usedPhysRegs.empty());
}
