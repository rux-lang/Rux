#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Fixed frame addresses whose pattern-binding uses are only conditionally reachable become ordinary dominating
/// definitions. This moves address materialization only, never initialization or object lifetime operations.
class LirFrameAddressPlacement final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};

class LirSsaVerifier final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
