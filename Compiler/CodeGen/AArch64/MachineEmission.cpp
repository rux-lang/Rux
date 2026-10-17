#include "CodeGen/AArch64/MachineEmission.h"

#include "CodeGen/Machine/RegisterAllocator.h"

#include <algorithm>
#include <limits>

namespace Rux::AArch64Machine {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::Operand;
using Machine::RegClass;

namespace {
[[nodiscard]] bool IsPure(const Op op) {
    switch (op) {
    case Op::Mov:
    case Op::MovImm:
    case Op::Load:
    case Op::LoadSigned:
    case Op::AddressOf:
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
    case Op::MultiplySubtract:
    case Op::Neg:
    case Op::Not:
    case Op::Shl:
    case Op::Shr:
    case Op::Sar:
    case Op::Ror:
    case Op::ZeroExtend:
    case Op::SignExtend:
    case Op::Bswap:
    case Op::CSet:
    case Op::MovFp:
    case Op::LoadFp:
    case Op::AddFp:
    case Op::SubFp:
    case Op::MulFp:
    case Op::DivFp:
    case Op::SqrtFp:
    case Op::NegFp:
    case Op::IntToFp:
    case Op::UIntToFp:
    case Op::FpToInt:
    case Op::FpToUInt:
    case Op::FpToFp:
    case Op::GprToFp:
    case Op::FpToGpr:
        return true;
    default:
        return false;
    }
}

/// Removes instructions whose only effect is a virtual register nothing reads.
void EliminateDeadCode(Machine::Function &function) {
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<bool> used(Machine::PhysicalRegisterCount + function.VirtualCount(), false);
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instructions) {
                for (const auto &operand : instruction.operands) {
                    if (operand.kind == Operand::Kind::Register && operand.use)
                        used[operand.reg] = true;
                    else if (operand.kind == Operand::Kind::Memory) {
                        if (operand.address.base != NoReg)
                            used[operand.address.base] = true;
                        if (operand.address.index != NoReg)
                            used[operand.address.index] = true;
                    }
                }
            }
        }
        for (auto &block : function.blocks) {
            const auto before = block.instructions.size();
            std::erase_if(block.instructions, [&](const Instruction &instruction) {
                if (!IsPure(static_cast<Op>(instruction.opcode)) || instruction.implicitDefs != 0)
                    return false;
                bool defines = false;
                for (const auto &operand : instruction.operands) {
                    if (operand.kind != Operand::Kind::Register || !operand.def)
                        continue;
                    defines = true;
                    if (!Machine::IsVirtual(operand.reg) || used[operand.reg])
                        return false;
                }
                return defines;
            });
            changed = changed || block.instructions.size() != before;
        }
    }
}

class SpillCode final : public Machine::SpillCodeFactory {
public:
    [[nodiscard]] Instruction Load(const Machine::Reg destination, const RegClass regClass,
                                   const std::int32_t frameObject) const override {
        Instruction instruction;
        Address address;
        address.frameObject = frameObject;
        instruction.opcode = static_cast<std::uint16_t>(regClass == RegClass::Vector ? Op::LoadFp : Op::Load);
        instruction.size = 8;
        instruction.sourceSize = 8;
        instruction.operands = {Operand::Def(destination), Operand::Mem(address)};
        return instruction;
    }

    [[nodiscard]] Instruction Store(const Machine::Reg source, const RegClass regClass,
                                    const std::int32_t frameObject) const override {
        Instruction instruction;
        Address address;
        address.frameObject = frameObject;
        instruction.opcode = static_cast<std::uint16_t>(regClass == RegClass::Vector ? Op::StoreFp : Op::Store);
        instruction.size = 8;
        instruction.operands = {Operand::Mem(address), Operand::Use(source)};
        return instruction;
    }

    // Vector registers hold scalars only, so the low doubleword is the whole value.
    [[nodiscard]] std::int32_t SpillSize(const RegClass) const override {
        return 8;
    }
};

[[nodiscard]] Machine::TargetRegisters TargetRegisters() {
    Machine::TargetRegisters target;
    auto &general = target.allocationOrder[0];
    auto &vector = target.allocationOrder[1];
    // X16 and X17 are the encoder's scratch, X18 the platform register, X29 and X30 the frame record.
    for (Reg reg = 0; reg <= 15; ++reg)
        general.push_back(reg);
    for (Reg reg = 19; reg <= 28; ++reg) {
        general.push_back(reg);
        target.calleeSaved |= Machine::RegisterBit(reg);
    }
    for (Reg index = 0; index < 32; ++index)
        if (index < 8 || index > 15)
            vector.push_back(V0 + index);
    // AAPCS64 preserves the low halves of V8 to V15, which is all a scalar uses.
    for (Reg index = 8; index <= 15; ++index) {
        vector.push_back(V0 + index);
        target.calleeSaved |= Machine::RegisterBit(V0 + index);
    }
    for (const auto reg : general)
        target.classMask[0] |= Machine::RegisterBit(reg);
    for (const auto reg : vector)
        target.classMask[1] |= Machine::RegisterBit(reg);
    return target;
}

/// The frame, from the stack pointer up: the outgoing argument area, the frame objects, the callee-saved registers
/// the function uses, and the frame record, which the frame pointer addresses and which ends where the caller's
/// stack pointer was.
struct Frame {
    std::vector<std::pair<Reg, std::int32_t>> saves;
    std::vector<std::int32_t> objectOffsets;
    std::int32_t record = 0;
    std::int32_t size = 0;
};

[[nodiscard]] Frame LayoutFrame(const SelectedFunction &selected, const std::uint64_t usedCalleeSaved) {
    Frame frame;
    std::int32_t offset = selected.outgoingArgumentSize;
    std::vector<bool> referenced(selected.function.frameObjects.size(), false);
    for (const auto &block : selected.function.blocks)
        for (const auto &instruction : block.instructions)
            for (const auto &operand : instruction.operands)
                if (operand.kind == Operand::Kind::Memory && operand.address.frameObject >= 0)
                    referenced[static_cast<std::size_t>(operand.address.frameObject)] = true;
    for (std::size_t index = 0; index < selected.function.frameObjects.size(); ++index) {
        const auto &object = selected.function.frameObjects[index];
        if (!referenced[index]) {
            frame.objectOffsets.push_back(offset);
            continue;
        }
        const std::int32_t alignment = std::max<std::int32_t>(1, object.alignment);
        offset = (offset + alignment - 1) / alignment * alignment;
        frame.objectOffsets.push_back(offset);
        offset += object.size;
    }
    offset = (offset + 7) / 8 * 8;
    for (Reg reg = 0; reg < Machine::PhysicalRegisterCount; ++reg) {
        if ((usedCalleeSaved & Machine::RegisterBit(reg)) == 0)
            continue;
        frame.saves.emplace_back(reg, offset);
        offset += 8;
    }
    frame.record = (offset + 15) / 16 * 16;
    frame.size = frame.record + 16;
    return frame;
}

/// Orders blocks so a hot path falls through, with blocks that only lead to a failure report after everything else.
[[nodiscard]] std::vector<std::uint32_t> LayoutBlocks(const Machine::Function &function) {
    const auto count = static_cast<std::uint32_t>(function.blocks.size());
    std::vector<bool> placed(count, false);
    std::vector<std::uint32_t> order;
    order.reserve(count);
    const auto place = [&](std::uint32_t block, const bool allowCold) {
        while (true) {
            placed[block] = true;
            order.push_back(block);
            std::optional<std::uint32_t> best;
            for (const auto successor : function.blocks[block].successors) {
                if (placed[successor] || (!allowCold && function.blocks[successor].cold))
                    continue;
                if (!best || function.blocks[successor].loopDepth > function.blocks[*best].loopDepth ||
                    (function.blocks[successor].loopDepth == function.blocks[*best].loopDepth && successor < *best))
                    best = successor;
            }
            if (!best)
                return;
            block = *best;
        }
    };
    place(0, false);
    for (std::uint32_t block = 0; block < count; ++block)
        if (!placed[block] && !function.blocks[block].cold)
            place(block, false);
    for (std::uint32_t block = 0; block < count; ++block)
        if (!placed[block])
            place(block, true);
    return order;
}

/// Removes jumps to the block that follows and inverts a conditional jump over an unconditional one.
void SimplifyBranches(Machine::Function &function, const std::vector<std::uint32_t> &order) {
    for (std::size_t position = 0; position + 1 < order.size(); ++position) {
        auto &instructions = function.blocks[order[position]].instructions;
        const std::uint32_t next = order[position + 1];
        if (instructions.empty())
            continue;
        const auto is = [](const Instruction &instruction, const Op op) {
            return static_cast<Op>(instruction.opcode) == op;
        };
        if (is(instructions.back(), Op::Jmp) && instructions.back().operands[0].target == next) {
            instructions.pop_back();
            continue;
        }
        if (instructions.size() >= 2 && is(instructions.back(), Op::Jmp) &&
            is(instructions[instructions.size() - 2], Op::Jcc) &&
            instructions[instructions.size() - 2].operands[0].target == next) {
            auto &conditional = instructions[instructions.size() - 2];
            conditional.condition =
                static_cast<std::uint8_t>(A64::InvertCondition(static_cast<A64Condition>(conditional.condition)));
            conditional.operands[0].target = instructions.back().operands[0].target;
            instructions.pop_back();
        }
    }
}

struct BlockCode {
    std::vector<std::uint8_t> bytes;
    std::vector<SymbolUse> symbolUses;

    struct Branch {
        bool conditional = false;
        A64Condition condition = A64Condition::Eq;
        std::uint32_t target = 0;
    };

    std::vector<Branch> branches;
    std::uint32_t offset = 0;
};

void StoreSaved(MachineEncoder &encoder, const Reg reg, const std::int32_t offset, const bool restore) {
    const A64Enc &enc = encoder.Words();
    const A64Reg value = IsVectorRegister(reg) ? A64::Dn(HardwareNumber(reg)) : A64::Xn(HardwareNumber(reg));
    A64MemOperand memory{};
    encoder.Must(enc.ResolveMemOperand(A64::Sp, offset, 8, memory, A64::Xn(16)));
    if (memory.unscaled)
        encoder.Must(restore ? enc.Ldur(value, memory.base, memory.offset)
                             : enc.Stur(value, memory.base, memory.offset));
    else
        encoder.Must(restore ? enc.Ldr(value, memory.base, static_cast<std::uint64_t>(memory.offset))
                             : enc.Str(value, memory.base, static_cast<std::uint64_t>(memory.offset)));
}

void FrameRecord(MachineEncoder &encoder, const std::int32_t offset, const bool restore) {
    const A64Enc &enc = encoder.Words();
    A64Reg base = A64::Sp;
    std::int64_t displacement = offset;
    if (offset > 504) {
        encoder.Must(enc.AddSubLargeImm(A64::Xn(16), A64::Sp, offset, A64::Xn(16)));
        base = A64::Xn(16);
        displacement = 0;
    }
    encoder.Must(restore ? enc.Ldp(A64::Fp, A64::Lr, base, displacement)
                         : enc.Stp(A64::Fp, A64::Lr, base, displacement));
}
} // namespace

std::optional<EmittedFunction> EmitFunction(SelectedFunction &selected, const Target::OS targetOs) {
    auto &function = selected.function;
    EliminateDeadCode(function);
    const SpillCode spills;
    const auto allocation = Machine::AllocateRegisters(function, TargetRegisters(), spills);
    if (!allocation.succeeded)
        return std::nullopt;
    const Frame frame = LayoutFrame(selected, allocation.usedCalleeSaved);
    const auto order = LayoutBlocks(function);
    SimplifyBranches(function, order);

    std::vector<std::uint32_t> position(function.blocks.size(), 0);
    for (std::size_t index = 0; index < order.size(); ++index)
        position[order[index]] = static_cast<std::uint32_t>(index);

    std::vector<BlockCode> code(order.size());
    bool failed = false;
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto &block = function.blocks[order[index]];
        auto &blockCode = code[index];
        MachineEncoder encoder(blockCode.bytes, blockCode.symbolUses, frame.objectOffsets, selected, targetOs);
        const A64Enc &enc = encoder.Words();
        if (index == 0) {
            encoder.Must(targetOs == Target::OS::Windows ? enc.ProbeStack(frame.size) : enc.FrameAdjust(-frame.size));
            FrameRecord(encoder, frame.record, false);
            encoder.Must(enc.AddSubLargeImm(A64::Fp, A64::Sp, frame.record, A64::Xn(16)));
            for (const auto &[reg, offset] : frame.saves)
                StoreSaved(encoder, reg, offset, false);
        }
        for (const auto &instruction : block.instructions) {
            const auto op = static_cast<Op>(instruction.opcode);
            if (op == Op::Jmp || op == Op::Jcc) {
                BlockCode::Branch branch;
                branch.conditional = op == Op::Jcc;
                branch.condition = static_cast<A64Condition>(instruction.condition);
                branch.target = position[instruction.operands[0].target];
                blockCode.branches.push_back(branch);
                continue;
            }
            if (op == Op::Ret) {
                for (const auto &[reg, offset] : frame.saves)
                    StoreSaved(encoder, reg, offset, true);
                FrameRecord(encoder, frame.record, true);
                encoder.Must(enc.FrameAdjust(frame.size));
                encoder.Must(enc.Ret());
                continue;
            }
            encoder.Encode(instruction);
        }
        failed = failed || encoder.Failed();
    }
    if (failed)
        return std::nullopt;

    // Every instruction is four bytes, branches included, so the layout is known before any branch is encoded.
    std::uint32_t offset = 0;
    for (auto &blockCode : code) {
        blockCode.offset = offset;
        offset += static_cast<std::uint32_t>(blockCode.bytes.size() + blockCode.branches.size() * A64Enc::InstrSize);
    }
    EmittedFunction emitted;
    std::vector<SymbolUse> unused;
    const std::vector<std::int32_t> noFrame;
    for (auto &blockCode : code) {
        const auto base = static_cast<std::uint32_t>(emitted.bytes.size());
        emitted.bytes.insert(emitted.bytes.end(), blockCode.bytes.begin(), blockCode.bytes.end());
        for (auto use : blockCode.symbolUses) {
            use.offset += base;
            emitted.symbolUses.push_back(use);
        }
        MachineEncoder encoder(emitted.bytes, unused, noFrame, selected, targetOs);
        for (const auto &branch : blockCode.branches) {
            const auto displacement =
                static_cast<std::int64_t>(code[branch.target].offset) - static_cast<std::int64_t>(emitted.bytes.size());
            encoder.Must(branch.conditional ? encoder.Words().BCond(branch.condition, displacement)
                                            : encoder.Words().B(displacement));
        }
        if (encoder.Failed())
            return std::nullopt;
    }
    return emitted;
}
} // namespace Rux::AArch64Machine
