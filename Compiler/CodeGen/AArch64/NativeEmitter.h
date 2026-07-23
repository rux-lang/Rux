#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
// Native macOS/AArch64 backend.
//
// Rux keeps its target-neutral LIR as the contract between the frontend and
// machine backends. The x86-64 backend encodes RCU objects directly; this
// backend emits a short-lived C translation unit and asks the platform Clang
// driver to lower and link it for arm64. Using Clang here gives the compiler a
// complete AAPCS64 implementation (including aggregate and variadic calls)
// and produces a native Mach-O executable.
class AArch64NativeEmitter {
public:
    explicit AArch64NativeEmitter(const LirPackage &package, std::string packageName);

    [[nodiscard]] bool EmitExecutable(const std::filesystem::path &outputPath,
                                      const std::filesystem::path &temporaryDirectory, bool release,
                                      const std::optional<std::filesystem::path> &assemblyPath = std::nullopt);

    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const {
        return diagnostics;
    }

private:
    const LirPackage &lir;
    std::string packageName;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
