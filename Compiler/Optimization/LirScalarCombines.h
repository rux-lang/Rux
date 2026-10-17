#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
class LirScalarCombines final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
