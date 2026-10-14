#pragma once

#include "Ir/Lir/Lir.h"
#include "Ir/Lir/RegisterTable.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace Rux {
/// One list of blocks for each block of a function, stored back to back in one array.
class LirBlockLists {
public:
    [[nodiscard]] std::span<const std::uint32_t> operator[](const std::size_t block) const {
        return {items.data() + starts[block], starts[block + 1] - starts[block]};
    }

    /// How many blocks have a list.
    [[nodiscard]] std::size_t size() const {
        return starts.empty() ? 0 : starts.size() - 1;
    }

    [[nodiscard]] bool empty() const {
        return size() == 0;
    }

private:
    friend struct LirControlFlow;

    std::vector<std::uint32_t> items;
    std::vector<std::size_t> starts;
};

/// A per-function snapshot. Rebuild after changing terminators or block numbering.
struct LirControlFlow {
    /// Each block's distinct successors, in the order its terminator names them.
    LirBlockLists successors;
    /// Each block's predecessors, in block order.
    LirBlockLists predecessors;
    std::vector<bool> reachable;

    explicit LirControlFlow(const LirFunc &function)
        : reachable(function.blocks.size(), false) {
        const std::size_t count = function.blocks.size();
        auto &targets = successors.items;
        targets.reserve(count * 2);
        successors.starts.assign(count + 1, 0);
        for (std::uint32_t block = 0; block < count; ++block) {
            const auto add = [&](const std::uint32_t target) {
                if (target < count && std::find(targets.begin() + static_cast<std::ptrdiff_t>(successors.starts[block]),
                                                targets.end(), target) == targets.end()) {
                    targets.push_back(target);
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
            successors.starts[block + 1] = targets.size();
        }
        // Predecessors by counting: each block's list fills in block order, as appending edge by edge would.
        predecessors.starts.assign(count + 1, 0);
        for (const auto target : targets)
            ++predecessors.starts[target + 1];
        for (std::size_t block = 0; block < count; ++block)
            predecessors.starts[block + 1] += predecessors.starts[block];
        predecessors.items.resize(targets.size());
        std::vector<std::size_t> filled(predecessors.starts.begin(), predecessors.starts.end() - 1);
        for (std::uint32_t block = 0; block < count; ++block)
            for (const auto target : successors[block])
                predecessors.items[filled[target]++] = block;
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
    /// Sorted, without duplicates.
    using Registers = std::vector<LirReg>;
    std::vector<Registers> liveIn;
    std::vector<Registers> liveOut;

    LirLiveness(const LirFunc &function, const LirControlFlow &cfg)
        : liveIn(function.blocks.size())
        , liveOut(function.blocks.size()) {
        const auto count = function.blocks.size();
        std::vector<Registers> uses(count), defs(count), edgeUses(count);
        // The block most recently seen defining each register, plus one, which tells a read of a value the block
        // defined earlier from a read of one live on entry.
        LirRegisterTable<std::uint32_t> definedIn(RegisterCountHint(function));
        for (std::size_t block = 0; block < count; ++block) {
            const auto mark = static_cast<std::uint32_t>(block + 1);
            const auto read = [&](const LirReg reg) {
                if (reg == LirNoReg)
                    return;
                const std::uint32_t *defined = definedIn.Find(reg);
                if (defined == nullptr || *defined != mark)
                    uses[block].push_back(reg);
            };
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto reg : instruction.srcs) {
                    read(reg);
                }
                for (const auto &[reg, predecessor] : instruction.phiPreds) {
                    if (reg != LirNoReg && predecessor < count) {
                        edgeUses[predecessor].push_back(reg);
                    }
                }
                if (instruction.dst != LirNoReg) {
                    definedIn.Assign(instruction.dst, mark);
                    defs[block].push_back(instruction.dst);
                }
            }
            if (const auto &term = function.blocks[block].term) {
                read(term->cond);
                if (term->retVal) {
                    read(*term->retVal);
                }
            }
        }
        const auto normalize = [](Registers &registers) {
            std::ranges::sort(registers);
            registers.erase(std::ranges::unique(registers).begin(), registers.end());
        };
        for (std::size_t block = 0; block < count; ++block) {
            normalize(uses[block]);
            normalize(defs[block]);
            normalize(edgeUses[block]);
        }
        std::vector<std::uint32_t> work;
        std::vector<bool> queued(count, true);
        for (std::uint32_t block = 0; block < count; ++block) {
            work.push_back(block);
        }
        Registers outgoing, incoming, scratch;
        while (!work.empty()) {
            const auto block = work.back();
            work.pop_back();
            queued[block] = false;
            outgoing = edgeUses[block];
            for (const auto next : cfg.successors[block]) {
                scratch.clear();
                std::ranges::set_union(outgoing, liveIn[next], std::back_inserter(scratch));
                std::swap(outgoing, scratch);
            }
            scratch.clear();
            std::ranges::set_difference(outgoing, defs[block], std::back_inserter(scratch));
            incoming.clear();
            std::ranges::set_union(uses[block], scratch, std::back_inserter(incoming));
            liveOut[block] = outgoing;
            if (incoming != liveIn[block]) {
                liveIn[block] = incoming;
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
