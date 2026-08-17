#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Optimization/Pass.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux::Optimization {
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
        PassRunReport report;
        if (passes.empty()) {
            return report;
        }

        report.reachedFixedPoint = false;
        for (std::size_t iteration = 0; iteration < fixedPointLimit; ++iteration) {
            PassChange iterationChange = PassChange::None;
            const PassContext context{profile, iteration, fixedPointLimit, &report.diagnostics, &report.lirPruning};
            for (const auto &pass : passes) {
                const std::size_t diagnosticStart = report.diagnostics.size();
                if (pass->Run(ir, context) == PassChange::Changed) {
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

private:
    OptimizationPipeline(BuildProfile profile, std::size_t fixedPointLimit);

    HirPassPipeline hir;
    LirPassPipeline lir;
};
} // namespace Rux::Optimization
