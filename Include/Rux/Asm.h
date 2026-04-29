/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Lir.h"

#include <filesystem>
#include <string>

namespace Rux {

// Generates x86-64 assembly text from a LIR package.
//
// Syntax is NASM-compatible with minor Rux-specific conventions:
//   - Sections use NASM keywords (.text / .data / .rodata)
//   - Intel syntax, System V AMD64 ABI calling convention
//   - All virtual registers are spilled to the stack (naive allocation)
//   - Parameters arrive in rdi/rsi/rdx/rcx/r8/r9 (integer) or xmm0-7 (float)
//   - r10/r11 are used as caller-saved scratch registers
class Asm {
public:
    explicit Asm(LirPackage package);

    // Generate assembly text for all modules in the package.
    [[nodiscard]] std::string Generate();

    // Write the assembly text to `path`. Returns false on I/O error.
    static bool Emit(const LirPackage& package, const std::filesystem::path& path);

private:
    LirPackage lir_;
};

} // namespace Rux
