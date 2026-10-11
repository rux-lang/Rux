#pragma once

#include "BuildInfo/BuildInfo.h"
#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>
#include <vector>

namespace Rux {
class RcuEmitter {
public:
    explicit RcuEmitter(const LirPackage &package, std::string inputPackageName = {},
                        Target::OS inputTargetOs = Target::HostOS, BuildInfo inputBuildInfo = {});
    [[nodiscard]] std::vector<RcuFile> Generate() const;

    /// Selects machine instructions, allocates registers over the whole register file and lays out blocks for every
    /// function the machine selector supports (`MachineSelection.h`); the rest keep the direct emitter. The Release
    /// profile enables this.
    void SetMachineSelection(const bool enabled) noexcept {
        machineSelection = enabled;
    }

    /// Diagnostics accumulated during generation (e.g. errors encoding an `asm func` body). Populated by Generate();
    /// check after calling it.
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const {
        return diagnostics;
    }

private:
    const LirPackage &lir;
    std::string packageName;
    Target::OS targetOs;
    BuildInfo buildInfo;
    bool machineSelection = false;
    mutable std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
