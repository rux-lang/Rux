#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Folds exact typed loads from immutable constant slice headers, without assuming that other memory is immutable.
class LirConstantMemory final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
