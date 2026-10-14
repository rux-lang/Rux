#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Parallel/WorkerPool.h"
#include "Target/Target.h"

#include <vector>

namespace Rux {
/**
 * @brief Lowers a semantically valid HIR package to LIR.
 *
 * This is where the tree form gives way to basic blocks and registers: control flow becomes explicit edges, and every
 * construct is reduced to the small instruction set the backends share. Input is expected to be valid, so anything
 * reported here is a compiler bug rather than a user error.
 */
class HirToLirLowering {
public:
    /// The target decides which C ABI an extern declaration without an explicit `#Abi` resolves to, so it has to be
    /// supplied rather than taken from the host: a cross-build must record the target architecture's convention.
    HirToLirLowering(HirPackage inputHir, TargetContext inputTarget);

    /// Lower the whole package. Check `Diagnostics` afterwards: on failure the result is incomplete rather than absent.
    [[nodiscard]] LirPackage Generate();

    /// Problems found while lowering, all of them internal errors.
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const noexcept;

    /// Threads modules may be lowered on; null lowers them on the calling thread. The result never depends on it.
    void SetWorkerPool(Parallel::WorkerPool *inputPool) noexcept {
        pool = inputPool;
    }

    /// Put the stack-trace printer into a module of its own, `kProgramModuleName`, after the source modules, rather
    /// than into the last source module, so no source module's code depends on whether anything else can fail. The
    /// driver does; a package lowered without it keeps one module per source.
    void SetProgramModule(const bool enabled) noexcept {
        programModule = enabled;
    }

    /// Whether code this package is linked with but does not lower can panic or fail an assertion, which calls for the
    /// stack-trace printer as much as code it lowers does.
    void SetExternalFailures(const bool present) noexcept {
        externalFailures = present;
    }

    /// After `Generate`: per module, whether its code can panic or fail an assertion.
    [[nodiscard]] const std::vector<char> &ModuleFailures() const noexcept {
        return moduleFailures;
    }

private:
    HirPackage hir;
    TargetContext target;
    std::vector<Diagnostic> diagnostics;
    Parallel::WorkerPool *pool = nullptr;
    bool programModule = false;
    bool externalFailures = false;
    std::vector<char> moduleFailures;
};
} // namespace Rux
