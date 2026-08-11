#pragma once

#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

namespace Rux {
class HirToLirLowering {
public:
    // The target decides which C ABI an extern declaration without an explicit
    // `#Abi` resolves to, so it has to be supplied rather than taken from the
    // host: a Linux-to-Windows build must record Win64.
    HirToLirLowering(HirPackage package, TargetContext target);
    [[nodiscard]] LirPackage Generate();

private:
    HirPackage hir_;
    TargetContext target_;
};
} // namespace Rux
