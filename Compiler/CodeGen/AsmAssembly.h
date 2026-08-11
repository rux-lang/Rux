#pragma once

// What assembling an `asm func` body produces, shared by the two machine
// assemblers — CodeGen/X86_64/Assembler.h and CodeGen/AArch64/Assembler.h.
//
// A body is machine code plus the references that machine code could not
// resolve on its own, and that shape is the same on both architectures even
// though nothing else about them is: the object emitter turns a fixup into a
// relocation without knowing which assembler reported it.

#include "Diagnostics/Diagnostics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
// A reference the assembler could not resolve within the function. Offsets are
// absolute positions inside the buffer the body was assembled into (i.e. they
// already account for the function's start offset), and name the field to
// patch on x86-64 and the whole instruction word on AArch64, which is what the
// relocation kinds of each architecture are written in terms of.
struct AsmFixup {
    std::uint32_t offset = 0;  // start of the field / instruction to patch
    std::string symbol;        // target symbol name
    std::uint16_t relType = 0; // an RcuRelType value
    std::int32_t addend = 0;
};

struct AsmAssembly {
    bool ok = true;
    std::vector<AsmFixup> fixups;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
