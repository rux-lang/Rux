#pragma once

// Register allocation for machine IR by iterated register coalescing (George and Appel, 1996).
//
// Every virtual register receives a physical register of its class or is spilled to a frame object and replaced by
// short-lived temporaries around each mention. Copies the coloring makes redundant are removed. Precolored physical
// registers model fixed operands (call arguments, results, division) and clobbers, so the allocator needs no
// target-specific constraint language: a call's clobbers interfere with everything live across it, which keeps such
// values in callee-saved registers or memory.

#include "CodeGen/Machine/MachineIr.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Rux::Machine {
/// What the allocator needs to know about a target's registers.
struct TargetRegisters {
    /// Allocatable physical registers of each class, in the order the allocator prefers them. Cheap registers come
    /// first: putting caller-saved registers ahead of callee-saved ones leaves a function that never needs a saved
    /// register without a single push.
    std::array<std::vector<Reg>, 2> allocationOrder;
    /// The class of every physical register the allocator tracks; registers absent from both orders are ignored.
    std::array<std::uint64_t, 2> classMask{};
    std::uint64_t calleeSaved = 0;
};

/// The target's spill and reload instructions.
class SpillCodeFactory {
public:
    virtual ~SpillCodeFactory() = default;
    [[nodiscard]] virtual Instruction Load(Reg destination, RegClass regClass, std::int32_t frameObject) const = 0;
    [[nodiscard]] virtual Instruction Store(Reg source, RegClass regClass, std::int32_t frameObject) const = 0;
    [[nodiscard]] virtual std::int32_t SpillSize(RegClass regClass) const = 0;
};

struct AllocationResult {
    bool succeeded = false;
    /// Callee-saved registers some instruction now names; the prologue saves exactly these.
    std::uint64_t usedCalleeSaved = 0;
    std::uint32_t rounds = 0;
    std::uint32_t spilledRegisters = 0;
};

/// Limits beyond which allocation declines rather than spending unbounded time; the caller keeps another strategy.
inline constexpr std::size_t MaximumAllocatorNodes = 24000;
inline constexpr std::uint32_t MaximumAllocatorRounds = 24;

[[nodiscard]] AllocationResult AllocateRegisters(Function &function, const TargetRegisters &target,
                                                 const SpillCodeFactory &spills);
} // namespace Rux::Machine
