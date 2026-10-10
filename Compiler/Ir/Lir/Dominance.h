#pragma once

#include "Ir/Lir/ControlFlow.h"

#include <limits>
#include <utility>

namespace Rux {
/// Immutable CFG analysis. Reconstruct after changing edges or block indices; instruction-only edits do not affect it.
class LirDominance {
public:
    static constexpr std::uint32_t Invalid = std::numeric_limits<std::uint32_t>::max();

    explicit LirDominance(const LirControlFlow &cfg)
        : immediate(cfg.successors.size(), Invalid)
        , order(cfg.successors.size(), Invalid) {
        if (cfg.successors.empty()) {
            return;
        }
        std::vector<bool> visited(cfg.successors.size(), false);
        std::vector<std::pair<std::uint32_t, std::size_t>> stack{{0, 0}};
        visited[0] = true;
        while (!stack.empty()) {
            auto &[block, successor] = stack.back();
            if (successor == cfg.successors[block].size()) {
                reversePostorder.push_back(block);
                stack.pop_back();
                continue;
            }
            const auto next = cfg.successors[block][successor++];
            if (!visited[next]) {
                visited[next] = true;
                stack.emplace_back(next, 0);
            }
        }
        std::ranges::reverse(reversePostorder);
        for (std::uint32_t index = 0; index < reversePostorder.size(); ++index) {
            order[reversePostorder[index]] = index;
        }
        immediate[0] = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto block : reversePostorder) {
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
        if (dominator >= order.size() || block >= order.size() || order[dominator] == Invalid ||
            order[block] == Invalid) {
            return false;
        }
        while (order[block] > order[dominator]) {
            block = immediate[block];
        }
        return block == dominator;
    }

    [[nodiscard]] const std::vector<std::uint32_t> &ReversePostorder() const {
        return reversePostorder;
    }

    [[nodiscard]] const std::vector<std::uint32_t> &ImmediateDominators() const {
        return immediate;
    }

private:
    std::vector<std::uint32_t> immediate;
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> reversePostorder;

    [[nodiscard]] std::uint32_t Intersect(std::uint32_t left, std::uint32_t right) const {
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
