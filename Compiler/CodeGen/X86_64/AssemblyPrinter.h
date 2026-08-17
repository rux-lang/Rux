#pragma once

#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <filesystem>
#include <string>

namespace Rux {
/// Generates x86-64 assembly text from a LIR package. Syntax is NASM-compatible with minor Rux-specific conventions: -
/// Sections use NASM keywords (.text / .data / .rodata) - Intel syntax and the requested target's x86-64 ABI -
/// Virtual-register homes and stack slots match binary emission - r10/r11 are used as caller-saved scratch registers
class AssemblyPrinter {
public:
    explicit AssemblyPrinter(LirPackage package, Target::OS targetOs = Target::HostOS);

    /// Generate assembly text for all modules in the package.
    [[nodiscard]] std::string Generate() const;

    /// Write the assembly text to `path`. Returns false on I/O error.
    static bool Emit(const LirPackage &package, const std::filesystem::path &path,
                     Target::OS targetOs = Target::HostOS);

private:
    LirPackage lir;
    Target::OS targetOs;
};
} // namespace Rux
