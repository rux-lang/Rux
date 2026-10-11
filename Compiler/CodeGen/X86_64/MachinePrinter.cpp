#include "CodeGen/X86_64/MachinePrinter.h"

#include "CodeGen/X86_64/Registers.h"

#include <format>

namespace Rux::X86_64Machine {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::NoSymbol;
using Machine::Operand;

namespace {
[[nodiscard]] std::string RegisterName(const Reg reg, const int size) {
    if (IsXmm(reg))
        return std::format("xmm{}", reg - Xmm0);
    if (Machine::IsVirtual(reg))
        return std::format("v{}", reg - Machine::PhysicalRegisterCount);
    return std::string(X86SizedRegisterName(static_cast<int>(reg), size));
}

[[nodiscard]] std::string_view SizeName(const int size) {
    switch (size) {
    case 1:
        return "byte";
    case 2:
        return "word";
    case 4:
        return "dword";
    case 16:
        return "oword";
    default:
        return "qword";
    }
}

[[nodiscard]] std::string_view ConditionName(const Cond condition) {
    static constexpr std::string_view names[] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                                 "s", "ns", "p", "np", "l", "ge", "le", "g"};
    return names[static_cast<std::size_t>(condition) & 15U];
}

[[nodiscard]] std::string SymbolName(const SelectedFunction &selected, const std::uint32_t symbol) {
    if (symbol >= selected.symbols.size())
        return "?";
    const auto &reference = selected.symbols[symbol];
    switch (reference.kind) {
    case SymbolRef::Kind::String:
        return std::format("str({})", reference.text.size());
    case SymbolRef::Kind::Float32:
        return std::format("f32({})", reference.text);
    case SymbolRef::Kind::Float64:
        return std::format("f64({})", reference.text);
    case SymbolRef::Kind::Float32SignMask:
        return "__f32_sign_mask";
    case SymbolRef::Kind::Float64SignMask:
        return "__f64_sign_mask";
    default:
        return reference.text;
    }
}

[[nodiscard]] std::string MemoryText(const SelectedFunction &selected, const Address &address, const int size,
                                     const std::vector<std::int32_t> &objectOffsets, const bool sized = true) {
    std::string inner;
    if (address.symbol != NoSymbol) {
        inner = std::format("rel {}", SymbolName(selected, address.symbol));
        if (address.displacement != 0)
            inner += std::format(" + {}", address.displacement);
    }
    else {
        std::int64_t displacement = address.displacement;
        if (address.frameObject >= 0) {
            inner = "rbp";
            if (static_cast<std::size_t>(address.frameObject) < objectOffsets.size())
                displacement -= objectOffsets[static_cast<std::size_t>(address.frameObject)];
            else
                inner = std::format("frame{}", address.frameObject);
        }
        else if (address.base != NoReg) {
            inner = RegisterName(address.base, 8);
        }
        if (address.index != NoReg) {
            inner += std::format(" + {}", RegisterName(address.index, 8));
            if (address.scale != 1)
                inner += std::format("*{}", address.scale);
        }
        if (displacement > 0)
            inner += std::format(" + {}", displacement);
        else if (displacement < 0)
            inner += std::format(" - {}", -displacement);
    }
    return sized ? std::format("{} [{}]", SizeName(size), inner) : std::format("[{}]", inner);
}

[[nodiscard]] std::string OperandText(const SelectedFunction &selected, const Operand &operand, const int size,
                                      const std::vector<std::int32_t> &objectOffsets) {
    switch (operand.kind) {
    case Operand::Kind::Register:
        return RegisterName(operand.reg, size);
    case Operand::Kind::Immediate:
        return std::to_string(operand.immediate);
    case Operand::Kind::Memory:
        return MemoryText(selected, operand.address, size, objectOffsets);
    case Operand::Kind::Block:
        return std::format(".{}", selected.blockLabels.at(operand.target));
    case Operand::Kind::Symbol:
        return SymbolName(selected, operand.target);
    case Operand::Kind::None:
        break;
    }
    return {};
}

[[nodiscard]] std::string_view Mnemonic(const Op op, const int size) {
    const bool single = size == 4;
    switch (op) {
    case Op::Mov:
    case Op::MovImm:
    case Op::Store:
        return "mov";
    case Op::Load:
        return "movzx";
    case Op::LoadSigned:
        return "movsx";
    case Op::Lea:
        return "lea";
    case Op::Add:
        return "add";
    case Op::Sub:
        return "sub";
    case Op::And:
        return "and";
    case Op::Or:
        return "or";
    case Op::Xor:
        return "xor";
    case Op::Imul:
    case Op::ImulImm:
        return "imul";
    case Op::Cmp:
        return "cmp";
    case Op::Test:
        return "test";
    case Op::Neg:
        return "neg";
    case Op::Not:
        return "not";
    case Op::Shl:
        return "shl";
    case Op::Shr:
        return "shr";
    case Op::Sar:
        return "sar";
    case Op::Ror:
        return "ror";
    case Op::MovZx:
        return "movzx";
    case Op::MovSx:
        return "movsx";
    case Op::SignExtendAccumulator:
        return size == 8 ? "cqo" : "cdq";
    case Op::Div:
        return "div";
    case Op::Idiv:
        return "idiv";
    case Op::Call:
    case Op::CallIndirect:
        return "call";
    case Op::Ud2:
        return "ud2";
    case Op::MovFp:
        return "movaps";
    case Op::LoadFp:
    case Op::StoreFp:
        return single ? "movss" : "movsd";
    case Op::AddFp:
        return single ? "addss" : "addsd";
    case Op::SubFp:
        return single ? "subss" : "subsd";
    case Op::MulFp:
        return single ? "mulss" : "mulsd";
    case Op::DivFp:
        return single ? "divss" : "divsd";
    case Op::SqrtFp:
        return single ? "sqrtss" : "sqrtsd";
    case Op::CompareFp:
        return single ? "ucomiss" : "ucomisd";
    case Op::XorFp:
        return "xorps";
    case Op::IntToFp:
        return single ? "cvtsi2ss" : "cvtsi2sd";
    case Op::FpToFp:
        return single ? "cvtsd2ss" : "cvtss2sd";
    case Op::GprToXmm:
    case Op::XmmToGpr:
        return size == 8 ? "movq" : "movd";
    case Op::RepMovsb:
        return "rep movsb";
    default:
        return "?";
    }
}
} // namespace

std::string PrintInstruction(const SelectedFunction &selected, const Instruction &instruction,
                             const std::vector<std::int32_t> &objectOffsets) {
    const auto op = static_cast<Op>(instruction.opcode);
    const auto &operands = instruction.operands;
    const int size = instruction.size;
    const auto operand = [&](const std::size_t index, const int width) {
        return OperandText(selected, operands.at(index), width, objectOffsets);
    };
    switch (op) {
    case Op::Jmp:
        return std::format("jmp     {}", operand(0, 8));
    case Op::Jcc:
        return std::format("j{:<6} {}", ConditionName(static_cast<Cond>(instruction.condition)), operand(0, 8));
    case Op::SetCc:
        return std::format("set{:<4} {}", ConditionName(static_cast<Cond>(instruction.condition)), operand(0, 1));
    case Op::Ret:
        return "ret";
    case Op::Load:
    case Op::LoadSigned: {
        const int source = instruction.sourceSize;
        const bool plain = source == 8 || (source == 4 && op == Op::Load);
        const std::string_view mnemonic = plain ? "mov" : (source == 4 ? "movsxd" : Mnemonic(op, size));
        return std::format("{:<7} {}, {}", mnemonic, operand(0, op == Op::Load && source < 8 ? 4 : 8),
                           OperandText(selected, operands[1], source, objectOffsets));
    }
    case Op::Store:
        return std::format("mov     {}, {}", operand(0, size), operand(1, size));
    case Op::Lea:
        return std::format("lea     {}, {}", operand(0, size),
                           MemoryText(selected, operands[1].address, size, objectOffsets, false));
    case Op::MovZx:
    case Op::MovSx: {
        const int source = instruction.sourceSize;
        if (source == 4 && op == Op::MovZx)
            return std::format("mov     {}, {}", operand(0, 4), operand(1, 4));
        return std::format("{:<7} {}, {}", source == 4 ? "movsxd" : Mnemonic(op, size), operand(0, size),
                           operand(1, source));
    }
    case Op::Call:
    case Op::CallIndirect:
        return std::format("call    {}", operand(0, 8));
    case Op::IntToFp:
        return std::format("{:<7} {}, {}", Mnemonic(op, size), operand(0, size), operand(1, instruction.sourceSize));
    case Op::FpToInt:
        return std::format("{:<7} {}, {}", instruction.sourceSize == 4 ? "cvttss2si" : "cvttsd2si", operand(0, size),
                           operand(1, instruction.sourceSize));
    case Op::FpToUInt64:
        return std::format("cvt2u64 {}, {}  ; via {}", operand(0, 8), operand(1, 8), operand(2, 8));
    case Op::Panic:
        return std::format("panic   {}  ; site {}", operand(0, 8), operands[1].immediate);
    case Op::LoadFp:
    case Op::StoreFp:
    case Op::AddFp:
    case Op::SubFp:
    case Op::MulFp:
    case Op::DivFp:
    case Op::SqrtFp: {
        std::string text = std::format("{:<7} {}", Mnemonic(op, size), operand(0, size));
        for (std::size_t index = 1; index < operands.size(); ++index)
            text += ", " + operand(index, size);
        return text;
    }
    default: {
        std::string text(Mnemonic(op, size));
        if (operands.empty())
            return text;
        text.resize(std::max<std::size_t>(text.size(), 7), ' ');
        for (std::size_t index = 0; index < operands.size(); ++index) {
            text += index == 0 ? " " : ", ";
            text += operand(index, size);
        }
        if ((op == Op::Shl || op == Op::Shr || op == Op::Sar || op == Op::Ror) && operands.size() == 1)
            text += ", cl";
        return text;
    }
    }
}

std::vector<std::string> PrintFunction(const SelectedFunction &selected, const std::vector<std::uint32_t> &order,
                                       const std::vector<Reg> &savedGprs,
                                       const std::vector<std::pair<Reg, std::int32_t>> &savedVectors,
                                       const std::vector<std::int32_t> &objectOffsets, const std::int32_t allocation) {
    std::vector<std::string> lines;
    const auto instruction = [&](const std::string &text) { lines.push_back("    " + text); };
    instruction("push    rbp");
    instruction("mov     rbp, rsp");
    for (const auto reg : savedGprs)
        instruction(std::format("push    {}", RegisterName(reg, 8)));
    if (allocation > 0)
        instruction(std::format("sub     rsp, {}", allocation));
    for (const auto &[reg, offset] : savedVectors)
        instruction(std::format("movaps  oword [rbp - {}], {}", offset, RegisterName(reg, 16)));
    for (const auto block : order) {
        lines.push_back(std::format(".{}:", selected.blockLabels.at(block)));
        for (const auto &item : selected.function.blocks[block].instructions) {
            if (static_cast<Op>(item.opcode) == Op::Ret) {
                for (const auto &[reg, offset] : savedVectors)
                    instruction(std::format("movaps  {}, oword [rbp - {}]", RegisterName(reg, 16), offset));
                if (allocation > 0)
                    instruction(std::format("lea     rsp, [rbp - {}]", savedGprs.size() * 8));
                for (auto reg = savedGprs.rbegin(); reg != savedGprs.rend(); ++reg)
                    instruction(std::format("pop     {}", RegisterName(*reg, 8)));
                instruction("pop     rbp");
                instruction("ret");
                continue;
            }
            instruction(PrintInstruction(selected, item, objectOffsets));
        }
    }
    return lines;
}
} // namespace Rux::X86_64Machine
