#include "Optimization/LirLoopVersioning.h"

#include "Ir/Lir/NaturalLoops.h"

#include <unordered_map>

namespace Rux::Optimization {
namespace {
bool Word(const TypeRef &type) {
    // Every supported target has 64-bit uint. Signed and narrowing conversions need separate proofs.
    return type.kind == TypeRef::Kind::UInt || type.kind == TypeRef::Kind::UInt64;
}

struct Definition {
    const LirInstr *instruction;
    std::uint32_t block;
    TypeRef type;
};

struct Check {
    std::uint32_t block;
    LirReg offset;
    LirReg bound;
};

bool Version(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3 || function.blocks.size() > 2048)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    std::unordered_map<LirReg, Definition> definitions;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        if (!definitions.emplace(parameter.reg, Definition{nullptr, 0, parameter.type}).second)
            return false;
        next = std::max(next, parameter.reg + 1);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst == LirNoReg)
                continue;
            if (!definitions.emplace(instruction.dst, Definition{&instruction, block, instruction.type}).second)
                return false;
            next = std::max(next, instruction.dst + 1);
        }
    }
    const auto get = [&](const LirReg reg) -> const LirInstr * {
        const auto found = definitions.find(reg);
        return found == definitions.end() ? nullptr : found->second.instruction;
    };
    const auto word = [&](const LirReg reg) {
        const auto found = definitions.find(reg);
        return found != definitions.end() && Word(found->second.type);
    };
    const auto strip = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < 8; ++depth) {
            const auto *instruction = get(reg);
            if (!instruction || instruction->op != LirOpcode::Cast || instruction->isVolatile ||
                instruction->srcs.size() != 1 || !word(reg) || !word(instruction->srcs[0]))
                break;
            reg = instruction->srcs[0];
        }
        return reg;
    };
    const auto literal = [&](const LirReg reg, const std::string_view value) {
        const auto *instruction = get(reg);
        return instruction && instruction->op == LirOpcode::Const && !instruction->isVolatile &&
               Word(instruction->type) && instruction->strArg == value;
    };
    for (const auto &[header, members] : loops.members) {
        if (header == 0 || members.size() > 16 || std::ranges::any_of(loops.members, [&](const auto &loop) {
                return loop.first != header && members.contains(loop.first);
            }))
            continue;
        const auto &term = function.blocks[header].term;
        if (!term || term->kind != LirTermKind::Branch || !members.contains(term->trueTarget) ||
            members.contains(term->falseTarget))
            continue;
        const auto *condition = get(term->cond);
        if (!condition || condition->op != LirOpcode::CmpLt || condition->isVolatile || condition->srcs.size() != 2)
            continue;
        const auto induction = condition->srcs[0];
        const auto limit = condition->srcs[1];
        const auto *phi = get(induction);
        if (!word(induction) || !word(limit) || !phi || phi->op != LirOpcode::Phi || phi->phiPreds.size() != 2 ||
            definitions.at(induction).block != header || cfg.predecessors[header].size() != 2)
            continue;
        auto preheader = LirDominance::Invalid;
        auto latch = LirDominance::Invalid;
        auto initial = LirNoReg;
        auto increment = LirNoReg;
        for (const auto &[value, predecessor] : phi->phiPreds) {
            if (members.contains(predecessor)) {
                latch = predecessor;
                increment = value;
            }
            else {
                preheader = predecessor;
                initial = value;
            }
        }
        if (preheader == LirDominance::Invalid || latch == LirDominance::Invalid ||
            cfg.successors[preheader].size() != 1 || !dominance.Dominates(preheader, header) || !literal(initial, "0"))
            continue;
        const auto *step = get(increment);
        if (!step || step->op != LirOpcode::Add || step->isVolatile || step->type != phi->type ||
            step->srcs.size() != 2 || step->srcs[0] != induction || !literal(step->srcs[1], "1") ||
            !dominance.Dominates(definitions.at(increment).block, latch))
            continue;
        const auto invariant = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && Word(found->second.type) && !members.contains(found->second.block) &&
                   dominance.Dominates(found->second.block, preheader);
        };
        if (!invariant(limit))
            continue;
        bool eligible = true;
        std::size_t cost = 0;
        std::vector<Check> checks;
        for (const auto block : members) {
            // No side entries; the controlling comparison must execute before every iteration's body.
            for (const auto predecessor : cfg.predecessors[block])
                if (block != header && !members.contains(predecessor))
                    eligible = false;
            if (block != header && !dominance.Dominates(term->trueTarget, block))
                eligible = false;
            for (const auto &instruction : function.blocks[block].instrs) {
                ++cost;
                if (instruction.op == LirOpcode::Alloca || instruction.op == LirOpcode::Call ||
                    instruction.op == LirOpcode::CallIndirect || instruction.isVolatile)
                    eligible = false;
            }
            const auto &branch = function.blocks[block].term;
            if (block == header || !branch || branch->kind != LirTermKind::Branch ||
                !members.contains(branch->trueTarget) || members.contains(branch->falseTarget))
                continue;
            const auto *comparison = get(branch->cond);
            if (!comparison || comparison->op != LirOpcode::CmpLt || comparison->isVolatile ||
                comparison->srcs.size() != 2 || !word(comparison->srcs[0]) || !invariant(comparison->srcs[1]))
                continue;
            const auto index = strip(comparison->srcs[0]);
            LirReg offset = LirNoReg;
            if (index != induction) {
                const auto *add = get(index);
                if (!add || add->op != LirOpcode::Add || add->isVolatile || !Word(add->type) || add->srcs.size() != 2)
                    continue;
                if (strip(add->srcs[0]) == induction && invariant(add->srcs[1]))
                    offset = add->srcs[1];
                else if (strip(add->srcs[1]) == induction && invariant(add->srcs[0]))
                    offset = add->srcs[0];
                else
                    continue;
            }
            checks.push_back({block, offset, comparison->srcs[1]});
        }
        if (!eligible || cost > 96 || checks.empty() || checks.size() > 4)
            continue;
        // Require existing exit phis for live-out definitions. General LCSSA construction is a separate phase.
        const auto local = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && members.contains(found->second.block);
        };
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            if (members.contains(block))
                continue;
            for (const auto &instruction : function.blocks[block].instrs) {
                if (std::ranges::any_of(instruction.srcs, local))
                    eligible = false;
                for (const auto &[value, predecessor] : instruction.phiPreds)
                    if (local(value) && !members.contains(predecessor))
                        eligible = false;
            }
            if (const auto &exit = function.blocks[block].term)
                if (local(exit->cond) || (exit->retVal && local(*exit->retVal)))
                    eligible = false;
        }
        if (!eligible)
            continue;

        std::vector<std::uint32_t> ordered(members.begin(), members.end());
        std::ranges::sort(ordered);
        std::unordered_map<std::uint32_t, std::uint32_t> blocks;
        std::unordered_map<LirReg, LirReg> registers;
        const auto oldCount = static_cast<std::uint32_t>(function.blocks.size());
        for (std::uint32_t index = 0; index < ordered.size(); ++index) {
            blocks.emplace(ordered[index], oldCount + index);
            for (const auto &instruction : function.blocks[ordered[index]].instrs)
                if (instruction.dst != LirNoReg)
                    registers.emplace(instruction.dst, next++);
        }
        const auto mappedReg = [&](const LirReg reg) { return registers.contains(reg) ? registers.at(reg) : reg; };
        const auto mappedBlock = [&](const std::uint32_t block) {
            return blocks.contains(block) ? blocks.at(block) : block;
        };
        std::vector<LirBlock> clones;
        for (const auto block : ordered) {
            auto clone = function.blocks[block];
            clone.label += ".range.fast." + std::to_string(block);
            for (auto &instruction : clone.instrs) {
                instruction.dst = mappedReg(instruction.dst);
                for (auto &source : instruction.srcs)
                    source = mappedReg(source);
                for (auto &[source, predecessor] : instruction.phiPreds) {
                    source = mappedReg(source);
                    predecessor = mappedBlock(predecessor);
                }
            }
            if (auto &exit = clone.term) {
                if (std::ranges::any_of(checks, [&](const Check &check) { return check.block == block; })) {
                    exit->kind = LirTermKind::Jump;
                    exit->cond = LirNoReg;
                    exit->falseTarget = 0;
                }
                exit->cond = mappedReg(exit->cond);
                if (exit->retVal)
                    exit->retVal = mappedReg(*exit->retVal);
                exit->trueTarget = mappedBlock(exit->trueTarget);
                exit->falseTarget = mappedBlock(exit->falseTarget);
                exit->defaultTarget = mappedBlock(exit->defaultTarget);
                for (auto &entry : exit->cases)
                    entry.target = mappedBlock(entry.target);
            }
            clones.push_back(std::move(clone));
        }
        // Build non-trapping guards in the existing preheader. Subtraction may wrap when row > bound, but the
        // corresponding row <= bound conjunct rejects that case. No new loads, calls or panics are introduced.
        auto &setup = function.blocks[preheader].instrs;
        const auto emit = [&](const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources,
                              const std::string &argument = "") {
            LirInstr instruction;
            instruction.op = op;
            instruction.dst = next++;
            instruction.type = type;
            instruction.srcs = std::move(sources);
            instruction.strArg = argument;
            setup.push_back(std::move(instruction));
            return setup.back().dst;
        };
        const auto widen = [&](const LirReg value) {
            return emit(LirOpcode::Cast, TypeRef::MakeUInt64(), {value}, definitions.at(value).type.ToString());
        };
        const auto count = widen(limit);
        LirReg guard = LirNoReg;
        for (const auto &check : checks) {
            const auto bound = widen(check.bound);
            LirReg safe;
            if (check.offset == LirNoReg) {
                safe = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {count, bound});
            }
            else {
                const auto offset = widen(check.offset);
                const auto fits = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {offset, bound});
                const auto remaining = emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {bound, offset});
                const auto lengthFits = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {count, remaining});
                safe = emit(LirOpcode::And, TypeRef::MakeBool(), {fits, lengthFits});
            }
            guard = guard == LirNoReg ? safe : emit(LirOpcode::And, TypeRef::MakeBool(), {guard, safe});
        }
        auto &entry = *function.blocks[preheader].term;
        entry.kind = LirTermKind::Branch;
        entry.cond = guard;
        entry.trueTarget = blocks.at(header);
        entry.falseTarget = header;
        for (auto &clone : clones)
            function.blocks.push_back(std::move(clone));
        // Update only real outgoing edges of the clone (eliminated check-failure edges get no phi operands).
        const LirControlFlow updated(function);
        for (const auto block : ordered) {
            const auto clone = blocks.at(block);
            for (const auto target : updated.successors[clone]) {
                if (target >= oldCount)
                    continue;
                for (auto &instruction : function.blocks[target].instrs) {
                    if (instruction.op != LirOpcode::Phi)
                        continue;
                    const auto edge = std::ranges::find_if(
                        instruction.phiPreds, [&](const auto &incoming) { return incoming.second == block; });
                    if (edge != instruction.phiPreds.end())
                        instruction.phiPreds.emplace_back(mappedReg(edge->first), clone);
                }
            }
        }
        return true; // Bound code growth to one innermost loop per function and one invocation per pipeline run.
    }
    return false;
}
} // namespace

std::string_view LirLoopVersioning::Name() const noexcept {
    return "lir-loop-versioning";
}

PassChange LirLoopVersioning::Run(LirPackage &package, const PassContext &context) {
    if (context.iteration != 0)
        return PassChange::None;
    bool changed = false;
    for (auto &module : package.modules)
        for (auto &function : module.funcs)
            changed = Version(function) || changed;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
