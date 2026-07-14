#pragma once

// Encodes the body of an `asm func` — a sequence of parsed x86-64 instructions
// (AsmInstr) — into machine code. Labels and jumps inside the body are
// resolved here; references to symbols declared elsewhere (call targets,
// rip-relative data) are reported as fixups for the object emitter to relocate.

#include "Diagnostics/Diagnostics.h"
#include "Target/AsmInstr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
// A reference the assembler could not resolve within the function. Offsets are
// absolute positions inside the buffer passed to AssembleAsmFunc (i.e. they
// already account for the function's start offset).
struct AsmFixup {
    std::uint32_t offset = 0;  // start of the rel32 / abs64 field in the buffer
    std::string symbol;        // target symbol name
    std::uint16_t relType = 0; // RcuRelType::Rel32 / Abs64
    std::int32_t addend = 0;
};

struct AsmAssembly {
    bool ok = true;
    std::vector<AsmFixup> fixups;
    std::vector<Diagnostic> diagnostics;
};

// Encode `instrs`, appending the machine code to `out`. `funcName` and
// `sourceName` are used only for diagnostics.
AsmAssembly AssembleAsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName,
                            std::vector<std::uint8_t> &out);
} // namespace Rux
