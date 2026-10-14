#include "Optimization/LirSsaVerifier.h"

#include "Ir/Lir/Dominance.h"

#include <deque>
#include <format>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
struct Definition {
    std::uint32_t block;
    std::size_t position;
    bool parameter;
    /// The function's own type for the register, or the pointer type an allocation's address has; never copied.
    const TypeRef *type;
};

bool PlaceFrameAddresses(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.empty()) {
        return false;
    }
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    std::unordered_map<LirReg, std::pair<std::uint32_t, std::size_t>> addresses;
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (std::size_t index = 0; index < function.blocks[block].instrs.size(); ++index) {
            const auto &instruction = function.blocks[block].instrs[index];
            if (instruction.op == LirOpcode::Alloca && instruction.srcs.empty() && !instruction.isVolatile) {
                addresses.emplace(instruction.dst, std::pair{block, index});
            }
        }
    }
    std::unordered_set<LirReg> move;
    const auto use = [&](const LirReg reg, const std::uint32_t block, const std::size_t position) {
        const auto found = addresses.find(reg);
        if (found == addresses.end() || !cfg.reachable[block]) {
            return;
        }
        const auto [definitionBlock, definitionPosition] = found->second;
        if (!dominance.Dominates(definitionBlock, block) ||
            (definitionBlock == block && definitionPosition >= position)) {
            move.insert(reg);
        }
    };
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        const auto &instructions = function.blocks[block].instrs;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            for (const auto source : instructions[index].srcs) {
                use(source, block, index);
            }
            for (const auto &[source, predecessor] : instructions[index].phiPreds) {
                if (predecessor < function.blocks.size()) {
                    use(source, predecessor, function.blocks[predecessor].instrs.size());
                }
            }
        }
        if (const auto &term = function.blocks[block].term) {
            use(term->cond, block, instructions.size());
            if (term->retVal) {
                use(*term->retVal, block, instructions.size());
            }
        }
    }
    if (move.empty()) {
        return false;
    }
    std::vector<LirInstr> relocated;
    for (auto &block : function.blocks) {
        std::erase_if(block.instrs, [&](const auto &instruction) {
            if (instruction.op == LirOpcode::Alloca && move.contains(instruction.dst)) {
                relocated.push_back(instruction);
                return true;
            }
            return false;
        });
    }
    function.blocks[0].instrs.insert(function.blocks[0].instrs.begin(), relocated.begin(), relocated.end());
    return true;
}

// A borrowed pattern binding (or a residual payload to destroy) can carry an address across its boolean match
// merge. Materialize that pure address at the common dominator. Never speculate the payload load or destructor.
bool PlacePatternAddresses(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.empty()) {
        return false;
    }
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    bool changed = false;
    for (;;) {
        using Location = std::pair<std::uint32_t, std::size_t>;
        std::map<LirReg, Location> definitions;
        std::unordered_set<LirReg> parameters;
        // Only the kind of a register's type is asked about here.
        std::unordered_map<LirReg, TypeRef::Kind> types;
        std::map<LirReg, std::vector<Location>> uses;
        for (const auto &parameter : function.params) {
            parameters.insert(parameter.reg);
            types.emplace(parameter.reg, parameter.type.kind);
        }
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            const auto &instructions = function.blocks[block].instrs;
            for (std::size_t index = 0; index < instructions.size(); ++index) {
                const auto &instruction = instructions[index];
                if (instruction.dst != LirNoReg) {
                    definitions.emplace(instruction.dst, Location{block, index});
                    types.emplace(instruction.dst, instruction.op == LirOpcode::Alloca ? TypeRef::Kind::Pointer
                                                                                       : instruction.type.kind);
                }
                if (!cfg.reachable[block]) {
                    continue;
                }
                for (const auto source : instruction.srcs) {
                    uses[source].emplace_back(block, index);
                }
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    if (predecessor < function.blocks.size() && cfg.reachable[predecessor]) {
                        uses[source].emplace_back(predecessor, function.blocks[predecessor].instrs.size());
                    }
                }
            }
            if (cfg.reachable[block] && function.blocks[block].term) {
                const auto &term = *function.blocks[block].term;
                uses[term.cond].emplace_back(block, instructions.size());
                if (term.retVal) {
                    uses[*term.retVal].emplace_back(block, instructions.size());
                }
            }
        }
        bool moved = false;
        for (const auto &[reg, locations] : uses) {
            const auto definition = definitions.find(reg);
            if (definition == definitions.end() || types.at(reg) != TypeRef::Kind::Pointer) {
                continue;
            }
            auto target = definition->second.first;
            if (!cfg.reachable[target]) {
                continue;
            }
            for (const auto &[block, position] : locations) {
                static_cast<void>(position);
                while (!dominance.Dominates(target, block)) {
                    target = dominance.ImmediateDominators()[target];
                }
            }
            if (target == definition->second.first) {
                continue;
            }
            auto insertion = function.blocks[target].instrs.size();
            for (const auto &[block, position] : locations) {
                if (block == target) {
                    insertion = std::min(insertion, position);
                }
            }
            std::vector<LirInstr> relocated;
            std::unordered_set<LirReg> visited;
            const auto collect = [&] {
                // Iterative dependency ordering also handles deep generated patterns without recursive host stacks.
                std::vector<std::pair<LirReg, bool>> pending{{reg, false}};
                std::unordered_set<LirReg> complete;
                while (!pending.empty()) {
                    const auto [source, finish] = pending.back();
                    pending.pop_back();
                    if (parameters.contains(source) || complete.contains(source)) {
                        continue;
                    }
                    const auto found = definitions.find(source);
                    if (found == definitions.end()) {
                        return false;
                    }
                    const auto [block, position] = found->second;
                    if (dominance.Dominates(block, target) && (block != target || position < insertion)) {
                        continue;
                    }
                    const auto &instruction = function.blocks[block].instrs[position];
                    if (finish) {
                        relocated.push_back(instruction);
                        complete.insert(source);
                        continue;
                    }
                    if (!visited.insert(source).second) {
                        return false; // A malformed definition cycle remains for the verifier to diagnose.
                    }
                    const bool address =
                        instruction.type.kind == TypeRef::Kind::Pointer &&
                        (instruction.op == LirOpcode::FieldPtr || instruction.op == LirOpcode::IndexPtr ||
                         (instruction.op == LirOpcode::Cast && instruction.srcs.size() == 1 &&
                          types.contains(instruction.srcs[0]) &&
                          types.at(instruction.srcs[0]) == TypeRef::Kind::Pointer));
                    const bool offset = instruction.op == LirOpcode::Const && instruction.type.IsInteger();
                    if (instruction.isVolatile || (!address && !offset)) {
                        return false;
                    }
                    pending.emplace_back(source, true);
                    for (auto operand = instruction.srcs.rbegin(); operand != instruction.srcs.rend(); ++operand) {
                        pending.emplace_back(*operand, false);
                    }
                }
                return true;
            };
            if (!collect()) {
                continue;
            }
            for (auto &block : function.blocks) {
                std::erase_if(block.instrs, [&](const auto &instruction) { return visited.contains(instruction.dst); });
            }
            auto &instructions = function.blocks[target].instrs;
            instructions.insert(instructions.begin() + static_cast<std::ptrdiff_t>(insertion), relocated.begin(),
                                relocated.end());
            changed = true;
            moved = true;
            // Every moved definition goes to a strict ancestor, so normalization terminates without limiting valid
            // source programs to a fixed number of patterns. Rebuild positions; the CFG/dominators are unchanged.
            break;
        }
        if (!moved) {
            break;
        }
    }
    return changed;
}

void Verify(const LirFunc &function, const PassContext &context) {
    if (function.isExtern || function.isAsm || function.blocks.empty()) {
        return;
    }
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const auto report = [&](const LirReg reg, const std::uint32_t block, const std::string_view reason) {
        context.ReportInternalError(std::format("internal LIR SSA error in function '{}', block {}, register %{}: {}",
                                                function.name, block, reg, reason));
    };
    std::unordered_map<LirReg, Definition> definitions;
    // The pointer types allocations' addresses have, which the function holds nowhere itself.
    std::deque<TypeRef> addressTypes;
    for (const auto &parameter : function.params) {
        if (!definitions.emplace(parameter.reg, Definition{0, 0, true, &parameter.type}).second) {
            report(parameter.reg, 0, "more than one definition");
        }
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        const auto &instructions = function.blocks[block].instrs;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            const auto &instruction = instructions[index];
            if (instruction.dst == LirNoReg) {
                continue;
            }
            const TypeRef *type = &instruction.type;
            if (instruction.op == LirOpcode::Alloca)
                type = &addressTypes.emplace_back(TypeRef::MakePointer(instruction.type));
            if (!definitions.emplace(instruction.dst, Definition{block, index, false, type}).second) {
                report(instruction.dst, block, "more than one definition");
            }
        }
    }
    const auto use = [&](const LirReg reg, const std::uint32_t block, const std::size_t position) {
        if (reg == LirNoReg) {
            report(reg, block, "missing operand");
            return;
        }
        const auto found = definitions.find(reg);
        if (found == definitions.end()) {
            report(reg, block, "use has no definition");
            return;
        }
        const auto &definition = found->second;
        if (definition.parameter || !cfg.reachable[block]) {
            return;
        }
        if (!dominance.Dominates(definition.block, block) ||
            (definition.block == block && definition.position >= position)) {
            report(reg, block, "definition does not dominate this use");
        }
    };
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        const auto &instructions = function.blocks[block].instrs;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            const auto &instruction = instructions[index];
            if (instruction.op == LirOpcode::Sqrt &&
                (instruction.dst == LirNoReg || instruction.srcs.size() != 1 ||
                 (instruction.type.kind != TypeRef::Kind::Float32 && instruction.type.kind != TypeRef::Kind::Float64) ||
                 (instruction.srcs.size() == 1 && definitions.contains(instruction.srcs[0]) &&
                  *definitions.at(instruction.srcs[0]).type != instruction.type))) {
                report(instruction.dst, block, "sqrt requires one same-type float32/float64 operand and a result");
            }
            for (const auto source : instruction.srcs) {
                use(source, block, index);
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                if (predecessor < function.blocks.size()) {
                    use(source, predecessor, function.blocks[predecessor].instrs.size());
                }
                if (const auto definition = definitions.find(source);
                    definition != definitions.end() && *definition->second.type != instruction.type) {
                    report(source, block,
                           std::format("phi operand type '{}' differs from result '{}'",
                                       definition->second.type->ToString(), instruction.type.ToString()));
                }
            }
        }
        if (const auto &term = function.blocks[block].term) {
            if (term->kind == LirTermKind::Branch || term->kind == LirTermKind::Switch) {
                use(term->cond, block, instructions.size());
            }
            if (term->retVal) {
                use(*term->retVal, block, instructions.size());
            }
        }
    }
}
} // namespace

std::string_view LirFrameAddressPlacement::Name() const noexcept {
    return "lir-frame-address-placement";
}

PassChange LirFrameAddressPlacement::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    const bool frames = PlaceFrameAddresses(function);
    const bool patterns = PlacePatternAddresses(function);
    return frames || patterns ? PassChange::Changed : PassChange::None;
}

std::string_view LirSsaVerifier::Name() const noexcept {
    return "lir-ssa-verifier";
}

PassChange LirSsaVerifier::RunFunction(LirFunc &function, const std::size_t, const PassContext &context) const {
    Verify(function, context);
    return PassChange::None;
}
} // namespace Rux::Optimization
