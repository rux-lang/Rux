#include "CodeGen/X86_64/MachineEncoder.h"

#include <limits>

namespace Rux::X86_64Machine {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::NoSymbol;
using Machine::Operand;

namespace {
[[nodiscard]] bool FitsInt8(const std::int64_t value) noexcept {
    return value >= std::numeric_limits<std::int8_t>::min() && value <= std::numeric_limits<std::int8_t>::max();
}

[[nodiscard]] std::uint8_t ScaleBits(const std::uint8_t scale) noexcept {
    switch (scale) {
    case 2:
        return 1;
    case 4:
        return 2;
    case 8:
        return 3;
    default:
        return 0;
    }
}
} // namespace

MachineEncoder::MachineEncoder(std::vector<std::uint8_t> &bytes, std::vector<SymbolUse> &inputSymbolUses,
                               const std::vector<std::int32_t> &inputFrameOffsets,
                               const SelectedFunction &inputFunction, const Target::OS inputTargetOs)
    : out(bytes)
    , symbolUses(inputSymbolUses)
    , frameOffsets(inputFrameOffsets)
    , function(inputFunction)
    , targetOs(inputTargetOs) {
}

void MachineEncoder::Byte(const std::uint8_t value) {
    out.push_back(value);
}

void MachineEncoder::Immediate(const std::int64_t value, const int bytes) {
    for (int index = 0; index < bytes; ++index)
        Byte(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (index * 8)));
}

MachineEncoder::Rm MachineEncoder::RegisterRm(const Reg reg, const bool byteRegister) {
    Rm rm;
    rm.reg = HardwareNumber(reg);
    rm.byteRegister = byteRegister;
    return rm;
}

MachineEncoder::Rm MachineEncoder::MemoryRm(const Address &address) {
    Rm rm;
    rm.memory = true;
    rm.address = address;
    return rm;
}

void MachineEncoder::Emit(const std::uint8_t prefix, const bool rexW, const std::initializer_list<std::uint8_t> opcode,
                          const std::uint8_t regField, const bool regIsByte, const Rm &rm, const int immediateBytes,
                          const std::int64_t immediate) {
    if (prefix != 0)
        Byte(prefix);
    std::uint8_t rex = 0x40;
    if (rexW)
        rex |= 0x08;
    if ((regField & 8U) != 0)
        rex |= 0x04;
    bool force = regIsByte && regField >= 4 && regField < 8;
    if (rm.memory) {
        const Address &address = rm.address;
        if (address.symbol == NoSymbol) {
            if (address.frameObject < 0 && address.base != NoReg && (HardwareNumber(address.base) & 8U) != 0)
                rex |= 0x01;
            if (address.index != NoReg && (HardwareNumber(address.index) & 8U) != 0)
                rex |= 0x02;
        }
    }
    else {
        if ((rm.reg & 8U) != 0)
            rex |= 0x01;
        force = force || (rm.byteRegister && rm.reg >= 4 && rm.reg < 8);
    }
    if (rex != 0x40 || force)
        Byte(rex);
    for (const auto byte : opcode)
        Byte(byte);
    if (rm.memory)
        Memory(static_cast<std::uint8_t>(regField & 7U), rm.address, immediateBytes);
    else
        Byte(static_cast<std::uint8_t>(0xC0U | ((regField & 7U) << 3) | (rm.reg & 7U)));
    if (immediateBytes > 0)
        Immediate(immediate, immediateBytes);
}

void MachineEncoder::Memory(const std::uint8_t regField, const Address &address, const int immediateBytes) {
    if (address.symbol != NoSymbol) {
        Byte(static_cast<std::uint8_t>(0x05U | (regField << 3)));
        // The relocation measures from the end of its own field; any immediate after it moves the instruction end.
        symbolUses.push_back(
            {static_cast<std::uint32_t>(out.size()), address.symbol, address.displacement - immediateBytes});
        Immediate(0, 4);
        return;
    }
    std::uint8_t base = 0;
    std::int64_t displacement = address.displacement;
    if (address.frameObject >= 0) {
        base = HardwareNumber(Rbp);
        displacement -= frameOffsets.at(static_cast<std::size_t>(address.frameObject));
    }
    else {
        base = HardwareNumber(address.base);
    }
    const bool hasIndex = address.index != NoReg;
    std::uint8_t mod = 2;
    if (displacement == 0 && (base & 7U) != 5)
        mod = 0;
    else if (FitsInt8(displacement))
        mod = 1;
    if (!hasIndex && (base & 7U) != 4) {
        Byte(static_cast<std::uint8_t>((mod << 6) | (regField << 3) | (base & 7U)));
    }
    else {
        Byte(static_cast<std::uint8_t>((mod << 6) | (regField << 3) | 4U));
        const std::uint8_t index = hasIndex ? static_cast<std::uint8_t>(HardwareNumber(address.index) & 7U) : 4U;
        Byte(static_cast<std::uint8_t>((ScaleBits(address.scale) << 6) | (index << 3) | (base & 7U)));
    }
    if (mod == 1)
        Immediate(displacement, 1);
    else if (mod == 2)
        Immediate(displacement, 4);
}

void MachineEncoder::Push(const Reg reg) {
    const auto number = HardwareNumber(reg);
    if ((number & 8U) != 0)
        Byte(0x41);
    Byte(static_cast<std::uint8_t>(0x50U + (number & 7U)));
}

void MachineEncoder::Pop(const Reg reg) {
    const auto number = HardwareNumber(reg);
    if ((number & 8U) != 0)
        Byte(0x41);
    Byte(static_cast<std::uint8_t>(0x58U + (number & 7U)));
}

void MachineEncoder::MovRbpRsp() {
    Byte(0x48);
    Byte(0x89);
    Byte(0xE5);
}

void MachineEncoder::SubRsp(const std::int32_t bytes) {
    if (FitsInt8(bytes)) {
        Byte(0x48);
        Byte(0x83);
        Byte(0xEC);
        Byte(static_cast<std::uint8_t>(bytes));
        return;
    }
    Byte(0x48);
    Byte(0x81);
    Byte(0xEC);
    Immediate(bytes, 4);
}

void MachineEncoder::ProbeRsp() {
    // or qword [rsp], 0
    Byte(0x48);
    Byte(0x83);
    Byte(0x0C);
    Byte(0x24);
    Byte(0x00);
}

void MachineEncoder::LeaRspFromRbp(const std::int32_t displacement) {
    Address address;
    address.base = Rbp;
    address.displacement = displacement;
    Emit(0, true, {0x8D}, HardwareNumber(Rsp), false, MemoryRm(address));
}

void MachineEncoder::Ret() {
    Byte(0xC3);
}

void MachineEncoder::SaveVector(const Reg reg, const std::int32_t rbpDisplacement) {
    Address address;
    address.base = Rbp;
    address.displacement = rbpDisplacement;
    Emit(0, false, {0x0F, 0x29}, HardwareNumber(reg), false, MemoryRm(address));
}

void MachineEncoder::RestoreVector(const Reg reg, const std::int32_t rbpDisplacement) {
    Address address;
    address.base = Rbp;
    address.displacement = rbpDisplacement;
    Emit(0, false, {0x0F, 0x28}, HardwareNumber(reg), false, MemoryRm(address));
}

void MachineEncoder::Jmp(const std::int32_t displacement, const bool shortForm) {
    if (shortForm) {
        Byte(0xEB);
        Immediate(displacement, 1);
        return;
    }
    Byte(0xE9);
    Immediate(displacement, 4);
}

void MachineEncoder::Jcc(const Cond condition, const std::int32_t displacement, const bool shortForm) {
    if (shortForm) {
        Byte(static_cast<std::uint8_t>(0x70U | static_cast<std::uint8_t>(condition)));
        Immediate(displacement, 1);
        return;
    }
    Byte(0x0F);
    Byte(static_cast<std::uint8_t>(0x80U | static_cast<std::uint8_t>(condition)));
    Immediate(displacement, 4);
}

void MachineEncoder::CallSymbol(const std::uint32_t symbol) {
    Byte(0xE8);
    symbolUses.push_back({static_cast<std::uint32_t>(out.size()), symbol, 0});
    Immediate(0, 4);
}

void MachineEncoder::LeaRip(const Reg destination, const std::uint32_t symbol) {
    Address address;
    address.symbol = symbol;
    Emit(0, true, {0x8D}, HardwareNumber(destination), false, MemoryRm(address));
}

void MachineEncoder::EncodeMove(const Instruction &instruction) {
    const Operand &destination = instruction.operands[0];
    const auto value = instruction.operands[1].immediate;
    const auto number = HardwareNumber(destination.reg);
    const auto unsignedValue = static_cast<std::uint64_t>(value);
    if (unsignedValue <= 0xFFFF'FFFFULL) {
        if ((number & 8U) != 0)
            Byte(0x41);
        Byte(static_cast<std::uint8_t>(0xB8U + (number & 7U)));
        Immediate(value, 4);
        return;
    }
    if (value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max()) {
        Emit(0, true, {0xC7}, 0, false, RegisterRm(destination.reg), 4, value);
        return;
    }
    Byte(static_cast<std::uint8_t>(0x48U | ((number & 8U) != 0 ? 1U : 0U)));
    Byte(static_cast<std::uint8_t>(0xB8U + (number & 7U)));
    Immediate(value, 8);
}

void MachineEncoder::EncodeAlu(const Instruction &instruction, const std::uint8_t digit,
                               const std::uint8_t registerOpcode) {
    const std::uint8_t size = instruction.size;
    const std::uint8_t prefix = size == 2 ? 0x66 : 0;
    const bool wide = size == 8;
    const bool byte = size == 1;
    const Operand &left = instruction.operands[0];
    const Operand &right = instruction.operands[1];
    const Rm leftRm = RegisterRm(left.reg, byte);
    if (right.kind == Operand::Kind::Immediate) {
        if (byte) {
            Emit(prefix, false, {0x80}, digit, false, leftRm, 1, right.immediate);
        }
        else if (FitsInt8(right.immediate)) {
            Emit(prefix, wide, {0x83}, digit, false, leftRm, 1, right.immediate);
        }
        else {
            Emit(prefix, wide, {0x81}, digit, false, leftRm, size == 2 ? 2 : 4, right.immediate);
        }
        return;
    }
    const std::uint8_t opcode = byte ? static_cast<std::uint8_t>(registerOpcode - 1) : registerOpcode;
    const Rm source = right.kind == Operand::Kind::Memory ? MemoryRm(right.address) : RegisterRm(right.reg, byte);
    Emit(prefix, wide, {opcode}, HardwareNumber(left.reg), byte, source);
}

void MachineEncoder::EncodeShift(const Instruction &instruction, const std::uint8_t digit) {
    const std::uint8_t size = instruction.size;
    const std::uint8_t prefix = size == 2 ? 0x66 : 0;
    const bool wide = size == 8;
    const bool byte = size == 1;
    const Rm target = RegisterRm(instruction.operands[0].reg, byte);
    if (instruction.operands.size() > 1 && instruction.operands[1].kind == Operand::Kind::Immediate) {
        const std::int64_t count = instruction.operands[1].immediate;
        if (count == 1)
            Emit(prefix, wide, {byte ? std::uint8_t{0xD0} : std::uint8_t{0xD1}}, digit, false, target);
        else
            Emit(prefix, wide, {byte ? std::uint8_t{0xC0} : std::uint8_t{0xC1}}, digit, false, target, 1, count);
        return;
    }
    Emit(prefix, wide, {byte ? std::uint8_t{0xD2} : std::uint8_t{0xD3}}, digit, false, target);
}

void MachineEncoder::EncodeFloat(const Instruction &instruction, const std::uint8_t opcode) {
    const std::uint8_t prefix = instruction.size == 4 ? 0xF3 : 0xF2;
    const Operand &source = instruction.operands[1];
    const Rm rm = source.kind == Operand::Kind::Memory ? MemoryRm(source.address) : RegisterRm(source.reg);
    Emit(prefix, false, {0x0F, opcode}, HardwareNumber(instruction.operands[0].reg), false, rm);
}

void MachineEncoder::EncodeFpToUInt64(const Instruction &instruction) {
    const Reg destination = instruction.operands[0].reg;
    const Reg value = instruction.operands[1].reg;
    const Reg bound = instruction.operands[2].reg;
    const std::uint32_t symbol = instruction.operands[3].target;
    const bool single = instruction.sourceSize == 4;
    const std::uint8_t prefix = single ? 0xF3 : 0xF2;
    Address constant;
    constant.symbol = symbol;
    Emit(prefix, false, {0x0F, 0x10}, HardwareNumber(bound), false, MemoryRm(constant));
    Emit(single ? 0 : 0x66, false, {0x0F, 0x2E}, HardwareNumber(value), false, RegisterRm(bound));
    Byte(0x73); // jae large
    const auto large = out.size();
    Byte(0);
    Emit(prefix, true, {0x0F, 0x2C}, HardwareNumber(destination), false, RegisterRm(value));
    Byte(0xEB); // jmp done
    const auto done = out.size();
    Byte(0);
    out[large] = static_cast<std::uint8_t>(out.size() - (large + 1));
    Emit(prefix, false, {0x0F, 0x5C}, HardwareNumber(value), false, RegisterRm(bound));
    Emit(prefix, true, {0x0F, 0x2C}, HardwareNumber(destination), false, RegisterRm(value));
    Emit(0, true, {0x0F, 0xBA}, 7, false, RegisterRm(destination), 1, 63); // btc destination, 63
    out[done] = static_cast<std::uint8_t>(out.size() - (done + 1));
}

void MachineEncoder::EncodeUInt64ToFp(const Instruction &instruction) {
    const Reg destination = instruction.operands[0].reg;
    const Reg value = instruction.operands[1].reg;
    const Reg half = instruction.operands[2].reg;
    const Reg low = instruction.operands[3].reg;
    const bool single = instruction.size == 4;
    const std::uint8_t prefix = single ? 0xF3 : 0xF2;
    // test value, value; js large
    Emit(0, true, {0x85}, HardwareNumber(value), false, RegisterRm(value));
    Byte(0x78);
    const auto large = out.size();
    Byte(0);
    // A value below 2^63 converts as the signed integer it also is.
    Emit(prefix, true, {0x0F, 0x2A}, HardwareNumber(destination), false, RegisterRm(value));
    Byte(0xEB); // jmp done
    const auto done = out.size();
    Byte(0);
    out[large] = static_cast<std::uint8_t>(out.size() - (large + 1));
    // Otherwise halve it, keeping the bit shifted out as a sticky bit so the conversion still rounds correctly, then
    // double the result, which is exact. The value is read before either scratch register is written.
    Emit(0, true, {0x8B}, HardwareNumber(half), false, RegisterRm(value));
    Emit(0, true, {0x8B}, HardwareNumber(low), false, RegisterRm(value));
    Emit(0, true, {0xD1}, 5, false, RegisterRm(half));                   // shr half, 1
    Emit(0, true, {0x83}, 4, false, RegisterRm(low), 1, 1);              // and low, 1
    Emit(0, true, {0x0B}, HardwareNumber(half), false, RegisterRm(low)); // or half, low
    Emit(prefix, true, {0x0F, 0x2A}, HardwareNumber(destination), false, RegisterRm(half));
    Emit(prefix, false, {0x0F, 0x58}, HardwareNumber(destination), false, RegisterRm(destination));
    out[done] = static_cast<std::uint8_t>(out.size() - (done + 1));
}

void MachineEncoder::EncodePanic(const Instruction &instruction) {
    const Reg message = instruction.operands[0].reg;
    const PanicSite &site = function.panics.at(static_cast<std::size_t>(instruction.operands[1].immediate));
    // The failure path never returns, so it may use any register; RBX survives the calls it makes.
    if (message != Rbx)
        Emit(0, true, {0x8B}, HardwareNumber(Rbx), false, RegisterRm(message));
    const auto movImmediate32 = [&](const Reg reg, const std::int64_t value) {
        const auto number = HardwareNumber(reg);
        if ((number & 8U) != 0)
            Byte(0x41);
        Byte(static_cast<std::uint8_t>(0xB8U + (number & 7U)));
        Immediate(value, 4);
    };
    const auto loadFromRbx = [&](const Reg destination, const std::int32_t displacement) {
        Address address;
        address.base = Rbx;
        address.displacement = displacement;
        Emit(0, true, {0x8B}, HardwareNumber(destination), false, MemoryRm(address));
    };
    if (targetOs == Target::OS::Windows) {
        const auto prepare = [&] {
            movImmediate32(Rcx, 0xFFFF'FFF4LL); // STD_ERROR_HANDLE
            CallSymbol(site.getStdHandle);
            Emit(0, true, {0x8B}, HardwareNumber(Rcx), false, RegisterRm(Rax));
            Address written;
            written.base = Rsp;
            written.displacement = 40;
            Emit(0, true, {0x8D}, HardwareNumber(R9), false, MemoryRm(written));
            Address overlapped;
            overlapped.base = Rsp;
            overlapped.displacement = 32;
            Emit(0, true, {0xC7}, 0, false, MemoryRm(overlapped), 4, 0);
        };
        const auto writeStatic = [&](const std::uint32_t symbol, const std::uint32_t length) {
            prepare();
            LeaRip(Rdx, symbol);
            movImmediate32(R8, length);
            CallSymbol(site.writeFile);
        };
        writeStatic(site.prefix, site.prefixLength);
        prepare();
        loadFromRbx(Rdx, 0);
        loadFromRbx(R8, 8);
        CallSymbol(site.writeFile);
        writeStatic(site.location, site.locationLength);
    }
    else {
        const std::int64_t number = targetOs == Target::OS::Linux ? 1 : targetOs == Target::OS::MacOS ? 0x0200'0004 : 4;
        const auto syscall = [&] {
            movImmediate32(Rdi, 2);
            movImmediate32(Rax, number);
            Byte(0x0F);
            Byte(0x05);
        };
        LeaRip(Rsi, site.prefix);
        movImmediate32(Rdx, site.prefixLength);
        syscall();
        loadFromRbx(Rsi, 0);
        loadFromRbx(Rdx, 8);
        syscall();
        LeaRip(Rsi, site.location);
        movImmediate32(Rdx, site.locationLength);
        syscall();
    }
    if (site.backtrace != NoSymbol)
        CallSymbol(site.backtrace);
    Byte(0x0F);
    Byte(0x0B);
}

void MachineEncoder::Encode(const Instruction &instruction) {
    const auto op = static_cast<Op>(instruction.opcode);
    const auto &operands = instruction.operands;
    const auto reg = [&](const std::size_t index) { return operands[index].reg; };
    const auto number = [&](const std::size_t index) { return HardwareNumber(operands[index].reg); };
    switch (op) {
    case Op::Mov:
        Emit(0, instruction.size == 8, {0x8B}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::MovImm:
        EncodeMove(instruction);
        return;
    case Op::Load:
        switch (instruction.sourceSize) {
        case 1:
            Emit(0, false, {0x0F, 0xB6}, number(0), false, MemoryRm(operands[1].address));
            return;
        case 2:
            Emit(0, false, {0x0F, 0xB7}, number(0), false, MemoryRm(operands[1].address));
            return;
        case 4:
            Emit(0, false, {0x8B}, number(0), false, MemoryRm(operands[1].address));
            return;
        default:
            Emit(0, true, {0x8B}, number(0), false, MemoryRm(operands[1].address));
            return;
        }
    case Op::LoadSigned:
        switch (instruction.sourceSize) {
        case 1:
            Emit(0, true, {0x0F, 0xBE}, number(0), false, MemoryRm(operands[1].address));
            return;
        case 2:
            Emit(0, true, {0x0F, 0xBF}, number(0), false, MemoryRm(operands[1].address));
            return;
        case 4:
            Emit(0, true, {0x63}, number(0), false, MemoryRm(operands[1].address));
            return;
        default:
            Emit(0, true, {0x8B}, number(0), false, MemoryRm(operands[1].address));
            return;
        }
    case Op::Store: {
        const Rm target = MemoryRm(operands[0].address);
        const std::uint8_t size = instruction.size;
        if (operands[1].kind == Operand::Kind::Immediate) {
            const std::int64_t value = operands[1].immediate;
            if (size == 1)
                Emit(0, false, {0xC6}, 0, false, target, 1, value);
            else if (size == 2)
                Emit(0x66, false, {0xC7}, 0, false, target, 2, value);
            else
                Emit(0, size == 8, {0xC7}, 0, false, target, 4, value);
            return;
        }
        if (size == 1)
            Emit(0, false, {0x88}, number(1), true, target);
        else if (size == 2)
            Emit(0x66, false, {0x89}, number(1), false, target);
        else
            Emit(0, size == 8, {0x89}, number(1), false, target);
        return;
    }
    case Op::Lea:
        Emit(0, instruction.size == 8, {0x8D}, number(0), false, MemoryRm(operands[1].address));
        return;
    case Op::Add:
        EncodeAlu(instruction, 0, 0x03);
        return;
    case Op::Or:
        EncodeAlu(instruction, 1, 0x0B);
        return;
    case Op::And:
        EncodeAlu(instruction, 4, 0x23);
        return;
    case Op::Sub:
        EncodeAlu(instruction, 5, 0x2B);
        return;
    case Op::Xor:
        EncodeAlu(instruction, 6, 0x33);
        return;
    case Op::Cmp:
        EncodeAlu(instruction, 7, 0x3B);
        return;
    case Op::Test: {
        const std::uint8_t size = instruction.size;
        const std::uint8_t prefix = size == 2 ? 0x66 : 0;
        const bool byte = size == 1;
        if (operands[1].kind == Operand::Kind::Immediate) {
            Emit(prefix, size == 8, {byte ? std::uint8_t{0xF6} : std::uint8_t{0xF7}}, 0, false,
                 RegisterRm(reg(0), byte), byte ? 1 : (size == 2 ? 2 : 4), operands[1].immediate);
            return;
        }
        Emit(prefix, size == 8, {byte ? std::uint8_t{0x84} : std::uint8_t{0x85}}, number(1), byte,
             RegisterRm(reg(0), byte));
        return;
    }
    case Op::Imul: {
        const Rm source =
            operands[1].kind == Operand::Kind::Memory ? MemoryRm(operands[1].address) : RegisterRm(reg(1));
        Emit(instruction.size == 2 ? 0x66 : 0, instruction.size == 8, {0x0F, 0xAF}, number(0), false, source);
        return;
    }
    case Op::ImulImm: {
        const std::int64_t value = operands[2].immediate;
        const Rm source =
            operands[1].kind == Operand::Kind::Memory ? MemoryRm(operands[1].address) : RegisterRm(reg(1));
        if (FitsInt8(value))
            Emit(0, instruction.size == 8, {0x6B}, number(0), false, source, 1, value);
        else
            Emit(0, instruction.size == 8, {0x69}, number(0), false, source, 4, value);
        return;
    }
    case Op::Neg:
        Emit(instruction.size == 2 ? 0x66 : 0, instruction.size == 8,
             {instruction.size == 1 ? std::uint8_t{0xF6} : std::uint8_t{0xF7}}, 3, false,
             RegisterRm(reg(0), instruction.size == 1));
        return;
    case Op::Not:
        Emit(instruction.size == 2 ? 0x66 : 0, instruction.size == 8,
             {instruction.size == 1 ? std::uint8_t{0xF6} : std::uint8_t{0xF7}}, 2, false,
             RegisterRm(reg(0), instruction.size == 1));
        return;
    case Op::Shl:
        EncodeShift(instruction, 4);
        return;
    case Op::Shr:
        EncodeShift(instruction, 5);
        return;
    case Op::Sar:
        EncodeShift(instruction, 7);
        return;
    case Op::Ror:
        EncodeShift(instruction, 1);
        return;
    case Op::Bswap: {
        const auto hardware = HardwareNumber(reg(0));
        const std::uint8_t rex = static_cast<std::uint8_t>(0x40U | (instruction.size == 8 ? 0x08U : 0U) |
                                                           ((hardware & 8U) != 0 ? 0x01U : 0U));
        if (rex != 0x40)
            Byte(rex);
        Byte(0x0F);
        Byte(static_cast<std::uint8_t>(0xC8U + (hardware & 7U)));
        return;
    }
    case Op::MovZx:
        switch (instruction.sourceSize) {
        case 1:
            Emit(0, false, {0x0F, 0xB6}, number(0), false, RegisterRm(reg(1), true));
            return;
        case 2:
            Emit(0, false, {0x0F, 0xB7}, number(0), false, RegisterRm(reg(1)));
            return;
        case 4:
            Emit(0, false, {0x8B}, number(0), false, RegisterRm(reg(1)));
            return;
        default:
            Emit(0, true, {0x8B}, number(0), false, RegisterRm(reg(1)));
            return;
        }
    case Op::MovSx: {
        const bool wide = instruction.size == 8;
        switch (instruction.sourceSize) {
        case 1:
            Emit(0, wide, {0x0F, 0xBE}, number(0), false, RegisterRm(reg(1), true));
            return;
        case 2:
            Emit(0, wide, {0x0F, 0xBF}, number(0), false, RegisterRm(reg(1)));
            return;
        case 4:
            if (wide)
                Emit(0, true, {0x63}, number(0), false, RegisterRm(reg(1)));
            else
                Emit(0, false, {0x8B}, number(0), false, RegisterRm(reg(1)));
            return;
        default:
            Emit(0, true, {0x8B}, number(0), false, RegisterRm(reg(1)));
            return;
        }
    }
    case Op::SignExtendAccumulator:
        if (instruction.size == 8)
            Byte(0x48);
        Byte(0x99);
        return;
    case Op::Div:
        Emit(0, instruction.size == 8, {0xF7}, 6, false, RegisterRm(reg(0)));
        return;
    case Op::MulHigh:
        Emit(0, instruction.size == 8, {0xF7}, 4, false, RegisterRm(reg(0)));
        return;
    case Op::Idiv:
        Emit(0, instruction.size == 8, {0xF7}, 7, false, RegisterRm(reg(0)));
        return;
    case Op::SetCc:
        Emit(0, false, {0x0F, static_cast<std::uint8_t>(0x90U | instruction.condition)}, 0, false,
             RegisterRm(reg(0), true));
        return;
    case Op::Call:
        CallSymbol(operands[0].target);
        return;
    case Op::CallIndirect:
        Emit(0, false, {0xFF}, 2, false, RegisterRm(reg(0)));
        return;
    case Op::Ud2:
        Byte(0x0F);
        Byte(0x0B);
        return;
    case Op::MovFp:
        Emit(0, false, {0x0F, 0x28}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::LoadFp:
        Emit(instruction.size == 4 ? 0xF3 : 0xF2, false, {0x0F, 0x10}, number(0), false, MemoryRm(operands[1].address));
        return;
    case Op::StoreFp:
        Emit(instruction.size == 4 ? 0xF3 : 0xF2, false, {0x0F, 0x11}, number(1), false, MemoryRm(operands[0].address));
        return;
    case Op::AddFp:
        EncodeFloat(instruction, 0x58);
        return;
    case Op::MulFp:
        EncodeFloat(instruction, 0x59);
        return;
    case Op::SubFp:
        EncodeFloat(instruction, 0x5C);
        return;
    case Op::DivFp:
        EncodeFloat(instruction, 0x5E);
        return;
    case Op::SqrtFp:
        EncodeFloat(instruction, 0x51);
        return;
    case Op::CompareFp:
        Emit(instruction.size == 4 ? 0 : 0x66, false, {0x0F, 0x2E}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::XorFp:
        Emit(0, false, {0x0F, 0x57}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::IntToFp:
        Emit(instruction.size == 4 ? 0xF3 : 0xF2, instruction.sourceSize == 8, {0x0F, 0x2A}, number(0), false,
             RegisterRm(reg(1)));
        return;
    case Op::FpToInt:
        Emit(instruction.sourceSize == 4 ? 0xF3 : 0xF2, instruction.size == 8, {0x0F, 0x2C}, number(0), false,
             RegisterRm(reg(1)));
        return;
    case Op::FpToFp:
        Emit(instruction.size == 8 ? 0xF3 : 0xF2, false, {0x0F, 0x5A}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::GprToXmm:
        Emit(0x66, instruction.size == 8, {0x0F, 0x6E}, number(0), false, RegisterRm(reg(1)));
        return;
    case Op::XmmToGpr:
        Emit(0x66, instruction.size == 8, {0x0F, 0x7E}, number(1), false, RegisterRm(reg(0)));
        return;
    case Op::FpToUInt64:
        EncodeFpToUInt64(instruction);
        return;
    case Op::UInt64ToFp:
        EncodeUInt64ToFp(instruction);
        return;
    case Op::Panic:
        EncodePanic(instruction);
        return;
    case Op::RepMovsb:
        Byte(0xF3);
        Byte(0xA4);
        return;
    case Op::Jmp:
    case Op::Jcc:
    case Op::Ret:
        return;
    }
}
} // namespace Rux::X86_64Machine
