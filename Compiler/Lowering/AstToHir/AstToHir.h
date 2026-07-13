#pragma once

#include "Ir/Hir/Hir.h"

#include <vector>

#include "Semantic/SemanticModel.h"

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
