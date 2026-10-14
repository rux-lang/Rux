#pragma once

#include "BuildInfo/BuildInfo.h"
#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Object/Rcu/Rcu.h"
#include "Parallel/WorkerPool.h"
#include "Target/Target.h"

#include <string>
#include <utility>
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

    /// Threads modules may be emitted on; null emits them on the calling thread. The objects never depend on it.
    void SetWorkerPool(Parallel::WorkerPool *inputPool) noexcept {
        pool = inputPool;
    }

    /// Have every module trace its failures and list its functions for the stack-trace printer whether or not anything
    /// in the package can fail, so a module's object depends only on the module. The driver does; otherwise modules
    /// do so only when the package holds the printer.
    void SetBacktraces(const bool enabled) noexcept {
        backtraces = enabled;
    }

    /// Emit only the modules whose entry is true, leaving an empty object in the place of every other one, whose object
    /// the caller already holds. Every module is emitted when this is empty.
    void SetEmittedModules(std::vector<bool> modules) {
        emittedModules = std::move(modules);
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
    bool backtraces = false;
    std::vector<bool> emittedModules;
    Parallel::WorkerPool *pool = nullptr;
    mutable std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
