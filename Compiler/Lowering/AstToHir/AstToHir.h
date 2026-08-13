#pragma once

#include "Ir/Hir/Hir.h"
#include "Semantic/SemanticModel.h"

namespace Rux {
class AstToHirLowering {
public:
    explicit AstToHirLowering(const SemanticModel &model);
    [[nodiscard]] HirPackage Generate();

private:
    const SemanticModel &semanticModel_;
};
} // namespace Rux
