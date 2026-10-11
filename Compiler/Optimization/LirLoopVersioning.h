#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Versions a bounded innermost unsigned traversal when invariant row ranges prove its checks redundant.
class LirLoopVersioning final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
