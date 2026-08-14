#pragma once

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/AArch64/RuntimeHelpers.h"
#include "CodeGen/Layout.h"
#include "Ir/Lir/Lir.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Rux {
// Narrow access to register homes, stack slots and diagnostics owned by the
// surrounding module emitter. Instruction-family ownership stays here without
// exposing symbols, literal pools or the rest of function emission.
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
    virtual void ReportFunctionDiagnostic(std::string message) = 0;
};

// Selects the arithmetic side of one planned AArch64 function. Returns false
// for instruction families owned by the surrounding emitter.
class AArch64FunctionEmitter {
public:
    AArch64FunctionEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan,
                           AArch64RuntimeHelperEmitter &runtimeHelpers, const Layout::LayoutMap &layouts,
                           const std::unordered_set<std::string> &interfaceNames, std::string functionName,
                           AArch64FunctionEmitterHooks &hooks);

    // Includes integer and floating arithmetic, comparisons, shifts, unary and
    // bit operations, casts, power operations and float-bit reinterpretations.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);

private:
    struct BinaryOperands {
        A64Reg lhs;
        A64Reg rhs;
    };

    A64Enc &encoder;
    const AArch64FramePlan &framePlan;
    AArch64RuntimeHelperEmitter &runtimeHelpers;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    std::string functionName;
    AArch64FunctionEmitterHooks &hooks;

    [[nodiscard]] TypeRef TypeOfReg(LirReg reg) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsRegisterValue(const TypeRef &type) const;
    [[nodiscard]] static A64Reg FpReg(const TypeRef &type, unsigned index);
    [[nodiscard]] std::optional<BinaryOperands> LoadBinaryOperands(const LirInstr &instruction, const TypeRef &lhsType,
                                                                   const TypeRef &rhsType);
    [[nodiscard]] std::optional<A64Reg> LoadUnaryOperand(const LirInstr &instruction, const TypeRef &type);
    [[nodiscard]] std::optional<BinaryOperands> LoadFloatOperands(const LirInstr &instruction, const TypeRef &type);
    void EmitFloatBits(const LirInstr &instruction);
    void Report(std::string message);
    void NotImplemented(std::string what);
    void Must(A64Status status, std::string_view what);
};
} // namespace Rux
