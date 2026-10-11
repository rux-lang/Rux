#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Duplicates a small straight-line unsigned loop body in execution order, retaining a scalar tail.
class LirLoopUnrolling final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
