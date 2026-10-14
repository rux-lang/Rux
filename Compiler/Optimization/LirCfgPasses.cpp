#include "Optimization/LirCfgPasses.h"

#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <unordered_map>
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

/// Distinct successor blocks in the order a terminator names them. Jumps and branches, nearly every terminator, fit
/// inline; only a switch with more distinct targets spills to the heap.
class SuccessorList {
public:
    void Add(const BlockIndex target) {
        if (std::find(begin(), end(), target) != end())
            return;
        if (spill.empty() && count < held.size()) {
            held[count++] = target;
            return;
        }
        if (spill.empty())
            spill.assign(held.begin(), held.end());
        spill.push_back(target);
        ++count;
    }

    [[nodiscard]] const BlockIndex *begin() const {
        return spill.empty() ? held.data() : spill.data();
    }

    [[nodiscard]] const BlockIndex *end() const {
        return begin() + count;
    }

private:
    std::array<BlockIndex, 2> held{};
    std::vector<BlockIndex> spill;
    std::size_t count = 0;
};

/// The blocks a terminator can transfer to. One place computes this so the verifier, the reachability walk, and phi
/// maintenance can never disagree about the shape of the graph.
SuccessorList Successors(const LirTerminator &terminator) {
    SuccessorList result;
    const auto add = [&result](const BlockIndex target) { result.Add(target); };

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
    // The phi each predecessor was last named by, stamped so one table serves every phi of the function.
    std::vector<std::size_t> namedBy(function.blocks.size(), 0);
    std::size_t phiStamp = 0;
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
            if (instruction.op == LirOpcode::RotateRight) {
                unsigned amount = 64;
                const auto parsed = std::from_chars(instruction.strArg.data(),
                                                    instruction.strArg.data() + instruction.strArg.size(), amount);
                const bool word =
                    instruction.type.kind == TypeRef::Kind::UInt32 || instruction.type.kind == TypeRef::Kind::UInt64;
                const unsigned width = instruction.type.kind == TypeRef::Kind::UInt32 ? 32 : 64;
                if (!word || instruction.srcs.size() != 1 || instruction.dst == LirNoReg || parsed.ec != std::errc{} ||
                    parsed.ptr != instruction.strArg.data() + instruction.strArg.size() || amount >= width) {
                    Report(context, function, blockIndex,
                           "rotr requires uint32/uint64, one value and a count below its width");
                    valid = false;
                }
            }
            if (instruction.op != LirOpcode::Phi) {
                continue;
            }
            ++phiStamp;
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
                if (namedBy[predecessor] == phiStamp) {
                    Report(context, function, blockIndex,
                           std::format("phi names predecessor block {} more than once", predecessor));
                    valid = false;
                }
                namedBy[predecessor] = phiStamp;
            }
            for (const BlockIndex predecessor : actualPredecessors) {
                if (namedBy[predecessor] != phiStamp) {
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
/// The constants a function defines, by register. Each register has one definition in SSA form, so a constant
/// condition is known wherever it is read, not only in the block that defines it.
/// Only the registers a branch or a switch decides on are ever asked for, so only those are collected, sorted by
/// register for lookup.
using Constants = std::vector<std::pair<LirReg, const LirInstr *>>;

Constants FunctionConstants(const LirFunc &function) {
    std::vector<LirReg> conditions;
    for (const auto &block : function.blocks)
        if (block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch)
            conditions.push_back(block.term->cond);
    Constants constants;
    if (conditions.empty())
        return constants;
    std::ranges::sort(conditions);
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.op == LirOpcode::Const && instruction.dst != LirNoReg && !instruction.isVolatile &&
                std::ranges::binary_search(conditions, instruction.dst))
                constants.emplace_back(instruction.dst, &instruction);
    // The first definition of a register wins, as it would in a map filled in order.
    std::ranges::stable_sort(constants, {}, &Constants::value_type::first);
    const auto duplicates = std::ranges::unique(constants, {}, &Constants::value_type::first);
    constants.erase(duplicates.begin(), duplicates.end());
    return constants;
}

std::optional<TypedConstant> KnownConstant(const Constants &constants, const LirReg reg) {
    const auto found = std::ranges::lower_bound(constants, reg, {}, &Constants::value_type::first);
    if (found == constants.end() || found->first != reg)
        return std::nullopt;
    return ParseConstant(found->second->strArg, found->second->type);
}

bool SameConstant(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() == right.GetKind() && left.Width() == right.Width() && left.Bits() == right.Bits();
}

/// The single block a switch must reach when its subject is a known constant, falling through to the default when no
/// case matches.
///
/// @return nullopt when the subject is not known at compile time
std::optional<BlockIndex> KnownSwitchTarget(const Constants &constants, const LirTerminator &terminator) {
    const auto condition = KnownConstant(constants, terminator.cond);
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
    const auto constants = FunctionConstants(function);
    for (auto &block : function.blocks) {
        auto &terminator = *block.term;
        std::optional<BlockIndex> target;
        if (terminator.kind == LirTermKind::Branch) {
            if (const auto condition = KnownConstant(constants, terminator.cond)) {
                if (const auto value = condition->BooleanValue()) {
                    target = *value ? terminator.trueTarget : terminator.falseTarget;
                }
            }
        }
        else if (terminator.kind == LirTermKind::Switch) {
            target = KnownSwitchTarget(constants, terminator);
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

/// Sends a predecessor straight to the successor its constant decides when a block does nothing but branch on a phi.
/// Short-circuit `&&` and `||` build exactly that block: the constant `false` of a short circuit flows into a phi
/// whose only purpose is the branch, and the predecessor supplying it always takes one side. The block keeps its
/// other predecessors; it disappears when none is left.
bool ThreadConstantBranches(LirFunc &function) {
    if (function.blocks.size() < 3 || function.blocks.size() > 4096)
        return false;
    // Only a block of nothing but phis ending in a two-way branch can be threaded; most functions have none, and need
    // no use counts.
    const bool candidate = std::any_of(function.blocks.begin() + 1, function.blocks.end(), [](const LirBlock &block) {
        return block.term->kind == LirTermKind::Branch && !block.instrs.empty() &&
               std::ranges::all_of(block.instrs,
                                   [](const LirInstr &instruction) { return instruction.op == LirOpcode::Phi; });
    });
    if (!candidate)
        return false;
    std::unordered_map<LirReg, std::size_t> uses;
    std::unordered_map<LirReg, std::optional<bool>> constants;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (const auto source : instruction.srcs)
                ++uses[source];
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                ++uses[source];
            }
            if (instruction.op == LirOpcode::Const && instruction.dst != LirNoReg && instruction.type.IsBool())
                if (const auto value = ParseConstant(instruction.strArg, instruction.type))
                    constants[instruction.dst] = value->BooleanValue();
        }
        ++uses[block.term->cond];
        if (block.term->retVal)
            ++uses[*block.term->retVal];
    }
    bool changed = false;
    for (BlockIndex block = 1; block < function.blocks.size(); ++block) {
        auto &current = function.blocks[block];
        if (current.term->kind != LirTermKind::Branch || current.term->trueTarget == current.term->falseTarget ||
            current.term->trueTarget == block || current.term->falseTarget == block)
            continue;
        // Nothing but phis, each read by nothing but this branch: skipping the block skips no work and no value.
        const LirInstr *condition = nullptr;
        bool simple = !current.instrs.empty();
        for (const auto &instruction : current.instrs) {
            if (instruction.op != LirOpcode::Phi) {
                simple = false;
                break;
            }
            const std::size_t expected = instruction.dst == current.term->cond ? 1 : 0;
            if (uses[instruction.dst] != expected) {
                simple = false;
                break;
            }
            if (expected == 1)
                condition = &instruction;
        }
        if (!simple || condition == nullptr)
            continue;
        Predecessors predecessors(function.blocks.size());
        for (BlockIndex index = 0; index < function.blocks.size(); ++index)
            for (const auto target : Successors(*function.blocks[index].term))
                predecessors[target].push_back(index);
        for (const auto &[value, predecessor] : std::vector(condition->phiPreds)) {
            const auto known = constants.find(value);
            if (known == constants.end() || !known->second || predecessor == block)
                continue;
            const BlockIndex target = *known->second ? current.term->trueTarget : current.term->falseTarget;
            if (std::ranges::find(predecessors[target], predecessor) != predecessors[target].end())
                continue;
            // The target's phis receive from the predecessor what they received from this block.
            bool movable = true;
            for (const auto &instruction : function.blocks[target].instrs)
                if (instruction.op == LirOpcode::Phi)
                    for (const auto &[incoming, from] : instruction.phiPreds)
                        if (from == block && std::ranges::any_of(current.instrs, [&](const LirInstr &phi) {
                                return phi.dst == incoming;
                            }))
                            movable = false;
            if (!movable)
                continue;
            for (auto &instruction : function.blocks[target].instrs) {
                if (instruction.op != LirOpcode::Phi)
                    continue;
                const auto found =
                    std::ranges::find_if(instruction.phiPreds, [&](const auto &edge) { return edge.second == block; });
                if (found != instruction.phiPreds.end())
                    instruction.phiPreds.emplace_back(found->first, predecessor);
            }
            auto &term = *function.blocks[predecessor].term;
            const auto redirect = [&](BlockIndex &edge) {
                if (edge == block)
                    edge = target;
            };
            switch (term.kind) {
            case LirTermKind::Jump:
                redirect(term.trueTarget);
                break;
            case LirTermKind::Branch:
                redirect(term.trueTarget);
                redirect(term.falseTarget);
                break;
            case LirTermKind::Switch:
                redirect(term.defaultTarget);
                for (auto &switchCase : term.cases)
                    redirect(switchCase.target);
                break;
            default:
                break;
            }
            for (auto &instruction : current.instrs)
                if (instruction.op == LirOpcode::Phi)
                    std::erase_if(instruction.phiPreds, [&](const auto &edge) { return edge.second == predecessor; });
            predecessors[target].push_back(predecessor);
            changed = true;
        }
    }
    return changed;
}

bool CoalesceBlocks(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 2 || function.blocks.size() > 4096)
        return false;
    const auto reachable = ReachableBlocks(function);
    Predecessors predecessors(function.blocks.size());
    for (BlockIndex block = 0; block < function.blocks.size(); ++block)
        for (const auto target : Successors(*function.blocks[block].term))
            predecessors[target].push_back(block);
    std::vector<bool> removed(function.blocks.size(), false);
    std::unordered_map<LirReg, LirReg> copies;
    bool changed = false;
    std::size_t budget = 200000;
    for (BlockIndex block = 0; block < function.blocks.size(); ++block) {
        if (!reachable[block] || removed[block])
            continue;
        auto &current = function.blocks[block];
        while (current.term->kind == LirTermKind::Jump) {
            const auto target = current.term->trueTarget;
            if (target == 0 || target == block || removed[target] || predecessors[target].size() != 1 ||
                predecessors[target][0] != block)
                break;
            auto &successor = function.blocks[target];
            const auto outgoing = Successors(*successor.term);
            std::size_t cost = 1 + successor.instrs.size();
            for (const auto next : outgoing)
                cost += function.blocks[next].instrs.size();
            if (cost > budget)
                break;
            bool validPhis = true;
            for (const auto &instruction : successor.instrs)
                if (instruction.op == LirOpcode::Phi &&
                    (instruction.phiPreds.size() != 1 || instruction.phiPreds[0].second != block ||
                     instruction.phiPreds[0].first == instruction.dst))
                    validPhis = false;
            if (!validPhis)
                break;
            budget -= cost;
            // A phi on this unique edge is a copy. Preserve aggregate/reference identity through SSA renaming,
            // rather than inventing a cast or memory operation with different representation semantics.
            for (auto &instruction : successor.instrs) {
                if (instruction.op == LirOpcode::Phi)
                    copies.emplace(instruction.dst, instruction.phiPreds[0].first);
                else
                    current.instrs.push_back(std::move(instruction));
            }
            current.term = std::move(successor.term);
            successor.instrs.clear();
            successor.term = LirTerminator{};
            successor.term->kind = LirTermKind::Unreachable;
            removed[target] = true;
            predecessors[target].clear();
            for (const auto next : outgoing) {
                for (auto &predecessor : predecessors[next])
                    if (predecessor == target)
                        predecessor = block;
                for (auto &instruction : function.blocks[next].instrs)
                    if (instruction.op == LirOpcode::Phi)
                        for (auto &[value, predecessor] : instruction.phiPreds) {
                            static_cast<void>(value);
                            if (predecessor == target)
                                predecessor = block;
                        }
            }
            changed = true;
        }
    }
    if (!changed)
        return false;
    const auto resolve = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < copies.size(); ++depth) {
            const auto found = copies.find(reg);
            if (found == copies.end())
                break;
            reg = found->second;
        }
        return reg;
    };
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            for (auto &source : instruction.srcs)
                source = resolve(source);
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                source = resolve(source);
            }
        }
        block.term->cond = resolve(block.term->cond);
        if (block.term->retVal)
            block.term->retVal = resolve(*block.term->retVal);
    }
    static_cast<void>(RemoveUnreachableBlocks(function));
    return true;
}
} // namespace

std::string_view LirCfgVerifier::Name() const noexcept {
    return "lir-cfg-verifier";
}

PassChange LirCfgVerifier::RunFunction(LirFunc &function, const std::size_t, const PassContext &context) const {
    ValidateFunction(function, context);
    return PassChange::None;
}

std::string_view LirCfgCleanup::Name() const noexcept {
    return "lir-cfg-cleanup";
}

PassChange LirCfgCleanup::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (function.isExtern || function.isAsm) {
        return PassChange::None;
    }
    const bool folded = FoldTerminators(function);
    const bool threaded = ThreadConstantBranches(function);
    const bool removed = RemoveUnreachableBlocks(function);
    if (folded || threaded) {
        RemoveStalePhiPredecessors(function);
    }
    return folded || threaded || removed ? PassChange::Changed : PassChange::None;
}

std::string_view LirBlockCoalescing::Name() const noexcept {
    return "lir-block-coalescing";
}

PassChange LirBlockCoalescing::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    // Every iteration: a later inlining round leaves the jump chains of the bodies it copies, and a function those
    // chains keep looking large would not be inlined in turn.
    return CoalesceBlocks(function) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
