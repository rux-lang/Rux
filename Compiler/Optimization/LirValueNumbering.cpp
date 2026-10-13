#include "Optimization/LirValueNumbering.h"

#include "Ir/Lir/Dominance.h"
#include "Optimization/ConstantAddresses.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool Pure(const LirOpcode op) {
    switch (op) {
    case LirOpcode::Const:
    case LirOpcode::GlobalAddr:
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
        return true;
    default:
        return false;
    }
}

using LiteralKeys = std::unordered_map<LirReg, std::string>;
/// The source of each conversion between two integer types of one width and signedness, such as `uint` and `uint64`:
/// the value's bits do not change, so an operand read through it is the same operand.
using SameBits = std::unordered_map<LirReg, LirReg>;

std::string Key(const LirInstr &instruction, const LiteralKeys &literals, const SameBits &sameBits = {}) {
    const auto type = instruction.type.ToString();
    std::string key = std::to_string(static_cast<unsigned>(instruction.op)) + ":" + std::to_string(type.size()) + ":" +
                      type + ":" + std::to_string(instruction.strArg.size()) + ":" + instruction.strArg;
    for (auto source : instruction.srcs) {
        // Only an operand is looked through; the instruction's own type still separates different results.
        if (instruction.op != LirOpcode::Cast)
            for (int depth = 0; depth < 4; ++depth) {
                const auto found = sameBits.find(source);
                if (found == sameBits.end())
                    break;
                source = found->second;
            }
        if (const auto literal = literals.find(source); literal != literals.end()) {
            key += ":c" + std::to_string(literal->second.size()) + ":" + literal->second;
        }
        else {
            key += ":r" + std::to_string(source);
        }
    }
    return key;
}

bool Number(LirFunc &function, const std::unordered_set<std::string> &constantNames) {
    // A load from read-only constant data is a pure value of its address, so it is shared like arithmetic.
    const std::unordered_set<LirReg> constantAddresses = ConstantAddresses(function, constantNames);
    std::unordered_set<LirReg> definitions;
    std::unordered_map<LirReg, TypeRef> types;
    LiteralKeys literals;
    for (const auto &parameter : function.params) {
        types.emplace(parameter.reg, parameter.type);
        if (!definitions.insert(parameter.reg).second) {
            return false;
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg)
                types.emplace(instruction.dst, instruction.type);
            if (instruction.dst != LirNoReg && !definitions.insert(instruction.dst).second) {
                return false;
            }
            if (instruction.dst != LirNoReg && instruction.op == LirOpcode::Const && !instruction.isVolatile &&
                instruction.srcs.empty() &&
                (instruction.type.IsInteger() || instruction.type.IsBool() || instruction.type.IsChar())) {
                // Preserve local constant definitions while recognizing equal typed operands. Own the text:
                // instruction storage is replaced during traversal, so retaining instruction pointers is unsafe.
                literals.emplace(instruction.dst, Key(instruction, {}));
            }
        }
    }
    SameBits sameBits;
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Cast || instruction.isVolatile || instruction.srcs.size() != 1 ||
                instruction.dst == LirNoReg || !types.contains(instruction.srcs[0]))
                continue;
            const TypeRef &from = types.at(instruction.srcs[0]);
            const TypeRef &to = instruction.type;
            if (from.IsInteger() && to.IsInteger() && from.IsSigned() == to.IsSigned() && from.SizeInBytes() &&
                from.SizeInBytes() == to.SizeInBytes())
                sameBits.emplace(instruction.dst, instruction.srcs[0]);
        }
    std::unordered_map<LirReg, LirReg> copies;
    const auto resolve = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < copies.size(); ++depth) {
            const auto found = copies.find(reg);
            if (found == copies.end()) {
                break;
            }
            reg = found->second;
        }
        return reg;
    };
    bool changed = false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    std::vector<std::vector<std::uint32_t>> children(function.blocks.size());
    const auto &parents = dominance.ImmediateDominators();
    for (std::uint32_t block = 1; block < parents.size(); ++block) {
        if (parents[block] != LirDominance::Invalid)
            children[parents[block]].push_back(block);
    }

    using MemoryFacts = std::unordered_map<std::string, LirReg>;

    struct Visit {
        std::uint32_t block;
        bool exiting;
        std::size_t checkpoint;
        std::shared_ptr<const MemoryFacts> memory = nullptr;
    };

    std::vector<Visit> visits;
    // Unreachable blocks are separate roots: nothing from a reachable block dominates them.
    for (std::uint32_t block = 0; block < parents.size(); ++block) {
        if (block == 0 || parents[block] == LirDominance::Invalid)
            visits.push_back({block, false, 0});
    }
    std::unordered_map<std::string, LirReg> dominating;
    std::vector<std::string> scopedKeys;
    std::size_t memoryBudget = 65536;
    const auto stableType = [](const TypeRef &type) {
        return type.IsInteger() || type.IsBool() || type.IsChar() || type.kind == TypeRef::Kind::Pointer ||
               type.kind == TypeRef::Kind::Reference;
    };
    while (!visits.empty()) {
        const auto visit = visits.back();
        visits.pop_back();
        if (visit.exiting) {
            while (scopedKeys.size() > visit.checkpoint) {
                dominating.erase(scopedKeys.back());
                scopedKeys.pop_back();
            }
            continue;
        }
        visits.push_back({visit.block, true, scopedKeys.size()});
        auto &block = function.blocks[visit.block];
        // Exact memory facts cross only a unique incoming edge, never a join or loop header. Each child owns its
        // mutable copy, so a write on a sibling path cannot clear or populate this path's facts. FP computations
        // remain block-local; integer/address expressions may also follow the general dominator tree.
        MemoryFacts loads = visit.memory ? *visit.memory : MemoryFacts{};
        std::unordered_map<std::string, LirReg> values;
        std::vector<LirInstr> retained;
        for (auto instruction : block.instrs) {
            for (auto &source : instruction.srcs) {
                source = resolve(source);
            }
            const bool load =
                instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && instruction.strArg.empty() &&
                !instruction.isVolatile &&
                (instruction.type.IsInteger() || instruction.type.IsFloat() || instruction.type.IsBool() ||
                 instruction.type.IsChar() || instruction.type.kind == TypeRef::Kind::Pointer);
            const bool constantLoad = load && constantAddresses.contains(instruction.srcs[0]);
            const bool pure = (Pure(instruction.op) && !instruction.isVolatile) || constantLoad;
            // Keep cheap constants local: consumers rematerialize immediates and CFG folding recognizes local
            // constants. Sharing them globally needlessly lengthens allocation intervals and hides those forms.
            const bool stable = pure && instruction.op != LirOpcode::Const && stableType(instruction.type) &&
                                std::ranges::all_of(instruction.srcs, [&](const LirReg source) {
                                    return types.contains(source) && stableType(types.at(source));
                                });
            if (instruction.dst != LirNoReg && (pure || load)) {
                auto &available = load && !constantLoad ? loads : stable ? dominating : values;
                const auto key = Key(instruction, literals, sameBits);
                const auto [entry, inserted] = available.emplace(key, instruction.dst);
                if (!inserted) {
                    copies[instruction.dst] = entry->second;
                    changed = true;
                    continue;
                }
                if (stable)
                    scopedKeys.push_back(key);
            }
            // No alias promise is made about pointers. Any possible write or ordering effect ends load reuse.
            // Reusing an already executed load never introduces a new speculative access or erases the first trap.
            if (!pure && instruction.op != LirOpcode::Load && instruction.op != LirOpcode::Alloca &&
                instruction.op != LirOpcode::Phi) {
                loads.clear();
                values.clear();
            }
            if (instruction.isVolatile) {
                loads.clear();
                values.clear();
            }
            // A typed store establishes one exact address/value pair. Every later possible write or ordering
            // effect invalidates it. Do not forward aggregate bytes or values requiring storage conversion.
            if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 && instruction.strArg.empty() &&
                !instruction.isVolatile && stableType(instruction.type) && types.contains(instruction.srcs[0]) &&
                types.at(instruction.srcs[0]) == instruction.type) {
                LirInstr forwarded;
                forwarded.op = LirOpcode::Load;
                forwarded.type = instruction.type;
                forwarded.srcs = {instruction.srcs[1]};
                loads.emplace(Key(forwarded, literals), instruction.srcs[0]);
            }
            retained.push_back(std::move(instruction));
        }
        block.instrs = std::move(retained);
        std::shared_ptr<const MemoryFacts> snapshot;
        if (!loads.empty() && loads.size() <= 128 && loads.size() <= memoryBudget)
            snapshot = std::make_shared<const MemoryFacts>(std::move(loads));
        for (const auto child : children[visit.block]) {
            std::shared_ptr<const MemoryFacts> incoming;
            if (snapshot && cfg.predecessors[child].size() == 1 && cfg.predecessors[child][0] == visit.block &&
                snapshot->size() <= memoryBudget) {
                memoryBudget -= snapshot->size();
                incoming = snapshot;
            }
            visits.push_back({child, false, 0, std::move(incoming)});
        }
    }
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            for (auto &source : instruction.srcs) {
                source = resolve(source);
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                source = resolve(source);
            }
        }
        if (block.term) {
            block.term->cond = resolve(block.term->cond);
            if (block.term->retVal) {
                block.term->retVal = resolve(*block.term->retVal);
            }
        }
    }
    // A uniquely entered successor records the exact SSA Boolean tested on its incoming edge. Reuse that fact
    // only in its dominator subtree; sibling paths, joins and later loop iterations get no guessed range facts.
    std::unordered_map<LirReg, bool> truths;

    struct Fact {
        LirReg condition;
        std::optional<bool> previous;
    };

    std::vector<Fact> facts;
    visits.clear();
    if (!function.blocks.empty())
        visits.push_back({0, false, 0});
    while (!visits.empty()) {
        const auto visit = visits.back();
        visits.pop_back();
        if (visit.exiting) {
            while (facts.size() > visit.checkpoint) {
                const auto fact = facts.back();
                if (fact.previous)
                    truths[fact.condition] = *fact.previous;
                else
                    truths.erase(fact.condition);
                facts.pop_back();
            }
            continue;
        }
        visits.push_back({visit.block, true, facts.size()});
        for (const auto child : children[visit.block])
            visits.push_back({child, false, 0});
        if (visit.block != 0 && cfg.predecessors[visit.block].size() == 1) {
            const auto &incoming = function.blocks[cfg.predecessors[visit.block][0]].term;
            if (incoming && incoming->kind == LirTermKind::Branch && incoming->trueTarget != incoming->falseTarget) {
                const auto found = truths.find(incoming->cond);
                facts.push_back({incoming->cond, found == truths.end() ? std::nullopt : std::optional{found->second}});
                truths[incoming->cond] = incoming->trueTarget == visit.block;
            }
        }
        auto &term = function.blocks[visit.block].term;
        if (!term || term->kind != LirTermKind::Branch || term->trueTarget == term->falseTarget)
            continue;
        const auto known = truths.find(term->cond);
        if (known == truths.end())
            continue;
        const auto discarded = known->second ? term->falseTarget : term->trueTarget;
        term->trueTarget = known->second ? term->trueTarget : term->falseTarget;
        term->kind = LirTermKind::Jump;
        term->cond = LirNoReg;
        term->falseTarget = 0;
        for (auto &instruction : function.blocks[discarded].instrs) {
            if (instruction.op == LirOpcode::Phi)
                std::erase_if(instruction.phiPreds, [&](const auto &edge) { return edge.second == visit.block; });
        }
        changed = true;
    }
    return changed;
}
} // namespace

std::string_view LirValueNumbering::Name() const noexcept {
    return "lir-local-value-numbering";
}

void LirValueNumbering::Prepare(const LirPackage &package) {
    constantNames = ConstantNames(package);
}

PassChange LirValueNumbering::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    return Number(function, constantNames) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
