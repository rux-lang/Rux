#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
class AArch64FramePlanner;

using AArch64PhiMoves = std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::vector<PhiMove>>>;

/// Every function-local placement decision needed by AArch64 emission. The plan is completed before the first
/// instruction is written and exposes only const views, so widening retries consume exactly the same frame.
class AArch64FramePlan {
public:
    [[nodiscard]] const std::unordered_map<LirReg, std::int32_t> &SlotOffsets() const {
        return slotOffsets;
    }

    [[nodiscard]] const std::unordered_map<LirReg, std::int32_t> &AllocaDataOffsets() const {
        return allocaDataOffsets;
    }

    [[nodiscard]] const std::unordered_map<LirReg, TypeRef> &RegisterTypes() const {
        return registerTypes;
    }

    [[nodiscard]] const AArch64PhiMoves &PhiMoves() const {
        return phiMoves;
    }

    /// Machine-register numbers, rather than allocation-pool indices. General homes are therefore X19-X28 and can never
    /// name the platform register X18.
    [[nodiscard]] const std::unordered_map<LirReg, unsigned> &GeneralRegisterHomes() const {
        return generalRegisterHomes;
    }

    [[nodiscard]] const std::unordered_map<LirReg, unsigned> &VectorRegisterHomes() const {
        return vectorRegisterHomes;
    }

    [[nodiscard]] const std::vector<unsigned> &SavedGeneralRegisters() const {
        return savedGeneralRegisters;
    }

    [[nodiscard]] const std::vector<unsigned> &SavedVectorRegisters() const {
        return savedVectorRegisters;
    }

    [[nodiscard]] std::int32_t FrameSize() const {
        return frameSize;
    }

    [[nodiscard]] std::int32_t IndirectResultOffset() const {
        return indirectResultOffset;
    }

    [[nodiscard]] std::int32_t CalleeSaveOffset() const {
        return calleeSaveOffset;
    }

    [[nodiscard]] std::int32_t PhiTemporaryOffset() const {
        return phiTemporaryOffset;
    }

    [[nodiscard]] int PhiTemporarySize() const {
        return phiTemporarySize;
    }

    [[nodiscard]] Target::OS TargetOs() const {
        return targetOs;
    }

private:
    friend class AArch64FramePlanner;
    friend AArch64FramePlan PlanAArch64Frame(const LirFunc &, const Layout::LayoutMap &,
                                             const std::unordered_set<std::string> &,
                                             const std::vector<LirStructDecl> &, Target::OS);

    std::unordered_map<LirReg, std::int32_t> slotOffsets;
    std::unordered_map<LirReg, std::int32_t> allocaDataOffsets;
    std::unordered_map<LirReg, TypeRef> registerTypes;
    AArch64PhiMoves phiMoves;
    std::unordered_map<LirReg, unsigned> generalRegisterHomes;
    std::unordered_map<LirReg, unsigned> vectorRegisterHomes;
    std::vector<unsigned> savedGeneralRegisters;
    std::vector<unsigned> savedVectorRegisters;
    std::int32_t frameSize = 0;
    std::int32_t indirectResultOffset = 0;
    std::int32_t calleeSaveOffset = 0;
    std::int32_t phiTemporaryOffset = 0;
    int phiTemporarySize = 0;
    Target::OS targetOs = Target::OS::Linux;
};

[[nodiscard]] AArch64FramePlan PlanAArch64Frame(const LirFunc &func, const Layout::LayoutMap &layouts,
                                                const std::unordered_set<std::string> &interfaceNames,
                                                const std::vector<LirStructDecl> &structDecls, Target::OS targetOs);
} // namespace Rux
