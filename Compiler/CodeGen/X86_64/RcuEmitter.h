#pragma once

#include "Ir/Lir/Lir.h"

#include <string>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

namespace Rux {

class RcuEmitter {
public:
    explicit RcuEmitter(const LirPackage &package, std::string inputPackageName = {},
                        Target::OS inputTargetOs = Target::HostOS);
    [[nodiscard]] std::vector<RcuFile> Generate() const;

    // Diagnostics accumulated during generation (e.g. errors encoding an
    // `asm func` body). Populated by Generate(); check after calling it.
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const {
        return diagnostics;
    }

private:
    const LirPackage &lir;
    std::string packageName;
    Target::OS targetOs;
    mutable std::vector<Diagnostic> diagnostics;
};

} // namespace Rux
