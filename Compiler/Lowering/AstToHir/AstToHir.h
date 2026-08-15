#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Semantic/SemanticModel.h"

#include <vector>

namespace Rux {
class AstToHirLowering {
public:
    explicit AstToHirLowering(const SemanticModel &model);
    [[nodiscard]] HirPackage Generate();
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const noexcept;

private:
    const SemanticModel &semanticModel_;
    std::vector<Diagnostic> diagnostics_;
};
} // namespace Rux
