#pragma once

// The register allocation both back ends run over a function before they emit
// it: where each virtual register is live, and which of them are worth keeping
// in a machine register rather than reading out of the frame at every mention.
//
// Allocation runs before emission. Without priorities a full pool leaves the
// incoming value in memory; optional priorities can instead spill an entire
// lower-priority interval. Every backend still provides those memory homes.
// There are no split intervals or halfway changes to an emitted value's home.
//
// Two things are the caller's rather than this pass's, because they are
// properties of a machine and not of a program: which intervals are candidates
// at all — a register file has classes, and a value too wide for one register
// belongs to none of them — and how many registers the pool it is allocating
// from holds.

#include "Ir/Lir/ControlFlow.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Rux {
/// The span of the numbering over which one virtual register is live: the instruction that first mentioned it and the
/// one that last did, with the type it holds, which is what decides the register file it could live in.
struct LiveInterval {
    LirReg reg = LirNoReg;
    int start = -1;
    int end = -1;
    /// How many times the function names this register, definitions and uses alike. A back end whose registers have to
    /// be saved and restored spends a fixed price for each one it takes and is paid back once per mention, so this is
    /// what says whether a particular value is worth taking one for.
    int mentions = 0;
    TypeRef type;
};

/// The type each parameter is read at, which is the one thing about a function's registers this pass cannot work out
/// for itself: Win64 passes a large composite as the address of the caller's copy, so the register holds a pointer
/// where the parameter's own type says otherwise.
using ParamTypeMap = std::unordered_map<LirReg, TypeRef>;

/// Where each virtual register the allocation reached ended up: an index into the caller's pool rather than a machine
/// register number, since the pool is the caller's to name. `usedPhysRegs` is ascending and holds each index once,
/// which is the order a prologue saves them in and an epilogue restores them.
struct RegisterAssignment {
    std::unordered_map<LirReg, int> physRegs;
    std::vector<int> usedPhysRegs;
};

/// Number every instruction and terminator of the function in the order they are emitted, and report the interval each
/// virtual register is live over.
///
/// Extend textual mentions over live block boundaries, including back edges. Phi destinations overlap every source,
/// destination and live-out value on their predecessor edges, so virtual parallel copies remain valid after allocation.
///
/// The intervals come back sorted by where they open, and by register number where two open together, so a function
/// allocates the same way every time it is compiled.
[[nodiscard]] inline std::vector<LiveInterval> ComputeLiveIntervals(const LirFunc &func,
                                                                    const ParamTypeMap &paramTypes) {
    std::unordered_map<LirReg, TypeRef> regTypes;
    std::unordered_map<LirReg, LiveInterval> intervals;
    std::vector<int> starts, ends;
    int position = 0;
    for (const auto &block : func.blocks) {
        starts.push_back(position);
        for (const auto &instr : block.instrs) {
            if (instr.dst != LirNoReg) {
                regTypes[instr.dst] = instr.op == LirOpcode::Alloca ? TypeRef::MakePointer(instr.type) : instr.type;
            }
            ++position;
        }
        ends.push_back(position++);
    }

    /// A register mentioned before anything gave it a type is an integer, which is what a back end assumes of one as
    /// well.
    const auto typeOf = [&](const LirReg reg) {
        const auto it = regTypes.find(reg);
        return it != regTypes.end() ? it->second : TypeRef::MakeInt64();
    };
    const auto mention = [&](const LirReg reg, const int index) {
        if (reg == LirNoReg) {
            return;
        }
        const auto it = intervals.find(reg);
        if (it == intervals.end()) {
            intervals.emplace(reg, LiveInterval{reg, index, index, 1, typeOf(reg)});
            return;
        }
        it->second.start = std::min(it->second.start, index);
        it->second.end = std::max(it->second.end, index);
        ++it->second.mentions;
    };

    /// A parameter is live from the entry rather than from its first mention: the prologue writes it before any
    /// instruction has run.
    for (const auto &param : func.params) {
        const auto found = paramTypes.find(param.reg);
        regTypes[param.reg] = found != paramTypes.end() ? found->second : param.type;
        intervals.emplace(param.reg, LiveInterval{param.reg, 0, 0, 1, regTypes[param.reg]});
    }

    int index = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instrs) {
            for (const LirReg src : instr.srcs) {
                mention(src, index);
            }
            if (instr.dst != LirNoReg) {
                mention(instr.dst, index);
            }
            if (instr.op == LirOpcode::Phi) {
                for (const auto &[src, pred] : instr.phiPreds) {
                    if (pred < ends.size()) {
                        mention(src, ends[pred]);
                        mention(instr.dst, ends[pred]);
                    }
                }
            }
            ++index;
        }
        if (block.term) {
            if (block.term->cond != LirNoReg) {
                mention(block.term->cond, index);
            }
            if (block.term->retVal && *block.term->retVal != LirNoReg) {
                mention(*block.term->retVal, index);
            }
        }
        ++index;
    }

    const LirControlFlow cfg(func);
    const LirLiveness liveness(func, cfg);
    const auto extend = [&](const LirReg reg, const int point) {
        if (const auto found = intervals.find(reg); found != intervals.end()) {
            found->second.start = std::min(found->second.start, point);
            found->second.end = std::max(found->second.end, point);
        }
    };
    for (std::size_t block = 0; block < func.blocks.size(); ++block) {
        for (const auto reg : liveness.liveIn[block]) {
            extend(reg, starts[block]);
        }
        for (const auto reg : liveness.liveOut[block]) {
            extend(reg, ends[block]);
        }
    }

    std::vector<LiveInterval> result;
    result.reserve(intervals.size());
    for (const auto &[reg, interval] : intervals) {
        result.push_back(interval);
    }
    std::sort(result.begin(), result.end(), [](const LiveInterval &a, const LiveInterval &b) {
        return a.start != b.start ? a.start < b.start : a.reg < b.reg;
    });
    return result;
}

/// Hand out `poolSize` registers to as many of the candidate intervals as they reach, in the order the intervals open.
///
/// Each register remembers where the interval holding it ends, and the first one free at an interval's start takes it.
/// Preferring the lowest free index rather than the least recently freed is what keeps a function that needs two
/// registers to two: the pool is saved and restored by the prologue, so an index never handed out costs nothing at all.
///
/// Optional priorities permit whole-interval eviction. Removing a home before emission makes every definition and use
/// of the evicted value follow the existing stack path. Previously tied endpoints still interfere with newcomers.
[[nodiscard]] inline RegisterAssignment AllocateRegisters(const std::vector<LiveInterval> &candidates,
                                                          const int poolSize,
                                                          const std::unordered_map<LirReg, LirReg> &tiedInputs = {},
                                                          const std::unordered_map<LirReg, double> &priorities = {}) {
    RegisterAssignment assignment;
    std::vector<int> freeAfter(static_cast<std::size_t>(std::max(poolSize, 0)), -1);
    std::vector<LirReg> occupants(freeAfter.size(), LirNoReg);
    std::vector<int> previousEnd(freeAfter.size(), -1);
    const auto priority = [&](const LirReg reg) {
        const auto found = priorities.find(reg);
        return found == priorities.end() ? 0.0 : found->second;
    };
    for (const auto &interval : candidates) {
        // A selector may explicitly permit its output to overwrite one input after its last read. The interval
        // must open exactly at that read, and the input must still own the register. Never relax phi interference
        // or general endpoint overlap merely because two intervals happen to meet.
        if (const auto tied = tiedInputs.find(interval.reg); tied != tiedInputs.end()) {
            if (const auto home = assignment.physRegs.find(tied->second); home != assignment.physRegs.end()) {
                const auto index = static_cast<std::size_t>(home->second);
                if (occupants[index] == tied->second && freeAfter[index] == interval.start) {
                    assignment.physRegs[interval.reg] = home->second;
                    previousEnd[index] = freeAfter[index];
                    occupants[index] = interval.reg;
                    freeAfter[index] = interval.end;
                    continue;
                }
            }
        }
        for (std::size_t r = 0; r < freeAfter.size(); ++r) {
            if (freeAfter[r] >= interval.start) {
                continue;
            }
            const auto index = static_cast<int>(r);
            assignment.physRegs[interval.reg] = index;
            previousEnd[r] = freeAfter[r];
            occupants[r] = interval.reg;
            freeAfter[r] = interval.end;
            if (std::find(assignment.usedPhysRegs.begin(), assignment.usedPhysRegs.end(), index) ==
                assignment.usedPhysRegs.end()) {
                assignment.usedPhysRegs.push_back(index);
            }
            break;
        }
        if (!priorities.empty() && !assignment.physRegs.contains(interval.reg)) {
            std::size_t victim = freeAfter.size();
            double lowest = priority(interval.reg);
            for (std::size_t r = 0; r < freeAfter.size(); ++r) {
                const double value = priority(occupants[r]);
                // A tied predecessor may still be read at this point. Spilling only its successor cannot permit an
                // unrelated definition to overwrite that predecessor's physical value.
                if (previousEnd[r] < interval.start && value < lowest) {
                    victim = r;
                    lowest = value;
                }
            }
            if (victim != freeAfter.size()) {
                assignment.physRegs.erase(occupants[victim]);
                assignment.physRegs[interval.reg] = static_cast<int>(victim);
                occupants[victim] = interval.reg;
                freeAfter[victim] = interval.end;
            }
        }
    }
    std::sort(assignment.usedPhysRegs.begin(), assignment.usedPhysRegs.end());
    return assignment;
}
} // namespace Rux
