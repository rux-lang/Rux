#pragma once

#include "Ir/Lir/Lir.h"

namespace Rux {
/// Whether a function holds any overflow-checked instruction.
[[nodiscard]] bool HasCheckedArithmetic(const LirFunc &function) noexcept;

/// Rewrites every overflow-checked instruction of `function` into the wrapping operation it computes, a comparison
/// that tells whether the exact result fit, and a branch to a block that reports the instruction's message at its
/// source location and stops, for a back end that does not test the machine's flags. A sum or difference is checked
/// by the signs of its operands and result, or for an unsigned type by comparing with an operand; a product is taken
/// at twice the width and checked by narrowing it back. The block holding the instruction is split there, so its
/// successors' phis name the block that now ends it.
void ExpandCheckedArithmetic(LirFunc &function);
} // namespace Rux
