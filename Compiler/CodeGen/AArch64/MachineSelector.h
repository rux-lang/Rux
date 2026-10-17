#pragma once

// Private state of AArch64 machine instruction selection, shared by the translation units that lower each family of
// LIR operations. Nothing outside `MachineSelection*.cpp` includes this header.

#include "CodeGen/AArch64/MachineSelection.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::AArch64Machine::Detail {
using Machine::Address;
using Machine::Instruction;
using Machine::NoReg;
using Machine::Operand;
using Machine::RegClass;

enum class ValueKind : std::uint8_t {
    Gpr,
    Fpr,
    Aggregate,
    Unsupported
};

/// Registers a call may overwrite: X0 to X18 and every vector register but the low halves of V8 to V15, which only
/// ever hold scalars here and so survive whole.
[[nodiscard]] constexpr std::uint64_t CallClobbers() noexcept {
    std::uint64_t mask = 0;
    for (Reg reg = 0; reg <= 18; ++reg)
        mask |= Machine::RegisterBit(reg);
    mask |= Machine::RegisterBit(Lr);
    for (Reg index = 0; index < 32; ++index)
        if (index < 8 || index > 15)
            mask |= Machine::RegisterBit(V0 + index);
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

    std::unordered_map<LirReg, TypeRef> types;
    std::unordered_map<LirReg, const LirInstr *> definitions;
    std::unordered_map<LirReg, std::uint64_t> integerConstants;
    std::unordered_map<LirReg, std::string> floatConstants;
    std::unordered_map<LirReg, std::size_t> useCounts;
    std::unordered_map<LirReg, Reg> registers;
    std::unordered_map<LirReg, std::int32_t> homes;
    std::unordered_map<LirReg, std::int32_t> allocas;
    /// For a value whose register is known to hold zeros above some bit, that bit count.
    std::unordered_map<LirReg, int> zeroAbove;
    std::unordered_map<LirReg, Reg> phiTemporaries;
    std::unordered_map<LirReg, std::int32_t> phiTemporaryHomes;
    std::unordered_map<const LirInstr *, std::vector<LirReg>> lastUses;
    std::unordered_set<const LirInstr *> fusedComparisons;
    std::vector<std::uint32_t> blockEntry;
    std::uint32_t current = 0;
    std::int32_t outgoingSize = 0;
    Reg indirectResult = NoReg;

    // ---- types (MachineSelection.cpp) --------------------------------------------------------------------------
    [[nodiscard]] int Size(const TypeRef &type) const;
    [[nodiscard]] int ScalarSize(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] ValueKind KindOf(const TypeRef &type) const;
    [[nodiscard]] const TypeRef &TypeOf(LirReg reg) const;
    [[nodiscard]] ValueKind KindOfReg(LirReg reg) const;
    [[nodiscard]] int SlotSize(const TypeRef &type) const;
    [[nodiscard]] int StorageAlign(const TypeRef &type) const;
    [[nodiscard]] int AllocaSize(const LirInstr &instruction) const;
    [[nodiscard]] bool IsPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] bool IsKilledAt(const LirInstr &instruction, LirReg reg) const;
    [[nodiscard]] int KnownZeroAbove(LirReg reg) const;
    void NoteZeroAbove(LirReg reg, int bits);
    [[nodiscard]] bool AlreadyExtended(LirReg reg, int size, bool isSigned) const;

    // ---- analysis and structure (MachineSelection.cpp) ---------------------------------------------------------
    [[nodiscard]] bool CollectFacts();
    [[nodiscard]] bool Eligible() const;
    [[nodiscard]] bool EligibleInstruction(const LirInstr &instruction) const;
    [[nodiscard]] bool EligibleCall(const LirInstr &instruction) const;
    [[nodiscard]] bool EligibleLocation(const AArch64ArgumentLocation &location) const;
    void ComputeLastUses();
    void MarkBlocks();
    void ComputeSuccessors();

    // ---- emission helpers (MachineSelection.cpp) ---------------------------------------------------------------
    Instruction &Emit(Op op, std::uint8_t size, std::vector<Operand> operands);
    void EmitCopy(Reg destination, Reg source, RegClass regClass);
    [[nodiscard]] Reg NewGpr();
    [[nodiscard]] Reg NewFpr();
    [[nodiscard]] std::uint32_t NewBlock(std::uint32_t loopDepth, bool cold);
    [[nodiscard]] std::uint32_t Symbol(SymbolRef symbol);
    [[nodiscard]] Reg Value(LirReg reg);
    [[nodiscard]] std::int32_t Home(LirReg reg);
    [[nodiscard]] Address HomeAddress(LirReg reg);
    [[nodiscard]] Reg Extended(LirReg reg, bool forceSigned = false, bool forceUnsigned = false);
    [[nodiscard]] Reg ExtendRegister(Reg value, int size, bool isSigned);
    [[nodiscard]] Reg Constant(std::uint64_t value);
    [[nodiscard]] Address AddressOf(LirReg pointer, int depth = 0);
    [[nodiscard]] static Address Offset(Address address, std::int64_t displacement, bool &fits);
    [[nodiscard]] Reg AddressRegister(const Address &address);
    void CopyMemory(const Address &destination, const Address &source, int size);
    void ZeroMemory(const Address &destination, int size);
    void LoadInto(Reg destination, const Address &address, const TypeRef &type);
    void LoadWidth(Reg destination, const Address &address, int width, bool isSigned);

    // ---- instructions (MachineSelection.cpp, MachineSelectionArithmetic.cpp) ------------------------------------
    void SelectInstruction(const LirInstr &instruction);
    void SelectConstant(const LirInstr &instruction);
    void SelectLoad(const LirInstr &instruction);
    void SelectStore(const LirInstr &instruction);
    void SelectAddress(const LirInstr &instruction);
    void SelectBinary(const LirInstr &instruction);
    void SelectDivision(const LirInstr &instruction);
    [[nodiscard]] bool SelectConstantDivision(const LirInstr &instruction);
    [[nodiscard]] bool SelectSignedConstantDivision(const LirInstr &instruction);
    void SelectShift(const LirInstr &instruction);
    void SelectUnary(const LirInstr &instruction);
    void SelectComparison(const LirInstr &instruction);
    void SelectCast(const LirInstr &instruction);
    /// Sets the flags for an integer or floating comparison and returns the condition that is true when the comparison
    /// holds.
    [[nodiscard]] A64Condition EmitCompare(const LirInstr &comparison);

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
    [[nodiscard]] std::int32_t PanicSiteFor(const LirInstr &instruction);
};
} // namespace Rux::AArch64Machine::Detail
