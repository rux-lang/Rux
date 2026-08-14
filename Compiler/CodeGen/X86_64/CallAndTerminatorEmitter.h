#pragma once

#include "CodeGen/X86_64/FramePlan.h"
#include "Ir/Lir/Lir.h"
#include "Target/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
class X64Enc;

// Narrow access to function values and module symbols while calls and control
// flow are emitted. The collaborators retain no module-owned state.
class X86_64CallAndTerminatorHooks {
public:
    virtual ~X86_64CallAndTerminatorHooks() = default;

    [[nodiscard]] virtual int SizeOfRuntime(const TypeRef &type) const = 0;
    [[nodiscard]] virtual bool IsAggregate(const TypeRef &type) const = 0;
    [[nodiscard]] virtual bool IsRegPointerTo(LirReg reg, const TypeRef &pointee) const = 0;
    virtual void LoadA(LirReg reg, const TypeRef &type) const = 0;
    virtual void StoreA(LirReg reg, const TypeRef &type) const = 0;
    virtual void StoreHiddenReturnValue(LirReg reg, const TypeRef &type) const = 0;
    [[nodiscard]] virtual std::uint32_t ResolveCallSymbol(const std::string &name) = 0;
    virtual void AddTextRelocation(std::uint32_t sectionOffset, std::uint32_t symbol) = 0;
};

class X86_64CallEmitter {
public:
    X86_64CallEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan, Target::OS targetOs,
                      X86_64CallAndTerminatorHooks &hooks);

    // Returns true exactly when the instruction is a direct or indirect call.
    [[nodiscard]] bool Emit(const LirInstr &instruction);
    void EmitArguments(const std::vector<LirReg> &arguments, CallingConvention convention = CallingConvention::Default,
                       int startIndex = 0) const;

private:
    X64Enc &encoder;
    const X86_64FramePlan &framePlan;
    Target::OS targetOs;
    X86_64CallAndTerminatorHooks &hooks;

    [[nodiscard]] CallingConvention EffectiveConvention(CallingConvention convention) const;
    [[nodiscard]] std::int32_t Disp(LirReg reg) const;
    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &type) const;
    [[nodiscard]] bool IsPointerToWin64ByRefAggregate(const TypeRef &type) const;
    [[nodiscard]] int CallFrameSize(const std::vector<LirReg> &arguments, bool win64, int startIndex) const;
    void StoreSysVStackArguments(const std::vector<LirReg> &arguments, int startIndex) const;
    void StoreReturnValue(LirReg destination, const TypeRef &type) const;
    [[nodiscard]] bool EmitBitCast(const LirInstr &instruction) const;
    void EmitPreparedCall(const LirInstr &instruction, const std::vector<LirReg> &arguments, bool indirect) const;
};

class X86_64TerminatorEmitter {
public:
    X86_64TerminatorEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan, X86_64CallAndTerminatorHooks &hooks);

    void Begin(std::size_t blockCount);
    void MarkBlock(std::uint32_t blockIndex);
    void Emit(std::uint32_t blockIndex, const LirTerminator &terminator);
    void PatchJumps();

private:
    struct JumpPatch {
        std::uint32_t patchOffset;
        std::uint32_t targetBlock;
    };

    X64Enc &encoder;
    const X86_64FramePlan &framePlan;
    X86_64CallAndTerminatorHooks &hooks;
    std::vector<std::uint32_t> blockOffsets;
    std::vector<JumpPatch> jumpPatches;

    [[nodiscard]] std::int32_t Disp(LirReg reg) const;
    [[nodiscard]] bool HasPhiMoves(std::uint32_t fromBlock, std::uint32_t toBlock) const;
    void EmitPhiMoves(std::uint32_t fromBlock, std::uint32_t toBlock);
    void LoadReturnValue(LirReg reg, const TypeRef &type) const;
};
} // namespace Rux
