#include "CodeGen/X86_64/FramePlan.h"

#include "CodeGen/LinearScan.h"
#include "Target/CallingConvention.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace Rux {
using namespace Layout;

namespace {
constexpr int kCalleeSavedRegisters = 5;
} // namespace

class X86_64FramePlanner {
public:
    X86_64FramePlanner(const LirFunc &inputFunc, const LayoutMap &inputLayouts,
                       const std::unordered_set<std::string> &inputInterfaceNames, const Target::OS inputTargetOs)
        : func(inputFunc)
        , layouts(inputLayouts)
        , interfaceNames(inputInterfaceNames)
        , targetOs(inputTargetOs) {
    }

    [[nodiscard]] X86_64FramePlan Build() {
        PlanCalleeSaves();
        nextOffset = static_cast<std::int32_t>(plan.usedPhysicalRegisters.size() * 8);
        PlanHiddenReturn();
        PlanParameters();
        PlanInstructions();
        PlanWideTemporaries();
        PlanPhiTemporary();

        plan.frameSize = AlignUp(nextOffset, 16);
        if (plan.frameSize == 0) {
            plan.frameSize = 16;
        }
        return std::move(plan);
    }

private:
    const LirFunc &func;
    const LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;
    X86_64FramePlan plan;
    std::int32_t nextOffset = 0;

    [[nodiscard]] CallingConvention EffectiveConvention() const {
        if (func.callConv == CallingConvention::Default) {
            return PlatformDefaultConvention(targetOs, Target::Arch::X86_64);
        }
        return ResolveCConvention(func.callConv, targetOs, Target::Arch::X86_64);
    }

    [[nodiscard]] int RuntimeSize(const TypeRef &type) const {
        return RuntimeSizeOf(type, layouts, interfaceNames);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &type) const {
        if (IsWideInteger(type) || (IsSoftwareFloat(type) && RuntimeSize(type) > 8)) {
            return true;
        }
        if (type.IsRange()) {
            return true;
        }
        // A string is a 16-byte {data, length} view, exactly the shape a slice has, so it is
        // classified and placed the way a slice is.
        if (type.IsString()) {
            return true;
        }
        switch (type.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(type.name);
            return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base) ||
                   (!type.inner.empty() && SizeOf(type) > 8);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &type) const {
        if (!IsAggregate(type)) {
            return false;
        }
        const int size = RuntimeSize(type);
        return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
    }

    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &type) const {
        return IsAggregate(type) && RuntimeSize(type) > 16;
    }

    [[nodiscard]] bool IsWin64AddressParameter(const TypeRef &type) const {
        if (type.IsString()) {
            return true;
        }
        if (type.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(type.name);
        return base == "Slice" || interfaceNames.contains(base);
    }

    [[nodiscard]] TypeRef ParameterRegisterType(const TypeRef &type) const {
        return IsWin64AddressParameter(type) ? TypeRef::MakePointer(type) : type;
    }

    std::int32_t AllocateRegion(const int byteCount) {
        const int alignment = byteCount > 0 ? std::min(byteCount, 8) : 1;
        nextOffset = AlignUp(nextOffset, alignment);
        nextOffset += byteCount > 0 ? byteCount : 8;
        return nextOffset;
    }

    std::int32_t AllocateSlot(const LirReg reg, const int byteCount) {
        if (const auto found = plan.slotOffsets.find(reg); found != plan.slotOffsets.end()) {
            return found->second;
        }
        const std::int32_t offset = AllocateRegion(byteCount);
        plan.slotOffsets[reg] = offset;
        return offset;
    }

    void PlanCalleeSaves() {
        ParamTypeMap parameterTypes;
        for (const auto &parameter : func.params) {
            parameterTypes[parameter.reg] = ParameterRegisterType(parameter.type);
        }

        std::vector<LiveInterval> candidates;
        if (func.blocks.size() == 1) {
            for (const auto &interval : ComputeLiveIntervals(func, parameterTypes)) {
                if (IsFloat(interval.type) || IsAggregate(interval.type) || RuntimeSize(interval.type) > 8) {
                    continue;
                }
                candidates.push_back(interval);
            }
        }

        RegisterAssignment assignment = AllocateRegisters(candidates, kCalleeSavedRegisters);
        plan.physicalRegisters = std::move(assignment.physRegs);
        plan.usedPhysicalRegisters = std::move(assignment.usedPhysRegs);
    }

    void PlanHiddenReturn() {
        const CallingConvention convention = EffectiveConvention();
        if ((convention == CallingConvention::Win64 && IsWin64ByRefAggregate(func.returnType)) ||
            (convention == CallingConvention::SysV && IsSysVMemoryAggregate(func.returnType))) {
            plan.hiddenReturnOffset = AllocateRegion(8);
        }
    }

    void PlanParameters() {
        for (const auto &parameter : func.params) {
            TypeRef registerType = ParameterRegisterType(parameter.type);
            plan.registerTypes[parameter.reg] = registerType;
            AllocateSlot(parameter.reg, std::max(8, RuntimeSize(registerType)));
        }
    }

    [[nodiscard]] int AllocaSize(const LirInstr &instruction) const {
        if (instruction.strArg.empty()) {
            return RuntimeSize(instruction.type);
        }

        int count = 0;
        const std::string_view text = instruction.strArg;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            count = 0;
        }
        const TypeRef &elementType = instruction.type.inner.empty() ? instruction.type : instruction.type.inner[0];
        const int elementSize = RuntimeSize(elementType);
        return count * (elementSize > 0 ? elementSize : 8);
    }

    void PlanInstructions() {
        for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
            for (const auto &instruction : func.blocks[blockIndex].instrs) {
                if (instruction.op == LirOpcode::Phi) {
                    for (const auto &[source, predecessor] : instruction.phiPreds) {
                        plan.phiMoves[predecessor][blockIndex].push_back({instruction.dst, source, instruction.type});
                    }
                }
                if (instruction.dst == LirNoReg) {
                    continue;
                }

                if (instruction.op == LirOpcode::Alloca) {
                    AllocateSlot(instruction.dst, 8);
                    const int dataSize = AllocaSize(instruction);
                    plan.allocaDataOffsets[instruction.dst] = AllocateRegion(dataSize > 0 ? dataSize : 8);
                    plan.registerTypes[instruction.dst] = TypeRef::MakePointer(instruction.type);
                    continue;
                }

                plan.registerTypes[instruction.dst] = instruction.type;
                const int size = RuntimeSize(instruction.type);
                int slotSize = size > 0 ? size : 8;
                if (slotSize == 3 || slotSize == 5 || slotSize == 6 || slotSize == 7) {
                    // An odd-width aggregate is moved slot-to-slot as a whole word, since no scalar move spells
                    // its width; the slot is padded to the word so that move stays inside it.
                    slotSize = 8;
                }
                AllocateSlot(instruction.dst, slotSize);
            }
        }
    }

    void PlanWideTemporaries() {
        int largest = 0;
        for (const LirBlock &block : func.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (IsWideInteger(instruction.type)) {
                    largest = std::max(largest, RuntimeSize(instruction.type));
                }
            }
        }
        if (largest == 0) {
            return;
        }
        plan.wideTemporarySize = largest;
        for (std::int32_t &offset : plan.wideTemporaryOffsets) {
            offset = AllocateRegion(largest);
        }
    }

    void PlanPhiTemporary() {
        for (const auto &[fromBlock, destinations] : plan.phiMoves) {
            (void)fromBlock;
            for (const auto &[toBlock, moves] : destinations) {
                (void)toBlock;
                for (const auto &step : ResolvePhiMoves(moves)) {
                    if (step.kind != PhiMoveStep::Kind::SaveDestination) {
                        continue;
                    }
                    // Reserve what the value actually occupies. Clamping to sixteen would under-reserve any wider
                    // aggregate carried through a phi, which is the same overrun that undersized array allocas
                    // produced. AArch64 already sizes this the same way.
                    plan.phiTemporarySize = std::max(plan.phiTemporarySize, std::max(8, RuntimeSize(step.type)));
                }
            }
        }
        if (plan.phiTemporarySize > 0) {
            plan.phiTemporaryOffset = AllocateRegion(plan.phiTemporarySize);
        }
    }
};

X86_64FramePlan PlanX86_64Frame(const LirFunc &func, const LayoutMap &layouts,
                                const std::unordered_set<std::string> &interfaceNames, const Target::OS targetOs) {
    return X86_64FramePlanner(func, layouts, interfaceNames, targetOs).Build();
}
} // namespace Rux
