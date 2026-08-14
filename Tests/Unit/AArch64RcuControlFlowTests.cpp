// AArch64 RCU control flow, phi lowering and register allocation.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

TEST_CASE("AArch64 RCU emitter branches on a boolean and patches both edges") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 3;
            var b: int = 4;
            if a < b {
                return 7;
            }
            return 9;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A boolean in a register is a value that is zero or is not, so the branch
    // needs no comparison of its own: CBZ takes the edge the condition failed
    // and the B beside it takes the other.
    const auto branch =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(branch != words.end(), "cbz x9, #imm");
    const auto index = static_cast<std::size_t>(branch - words.begin());
    REQUIRE_LT(index + 1, words.size());
    CHECK_EQ(HexWord(words[index - 1] & 0xFFC003FFU), HexWord(0x394003A9U)); // ldrb w9, [x29, #imm]
    CHECK_EQ(HexWord(words[index + 1] & 0xFC000000U), HexWord(0x14000000U)); // b

    // Both displacements were filled in once the blocks they name had offsets,
    // and each one lands on the value that branch's edge returns.
    const auto whenFalse = index + static_cast<std::size_t>(BranchDisplacement(words[index]));
    const auto whenTrue = index + 1 + static_cast<std::size_t>(BranchDisplacement(words[index + 1]));
    REQUIRE_LT(whenFalse, words.size());
    REQUIRE_LT(whenTrue, words.size());
    CHECK_EQ(HexWord(words[whenTrue]), HexWord(0xD28000E9U));  // mov x9, #7
    CHECK_EQ(HexWord(words[whenFalse]), HexWord(0xD2800129U)); // mov x9, #9
}

TEST_CASE("AArch64 RCU emitter closes a loop with a backward branch") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var i: int = 0;
            while i < 10 {
                i += 1;
            }
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The back edge is the one branch whose target is behind it, and there is
    // exactly one: what it names is the block that tests the condition, which is
    // the block the conditional branch leaving the loop ends.
    const auto backward = std::ranges::find_if(
        words, [](const std::uint32_t w) { return (w & 0xFC000000U) == 0x14000000U && BranchDisplacement(w) < 0; });
    REQUIRE_MESSAGE(backward != words.end(), "a backward b");
    const auto index = static_cast<std::size_t>(backward - words.begin());
    const auto target = static_cast<std::size_t>(index + BranchDisplacement(words[index]));

    const auto exiting =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(exiting != words.end(), "cbz x9, #imm");
    const auto test = static_cast<std::size_t>(exiting - words.begin());
    REQUIRE_LT(target, test);
    const std::vector<std::uint32_t> condition(words.begin() + static_cast<std::ptrdiff_t>(target),
                                               words.begin() + static_cast<std::ptrdiff_t>(test));
    CHECK_MESSAGE(std::ranges::find(condition, 0xEB0C013FU) != condition.end(), "cmp x9, x12");
    CHECK_MESSAGE(std::ranges::find(condition, 0x9A9FA7E9U) != condition.end(), "cset x9, lt");
}

TEST_CASE("AArch64 RCU emitter widens a conditional branch that cannot reach its block") {
    // A conditional branch keeps nineteen bits of instruction displacement, so
    // it reaches a megabyte of code. Nothing a program is likely to contain puts
    // a block further away than that, and this is what a program that does gets:
    // enough instructions between the branch and the block it skips to that the
    // short form has no encoding at all.
    constexpr std::uint32_t kFiller = 100000;
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "true", TypeRef::MakeBool()));
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = 0;
    branch.trueTarget = 1;
    branch.falseTarget = 2;
    entry.term = branch;

    LirBlock filler;
    filler.label = "filler";
    for (std::uint32_t i = 0; i < kFiller; ++i) {
        filler.instrs.push_back(ConstInstr(i + 2, "1", TypeRef::MakeInt64()));
    }
    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = 2;
    filler.term = jump;

    LirBlock exit;
    exit.label = "exit";
    exit.instrs.push_back(ConstInstr(1, "7", TypeRef::MakeInt64()));
    exit.term = ReturnTerm(1);

    func.blocks = {std::move(entry), std::move(filler), std::move(exit)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The branch is the inverse of the one the terminator asked for, jumping
    // over an unconditional branch that carries the target: two instructions
    // rather than one, and twenty-six bits of displacement rather than nineteen.
    const auto widened = std::ranges::find(words, 0xB5000049U); // cbnz x9, #8
    REQUIRE_MESSAGE(widened != words.end(), "cbnz x9, #8");
    const auto index = static_cast<std::size_t>(widened - words.begin());
    REQUIRE_LT(index + 2, words.size());
    CHECK_EQ(HexWord(words[index + 1] & 0xFC000000U), HexWord(0x14000000U)); // b — the false edge
    CHECK_EQ(HexWord(words[index + 2] & 0xFC000000U), HexWord(0x14000000U)); // b — the true edge
    CHECK_MESSAGE(std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; }) ==
                      words.end(),
                  "no cbz survived the widening");

    // The false edge really was out of reach — a displacement no nineteen-bit
    // field holds — and the true edge is the instruction after the pair.
    CHECK_GT(BranchDisplacement(words[index + 1]), 1 << 18);
    CHECK_EQ(BranchDisplacement(words[index + 2]), 1);
    const auto whenFalse = index + 1 + static_cast<std::size_t>(BranchDisplacement(words[index + 1]));
    REQUIRE_LT(whenFalse, words.size());
    CHECK_EQ(HexWord(words[whenFalse]), HexWord(0xD28000E9U)); // mov x9, #7
}

TEST_CASE("AArch64 RCU emitter breaks a cycle of phi copies through a frame slot") {
    // Two phis in one block naming each other: on the edge that closes the loop
    // the copies exchange two values, which no order of stores performs — the
    // second copy would read what the first one has already overwritten. The
    // shared resolver saves one of them first, and the loop is here because a
    // cycle needs an edge whose source block is the block the phis are in.
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "1", TypeRef::MakeInt64()));
    entry.instrs.push_back(ConstInstr(1, "2", TypeRef::MakeInt64()));
    entry.instrs.push_back(ConstInstr(2, "0", TypeRef::MakeInt64()));
    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = 1;
    entry.term = jump;

    LirBlock loop;
    loop.label = "loop";
    loop.instrs.push_back(PhiInstr(3, {{0, 0}, {4, 1}})); // the two that swap
    loop.instrs.push_back(PhiInstr(4, {{1, 0}, {3, 1}}));
    loop.instrs.push_back(PhiInstr(5, {{2, 0}, {6, 1}})); // and one that does not
    loop.instrs.push_back(ConstInstr(7, "1", TypeRef::MakeInt64()));
    LirInstr add;
    add.op = LirOpcode::Add;
    add.dst = 6;
    add.type = TypeRef::MakeInt64();
    add.srcs = {5, 7};
    loop.instrs.push_back(add);
    loop.instrs.push_back(ConstInstr(8, "5", TypeRef::MakeInt64()));
    LirInstr compare;
    compare.op = LirOpcode::CmpLt;
    compare.dst = 9;
    compare.type = TypeRef::MakeBool();
    compare.srcs = {6, 8};
    loop.instrs.push_back(compare);
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = 9;
    branch.trueTarget = 1;
    branch.falseTarget = 2;
    loop.term = branch;

    LirBlock exit;
    exit.label = "exit";
    exit.term = ReturnTerm(3);

    func.blocks = {std::move(entry), std::move(loop), std::move(exit)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The copies belong to the edge rather than to a block, so they sit in the
    // terminator, after a branch that the other edge takes over them.
    const auto over =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(over != words.end(), "cbz x9, #imm");
    const auto index = static_cast<std::size_t>(over - words.begin());
    REQUIRE_EQ(BranchDisplacement(words[index]), 10); // eight words of copies and two branches

    // Four load-and-store pairs where three copies were asked for: the one the
    // cycle does not touch, then the save, then the two that read it.
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
    for (std::size_t i = index + 1; i < index + 9; i += 2) {
        const auto read = SlotDisplacement(words[i], false);
        const auto write = SlotDisplacement(words[i + 1], true);
        REQUIRE_MESSAGE(read.has_value(), HexWord(words[i]));
        REQUIRE_MESSAGE(write.has_value(), HexWord(words[i + 1]));
        reads.push_back(*read);
        writes.push_back(*write);
    }

    // The save is a copy to a place nothing else in the frame occupies, past
    // every slot because it was reserved after all of them; the copy that would
    // otherwise have read a slot already overwritten reads it instead.
    const std::uint32_t temporary = writes[1];
    CHECK_EQ(reads[3], temporary);
    const std::vector<std::uint32_t> slots = {reads[0], reads[1], reads[2], writes[0], writes[2], writes[3]};
    CHECK_GT(temporary, std::ranges::max(slots));
    CHECK_EQ(reads[1], writes[2]); // saved, then overwritten by the other value
    CHECK_EQ(reads[2], writes[3]); // and that value's own slot takes the saved one
    CHECK_EQ(std::ranges::count(writes, temporary), 1);
}

TEST_CASE("AArch64 RCU emitter lowers a switch to a compare chain and traps where control cannot arrive") {
    // Neither of these reaches a source program yet — the front end lowers a
    // `match` to comparisons of its own and only emits `unreachable` after a
    // panic or a call that does not return — so the LIR is written out here
    // rather than compiled.
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "5000", TypeRef::MakeInt64()));
    LirTerminator term;
    term.kind = LirTermKind::Switch;
    term.cond = 0;
    term.defaultTarget = 3;
    term.cases = {{"1", 1}, {"5000", 2}};
    entry.term = term;

    LirBlock first;
    first.label = "first";
    first.instrs.push_back(ConstInstr(1, "10", TypeRef::MakeInt64()));
    first.term = ReturnTerm(1);

    LirBlock second;
    second.label = "second";
    second.instrs.push_back(ConstInstr(2, "20", TypeRef::MakeInt64()));
    second.term = ReturnTerm(2);

    LirBlock unreachable;
    unreachable.label = "unreachable";
    LirTerminator trap;
    trap.kind = LirTermKind::Unreachable;
    unreachable.term = trap;

    func.blocks = {std::move(entry), std::move(first), std::move(second), std::move(unreachable)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A label the arithmetic immediate reaches is one instruction; one it does
    // not is materialized first, which is the encoder's refusal being read
    // rather than a rule about labels restated here.
    const auto reachable = std::ranges::find(words, 0xF100053FU); // cmp x9, #1
    REQUIRE_MESSAGE(reachable != words.end(), "cmp x9, #1");
    const auto materialized = std::ranges::find(words, 0xD282710CU); // mov x12, #5000
    REQUIRE_MESSAGE(materialized != words.end(), "mov x12, #5000");
    const auto index = static_cast<std::size_t>(materialized - words.begin());
    REQUIRE_LT(index + 2, words.size());
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0xEB0C013FU)); // cmp x9, x12

    // Each arm is a branch on equality to its own block, and the default is the
    // fall-through the chain ends in.
    const auto firstArm = static_cast<std::size_t>(reachable - words.begin()) + 1;
    const auto secondArm = index + 2;
    CHECK_EQ(HexWord(words[firstArm] & 0xFF00001FU), HexWord(0x54000000U));  // b.eq
    CHECK_EQ(HexWord(words[secondArm] & 0xFF00001FU), HexWord(0x54000000U)); // b.eq
    const auto whenFirst = firstArm + static_cast<std::size_t>(BranchDisplacement(words[firstArm]));
    const auto whenSecond = secondArm + static_cast<std::size_t>(BranchDisplacement(words[secondArm]));
    REQUIRE_LT(whenFirst, words.size());
    REQUIRE_LT(whenSecond, words.size());
    CHECK_EQ(HexWord(words[whenFirst]), HexWord(0xD2800149U));  // mov x9, #10
    CHECK_EQ(HexWord(words[whenSecond]), HexWord(0xD2800289U)); // mov x9, #20

    // The default block traps: a program that arrives where nothing can arrive
    // stops at the instruction rather than wherever falling through led.
    CHECK_EQ(HexWord(words.back()), HexWord(0x00000000U)); // udf #0
}

// Register allocation
//
// Which values a function keeps in machine registers is the shared linear
// scan's answer; what this back end adds is the pool each register file is
// allocated from and the prologue that preserves it. Every program below is a
// function of no parameters, so the run of frame stores after `mov x29, sp` is
// exactly what the allocation took and nothing else.

TEST_CASE("AArch64 RCU emitter preserves the callee-saved registers it allocated") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 1;
            var b: int = 2;
            var c = a + b;
            var d = c * a;
            return c + d;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    CHECK(saved.vector.empty());
    REQUIRE_FALSE(saved.general.empty());
    // X19 through X28 and nothing else: X29 and X30 are the frame record the
    // prologue already wrote, X18 is the platform register no program may
    // touch, and everything below it is a caller's to clobber.
    for (const unsigned reg : saved.general) {
        CHECK_MESSAGE(reg >= 19, reg);
        CHECK_MESSAGE(reg <= 28, reg);
        // What the prologue preserved the epilogue gives back.
        CHECK_MESSAGE(RestoresRegister(words, reg, false), reg);
    }
    // The allocation starts at the bottom of the pool, so the first register a
    // function needs is always X19.
    CHECK_EQ(std::ranges::count(saved.general, 19U), 1);
}

TEST_CASE("AArch64 RCU emitter preserves the low half of every vector register it allocates") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: float64 = 1.5;
            var b: float64 = 2.5;
            var c = a + b;
            var d = c * a;
            var e = c - d;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    REQUIRE_FALSE(saved.vector.empty());
    for (const unsigned reg : saved.vector) {
        CHECK_MESSAGE(reg >= 8, reg);
        CHECK_MESSAGE(reg <= 15, reg);
        CHECK_MESSAGE(RestoresRegister(words, reg, true), reg);
    }
    // A doubleword each, which is the whole of what AAPCS64 asks a callee to
    // preserve of V8 through V15 — and the whole of what this back end puts
    // there, since a float64 is a doubleword and a float32 is half of one.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFC003E0U) == 0x3D0003A0U; }),
             0); // no str qN, [x29, #imm]
}

TEST_CASE("AArch64 RCU emitter never allocates the platform register") {
    // Fourteen values live at once, which is more than the ten X19 through X28
    // supply: what the pool does not reach stays in the frame rather than
    // reaching past the end of it.
    std::string body;
    std::string sum;
    for (int i = 0; i < 14; ++i) {
        body += std::format("            var v{}: int = {};\n", i, i);
        sum += std::format("{}v{}", i == 0 ? "" : " + ", i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        func Main() -> int {{
{}            return {};
        }}
    )",
                                                         body, sum));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    CHECK_LE(saved.general.size(), 10);
    CHECK_FALSE(std::ranges::contains(saved.general, 18U));
    for (const unsigned reg : saved.general) {
        CHECK_MESSAGE(reg >= 19, reg);
        CHECK_MESSAGE(reg <= 28, reg);
    }
}

TEST_CASE("AArch64 RCU emitter keeps every value in the frame where control branches") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 1;
            var b: int = 2;
            if a < b {
                b = a + b;
            }
            return b;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A value in a register would have to be correct at every edge, and a phi
    // lowers to copies between slots here, so a function of more than one block
    // allocates nothing at all — and preserves nothing, which is how a prologue
    // says so.
    const auto saved = SavedRegisters(words);
    CHECK(saved.general.empty());
    CHECK(saved.vector.empty());
}
