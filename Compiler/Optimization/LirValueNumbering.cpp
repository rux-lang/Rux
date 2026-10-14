#include "Optimization/LirValueNumbering.h"

#include "Ir/Lir/Dominance.h"
#include "Ir/Lir/RegisterTable.h"
#include "Optimization/ConstantAddresses.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

/// The literal each constant register holds, as a number shared by every register holding an equal literal.
using LiteralKeys = LirRegisterTable<std::uint32_t>;
/// The source of each conversion between two integer types of one width and signedness, such as `uint` and `uint64`:
/// the value's bits do not change, so an operand read through it is the same operand.
using SameBits = LirRegisterTable<LirReg>;

struct KeyHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

/// Available values by key. Keyed by text so a lookup can use the one buffer every key is built in.
using ValueTable = std::unordered_map<std::string, LirReg, KeyHash, std::equal_to<>>;

/// Append `value` in as few bytes as it needs, seven bits a byte with the top bit marking that more follow.
void AppendLength(std::string &key, std::size_t value) {
    while (value >= 0x80) {
        key += static_cast<char>(0x80 | (value & 0x7F));
        value >>= 7;
    }
    key += static_cast<char>(value);
}

void AppendWord(std::string &key, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        key += static_cast<char>((value >> shift) & 0xFF);
}

/// Write the key of `instruction` to `key`, replacing what it held: equal keys mean equal values. Every part is either
/// of fixed size or preceded by its length, so two different instructions never share a key. The encoding is binary to
/// keep the key of an operation on two registers short enough to need no heap storage of its own.
void BuildKey(std::string &key, const LirInstr &instruction, const LiteralKeys *literals, const SameBits *sameBits) {
    key.clear();
    const auto op = static_cast<std::uint32_t>(instruction.op);
    if (op < 0xFF) {
        key += static_cast<char>(op);
    }
    else {
        key += static_cast<char>(0xFF);
        AppendWord(key, op);
    }
    // A primitive is spelled by its kind alone, so its kind stands in for the spelling.
    const TypeRef &type = instruction.type;
    if (type->kind <= TypeRef::Kind::Float512 && type->name.empty() && type->inner.empty() && !type->arrayLength) {
        key += 'p';
        key += static_cast<char>(static_cast<unsigned>(type->kind));
    }
    else {
        const auto spelling = type.ToString();
        key += 's';
        AppendLength(key, spelling.size());
        key += spelling;
    }
    AppendLength(key, instruction.strArg.size());
    key += instruction.strArg;
    for (auto source : instruction.srcs) {
        // Only an operand is looked through; the instruction's own type still separates different results.
        if (instruction.op != LirOpcode::Cast && sameBits != nullptr)
            for (int depth = 0; depth < 4; ++depth) {
                const LirReg *found = sameBits->Find(source);
                if (found == nullptr)
                    break;
                source = *found;
            }
        if (const std::uint32_t *literal = literals == nullptr ? nullptr : literals->Find(source)) {
            key += 'c';
            AppendWord(key, *literal);
        }
        else {
            key += 'r';
            AppendWord(key, source);
        }
    }
}

bool Number(LirFunc &function, const std::unordered_set<std::string> &constantNames) {
    // A load from read-only constant data is a pure value of its address, so it is shared like arithmetic.
    const std::unordered_set<LirReg> constantAddresses = ConstantAddresses(function, constantNames);
    // The type each register is defined with. It points at the function's own parameters and instructions, which stay
    // where they are until every block is visited: instructions found redundant are only marked, and removed at the
    // end.
    const std::size_t registers = RegisterCountHint(function);
    LirRegisterTable<const TypeRef *> types(registers);
    LiteralKeys literals(registers);
    for (const auto &parameter : function.params) {
        if (!types.Insert(parameter.reg, &parameter.type)) {
            return false;
        }
    }
    std::string key;
    // Each distinct literal's number, in the order the literals first appear.
    ValueTable literalNumbers;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg && !types.Insert(instruction.dst, &instruction.type)) {
                return false;
            }
            if (instruction.dst != LirNoReg && instruction.op == LirOpcode::Const && !instruction.isVolatile &&
                instruction.srcs.empty() &&
                (instruction.type.IsInteger() || instruction.type.IsBool() || instruction.type.IsChar())) {
                // Preserve local constant definitions while recognizing equal typed operands.
                BuildKey(key, instruction, nullptr, nullptr);
                auto found = literalNumbers.find(std::string_view(key));
                if (found == literalNumbers.end())
                    found = literalNumbers.emplace(key, static_cast<LirReg>(literalNumbers.size())).first;
                literals.Insert(instruction.dst, found->second);
            }
        }
    }
    SameBits sameBits(registers);
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Cast || instruction.isVolatile || instruction.srcs.size() != 1 ||
                instruction.dst == LirNoReg || !types.Contains(instruction.srcs[0]))
                continue;
            const TypeRef &from = **types.Find(instruction.srcs[0]);
            const TypeRef &to = instruction.type;
            if (from.IsInteger() && to.IsInteger() && from.IsSigned() == to.IsSigned() && from.SizeInBytes() &&
                from.SizeInBytes() == to.SizeInBytes())
                sameBits.Insert(instruction.dst, instruction.srcs[0]);
        }
    LirRegisterTable<LirReg> copies(registers);
    const auto resolve = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < copies.Size(); ++depth) {
            const LirReg *found = copies.Find(reg);
            if (found == nullptr) {
                break;
            }
            reg = *found;
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

    using MemoryFacts = ValueTable;

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
    ValueTable dominating;
    // Keys of `dominating` entries in scope, viewing the entries' own keys, which stay put until the entry is erased.
    std::vector<std::string_view> scopedKeys;
    // Instructions found redundant, by block, removed once every block is visited.
    std::vector<std::vector<bool>> redundant(function.blocks.size());
    std::size_t memoryBudget = 65536;
    const auto stableType = [](const TypeRef &type) {
        return type.IsInteger() || type.IsBool() || type.IsChar() || type->kind == TypeRef::Kind::Pointer ||
               type->kind == TypeRef::Kind::Reference;
    };
    // Reused from block to block: even an empty table allocates with some standard libraries.
    MemoryFacts loads;
    ValueTable values;
    while (!visits.empty()) {
        const auto visit = visits.back();
        visits.pop_back();
        if (visit.exiting) {
            while (scopedKeys.size() > visit.checkpoint) {
                dominating.erase(dominating.find(scopedKeys.back()));
                scopedKeys.pop_back();
            }
            continue;
        }
        visits.push_back({visit.block, true, scopedKeys.size()});
        auto &block = function.blocks[visit.block];
        // Exact memory facts cross only a unique incoming edge, never a join or loop header. Each child owns its
        // mutable copy, so a write on a sibling path cannot clear or populate this path's facts. FP computations
        // remain block-local; integer/address expressions may also follow the general dominator tree.
        if (visit.memory)
            loads = *visit.memory;
        else
            loads.clear();
        values.clear();
        for (std::size_t index = 0; index < block.instrs.size(); ++index) {
            auto &instruction = block.instrs[index];
            for (auto &source : instruction.srcs) {
                source = resolve(source);
            }
            const bool load =
                instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && instruction.strArg.empty() &&
                !instruction.isVolatile &&
                (instruction.type.IsInteger() || instruction.type.IsFloat() || instruction.type.IsBool() ||
                 instruction.type.IsChar() || instruction.type->kind == TypeRef::Kind::Pointer);
            const bool constantLoad = load && constantAddresses.contains(instruction.srcs[0]);
            const bool pure = (Pure(instruction.op) && !instruction.isVolatile) || constantLoad;
            // Keep cheap constants local: consumers rematerialize immediates and CFG folding recognizes local
            // constants. Sharing them globally needlessly lengthens allocation intervals and hides those forms.
            const bool stable = pure && instruction.op != LirOpcode::Const && stableType(instruction.type) &&
                                std::ranges::all_of(instruction.srcs, [&](const LirReg source) {
                                    const TypeRef *const *type = types.Find(source);
                                    return type != nullptr && stableType(**type);
                                });
            if (instruction.dst != LirNoReg && (pure || load)) {
                auto &available = load && !constantLoad ? loads : stable ? dominating : values;
                BuildKey(key, instruction, &literals, &sameBits);
                if (const auto found = available.find(std::string_view(key)); found != available.end()) {
                    copies.Assign(instruction.dst, found->second);
                    changed = true;
                    if (redundant[visit.block].empty())
                        redundant[visit.block].resize(block.instrs.size(), false);
                    redundant[visit.block][index] = true;
                    continue;
                }
                const auto entry = available.emplace(key, instruction.dst).first;
                if (stable)
                    scopedKeys.emplace_back(entry->first);
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
                !instruction.isVolatile && stableType(instruction.type) && types.Contains(instruction.srcs[0]) &&
                **types.Find(instruction.srcs[0]) == instruction.type) {
                LirInstr forwarded;
                forwarded.op = LirOpcode::Load;
                forwarded.type = instruction.type;
                forwarded.srcs = {instruction.srcs[1]};
                BuildKey(key, forwarded, &literals, nullptr);
                if (!loads.contains(std::string_view(key)))
                    loads.emplace(key, instruction.srcs[0]);
            }
        }
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
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (redundant[blockIndex].empty())
            continue;
        auto &instructions = function.blocks[blockIndex].instrs;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < instructions.size(); ++index)
            if (!redundant[blockIndex][index]) {
                if (kept != index)
                    instructions[kept] = std::move(instructions[index]);
                ++kept;
            }
        instructions.erase(instructions.begin() + static_cast<std::ptrdiff_t>(kept), instructions.end());
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
                instruction.phiPreds.EraseIf([&](const auto &edge) { return edge.second == visit.block; });
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
