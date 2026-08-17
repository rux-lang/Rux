#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Replaces a register with the constant or the earlier register it is known to hold. Tracking copies as well as
/// constants is what lets the passes after it see through the temporaries lowering introduces; on its own it removes
/// nothing, leaving the now-unused definitions to dead-code elimination.
class LirConstantPropagation final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
