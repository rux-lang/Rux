#pragma once

#include "Ir/Lir/Lir.h"

namespace Rux {
/// Whether `op` is a two-lane vector instruction.
[[nodiscard]] bool IsVectorOpcode(LirOpcode op) noexcept;

/// Whether a function holds any two-lane vector instruction.
[[nodiscard]] bool HasVectorInstructions(const LirFunc &function) noexcept;

/// Rewrites every vector instruction of `function` into the two scalar instructions it stands for, lane 0 then lane
/// 1, for a back end that does not select vectors. Each vector value becomes a pair of scalar values; a vector load
/// or store reads or writes the element at its pointer and the one after it. The order the vectorizer chose stays,
/// which is the order it proved equivalent to the loop it came from.
void Devectorize(LirFunc &function);
} // namespace Rux
