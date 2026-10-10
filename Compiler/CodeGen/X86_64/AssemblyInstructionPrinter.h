#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "Target/Target.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Rux {
class AssemblyModulePrinter;

/// Owns function setup, function-local operand spelling, and scalar and memory LIR instruction text for one planned
/// x86-64 function. The control-flow printer delegates exactly these opcode families through this boundary.
///
/// The text mirrors the object emitter (`X86_64FunctionEmitter`, `X86_64CallEmitter`, `X86_64TerminatorEmitter` and the
/// module emitter's operand moves) instruction for instruction and on the same frame addresses, so `--emit asm` reads
/// as what the object code does. A frame address is taken as the emitter takes it: a signed displacement from rbp,
/// negative for a slot (`Disp`), positive for an incoming stack argument.
class AssemblyInstructionPrinter {
public:
    AssemblyInstructionPrinter(AssemblyModulePrinter &modulePrinter, const X86_64FramePlan &framePlan,
                               const Layout::LayoutMap &layouts, const std::unordered_set<std::string> &interfaceNames,
                               Target::OS targetOs);

    void EmitFunctionSetup(const LirFunc &function);

    /// Return true exactly when this printer owns and emitted the instruction.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);
    [[nodiscard]] bool EmitMemory(const LirInstr &instruction);
    [[nodiscard]] bool EmitAtomic(const LirInstr &instruction);

    /// Operand operations shared with call and control-flow text. They remain the single owner of virtual-register and
    /// stack-slot spelling.
    void LoadA(LirReg reg, const TypeRef &type);
    void StoreA(LirReg reg, const TypeRef &type);
    /// Writes rax (rax:rdx, xmm0) to `reg`'s frame slot even when the register also has a physical home.
    void StoreStack(LirReg reg, const TypeRef &type);
    void LoadReturnValue(LirReg reg, const TypeRef &type);
    /// Copies the returned aggregate through the hidden result pointer and leaves that pointer in rax.
    void StoreHiddenReturnValue(LirReg reg, const TypeRef &type);
    /// Copies `size` bytes from [r10] to [r11] a chunk at a time.
    void CopyAggregateFromR10ToR11(int size);

    /// One instruction line, the opcode padded to the operand column.
    void Instruction(std::string_view opcode, std::string_view operands = {}) const;
    /// `[rbp - n]` or `[rbp + n]` for a signed rbp displacement.
    [[nodiscard]] static std::string Rbp(std::int32_t displacement);
    /// A sized memory operand at a signed rbp displacement, as in `qword [rbp - 8]`.
    [[nodiscard]] static std::string Rbp(std::string_view size, std::int32_t displacement);
    /// The signed rbp displacement of `reg`'s frame slot.
    [[nodiscard]] std::int32_t Disp(LirReg reg) const;

    [[nodiscard]] const X86_64FramePlan &FramePlan() const;
    [[nodiscard]] bool IsWin64Convention(CallingConvention convention) const;
    [[nodiscard]] int SizeOfRuntime(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsRegPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] std::string_view PhysicalRegisterName(int index) const;
    [[nodiscard]] AssemblyModulePrinter &ModulePrinter() const;

private:
    AssemblyModulePrinter &modulePrinter;
    const X86_64FramePlan &framePlan;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;

    void LoadB(LirReg reg, const TypeRef &type);
    void LoadAtomicAddress(LirReg pointer);
    void ExtendAccumulator(const TypeRef &type, int size);
    void EmitStackAllocation(std::int32_t bytes) const;
    void LoadChunkFromR10(std::int32_t offset, int size) const;
    void StoreChunkToR11(std::int32_t offset, int size) const;
    void CopyAggregateFromR10ToStack(std::int32_t destination, int size) const;

    // The word-by-word lowering of wide integers, bools and characters, and of the software floats.
    [[nodiscard]] bool EmitWideArithmetic(const LirInstr &instruction);
    [[nodiscard]] bool EmitWideSoftwareFloatNegation(const LirInstr &instruction);
    void EmitSoftwareFloatConstant(const LirInstr &instruction);
    void EmitWideDivision(const LirInstr &instruction, const TypeRef &wideType);
    void LoadWord(std::uint64_t word) const;
    void ZeroWide(std::int32_t destination, int size) const;
    void CopyWide(std::int32_t source, std::int32_t destination, int size) const;
    void NegateWide(std::int32_t value, int size) const;
    void MultiplyWide(std::int32_t left, std::int32_t right, std::int32_t destination, int size) const;
    [[nodiscard]] std::string Label(std::string_view purpose) const;

    [[nodiscard]] bool IsWin64AddressParameter(const TypeRef &type) const;
    [[nodiscard]] int ResolveFieldOffset(LirReg base, const std::string &fieldName) const;
};
} // namespace Rux
