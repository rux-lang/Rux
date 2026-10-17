#pragma once

#include "CodeGen/AArch64/MachineEncoder.h"
#include "CodeGen/AArch64/MachineInstructions.h"
#include "Target/Target.h"

#include <optional>
#include <vector>

namespace Rux::AArch64Machine {
struct EmittedFunction {
    std::vector<std::uint8_t> bytes;
    /// Relocations, with offsets from the start of the function.
    std::vector<SymbolUse> symbolUses;
};

/// Allocates registers for a selected function, lays out its frame and blocks, and encodes it. Returns nothing when
/// allocation gives up on an unusually large function or an instruction does not encode, and the function then keeps
/// the established emitter.
[[nodiscard]] std::optional<EmittedFunction> EmitFunction(SelectedFunction &selected, Target::OS targetOs);
} // namespace Rux::AArch64Machine
