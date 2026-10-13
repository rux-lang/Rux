#include "Optimization/LirDeadCodeElimination.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Rux::Optimization {
namespace {
enum class InstructionEffect {
    Pure,
    MayTrap,
    ReadsMemory,
    WritesMemory,
    Observable,
};

/// What an opcode does besides producing its result. This is the whole safety argument for the pass: only a `Pure`
/// instruction may be removed for having no readers, because every other category is worth running for its effect.
InstructionEffect EffectOf(const LirOpcode opcode) {
    switch (opcode) {
    case LirOpcode::Const:
    case LirOpcode::Alloca:
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
    case LirOpcode::RotateRight:
    case LirOpcode::Neg:
    case LirOpcode::Not:
    case LirOpcode::BitNot:
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
    case LirOpcode::Cast:
    case LirOpcode::FieldPtr:
    case LirOpcode::IndexPtr:
    case LirOpcode::Phi:
    case LirOpcode::GlobalAddr:
    case LirOpcode::ThreadLocalAddr:
    case LirOpcode::StringAddr:
        return InstructionEffect::Pure;
    case LirOpcode::Div:
    case LirOpcode::Mod:
    case LirOpcode::Sqrt:
        return InstructionEffect::MayTrap;
    case LirOpcode::Load:
        return InstructionEffect::ReadsMemory;
    case LirOpcode::Store:
        return InstructionEffect::WritesMemory;
    case LirOpcode::Call:
    case LirOpcode::CallIndirect:
    case LirOpcode::Assert:
    case LirOpcode::Panic:
        return InstructionEffect::Observable;
    // Another thread can see an atomic operation and a fence, and an atomic load orders the memory around it even
    // when nothing reads its result, so none of them is ever removed.
    case LirOpcode::AtomicLoad:
    case LirOpcode::AtomicStore:
    case LirOpcode::AtomicExchange:
    case LirOpcode::AtomicCmpXchg:
    case LirOpcode::AtomicFetchAdd:
    case LirOpcode::AtomicFetchSub:
    case LirOpcode::AtomicFetchAnd:
    case LirOpcode::AtomicFetchOr:
    case LirOpcode::AtomicFetchXor:
    case LirOpcode::Fence:
    case LirOpcode::SpinHint:
    case LirOpcode::WriteStandardError:
        return InstructionEffect::Observable;
    // The frame pointer changes only on entry, so reading it twice reads the same value.
    case LirOpcode::FramePointer:
        return InstructionEffect::Pure;
    case LirOpcode::VectorSplat:
    case LirOpcode::VectorAdd:
    case LirOpcode::VectorSub:
    case LirOpcode::VectorMul:
    case LirOpcode::VectorAnd:
    case LirOpcode::VectorOr:
    case LirOpcode::VectorXor:
    case LirOpcode::VectorShl:
    case LirOpcode::VectorShr:
    case LirOpcode::VectorRotateRight:
        return InstructionEffect::Pure;
    case LirOpcode::VectorDiv:
        return InstructionEffect::MayTrap;
    // Vector memory accesses are produced last, by the unroller, and kept as they are: no store analysis here
    // reasons about two elements at once.
    case LirOpcode::VectorLoad:
    case LirOpcode::VectorStore:
        return InstructionEffect::Observable;
    }
    return InstructionEffect::Observable;
}

/// One past the highest register `function` names, so per-register facts fit in flat vectors.
std::size_t RegisterBound(const LirFunc &function) {
    std::size_t bound = 0;
    const auto note = [&](const LirReg reg) {
        if (reg != LirNoReg)
            bound = std::max(bound, static_cast<std::size_t>(reg) + 1);
    };
    for (const auto &parameter : function.params)
        note(parameter.reg);
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            note(instruction.dst);
            for (const LirReg source : instruction.srcs)
                note(source);
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                note(source);
            }
        }
        if (block.term) {
            note(block.term->cond);
            if (block.term->retVal)
                note(*block.term->retVal);
        }
    }
    return bound;
}

/// The stack slots this function allocates itself, as a dense slot number per register (-1 for any other register).
///
/// A store is only a candidate for removal when its destination is one of these: the function owns the storage, so
/// nothing outside it can observe the write. A store through a pointer that arrived from elsewhere always stays.
std::vector<std::int32_t> FindLocalAllocas(const LirFunc &function, const std::size_t registerBound,
                                           std::size_t &slotCount) {
    std::vector<char> allocated(registerBound, 0);
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.op == LirOpcode::Alloca && instruction.dst != LirNoReg)
                allocated[instruction.dst] = 1;

    std::vector<char> escaped(registerBound, 0);
    const auto isAllocated = [&](const LirReg reg) { return reg != LirNoReg && allocated[reg] != 0; };
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const LirReg source = instruction.srcs[index];
                if (!isAllocated(source))
                    continue;
                const bool directLoad = instruction.op == LirOpcode::Load && index == 0;
                const bool directStore = instruction.op == LirOpcode::Store && index == 1;
                if (!directLoad && !directStore)
                    escaped[source] = 1;
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                if (isAllocated(source))
                    escaped[source] = 1;
            }
        }
        if (block.term) {
            if ((block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) &&
                isAllocated(block.term->cond))
                escaped[block.term->cond] = 1;
            if (block.term->retVal && isAllocated(*block.term->retVal))
                escaped[*block.term->retVal] = 1;
        }
    }
    std::vector<std::int32_t> slots(registerBound, -1);
    slotCount = 0;
    for (std::size_t reg = 0; reg < registerBound; ++reg)
        if (allocated[reg] != 0 && escaped[reg] == 0)
            slots[reg] = static_cast<std::int32_t>(slotCount++);
    return slots;
}

std::vector<std::uint32_t> Successors(const LirBlock &block, const std::size_t blockCount) {
    std::vector<std::uint32_t> successors;
    if (!block.term) {
        return successors;
    }
    const auto append = [&](const std::uint32_t target) {
        if (target < blockCount && std::ranges::find(successors, target) == successors.end()) {
            successors.push_back(target);
        }
    };
    switch (block.term->kind) {
    case LirTermKind::Jump:
        append(block.term->trueTarget);
        break;
    case LirTermKind::Branch:
        append(block.term->trueTarget);
        append(block.term->falseTarget);
        break;
    case LirTermKind::Switch:
        append(block.term->defaultTarget);
        for (const auto &switchCase : block.term->cases) {
            append(switchCase.target);
        }
        break;
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        break;
    }
    return successors;
}

/// A set of local slots, one bit each.
class SlotSet {
public:
    explicit SlotSet(const std::size_t slots)
        : words((slots + 63) / 64, 0) {
    }

    void Insert(const std::size_t slot) {
        words[slot / 64] |= std::uint64_t{1} << (slot % 64);
    }

    void Erase(const std::size_t slot) {
        words[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
    }

    [[nodiscard]] bool Contains(const std::size_t slot) const {
        return ((words[slot / 64] >> (slot % 64)) & 1U) != 0;
    }

    void Union(const SlotSet &other) {
        for (std::size_t word = 0; word < words.size(); ++word)
            words[word] |= other.words[word];
    }

    bool operator==(const SlotSet &) const = default;

private:
    std::vector<std::uint64_t> words;
};

/// The local slot `instruction` loads directly, or -1.
std::int32_t LoadedSlot(const LirInstr &instruction, const std::vector<std::int32_t> &slots) {
    return instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && instruction.srcs[0] != LirNoReg
             ? slots[instruction.srcs[0]]
             : -1;
}

/// The local slot `instruction` stores to directly, or -1.
std::int32_t StoredSlot(const LirInstr &instruction, const std::vector<std::int32_t> &slots) {
    return instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 && instruction.srcs[1] != LirNoReg
             ? slots[instruction.srcs[1]]
             : -1;
}

/// Walk one block backwards, updating which slots hold a value that is still read later. Backwards is the natural
/// direction because liveness is about the future: a slot becomes live at a load and dies at the store above it.
void TransferStorageLiveness(const LirBlock &block, const std::vector<std::int32_t> &slots, SlotSet &live) {
    for (auto instruction = block.instrs.rbegin(); instruction != block.instrs.rend(); ++instruction) {
        if (const auto loaded = LoadedSlot(*instruction, slots); loaded >= 0)
            live.Insert(static_cast<std::size_t>(loaded));
        else if (const auto stored = StoredSlot(*instruction, slots); stored >= 0)
            live.Erase(static_cast<std::size_t>(stored));
    }
}

/// Stores whose value no later load can read, one flag per instruction of each block, found by iterating liveness
/// across the control-flow graph until it settles. The iteration is bounded so a malformed graph cannot spin here.
std::vector<std::vector<char>> FindDeadStores(const LirFunc &function, const std::vector<std::int32_t> &slots,
                                              const std::size_t slotCount) {
    const std::size_t blockCount = function.blocks.size();
    std::vector<std::vector<char>> deadStores(blockCount);
    for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        deadStores[blockIndex].assign(function.blocks[blockIndex].instrs.size(), 0);
    if (slotCount == 0)
        return deadStores;
    std::vector<std::vector<std::uint32_t>> successors(blockCount);
    for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        successors[blockIndex] = Successors(function.blocks[blockIndex], blockCount);
    std::vector<SlotSet> liveIn(blockCount, SlotSet(slotCount));
    std::vector<SlotSet> liveOut(blockCount, SlotSet(slotCount));
    const std::size_t limit = std::max<std::size_t>(1, blockCount * slotCount + 1);
    for (std::size_t iteration = 0; iteration < limit; ++iteration) {
        bool changed = false;
        for (std::size_t blockIndex = blockCount; blockIndex-- > 0;) {
            SlotSet outgoing(slotCount);
            for (const std::uint32_t successor : successors[blockIndex])
                outgoing.Union(liveIn[successor]);
            SlotSet incoming = outgoing;
            TransferStorageLiveness(function.blocks[blockIndex], slots, incoming);
            if (outgoing != liveOut[blockIndex] || incoming != liveIn[blockIndex]) {
                liveOut[blockIndex] = std::move(outgoing);
                liveIn[blockIndex] = std::move(incoming);
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        SlotSet live = liveOut[blockIndex];
        const auto &instructions = function.blocks[blockIndex].instrs;
        for (std::size_t position = instructions.size(); position-- > 0;) {
            const auto &instruction = instructions[position];
            if (const auto loaded = LoadedSlot(instruction, slots); loaded >= 0) {
                live.Insert(static_cast<std::size_t>(loaded));
            }
            else if (const auto stored = StoredSlot(instruction, slots); stored >= 0) {
                // A volatile store is required to happen, so no later read decides whether it survives.
                if (!live.Contains(static_cast<std::size_t>(stored)) && !instruction.isVolatile)
                    deadStores[blockIndex][position] = 1;
                live.Erase(static_cast<std::size_t>(stored));
            }
        }
    }
    return deadStores;
}

/// Whether the instruction must be kept regardless of who reads its result.
bool IsRequiredEffect(const LirInstr &instruction, const std::vector<std::int32_t> &slots, const bool deadStore) {
    if (deadStore) {
        return false;
    }
    const InstructionEffect effect = EffectOf(instruction.op);
    if (effect == InstructionEffect::Pure) {
        return false;
    }
    if (effect == InstructionEffect::ReadsMemory && instruction.srcs.size() == 1 && instruction.srcs[0] != LirNoReg &&
        slots[instruction.srcs[0]] >= 0) {
        return false;
    }
    return true;
}

bool EliminateFromFunction(LirFunc &function) {
    const std::size_t registerBound = RegisterBound(function);
    std::size_t slotCount = 0;
    const auto slots = FindLocalAllocas(function, registerBound, slotCount);
    const auto deadStores = FindDeadStores(function, slots, slotCount);

    // Instructions are numbered in block order; a register's definition is the last instruction that writes it.
    std::vector<std::size_t> blockStart(function.blocks.size() + 1, 0);
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex)
        blockStart[blockIndex + 1] = blockStart[blockIndex] + function.blocks[blockIndex].instrs.size();
    std::vector<const LirInstr *> instructionAt(blockStart.back());
    constexpr std::size_t kNoDefinition = ~std::size_t{0};
    std::vector<std::size_t> definitions(registerBound, kNoDefinition);
    std::vector<char> neededRegisters(registerBound, 0);
    std::vector<char> neededInstructions(blockStart.back(), 0);
    std::vector<LirReg> worklist;
    const auto needRegister = [&](const LirReg reg) {
        if (reg != LirNoReg && neededRegisters[reg] == 0) {
            neededRegisters[reg] = 1;
            worklist.push_back(reg);
        }
    };
    const auto needUses = [&](const LirInstr &instruction) {
        for (const LirReg source : instruction.srcs)
            needRegister(source);
        for (const auto &[source, predecessor] : instruction.phiPreds) {
            static_cast<void>(predecessor);
            needRegister(source);
        }
    };
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto &block = function.blocks[blockIndex];
        for (std::size_t position = 0; position < block.instrs.size(); ++position) {
            const auto &instruction = block.instrs[position];
            const std::size_t number = blockStart[blockIndex] + position;
            instructionAt[number] = &instruction;
            if (instruction.dst != LirNoReg) {
                definitions[instruction.dst] = number;
            }
            if (IsRequiredEffect(instruction, slots, deadStores[blockIndex][position] != 0)) {
                neededInstructions[number] = 1;
                needUses(instruction);
            }
        }
        if (block.term) {
            if (block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) {
                needRegister(block.term->cond);
            }
            if (block.term->retVal) {
                needRegister(*block.term->retVal);
            }
        }
    }

    while (!worklist.empty()) {
        const LirReg reg = worklist.back();
        worklist.pop_back();
        const std::size_t definition = definitions[reg];
        if (definition == kNoDefinition || neededInstructions[definition] != 0) {
            continue;
        }
        neededInstructions[definition] = 1;
        needUses(*instructionAt[definition]);
    }

    bool changed = false;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        auto &block = function.blocks[blockIndex];
        const std::size_t start = blockStart[blockIndex];
        const auto removed = [&](const std::size_t position) {
            return deadStores[blockIndex][position] != 0 || neededInstructions[start + position] == 0;
        };
        bool blockChanged = false;
        for (std::size_t position = 0; position < block.instrs.size() && !blockChanged; ++position)
            blockChanged = removed(position);
        if (!blockChanged)
            continue;
        changed = true;
        std::vector<LirInstr> retained;
        retained.reserve(block.instrs.size());
        for (std::size_t position = 0; position < block.instrs.size(); ++position)
            if (!removed(position))
                retained.push_back(std::move(block.instrs[position]));
        block.instrs = std::move(retained);
    }
    return changed;
}
} // namespace

std::string_view LirDeadCodeElimination::Name() const noexcept {
    return "lir-dead-code-elimination";
}

PassChange LirDeadCodeElimination::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (function.isExtern || function.isAsm)
        return PassChange::None;
    return EliminateFromFunction(function) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
