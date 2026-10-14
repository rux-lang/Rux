#include "Optimization/Pipeline.h"

#include "Optimization/HirConstantFolder.h"
#include "Optimization/LirCfgPasses.h"
#include "Optimization/LirConstantMemory.h"
#include "Optimization/LirConstantPropagation.h"
#include "Optimization/LirDeadCodeElimination.h"
#include "Optimization/LirDeadLoops.h"
#include "Optimization/LirDeclarationPruner.h"
#include "Optimization/LirFillWidening.h"
#include "Optimization/LirIfConversion.h"
#include "Optimization/LirInliner.h"
#include "Optimization/LirLoopInvariants.h"
#include "Optimization/LirLoopUnrolling.h"
#include "Optimization/LirLoopVersioning.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirPointerInduction.h"
#include "Optimization/LirScalarCombines.h"
#include "Optimization/LirScalarReplacement.h"
#include "Optimization/LirSsaVerifier.h"
#include "Optimization/LirValueNumbering.h"
#include "Optimization/LirViewParameters.h"
#include "Parallel/WorkerPool.h"

#include <cstdint>
#include <memory>

namespace Rux::Optimization {
namespace {
std::size_t CountInstructions(const LirFunc &function) {
    std::size_t count = 0;
    for (const auto &block : function.blocks)
        count += block.instrs.size();
    return count;
}

std::size_t CountInstructions(const LirPackage &package) {
    std::size_t count = 0;
    for (const auto &module : package.modules)
        for (const auto &function : module.funcs)
            count += CountInstructions(function);
    return count;
}

bool HasError(const std::vector<Diagnostic> &diagnostics) {
    return std::ranges::any_of(diagnostics, [](const Diagnostic &diagnostic) { return diagnostic.IsError(); });
}

/// One function's run through one pass of a segment.
struct FunctionOutcome {
    bool ran = false;
    bool changed = false;
    std::int64_t instructionDelta = 0;
    std::chrono::microseconds elapsed{};
    std::vector<Diagnostic> diagnostics;
};

struct FunctionSlot {
    std::size_t moduleIndex = 0;
    LirFunc *function = nullptr;
};

void RecordInstructionDelta(PassStatistics &measurement, const std::int64_t delta) {
    if (delta > 0)
        measurement.instructionsAdded += static_cast<std::size_t>(delta);
    else
        measurement.instructionsRemoved += static_cast<std::size_t>(-delta);
}
} // namespace

PassRunReport Detail::RunLirPasses(LirPackage &package, const std::vector<std::unique_ptr<LirPass>> &passes,
                                   const std::unordered_set<std::string> &disabledPasses, const bool collectStatistics,
                                   const BuildProfile profile, const std::size_t fixedPointLimit,
                                   const std::string_view irName, Parallel::WorkerPool *pool) {
    PassRunReport report;
    if (passes.empty())
        return report;
    const auto disabled = [&](const LirPass &pass) {
        return !disabledPasses.empty() && disabledPasses.contains(std::string(pass.Name()));
    };
    if (collectStatistics) {
        for (const auto &pass : passes)
            report.passStatistics.push_back(
                {.ir = std::string(irName), .name = std::string(pass->Name()), .disabled = disabled(*pass)});
    }
    const auto annotate = [&](Diagnostic &diagnostic, const LirPass &pass, const std::size_t iteration) {
        diagnostic.notes.push_back(
            std::format("while running {} optimization pass '{}' (iteration {})", irName, pass.Name(), iteration + 1));
    };

    report.reachedFixedPoint = false;
    // Functions that changed in the previous iteration; every other function is settled. Before the first iteration,
    // and after a package pass that changed the IR without saying where, every function counts as changed.
    std::unordered_set<std::string> changedBefore;
    bool everythingChanged = true;
    for (std::size_t iteration = 0; iteration < fixedPointLimit; ++iteration) {
        const PassContext context{profile, iteration, fixedPointLimit, &report.diagnostics, &report.lirPruning,
                                  nullptr, pool};
        std::unordered_set<std::string> changedNow;
        bool everythingNow = everythingChanged;
        bool unreportedChange = false;
        bool iterationChanged = false;
        const auto active = [&](const LirFunc &function) {
            return everythingNow || changedBefore.contains(function.name) || changedNow.contains(function.name);
        };

        for (std::size_t passIndex = 0; passIndex < passes.size();) {
            if (dynamic_cast<const LirFunctionPass *>(passes[passIndex].get()) == nullptr) {
                auto &pass = *passes[passIndex];
                if (disabled(pass)) {
                    ++passIndex;
                    continue;
                }
                const std::size_t diagnosticStart = report.diagnostics.size();
                const auto before = collectStatistics ? CountInstructions(package) : 0;
                const auto start =
                    collectStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                std::vector<const LirFunc *> rewritten;
                PassContext passContext = context;
                passContext.changedFunctions = &rewritten;
                const auto change = pass.Run(package, passContext);
                if (collectStatistics) {
                    const auto elapsed = std::chrono::steady_clock::now() - start;
                    auto &measurement = report.passStatistics[passIndex];
                    ++measurement.runs;
                    measurement.changes += change == PassChange::Changed;
                    measurement.elapsed += std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
                    RecordInstructionDelta(measurement, static_cast<std::int64_t>(CountInstructions(package)) -
                                                            static_cast<std::int64_t>(before));
                }
                if (change == PassChange::Changed) {
                    iterationChanged = true;
                    report.change = PassChange::Changed;
                    if (pass.ReportsChangedFunctions()) {
                        for (const auto *function : rewritten)
                            changedNow.insert(function->name);
                    }
                    else {
                        everythingNow = true;
                        unreportedChange = true;
                    }
                }
                for (std::size_t i = diagnosticStart; i < report.diagnostics.size(); ++i)
                    annotate(report.diagnostics[i], pass, iteration);
                if (report.HasErrors()) {
                    ++report.iterations;
                    return report;
                }
                ++passIndex;
                continue;
            }

            // A run of function passes: every active function goes through all of them on its own.
            std::size_t segmentEnd = passIndex;
            while (segmentEnd < passes.size() &&
                   dynamic_cast<const LirFunctionPass *>(passes[segmentEnd].get()) != nullptr)
                ++segmentEnd;
            std::vector<std::size_t> members;
            for (std::size_t index = passIndex; index < segmentEnd; ++index) {
                auto &pass = static_cast<LirFunctionPass &>(*passes[index]);
                if (disabled(pass))
                    continue;
                if (collectStatistics)
                    ++report.passStatistics[index].runs;
                if (pass.Enabled(context)) {
                    pass.Prepare(package);
                    members.push_back(index);
                }
            }
            std::vector<FunctionSlot> slots;
            if (!members.empty()) {
                for (std::size_t moduleIndex = 0; moduleIndex < package.modules.size(); ++moduleIndex)
                    for (auto &function : package.modules[moduleIndex].funcs)
                        if (active(function))
                            slots.push_back({moduleIndex, &function});
            }
            std::vector<std::vector<FunctionOutcome>> outcomes(slots.size());
            const auto runSlot = [&](const std::size_t slotIndex) {
                const auto &slot = slots[slotIndex];
                auto &results = outcomes[slotIndex];
                results.resize(members.size());
                for (std::size_t member = 0; member < members.size(); ++member) {
                    const auto &pass = static_cast<const LirFunctionPass &>(*passes[members[member]]);
                    auto &outcome = results[member];
                    PassContext passContext = context;
                    passContext.diagnostics = &outcome.diagnostics;
                    passContext.lirPruning = nullptr;
                    passContext.pool = nullptr;
                    const auto before = collectStatistics ? CountInstructions(*slot.function) : 0;
                    const auto start =
                        collectStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    outcome.ran = true;
                    outcome.changed =
                        pass.RunFunction(*slot.function, slot.moduleIndex, passContext) == PassChange::Changed;
                    if (collectStatistics) {
                        outcome.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start);
                        outcome.instructionDelta = static_cast<std::int64_t>(CountInstructions(*slot.function)) -
                                                   static_cast<std::int64_t>(before);
                    }
                    if (HasError(outcome.diagnostics))
                        break;
                }
            };
            if (pool != nullptr)
                pool->ForEach(slots.size(), runSlot);
            else
                for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
                    runSlot(slotIndex);

            // Merge pass by pass in package order, as if each pass had swept the whole package in turn.
            for (std::size_t member = 0; member < members.size(); ++member) {
                const auto &pass = *passes[members[member]];
                bool passChanged = false;
                std::int64_t delta = 0;
                std::chrono::microseconds elapsed{};
                for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
                    auto &outcome = outcomes[slotIndex][member];
                    if (!outcome.ran)
                        continue;
                    passChanged = passChanged || outcome.changed;
                    if (outcome.changed)
                        changedNow.insert(slots[slotIndex].function->name);
                    delta += outcome.instructionDelta;
                    elapsed += outcome.elapsed;
                    for (auto &diagnostic : outcome.diagnostics) {
                        annotate(diagnostic, pass, iteration);
                        report.diagnostics.push_back(std::move(diagnostic));
                    }
                }
                if (collectStatistics) {
                    auto &measurement = report.passStatistics[members[member]];
                    measurement.changes += passChanged;
                    measurement.elapsed += elapsed;
                    RecordInstructionDelta(measurement, delta);
                }
                if (passChanged) {
                    iterationChanged = true;
                    report.change = PassChange::Changed;
                }
                if (report.HasErrors()) {
                    ++report.iterations;
                    return report;
                }
            }
            passIndex = segmentEnd;
        }

        ++report.iterations;
        if (!iterationChanged) {
            report.reachedFixedPoint = true;
            break;
        }
        changedBefore = std::move(changedNow);
        everythingChanged = unreportedChange;
    }
    if (!report.reachedFixedPoint) {
        report.diagnostics.push_back(ErrorDiagnostic(
            std::format("{} optimization did not reach a fixed point after {} iterations", irName, report.iterations),
            {std::format("the optimization limit is {} iterations", fixedPointLimit)},
            "simplify the input or report a compiler optimization bug"));
    }
    return report;
}

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
        pipeline.lir.Add(std::make_unique<LirViewParameters>());
        pipeline.lir.Add(std::make_unique<LirScalarReplacement>());
        pipeline.lir.Add(std::make_unique<LirMemoryPromotion>());
        pipeline.lir.Add(std::make_unique<LirConstantMemory>());
        pipeline.lir.Add(std::make_unique<LirConstantPropagation>());
        pipeline.lir.Add(std::make_unique<LirScalarCombines>());
        pipeline.lir.Add(std::make_unique<LirCfgCleanup>());
        pipeline.lir.Add(std::make_unique<LirDeadCodeElimination>());
        pipeline.lir.Add(std::make_unique<LirInliner>());
        pipeline.lir.Add(std::make_unique<LirScalarCombines>());
        pipeline.lir.Add(std::make_unique<LirValueNumbering>());
        pipeline.lir.Add(std::make_unique<LirLoopInvariants>());
        pipeline.lir.Add(std::make_unique<LirLoopVersioning>());
        pipeline.lir.Add(std::make_unique<LirBlockCoalescing>());
        pipeline.lir.Add(std::make_unique<LirFillWidening>());
        pipeline.lir.Add(std::make_unique<LirDeadCodeElimination>());
        pipeline.lir.Add(std::make_unique<LirDeadLoops>());
        pipeline.lir.Add(std::make_unique<LirIfConversion>());
        pipeline.lir.Add(std::make_unique<LirCfgCleanup>());
        pipeline.lir.Add(std::make_unique<LirPointerInduction>());
        pipeline.lir.Add(std::make_unique<LirDeadCodeElimination>());
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
        pipeline.initialPruning = std::make_unique<LirDeclarationPruner>(artifactKind);
        auto loopPruning = std::make_unique<LirDeclarationPruner>(artifactKind);
        pipeline.loopPruning = loopPruning.get();
        pipeline.lir.Add(std::move(loopPruning));
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
    // A program usually reaches a small part of its dependencies; dropping the rest first spares every pass the work.
    // The pruner in the pipeline still runs after each iteration, for declarations inlining leaves unreferenced.
    LirPruningStats initial;
    std::vector<Diagnostic> initialDiagnostics;
    if (initialPruning) {
        const PassContext context{BuildProfile::Release, 0, 1, &initialDiagnostics, &initial};
        static_cast<void>(initialPruning->Run(package, context));
    }
    auto report = lir.Run(package);
    report.lirPruning.functionDefinitions += initial.functionDefinitions;
    report.lirPruning.constants += initial.constants;
    report.lirPruning.vtables += initial.vtables;
    report.lirPruning.externDeclarations += initial.externDeclarations;
    report.lirPruning.estimatedIrNodes += initial.estimatedIrNodes;
    report.diagnostics.insert(report.diagnostics.begin(), initialDiagnostics.begin(), initialDiagnostics.end());
    return report;
}

void OptimizationPipeline::SetObservers(HirPassPipeline::Observer hirObserver, LirPassPipeline::Observer lirObserver) {
    hir.SetObserver(std::move(hirObserver));
    lir.SetObserver(std::move(lirObserver));
}

void OptimizationPipeline::SetModuleRoots(std::vector<std::vector<LirModuleRoot>> roots) {
    const auto shared = std::make_shared<const std::vector<std::vector<LirModuleRoot>>>(std::move(roots));
    if (initialPruning)
        initialPruning->SetModuleRoots(shared);
    if (loopPruning != nullptr)
        loopPruning->SetModuleRoots(shared);
}

void OptimizationPipeline::SetWorkerPool(Parallel::WorkerPool *pool) noexcept {
    hir.SetWorkerPool(pool);
    lir.SetWorkerPool(pool);
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
