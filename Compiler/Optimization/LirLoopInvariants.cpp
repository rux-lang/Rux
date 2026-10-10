#include "Optimization/LirLoopInvariants.h"

#include "Ir/Lir/NaturalLoops.h"

#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool Speculatable(const LirInstr &instruction) {
    if (instruction.isVolatile || instruction.dst == LirNoReg ||
        !(instruction.type.IsInteger() || instruction.type.IsBool() || instruction.type.IsChar() ||
          instruction.type.kind == TypeRef::Kind::Pointer)) {
        return false;
    }
    switch (instruction.op) {
    case LirOpcode::Const:
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
    case LirOpcode::FieldPtr:
    case LirOpcode::IndexPtr:
        return true;
    default:
        return false; // No loads, traps, calls, checks, casts, FP changes or address/lifetime creation.
    }
}

bool Hoist(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 2 || function.blocks.size() > 4096) {
        return false;
    }
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirLiveness liveness(function, cfg);
    std::unordered_map<LirReg, std::uint32_t> definitions;
    std::unordered_map<LirReg, LirInstr> constants;
    std::unordered_map<LirReg, TypeRef> types;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions.emplace(parameter.reg, 0);
        types.emplace(parameter.reg, parameter.type);
        next = std::max(next, parameter.reg + 1);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst != LirNoReg && !definitions.emplace(instruction.dst, block).second) {
                return false;
            }
            if (instruction.dst != LirNoReg) {
                next = std::max(next, instruction.dst + 1);
                types.emplace(instruction.dst, instruction.op == LirOpcode::Alloca
                                                   ? TypeRef::MakePointer(instruction.type)
                                                   : instruction.type);
            }
            if (instruction.op == LirOpcode::Const && Speculatable(instruction)) {
                constants.emplace(instruction.dst, instruction);
            }
        }
    }
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    bool changed = false;
    std::size_t budget = 512;
    for (const auto &[header, members] : loops.members) {
        auto preheader = LirDominance::Invalid;
        bool unique = true;
        for (const auto predecessor : cfg.predecessors[header]) {
            if (!members.contains(predecessor)) {
                if (preheader != LirDominance::Invalid) {
                    unique = false;
                }
                preheader = predecessor;
            }
        }
        if (!unique || preheader == LirDominance::Invalid || cfg.successors[preheader].size() != 1 ||
            !dominance.Dominates(preheader, header)) {
            continue;
        }
        bool again = true;
        std::unordered_map<LirReg, LirReg> materialized;
        const auto general = [&](const LirReg reg) {
            const auto found = types.find(reg);
            return found != types.end() &&
                   (found->second.IsInteger() || found->second.IsBool() || found->second.IsChar() ||
                    found->second.kind == TypeRef::Kind::Pointer || found->second.kind == TypeRef::Kind::Reference ||
                    found->second.kind == TypeRef::Kind::Func);
        };
        std::unordered_set<LirReg> live;
        for (const auto reg : liveness.liveOut[header]) {
            if (general(reg)) {
                live.insert(reg);
            }
        }
        std::unordered_map<LirReg, std::size_t> uses;
        for (const auto block : members) {
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto source : instruction.srcs) {
                    ++uses[source];
                }
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    static_cast<void>(predecessor);
                    ++uses[source];
                }
            }
            if (const auto &term = function.blocks[block].term) {
                ++uses[term->cond];
                if (term->retVal) {
                    ++uses[*term->retVal];
                }
            }
        }
        while (again && budget != 0) {
            again = false;
            for (const auto block : dominance.ReversePostorder()) {
                if (!members.contains(block)) {
                    continue;
                }
                std::vector<LirInstr> retained;
                for (auto &instruction : function.blocks[block].instrs) {
                    // A constant used by a varying recurrence is cheap to recreate near its use. Hoisting it alone
                    // extends its live range and can displace hotter loop values from the small register pool.
                    bool invariant = budget != 0 && instruction.op != LirOpcode::Const && Speculatable(instruction);
                    std::unordered_set<LirReg> neededConstants;
                    std::unordered_map<LirReg, std::size_t> consumed;
                    for (const auto source : instruction.srcs) {
                        ++consumed[source];
                        const auto definition = definitions.find(source);
                        if (constants.contains(source) && definition != definitions.end() &&
                            members.contains(definition->second)) {
                            if (!materialized.contains(source)) {
                                neededConstants.insert(source);
                            }
                        }
                        else {
                            invariant = invariant && definition != definitions.end() &&
                                        !members.contains(definition->second) &&
                                        dominance.Dominates(definition->second, preheader);
                        }
                    }
                    std::size_t freed = 0;
                    for (const auto &[source, count] : consumed) {
                        if (live.contains(source) && uses[source] == count) {
                            ++freed;
                        }
                    }
                    // Five is the smaller current native GPR pool. Until machine-level costing exists, avoid adding
                    // long-lived values to crowded loops. Replacing the sole loop use of an input need not add
                    // pressure.
                    invariant = invariant && (live.size() < 5 || freed != 0);
                    if (invariant && 1 + neededConstants.size() <= budget) {
                        for (const auto &[source, count] : consumed) {
                            uses[source] -= count;
                            if (uses[source] == 0) {
                                live.erase(source);
                            }
                        }
                        live.insert(instruction.dst);
                        for (auto &source : instruction.srcs) {
                            if (!constants.contains(source) || !members.contains(definitions.at(source))) {
                                continue;
                            }
                            if (!materialized.contains(source)) {
                                auto constant = constants.at(source);
                                constant.dst = next++;
                                definitions[constant.dst] = preheader;
                                materialized.emplace(source, constant.dst);
                                function.blocks[preheader].instrs.push_back(std::move(constant));
                                --budget;
                            }
                            source = materialized.at(source);
                        }
                        definitions[instruction.dst] = preheader;
                        function.blocks[preheader].instrs.push_back(std::move(instruction));
                        --budget;
                        changed = true;
                        again = true;
                    }
                    else {
                        retained.push_back(std::move(instruction));
                    }
                }
                function.blocks[block].instrs = std::move(retained);
            }
        }
    }
    return changed;
}
} // namespace

std::string_view LirLoopInvariants::Name() const noexcept {
    return "lir-loop-invariants";
}

PassChange LirLoopInvariants::Run(LirPackage &package, const PassContext &context) {
    // This bounded motion phase runs once; reaching its budget leaves code in place, never drives nonconvergence.
    if (context.iteration != 0) {
        return PassChange::None;
    }
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            changed = Hoist(function) || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
