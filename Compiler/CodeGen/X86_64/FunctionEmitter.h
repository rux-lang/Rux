#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RuntimeHelpers.h"
#include "Ir/Lir/Lir.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Rux {
class X64Enc;

/// Narrow access to module-owned operations needed while selecting instructions for one function. Keeping these
/// operations behind hooks lets the function emitter own instruction families without taking ownership of module
/// symbols or literal pools.
class X86_64FunctionEmitterHooks {
public:
    virtual ~X86_64FunctionEmitterHooks() = default;

    virtual void LoadA(LirReg reg, const TypeRef &type) const = 0;
    virtual void LoadB(LirReg reg, const TypeRef &type) const = 0;
    virtual void StoreA(LirReg reg, const TypeRef &type) const = 0;
    [[nodiscard]] virtual std::uint32_t InternStringLiteral(const std::string &value) = 0;
    [[nodiscard]] virtual std::uint32_t InternFloat32Literal(const std::string &value) = 0;
    [[nodiscard]] virtual std::uint32_t InternFloat64Literal(const std::string &value) = 0;
    [[nodiscard]] virtual std::uint32_t InternFloatSignMask(bool float32) = 0;
    [[nodiscard]] virtual std::uint32_t ResolveNamedDataSymbol(const std::string &name) = 0;
    [[nodiscard]] virtual std::uint32_t ResolveGlobalSymbol(const std::string &name) = 0;
    virtual void AddTextRelocation(std::uint32_t sectionOffset, std::uint32_t symbol) = 0;
    virtual void EmitCallArguments(const std::vector<LirReg> &arguments) = 0;
};

/// Selects LIR instructions in the context of one planned x86-64 function. Additional instruction families can move
/// behind this boundary without exposing frame or module-emission state.
class X86_64FunctionEmitter {
public:
    X86_64FunctionEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan, X86_64RuntimeHelperEmitter &runtimeHelpers,
                          CallingConvention defaultConvention, const Layout::LayoutMap &layouts,
                          const std::unordered_set<std::string> &interfaceNames, X86_64FunctionEmitterHooks &hooks);

    /// Returns true exactly when the instruction belongs to the arithmetic, comparison, shift, cast, or unary family
    /// and was emitted here.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);

    /// Returns true exactly when the instruction materializes a constant, accesses memory, or computes an aggregate
    /// address and was emitted here.
    [[nodiscard]] bool EmitMemory(const LirInstr &instruction);

private:
    X64Enc &encoder;
    const X86_64FramePlan &framePlan;
    X86_64RuntimeHelperEmitter &runtimeHelpers;
    CallingConvention defaultConvention;
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    X86_64FunctionEmitterHooks &hooks;

    [[nodiscard]] std::int32_t Disp(LirReg reg) const;
    [[nodiscard]] int SizeOfRuntime(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsRegPointerTo(LirReg reg, const TypeRef &pointee) const;
    [[nodiscard]] int FieldOffset(LirReg base, const std::string &fieldName) const;
    [[nodiscard]] bool EmitWideArithmetic(const LirInstr &instruction);
    [[nodiscard]] bool EmitWideSoftwareFloatNegation(const LirInstr &instruction);
    void EmitSoftwareFloatConstant(const LirInstr &instruction);
    void ZeroWide(std::int32_t destination, int size) const;
    void CopyWide(std::int32_t source, std::int32_t destination, int size) const;
    void NegateWide(std::int32_t value, int size) const;
    void MultiplyWide(std::int32_t left, std::int32_t right, std::int32_t destination, int size) const;
    [[nodiscard]] std::uint32_t JumpIf(std::uint8_t condition) const;
    void PatchHere(std::uint32_t offset) const;
    void LoadChunkFromR10(std::int32_t offset, int size) const;
    void StoreChunkToR11(std::int32_t offset, int size) const;
    void CopyAggregateFromR10ToStack(std::int32_t destinationDisp, int size) const;
    void CopyAggregateFromR10ToR11(int size) const;
};
} // namespace Rux
