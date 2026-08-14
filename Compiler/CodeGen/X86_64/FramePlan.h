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
class X86_64FramePlanner;

using X86_64PhiMoves = std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::vector<PhiMove>>>;

// Every function-local placement decision needed by x86-64 emission. Planning
// finishes before the first instruction is written; consumers can only inspect
// the result, so instruction selection cannot grow or otherwise reshape a
// frame while it is being emitted.
class X86_64FramePlan {
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

    [[nodiscard]] const X86_64PhiMoves &PhiMoves() const {
        return phiMoves;
    }

    [[nodiscard]] const std::unordered_map<LirReg, int> &PhysicalRegisters() const {
        return physicalRegisters;
    }

    [[nodiscard]] const std::vector<int> &UsedPhysicalRegisters() const {
        return usedPhysicalRegisters;
    }

    [[nodiscard]] std::int32_t FrameSize() const {
        return frameSize;
    }

    [[nodiscard]] std::int32_t HiddenReturnOffset() const {
        return hiddenReturnOffset;
    }

    // End offset of the one reusable phi-cycle scratch region, or zero when
    // every phi edge is already an acyclic copy schedule.
    [[nodiscard]] std::int32_t PhiTemporaryOffset() const {
        return phiTemporaryOffset;
    }

    [[nodiscard]] std::int32_t PhiTemporarySize() const {
        return phiTemporarySize;
    }

private:
    friend class X86_64FramePlanner;
    friend X86_64FramePlan PlanX86_64Frame(const LirFunc &, const Layout::LayoutMap &,
                                           const std::unordered_set<std::string> &, Target::OS);

    std::unordered_map<LirReg, std::int32_t> slotOffsets;
    std::unordered_map<LirReg, std::int32_t> allocaDataOffsets;
    std::unordered_map<LirReg, TypeRef> registerTypes;
    X86_64PhiMoves phiMoves;
    std::unordered_map<LirReg, int> physicalRegisters;
    std::vector<int> usedPhysicalRegisters;
    std::int32_t frameSize = 0;
    std::int32_t hiddenReturnOffset = 0;
    std::int32_t phiTemporaryOffset = 0;
    std::int32_t phiTemporarySize = 0;
};

[[nodiscard]] X86_64FramePlan PlanX86_64Frame(const LirFunc &func, const Layout::LayoutMap &layouts,
                                              const std::unordered_set<std::string> &interfaceNames,
                                              Target::OS targetOs);
} // namespace Rux
