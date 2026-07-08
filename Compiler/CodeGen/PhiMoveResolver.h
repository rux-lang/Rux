#pragma once

#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace Rux {

struct PhiMove {
    LirReg dst;
    LirReg src;
    TypeRef type;
};

struct PhiMoveStep {
    enum class Kind {
        SaveDestination,
        Copy,
    };

    Kind kind;
    LirReg dst;
    LirReg src = LirNoReg;
    TypeRef type;
    bool sourceIsTemporary = false;
};

// Convert an SSA parallel copy into a safe sequential schedule. A cycle is
// broken by saving one destination in a temporary stack slot. The caller may
// reuse that slot: all reads from it are scheduled before another save.
inline std::vector<PhiMoveStep> ResolvePhiMoves(std::vector<PhiMove> moves) {
    struct PendingMove {
        PhiMove move;
        bool sourceIsTemporary = false;
    };

    std::vector<PendingMove> pending;
    pending.reserve(moves.size());
    for (auto &move : moves) {
        if (move.dst != move.src) {
            pending.push_back({std::move(move), false});
        }
    }

    std::vector<PhiMoveStep> steps;
    steps.reserve(pending.size() + 1);
    while (!pending.empty()) {
        const auto safe = std::find_if(pending.begin(), pending.end(), [&](const PendingMove &candidate) {
            return std::none_of(pending.begin(), pending.end(), [&](const PendingMove &other) {
                return !other.sourceIsTemporary && other.move.src == candidate.move.dst;
            });
        });

        if (safe != pending.end()) {
            steps.push_back(
                {PhiMoveStep::Kind::Copy, safe->move.dst, safe->move.src, safe->move.type, safe->sourceIsTemporary});
            pending.erase(safe);
            continue;
        }

        // Every remaining destination is still needed as a source, so at
        // least one cycle remains. Preserve one value and redirect all uses.
        const PhiMove &cycleMove = pending.front().move;
        steps.push_back({PhiMoveStep::Kind::SaveDestination, cycleMove.dst, LirNoReg, cycleMove.type, false});
        for (auto &move : pending) {
            if (!move.sourceIsTemporary && move.move.src == cycleMove.dst) {
                move.sourceIsTemporary = true;
            }
        }
    }
    return steps;
}

} // namespace Rux
