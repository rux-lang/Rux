#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Fixed frame addresses whose pattern-binding uses are only conditionally reachable become ordinary dominating
/// definitions. This moves address materialization only, never initialization or object lifetime operations.
class LirFrameAddressPlacement final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;

    [[nodiscard]] bool Enabled(const PassContext &context) const override {
        return context.iteration == 0;
    }
};

class LirSsaVerifier final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
