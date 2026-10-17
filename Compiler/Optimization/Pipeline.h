#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Optimization/LirDeclarationPruner.h"
#include "Optimization/Pass.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux::Parallel {
class WorkerPool;
} // namespace Rux::Parallel

namespace Rux::Optimization {
namespace Detail {
/**
 * @brief Run LIR passes to a fixed point function by function.
 *
 * Consecutive function passes form a segment that every function runs through on its own, on the pool's threads when
 * one is given. A function that went through a whole iteration unchanged is skipped until a package pass rewrites it,
 * because running the same passes on the same body would leave it unchanged again. The IR, diagnostics and statistics
 * match running each pass over the whole package in turn.
 */
[[nodiscard]] PassRunReport RunLirPasses(LirPackage &package, const std::vector<std::unique_ptr<LirPass>> &passes,
                                         const std::unordered_set<std::string> &disabledPasses, bool collectStatistics,
                                         BuildProfile profile, std::size_t fixedPointLimit, std::string_view irName,
                                         Parallel::WorkerPool *pool);
} // namespace Detail

/**
 * @brief Runs an ordered list of passes over one IR form until it stops changing.
 *
 * Passes run in the order they were added, and the whole list repeats until an iteration reports no change. Iterating
 * is what lets one pass expose work for another — folding a constant in HIR can make a branch dead — without every pass
 * having to know which other passes exist.
 *
 * Failing to settle is treated as a compiler bug, not as a reason to keep the partly-optimized IR: exhausting the
 * iteration limit is reported as an error diagnostic. So is an error from any pass, which stops the run immediately
 * rather than letting later passes build on a broken IR.
 */
template <typename Ir>
class PassPipeline {
public:
    /// Read-only snapshots before optimization and after each executed pass. Returning false aborts the pipeline.
    /// Callbacks are outside pass timings and are never invoked for disabled passes.
    using Observer = std::function<bool(const Ir &, std::string_view, std::size_t, std::size_t)>;

    void SetObserver(Observer inputObserver) {
        observer = std::move(inputObserver);
    }

    /**
     * @brief Build an empty pipeline.
     *
     * @param inputFixedPointLimit How many times the pass list may repeat before the run is declared not to settle;
     * clamped to at least 1
     * @param inputIrName Names this IR in diagnostics, so a message says which of the two pipelines failed
     */
    explicit PassPipeline(const BuildProfile inputProfile, const std::size_t inputFixedPointLimit = 8,
                          std::string inputIrName = "IR")
        : profile(inputProfile)
        , fixedPointLimit(std::max<std::size_t>(inputFixedPointLimit, 1))
        , irName(std::move(inputIrName)) {
    }

    /// Append a pass. Order is significant: within one iteration a pass sees whatever the passes before it left behind.
    void Add(std::unique_ptr<Pass<Ir>> pass) {
        passes.push_back(std::move(pass));
    }

    void Configure(const std::span<const std::string> disabled, const bool statistics) {
        disabledPasses = std::unordered_set<std::string>(disabled.begin(), disabled.end());
        collectStatistics = statistics;
    }

    /// Threads LIR function passes may run on; null runs them on the calling thread. The result never depends on it.
    void SetWorkerPool(Parallel::WorkerPool *inputPool) noexcept {
        workerPool = inputPool;
    }

    /// The configured passes in run order, for the `--stats` report.
    [[nodiscard]] std::vector<std::string_view> PassNames() const {
        std::vector<std::string_view> names;
        names.reserve(passes.size());
        for (const auto &pass : passes) {
            names.push_back(pass->Name());
        }
        return names;
    }

    /**
     * @brief Rewrite `ir` in place until no pass changes it further.
     *
     * Every diagnostic a pass raises gains a note naming that pass and the iteration it ran in, because the same pass
     * failing on its first and fourth pass over the IR usually means different things. An empty pipeline is not a
     * failure: it settles immediately and reports no change.
     */
    [[nodiscard]] PassRunReport Run(Ir &ir) {
        if constexpr (std::is_same_v<Ir, LirPackage>) {
            // Snapshots show the package between passes, which only the pass-at-a-time order below produces.
            if (!observer)
                return Detail::RunLirPasses(ir, passes, disabledPasses, collectStatistics, profile, fixedPointLimit,
                                            irName, workerPool);
        }
        PassRunReport report;
        const auto observe = [&](const std::string_view name, const std::size_t iteration, const std::size_t index) {
            if (observer && !observer(ir, name, iteration, index)) {
                report.diagnostics.push_back(
                    ErrorDiagnostic(std::format("unable to capture {} optimization snapshot", irName)));
                report.reachedFixedPoint = false;
                return false;
            }
            return true;
        };
        if (!observe("input", 0, 0))
            return report;
        if (passes.empty()) {
            return report;
        }

        if (collectStatistics) {
            for (const auto &pass : passes) {
                report.passStatistics.push_back({.ir = irName,
                                                 .name = std::string(pass->Name()),
                                                 .disabled = disabledPasses.contains(std::string(pass->Name()))});
            }
        }

        report.reachedFixedPoint = false;
        for (std::size_t iteration = 0; iteration < fixedPointLimit; ++iteration) {
            PassChange iterationChange = PassChange::None;
            const PassContext context{profile, iteration, fixedPointLimit, &report.diagnostics, &report.lirPruning,
                                      nullptr, workerPool};
            for (std::size_t passIndex = 0; passIndex < passes.size(); ++passIndex) {
                const auto &pass = passes[passIndex];
                if (!disabledPasses.empty() && disabledPasses.contains(std::string(pass->Name())))
                    continue;
                const std::size_t diagnosticStart = report.diagnostics.size();
                const auto before = collectStatistics ? InstructionCount(ir) : 0;
                const auto start =
                    collectStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto change = pass->Run(ir, context);
                if (collectStatistics) {
                    const auto elapsed = std::chrono::steady_clock::now() - start;
                    const auto after = InstructionCount(ir);
                    auto &measurement = report.passStatistics[passIndex];
                    ++measurement.runs;
                    measurement.changes += change == PassChange::Changed;
                    measurement.elapsed += std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
                    measurement.instructionsAdded += after > before ? after - before : 0;
                    measurement.instructionsRemoved += before > after ? before - after : 0;
                }
                if (change == PassChange::Changed) {
                    iterationChange = PassChange::Changed;
                    report.change = PassChange::Changed;
                }
                for (std::size_t i = diagnosticStart; i < report.diagnostics.size(); ++i) {
                    report.diagnostics[i].notes.push_back(std::format(
                        "while running {} optimization pass '{}' (iteration {})", irName, pass->Name(), iteration + 1));
                }
                if (report.HasErrors()) {
                    ++report.iterations;
                    return report;
                }
                if (!observe(pass->Name(), iteration + 1, passIndex + 1))
                    return report;
            }

            ++report.iterations;
            if (iterationChange == PassChange::None) {
                report.reachedFixedPoint = true;
                break;
            }
        }
        if (!report.reachedFixedPoint) {
            report.diagnostics.push_back(
                ErrorDiagnostic(std::format("{} optimization did not reach a fixed point after {} iterations", irName,
                                            report.iterations),
                                {std::format("the optimization limit is {} iterations", fixedPointLimit)},
                                "simplify the input or report a compiler optimization bug"));
        }
        return report;
    }

private:
    BuildProfile profile;
    std::size_t fixedPointLimit;
    std::string irName;
    std::vector<std::unique_ptr<Pass<Ir>>> passes;
    std::unordered_set<std::string> disabledPasses;
    bool collectStatistics = false;
    Observer observer;
    Parallel::WorkerPool *workerPool = nullptr;

    [[nodiscard]] static std::size_t InstructionCount(const Ir &ir) {
        std::size_t count = 0;
        if constexpr (std::is_same_v<Ir, LirPackage>) {
            for (const auto &module : ir.modules)
                for (const auto &function : module.funcs)
                    for (const auto &block : function.blocks)
                        count += block.instrs.size();
        }
        return count;
    }
};

using HirPassPipeline = PassPipeline<HirPackage>;
using LirPassPipeline = PassPipeline<LirPackage>;

/**
 * @brief The HIR and LIR pipelines one build runs, chosen by profile.
 *
 * The two are separate because they run at different points in lowering: HIR is optimized before it becomes LIR, so the
 * caller drives `RunHir` and `RunLir` itself rather than handing over the whole pipeline.
 *
 * Debug builds still verify the LIR control-flow graph — a malformed CFG is a compiler bug worth catching in the
 * configuration used to debug one — but run no transforming pass, so what runs matches what was written.
 */
class OptimizationPipeline {
public:
    /// The passes for `profile`, without declaration pruning. Suitable when nothing is being linked and the artifact
    /// form is therefore unknown.
    [[nodiscard]] static OptimizationPipeline ForProfile(BuildProfile profile, std::size_t fixedPointLimit = 8);

    /// The passes for `profile`, adding declaration pruning for Release builds. Pruning needs the artifact kind because
    /// that decides the root set: an executable keeps what `Main` reaches, a library keeps its public API.
    [[nodiscard]] static OptimizationPipeline ForProfile(BuildProfile profile, ArtifactKind artifactKind,
                                                         std::size_t fixedPointLimit = 8);

    [[nodiscard]] std::vector<std::string_view> HirPassNames() const;
    [[nodiscard]] std::vector<std::string_view> LirPassNames() const;
    [[nodiscard]] PassRunReport RunHir(HirPackage &package);
    [[nodiscard]] PassRunReport RunLir(LirPackage &package);
    /// Validate diagnostic bisection options. Required normalization and verification cannot be disabled.
    [[nodiscard]] std::vector<Diagnostic> Configure(std::span<const std::string> disabled, bool statistics);
    void SetObservers(HirPassPipeline::Observer hirObserver, LirPassPipeline::Observer lirObserver);
    void SetWorkerPool(Parallel::WorkerPool *pool) noexcept;
    /// Prune each module from the declarations the artifact's roots and the other modules enter it through
    /// (`ModuleRoots`, one list per module in package order), following only its own references, instead of walking the
    /// whole package. What a module keeps then depends on the other modules only through these roots.
    void SetModuleRoots(std::vector<std::vector<LirModuleRoot>> roots);

private:
    OptimizationPipeline(BuildProfile profile, std::size_t fixedPointLimit);

    HirPassPipeline hir;
    LirPassPipeline lir;
    /// Release pruning before the first LIR iteration, so no pass optimizes a declaration the artifact will not keep.
    std::unique_ptr<LirDeclarationPruner> initialPruning;
    /// The pruner inside the LIR pipeline, owned by it.
    LirDeclarationPruner *loopPruning = nullptr;
    Parallel::WorkerPool *workerPool = nullptr;
};
} // namespace Rux::Optimization
