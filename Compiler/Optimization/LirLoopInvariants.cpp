#include "Optimization/LirLoopInvariants.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Ir/Lir/RegisterTable.h"
#include "Optimization/ConstantAddresses.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

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
[[nodiscard]] bool SpeculatableCast(const LirInstr &instruction, const LirRegisterTable<TypeRef> &types) {
    if (instruction.op != LirOpcode::Cast || instruction.isVolatile || instruction.srcs.size() != 1)
        return false;
    const auto *source = types.Find(instruction.srcs[0]);
    const auto general = [](const TypeRef &type) {
        return type.IsInteger() || type.IsBool() || type.IsChar() || type.kind == TypeRef::Kind::Pointer;
    };
    return source != nullptr && general(*source) && general(instruction.type) &&
           source->SizeInBytes().value_or(16) <= 8 && instruction.type.SizeInBytes().value_or(16) <= 8;
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
    const std::size_t registers = RegisterCountHint(function);
    LirRegisterTable<std::uint32_t> definitions(registers);
    LirRegisterTable<LirInstr> constants(registers);
    LirRegisterTable<TypeRef> types(registers);
    // Where an address comes from, for the addresses a load may be read through anywhere: an allocation, a global,
    // a by-address parameter, or a field of one of those.
    LirRegisterTable<std::pair<LirOpcode, LirReg>> origins(registers);
    LirRegisterTable<bool> validParameters(registers);
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions.Insert(parameter.reg, 0);
        types.Insert(parameter.reg, parameter.type);
        next = std::max(next, parameter.reg + 1);
        if (parameter.byAddress)
            validParameters.Insert(parameter.reg, true);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst != LirNoReg && !definitions.Insert(instruction.dst, block)) {
                return false;
            }
            if (instruction.dst != LirNoReg) {
                next = std::max(next, instruction.dst + 1);
                types.Insert(instruction.dst, instruction.op == LirOpcode::Alloca
                                                  ? TypeRef::MakePointer(instruction.type)
                                                  : instruction.type);
                if (instruction.op == LirOpcode::Alloca || instruction.op == LirOpcode::GlobalAddr ||
                    (instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1))
                    origins.Insert(
                        instruction.dst,
                        std::pair{instruction.op, instruction.srcs.empty() ? LirNoReg : instruction.srcs[0]});
            }
            if (instruction.op == LirOpcode::Const && Speculatable(instruction)) {
                constants.Insert(instruction.dst, instruction);
            }
        }
    }
    const auto dereferenceable = [&](LirReg address) {
        for (int depth = 0; depth < 8; ++depth) {
            if (validParameters.Contains(address))
                return true;
            const auto *origin = origins.Find(address);
            if (origin == nullptr)
                return false;
            if (origin->first != LirOpcode::FieldPtr)
                return true;
            address = origin->second;
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
        LirRegisterTable<LirReg> materialized(registers);
        const auto general = [&](const LirReg reg) {
            const auto *found = types.Find(reg);
            return found != nullptr &&
                   (found->IsInteger() || found->IsBool() || found->IsChar() || found->kind == TypeRef::Kind::Pointer ||
                    found->kind == TypeRef::Kind::Reference || found->kind == TypeRef::Kind::Func);
        };
        LirRegisterTable<bool> live(registers);
        for (const auto reg : liveness.liveOut[header]) {
            if (general(reg)) {
                live.Insert(reg, true);
            }
        }
        LirRegisterTable<std::size_t> uses(registers);
        for (const auto block : members) {
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto source : instruction.srcs) {
                    ++uses.Ref(source);
                }
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    static_cast<void>(predecessor);
                    ++uses.Ref(source);
                }
            }
            if (const auto &term = function.blocks[block].term) {
                ++uses.Ref(term->cond);
                if (term->retVal) {
                    ++uses.Ref(*term->retVal);
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
                    // An instruction has a few sources, so short lists stand in for a set and a count per source.
                    std::vector<LirReg> neededConstants;
                    std::vector<std::pair<LirReg, std::size_t>> consumed;
                    for (const auto source : instruction.srcs) {
                        if (auto counted = std::ranges::find(consumed, source, &std::pair<LirReg, std::size_t>::first);
                            counted != consumed.end())
                            ++counted->second;
                        else
                            consumed.emplace_back(source, 1);
                        const auto *definition = definitions.Find(source);
                        if (constants.Contains(source) && definition != nullptr && members.contains(*definition)) {
                            if (!materialized.Contains(source) && !std::ranges::contains(neededConstants, source)) {
                                neededConstants.push_back(source);
                            }
                        }
                        else {
                            invariant = invariant && definition != nullptr && !members.contains(*definition) &&
                                        dominance.Dominates(*definition, preheader);
                        }
                    }
                    std::size_t freed = 0;
                    for (const auto &[source, count] : consumed) {
                        if (live.Contains(source) && uses.Ref(source) == count) {
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
                                                 origins.Contains(instruction.srcs[0]) &&
                                                 origins.At(instruction.srcs[0]).first != LirOpcode::FieldPtr;
                    invariant = invariant && (live.Size() < 12 || freed != 0 || load || constantAddress);
                    if (invariant && 1 + neededConstants.size() <= budget) {
                        for (const auto &[source, count] : consumed) {
                            auto &remaining = uses.Ref(source);
                            remaining -= count;
                            if (remaining == 0) {
                                live.Erase(source);
                            }
                        }
                        live.Insert(instruction.dst, true);
                        for (auto &source : instruction.srcs) {
                            if (!constants.Contains(source) || !members.contains(definitions.At(source))) {
                                continue;
                            }
                            if (!materialized.Contains(source)) {
                                auto constant = constants.At(source);
                                constant.dst = next++;
                                definitions.Assign(constant.dst, preheader);
                                materialized.Insert(source, constant.dst);
                                function.blocks[preheader].instrs.push_back(std::move(constant));
                                --budget;
                            }
                            source = materialized.At(source);
                        }
                        definitions.Assign(instruction.dst, preheader);
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

void LirLoopInvariants::Prepare(const LirPackage &package) {
    constantNames = ConstantNames(package);
}

PassChange LirLoopInvariants::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (!MayHaveLoops(function))
        return PassChange::None;
    // This bounded motion phase runs once; reaching its budget leaves code in place, never drives nonconvergence.
    const bool preheaders = InsertPreheaders(function);
    const bool hoisted = Hoist(function, constantNames);
    return preheaders || hoisted ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
