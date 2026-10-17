#include "CodeGen/AArch64/MachineEncoder.h"

#include "Object/Rcu/Rcu.h"

#include <bit>

namespace Rux::AArch64Machine {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::Operand;

namespace {
[[nodiscard]] A64Reg General(const Reg reg, const unsigned size) {
    if (reg == Sp)
        return size == 8 ? A64::Sp : A64::Wsp;
    return size == 8 ? A64::Xn(HardwareNumber(reg)) : A64::Wn(HardwareNumber(reg));
}

[[nodiscard]] A64Reg Floating(const Reg reg, const unsigned bytes) {
    return A64::Vn(HardwareNumber(reg), bytes * 8);
}

[[nodiscard]] A64Reg Zero(const unsigned size) {
    return size == 8 ? A64::Xzr : A64::Wzr;
}

[[nodiscard]] unsigned Width(const std::uint8_t size) {
    return size == 8 ? 8 : 4;
}

/// How a Unix kernel is asked to write: the call number, the register it travels in, and the SVC immediate.
struct WriteCall {
    std::uint64_t number;
    unsigned numberReg;
    std::uint16_t trap;
};

[[nodiscard]] WriteCall WriteCallFor(const Target::OS os) {
    switch (os) {
    case Target::OS::MacOS:
        return {4, 16, 0x80};
    case Target::OS::FreeBSD:
        return {4, 8, 0};
    default:
        return {64, 8, 0};
    }
}
} // namespace

MachineEncoder::MachineEncoder(std::vector<std::uint8_t> &outputBytes, std::vector<SymbolUse> &outputSymbolUses,
                               const std::vector<std::int32_t> &frameObjectOffsets, const SelectedFunction &function,
                               const Target::OS os)
    : symbolUses(outputSymbolUses)
    , objectOffsets(frameObjectOffsets)
    , selected(function)
    , targetOs(os)
    , enc(outputBytes) {
}

void MachineEncoder::Must(const A64Status status) {
    failed = failed || status != A64Status::Ok;
}

void MachineEncoder::SymbolAddress(const A64Reg destination, const std::uint32_t symbol, const std::int32_t addend) {
    const std::uint32_t page = enc.Size();
    Must(enc.Adrp(destination, 0));
    symbolUses.push_back({page, symbol, RcuRelType::AArch64AdrPrelPgHi21, addend});
    const std::uint32_t low = enc.Size();
    Must(enc.AddImm(destination, destination, 0));
    symbolUses.push_back({low, symbol, RcuRelType::AArch64AddAbsLo12Nc, addend});
}

MachineEncoder::Memory MachineEncoder::Resolve(const Address &address, const unsigned width, const A64Reg scratch) {
    const A64Reg other = scratch.code == 16 ? A64::Xn(17) : A64::Xn(16);
    A64Reg base;
    std::int64_t offset = address.displacement;
    if (address.symbol != Machine::NoSymbol) {
        SymbolAddress(scratch, address.symbol, address.displacement);
        base = scratch;
        offset = 0;
    }
    else if (address.frameObject >= 0) {
        base = A64::Sp;
        offset += objectOffsets.at(static_cast<std::size_t>(address.frameObject));
    }
    else {
        base = General(address.base, 8);
    }
    if (address.index != NoReg) {
        const auto shift = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(address.scale)));
        const A64Reg index = General(address.index, 8);
        if (offset == 0 && (shift == 0 || (1U << shift) == width) && !base.IsStackPointer())
            return Memory{base, index, true, shift, 0, false};
        if (base.IsStackPointer()) {
            Must(enc.AddSubLargeImm(scratch, base, 0, other));
            base = scratch;
        }
        Must(enc.Add(scratch, base, index, A64ShiftKind::Lsl, shift));
        base = scratch;
    }
    A64MemOperand operand{};
    Must(enc.ResolveMemOperand(base, offset, width, operand, base.code == scratch.code ? other : scratch));
    return Memory{operand.base, {}, false, 0, operand.offset, operand.unscaled};
}

void MachineEncoder::ComputeAddress(const A64Reg destination, const Address &address) {
    if (address.symbol != Machine::NoSymbol) {
        SymbolAddress(destination, address.symbol, address.displacement);
        return;
    }
    if (address.frameObject >= 0) {
        Must(enc.AddSubLargeImm(destination, A64::Sp,
                                objectOffsets.at(static_cast<std::size_t>(address.frameObject)) + address.displacement,
                                A64::Xn(16)));
        return;
    }
    const A64Reg base = General(address.base, 8);
    if (address.index != NoReg) {
        const auto shift = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(address.scale)));
        Must(enc.Add(destination, base, General(address.index, 8), A64ShiftKind::Lsl, shift));
        if (address.displacement != 0)
            Must(enc.AddSubLargeImm(destination, destination, address.displacement, A64::Xn(16)));
        return;
    }
    Must(enc.AddSubLargeImm(destination, base, address.displacement, A64::Xn(16)));
}

void MachineEncoder::LoadStore(const Instruction &instruction) {
    const auto op = static_cast<Op>(instruction.opcode);
    const auto &operands = instruction.operands;
    const bool store = op == Op::Store || op == Op::StoreFp;
    const Operand &value = operands[store ? 1 : 0];
    const Address &address = operands[store ? 0 : 1].address;
    const bool floating = op == Op::LoadFp || op == Op::StoreFp;
    const unsigned width = floating ? instruction.size : (store ? instruction.size : instruction.sourceSize);
    const Memory memory = Resolve(address, width, A64::Xn(16));
    const auto scaled = static_cast<std::uint64_t>(memory.offset);
    if (floating) {
        const A64Reg reg = Floating(value.reg, instruction.size);
        if (memory.indexed)
            Must(store ? enc.StrReg(reg, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                       : enc.LdrReg(reg, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift));
        else if (memory.unscaled)
            Must(store ? enc.Stur(reg, memory.base, memory.offset) : enc.Ldur(reg, memory.base, memory.offset));
        else
            Must(store ? enc.Str(reg, memory.base, scaled) : enc.Ldr(reg, memory.base, scaled));
        return;
    }
    if (store) {
        const A64Reg source =
            value.kind == Operand::Kind::Immediate ? Zero(width == 8 ? 8 : 4) : General(value.reg, width == 8 ? 8 : 4);
        switch (width) {
        case 1:
            Must(memory.indexed    ? enc.StrbReg(source, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Sturb(source, memory.base, memory.offset)
                                   : enc.Strb(source, memory.base, scaled));
            return;
        case 2:
            Must(memory.indexed    ? enc.StrhReg(source, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Sturh(source, memory.base, memory.offset)
                                   : enc.Strh(source, memory.base, scaled));
            return;
        default:
            Must(memory.indexed    ? enc.StrReg(source, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Stur(source, memory.base, memory.offset)
                                   : enc.Str(source, memory.base, scaled));
            return;
        }
    }
    const bool sign = op == Op::LoadSigned && width < 8;
    const A64Reg wide = General(value.reg, 8);
    const A64Reg narrow = General(value.reg, 4);
    switch (width) {
    case 1:
        if (sign)
            Must(memory.indexed    ? enc.LdrsbReg(wide, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldursb(wide, memory.base, memory.offset)
                                   : enc.Ldrsb(wide, memory.base, scaled));
        else
            Must(memory.indexed    ? enc.LdrbReg(narrow, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldurb(narrow, memory.base, memory.offset)
                                   : enc.Ldrb(narrow, memory.base, scaled));
        return;
    case 2:
        if (sign)
            Must(memory.indexed    ? enc.LdrshReg(wide, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldursh(wide, memory.base, memory.offset)
                                   : enc.Ldrsh(wide, memory.base, scaled));
        else
            Must(memory.indexed    ? enc.LdrhReg(narrow, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldurh(narrow, memory.base, memory.offset)
                                   : enc.Ldrh(narrow, memory.base, scaled));
        return;
    case 4:
        if (sign)
            Must(memory.indexed    ? enc.LdrswReg(wide, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldursw(wide, memory.base, memory.offset)
                                   : enc.Ldrsw(wide, memory.base, scaled));
        else
            Must(memory.indexed    ? enc.LdrReg(narrow, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
                 : memory.unscaled ? enc.Ldur(narrow, memory.base, memory.offset)
                                   : enc.Ldr(narrow, memory.base, scaled));
        return;
    default:
        Must(memory.indexed    ? enc.LdrReg(wide, memory.base, memory.index, A64ExtendKind::Uxtx, memory.shift)
             : memory.unscaled ? enc.Ldur(wide, memory.base, memory.offset)
                               : enc.Ldr(wide, memory.base, scaled));
        return;
    }
}

void MachineEncoder::Arithmetic(const Instruction &instruction) {
    const auto op = static_cast<Op>(instruction.opcode);
    const auto &operands = instruction.operands;
    const unsigned size = Width(instruction.size);
    const A64Reg destination = General(operands[0].reg, size);
    const A64Reg left = General(operands[1].reg, size);
    if (operands[2].kind == Operand::Kind::Immediate) {
        const auto immediate = static_cast<std::uint64_t>(operands[2].immediate);
        switch (op) {
        case Op::Add:
            Must(enc.AddImm(destination, left, immediate));
            return;
        case Op::Sub:
            Must(enc.SubImm(destination, left, immediate));
            return;
        case Op::And:
            Must(enc.AndImm(destination, left, immediate));
            return;
        case Op::Or:
            Must(enc.OrrImm(destination, left, immediate));
            return;
        case Op::Xor:
            Must(enc.EorImm(destination, left, immediate));
            return;
        case Op::Shl:
            Must(enc.Lsl(destination, left, static_cast<unsigned>(immediate)));
            return;
        case Op::Shr:
            Must(enc.Lsr(destination, left, static_cast<unsigned>(immediate)));
            return;
        case Op::Sar:
            Must(enc.Asr(destination, left, static_cast<unsigned>(immediate)));
            return;
        case Op::Ror:
            Must(enc.Ror(destination, left, static_cast<unsigned>(immediate)));
            return;
        default:
            failed = true;
            return;
        }
    }
    const A64Reg right = General(operands[2].reg, size);
    switch (op) {
    case Op::Add:
        Must(enc.Add(destination, left, right));
        return;
    case Op::Sub:
        Must(enc.Sub(destination, left, right));
        return;
    case Op::And:
        Must(enc.And(destination, left, right));
        return;
    case Op::Or:
        Must(enc.Orr(destination, left, right));
        return;
    case Op::Xor:
        Must(enc.Eor(destination, left, right));
        return;
    case Op::Mul:
        Must(enc.Mul(destination, left, right));
        return;
    case Op::MulHigh:
        Must(enc.Umulh(destination, left, right));
        return;
    case Op::MulHighSigned:
        Must(enc.Smulh(destination, left, right));
        return;
    case Op::Div:
        Must(enc.Udiv(destination, left, right));
        return;
    case Op::Idiv:
        Must(enc.Sdiv(destination, left, right));
        return;
    case Op::Shl:
        Must(enc.Lslv(destination, left, right));
        return;
    case Op::Shr:
        Must(enc.Lsrv(destination, left, right));
        return;
    case Op::Sar:
        Must(enc.Asrv(destination, left, right));
        return;
    case Op::Ror:
        Must(enc.Rorv(destination, left, right));
        return;
    default:
        failed = true;
        return;
    }
}

void MachineEncoder::WriteStatic(const std::uint32_t symbol, const std::uint32_t length) {
    const WriteCall call = WriteCallFor(targetOs);
    SymbolAddress(A64::Xn(1), symbol, 0);
    Must(enc.LoadImm64(A64::Xn(2), length));
    Must(enc.LoadImm64(A64::Xn(0), 2));
    Must(enc.LoadImm64(A64::Xn(call.numberReg), call.number));
    Must(enc.Svc(call.trap));
}

void MachineEncoder::Panic(const Instruction &instruction) {
    // Three writes to standard error — the prefix, the message slice the program built, the location — then the
    // stack-trace printer when there is one, then BRK, as the established emitter reports a failure. Nothing returns,
    // so every register is free; the message pointer moves to X17 first, clear of the argument registers.
    if (targetOs == Target::OS::Windows) {
        failed = true;
        return;
    }
    const PanicSite &site = selected.panics.at(static_cast<std::size_t>(instruction.operands[1].immediate));
    const WriteCall call = WriteCallFor(targetOs);
    Must(enc.Mov(A64::Xn(17), General(instruction.operands[0].reg, 8)));
    WriteStatic(site.prefix, site.prefixLength);
    Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(17), 0));
    Must(enc.LoadImm64(A64::Xn(0), 2));
    Must(enc.LoadImm64(A64::Xn(call.numberReg), call.number));
    Must(enc.Svc(call.trap));
    WriteStatic(site.location, site.locationLength);
    if (site.backtrace != Machine::NoSymbol) {
        symbolUses.push_back({enc.Size(), site.backtrace, RcuRelType::AArch64Call26, 0});
        Must(enc.Bl(0));
    }
    Must(enc.Brk(1));
}

void MachineEncoder::Encode(const Instruction &instruction) {
    const auto op = static_cast<Op>(instruction.opcode);
    const auto &operands = instruction.operands;
    const auto reg = [&](const std::size_t index, const unsigned size) { return General(operands[index].reg, size); };
    const auto vector = [&](const std::size_t index, const unsigned bytes) {
        return Floating(operands[index].reg, bytes);
    };
    switch (op) {
    case Op::Mov:
        Must(enc.Mov(reg(0, 8), reg(1, 8)));
        return;
    case Op::MovImm:
        Must(enc.LoadImm64(reg(0, 8), static_cast<std::uint64_t>(operands[1].immediate)));
        return;
    case Op::Load:
    case Op::LoadSigned:
    case Op::Store:
    case Op::LoadFp:
    case Op::StoreFp:
        LoadStore(instruction);
        return;
    case Op::AddressOf:
        ComputeAddress(reg(0, 8), operands[1].address);
        return;
    case Op::Add:
    case Op::Sub:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::Mul:
    case Op::MulHigh:
    case Op::MulHighSigned:
    case Op::Div:
    case Op::Idiv:
    case Op::Shl:
    case Op::Shr:
    case Op::Sar:
    case Op::Ror:
        Arithmetic(instruction);
        return;
    case Op::MultiplySubtract:
        Must(enc.Msub(reg(0, 8), reg(1, 8), reg(2, 8), reg(3, 8)));
        return;
    case Op::Neg:
        Must(enc.Neg(reg(0, 8), reg(1, 8)));
        return;
    case Op::Not:
        Must(enc.Mvn(reg(0, 8), reg(1, 8)));
        return;
    case Op::ZeroExtend:
        switch (instruction.sourceSize) {
        case 1:
            Must(enc.Uxtb(reg(0, 4), reg(1, 4)));
            return;
        case 2:
            Must(enc.Uxth(reg(0, 4), reg(1, 4)));
            return;
        default:
            Must(enc.Mov(reg(0, 4), reg(1, 4)));
            return;
        }
    case Op::SignExtend:
        switch (instruction.sourceSize) {
        case 1:
            Must(enc.Sxtb(reg(0, 8), reg(1, 4)));
            return;
        case 2:
            Must(enc.Sxth(reg(0, 8), reg(1, 4)));
            return;
        default:
            Must(enc.Sxtw(reg(0, 8), reg(1, 4)));
            return;
        }
    case Op::Bswap:
        Must(enc.Rev(reg(0, Width(instruction.size)), reg(1, Width(instruction.size))));
        return;
    case Op::Cmp: {
        const unsigned size = Width(instruction.size);
        if (operands[1].kind == Operand::Kind::Immediate) {
            const std::int64_t immediate = operands[1].immediate;
            Must(immediate >= 0 ? enc.SubsImm(Zero(size), reg(0, size), static_cast<std::uint64_t>(immediate))
                                : enc.AddsImm(Zero(size), reg(0, size), static_cast<std::uint64_t>(-immediate)));
            return;
        }
        Must(enc.Cmp(reg(0, size), reg(1, size)));
        return;
    }
    case Op::Tst: {
        const unsigned size = Width(instruction.size);
        if (operands[1].kind == Operand::Kind::Immediate)
            Must(enc.AndsImm(Zero(size), reg(0, size), static_cast<std::uint64_t>(operands[1].immediate)));
        else
            Must(enc.Tst(reg(0, size), reg(1, size)));
        return;
    }
    case Op::CSet:
        Must(enc.Cset(reg(0, 4), static_cast<A64Condition>(instruction.condition)));
        return;
    case Op::Call:
        symbolUses.push_back({enc.Size(), operands[0].target, RcuRelType::AArch64Call26, 0});
        Must(enc.Bl(0));
        return;
    case Op::CallIndirect:
        Must(enc.Blr(reg(0, 8)));
        return;
    case Op::Trap:
        Must(enc.Brk(1));
        return;
    case Op::MovFp:
        Must(enc.Fmov(vector(0, 8), vector(1, 8)));
        return;
    case Op::AddFp:
        Must(enc.Fadd(vector(0, instruction.size), vector(1, instruction.size), vector(2, instruction.size)));
        return;
    case Op::SubFp:
        Must(enc.Fsub(vector(0, instruction.size), vector(1, instruction.size), vector(2, instruction.size)));
        return;
    case Op::MulFp:
        Must(enc.Fmul(vector(0, instruction.size), vector(1, instruction.size), vector(2, instruction.size)));
        return;
    case Op::DivFp:
        Must(enc.Fdiv(vector(0, instruction.size), vector(1, instruction.size), vector(2, instruction.size)));
        return;
    case Op::SqrtFp:
        Must(enc.Fsqrt(vector(0, instruction.size), vector(1, instruction.size)));
        return;
    case Op::NegFp:
        Must(enc.Fneg(vector(0, instruction.size), vector(1, instruction.size)));
        return;
    case Op::CompareFp:
        Must(enc.Fcmp(vector(0, instruction.size), vector(1, instruction.size)));
        return;
    case Op::IntToFp:
        Must(enc.Scvtf(vector(0, instruction.size), reg(1, Width(instruction.sourceSize))));
        return;
    case Op::UIntToFp:
        Must(enc.Ucvtf(vector(0, instruction.size), reg(1, Width(instruction.sourceSize))));
        return;
    case Op::FpToInt:
        Must(enc.Fcvtzs(reg(0, Width(instruction.size)), vector(1, instruction.sourceSize)));
        return;
    case Op::FpToUInt:
        Must(enc.Fcvtzu(reg(0, Width(instruction.size)), vector(1, instruction.sourceSize)));
        return;
    case Op::FpToFp:
        Must(enc.Fcvt(vector(0, instruction.size), vector(1, instruction.sourceSize)));
        return;
    case Op::GprToFp:
        Must(enc.Fmov(vector(0, instruction.size), reg(1, Width(instruction.size))));
        return;
    case Op::FpToGpr:
        Must(enc.Fmov(reg(0, Width(instruction.size)), vector(1, instruction.size)));
        return;
    case Op::CopyBytes: {
        // Words through X16 with X17 counting them, then the bytes that remain; both addresses advance.
        const A64Reg to = reg(0, 8);
        const A64Reg from = reg(1, 8);
        const auto size = static_cast<std::uint64_t>(operands[2].immediate);
        if (size / 8 != 0) {
            Must(enc.LoadImm64(A64::Xn(17), size / 8));
            const std::uint32_t loop = enc.Size();
            Must(enc.Ldur(A64::Xn(16), from, 8, A64IndexMode::PostIndex));
            Must(enc.Stur(A64::Xn(16), to, 8, A64IndexMode::PostIndex));
            Must(enc.SubsImm(A64::Xn(17), A64::Xn(17), 1));
            Must(enc.BCond(A64Condition::Ne, static_cast<std::int64_t>(loop) - enc.Size()));
        }
        for (std::uint64_t byte = 0; byte < size % 8; ++byte) {
            Must(enc.Ldurb(A64::Wn(16), from, 1, A64IndexMode::PostIndex));
            Must(enc.Sturb(A64::Wn(16), to, 1, A64IndexMode::PostIndex));
        }
        return;
    }
    case Op::Panic:
        Panic(instruction);
        return;
    default:
        failed = true;
        return;
    }
}
} // namespace Rux::AArch64Machine
