#pragma once

#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "Ir/Lir/Lir.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Rux {
// Narrow access to function values, diagnostics, and module symbols while
// calls and control flow are emitted. Relocation construction and shared
// register/stack movement remain owned by the surrounding module emitter.
class AArch64CallAndTerminatorHooks {
public:
    virtual ~AArch64CallAndTerminatorHooks() = default;

    [[nodiscard]] virtual std::int32_t Disp(LirReg reg) = 0;
    [[nodiscard]] virtual int RuntimeSize(const TypeRef &type) const = 0;
    [[nodiscard]] virtual int RuntimeAlign(const TypeRef &type) const = 0;
    [[nodiscard]] virtual bool IsAggregate(const TypeRef &type) const = 0;
    virtual void LoadScalar(A64Reg destination, A64Reg base, std::int64_t offset, unsigned width, bool sign) = 0;
    virtual void StoreScalar(A64Reg value, A64Reg base, std::int64_t offset, unsigned width) = 0;
    virtual void LoadFromSlot(A64Reg destination, LirReg reg, const TypeRef &type) = 0;
    virtual void LoadWidthFromSlot(A64Reg destination, LirReg reg, unsigned width, bool sign) = 0;
    virtual void StoreToSlot(A64Reg value, LirReg reg, const TypeRef &type) = 0;
    virtual void LoadFpFromSlot(A64Reg destination, LirReg reg) = 0;
    virtual void StoreFpToSlot(A64Reg value, LirReg reg) = 0;
    virtual void LoadPointer(A64Reg destination, LirReg reg) = 0;
    [[nodiscard]] virtual A64Reg ReadPointerOperand(LirReg reg, A64Reg scratch) = 0;
    virtual void SlotAddress(A64Reg destination, LirReg reg) = 0;
    virtual void CopyBlock(A64Reg destination, std::int64_t destinationOffset, A64Reg source, std::int64_t sourceOffset,
                           int size, bool paired) = 0;
    virtual void CopyFrameValue(std::int32_t destinationOffset, std::int32_t sourceOffset, const TypeRef &type) = 0;
    virtual void OpenStackArea(std::int32_t bytes, std::string_view what) = 0;
    virtual void EmitEpilogue() = 0;
    [[nodiscard]] virtual std::uint32_t ResolveCallSymbol(const std::string &name) = 0;
    virtual void AddCallRelocation(std::uint32_t sectionOffset, std::uint32_t symbol) = 0;
    virtual void ReportCallAndTerminatorDiagnostic(std::string message) = 0;
};

class AArch64CallEmitter {
public:
    AArch64CallEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan, AArch64CallPlanner &callPlanner,
                       std::string functionName, AArch64CallAndTerminatorHooks &hooks);

    // Returns true exactly when the instruction is a direct or indirect call.
    [[nodiscard]] bool Emit(const LirInstr &instruction);
    void EmitParamSpills(const LirFunc &function);
    void EmitReturnValue(LirReg reg, const TypeRef &type);

private:
    using ArgLocation = AArch64ArgumentLocation;
    using CallLayout = AArch64CallLayout;

    A64Enc &encoder;
    const AArch64FramePlan &framePlan;
    AArch64CallPlanner &callPlanner;
    std::string functionName;
    AArch64CallAndTerminatorHooks &hooks;

    [[nodiscard]] TypeRef TypeOfReg(LirReg reg) const;
    void EmitWindowsVariadicCallArgs(const std::vector<LirReg> &arguments, const std::vector<TypeRef> &types,
                                     const CallLayout &layout);
    void MoveRegisterArgument(const ArgLocation &location, LirReg reg, const TypeRef &type, bool toRegisters);
    void LoadResultFromAddress(const ArgLocation &location, A64Reg base, const TypeRef &type);
    void EmitCallArgs(const std::vector<LirReg> &arguments, const std::vector<TypeRef> &types,
                      const CallLayout &layout);
    void EmitCall(const LirInstr &instruction, const std::vector<LirReg> &arguments, bool indirect);
    void Report(std::string message);
    void Must(A64Status status, std::string_view what);
};

class AArch64TerminatorEmitter {
public:
    AArch64TerminatorEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan, AArch64CallEmitter &callEmitter,
                             std::string functionName, AArch64CallAndTerminatorHooks &hooks);

    void BeginFunction();
    void BeginPass(std::size_t blockCount);
    void MarkBlock(std::uint32_t blockIndex);
    void Emit(std::uint32_t blockIndex, const LirTerminator &terminator);
    [[nodiscard]] bool PatchJumps();
    [[nodiscard]] std::size_t WidenedSiteCount() const;

    // Assertion lowering remains with the runtime/platform emitter but uses the
    // same checked short-branch implementation as ordinary control flow.
    [[nodiscard]] std::uint32_t EmitBranchOverNonZero(A64Reg reg);
    void PatchBranchOver(std::uint32_t patchOffset);

private:
    struct JumpPatch {
        std::uint32_t patchOffset = 0;
        std::uint32_t targetBlock = 0;
        unsigned lsb = 0;
        unsigned width = 0;
        std::uint64_t site = 0;
    };

    struct ConditionalBranch {
        enum class Form : std::uint8_t {
            Condition,
            Zero,
            NotZero
        };

        Form form = Form::Condition;
        A64Condition condition = A64Condition::Eq;
        A64Reg reg{};

        [[nodiscard]] ConditionalBranch Inverted() const;
    };

    A64Enc &encoder;
    const AArch64FramePlan &framePlan;
    AArch64CallEmitter &callEmitter;
    std::string functionName;
    AArch64CallAndTerminatorHooks &hooks;
    std::vector<std::uint32_t> blockOffsets;
    std::vector<JumpPatch> jumpPatches;
    std::unordered_set<std::uint64_t> widenedSites;

    [[nodiscard]] static std::uint64_t BranchSite(std::uint32_t block, unsigned ordinal);
    [[nodiscard]] static ConditionalBranch OnCondition(A64Condition condition);
    [[nodiscard]] static ConditionalBranch OnZero(A64Reg reg);
    [[nodiscard]] bool HasPhiMoves(std::uint32_t from, std::uint32_t to) const;
    void EmitPhiMoves(std::uint32_t from, std::uint32_t to);
    void EmitJumpTo(std::uint32_t targetBlock);
    void EmitConditionalBranch(const ConditionalBranch &branch, std::int64_t offset);
    void EmitConditionalBranchTo(const ConditionalBranch &branch, std::uint32_t targetBlock, std::uint64_t site);
    [[nodiscard]] std::uint32_t EmitBranchOver(const ConditionalBranch &branch);
    void CompareAgainstCase(const std::string &value);
    void Report(std::string message);
    void Must(A64Status status, std::string_view what);
};
} // namespace Rux
