#pragma once

#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux {
/// A per-function snapshot. Rebuild after changing terminators or block numbering.
struct LirControlFlow {
    std::vector<std::vector<std::uint32_t>> successors;
    std::vector<std::vector<std::uint32_t>> predecessors;
    std::vector<bool> reachable;

    explicit LirControlFlow(const LirFunc &function)
        : successors(function.blocks.size())
        , predecessors(function.blocks.size())
        , reachable(function.blocks.size(), false) {
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            const auto add = [&](const std::uint32_t target) {
                if (target < function.blocks.size() &&
                    std::ranges::find(successors[block], target) == successors[block].end()) {
                    successors[block].push_back(target);
                    predecessors[target].push_back(block);
                }
            };
            if (const auto &term = function.blocks[block].term) {
                switch (term->kind) {
                case LirTermKind::Jump:
                    add(term->trueTarget);
                    break;
                case LirTermKind::Branch:
                    add(term->trueTarget);
                    add(term->falseTarget);
                    break;
                case LirTermKind::Switch:
                    add(term->defaultTarget);
                    for (const auto &entry : term->cases) {
                        add(entry.target);
                    }
                    break;
                case LirTermKind::Return:
                case LirTermKind::Unreachable:
                    break;
                }
            }
        }
        if (function.blocks.empty()) {
            return;
        }
        std::vector<std::uint32_t> work{0};
        reachable[0] = true;
        while (!work.empty()) {
            const auto block = work.back();
            work.pop_back();
            for (const auto next : successors[block]) {
                if (!reachable[next]) {
                    reachable[next] = true;
                    work.push_back(next);
                }
            }
        }
    }
};

/// Phi operands are reads on predecessor edges. The snapshot is invalid after any instruction change.
struct LirLiveness {
    using Registers = std::unordered_set<LirReg>;
    std::vector<Registers> liveIn;
    std::vector<Registers> liveOut;

    LirLiveness(const LirFunc &function, const LirControlFlow &cfg)
        : liveIn(function.blocks.size())
        , liveOut(function.blocks.size()) {
        const auto count = function.blocks.size();
        std::vector<Registers> uses(count), defs(count), edgeUses(count);
        for (std::size_t block = 0; block < count; ++block) {
            const auto read = [&](const LirReg reg) {
                if (reg != LirNoReg && !defs[block].contains(reg)) {
                    uses[block].insert(reg);
                }
            };
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto reg : instruction.srcs) {
                    read(reg);
                }
                for (const auto &[reg, predecessor] : instruction.phiPreds) {
                    if (reg != LirNoReg && predecessor < count) {
                        edgeUses[predecessor].insert(reg);
                    }
                }
                if (instruction.dst != LirNoReg) {
                    defs[block].insert(instruction.dst);
                }
            }
            if (const auto &term = function.blocks[block].term) {
                read(term->cond);
                if (term->retVal) {
                    read(*term->retVal);
                }
            }
        }
        std::vector<std::uint32_t> work;
        std::vector<bool> queued(count, true);
        for (std::uint32_t block = 0; block < count; ++block) {
            work.push_back(block);
        }
        while (!work.empty()) {
            const auto block = work.back();
            work.pop_back();
            queued[block] = false;
            Registers outgoing = edgeUses[block];
            for (const auto next : cfg.successors[block]) {
                outgoing.insert(liveIn[next].begin(), liveIn[next].end());
            }
            Registers incoming = uses[block];
            for (const auto reg : outgoing) {
                if (!defs[block].contains(reg)) {
                    incoming.insert(reg);
                }
            }
            liveOut[block] = std::move(outgoing);
            if (incoming != liveIn[block]) {
                liveIn[block] = std::move(incoming);
                for (const auto pred : cfg.predecessors[block]) {
                    if (!queued[pred]) {
                        queued[pred] = true;
                        work.push_back(pred);
                    }
                }
            }
        }
    }
};
} // namespace Rux
