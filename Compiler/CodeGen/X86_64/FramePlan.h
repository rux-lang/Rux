#pragma once

#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
#include "CodeGen/X86_64/Conditions.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
class X86_64FramePlanner;

using X86_64PhiMoves = std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::vector<PhiMove>>>;

struct X86BranchComparison {
    LirReg left;
    LirReg right;
    TypeRef type;
    X86Condition condition;
};

/// Every function-local placement decision needed by x86-64 emission. Planning finishes before the first instruction is
/// written; consumers can only inspect the result, so instruction selection cannot grow or otherwise reshape a frame
/// while it is being emitted.
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

    /// Scalar float32/float64 homes are XMM8-XMM15, disjoint from every ABI argument and emitter scratch register.
    [[nodiscard]] const std::unordered_map<LirReg, int> &FloatRegisters() const {
        return floatRegisters;
    }

    /// Full 128-bit Win64 nonvolatile saves, as {architectural XMM number, positive frame offset}.
    [[nodiscard]] const std::vector<std::pair<int, std::int32_t>> &FloatSaves() const {
        return floatSaves;
    }

    [[nodiscard]] const std::unordered_map<LirReg, std::uint64_t> &ImmediateValues() const {
        return immediateValues;
    }

    [[nodiscard]] const std::unordered_map<LirReg, X86BranchComparison> &BranchComparisons() const {
        return branchComparisons;
    }

    [[nodiscard]] std::int32_t FrameSize() const {
        return frameSize;
    }

    [[nodiscard]] std::int32_t HiddenReturnOffset() const {
        return hiddenReturnOffset;
    }

    /// End offset of the one reusable phi-cycle scratch region, or zero when every phi edge is already an acyclic copy
    /// schedule.
    [[nodiscard]] std::int32_t PhiTemporaryOffset() const {
        return phiTemporaryOffset;
    }

    [[nodiscard]] std::int32_t PhiTemporarySize() const {
        return phiTemporarySize;
    }

    [[nodiscard]] std::int32_t WideTemporaryOffset(const std::size_t index) const {
        return wideTemporaryOffsets.at(index);
    }

    [[nodiscard]] std::int32_t WideTemporarySize() const {
        return wideTemporarySize;
    }

private:
    friend class X86_64FramePlanner;
    friend X86_64FramePlan PlanX86_64Frame(const LirFunc &, const Layout::LayoutMap &,
                                           const std::unordered_set<std::string> &, Target::OS);

    std::unordered_map<LirReg, std::int32_t> slotOffsets;
    std::unordered_map<LirReg, std::int32_t> allocaDataOffsets;
    std::unordered_map<LirReg, TypeRef> registerTypes;
    std::unordered_map<LirReg, std::uint64_t> immediateValues;
    std::unordered_map<LirReg, X86BranchComparison> branchComparisons;
    X86_64PhiMoves phiMoves;
    std::unordered_map<LirReg, int> physicalRegisters;
    std::vector<int> usedPhysicalRegisters;
    std::unordered_map<LirReg, int> floatRegisters;
    std::vector<std::pair<int, std::int32_t>> floatSaves;
    std::int32_t frameSize = 0;
    std::int32_t hiddenReturnOffset = 0;
    std::int32_t phiTemporaryOffset = 0;
    std::int32_t phiTemporarySize = 0;
    std::array<std::int32_t, 3> wideTemporaryOffsets{};
    std::int32_t wideTemporarySize = 0;
};

[[nodiscard]] X86_64FramePlan PlanX86_64Frame(const LirFunc &func, const Layout::LayoutMap &layouts,
                                              const std::unordered_set<std::string> &interfaceNames,
                                              Target::OS targetOs);
} // namespace Rux
