#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
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

private:
    HirPackage hir;
    TargetContext target;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
