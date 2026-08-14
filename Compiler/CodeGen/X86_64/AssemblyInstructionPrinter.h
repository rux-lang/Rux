#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "Target/Target.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace Rux {
class AssemblyModulePrinter;

// Owns function setup, function-local operand spelling, and scalar and memory
// LIR instruction text for one planned x86-64 function. The control-flow
// printer delegates exactly these opcode families through this boundary.
class AssemblyInstructionPrinter {
public:
    AssemblyInstructionPrinter(AssemblyModulePrinter &modulePrinter, const X86_64FramePlan &framePlan,
                               const Layout::LayoutMap &layouts, const std::unordered_set<std::string> &interfaceNames,
                               Target::OS targetOs);

    void EmitFunctionSetup(const LirFunc &function);

    // Return true exactly when this printer owns and emitted the instruction.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);
    [[nodiscard]] bool EmitMemory(const LirInstr &instruction);

    // Operand operations shared temporarily with call and control-flow text.
    // They remain the single owner of virtual-register and stack-slot spelling.
    void LoadA(LirReg reg, const TypeRef &type);
    void StoreA(LirReg reg, const TypeRef &type);
    void LoadReturnValue(LirReg reg, const TypeRef &type);

    [[nodiscard]] const X86_64FramePlan &FramePlan() const;
    [[nodiscard]] bool IsWin64Convention(CallingConvention convention) const;
    [[nodiscard]] int SizeOfRuntime(const TypeRef &type) const;
    [[nodiscard]] std::string_view PhysicalRegisterName(int index) const;

private:
    AssemblyModulePrinter &modulePrinter;
    const X86_64FramePlan &framePlan;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;

    void LoadB(LirReg reg, const TypeRef &type);
    [[nodiscard]] bool IsWin64AddressParameter(const TypeRef &type) const;
    [[nodiscard]] bool IsRegPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] int ResolveFieldOffset(LirReg base, const std::string &fieldName) const;
};
} // namespace Rux
