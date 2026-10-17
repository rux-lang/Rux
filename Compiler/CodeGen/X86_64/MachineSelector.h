#pragma once

// Private state of x86-64 machine instruction selection, shared by the translation units that lower each family of
// LIR operations. Nothing outside `MachineSelection*.cpp` includes this header.

#include "CodeGen/X86_64/MachineSelection.h"
#include "Ir/Lir/RegisterTable.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::X86_64Machine::Detail {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::Operand;
using Machine::RegClass;

enum class ValueKind : std::uint8_t {
    Gpr,
    Xmm,
    Aggregate,
    Unsupported
};

/// The integer argument registers of each convention, in order.
inline constexpr Reg Win64IntegerArguments[] = {Rcx, Rdx, R8, R9};
inline constexpr Reg SysVIntegerArguments[] = {Rdi, Rsi, Rdx, Rcx, R8, R9};

/// Registers a call may overwrite.
[[nodiscard]] constexpr std::uint64_t CallClobbers(const bool win64) noexcept {
    std::uint64_t mask = Machine::RegisterBit(Rax) | Machine::RegisterBit(Rcx) | Machine::RegisterBit(Rdx) |
                         Machine::RegisterBit(R8) | Machine::RegisterBit(R9) | Machine::RegisterBit(R10) |
                         Machine::RegisterBit(R11);
    if (!win64)
        mask |= Machine::RegisterBit(Rsi) | Machine::RegisterBit(Rdi);
    const int vectorClobbers = win64 ? 6 : 16;
    for (int index = 0; index < vectorClobbers; ++index)
        mask |= Machine::RegisterBit(Xmm0 + static_cast<Reg>(index));
    return mask;
}

class Selector {
public:
    Selector(const LirFunc &inputFunction, const SelectionContext &inputContext);

    [[nodiscard]] std::optional<SelectedFunction> Run();

private:
    const LirFunc &func;
    const SelectionContext &context;
    SelectedFunction out;
    bool win64 = false;

    LirRegisterTable<TypeRef> types;
    /// Values of vector instructions: whatever their element type, they live in XMM registers.
    LirRegisterTable<bool> vectorValues;
    LirRegisterTable<const LirInstr *> definitions;
    LirRegisterTable<std::uint64_t> integerConstants;
    LirRegisterTable<std::string> floatConstants;
    LirRegisterTable<std::size_t> useCounts;
    LirRegisterTable<Reg> registers;
    LirRegisterTable<std::int32_t> homes;
    LirRegisterTable<std::int32_t> allocas;
    /// For a value whose register is known to hold zeros above some bit, that bit count. A zero extension of such a
    /// value is a copy; so is a sign extension when the sign bit is among the zeros.
    LirRegisterTable<int> zeroAbove;
    LirRegisterTable<Reg> phiTemporaries;
    LirRegisterTable<std::int32_t> phiTemporaryHomes;
    /// (instruction, register) for every register an instruction reads for the last time, sorted.
    std::vector<std::pair<const LirInstr *, LirReg>> lastUses;
    /// Comparisons fused into the branch that reads them, by the register they define.
    LirRegisterTable<bool> fusedComparisons;
    std::vector<std::uint32_t> blockEntry;
    std::uint32_t current = 0;
    /// The LIR block being selected, for combines that must see the instructions around the one they replace.
    const LirBlock *currentBlock = nullptr;
    std::int32_t outgoingSize = 0;
    bool hasPanic = false;
    Reg hiddenReturnPointer = NoReg;

    // ---- types (MachineSelection.cpp) --------------------------------------------------------------------------
    [[nodiscard]] int Size(const TypeRef &type) const;
    [[nodiscard]] int ScalarSize(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsInterface(const TypeRef &type) const;
    [[nodiscard]] ValueKind KindOf(const TypeRef &type) const;
    [[nodiscard]] const TypeRef &TypeOf(LirReg reg) const;
    [[nodiscard]] ValueKind KindOfReg(LirReg reg) const;
    [[nodiscard]] int SlotSize(const TypeRef &type) const;
    [[nodiscard]] int StorageAlign(const TypeRef &type) const;
    [[nodiscard]] int AllocaSize(const LirInstr &instruction) const;
    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] bool IsKilledAt(const LirInstr &instruction, LirReg reg) const;
    [[nodiscard]] int KnownZeroAbove(LirReg reg) const;
    void NoteZeroAbove(LirReg reg, int bits);
    /// Whether extending `reg` from its type's width changes nothing in its register.
    [[nodiscard]] bool AlreadyExtended(LirReg reg, int size, bool isSigned) const;

    // ---- analysis and structure (MachineSelection.cpp) ---------------------------------------------------------
    [[nodiscard]] bool CollectFacts();
    [[nodiscard]] bool Eligible() const;
    [[nodiscard]] bool EligibleInstruction(const LirInstr &instruction) const;
    void ComputeLastUses();
    void MarkBlocks();
    void ComputeSuccessors();

    // ---- emission helpers (MachineSelection.cpp) ---------------------------------------------------------------
    Instruction &Emit(Op op, std::uint8_t size, std::vector<Operand> operands);
    void EmitCopy(Reg destination, Reg source, RegClass regClass);
    [[nodiscard]] Reg NewGpr();
    [[nodiscard]] Reg NewXmm();
    [[nodiscard]] std::uint32_t NewBlock(std::uint32_t loopDepth, bool cold);
    [[nodiscard]] std::uint32_t Symbol(SymbolRef symbol);
    [[nodiscard]] Reg Value(LirReg reg);
    [[nodiscard]] std::int32_t Home(LirReg reg);
    [[nodiscard]] Address HomeAddress(LirReg reg);
    [[nodiscard]] Reg Extended(LirReg reg, bool forceSigned = false, bool forceUnsigned = false);
    [[nodiscard]] Reg ExtendRegister(Reg value, int size, bool isSigned);
    [[nodiscard]] Reg Materialize(LirReg reg);
    [[nodiscard]] std::optional<std::int64_t> Immediate(LirReg reg, int width) const;
    [[nodiscard]] Address AddressOf(LirReg pointer, int depth = 0);
    [[nodiscard]] static Address Offset(Address address, std::int64_t displacement, bool &fits);
    void CopyMemory(const Address &destination, const Address &source, int size);
    void ZeroMemory(const Address &destination, int size);
    void LoadInto(Reg destination, const Address &address, const TypeRef &type);

    // ---- instructions (MachineSelection.cpp, MachineSelectionArithmetic.cpp) ------------------------------------
    void SelectInstruction(const LirInstr &instruction);
    void SelectConstant(const LirInstr &instruction);
    void SelectLoad(const LirInstr &instruction);
    /// Selects an `or` that assembles a whole value from consecutive bytes of memory as one wide load, reversed for
    /// big-endian order. Returns false, emitting nothing, for any other `or`.
    [[nodiscard]] bool SelectCombinedLoad(const LirInstr &instruction);
    /// Selects an unsigned division or remainder by a nonzero constant as a multiplication by its reciprocal.
    /// Returns false, emitting nothing, for any other division.
    [[nodiscard]] bool SelectConstantDivision(const LirInstr &instruction);
    /// The same for a signed division or remainder by a constant other than 0 and -1.
    [[nodiscard]] bool SelectSignedConstantDivision(const LirInstr &instruction);
    void SelectStore(const LirInstr &instruction);
    void SelectAddress(const LirInstr &instruction);
    void SelectBinary(const LirInstr &instruction);
    void SelectDivision(const LirInstr &instruction);
    void SelectShift(const LirInstr &instruction);
    void SelectUnary(const LirInstr &instruction);
    void SelectPackedInteger(const LirInstr &instruction);
    void SelectComparison(const LirInstr &instruction);
    void SelectCast(const LirInstr &instruction);
    /// Sets the flags for an integer or floating comparison and returns the condition that is true when the comparison
    /// holds; a floating `==` or `!=` also needs the parity flag, which the caller handles.
    [[nodiscard]] Cond EmitCompare(const LirInstr &comparison);

    // ---- control flow (MachineSelection.cpp) -------------------------------------------------------------------
    void SelectBlock(std::uint32_t lirBlock);
    void SelectTerminator(std::uint32_t lirBlock, const LirTerminator &terminator);
    [[nodiscard]] std::uint32_t EdgeTarget(std::uint32_t from, std::uint32_t to, bool shared);
    void EmitPhiSources(std::uint32_t from, std::uint32_t to);
    void EmitPhiDestinations(std::uint32_t lirBlock);
    void EmitBranch(const LirTerminator &terminator, std::uint32_t lirBlock);
    void EmitSwitch(const LirTerminator &terminator, std::uint32_t lirBlock);
    [[nodiscard]] bool Fusable(std::uint32_t lirBlock, const LirTerminator &terminator) const;

    // ---- calls and the calling convention (MachineSelectionCalls.cpp) -------------------------------------------
    void SelectParameters();
    void SelectCall(const LirInstr &instruction);
    void SelectReturn(const LirTerminator &terminator);
    void SelectPanic(const LirInstr &instruction);
    void SelectAssert(const LirInstr &instruction);
    [[nodiscard]] bool SelectBitCast(const LirInstr &instruction);
    void PassScalar(LirReg argument, const TypeRef &type, Reg destination);
    [[nodiscard]] bool IsCallClobberingWin64(CallingConvention convention) const;
    [[nodiscard]] bool UsesWin64(CallingConvention convention) const;
    [[nodiscard]] std::int32_t PanicSiteFor(const LirInstr &instruction);
};
} // namespace Rux::X86_64Machine::Detail
