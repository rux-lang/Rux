#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Advances affine row addresses in simple unsigned loops without changing memory operation order.
class LirPointerInduction final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
