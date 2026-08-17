#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux::Optimization {
/// Whether a pass rewrote the IR it was given. This is what drives the pipeline's fixed-point loop, so a pass that
/// reports `None` after rewriting something will have its change silently left un-propagated.
enum class PassChange {
    None,
    Changed,
};

/// What declaration pruning removed, counted for the `--stats` report.
///
/// The four declaration counts are the parts of `TotalDeclarations`; `estimatedIrNodes` is deliberately outside that
/// sum, being a size estimate rather than a declaration, and the report keeps it in its own group.
struct LirPruningStats {
    std::size_t functionDefinitions = 0;
    std::size_t constants = 0;
    std::size_t vtables = 0;
    std::size_t externDeclarations = 0;
    std::size_t estimatedIrNodes = 0;

    [[nodiscard]] std::size_t TotalDeclarations() const noexcept {
        return functionDefinitions + constants + vtables + externDeclarations;
    }
};

/**
 * @brief What every pass in one pipeline run shares.
 *
 * `diagnostics` and `lirPruning` are borrowed from the `PassRunReport` the pipeline is filling in, so a pass reports
 * through them rather than owning anything: a pass that fails does so by appending a diagnostic, never by throwing.
 * Both may be null when a pass is driven outside a pipeline, which is why reporting goes through `ReportInternalError`
 * rather than touching the pointer directly. `iteration` and `fixedPointLimit` are visible so a pass can tell how close
 * the pipeline is to giving up.
 */
struct PassContext {
    BuildProfile profile = BuildProfile::Debug;
    std::size_t iteration = 0;
    std::size_t fixedPointLimit = 1;
    std::vector<Diagnostic> *diagnostics = nullptr;
    LirPruningStats *lirPruning = nullptr;

    /// Report a compiler bug found while optimizing. Silently drops the message when no pipeline is collecting
    /// diagnostics.
    void ReportInternalError(std::string message) const {
        if (diagnostics != nullptr) {
            diagnostics->push_back(ErrorDiagnostic(std::move(message)));
        }
    }
};

/// The outcome of running one pipeline to completion. `reachedFixedPoint` is false only when the iteration limit ran
/// out with passes still changing the IR, which the pipeline also reports as an error diagnostic.
struct PassRunReport {
    PassChange change = PassChange::None;
    std::size_t iterations = 0;
    bool reachedFixedPoint = true;
    std::vector<Diagnostic> diagnostics;
    LirPruningStats lirPruning;

    [[nodiscard]] bool HasErrors() const noexcept {
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.IsError()) {
                return true;
            }
        }
        return false;
    }
};

/**
 * @brief One optimization pass over a single IR form.
 *
 * The IR is a template parameter rather than a common base class because HIR and LIR share no node hierarchy; `HirPass`
 * and `LirPass` below are the only two instantiations. A pass must be re-runnable: the pipeline calls it once per
 * iteration until nothing changes, so it has to reach the same answer when handed IR it has already seen.
 */
template <typename Ir>
class Pass {
public:
    virtual ~Pass() = default;

    /// Stable identifier, used in `--stats` output and in the note the pipeline attaches to any diagnostic raised while
    /// this pass was running.
    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;

    /// Rewrite `ir` in place, reporting whether anything changed.
    virtual PassChange Run(Ir &ir, const PassContext &context) = 0;
};

using HirPass = Pass<HirPackage>;
using LirPass = Pass<LirPackage>;
} // namespace Rux::Optimization
