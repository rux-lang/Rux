#pragma once

// Encodes the body of an `asm func` — a sequence of AArch64 instructions the
// parser read for Target::Arch::AArch64 — into machine code. Branches to
// labels the body itself defines are resolved here; references to symbols
// declared elsewhere are reported as fixups for the object emitter to
// relocate.
//
// The counterpart of CodeGen/X86_64/Assembler.h, with the same signature and
// the same result, under a name of its own: one `rux` links both assemblers,
// and the architecture belongs in the name for the same reason it belongs in
// AArch64RcuEmitter's.

#include "CodeGen/AsmAssembly.h"
#include "Target/AsmInstr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
// Encode `instrs`, appending the machine code to `out`. `sourceName` is used
// only for diagnostics.
AsmAssembly AssembleAArch64AsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName,
                                   std::vector<std::uint8_t> &out, Target::OS targetOs = Target::OS::Linux);
} // namespace Rux
