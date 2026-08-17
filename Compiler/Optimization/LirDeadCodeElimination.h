#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Removes instructions whose results nothing reads. An instruction is only a candidate when it has no effect worth
/// keeping, so a store, a call, or an operation that can trap survives even with an unused result: the point of
/// evaluating it was never the value.
class LirDeadCodeElimination final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
