#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Optimization/Pass.h"

namespace Rux::Optimization {
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
