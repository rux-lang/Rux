#pragma once

#include "CodeGen/Layout.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <unordered_set>

namespace Rux {
class AssemblyModulePrinter;

/// Owns complete function traversal plus the x86-64 text for calls, runtime failure paths, phi-edge moves, branches,
/// returns, switches, and epilogues. Scalar and memory instructions remain delegated to AssemblyInstructionPrinter.
class AssemblyControlFlowPrinter {
public:
    AssemblyControlFlowPrinter(AssemblyModulePrinter &modulePrinter, const Layout::LayoutMap &layouts,
                               const std::unordered_set<std::string> &interfaceNames, Target::OS targetOs);

    void EmitFunction(const LirFunc &function);

private:
    AssemblyModulePrinter &modulePrinter;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;
};
} // namespace Rux
