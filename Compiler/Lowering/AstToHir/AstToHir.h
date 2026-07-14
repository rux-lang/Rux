#pragma once

#include "Ir/Hir/Hir.h"
#include "Semantic/SemanticModel.h"

#include <vector>

namespace Rux {
class AstToHirLowering {
public:
    explicit AstToHirLowering(const SemanticModel &model);
    [[nodiscard]] HirPackage Generate();

private:
    std::vector<const Module *> modules_;
    CompileTimeContext compileTimeContext_;
};
} // namespace Rux
