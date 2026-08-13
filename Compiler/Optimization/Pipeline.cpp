#include "Optimization/Pipeline.h"

#include "Optimization/HirConstantFolder.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirConstantPropagation.h"

#include <memory>

namespace Rux::Optimization {
OptimizationPipeline::OptimizationPipeline(const BuildProfile profile, const std::size_t fixedPointLimit)
    : hir_(profile, fixedPointLimit)
    , lir_(profile, fixedPointLimit) {
}

OptimizationPipeline OptimizationPipeline::ForProfile(const BuildProfile profile, const std::size_t fixedPointLimit) {
    OptimizationPipeline pipeline(profile, fixedPointLimit);
    pipeline.lir_.Add(std::make_unique<LirCfgVerifier>());
    if (profile == BuildProfile::Release) {
        pipeline.hir_.Add(std::make_unique<HirConstantFolder>());
        pipeline.lir_.Add(std::make_unique<LirConstantPropagation>());
        pipeline.lir_.Add(std::make_unique<LirCfgCleanup>());
    }
    return pipeline;
}

std::vector<std::string_view> OptimizationPipeline::HirPassNames() const {
    return hir_.PassNames();
}

std::vector<std::string_view> OptimizationPipeline::LirPassNames() const {
    return lir_.PassNames();
}

PassRunReport OptimizationPipeline::RunHir(HirPackage &package) {
    return hir_.Run(package);
}

PassRunReport OptimizationPipeline::RunLir(LirPackage &package) {
    return lir_.Run(package);
}
} // namespace Rux::Optimization
