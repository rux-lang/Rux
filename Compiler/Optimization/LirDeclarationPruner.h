#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Optimization/LirReachability.h"
#include "Optimization/Pass.h"

#include <memory>
#include <vector>

namespace Rux::Optimization {
/**
 * @brief Drops the declarations reachability could not reach.
 *
 * This is the pass that keeps an artifact from carrying every function of every dependency it linked against. It
 * removes whole declarations rather than instructions, which is why it runs after the passes that shrink bodies: a
 * function only becomes unreachable once the call that named it is gone.
 *
 * What it removed is recorded in the run report's pruning statistics for the `--stats` report.
 */
class LirDeclarationPruner final : public LirPass {
public:
    explicit LirDeclarationPruner(ArtifactKind inputArtifactKind);

    [[nodiscard]] std::string_view Name() const noexcept override {
        return "lir-declaration-pruner";
    }

    PassChange Run(LirPackage &package, const PassContext &context) override;

    /// Pruning removes whole declarations and rewrites no function body.
    [[nodiscard]] bool ReportsChangedFunctions() const noexcept override {
        return true;
    }

    /// Prune every module from its own roots (`ModuleRoots`, one list per module) through its own references only,
    /// rather than walking the whole package from the artifact's roots.
    void SetModuleRoots(std::shared_ptr<const std::vector<std::vector<LirModuleRoot>>> roots) noexcept {
        moduleRoots = std::move(roots);
    }

private:
    ArtifactKind artifactKind;
    std::shared_ptr<const std::vector<std::vector<LirModuleRoot>>> moduleRoots;
};
} // namespace Rux::Optimization
