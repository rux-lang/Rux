#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Promote private, consistently typed scalar storage with a defined value on every read path.
class LirMemoryPromotion final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
