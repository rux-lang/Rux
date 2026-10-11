#include "Optimization/LirLoopVersioning.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantEvaluator.h"

#include <limits>
#include <optional>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
bool Word(const TypeRef &type) {
    // Every supported target has 64-bit uint. Signed and narrowing conversions need separate proofs.
    return type.kind == TypeRef::Kind::UInt || type.kind == TypeRef::Kind::UInt64;
}

bool SignedWord(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Int || type.kind == TypeRef::Kind::Int64;
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
    /// For a signed induction value, a constant added to it; its first index is non-negative by construction.
    std::int64_t constantOffset = 0;
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
        const auto signedWord = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && SignedWord(found->second.type);
        };
        // A signed induction value counts up by one from a non-negative constant, so every value it takes in the
        // loop is non-negative and below the limit, and converting it to an unsigned index changes nothing.
        const bool signedInduction =
            signedWord(induction) && signedWord(limit) && definitions.at(induction).type == definitions.at(limit).type;
        if ((!signedInduction && (!word(induction) || !word(limit))) || !phi || phi->op != LirOpcode::Phi ||
            phi->phiPreds.size() != 2 || definitions.at(induction).block != header ||
            cfg.predecessors[header].size() != 2)
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
            cfg.successors[preheader].size() != 1 || !dominance.Dominates(preheader, header))
            continue;
        // The first index: zero for an unsigned induction with any stride, any non-negative constant otherwise.
        std::optional<std::int64_t> start;
        if (const auto *first = get(initial); first && first->op == LirOpcode::Const && !first->isVolatile &&
                                              first->srcs.empty() && first->type == phi->type) {
            if (const auto value = ParseConstant(first->strArg, first->type)) {
                const auto bits = static_cast<std::int64_t>(value->RawBits());
                if (bits >= 0)
                    start = bits;
            }
        }
        if (!start)
            continue;
        const auto *step = get(increment);
        if (!step || step->op != LirOpcode::Add || step->isVolatile || step->type != phi->type ||
            step->srcs.size() != 2 || step->srcs[0] != induction ||
            !dominance.Dominates(definitions.at(increment).block, latch))
            continue;
        const auto *strideInstruction = get(step->srcs[1]);
        if (!strideInstruction || strideInstruction->op != LirOpcode::Const || strideInstruction->isVolatile ||
            strideInstruction->type != phi->type || !strideInstruction->srcs.empty())
            continue;
        const auto strideValue = ParseConstant(strideInstruction->strArg, strideInstruction->type);
        if (!strideValue || strideValue->RawBits() == 0 || strideValue->RawBits() > 64)
            continue;
        const auto stride = strideValue->RawBits();
        // Strides other than one compute the last index from a zero start in unsigned arithmetic.
        if ((stride != 1 && (signedInduction || *start != 0)))
            continue;
        // A constant counts as invariant wherever it is defined: the guard recreates it in the preheader.
        const auto loopConstant = [&](const LirReg reg) {
            const auto *instruction = get(reg);
            return instruction && instruction->op == LirOpcode::Const && !instruction->isVolatile &&
                   instruction->srcs.empty() && members.contains(definitions.at(reg).block);
        };
        const auto invariant = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && Word(found->second.type) &&
                   ((!members.contains(found->second.block) && dominance.Dominates(found->second.block, preheader)) ||
                    loopConstant(reg));
        };
        const auto invariantLimit = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() &&
                   ((!members.contains(found->second.block) && dominance.Dominates(found->second.block, preheader)) ||
                    loopConstant(reg));
        };
        if (signedInduction ? !invariantLimit(limit) : !invariant(limit))
            continue;
        // The signed index expression a check compares, as the induction value plus a constant.
        const auto signedIndex = [&](LirReg reg) -> std::optional<std::int64_t> {
            const auto *cast = get(reg);
            if (!cast || cast->op != LirOpcode::Cast || cast->isVolatile || cast->srcs.size() != 1 ||
                !signedWord(cast->srcs[0]))
                return std::nullopt;
            reg = cast->srcs[0];
            if (reg == induction)
                return 0;
            const auto *arithmetic = get(reg);
            if (!arithmetic || (arithmetic->op != LirOpcode::Add && arithmetic->op != LirOpcode::Sub) ||
                arithmetic->isVolatile || arithmetic->srcs.size() != 2 || arithmetic->srcs[0] != induction)
                return std::nullopt;
            const auto *amount = get(arithmetic->srcs[1]);
            if (!amount || amount->op != LirOpcode::Const || amount->isVolatile || amount->type != phi->type)
                return std::nullopt;
            const auto value = ParseConstant(amount->strArg, amount->type);
            if (!value)
                return std::nullopt;
            const auto magnitude = static_cast<std::int64_t>(value->RawBits());
            if (magnitude < -(std::int64_t{1} << 40) || magnitude > (std::int64_t{1} << 40))
                return std::nullopt;
            return arithmetic->op == LirOpcode::Add ? magnitude : -magnitude;
        };
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
            if (signedInduction) {
                // Only checks whose first index is already non-negative: below zero the conversion would wrap.
                if (const auto offset = signedIndex(comparison->srcs[0]); offset && *start + *offset >= 0)
                    checks.push_back({block, LirNoReg, comparison->srcs[1], *offset});
                continue;
            }
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
        if (!eligible || cost > 160 || checks.empty() || checks.size() > 8)
            continue;
        // A value defined in the loop and read after it must reach its readers through an exit phi, which merges
        // what the checked and the fast loop computed. When the normal exit has the header as its only predecessor,
        // a dedicated exit block holds those phis; any other shape keeps the loop unversioned.
        const auto local = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && members.contains(found->second.block);
        };
        const auto exitBlock = term->falseTarget;
        std::vector<LirReg> liveOut;
        std::vector<std::uint32_t> readers;
        const auto note = [&](const LirReg reg) {
            if (std::ranges::find(liveOut, reg) == liveOut.end())
                liveOut.push_back(reg);
        };
        // Phi operands on loop exit edges keep their edges in the clone. Only when a dedicated exit block takes over
        // the normal exit do the normal exit's own phis read through it.
        std::vector<LirReg> exitEdgeValues;
        for (std::uint32_t block = 0; block < function.blocks.size() && eligible; ++block) {
            if (members.contains(block))
                continue;
            bool reads = false;
            bool readsThroughPhi = false;
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto source : instruction.srcs)
                    if (local(source)) {
                        reads = true;
                        note(source);
                    }
                // Otherwise a phi operand is read at the end of its predecessor, which the normal exit must
                // dominate, as it does the jump an inlined callee's return takes.
                for (const auto &[value, predecessor] : instruction.phiPreds) {
                    if (!local(value))
                        continue;
                    if (members.contains(predecessor)) {
                        if (block == exitBlock)
                            exitEdgeValues.push_back(value);
                    }
                    else if (dominance.Dominates(exitBlock, predecessor)) {
                        readsThroughPhi = true;
                        note(value);
                    }
                    else {
                        eligible = false;
                    }
                }
            }
            if (const auto &exit = function.blocks[block].term)
                for (const auto reg : {exit->cond, exit->retVal.value_or(LirNoReg)})
                    if (reg != LirNoReg && local(reg)) {
                        reads = true;
                        note(reg);
                    }
            if (reads && block != exitBlock && !dominance.Dominates(exitBlock, block))
                eligible = false;
            if (reads || readsThroughPhi)
                readers.push_back(block);
        }
        if (!eligible || (!liveOut.empty() && cfg.predecessors[exitBlock] != std::vector<std::uint32_t>{header}))
            continue;
        if (!liveOut.empty() && !exitEdgeValues.empty()) {
            for (const auto value : exitEdgeValues)
                note(value);
            if (std::ranges::find(readers, exitBlock) == readers.end())
                readers.push_back(exitBlock);
        }
        if (!liveOut.empty()) {
            const auto exitIndex = static_cast<std::uint32_t>(function.blocks.size());
            LirBlock exit;
            exit.label = function.blocks[header].label + ".exit";
            std::unordered_map<LirReg, LirReg> merged;
            for (const auto value : liveOut) {
                LirInstr merge;
                merge.op = LirOpcode::Phi;
                merge.dst = next++;
                merge.type = definitions.at(value).type;
                merge.phiPreds = {{value, header}};
                merged.emplace(value, merge.dst);
                exit.instrs.push_back(std::move(merge));
            }
            exit.term.emplace();
            exit.term->kind = LirTermKind::Jump;
            exit.term->trueTarget = exitBlock;
            for (auto &instruction : function.blocks[exitBlock].instrs)
                for (auto &[value, predecessor] : instruction.phiPreds) {
                    static_cast<void>(value);
                    if (predecessor == header)
                        predecessor = exitIndex;
                }
            const auto rename = [&](LirReg &reg) {
                if (const auto found = merged.find(reg); found != merged.end())
                    reg = found->second;
            };
            for (const auto block : readers) {
                for (auto &instruction : function.blocks[block].instrs) {
                    for (auto &source : instruction.srcs)
                        rename(source);
                    for (auto &[value, predecessor] : instruction.phiPreds) {
                        static_cast<void>(predecessor);
                        rename(value);
                    }
                }
                if (auto &exitTerm = function.blocks[block].term) {
                    rename(exitTerm->cond);
                    if (exitTerm->retVal)
                        rename(*exitTerm->retVal);
                }
            }
            function.blocks[header].term->falseTarget = exitIndex;
            function.blocks.push_back(std::move(exit));
        }

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
        const auto widenFrom = [&](const LirReg value, const TypeRef &from) {
            return emit(LirOpcode::Cast, TypeRef::MakeUInt64(), {value}, from.ToString());
        };
        // The value as the preheader sees it: a constant defined inside the loop is recreated there.
        const auto outside = [&](const LirReg value) {
            const auto &definition = definitions.at(value);
            if (definition.instruction && definition.instruction->op == LirOpcode::Const &&
                members.contains(definition.block))
                return emit(LirOpcode::Const, definition.type, {}, definition.instruction->strArg);
            return value;
        };
        const auto widen = [&](const LirReg value) { return widenFrom(outside(value), definitions.at(value).type); };
        const auto limit64 = widen(limit);
        auto count = limit64;
        LirReg guard = LirNoReg;
        if (stride != 1) {
            // The last visited index is floor((limit - 1) / stride) * stride. Reject limits for which the
            // increment could wrap: after wrapping, a non-power-of-two stride can visit different residues.
            // For an empty loop, these unsigned calculations may wrap but never access memory or trap.
            const auto one = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, "1");
            const auto stepSize = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, std::to_string(stride));
            const auto last = emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {limit64, one});
            const auto quotient = emit(LirOpcode::Div, TypeRef::MakeUInt64(), {last, stepSize});
            const auto lastIndex = emit(LirOpcode::Mul, TypeRef::MakeUInt64(), {quotient, stepSize});
            count = emit(LirOpcode::Add, TypeRef::MakeUInt64(), {lastIndex, one});
            const auto maximum = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {},
                                      std::to_string(std::numeric_limits<std::uint64_t>::max() - (stride - 1)));
            guard = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {limit64, maximum});
        }
        for (const auto &check : checks) {
            const auto bound = widen(check.bound);
            LirReg safe;
            if (signedInduction) {
                // Every index the loop computes is at most limit - 1 + offset, so limit + offset must not exceed
                // the bound. When the loop runs, limit exceeds the non-negative start, and limit + offset is
                // positive; a sum that overflows wraps to a negative value, which as an unsigned bound fails the
                // guard and keeps the checked loop.
                LirReg last = outside(limit);
                if (check.constantOffset != 0) {
                    const auto amount =
                        emit(LirOpcode::Const, definitions.at(limit).type, {}, std::to_string(check.constantOffset));
                    last = emit(LirOpcode::Add, definitions.at(limit).type, {last, amount});
                }
                safe =
                    emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {widenFrom(last, definitions.at(limit).type), bound});
            }
            else if (check.offset == LirNoReg) {
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
    // Each round versions one loop; a versioned loop's entry becomes a guard branch, which no later round accepts as
    // a preheader, so repeating reaches the function's other loops without revisiting one.
    for (auto &module : package.modules)
        for (auto &function : module.funcs)
            for (int round = 0; round < 8 && Version(function); ++round)
                changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
