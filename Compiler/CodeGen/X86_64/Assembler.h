#pragma once

// Encodes the body of an `asm func` — a sequence of parsed x86-64 instructions
// (AsmInstr) — into machine code. Labels and jumps inside the body are
// resolved here; references to symbols declared elsewhere (call targets,
// rip-relative data) are reported as fixups for the object emitter to relocate.

#include "CodeGen/AsmAssembly.h"
#include "Target/AsmInstr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
// Encode `instrs`, appending the machine code to `out`. `funcName` and
// `sourceName` are used only for diagnostics.
AsmAssembly AssembleAsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName,
                            std::vector<std::uint8_t> &out, Target::OS targetOs = Target::OS::Linux);
} // namespace Rux
