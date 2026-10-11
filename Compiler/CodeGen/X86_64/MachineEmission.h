#pragma once

#include "CodeGen/X86_64/MachineEncoder.h"
#include "CodeGen/X86_64/MachineInstructions.h"
#include "Target/Target.h"

#include <optional>
#include <string>
#include <vector>

namespace Rux::X86_64Machine {
struct EmittedFunction {
    std::vector<std::uint8_t> bytes;
    /// Symbol operands, with offsets from the start of the function.
    std::vector<SymbolUse> symbolUses;
    /// The function as assembly text, one instruction or label per line, when it was asked for.
    std::vector<std::string> assembly;
};

/// Allocates registers for a selected function, lays out its frame and blocks, and encodes it. Returns nothing when
/// allocation gives up on an unusually large function, which then keeps the established emitter.
[[nodiscard]] std::optional<EmittedFunction> EmitFunction(SelectedFunction &selected, Target::OS targetOs,
                                                          bool printAssembly);
} // namespace Rux::X86_64Machine
