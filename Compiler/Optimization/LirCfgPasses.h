#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
class LirCfgVerifier final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};

class LirCfgCleanup final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
