#pragma once

#include "Ir/Lir/Dominance.h"

#include <cstdint>
#include <map>
#include <unordered_set>

namespace Rux {
/// Whether `function` can have a natural loop at all. A cycle among blocks needs at least one edge to a block at or
/// before its source in block order, so without such an edge there is no loop, and a loop pass can skip the function
/// before building its dominator tree.
inline bool MayHaveLoops(const LirFunc &function) {
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        const auto &term = function.blocks[block].term;
        if (!term)
            continue;
        switch (term->kind) {
        case LirTermKind::Jump:
            if (term->trueTarget <= block)
                return true;
            break;
        case LirTermKind::Branch:
            if (term->trueTarget <= block || term->falseTarget <= block)
                return true;
            break;
        case LirTermKind::Switch:
            if (term->defaultTarget <= block)
                return true;
            for (const auto &switchCase : term->cases)
                if (switchCase.target <= block)
                    return true;
            break;
        case LirTermKind::Return:
        case LirTermKind::Unreachable:
            break;
        }
    }
    return false;
}

/// Immutable natural-loop membership. Rebuild after CFG changes. Irreducible cycles are intentionally not natural
/// loops. A work limit keeps deeply nested/generated CFGs bounded; clients must check complete before using results.
struct LirNaturalLoops {
    std::map<std::uint32_t, std::unordered_set<std::uint32_t>> members;
    bool complete = true;

    LirNaturalLoops(const LirControlFlow &cfg, const LirDominance &dominance, const std::size_t workLimit = 2000000) {
        std::size_t workDone = 0;
        for (std::uint32_t latch = 0; latch < cfg.successors.size(); ++latch) {
            if (!cfg.reachable[latch])
                continue;
            for (const auto header : cfg.successors[latch]) {
                if (!dominance.Dominates(header, latch))
                    continue;
                auto &loop = members[header];
                loop.insert(header);
                std::vector<std::uint32_t> work{latch};
                while (!work.empty()) {
                    if (++workDone > workLimit) {
                        members.clear();
                        complete = false;
                        return;
                    }
                    const auto block = work.back();
                    work.pop_back();
                    if (!loop.insert(block).second)
                        continue;
                    for (const auto predecessor : cfg.predecessors[block])
                        if (cfg.reachable[predecessor])
                            work.push_back(predecessor);
                }
            }
        }
    }
};
} // namespace Rux
