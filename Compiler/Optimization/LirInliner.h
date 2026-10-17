#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Inline bounded scalar functions in callee-first order, including branches and multiple returns. Runs once per phase.
class LirInliner final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;

    [[nodiscard]] bool ReportsChangedFunctions() const noexcept override {
        return true;
    }
};
} // namespace Rux::Optimization
