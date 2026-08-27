#include "Optimization/LirDeadCodeElimination.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::Optimization {
namespace {
using Registers = std::unordered_set<LirReg>;
using InstructionSet = std::unordered_set<const LirInstr *>;

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
    case LirOpcode::StringAddr:
        return InstructionEffect::Pure;
    case LirOpcode::Div:
    case LirOpcode::Mod:
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
    }
    return InstructionEffect::Observable;
}

void AddRegister(Registers &registers, const LirReg reg) {
    if (reg != LirNoReg) {
        registers.insert(reg);
    }
}

void AddUses(Registers &registers, const LirInstr &instruction) {
    for (const LirReg source : instruction.srcs) {
        AddRegister(registers, source);
    }
    for (const auto &[source, predecessor] : instruction.phiPreds) {
        static_cast<void>(predecessor);
        AddRegister(registers, source);
    }
}

/// The stack slots this function allocates itself.
///
/// A store is only a candidate for removal when its destination is one of these: the function owns the storage, so
/// nothing outside it can observe the write. A store through a pointer that arrived from elsewhere always stays.
Registers FindLocalAllocas(const LirFunc &function) {
    Registers localAllocas;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && instruction.dst != LirNoReg) {
                localAllocas.insert(instruction.dst);
            }
        }
    }

    Registers escaped;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const LirReg source = instruction.srcs[index];
                if (!localAllocas.contains(source)) {
                    continue;
                }
                const bool directLoad = instruction.op == LirOpcode::Load && index == 0;
                const bool directStore = instruction.op == LirOpcode::Store && index == 1;
                if (!directLoad && !directStore) {
                    escaped.insert(source);
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                if (localAllocas.contains(source)) {
                    escaped.insert(source);
                }
            }
        }
        if (block.term) {
            if ((block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) &&
                localAllocas.contains(block.term->cond)) {
                escaped.insert(block.term->cond);
            }
            if (block.term->retVal && localAllocas.contains(*block.term->retVal)) {
                escaped.insert(*block.term->retVal);
            }
        }
    }
    for (const LirReg reg : escaped) {
        localAllocas.erase(reg);
    }
    return localAllocas;
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

/// Walk one block backwards, updating which slots hold a value that is still read later. Backwards is the natural
/// direction because liveness is about the future: a slot becomes live at a load and dies at the store above it.
void TransferStorageLiveness(const LirBlock &block, const Registers &localAllocas, Registers &live) {
    for (auto instruction = block.instrs.rbegin(); instruction != block.instrs.rend(); ++instruction) {
        if (instruction->op == LirOpcode::Load && instruction->srcs.size() == 1 &&
            localAllocas.contains(instruction->srcs[0])) {
            live.insert(instruction->srcs[0]);
        }
        else if (instruction->op == LirOpcode::Store && instruction->srcs.size() == 2 &&
                 localAllocas.contains(instruction->srcs[1])) {
            live.erase(instruction->srcs[1]);
        }
    }
}

/// Stores whose value no later load can read, found by iterating liveness across the control-flow graph until it
/// settles. The iteration is bounded so a malformed graph cannot spin here; running out means nothing is reported dead,
/// which is the safe answer.
InstructionSet FindDeadStores(const LirFunc &function, const Registers &localAllocas) {
    const std::size_t blockCount = function.blocks.size();
    std::vector<Registers> liveIn(blockCount);
    std::vector<Registers> liveOut(blockCount);
    const std::size_t limit = std::max<std::size_t>(1, blockCount * std::max<std::size_t>(localAllocas.size(), 1) + 1);
    for (std::size_t iteration = 0; iteration < limit; ++iteration) {
        bool changed = false;
        for (std::size_t blockIndex = blockCount; blockIndex-- > 0;) {
            Registers outgoing;
            for (const std::uint32_t successor : Successors(function.blocks[blockIndex], blockCount)) {
                outgoing.insert(liveIn[successor].begin(), liveIn[successor].end());
            }
            Registers incoming = outgoing;
            TransferStorageLiveness(function.blocks[blockIndex], localAllocas, incoming);
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

    InstructionSet deadStores;
    for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        Registers live = liveOut[blockIndex];
        for (auto instruction = function.blocks[blockIndex].instrs.rbegin();
             instruction != function.blocks[blockIndex].instrs.rend(); ++instruction) {
            if (instruction->op == LirOpcode::Load && instruction->srcs.size() == 1 &&
                localAllocas.contains(instruction->srcs[0])) {
                live.insert(instruction->srcs[0]);
            }
            else if (instruction->op == LirOpcode::Store && instruction->srcs.size() == 2 &&
                     localAllocas.contains(instruction->srcs[1])) {
                // A volatile store is required to happen, so no later read decides whether it survives.
                if (!live.contains(instruction->srcs[1]) && !instruction->isVolatile) {
                    deadStores.insert(&*instruction);
                }
                live.erase(instruction->srcs[1]);
            }
        }
    }
    return deadStores;
}

/// Whether the instruction must be kept regardless of who reads its result.
bool IsRequiredEffect(const LirInstr &instruction, const Registers &localAllocas, const InstructionSet &deadStores) {
    if (deadStores.contains(&instruction)) {
        return false;
    }
    const InstructionEffect effect = EffectOf(instruction.op);
    if (effect == InstructionEffect::Pure) {
        return false;
    }
    if (effect == InstructionEffect::ReadsMemory && instruction.srcs.size() == 1 &&
        localAllocas.contains(instruction.srcs[0])) {
        return false;
    }
    return true;
}

bool EliminateFromFunction(LirFunc &function) {
    const Registers localAllocas = FindLocalAllocas(function);
    const InstructionSet deadStores = FindDeadStores(function, localAllocas);

    std::unordered_map<LirReg, const LirInstr *> definitions;
    Registers neededRegisters;
    InstructionSet neededInstructions;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                definitions.insert_or_assign(instruction.dst, &instruction);
            }
            if (IsRequiredEffect(instruction, localAllocas, deadStores)) {
                neededInstructions.insert(&instruction);
                AddUses(neededRegisters, instruction);
            }
        }
        if (block.term) {
            if (block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) {
                AddRegister(neededRegisters, block.term->cond);
            }
            if (block.term->retVal) {
                AddRegister(neededRegisters, *block.term->retVal);
            }
        }
    }

    std::vector<LirReg> worklist(neededRegisters.begin(), neededRegisters.end());
    while (!worklist.empty()) {
        const LirReg reg = worklist.back();
        worklist.pop_back();
        const auto definition = definitions.find(reg);
        if (definition == definitions.end() || !neededInstructions.insert(definition->second).second) {
            continue;
        }
        Registers sources;
        AddUses(sources, *definition->second);
        for (const LirReg source : sources) {
            if (neededRegisters.insert(source).second) {
                worklist.push_back(source);
            }
        }
    }

    bool changed = false;
    for (auto &block : function.blocks) {
        std::vector<LirInstr> retained;
        retained.reserve(block.instrs.size());
        for (auto &instruction : block.instrs) {
            if (deadStores.contains(&instruction) || !neededInstructions.contains(&instruction)) {
                changed = true;
            }
            else {
                retained.push_back(std::move(instruction));
            }
        }
        block.instrs = std::move(retained);
    }
    return changed;
}
} // namespace

std::string_view LirDeadCodeElimination::Name() const noexcept {
    return "lir-dead-code-elimination";
}

PassChange LirDeadCodeElimination::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            if (!function.isExtern && !function.isAsm) {
                changed = EliminateFromFunction(function) || changed;
            }
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
