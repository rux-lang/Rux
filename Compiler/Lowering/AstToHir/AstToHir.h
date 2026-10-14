#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Parallel/WorkerPool.h"
#include "Semantic/Model/SemanticModel.h"

#include <unordered_set>
#include <utility>
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

    /// Threads modules may be lowered on; null lowers them on the calling thread. The package never depends on it.
    void SetWorkerPool(Parallel::WorkerPool *inputPool) noexcept {
        pool = inputPool;
    }

    /// Lower only the declarations of these modules and the signatures of their non-generic functions, whose compiled
    /// code the build reuses; analysis checked no body of theirs but the generic ones.
    void SetDeclarationOnlyModules(std::unordered_set<const Module *> inputModules) {
        declarationOnlyModules = std::move(inputModules);
    }

private:
    const SemanticModel &semanticModel;
    std::vector<Diagnostic> diagnostics;
    Parallel::WorkerPool *pool = nullptr;
    std::unordered_set<const Module *> declarationOnlyModules;
};
} // namespace Rux
