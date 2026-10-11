#include "CodeGen/X86_64/MachineEmission.h"

#include "CodeGen/Machine/RegisterAllocator.h"
#include "CodeGen/X86_64/MachinePrinter.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Rux::X86_64Machine {
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
    case Op::Lea:
    case Op::Add:
    case Op::Sub:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::Imul:
    case Op::ImulImm:
    case Op::Neg:
    case Op::Not:
    case Op::Shl:
    case Op::Shr:
    case Op::Sar:
    case Op::Ror:
    case Op::Bswap:
    case Op::MovZx:
    case Op::MovSx:
    case Op::SetCc:
    case Op::MovFp:
    case Op::LoadFp:
    case Op::AddFp:
    case Op::SubFp:
    case Op::MulFp:
    case Op::DivFp:
    case Op::SqrtFp:
    case Op::XorFp:
    case Op::IntToFp:
    case Op::FpToInt:
    case Op::FpToFp:
    case Op::GprToXmm:
    case Op::XmmToGpr:
    case Op::FpToUInt64:
        return true;
    default:
        return false;
    }
}

/// Removes instructions whose only effect is a virtual register nothing reads. Selection defines values eagerly — a
/// constant whose every use became an immediate, an address every access folded — and leaves the cleanup here.
void EliminateDeadCode(Machine::Function &function) {
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<bool> used(Machine::PhysicalRegisterCount + function.VirtualCount(), false);
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instructions) {
                // A pure two-address operation's read of the one register it overwrites does not keep that register
                // alive: if nothing else reads it, the operation and the copy feeding it are both dead.
                const auto defines = std::ranges::count_if(instruction.operands, [](const Operand &operand) {
                    return operand.kind == Operand::Kind::Register && operand.def;
                });
                const bool removable =
                    IsPure(static_cast<Op>(instruction.opcode)) && instruction.implicitDefs == 0 && defines == 1;
                for (const auto &operand : instruction.operands) {
                    if (operand.kind == Operand::Kind::Register && operand.use && !(operand.def && removable))
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

/// Whether an operation leaves the flags as they were.
[[nodiscard]] bool KeepsFlags(const Op op) {
    switch (op) {
    case Op::Mov:
    case Op::MovImm:
    case Op::Load:
    case Op::LoadSigned:
    case Op::Store:
    case Op::Lea:
    case Op::MovZx:
    case Op::MovSx:
    case Op::SetCc:
    case Op::Bswap:
    case Op::MovFp:
    case Op::LoadFp:
    case Op::StoreFp:
        return true;
    default:
        return false;
    }
}

/// A branch on a Boolean register that a comparison earlier in the same block produced branches on the comparison's
/// flags instead. The register is traced back through copies and its zero extension to the `setcc` that made it;
/// nothing between that and the branch may write the flags, except an addition or subtraction of a constant, which
/// becomes the equivalent `lea`. A loop testing a flag it computed in its latch, such as `while same && ...` after
/// rotation, then ends in the comparison's own conditional branch. The `setcc` stays for any other reader and is
/// otherwise dead.
void FuseFlagBranches(Machine::Function &function) {
    for (auto &block : function.blocks) {
        auto &code = block.instructions;
        for (std::size_t branch = 1; branch < code.size(); ++branch) {
            const auto &test = code[branch - 1];
            auto &jump = code[branch];
            const auto condition = static_cast<Cond>(jump.condition);
            if (static_cast<Op>(jump.opcode) != Op::Jcc || (condition != Cond::Equal && condition != Cond::NotEqual) ||
                static_cast<Op>(test.opcode) != Op::Test || test.operands.size() != 2 ||
                !test.operands[0].IsRegister() || !test.operands[1].IsRegister() ||
                test.operands[0].reg != test.operands[1].reg)
                continue;
            Reg wanted = test.operands[0].reg;
            std::size_t set = branch - 1;
            bool found = false;
            while (set > 0 && !found) {
                --set;
                const auto &instruction = code[set];
                const bool defines = std::ranges::any_of(instruction.operands, [&](const Operand &operand) {
                    return operand.IsRegister() && operand.def && operand.reg == wanted;
                });
                if (!defines)
                    continue;
                const auto op = static_cast<Op>(instruction.opcode);
                if (op == Op::SetCc && instruction.operands.size() == 1)
                    found = true;
                else if ((op == Op::Mov || (op == Op::MovZx && instruction.sourceSize == 1)) &&
                         instruction.operands.size() == 2 && instruction.operands[1].IsRegister() &&
                         instruction.operands[0].IsRegister() && instruction.operands[0].use == false)
                    wanted = instruction.operands[1].reg;
                else
                    break;
            }
            if (!found)
                continue;
            std::vector<std::size_t> additions;
            bool kept = true;
            for (std::size_t index = set + 1; index + 1 < branch && kept; ++index) {
                const auto &instruction = code[index];
                const auto op = static_cast<Op>(instruction.opcode);
                if (KeepsFlags(op) && instruction.implicitDefs == 0)
                    continue;
                if ((op == Op::Add || op == Op::Sub) && instruction.operands.size() == 2 &&
                    instruction.operands[0].IsRegister() && instruction.operands[0].use &&
                    instruction.operands[0].def && instruction.operands[1].kind == Operand::Kind::Immediate &&
                    instruction.size == 8 &&
                    instruction.operands[1].immediate > std::numeric_limits<std::int32_t>::min() &&
                    instruction.operands[1].immediate <= std::numeric_limits<std::int32_t>::max())
                    additions.push_back(index);
                else
                    kept = false;
            }
            if (!kept)
                continue;
            for (const auto index : additions) {
                auto &instruction = code[index];
                const Reg reg = instruction.operands[0].reg;
                auto amount = instruction.operands[1].immediate;
                if (static_cast<Op>(instruction.opcode) == Op::Sub)
                    amount = -amount;
                Machine::Address address;
                address.base = reg;
                address.displacement = static_cast<std::int32_t>(amount);
                instruction.opcode = static_cast<std::uint16_t>(Op::Lea);
                instruction.operands = {Operand::Def(reg), Operand::Mem(address)};
            }
            const auto flags = static_cast<Cond>(code[set].condition);
            jump.condition = static_cast<std::uint8_t>(condition == Cond::NotEqual ? flags : Invert(flags));
            code.erase(code.begin() + static_cast<std::ptrdiff_t>(branch - 1));
            --branch;
        }
    }
}

/// Copies a loop's small condition block into every block in the loop that jumps back to it, so an iteration ends in
/// the conditional branch itself rather than a jump to the condition. The original stays as the loop entry. The
/// machine IR is not in SSA form, so a duplicated definition needs no renaming.
void RotateLoops(Machine::Function &function) {
    const auto rotatable = [&](const Machine::Block &block) {
        if (block.loopDepth == 0 || block.instructions.empty() || block.instructions.size() > 12)
            return false;
        bool branches = false;
        for (const auto &instruction : block.instructions) {
            switch (static_cast<Op>(instruction.opcode)) {
            case Op::Mov:
            case Op::MovImm:
            case Op::Load:
            case Op::LoadSigned:
            case Op::Lea:
            case Op::Add:
            case Op::Sub:
            case Op::And:
            case Op::Or:
            case Op::Xor:
            case Op::Cmp:
            case Op::Test:
            case Op::MovZx:
            case Op::MovSx:
            case Op::SetCc:
            case Op::MovFp:
            case Op::LoadFp:
            case Op::CompareFp:
                break;
            case Op::Jcc:
                branches = true;
                break;
            case Op::Jmp:
                break;
            default:
                return false;
            }
        }
        return branches && static_cast<Op>(block.instructions.back().opcode) == Op::Jmp;
    };
    // Branches end a block: the emitter places every jump after the block's other instructions. A latch whose final
    // jump follows a conditional one would interleave its own branch with the copied comparison, so only a latch
    // ending in its single jump takes the copy.
    const auto singleJump = [](const Machine::Block &block) {
        if (block.instructions.empty() || static_cast<Op>(block.instructions.back().opcode) != Op::Jmp)
            return false;
        return std::ranges::none_of(block.instructions.begin(), block.instructions.end() - 1,
                                    [](const Instruction &instruction) {
                                        const auto op = static_cast<Op>(instruction.opcode);
                                        return op == Op::Jmp || op == Op::Jcc;
                                    });
    };
    for (std::uint32_t latch = 1; latch < function.blocks.size(); ++latch) {
        auto &block = function.blocks[latch];
        if (block.loopDepth == 0 || !singleJump(block))
            continue;
        const std::uint32_t header = block.instructions.back().operands[0].target;
        if (header == latch || header == 0 || !rotatable(function.blocks[header]))
            continue;
        const auto copy = function.blocks[header].instructions;
        const auto successors = function.blocks[header].successors;
        auto &target = function.blocks[latch];
        target.instructions.pop_back();
        target.instructions.insert(target.instructions.end(), copy.begin(), copy.end());
        target.successors = successors;
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

    [[nodiscard]] std::int32_t SpillSize(const RegClass) const override {
        return 8;
    }
};

[[nodiscard]] Machine::TargetRegisters TargetRegistersFor(const bool win64) {
    Machine::TargetRegisters target;
    auto &general = target.allocationOrder[0];
    auto &vector = target.allocationOrder[1];
    if (win64) {
        general = {Rax, Rcx, Rdx, R8, R9, R10, R11, Rbx, Rsi, Rdi, R12, R13, R14, R15};
        target.calleeSaved = Machine::RegisterBit(Rbx) | Machine::RegisterBit(Rsi) | Machine::RegisterBit(Rdi) |
                             Machine::RegisterBit(R12) | Machine::RegisterBit(R13) | Machine::RegisterBit(R14) |
                             Machine::RegisterBit(R15);
        for (Machine::Reg index = 0; index < 16; ++index)
            vector.push_back(Xmm0 + index);
        for (Machine::Reg index = 6; index < 16; ++index)
            target.calleeSaved |= Machine::RegisterBit(Xmm0 + index);
    }
    else {
        general = {Rax, Rcx, Rdx, Rsi, Rdi, R8, R9, R10, R11, Rbx, R12, R13, R14, R15};
        target.calleeSaved = Machine::RegisterBit(Rbx) | Machine::RegisterBit(R12) | Machine::RegisterBit(R13) |
                             Machine::RegisterBit(R14) | Machine::RegisterBit(R15);
        for (Machine::Reg index = 0; index < 16; ++index)
            vector.push_back(Xmm0 + index);
    }
    for (const auto reg : general)
        target.classMask[0] |= Machine::RegisterBit(reg);
    for (const auto reg : vector)
        target.classMask[1] |= Machine::RegisterBit(reg);
    return target;
}

struct Frame {
    std::vector<Machine::Reg> savedGprs;
    std::vector<std::pair<Machine::Reg, std::int32_t>> savedVectors;
    std::vector<std::int32_t> objectOffsets;
    std::int32_t allocation = 0;
};

[[nodiscard]] Frame LayoutFrame(const SelectedFunction &selected, const std::uint64_t usedCalleeSaved) {
    Frame frame;
    for (const auto reg : {Rbx, Rsi, Rdi, R12, R13, R14, R15})
        if ((usedCalleeSaved & Machine::RegisterBit(reg)) != 0)
            frame.savedGprs.push_back(reg);
    std::int32_t offset = static_cast<std::int32_t>(frame.savedGprs.size() * 8);
    for (Machine::Reg index = 0; index < 16; ++index) {
        if ((usedCalleeSaved & Machine::RegisterBit(Xmm0 + index)) == 0)
            continue;
        offset = (offset + 16 + 15) / 16 * 16;
        frame.savedVectors.emplace_back(Xmm0 + index, offset);
    }
    // Objects no instruction addresses any more — the storage of allocations that folded away, or of failure messages
    // in blocks that were removed — take no space.
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
        offset = (offset + object.size + alignment - 1) / alignment * alignment;
        frame.objectOffsets.push_back(offset);
    }
    // The frame pointer is sixteen-byte aligned, so ending the frame on a multiple of sixteen below it leaves the
    // stack pointer aligned for every call, whatever number of registers the prologue pushed.
    const std::int32_t bottom = (offset + selected.outgoingArgumentSize + 15) / 16 * 16;
    frame.allocation = bottom - static_cast<std::int32_t>(frame.savedGprs.size() * 8);
    return frame;
}

/// Orders blocks so a hot path falls through: each block is followed by its most deeply nested unplaced successor,
/// and blocks that only lead to a failure report come after everything else.
[[nodiscard]] std::vector<std::uint32_t> LayoutBlocks(const Machine::Function &function) {
    const auto count = static_cast<std::uint32_t>(function.blocks.size());
    std::vector<bool> placed(count, false);
    std::vector<std::uint32_t> order;
    order.reserve(count);
    const auto place = [&](std::uint32_t block, const bool allowCold) {
        while (true) {
            placed[block] = true;
            order.push_back(block);
            const auto &successors = function.blocks[block].successors;
            std::optional<std::uint32_t> best;
            for (const auto successor : successors) {
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
    for (std::size_t position = 0; position < order.size(); ++position) {
        auto &instructions = function.blocks[order[position]].instructions;
        const std::optional<std::uint32_t> next =
            position + 1 < order.size() ? std::optional<std::uint32_t>(order[position + 1]) : std::nullopt;
        if (instructions.empty() || !next)
            continue;
        const auto isJump = [&](const Instruction &instruction, const Op op) {
            return static_cast<Op>(instruction.opcode) == op;
        };
        if (isJump(instructions.back(), Op::Jmp) && instructions.back().operands[0].target == *next) {
            instructions.pop_back();
            continue;
        }
        if (instructions.size() >= 2 && isJump(instructions.back(), Op::Jmp) &&
            isJump(instructions[instructions.size() - 2], Op::Jcc) &&
            instructions[instructions.size() - 2].operands[0].target == *next) {
            auto &conditional = instructions[instructions.size() - 2];
            conditional.condition = static_cast<std::uint8_t>(Invert(static_cast<Cond>(conditional.condition)));
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
        Cond condition = Cond::Equal;
        std::uint32_t target = 0;
        bool longForm = false;
        /// Bytes of block body that precede this branch.
        std::uint32_t bodyOffset = 0;
    };

    std::vector<Branch> branches;
    std::uint32_t offset = 0;
    std::uint32_t padding = 0;
    bool aligned = false;
};
} // namespace

std::optional<EmittedFunction> EmitFunction(SelectedFunction &selected, const Target::OS targetOs,
                                            const bool printAssembly) {
    auto &function = selected.function;
    EliminateDeadCode(function);
    RotateLoops(function);
    FuseFlagBranches(function);
    EliminateDeadCode(function);
    const SpillCode spills;
    const auto allocation = Machine::AllocateRegisters(function, TargetRegistersFor(selected.win64), spills);
    if (!allocation.succeeded)
        return std::nullopt;
    const Frame frame = LayoutFrame(selected, allocation.usedCalleeSaved);
    const auto order = LayoutBlocks(function);
    SimplifyBranches(function, order);

    std::vector<std::uint32_t> position(function.blocks.size(), 0);
    for (std::size_t index = 0; index < order.size(); ++index)
        position[order[index]] = static_cast<std::uint32_t>(index);
    // A loop block that some later block branches back to starts on a sixteen-byte boundary.
    std::vector<bool> backwardTarget(function.blocks.size(), false);
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block)
        for (const auto successor : function.blocks[block].successors)
            if (position[block] >= position[successor] && function.blocks[successor].loopDepth > 0)
                backwardTarget[successor] = true;

    std::vector<BlockCode> code(order.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto &block = function.blocks[order[index]];
        auto &blockCode = code[index];
        MachineEncoder encoder(blockCode.bytes, blockCode.symbolUses, frame.objectOffsets, selected, targetOs);
        if (index == 0) {
            encoder.Push(Rbp);
            encoder.MovRbpRsp();
            for (const auto reg : frame.savedGprs)
                encoder.Push(reg);
            std::int32_t remaining = frame.allocation;
            while (remaining > 4096) {
                encoder.SubRsp(4096);
                encoder.ProbeRsp();
                remaining -= 4096;
            }
            if (remaining > 0) {
                encoder.SubRsp(remaining);
                if (remaining == 4096)
                    encoder.ProbeRsp();
            }
            for (const auto &[reg, offset] : frame.savedVectors)
                encoder.SaveVector(reg, -offset);
        }
        blockCode.aligned = index != 0 && backwardTarget[order[index]];
        for (const auto &instruction : block.instructions) {
            const auto op = static_cast<Op>(instruction.opcode);
            if (op == Op::Jmp || op == Op::Jcc) {
                BlockCode::Branch branch;
                branch.conditional = op == Op::Jcc;
                branch.condition = static_cast<Cond>(instruction.condition);
                branch.target = position[instruction.operands[0].target];
                branch.bodyOffset = static_cast<std::uint32_t>(blockCode.bytes.size());
                blockCode.branches.push_back(branch);
                continue;
            }
            if (op == Op::Ret) {
                for (const auto &[reg, offset] : frame.savedVectors)
                    encoder.RestoreVector(reg, -offset);
                if (frame.allocation > 0)
                    encoder.LeaRspFromRbp(-static_cast<std::int32_t>(frame.savedGprs.size() * 8));
                for (auto reg = frame.savedGprs.rbegin(); reg != frame.savedGprs.rend(); ++reg)
                    encoder.Pop(*reg);
                encoder.Pop(Rbp);
                encoder.Ret();
                continue;
            }
            encoder.Encode(instruction);
        }
    }

    // Branch relaxation: every branch starts short and grows until its displacement fits. Branches only grow, so the
    // iteration ends.
    const auto layout = [&]() {
        std::uint32_t offset = 0;
        for (auto &blockCode : code) {
            blockCode.padding = 0;
            if (blockCode.aligned)
                blockCode.padding = (16 - offset % 16) % 16;
            offset += blockCode.padding;
            blockCode.offset = offset;
            offset += static_cast<std::uint32_t>(blockCode.bytes.size());
            for (const auto &branch : blockCode.branches)
                offset += static_cast<std::uint32_t>(branch.conditional ? MachineEncoder::JccSize(!branch.longForm)
                                                                        : MachineEncoder::JmpSize(!branch.longForm));
        }
        return offset;
    };
    const auto branchEnd = [&](const BlockCode &blockCode, const std::size_t branchIndex) {
        // Branches only ever sit after every other instruction of their block, but several may follow each other.
        std::uint32_t end = blockCode.offset + static_cast<std::uint32_t>(blockCode.bytes.size());
        for (std::size_t index = 0; index <= branchIndex; ++index) {
            const auto &branch = blockCode.branches[index];
            end += static_cast<std::uint32_t>(branch.conditional ? MachineEncoder::JccSize(!branch.longForm)
                                                                 : MachineEncoder::JmpSize(!branch.longForm));
        }
        return end;
    };
    for (bool grew = true; grew;) {
        grew = false;
        layout();
        for (auto &blockCode : code) {
            for (std::size_t index = 0; index < blockCode.branches.size(); ++index) {
                auto &branch = blockCode.branches[index];
                if (branch.longForm)
                    continue;
                const std::int64_t displacement =
                    static_cast<std::int64_t>(code[branch.target].offset) - branchEnd(blockCode, index);
                if (displacement < std::numeric_limits<std::int8_t>::min() ||
                    displacement > std::numeric_limits<std::int8_t>::max()) {
                    branch.longForm = true;
                    grew = true;
                }
            }
        }
    }
    layout();

    EmittedFunction emitted;
    std::vector<SymbolUse> unused;
    const std::vector<std::int32_t> noFrame;
    for (auto &blockCode : code) {
        // Multi-byte no-ops keep alignment padding to a few instructions.
        static constexpr std::uint8_t nops[][9] = {
            {},
            {0x90},
            {0x66, 0x90},
            {0x0F, 0x1F, 0x00},
            {0x0F, 0x1F, 0x40, 0x00},
            {0x0F, 0x1F, 0x44, 0x00, 0x00},
            {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
            {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
            {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
            {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        };
        for (std::uint32_t remaining = blockCode.padding; remaining > 0;) {
            const std::uint32_t chunk = std::min<std::uint32_t>(remaining, 9);
            emitted.bytes.insert(emitted.bytes.end(), nops[chunk], nops[chunk] + chunk);
            remaining -= chunk;
        }
        const auto base = static_cast<std::uint32_t>(emitted.bytes.size());
        emitted.bytes.insert(emitted.bytes.end(), blockCode.bytes.begin(), blockCode.bytes.end());
        for (auto use : blockCode.symbolUses) {
            use.offset += base;
            emitted.symbolUses.push_back(use);
        }
        MachineEncoder encoder(emitted.bytes, unused, noFrame, selected, targetOs);
        for (std::size_t index = 0; index < blockCode.branches.size(); ++index) {
            const auto &branch = blockCode.branches[index];
            const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(code[branch.target].offset) -
                                                                branchEnd(blockCode, index));
            if (branch.conditional)
                encoder.Jcc(branch.condition, displacement, !branch.longForm);
            else
                encoder.Jmp(displacement, !branch.longForm);
        }
    }
    if (printAssembly)
        emitted.assembly =
            PrintFunction(selected, order, frame.savedGprs, frame.savedVectors, frame.objectOffsets, frame.allocation);
    return emitted;
}
} // namespace Rux::X86_64Machine
