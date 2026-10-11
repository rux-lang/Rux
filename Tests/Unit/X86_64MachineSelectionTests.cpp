// x86-64 machine selection, encoding and emission (`CodeGen/X86_64/Machine*.h`).

#include "CodeGen/X86_64/MachineEmission.h"
#include "CodeGen/X86_64/MachineEncoder.h"
#include "CodeGen/X86_64/MachineSelection.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::X86_64Machine;

namespace {
[[nodiscard]] LirInstr Instruction(const LirOpcode op, const LirReg dst, const TypeRef &type,
                                   std::vector<LirReg> srcs = {}, std::string text = {}) {
    LirInstr instruction;
    instruction.op = op;
    instruction.dst = dst;
    instruction.type = type;
    instruction.srcs = std::move(srcs);
    instruction.strArg = std::move(text);
    return instruction;
}

/// sum = 0; for i in 0..n { sum += i } return sum, as SSA with phis.
[[nodiscard]] LirFunc SumTo() {
    const TypeRef word = TypeRef::MakeUInt64();
    LirFunc function;
    function.name = "SumTo";
    function.returnType = word;
    function.params.push_back({0, word, "n"});

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(Instruction(LirOpcode::Const, 1, word, {}, "0"));
    LirTerminator toLoop;
    toLoop.kind = LirTermKind::Jump;
    toLoop.trueTarget = 1;
    entry.term = toLoop;

    LirBlock loop;
    loop.label = "loop";
    LirInstr index = Instruction(LirOpcode::Phi, 2, word);
    index.phiPreds = {{1, 0}, {5, 2}};
    LirInstr sum = Instruction(LirOpcode::Phi, 3, word);
    sum.phiPreds = {{1, 0}, {6, 2}};
    loop.instrs.push_back(index);
    loop.instrs.push_back(sum);
    loop.instrs.push_back(Instruction(LirOpcode::CmpLt, 4, TypeRef::MakeBool(), {2, 0}));
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = 4;
    branch.trueTarget = 2;
    branch.falseTarget = 3;
    loop.term = branch;

    LirBlock body;
    body.label = "body";
    body.instrs.push_back(Instruction(LirOpcode::Add, 6, word, {3, 2}));
    body.instrs.push_back(Instruction(LirOpcode::Const, 7, word, {}, "1"));
    body.instrs.push_back(Instruction(LirOpcode::Add, 5, word, {2, 7}));
    LirTerminator back;
    back.kind = LirTermKind::Jump;
    back.trueTarget = 1;
    body.term = back;

    LirBlock exit;
    exit.label = "exit";
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = 3;
    ret.retType = word;
    exit.term = ret;

    function.blocks = {entry, loop, body, exit};
    return function;
}

[[nodiscard]] std::vector<std::uint8_t> Encode(const Machine::Instruction &instruction,
                                               std::vector<SymbolUse> *uses = nullptr) {
    std::vector<std::uint8_t> bytes;
    std::vector<SymbolUse> symbolUses;
    const std::vector<std::int32_t> frame;
    const SelectedFunction function;
    MachineEncoder encoder(bytes, symbolUses, frame, function, Target::OS::Windows);
    encoder.Encode(instruction);
    if (uses != nullptr)
        *uses = symbolUses;
    return bytes;
}

[[nodiscard]] Machine::Instruction Make(const Op op, const std::uint8_t size, std::vector<Machine::Operand> operands,
                                        const std::uint8_t sourceSize = 0) {
    Machine::Instruction instruction;
    instruction.opcode = static_cast<std::uint16_t>(op);
    instruction.size = size;
    instruction.sourceSize = sourceSize;
    instruction.operands = std::move(operands);
    return instruction;
}
} // namespace

TEST_CASE("machine selection keeps a counted loop in registers") {
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const SelectionContext context{layouts, interfaces, Target::OS::Windows, false};
    const LirFunc function = SumTo();
    auto selected = SelectFunction(function, context);
    REQUIRE(selected.has_value());
    const auto emitted = EmitFunction(*selected, Target::OS::Windows, true);
    REQUIRE(emitted.has_value());
    REQUIRE(emitted->bytes.size() > 4);
    // push rbp; mov rbp, rsp
    CHECK(emitted->bytes[0] == 0x55);
    CHECK(emitted->bytes[1] == 0x48);
    CHECK(emitted->bytes[2] == 0x89);
    CHECK(emitted->bytes[3] == 0xE5);
    std::string text;
    for (const auto &line : emitted->assembly)
        text += line + "\n";
    CAPTURE(text);
    // Nothing lives in the frame, the comparison feeds the branch directly, and the loop adds in registers.
    CHECK(text.find("[rbp") == std::string::npos);
    CHECK(text.find("set") == std::string::npos);
    CHECK(text.find("cmp") != std::string::npos);
    CHECK(text.find("add") != std::string::npos);
}

TEST_CASE("machine selection declines wide integers") {
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const SelectionContext context{layouts, interfaces, Target::OS::Linux, false};
    LirFunc function;
    function.name = "Wide";
    function.returnType = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    function.params.push_back({0, TypeRef::MakePrimitive(TypeRef::Kind::Int128), "value"});
    LirBlock block;
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = 0;
    ret.retType = function.returnType;
    block.term = ret;
    function.blocks.push_back(block);
    CHECK_FALSE(SelectFunction(function, context).has_value());
}

TEST_CASE("machine encoding spells general operand forms") {
    using Machine::Address;
    using Machine::Operand;
    Address indexed;
    indexed.base = R12;
    indexed.index = R13;
    indexed.scale = 8;
    indexed.displacement = 16;
    CHECK(Encode(Make(Op::Load, 8, {Operand::Def(Rax), Operand::Mem(indexed)}, 8)) ==
          std::vector<std::uint8_t>{0x4B, 0x8B, 0x44, 0xEC, 0x10});

    Address frame;
    frame.base = Rbp;
    frame.displacement = -8;
    CHECK(Encode(Make(Op::Store, 1, {Operand::Mem(frame), Operand::Use(Rsi)})) ==
          std::vector<std::uint8_t>{0x40, 0x88, 0x75, 0xF8});

    CHECK(Encode(Make(Op::MovZx, 4, {Operand::Def(Rax), Operand::Use(Rcx)}, 1)) ==
          std::vector<std::uint8_t>{0x0F, 0xB6, 0xC1});
    CHECK(Encode(Make(Op::MovZx, 4, {Operand::Def(R8), Operand::Use(Rsi)}, 1)) ==
          std::vector<std::uint8_t>{0x44, 0x0F, 0xB6, 0xC6});

    Address stack;
    stack.base = Rsp;
    stack.displacement = 32;
    CHECK(Encode(Make(Op::Store, 8, {Operand::Mem(stack), Operand::Imm(0)})) ==
          std::vector<std::uint8_t>{0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00});

    CHECK(Encode(Make(Op::Add, 8, {Operand::UseDef(R9), Operand::Imm(1)})) ==
          std::vector<std::uint8_t>{0x49, 0x83, 0xC1, 0x01});
    CHECK(Encode(Make(Op::Cmp, 4, {Operand::Use(Rdx), Operand::Use(R11)})) ==
          std::vector<std::uint8_t>{0x41, 0x3B, 0xD3});

    // A byte swap names its register in the opcode byte; REX.W widens it and REX.B reaches r8-r15.
    CHECK(Encode(Make(Op::Bswap, 8, {Operand::UseDef(Rax)})) == std::vector<std::uint8_t>{0x48, 0x0F, 0xC8});
    CHECK(Encode(Make(Op::Bswap, 8, {Operand::UseDef(R9)})) == std::vector<std::uint8_t>{0x49, 0x0F, 0xC9});
    CHECK(Encode(Make(Op::Bswap, 4, {Operand::UseDef(Rdx)})) == std::vector<std::uint8_t>{0x0F, 0xCA});
    CHECK(Encode(Make(Op::Bswap, 4, {Operand::UseDef(R10)})) == std::vector<std::uint8_t>{0x41, 0x0F, 0xCA});
    // The widening multiply's operand is its only explicit one: F7 /4.
    CHECK(Encode(Make(Op::MulHigh, 8, {Operand::Use(Rcx)})) == std::vector<std::uint8_t>{0x48, 0xF7, 0xE1});
    CHECK(Encode(Make(Op::MulHigh, 8, {Operand::Use(R11)})) == std::vector<std::uint8_t>{0x49, 0xF7, 0xE3});
    // Its signed form is F7 /5.
    CHECK(Encode(Make(Op::MulHighSigned, 8, {Operand::Use(Rcx)})) == std::vector<std::uint8_t>{0x48, 0xF7, 0xE9});

    // A RIP-relative operand records its symbol with the displacement folded into the addend, less any immediate
    // that follows the field.
    Address constant;
    constant.symbol = 0;
    constant.displacement = 8;
    std::vector<SymbolUse> uses;
    CHECK(Encode(Make(Op::Store, 4, {Operand::Mem(constant), Operand::Imm(7)}), &uses) ==
          std::vector<std::uint8_t>{0xC7, 0x05, 0, 0, 0, 0, 0x07, 0, 0, 0});
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].offset == 2);
    CHECK(uses[0].addend == 8 - 4);
}

TEST_CASE("machine selection folds a constant index offset into the displacement") {
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const SelectionContext context{layouts, interfaces, Target::OS::Linux, false};
    const TypeRef word = TypeRef::MakeUInt64();
    const TypeRef byte = TypeRef::MakePrimitive(TypeRef::Kind::UInt8);
    const TypeRef pointer = TypeRef::MakePointer(byte);
    LirFunc function;
    function.name = "Next";
    function.returnType = byte;
    function.params.push_back({0, pointer, "bytes"});
    function.params.push_back({1, word, "index"});
    LirBlock block;
    block.label = "entry";
    block.instrs.push_back(Instruction(LirOpcode::Const, 2, word, {}, "3"));
    block.instrs.push_back(Instruction(LirOpcode::Add, 3, word, {1, 2}));
    block.instrs.push_back(Instruction(LirOpcode::IndexPtr, 4, pointer, {0, 3}));
    block.instrs.push_back(Instruction(LirOpcode::Load, 5, byte, {4}));
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = 5;
    ret.retType = byte;
    block.term = ret;
    function.blocks.push_back(block);
    auto selected = SelectFunction(function, context);
    REQUIRE(selected.has_value());
    const auto emitted = EmitFunction(*selected, Target::OS::Linux, true);
    REQUIRE(emitted.has_value());
    std::string text;
    for (const auto &line : emitted->assembly)
        text += line + "\n";
    CAPTURE(text);
    // `bytes[index + 3]` loads from [bytes + index + 3] without computing the sum.
    CHECK(text.find("lea") == std::string::npos);
    CHECK(text.find("add") == std::string::npos);
    CHECK(text.find("+ 3]") != std::string::npos);
}

TEST_CASE("machine emission branches on a flag's comparison instead of testing the flag") {
    // while same && k < n { same = p[k] == 0; k += 1 } return same: after rotation the latch would materialize `same`
    // and test it; it branches on the comparison's flags instead.
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const SelectionContext context{layouts, interfaces, Target::OS::Linux, false};
    const TypeRef word = TypeRef::MakeUInt64();
    const TypeRef byte = TypeRef::MakePrimitive(TypeRef::Kind::UInt8);
    const TypeRef flag = TypeRef::MakeBool();
    LirFunc function;
    function.name = "Zeros";
    function.returnType = flag;
    function.params.push_back({0, TypeRef::MakePointer(byte), "bytes"});
    function.params.push_back({1, word, "count"});
    const auto jump = [](const std::uint32_t target) {
        LirTerminator term;
        term.kind = LirTermKind::Jump;
        term.trueTarget = target;
        return term;
    };
    const auto branch = [](const LirReg condition, const std::uint32_t yes, const std::uint32_t no) {
        LirTerminator term;
        term.kind = LirTermKind::Branch;
        term.cond = condition;
        term.trueTarget = yes;
        term.falseTarget = no;
        return term;
    };
    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(Instruction(LirOpcode::Const, 2, word, {}, "0"));
    entry.instrs.push_back(Instruction(LirOpcode::Const, 3, flag, {}, "true"));
    entry.term = jump(1);
    LirBlock header;
    header.label = "header";
    LirInstr index = Instruction(LirOpcode::Phi, 4, word);
    index.phiPreds = {{2, 0}, {10, 3}};
    LirInstr same = Instruction(LirOpcode::Phi, 5, flag);
    same.phiPreds = {{3, 0}, {8, 3}};
    header.instrs = {index, same};
    header.term = branch(5, 2, 4);
    LirBlock bounded;
    bounded.label = "bounded";
    bounded.instrs.push_back(Instruction(LirOpcode::CmpLt, 6, flag, {4, 1}));
    bounded.term = branch(6, 3, 4);
    LirBlock body;
    body.label = "body";
    body.instrs.push_back(Instruction(LirOpcode::IndexPtr, 11, TypeRef::MakePointer(byte), {0, 4}));
    body.instrs.push_back(Instruction(LirOpcode::Load, 7, byte, {11}));
    body.instrs.push_back(Instruction(LirOpcode::Const, 12, byte, {}, "0"));
    body.instrs.push_back(Instruction(LirOpcode::CmpEq, 8, flag, {7, 12}));
    body.instrs.push_back(Instruction(LirOpcode::Const, 9, word, {}, "1"));
    body.instrs.push_back(Instruction(LirOpcode::Add, 10, word, {4, 9}));
    body.term = jump(1);
    LirBlock exit;
    exit.label = "exit";
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = 5;
    ret.retType = flag;
    exit.term = ret;
    function.blocks = {entry, header, bounded, body, exit};
    auto selected = SelectFunction(function, context);
    REQUIRE(selected.has_value());
    const auto emitted = EmitFunction(*selected, Target::OS::Linux, true);
    REQUIRE(emitted.has_value());
    std::string text;
    for (const auto &line : emitted->assembly)
        text += line + "\n";
    CAPTURE(text);
    // The latch's comparison feeds its branch: nothing between the flag's `sete` and the next jump tests it, and the
    // count's step that sits between them no longer writes the flags.
    const auto set = text.find("sete");
    REQUIRE(set != std::string::npos);
    const auto next = text.find("\n    j", set);
    REQUIRE(next != std::string::npos);
    const auto between = text.substr(set, next - set);
    CHECK(between.find("test") == std::string::npos);
    CHECK(between.find("lea") != std::string::npos);
}
