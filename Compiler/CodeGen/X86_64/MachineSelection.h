#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/MachineInstructions.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace Rux::X86_64Machine {
struct SelectionContext {
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;
    /// Whether a runtime failure calls the program's stack-trace printer before trapping.
    bool backtraceEnabled = false;
};

/// Selects machine instructions for one function, or declines when the function uses something this selector does
/// not lower: wide integers, software or x87 floating point, atomics, thread-local storage, C variadic calls, or an
/// operand shape outside the supported set. A declined function keeps the established emitter, whose calling
/// convention this selector reproduces exactly so the two interoperate.
[[nodiscard]] std::optional<SelectedFunction> SelectFunction(const LirFunc &function, const SelectionContext &context);
} // namespace Rux::X86_64Machine
