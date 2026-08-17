#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Optimization/Pass.h"

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

private:
    ArtifactKind artifactKind;
};
} // namespace Rux::Optimization
