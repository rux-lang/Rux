// AArch64 machine selection, encoding and emission (`CodeGen/AArch64/Machine*.h`).

#include "CodeGen/AArch64/MachineEmission.h"
#include "CodeGen/AArch64/MachineEncoder.h"
#include "CodeGen/AArch64/MachineSelection.h"
#include "Object/Rcu/Rcu.h"

#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::AArch64Machine;

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
    entry.instrs.push_back(Instruction(LirOpcode::Const, 1, word, {}, "0"));
    LirTerminator toLoop;
    toLoop.kind = LirTermKind::Jump;
    toLoop.trueTarget = 1;
    entry.term = toLoop;

    LirBlock loop;
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
    body.instrs.push_back(Instruction(LirOpcode::Add, 6, word, {3, 2}));
    body.instrs.push_back(Instruction(LirOpcode::Const, 7, word, {}, "1"));
    body.instrs.push_back(Instruction(LirOpcode::Add, 5, word, {2, 7}));
    LirTerminator back;
    back.kind = LirTermKind::Jump;
    back.trueTarget = 1;
    body.term = back;

    LirBlock exit;
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = 3;
    ret.retType = word;
    exit.term = ret;

    function.blocks = {entry, loop, body, exit};
    return function;
}

[[nodiscard]] std::vector<std::uint32_t> Words(const std::vector<std::uint8_t> &bytes) {
    std::vector<std::uint32_t> words;
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
        words.push_back(static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
                        static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
                        static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return words;
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

/// The bytes the machine encoder gives one instruction, with frame object 0 at offset 24 from SP.
[[nodiscard]] std::vector<std::uint8_t> Encode(const Machine::Instruction &instruction,
                                               std::vector<SymbolUse> *uses = nullptr) {
    std::vector<std::uint8_t> bytes;
    std::vector<SymbolUse> symbolUses;
    const std::vector<std::int32_t> frame{24};
    const SelectedFunction function;
    MachineEncoder encoder(bytes, symbolUses, frame, function, Target::OS::Linux);
    encoder.Encode(instruction);
    CHECK_FALSE(encoder.Failed());
    if (uses != nullptr)
        *uses = symbolUses;
    return bytes;
}

/// The bytes the AArch64 encoder gives the instruction the machine encoder should have chosen.
template <typename Spell>
[[nodiscard]] std::vector<std::uint8_t> Expected(const Spell &spell) {
    std::vector<std::uint8_t> bytes;
    const A64Enc enc(bytes);
    CHECK(spell(enc) == A64Status::Ok);
    return bytes;
}
} // namespace

TEST_CASE("AArch64 machine selection keeps a counted loop in registers") {
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const AArch64CallPlanner planner(layouts, interfaces, {}, Target::OS::Linux);
    const SelectionContext context{layouts, interfaces, planner, Target::OS::Linux, false};
    const LirFunc function = SumTo();
    auto selected = SelectFunction(function, context);
    REQUIRE(selected.has_value());
    const auto emitted = EmitFunction(*selected, Target::OS::Linux);
    REQUIRE(emitted.has_value());
    const auto words = Words(emitted->bytes);
    REQUIRE(words.size() > 6);
    // sub sp, sp, #16; stp x29, x30, [sp]; add x29, sp, #0 — and the mirror image before the final ret.
    CHECK(words[0] == 0xD10043FFU);
    CHECK(words[1] == 0xA9007BFDU);
    CHECK(words[2] == 0x910003FDU);
    CHECK(words.back() == 0xD65F03C0U);
    // Between them nothing reaches memory: the loop adds and compares in registers.
    std::size_t memory = 0;
    std::size_t additions = 0;
    for (std::size_t index = 3; index + 3 < words.size(); ++index) {
        const std::uint32_t word = words[index];
        memory += (word & 0x0A000000U) == 0x08000000U;
        additions += (word & 0xFF200000U) == 0x8B000000U || (word & 0xFF000000U) == 0x91000000U;
    }
    CHECK(memory == 0);
    CHECK(additions >= 2);
}

TEST_CASE("AArch64 machine selection declines wide integers and untested systems") {
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaces;
    const AArch64CallPlanner planner(layouts, interfaces, {}, Target::OS::Linux);
    const SelectionContext onLinux{layouts, interfaces, planner, Target::OS::Linux, false};
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
    CHECK_FALSE(SelectFunction(function, onLinux).has_value());

    const AArch64CallPlanner windowsPlanner(layouts, interfaces, {}, Target::OS::Windows);
    const SelectionContext windows{layouts, interfaces, windowsPlanner, Target::OS::Windows, false};
    CHECK_FALSE(SelectFunction(SumTo(), windows).has_value());
}

TEST_CASE("AArch64 machine encoding picks the short form each operand allows") {
    using Machine::Operand;
    // An encodable immediate is the instruction's own; a negative comparison is CMN.
    CHECK(Encode(Make(Op::Add, 8, {Operand::Def(0), Operand::Use(1), Operand::Imm(16)})) ==
          Expected([](const A64Enc &enc) { return enc.AddImm(A64::Xn(0), A64::Xn(1), 16); }));
    CHECK(Encode(Make(Op::Cmp, 4, {Operand::Use(2), Operand::Imm(-5)})) ==
          Expected([](const A64Enc &enc) { return enc.AddsImm(A64::Wzr, A64::Wn(2), 5); }));
    CHECK(Encode(Make(Op::CSet, 4, {Operand::Def(3)})) ==
          Expected([](const A64Enc &enc) { return enc.Cset(A64::Wn(3), A64Condition::Eq); }));
    // An index scaled by the access width is the register-offset form; a signed narrow load extends to 64 bits.
    Machine::Address indexed;
    indexed.base = 5;
    indexed.index = 6;
    indexed.scale = 2;
    CHECK(Encode(Make(Op::LoadSigned, 8, {Operand::Def(4), Operand::Mem(indexed)}, 2)) ==
          Expected([](const A64Enc &enc) {
              return enc.LdrshReg(A64::Xn(4), A64::Xn(5), A64::Xn(6), A64ExtendKind::Uxtx, 1);
          }));
    // A frame object is an offset from SP, and storing zero names the zero register.
    Machine::Address object;
    object.frameObject = 0;
    object.displacement = 8;
    CHECK(Encode(Make(Op::Store, 4, {Operand::Mem(object), Operand::Imm(0)})) ==
          Expected([](const A64Enc &enc) { return enc.Str(A64::Wzr, A64::Sp, 32); }));
    // A symbol is a page and an offset within it, one relocation on each.
    Machine::Address symbol;
    symbol.symbol = 3;
    std::vector<SymbolUse> uses;
    const auto bytes = Encode(Make(Op::AddressOf, 8, {Operand::Def(7), Operand::Mem(symbol)}), &uses);
    CHECK(bytes.size() == 8);
    REQUIRE(uses.size() == 2);
    CHECK(uses[0].type == RcuRelType::AArch64AdrPrelPgHi21);
    CHECK(uses[1].type == RcuRelType::AArch64AddAbsLo12Nc);
    CHECK(uses[1].offset == 4);
    CHECK(uses[0].symbol == 3);
    // A call is BL with its target left to the linker.
    const auto call = Encode(Make(Op::Call, 8, {Operand::SymbolTarget(2)}), &uses);
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].type == RcuRelType::AArch64Call26);
    CHECK(call == Expected([](const A64Enc &enc) { return enc.Bl(0); }));
}
