#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

namespace Rux {
class HirToLirLowering {
public:
    // The target decides which C ABI an extern declaration without an explicit
    // `#Abi` resolves to, so it has to be supplied rather than taken from the
    // host: a cross-build must record the target architecture's convention.
    HirToLirLowering(HirPackage package, TargetContext target, BuildProfile profile);
    [[nodiscard]] LirPackage Generate();

private:
    HirPackage hir_;
    TargetContext target_;
    BuildProfile profile_;
};
} // namespace Rux
