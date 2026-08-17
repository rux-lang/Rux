#include "Optimization/Pipeline.h"

#include "Optimization/HirConstantFolder.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirConstantPropagation.h"
#include "Optimization/LirDeadCodeElimination.h"
#include "Optimization/LirDeclarationPruner.h"

#include <memory>

namespace Rux::Optimization {
OptimizationPipeline::OptimizationPipeline(const BuildProfile profile, const std::size_t fixedPointLimit)
    : hir(profile, fixedPointLimit, "HIR")
    , lir(profile, fixedPointLimit, "LIR") {
}

OptimizationPipeline OptimizationPipeline::ForProfile(const BuildProfile profile, const std::size_t fixedPointLimit) {
    OptimizationPipeline pipeline(profile, fixedPointLimit);
    pipeline.lir.Add(std::make_unique<LirCfgVerifier>());
    if (profile == BuildProfile::Release) {
        pipeline.hir.Add(std::make_unique<HirConstantFolder>());
        pipeline.lir.Add(std::make_unique<LirConstantPropagation>());
        pipeline.lir.Add(std::make_unique<LirDeadCodeElimination>());
        pipeline.lir.Add(std::make_unique<LirCfgCleanup>());
    }
    return pipeline;
}

OptimizationPipeline OptimizationPipeline::ForProfile(const BuildProfile profile, const ArtifactKind artifactKind,
                                                      const std::size_t fixedPointLimit) {
    OptimizationPipeline pipeline = ForProfile(profile, fixedPointLimit);
    if (profile == BuildProfile::Release) {
        pipeline.lir.Add(std::make_unique<LirDeclarationPruner>(artifactKind));
    }
    return pipeline;
}

std::vector<std::string_view> OptimizationPipeline::HirPassNames() const {
    return hir.PassNames();
}

std::vector<std::string_view> OptimizationPipeline::LirPassNames() const {
    return lir.PassNames();
}

PassRunReport OptimizationPipeline::RunHir(HirPackage &package) {
    return hir.Run(package);
}

PassRunReport OptimizationPipeline::RunLir(LirPackage &package) {
    return lir.Run(package);
}
} // namespace Rux::Optimization
