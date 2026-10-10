#pragma once

#include "CodeGen/LinearScan.h"
#include "Ir/Lir/NaturalLoops.h"

namespace Rux {
/// Static use density, weighted toward nested loops. These are allocation hints, never semantic facts. Phi inputs
/// belong to their predecessor edges. Large CFGs retain unweighted counts instead of performing unbounded analysis.
[[nodiscard]] inline std::unordered_map<LirReg, double>
ComputeSpillPriorities(const LirFunc &function, const std::vector<LiveInterval> &intervals) {
    std::vector<std::uint64_t> weights(function.blocks.size(), 1);
    if (function.blocks.size() <= 4096) {
        const LirControlFlow cfg(function);
        const LirDominance dominance(cfg);
        const LirNaturalLoops loops(cfg, dominance);
        if (loops.complete) {
            for (const auto &[header, members] : loops.members) {
                static_cast<void>(header);
                for (const auto block : members)
                    weights[block] = std::min<std::uint64_t>(65536, weights[block] * 8);
            }
        }
    }
    std::unordered_map<LirReg, double> uses;
    const auto add = [&](const LirReg reg, const std::size_t block) {
        if (reg != LirNoReg && block < weights.size())
            uses[reg] += static_cast<double>(weights[block]);
    };
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            add(instruction.dst, block);
            for (const auto source : instruction.srcs)
                add(source, block);
            for (const auto &[source, predecessor] : instruction.phiPreds)
                add(source, predecessor);
        }
        if (const auto &term = function.blocks[block].term) {
            add(term->cond, block);
            if (term->retVal)
                add(*term->retVal, block);
        }
    }
    for (const auto &interval : intervals) {
        const auto length = std::max<std::int64_t>(1, static_cast<std::int64_t>(interval.end) - interval.start + 1);
        uses[interval.reg] /= static_cast<double>(length);
    }
    return uses;
}
} // namespace Rux
