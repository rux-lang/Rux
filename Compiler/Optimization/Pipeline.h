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
template <typename Ir>
class PassPipeline {
public:
    explicit PassPipeline(const BuildProfile profile, const std::size_t fixedPointLimit = 8, std::string irName = "IR")
        : profile_(profile)
        , fixedPointLimit_(std::max<std::size_t>(fixedPointLimit, 1))
        , irName_(std::move(irName)) {
    }

    void Add(std::unique_ptr<Pass<Ir>> pass) {
        passes_.push_back(std::move(pass));
    }

    [[nodiscard]] std::vector<std::string_view> PassNames() const {
        std::vector<std::string_view> names;
        names.reserve(passes_.size());
        for (const auto &pass : passes_) {
            names.push_back(pass->Name());
        }
        return names;
    }

    [[nodiscard]] PassRunReport Run(Ir &ir) {
        PassRunReport report;
        if (passes_.empty()) {
            return report;
        }

        report.reachedFixedPoint = false;
        for (std::size_t iteration = 0; iteration < fixedPointLimit_; ++iteration) {
            PassChange iterationChange = PassChange::None;
            const PassContext context{profile_, iteration, fixedPointLimit_, &report.diagnostics, &report.lirPruning};
            for (const auto &pass : passes_) {
                const std::size_t diagnosticStart = report.diagnostics.size();
                if (pass->Run(ir, context) == PassChange::Changed) {
                    iterationChange = PassChange::Changed;
                    report.change = PassChange::Changed;
                }
                for (std::size_t i = diagnosticStart; i < report.diagnostics.size(); ++i) {
                    report.diagnostics[i].notes.push_back(
                        std::format("while running {} optimization pass '{}' (iteration {})", irName_, pass->Name(),
                                    iteration + 1));
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
                ErrorDiagnostic(std::format("{} optimization did not reach a fixed point after {} iterations", irName_,
                                            report.iterations),
                                {std::format("the optimization limit is {} iterations", fixedPointLimit_)},
                                "simplify the input or report a compiler optimization bug"));
        }
        return report;
    }

private:
    BuildProfile profile_;
    std::size_t fixedPointLimit_;
    std::string irName_;
    std::vector<std::unique_ptr<Pass<Ir>>> passes_;
};

using HirPassPipeline = PassPipeline<HirPackage>;
using LirPassPipeline = PassPipeline<LirPackage>;

class OptimizationPipeline {
public:
    [[nodiscard]] static OptimizationPipeline ForProfile(BuildProfile profile, std::size_t fixedPointLimit = 8);
    [[nodiscard]] static OptimizationPipeline ForProfile(BuildProfile profile, ArtifactKind artifactKind,
                                                         std::size_t fixedPointLimit = 8);

    [[nodiscard]] std::vector<std::string_view> HirPassNames() const;
    [[nodiscard]] std::vector<std::string_view> LirPassNames() const;
    [[nodiscard]] PassRunReport RunHir(HirPackage &package);
    [[nodiscard]] PassRunReport RunLir(LirPackage &package);

private:
    OptimizationPipeline(BuildProfile profile, std::size_t fixedPointLimit);

    HirPassPipeline hir_;
    LirPassPipeline lir_;
};
} // namespace Rux::Optimization
