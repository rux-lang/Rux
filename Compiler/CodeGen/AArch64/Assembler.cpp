// Public facade for assembling AArch64 `asm func` bodies. Instruction-family
// selection and all private assembler state live behind AssemblerContext.h.

#include "CodeGen/AArch64/AssemblerContext.h"

namespace Rux {
AsmAssembly AssembleAArch64AsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName,
                                   std::vector<std::uint8_t> &out) {
    AArch64AssemblerPrivate::BranchSystemAssemblerContext asmr(instrs, sourceName, out);
    return asmr.Run();
}
} // namespace Rux
