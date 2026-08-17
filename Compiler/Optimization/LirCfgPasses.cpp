#include "Optimization/LirCfgPasses.h"

#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
using BlockIndex = std::uint32_t;
using Predecessors = std::vector<std::vector<BlockIndex>>;

/// Name a block for a diagnostic, preferring its label so the message points at something a dump also shows.
std::string BlockContext(const LirFunc &function, const BlockIndex blockIndex) {
    if (blockIndex >= function.blocks.size()) {
        return std::format("function '{}', block {}", function.name, blockIndex);
    }
    return std::format("function '{}', block {} ('{}')", function.name, blockIndex, function.blocks[blockIndex].label);
}

void Report(const PassContext &context, const LirFunc &function, const BlockIndex blockIndex, std::string message) {
    context.ReportInternalError(
        std::format("internal LIR CFG error in {}: {}", BlockContext(function, blockIndex), std::move(message)));
}

/// The blocks a terminator can transfer to. One place computes this so the verifier, the reachability walk, and phi
/// maintenance can never disagree about the shape of the graph.
std::vector<BlockIndex> Successors(const LirTerminator &terminator) {
    std::vector<BlockIndex> result;
    const auto add = [&result](const BlockIndex target) {
        if (std::ranges::find(result, target) == result.end()) {
            result.push_back(target);
        }
    };

    switch (terminator.kind) {
    case LirTermKind::Jump:
        add(terminator.trueTarget);
        break;
    case LirTermKind::Branch:
        add(terminator.trueTarget);
        add(terminator.falseTarget);
        break;
    case LirTermKind::Switch:
        add(terminator.defaultTarget);
        for (const auto &switchCase : terminator.cases) {
            add(switchCase.target);
        }
        break;
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        break;
    }
    return result;
}

/// Check one function's graph, reporting every problem rather than the first, so a broken lowering is diagnosed in one
/// run.
///
/// @return false when the function is malformed
bool ValidateFunction(const LirFunc &function, const PassContext &context) {
    if (function.isExtern || function.isAsm) {
        return true;
    }
    if (function.blocks.empty()) {
        context.ReportInternalError(
            std::format("internal LIR CFG error in function '{}': definition has no entry block", function.name));
        return false;
    }

    bool valid = true;
    Predecessors predecessors(function.blocks.size());
    for (BlockIndex blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto &block = function.blocks[blockIndex];
        if (!block.term) {
            Report(context, function, blockIndex, "block has no terminator");
            valid = false;
            continue;
        }
        for (const BlockIndex target : Successors(*block.term)) {
            if (target >= function.blocks.size()) {
                Report(
                    context, function, blockIndex,
                    std::format("{} terminator targets invalid block {}", LirTermKindName(block.term->kind), target));
                valid = false;
                continue;
            }
            predecessors[target].push_back(blockIndex);
        }
    }

    for (BlockIndex blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto &actualPredecessors = predecessors[blockIndex];
        for (const auto &instruction : function.blocks[blockIndex].instrs) {
            if (instruction.op != LirOpcode::Phi) {
                continue;
            }
            std::unordered_set<BlockIndex> namedPredecessors;
            for (const auto &[value, predecessor] : instruction.phiPreds) {
                static_cast<void>(value);
                if (predecessor >= function.blocks.size()) {
                    Report(context, function, blockIndex,
                           std::format("phi names invalid predecessor block {}", predecessor));
                    valid = false;
                    continue;
                }
                if (std::ranges::find(actualPredecessors, predecessor) == actualPredecessors.end()) {
                    Report(context, function, blockIndex,
                           std::format("phi names block {}, which is not an actual predecessor", predecessor));
                    valid = false;
                }
                if (!namedPredecessors.insert(predecessor).second) {
                    Report(context, function, blockIndex,
                           std::format("phi names predecessor block {} more than once", predecessor));
                    valid = false;
                }
            }
            for (const BlockIndex predecessor : actualPredecessors) {
                if (!namedPredecessors.contains(predecessor)) {
                    Report(context, function, blockIndex,
                           std::format("phi has no value for predecessor block {}", predecessor));
                    valid = false;
                }
            }
        }
    }
    return valid;
}

/// The constant a register holds, if it was defined by a constant in this same block. Deliberately block-local: a
/// definition elsewhere might not dominate this use, and proving that it does is not this pass's job.
std::optional<TypedConstant> LocalConstant(const LirBlock &block, const LirReg reg) {
    for (auto instruction = block.instrs.rbegin(); instruction != block.instrs.rend(); ++instruction) {
        if (instruction->dst == reg) {
            if (instruction->op == LirOpcode::Const) {
                return ParseConstant(instruction->strArg, instruction->type);
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool SameConstant(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() == right.GetKind() && left.Width() == right.Width() && left.RawBits() == right.RawBits();
}

/// The single block a switch must reach when its subject is a known constant, falling through to the default when no
/// case matches.
///
/// @return nullopt when the subject is not known at compile time
std::optional<BlockIndex> KnownSwitchTarget(const LirBlock &block, const LirTerminator &terminator) {
    const auto condition = LocalConstant(block, terminator.cond);
    if (!condition) {
        return std::nullopt;
    }

    std::vector<TypedConstant> caseValues;
    caseValues.reserve(terminator.cases.size());
    for (const auto &switchCase : terminator.cases) {
        const auto value = ParseConstant(switchCase.value, terminator.retType);
        if (!value) {
            return std::nullopt;
        }
        caseValues.push_back(*value);
    }
    for (std::size_t index = 0; index < caseValues.size(); ++index) {
        if (SameConstant(*condition, caseValues[index])) {
            return terminator.cases[index].target;
        }
    }
    return terminator.defaultTarget;
}

/// Turn conditional terminators whose condition is known into unconditional jumps, which is what makes the branch not
/// taken unreachable for the pass below.
bool FoldTerminators(LirFunc &function) {
    bool changed = false;
    for (auto &block : function.blocks) {
        auto &terminator = *block.term;
        std::optional<BlockIndex> target;
        if (terminator.kind == LirTermKind::Branch) {
            if (const auto condition = LocalConstant(block, terminator.cond)) {
                if (const auto value = condition->BooleanValue()) {
                    target = *value ? terminator.trueTarget : terminator.falseTarget;
                }
            }
        }
        else if (terminator.kind == LirTermKind::Switch) {
            target = KnownSwitchTarget(block, terminator);
        }
        if (target) {
            LirTerminator jump;
            jump.kind = LirTermKind::Jump;
            jump.trueTarget = *target;
            terminator = std::move(jump);
            changed = true;
        }
    }
    return changed;
}

/// Mark the blocks reachable from entry. Reachability is by edge, not by position: a block is kept because something
/// branches to it, never because it is written between two blocks that are kept.
std::vector<bool> ReachableBlocks(const LirFunc &function) {
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<BlockIndex> pending = {0};
    while (!pending.empty()) {
        const BlockIndex blockIndex = pending.back();
        pending.pop_back();
        if (reachable[blockIndex]) {
            continue;
        }
        reachable[blockIndex] = true;
        for (const BlockIndex target : Successors(*function.blocks[blockIndex].term)) {
            if (!reachable[target]) {
                pending.push_back(target);
            }
        }
    }
    return reachable;
}

/// Drop unreachable blocks and renumber every edge that pointed past them, since block indices are positional.
bool RemoveUnreachableBlocks(LirFunc &function) {
    const std::vector<bool> reachable = ReachableBlocks(function);
    if (std::ranges::all_of(reachable, [](const bool value) { return value; })) {
        return false;
    }

    constexpr BlockIndex invalidBlock = ~BlockIndex{};
    std::vector<BlockIndex> remap(function.blocks.size(), invalidBlock);
    std::vector<LirBlock> retained;
    retained.reserve(std::ranges::count(reachable, true));
    for (BlockIndex oldIndex = 0; oldIndex < function.blocks.size(); ++oldIndex) {
        if (reachable[oldIndex]) {
            remap[oldIndex] = static_cast<BlockIndex>(retained.size());
            retained.push_back(std::move(function.blocks[oldIndex]));
        }
    }

    for (auto &block : retained) {
        auto &terminator = *block.term;
        switch (terminator.kind) {
        case LirTermKind::Jump:
            terminator.trueTarget = remap[terminator.trueTarget];
            break;
        case LirTermKind::Branch:
            terminator.trueTarget = remap[terminator.trueTarget];
            terminator.falseTarget = remap[terminator.falseTarget];
            break;
        case LirTermKind::Switch:
            terminator.defaultTarget = remap[terminator.defaultTarget];
            for (auto &switchCase : terminator.cases) {
                switchCase.target = remap[switchCase.target];
            }
            break;
        case LirTermKind::Return:
        case LirTermKind::Unreachable:
            break;
        }

        for (auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Phi) {
                continue;
            }
            std::erase_if(instruction.phiPreds,
                          [&remap](const auto &entry) { return remap[entry.second] == invalidBlock; });
            for (auto &[value, predecessor] : instruction.phiPreds) {
                static_cast<void>(value);
                predecessor = remap[predecessor];
            }
        }
    }
    function.blocks = std::move(retained);
    return true;
}

/// Drop phi operands naming a predecessor that no longer branches here. A phi that outlived its incoming edge would
/// otherwise claim a value from a block that cannot reach it.
void RemoveStalePhiPredecessors(LirFunc &function) {
    Predecessors predecessors(function.blocks.size());
    for (BlockIndex blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (const BlockIndex target : Successors(*function.blocks[blockIndex].term)) {
            predecessors[target].push_back(blockIndex);
        }
    }
    for (BlockIndex blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (auto &instruction : function.blocks[blockIndex].instrs) {
            if (instruction.op == LirOpcode::Phi) {
                std::erase_if(instruction.phiPreds, [&](const auto &entry) {
                    return std::ranges::find(predecessors[blockIndex], entry.second) == predecessors[blockIndex].end();
                });
            }
        }
    }
}
} // namespace

std::string_view LirCfgVerifier::Name() const noexcept {
    return "lir-cfg-verifier";
}

PassChange LirCfgVerifier::Run(LirPackage &package, const PassContext &context) {
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            ValidateFunction(function, context);
        }
    }
    return PassChange::None;
}

std::string_view LirCfgCleanup::Name() const noexcept {
    return "lir-cfg-cleanup";
}

PassChange LirCfgCleanup::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            if (function.isExtern || function.isAsm) {
                continue;
            }
            const bool folded = FoldTerminators(function);
            const bool removed = RemoveUnreachableBlocks(function);
            if (folded) {
                RemoveStalePhiPredecessors(function);
            }
            changed = folded || removed || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
