#pragma once

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/Layout.h"
#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Rux {
/// Narrow access to register homes, stack slots, diagnostics, and symbol/literal operations owned by the surrounding
/// module emitter. Instruction-family ownership stays here without exposing the rest of function emission.
class AArch64FunctionEmitterHooks {
public:
    virtual ~AArch64FunctionEmitterHooks() = default;

    [[nodiscard]] virtual A64Reg ReadOperand(LirReg reg, const TypeRef &type, A64Reg scratch) = 0;
    [[nodiscard]] virtual A64Reg ReadRawOperand(LirReg reg, unsigned width, A64Reg scratch) = 0;
    [[nodiscard]] virtual A64Reg ReadFloatOperand(LirReg reg, A64Reg scratch) = 0;
    [[nodiscard]] virtual A64Reg ResultRegister(LirReg reg, A64Reg scratch) const = 0;
    [[nodiscard]] virtual A64Reg FloatResultRegister(LirReg reg, A64Reg scratch) const = 0;
    virtual void StoreToSlot(A64Reg value, LirReg reg, const TypeRef &type) = 0;
    virtual void StoreWidthToSlot(A64Reg value, LirReg reg, unsigned width) = 0;
    virtual void StoreFpToSlot(A64Reg value, LirReg reg) = 0;
    virtual void LoadFromSlot(A64Reg destination, LirReg reg, const TypeRef &type) = 0;
    virtual void LoadFpFromSlot(A64Reg destination, LirReg reg) = 0;
    [[nodiscard]] virtual A64Reg ReadPointerOperand(LirReg reg, A64Reg scratch) = 0;
    virtual void LoadScalar(A64Reg destination, A64Reg base, std::int64_t offset, unsigned width, bool sign) = 0;
    virtual void StoreScalar(A64Reg value, A64Reg base, std::int64_t offset, unsigned width) = 0;
    virtual void SlotAddress(A64Reg destination, LirReg reg) = 0;
    virtual void CopyBlock(A64Reg destination, std::int64_t destinationOffset, A64Reg source, std::int64_t sourceOffset,
                           int size, bool paired) = 0;
    [[nodiscard]] virtual std::uint32_t InternStringLiteral(const std::string &value) = 0;
    [[nodiscard]] virtual std::uint32_t ResolveGlobalSymbol(const std::string &name) = 0;
    virtual void LoadSymbolAddress(A64Reg destination, std::uint32_t symbol) = 0;
    virtual void LoadNamedDataSymbol(A64Reg destination, const std::string &name) = 0;
    virtual void LoadFloatConstant(A64Reg destination, const TypeRef &type, const std::string &literal) = 0;
    virtual void ReportFunctionDiagnostic(std::string message) = 0;
    virtual void ReportFunctionDiagnostic(Diagnostic diagnostic) = 0;
};

/// Selects instruction families for one planned AArch64 function. Each family returns false for instructions owned by
/// another emitter.
class AArch64FunctionEmitter {
public:
    AArch64FunctionEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan, const Layout::LayoutMap &layouts,
                           const std::unordered_set<std::string> &interfaceNames, std::string functionName,
                           Target::OS targetOs, AArch64FunctionEmitterHooks &hooks);

    /// Includes integer and floating arithmetic, comparisons, shifts, unary and bit operations, casts, power operations
    /// and float-bit reinterpretations.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);

    /// Includes constants, local and symbol addresses, scalar and aggregate loads/stores, and field/index address
    /// computation.
    void EmitAggregateConstant(const LirInstr &instruction);
    [[nodiscard]] bool EmitMemory(const LirInstr &instruction);

private:
    struct BinaryOperands {
        A64Reg lhs;
        A64Reg rhs;
    };

    A64Enc &encoder;
    const AArch64FramePlan &framePlan;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    std::string functionName;
    Target::OS targetOs;
    AArch64FunctionEmitterHooks &hooks;

    [[nodiscard]] TypeRef TypeOfReg(LirReg reg) const;
    [[nodiscard]] std::int32_t Disp(LirReg reg);
    [[nodiscard]] int RuntimeSize(const TypeRef &type) const;
    [[nodiscard]] int RuntimeAlign(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsRegisterValue(const TypeRef &type) const;
    [[nodiscard]] bool IsRegPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] static A64Reg FpReg(const TypeRef &type, unsigned index);
    [[nodiscard]] static std::uint64_t ConstantBits(const LirInstr &instruction);
    [[nodiscard]] std::optional<BinaryOperands> LoadBinaryOperands(const LirInstr &instruction, const TypeRef &lhsType,
                                                                   const TypeRef &rhsType);
    [[nodiscard]] std::optional<A64Reg> LoadUnaryOperand(const LirInstr &instruction, const TypeRef &type);
    [[nodiscard]] std::optional<BinaryOperands> LoadFloatOperands(const LirInstr &instruction, const TypeRef &type);
    [[nodiscard]] bool EmitSoftwareFloatNegation(const LirInstr &instruction);
    [[nodiscard]] bool EmitSoftwareFloatConstant(const LirInstr &instruction);
    [[nodiscard]] bool EmitWideArithmetic(const LirInstr &instruction);
    [[nodiscard]] bool EmitWideConstant(const LirInstr &instruction);
    void LoadWideWord(A64Reg destination, LirReg value, int word);
    void LoadWideTemporaryWord(A64Reg destination, std::size_t temporary, int word);
    void StoreWideWord(A64Reg value, LirReg destination, int word);
    void StoreWideTemporaryWord(A64Reg value, std::size_t temporary, int word);
    void ZeroWide(std::int32_t destination, int size);
    void CopyWide(std::int32_t source, std::int32_t destination, int size);
    void NegateWide(std::int32_t value, int size);
    void MultiplyWide(std::int32_t left, std::int32_t right, std::int32_t destination, int size);
    [[nodiscard]] std::uint32_t BranchIf(A64Condition condition);
    [[nodiscard]] std::uint32_t Branch();
    void PatchConditionalBranch(std::uint32_t site);
    void PatchBranch(std::uint32_t site);
    void EmitFloatBits(const LirInstr &instruction);
    void Report(std::string message);
    void NotImplemented(std::string what);
    void Must(A64Status status, std::string_view what);
};
} // namespace Rux
