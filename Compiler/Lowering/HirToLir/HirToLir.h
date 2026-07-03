#pragma once

#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"

namespace Rux {

class HirToLirLowering {
public:
    explicit HirToLirLowering(HirPackage package);
    [[nodiscard]] LirPackage Generate();

private:
    HirPackage hir_;
};

} // namespace Rux
