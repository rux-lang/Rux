#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <vector>

namespace Rux {
class HirToLirLowering {
public:
    // The target decides which C ABI an extern declaration without an explicit
    // `#Abi` resolves to, so it has to be supplied rather than taken from the
    // host: a cross-build must record the target architecture's convention.
    HirToLirLowering(HirPackage inputHir, TargetContext inputTarget);
    [[nodiscard]] LirPackage Generate();
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const noexcept;

private:
    HirPackage hir;
    TargetContext target;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
