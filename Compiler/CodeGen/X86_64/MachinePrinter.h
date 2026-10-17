#pragma once

#include "CodeGen/X86_64/MachineInstructions.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Rux::X86_64Machine {
/// NASM-style text for an allocated, laid-out function: the prologue, every block in layout order under its label,
/// and each epilogue where a block returns.
[[nodiscard]] std::vector<std::string>
PrintFunction(const SelectedFunction &selected, const std::vector<std::uint32_t> &order,
              const std::vector<Reg> &savedGprs, const std::vector<std::pair<Reg, std::int32_t>> &savedVectors,
              const std::vector<std::int32_t> &objectOffsets, std::int32_t allocation);

/// One instruction's text, for diagnostics and tests.
[[nodiscard]] std::string PrintInstruction(const SelectedFunction &selected, const Machine::Instruction &instruction,
                                           const std::vector<std::int32_t> &objectOffsets);
} // namespace Rux::X86_64Machine
