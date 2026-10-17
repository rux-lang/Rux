#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Removes instructions whose results nothing reads. An instruction is only a candidate when it has no effect worth
/// keeping, so a store, a call, or an operation that can trap survives even with an unused result: the point of
/// evaluating it was never the value.
class LirDeadCodeElimination final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
