#pragma once

#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/MachineInstructions.h"
#include "CodeGen/Layout.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace Rux::AArch64Machine {
struct SelectionContext {
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    /// The AAPCS64 placement the established emitter uses, so both agree on every call.
    const AArch64CallPlanner &callPlanner;
    Target::OS targetOs;
    /// Whether a runtime failure calls the program's stack-trace printer before trapping.
    bool backtraceEnabled = false;
};

/// Selects machine instructions for one function, or declines when the function uses something this selector does
/// not lower: wide integers, software floating point, atomics, thread-local storage, vectors, C variadic calls, or an
/// operand shape outside the supported set. A declined function keeps the established emitter, whose calling
/// convention this selector reproduces exactly so the two interoperate.
[[nodiscard]] std::optional<SelectedFunction> SelectFunction(const LirFunc &function, const SelectionContext &context);
} // namespace Rux::AArch64Machine
