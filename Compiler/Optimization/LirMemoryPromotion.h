#pragma once

#include "Optimization/Pass.h"
#include "Optimization/ReferenceTypes.h"

#include <optional>

namespace Rux::Optimization {
/// Promote private, consistently typed scalar storage with a defined value on every read path.
class LirMemoryPromotion final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
    void Prepare(const LirPackage &package) override;

private:
    std::optional<ConcreteReferenceTypes> references;
};
} // namespace Rux::Optimization
