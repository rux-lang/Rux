#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Inline bounded, straight-line scalar leaf functions. Runs once per optimization invocation.
class LirInliner final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
