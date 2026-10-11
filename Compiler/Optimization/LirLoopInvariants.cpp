#include "Optimization/LirLoopInvariants.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantAddresses.h"

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
        return false; // No loads, traps, calls, checks, FP changes or address/lifetime creation.
    }
}

[[nodiscard]] bool ScalarValue(const TypeRef &type) {
    return type.IsInteger() || type.IsBool() || type.IsChar() || type.kind == TypeRef::Kind::Pointer ||
           type.kind == TypeRef::Kind::Reference || type.kind == TypeRef::Kind::Func ||
           type.kind == TypeRef::Kind::Float32 || type.kind == TypeRef::Kind::Float64;
}

/// An integer, character, Boolean or pointer conversion of a value that is not floating point: it cannot trap or
/// raise a floating-point exception wherever it runs.
[[nodiscard]] bool SpeculatableCast(const LirInstr &instruction, const std::unordered_map<LirReg, TypeRef> &types) {
    if (instruction.op != LirOpcode::Cast || instruction.isVolatile || instruction.srcs.size() != 1)
        return false;
    const auto source = types.find(instruction.srcs[0]);
    const auto general = [](const TypeRef &type) {
        return type.IsInteger() || type.IsBool() || type.IsChar() || type.kind == TypeRef::Kind::Pointer;
    };
    return source != types.end() && general(source->second) && general(instruction.type) &&
           source->second.SizeInBytes().value_or(16) <= 8 && instruction.type.SizeInBytes().value_or(16) <= 8;
}

/// Whether the loop body can write memory: any store, call or atomic or ordering operation does.
[[nodiscard]] bool WritesMemory(const LirFunc &function, const std::unordered_set<std::uint32_t> &members) {
    for (const auto block : members) {
        for (const auto &instruction : function.blocks[block].instrs) {
            switch (instruction.op) {
            case LirOpcode::Store:
            case LirOpcode::Call:
            case LirOpcode::CallIndirect:
            case LirOpcode::AtomicStore:
            case LirOpcode::AtomicExchange:
            case LirOpcode::AtomicCmpXchg:
            case LirOpcode::AtomicFetchAdd:
            case LirOpcode::AtomicFetchSub:
            case LirOpcode::AtomicFetchAnd:
            case LirOpcode::AtomicFetchOr:
            case LirOpcode::AtomicFetchXor:
            case LirOpcode::AtomicLoad:
            case LirOpcode::Fence:
            case LirOpcode::WriteStandardError:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

/// Gives every loop entered from a single block that also branches elsewhere its own preheader on that edge, so the
/// loop has a block that runs exactly when the loop is entered and nothing else depends on.
bool InsertPreheaders(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 2 || function.blocks.size() > 4096)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    bool changed = false;
    for (const auto &[header, members] : loops.members) {
        std::vector<std::uint32_t> outside;
        for (const auto predecessor : cfg.predecessors[header])
            if (!members.contains(predecessor))
                outside.push_back(predecessor);
        if (outside.size() != 1 || cfg.successors[outside[0]].size() == 1 || header == 0)
            continue;
        const auto entry = outside[0];
        const auto preheader = static_cast<std::uint32_t>(function.blocks.size());
        auto &term = *function.blocks[entry].term;
        const auto retarget = [&](std::uint32_t &target) {
            if (target == header)
                target = preheader;
        };
        switch (term.kind) {
        case LirTermKind::Jump:
            retarget(term.trueTarget);
            break;
        case LirTermKind::Branch:
            retarget(term.trueTarget);
            retarget(term.falseTarget);
            break;
        case LirTermKind::Switch:
            retarget(term.defaultTarget);
            for (auto &item : term.cases)
                retarget(item.target);
            break;
        default:
            continue;
        }
        for (auto &instruction : function.blocks[header].instrs)
            for (auto &[value, predecessor] : instruction.phiPreds) {
                static_cast<void>(value);
                if (predecessor == entry)
                    predecessor = preheader;
            }
        LirBlock block;
        block.label = function.blocks[header].label + ".preheader";
        block.term.emplace();
        block.term->kind = LirTermKind::Jump;
        block.term->trueTarget = header;
        function.blocks.push_back(std::move(block));
        changed = true;
    }
    return changed;
}

bool Hoist(LirFunc &function, const std::unordered_set<std::string> &constantNames) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 2 || function.blocks.size() > 4096) {
        return false;
    }
    const std::unordered_set<LirReg> constantAddresses = ConstantAddresses(function, constantNames);
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirLiveness liveness(function, cfg);
    std::unordered_map<LirReg, std::uint32_t> definitions;
    std::unordered_map<LirReg, LirInstr> constants;
    std::unordered_map<LirReg, TypeRef> types;
    // Where an address comes from, for the addresses a load may be read through anywhere: an allocation, a global,
    // a by-address parameter, or a field of one of those.
    std::unordered_map<LirReg, std::pair<LirOpcode, LirReg>> origins;
    std::unordered_set<LirReg> validParameters;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions.emplace(parameter.reg, 0);
        types.emplace(parameter.reg, parameter.type);
        next = std::max(next, parameter.reg + 1);
        if (parameter.byAddress)
            validParameters.insert(parameter.reg);
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
                if (instruction.op == LirOpcode::Alloca || instruction.op == LirOpcode::GlobalAddr ||
                    (instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1))
                    origins.emplace(
                        instruction.dst,
                        std::pair{instruction.op, instruction.srcs.empty() ? LirNoReg : instruction.srcs[0]});
            }
            if (instruction.op == LirOpcode::Const && Speculatable(instruction)) {
                constants.emplace(instruction.dst, instruction);
            }
        }
    }
    const auto dereferenceable = [&](LirReg address) {
        for (int depth = 0; depth < 8; ++depth) {
            if (validParameters.contains(address))
                return true;
            const auto origin = origins.find(address);
            if (origin == origins.end())
                return false;
            if (origin->second.first != LirOpcode::FieldPtr)
                return true;
            address = origin->second.second;
        }
        return false;
    };
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
        const bool writes = WritesMemory(function, members);
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
                    // A load moves only out of a loop that writes no memory, or reads read-only constant data, and
                    // only through an address that is valid whether or not the loop runs, so hoisting it can
                    // neither change its value nor fault.
                    const bool load = instruction.op == LirOpcode::Load && instruction.strArg.empty() &&
                                      !instruction.isVolatile && instruction.srcs.size() == 1 &&
                                      instruction.dst != LirNoReg && ScalarValue(instruction.type) &&
                                      (!writes || constantAddresses.contains(instruction.srcs[0])) &&
                                      dereferenceable(instruction.srcs[0]);
                    bool invariant = budget != 0 && instruction.op != LirOpcode::Const &&
                                     (Speculatable(instruction) || SpeculatableCast(instruction, types) || load);
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
                    // Keep crowded loops from gaining more long-lived values than the allocatable registers hold.
                    // Replacing the sole loop use of an input need not add pressure, and a hoisted load costs the
                    // loop no more than the load it replaces even when its value has to be spilled.
                    // A field address of an allocation or global is a frame or symbol constant the back end
                    // rematerializes or folds into an access, so it never occupies a register across the loop.
                    const bool constantAddress = instruction.op == LirOpcode::FieldPtr &&
                                                 instruction.srcs.size() == 1 &&
                                                 origins.contains(instruction.srcs[0]) &&
                                                 origins.at(instruction.srcs[0]).first != LirOpcode::FieldPtr;
                    invariant = invariant && (live.size() < 12 || freed != 0 || load || constantAddress);
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
    const auto constantNames = ConstantNames(package);
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            const bool preheaders = InsertPreheaders(function);
            const bool hoisted = Hoist(function, constantNames);
            changed = preheaders || hoisted || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
