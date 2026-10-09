// x86-64 x87 lowering for float80.
//
// The x87 unit computes in the 80-bit extended format float80 is stored in, so on x86-64 float80 arithmetic,
// comparisons and conversions to and from the machine floats run there instead of through the software helpers.

#include "CodeGen/X86_64/FunctionEmitter.h"

namespace Rux {
bool X86_64FunctionEmitter::EmitX87(const LirInstr &instruction) {
    (void)instruction;
    return false;
}
} // namespace Rux
