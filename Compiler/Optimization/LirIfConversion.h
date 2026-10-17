#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Turns small conditional updates inside loops into arithmetic on the condition.
///
/// The shape is a triangle: a block branches on a Boolean either to a side block or straight to the merge block, the
/// side block only computes integer values without effects or traps and jumps to the merge, and every value the
/// merge receives is an integer. The side block's instructions move above the branch and each merged value becomes
/// a function of the condition's 0 or 1: `x + 1` taken conditionally is `x + c`, `x + y` is `x + (y & -c)`, and any
/// other pair `a`, `b` is `a ^ ((a ^ b) & -c)`. The branch disappears, so a condition that follows no pattern, such
/// as whether a number is prime, no longer costs a misprediction on every change.
class LirIfConversion final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
