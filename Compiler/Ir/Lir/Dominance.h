#pragma once

#include "Ir/Lir/ControlFlow.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Rux {
/// Immutable CFG analysis. Reconstruct after changing edges or block indices; instruction-only edits do not affect it.
class LirDominance {
public:
    static constexpr std::uint32_t Invalid = std::numeric_limits<std::uint32_t>::max();

    explicit LirDominance(const LirControlFlow &cfg)
        : blocks(static_cast<std::uint32_t>(cfg.successors.size())) {
        // One block holds the immediate dominators, the reverse-postorder positions, the order itself and the walk's
        // stack: this is built for nearly every function in every pass that asks about dominance.
        storage.assign(static_cast<std::size_t>(blocks) * 5, Invalid);
        if (blocks == 0) {
            return;
        }
        std::uint32_t *const immediate = Immediate();
        std::uint32_t *const order = Order();
        std::uint32_t *const postorder = storage.data() + 2 * static_cast<std::size_t>(blocks);
        std::uint32_t *const stack = storage.data() + 3 * static_cast<std::size_t>(blocks);
        // The walk's stack holds a block and the next successor to try; a block is pushed once, when first reached.
        std::uint32_t depth = 0;
        const auto push = [&](const std::uint32_t block) {
            order[block] = 0;
            stack[2 * depth] = block;
            stack[2 * depth + 1] = 0;
            ++depth;
        };
        push(0);
        while (depth != 0) {
            const std::uint32_t block = stack[2 * (depth - 1)];
            std::uint32_t &successor = stack[2 * (depth - 1) + 1];
            const auto successors = cfg.successors[block];
            if (successor == successors.size()) {
                postorder[count++] = block;
                --depth;
                continue;
            }
            const auto next = successors[successor++];
            if (order[next] == Invalid) {
                push(next);
            }
        }
        std::reverse(postorder, postorder + count);
        for (std::uint32_t index = 0; index < count; ++index) {
            order[postorder[index]] = index;
        }
        immediate[0] = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t block = postorder[index];
                if (block == 0) {
                    continue;
                }
                auto parent = Invalid;
                for (const auto predecessor : cfg.predecessors[block]) {
                    if (immediate[predecessor] != Invalid) {
                        parent = parent == Invalid ? predecessor : Intersect(parent, predecessor);
                    }
                }
                if (parent != immediate[block]) {
                    immediate[block] = parent;
                    changed = true;
                }
            }
        }
    }

    [[nodiscard]] bool Dominates(const std::uint32_t dominator, std::uint32_t block) const {
        const std::uint32_t *const order = Order();
        const std::uint32_t *const immediate = Immediate();
        if (dominator >= blocks || block >= blocks || order[dominator] == Invalid || order[block] == Invalid) {
            return false;
        }
        while (order[block] > order[dominator]) {
            block = immediate[block];
        }
        return block == dominator;
    }

    /// The blocks reachable from the entry, in reverse postorder.
    [[nodiscard]] std::span<const std::uint32_t> ReversePostorder() const {
        return {storage.data() + 2 * static_cast<std::size_t>(blocks), count};
    }

    /// Each block's immediate dominator, `Invalid` for an unreachable block; the entry is its own.
    [[nodiscard]] std::span<const std::uint32_t> ImmediateDominators() const {
        return {storage.data(), blocks};
    }

private:
    std::uint32_t blocks = 0;
    std::uint32_t count = 0;
    std::vector<std::uint32_t> storage;

    [[nodiscard]] std::uint32_t *Immediate() {
        return storage.data();
    }

    [[nodiscard]] const std::uint32_t *Immediate() const {
        return storage.data();
    }

    [[nodiscard]] std::uint32_t *Order() {
        return storage.data() + blocks;
    }

    [[nodiscard]] const std::uint32_t *Order() const {
        return storage.data() + blocks;
    }

    [[nodiscard]] std::uint32_t Intersect(std::uint32_t left, std::uint32_t right) const {
        const std::uint32_t *const order = Order();
        const std::uint32_t *const immediate = Immediate();
        while (left != right) {
            while (order[left] > order[right]) {
                left = immediate[left];
            }
            while (order[right] > order[left]) {
                right = immediate[right];
            }
        }
        return left;
    }
};
} // namespace Rux
