#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Advances affine row addresses in simple unsigned loops without changing memory operation order.
class LirPointerInduction final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;

    [[nodiscard]] bool Enabled(const PassContext &context) const override {
        return context.iteration == 0;
    }
};
} // namespace Rux::Optimization
