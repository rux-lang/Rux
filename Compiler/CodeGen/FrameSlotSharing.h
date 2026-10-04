#pragma once

// Which values of a function may keep their storage in the same part of its frame.
//
// Every back end gives each virtual register a frame slot and each alloca a data region. Most of them hold an
// intermediate value for a few instructions, so a slot apiece makes a frame as large as everything the function ever
// computed, and a recursive function pays that once per level. This pass finds values that are never live together
// and lets them share.
//
// Liveness is computed over the control-flow graph, so a value carried around a loop keeps its storage for the whole
// loop. Two values interfere, and never share, when one is written while the other still has a read ahead of it.
//
// Register slots (`ShareFrameSlots`) follow three more rules that cover what a back end does and the instruction list
// does not spell:
//
//   - An instruction's result interferes with its own operands. A call returning an aggregate has its result slot
//     written through the hidden return pointer while its arguments are still being read, and a wide copy reads its
//     source after it starts writing its destination.
//   - A phi is written on the edges into its block, not at its instruction. Its register interferes with everything
//     live out of each predecessor, with what that predecessor's terminator reads, and with every other phi written
//     from that predecessor, which is what lets the phi move schedule keep treating registers as distinct places.
//   - Only a register with exactly one definition takes part. Parameters and alloca addresses are written by the
//     prologue or read for as long as the storage they name, and keep slots of their own.
//
// Alloca data (`ShareAllocaData`) is shared only when the function itself is the only thing that can reach it: every
// use of the address is a load, a store through it, or a field, element or pointer cast of it that is used the same
// way. An address passed to a call, stored as a value, merged by a phi, compared or returned may be kept by something
// this pass cannot see, and that alloca keeps a region of its own until the function returns. A shared alloca is live
// from its own instruction to the last instruction that reads or writes through its address.
//
// The pass decides sharing and nothing else: the caller says which values have storage and how large, and places the
// shared storage in its own frame.

#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux {
/// The outcome of sharing: the shared slot each participating register was given, and the size each shared slot
/// needs, which is that of its largest member.
struct FrameSlotSharing {
    std::unordered_map<LirReg, std::uint32_t> slotOf;
    std::vector<int> slotSizes;
};

namespace FrameSlotSharingDetail {
/// A function reduced to what sharing needs: for each step, the value it writes and the values it reads. A block's
/// terminator is its last step.
struct Program {
    struct Step {
        std::int32_t written = -1;
        std::uint32_t readsBegin = 0;
        std::uint32_t readsEnd = 0;
        bool phi = false;
        std::uint32_t predecessorsBegin = 0;
        std::uint32_t predecessorsEnd = 0;
    };

    std::vector<LirReg> registers;
    std::vector<int> sizes;
    std::vector<std::vector<Step>> blocks;
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> phiPredecessors;
    /// Values a phi of some successor reads on the way out of each block.
    std::vector<std::vector<std::uint32_t>> edgeReads;
    std::vector<std::vector<std::uint32_t>> successors;

    explicit Program(const LirFunc &func)
        : blocks(func.blocks.size())
        , edgeReads(func.blocks.size())
        , successors(func.blocks.size()) {
        for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
            const auto &terminator = func.blocks[blockIndex].term;
            if (!terminator) {
                continue;
            }
            const auto successor = [&](const std::uint32_t target) {
                if (target < func.blocks.size()) {
                    successors[blockIndex].push_back(target);
                }
            };
            switch (terminator->kind) {
            case LirTermKind::Jump:
                successor(terminator->trueTarget);
                break;
            case LirTermKind::Branch:
                successor(terminator->trueTarget);
                successor(terminator->falseTarget);
                break;
            case LirTermKind::Switch:
                successor(terminator->defaultTarget);
                for (const LirSwitchCase &switchCase : terminator->cases) {
                    successor(switchCase.target);
                }
                break;
            case LirTermKind::Return:
            case LirTermKind::Unreachable:
                break;
            }
        }
    }

    std::uint32_t AddValue(const LirReg reg, const int size) {
        registers.push_back(reg);
        sizes.push_back(size);
        return static_cast<std::uint32_t>(registers.size() - 1);
    }
};

/// Assign shared slots to the values of `program`.
[[nodiscard]] inline FrameSlotSharing Solve(const Program &program) {
    using Step = Program::Step;
    FrameSlotSharing sharing;
    const std::size_t valueCount = program.registers.size();
    const std::size_t blockCount = program.blocks.size();
    if (valueCount == 0) {
        return sharing;
    }

    // Where each value is written. A value is local to its block when every read follows its write there;
    // anything else is carried between blocks and needs the dataflow below.
    constexpr std::uint32_t kNone = ~std::uint32_t{};
    std::vector<std::uint32_t> writeBlock(valueCount, kNone);
    std::vector<std::uint32_t> writeStep(valueCount, 0);
    for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const auto &steps = program.blocks[blockIndex];
        for (std::uint32_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
            if (steps[stepIndex].written >= 0) {
                writeBlock[static_cast<std::size_t>(steps[stepIndex].written)] = blockIndex;
                writeStep[static_cast<std::size_t>(steps[stepIndex].written)] = stepIndex;
            }
        }
    }
    std::vector<bool> crosses(valueCount, false);
    for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const auto &steps = program.blocks[blockIndex];
        for (std::uint32_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
            for (std::uint32_t read = steps[stepIndex].readsBegin; read < steps[stepIndex].readsEnd; ++read) {
                const std::uint32_t value = program.reads[read];
                if (writeBlock[value] != blockIndex || stepIndex <= writeStep[value]) {
                    crosses[value] = true;
                }
            }
        }
        for (const std::uint32_t value : program.edgeReads[blockIndex]) {
            crosses[value] = true;
        }
    }
    std::vector<std::uint32_t> crossingBit(valueCount, kNone);
    std::vector<std::uint32_t> crossingValues;
    for (std::uint32_t value = 0; value < valueCount; ++value) {
        if (crosses[value]) {
            crossingBit[value] = static_cast<std::uint32_t>(crossingValues.size());
            crossingValues.push_back(value);
        }
    }

    // Backward dataflow over the values carried between blocks, as bit sets. A phi reads its incoming values on
    // the edge, so they are live out of the predecessor and not live into the phi's own block.
    const std::size_t words = (crossingValues.size() + 63) / 64;
    using Bits = std::vector<std::uint64_t>;
    const auto set = [](Bits &bits, const std::uint32_t bit) { bits[bit / 64] |= std::uint64_t{1} << (bit % 64); };
    const auto test = [](const Bits &bits, const std::uint32_t bit) { return (bits[bit / 64] >> (bit % 64) & 1) != 0; };
    std::vector<Bits> exposed(blockCount, Bits(words));
    std::vector<Bits> written(blockCount, Bits(words));
    std::vector<Bits> edgeRead(blockCount, Bits(words));
    std::vector<Bits> liveIn(blockCount, Bits(words));
    std::vector<Bits> liveOut(blockCount, Bits(words));
    for (std::uint32_t blockIndex = 0; blockIndex < blockCount && words > 0; ++blockIndex) {
        for (const Step &step : program.blocks[blockIndex]) {
            for (std::uint32_t read = step.readsBegin; read < step.readsEnd; ++read) {
                const std::uint32_t bit = crossingBit[program.reads[read]];
                if (bit != kNone && !test(written[blockIndex], bit)) {
                    set(exposed[blockIndex], bit);
                }
            }
            if (step.written >= 0 && crossingBit[static_cast<std::size_t>(step.written)] != kNone) {
                set(written[blockIndex], crossingBit[static_cast<std::size_t>(step.written)]);
            }
        }
        for (const std::uint32_t value : program.edgeReads[blockIndex]) {
            set(edgeRead[blockIndex], crossingBit[value]);
        }
    }
    for (bool changed = words > 0; changed;) {
        changed = false;
        for (std::size_t blockIndex = blockCount; blockIndex-- > 0;) {
            Bits out = edgeRead[blockIndex];
            for (const std::uint32_t target : program.successors[blockIndex]) {
                for (std::size_t word = 0; word < words; ++word) {
                    out[word] |= liveIn[target][word];
                }
            }
            Bits in(words);
            for (std::size_t word = 0; word < words; ++word) {
                in[word] = exposed[blockIndex][word] | (out[word] & ~written[blockIndex][word]);
            }
            if (out != liveOut[blockIndex] || in != liveIn[blockIndex]) {
                liveOut[blockIndex] = std::move(out);
                liveIn[blockIndex] = std::move(in);
                changed = true;
            }
        }
    }

    // Interference, found by walking each block backwards with the set of values that still have a read ahead.
    std::vector<std::vector<std::uint32_t>> interference(valueCount);
    const auto interfere = [&](const std::uint32_t left, const std::uint32_t right) {
        if (left != right) {
            interference[left].push_back(right);
            interference[right].push_back(left);
        }
    };
    const auto liveOutOf = [&](const std::uint32_t blockIndex, auto &&visit) {
        for (const std::uint32_t value : crossingValues) {
            if (test(liveOut[blockIndex], crossingBit[value])) {
                visit(value);
            }
        }
    };

    std::vector<std::uint32_t> live;
    std::vector<std::int32_t> livePosition(valueCount, -1);
    const auto addLive = [&](const std::uint32_t value) {
        if (livePosition[value] < 0) {
            livePosition[value] = static_cast<std::int32_t>(live.size());
            live.push_back(value);
        }
    };
    const auto removeLive = [&](const std::uint32_t value) {
        const std::int32_t position = livePosition[value];
        if (position < 0) {
            return;
        }
        const std::uint32_t last = live.back();
        live[static_cast<std::size_t>(position)] = last;
        livePosition[last] = position;
        live.pop_back();
        livePosition[value] = -1;
    };

    for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const auto &steps = program.blocks[blockIndex];
        for (const std::uint32_t value : live) {
            livePosition[value] = -1;
        }
        live.clear();
        liveOutOf(blockIndex, addLive);

        for (std::size_t stepIndex = steps.size(); stepIndex-- > 0;) {
            const Step &step = steps[stepIndex];
            if (step.written >= 0) {
                const auto result = static_cast<std::uint32_t>(step.written);
                for (const std::uint32_t other : live) {
                    interfere(result, other);
                }
                removeLive(result);
                for (std::uint32_t read = step.readsBegin; read < step.readsEnd; ++read) {
                    interfere(result, program.reads[read]);
                }
            }
            for (std::uint32_t read = step.readsBegin; read < step.readsEnd; ++read) {
                addLive(program.reads[read]);
            }
        }

        // The moves for every edge out of this block may all run at its end, before the terminator picks one, so
        // the phis of all its successors are written together.
        std::vector<std::uint32_t> phis;
        for (const std::uint32_t target : program.successors[blockIndex]) {
            for (const Step &step : program.blocks[target]) {
                if (step.phi && step.written >= 0) {
                    phis.push_back(static_cast<std::uint32_t>(step.written));
                }
            }
        }
        for (std::size_t left = 0; left < phis.size(); ++left) {
            for (std::size_t right = left + 1; right < phis.size(); ++right) {
                interfere(phis[left], phis[right]);
            }
        }
        for (const Step &step : steps) {
            if (!step.phi || step.written < 0) {
                continue;
            }
            const auto result = static_cast<std::uint32_t>(step.written);
            for (std::uint32_t entry = step.predecessorsBegin; entry < step.predecessorsEnd; ++entry) {
                const std::uint32_t predecessor = program.phiPredecessors[entry];
                liveOutOf(predecessor, [&](const std::uint32_t other) { interfere(result, other); });
                if (const auto &predecessorSteps = program.blocks[predecessor]; !predecessorSteps.empty()) {
                    const Step &terminator = predecessorSteps.back();
                    for (std::uint32_t read = terminator.readsBegin; read < terminator.readsEnd; ++read) {
                        interfere(result, program.reads[read]);
                    }
                }
            }
        }
    }

    // Greedy assignment in definition order. A value takes the free slot nearest its own size, so a small value
    // does not pin a large slot while one of its own size is free, and opens a new slot only when every existing
    // one holds something it interferes with.
    std::vector<std::uint32_t> assigned(valueCount, kNone);
    std::vector<std::uint32_t> blockedBy;
    for (std::uint32_t value = 0; value < valueCount; ++value) {
        const int size = program.sizes[value];
        blockedBy.resize(sharing.slotSizes.size(), kNone);
        for (const std::uint32_t other : interference[value]) {
            if (assigned[other] != kNone) {
                blockedBy[assigned[other]] = value;
            }
        }
        std::uint32_t chosen = kNone;
        int chosenDistance = 0;
        for (std::uint32_t slot = 0; slot < sharing.slotSizes.size(); ++slot) {
            if (blockedBy[slot] == value) {
                continue;
            }
            const int distance =
                sharing.slotSizes[slot] >= size ? sharing.slotSizes[slot] - size : 2 * (size - sharing.slotSizes[slot]);
            if (chosen == kNone || distance < chosenDistance) {
                chosen = slot;
                chosenDistance = distance;
            }
        }
        if (chosen == kNone) {
            chosen = static_cast<std::uint32_t>(sharing.slotSizes.size());
            sharing.slotSizes.push_back(size);
        }
        else {
            sharing.slotSizes[chosen] = std::max(sharing.slotSizes[chosen], size);
        }
        assigned[value] = chosen;
        sharing.slotOf.emplace(program.registers[value], chosen);
    }
    return sharing;
}
} // namespace FrameSlotSharingDetail

/// Decide which registers of `func` share a frame slot.
///
/// `slotSize` is called for every instruction that defines a register and returns the bytes its slot needs, or zero
/// for a result that must keep a slot of its own. Slots are numbered in the order their first member is defined, so a
/// function is laid out the same way every time it is compiled.
template <typename SlotSize>
[[nodiscard]] FrameSlotSharing ShareFrameSlots(const LirFunc &func, SlotSize &&slotSize) {
    using FrameSlotSharingDetail::Program;
    std::unordered_set<LirReg> parameters;
    for (const LirParam &parameter : func.params) {
        parameters.insert(parameter.reg);
    }
    std::unordered_map<LirReg, int> definitionCounts;
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                ++definitionCounts[instruction.dst];
            }
        }
    }

    Program program(func);
    std::unordered_map<LirReg, std::uint32_t> valueOf;
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.dst == LirNoReg || instruction.op == LirOpcode::Alloca ||
                parameters.contains(instruction.dst) || definitionCounts[instruction.dst] != 1) {
                continue;
            }
            if (const int size = slotSize(instruction); size > 0) {
                valueOf.emplace(instruction.dst, program.AddValue(instruction.dst, size));
            }
        }
    }
    if (valueOf.empty()) {
        return {};
    }

    const auto read = [&](const LirReg reg) {
        if (const auto found = valueOf.find(reg); found != valueOf.end()) {
            program.reads.push_back(found->second);
        }
    };
    for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
        const LirBlock &block = func.blocks[blockIndex];
        auto &steps = program.blocks[blockIndex];
        steps.reserve(block.instrs.size() + 1);
        for (const LirInstr &instruction : block.instrs) {
            Program::Step step;
            if (const auto found = valueOf.find(instruction.dst); found != valueOf.end()) {
                step.written = static_cast<std::int32_t>(found->second);
            }
            step.readsBegin = static_cast<std::uint32_t>(program.reads.size());
            for (const LirReg source : instruction.srcs) {
                read(source);
            }
            step.readsEnd = static_cast<std::uint32_t>(program.reads.size());
            if (instruction.op == LirOpcode::Phi) {
                step.phi = true;
                step.predecessorsBegin = static_cast<std::uint32_t>(program.phiPredecessors.size());
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    if (predecessor >= func.blocks.size()) {
                        continue;
                    }
                    program.phiPredecessors.push_back(predecessor);
                    if (const auto found = valueOf.find(source); found != valueOf.end()) {
                        program.edgeReads[predecessor].push_back(found->second);
                    }
                }
                step.predecessorsEnd = static_cast<std::uint32_t>(program.phiPredecessors.size());
            }
            steps.push_back(step);
        }
        Program::Step terminator;
        terminator.readsBegin = static_cast<std::uint32_t>(program.reads.size());
        if (block.term) {
            read(block.term->cond);
            if (block.term->retVal) {
                read(*block.term->retVal);
            }
        }
        terminator.readsEnd = static_cast<std::uint32_t>(program.reads.size());
        steps.push_back(terminator);
    }
    return FrameSlotSharingDetail::Solve(program);
}

/// Decide which allocas of `func` share a data region.
///
/// `dataSize` is called for every alloca and returns the bytes its data needs. Only an alloca whose address never
/// leaves the function's own loads and stores takes part; the result is keyed by the alloca's address register.
template <typename DataSize>
[[nodiscard]] FrameSlotSharing ShareAllocaData(const LirFunc &func, DataSize &&dataSize) {
    using FrameSlotSharingDetail::Program;
    // The alloca each address register points into: the alloca's own register, and every field, element or pointer
    // cast taken from one.
    std::unordered_map<LirReg, LirReg> allocaOf;
    std::unordered_set<LirReg> escaped;
    std::unordered_map<LirReg, int> definitionCounts;
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                ++definitionCounts[instruction.dst];
            }
        }
    }
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && instruction.dst != LirNoReg &&
                definitionCounts[instruction.dst] == 1) {
                allocaOf.emplace(instruction.dst, instruction.dst);
            }
        }
    }
    if (allocaOf.empty()) {
        return {};
    }

    const auto derives = [](const LirInstr &instruction, const std::size_t operand) {
        if (operand != 0 || instruction.dst == LirNoReg) {
            return false;
        }
        switch (instruction.op) {
        case LirOpcode::FieldPtr:
        case LirOpcode::IndexPtr:
            return true;
        case LirOpcode::Cast:
            return instruction.type.kind == TypeRef::Kind::Pointer;
        default:
            return false;
        }
    };
    // Addresses are derived in any block order, so the set grows until it stops changing; an address derived twice,
    // or derived into a register something else also writes, makes its alloca escape.
    for (bool changed = true; changed;) {
        changed = false;
        for (const LirBlock &block : func.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                for (std::size_t operand = 0; operand < instruction.srcs.size(); ++operand) {
                    const auto base = allocaOf.find(instruction.srcs[operand]);
                    if (base == allocaOf.end() || !derives(instruction, operand)) {
                        continue;
                    }
                    const LirReg alloca = base->second;
                    if (definitionCounts[instruction.dst] != 1) {
                        changed = escaped.insert(alloca).second || changed;
                        continue;
                    }
                    const auto [existing, inserted] = allocaOf.emplace(instruction.dst, alloca);
                    if (inserted) {
                        changed = true;
                    }
                    else if (existing->second != alloca) {
                        changed = escaped.insert(alloca).second || changed;
                        changed = escaped.insert(existing->second).second || changed;
                    }
                }
            }
        }
    }
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            for (std::size_t operand = 0; operand < instruction.srcs.size(); ++operand) {
                const auto base = allocaOf.find(instruction.srcs[operand]);
                if (base == allocaOf.end()) {
                    continue;
                }
                const bool loaded = instruction.op == LirOpcode::Load && operand == 0;
                const bool storedThrough = instruction.op == LirOpcode::Store && operand == 1;
                if (!loaded && !storedThrough && !derives(instruction, operand)) {
                    escaped.insert(base->second);
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                if (const auto base = allocaOf.find(source); base != allocaOf.end()) {
                    escaped.insert(base->second);
                }
            }
        }
        if (block.term) {
            if (const auto base = allocaOf.find(block.term->cond); base != allocaOf.end()) {
                escaped.insert(base->second);
            }
            if (block.term->retVal) {
                if (const auto base = allocaOf.find(*block.term->retVal); base != allocaOf.end()) {
                    escaped.insert(base->second);
                }
            }
        }
    }

    Program program(func);
    std::unordered_map<LirReg, std::uint32_t> valueOf;
    for (const LirBlock &block : func.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Alloca || !allocaOf.contains(instruction.dst) ||
                escaped.contains(instruction.dst)) {
                continue;
            }
            if (const int size = dataSize(instruction); size > 0) {
                valueOf.emplace(instruction.dst, program.AddValue(instruction.dst, size));
            }
        }
    }
    if (valueOf.empty()) {
        return {};
    }

    for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
        const LirBlock &block = func.blocks[blockIndex];
        auto &steps = program.blocks[blockIndex];
        steps.reserve(block.instrs.size() + 1);
        for (const LirInstr &instruction : block.instrs) {
            Program::Step step;
            step.readsBegin = static_cast<std::uint32_t>(program.reads.size());
            if (instruction.op == LirOpcode::Alloca) {
                if (const auto found = valueOf.find(instruction.dst); found != valueOf.end()) {
                    step.written = static_cast<std::int32_t>(found->second);
                }
            }
            else {
                for (const LirReg source : instruction.srcs) {
                    if (const auto base = allocaOf.find(source); base != allocaOf.end()) {
                        if (const auto found = valueOf.find(base->second); found != valueOf.end()) {
                            program.reads.push_back(found->second);
                        }
                    }
                }
            }
            step.readsEnd = static_cast<std::uint32_t>(program.reads.size());
            steps.push_back(step);
        }
        Program::Step terminator;
        terminator.readsBegin = terminator.readsEnd = static_cast<std::uint32_t>(program.reads.size());
        steps.push_back(terminator);
    }
    return FrameSlotSharingDetail::Solve(program);
}
} // namespace Rux
