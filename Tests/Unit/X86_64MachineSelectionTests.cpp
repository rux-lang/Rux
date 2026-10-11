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
