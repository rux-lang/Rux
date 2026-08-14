#pragma once

#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RuntimeHelpers.h"
#include "Ir/Lir/Lir.h"

#include <cstdint>
#include <vector>

namespace Rux {
class X64Enc;

// Narrow access to module-owned operations needed while selecting instructions
// for one function. Keeping these operations behind hooks lets the function
// emitter own instruction families without taking ownership of module symbols
// or literal pools.
class X86_64FunctionEmitterHooks {
public:
    virtual ~X86_64FunctionEmitterHooks() = default;

    virtual void LoadA(LirReg reg, const TypeRef &type) const = 0;
    virtual void LoadB(LirReg reg, const TypeRef &type) const = 0;
    virtual void StoreA(LirReg reg, const TypeRef &type) const = 0;
    [[nodiscard]] virtual std::uint32_t InternFloatSignMask(bool float32) = 0;
    virtual void AddTextRelocation(std::uint32_t sectionOffset, std::uint32_t symbol) = 0;
    virtual void EmitCallArguments(const std::vector<LirReg> &arguments) = 0;
};

// Selects LIR instructions in the context of one planned x86-64 function.
// Additional instruction families can move behind this boundary without
// exposing frame or module-emission state.
class X86_64FunctionEmitter {
public:
    X86_64FunctionEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan, X86_64RuntimeHelperEmitter &runtimeHelpers,
                          CallingConvention defaultConvention, X86_64FunctionEmitterHooks &hooks);

    // Returns true exactly when the instruction belongs to the arithmetic,
    // comparison, shift, cast, or unary family and was emitted here.
    [[nodiscard]] bool EmitArithmetic(const LirInstr &instruction);

private:
    X64Enc &encoder;
    const X86_64FramePlan &framePlan;
    X86_64RuntimeHelperEmitter &runtimeHelpers;
    CallingConvention defaultConvention;
    X86_64FunctionEmitterHooks &hooks;

    [[nodiscard]] std::int32_t Disp(LirReg reg) const;
};
} // namespace Rux
