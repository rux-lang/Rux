#include "Optimization/Pipeline.h"

#include "Optimization/HirConstantFolder.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirConstantPropagation.h"
#include "Optimization/LirDeadCodeElimination.h"
#include "Optimization/LirDeclarationPruner.h"
#include "Optimization/LirInliner.h"
#include "Optimization/LirLoopInvariants.h"
#include "Optimization/LirLoopUnrolling.h"
#include "Optimization/LirLoopVersioning.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirScalarCombines.h"
#include "Optimization/LirScalarReplacement.h"
#include "Optimization/LirSsaVerifier.h"
#include "Optimization/LirValueNumbering.h"

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
        pipeline.lir.Add(std::make_unique<LirFrameAddressPlacement>());
        pipeline.lir.Add(std::make_unique<LirSsaVerifier>());
        pipeline.lir.Add(std::make_unique<LirCfgCleanup>());
        pipeline.lir.Add(std::make_unique<LirScalarReplacement>());
        pipeline.lir.Add(std::make_unique<LirMemoryPromotion>());
        pipeline.lir.Add(std::make_unique<LirConstantPropagation>());
        pipeline.lir.Add(std::make_unique<LirInliner>());
        pipeline.lir.Add(std::make_unique<LirScalarCombines>());
        pipeline.lir.Add(std::make_unique<LirValueNumbering>());
        pipeline.lir.Add(std::make_unique<LirLoopInvariants>());
        pipeline.lir.Add(std::make_unique<LirLoopVersioning>());
        pipeline.lir.Add(std::make_unique<LirBlockCoalescing>());
        pipeline.lir.Add(std::make_unique<LirDeadCodeElimination>());
        pipeline.lir.Add(std::make_unique<LirCfgCleanup>());
        pipeline.lir.Add(std::make_unique<LirLoopUnrolling>());
        pipeline.lir.Add(std::make_unique<LirCfgVerifier>());
        pipeline.lir.Add(std::make_unique<LirSsaVerifier>());
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

void OptimizationPipeline::SetObservers(HirPassPipeline::Observer hirObserver, LirPassPipeline::Observer lirObserver) {
    hir.SetObserver(std::move(hirObserver));
    lir.SetObserver(std::move(lirObserver));
}

std::vector<Diagnostic> OptimizationPipeline::Configure(const std::span<const std::string> disabled,
                                                        const bool statistics) {
    auto names = HirPassNames();
    const auto lirNames = LirPassNames();
    names.insert(names.end(), lirNames.begin(), lirNames.end());
    std::vector<Diagnostic> diagnostics;
    for (const auto &name : disabled) {
        if (!std::ranges::contains(names, name)) {
            diagnostics.push_back(
                ErrorDiagnostic(std::format("optimization pass '{}' is not active in this profile", name)));
        }
        else if (name.ends_with("verifier") || name == "lir-frame-address-placement") {
            diagnostics.push_back(
                ErrorDiagnostic(std::format("required optimization pass '{}' cannot be disabled", name)));
        }
    }
    if (diagnostics.empty()) {
        hir.Configure(disabled, statistics);
        lir.Configure(disabled, statistics);
    }
    return diagnostics;
}
} // namespace Rux::Optimization
