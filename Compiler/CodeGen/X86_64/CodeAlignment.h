#pragma once

#include "Ir/Lir/Lir.h"

#include <vector>

namespace Rux {
inline constexpr std::uint16_t X86CodeAlignment = 32;

// Backward branch destinations are likely loop entries. This is a layout heuristic,
// not a claim about dominance or loop semantics. Entry zero keeps the existing prologue label.
[[nodiscard]] inline std::vector<bool> X86AlignedBlocks(const LirFunc &function) {
    std::vector<bool> aligned(function.blocks.size(), false);
    for (std::size_t source = 0; source < function.blocks.size(); ++source) {
        const auto &terminator = function.blocks[source].term;
        if (!terminator) {
            continue;
        }
        const auto mark = [&](const std::uint32_t destination) {
            if (destination != 0 && destination <= source) {
                aligned[destination] = true;
            }
        };
        switch (terminator->kind) {
        case LirTermKind::Jump:
            mark(terminator->trueTarget);
            break;
        case LirTermKind::Branch:
            mark(terminator->trueTarget);
            mark(terminator->falseTarget);
            break;
        case LirTermKind::Switch:
            mark(terminator->defaultTarget);
            for (const auto &item : terminator->cases) {
                mark(item.target);
            }
            break;
        default:
            break;
        }
    }
    return aligned;
}
} // namespace Rux
