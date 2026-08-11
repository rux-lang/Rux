#pragma once

// The register allocation both back ends run over a function before they emit
// it: where each virtual register is live, and which of them are worth keeping
// in a machine register rather than reading out of the frame at every mention.
//
// The allocation is linear scan over a numbering of the instructions, without
// eviction: an interval that finds no free register simply keeps its stack
// slot, which every back end still gives it. That is what makes the pass safe
// to run ahead of instruction selection — nothing it decides can fail later,
// and a register it did not hand out costs a load rather than a wrong answer.
//
// Two things are the caller's rather than this pass's, because they are
// properties of a machine and not of a program: which intervals are candidates
// at all — a register file has classes, and a value too wide for one register
// belongs to none of them — and how many registers the pool it is allocating
// from holds.

#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Rux {
// The span of the numbering over which one virtual register is live: the
// instruction that first mentioned it and the one that last did, with the type
// it holds, which is what decides the register file it could live in.
struct LiveInterval {
    LirReg reg = LirNoReg;
    int start = -1;
    int end = -1;
    // How many times the function names this register, definitions and uses
    // alike. A back end whose registers have to be saved and restored spends a
    // fixed price for each one it takes and is paid back once per mention, so
    // this is what says whether a particular value is worth taking one for.
    int mentions = 0;
    TypeRef type;
};

// The type each parameter is read at, which is the one thing about a function's
// registers this pass cannot work out for itself: Win64 passes a large
// composite as the address of the caller's copy, so the register holds a
// pointer where the parameter's own type says otherwise.
using ParamTypeMap = std::unordered_map<LirReg, TypeRef>;

// Where each virtual register the allocation reached ended up: an index into
// the caller's pool rather than a machine register number, since the pool is
// the caller's to name. `usedPhysRegs` is ascending and holds each index once,
// which is the order a prologue saves them in and an epilogue restores them.
struct RegisterAssignment {
    std::unordered_map<LirReg, int> physRegs;
    std::vector<int> usedPhysRegs;
};

// Number every instruction and terminator of the function in the order they are
// emitted, and report the interval each virtual register is live over.
//
// A mention is a definition or a use, and both ends move: the first mention
// opens the interval and every later one extends it. A phi's incoming values
// count as mentions at the phi itself, which is later than the edge that
// carries them — deliberately, since a longer interval can only make the
// allocation more conservative, and the edge is not a point in this numbering.
//
// The intervals come back sorted by where they open, and by register number
// where two open together, so a function allocates the same way every time it
// is compiled.
[[nodiscard]] inline std::vector<LiveInterval> ComputeLiveIntervals(const LirFunc &func,
                                                                    const ParamTypeMap &paramTypes) {
    std::unordered_map<LirReg, TypeRef> regTypes;
    std::unordered_map<LirReg, LiveInterval> intervals;

    // A register mentioned before anything gave it a type is an integer, which
    // is what a back end assumes of one as well.
    const auto typeOf = [&](const LirReg reg) {
        const auto it = regTypes.find(reg);
        return it != regTypes.end() ? it->second : TypeRef::MakeInt64();
    };
    const auto mention = [&](const LirReg reg, const int index) {
        const auto it = intervals.find(reg);
        if (it == intervals.end()) {
            intervals.emplace(reg, LiveInterval{reg, index, index, 1, typeOf(reg)});
            return;
        }
        it->second.end = index;
        ++it->second.mentions;
    };

    // A parameter is live from the entry rather than from its first mention:
    // the prologue writes it before any instruction has run.
    for (const auto &param : func.params) {
        const auto found = paramTypes.find(param.reg);
        regTypes[param.reg] = found != paramTypes.end() ? found->second : param.type;
        intervals.emplace(param.reg, LiveInterval{param.reg, 0, 0, 1, regTypes[param.reg]});
    }

    int index = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instrs) {
            // An alloca's register holds the address of what it reserved rather
            // than a value of the type it names.
            if (instr.dst != LirNoReg) {
                regTypes[instr.dst] = instr.op == LirOpcode::Alloca ? TypeRef::MakePointer(instr.type) : instr.type;
            }
            for (const LirReg src : instr.srcs) {
                mention(src, index);
            }
            if (instr.dst != LirNoReg) {
                mention(instr.dst, index);
            }
            if (instr.op == LirOpcode::Phi) {
                for (const auto &[src, pred] : instr.phiPreds) {
                    mention(src, index);
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
            ++index;
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

// Hand out `poolSize` registers to as many of the candidate intervals as they
// reach, in the order the intervals open.
//
// Each register remembers where the interval holding it ends, and the first one
// free at an interval's start takes it. Preferring the lowest free index rather
// than the least recently freed is what keeps a function that needs two
// registers to two: the pool is saved and restored by the prologue, so an index
// never handed out costs nothing at all.
//
// Nothing is evicted. An interval that finds every register busy keeps the
// stack slot the caller gave it, and the back end reads it there.
[[nodiscard]] inline RegisterAssignment AllocateRegisters(const std::vector<LiveInterval> &candidates,
                                                          const int poolSize) {
    RegisterAssignment assignment;
    std::vector<int> freeAfter(static_cast<std::size_t>(std::max(poolSize, 0)), -1);
    for (const auto &interval : candidates) {
        for (std::size_t r = 0; r < freeAfter.size(); ++r) {
            if (freeAfter[r] >= interval.start) {
                continue;
            }
            const auto index = static_cast<int>(r);
            assignment.physRegs[interval.reg] = index;
            freeAfter[r] = interval.end;
            if (std::find(assignment.usedPhysRegs.begin(), assignment.usedPhysRegs.end(), index) ==
                assignment.usedPhysRegs.end()) {
                assignment.usedPhysRegs.push_back(index);
            }
            break;
        }
    }
    std::sort(assignment.usedPhysRegs.begin(), assignment.usedPhysRegs.end());
    return assignment;
}
} // namespace Rux
