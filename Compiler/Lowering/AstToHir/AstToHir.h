#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Semantic/Model/SemanticModel.h"

#include <vector>

namespace Rux {
/**
 * @brief Lowers the analyzed AST to HIR.
 *
 * The AST records what was written; HIR records what it means. Names are already resolved and types already known by
 * this point, so lowering reads the semantic model rather than re-deriving anything, and syntactic sugar is expanded
 * here so that later stages see one form per construct.
 */
class AstToHirLowering {
public:
    /// `model` must outlive this lowering: it is borrowed, not copied.
    explicit AstToHirLowering(const SemanticModel &model);

    /// Lower every module in the model. Check `Diagnostics` afterwards.
    [[nodiscard]] HirPackage Generate();

    /// Problems found while lowering. Since analysis has already accepted the program, these are internal errors rather
    /// than user-facing ones.
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const noexcept;

private:
    const SemanticModel &semanticModel;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
